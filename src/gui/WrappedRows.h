#ifndef TERMINATOR_WRAPPED_ROWS_H
#define TERMINATOR_WRAPPED_ROWS_H
#include <X11/Xlib.h>
#include <algorithm>
#include <string>
#include <vector>

// Display-only soft wrapping: never changes the command sent to the shell.
namespace WrappedRows {
inline std::vector<std::string> wrap(const std::string& text, XFontStruct* font, int maxPixels = 840) {
    std::vector<std::string> result;
    if (text.empty()) { result.emplace_back(); return result; }
    size_t start = 0;
    while (start < text.size()) {
        size_t end = start;
        int width = 0;
        while (end < text.size()) {
            int charWidth = font ? XTextWidth(font, text.data() + end, 1) : 8;
            if (width + charWidth > maxPixels && end > start) break;
            width += charWidth;
            ++end;
        }
        result.push_back(text.substr(start, end - start));
        start = end;
    }
    return result;
}
inline std::vector<std::string> build(const std::vector<std::string>& output,
                                     const std::string& prompt, XFontStruct* font,
                                     int maxPixels = 840) {
    std::vector<std::string> rows;
    for (const auto& line : output) {
        auto wrapped = wrap(line, font, maxPixels);
        rows.insert(rows.end(), wrapped.begin(), wrapped.end());
    }
    auto wrapped = wrap(prompt, font, maxPixels);
    rows.insert(rows.end(), wrapped.begin(), wrapped.end());
    return rows;
}
inline std::vector<std::string> visible(const std::vector<std::string>& rows,
                                        int scrollOffset, int count = 20) {
    int start = std::max(0, static_cast<int>(rows.size()) - count - scrollOffset);
    int end = std::min(static_cast<int>(rows.size()), start + count);
    return std::vector<std::string>(rows.begin() + start, rows.begin() + end);
}
}
#endif
