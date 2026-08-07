#include "panicast/app/subtitle_service.h"

#include "panicast/core/thread_pool.h"
#include "panicast/playback/mpv_controller.h"
#include "panicast/ui/ui.h"

namespace panicast
{

void SubtitleService::init(ThreadPool &pool, MPVController &mpv) {
    // Y24.28: pass mpv for video ASR. Y24.19: whisper.cpp transcription. The engine is wired to
    //   this service's own SubtitleManager — the inter-object dependency stays internal.
    transcription_engine_.init(&subtitle_mgr_, &pool, &mpv);
}

void SubtitleService::shutdown() {
    transcription_engine_.shutdown(); // Y24.19: stop transcription dispatcher
}

void SubtitleService::poll(UI &ui, bool lyric_bar_requested) {
    // Y24.7: SubtitleManager poll — handoff pending transcript to UI + offset + logs.
    subtitle_mgr_.poll(ui, lyric_bar_requested);
}

} // namespace panicast
