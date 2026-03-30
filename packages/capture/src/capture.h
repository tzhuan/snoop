#ifndef SNOOP_CAPTURE_H
#define SNOOP_CAPTURE_H

#include <stdint.h>

typedef struct CaptureState CaptureState;
typedef void (*frame_callback_t)(void *userdata, uint8_t *data,
                                 int width, int height, int stride);

CaptureState *capture_create(void);
int capture_start(CaptureState *state, frame_callback_t cb, void *userdata);
void capture_stop(CaptureState *state);
void capture_destroy(CaptureState *state);
void capture_set_region(CaptureState *state, int x, int y, int w, int h);
void capture_set_rate(CaptureState *state, int fps);
void capture_suspend(CaptureState *state);
void capture_resume(CaptureState *state);
void capture_snap(CaptureState *state);

#endif
