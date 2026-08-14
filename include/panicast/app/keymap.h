// Keymap — key (int) → Action. handle_input looks the key up here FIRST; if bound, the Action is
// published on the message bus (UI never calls Core directly for bound keys). Unbound keys fall
// through to the legacy switch (complex flows not yet migrated). Defaults are built in code by
// App::build_keymap from a defaults table; an INI [keys] section overrides them (D44 — rebindable
// hotkeys, D7's full goal). parse_token is the single parser shared by build_keymap + tests.
// (D7 build_keymap; D44 INI override + parse_token.)
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "panicast/app/actions.h"

namespace panicast
{

class Keymap {
public:
    void bind(int key, Action a) { map_[key] = std::move(a); }
    const Action *lookup(int key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    bool contains(int key) const { return map_.count(key) > 0; }

    // D44: parse a [keys] INI token → ncurses keycode (int). Returns -1 if unparseable.
    //   Supports: space/enter/esc/tab/backspace names, a single printable char, or a numeric
    //   keycode (e.g. 25 = Ctrl+Y). The single-char path returns the char AS-IS — case matters
    //   ('r' and 'R' are different keys), matching wget_wch which returns exactly what is typed.
    //   Exposed static so build_keymap + unit tests share one parser.
    static int parse_token(const std::string &raw) {
        std::string t = raw;
        auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
        while (!t.empty() && !not_space((unsigned char)t.front()))
            t.erase(0, 1);
        while (!t.empty() && !not_space((unsigned char)t.back()))
            t.pop_back();
        if (t.empty())
            return -1;
        std::string low;
        low.reserve(t.size());
        for (char c : t)
            low.push_back((char)std::tolower((unsigned char)c));
        if (low == "space")
            return ' ';
        if (low == "enter" || low == "return")
            return '\n';
        if (low == "esc" || low == "escape")
            return 27;
        if (low == "tab")
            return '\t';
        if (low == "backspace" || low == "bs")
            return 127;
        if (t.size() == 1)
            return (int)(unsigned char)t[0];
        bool all_digit = !t.empty();
        for (char c : t)
            if (!std::isdigit((unsigned char)c)) {
                all_digit = false;
                break;
            }
        if (all_digit)
            return std::atoi(t.c_str());
        return -1;
    }

private:
    std::unordered_map<int, Action> map_;
};

}  // namespace panicast
