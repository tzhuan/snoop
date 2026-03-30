#include <node_api.h>
#include <stdlib.h>
#include <string.h>
#include "capture.h"

/* Per-instance data attached to the JS handle object */
typedef struct {
    CaptureState *capture;
    napi_threadsafe_function tsfn;
    napi_ref handle_ref;
} AddonCapture;

/* Data passed from PipeWire thread to JS thread via threadsafe function */
typedef struct {
    uint8_t *data;
    int width;
    int height;
    int stride;
} FrameData;

/* Called on the JS main thread when a frame arrives */
static void frame_call_js(napi_env env, napi_value js_cb, void *context, void *data) {
    FrameData *frame = data;
    if (!env || !frame) {
        free(frame->data);
        free(frame);
        return;
    }

    napi_value argv[3];
    /* Create ArrayBuffer wrapping a copy of the frame data */
    void *ab_data = NULL;
    size_t byte_length = (size_t)frame->stride * frame->height;
    napi_create_arraybuffer(env, byte_length, &ab_data, &argv[0]);
    memcpy(ab_data, frame->data, byte_length);

    napi_create_int32(env, frame->width, &argv[1]);
    napi_create_int32(env, frame->height, &argv[2]);

    napi_value undefined;
    napi_get_undefined(env, &undefined);
    napi_call_function(env, undefined, js_cb, 3, argv, NULL);

    free(frame->data);
    free(frame);
}

/* Native frame callback — called from PipeWire thread */
static void native_frame_cb(void *userdata, uint8_t *data,
                             int width, int height, int stride) {
    AddonCapture *ac = userdata;
    if (!ac->tsfn) return;

    size_t byte_length = (size_t)stride * height;
    FrameData *frame = malloc(sizeof(FrameData));
    if (!frame) return;
    frame->data = malloc(byte_length);
    if (!frame->data) { free(frame); return; }
    memcpy(frame->data, data, byte_length);
    frame->width = width;
    frame->height = height;
    frame->stride = stride;

    napi_call_threadsafe_function(ac->tsfn, frame, napi_tsfn_nonblocking);
}

/* Release the threadsafe function ref */
static void capture_release_tsfn(AddonCapture *ac) {
    if (ac->tsfn) {
        napi_release_threadsafe_function(ac->tsfn, napi_tsfn_release);
        ac->tsfn = NULL;
    }
}

/* GC destructor for the wrapped CaptureState */
static void capture_destructor(napi_env env, void *data, void *hint) {
    AddonCapture *ac = data;
    capture_release_tsfn(ac);
    capture_destroy(ac->capture);
    free(ac);
}

/* Helper: unwrap AddonCapture from `this` */
static AddonCapture *unwrap_capture(napi_env env, napi_callback_info info) {
    napi_value this_val;
    napi_get_cb_info(env, info, NULL, NULL, &this_val, NULL);
    AddonCapture *ac = NULL;
    napi_unwrap(env, this_val, (void **)&ac);
    return ac;
}

/* ---------- JS methods ---------- */

static napi_value js_on_frame(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], this_val;
    napi_get_cb_info(env, info, &argc, argv, &this_val, NULL);

    AddonCapture *ac = NULL;
    napi_unwrap(env, this_val, (void **)&ac);
    if (!ac) return NULL;

    /* Release previous tsfn if any */
    capture_release_tsfn(ac);

    /* Create threadsafe function from the JS callback */
    napi_value resource_name;
    napi_create_string_utf8(env, "snoop_capture_frame", NAPI_AUTO_LENGTH, &resource_name);
    napi_create_threadsafe_function(env, argv[0], NULL, resource_name,
                                    0, 1, NULL, NULL, NULL,
                                    frame_call_js, &ac->tsfn);
    return NULL;
}

static napi_value js_start(napi_env env, napi_callback_info info) {
    AddonCapture *ac = unwrap_capture(env, info);
    if (!ac) return NULL;
    int ret = capture_start(ac->capture, native_frame_cb, ac);
    napi_value result;
    napi_create_int32(env, ret, &result);
    return result;
}

static napi_value js_stop(napi_env env, napi_callback_info info) {
    AddonCapture *ac = unwrap_capture(env, info);
    if (!ac) return NULL;
    capture_release_tsfn(ac);
    capture_stop(ac->capture);
    return NULL;
}

static napi_value js_set_region(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value argv[4];
    napi_value this_val;
    napi_get_cb_info(env, info, &argc, argv, &this_val, NULL);

    AddonCapture *ac = NULL;
    napi_unwrap(env, this_val, (void **)&ac);
    if (!ac) return NULL;

    int x, y, w, h;
    napi_get_value_int32(env, argv[0], &x);
    napi_get_value_int32(env, argv[1], &y);
    napi_get_value_int32(env, argv[2], &w);
    napi_get_value_int32(env, argv[3], &h);
    capture_set_region(ac->capture, x, y, w, h);
    return NULL;
}

static napi_value js_set_rate(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_value this_val;
    napi_get_cb_info(env, info, &argc, argv, &this_val, NULL);

    AddonCapture *ac = NULL;
    napi_unwrap(env, this_val, (void **)&ac);
    if (!ac) return NULL;

    int fps;
    napi_get_value_int32(env, argv[0], &fps);
    capture_set_rate(ac->capture, fps);
    return NULL;
}

static napi_value js_suspend(napi_env env, napi_callback_info info) {
    AddonCapture *ac = unwrap_capture(env, info);
    if (ac) capture_suspend(ac->capture);
    return NULL;
}

static napi_value js_resume(napi_env env, napi_callback_info info) {
    AddonCapture *ac = unwrap_capture(env, info);
    if (ac) capture_resume(ac->capture);
    return NULL;
}

static napi_value js_snap(napi_env env, napi_callback_info info) {
    AddonCapture *ac = unwrap_capture(env, info);
    if (ac) capture_snap(ac->capture);
    return NULL;
}

/* ---------- createCapture() ---------- */

static napi_value js_create_capture(napi_env env, napi_callback_info info) {
    CaptureState *cap = capture_create();
    if (!cap) {
        napi_throw_error(env, NULL, "Failed to create capture state");
        return NULL;
    }

    AddonCapture *ac = calloc(1, sizeof(AddonCapture));
    ac->capture = cap;

    /* Create the handle object with methods */
    napi_value obj;
    napi_create_object(env, &obj);
    napi_wrap(env, obj, ac, capture_destructor, NULL, NULL);

    /* Attach methods */
    napi_value fn;
    #define ADD_METHOD(name, func) \
        napi_create_function(env, name, NAPI_AUTO_LENGTH, func, NULL, &fn); \
        napi_set_named_property(env, obj, name, fn);

    ADD_METHOD("onFrame", js_on_frame)
    ADD_METHOD("start", js_start)
    ADD_METHOD("stop", js_stop)
    ADD_METHOD("setRegion", js_set_region)
    ADD_METHOD("setRate", js_set_rate)
    ADD_METHOD("suspend", js_suspend)
    ADD_METHOD("resume", js_resume)
    ADD_METHOD("snap", js_snap)

    #undef ADD_METHOD

    return obj;
}

/* ---------- Module init ---------- */

NAPI_MODULE_INIT() {
    napi_value fn;
    napi_create_function(env, "createCapture", NAPI_AUTO_LENGTH,
                         js_create_capture, NULL, &fn);
    napi_set_named_property(env, exports, "createCapture", fn);
    return exports;
}
