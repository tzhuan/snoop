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

/* Error callback — called when the capture stream encounters a fatal error.
 * The backend may call this from any thread. */
typedef void (*error_callback_t)(void *userdata, const char *message);
void capture_on_error(CaptureState *state, error_callback_t cb, void *userdata);

/* Multi-monitor support.
 * display_id meaning is platform-specific:
 *   - macOS: CGDirectDisplayID as decimal string
 *   - Wayland/PipeWire: Mutter connector string (e.g. "DP-1")
 *   - X11: no-op (root window spans all monitors)
 *   - Windows/DXGI: output index as decimal string
 * Returns 0 on success, -1 on error. */
int capture_set_display(CaptureState *state, const char *display_id);

/* Display enumeration (Linux/PipeWire only currently).
 * Writes up to max_displays entries. Returns number written, or -1 on error. */
typedef struct {
    char connector[64];
    int x, y, width, height;
} CaptureDisplayInfo;

int capture_list_displays(CaptureState *state,
                          CaptureDisplayInfo *out, int max_displays);

#endif
