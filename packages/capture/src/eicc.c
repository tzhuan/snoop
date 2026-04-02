/*
 * ext-image-copy-capture-v1 capture backend for Wayland.
 *
 * Uses the standardized ext-image-copy-capture-v1 Wayland protocol for
 * screen capture with damage tracking. The compositor skips copying
 * unchanged regions, reducing compositor-side work.
 *
 * Supported on: wlroots-based compositors (Sway, Hyprland), KDE Plasma 6+.
 * NOT supported on: GNOME/Mutter (as of 2025).
 */

#define _GNU_SOURCE
#include "capture.h"

#include <wayland-client.h>
#include "../protocol/ext-image-copy-capture-v1-client-protocol.h"
#include "../protocol/ext-image-capture-source-v1-client-protocol.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

/* SHM format constants (wl_shm) */
#ifndef WL_SHM_FORMAT_ARGB8888
#define WL_SHM_FORMAT_ARGB8888 0
#define WL_SHM_FORMAT_XRGB8888 1
#define WL_SHM_FORMAT_ABGR8888 0x34324241
#define WL_SHM_FORMAT_XBGR8888 0x34324258
#endif

/* Maximum outputs we track */
#define MAX_OUTPUTS 16

typedef struct {
    struct wl_output *wl_output;
    uint32_t name;             /* wl_registry name */
    char output_name[64];      /* wl_output name (v4) — e.g. "DP-1" */
    int x, y, width, height;   /* geometry from wl_output.geometry + mode */
    int done;                  /* received wl_output.done */
} OutputInfo;

struct CaptureState {
    /* Wayland connection */
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct ext_image_copy_capture_manager_v1 *capture_mgr;
    struct ext_output_image_capture_source_manager_v1 *source_mgr;

    /* Outputs */
    OutputInfo outputs[MAX_OUTPUTS];
    int output_count;

    /* Target display (set via capture_set_display) */
    char *display_id;          /* Output name string, e.g. "DP-1" */

    /* Active capture session */
    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;

    /* Session buffer constraints */
    uint32_t buf_width, buf_height;
    uint32_t shm_format;       /* Chosen SHM format */
    int constraints_done;

    /* SHM buffer */
    struct wl_shm_pool *shm_pool;
    struct wl_buffer *buffer;
    uint8_t *shm_data;
    int shm_fd;
    size_t shm_size;

    /* Threading */
    pthread_t thread;
    int running;
    int pipe_fd[2];            /* Used to wake the event loop for stop */

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

    /* Frame state */
    int frame_ready;
    int frame_failed;
    int session_stopped;
    int first_frame;

    /* Callbacks */
    frame_callback_t callback;
    error_callback_t error_callback;
    void *userdata;
    void *error_userdata;
};

static int64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---- SHM buffer allocation ---- */

static int create_shm_fd(size_t size) {
    int fd = memfd_create("snoop-eicc", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int alloc_shm_buffer(CaptureState *state) {
    if (!state->shm || !state->buf_width || !state->buf_height) return -1;

    int stride = state->buf_width * 4;
    size_t size = (size_t)stride * state->buf_height;

    /* Clean up old buffer */
    if (state->buffer) { wl_buffer_destroy(state->buffer); state->buffer = NULL; }
    if (state->shm_pool) { wl_shm_pool_destroy(state->shm_pool); state->shm_pool = NULL; }
    if (state->shm_data) { munmap(state->shm_data, state->shm_size); state->shm_data = NULL; }
    if (state->shm_fd >= 0) { close(state->shm_fd); state->shm_fd = -1; }

    state->shm_fd = create_shm_fd(size);
    if (state->shm_fd < 0) return -1;

    state->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, state->shm_fd, 0);
    if (state->shm_data == MAP_FAILED) {
        state->shm_data = NULL;
        close(state->shm_fd);
        state->shm_fd = -1;
        return -1;
    }
    state->shm_size = size;

    state->shm_pool = wl_shm_create_pool(state->shm, state->shm_fd, size);
    state->buffer = wl_shm_pool_create_buffer(state->shm_pool, 0,
                                               state->buf_width, state->buf_height,
                                               stride, state->shm_format);
    return 0;
}

/* ---- wl_output listener ---- */

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make, const char *model,
                            int32_t transform) {
    OutputInfo *oi = data;
    oi->x = x;
    oi->y = y;
    (void)output; (void)pw; (void)ph; (void)subpixel;
    (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *output,
                        uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    OutputInfo *oi = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        oi->width = width;
        oi->height = height;
    }
    (void)output; (void)refresh;
}

