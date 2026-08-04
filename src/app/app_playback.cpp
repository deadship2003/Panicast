#include "podradio/app/app.h"

#include "podradio/parsers/transcript_parser.h"

namespace podradio
{

// Y24.17: helpers shared by play_current's subtitle handling (sync + async pool paths).
static bool is_mpv_sub_url(const std::string& url) {
    if (url.empty()) return false;
    auto ew = [](const std::string& s, const char* suf) {
        size_t n = 0; while (suf[n]) ++n;
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if ((char)std::tolower((unsigned char)s[s.size()-n+i]) != (char)std::tolower((unsigned char)suf[i])) return false;
        return true;
    };
    return ew(url, ".vtt") || ew(url, ".srt") || ew(url, ".ass") || ew(url, ".ssa");
}
static std::string basename_of(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

    // Y24.7: thin delegates to SubtitleManager (load/probe/offset centralized there).
    void App::load_transcript(TreeNodePtr node) { subtitle_mgr_.load_async(node, pool_); }
    void App::probe_local_sidecar(TreeNodePtr node) { subtitle_mgr_.probe_sidecar(node); }

    // Check whether a node is playable
    bool App::is_playable_node(TreeNodePtr node) {
        if (!node) return false;
        // Only leaf playable types can be played — the original code returned true for any non-empty URL,
        //   handing PODCAST_FEED's feed URL to mpv as playable (which always fails)
        if (node->url.empty()) return false;
        return node->type == NodeType::RADIO_STREAM || node->type == NodeType::PODCAST_EPISODE;
    }

    // N04-fix: clear the implicit peer playlist while keeping the current track playing.
    //   The mpv handle is untouched (the current track keeps playing); on_playback_ended()
    //   sees an empty current_playlist and returns without advancing, so auto-advance stops.
    void App::clear_playlist() {
        std::lock_guard<std::mutex> pl_lock(playlist_mutex_);
        current_playlist.clear();
        shuffle_queue_.clear();
        current_index = -1;
        EVENT_LOG("Playlist cleared (keep playing)");
    }

    // Playback-ended callback (fires on the MPV event thread).
    // Pointer-driven model: a single track is loaded into mpv at a time; when it ends
    //   normally (reason=0), the app advances current_index per play_mode and plays the
    //   next track. REPEAT relies on mpv loop_file (no app action).
    //   SHUFFLE consumes the pre-generated shuffle_queue_ front and refills it.
    void App::on_playback_ended(int reason) {
        transcription_engine_.stop_realtime();  // Y24.20: stop realtime transcription on track end
        std::lock_guard<std::mutex> pl_lock(playlist_mutex_);
        if (reason != 0) {
            playback_pending_ = false;  // Y23.9: error/stop → clear pending (back to BROWSING)
            // Y24.8: human-readable reason (was raw "reason=X, ignored"). reason 0=EOF (advances),
            //   2=stop, 3=quit, 4=error, 5=redirect — none advance the queue.
            LOG(fmt::format("[AUTOPLAY] End file: {} — not advancing", MPVController::end_file_reason_str(reason)));
            return;
        }

        int size = static_cast<int>(current_playlist.size());
        if (size == 0) {
            LOG("[AUTOPLAY] Peer list empty, nothing to advance");
            return;
        }

        // REPEAT: mpv loop_file replays the same track; no app intervention.
        if (play_mode == PlayMode::REPEAT) {
            LOG("[AUTOPLAY] REPEAT: loop_file replays current track");
            return;
        }

        // Compute the next pointer
        int next = current_index;
        if (play_mode == PlayMode::CYCLE) {
            next = (current_index < 0) ? 0 : (current_index + 1) % size;
        } else {  // PlayMode::SHUFFLE — consume the lookahead queue
            if (shuffle_queue_.empty()) refill_shuffle_queue();
            if (!shuffle_queue_.empty()) {
                next = shuffle_queue_.front();
                shuffle_queue_.pop_front();
            }
            // ensure valid + not stuck on the same item when there's a choice
            if (size > 1 && next == current_index) {
                next = (next + 1) % size;
            }
            // keep 3 ahead
            refill_shuffle_queue();
        }

        current_index = next;

        // Y11 bugfix: update playback_node to the NEXT track's source node, else the INFO area
        //   keeps showing the PREVIOUS track's title/info after auto-advance (on_playback_ended
        //   plays inline without play_current, which is the only place playback_node was set).
        //   Set on this (event) thread the same way current_index is; the TreeNode is retained by
        //   the tree, so the shared_ptr reassignment is safe in practice (matches existing pattern).
        playback_node = current_playlist[next].node;
        playback_mode_ = mode;  // Y24.54: save mode for N = jump-to-playing
        // Y24.55: keep IPTV context flag in sync on auto-advance (same reasoning as play_current).
        player.set_iptv_context(mode == AppMode::IPTV);
        load_transcript(playback_node);  // Y23.4: load transcript for the advanced track (method B)
        playback_pending_ = true;  // Y23.9: BUFFERING until mpv loads the next track
        playback_pending_start_ = std::chrono::steady_clock::now();

        // Play the next track inline (must NOT call play_current — it also locks
        //   playlist_mutex_ (non-recursive) and would deadlock).
        std::string orig_url = current_playlist[next].url;
        bool has_video = current_playlist[next].is_video;

        // F23: async YouTube resolve — don't block the mpv event thread
        std::string local = CacheManager::instance().get_local_file(orig_url);
        if (!local.empty() && fs::exists(local)) {
            // Local file — play immediately
            std::string url = local;  // Y23.9: raw path (no file://) — mpv handles special chars
            player.play(url, has_video);
            player.set_keep_open(false);
            player.set_pause(false);
            player.set_loop_file(false);
        } else {
            URLType ut = URLClassifier::classify(orig_url);
            // P1-4: snapshot title/duration here (under playlist_mutex_) — the pool lambda must
            //   not read current_playlist[next] later, by which time the UI thread may have reset it.
            std::string title = current_playlist[next].title;
            int duration = current_playlist[next].duration;
            if (URLClassifier::is_youtube(ut)) {
                EVENT_LOG("Resolving YouTube stream via yt-dlp -g (async)...");
                pool_.submit([this, orig_url, has_video, next, title, duration]() {
                    auto yt_urls = resolve_youtube_url(orig_url, has_video);
                    if (yt_urls.empty()) { EVENT_LOG("YouTube resolve failed — skipping playback"); return; }
                    std::string audio = (yt_urls.size() >= 2) ? yt_urls[1] : std::string();
                    std::string sub = (yt_urls.size() >= 3) ? yt_urls[2] : std::string();
                    player.play(yt_urls[0], has_video, audio, sub);
                    player.set_keep_open(false);
                    player.set_pause(false);
                    player.set_loop_file(false);
                    record_play_history(orig_url, title, duration);
                    EVENT_LOG(fmt::format("[AUTOPLAY] -> idx {} '{}'", next, title));
                });
                return;  // don't proceed synchronously
            } else if (ut == URLType::BILIBILI_VIDEO || ut == URLType::DOUYIN_VIDEO || ut == URLType::TIKTOK_VIDEO) {
                // Y21 (issue 3): play the watch URL via mpv ytdl_hook (Referer from yt-dlp extractor).
                player.play(orig_url, true);
                player.set_keep_open(false);
                player.set_pause(false);
                player.set_loop_file(false);
                record_play_history(orig_url, title, duration);
                EVENT_LOG(fmt::format("[AUTOPLAY] -> idx {} '{}'", next, title));
                return;
            } else {
                // Non-YouTube — play directly
                player.play(orig_url, has_video);
                player.set_keep_open(false);
                player.set_pause(false);
                player.set_loop_file(false);
            }
        }

        record_play_history(orig_url, current_playlist[next].title,
                            current_playlist[next].duration);

        EVENT_LOG(fmt::format("[AUTOPLAY] {} -> idx {} '{}'",
            play_mode == PlayMode::SHUFFLE ? "SHUFFLE" : "CYCLE",
            next, current_playlist[next].title));
    }

    // F23: Extract YouTube URL resolution into a standalone method (callable from pool threads).
    //   Y09 (1A DASH): picks the yt-dlp `-f` format from [youtube] play_format_video/play_format_audio
    //   by has_video, and returns ALL stream URLs yt-dlp prints (1 for muxed/HLS/audio-only,
    //   2 for DASH bestvideo+bestaudio). The caller feeds video URL + audio-file to mpv so it can
    //   merge DASH streams → 1080p. Empty vector = resolve failed (caller skips, no fallback).
    //   Y24.47: in audio-only mode (--quiet / vo=null / vid=no) pick the AUDIO format even for video
    //   items, so yt-dlp returns an audio-only stream URL → the video stream is never downloaded
    //   (saves bandwidth). Played via play_video with vo=null (no window) so sub_file (lyrics) still
    //   loads. For direct muxed URLs (non-YouTube) this can't apply — see mpv/container limitation.
    std::vector<std::string> App::resolve_youtube_url(const std::string& url, bool has_video) const {
        std::vector<std::string> base_args = YouTubeChannelParser::ytdlp_youtube_args();
        bool audio_only = MPVController::is_audio_only_mode();
        bool want_video = has_video && !audio_only;
        std::string fmt = want_video
            ? IniConfig::instance().get_youtube_play_format_video()
            : IniConfig::instance().get_youtube_play_format_audio();

        // YT-fix: retry loop with a generous, configurable timeout. yt-dlp YouTube resolution is flaky
        //   — through a SOCKS proxy + quickjs/ejs nsig solving a single `-g` can take 30-60s, and the
        //   old fixed 30s cap caused intermittent "YouTube resolve failed" (exact-30s YtdlpRunner
        //   timeouts in the log). Re-resolving is cheap and usually succeeds on retry.
        int timeout_sec = IniConfig::instance().get_youtube_resolve_timeout_sec();
        int attempts = IniConfig::instance().get_youtube_resolve_retries();
        if (attempts < 1) attempts = 1;
        std::vector<std::string> urls;
        YtdlpRunner::Result result;
        int attempt = 0;
        for (attempt = 1; attempt <= attempts && urls.empty(); ++attempt) {
            std::vector<std::string> args = base_args;
            args.push_back("-f"); args.push_back(fmt);
            args.push_back("-g");
            args.push_back("--no-warnings");
            args.push_back(url);
            result = YtdlpRunner::run(args, nullptr, timeout_sec);
            urls.clear();
            if (result.launched && result.exit_code == 0 && !result.stdout_output.empty()) {
                std::istringstream ss(result.stdout_output);
                std::string line;
                while (std::getline(ss, line)) {
                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                        line.pop_back();
                    if (!line.empty() && line.rfind("http", 0) == 0) {
                        urls.push_back(line);
                    }
                }
            }
            if (urls.empty()) {
                LOG(fmt::format("[YouTube] resolve attempt {}/{} failed (launched={}, exit={}, timeout={}s)",
                                attempt, attempts, result.launched, result.exit_code, timeout_sec));
                if (attempt < attempts)
                    EVENT_LOG(fmt::format("YouTube resolve retry {}/{} (nsig/proxy can be slow)...", attempt, attempts));
            }
        }
        // Y11: fetch soft subtitle (.vtt) when [youtube] sub_lang is set; append its path as urls[2].
        //   mpv loads it via sub-file (per-file option) so F/G size, z/Z sync, r/R pos, v visibility
        //   all work (vtt subs are scalable; mpv centers by default). sub_auto=true falls back to
        //   auto-generated captions when no manual subtitle exists for sub_lang.
        std::string sub_lang = IniConfig::instance().get_youtube_sub_lang();
        if (!urls.empty() && !sub_lang.empty()) {
            std::string tmp_dir = Paths::get_tmp_dir();
            if (!tmp_dir.empty()) {
                std::string prefix = tmp_dir + "/pod_sub";
                std::vector<std::string> sargs = YouTubeChannelParser::ytdlp_youtube_args();
                sargs.push_back("--write-subs");
                if (IniConfig::instance().get_youtube_sub_auto()) sargs.push_back("--write-auto-sub");
                sargs.push_back("--sub-langs"); sargs.push_back(sub_lang);
                sargs.push_back("--sub-format"); sargs.push_back("vtt");
                sargs.push_back("--skip-download");
                sargs.push_back("-o"); sargs.push_back(prefix);
                sargs.push_back("--no-warnings");
                sargs.push_back(url);
                YtdlpRunner::run(sargs, nullptr, 20);
                // yt-dlp writes <prefix>.<lang>.vtt (or <prefix>.<lang>-auto.vtt); pick the first .vtt.
                std::error_code ec;
                for (auto& e : fs::directory_iterator(tmp_dir, ec)) {
                    std::string n = e.path().filename().string();
                    if (n.rfind("pod_sub", 0) == 0 && e.path().extension() == ".vtt") {
                        urls.push_back(e.path().string());
                        LOG(fmt::format("[YouTube] subtitle loaded: {}", n));
                        break;
                    }
                }
            }
        }
        if (!urls.empty()) {
            LOG(fmt::format("[YouTube] Resolved {} stream URL(s) (fmt={}, has_video={}, audio_only={}, attempts={}): {}",
                            urls.size(), fmt, has_video, audio_only, attempt > attempts ? attempts : attempt,
                            urls[0].substr(0, 70)));
            return urls;
        }
        LOG(fmt::format("[YouTube] yt-dlp -g failed after {} attempt(s) (launched={}, exit={}, fmt={})",
                        attempt > attempts ? attempts : attempt, result.launched, result.exit_code, fmt));
        if (!result.stderr_output.empty()) {
            std::string err = result.stderr_output;
            size_t pos = err.find_last_of('\n');
            if (pos != std::string::npos && pos > 0) {
                size_t prev = err.find_last_of('\n', pos - 1);
                err = err.substr(prev == std::string::npos ? 0 : prev + 1);
                while (!err.empty() && err.back() == '\n') err.pop_back();
            }
            LOG(fmt::format("[YouTube] yt-dlp error: {}", err));
            EVENT_LOG(fmt::format("YouTube resolve failed: {}", err));
        } else {
            EVENT_LOG(fmt::format("YouTube resolve failed after {} attempt(s) (no yt-dlp output) — cookies/proxy/JS-runtime?",
                                  attempt > attempts ? attempts : attempt));
        }
        return {};  // empty → caller skips playback (single resolve path, no fallback)
    }

    // Pick one random index in [0, size) avoiding `avoid` when possible.
    int App::random_peer_index(int avoid) const {
        int size = static_cast<int>(current_playlist.size());
        if (size <= 0) return -1;
        if (size == 1) return 0;
        static thread_local std::mt19937 gen(std::random_device{}());
        
        std::uniform_int_distribution<int> dist(0, size - 1);
        int idx = dist(gen);
        if (idx == avoid) idx = (idx + 1) % size;
        return idx;
    }

    // Keep shuffle_queue_ at 3 entries (pre-generated upcoming random indices).
    // Called under playlist_mutex_.
    void App::refill_shuffle_queue() {
        while (shuffle_queue_.size() < 3) {
            int last = shuffle_queue_.empty() ? current_index : shuffle_queue_.back();
            int idx = random_peer_index(last);
            if (idx < 0) break;
            shuffle_queue_.push_back(idx);
        }
    }

    // Play a single item by index (pointer-driven model).
    // F23: YouTube URLs resolved async in pool_ (non-blocking); local/non-YouTube play immediately.
    void App::play_current(int idx) {
        transcription_engine_.stop_realtime();  // Y24.20: realtime transcription doesn't carry across tracks
        std::string orig_url;
        bool has_video = false;
        std::string title;       // P1-4: snapshot under the lock; lambdas capture by value
        int duration = 0;
        TreeNodePtr pn;  // F35: source node of the playing item (for INFO title)
        {
            std::lock_guard<std::mutex> pl_lock(playlist_mutex_);
            if (idx < 0 || idx >= static_cast<int>(current_playlist.size())) return;
            current_index = idx;
            orig_url = current_playlist[idx].url;
            has_video = current_playlist[idx].is_video;
            title = current_playlist[idx].title;
            duration = current_playlist[idx].duration;
            pn = current_playlist[idx].node;
            if (play_mode == PlayMode::SHUFFLE) {
                shuffle_queue_.clear();
                refill_shuffle_queue();
            }
        }
        // F35: track the playing node on the main thread (for all 3 play paths, incl. YouTube
        //   which resolves the stream async — the TITLE is the node title, known immediately).
        //   Previously playback_node was always reset to nullptr, so INFO Title fell back to
        //   mpv media-title (stream URL/ICY for radio, not the station name).
        playback_node = pn;
        playback_mode_ = mode;  // Y24.54: save mode for N = jump-to-playing
        // Y24.55: flag IPTV context so mpv_controller emits IPTV: messages alongside MPV: behavior
        //   for the same event (off-air, audio-only, slow, load/AO/VO/decode failures).
        player.set_iptv_context(mode == AppMode::IPTV);
        // Y24.8: subtitle handling is FULLY ASYNC for audio — player.play() is called immediately
        //   below with no synchronous fs::exists probe (which was slow on /mnt/e WSL2 mounts).
        //   Method A (mpv sub-file) is only used for VIDEO (mpv renders in the video window); AUDIO
        //   always uses Method B (SubtitleManager fetches+parses async, drives LYRIC via time_pos).
        // Y24.17: VIDEO sidecar probe is now ASYNC too (was sync fs::exists for .vtt/.srt/.ass).
        //   The sub-file URL isn't known at loadfile time, so it's added via mpv sub-add after the
        //   async probe finds it. LOG each step. AUDIO was already async.
        if (has_video) {
            // Y24.18: reset Method B first — video uses Method A (mpv renders) or no subtitle; either
            //   way the LYRIC panel must drop the previous (audio) track's lyrics. JSON → Method B
            //   fill below. Done sync (on the calling thread) so the panel clears immediately.
            subtitle_mgr_.reset();
            TreeNodePtr pn_cap = pn;
            pool_.submit([this, pn_cap]() {
                LOG("[Subtitle] video sidecar probe (async)...");
                subtitle_mgr_.probe_sidecar(pn_cap);
                if (pn_cap && pn_cap->has_subtitle && !pn_cap->subtitle_url.empty()) {
                    if (is_mpv_sub_url(pn_cap->subtitle_url)) {
                        player.sub_add(pn_cap->subtitle_url);
                        LOG(fmt::format("[Subtitle] mpv sub-add: {} (Method A — mpv renders)", basename_of(pn_cap->subtitle_url)));
                    } else {
                        LOG(fmt::format("[Subtitle] podradio resolves: {} (Method B — video, non-mpv format)", basename_of(pn_cap->subtitle_url)));
                        subtitle_mgr_.load_async(pn_cap, pool_);  // fills Method B (LYRIC for JSON)
                    }
                } else {
                    LOG("[Subtitle] video: no local sidecar found (async)");
                }
            });
        } else {
            // AUDIO: always Method B, fully async (probe+fetch+parse in pool). Never blocks play.
            //   load_async probes for a local sidecar async; if none and no transcript URL → NONE.
            subtitle_mgr_.load_async(pn, pool_);
            if (pn && pn->has_subtitle && !pn->subtitle_url.empty())
                LOG(fmt::format("[Subtitle] podradio resolves: {} (Method B — audio, async)", basename_of(pn->subtitle_url)));
        }

        // Y23.9: BUFFERING state — set pending before play; cleared when mpv reports has_media.
        // Y24.17: timestamp each step so podradio.log shows WHERE the wait goes (sync fs::exists/DB
        //   vs mpv load). >5s on a local file is abnormal — this pinpoints it.
        playback_pending_ = true;
        playback_pending_start_ = std::chrono::steady_clock::now();
        auto play_t0 = std::chrono::steady_clock::now();
        auto ms_since = [&]() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - play_t0).count();
        };
        LOG(fmt::format("[PLAY] play_current start: idx={} '{}' (is_video={})", idx, title, has_video));

