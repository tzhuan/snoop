#include "capture.h"

#include <dbus/dbus.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define MUTTER_SCREENCAST_BUS   "org.gnome.Mutter.ScreenCast"
#define MUTTER_SCREENCAST_PATH  "/org/gnome/Mutter/ScreenCast"
#define MUTTER_SCREENCAST_IFACE "org.gnome.Mutter.ScreenCast"
#define MUTTER_SESSION_IFACE    "org.gnome.Mutter.ScreenCast.Session"
#define MUTTER_STREAM_IFACE     "org.gnome.Mutter.ScreenCast.Stream"

#define PORTAL_BUS              "org.freedesktop.portal.Desktop"
#define PORTAL_PATH             "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST_IFACE    "org.freedesktop.portal.Request"

/* Portal capture uses session_path for the portal session handle */
#define CAPTURE_METHOD_NONE   0
#define CAPTURE_METHOD_MUTTER 1
#define CAPTURE_METHOD_PORTAL 2

struct CaptureState {
    /* D-Bus */
    DBusConnection *dbus;
    char *session_path;
    char *stream_path;
    uint32_t pipewire_node_id;
    int capture_method;
    char *display_id;  /* Mutter connector string, e.g. "DP-1" */

    /* PipeWire */
    struct pw_main_loop *pw_loop;
    struct pw_context *pw_context;
    struct pw_core *pw_core;
    struct pw_stream *pw_stream;
    struct spa_hook stream_listener;
    int stream_width;
    int stream_height;
    enum spa_video_format stream_format;

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

    /* Error callback */
    error_callback_t error_callback;
    void *error_userdata;
};

/* ---------- helpers ---------- */

static int64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static DBusMessage *dbus_call_sync(DBusConnection *conn,
                                   const char *dest, const char *path,
                                   const char *iface, const char *method,
                                   int first_arg_type, ...) {
    DBusMessage *msg = dbus_message_new_method_call(dest, path, iface, method);
    if (!msg) return NULL;

    if (first_arg_type != DBUS_TYPE_INVALID) {
        va_list args;
        va_start(args, first_arg_type);
        dbus_message_append_args_valist(msg, first_arg_type, args);
        va_end(args);
    }

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        conn, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "D-Bus call %s.%s failed: %s\n", iface, method, err.message);
        dbus_error_free(&err);
        return NULL;
    }
    return reply;
}

/* ---------- D-Bus: Mutter ScreenCast session setup ---------- */

static int dbus_create_session(CaptureState *state) {
    /* CreateSession(properties: a{sv}) -> (session_path: o) */
    DBusMessage *msg = dbus_message_new_method_call(
        MUTTER_SCREENCAST_BUS, MUTTER_SCREENCAST_PATH,
        MUTTER_SCREENCAST_IFACE, "CreateSession");
    if (!msg) return -1;

    /* Append empty a{sv} properties dict */
    DBusMessageIter iter, dict_iter;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
    dbus_message_iter_close_container(&iter, &dict_iter);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        state->dbus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "CreateSession failed: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    if (!reply) return -1;

    const char *session_path = NULL;
    if (!dbus_message_get_args(reply, NULL,
                               DBUS_TYPE_OBJECT_PATH, &session_path,
                               DBUS_TYPE_INVALID)) {
        dbus_message_unref(reply);
        return -1;
    }
    state->session_path = strdup(session_path);
    dbus_message_unref(reply);
    return 0;
}

