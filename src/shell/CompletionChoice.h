#ifndef COMPLETION_CHOICE_H
#define COMPLETION_CHOICE_H
#include <string>
#include <vector>
#include <X11/Xlib.h>
#include <X11/keysym.h>

struct CompletionChoice {
    std::vector<std::string> matches;
    std::string directory;
    std::string digits;
    bool active() const { return !matches.empty(); }
    void clear() { matches.clear(); directory.clear(); digits.clear(); }
    // Return selected index, -1 when waiting, -2 on cancel/invalid input.
    int key(KeySym sym, const char* chars, int count) {
        if (sym == XK_Escape) { clear(); return -2; }
        if (sym == XK_Return || sym == XK_KP_Enter) {
            if (digits.empty()) { clear(); return -2; }
            int index = 0;
            for (char c : digits) {
                if (index > 100000) { clear(); return -2; }
                index = index * 10 + c - '0';
            }
            if (index < 1 || static_cast<size_t>(index) > matches.size()) {
                clear(); return -2;
            }
            return index - 1;
        }
        if (count == 1 && chars[0] >= '1' && chars[0] <= '9') {
            digits.push_back(chars[0]);
            int index = 0;
            for (char c : digits) index = index * 10 + c - '0';
            if (digits.size() == 1 && matches.size() <= 9) {
                if (static_cast<size_t>(index) <= matches.size()) return index - 1;
                clear(); return -2;
            }
            return -1;
        }
        if (count == 1 && chars[0] == '0' && !digits.empty()) {
            digits.push_back('0'); return -1;
        }
        clear(); return -2;
    }
};
#endif
