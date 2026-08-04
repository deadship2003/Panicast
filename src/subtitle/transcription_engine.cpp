// Y24.19/20: TranscriptionEngine — whisper.cpp offline + real-time transcription.
#include "podradio/subtitle/transcription_engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <thread>

#include <fmt/format.h>

#include "podradio/config/ini_config.h"
#include "podradio/core/event_log.h"
#include "podradio/core/logger.h"
#include "podradio/core/paths.h"
#include "podradio/core/utils.h"
#include "podradio/core/thread_pool.h"
#include "podradio/playback/mpv_controller.h"  // Y24.28: for video ASR (sub_add + show_osd)
#include "podradio/storage/database.h"
#include "podradio/subtitle/subtitle_manager.h"

namespace podradio
{
namespace fs = std::filesystem;

namespace {
std::string temp_basename() {
    static std::atomic<unsigned> n{0};
    return "/tmp/podradio_wh_" + std::to_string(++n);
}
std::string expand_home(const std::string& p) {
    if (!p.empty() && p[0] == '~') {
        const char* h = std::getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}
bool cpu_has_room(int active, int max_concurrent) {
    if (active >= max_concurrent) return false;
    double load[1] = {0.0};
    int n = getloadavg(load, 1);
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    double threshold = (cores > 1) ? (static_cast<double>(cores) - 1.0) : 1.0;
    if (n < 1) return true;
    return load[0] < threshold;
}
// Parse a whisper-cli stdout line "[HH:MM:SS.mmm --> HH:MM:SS.mmm]  text" into a segment.
bool parse_whisper_line(const std::string& line, TranscriptSegment& seg) {
    static const std::regex re(
        R"(\[(\d+):(\d+):(\d+\.\d+)\s*-->\s*(\d+):(\d+):(\d+\.\d+)\]\s*(.*))");
    std::smatch m;
    if (!std::regex_match(line, m, re)) return false;
    auto to_sec = [](const std::string& h, const std::string& mm, const std::string& ss) {
        return std::stod(h) * 3600.0 + std::stod(mm) * 60.0 + std::stod(ss);
    };
    seg.start = to_sec(m[1].str(), m[2].str(), m[3].str());
    seg.end = to_sec(m[4].str(), m[5].str(), m[6].str());
    seg.text = m[7].str();
    return true;
}
// djb2 string hash → hex (for streaming sidecar filenames; no Math/random).
std::string url_hash(const std::string& s) {
    unsigned long h = 5381;
    for (char c : s) h = ((h << 5) + h) + static_cast<unsigned char>(c);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lx", h);
    return buf;
}
// Format seconds → SRT timestamp HH:MM:SS,mmm.
std::string srt_ts(double t) {
    if (t < 0) t = 0;
    int ms = static_cast<int>(t * 1000) % 1000;
    int s = static_cast<int>(t);
    int h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d,%03d", h, m, sec, ms);
    return buf;
}
// Write segments to an SRT file.
void write_srt_file(const std::string& path, const std::vector<TranscriptSegment>& segs) {
    std::ofstream f(path);
    if (!f) return;
    for (size_t i = 0; i < segs.size(); ++i) {
        f << (i + 1) << "\n" << srt_ts(segs[i].start) << " --> " << srt_ts(segs[i].end)
          << "\n" << segs[i].text << "\n\n";
    }
}
// Parse an existing SRT file into segments (via the subtitle parser registry).
std::vector<TranscriptSegment> parse_srt_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return SubtitleParserRegistry::instance().parse(content, "srt");
}
// Get audio duration in seconds (ffmpeg -i → parse stderr "Duration: HH:MM:SS.xx").
double get_audio_duration(const std::string& file) {
    auto r = Utils::run_process("ffmpeg", {"-i", file});
    // ffmpeg without output exits 1; stderr has "Duration: 00:30:15.23,"
    size_t pos = r.stderr_out.find("Duration: ");
    if (pos == std::string::npos) return -1.0;
    pos += 10;  // skip "Duration: "
    // Parse HH:MM:SS.xx
    int h = 0, m = 0; double s = 0.0;
    if (std::sscanf(r.stderr_out.c_str() + pos, "%d:%d:%lf", &h, &m, &s) < 3) return -1.0;

    return h * 3600.0 + m * 60.0 + s;
}

// Y24.22: compute the SRT sidecar path for a given url (local: <file>.srt; streaming: <data_dir>/transcripts/<hash>.srt).
std::string compute_srt_path(const std::string& url, bool is_streaming, TreeNodePtr node) {
    if (!is_streaming && node) {
        std::string file = node->local_file.empty() ? node->url : node->local_file;
        std::string base = file;
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base = base.substr(0, dot);
        return base + ".srt";
    }
    return Paths::get_data_dir() + "/transcripts/" + url_hash(url) + ".srt";
}

// Y24.22/23: persist the ASR subtitle marker (has_asr_srt + asr_srt_path) to episode_cache.
void persist_subtitle_marker(TreeNodePtr node) {
    if (!node) return;
    auto parent = node->parent.lock();
    if (!parent || parent->type != NodeType::PODCAST_FEED) return;
    // Y24.23: ASR SRT -> update has_asr_srt + asr_srt_path (NOT has_subtitle, which is for online 📜).
    node->has_asr_srt = true;
    node->asr_srt_path = node->subtitle_url;  // subtitle_url was set by probe_sidecar to the SRT path
    DatabaseManager::instance().update_episode_asr(parent->url, node->url, node->asr_srt_path);
    LOG(fmt::format("[Transcribe] persisted ASR marker: feed={} ep={} srt={}",
                    parent->url.substr(0, 40), node->url.substr(0, 40), node->asr_srt_path));
}
} // namespace

// ── Path resolution (BTW feedback) ──
std::string TranscriptionEngine::resolve_whisper_bin() {
    std::string v = IniConfig::instance().get("transcription", "whisper_bin", "whisper-cli");
    v = expand_home(v);
    if (v.empty()) return "";
    if (v[0] == '/') return fs::exists(v) ? v : "";            // absolute → fs::exists
    return Utils::which_binary(v);                              // bare → PATH search
}
std::string TranscriptionEngine::resolve_model() {
    std::string v = IniConfig::instance().get("transcription", "model",
                                              "~/.local/share/podradio/models/ggml-small.en-q5_1.bin");
    v = expand_home(v);
    if (v.empty()) return "";
    if (v[0] == '/') return fs::exists(v) ? v : "";            // absolute → fs::exists
    if (v.find('/') != std::string::npos) return fs::exists(v) ? v : "";  // relative path
    // bare filename → <data_dir>/models/<file>
    std::string p = Paths::get_data_dir() + "/models/" + v;
    return fs::exists(p) ? p : "";
}

// ── Offline (Y24.19) ──
bool TranscriptionEngine::queue_empty() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.empty();
}
int TranscriptionEngine::queue_remaining() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return static_cast<int>(queue_.size());
}