static int dbus_record_monitor(CaptureState *state) {
    /* RecordMonitor(connector: s, properties: a{sv}) -> (stream_path: o) */
    DBusMessage *msg = dbus_message_new_method_call(
        MUTTER_SCREENCAST_BUS, state->session_path,
        MUTTER_SESSION_IFACE, "RecordMonitor");
    if (!msg) return -1;

    /* Empty connector string = primary monitor; set via capture_set_display() */
    const char *connector = state->display_id ? state->display_id : "";
    DBusMessageIter iter, dict_iter, entry_iter, variant_iter;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &connector);

    /* Properties dict: cursor-mode = 0 (hidden) */
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
    {
        const char *key = "cursor-mode";
        uint32_t cursor_mode = 0; /* 0=Hidden, 1=Embedded, 2=Metadata */
        dbus_message_iter_open_container(&dict_iter, DBUS_TYPE_DICT_ENTRY, NULL, &entry_iter);
        dbus_message_iter_append_basic(&entry_iter, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry_iter, DBUS_TYPE_VARIANT, "u", &variant_iter);
        dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_UINT32, &cursor_mode);
        dbus_message_iter_close_container(&entry_iter, &variant_iter);
        dbus_message_iter_close_container(&dict_iter, &entry_iter);
    }
    dbus_message_iter_close_container(&iter, &dict_iter);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        state->dbus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "RecordMonitor failed: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }

    const char *stream_path = NULL;
    if (!dbus_message_get_args(reply, NULL,
                               DBUS_TYPE_OBJECT_PATH, &stream_path,
                               DBUS_TYPE_INVALID)) {
        dbus_message_unref(reply);
        return -1;
    }
    state->stream_path = strdup(stream_path);
    dbus_message_unref(reply);
    return 0;
}

static int dbus_start_and_get_node_id(CaptureState *state) {
    /*
     * Subscribe to PipeWireStreamAdded signal on the STREAM object.
     * Use a broad match (no sender filter) to avoid missing it.
     */
    char match_rule[512];
    snprintf(match_rule, sizeof(match_rule),
        "type='signal',interface='%s',member='PipeWireStreamAdded',path='%s'",
        MUTTER_STREAM_IFACE, state->stream_path);

    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(state->dbus, match_rule, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "capture: add_match failed: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }
    dbus_connection_flush(state->dbus);

    /*
     * Send Start as a non-blocking call so we don't consume the signal
     * inside dbus_connection_send_with_reply_and_block.
     */
    DBusMessage *start_msg = dbus_message_new_method_call(
        MUTTER_SCREENCAST_BUS, state->session_path,
        MUTTER_SESSION_IFACE, "Start");
    if (!start_msg) return -1;

    dbus_connection_send(state->dbus, start_msg, NULL);
    dbus_connection_flush(state->dbus);
    dbus_message_unref(start_msg);

    /* Pump the connection for the signal and the Start reply (up to 3 seconds) */
    int64_t deadline = get_time_ns() + 3000000000LL;
    int got_node_id = 0;

    while (get_time_ns() < deadline) {
        dbus_connection_read_write(state->dbus, 100);
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(state->dbus)) != NULL) {
            if (dbus_message_is_signal(msg, MUTTER_STREAM_IFACE, "PipeWireStreamAdded")) {
                uint32_t node_id = 0;
                if (dbus_message_get_args(msg, NULL,
                                           DBUS_TYPE_UINT32, &node_id,
                                           DBUS_TYPE_INVALID)) {
                    state->pipewire_node_id = node_id;
                    got_node_id = 1;
                }
            }
            dbus_message_unref(msg);
            if (got_node_id) break;
        }
        if (got_node_id) break;
    }

    dbus_bus_remove_match(state->dbus, match_rule, NULL);

    if (!got_node_id) {
        fprintf(stderr, "capture: timed out waiting for PipeWireStreamAdded\n");
        return -1;
    }
    return 0;
}

/* ---------- D-Bus: xdg-desktop-portal ScreenCast session setup ---------- */

/*
 * Portal uses an async Request/Response pattern. Each method returns a
 * request object path. We subscribe to the Response signal on that path,
 * then pump the D-Bus connection until we get the response.
 */

/* Wait for a Response signal on the given request path.
 * Returns the response_code (0 = success), or -1 on timeout.
 * If results_out is provided, the results dict message is NOT unref'd
 * and the caller must extract data from it. */
