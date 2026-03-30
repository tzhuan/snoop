#include "platform.h"
#include <CoreGraphics/CoreGraphics.h>
#include <string.h>

ActiveWindowInfo platform_get_active_window(void) {
    ActiveWindowInfo info = {0};

    CFArrayRef windows = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
        kCGNullWindowID);
    if (!windows) return info;

    CFIndex count = CFArrayGetCount(windows);
    for (CFIndex i = 0; i < count; i++) {
        CFDictionaryRef win = CFArrayGetValueAtIndex(windows, i);

        /* Find the frontmost application window (layer 0) */
        CFNumberRef layerRef = CFDictionaryGetValue(win, kCGWindowLayer);
        int layer = -1;
        if (layerRef) CFNumberGetValue(layerRef, kCFNumberIntType, &layer);
        if (layer != 0) continue;

        CFDictionaryRef bounds = CFDictionaryGetValue(win, kCGWindowBounds);
        if (!bounds) continue;

        CGRect rect;
        if (!CGRectMakeWithDictionaryRepresentation(bounds, &rect)) continue;

        info.x = (int)rect.origin.x;
        info.y = (int)rect.origin.y;
        info.width = (int)rect.size.width;
        info.height = (int)rect.size.height;

        CFStringRef nameRef = CFDictionaryGetValue(win, kCGWindowOwnerName);
        if (nameRef) {
            CFStringGetCString(nameRef, info.title, sizeof(info.title),
                               kCFStringEncodingUTF8);
        }

        info.valid = 1;
        break;
    }

    CFRelease(windows);
    return info;
}

CursorPosition platform_get_cursor_position(void) {
    CursorPosition pos = {0};
    CGEventRef event = CGEventCreate(NULL);
    if (event) {
        CGPoint loc = CGEventGetLocation(event);
        pos.x = (int)loc.x;
        pos.y = (int)loc.y;
        CFRelease(event);
    }
    return pos;
}
