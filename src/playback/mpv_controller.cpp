// MPV playback controller implementation.
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

std::string MPVController::cli_vo_override_;
std::string MPVController::cli_vid_override_;
std::string MPVController::cli_ao_override_;

MPVController::~MPVController() {
    running_ = false;
    // Async: detach the event thread (don't join — could hang on WSLg VO teardown). ctx_ is left
    //   valid (not destroyed) so the detached thread can use it until it exits. OS reclaims on exit.
    if (mpv_thread_.joinable())
        mpv_thread_.detach();
}

bool MPVController::initialize() {
    // Only switch LC_NUMERIC, preserve LC_CTYPE wide-char environment
    // mpv_create() detects locale and prints warnings, but only LC_NUMERIC needs to be "C"
    setlocale(LC_NUMERIC, "C");
    ctx_ = mpv_create();
    setlocale(LC_NUMERIC, ""); // restore

    if (!ctx_) {
        LOG("[MPV] Failed to create context");
        return false;
    }

    ytdl_path_ = Utils::which_binary("yt-dlp");
    while (!ytdl_path_.empty() && (ytdl_path_.back() == '\n' || ytdl_path_.back() == '\r'))
        ytdl_path_.pop_back();
    ytdl_available_ = !ytdl_path_.empty();
    LOG(fmt::format("[MPV] yt-dlp: {}", ytdl_available_ ? ytdl_path_ : "not found"));

    // F24: force-window defaults to "no" in mpv — no need to set explicitly.
    //   mpv opens a window only when a video track is present (correct behavior).
    // F24: all user-configurable mpv params (vo/vid/ytdl-format/cache/demuxer/tls/user-agent/keep-open)
    //   are read from the [mpv] section of IniConfig (single source of truth). CLI overrides
    //   (--vo/--vid/--quiet) take precedence over INI values.
    mpv_set_option_string(ctx_, "idle", "yes");
    // Center the video window (WSLg Weston doesn't auto-center) + keep on top (avoid being covered by terminal).
    //   Only effective when opening a window with vo=gpu; no-op when audio vo=null.
    mpv_set_option_string(ctx_, "geometry", "+50%+50%");
    mpv_set_option_string(ctx_, "ontop", "yes");

    // F25: terminal=no is the single correct mpv terminal option. It disables mpv's entire
    //   terminal output system (status line + log messages) AND terminal input — the input part
    //   is essential: otherwise mpv would grab stdin / put the terminal in raw mode and fight
    //   ncurses for keyboard control. Since terminal=no already fully suppresses mpv's own
    //   terminal writes, the former msg-level / quiet options were pure redundancy and are removed.
    //   terminal=no does NOT affect vo=gpu/vo=auto video windows.
    mpv_set_option_string(ctx_, "terminal", "no");
    // The TUI (ncurses) owns all keyboard/mouse input. Disable mpv's own key bindings so its
    //   video window never interprets keys (prevents conflicts / the "No key binding found for
    //   key X" key-eating seen in logs when the video window has focus).
    mpv_set_option_string(ctx_, "input-default-bindings", "no");

    mpv_set_option_string(ctx_, "ytdl", "yes");
    if (ytdl_available_) {
        // mpv >=0.36 removed the --ytdl-path option; the ytdl_hook now reads the path from script-opts
        //   (ytdl_hook-ytdl_path). Set it explicitly so playback finds the same yt-dlp that parsing uses
        //   (YtdlpRunner/which_binary), even when it isn't on $PATH. On older mpv that still has ytdl-path
        //   this is simply ignored and the PATH fallback applies — strictly additive, no regression.
        std::string so = "ytdl_hook-ytdl_path=" + ytdl_path_;
        mpv_set_option_string(ctx_, "script-opts", so.c_str());
    }

    // D47: apply all [mpv]-section options (vo/vid/ao/subtitles/proxy/tls/cache/user-agent) —
    //   extracted to apply_mpv_options_() (mpv_init.cpp). CLI overrides take precedence over INI.
    apply_mpv_options_();

    // F25: removed F23's process-global dup2(stderr → /dev/null) around mpv_initialize().
    //   Root cause of the F24 VO/AO init failure: dup2 mutated fd 2 / TTY state process-wide
    //   exactly during the VO/AO backend probe window inside mpv_initialize(), which broke VO/AO
    //   init in constrained sessions (Wayland / container / systemd, no controlling TTY).
    //   terminal=no already keeps mpv itself silent; the only stderr writers dup2 was hiding were
    //   ALSA/Pulse C libraries, and those only fire when mpv probes an *unavailable* backend. With
    //   ao left unset (auto), mpv probes pulse first — on this host AO=pulse succeeds (see log) and
    //   ALSA is never probed, so the library noise dup2 guarded against does not occur. Removing
    //   dup2 is a pure subtractive fix: no fd/TTY mutation, no save/restore. NOTE: VO/AO=null right
    //   after mpv_initialize() is the NORMAL lazy-init behavior of vo=auto+idle=yes, NOT a failure.
    if (mpv_initialize(ctx_) < 0) {
        // Must release ctx on init failure to avoid handle leak
        mpv_terminate_destroy(ctx_);
        ctx_ = nullptr;
        return false;
    }

    // Y24.17: subscribe to mpv log messages so the user can SEE what mpv does during BUFFERING
    //   (AO init, demuxer probe/index, cache fill). "info" = INFO+ (warn/error). INFO is logged only
    //   during the load window (logging_load_, set on play, cleared on FILE_LOADED) to avoid spam;
    //   WARN/ERROR always.
    mpv_request_log_messages(ctx_, "info");

    // Log actual VO/AO after init
    const char *vo_actual = mpv_get_property_string(ctx_, "current-vo");
    const char *ao_actual = mpv_get_property_string(ctx_, "current-ao");
    LOG(fmt::format("[MPV] Actual VO={}, AO={}", vo_actual ? vo_actual : "null",
                    ao_actual ? ao_actual : "null"));
    // Y24.8: warn prominently if the audio output driver failed to init — playback cannot produce
    //   sound (mpv will later emit AO_INIT_FAILED -14). Common on WSL2 when PulseAudio/WSLg is down.
    if (!ao_actual || ao_actual[0] == '\0') {
        EVENT_LOG("MPV: No audio output driver (AO=null) — playback will fail silently. "
                  "Set [mpv] ao (pulse/pipewire/alsa) or start PulseAudio/WSLg, then restart.");
    }
    if (vo_actual)
        mpv_free((void *)vo_actual);
    if (ao_actual)
        mpv_free((void *)ao_actual);

    // Y24.55: permanently redirect stderr → /dev/null AFTER mpv_initialize. PULSE/ALSA libraries
    //   (libpulse/libasound) write errors directly to stderr, bypassing mpv's terminal=no → garbles
    //   the ncurses TUI in WSL2. mpv's own AO errors are captured via the log-message callback
    //   (line 233) → shown in the LOG area. PaniCast's LOG uses a file, not stderr. Post-init
    //   redirect is safe: VO/AO probing inside mpv_initialize is already done (F25 showed during-init
    //   dup2 broke VO probing; this runs after).
    {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    }

    // Removed mpv_observe_property calls — event_loop doesn't handle
    //   PROPERTY_CHANGE events (all via update_state() polling); registering them is pure waste and
    //   pollutes the event queue; volume was also erroneously observed as INT64 (actually double).

    running_ = true;
    mpv_thread_ = std::thread(&MPVController::event_loop, this);
    cmd_running_ = true;
    cmd_done_ = false;
    cmd_thread_ = std::thread(&MPVController::cmd_loop_, this);
    return true;
}

