// MPV playback control layer — extracted implementation unit (D18 god-object split).
//   Thin-wrapper command/state-query methods (pause/volume/speed/loop/sub/osd/state
//   getter/handle/callback/resume setters). They remain MPVController members — they
//   touch private controller state (ctx_/state_/mtx_/cb_mtx_/enqueue_cmd_/cli_*_override_/
//   end_file_callback_/pending_resume_*); only their implementations live here.
//   Declarations stay in mpv_controller.h. Mechanical verbatim move from mpv_controller.cpp.
#include "panicast/playback/mpv_controller.h"

#include <cstring>
#include <string>

#include <fmt/format.h>

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"

namespace panicast
{

void MPVController::toggle_pause() {
    if (!ctx_)
        return;
    enqueue_cmd_([this] {
        int p = 0;
        mpv_get_property(ctx_, "pause", MPV_FORMAT_FLAG, &p);
        p = !p;
        mpv_set_property(ctx_, "pause", MPV_FORMAT_FLAG, &p);
    });
}

void MPVController::set_pause(bool paused) {
    if (!ctx_)
        return;
    int p = paused ? 1 : 0;
    { std::lock_guard<std::mutex> lock(mtx_); state_.paused = paused; } // optimistic (UI shows immediately)
    enqueue_cmd_([this, p]() mutable { mpv_set_property(ctx_, "pause", MPV_FORMAT_FLAG, &p); });
}

void MPVController::set_volume(int vol) {
    if (!ctx_)
        return;
    if (vol < 0)
        vol = 0;
    if (vol > MAX_VOLUME)
        vol = MAX_VOLUME;
    { std::lock_guard<std::mutex> lock(mtx_); state_.volume = vol; } // optimistic
    double dv = vol; // volume property is actually double, use MPV_FORMAT_DOUBLE
    enqueue_cmd_([this, dv]() mutable { mpv_set_property(ctx_, "volume", MPV_FORMAT_DOUBLE, &dv); });
}

void MPVController::adjust_speed(bool faster) {
    if (!ctx_)
        return;
    enqueue_cmd_([this, faster] {
        double s = DEFAULT_SPEED;
        mpv_get_property(ctx_, "speed", MPV_FORMAT_DOUBLE, &s);
        s = faster ? s * (1.0 + SPEED_STEP) : s / (1.0 + SPEED_STEP);
        if (s < MIN_SPEED)
            s = MIN_SPEED;
        if (s > MAX_SPEED)
            s = MAX_SPEED;
        mpv_set_property(ctx_, "speed", MPV_FORMAT_DOUBLE, &s);
    });
}

void MPVController::reset_speed() {
    if (!ctx_)
        return;
    enqueue_cmd_([this] {
        double s = DEFAULT_SPEED;
        mpv_set_property(ctx_, "speed", MPV_FORMAT_DOUBLE, &s);
    });
}

void MPVController::set_speed(double s) {
    if (!ctx_)
        return;
    if (s < MIN_SPEED)
        s = MIN_SPEED;
    if (s > MAX_SPEED)
        s = MAX_SPEED;
    { std::lock_guard<std::mutex> lock(mtx_); state_.speed = s; } // optimistic
    enqueue_cmd_([this, s]() mutable { mpv_set_property(ctx_, "speed", MPV_FORMAT_DOUBLE, &s); });
}

// Freeze-fix (2026-08-15): enqueued — these sync property sets run right after play() in the
//   play_current sequence (UI thread) and must not block on a wedged core. FIFO order in the
//   single cmd worker preserves the play→keep-open→loop→pause sequence exactly.
void MPVController::set_loop_file(bool loop) {
    if (!ctx_)
        return;
    enqueue_cmd_([this, loop] {
        int rc_loop_file = mpv_set_property_string(ctx_, "loop-file", loop ? "inf" : "no");
        if (rc_loop_file < 0)
            LOG(fmt::format("[MPV] WARNING: set property loop-file failed (rc={})", rc_loop_file));
        LOG(fmt::format("[MPV] loop-file set to: {}", loop ? "inf" : "no"));
    });
}

void MPVController::set_keep_open(bool keep) {
    if (!ctx_)
        return;
    enqueue_cmd_([this, keep] {
        mpv_set_property_string(ctx_, "keep-open", keep ? "yes" : "no");
        LOG(fmt::format("[MPV] keep-open set to: {}", keep ? "yes" : "no"));
    });
}

void MPVController::sub_add(const std::string &url) {
    if (!ctx_ || url.empty())
        return;
    // Freeze-fix (2026-08-15): via the cmd worker — direct mpv_command on the UI thread (L-key
    //   video path) blocks forever on a wedged core.
    enqueue_cmd_([this, url] {
        const char *args[] = {"sub-add", url.c_str(), "select", nullptr};
        int rc = mpv_command(ctx_, args);
        LOG(fmt::format("[MPV] sub-add '{}' (rc={})", url, rc));
    });
}

// Y24.28: show OSD text (for ASR transcription progress).
// Freeze-fix (2026-08-15): routed through the cmd worker — a direct mpv_command from the UI
//   thread (L-key path) would block forever on a wedged core, freezing the TUI.
void MPVController::show_osd(const std::string &text, int duration_ms) {
    if (!ctx_ || text.empty())
        return;
    std::string ms = std::to_string(duration_ms);
    enqueue_cmd_([this, text, ms] {
        const char *args[] = {"show-text", text.c_str(), ms.c_str(), nullptr};
        mpv_command(ctx_, args);
    });
}

// Y24.43: is the mpv video output window actually rendering? current-vo is "null"/empty for audio-only.
bool MPVController::is_video_window_open() const {
    return !state_.current_vo.empty() && state_.current_vo != "null";
}

// Y24.47: is audio-only mode configured (vo=null or vid=no, via --quiet / INI)? Playback INTENT —
//   computed from CLI overrides (precedence) + INI, before play starts. Used to pick an audio-only
//   stream format for YouTube/adaptive sources so the video stream is never fetched (saves bandwidth).
bool MPVController::is_audio_only_mode() {
    std::string vo =
        !cli_vo_override_.empty() ? cli_vo_override_ : IniConfig::instance().get_mpv_vo();
    std::string vid =
        !cli_vid_override_.empty() ? cli_vid_override_ : IniConfig::instance().get_mpv_vid();
    return vo == "null" || vid == "no";
}

// Y24.43: does the current media have an embedded subtitle track?
// Freeze-fix (2026-08-15): plain state read — the track-list scan moved to update_state() on the
//   EVENT thread (scan_sub_track_()). This used to call mpv_get_property(track-list)
//   synchronously; called from the UI thread every frame (update_remote_state_cache), a wedged
//   mpv core (dropped paused stream / reconnect black-hole / AO-init timeout) blocked the UI
//   frame forever — "paused a while → UI unresponsive".
bool MPVController::has_active_subtitle() const {
    return state_.has_sub_track; // last-known-good, same pattern as is_video_window_open()
}

// Event-thread-only: raw mpv track-list scan for a type=sub entry (was the body of
//   has_active_subtitle). Called from update_state() under the 100ms refresh gate.
bool MPVController::scan_sub_track_() const {
    if (!ctx_)
        return false;
    mpv_node node;
    if (mpv_get_property(ctx_, "track-list", MPV_FORMAT_NODE, &node) < 0)
        return false;
    bool found = false;
    if (node.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node.u.list->num; ++i) {
            mpv_node *item = &node.u.list->values[i];
            if (item->format != MPV_FORMAT_NODE_MAP)
                continue;
            for (int j = 0; j < item->u.list->num; ++j) {
                if (std::strcmp(item->u.list->keys[j], "type") == 0 &&
                    item->u.list->values[j].format == MPV_FORMAT_STRING &&
                    std::strcmp(item->u.list->values[j].u.string, "sub") == 0) {
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
    }
    mpv_free_node_contents(&node);
    return found;
}

void MPVController::set_loop_playlist(bool loop) {
    if (!ctx_)
        return;
    enqueue_cmd_([this, loop] {
        int rc_loop_playlist = mpv_set_property_string(ctx_, "loop-playlist", loop ? "inf" : "no");
        if (rc_loop_playlist < 0)
            LOG(fmt::format("[MPV] WARNING: set property loop-playlist failed (rc={})",
                            rc_loop_playlist));
        LOG(fmt::format("[MPV] loop-playlist set to: {}", loop ? "inf" : "no"));
    });
}

MPVController::State MPVController::get_state() {
    std::lock_guard<std::mutex> lock(mtx_);
    return state_;
}

mpv_handle *MPVController::get_handle() {
    return ctx_;
}

void MPVController::set_end_file_callback(EndFileCallback callback) {
    std::lock_guard<std::mutex> lock(cb_mtx_); // Mutex with event_loop calls
    end_file_callback_ = std::move(callback);
}

void MPVController::set_resume_position(const std::string &url, double position) {
    std::lock_guard<std::mutex> lock(cb_mtx_); // Mutex with event_loop reads
    pending_resume_url_ = url;
    pending_resume_pos_ = position;
}
} // namespace panicast
