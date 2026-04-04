#include "capture.h"

#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <mach/mach_time.h>

/* ---------- CaptureState ---------- */

struct CaptureState {
    /* ScreenCaptureKit */
    SCStream *stream;
    dispatch_queue_t queue;
    id delegate;
    int screen_width;
    int screen_height;

    /* Multi-monitor: 0 = main display (default) */
    uint32_t target_display_id;

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

    /* Error callback */
    error_callback_t error_callback;
    void *error_userdata;
};

static int64_t get_time_ns(void) {
    static mach_timebase_info_data_t info = {0};
    if (info.denom == 0) mach_timebase_info(&info);
    return (int64_t)(mach_absolute_time() * info.numer / info.denom);
}

/* ---------- SCStreamOutput delegate ---------- */

@interface SnoopStreamDelegate : NSObject <SCStreamOutput>
@property (assign) CaptureState *state;
@end

@implementation SnoopStreamDelegate

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
               ofType:(SCStreamOutputType)type {
    (void)stream;
    if (type != SCStreamOutputTypeScreen) return;

    CaptureState *state = self.state;
    if (!state || !state->callback) return;

    /* Rate limiting */
    if (state->min_frame_interval_ns > 0) {
        int64_t now = get_time_ns();
        if ((now - state->last_frame_time_ns) < state->min_frame_interval_ns) return;
        state->last_frame_time_ns = now;
    }

    /* Suspend check */
    if (state->suspended && !state->snap_requested) return;
    state->snap_requested = 0;

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) return;

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
    uint8_t *base = CVPixelBufferGetBaseAddress(imageBuffer);
    int stride = (int)CVPixelBufferGetBytesPerRow(imageBuffer);
    int width = (int)CVPixelBufferGetWidth(imageBuffer);
    int height = (int)CVPixelBufferGetHeight(imageBuffer);

    if (!base || width <= 0 || height <= 0) {
        CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
        return;
    }

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
        int out_stride = width * 4;
        uint8_t *rgba = malloc(out_stride * height);
        if (rgba) {
            for (int row = 0; row < height; row++) {
                uint8_t *src = base + row * stride;
                uint8_t *dst = rgba + row * out_stride;
                for (int col = 0; col < width; col++) {
                    dst[col * 4 + 0] = src[col * 4 + 2];
                    dst[col * 4 + 1] = src[col * 4 + 1];
                    dst[col * 4 + 2] = src[col * 4 + 0];
                    dst[col * 4 + 3] = src[col * 4 + 3];
                }
            }
            state->callback(state->userdata, rgba, width, height, out_stride);
            free(rgba);
        }
    }

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

@end

/* ---------- Public API ---------- */

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

    @autoreleasepool {
        /* Get the main display */
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block SCDisplay *mainDisplay = nil;
        __block NSError *fetchError = nil;

        uint32_t targetId = state->target_display_id;
        [SCShareableContent getShareableContentWithCompletionHandler:
            ^(SCShareableContent *content, NSError *error) {
            if (error) {
                fetchError = error;
            } else {
                /* If a target display is set, find it; otherwise use main */
                SCDisplay *fallback = nil;
                for (SCDisplay *d in content.displays) {
                    if (targetId != 0 && d.displayID == targetId) {
                        mainDisplay = d;
                        break;
                    }
                    if (CGDisplayIsMain(d.displayID)) {
                        fallback = d;
                    }
                }
                if (!mainDisplay) {
                    /* Target not found (or not set) — fall back to main, then first */
                    mainDisplay = fallback ? fallback :
                        (content.displays.count > 0 ? content.displays[0] : nil);
                }
            }
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

        if (!mainDisplay) {
            fprintf(stderr, "capture: failed to get display%s%s\n",
                    fetchError ? ": " : "",
                    fetchError ? fetchError.localizedDescription.UTF8String : "");
            return -1;
        }

        state->screen_width = (int)mainDisplay.width;
        state->screen_height = (int)mainDisplay.height;

        /* Configure stream: BGRA, no cursor */
        SCContentFilter *filter = [[SCContentFilter alloc]
            initWithDisplay:mainDisplay excludingWindows:@[]];
        SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
        config.width = state->screen_width;
        config.height = state->screen_height;
        config.pixelFormat = kCVPixelFormatType_32BGRA;
        config.showsCursor = NO;
        config.minimumFrameInterval = CMTimeMake(1, state->target_fps);

        state->stream = [[SCStream alloc] initWithFilter:filter
                                           configuration:config
                                                delegate:nil];

        /* Create delegate and output queue */
        SnoopStreamDelegate *delegate = [[SnoopStreamDelegate alloc] init];
        delegate.state = state;
        state->delegate = delegate;
        state->queue = dispatch_queue_create("com.snoop.capture", DISPATCH_QUEUE_SERIAL);

        NSError *addError = nil;
        [state->stream addStreamOutput:delegate
                                  type:SCStreamOutputTypeScreen
                    sampleHandlerQueue:state->queue
                                 error:&addError];
        if (addError) {
            fprintf(stderr, "capture: addStreamOutput failed: %s\n",
                    addError.localizedDescription.UTF8String);
            return -1;
        }

        /* Start capture */
        __block BOOL started = NO;
        __block NSError *startError = nil;
        dispatch_semaphore_t startSem = dispatch_semaphore_create(0);
        [state->stream startCaptureWithCompletionHandler:^(NSError *error) {
            started = (error == nil);
            startError = error;
            dispatch_semaphore_signal(startSem);
        }];
        dispatch_semaphore_wait(startSem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));

        if (!started) {
            fprintf(stderr, "capture: startCapture failed%s%s\n",
                    startError ? ": " : "",
                    startError ? startError.localizedDescription.UTF8String : "");
            return -1;
        }

        state->running = 1;
        return 0;
    }
}