void MPVController::stop() {
    if (!ctx_)
        return; // idempotent

    // Signal mpv to release resources: stop the stream + quit the core (async). "quit" triggers mpv
    //   core shutdown → AO/VO uninit; VO uninit destroys the wl_surface, which is what actually
    //   CLOSES the wlshm/WSLg video window. ctx_ is left valid so the event thread can finish its
    //   shutdown safely; a second stop() call re-sends the commands (harmless no-op).
    const char *stop_cmd[] = {"stop", nullptr};
    mpv_command(ctx_, stop_cmd); // release the media stream (async)
    // NOTE: deliberately do NOT set running_=false here. The event loop must keep driving mpv
    //   (calling mpv_wait_event) until it receives MPV_EVENT_SHUTDOWN — which mpv emits ONLY AFTER
    //   VO/AO uninit, i.e. AFTER the wl_surface is destroyed (that closes the wlshm/WSLg window).
    //   Setting running_=false would make the loop bail at the next while-check, possibly before
    //   SHUTDOWN/VO-uninit → the video window would stay open. "quit" below reliably produces
    //   SHUTDOWN; ~MPVController still sets running_=false as a fallback for the non-_exit path.
    const char *quit_cmd[] = {"quit", nullptr};
    mpv_command(ctx_, quit_cmd); // core shutdown (async) → VO uninit → SHUTDOWN event

    // Bounded wait for the VO to uninit so the wlshm/WSLg video window actually CLOSES before we
    //   exit. Previously this detached the event thread immediately, so the VO teardown raced with
    //   the following _exit(0) and the window was left open (a ghost window in WSLg) after the
    //   process exited — most visible right after playing a video. mpv_thread_done_ is set by
    //   event_loop() once it sees MPV_EVENT_SHUTDOWN (emitted AFTER VO/AO uninit), so waiting for it
    //   guarantees the surface is destroyed. VO teardown normally completes in <100ms; bound the
    //   wait at ~1.2s so a pathological WSLg teardown hang (the original reason for fire-and-forget)
    //   can't block the exit indefinitely — fall back to detach if it hasn't finished. stop() runs
    //   only on the exit path (never per-track), so this latency is exit-only.
    if (mpv_thread_.joinable()) {
        for (int i = 0; i < 120 && !mpv_thread_done_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (mpv_thread_done_.load())
            mpv_thread_.join(); // VO uninited → window closed; reap
        else
            mpv_thread_.detach(); // timed out → fire-and-forget (no hang)
    }

    // Stop the command worker. Bounded join mirroring the event thread: a worker blocked in a
    //   hung-mpv command can't join promptly → detach (process exits via _exit, so a detached
    //   worker never outlives a live controller).
    cmd_running_ = false;
    cmd_cv_.notify_all();
    if (cmd_thread_.joinable()) {
        for (int i = 0; i < 120 && !cmd_done_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (cmd_done_.load())
            cmd_thread_.join();
        else
            cmd_thread_.detach();
    }
}

void MPVController::enqueue_cmd_(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(cmd_mtx_);
        cmd_queue_.push(std::move(fn));
    }
    cmd_cv_.notify_one();
}

void MPVController::cmd_loop_() {
    // Runs interactive mpv commands OFF the UI thread so a hung mpv (e.g. a PulseAudio cork
    //   timeout on pause) cannot freeze the TUI. Commands run one-at-a-time in submission order;
    //   a blocked command blocks only this worker (the UI keeps running on optimistically-updated
    //   state_, refreshed by update_state on the event thread).
    while (cmd_running_.load()) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(cmd_mtx_);
            cmd_cv_.wait(lock, [this] { return !cmd_running_.load() || !cmd_queue_.empty(); });
            if (!cmd_queue_.empty()) {
                fn = std::move(cmd_queue_.front());
                cmd_queue_.pop();
            }
        }
        if (fn)
            fn();
    }
    cmd_done_.store(true);
}

