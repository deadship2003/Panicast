// PlaybackService — the first Application Service (功能抽象层). Owns playback-related ACTION
// handling (UI→Core via the message bus): play/pause, volume. The mpv command worker (in
// MPVController) executes the actual calls off the UI thread. Playback STATE (current_playlist,
// auto-advance, play_mode…) moves here in later increments. (D8 — UI-decoupling M1.)
#pragma once

#include <cstddef>
#include <vector>

#include "panicast/playback/mpv_controller.h"

namespace panicast
{

class PlaybackService {
public:
    explicit PlaybackService(MPVController &player) : player_(player) {}

    // Subscribe to playback Actions on the EventBus (PlayPause / VolumeUp / VolumeDown).
    void init();
    // Unsubscribe (optional — the app exits via _exit, so this is mainly for completeness/tests).
    void shutdown();

private:
    MPVController &player_;
    std::vector<std::size_t> subs_;

    void on_play_pause();
    void on_volume_up();
    void on_volume_down();
};

}  // namespace panicast
