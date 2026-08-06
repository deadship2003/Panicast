// Actions — UI → Core commands (the INPUT direction of the message bus). UI emits via
// publish_action (usually through the Keymap: key → Action → publish). App / Services subscribe
// to each action type + handle. This is the seam that lets the UI be a pure interaction layer
// (no direct Core calls) — see docs/DESIGN.md "目标架构".
// (D6 seed: PlayPause. D7: + Volume/Nav, + Keymap.)
#pragma once

#include <variant>

#include "panicast/core/event_bus.h"

namespace panicast
{

struct PlayPauseAction {};
struct VolumeUpAction {};
struct VolumeDownAction {};
struct NavUpAction {};
struct NavDownAction {};

// A key binding's target: one of the actions above. The Keymap maps a key (int) → Action.
using Action = std::variant<PlayPauseAction, VolumeUpAction, VolumeDownAction,
                            NavUpAction, NavDownAction>;

// Publish whichever action the variant holds onto the EventBus (lets handle_input be a generic
// Keymap lookup instead of a per-key switch).
inline void publish_action(const Action &a) {
    std::visit([](const auto &x) { EventBus::instance().publish(x); }, a);
}

}  // namespace panicast