void MPVController::play_list(const std::vector<std::string> &urls, bool is_video) {
    if (urls.empty())
        return;
    if (!ctx_)
        return;              // Guard against NULL handle segfault after initialize failure
    reset_iptv_detection_(); // Y24.55: re-arm per-track IPTV diagnostics

    // Playlist mode settings
    // keep-open=no allows the playlist to auto-play the next item
    // This fixes the issue where playback stopped after finishing one program
    int rc_keep_open = mpv_set_property_string(ctx_, "keep-open", "no");
    if (rc_keep_open < 0)
        LOG(fmt::format("[MPV] WARNING: set property keep-open failed (rc={})", rc_keep_open));
    LOG("[MPV] Playlist mode: keep-open=no for auto-play next");

    if (is_video) {
        if (!vo_gpu_) {
            mpv_set_property_string(ctx_, "vo", "auto");
            vo_gpu_ = true;
        }
    } else {
        // Don't change vo back to null (see vo_gpu_ note), avoids segfault on video->audio switch
    }

    std::string tmp = SafeTmpFile::create(".m3u");
    std::ofstream f(tmp);
    if (f.is_open()) {
        for (const auto &url : urls)
            f << url << "\n";
        f.close();
        const char *cmd[] = {"loadlist", tmp.c_str(), "replace", nullptr};
        int rc_loadlist = mpv_command(ctx_, cmd); // P3-C5: check return (was ignored)
        if (rc_loadlist < 0)
            LOG(fmt::format("[MPV] WARNING: loadlist failed (rc={})", rc_loadlist));
        SafeTmpFile::remove(tmp); // Clean up temp file after loading

        // Ensure playback starts (not paused)
        int pause_val = 0;
        int rc_pause = mpv_set_property(ctx_, "pause", MPV_FORMAT_FLAG, &pause_val);
        if (rc_pause < 0)
            LOG(fmt::format("[MPV] WARNING: set pause failed (rc={})", rc_pause));
        LOG("[MPV] play_list: Ensured playing (pause=no)");
    } else
        play(urls[0], is_video);
}

