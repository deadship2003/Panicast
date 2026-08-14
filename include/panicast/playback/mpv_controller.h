// MPV playback controller: wraps the mpv client API, managing playback context, the event thread, and state polling.
//   - initialize/play/play_audio/play_video/play_list/play_list_from and other playback controls
//   - the event_loop thread handles FILE_LOADED/END_FILE/SEEK etc., supporting resume playback
//   - EndFileCallback notifies the upper layer when a track ends (used for auto-play next)
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <mpv/client.h>

#include "panicast/core/constants.h"

namespace panicast
{

class MPVController {
public:
    //Playback end callback type
    // reason: 0=EOF (normal end), 1=stop, 2=quit, 3=error, 4=redirect, 5=interrupted
    using EndFileCallback = std::function<void(int reason)>;

    ~MPVController();

    bool initialize();
    static void set_cli_overrides(const std::string &vo, const std::string &vid,
                                  const std::string &ao); // CLI --vo/--vid/--ao overrides
    static std::string cli_vo_override_;                  // empty = no override
    static std::string cli_vid_override_;                 // empty = no override
    static std::string cli_ao_override_; // F29: empty = no override (mpv default auto)
    // Explicit stop — call before App destruction to ensure the mpv event thread is joined
    //   before current_playlist/playlist_mutex_ and other members are destroyed, avoiding callbacks accessing freed objects
    void stop();

    void play_audio(const std::string &url);
    void play_video(const std::string &url, const std::string &audio_file = "",
                    const std::string &sub_file = "");
    void play(const std::string &url, bool force_video = false, const std::string &audio_file = "",
              const std::string &sub_file = "");

    void play_list(const std::vector<std::string> &urls, bool is_video = false);
    // Used for L mode: full playlist, starting playback from the given index
    void play_list_from(const std::vector<std::string> &urls, int start_idx, bool is_video = false);

    void toggle_pause();
    void set_pause(bool paused); // explicitly set pause state (for auto-advance after keep-open)
    void set_volume(int vol);
    void adjust_speed(bool faster);
    void reset_speed();
    //Directly set playback speed
    void set_speed(double s);
    void
    set_keep_open(bool keep); // keep-open=yes pauses at EOF; no lets END_FILE fire for auto-advance
    // Y24.17: add an external subtitle file to the running playback (mpv sub-add, select immediately).
    //   Used by the async video-sidecar probe — the sub-file URL isn't known at loadfile time, so it's
    //   added after play once the async probe finds it. Thread-safe (mpv client API).
    void sub_add(const std::string &url);
    // Y24.28: show OSD text in the video window (for ASR progress).
    void show_osd(const std::string &text, int duration_ms = 2000);
    // Y24.43: is the mpv VIDEO OUTPUT window actually rendering? (current-vo != "null"/empty).
    //   Distinct from State.has_video (which only means the file has a video track). A video file
    //   played audio-only (vo=null/vid=no) has has_video possibly false AND is_video_window_open false.
    bool is_video_window_open() const;
    // Y24.47: is the user in audio-only mode (vo=null or vid=no, via --quiet / INI)? This is the
    //   playback INTENT (configured before play starts), used to pick an audio-only stream format for
    //   YouTube/adaptive sources so the video stream is never downloaded (saves bandwidth). Static —
    //   depends only on CLI overrides + INI, not instance/runtime state.
    static bool is_audio_only_mode();
    // Y24.43: does the current media have an embedded subtitle track? Queries mpv track-list for a
    //   type=sub track (reliable across subtitle cue gaps, unlike sub-text which is empty between cues).
    bool has_active_subtitle() const;
    // When loop=true loop the current file, when loop=false cancel looping
    void set_loop_file(bool loop);
    // When loop=true loop the entire playlist, when loop=false cancel looping
    void set_loop_playlist(bool loop);

