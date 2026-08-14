// Keymap implementation unit (D44-prof: parse_token moved out of the header — 40 lines of
//   parsing logic inline in keymap.h recompiled in every including TU).
#include "panicast/app/keymap.h"

#include <cctype>
#include <charconv>
#include <string>

namespace {
// D44-prof: was std::atoi — no error detection and UB on overflow ("99999999999"). from_chars
//   reports failure and clamps nothing; we reject unparsed/out-of-range tokens explicitly.
bool parse_keycode(const std::string &digits, int &out) {
    int v = 0;
    auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
    if (ec != std::errc() || end != digits.data() + digits.size())
        return false;
    if (v < 0 || v > 0xFFFF) // sane keycode ceiling (ncurses KEY_* values live well below)
        return false;
    out = v;
    return true;
}
} // namespace

namespace panicast
{

// D44: parse a [keys] INI token → ncurses keycode (int). Returns -1 if unparseable.
//   Supports: space/enter/esc/tab/backspace names, a single printable char, or a numeric
//   keycode (e.g. 25 = Ctrl+Y). The single-char path returns the char AS-IS — case matters
//   ('r' and 'R' are different keys), matching wget_wch which returns exactly what is typed.
//   Exposed static so build_keymap + unit tests share one parser.
int Keymap::parse_token(const std::string &raw) {
    std::string t = raw;
    auto not_space = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    while (!t.empty() && !not_space(static_cast<unsigned char>(t.front())))
        t.erase(0, 1);
    while (!t.empty() && !not_space(static_cast<unsigned char>(t.back())))
        t.pop_back();
    if (t.empty())
        return -1;
    std::string low;
    low.reserve(t.size());
    for (char c : t)
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
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
        return static_cast<int>(static_cast<unsigned char>(t[0]));
    bool all_digit = true;
    for (char c : t)
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            all_digit = false;
            break;
        }
    int code = -1;
    if (all_digit && parse_keycode(t, code))
        return code;
    return -1;
}

} // namespace panicast