static int portal_wait_response(CaptureState *state, const char *request_path,
                                DBusMessage **response_out) {
    char match_rule[512];
    snprintf(match_rule, sizeof(match_rule),
        "type='signal',interface='%s',member='Response',path='%s'",
        PORTAL_REQUEST_IFACE, request_path);

    DBusError err;
    dbus_error_init(&err);
    dbus_bus_add_match(state->dbus, match_rule, &err);
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
        return -1;
    }
    dbus_connection_flush(state->dbus);

    int64_t deadline = get_time_ns() + 30000000000LL; /* 30s for user interaction */
    int result = -1;

    while (get_time_ns() < deadline) {
        dbus_connection_read_write(state->dbus, 200);
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(state->dbus)) != NULL) {
            if (dbus_message_is_signal(msg, PORTAL_REQUEST_IFACE, "Response")) {
                uint32_t response_code = 2;
                DBusMessageIter iter;
                dbus_message_iter_init(msg, &iter);
                if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32) {
                    dbus_message_iter_get_basic(&iter, &response_code);
                }
                if (response_code == 0) {
                    result = 0;
                    if (response_out) {
                        *response_out = msg; /* caller takes ownership */
                    } else {
                        dbus_message_unref(msg);
                    }
                } else {
                    fprintf(stderr, "capture: portal Response code=%u\n", response_code);
                    dbus_message_unref(msg);
                    result = (int)response_code;
                }
                goto done;
            }
            dbus_message_unref(msg);
        }
    }

done:
    dbus_bus_remove_match(state->dbus, match_rule, NULL);
    return result;
}

/* Helper: append a string variant to a dict entry */
static void dict_append_string(DBusMessageIter *dict, const char *key, const char *val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

/* Helper: append a uint32 variant to a dict entry */
static void dict_append_uint32(DBusMessageIter *dict, const char *key, uint32_t val) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &val);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

static int portal_create_session(CaptureState *state) {
    DBusMessage *msg = dbus_message_new_method_call(
        PORTAL_BUS, PORTAL_PATH,
        PORTAL_SCREENCAST_IFACE, "CreateSession");
    if (!msg) return -1;

    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dict_append_string(&dict, "handle_token", "snoop_session");
    dict_append_string(&dict, "session_handle_token", "snoop_capture");
    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        state->dbus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "capture: portal CreateSession failed: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }

    /* Get request handle from reply */
    const char *request_path = NULL;
    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path,
                          DBUS_TYPE_INVALID);
    char *req_path = strdup(request_path);
    dbus_message_unref(reply);

    /* Wait for Response to get session handle */
    DBusMessage *response = NULL;
    int ret = portal_wait_response(state, req_path, &response);
    free(req_path);
    if (ret != 0 || !response) return -1;

    /* Extract session_handle from results dict */
    DBusMessageIter resp_iter, results_iter;
    dbus_message_iter_init(response, &resp_iter);
    dbus_message_iter_next(&resp_iter); /* skip response_code */
    if (dbus_message_iter_get_arg_type(&resp_iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&resp_iter, &results_iter);
        while (dbus_message_iter_get_arg_type(&results_iter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry_iter, val_iter;
            const char *key;
            dbus_message_iter_recurse(&results_iter, &entry_iter);
            dbus_message_iter_get_basic(&entry_iter, &key);
            dbus_message_iter_next(&entry_iter);
            dbus_message_iter_recurse(&entry_iter, &val_iter);
            if (strcmp(key, "session_handle") == 0) {
                const char *handle;
                dbus_message_iter_get_basic(&val_iter, &handle);
                state->session_path = strdup(handle);
            }
            dbus_message_iter_next(&results_iter);
        }
    }
    dbus_message_unref(response);

    if (!state->session_path) {
        fprintf(stderr, "capture: portal no session_handle in response\n");
        return -1;
    }
    fprintf(stderr, "capture: portal session=%s\n", state->session_path);
    return 0;
}

static int portal_select_sources(CaptureState *state) {
    DBusMessage *msg = dbus_message_new_method_call(
        PORTAL_BUS, PORTAL_PATH,
        PORTAL_SCREENCAST_IFACE, "SelectSources");
    if (!msg) return -1;

    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &state->session_path);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dict_append_string(&dict, "handle_token", "snoop_select");
    dict_append_uint32(&dict, "types", 1);        /* 1 = monitor */
    dict_append_uint32(&dict, "cursor_mode", 2);  /* 2 = hidden */
    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        state->dbus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "capture: portal SelectSources failed: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }

    const char *request_path = NULL;
    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path,
                          DBUS_TYPE_INVALID);
    char *req_path = strdup(request_path);
    dbus_message_unref(reply);

    int ret = portal_wait_response(state, req_path, NULL);
    free(req_path);
    return ret;
}

