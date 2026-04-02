#include "capture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct CaptureState {
    /* Virtual screen geometry (spans all monitors) */
    int vscreen_x, vscreen_y;
    int vscreen_w, vscreen_h;

    /* Threading */
    HANDLE thread;
    volatile int running;

    /* Region crop */
    int has_region;
    int region_x, region_y, region_w, region_h;

    /* Rate limiting */
    int target_fps;
    LONGLONG min_frame_interval;
    LONGLONG last_frame_time;
    LONGLONG perf_freq;

    /* Suspend/snap */
    int suspended;
    int snap_requested;

    /* Callbacks */
    frame_callback_t callback;
    error_callback_t error_callback;
    void *userdata;
    void *error_userdata;
};

static LONGLONG get_time(CaptureState *state) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000000000LL / state->perf_freq;
}

static DWORD WINAPI capture_thread(LPVOID arg) {
    CaptureState *state = arg;

    /* Create compatible DC and bitmap for capture */
    HDC screen_dc = GetDC(NULL);
    if (!screen_dc) return 1;

    HDC mem_dc = CreateCompatibleDC(screen_dc);
    if (!mem_dc) {
        ReleaseDC(NULL, screen_dc);
        return 1;
    }

    /* Determine capture area and allocate bitmap */
    int cap_x, cap_y, cap_w, cap_h;

    BITMAPINFOHEADER bmi = {0};
    bmi.biSize = sizeof(bmi);
    bmi.biPlanes = 1;
    bmi.biBitCount = 32;
    bmi.biCompression = BI_RGB;

    HBITMAP hbmp = NULL;
    uint8_t *bmp_data = NULL;
    int prev_rx = 0, prev_ry = 0, prev_rw = 0, prev_rh = 0;
    int prev_has_region = 0;

    while (state->running) {
        /* Rate limiting */
        if (state->min_frame_interval > 0) {
            LONGLONG now = get_time(state);
            LONGLONG elapsed = now - state->last_frame_time;
            if (elapsed < state->min_frame_interval) {
                LONGLONG remaining_ms = (state->min_frame_interval - elapsed) / 1000000LL;
                if (remaining_ms > 0) Sleep((DWORD)remaining_ms);
                else Sleep(0);
                continue;
            }
            state->last_frame_time = get_time(state);
        }

        /* Suspend check */
        if (state->suspended && !state->snap_requested) {
            Sleep(10);
            continue;
        }
        state->snap_requested = 0;

        /* Refresh virtual screen geometry (handles hotplug) */
        state->vscreen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        state->vscreen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        state->vscreen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        state->vscreen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        /* Determine capture rect */
        if (state->has_region) {
            cap_x = state->region_x;
            cap_y = state->region_y;
            cap_w = state->region_w;
            cap_h = state->region_h;
            /* Clamp to virtual screen */
            if (cap_x < state->vscreen_x) {
                cap_w -= (state->vscreen_x - cap_x);
                cap_x = state->vscreen_x;
            }
            if (cap_y < state->vscreen_y) {
                cap_h -= (state->vscreen_y - cap_y);
                cap_y = state->vscreen_y;
            }
            if (cap_x + cap_w > state->vscreen_x + state->vscreen_w)
                cap_w = state->vscreen_x + state->vscreen_w - cap_x;
            if (cap_y + cap_h > state->vscreen_y + state->vscreen_h)
                cap_h = state->vscreen_y + state->vscreen_h - cap_y;
            if (cap_w <= 0 || cap_h <= 0) continue;
        } else {
            cap_x = state->vscreen_x;
            cap_y = state->vscreen_y;
            cap_w = state->vscreen_w;
            cap_h = state->vscreen_h;
        }

        /* Recreate bitmap if size changed */
        int need_recreate = !hbmp ||
            (state->has_region != prev_has_region) ||
            (state->has_region && (cap_w != prev_rw || cap_h != prev_rh)) ||
            (!state->has_region && (cap_w != prev_rw || cap_h != prev_rh));

        if (need_recreate) {
            if (hbmp) DeleteObject(hbmp);
            bmi.biWidth = cap_w;
            bmi.biHeight = -cap_h; /* Top-down DIB */
            hbmp = CreateDIBSection(mem_dc, (BITMAPINFO *)&bmi,
                                    DIB_RGB_COLORS, (void **)&bmp_data, NULL, 0);
            if (!hbmp) continue;
            SelectObject(mem_dc, hbmp);
            prev_has_region = state->has_region;
            prev_rw = cap_w;
            prev_rh = cap_h;
        }

        /* Capture */
        if (!BitBlt(mem_dc, 0, 0, cap_w, cap_h, screen_dc, cap_x, cap_y, SRCCOPY))
            continue;
        GdiFlush();

        if (!state->callback) continue;

        /* Convert BGRA → RGBA in-place copy */
        int stride = cap_w * 4;
        uint8_t *rgba = malloc(stride * cap_h);
        if (!rgba) continue;

        for (int row = 0; row < cap_h; row++) {
            uint8_t *src = bmp_data + row * stride;
            uint8_t *dst = rgba + row * stride;
            for (int col = 0; col < cap_w; col++) {
                dst[col * 4 + 0] = src[col * 4 + 2]; /* R */
                dst[col * 4 + 1] = src[col * 4 + 1]; /* G */
                dst[col * 4 + 2] = src[col * 4 + 0]; /* B */
                dst[col * 4 + 3] = 255;              /* A */
            }
        }

        state->callback(state->userdata, rgba, cap_w, cap_h, stride);
        free(rgba);
    }

    if (hbmp) DeleteObject(hbmp);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);
    return 0;
}

CaptureState *capture_create(void) {
    CaptureState *state = calloc(1, sizeof(CaptureState));
    if (!state) return NULL;
    state->target_fps = 30;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    state->perf_freq = freq.QuadPart;
    state->min_frame_interval = 1000000000LL / 30;
    return state;
}

int capture_start(CaptureState *state, frame_callback_t cb, void *userdata) {
    state->callback = cb;
    state->userdata = userdata;

    state->vscreen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    state->vscreen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    state->vscreen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    state->vscreen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    fprintf(stderr, "capture: BitBlt started (virtual screen %d,%d %dx%d)\n",
            state->vscreen_x, state->vscreen_y,
            state->vscreen_w, state->vscreen_h);

    state->running = 1;
    state->thread = CreateThread(NULL, 0, capture_thread, state, 0, NULL);
    if (!state->thread) {
        state->running = 0;
        return -1;
    }
    return 0;
}

void capture_stop(CaptureState *state) {
    if (!state || !state->running) return;

    state->running = 0;
    WaitForSingleObject(state->thread, INFINITE);
    CloseHandle(state->thread);
    state->thread = NULL;
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
    state->min_frame_interval = fps > 0 ? 1000000000LL / fps : 0;
}

void capture_suspend(CaptureState *state) { state->suspended = 1; }
void capture_resume(CaptureState *state) { state->suspended = 0; }
void capture_snap(CaptureState *state) { state->snap_requested = 1; }

void capture_on_error(CaptureState *state, error_callback_t cb, void *userdata) {
    state->error_callback = cb;
    state->error_userdata = userdata;
}

/* BitBlt captures any rect across all monitors — no-op */
int capture_set_display(CaptureState *state, const char *display_id) {
    (void)state; (void)display_id;
    return 0;
}

int capture_list_displays(CaptureState *state,
                          CaptureDisplayInfo *out, int max_displays) {
    (void)state; (void)out; (void)max_displays;
    return -1; /* Not needed — BitBlt captures across all monitors */
}
