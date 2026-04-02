#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

ActiveWindowInfo platform_get_active_window(void) {
    ActiveWindowInfo info = {0};

    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return info;

    RECT rect;
    if (!GetWindowRect(hwnd, &rect)) return info;

    info.x = rect.left;
    info.y = rect.top;
    info.width = rect.right - rect.left;
    info.height = rect.bottom - rect.top;

    GetWindowTextA(hwnd, info.title, sizeof(info.title));
    info.valid = 1;
    return info;
}

CursorPosition platform_get_cursor_position(void) {
    CursorPosition pos = {0};
    POINT pt;
    if (GetCursorPos(&pt)) {
        pos.x = pt.x;
        pos.y = pt.y;
    }
    return pos;
}

int platform_set_cursor_position(int x, int y) {
    return SetCursorPos(x, y) ? 0 : -1;
}