void MPVController::play_list_from(const std::vector<std::string> &urls, int start_idx,
                                   bool is_video) {
    if (urls.empty())
        return;
    if (!ctx_)
        return; // Guard against NULL handle segfault after initialize failure
    if (start_idx < 0)
        start_idx = 0;
    if (start_idx >= (int)urls.size())
        start_idx = urls.size() - 1;
    reset_iptv_detection_(); // Y24.55: re-arm per-track IPTV diagnostics

    // Playlist mode settings
    int rc_keep_open = mpv_set_property_string(ctx_, "keep-open", "no");
    if (rc_keep_open < 0)
        LOG(fmt::format("[MPV] WARNING: set property keep-open failed (rc={})", rc_keep_open));
    LOG(fmt::format("[MPV] Playlist mode: keep-open=no, start from idx {}", start_idx));

    if (is_video) {
        if (!vo_gpu_) {
            mpv_set_property_string(ctx_, "vo", "auto");
            vo_gpu_ = true;
        }
    } else {
        // Don't change vo back to null (see vo_gpu_ note), avoids segfault on video->audio switch
    }

    std::string tmp = SafeTmpFile::create(".m3u");
    std::ofstream f(tmp);
    if (f.is_open()) {
        for (const auto &url : urls)
            f << url << "\n";
        f.close();

        // Load the playlist
        const char *cmd[] = {"loadlist", tmp.c_str(), "replace", nullptr};
        int rc_loadlist = mpv_command(ctx_, cmd); // P3-C5: check return (was ignored)
        if (rc_loadlist < 0)
            LOG(fmt::format("[MPV] WARNING: loadlist failed (rc={})", rc_loadlist));
        SafeTmpFile::remove(tmp); // Clean up temp file after loading

        // Set playback position to the specified start index
        int64_t pos = start_idx;
        if (rc_loadlist >= 0)
            mpv_set_property(ctx_, "playlist-pos", MPV_FORMAT_INT64, &pos);
        LOG(fmt::format("[MPV] play_list_from: Set playlist-pos to {}", start_idx));

        // Ensure playback starts (not paused)
        int pause_val = 0;
        int rc_pause = mpv_set_property(ctx_, "pause", MPV_FORMAT_FLAG, &pause_val);
        if (rc_pause < 0)
            LOG(fmt::format("[MPV] WARNING: set pause failed (rc={})", rc_pause));
        LOG("[MPV] play_list_from: Ensured playing (pause=no)");
    } else
        play(urls[start_idx], is_video);
}

