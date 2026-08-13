#include "panicast/config/ini_config.h"

namespace panicast
{

IniConfig &IniConfig::instance() {
    static IniConfig ic;
    return ic;
}


// ── mpv-config getters (D24: moved out-of-line from ini_config.h) ──
std::string IniConfig::get_mpv_vo() const {
    return get("mpv", "vo", "auto");
}
std::string IniConfig::get_mpv_vid() const {
    return get("mpv", "vid", "auto");
}
std::string IniConfig::get_mpv_ao() const {
    // F40: treat empty INI value as the default (old config.ini had "ao =" empty,
    //   which otherwise overrides the default → mpv auto → pipewire probe noise).
    std::string v = get("mpv", "ao", "pulse,alsa");
    return v.empty() ? "pulse,alsa" : v;
}
std::string IniConfig::get_mpv_ytdl_format() const {
    return get("mpv", "ytdl_format", "bestvideo+bestaudio");
}
std::string IniConfig::get_mpv_user_agent() const {
    return get("mpv", "user_agent",
               "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
}
std::string IniConfig::get_mpv_cache() const {
    return get("mpv", "cache", "yes");
}
std::string IniConfig::get_mpv_demuxer_max_bytes() const {
    return get("mpv", "demuxer_max_bytes", "50MiB");
}
std::string IniConfig::get_mpv_demuxer_max_back_bytes() const {
    return get("mpv", "demuxer_max_back_bytes", "25MiB");
}
int IniConfig::get_mpv_cache_secs() const {
    return get_int("mpv", "cache_secs", 10);
}
int IniConfig::get_mpv_audio_cache_secs() const {
    return get_int("mpv", "audio_cache_secs", 5);
}
std::string IniConfig::get_mpv_audio_demuxer_max_bytes() const {
    return get("mpv", "audio_demuxer_max_bytes", "5MiB");
}
std::string IniConfig::get_mpv_audio_demuxer_max_back_bytes() const {
    return get("mpv", "audio_demuxer_max_back_bytes", "2MiB");
}
int IniConfig::get_mpv_audio_cache_pause_wait() const {
    return get_int("mpv", "audio_cache_pause_wait", 1);
}
bool IniConfig::get_mpv_tls_verify() const {
    return get_bool("mpv", "tls_verify", true);
}
bool IniConfig::get_mpv_keep_open() const {
    return get_bool("mpv", "keep_open", true);
}
std::string IniConfig::get_mpv_sub_align_x() const {
    return get("mpv", "sub_align_x", "center");
}
std::string IniConfig::get_mpv_sub_align_y() const {
    return get("mpv", "sub_align_y", "bottom");
}
std::string IniConfig::get_mpv_sub_visibility() const {
    return get("mpv", "sub_visibility", "yes");
}
std::string IniConfig::get_mpv_sub_ass_override() const {
    return get("mpv", "sub_ass_override", "auto");
}
std::string IniConfig::get_mpv_sub_lang() const {
    return get("mpv", "sub_lang", "en");
}


// ── YouTube-config getters (D25: moved out-of-line from ini_config.h) ──
std::string IniConfig::get_youtube_cookies_file() const {
    return resolve_cookies_path("youtube", "cookies_file", "youtube_cookie.txt");
}
std::string IniConfig::get_youtube_player_client() const {
    std::string v = get("youtube", "player_client", "tv_downgraded,web");
    return v.empty() ? "tv_downgraded,web" : v;
}
std::string IniConfig::get_youtube_js_runtime() const {
    return get("youtube", "js_runtime", "quickjs");
}
std::string IniConfig::get_youtube_play_format_video() const {
    return get("youtube", "play_format_video", "bestvideo[height<=1080]+bestaudio");
}
std::string IniConfig::get_youtube_play_format_audio() const {
    return get("youtube", "play_format_audio", "bestaudio");
}
int IniConfig::get_youtube_resolve_timeout_sec() const {
    return get_int("youtube", "resolve_timeout_sec", 90);
}
int IniConfig::get_youtube_resolve_retries() const {
    return get_int("youtube", "resolve_retries", 3);
}
std::string IniConfig::get_youtube_sub_lang() const {
    return get("youtube", "sub_lang", "");
}
bool IniConfig::get_youtube_sub_auto() const {
    return get_bool("youtube", "sub_auto", true);
}


// ── cookies / IPTV / remote-control getters (D26: moved out-of-line) ──
std::string IniConfig::get_bilibili_cookies_file() const {
    return resolve_cookies_path("bilibili", "cookies_file", "bilibili_cookie.txt");
}
std::string IniConfig::get_tiktok_douyin_cookies_file() const {
    return resolve_cookies_path("tiktok", "douyin_cookies_file", "douyin_cookie.txt");
}
std::string IniConfig::get_tiktok_cookies_file() const {
    return resolve_cookies_path("tiktok", "cookies_file", "tiktok_cookie.txt");
}
std::string IniConfig::get_iptv_base_url() const {
    return get("iptv", "base_url", "https://iptv-org.github.io/iptv");
}
std::string IniConfig::get_iptv_api_url() const {
    return get("iptv", "api_url", "https://iptv-org.github.io/api");
}
int IniConfig::get_iptv_cache_hours() const {
    return get_int("iptv", "cache_hours", 24);
}
std::string IniConfig::get_iptv_custom_urls() const {
    return get("iptv", "custom_urls", "");
}
int IniConfig::get_iptv_offair_detect_secs() const {
    return get_int("iptv", "offair_detect_secs", 12);
}
bool IniConfig::get_remote_enabled() const {
    return get_bool("remote", "enable", false);
}
int IniConfig::get_remote_port() const {
    return get_int("remote", "port", 8421);
}
std::string IniConfig::get_remote_bind() const {
    return get("remote", "bind", "127.0.0.1");
}
std::string IniConfig::get_remote_auth_token() const {
    return get("remote", "auth_token", "");
}
std::string IniConfig::get_remote_universal_pin() const {
    return get("remote", "universal_pin", "6696");
}
int IniConfig::get_remote_discovery_port() const {
    return get_int("remote", "discovery_port", 18430);
}


// ── misc getters: search/history/region/network/display (D27: moved out-of-line) ──
int IniConfig::get_search_cache_max() {
    return get_int("storage", "search_cache_max", 1024);
}
int IniConfig::get_history_max_records() {
    return get_int("storage", "history_max_records", 2048);
}
int IniConfig::get_history_max_days() {
    return get_int("storage", "history_max_days", 1080);
}
std::string IniConfig::get_default_region() {
    return get("search", "default_region", "US");
}
bool IniConfig::get_tls_verify() const {
    return get_bool("network", "tls_verify", true);
}
bool IniConfig::get_reject_unsafe_url() const {
    return get_bool("network", "reject_unsafe_url", true);
}
int IniConfig::get_network_timeout() const {
    return get_int("network", "timeout", 30);
}
bool IniConfig::get_url_hyperlink() const {
    return get_bool("display", "url_hyperlink", true);
}


// ── display getters ([display] section) (D28: moved out-of-line from ini_config.h) ──
float IniConfig::get_log_height_ratio() const {
    return get_float("display", "log_height_ratio", 0.3f);
}
int IniConfig::get_log_compress_height() const {
    return get_int("display", "log_compress_height", 23);
}
int IniConfig::get_display_state_refresh_ms() const {
    return get_int("display", "state_refresh_ms", 100);
}
bool IniConfig::get_display_lyric() const {
    return get_bool("display", "lyric", true);
}
int IniConfig::get_display_lyric_lines() const {
    return get_int("display", "lyric_lines", 3);
}
bool IniConfig::get_display_lyric_bar() const {
    return get_bool("display", "lyric_bar", false);
}
int IniConfig::get_display_lyric_bar_height() const {
    return get_int("display", "lyric_bar_height", 5);
}


// ── core accessors: load/save/get_int/set (D29: moved out-of-line from ini_config.h) ──
void IniConfig::load() {
    std::string path = get_config_file();
    if (!fs::exists(path)) {
        create_default(path);
    }

    std::ifstream f(path);
    std::string line;
    std::string current_section;
    raw_lines_.clear();

    while (std::getline(f, line)) {
        // Keep original lines for writeback in save() (including comments, blank lines, original order)
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        raw_lines_.push_back(line);

        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
            continue;

        if (trimmed[0] == '[' && trimmed.back() == ']') {
            current_section = trimmed.substr(1, trimmed.length() - 2);
            continue;
        }

        size_t pos = trimmed.find('=');
        if (pos != std::string::npos) {
            std::string key = trimmed.substr(0, pos);
            std::string value = trimmed.substr(pos + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (value.length() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.length() - 2);
            }

            data_[current_section][key] = value;
        }
    }
}
int IniConfig::get_int(const std::string &section, const std::string &key, int default_val) const {
    std::string val = get(section, key);
    if (!val.empty()) {
        try {
            return std::stoi(val);
        } catch (...) {
        }
    }
    return default_val;
}
void IniConfig::set(const std::string &section, const std::string &key, const std::string &value) {
    std::unique_lock<std::shared_mutex> lk(cfg_mtx_); // P2 (Y23.7): exclusive write
    data_[section][key] = value;
    lk.unlock();
    save();
}
void IniConfig::save() {
    std::string path = get_config_file();
    if (path.empty())
        return;
    std::ofstream f(path);
    if (!f.is_open())
        return;
    std::set<std::string> written; // "section\x1fkey"
    std::string current_section;
    for (const auto &raw : raw_lines_) {
        std::string line = raw;
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) {
            f << raw << "\n";
            continue;
        }
        if (line[a] == '#' || line[a] == ';') {
            f << raw << "\n";
            continue;
        }
        if (line[a] == '[') {
            size_t rb = line.find(']', a);
            if (rb != std::string::npos)
                current_section = line.substr(a + 1, rb - a - 1);
            f << raw << "\n";
            continue;
        }
        size_t eq = line.find('=', a);
        if (eq == std::string::npos) {
            f << raw << "\n";
            continue;
        }
        std::string key = line.substr(a, eq - a);
        key.erase(key.find_last_not_of(" \t") + 1);
        std::string id = current_section + "\x1f" + key;
        if (data_.count(current_section) && data_[current_section].count(key)) {
            f << key << " = " << data_[current_section][key] << "\n";
            written.insert(id);
        } else {
            f << raw << "\n";
        }
    }
    // Append new keys not present in the file
    for (const auto &[section, kv] : data_) {
        for (const auto &[k, v] : kv) {
            std::string id = section + "\x1f" + k;
            if (!written.count(id)) {
                f << "[" << section << "]\n" << k << " = " << v << "\n";
                written.insert(id);
            }
        }
    }
}


// ── logic + static helpers: statusbar/play-mode/proxy/color/url/config-file (D30: moved out-of-line) ──
StatusBarColorConfig IniConfig::get_statusbar_color_config() {
    StatusBarColorConfig cfg;

    std::string mode_str = get("statusbar_color", "mode", "rainbow");
    if (mode_str == "random")
        cfg.mode = StatusBarColorMode::RANDOM;
    else if (mode_str == "time_brightness")
        cfg.mode = StatusBarColorMode::TIME_BRIGHTNESS;
    else if (mode_str == "fixed")
        cfg.mode = StatusBarColorMode::FIXED;
    else if (mode_str == "custom")
        cfg.mode = StatusBarColorMode::CUSTOM;
    else
        cfg.mode = StatusBarColorMode::RAINBOW;

    cfg.update_interval_ms = get_int("statusbar_color", "update_interval_ms", 100);
    cfg.brightness_min = get_float("statusbar_color", "brightness_min", 0.5f);
    cfg.brightness_max = get_float("statusbar_color", "brightness_max", 1.0f);
    cfg.time_adjust = get_bool("statusbar_color", "time_adjust", true);
    cfg.fixed_color = get("statusbar_color", "fixed_color", "cyan");
    cfg.rainbow_speed = get_int("statusbar_color", "rainbow_speed", 1);
    // Parse CUSTOM mode configuration
    std::string colors_str = get("statusbar_color", "custom_colors", "");
    if (!colors_str.empty()) {
        std::stringstream ss(colors_str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                cfg.custom_colors.push_back(std::stoi(token));
            } catch (...) {
            }
        }
    }
    cfg.custom_speed = get_int("statusbar_color", "custom_speed", 2);
    if (cfg.rainbow_speed < 1)
        cfg.rainbow_speed = 1;
    if (cfg.rainbow_speed > 10)
        cfg.rainbow_speed = 10;

