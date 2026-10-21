#ifndef TERMINATOR_MOUSE_SELECTION_H
#define TERMINATOR_MOUSE_SELECTION_H

#include <X11/Xlib.h>
#include <algorithm>
#include <string>
#include <vector>

// Select text in the currently visible rows. Does not invoke external tools.
class MouseSelection {
    std::vector<std::string> rows;
    int anchorRow = -1, anchorCol = 0, endRow = -1, endCol = 0;
    bool dragging = false;
    XFontStruct* font = nullptr; // Borrowed: owned by the X server/GC.
    int charPosition(const std::string& s, int x) const {
        x = std::max(0, x - 20);
        if (!font) return std::min(static_cast<int>(s.size()), x / 6);
        for (size_t i = 0; i < s.size(); ++i) {
            int left = XTextWidth(font, s.c_str(), static_cast<int>(i));
            int right = XTextWidth(font, s.c_str(), static_cast<int>(i + 1));
            if (x < (left + right) / 2) return static_cast<int>(i);
        }
        return static_cast<int>(s.size());
    }
    int rowAt(int y) const { return std::clamp((y - 54) / 22, 0, std::max(0, static_cast<int>(rows.size()) - 1)); }
public:
    void setFont(XFontStruct* f) { font = f; }
    void clear() { rows.clear(); anchorRow = endRow = -1; dragging = false; }
    bool active() const { return anchorRow >= 0 && endRow >= 0 && !rows.empty(); }
    void begin(int x, int y, std::vector<std::string> visible) {
        clear();
        if (visible.empty() || y < 48 || y > 525 || x < 20 || x > 870) return;
        rows = std::move(visible);
        anchorRow = endRow = rowAt(y);
        anchorCol = endCol = charPosition(rows[anchorRow], x);
        dragging = true;
    }
    void move(int x, int y) {
        if (!dragging || rows.empty()) return;
        endRow = rowAt(y);
        endCol = charPosition(rows[endRow], x);
    }
    void finish(int x, int y) { move(x, y); dragging = false; }
    std::string selectedText() const {
        if (!active()) return {};
        int aR=anchorRow,aC=anchorCol,bR=endRow,bC=endCol;
        if (aR > bR || (aR == bR && aC > bC)) { std::swap(aR,bR); std::swap(aC,bC); }
        std::string result;
        for (int r=aR; r<=bR; ++r) {
            int start = (r == aR ? aC : 0);
            int stop = (r == bR ? bC : static_cast<int>(rows[r].size()));
            if (r != aR) result += '\n';
            if (stop > start) result += rows[r].substr(start, stop-start);
        }
        return result;
    }
    void draw(Display* d, Window w, GC gc) const {
        if (!active()) return;
        int aR=anchorRow,aC=anchorCol,bR=endRow,bC=endCol;
        if (aR > bR || (aR == bR && aC > bC)) { std::swap(aR,bR); std::swap(aC,bC); }
        for (int r=aR; r<=bR; ++r) {
            int start = (r == aR ? aC : 0);
            int stop = (r == bR ? bC : static_cast<int>(rows[r].size()));
            if (stop <= start) continue;
            int x1 = font ? XTextWidth(font, rows[r].c_str(), start) : start*6;
            int x2 = font ? XTextWidth(font, rows[r].c_str(), stop) : stop*6;
            // Underline selection without erasing text or terminal output.
            XDrawLine(d, w, gc, 20+x1, 67+r*22, 20+x2, 67+r*22);
        }
    }
};

#endif // TERMINATOR_MOUSE_SELECTION_H