void MPVController::event_loop() {
    mpv_thread_done_.store(false); // reset for this run (for bounded join in stop())
    while (running_) {
        mpv_event *event = mpv_wait_event(ctx_, 0.05);
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            break;

        // Y24.17: mpv log messages — WARN/ERROR always; INFO only during the load window
        //   (logging_load_, set on play, cleared on FILE_LOADED) so AO/demuxer/cache events show
        //   during BUFFERING without spamming the log at other times.
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto *lm = static_cast<mpv_event_log_message *>(event->data);
            if (lm && lm->text) {
                std::string txt = lm->text;
                while (!txt.empty() && (txt.back() == '\n' || txt.back() == '\r'))
                    txt.pop_back();
                if (!txt.empty()) {
                    // Y24.55: remember the most recent warn/error line so a following END_FILE -13
                    //   (loading failed) can be sub-classified into 404/403/5xx/unreachable for the
                    //   IPTV context message. Only warn/error carry actionable keywords; INFO during
                    //   the load window is just progress noise.
                    if (lm->log_level <= MPV_LOG_LEVEL_WARN) {
                        last_log_text_ = txt;
                    }
                    if (lm->log_level <= MPV_LOG_LEVEL_WARN || logging_load_.load()) {
                        LOG(fmt::format("[MPV/log] {}: {}", lm->prefix ? lm->prefix : "", txt));
                    }
                }
            }
        }

        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            logging_load_ = false; // Y24.17: load window ends
            // Y24.55: mark when the stream actually loaded — the off-air / audio-only / slow
            //   detections time themselves relative to this (address OK, HTTP 200, mpv accepted it).
            //   Re-arm the one-shots here too so a re-load of the same URL re-evaluates cleanly.
            file_loaded_time_ = std::chrono::steady_clock::now();
            file_loaded_time_set_ = true;
            stuck_timing_ = false;
            offair_reported_ = false;
            audio_only_reported_ = false;
            slow_reported_ = false;
            had_playback_started_ = false;
            // Y24.8: log how long mpv took from loadfile → File loaded (helps spot slow local mounts
            //   / network buffering). Steady_clock default-constructed = epoch (timing disabled).
            int64_t saved_ms = last_loadfile_ms_.load();
            if (saved_ms > 0) {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
                LOG(fmt::format("[MPV] File loaded ({} ms after loadfile)", now_ms - saved_ms));
            } else {
                LOG("[MPV] File loaded");
            }
            EVENT_LOG("MPV: File loaded");
            // F26: Reset the INFO-area fields on track change. VO/AO/dimensions/bitrate/
            //   samplerate/channels/codec are NOT reliably available at the FILE_LOADED instant
            //   (mpv populates them asynchronously as decoding/buffering progresses). Reading them
            //   once here was a race — sometimes caught them populated, sometimes not, with no
            //   reproducibility (re-play re-fired FILE_LOADED at a different moment). They are now
            //   continuously re-read in update_state() with last-known-good semantics; here we only
            //   clear them so the previous track's info does not bleed into the new one.
            {
                std::lock_guard<std::mutex> slock(mtx_);
                state_.current_vo = "null";
                state_.current_ao = "null";
                state_.video_width = 0;
                state_.video_height = 0;
                state_.video_bitrate = 0;
                state_.audio_bitrate = 0;
                state_.audio_samplerate.clear();
                state_.audio_channels.clear();
                state_.audio_codec.clear();
                state_.video_codec.clear();
                state_.hwdec_current.clear();
                restart_info_logged_ =
                    false; // F28: re-arm one-shot decode-info log (fires on PLAYBACK_RESTART)
            }
            // Resume playback - restore to last position after file is loaded
            // time-pos must be set after FILE_LOADED; setting it too early mpv will ignore
            // Copy pending values and apply outside the lock to avoid racing with set_resume_position
            std::string resume_url;
            double resume_pos = 0.0;
            {
                std::lock_guard<std::mutex> lock(cb_mtx_);
                resume_url = pending_resume_url_;
                resume_pos = pending_resume_pos_;
                pending_resume_url_.clear();
                pending_resume_pos_ = 0.0;
            }
            if (!resume_url.empty() && resume_pos > 5.0) {
                int rc = mpv_set_property(ctx_, "time-pos", MPV_FORMAT_DOUBLE, &resume_pos);
                if (rc >= 0) {
                    LOG(fmt::format("[MPV] Resumed to {:.1f}s for {}", resume_pos, resume_url));
                    EVENT_LOG(fmt::format("Resumed: {:.0f}:{:02d}",
                                          static_cast<int>(resume_pos) / 60,
                                          static_cast<int>(resume_pos) % 60));
                } else {
                    LOG(fmt::format("[MPV] Resume failed (rc={}, url={})", rc, resume_url));
                }
            }
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            if (!event->data) {
                LOG("[MPV] End file event with null data");
            } else {
                handle_end_file_((mpv_event_end_file *)event->data);
            }
        } else if (event->event_id == MPV_EVENT_SEEK) {
            LOG("[MPV] Seek event");
        } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            LOG("[MPV] Playback restart");
            EVENT_LOG("MPV: Playback restart");
            had_playback_started_ =
                true; // Y24.55: playback actually began → a later END_FILE r=4 is a mid-playback drop (#12), not a load failure
            // F28: log video codec + hwdec once per track, here — because PLAYBACK_RESTART fires
            //   AFTER the decoder has initialized (unlike FILE_LOADED, where hwdec-current is not yet
            //   set), so codec/bitrate/hwdec are all confirmed ready. Guarded to once-per-track so
            //   seeks within a track (which also fire PLAYBACK_RESTART) don't re-log.
            if (!restart_info_logged_) {
                restart_info_logged_ = true;
                log_track_codec_info_();
            }
        }

        update_state();
    }
    mpv_thread_done_.store(true); // signal exit for bounded join in stop()
}

