#include "capture.h"

#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <mach/mach_time.h>

struct CaptureState {
    /* CoreGraphics */
    CGDisplayStreamRef stream;
    dispatch_queue_t queue;
    int screen_width;
    int screen_height;

    /* State */
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
    static mach_timebase_info_data_t info = {0};
    if (info.denom == 0) mach_timebase_info(&info);
    return (int64_t)(mach_absolute_time() * info.numer / info.denom);
}

CaptureState *capture_create(void) {
    CaptureState *state = calloc(1, sizeof(CaptureState));
    if (!state) return NULL;
    state->target_fps = 30;
    state->min_frame_interval_ns = 1000000000LL / 30;
    return state;
}

int capture_start(CaptureState *state, frame_callback_t cb, void *userdata) {
    state->callback = cb;
    state->userdata = userdata;

    CGDirectDisplayID display = CGMainDisplayID();
    state->screen_width = (int)CGDisplayPixelsWide(display);
    state->screen_height = (int)CGDisplayPixelsHigh(display);

    state->queue = dispatch_queue_create("com.snoop.capture", DISPATCH_QUEUE_SERIAL);

    CGDisplayStreamFrameAvailableHandler handler =
        ^(CGDisplayStreamFrameStatus status, uint64_t displayTime,
          IOSurfaceRef surface, CGDisplayStreamUpdateRef updateRef) {
        (void)displayTime;
        (void)updateRef;

        if (status != kCGDisplayStreamFrameStatusFrameComplete) return;
        if (!state->callback) return;

        /* Rate limiting */
        if (state->min_frame_interval_ns > 0) {
            int64_t now = get_time_ns();
            if ((now - state->last_frame_time_ns) < state->min_frame_interval_ns) return;
            state->last_frame_time_ns = now;
        }

        /* Suspend check */
        if (state->suspended && !state->snap_requested) return;
        state->snap_requested = 0;

        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        uint8_t *base = IOSurfaceGetBaseAddress(surface);
        int stride = (int)IOSurfaceGetBytesPerRow(surface);
        int width = state->screen_width;
        int height = state->screen_height;

        /* BGRA→RGBA + optional region crop */
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
                    for (int row = 0; row < rh; row++) {
                        uint8_t *src = base + (ry + row) * stride + rx * 4;
                        uint8_t *dst = crop + row * crop_stride;
                        for (int col = 0; col < rw; col++) {
                            dst[col * 4 + 0] = src[col * 4 + 2];
                            dst[col * 4 + 1] = src[col * 4 + 1];
                            dst[col * 4 + 2] = src[col * 4 + 0];
                            dst[col * 4 + 3] = src[col * 4 + 3];
                        }
                    }
                    state->callback(state->userdata, crop, rw, rh, crop_stride);
                    free(crop);
                }
            }
        } else {
            uint8_t *rgba = malloc(width * 4 * height);
            if (rgba) {
                for (int row = 0; row < height; row++) {
                    uint8_t *src = base + row * stride;
                    uint8_t *dst = rgba + row * width * 4;
                    for (int col = 0; col < width; col++) {
                        dst[col * 4 + 0] = src[col * 4 + 2];
                        dst[col * 4 + 1] = src[col * 4 + 1];
                        dst[col * 4 + 2] = src[col * 4 + 0];
                        dst[col * 4 + 3] = src[col * 4 + 3];
                    }
                }
                state->callback(state->userdata, rgba, width, height, width * 4);
                free(rgba);
            }
        }

        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    };

    state->stream = CGDisplayStreamCreateWithDispatchQueue(
        display, state->screen_width, state->screen_height,
        'BGRA', NULL, state->queue, handler);
    if (!state->stream) return -1;

    CGError err = CGDisplayStreamStart(state->stream);
    if (err != kCGErrorSuccess) return -1;

    state->running = 1;
    return 0;
}

void capture_stop(CaptureState *state) {
    if (!state || !state->running) return;

    state->running = 0;
    if (state->stream) {
        CGDisplayStreamStop(state->stream);
        CFRelease(state->stream);
        state->stream = NULL;
    }
    if (state->queue) {
        dispatch_release(state->queue);
        state->queue = NULL;
    }
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
