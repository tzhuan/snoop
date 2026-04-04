#include "capture.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

struct CaptureState {
    /* X11 */
    Display *dpy;
    Window root;
    XShmSegmentInfo shm_info;
    XImage *image;
    int screen_width;
    int screen_height;

    /* Current XShm buffer dimensions (may differ from screen if region-sized) */
    int shm_w;
    int shm_h;

    /* Threading */
    pthread_t thread;
    int running;

    /* Region crop */
    int has_region;
    int region_x, region_y, region_w, region_h;

    /* Rate limiting */
    int target_fps;
    int64_t min_frame_interval_ns;
    int64_t last_frame_time_ns;

    /* Suspend/snap */
    int suspended;
    int snap_requested;

    /* Callback */
    frame_callback_t callback;
    void *userdata;
};

static int64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Allocate (or reallocate) the XShm image to the given dimensions */
static int alloc_xshm_image(CaptureState *state, int w, int h) {
    /* Detach and free old resources */
    if (state->image) {
        XShmDetach(state->dpy, &state->shm_info);
        if (state->shm_info.shmaddr && state->shm_info.shmaddr != (void *)-1) {
            shmdt(state->shm_info.shmaddr);
        }
        state->image->data = NULL;
        XDestroyImage(state->image);
        state->image = NULL;
        state->shm_info.shmaddr = (void *)-1;
    }

    Visual *visual = DefaultVisual(state->dpy, DefaultScreen(state->dpy));
    int depth = DefaultDepth(state->dpy, DefaultScreen(state->dpy));

    state->image = XShmCreateImage(state->dpy, visual, depth, ZPixmap,
                                    NULL, &state->shm_info, w, h);
    if (!state->image) return -1;

    state->shm_info.shmid = shmget(IPC_PRIVATE,
                                    state->image->bytes_per_line * state->image->height,
                                    IPC_CREAT | 0600);
    if (state->shm_info.shmid < 0) return -1;

    state->shm_info.shmaddr = state->image->data = shmat(state->shm_info.shmid, NULL, 0);
    if (state->shm_info.shmaddr == (void *)-1) return -1;

    state->shm_info.readOnly = True;
    if (!XShmAttach(state->dpy, &state->shm_info)) return -1;

    shmctl(state->shm_info.shmid, IPC_RMID, NULL);

    state->shm_w = w;
    state->shm_h = h;
    return 0;
}

static int init_xshm(CaptureState *state) {
    state->dpy = XOpenDisplay(NULL);
    if (!state->dpy) return -1;

    int screen = DefaultScreen(state->dpy);
    state->root = RootWindow(state->dpy, screen);
    state->screen_width = DisplayWidth(state->dpy, screen);
    state->screen_height = DisplayHeight(state->dpy, screen);

    if (!XShmQueryExtension(state->dpy)) {
        fprintf(stderr, "capture: XShm extension not available\n");
        return -1;
    }

    /* Start with full-screen buffer; will be resized to region on first frame */
    if (alloc_xshm_image(state, state->screen_width, state->screen_height) < 0)
        return -1;

    return 0;
}

static void cleanup_xshm(CaptureState *state) {
    if (state->dpy && state->image) {
        XShmDetach(state->dpy, &state->shm_info);
    }
    if (state->shm_info.shmaddr && state->shm_info.shmaddr != (void *)-1) {
        shmdt(state->shm_info.shmaddr);
        state->shm_info.shmaddr = NULL;
    }
    if (state->image) {
        state->image->data = NULL; /* Already freed by shmdt */
        XDestroyImage(state->image);
        state->image = NULL;
    }
    if (state->dpy) {
        XCloseDisplay(state->dpy);
        state->dpy = NULL;
    }
}

/* Convert BGRA (X11 native on 24/32-bit) to RGBA */
static void bgra_to_rgba(uint8_t *dst, const uint8_t *src, int pixels) {
    for (int i = 0; i < pixels; i++) {
        dst[i * 4 + 0] = src[i * 4 + 2]; /* R */
        dst[i * 4 + 1] = src[i * 4 + 1]; /* G */
        dst[i * 4 + 2] = src[i * 4 + 0]; /* B */
        dst[i * 4 + 3] = 255;            /* A */
    }
}