// D36: log codec/bitrate/hwdec once per track. Called from event_loop's PLAYBACK_RESTART
//   branch (decoder initialized, properties ready). Guarded once-per-track by the caller.
void MPVController::log_track_codec_info_() {
    const char *vc = mpv_get_property_string(ctx_, "video-codec");
    const char *hw = mpv_get_property_osd_string(ctx_, "hwdec-current");
    const char *ac = mpv_get_property_string(ctx_, "audio-codec");
    std::string vcodec = (vc && vc[0]) ? vc : "";
    std::string hwdec = (hw && hw[0] && strcmp(hw, "no") != 0) ? hw : "";
    std::string acodec = (ac && ac[0]) ? ac : "";
    if (vc)
        mpv_free((void *)vc);
    if (hw)
        mpv_free((void *)hw);
    if (ac)
        mpv_free((void *)ac);
    // Y16: expand to full two-line codec info (resolution, bitrate, channels, samplerate).
    //   Reads the same properties as update_state but logs them once (not per-frame).
    int64_t vw = 0, vh = 0;
    mpv_get_property(ctx_, "width", MPV_FORMAT_INT64, &vw);
    mpv_get_property(ctx_, "height", MPV_FORMAT_INT64, &vh);
    double vbr = 0;
    mpv_get_property(ctx_, "video-bitrate", MPV_FORMAT_DOUBLE, &vbr);
    double abr = 0;
    mpv_get_property(ctx_, "audio-bitrate", MPV_FORMAT_DOUBLE, &abr);
    char *asr = mpv_get_property_osd_string(ctx_, "audio-params/samplerate");
    char *ach = mpv_get_property_osd_string(ctx_, "audio-params/channel-count");
    std::string samplerate = (asr && asr[0]) ? asr : "";
    std::string channels = (ach && ach[0]) ? ach : "";
    if (asr)
        mpv_free(asr);
    if (ach)
        mpv_free(ach);

    if (!vcodec.empty()) {
        std::string hwdec_disp = hwdec.empty() ? "software" : hwdec;
        // Y16: full line — Video: <codec> <WxH> <bitrate>kbps [<hwdec>]
        std::string vline = fmt::format("Video: {} {}x{}", vcodec, (int)vw, (int)vh);
        if (vbr > 0)
            vline += fmt::format(" {}kbps", (int)(vbr / 1000));
        vline += fmt::format(" [{}]", hwdec_disp);
        LOG(fmt::format("[MPV] {}", vline));
        EVENT_LOG(vline);
    }
    if (!acodec.empty()) {
        // Y16: full line — Audio: <codec> <channels>ch <samplerate>Hz <bitrate>kbps
        std::string aline = fmt::format("Audio: {}", acodec);
        if (!channels.empty())
            aline += fmt::format(" {}ch", channels);
        if (!samplerate.empty())
            aline += fmt::format(" {}Hz", samplerate);
        if (abr > 0)
            aline += fmt::format(" {}kbps", (int)(abr / 1000));
        LOG(fmt::format("[MPV] {}", aline));
        EVENT_LOG(aline);
    }
}


// D41: END_FILE event handler (Extract Method from event_loop). Dispatches on mpv's
//   end-file reason (0=EOF, 2=stop, 3=quit, 4=error, 5=redirect): logs a human-readable
//   message, runs the reason=4 error path (VO-init-failed audio-only fallback + IPTV
//   context message), then invokes the end-file callback outside the callback mutex.
void MPVController::handle_end_file_(mpv_event_end_file *ef) {
    int reason = static_cast<int>(ef->reason);
    int error_code = static_cast<int>(ef->error);
    // Y24.8: human-readable reason/error (was raw "reason: X, error: Y").
    //   mpv end-file reasons: 0=EOF, 2=stop, 3=quit, 4=error, 5=redirect.
    LOG(fmt::format("[MPV] End file: {}{}", end_file_reason_str(reason),
                    reason == 4 ? fmt::format(" — {}", mpv_error_str(error_code))
                                : ""));
    if (reason == 0) {
        EVENT_LOG("MPV: Track ended");
    } else if (reason == 4) {
        handle_playback_error_(error_code);
    } else if (reason == 2) {
        EVENT_LOG("MPV: Stopped");
    } else if (reason == 3) {
        EVENT_LOG("MPV: Quitting");
    } else if (reason == 5) {
        EVENT_LOG("MPV: Redirected");
    }

    // Call end-file callback (call outside lock to avoid executing user callback while holding lock)
    EndFileCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        cb = end_file_callback_;
    }
    if (cb)
        cb(reason);
}

// D41: END_FILE reason=4 (playback error) path (Extract Method from handle_end_file_).
//   Translates the mpv error to a human message, runs the -15 VO_INIT_FAILED audio-only
//   fallback (vo=null + vid=no + retry same URL once — GPU-less/SSH hosts), and emits the
//   IPTV context message (mid-playback drop vs load-failure, by had_playback_started_).
void MPVController::handle_playback_error_(int error_code) {
    // Y24.8: translate the mpv error code to a human message + hint.
    std::string err_msg = mpv_error_str(error_code);
    LOG(fmt::format("[MPV] Playback error: {}", err_msg));
    EVENT_LOG(fmt::format("MPV: {}", err_msg));

    // -15 VO_INIT_FAILED → fall back to audio-only (vo=null + vid=no) + retry same URL once.
    //   GPU-less / stale-DISPLAY hosts (SSH) hit this.
    std::string load_url;
    bool do_vo_fallback = false;
    {
        std::lock_guard<std::mutex> lock(cb_mtx_);
        load_url = last_load_url_;
        if (error_code == -15 && !vo_fallback_done_ && !load_url.empty()) {
            do_vo_fallback = true;
            vo_fallback_done_ = true;
        }
    }
    if (do_vo_fallback) {
        LOG("[MPV] VO init failed, falling back to audio-only");
        EVENT_LOG("MPV: VO init failed, falling back to audio-only");
        mpv_set_property_string(ctx_, "vo", "null");
        mpv_set_property_string(ctx_, "vid", "no");
        mpv_set_property_string(ctx_, "ytdl-format", "bestaudio/best");
        const char *retry_cmd[] = {"loadfile", load_url.c_str(), "replace",
                                   nullptr};
        int rc = mpv_command(ctx_, retry_cmd);
        LOG(fmt::format("[MPV] VO-fallback retry loadfile result: {} ({})", rc,
                        load_url));
    }
    // -14 AO_INIT_FAILED: no code-side fallback (can't play sound without an AO).
    //   The human-readable message above tells the user to check [mpv] ao / PulseAudio / WSLg.

    // Y24.55: IPTV context message — emitted AFTER the MPV: behavior message(s) above
    //   so the same event prints both in time order (MPV: cause → IPTV: explanation).
    //   Both reach the on-screen LOG area and panicast.log via EVENT_LOG. Only when the
    //   app flagged this load as IPTV context. -13 is sub-classified via last_log_text_.
    if (iptv_context_.load()) {
        // Y24.55: if playback had actually started (PLAYBACK_RESTART fired), this
        //   END_FILE r=4 is a mid-playback drop (#12) regardless of error code;
        //   otherwise it's a load/init failure (#1-4/#6/#8/#9/#10) by error code.
        std::string iptv_msg = had_playback_started_
                                   ? "IPTV: stream dropped mid-playback — source "
                                     "interrupted; switch channel or retry"
                                   : iptv_message_for_error_(error_code);
        if (!iptv_msg.empty())
            EVENT_LOG(iptv_msg);
    }
}

