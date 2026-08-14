#include "panicast/playback/mpv_controller.h"
#include <clocale> // setlocale
#include <chrono>  // steady_clock (bounded join in stop())
#include <cstring> // strlen
#include <thread>  // std::this_thread::sleep_for (bounded VO-teardown wait in stop())
#include <fcntl.h> // open, O_WRONLY (Y24.55: stderr redirect)
#include <fstream>
#include <sstream>
#include <string>   // std::to_string
#include <unistd.h> // dup2, close, STDERR_FILENO (Y24.55)
#include <fmt/format.h>
#include "panicast/config/ini_config.h"
#include "panicast/core/constants.h"
#include "panicast/core/event_log.h"
#include "panicast/core/logger.h"
#include "panicast/core/safe_tmp.h"
#include "panicast/core/utils.h"
#include "panicast/net/url_classifier.h"
#include "panicast/net/ytdlp_runner.h"
#include "panicast/storage/accounts.h"
#include "panicast/parsers/youtube_channel_parser.h"

namespace panicast
{

void MPVController::play_audio(const std::string &url) {
    if (url.empty()) {
        LOG("[MPV] Error: Empty URL");
        return;
    }
    LOG(fmt::format("[MPV] Playing audio: {}", url));

    if (!ctx_) {
        LOG("[MPV] Error: ctx_ is null");
        return;
    }

    // Y24.12: audio fast-start profile — a smaller buffer so long podcast episodes start playing
    //   after ~5s (not 10s) and stream while buffering. Local files fill this near-instantly (disk
    //   throughput >> realtime), so cache=yes doesn't cause long BUFFERING for them — the >5s cases
    //   are mpv's open/probe/index per-op latency on slow mounts (WSL2 /mnt/e), diagnosed via the
    //   mpv log subscription + play_current timestamps added in Y24.17. Options are process-global
    //   last-set-wins, so play_video() restores the larger buffer.
    auto &ini = IniConfig::instance();
    mpv_set_option_string(ctx_, "cache-secs",
                          std::to_string(ini.get_mpv_audio_cache_secs()).c_str());
    mpv_set_option_string(ctx_, "demuxer-max-bytes", ini.get_mpv_audio_demuxer_max_bytes().c_str());
    mpv_set_option_string(ctx_, "demuxer-max-back-bytes",
                          ini.get_mpv_audio_demuxer_max_back_bytes().c_str());
    mpv_set_option_string(ctx_, "cache-pause-wait",
                          std::to_string(ini.get_mpv_audio_cache_pause_wait()).c_str());
    LOG(fmt::format(
        "[MPV] Audio fast-start: cache-secs={}, demuxer-max-bytes={}, cache-pause-wait={}",
        ini.get_mpv_audio_cache_secs(), ini.get_mpv_audio_demuxer_max_bytes(),
        ini.get_mpv_audio_cache_pause_wait()));

    // F20: keep-open is NOT set here — play_current controls it (keep-open=no for CYCLE/SHUFFLE
    //   so EOF fires → auto-advance; keep-open=yes for REPEAT). Setting it here would override
    //   play_current's choice and prevent auto-advance (the PAUSE bug).

    // F23: no force-window setting — mpv default (no) handles it: no window for audio-only.
    // Don't change vo back to null: see vo_gpu_ note, avoids segfault on video->audio switch.

    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        last_load_url_ = url; // record for END_FILE -15 VO fallback
        vo_fallback_done_ = false;
    }

    const char *cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
    last_loadfile_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count(); // Y24.8: loadfile→File loaded timing
    int result = mpv_command(ctx_, cmd);
    LOG(fmt::format("[MPV] loadfile result: {}", result));

    // Ensure playback starts (not paused)
    int pause_val = 0;
    int rc_pause = mpv_set_property(ctx_, "pause", MPV_FORMAT_FLAG, &pause_val);
    if (rc_pause < 0)
        LOG(fmt::format("[MPV] WARNING: set pause failed (rc={})", rc_pause));
    LOG("[MPV] Ensured playing (pause=no)");
}

