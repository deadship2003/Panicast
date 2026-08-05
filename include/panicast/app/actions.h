// Actions — UI → Core commands (the INPUT direction of the message bus). UI emits these on
// EventBus; Application Services (later) subscribe + handle. This is the seam that lets the UI
// be a pure interaction layer (no direct Core calls) — see docs/DESIGN.md "目标架构".
// (D6 — UI-decoupling seed; more actions added as inputs migrate in D7.)
#pragma once

namespace panicast
{

// Toggle playback pause/resume (Space / 'p' key).
struct PlayPauseAction {};

}  // namespace panicast