void TranscriptionEngine::enqueue_offline(const std::vector<TreeNodePtr>& nodes) {
    stop_offline_ = false;  // Y24.21: reset stop flag for a new batch
    int added = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& n : nodes) {
            if (!n) continue;
            std::string file = n->local_file.empty() ? n->url : n->local_file;
            if (file.empty()) continue;
            queue_.push_back(n);
            ++added; ++total_;
        }
    }
    if (added > 0) {
        EVENT_LOG(fmt::format("Transcribe: queued {} file(s) (whisper.cpp offline)", added));
        start_dispatcher();
    } else {
        EVENT_LOG("Transcribe: no playable local files selected");
    }
}

void TranscriptionEngine::stop_offline() {
    stop_offline_ = true;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
    }
    EVENT_LOG("Transcribe: offline stopped (partial SRT saved for resume)");
}

void TranscriptionEngine::start_dispatcher() {
    if (dispatcher_started_) return;
    dispatcher_started_ = true;
    stop_ = false;
    if (dispatcher_.joinable()) dispatcher_.join();
    dispatcher_ = std::thread([this]() { dispatcher_loop(); });
}

void TranscriptionEngine::dispatcher_loop() {
    int max_concurrent = IniConfig::instance().get_int("transcription", "max_concurrent", 3);
    if (max_concurrent < 1) max_concurrent = 1;
    while (!stop_ && !stop_offline_.load()) {
        if (!cpu_has_room(active_.load(), max_concurrent)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        TreeNodePtr node;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.empty()) break;
            node = queue_.front();
            queue_.pop_front();
        }
        active_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            workers_.emplace_back([this, node]() {
                transcribe_one(node);
                active_.fetch_sub(1);
                done_.fetch_add(1);
            });
        }
    }
    dispatcher_started_ = false;
    LOG(fmt::format("[Transcribe] dispatcher idle: {}/{} done", done_.load(), total_.load()));
}