void MPVController::play_video(const std::string &url, const std::string &audio_file,
                               const std::string &sub_file) {
    if (url.empty()) {
        LOG("[MPV] Error: Empty URL");
        return;
    }
    LOG(fmt::format("[MPV] Playing video: {}", url));

    if (!ctx_) {
        LOG("[MPV] Error: ctx_ is null");
        return;
    }

    // F20: keep-open NOT set here — play_current controls it (same as play_audio).

    // F24: do NOT set vo/vid here — mpv handles VO activation automatically using the init config.
    // Y09/Y10 (1A DASH) + Y11 (subtitles): attach external audio (DASH) and/or subtitle streams as
    //   per-file options via loadfile's <options> arg (comma-separated `key=value`). mpv has no
    //   `audio-file`/`sub-file` runtime PROPERTY (only the list forms) — the per-file OPTIONS string
    //   (loadfile 4th arg; 3rd arg = index "-1" since mpv 0.38) is the correct mechanism. vtt subs
    //   loaded this way respond to sub-scale/sub-pos/sub-delay and are centered by default.
    LOG("[MPV] Video mode: loading (vo/vid from init config)");

    // Y24.12: restore the larger [mpv] video cache (play_audio() shrinks it for podcast fast-start;
    //   options are process-global last-set-wins, so video must reset them to avoid rebuffers on
    //   YouTube/TikTok 1080p). Local video fills it fast (disk throughput); long BUFFERING on local
    //   video is mpv open/probe latency, not cache — diagnosed via Y24.17 mpv log + timestamps.
    auto &ini = IniConfig::instance();
    mpv_set_option_string(ctx_, "cache-secs", std::to_string(ini.get_mpv_cache_secs()).c_str());
    mpv_set_option_string(ctx_, "demuxer-max-bytes", ini.get_mpv_demuxer_max_bytes().c_str());
    mpv_set_option_string(ctx_, "demuxer-max-back-bytes",
                          ini.get_mpv_demuxer_max_back_bytes().c_str());
    mpv_set_option_string(ctx_, "cache-pause-wait", "10");
    LOG(fmt::format("[MPV] Video cache restored: cache-secs={}, demuxer-max-bytes={}",
                    ini.get_mpv_cache_secs(), ini.get_mpv_demuxer_max_bytes()));

    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        last_load_url_ = url; // record for END_FILE -15 VO fallback
        vo_fallback_done_ = false;
    }

    // Build the per-file options string: audio-file=<a>,sub-file=<s> (whichever are present).
    std::string opts;
    if (!audio_file.empty())
        opts += "audio-file=" + audio_file;
    if (!sub_file.empty())
        opts += (opts.empty() ? "" : ",") + std::string("sub-file=") + sub_file;

    int result;
    last_loadfile_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count(); // Y24.8: loadfile→File loaded timing
    if (!opts.empty()) {
        const char *cmd[] = {"loadfile", url.c_str(), "replace", "-1", opts.c_str(), nullptr};
        result = mpv_command(ctx_, cmd);
        LOG(fmt::format("[MPV] loadfile (+{}) result: {}", opts, result));
    } else {
        const char *cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
        result = mpv_command(ctx_, cmd);
        LOG(fmt::format("[MPV] loadfile result: {}", result));
    }

    // Ensure playback starts (not paused)
    int pause_val = 0;
    int rc_pause = mpv_set_property(ctx_, "pause", MPV_FORMAT_FLAG, &pause_val);
    if (rc_pause < 0)
        LOG(fmt::format("[MPV] WARNING: set pause failed (rc={})", rc_pause));
    LOG("[MPV] Ensured playing (pause=no)");
}

void MPVController::play(const std::string &url, bool force_video, const std::string &audio_file,
                         const std::string &sub_file) {
    URLType type = URLClassifier::classify(url);
    logging_load_ = true; // Y24.17: start the load window — log mpv INFO events until FILE_LOADED
    reset_iptv_detection_(); // Y24.55: re-arm per-track IPTV diagnostics for the new channel

    // F23: no display detection — always route video to play_video. mpv's vo=auto handles
    //   VO selection (wlshm on WSL2, gpu on native Linux, null if nothing). If no display,
    //   mpv falls back to audio-only automatically.
    bool should_play_video =
        force_video || type == URLType::VIDEO_FILE || type == URLType::YOUTUBE_VIDEO;

    if (URLClassifier::is_youtube(type)) {
        should_play_video = true;
    }

    if (should_play_video) {
        play_video(url, audio_file, sub_file);
    } else {
        play_audio(url);
    }
}

} // namespace panicast