static int portal_start(CaptureState *state) {
    DBusMessage *msg = dbus_message_new_method_call(
        PORTAL_BUS, PORTAL_PATH,
        PORTAL_SCREENCAST_IFACE, "Start");
    if (!msg) return -1;

    const char *parent_window = "";
    DBusMessageIter iter, dict;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &state->session_path);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &parent_window);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dict_append_string(&dict, "handle_token", "snoop_start");
    dbus_message_iter_close_container(&iter, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(
        state->dbus, msg, 5000, &err);
    dbus_message_unref(msg);

    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "capture: portal Start failed: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }

    const char *request_path = NULL;
    dbus_message_get_args(reply, NULL,
                          DBUS_TYPE_OBJECT_PATH, &request_path,
                          DBUS_TYPE_INVALID);
    char *req_path = strdup(request_path);
    dbus_message_unref(reply);

    /* Wait for Response — this may show a user dialog on some desktops */
    DBusMessage *response = NULL;
    int ret = portal_wait_response(state, req_path, &response);
    free(req_path);
    if (ret != 0 || !response) return -1;

    /* Extract PipeWire node_id from streams: a(ua{sv}) */
    DBusMessageIter resp_iter, results_iter;
    dbus_message_iter_init(response, &resp_iter);
    dbus_message_iter_next(&resp_iter); /* skip response_code */
    int found = 0;

    if (dbus_message_iter_get_arg_type(&resp_iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&resp_iter, &results_iter);
        while (dbus_message_iter_get_arg_type(&results_iter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry_iter, val_iter;
            const char *key;
            dbus_message_iter_recurse(&results_iter, &entry_iter);
            dbus_message_iter_get_basic(&entry_iter, &key);
            dbus_message_iter_next(&entry_iter);
            dbus_message_iter_recurse(&entry_iter, &val_iter);

            if (strcmp(key, "streams") == 0 &&
                dbus_message_iter_get_arg_type(&val_iter) == DBUS_TYPE_ARRAY) {
                DBusMessageIter streams_iter;
                dbus_message_iter_recurse(&val_iter, &streams_iter);
                if (dbus_message_iter_get_arg_type(&streams_iter) == DBUS_TYPE_STRUCT) {
                    DBusMessageIter struct_iter;
                    dbus_message_iter_recurse(&streams_iter, &struct_iter);
                    if (dbus_message_iter_get_arg_type(&struct_iter) == DBUS_TYPE_UINT32) {
                        dbus_message_iter_get_basic(&struct_iter,
                                                    &state->pipewire_node_id);
                        found = 1;
                    }
                }
            }
            dbus_message_iter_next(&results_iter);
        }
    }
    dbus_message_unref(response);

    if (!found) {
        fprintf(stderr, "capture: portal no streams in Start response\n");
        return -1;
    }
    return 0;
}

static int setup_portal(CaptureState *state) {
    fprintf(stderr, "capture: trying xdg-desktop-portal...\n");
    if (portal_create_session(state) < 0) return -1;
    if (portal_select_sources(state) < 0) return -1;
    if (portal_start(state) < 0) return -1;
    state->capture_method = CAPTURE_METHOD_PORTAL;
    return 0;
}

/* ---------- D-Bus: try Mutter first, then portal ---------- */

static int setup_mutter(CaptureState *state) {
    fprintf(stderr, "capture: trying Mutter ScreenCast...\n");
    if (dbus_create_session(state) < 0) return -1;
    if (dbus_record_monitor(state) < 0) return -1;
    if (dbus_start_and_get_node_id(state) < 0) return -1;
    state->capture_method = CAPTURE_METHOD_MUTTER;
    return 0;
}

static void dbus_stop_session(CaptureState *state) {
    if (!state->dbus || !state->session_path) return;
    if (state->capture_method == CAPTURE_METHOD_MUTTER) {
        DBusMessage *reply = dbus_call_sync(
            state->dbus,
            MUTTER_SCREENCAST_BUS, state->session_path,
            MUTTER_SESSION_IFACE, "Stop",
            DBUS_TYPE_INVALID);
        if (reply) dbus_message_unref(reply);
    }
    /* Portal sessions are automatically closed when the D-Bus connection drops */
}

/* ---------- PipeWire stream callbacks ---------- */

static void on_stream_param_changed(void *userdata, uint32_t id,
                                    const struct spa_pod *param) {
    CaptureState *state = userdata;
    if (!param || id != SPA_PARAM_Format) return;

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0) return;

    state->stream_width = info.size.width;
    state->stream_height = info.size.height;
    state->stream_format = info.format;
    fprintf(stderr, "capture: stream format %dx%d fmt=%d\n",
            state->stream_width, state->stream_height, info.format);

    /* Set buffer requirements */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_MemPtr));
    pw_stream_update_params(state->pw_stream, params, 1);
}