        std::string local = CacheManager::instance().get_local_file(orig_url);
        std::string local_url;
        // Y23.9: pass the RAW path to mpv (no file:// prefix) — mpv handles special chars
        //   (Chinese, spaces, : ! etc.) natively in raw paths, but file:// URLs require encoding
        //   which was missing → MPV_ERROR_LOADING_FAILED (-14) on paths with non-ASCII/special chars.
        if (!local.empty() && fs::exists(local)) local_url = local;
        LOG(fmt::format("[PLAY] get_local_file+fs::exists: local='{}' ({}ms)", local_url, ms_since()));
        URLType ut = URLClassifier::classify(orig_url);

        if (!local_url.empty()) {
            auto [saved_pos, completed] = DatabaseManager::instance().get_progress(local_url);
            if (saved_pos > 5.0 && !completed && ut != URLType::RADIO_STREAM) {
                player.set_resume_position(local_url, saved_pos);
                EVENT_LOG(fmt::format("Resume from {:02d}:{:02d}", static_cast<int>(saved_pos) / 60, static_cast<int>(saved_pos) % 60));
            }
            // Y24.17: video subtitle is probed+added async (mpv sub-add); pass "" here.
            player.play(local_url, has_video, "", "");  // Y24.17: video sub added async via sub-add
            player.set_keep_open(play_mode == PlayMode::REPEAT);
            player.set_loop_file(play_mode == PlayMode::REPEAT);
            player.set_pause(false);
            record_play_history(orig_url, title, duration);
            EVENT_LOG(fmt::format("Play local file: idx {} '{}'", idx, orig_url));
        } else if (URLClassifier::is_youtube(ut)) {
            EVENT_LOG("Resolving YouTube stream via yt-dlp -g (async)...");
            pool_.submit([this, orig_url, has_video, idx, title, duration]() {
                auto yt_urls = resolve_youtube_url(orig_url, has_video);
                if (yt_urls.empty()) { EVENT_LOG("YouTube resolve failed — skipping playback"); return; }
                std::string audio = (yt_urls.size() >= 2) ? yt_urls[1] : std::string();
                std::string sub = (yt_urls.size() >= 3) ? yt_urls[2] : std::string();
                player.play(yt_urls[0], has_video, audio, sub);
                player.set_keep_open(play_mode == PlayMode::REPEAT);
                player.set_loop_file(play_mode == PlayMode::REPEAT);
                player.set_pause(false);
                record_play_history(orig_url, title, duration);
                EVENT_LOG(fmt::format("Play online streaming: idx {} '{}'", idx, orig_url));
            });
        } else if (ut == URLType::BILIBILI_VIDEO || ut == URLType::DOUYIN_VIDEO || ut == URLType::TIKTOK_VIDEO) {
            // Y21 (issue 3): play the watch URL directly via mpv's ytdl_hook. yt-dlp's bilibili
            //   extractor supplies the required Referer/http_headers to mpv for ALL streams
            //   (video+audio DASH), which a pre-resolve `yt-dlp -g` + manual audio-file approach
            //   cannot do (CDN 403 on the audio stream). Correct single path — no pre-resolve.
            player.play(orig_url, true);
            player.set_keep_open(play_mode == PlayMode::REPEAT);
            player.set_loop_file(play_mode == PlayMode::REPEAT);
            player.set_pause(false);
            record_play_history(orig_url, title, duration);
            EVENT_LOG(fmt::format("Play Bilibili (ytdl_hook): idx {} '{}'", idx, orig_url));
        } else {
            // Y24.17: video subtitle probed+added async; pass "" here.
            player.play(orig_url, has_video, "", "");  // Y24.17: video sub added async via sub-add
            player.set_keep_open(play_mode == PlayMode::REPEAT);
            player.set_loop_file(play_mode == PlayMode::REPEAT);
            player.set_pause(false);
            record_play_history(orig_url, title, duration);
            // F41: file:// URLs are local files (incl. WSL2 /mnt/ mounts), not "online streaming".
            const char* play_kind = (orig_url.rfind("file://", 0) == 0) ? "local file" : "online streaming";
            EVENT_LOG(fmt::format("Play {}: idx {} '{}'", play_kind, idx, orig_url));
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Peer-list construction (the implicit playlist = siblings of the played episode)
    // ═════════════════════════════════════════════════════════════════════════

    // Build current_playlist from the playable siblings (peers) of `node` under its
    //   parent, honoring the parent's sort_reversed order. current_index is set to the
    //   position of `node` itself (so playback starts there). If `node` has no parent /
    //   no siblings, the list contains just `node`.
    // Returns the resulting current_index, or -1 on failure.
    int App::build_peer_list(TreeNodePtr node) {
        if (!node) return -1;
        std::vector<PlaylistItem> peers;
        int target_idx = -1;

        auto parent = node->parent.lock();
        if (!parent || parent->children.empty()) {
            PlaylistItem it;
            it.title = node->title;
            it.url = node->url;
            it.duration = node->duration;
            URLType ut = URLClassifier::classify(node->url);
            it.is_video = node->is_youtube || URLClassifier::is_video(ut);
            it.node = node;
            peers.push_back(it);
            target_idx = 0;
        } else {
            auto& siblings = parent->children;
            bool reversed = parent->sort_reversed;
            auto push_peer = [&](TreeNodePtr sib) {
                if (sib->url.empty()) return;  // skip folders/titles
                PlaylistItem it;
                it.title = sib->title;
                it.url = sib->url;
                it.duration = sib->duration;
                URLType ut = URLClassifier::classify(sib->url);
                it.is_video = sib->is_youtube || URLClassifier::is_video(ut);
                it.node = sib;
                if (sib.get() == node.get()) target_idx = static_cast<int>(peers.size());
                peers.push_back(it);
            };
            if (!reversed) {
                for (auto& s : siblings) push_peer(s);
            } else {
                for (int i = static_cast<int>(siblings.size()) - 1; i >= 0; --i) push_peer(siblings[i]);
            }
            if (peers.empty()) {
                // No playable siblings (e.g. parent is a flat folder of subfolders). Scan the parent
                //   folder recursively for playable files to form the play list, so playback still
                //   has a queue. Final fallback: single item.
                auto parent = node->parent.lock();
                if (parent) {
                    std::vector<TreeNodePtr> items;
                    collect_playable_items(parent, items);
                    for (auto& it : items) {
                        PlaylistItem pi;
                        pi.title = it->title;
                        pi.url = it->url;
                        pi.duration = it->duration;
                        URLType ut = URLClassifier::classify(it->url);
                        pi.is_video = it->is_youtube || URLClassifier::is_video(ut);
                        pi.node = it;
                        if (it.get() == node.get()) target_idx = static_cast<int>(peers.size());
                        peers.push_back(pi);
                    }
                }
                if (peers.empty()) {
                    LOG(fmt::format("[PEERS] No playable files in parent folder, single-item fallback: {}", node->title));
                    // Final fallback: single item
                    PlaylistItem it;
                    it.title = node->title;
                    it.url = node->url;
                    it.duration = node->duration;
                    URLType ut = URLClassifier::classify(node->url);
                    it.is_video = node->is_youtube || URLClassifier::is_video(ut);
                    peers.push_back(it);
                    target_idx = 0;
                }
            }
        }

        std::lock_guard<std::mutex> pl_lock(playlist_mutex_);
        current_playlist = std::move(peers);
        current_index = target_idx;
        shuffle_queue_.clear();
        if (play_mode == PlayMode::SHUFFLE) refill_shuffle_queue();
        LOG(fmt::format("[PEERS] Built {} peers, current at idx {}", current_playlist.size(), current_index));
        return current_index;
    }

    // Play an episode node: snapshot its peers into current_playlist and play it.
    // Used by Enter/l in the main view.
    void App::play_episode(TreeNodePtr node) {
        if (!is_playable_node(node)) return;
        int idx = build_peer_list(node);
        if (idx >= 0) play_current(idx);
    }

    // Record playback history
    void App::record_play_history(const std::string& url, const std::string& title, int duration) {
        if (url.empty()) return;
        DatabaseManager::instance().add_history(url, title, duration);
        // Y24.26: rebuild history tree async (was sync — caused UI stutter on every track switch).
        pool_.submit([this]() { load_history_to_root(); });
    }

} // namespace podradio