void capture_stop(CaptureState *state) {
    if (!state || !state->running) return;

    @autoreleasepool {
        state->running = 0;
        if (state->stream) {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            [state->stream stopCaptureWithCompletionHandler:^(NSError *error) {
                (void)error;
                dispatch_semaphore_signal(sem);
            }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
            state->stream = nil;
        }
        state->delegate = nil;
        state->queue = nil;
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

    /* Update SCStream sourceRect so the compositor only captures the viewport
     * region, reducing GPU/memory work. The delegate still receives cropped
     * frames in display-local coords matching the sourceRect. */
    if (state->running && state->stream) {
        /* Clamp to display bounds */
        int rx = x < 0 ? 0 : x;
        int ry = y < 0 ? 0 : y;
        int rw = w + (x < 0 ? x : 0);
        int rh = h + (y < 0 ? y : 0);
        if (rx + rw > state->screen_width) rw = state->screen_width - rx;
        if (ry + rh > state->screen_height) rh = state->screen_height - ry;
        if (rw <= 0 || rh <= 0) return;

        @autoreleasepool {
            SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
            config.sourceRect = CGRectMake(rx, ry, rw, rh);
            config.width = rw;
            config.height = rh;
            config.pixelFormat = kCVPixelFormatType_32BGRA;
            config.showsCursor = NO;
            config.minimumFrameInterval = CMTimeMake(1, state->target_fps > 0 ? state->target_fps : 30);
            [state->stream updateConfiguration:config completionHandler:^(NSError *error) {
                if (error) {
                    fprintf(stderr, "capture: updateConfiguration failed: %s\n",
                            error.localizedDescription.UTF8String);
                }
            }];
        }

        /* With sourceRect active, the delegate receives frames sized to the
         * region — disable the software crop since SCK already did it. */
        state->has_region = 0;
    }
}

void capture_set_rate(CaptureState *state, int fps) {
    state->target_fps = fps;
    state->min_frame_interval_ns = fps > 0 ? 1000000000LL / fps : 0;
}

void capture_suspend(CaptureState *state) { state->suspended = 1; }
void capture_resume(CaptureState *state) { state->suspended = 0; }
void capture_snap(CaptureState *state) { state->snap_requested = 1; }

void capture_on_error(CaptureState *state, error_callback_t cb, void *userdata) {
    if (!state) return;
    state->error_callback = cb;
    state->error_userdata = userdata;
}

int capture_set_display(CaptureState *state, const char *display_id) {
    if (!state || !display_id) return -1;
    state->target_display_id = (uint32_t)strtoul(display_id, NULL, 10);
    return 0;
}

int capture_list_displays(CaptureState *state,
                          CaptureDisplayInfo *out, int max_displays) {
    (void)state;
    if (!out || max_displays <= 0) return 0;

    /* Use CoreGraphics to enumerate displays */
    uint32_t count = 0;
    CGDirectDisplayID displays[32];
    if (CGGetActiveDisplayList(32, displays, &count) != kCGErrorSuccess)
        return -1;

    int n = (int)count < max_displays ? (int)count : max_displays;
    for (int i = 0; i < n; i++) {
        CGRect bounds = CGDisplayBounds(displays[i]);
        snprintf(out[i].connector, sizeof(out[i].connector), "%u", displays[i]);
        out[i].x = (int)bounds.origin.x;
        out[i].y = (int)bounds.origin.y;
        out[i].width = (int)bounds.size.width;
        out[i].height = (int)bounds.size.height;
    }
    return n;
}
