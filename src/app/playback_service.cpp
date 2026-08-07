#include "panicast/app/playback_service.h"

#include <random>

#include "panicast/app/actions.h"
#include "panicast/core/constants.h"
#include "panicast/core/event_bus.h"
#include "panicast/core/event_log.h"

namespace panicast
{

// ── Action handling (D8a) ─────────────────────────────────────────────────────
void PlaybackService::init() {
    subs_.push_back(EventBus::instance().subscribe<PlayPauseAction>(
        [this](const PlayPauseAction &) { on_play_pause(); }));
    subs_.push_back(EventBus::instance().subscribe<VolumeUpAction>(
        [this](const VolumeUpAction &) { on_volume_up(); }));
    subs_.push_back(EventBus::instance().subscribe<VolumeDownAction>(
        [this](const VolumeDownAction &) { on_volume_down(); }));
}

void PlaybackService::on_play_pause() {
    player_.toggle_pause();
}

void PlaybackService::on_volume_up() {
    player_.set_volume(player_.get_state().volume + VOLUME_STEP);
}

void PlaybackService::on_volume_down() {
    player_.set_volume(player_.get_state().volume - VOLUME_STEP);
}

void PlaybackService::shutdown() {
    for (std::size_t t : subs_)
        EventBus::instance().unsubscribe(t);
    subs_.clear();
}

// ── Queue logic (D8b-1, moved from app_playback.cpp) ──────────────────────────

// N04-fix: clear the implicit peer playlist while keeping the current track playing.
//   The mpv handle is untouched (the current track keeps playing); on_playback_ended()
//   sees an empty current_playlist and returns without advancing, so auto-advance stops.
void PlaybackService::clear_playlist() {
    std::lock_guard<std::mutex> pl_lock(playlist_mutex_);
    current_playlist_.clear();
    shuffle_queue_.clear();
    current_index_ = -1;
    EVENT_LOG("Playlist cleared (keep playing)");
}

// Keep shuffle_queue_ at 3 entries (pre-generated upcoming random indices).
// Called under playlist_mutex_ (NOT self-locking — original contract preserved).
void PlaybackService::refill_shuffle_queue() {
    while (shuffle_queue_.size() < 3) {
        int last = shuffle_queue_.empty() ? current_index_ : shuffle_queue_.back();
        int idx = random_peer_index(last);
        if (idx < 0)
            break;
        shuffle_queue_.push_back(idx);
    }
}

// Pick one random index in [0, size) avoiding `avoid` when possible.
int PlaybackService::random_peer_index(int avoid) const {
    int size = static_cast<int>(current_playlist_.size());
    if (size <= 0)
        return -1;
    if (size == 1)
        return 0;
    static thread_local std::mt19937 gen(std::random_device{}());

    std::uniform_int_distribution<int> dist(0, size - 1);
    int idx = dist(gen);
    if (idx == avoid)
        idx = (idx + 1) % size;
    return idx;
}

} // namespace panicast
