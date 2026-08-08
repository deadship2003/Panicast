#include "panicast/app/subtitle_service.h"

#include <cctype>
#include <string>

#include <fmt/format.h>

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
    // D10-3 Step 1: retain pool/mpv for the track-change orchestration (begin_track submits pool
    //   work + calls mpv sub_add). Same objects the engine was wired with.
    pool_ = &pool;
    mpv_ = &mpv;
}

void SubtitleService::shutdown() {
    transcription_engine_.shutdown(); // Y24.19: stop transcription dispatcher
}

void SubtitleService::poll(UI &ui, bool lyric_bar_requested) {
    // Y24.7: SubtitleManager poll — handoff pending transcript to UI + offset + logs.
    subtitle_mgr_.poll(ui, lyric_bar_requested);
}

// D10-3 Step 1: the three methods below relocate PlaybackService's subtitle orchestration verbatim
//   (same objects: pool_/mpv_/subtitle_mgr_ are the very ones App passed to init — and the same mpv
//   PlaybackService used via player_, since there is one MPVController). Step 2 retriggers them via
//   PlaybackTrackChanged instead of an imperative call from PlaybackService.

void SubtitleService::stop_realtime() {
    transcription_engine_.stop_realtime(); // Y24.20: realtime transcription doesn't carry across tracks
}

void SubtitleService::load_transcript(TreeNodePtr node) {
    // Y23.4: load transcript for the (auto-advanced) track — Method B only (advance path semantics).
    subtitle_mgr_.load_async(node, *pool_);
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
