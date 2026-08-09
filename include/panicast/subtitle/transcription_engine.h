// Y24.19/20: TranscriptionEngine — whisper.cpp speech-to-text.
//   Offline (Y24.19): queue local-file nodes → ffmpeg + whisper-cli → <file>.srt sidecar.
//   Real-time (Y24.20): playing + no subtitle → L → ffmpeg (→wav) + whisper-cli (progressive stdout)
//   → segments fed to SubtitleManager → LYRIC shows them; on completion save SRT.
//   Dynamic concurrency for offline (≤ max_concurrent, getloadavg CPU-aware). No shell (posix_spawnp).
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "panicast/core/types.h"               // TreeNodePtr
#include "panicast/subtitle/subtitle_parser.h" // TranscriptSegment

namespace panicast
{

class SubtitleManager;
class ThreadPool;
class IFrontend;
class MPVController; // Y24.28: for video ASR

class TranscriptionEngine {
public:
    void init(SubtitleManager *sm, ThreadPool *pool, MPVController *mpv = nullptr) {
        sm_ = sm;
        pool_ = pool;
        mpv_ = mpv;
    }

    // Offline (Y24.19): queue local-file nodes for transcription → <file>.srt sidecar.
    void enqueue_offline(const std::vector<TreeNodePtr> &nodes);
    void stop_offline(); // Y24.21: stop the offline batch (kill current + clear queue)

    // Real-time (Y24.20): transcribe the playing track progressively → LYRIC; save SRT on done.
    //   url = the playing media URL/path; is_streaming = true if not a local file (SRT → data_dir).
    void start_realtime(TreeNodePtr node, const std::string &url, bool is_streaming,
                        bool is_video = false);
    void stop_realtime(); // kill + invalidate the running realtime job
    bool realtime_running() const {
        return realtime_gen_.load() > 0 && realtime_active_.load();
    }

    bool busy() const {
        return active_.load() > 0 || !queue_empty();
    }
    int queue_remaining() const;
    int active_count() const {
        return active_.load();
    }
    void poll(IFrontend &ui);
    void shutdown();

private:
    SubtitleManager *sm_ = nullptr;
    ThreadPool *pool_ = nullptr;
    MPVController *mpv_ = nullptr; // Y24.28: for video ASR (sub_add + OSD)
    mutable std::mutex mtx_;
    std::deque<TreeNodePtr> queue_;
    std::atomic<int> active_{0};
    std::atomic<int> done_{0};
    std::atomic<int> total_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> stop_offline_{
        false}; // Y24.21: stop the offline batch (kill whisper-cli + clear queue)
    std::thread dispatcher_;
    std::atomic<bool> dispatcher_started_{false}; // Y24.27: atomic (was plain bool — race)
    std::vector<std::thread> workers_;            // Y24.27: track offline workers (was detached)
    std::thread realtime_thread_;                 // Y24.27: track realtime worker (was detached)

    // Real-time state.
    std::atomic<unsigned> realtime_gen_{0}; // bumped on stop/track-change to invalidate workers
    std::atomic<bool> realtime_active_{false};

    bool queue_empty() const;
    void start_dispatcher();
    void dispatcher_loop();
    void transcribe_one(TreeNodePtr node); // offline: ffmpeg + whisper-cli → <file>.srt
    void realtime_worker(TreeNodePtr node, std::string url, bool is_streaming, bool is_video,
                         unsigned gen);

    // Y24.20 path resolution (BTW feedback): bare→PATH/which, abs→fs::exists, ~→$HOME.
    static std::string resolve_whisper_bin();
    static std::string resolve_model();
    // Save segments as SRT; local → <file>.srt next to media; streaming → <data_dir>/transcripts/<hash>.srt.
    static void save_srt(const std::vector<TranscriptSegment> &segs, TreeNodePtr node,
                         const std::string &url, bool is_streaming);
};

} // namespace panicast