void MPVController::update_state() {
    // Null pointer check to prevent segfault
    if (!ctx_)
        return;

    // Y11: unified playback-state refresh timer (INI [display] state_refresh_ms, default 100ms).
    //   ALL playback-state reads (codec/bitrate/network/VO/AO/position/...) happen at this cadence;
    //   between ticks the last state_ is kept. One timer, INI-tunable, simplest.
    auto now = std::chrono::steady_clock::now();
    int refresh_ms = IniConfig::instance().get_display_state_refresh_ms();
    if (now - last_state_refresh_ < std::chrono::milliseconds(refresh_ms))
        return;
    last_state_refresh_ = now;

    int p = 0;
    mpv_get_property(ctx_, "pause", MPV_FORMAT_FLAG, &p);

    char *path = nullptr;
    int has_path = mpv_get_property(ctx_, "path", MPV_FORMAT_STRING, &path);

    double dv = 100.0; // volume property is actually double
    mpv_get_property(ctx_, "volume", MPV_FORMAT_DOUBLE, &dv);

    double sp = 1.0;
    mpv_get_property(ctx_, "speed", MPV_FORMAT_DOUBLE, &sp);

    char *t = nullptr;
    mpv_get_property(ctx_, "media-title", MPV_FORMAT_STRING, &t);

    int idle = 1;
    mpv_get_property(ctx_, "core-idle", MPV_FORMAT_FLAG, &idle);

    char *codec = nullptr;
    mpv_get_property(ctx_, "audio-codec", MPV_FORMAT_STRING, &codec);

    char *vcodec = nullptr;
    mpv_get_property(ctx_, "video-codec", MPV_FORMAT_STRING, &vcodec);

    // F26: VO/AO/dimensions/bitrate/audio-params are read continuously (not once at FILE_LOADED)
    //   because mpv populates them asynchronously. Last-known-good: only overwrite when mpv
    //   actually returns a value, so a transient unavailable/0 read does not erase known info.
    char *vo = nullptr;
    mpv_get_property(ctx_, "current-vo", MPV_FORMAT_STRING, &vo);
    char *ao = nullptr;
    mpv_get_property(ctx_, "current-ao", MPV_FORMAT_STRING, &ao);
    int64_t vw = 0, vh = 0;
    mpv_get_property(ctx_, "width", MPV_FORMAT_INT64, &vw);
    mpv_get_property(ctx_, "height", MPV_FORMAT_INT64, &vh);
    double vbr = 0;
    mpv_get_property(ctx_, "video-bitrate", MPV_FORMAT_DOUBLE, &vbr);
    double abr = 0;
    mpv_get_property(ctx_, "audio-bitrate", MPV_FORMAT_DOUBLE, &abr);
    char *asr = mpv_get_property_osd_string(ctx_, "audio-params/samplerate");
    char *ach = mpv_get_property_osd_string(ctx_, "audio-params/channel-count");
    // F27: hwdec-current = active hardware decoder method (e.g. "vaapi-copy"); empty/"no" = software.
    char *hwdec = mpv_get_property_osd_string(ctx_, "hwdec-current");

    double time_pos = 0.0;
    mpv_get_property(ctx_, "time-pos", MPV_FORMAT_DOUBLE, &time_pos);

    double duration = 0.0;
    mpv_get_property(ctx_, "duration", MPV_FORMAT_DOUBLE, &duration);

    int64_t pl_pos = -1;
    mpv_get_property(ctx_, "playlist-pos", MPV_FORMAT_INT64, &pl_pos);

    int64_t pl_count = 0;
    mpv_get_property(ctx_, "playlist-count", MPV_FORMAT_INT64, &pl_count);

    // Y11: network/stream health for the INFO "Network: | Buffering:" line.
    int64_t cache_speed = 0;
    mpv_get_property(ctx_, "cache-speed", MPV_FORMAT_INT64, &cache_speed);
    double buf_dur = 0.0;
    mpv_get_property(ctx_, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &buf_dur);
    int64_t buf_pct = 0;
    mpv_get_property(ctx_, "cache-buffering-state", MPV_FORMAT_INT64, &buf_pct);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        state_.paused = p;
        // Key fix - use has_path >= 0 && path to check
        state_.has_media = (has_path >= 0 && path);
        state_.volume = (int)(dv + 0.5); // Round double back to int
        state_.speed = sp;
        state_.time_pos = time_pos;
        state_.media_duration = duration;
        state_.playlist_pos = (int)pl_pos;
        state_.playlist_count = (int)pl_count;
        if (t)
            state_.title = t;
        if (path)
            state_.current_url = path;
        else if (state_.has_media == false)
            state_.current_url
                .clear(); // Clear after track ends, avoid stale URL causing "now playing" highlight misalignment
        state_.core_idle = idle;
        if (codec)
            state_.audio_codec = codec;
        if (vcodec)
            state_.video_codec = vcodec;
        // Runtime detection of whether a video track exists
        // If the video-codec property exists and is non-empty, there is a video stream
        state_.has_video = (vcodec != nullptr && strlen(vcodec) > 0);
        // F26: VO/AO track the current value (audio-only → "null", which hides the VO line in UI);
        //   dimensions/bitrate use last-known-good (only update when mpv reports a real value).
        if (vo)
            state_.current_vo = vo;
        if (ao)
            state_.current_ao = ao;
        if (vw > 0)
            state_.video_width = (int)vw;
        if (vh > 0)
            state_.video_height = (int)vh;
        if (vbr > 0)
            state_.video_bitrate = (int)(vbr / 1000); // bps → kbps
        if (abr > 0)
            state_.audio_bitrate = (int)(abr / 1000); // bps → kbps
        if (asr && asr[0])
            state_.audio_samplerate = asr;
        if (ach && ach[0])
            state_.audio_channels = ach;
        // F27: hwdec-current tracks current value (only non-empty while a video track is HW-decoding).
        //   Treat "no"/empty as software; store raw otherwise. (F28: the one-shot LOG moved to
        //   PLAYBACK_RESTART; update_state only feeds the live INFO display.)
        if (hwdec && hwdec[0] && strcmp(hwdec, "no") != 0)
            state_.hwdec_current = hwdec;
        else
            state_.hwdec_current.clear();
        // Y11: network/stream health (last-known-good: only overwrite when mpv returns a value).
        state_.net_speed_bps = (double)cache_speed;
        state_.buffering_sec = buf_dur;
        state_.buffering_pct = (int)buf_pct;
        // Y12: current subtitle/lyric line (mpv sub-text). Empty when no sub active (between lines / no sub).
        char *subtxt = nullptr;
        if (mpv_get_property(ctx_, "sub-text", MPV_FORMAT_STRING, &subtxt) >= 0 && subtxt) {
            state_.sub_text = subtxt;
            mpv_free(subtxt);
        } else {
            state_.sub_text.clear();
        }
    }

    // D46: IPTV runtime diagnostics extracted to detect_iptv_states_() (mpv_iptv.cpp) — off-air
    //   (#5) / audio-only (#7) / slow (#11) detection. Inputs derived from the property reads
    //   above (still valid here — the mpv_free cleanup runs after this call).
    detect_iptv_states_((has_path >= 0 && path), (codec != nullptr && codec[0] != '\0'),
                        (vcodec != nullptr && vcodec[0] != '\0'), idle, cache_speed, buf_pct,
                        buf_dur);

    if (path)
        mpv_free(path);
    if (t)
        mpv_free(t);
    if (codec)
        mpv_free(codec);
    if (vcodec)
        mpv_free(vcodec);
    if (vo)
        mpv_free(vo);
    if (ao)
        mpv_free(ao);
    if (asr)
        mpv_free(asr);
    if (ach)
        mpv_free(ach);
    if (hwdec)
        mpv_free(hwdec);
}

} // namespace panicast