    struct State {
        bool paused = true, has_media = false, core_idle = true;
        int volume = MAX_VOLUME;
        double speed = DEFAULT_SPEED;
        double time_pos = 0.0;       // Current playback position
        double media_duration = 0.0; // Total media duration
        std::string title, current_url, audio_codec, video_codec;
        bool has_video = false; // Whether a video track exists (detected at runtime)
        int playlist_pos = -1;  // Current playlist position (0-based)
        int playlist_count = 0; // Total playlist count
        // F22: VO/AO runtime info for INFO area display
        std::string current_vo;       // e.g. "gpu", "wlshm", "null"
        std::string current_ao;       // e.g. "pulse", "alsa"
        int video_width = 0;          // Video resolution width
        int video_height = 0;         // Video resolution height
        int video_bitrate = 0;        // Video bitrate (kbps)
        int audio_bitrate = 0;        // Audio bitrate (kbps)
        std::string audio_samplerate; // e.g. "48000"
        std::string audio_channels;   // e.g. "2"
        std::string
            hwdec_current; // F27: active hwdec method (e.g. "vaapi-copy"); empty/"no" = software decode
        // Y11: network/stream health for the INFO area "Network: ... | Buffering: ..." line.
        //   cache-speed = demuxer download rate (bytes/sec); demuxer-cache-duration = seconds
        //   buffered ahead; cache-buffering-state = 0-100 while buffering (>0 means buffering).
        double net_speed_bps = 0.0; // cache-speed (bytes/sec)
        double buffering_sec = 0.0; // demuxer-cache-duration (seconds buffered ahead)
        int buffering_pct = 0;      // cache-buffering-state (0-100; >0 = buffering in progress)
        // Y12: current subtitle/lyric text (mpv `sub-text` — the active line at time_pos).
        //   For local .lrc (auto-loaded external sub) and YouTube soft subs (sub-file). Empty between lines.
        std::string sub_text;
    };

    State get_state();
    mpv_handle *get_handle();

    //Set the playback end callback
    void set_end_file_callback(EndFileCallback callback);

    // Y24.8: human-readable mpv end-file reason / error code (replaces raw ints in logs).
    //   reason: 0=EOF, 2=stopped, 3=quit, 4=error, 5=redirect.
    //   error: mpv error code (e.g. -14=AO init failed). "" / 0 = no error.
    static const char *end_file_reason_str(int reason);
    static std::string mpv_error_str(int error);

    // Y24.55: mark the next/ongoing load as IPTV context. While on, the controller emits IPTV:
    //   context messages alongside the existing MPV: behavior messages for the same event (off-air,
    //   audio-only, slow, load/AO/VO/decode failures). Set by the app before play when mode==IPTV.
    //   Same event → both MPV: and IPTV: are printed (MPV: first as the cause, IPTV: second as the
    //   explanation); both reach the on-screen LOG area and panicast.log via EVENT_LOG.
    void set_iptv_context(bool on);

    // Set the playback position to resume from
    // Call before play/play_audio/play_video
    // Automatically applied and cleared after the FILE_LOADED event fires
    void set_resume_position(const std::string &url, double position);

    // Re-apply the [network] proxy to the live mpv context (http-proxy option) + the
    //   http_proxy/https_proxy env vars (read by ffmpeg for SOCKS). Called at init via
    //   apply_mpv_options_() and again by Ctrl+N's ENTER so a proxy edit takes effect
    //   for playback immediately — no program restart (mpv options are runtime-writable).
    void refresh_proxy();

private:
    mpv_handle *ctx_ = nullptr;
    std::thread mpv_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> mpv_thread_done_{false}; // set by event_loop when it exits (for bounded join)
    // Command worker: interactive mpv commands (pause/volume/speed/...) execute HERE, off the UI
    //   thread, so a hung mpv (e.g. a PulseAudio cork timeout on pause) cannot freeze the TUI.
    //   The UI returns immediately (state_ updated optimistically); the worker runs the mpv call.
    std::thread cmd_thread_;
    std::mutex cmd_mtx_;
    std::condition_variable cmd_cv_;
    std::queue<std::function<void()>> cmd_queue_;
    std::atomic<bool> cmd_running_{false};
    std::atomic<bool> cmd_done_{false};
    void enqueue_cmd_(std::function<void()> fn);
    void cmd_loop_();
    std::mutex mtx_;
    State state_;
    bool ytdl_available_ = false;
    std::string ytdl_path_;
    // Track whether vo has been switched to gpu (avoid repeated null↔gpu switching at runtime causing segfaults).
    //   Initialized to vo=null (audio with no window); on first video playback null→gpu (safe);
    //   afterwards switching back to audio does not change vo back to null — audio streams have no
    //   video track, and with force-window=no even vo=gpu won't open a window, avoiding the crash
    //   of "a windowed gpu VO being torn down at runtime".
    bool vo_gpu_ = false;
    bool restart_info_logged_ =
        false; // F28: one-shot per-track log guard, fired on PLAYBACK_RESTART (codec + hwdec)
    std::atomic<int64_t> last_loadfile_ms_{
        0}; // Y24.27: atomic (was steady_clock::time_point — data race between UI + mpv threads)
    std::atomic<bool> logging_load_{
        false}; // Y24.17: load window — log mpv INFO messages (AO/demuxer/cache) during BUFFERING
    EndFileCallback end_file_callback_; //Playback end callback
    // Resume playback support
    // pending_resume_url_: URL to resume from (set on play, cleared after FILE_LOADED)
    // pending_resume_pos_: playback position to resume from (seconds)
    std::string pending_resume_url_;
    double pending_resume_pos_ = 0.0;
    std::mutex cb_mtx_; // Protects end_file_callback_ and pending_resume_*