void TranscriptionEngine::transcribe_one(TreeNodePtr node) {
    std::string file = node->local_file.empty() ? node->url : node->local_file;
    if (node->local_file.empty()) node->local_file = file;
    std::string whisper_bin = resolve_whisper_bin();
    std::string model = resolve_model();
    if (whisper_bin.empty()) { EVENT_LOG("Transcribe: whisper-cli not found — install whisper-cpp"); return; }
    if (model.empty()) { EVENT_LOG("Transcribe: model not found — set [transcription] model"); return; }

    // Y24.20: skip/resume check \u2014 don\u2019t waste CPU re-transcribing files that already have subtitles.
    std::string base = file;
    size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    std::string srt_dst = base + ".srt";

    std::vector<TranscriptSegment> existing_segs;
    int offset_ms = 0;  // 0 = full transcribe; >0 = resume from this offset
    if (fs::exists(srt_dst)) {
        existing_segs = parse_srt_file(srt_dst);
        if (!existing_segs.empty()) {
            double last_end = existing_segs.back().end;
            double duration = get_audio_duration(file);
            if (duration > 0 && last_end >= duration - 5.0) {
                EVENT_LOG(fmt::format("Transcribe skip: \u2018{}\u2019 (already has {} segs, {:.0f}s/{:.0f}s)",
                                      node->title, existing_segs.size(), last_end, duration));
                LOG(fmt::format("[Transcribe] skip (complete): {} ({} segs, {:.1}s/{:.1}s)",
                                srt_dst, existing_segs.size(), last_end, duration));
                SubtitleManager::probe_sidecar(node);
                persist_subtitle_marker(node);
                return;
            }
            offset_ms = static_cast<int>(last_end * 1000);
            EVENT_LOG(fmt::format("Transcribe resume: \u2018{}\u2019 ({} segs to {:.0f}s, resuming)",
                                  node->title, existing_segs.size(), last_end));
            LOG(fmt::format("[Transcribe] resume from {:.1}s (offset_ms={}): {}", last_end, offset_ms, srt_dst));
        }
    }

    std::string tmp_base = temp_basename();
    std::string tmp_wav = tmp_base + ".wav";
    std::string out_base = tmp_base + "_out";
    LOG(fmt::format("[Transcribe] offline start: \u2018{}\u2019 (model={}, offset_ms={})", node->title, model, offset_ms));
    auto r1 = Utils::run_process("ffmpeg", {"-y", "-i", file, "-ar", "16000", "-ac", "1", "-f", "wav", tmp_wav});
    if (!r1.launched || r1.exit_code != 0) {
        LOG(fmt::format("[Transcribe] ffmpeg failed (exit={}): {}", r1.exit_code, r1.stderr_out.substr(0, 200)));
        fs::remove(tmp_wav); return;
    }
    unsigned threads = std::thread::hardware_concurrency(); if (threads == 0) threads = 4;
    // Y24.21: streaming whisper-cli (stop_pred enables L-toggle stop; segments captured from stdout).
    std::vector<std::string> wargs = {"-m", model, "-f", tmp_wav, "-t", std::to_string(threads)};
    if (offset_ms > 0) { wargs.push_back("-ot"); wargs.push_back(std::to_string(offset_ms)); }
    std::vector<TranscriptSegment> new_segs;
    int rc = Utils::run_process_streaming(whisper_bin, wargs,
        [&new_segs](const std::string& line) {
            TranscriptSegment seg;
            if (parse_whisper_line(line, seg)) new_segs.push_back(seg);
        },
        [this]() { return stop_offline_.load(); }  // stop_pred: L-toggle kills whisper-cli
    );
    fs::remove(tmp_wav);
    bool stopped = stop_offline_.load();
    if (rc != 0 && !stopped && new_segs.empty()) {
        LOG(fmt::format("[Transcribe] whisper-cli failed (exit={})", rc));
        return;
    }
    // Combine existing + new, save SRT (full or partial for resume).
    if (!new_segs.empty()) existing_segs.insert(existing_segs.end(), new_segs.begin(), new_segs.end());
    if (!existing_segs.empty()) write_srt_file(srt_dst, existing_segs);
    if (stopped) {
        EVENT_LOG(fmt::format("Transcribe stopped: {} ({} segs saved, resume with L)",
                              node->title, existing_segs.size()));
        LOG(fmt::format("[Transcribe] stopped (partial): {} ({} segs)", srt_dst, existing_segs.size()));
    } else {
        EVENT_LOG(fmt::format("Transcribe done: {} -> {} ({} segs)", node->title, srt_dst, existing_segs.size()));
    }
    SubtitleManager::probe_sidecar(node);
    persist_subtitle_marker(node);
}

