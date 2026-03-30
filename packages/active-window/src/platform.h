#ifndef SNOOP_ACTIVE_WINDOW_PLATFORM_H
#define SNOOP_ACTIVE_WINDOW_PLATFORM_H

typedef struct {
    int x, y, width, height;
    char title[256];
    int valid;
} ActiveWindowInfo;

typedef struct {
    int x, y;
} CursorPosition;

ActiveWindowInfo platform_get_active_window(void);
CursorPosition platform_get_cursor_position(void);

#endif