    return cfg;
}
PlayMode IniConfig::get_play_mode() const {
    std::string v = get("playback", "mode", "cycle");
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (v == "repeat" || v == "r")
        return PlayMode::REPEAT;
    if (v == "shuffle" || v == "s" || v == "random")
        return PlayMode::SHUFFLE;
    return PlayMode::CYCLE; // "cycle"/"c"/default
}
void IniConfig::set_play_mode(PlayMode m) {
    const char *s = (m == PlayMode::REPEAT)    ? "repeat"
                    : (m == PlayMode::SHUFFLE) ? "shuffle"
                                               : "cycle";
    set("playback", "mode", s);
}
std::string IniConfig::get_proxy() const {
    return normalize_proxy(get("network", "proxy", ""));
}
std::string IniConfig::normalize_proxy(const std::string &input, bool *is_valid) {
    if (is_valid)
        *is_valid = true;
    std::string s = input;
    // Trim leading/trailing whitespace
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    if (s.empty())
        return ""; // proxy disabled
    // Lowercase the scheme (before "://")
    size_t pos = s.find("://");
    if (pos == std::string::npos) {
        if (is_valid)
            *is_valid = false;
        return s;
    }
    std::string scheme = s.substr(0, pos);
    for (auto &c : scheme)
        c = (char)std::tolower((unsigned char)c);
    std::string rest = s.substr(pos + 3);
    // socks:// → socks5h://
    if (scheme == "socks")
        scheme = "socks5h";
    // Validate scheme whitelist
    static const std::set<std::string> ok = {"http",    "https",  "socks4",
                                             "socks4a", "socks5", "socks5h"};
    if (!ok.count(scheme)) {
        if (is_valid)
            *is_valid = false;
        return s;
    }
    // Validate host[:port], no path (no '/')
    if (rest.empty() || rest.find('/') != std::string::npos) {
        if (is_valid)
            *is_valid = false;
        return s;
    }
    return scheme + "://" + rest;
}
short IniConfig::resolve_color(const std::string &s, short fallback) {
    std::string t = s;
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    t.erase(0, t.find_first_not_of(" \t"));
    t.erase(t.find_last_not_of(" \t") + 1);
    if (t.empty())
        return fallback;
    // D12-3a: raw ANSI/ncurses color indices (0-7) — decoupled from <ncurses.h> so this
    //   config header need not pull ncurses (see docs/ARCHITECTURE.md §2.1). Values are
    //   identical to ncurses COLOR_BLACK..COLOR_WHITE; -1 = default passthrough.
    static const std::map<std::string, short> names = {
        {"black", 0}, {"red", 1}, {"green", 2}, {"yellow", 3},
        {"blue", 4},  {"magenta", 5}, {"cyan", 6}, {"white", 7}, {"default", -1}};
    auto it = names.find(t);
    if (it != names.end())
        return it->second;
    // Pure number (negative sign allowed)
    bool numeric = !t.empty();
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        bool ok = (c >= '0' && c <= '9') || (i == 0 && c == '-');
        if (!ok) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        try {
            int n = std::stoi(t);
            if (n >= -1 && n <= 255)
                return (short)n;
        } catch (...) {
        }
    }
    return fallback;
}
short IniConfig::get_node_color(const std::string &key, short fallback) const {
    return resolve_color(get("colors", key), fallback);
}
bool IniConfig::is_url_safe(const std::string &url) const {
    if (!get_reject_unsafe_url())
        return true; // user explicitly disabled
    // Only allow http/https protocols
    return url.size() >= 8 &&
           (url.compare(0, 8, "https://") == 0 || url.compare(0, 7, "http://") == 0);
}
std::string IniConfig::get_config_file() {
    const char *home = std::getenv("HOME");
    return home ? std::string(home) + CONFIG_DIR + "/config.ini" : "";
}

} // namespace panicast