static void on_stream_process(void *userdata) {
    CaptureState *state = userdata;

    struct pw_buffer *pw_buf = pw_stream_dequeue_buffer(state->pw_stream);
    if (!pw_buf) return;

    struct spa_buffer *spa_buf = pw_buf->buffer;
    uint8_t *data = spa_buf->datas[0].data;
    if (!data) {
        pw_stream_queue_buffer(state->pw_stream, pw_buf);
        return;
    }

    int width = state->stream_width;
    int height = state->stream_height;
    int stride = spa_buf->datas[0].chunk->stride;

    /* Rate limiting */
    if (state->min_frame_interval_ns > 0) {
        int64_t now = get_time_ns();
        if ((now - state->last_frame_time_ns) < state->min_frame_interval_ns) {
            pw_stream_queue_buffer(state->pw_stream, pw_buf);
            return;
        }
        state->last_frame_time_ns = now;
    }

    /* Suspend check */
    if (state->suspended && !state->snap_requested) {
        pw_stream_queue_buffer(state->pw_stream, pw_buf);
        return;
    }
    state->snap_requested = 0;

    /* Determine if we need BGRA→RGBA conversion */
    int need_swap = (state->stream_format == SPA_VIDEO_FORMAT_BGRA ||
                     state->stream_format == SPA_VIDEO_FORMAT_BGRx);

    /* Copy a row, optionally swapping B and R channels */
    #define COPY_ROW(dst, src, pixels) do { \
        if (need_swap) { \
            for (int _i = 0; _i < (pixels); _i++) { \
                (dst)[_i*4+0] = (src)[_i*4+2]; \
                (dst)[_i*4+1] = (src)[_i*4+1]; \
                (dst)[_i*4+2] = (src)[_i*4+0]; \
                (dst)[_i*4+3] = 255;           \
            } \
        } else { \
            memcpy((dst), (src), (pixels) * 4); \
        } \
    } while(0)

    /* Region crop + format conversion */
    if (state->callback) {
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
                        COPY_ROW(crop + row * crop_stride,
                                 data + (ry + row) * stride + rx * 4,
                                 rw);
                    }
                    state->callback(state->userdata, crop, rw, rh, crop_stride);
                    free(crop);
                }
            }
        } else {
            if (need_swap) {
                int out_stride = width * 4;
                uint8_t *rgba = malloc(out_stride * height);
                if (rgba) {
                    for (int row = 0; row < height; row++) {
                        COPY_ROW(rgba + row * out_stride,
                                 data + row * stride, width);
                    }
                    state->callback(state->userdata, rgba, width, height, out_stride);
                    free(rgba);
                }
            } else {
                state->callback(state->userdata, data, width, height, stride);
            }
        }
    }
    #undef COPY_ROW

    pw_stream_queue_buffer(state->pw_stream, pw_buf);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error) {
    (void)userdata;
    fprintf(stderr, "capture: PipeWire stream state: %s -> %s%s%s\n",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state),
            error ? " error: " : "",
            error ? error : "");
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_stream_param_changed,
    .process = on_stream_process,
};

/* ---------- PipeWire thread ---------- */