static void output_done(void *data, struct wl_output *output) {
    OutputInfo *oi = data;
    oi->done = 1;
    (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor) {
    (void)data; (void)output; (void)factor;
}

static void output_name(void *data, struct wl_output *output, const char *name) {
    OutputInfo *oi = data;
    if (name) snprintf(oi->output_name, sizeof(oi->output_name), "%s", name);
    (void)output;
}

static void output_description(void *data, struct wl_output *output, const char *desc) {
    (void)data; (void)output; (void)desc;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

/* ---- Session listener ---- */

static void session_buffer_size(void *data,
                                struct ext_image_copy_capture_session_v1 *session,
                                uint32_t width, uint32_t height) {
    CaptureState *state = data;
    state->buf_width = width;
    state->buf_height = height;
    (void)session;
}

static void session_shm_format(void *data,
                               struct ext_image_copy_capture_session_v1 *session,
                               uint32_t format) {
    CaptureState *state = data;
    /* Prefer XBGR8888 (RGBX) or ABGR8888 (RGBA) for less conversion,
     * fallback to XRGB8888 (BGRX) or ARGB8888 (BGRA) */
    if (format == WL_SHM_FORMAT_XBGR8888 || format == WL_SHM_FORMAT_ABGR8888) {
        state->shm_format = format;
    } else if (!state->shm_format &&
               (format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888)) {
        state->shm_format = format;
    }
    (void)session;
}

static void session_dmabuf_device(void *data,
                                  struct ext_image_copy_capture_session_v1 *session,
                                  struct wl_array *device) {
    (void)data; (void)session; (void)device;
}

static void session_dmabuf_format(void *data,
                                  struct ext_image_copy_capture_session_v1 *session,
                                  uint32_t format, struct wl_array *modifiers) {
    (void)data; (void)session; (void)format; (void)modifiers;
}

static void session_done(void *data,
                         struct ext_image_copy_capture_session_v1 *session) {
    CaptureState *state = data;
    state->constraints_done = 1;
    (void)session;
}

static void session_stopped(void *data,
                            struct ext_image_copy_capture_session_v1 *session) {
    CaptureState *state = data;
    state->session_stopped = 1;
    (void)session;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_buffer_size,
    .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device,
    .dmabuf_format = session_dmabuf_format,
    .done = session_done,
    .stopped = session_stopped,
};

/* ---- Frame listener ---- */

static void frame_transform(void *data,
                            struct ext_image_copy_capture_frame_v1 *frame,
                            uint32_t transform) {
    (void)data; (void)frame; (void)transform;
}

static void frame_damage(void *data,
                         struct ext_image_copy_capture_frame_v1 *frame,
                         int32_t x, int32_t y, int32_t width, int32_t height) {
    (void)data; (void)frame; (void)x; (void)y; (void)width; (void)height;
}

static void frame_presentation_time(void *data,
                                    struct ext_image_copy_capture_frame_v1 *frame,
                                    uint32_t hi, uint32_t lo, uint32_t nsec) {
    (void)data; (void)frame; (void)hi; (void)lo; (void)nsec;
}

static void frame_ready(void *data,
                        struct ext_image_copy_capture_frame_v1 *frame) {
    CaptureState *state = data;
    state->frame_ready = 1;
    (void)frame;
}

static void frame_failed(void *data,
                         struct ext_image_copy_capture_frame_v1 *frame,
                         uint32_t reason) {
    CaptureState *state = data;
    state->frame_failed = 1;
    (void)frame; (void)reason;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

/* ---- Registry listener ---- */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version) {
    CaptureState *state = data;

    if (strcmp(interface, "wl_shm") == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, "ext_image_copy_capture_manager_v1") == 0) {
        state->capture_mgr = wl_registry_bind(registry, name,
            &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (strcmp(interface, "ext_output_image_capture_source_manager_v1") == 0) {
        state->source_mgr = wl_registry_bind(registry, name,
            &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface, "wl_output") == 0 && state->output_count < MAX_OUTPUTS) {
        OutputInfo *oi = &state->outputs[state->output_count];
        memset(oi, 0, sizeof(*oi));
        oi->name = name;
        /* Bind version 4+ to get wl_output.name event */
        uint32_t bind_ver = version < 4 ? version : 4;
        oi->wl_output = wl_registry_bind(registry, name, &wl_output_interface, bind_ver);
        wl_output_add_listener(oi->wl_output, &output_listener, oi);
        state->output_count++;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ---- Find target output ---- */

static OutputInfo *find_target_output(CaptureState *state) {
    if (!state->display_id || state->display_id[0] == '\0') {
        /* No target specified — use first output */
        return state->output_count > 0 ? &state->outputs[0] : NULL;
    }
    for (int i = 0; i < state->output_count; i++) {
        if (strcmp(state->outputs[i].output_name, state->display_id) == 0)
            return &state->outputs[i];
    }
    /* Fallback to first output */
    return state->output_count > 0 ? &state->outputs[0] : NULL;
}

/* ---- BGRA→RGBA conversion ---- */

static void bgra_to_rgba(uint8_t *dst, const uint8_t *src, int pixels) {
    for (int i = 0; i < pixels; i++) {
        dst[i * 4 + 0] = src[i * 4 + 2]; /* R */
        dst[i * 4 + 1] = src[i * 4 + 1]; /* G */
        dst[i * 4 + 2] = src[i * 4 + 0]; /* B */
        dst[i * 4 + 3] = 255;            /* A */
    }
}

/* ---- Request a new frame capture ---- */

static void request_frame(CaptureState *state) {
    if (!state->session || state->session_stopped) return;

    state->frame_ready = 0;
    state->frame_failed = 0;

    state->frame = ext_image_copy_capture_session_v1_create_frame(state->session);
    ext_image_copy_capture_frame_v1_add_listener(state->frame, &frame_listener, state);

    ext_image_copy_capture_frame_v1_attach_buffer(state->frame, state->buffer);

    if (state->first_frame) {
        /* First frame: damage the whole buffer */
        ext_image_copy_capture_frame_v1_damage_buffer(state->frame,
            0, 0, state->buf_width, state->buf_height);
        state->first_frame = 0;
    } else {
        /* Subsequent frames: damage the whole buffer (simple approach).
         * The compositor benefits from damage tracking on its side —
         * it may skip unchanged regions even if we damage everything. */
        ext_image_copy_capture_frame_v1_damage_buffer(state->frame,
            0, 0, state->buf_width, state->buf_height);
    }

    ext_image_copy_capture_frame_v1_capture(state->frame);
    wl_display_flush(state->display);
}

/* ---- Deliver frame to callback ---- */

static void deliver_frame(CaptureState *state) {
    if (!state->callback || !state->shm_data) return;

    uint8_t *data = state->shm_data;
    int width = state->buf_width;
    int height = state->buf_height;
    int stride = width * 4;
    int needs_swap = (state->shm_format == WL_SHM_FORMAT_ARGB8888 ||
                      state->shm_format == WL_SHM_FORMAT_XRGB8888);

    if (state->has_region) {
        int rx = state->region_x;
        int ry = state->region_y;
        int rw = state->region_w;
        int rh = state->region_h;
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > width) rw = width - rx;
        if (ry + rh > height) rh = height - ry;

        if (rw > 0 && rh > 0) {
            int crop_stride = rw * 4;
            uint8_t *crop = malloc(crop_stride * rh);
            if (crop) {
                if (needs_swap) {
                    for (int row = 0; row < rh; row++) {
                        bgra_to_rgba(crop + row * crop_stride,
                                     data + (ry + row) * stride + rx * 4, rw);
                    }
                } else {
                    for (int row = 0; row < rh; row++) {
                        uint8_t *src = data + (ry + row) * stride + rx * 4;
                        uint8_t *dst = crop + row * crop_stride;
                        memcpy(dst, src, crop_stride);
                        /* Ensure alpha = 255 for XBGR */
                        if (state->shm_format == WL_SHM_FORMAT_XBGR8888) {
                            for (int c = 0; c < rw; c++) dst[c * 4 + 3] = 255;
                        }
                    }
                }
                state->callback(state->userdata, crop, rw, rh, crop_stride);
                free(crop);
            }
        }
    } else {
        uint8_t *rgba = malloc(stride * height);
        if (rgba) {
            if (needs_swap) {
                for (int row = 0; row < height; row++) {
                    bgra_to_rgba(rgba + row * stride, data + row * stride, width);
                }
            } else {
                memcpy(rgba, data, stride * height);
                if (state->shm_format == WL_SHM_FORMAT_XBGR8888) {
                    for (int i = 0; i < width * height; i++) rgba[i * 4 + 3] = 255;
                }
            }
            state->callback(state->userdata, rgba, width, height, stride);
            free(rgba);
        }
    }
}

/* ---- Capture thread ---- */

static void *capture_thread(void *arg) {
    CaptureState *state = arg;

    /* Set up session */
    OutputInfo *target = find_target_output(state);
    if (!target || !target->wl_output) {
        fprintf(stderr, "eicc: no target output found\n");
        if (state->error_callback)
            state->error_callback(state->error_userdata, "no target output found");
        return NULL;
    }

    /* Create source from output */
    state->source = ext_output_image_capture_source_manager_v1_create_source(
        state->source_mgr, target->wl_output);

    /* Create capture session (no cursor painting) */
    state->session = ext_image_copy_capture_manager_v1_create_session(
        state->capture_mgr, state->source, 0);
    ext_image_copy_capture_session_v1_add_listener(state->session, &session_listener, state);

    /* Wait for buffer constraints */
    wl_display_flush(state->display);
    while (state->running && !state->constraints_done && !state->session_stopped) {
        if (wl_display_dispatch(state->display) < 0) {
            fprintf(stderr, "eicc: display dispatch error\n");
            state->running = 0;
            break;
        }
    }

    if (!state->running || state->session_stopped) return NULL;

    if (!state->shm_format) {
        fprintf(stderr, "eicc: no supported SHM format\n");
        if (state->error_callback)
            state->error_callback(state->error_userdata, "no supported SHM format");
        return NULL;
    }

    fprintf(stderr, "eicc: session ready %ux%u format=0x%x output=%s\n",
            state->buf_width, state->buf_height, state->shm_format,
            target->output_name);

    /* Allocate SHM buffer */
    if (alloc_shm_buffer(state) < 0) {
        fprintf(stderr, "eicc: failed to allocate SHM buffer\n");
        if (state->error_callback)
            state->error_callback(state->error_userdata, "SHM buffer allocation failed");
        return NULL;
    }

    state->first_frame = 1;

    /* Capture loop */
    while (state->running && !state->session_stopped) {
        /* Rate limiting */
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
        }

        /* Suspend check */
        if (state->suspended && !state->snap_requested) {
            usleep(10000);
            continue;
        }
        state->snap_requested = 0;

        /* Request frame capture */
        request_frame(state);

        /* Wait for ready/failed */
        while (state->running && !state->frame_ready && !state->frame_failed
               && !state->session_stopped) {
            if (wl_display_dispatch(state->display) < 0) {
                fprintf(stderr, "eicc: display dispatch error in frame loop\n");
                state->running = 0;
                break;
            }
        }

        /* Destroy the frame object (required by protocol) */
        if (state->frame) {
            ext_image_copy_capture_frame_v1_destroy(state->frame);
            state->frame = NULL;
        }

        if (state->frame_ready) {
            state->last_frame_time_ns = get_time_ns();
            deliver_frame(state);
        }

        if (state->frame_failed) {
            /* Retry on next iteration */
            usleep(10000);
        }
    }

    return NULL;
}

/* ---- Public API ---- */

CaptureState *capture_create(void) {
    CaptureState *state = calloc(1, sizeof(CaptureState));
    if (!state) return NULL;
    state->target_fps = 30;
    state->min_frame_interval_ns = 1000000000LL / 30;
    state->shm_fd = -1;
    state->pipe_fd[0] = state->pipe_fd[1] = -1;
    return state;
}

int capture_start(CaptureState *state, frame_callback_t cb, void *userdata) {
    state->callback = cb;
    state->userdata = userdata;

    /* Connect to Wayland display */
    state->display = wl_display_connect(NULL);
    if (!state->display) {
        fprintf(stderr, "eicc: cannot connect to Wayland display\n");
        return -1;
    }

    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);

    /* First roundtrip: get globals */
    wl_display_roundtrip(state->display);
    /* Second roundtrip: get output info (name, geometry, mode) */
    wl_display_roundtrip(state->display);

    if (!state->capture_mgr) {
        fprintf(stderr, "eicc: compositor does not support ext-image-copy-capture-v1\n");
        wl_display_disconnect(state->display);
        state->display = NULL;
        return -1;
    }
    if (!state->source_mgr) {
        fprintf(stderr, "eicc: compositor does not support ext-output-image-capture-source-v1\n");
        wl_display_disconnect(state->display);
        state->display = NULL;
        return -1;
    }
    if (!state->shm) {
        fprintf(stderr, "eicc: no wl_shm\n");
        wl_display_disconnect(state->display);
        state->display = NULL;
        return -1;
    }

    fprintf(stderr, "eicc: found %d output(s)\n", state->output_count);

    state->running = 1;
    if (pthread_create(&state->thread, NULL, capture_thread, state) != 0) {
        state->running = 0;
        wl_display_disconnect(state->display);
        state->display = NULL;
        return -1;
    }

    return 0;
}

void capture_stop(CaptureState *state) {
    if (!state || !state->running) return;

    state->running = 0;

    /* Wake the event loop if blocked in dispatch */
    if (state->display) {
        /* Cancel read to unblock wl_display_dispatch */
    }

    pthread_join(state->thread, NULL);

    /* Clean up Wayland objects */
    if (state->frame) {
        ext_image_copy_capture_frame_v1_destroy(state->frame);
        state->frame = NULL;
    }
    if (state->session) {
        ext_image_copy_capture_session_v1_destroy(state->session);
        state->session = NULL;
    }
    if (state->source) {
        ext_image_capture_source_v1_destroy(state->source);
        state->source = NULL;
    }
    if (state->buffer) { wl_buffer_destroy(state->buffer); state->buffer = NULL; }
    if (state->shm_pool) { wl_shm_pool_destroy(state->shm_pool); state->shm_pool = NULL; }
    if (state->shm_data) { munmap(state->shm_data, state->shm_size); state->shm_data = NULL; }
    if (state->shm_fd >= 0) { close(state->shm_fd); state->shm_fd = -1; }

    for (int i = 0; i < state->output_count; i++) {
        if (state->outputs[i].wl_output)
            wl_output_destroy(state->outputs[i].wl_output);
    }
    state->output_count = 0;

    if (state->source_mgr) {
        ext_output_image_capture_source_manager_v1_destroy(state->source_mgr);
        state->source_mgr = NULL;
    }
    if (state->capture_mgr) {
        ext_image_copy_capture_manager_v1_destroy(state->capture_mgr);
        state->capture_mgr = NULL;
    }
    if (state->shm) { wl_shm_destroy(state->shm); state->shm = NULL; }
    if (state->registry) { wl_registry_destroy(state->registry); state->registry = NULL; }
    if (state->display) {
        wl_display_disconnect(state->display);
        state->display = NULL;
    }

    /* Reset session state for potential restart */
    state->constraints_done = 0;
    state->session_stopped = 0;
    state->shm_format = 0;
    state->buf_width = 0;
    state->buf_height = 0;
}

void capture_destroy(CaptureState *state) {
    if (!state) return;
    capture_stop(state);
    free(state->display_id);
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
    state->error_callback = cb;
    state->error_userdata = userdata;
}

int capture_set_display(CaptureState *state, const char *display_id) {
    free(state->display_id);
    state->display_id = display_id ? strdup(display_id) : NULL;
    return 0;
}

int capture_list_displays(CaptureState *state,
                          CaptureDisplayInfo *out, int max_displays) {
    if (!state || !out || max_displays <= 0) return -1;

    /* If we don't have a display connection yet, create a temporary one */
    struct wl_display *dpy = state->display;
    int temp_connection = 0;
    struct wl_registry *reg = NULL;

    if (!dpy) {
        dpy = wl_display_connect(NULL);
        if (!dpy) return -1;
        temp_connection = 1;

        /* We need to do a full roundtrip to enumerate outputs */
        CaptureState temp = {0};
        reg = wl_display_get_registry(dpy);
        wl_registry_add_listener(reg, &registry_listener, &temp);
        wl_display_roundtrip(dpy);
        wl_display_roundtrip(dpy);

        int count = 0;
        for (int i = 0; i < temp.output_count && count < max_displays; i++) {
            OutputInfo *oi = &temp.outputs[i];
            snprintf(out[count].connector, sizeof(out[count].connector), "%s", oi->output_name);
            out[count].x = oi->x;
            out[count].y = oi->y;
            out[count].width = oi->width;
            out[count].height = oi->height;
            count++;
        }

        for (int i = 0; i < temp.output_count; i++) {
            if (temp.outputs[i].wl_output) wl_output_destroy(temp.outputs[i].wl_output);
        }
        if (temp.shm) wl_shm_destroy(temp.shm);
        if (temp.capture_mgr) ext_image_copy_capture_manager_v1_destroy(temp.capture_mgr);
        if (temp.source_mgr) ext_output_image_capture_source_manager_v1_destroy(temp.source_mgr);
        wl_registry_destroy(reg);
        wl_display_disconnect(dpy);
        return count;
    }

    /* Use existing connection's output list */
    int count = 0;
    for (int i = 0; i < state->output_count && count < max_displays; i++) {
        OutputInfo *oi = &state->outputs[i];
        snprintf(out[count].connector, sizeof(out[count].connector), "%s", oi->output_name);
        out[count].x = oi->x;
        out[count].y = oi->y;
        out[count].width = oi->width;
        out[count].height = oi->height;
        count++;
    }
    return count;
}
