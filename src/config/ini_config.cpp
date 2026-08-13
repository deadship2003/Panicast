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

} // namespace panicast