static void *capture_thread(void *arg) {
    CaptureState *state = arg;

    while (state->running) {
        /* Rate limiting — sleep until next frame */
        if (state->min_frame_interval_ns > 0) {
            int64_t now = get_time_ns();
            int64_t elapsed = now - state->last_frame_time_ns;
            if (elapsed < state->min_frame_interval_ns) {
                struct timespec sleep_ts;
                int64_t remaining = state->min_frame_interval_ns - elapsed;
                sleep_ts.tv_sec = remaining / 1000000000LL;
                sleep_ts.tv_nsec = remaining % 1000000000LL;
                nanosleep(&sleep_ts, NULL);
            }
            state->last_frame_time_ns = get_time_ns();
        }

        /* Suspend check */
        if (state->suspended && !state->snap_requested) {
            usleep(10000); /* 10ms idle sleep */
            continue;
        }
        state->snap_requested = 0;

        if (!state->callback) continue;

        if (state->has_region) {
            /* Clamp region to screen bounds */
            int rx = state->region_x;
            int ry = state->region_y;
            int rw = state->region_w;
            int rh = state->region_h;
            if (rx < 0) { rw += rx; rx = 0; }
            if (ry < 0) { rh += ry; ry = 0; }
            if (rx + rw > state->screen_width) rw = state->screen_width - rx;
            if (ry + rh > state->screen_height) rh = state->screen_height - ry;
            if (rw <= 0 || rh <= 0) continue;

            /* Resize XShm buffer if region size changed */
            if (rw != state->shm_w || rh != state->shm_h) {
                if (alloc_xshm_image(state, rw, rh) < 0) {
                    fprintf(stderr, "capture: XShm realloc failed (%dx%d)\n", rw, rh);
                    continue;
                }
            }

            /* Capture only the viewport region — X server copies just this rect */
            XShmGetImage(state->dpy, state->root, state->image, rx, ry, AllPlanes);

            uint8_t *data = (uint8_t *)state->image->data;
            int stride = state->image->bytes_per_line;
            int out_stride = rw * 4;
            uint8_t *rgba = malloc(out_stride * rh);
            if (rgba) {
                for (int row = 0; row < rh; row++) {
                    bgra_to_rgba(rgba + row * out_stride,
                                 data + row * stride, rw);
                }
                state->callback(state->userdata, rgba, rw, rh, out_stride);
                free(rgba);
            }
        } else {
            /* No region — capture full screen (startup / fallback) */
            if (state->shm_w != state->screen_width || state->shm_h != state->screen_height) {
                if (alloc_xshm_image(state, state->screen_width, state->screen_height) < 0)
                    continue;
            }

            XShmGetImage(state->dpy, state->root, state->image, 0, 0, AllPlanes);

            uint8_t *data = (uint8_t *)state->image->data;
            int width = state->screen_width;
            int height = state->screen_height;
            int stride = state->image->bytes_per_line;
            int out_stride = width * 4;
            uint8_t *rgba = malloc(out_stride * height);
            if (rgba) {
                for (int row = 0; row < height; row++) {
                    bgra_to_rgba(rgba + row * out_stride,
                                 data + row * stride, width);
                }
                state->callback(state->userdata, rgba, width, height, out_stride);
                free(rgba);
            }
        }
    }

    return NULL;
}

CaptureState *capture_create(void) {
    CaptureState *state = calloc(1, sizeof(CaptureState));
    if (!state) return NULL;
    state->target_fps = 30;
    state->min_frame_interval_ns = 1000000000LL / 30;
    state->shm_info.shmaddr = (void *)-1;
    return state;
}

int capture_start(CaptureState *state, frame_callback_t cb, void *userdata) {
    state->callback = cb;
    state->userdata = userdata;

    if (init_xshm(state) < 0) {
        fprintf(stderr, "capture: XShm init failed\n");
        cleanup_xshm(state);
        return -1;
    }

    fprintf(stderr, "capture: XShm started (%dx%d)\n",
            state->screen_width, state->screen_height);

    state->running = 1;
    if (pthread_create(&state->thread, NULL, capture_thread, state) != 0) {
        state->running = 0;
        cleanup_xshm(state);
        return -1;
    }

    return 0;
}

void capture_stop(CaptureState *state) {
    if (!state || !state->running) return;

    state->running = 0;
    pthread_join(state->thread, NULL);
    cleanup_xshm(state);
}

void capture_destroy(CaptureState *state) {
    if (!state) return;
    capture_stop(state);
    free(state);
}

void capture_set_region(CaptureState *state, int x, int y, int w, int h) {
    state->region_x = x;
    state->region_y = y;
    state->region_w = w;
    state->region_h = h;
    state->has_region = 1;
}

void capture_set_rate(CaptureState *state, int fps) {
    state->target_fps = fps;
    state->min_frame_interval_ns = fps > 0 ? 1000000000LL / fps : 0;
}

void capture_suspend(CaptureState *state) { state->suspended = 1; }
void capture_resume(CaptureState *state) { state->suspended = 0; }
void capture_snap(CaptureState *state) { state->snap_requested = 1; }

void capture_on_error(CaptureState *state, error_callback_t cb, void *userdata) {
    (void)state; (void)cb; (void)userdata;
}

/* X11 root window already spans all monitors — no-op */
int capture_set_display(CaptureState *state, const char *display_id) {
    (void)state; (void)display_id;
    return 0;
}

int capture_list_displays(CaptureState *state,
                          CaptureDisplayInfo *out, int max_displays) {
    (void)state; (void)out; (void)max_displays;
    return -1; /* Not supported on X11 — use Electron screen API instead */
}