static void *pw_thread_func(void *arg) {
    CaptureState *state = arg;
    pw_main_loop_run(state->pw_loop);
    return NULL;
}

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

    /* D-Bus connection */
    DBusError err;
    dbus_error_init(&err);
    state->dbus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
        fprintf(stderr, "capture: D-Bus connection failed: %s\n", err.message);
        dbus_error_free(&err);
        return -1;
    }

    /* Try Mutter ScreenCast first (GNOME), then xdg-desktop-portal (KDE, etc.) */
    if (setup_mutter(state) < 0) {
        /* Reset for portal attempt */
        free(state->session_path); state->session_path = NULL;
        free(state->stream_path); state->stream_path = NULL;
        if (setup_portal(state) < 0) return -1;
    }

    fprintf(stderr, "capture: PipeWire node ID = %u\n", state->pipewire_node_id);

    /* PipeWire init */
    pw_init(NULL, NULL);

    state->pw_loop = pw_main_loop_new(NULL);
    if (!state->pw_loop) return -1;

    state->pw_context = pw_context_new(
        pw_main_loop_get_loop(state->pw_loop), NULL, 0);
    if (!state->pw_context) return -1;

    state->pw_core = pw_context_connect(state->pw_context, NULL, 0);
    if (!state->pw_core) return -1;

    /* Create stream */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        NULL);

    state->pw_stream = pw_stream_new(state->pw_core, "snoop-capture", props);
    if (!state->pw_stream) return -1;

    pw_stream_add_listener(state->pw_stream, &state->stream_listener,
                           &stream_events, state);

    /* Negotiate video format — accept RGBA, BGRA, BGRx */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(4,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_RGBA,
            SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_BGRx),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
            &SPA_RECTANGLE(1920, 1080),
            &SPA_RECTANGLE(1, 1),
            &SPA_RECTANGLE(8192, 8192)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
            &SPA_FRACTION(30, 1),
            &SPA_FRACTION(0, 1),
            &SPA_FRACTION(120, 1)));

    if (pw_stream_connect(state->pw_stream,
                          PW_DIRECTION_INPUT,
                          state->pipewire_node_id,
                          PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                          params, 1) < 0) {
        return -1;
    }

    /* Start PipeWire thread */
    state->running = 1;
    if (pthread_create(&state->thread, NULL, pw_thread_func, state) != 0) {
        state->running = 0;
        return -1;
    }

    return 0;
}

void capture_stop(CaptureState *state) {
    if (!state || !state->running) return;

    state->running = 0;
    pw_main_loop_quit(state->pw_loop);
    pthread_join(state->thread, NULL);

    if (state->pw_stream) {
        pw_stream_disconnect(state->pw_stream);
        pw_stream_destroy(state->pw_stream);
        state->pw_stream = NULL;
    }
    if (state->pw_core) {
        pw_core_disconnect(state->pw_core);
        state->pw_core = NULL;
    }
    if (state->pw_context) {
        pw_context_destroy(state->pw_context);
        state->pw_context = NULL;
    }
    if (state->pw_loop) {
        pw_main_loop_destroy(state->pw_loop);
        state->pw_loop = NULL;
    }

    dbus_stop_session(state);

    pw_deinit();
}

void capture_destroy(CaptureState *state) {
    if (!state) return;
    capture_stop(state);
    free(state->display_id);
    free(state->session_path);
    free(state->stream_path);
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
    if (fps > 0) {
        state->min_frame_interval_ns = 1000000000LL / fps;
    } else {
        state->min_frame_interval_ns = 0;
    }
}

void capture_suspend(CaptureState *state) {
    state->suspended = 1;
}

void capture_resume(CaptureState *state) {
    state->suspended = 0;
}

void capture_snap(CaptureState *state) {
    state->snap_requested = 1;
}

void capture_on_error(CaptureState *state, error_callback_t cb, void *userdata) {
    if (!state) return;
    state->error_callback = cb;
    state->error_userdata = userdata;
}

int capture_set_display(CaptureState *state, const char *display_id) {
    if (!state) return -1;
    free(state->display_id);
    state->display_id = display_id ? strdup(display_id) : NULL;
    return 0;
}

int capture_list_displays(CaptureState *state,
                          CaptureDisplayInfo *out, int max_displays) {
    /* TODO Phase 3: query Mutter GetCurrentState for connector + geometry */
    (void)state; (void)out; (void)max_displays;
    return -1;
}
