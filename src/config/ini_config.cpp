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

} // namespace panicast