// ── Real-time (Y24.20) ──
void TranscriptionEngine::start_realtime(TreeNodePtr node, const std::string& url, bool is_streaming, bool is_video) {
    unsigned gen = ++realtime_gen_;
    realtime_active_ = true;
    EVENT_LOG(fmt::format("Transcribe: real-time start for '{}' (video={})", node ? node->title : url, is_video));
    if (realtime_thread_.joinable()) realtime_thread_.join();
    realtime_thread_ = std::thread([this, node, url, is_streaming, is_video, gen]() { realtime_worker(node, url, is_streaming, is_video, gen); });
}

void TranscriptionEngine::stop_realtime() {
    if (!realtime_active_.load()) return;
    ++realtime_gen_;  // invalidate the running worker (stop_pred → kill whisper-cli)
    realtime_active_ = false;
    EVENT_LOG("Transcribe: real-time stopped");
}

void TranscriptionEngine::realtime_worker(TreeNodePtr node, std::string url, bool is_streaming, bool is_video, unsigned gen) {
    std::string whisper_bin = resolve_whisper_bin();
    std::string model = resolve_model();
    if (whisper_bin.empty()) { EVENT_LOG("Transcribe: whisper-cli not found — install whisper-cpp"); realtime_active_ = false; return; }
    if (model.empty()) { EVENT_LOG("Transcribe: model not found — set [transcription] model"); realtime_active_ = false; return; }

    // Y24.22: skip/resume check (same logic as offline transcribe_one).
    std::string srt_path = compute_srt_path(url, is_streaming, node);
    std::vector<TranscriptSegment> existing_segs;
    int offset_ms = 0;
    if (fs::exists(srt_path)) {
        existing_segs = parse_srt_file(srt_path);
        if (!existing_segs.empty()) {
            double last_end = existing_segs.back().end;
            double duration = get_audio_duration(url);
            if (duration > 0 && last_end >= duration - 5.0) {
                // Complete — just load, don't transcribe.
                EVENT_LOG(fmt::format("Transcribe skip (realtime): already has {} segs ({:.0f}s/{:.0f}s)",
                                      existing_segs.size(), last_end, duration));
                if (sm_) sm_->set_pending(existing_segs, url);
                if (node) { node->has_asr_srt = true; node->subtitle_url = srt_path; node->subtitle_type = "srt"; node->asr_srt_path = srt_path; }
                persist_subtitle_marker(node);
                realtime_active_ = false;
                return;
            }
            offset_ms = static_cast<int>(last_end * 1000);
            EVENT_LOG(fmt::format("Transcribe resume (realtime): {} segs to {:.0f}s, resuming",
                                  existing_segs.size(), last_end));
            // Load existing segments immediately so LYRIC shows them while new ones transcribe.
            if (sm_) sm_->set_pending(existing_segs, url);
        }
    }

    std::string tmp_wav = temp_basename() + ".wav";
    LOG(fmt::format("[Transcribe] realtime: decode '{}' → wav", url));
    auto r1 = Utils::run_process("ffmpeg", {"-y", "-i", url, "-ar", "16000", "-ac", "1", "-f", "wav", tmp_wav});
    if (realtime_gen_.load() != gen) { fs::remove(tmp_wav); return; }  // stopped during decode
    if (!r1.launched || r1.exit_code != 0) {
        LOG(fmt::format("[Transcribe] realtime ffmpeg failed (exit={}): {}", r1.exit_code, r1.stderr_out.substr(0, 200)));
        EVENT_LOG("Transcribe: real-time failed (ffmpeg decode)");
        fs::remove(tmp_wav); realtime_active_ = false; return;
    }

    unsigned threads = std::thread::hardware_concurrency(); if (threads == 0) threads = 4;
    std::vector<std::string> wargs = {"-m", model, "-f", tmp_wav, "-t", std::to_string(threads)};
    if (offset_ms > 0) { wargs.push_back("-ot"); wargs.push_back(std::to_string(offset_ms)); }
    // Start with existing segments (from partial SRT) + append new ones.
    std::vector<TranscriptSegment> segs = existing_segs;  // Y24.22: pre-fill with existing partial
    LOG(fmt::format("[Transcribe] realtime: whisper-cli streaming (model={}, offset_ms={})", model, offset_ms));
    int rc = Utils::run_process_streaming(
        whisper_bin, wargs,
        [this, &segs, &url, gen, is_video](const std::string& line) {
            if (realtime_gen_.load() != gen) return;
            TranscriptSegment seg;
            if (parse_whisper_line(line, seg)) {
                segs.push_back(seg);
                if (!is_video && sm_) sm_->set_pending(segs, url);  // audio: LYRIC feed
                if (is_video && mpv_ && (segs.size() % 5 == 0))
                    mpv_->show_osd(fmt::format("Transcribing... {} segments", segs.size()), 1500);
            }
        },
        [this, gen]() { return realtime_gen_.load() != gen; }  // stop_pred → kill on stop
    );
    fs::remove(tmp_wav);

    if (realtime_gen_.load() != gen) { realtime_active_ = false; return; }  // stopped — don't save
    realtime_active_ = false;
    if (rc != 0) {
        LOG(fmt::format("[Transcribe] realtime whisper-cli exit={}", rc));
        if (segs.empty()) { EVENT_LOG("Transcribe: real-time failed (whisper-cli)"); return; }
    }
    if (segs.empty()) { EVENT_LOG("Transcribe: real-time produced no segments"); return; }
    save_srt(segs, node, url, is_streaming);
    if (node && !is_streaming) SubtitleManager::probe_sidecar(node);
    // Y24.28: video → load the SRT into mpv (renders in video window, bottom center).
    if (is_video && mpv_) {
        std::string srt_path = compute_srt_path(url, is_streaming, node);
        mpv_->sub_add(srt_path);
        mpv_->show_osd(fmt::format("Transcription complete: {} segments", segs.size()), 3000);
    }
    persist_subtitle_marker(node);
    EVENT_LOG(fmt::format("Transcribe done (real-time): {} segments → saved", segs.size()));
}

