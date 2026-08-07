// PlaybackService — the Application Service (功能抽象层) for playback. Owns:
//   • playback ACTION handling (UI→Core via the message bus): play/pause, volume (D8a).
//   • the implicit "playlist" QUEUE STATE + pure queue logic (D8b-1): current_playlist,
//     current_index, shuffle_queue_ and the playlist_mutex_ that guards them, plus
//     clear_playlist / refill_shuffle_queue / random_peer_index.
//   • playback / autoplay LOGIC (D8b-2): play_current / on_playback_ended / record_play_history /
//     resolve_youtube_url (moved out of app_playback.cpp). pool_ / subtitle / transcription deps
//     are injected late via attach() — they are declared AFTER playback_ in App, so they can't be
//     construction-time references.
// The mpv command worker (in MPVController) executes player calls off the UI thread.
// Playback RUNTIME handles still live in App (playback_node / playback_pending_ / playback_mode_) —
// D8b-2 writes them back through a small callback seam (attach()), which D9's event layer will
// replace once the UI reads them via events instead of direct member access. play_mode is a setting
// kept in App and passed into each call. (D8 — UI-decoupling M1.)
#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "panicast/core/types.h" // AppMode, PlaylistItem, PlayMode, TreeNodePtr
#include "panicast/playback/mpv_controller.h"

namespace panicast
{

// Forward declarations — injected late via attach() (pointers only, to avoid heavy includes here).
class ThreadPool;
class SubtitleManager;
class TranscriptionEngine;

class PlaybackService {
public:
    explicit PlaybackService(MPVController &player) : player_(player) {}

    // ── Action handling (D8a) ────────────────────────────────────────────────
    // Subscribe to playback Actions on the EventBus (PlayPause / VolumeUp / VolumeDown).
    void init();
    // Unsubscribe (optional — the app exits via _exit, so this is mainly for completeness/tests).
    void shutdown();

    // ── Queue state access (D8b-1) ───────────────────────────────────────────
    // The implicit playlist = siblings (peers) of the playing episode. Guarded by
    // playlist_mutex() — callers lock it before touching playlist()/current_index()/
    // shuffle_queue(), exactly as the pre-refactor code locked playlist_mutex_.
    std::mutex &playlist_mutex() {
        return playlist_mutex_;
    }
    std::vector<PlaylistItem> &playlist() {
        return current_playlist_;
    }
    const std::vector<PlaylistItem> &playlist() const {
        return current_playlist_;
    }
    int current_index() const {
        return current_index_;
    }
    void set_current_index(int idx) {
        current_index_ = idx;
    }
    std::deque<int> &shuffle_queue() {
        return shuffle_queue_;
    }

    // N04-fix: clear the implicit peer playlist while keeping the current track playing.
    //   The mpv handle is untouched (the current track keeps playing); on_playback_ended()
    //   sees an empty playlist and returns without advancing, so auto-advance stops.
    // Locks playlist_mutex_ internally.
    void clear_playlist();
    // Keep shuffle_queue_ at 3 entries (pre-generated upcoming random indices).
    // NOT self-locking — caller must hold playlist_mutex_ (matches original contract).
    void refill_shuffle_queue();

    // ── Playback / autoplay logic (D8b-2, moved from app_playback.cpp) ────────
    // Inject the services declared after playback_ in App + the runtime-handle write callbacks.
    //   set_pending(true) stamps a fresh BUFFERING start time; set_pending(false) clears it;
    //   on_history_changed fires after a history DB write (App rebuilds the history tree async).
    // Must be called once before any playback (App::run wires it right after playback_.init()).
    void attach(ThreadPool &pool, SubtitleManager &subtitle_mgr,
                TranscriptionEngine &transcription_engine,
                std::function<void(TreeNodePtr)> set_playback_node,
                std::function<void(bool)> set_pending,
                std::function<void(AppMode)> set_playback_mode,
                std::function<void()> on_history_changed);
    // Play a single item by index. mode = active App mode (IPTV flag); play_mode = loop setting.
    void play_current(int idx, AppMode mode, PlayMode play_mode);
    // Pointer-driven auto-advance (runs on the UI thread — D4 invariant). Advances current_index_
    //   per play_mode and plays the next track inline (must NOT call play_current: playlist_mutex_
    //   is non-recursive, and this method already holds it).
    void on_playback_ended(int reason, AppMode mode, PlayMode play_mode);

private:
    MPVController &player_;
    std::vector<std::size_t> subs_;

    // ── Action handlers (D8a) ────────────────────────────────────────────────
    void on_play_pause();
    void on_volume_up();
    void on_volume_down();

    // ── Queue state (D8b-1) ──────────────────────────────────────────────────
    std::mutex playlist_mutex_;
    std::vector<PlaylistItem> current_playlist_;
    int current_index_ = -1;        // playing pointer (index into current_playlist_)
    std::deque<int> shuffle_queue_; // SHUFFLE lookahead (pre-generated upcoming indices)

    // Pick one random index in [0, size) avoiding `avoid` when possible.
    int random_peer_index(int avoid) const;

    // ── Playback-logic helpers (D8b-2) ───────────────────────────────────────
    void record_play_history(const std::string &url, const std::string &title, int duration);
    // F23: standalone YouTube `-g` resolve (pool-safe). Y09 1A: returns 1-2 stream URLs (DASH=2).
    // Stateless (uses IniConfig / yt-dlp / Paths only) → const.
    std::vector<std::string> resolve_youtube_url(const std::string &url, bool has_video) const;

    // ── Injected deps + runtime-handle write seam (D8b-2) ────────────────────
    // Null until attach(); dereferenced only after App::run has wired them.
    ThreadPool *pool_ = nullptr;
    SubtitleManager *subtitle_mgr_ = nullptr;
    TranscriptionEngine *transcription_engine_ = nullptr;
    std::function<void(TreeNodePtr)> set_playback_node_;
    std::function<void(bool)> set_pending_;        // true → BUFFERING (stamps start); false → clear
    std::function<void(AppMode)> set_playback_mode_;
    std::function<void()> on_history_changed_;
};

} // namespace panicast
