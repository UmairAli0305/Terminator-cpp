#ifndef LINE_EDITING_H
#define LINE_EDITING_H
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <algorithm>
#include <cstddef>
#include <string>

namespace LineEditing {
inline bool apply(std::string& input, size_t& cursor, KeySym key, bool ctrl) {
    cursor = std::min(cursor, input.size());
    if (ctrl) {
        switch (key) {
        case XK_a: case XK_A: cursor = 0; return true;
        case XK_e: case XK_E: cursor = input.size(); return true;
        case XK_u: case XK_U: input.erase(0, cursor); cursor = 0; return true;
        case XK_k: case XK_K: input.erase(cursor); return true;
        case XK_w: case XK_W: {
            size_t start = cursor;
            while (start > 0 && input[start - 1] == ' ') --start;
            while (start > 0 && input[start - 1] != ' ') --start;
            input.erase(start, cursor - start);
            cursor = start;
            return true;
        }
        default: return false;
        }
    }
    if (key == XK_Left) { if (cursor) --cursor; return true; }
    if (key == XK_Right) { if (cursor < input.size()) ++cursor; return true; }
    if (key == XK_Home) { cursor = 0; return true; }
    if (key == XK_End) { cursor = input.size(); return true; }
    if (key == XK_BackSpace) {
        if (cursor) { input.erase(cursor - 1, 1); --cursor; }
        return true;
    }
    if (key == XK_Delete) {
        if (cursor < input.size()) input.erase(cursor, 1);
        return true;
    }
    return false;
}
inline void insert(std::string& input, size_t& cursor, char ch) {
    cursor = std::min(cursor, input.size());
    input.insert(cursor++, 1, ch);
}
inline std::string display(const std::string& input, size_t cursor) {
    std::string result = input;
    result.insert(std::min(cursor, result.size()), "_");
    return result;
}
}

#endif // LINE_EDITING_H