void TranscriptionEngine::save_srt(const std::vector<TranscriptSegment>& segs, TreeNodePtr node,
                                   const std::string& url, bool is_streaming) {
    std::string dst = compute_srt_path(url, is_streaming, node);
    if (is_streaming) { std::error_code ec; fs::create_directories(Paths::get_data_dir() + "/transcripts", ec); }
    std::ofstream f(dst);
    if (!f) { LOG(fmt::format("[Transcribe] save_srt: cannot write {}", dst)); return; }
    for (size_t i = 0; i < segs.size(); ++i) {
        f << (i + 1) << "\n" << srt_ts(segs[i].start) << " --> " << srt_ts(segs[i].end) << "\n"
          << segs[i].text << "\n\n";
    }
    LOG(fmt::format("[Transcribe] SRT saved: {} ({} segments)", dst, segs.size()));
    if (node && is_streaming) {
        // Streaming: record the sidecar path on the node so probe/replay can find it.
        node->subtitle_url = dst;
        node->has_asr_srt = true;
        node->asr_srt_path = dst;
        node->subtitle_type = "srt";
    }
}

void TranscriptionEngine::poll(UI& /*ui*/) { /* progress via EVENT_LOG; reserved for a progress bar */ }

void TranscriptionEngine::shutdown() {
    stop_ = true;
    stop_offline_ = true;
    ++realtime_gen_;  // invalidate any running realtime job
    realtime_active_ = false;
    if (realtime_thread_.joinable()) realtime_thread_.join();
    if (dispatcher_.joinable()) dispatcher_.join();
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& w : workers_) if (w.joinable()) w.join();
    workers_.clear();
}

} // namespace podradio
