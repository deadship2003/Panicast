#include "panicast/app/playback_service.h"

#include "panicast/app/actions.h"
#include "panicast/core/constants.h"
#include "panicast/core/event_bus.h"

namespace panicast
{

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

}  // namespace panicast
