#ifndef SNOOP_GEOMETRY_PLATFORM_H
#define SNOOP_GEOMETRY_PLATFORM_H

typedef struct {
    int x, y, width, height;
    char title[256];
    int valid;
} ActiveWindowInfo;

typedef struct {
    int x, y;
    int valid;
} CursorPosition;

ActiveWindowInfo platform_get_active_window(void);
CursorPosition platform_get_cursor_position(void);
int platform_set_cursor_position(int x, int y);

#endif
