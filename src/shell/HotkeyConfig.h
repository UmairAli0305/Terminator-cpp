#ifndef TERMINATOR_HOTKEY_CONFIG_H
#define TERMINATOR_HOTKEY_CONFIG_H

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// One immutable mapping per application run. Edit ~/.terminatorrc and restart.
// Example: copy = Ctrl+Shift+Y
namespace HotkeyConfig {
struct Binding { KeySym key; unsigned int modifiers; };
inline unsigned int relevant(unsigned int state) {
    return state & (ControlMask | ShiftMask | Mod1Mask);
}
inline KeySym normalized(KeySym key) {
    if (key >= XK_A && key <= XK_Z) return key + (XK_a - XK_A);
    return key;
}
inline std::map<std::string, Binding> defaults() {
    return {
        {"copy", {XK_c, ControlMask | ShiftMask}},
        {"paste", {XK_v, ControlMask | ShiftMask}},
        {"clear", {XK_l, ControlMask}},
        {"close_empty", {XK_d, ControlMask}},
        {"interrupt", {XK_c, ControlMask}},
        {"suspend", {XK_z, ControlMask}},
        {"new_tab", {XK_t, ControlMask}},
        {"next_tab", {XK_Tab, ControlMask}},
        {"close_tab", {XK_w, ControlMask | ShiftMask}},
        {"line_start", {XK_a, ControlMask}},
        {"line_end", {XK_e, ControlMask}},
        {"delete_before", {XK_u, ControlMask}},
        {"delete_after", {XK_k, ControlMask}},
        {"delete_word", {XK_w, ControlMask}},
    };
}
inline std::string trim(std::string s) {
    auto notspace = [](unsigned char c){return !std::isspace(c);};
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}
inline bool parse(const std::string& value, Binding& result) {
    unsigned int mods = 0;
    std::string keyName;
    std::istringstream stream(value);
    std::string part;
    while (std::getline(stream, part, '+')) {
        part = trim(part);
        std::transform(part.begin(), part.end(), part.begin(),
                       [](unsigned char c){return static_cast<char>(std::tolower(c));});
        if (part == "ctrl" || part == "control") mods |= ControlMask;
        else if (part == "shift") mods |= ShiftMask;
        else if (part == "alt") mods |= Mod1Mask;
        else if (keyName.empty()) keyName = part;
        else return false;
    }
    if (!(mods & (ControlMask | Mod1Mask)) || keyName.empty()) return false;
    if (keyName == "tab") keyName = "Tab";
    else if (keyName == "space") keyName = "space";
    KeySym sym = XStringToKeysym(keyName.c_str());
    if (sym == NoSymbol) return false;
    result = {normalized(sym), mods};
    return true;
}
inline bool same(const Binding& a, const Binding& b) {
    return a.key == b.key && a.modifiers == b.modifiers;
}
inline const std::map<std::string, Binding>& bindings() {
    static const auto loaded = [] {
        auto result = defaults();
        const char* home = std::getenv("HOME");
        if (!home) return result;
        std::ifstream file(std::string(home) + "/.terminatorrc");
        if (!file) return result;
        std::string line;
        int lineNo = 0;
        while (std::getline(file, line)) {
            ++lineNo;
            auto comment = line.find('#');
            if (comment != std::string::npos) line.erase(comment);
            line = trim(line);
            if (line.empty()) continue;
            auto equals = line.find('=');
            if (equals == std::string::npos) {
                std::cerr << "terminatorrc:" << lineNo << ": expected action = shortcut\n";
                continue;
            }
            auto action = trim(line.substr(0, equals));
            if (!result.count(action)) {
                std::cerr << "terminatorrc:" << lineNo << ": unknown action " << action << "\n";
                continue;
            }
            Binding candidate{};
            if (!parse(trim(line.substr(equals + 1)), candidate)) {
                std::cerr << "terminatorrc:" << lineNo << ": invalid shortcut for " << action << "\n";
                continue;
            }
            bool conflict = false;
            for (const auto& item : result)
                if (item.first != action && same(item.second, candidate)) conflict = true;
            if (conflict) {
                std::cerr << "terminatorrc:" << lineNo << ": conflicting shortcut for " << action << "; using previous binding\n";
                continue;
            }
            result[action] = candidate;
        }
        return result;
    }();
    return loaded;
}
// Translate configured shortcuts into the original shortcuts used by the
// existing X11 handlers. All other keys, including ordinary typing, pass through.
inline void translate(KeySym& key, unsigned int& state) {
    const Binding pressed{normalized(key), relevant(state)};
    const auto& configured = bindings();
    const auto standard = defaults();
    for (const auto& item : configured) {
        if (same(pressed, item.second)) {
            const auto canonical = standard.at(item.first);
            key = canonical.key;
            state = (state & ~(ControlMask | ShiftMask | Mod1Mask)) | canonical.modifiers;
            return;
        }
    }
}
}
#endif
