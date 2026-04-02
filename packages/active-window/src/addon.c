#include <node_api.h>
#include "platform.h"

static napi_value GetActiveWindow(napi_env env, napi_callback_info info) {
    ActiveWindowInfo win = platform_get_active_window();
    if (!win.valid) {
        napi_value null_val;
        napi_get_null(env, &null_val);
        return null_val;
    }

    napi_value obj, val;
    napi_create_object(env, &obj);

    napi_create_int32(env, win.x, &val);
    napi_set_named_property(env, obj, "x", val);

    napi_create_int32(env, win.y, &val);
    napi_set_named_property(env, obj, "y", val);

    napi_create_int32(env, win.width, &val);
    napi_set_named_property(env, obj, "width", val);

    napi_create_int32(env, win.height, &val);
    napi_set_named_property(env, obj, "height", val);

    napi_create_string_utf8(env, win.title, NAPI_AUTO_LENGTH, &val);
    napi_set_named_property(env, obj, "title", val);

    return obj;
}

static napi_value GetCursorPosition(napi_env env, napi_callback_info info) {
    CursorPosition pos = platform_get_cursor_position();

    napi_value obj, val;
    napi_create_object(env, &obj);

    napi_create_int32(env, pos.x, &val);
    napi_set_named_property(env, obj, "x", val);

    napi_create_int32(env, pos.y, &val);
    napi_set_named_property(env, obj, "y", val);

    return obj;
}

static napi_value SetCursorPosition(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);

    int32_t x, y;
    napi_get_value_int32(env, argv[0], &x);
    napi_get_value_int32(env, argv[1], &y);

    int result = platform_set_cursor_position(x, y);

    napi_value ret;
    napi_get_boolean(env, result == 0, &ret);
    return ret;
}

NAPI_MODULE_INIT() {
    napi_value fn;

    napi_create_function(env, "getActiveWindow", NAPI_AUTO_LENGTH,
                         GetActiveWindow, NULL, &fn);
    napi_set_named_property(env, exports, "getActiveWindow", fn);

    napi_create_function(env, "getCursorPosition", NAPI_AUTO_LENGTH,
                         GetCursorPosition, NULL, &fn);
    napi_set_named_property(env, exports, "getCursorPosition", fn);

    napi_create_function(env, "setCursorPosition", NAPI_AUTO_LENGTH,
                         SetCursorPosition, NULL, &fn);
    napi_set_named_property(env, exports, "setCursorPosition", fn);

    return exports;
}
