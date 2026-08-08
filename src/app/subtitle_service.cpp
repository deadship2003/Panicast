#include "panicast/app/subtitle_service.h"

#include <cctype>
#include <string>

#include <fmt/format.h>

#include "panicast/app/playback_events.h" // D10-3 Step 2: PlaybackTrackChanged → begin_track
#include "panicast/core/event_bus.h"      // D10-3 Step 2: subscribe the reactor channel
#include "panicast/core/logger.h"
#include "panicast/core/thread_pool.h"
#include "panicast/playback/mpv_controller.h"
#include "panicast/ui/ui.h"

namespace panicast
{

namespace {
// Y24.17 helpers shared by begin_track's subtitle handling (sync reset + async pool probe path).
//   Relocated verbatim from playback_service.cpp (D10-3 Step 1) — behaviour-equivalent.
bool is_mpv_sub_url(const std::string &url) {
    if (url.empty())
        return false;
    auto ew = [](const std::string &s, const char *suf) {
        size_t n = 0;
        while (suf[n])
            ++n;
        if (s.size() < n)
            return false;
        for (size_t i = 0; i < n; ++i)
            if ((char)std::tolower((unsigned char)s[s.size() - n + i]) !=
                (char)std::tolower((unsigned char)suf[i]))
                return false;
        return true;
    };
    return ew(url, ".vtt") || ew(url, ".srt") || ew(url, ".ass") || ew(url, ".ssa");
}
std::string basename_of(const std::string &p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}
} // namespace

void SubtitleService::init(ThreadPool &pool, MPVController &mpv) {
    // Y24.28: pass mpv for video ASR. Y24.19: whisper.cpp transcription. The engine is wired to
    //   this service's own SubtitleManager — the inter-object dependency stays internal.
    transcription_engine_.init(&subtitle_mgr_, &pool, &mpv);
    pool_ = &pool;
    mpv_ = &mpv;
    // D10-3 Step 2: subscribe to track changes → auto-load the new track's subtitle (the reactor
    //   pattern — the FIRST real consumer of the D9 PlaybackTrackChanged channel). PlaybackService
    //   publishes {node, mode, has_video} on BOTH manual play and auto-advance; begin_track
    //   re-identifies the media type from has_video and runs the A/B branch (Option B: auto-advance
    //   now follows the same path as a manual play). EventBus dispatch is synchronous on the
    //   publisher's (UI) thread, so this matches the old imperative call's threading + ordering.
    subs_.push_back(EventBus::instance().subscribe<PlaybackTrackChanged>(
        [this](const PlaybackTrackChanged &e) { begin_track(e.node, e.has_video); }));
}

void SubtitleService::shutdown() {
    // D10-3 Step 2: drop the EventBus subscription before the engines tear down, so the bus never
    //   invokes begin_track on a half-destroyed service.
    for (std::size_t t : subs_)
        EventBus::instance().unsubscribe(t);
    subs_.clear();
    transcription_engine_.shutdown(); // Y24.19: stop transcription dispatcher
}

void SubtitleService::poll(UI &ui, bool lyric_bar_requested) {
    // Y24.7: SubtitleManager poll — handoff pending transcript to UI + offset + logs.
    subtitle_mgr_.poll(ui, lyric_bar_requested);
}

// D10-3: the methods below are the subtitle orchestration, relocated from PlaybackService. Step 2
//   made begin_track event-driven — it is now called by the PlaybackTrackChanged subscriber (not by
//   PlaybackService directly). stop_realtime stays a direct call from PlaybackService's track-end
//   sites (the residual coupling; D11 will cut it). Same objects throughout: pool_/mpv_/subtitle_mgr_
//   are the very ones App passed to init — and the same mpv PlaybackService used via player_ (there
//   is one MPVController in the system).

void SubtitleService::stop_realtime() {
    transcription_engine_.stop_realtime(); // Y24.20: realtime transcription doesn't carry across tracks
}

void SubtitleService::begin_track(TreeNodePtr node, bool has_video) {
    // Y24.8: subtitle handling is FULLY ASYNC for audio — play() is called immediately by the caller
    //   with no synchronous fs::exists probe (slow on /mnt/e WSL2 mounts). Method A (mpv sub-file) is
    //   only used for VIDEO (mpv renders in the video window); AUDIO always uses Method B
    //   (SubtitleManager fetches+parses async, drives LYRIC via time_pos).
    // Y24.17: VIDEO sidecar probe is ASYNC too. The sub-file URL isn't known at loadfile time, so
    //   it's added via mpv sub-add after the async probe finds it. AUDIO was already async.
    if (has_video) {
        // Y24.18: reset Method B first — video uses Method A (mpv renders) or no subtitle; either
        //   way the LYRIC panel must drop the previous (audio) track's lyrics. JSON → Method B fill
        //   below. Done sync (calling thread) so the panel clears immediately.
        subtitle_mgr_.reset();
        TreeNodePtr pn_cap = node;
        pool_->submit([this, pn_cap]() {
            LOG("[Subtitle] video sidecar probe (async)...");
            subtitle_mgr_.probe_sidecar(pn_cap);
            if (pn_cap && pn_cap->has_subtitle && !pn_cap->subtitle_url.empty()) {
                if (is_mpv_sub_url(pn_cap->subtitle_url)) {
                    mpv_->sub_add(pn_cap->subtitle_url);
                    LOG(fmt::format("[Subtitle] mpv sub-add: {} (Method A — mpv renders)",
                                    basename_of(pn_cap->subtitle_url)));
                } else {
                    LOG(fmt::format(
                        "[Subtitle] panicast resolves: {} (Method B — video, non-mpv format)",
                        basename_of(pn_cap->subtitle_url)));
                    subtitle_mgr_.load_async(pn_cap, *pool_); // fills Method B (LYRIC for JSON)
                }
            } else {
                LOG("[Subtitle] video: no local sidecar found (async)");
            }
        });
    } else {
        // AUDIO: always Method B, fully async (probe+fetch+parse in pool). Never blocks play.
        //   load_async probes for a local sidecar async; if none and no transcript URL → NONE.
        subtitle_mgr_.load_async(node, *pool_);
        if (node && node->has_subtitle && !node->subtitle_url.empty())
            LOG(fmt::format("[Subtitle] panicast resolves: {} (Method B — audio, async)",
                            basename_of(node->subtitle_url)));
    }
}

} // namespace panicast
