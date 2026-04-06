#include "platform.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <string.h>
#include <stdlib.h>

static Display *dpy = NULL;
static Atom net_active_window;
static Atom net_wm_name;
static Atom utf8_string;

static void ensure_display(void) {
    if (dpy) return;
    dpy = XOpenDisplay(NULL);
    if (!dpy) return;
    net_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    utf8_string = XInternAtom(dpy, "UTF8_STRING", False);
}

static Window get_active_window_xid(void) {
    ensure_display();
    if (!dpy) return None;

    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    Window root = DefaultRootWindow(dpy);
    if (XGetWindowProperty(dpy, root, net_active_window, 0, 1, False,
                           XA_WINDOW, &type, &format, &nitems,
                           &bytes_after, &data) != Success || !data) {
        return None;
    }

    Window win = None;
    if (nitems > 0) {
        win = *(Window *)data;
    }
    XFree(data);
    return win;
}

static void get_window_title(Window win, char *buf, size_t buflen) {
    buf[0] = '\0';
    if (!dpy || win == None) return;

    /* Try _NET_WM_NAME (UTF-8) first */
    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, win, net_wm_name, 0, 1024, False,
                           utf8_string, &type, &format, &nitems,
                           &bytes_after, &data) == Success && data && nitems > 0) {
        strncpy(buf, (char *)data, buflen - 1);
        buf[buflen - 1] = '\0';
        XFree(data);
        return;
    }
    if (data) XFree(data);

    /* Fallback to XFetchName (Latin-1) */
    char *name = NULL;
    if (XFetchName(dpy, win, &name) && name) {
        strncpy(buf, name, buflen - 1);
        buf[buflen - 1] = '\0';
        XFree(name);
    }
}

ActiveWindowInfo platform_get_active_window(void) {
    ActiveWindowInfo info = {0};
    ensure_display();
    if (!dpy) return info;

    Window win = get_active_window_xid();
    if (win == None) return info;

    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, win, &attrs)) return info;

    /* Translate to root coordinates */
    int x, y;
    Window child;
    XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &x, &y, &child);

    info.x = x;
    info.y = y;
    info.width = attrs.width;
    info.height = attrs.height;
    get_window_title(win, info.title, sizeof(info.title));
    info.valid = 1;
    return info;
}

CursorPosition platform_get_cursor_position(void) {
    CursorPosition pos = {0};
    ensure_display();
    if (!dpy) return pos;

    Window root = DefaultRootWindow(dpy);
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;

    XQueryPointer(dpy, root, &root_ret, &child_ret,
                  &root_x, &root_y, &win_x, &win_y, &mask);

    pos.x = root_x;
    pos.y = root_y;
    pos.valid = 1;
    return pos;
}

int platform_set_cursor_position(int x, int y) {
    ensure_display();
    if (!dpy) return -1;
    Window root = DefaultRootWindow(dpy);
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, x, y);
    XFlush(dpy);
    return 0;
}
