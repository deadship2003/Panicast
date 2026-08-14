// MPV playback init-options layer — extracted implementation unit (D47 god-object split).
//   apply_mpv_options_() applies all user-configurable mpv options (vo/vid/ao/ytdl-format/
//   keep-open/subtitle/slang/audio-display/proxy/tls/cache/user-agent) read from the [mpv]
//   section of IniConfig, with CLI overrides taking precedence. It remains an MPVController
//   member (reads ctx_ + the cli_*_override_ statics); only its implementation lives here.
//   Declaration stays in mpv_controller.h. Mechanical verbatim move from initialize() (mpv_controller.cpp).
#include "panicast/playback/mpv_controller.h"

#include <string>
#include <unistd.h> // setenv (mpv proxy env vars)

#include <fmt/format.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"

namespace panicast
{

// D47: apply all [mpv]-section IniConfig options to the context before mpv_initialize.
//   CLI overrides (--vo/--vid/--ao) take precedence over INI values. Extracted verbatim from
//   initialize() — single concern: "apply user-configurable mpv options". No control flow leaves
//   this method (no early return), so calling it once between the fixed-behavior flags and
//   mpv_initialize() is behavior-identical to the original inline block.
void MPVController::apply_mpv_options_() {
    // F24: vo/vid/ytdl-format/cache/demuxer/tls/user-agent/keep-open from [mpv] section of IniConfig.
    //   CLI overrides (--vo, --vid, --ao, --quiet = --vo=null --vid=no) take precedence over INI values.
    std::string mpv_vo = IniConfig::instance().get_mpv_vo();
    std::string mpv_vid = IniConfig::instance().get_mpv_vid();
    std::string mpv_ao = IniConfig::instance().get_mpv_ao(); // F40: returns pulse,alsa if INI empty
    // F40: one-time INI fixup — if ao was empty/absent, persist the default so it's visible/editable
    //   (old config.ini had "ao =" empty → overrode the default → needed explicit --ao=pulse).
    if (IniConfig::instance().get("mpv", "ao", "").empty()) {
        IniConfig::instance().set("mpv", "ao", "pulse,alsa");
    }
    if (!cli_vo_override_.empty())
        mpv_vo = cli_vo_override_;
    if (!cli_vid_override_.empty())
        mpv_vid = cli_vid_override_;
    if (!cli_ao_override_.empty())
        mpv_ao = cli_ao_override_;
    mpv_set_option_string(ctx_, "vo", mpv_vo.c_str());
    mpv_set_option_string(ctx_, "vid", mpv_vid.c_str());
    if (!mpv_ao.empty())
        mpv_set_option_string(ctx_, "ao", mpv_ao.c_str()); // empty = leave mpv default (auto)
    std::string mpv_ytdl_format = IniConfig::instance().get_mpv_ytdl_format();
    mpv_set_option_string(ctx_, "ytdl-format", mpv_ytdl_format.c_str());
    mpv_set_option_string(ctx_, "keep-open",
                          IniConfig::instance().get_mpv_keep_open() ? "yes" : "no");
    // Y14: subtitle settings now INI-configurable ([mpv] sub_align_x/y, sub_visibility, sub_ass_override).
    //   Defaults: center/bottom/yes/auto. For VIDEO: subtitles render in the video window (mpv native).
    //   For AUDIO (no video window): TUI lyric panel shows sub-text (gated by !has_video in draw_info).
    mpv_set_option_string(ctx_, "sub-ass-override",
                          IniConfig::instance().get_mpv_sub_ass_override().c_str());
    mpv_set_option_string(ctx_, "sub-align-x", IniConfig::instance().get_mpv_sub_align_x().c_str());
    mpv_set_option_string(ctx_, "sub-align-y", IniConfig::instance().get_mpv_sub_align_y().c_str());
    mpv_set_option_string(ctx_, "sub-visibility",
                          IniConfig::instance().get_mpv_sub_visibility().c_str());
    // Y24.43: prefer English among multiple embedded subtitle tracks (slang). mpv auto-selects the
    //   matching track → sub-text reflects the English subtitle. INI-configurable, default "en".
    mpv_set_option_string(ctx_, "slang", IniConfig::instance().get_mpv_sub_lang().c_str());
    // Y12: audio-display=no — don't open a video window for embedded cover art / pictures when
    //   playing audio files (mp3 with album art). Keeps audio playback windowless (no art popup).
    mpv_set_option_string(ctx_, "audio-display", "no");
    LOG(fmt::format("[MPV] Init: vo={}, vid={}, ao={}, ytdl-format={}, keep-open={}", mpv_vo,
                    mpv_vid, mpv_ao.empty() ? "auto" : mpv_ao, mpv_ytdl_format,
                    IniConfig::instance().get_mpv_keep_open() ? "yes" : "no"));

    // YouTube playback: resolve_youtube_url() pre-resolves every YouTube URL to a direct stream
    //   URL via `yt-dlp -g` (with --cookies + --proxy + --js-runtimes) BEFORE handing it to mpv, so
    //   mpv's ytdl hook is never used for YouTube and mpv needs no cookies/player_client here — one
    //   resolve path, no fallback. mpv only needs the proxy to fetch the resolved stream itself.
    {
        std::string proxy = IniConfig::instance().get_proxy();
        if (!proxy.empty()) {
            // Y24.53: FIX — mpv's proxy option is "http-proxy" (was "proxy" → mpv silently ignored
            //   it, NEVER used the proxy → geo-blocked streams failed even with [network] proxy set).
            //   Also set env vars for SOCKS proxy support (ffmpeg reads http_proxy/https_proxy;
            //   curl uses explicit CURLOPT_PROXY so env vars don't double-apply).
            mpv_set_option_string(ctx_, "http-proxy", proxy.c_str());
            setenv("http_proxy", proxy.c_str(), 1);
            setenv("https_proxy", proxy.c_str(), 1);
            LOG(fmt::format("[MPV] proxy: {} (http-proxy + env)", proxy));
        }
    }

    // F24: TLS verification from [mpv] section (default true, aligned with libcurl configuration)
    bool mpv_tls_verify = IniConfig::instance().get_mpv_tls_verify();
    mpv_set_option_string(ctx_, "tls-verify", mpv_tls_verify ? "yes" : "no");
    LOG(fmt::format("[MPV] TLS verify: {} (streaming {})", mpv_tls_verify ? "enabled" : "disabled",
                    mpv_tls_verify ? "secure" : "compatibility mode"));

    // F24: Network buffer configuration from [mpv] section — optimize streaming playback under VPN/high-latency
    std::string mpv_cache = IniConfig::instance().get_mpv_cache();
    std::string mpv_demuxer_max_bytes = IniConfig::instance().get_mpv_demuxer_max_bytes();
    std::string mpv_demuxer_max_back_bytes = IniConfig::instance().get_mpv_demuxer_max_back_bytes();
    int mpv_cache_secs = IniConfig::instance().get_mpv_cache_secs();
    mpv_set_option_string(ctx_, "cache", mpv_cache.c_str());
    mpv_set_option_string(ctx_, "demuxer-max-bytes", mpv_demuxer_max_bytes.c_str());
    mpv_set_option_string(ctx_, "demuxer-max-back-bytes", mpv_demuxer_max_back_bytes.c_str());
    mpv_set_option_string(ctx_, "cache-secs", std::to_string(mpv_cache_secs).c_str());
    LOG(fmt::format("[MPV] Network buffer: cache={}, demuxer-max-bytes={}, "
                    "demuxer-max-back-bytes={}, cache-secs={}",
                    mpv_cache, mpv_demuxer_max_bytes, mpv_demuxer_max_back_bytes, mpv_cache_secs));

    // F24: browser User-Agent from [mpv] section (some CDNs reject default mpv UA).
    //   Applied before mpv_initialize (option-string form). This complements the yt-dlp -g fallback
    //   in the event_loop for sites that still reject the player UA.
    std::string mpv_user_agent = IniConfig::instance().get_mpv_user_agent();
    mpv_set_option_string(ctx_, "user-agent", mpv_user_agent.c_str());
    LOG(fmt::format("[MPV] user-agent: {}", mpv_user_agent));
}

// Runtime proxy refresh (Ctrl+N ENTER): re-applies the CURRENT [network] proxy to the live
//   mpv context + env. curl/yt-dlp already resolve the proxy live per request (ProxyManager's
//   global source reads IniConfig each resolve); mpv's http-proxy option and the env vars were
//   the only init-time snapshot. Empty proxy → clear option + unsetenv (back to direct).
void MPVController::refresh_proxy() {
    if (!ctx_)
        return;
    std::string proxy = IniConfig::instance().get_proxy();
    mpv_set_option_string(ctx_, "http-proxy", proxy.c_str());
    if (!proxy.empty()) {
        setenv("http_proxy", proxy.c_str(), 1);
        setenv("https_proxy", proxy.c_str(), 1);
        LOG(fmt::format("[MPV] proxy refreshed: {} (http-proxy + env)", proxy));
    } else {
        unsetenv("http_proxy");
        unsetenv("https_proxy");
        LOG("[MPV] proxy cleared (direct) — http-proxy + env");
    }
}

} // namespace panicast