    // END_FILE fallback state (event-loop thread reads; play_* threads write).
    //   last_load_url_: the URL handed to the most recent loadfile (for the -15 VO fallback).
    //   vo_fallback_done_: true after a -15 VO_INIT_FAILED fallback to audio-only; prevents repeat fallback.
    //   (Y05: the former retried_ / -13 `yt-dlp -g` retry was removed — YouTube URLs are pre-resolved
    //    by resolve_youtube_url before reaching mpv, so mpv never gets a watch URL.)
    std::string last_load_url_;
    bool vo_fallback_done_ = false;

    // Y11: unified playback-state refresh timer. update_state() returns early until
    //   state_refresh_ms (INI [display] state_refresh_ms, default 100) elapses, so codec/bitrate/
    //   network/position are all polled at one configurable cadence (keeps last state_ between ticks).
    std::chrono::steady_clock::time_point last_state_refresh_{};

    // Y24.55: IPTV context diagnostics. All accessed only on the event-loop thread (LOG_MESSAGE,
    //   FILE_LOADED, END_FILE, update_state all run there), so no mutex needed — except
    //   iptv_context_ which is set from the app thread (atomic).
    std::atomic<bool> iptv_context_{false}; // app sets true when playing an IPTV channel
    std::string last_log_text_; // most recent mpv warn/error log line (for -13 sub-classification)
    std::chrono::steady_clock::time_point
        file_loaded_time_{}; // when FILE_LOADED fired (off-air/audio-only timing)
    bool file_loaded_time_set_ = false;
    std::chrono::steady_clock::time_point stuck_since_{}; // when the off-air/slow condition began
    bool stuck_timing_ = false;        // true while the off-air condition is continuously held
    bool offair_reported_ = false;     // one-shot: #5 already emitted for this track
    bool audio_only_reported_ = false; // one-shot: #7 already emitted for this track
    bool slow_reported_ = false;       // one-shot: #11 already emitted for this track
    bool had_playback_started_ =
        false; // Y24.55: true once PLAYBACK_RESTART fired → distinguishes #12 (mid-playback drop) from #1-10 (load/init failure)
    std::string classify_iptv_load_error_() const; // -13 → #1/#2/#3/#4 by last_log_text_ keywords
    std::string iptv_message_for_error_(int error) const; // END_FILE r=4 → IPTV: context message
    void reset_iptv_detection_(); // re-arm per-track one-shots/timing at each load entry point
    // D46: runtime IPTV diagnostics — off-air(#5)/audio-only(#7)/slow(#11) detection, extracted
    //   from update_state() into mpv_iptv.cpp. Inputs are the property-read snapshot (derived in
    //   update_state); mutates the per-track one-shot flags above. Runs on the event-loop thread.
    void detect_iptv_states_(bool has_media_now, bool has_audio, bool has_video_track,
                             bool idle, int64_t cache_speed, int64_t buf_pct, double buf_dur);
    // D47: apply all [mpv]-section IniConfig options (vo/vid/ao/ytdl-format/keep-open/subtitle/
    //   slang/audio-display/proxy/tls/cache/user-agent) to ctx_ before mpv_initialize, with CLI
    //   overrides taking precedence. Impl in mpv_init.cpp; extracted verbatim from initialize().
    void apply_mpv_options_();

    void event_loop();
    void update_state();
    // D36: Extract Method — log codec/bitrate/hwdec once per track (PLAYBACK_RESTART, decoder ready).
    void log_track_codec_info_();
    // D41: Extract Method — END_FILE event handler (reason dispatch + VO-fallback + IPTV msg + callback).
    void handle_end_file_(mpv_event_end_file *ef);
    // D41: Extract Method — END_FILE reason=4 error path (human msg + VO-fallback + IPTV msg).
    void handle_playback_error_(int error_code);
};

} // namespace panicast
