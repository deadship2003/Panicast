// SubtitleService — the Application Service (功能抽象层) for subtitles / transcription (ASR).
//   Owns the SubtitleManager (detect/probe/load/offset/status) and TranscriptionEngine
//   (whisper.cpp offline + real-time speech-to-text) that previously lived as bare App members.
//   Centralizes their LIFECYCLE wiring: init() wires the engine to its own SubtitleManager + the
//   shared pool + mpv (video ASR) — replacing App's transcription_engine_.init(&subtitle_mgr_,…)
//   so the inter-object wiring stays inside the service; shutdown()/poll() are the system-teardown
//   and per-frame handoff entry points. The remaining trigger sites (App input / remote / download
//   + PlaybackService) reach the engines through the accessors subtitle_mgr() /
//   transcription_engine() — transitional, like the D8b-1 queue-state accessors. D10-2 moves the
//   subtitle input keys (LYRIC toggle / offset / ASR) onto the message bus as SubtitleActions the
//   service subscribes to; D10-3 makes the service event-driven (subscribe PlaybackTrackChanged →
//   auto-load the new track's transcript; publish SubtitleStatusChanged for direct UI/remote subs).
//   This is the second Application Service (after PlaybackService) and the foundation of the M3
//   SubtitleController.   (D10-1 — UI-decoupling M1.)
#pragma once

#include "panicast/subtitle/subtitle_manager.h"     // Y24.7
#include "panicast/subtitle/transcription_engine.h" // Y24.19/20

namespace panicast
{

class ThreadPool;
class MPVController;
class UI;

class SubtitleService {
public:
    // Wire the transcription engine to this service's own SubtitleManager + the shared pool + mpv
    //   (Y24.28: mpv for video ASR). Replaces App's transcription_engine_.init(&subtitle_mgr_,…).
    void init(ThreadPool &pool, MPVController &mpv);

    // Teardown — stop the transcription dispatcher. Call before the pool joins (as App's dtor did).
    void shutdown();

    // Per-frame UI-thread handoff: push any pending transcript to the UI + offset + logs.
    //   Forwards SubtitleManager::poll (called once per frame from App's run loop).
    void poll(UI &ui, bool lyric_bar_requested);

    // ── Engine access (transitional — D10-2/3 replace direct use with Actions/events) ──
    //   PlaybackService.attach() and App's input/remote/download sites use these; they are the
    //   redirect seam for the D10-1 ownership move (no behaviour change).
    SubtitleManager &subtitle_mgr() {
        return subtitle_mgr_;
    }
    TranscriptionEngine &transcription_engine() {
        return transcription_engine_;
    }

private:
    SubtitleManager subtitle_mgr_;
    TranscriptionEngine transcription_engine_; // Y24.19: whisper.cpp offline (later realtime) ASR
};

} // namespace panicast
