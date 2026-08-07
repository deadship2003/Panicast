// PlaybackService — the Application Service (功能抽象层) for playback. Owns:
//   • playback ACTION handling (UI→Core via the message bus): play/pause, volume (D8a).
//   • the implicit "playlist" QUEUE STATE + pure queue logic (D8b-1): current_playlist,
//     current_index, shuffle_queue_ and the playlist_mutex_ that guards them, plus
//     clear_playlist / refill_shuffle_queue / random_peer_index.
// The mpv command worker (in MPVController) executes player calls off the UI thread.
// Playback RUNTIME handles still in App for now (playback_node / playback_pending_ /
// playback_mode_ / play_mode) move in a later increment once events replace direct reads.
// (D8 — UI-decoupling M1.)
#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include "panicast/core/types.h" // PlaylistItem, PlayMode, TreeNodePtr
#include "panicast/playback/mpv_controller.h"

namespace panicast
{

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
};

} // namespace panicast
