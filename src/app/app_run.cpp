#include <unistd.h> // _exit (skip destructors on clean exit)
#include "panicast/app/app.h"
#include "panicast/app/actions.h"
#include "panicast/app/playback_events.h"
#include "panicast/core/event_bus.h"
#include "panicast/net/bilibili_api.h"
#include "panicast/net/tiktok_region.h"
#include "panicast/parsers/bilibili_parser.h"
#include <cstdio>
#include <iostream>

namespace panicast
{

App::App() {
    Logger::instance().init();
    IniConfig::instance().load();
    // Initialize SQLite3 database
    DatabaseManager::instance().init();
    CacheManager::instance()
        .load(); // load media_cache into memory (else get_local_file empty -> downloaded items buffer)
    YouTubeCache::instance().load();

    // E: the 8 per-mode roots are now std::vector<TreeNodePtr> (default-constructed empty);
    //   no TreeNode root nodes to create. (online_root lives in OnlineState.)
    tiktok_region_ = IniConfig::instance().get("tiktok", "region", "US");
    if (TikTokRegion::name(tiktok_region_) == tiktok_region_ && tiktok_region_ != "US") {
        tiktok_region_ = "US"; // unknown code → default
    }
    TikTokRegion::set_current(tiktok_region_);

    // Load the global play mode at startup (no persisted playlist anymore)
    play_mode = IniConfig::instance().get_play_mode();
    LOG(fmt::format("[INIT] play_mode={}", static_cast<int>(play_mode)));
    transcription_engine_.init(
        &subtitle_mgr_, &pool_,
        &player); // Y24.28: pass mpv for video ASR  // Y24.19: whisper.cpp transcription
}

// P1-8: explicit destructor — stop the mpv event thread and join the pool BEFORE any member
//   is destroyed. The playlist queue (current_playlist / playlist_mutex_) now lives in
//   PlaybackService (D8b-1), declared right after player_; without this explicit stop the mpv
//   event thread could still fire on_playback_ended (touching the queue) during stack unwinding
//   → use-after-free. Order: player.stop() joins the mpv event thread, then pool_.shutdown()
//   joins workers.
App::~App() {
    running = false;
    // N01: stop the remote control server FIRST (joins accept + worker threads) and disable
    //   the command bus so any in-flight push becomes a no-op, before tearing down the rest.
    try {
        remote_server_.stop();
    } catch (...) {
    }
    remote_bus_.shutdown();
    try {
        player.stop();
    } catch (...) {
    }
    transcription_engine_.shutdown(); // Y24.19: stop transcription dispatcher
    Utils::
        kill_all_child_processes(); // N04-fix: kill tracked yt-dlp/whisper children before joining pool
    pool_.shutdown();
}

void App::run() {
    ui.init();
    // Confirm XML error handler is set (already set in main; this is a secondary confirmation)
    // Redirect errors to LOG/EVENT_LOG to avoid terminal output causing screen corruption
    xmlSetGenericErrorFunc(NULL, xml_error_handler);
    // libxml2 2.12+ callback signature uses const xmlError*; 2.9.x uses non-const.
    //   The handler uses the const version (for newer releases); this cast compatibilizes older libxml2 (e.g. Debian 12).
    xmlSetStructuredErrorFunc(NULL, (xmlStructuredErrorFunc)xml_structured_error_handler);
    if (!player.initialize())
        LOG("MPV initialization failed");

    // Register playback-ended callback to auto-play the next track
    player.set_end_file_callback([this](int reason) { pending_end_reason_.store(reason); });
    LOG("[AUTOPLAY] End-file callback registered");

    // D8: PlaybackService (first Application Service) owns the playback Action handlers
    //   (pause/volume) — subscribed via playback_.init(). Nav Actions stay here for now.
    playback_.init();
    // D8b-2: inject pool_/subtitle/transcription (declared after playback_ in App, so they can't
    //   be construction-time references). play_current / on_playback_ended live in PlaybackService
    //   now and own the track handles (playback_node_ / playback_mode_) directly (D9-2) — App reads
    //   them via playback_.playback_node() / playback_.playback_mode(). Must run before the loop.
    playback_.attach(pool_, subtitle_mgr_, transcription_engine_);
    // D9: subscribe to the playback state events App still consumes locally. The bus is
    //   synchronous, so these run on the publisher's thread (UI thread for buffering; pool thread
    //   for history). D9-2: PlaybackTrackChanged no longer needs an App subscriber — the track
    //   handles moved into the service (App reads the accessor); the event stays published as the
    //   reactor channel for future direct UI/remote subscribers (D10+).
    action_subs_.push_back(EventBus::instance().subscribe<PlaybackBufferingChanged>(
        [this](const PlaybackBufferingChanged &e) {
            if (e.pending) {
                playback_pending_ = true;
                playback_pending_start_ = std::chrono::steady_clock::now();
            } else {
                playback_pending_ = false;
            }
        }));
    action_subs_.push_back(EventBus::instance().subscribe<HistoryChanged>(
        [this](const HistoryChanged &) { load_history_to_root(); }));
    // D7: Keymap (key→Action) + nav input Actions.
    build_keymap();
    action_subs_.push_back(
        EventBus::instance().subscribe<NavUpAction>([this](const NavUpAction &) { nav_up(); }));
    action_subs_.push_back(EventBus::instance().subscribe<NavDownAction>(
        [this](const NavDownAction &) { nav_down(); }));

    // Y07: startup runtime-dependency pre-check for YouTube playback. Warn once in the LOG
    //   panel so a missing dependency is obvious immediately, instead of a cryptic
    //   "YouTube resolve failed (no yt-dlp output)" discovered only when the user tries to play.
    {
        std::string ytdlp = Utils::which_binary("yt-dlp");
        if (ytdlp.empty()) {
            EVENT_LOG("Warning: yt-dlp not installed / not in PATH — YouTube playback and parsing "
                      "unavailable. Install: pip install -U \"yt-dlp[default]\"");
        } else {
            LOG(fmt::format("[INIT] yt-dlp: {}", ytdlp));
            std::string qjs = Utils::which_binary("qjs");
            if (qjs.empty())
                qjs = Utils::which_binary("qjsng");
            if (qjs.empty() && Utils::which_binary("deno").empty()) {
                EVENT_LOG("Warning: no qjs/qjsng/deno detected — YouTube playback needs a JS "
                          "runtime to solve nsig. Install quickjs-ng (binary qjs) or deno");
            } else if (!qjs.empty()) {
                LOG(fmt::format("[INIT] JS runtime: {}", qjs));
                // quickjs can't fetch the EJS solver from npm (deno can); needs yt-dlp[default]
                //   (yt-dlp-ejs). Probe via yt-dlp's OWN environment (yt-dlp -v lists Optional
                //   libraries incl. yt_dlp_ejs) — reliable, no false negative from system python3
                //   differing from yt-dlp's install env. Best-effort, non-fatal.
                FILE *p = popen("yt-dlp -v 2>&1 | grep -q yt_dlp_ejs", "r");
                if (p) {
                    if (pclose(p) != 0)
                        EVENT_LOG("Warning: quickjs missing EJS solver — pip install -U "
                                  "\"yt-dlp[default]\" (includes yt-dlp-ejs)");
                }
            }
        }
    }

    Persistence::load_cache(radio_root, podcast_root);
    load_persistent_data();

    // Load history into history_root
    load_history_to_root();

    // Y01: load Google accounts into account_root (Y mode)
    load_accounts_root();
    load_bilibili_root(); // Y15: B-mode
    load_tiktok_root();   // Y24.11: T-mode

    // Restore last playback state
    restore_player_state();

    for (auto &it : radio_root)
        mark_cached_nodes(it);
    for (auto &it : podcast_root)
        mark_cached_nodes(it);

    if (radio_root.empty()) {
        // Use thread pool; App destructor safely joins (old code: .detach() had use-after-free)
        pool_.submit([this]() { load_radio_root(); });
    }
    // Always load built-in podcasts, merging with cached data
    load_default_podcasts();

    // Initialize layout manager
    LayoutMetrics::instance().check_resize();

    // N01: start the remote control server if [remote] enable=true (opt-in). Default off →
    //   zero impact on the local TUI. The server thread pushes RemoteCommands into remote_bus_;
    //   the main loop drains them below each frame.
    if (IniConfig::instance().get_remote_enabled()) {
        if (remote_server_.start(IniConfig::instance().get_remote_bind(),
                                 IniConfig::instance().get_remote_port(), this)) {
            EVENT_LOG(fmt::format("Remote control on — PIN {} (or {}). Open http://127.0.0.1:{}/ "
                                  "in a browser; ':pin' to show.",
                                  remote_server_.dynamic_pin(), remote_server_.universal_pin(),
                                  IniConfig::instance().get_remote_port()));
        } else {
            EVENT_LOG("Remote control server failed to start (see log); continuing without remote "
                      "control");
        }
    }

    while (running) {
        const auto frame_start_ = std::chrono::steady_clock::now();
        // Check SIGINT (CTRL+C) exit request
        if (g_exit_requested) {
            g_exit_requested = false; // reset flag
            // Termination signals (SIGHUP/SIGTERM/SIGQUIT — terminal closed / killed): flush & exit
            //   immediately WITHOUT a confirm popup. There may be no terminal to interact with, and the
            //   process is being told to die. Crucially this runs pool_.shutdown() so in-flight YouTube
            //   parses finish writing their episode_cache — otherwise the channel is empty on next start
            //   (the "abnormal exit loses the YouTube channel" bug).
            if (g_crash_sig != 0) {
                g_crash_sig = 0;
                EVENT_LOG(
                    fmt::format("Termination signal received, flushing cache before exit..."));
                running = false;
                break;
            }
            if (ui.confirm_box("Quit PANICAST? (CTRL+C)")) {
                running = false;
                break;
            }
        }

        // Check sleep timer
        if (SleepTimer::instance().is_active() && SleepTimer::instance().check_expired()) {
            EVENT_LOG("Sleep timer expired, exiting...");
            running = false;
            break;
        }

        // Unified window-size detection and scroll-offset reset
        // LayoutMetrics auto-detects size changes and resets all scroll offsets
        if (LayoutMetrics::instance().check_resize()) {
            // Use double buffering to avoid clear+refresh single-frame white flicker
            // Old code: clear(); refresh();  ← immediate refresh, white frame
            resizeterm(LINES, COLS);
            werase(stdscr);
            wnoutrefresh(stdscr);
            doupdate();
            EVENT_LOG(fmt::format("Terminal resized: {}x{}", COLS, LINES));
        }

        // N01: drain remote commands on the UI thread (server→bus→here). Non-blocking; no-op
        //   when the queue is empty or remote control is disabled.
        drain_remote_commands();
        // D4: run the playback-ended handler on the UI thread. The mpv event thread only QUEUES
        //   END_FILE here — previously it ran on_playback_ended on the mpv thread under
        //   playlist_mutex_, contending with this loop's draw lock (also playlist_mutex_) and
        //   freezing the TUI a while after pause (a paused live stream drops → END_FILE → the
        //   mpv thread blocks the UI's draw). Draining here runs the handler on the UI thread.
        int _end_reason = pending_end_reason_.exchange(-1);
        if (_end_reason != -1)
            playback_.on_playback_ended(_end_reason, mode, play_mode);
        // N02: refresh the state snapshot cache for remote query commands (status/currentsong).
        update_remote_state_cache();

        auto state = player.get_state();

        // Pointer-driven model: current_index is app-owned (set by play_current /
        //   on_playback_ended), NOT derived from mpv's playlist_pos. No per-frame
        //   sync from mpv is needed.

        // Improved state-detection logic to correctly show Navigating/Buffering/Playing/Pause
        // Add List-mode detection
        AppState app_state;
        if (state.has_media) {
            // Y24.17: log BUFFERING duration ONCE (on the pending→cleared transition, not every frame).
            if (playback_pending_) {
                auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - playback_pending_start_)
                                    .count();
                LOG(fmt::format(
                    "[PLAY] BUFFERING cleared: total {}ms (sync steps above + mpv load)",
                    total_ms));
                if (total_ms > 2000)
                    EVENT_LOG(
                        fmt::format("BUFFERING {}ms (see panicast.log for breakdown)", total_ms));
            }
            playback_pending_ = false; // Y23.9: mpv has media → clear pending
            if (state.core_idle && !state.paused) {
                // Has media, idle and not paused = buffering
                app_state = AppState::BUFFERING;
            } else if (state.paused) {
                // Has media, paused
                app_state = AppState::PAUSED;
            } else {
                // Has media, playing
                app_state = AppState::PLAYING;
            }
        } else if (playback_pending_) {
            // Y23.9: playback initiated but mpv hasn't loaded yet → BUFFERING (not BROWSING).
            // Timeout 30s → clear + BROWSING (stream likely failed).
            if (std::chrono::steady_clock::now() - playback_pending_start_ >
                std::chrono::seconds(30)) {
                playback_pending_ = false;
                EVENT_LOG("Playback pending timeout (30s) — stream may have failed");
                app_state = AppState::BROWSING;
            } else {
                app_state = AppState::BUFFERING;
            }
        } else {
            // No media, navigating
            app_state = AppState::BROWSING;
        }

        bool is_loading = false;
        {
            const auto _tw0 = std::chrono::steady_clock::now();
            std::lock_guard<std::recursive_mutex> lock(tree_mutex);
            const long _tree_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - _tw0)
                                           .count();
            if (_tree_wait_ms > 80)
                LOG(fmt::format("[WATCHDOG] waited {}ms for tree_mutex", _tree_wait_ms));
            display_list.clear();
            flatten_items(cur_items());
            for (const auto &item : display_list) {
                if (item.node->loading) {
                    is_loading = true;
                    break;
                }
            }
            // Y11: consume a pending async selection (set by a pool task that built a new node,
            //   e.g. YouTube search results) — move the cursor to it once, then clear.
            if (pending_select_) {
                for (size_t i = 0; i < display_list.size(); ++i) {
                    if (display_list[i].node == pending_select_) {
                        selected_idx = (int)i;
                        break;
                    }
                }
                pending_select_.reset();
            }
            // Y24.7: SubtitleManager poll — handoff pending transcript to UI + offset + logs.
            subtitle_mgr_.poll(ui, ui.is_lyric_bar_requested());
            // Y24.48: refresh lyric history EVERY frame (even when the bar is inactive) so an
            //   embedded sub cue (sub_text) is detected and can auto-open the bar.
            ui.update_lyric_history(player.get_state());
            // Y24.48: LYRIC bar activation — default CLOSED. Auto-open only when a displayable
            //   subtitle source exists (transcript READY / ASR running / embedded cue seen this
            //   track), OR the user manually opened (L). Manual=Closed suppresses auto until track
            //   change. VO open (video window) → always closed (subs render in mpv's window).
            bool lyric_active = false;
            if (!player.is_video_window_open() && ui.is_lyric_bar_requested()) {
                switch (ui.lyric_manual()) {
                case UI::LyricManual::Open:
                    lyric_active = true;
                    break;
                case UI::LyricManual::Closed:
                    lyric_active = false;
                    break;
                default: // Auto
                    lyric_active = subtitle_mgr_.status() == TranscriptStatus::READY ||
                                   transcription_engine_.realtime_running() ||
                                   ui.embedded_sub_confirmed();
                    break;
                }
            }
            ui.set_lyric_bar_active(lyric_active);
        }
        // Node-loading state has higher priority than browsing but lower than playback states
        if (is_loading && app_state == AppState::BROWSING)
            app_state = AppState::LOADING;

        int marked = count_marked_current();
        TreeNodePtr sel_node = (selected_idx >= 0 && selected_idx < (int)display_list.size())
                                   ? display_list[selected_idx].node
                                   : nullptr;
        auto downloads = ProgressManager::instance().get_all();
        // Free slots (completed entries just cleared by get_all) → promote pending downloads,
        //   capping active+visible entries at MAX_CONCURRENT_DOWNLOADS.
        pump_download_queue(downloads.size());
        // Collapsed summary for queued items (keeps the INFO panel from being overrun).
        if (!pending_downloads_.empty()) {
            DownloadProgress syn;
            syn.title = fmt::format("··· +{} pending download", pending_downloads_.size());
            syn.active = false;
            syn.complete = false;
            syn.failed = false;
            syn.is_youtube = false;
            downloads.push_back(syn);
        }

        int vh = LINES - 5;
        if (vh < 1)
            vh = 1;
        if (selected_idx < view_start)
            view_start = selected_idx;
        else if (selected_idx >= view_start + vh)
            view_start = selected_idx - vh + 1;
        if (view_start < 0)
            view_start = 0;

        // Snapshot the playing pointer + the INFO play-context (history 3 + next 3)
        //   under the lock. P1.2 (Y23.5): hold the lock during draw ONLY (not during input —
        //   handle_input → play_current → locks playlist_mutex_ → would deadlock if held).
        int current_index_snap = playback_.current_index();
        std::vector<int> next_snap;
        std::string cur_url_snap;
        {
            const auto _pw0 = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> pl_draw_lock(
                playback_.playlist_mutex()); // released before input
            const long _pl_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - _pw0)
                                         .count();
            if (_pl_wait_ms > 80)
                LOG(fmt::format("[WATCHDOG] waited {}ms for playlist_mutex_", _pl_wait_ms));
            {
                current_index_snap = playback_.current_index();
                int n = static_cast<int>(playback_.playlist().size());
                if (current_index_snap >= 0 && current_index_snap < n) {
                    cur_url_snap = playback_.playlist()[current_index_snap].url;
                    if (play_mode == PlayMode::REPEAT) {
                        for (int k = 0; k < 3; ++k)
                            next_snap.push_back(current_index_snap);
                    } else if (play_mode == PlayMode::CYCLE) {
                        for (int k = 1; k <= 3; ++k)
                            next_snap.push_back((current_index_snap + k) % n);
                    } else { // SHUFFLE — pre-generated lookahead
                        int cnt = 0;
                        for (int idx : playback_.shuffle_queue()) {
                            next_snap.push_back(idx);
                            if (++cnt == 3)
                                break;
                        }
                    }
                }
            }

            // Global play history (last 3, excluding the currently playing track)
            // P2 (Y23.7): throttle get_history(8) DB query to ~1/sec (was every frame ~33fps).
            //   record_play_history (on track change) invalidates immediately.
            static int hist_frame_cnt = 0;
            static std::vector<std::string> cached_hist_titles;
            static std::string cached_hist_url;
            if (++hist_frame_cnt >= 30 ||
                (!cur_url_snap.empty() && cur_url_snap != cached_hist_url)) {
                hist_frame_cnt = 0;
                cached_hist_url = cur_url_snap;
                cached_hist_titles.clear();
                auto hist = DatabaseManager::instance().get_history(8);
                for (auto &[u, t, ts, mt] : hist) {
                    if (!cur_url_snap.empty() && u == cur_url_snap)
                        continue;
                    cached_hist_titles.push_back(t);
                    if (cached_hist_titles.size() >= 3)
                        break;
                }
            }
            auto &hist_titles = cached_hist_titles;

            // INFO area renders the 7-line play context from playlist/current_index/
            //   play_mode + hist_titles + next_snap.
            // Y24.7: L-mode poll already ran above (handoff + activation); no separate call needed.
            ui.draw(mode, display_list, selected_idx, state, view_start, app_state,
                    playback_.playback_node(),
                    marked, search_query, current_match_idx, total_matches, sel_node, downloads,
                    visual_mode_, visual_start_, playback_.playlist(), current_index_snap,
                    play_mode, hist_titles, next_snap);
        } // release pl_draw_lock before input processing (avoids deadlock with play_current)

        // Wide-char input: wget_wch cleanly distinguishes special keys (KEY_CODE_YES) from
        //   committed characters (OK). In browsing mode only ASCII (<128) + special keys are
        //   dispatched to handle_input; non-ASCII (IME-committed CJK etc.) is silently dropped
        //   so it neither triggers commands nor pollutes the screen. Text-input popups
        //   (input_box/search/`:`) read their own input and still accept full UTF-8.
        wint_t wch;
        int wrc = wget_wch(stdscr, &wch);
        if (wrc == KEY_CODE_YES) {
            handle_input(static_cast<int>(wch), marked); // special key / mouse
        } else if (wrc == OK && wch < 128) {
            handle_input(static_cast<int>(wch), marked); // ASCII only
        }
        // wrc == ERR (no input) or non-ASCII char → drop silently
        const long _frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - frame_start_)
                                   .count();
        if (_frame_ms > 150)
            LOG(fmt::format("[WATCHDOG] slow frame: {}ms (tree/pl wait logged above if >80ms)",
                            _frame_ms));
    }

    // Shut down the thread pool and join all background load tasks before exiting,
    //   to avoid concurrent mutations causing torn reads/crashes while save_cache serializes the tree
    // N04-fix: kill in-flight yt-dlp/whisper children FIRST so worker threads blocked in
    //   YtdlpRunner::run's poll()/waitpid() unblock and pool_.shutdown() can join promptly.
    //   Without this, a worker stuck in a 600s yt-dlp call makes the process unkillable by SIGTERM.
    Utils::kill_all_child_processes();
    pool_.shutdown();

    save_persistent_data();
    Persistence::save_cache(radio_root, podcast_root);

    // Save player state
    auto player_state = player.get_state();
    const auto cur_node = playback_.playback_node();
    std::string current_title = cur_node ? cur_node->title : "";
    int mode_int = static_cast<int>(mode);
    DatabaseManager::instance().save_player_state(
        player_state.volume, player_state.speed, player_state.paused, player_state.current_url,
        player_state.time_pos, ui.is_scroll_mode(),
        ui.is_show_tree_lines(), // persist user T-key preference (no longer hardcoded true)
        current_title, mode_int);
    // Also save to the progress table (dedicated to resume playback)
    if (!player_state.current_url.empty() && player_state.time_pos > 5.0) {
        bool completed = (player_state.media_duration > 0 &&
                          player_state.time_pos >= player_state.media_duration - 5.0);
        DatabaseManager::instance().save_progress(player_state.current_url, player_state.time_pos,
                                                  completed);
        LOG(fmt::format("[Progress] Saved: {} at {:.1f}s (completed={})", player_state.current_url,
                        player_state.time_pos, completed));
    }
    EVENT_LOG("Player state saved");

    // Stop the mpv event thread before exit — ensures the on_playback_ended callback
    //   no longer fires, preventing it from accessing current_playlist/playlist_mutex_
    //   (destroyed before player) during App destruction
    player.stop();

    ui.cleanup();
    // Kill EVERY tracked child subtree before exiting. _exit(0) below skips ~App (where
    //   kill_all_child_processes normally runs), so without this only DIRECT children die —
    //   via the kernel's PR_SET_PDEATHSIG — while GRANDCHILDREN (e.g. yt-dlp's ffmpeg merge
    //   child, or ffmpeg spawned by a download) get reparented to init and keep running. The
    //   tracked children are each process-group leaders (pgid==pid), so kill(-pgid) takes down
    //   the whole subtree. Terminal is already restored above; the ~200ms SIGTERM→SIGKILL grace
    //   is invisible. After this, _exit(0) returns to the shell prompt immediately.
    Utils::kill_all_child_processes();
    // Exit IMMEDIATELY — skip ~App destructors (the pool workers are detached and could
    //   access App members during destruction → use-after-free). The terminal is already
    //   restored by ui.cleanup; the OS reclaims all resources (threads, mpv, DB handles).
    _exit(0);
}

// Public method for loading persistent data in command-line mode
void App::load_data() {
    Persistence::load_cache(radio_root, podcast_root);
    load_persistent_data();
    // Load region preference from INI at startup
    OnlineState::instance().load_region_from_config();
    std::cout << "Loaded " << podcast_root.size() << " podcasts from cache" << std::endl;
}

// Load history from SQLite into history_root
void App::load_history_to_root() {
    auto history = DatabaseManager::instance().get_history(100); // get the most recent 100 entries
    std::lock_guard<std::recursive_mutex> lock(tree_mutex);
    history_root.clear();

    for (const auto &[url, title, timestamp, mt] : history) {
        auto node = std::make_shared<TreeNode>();
        node->title = title.empty() ? "Unknown" : title;
        node->url = url;
        // N06: display category from the DB (no runtime URL inference of the icon).
        node->media_type = static_cast<MediaType>(mt);
        node->media_type_set = true;
        // Determine type from the URL
        URLType url_type = URLClassifier::classify(url);
        if (URLClassifier::is_video(url_type)) {
            node->type = NodeType::PODCAST_EPISODE;
            node->is_youtube = true;
        } else if (url.find(".mp3") != std::string::npos || url.find(".aac") != std::string::npos ||
                   url.find(".m3u8") != std::string::npos) {
            node->type = NodeType::RADIO_STREAM;
        } else {
            node->type = NodeType::PODCAST_EPISODE;
        }
        node->children_loaded = true;
        node->subtext = timestamp; // store timestamp in subtext
        history_root.push_back(node);
    }
}

void App::load_persistent_data() {
    std::vector<TreeNodePtr> podcasts, favs;
    Persistence::load_data(podcasts, favs);
    std::lock_guard<std::recursive_mutex> lock(tree_mutex);
    if (!podcast_loaded) {
        for (auto &n : podcasts) {
            n->parent.reset(); // set parent pointer
            podcast_root.push_back(n);
        }
        podcast_loaded = true;
    }
    for (auto &n : favs) {
        n->parent.reset(); // set parent pointer
        fav_root.push_back(n);
    }
}

void App::save_persistent_data() {
    std::lock_guard<std::recursive_mutex> lock(tree_mutex);
    Persistence::save_data(podcast_root, fav_root);
}

// Restore last playback state
// Extended to restore UI state
void App::restore_player_state() {
    auto saved_state = DatabaseManager::instance().load_player_state();

    // Restore volume
    if (saved_state.volume >= 0 && saved_state.volume <= 100) {
        player.set_volume(saved_state.volume);
        EVENT_LOG(fmt::format("Restored volume: {}%", saved_state.volume));
    }

    // Restore playback speed
    if (saved_state.speed >= 0.5 && saved_state.speed <= 2.0) {
        player.set_speed(saved_state.speed);
        EVENT_LOG(fmt::format("Restored speed: {:.2f}x", saved_state.speed));
    }

    // Restore playback position (if there is a media that was playing)
    if (!saved_state.current_url.empty() && saved_state.position > 0) {
        // Try to restore playback
        // Note: do not auto-start playback; only restore position info
        EVENT_LOG(fmt::format("Last played: {} (position: {:.1f}s)", saved_state.current_url,
                              saved_state.position));

        // Save to the progress table so playback can resume from this position next time
        DatabaseManager::instance().save_progress(saved_state.current_url, saved_state.position,
                                                  false);
    }

    // Restore UI state
    ui.set_scroll_mode(saved_state.scroll_mode);
    ui.set_show_tree_lines(saved_state.show_tree_lines);
    EVENT_LOG(fmt::format("Restored UI: scroll_mode={}, tree_lines={}",
                          saved_state.scroll_mode ? "ON" : "OFF",
                          saved_state.show_tree_lines ? "ON" : "OFF"));

    // Restore mode
    // Upper bound validated against the enum element count, to avoid hardcoded 4 breaking after enum extension
    constexpr int APP_MODE_COUNT = 8; // Y24.27: 8 modes (was 7, TIKTOK added in Y24.11)
    if (saved_state.current_mode >= 0 && saved_state.current_mode < APP_MODE_COUNT) {
        mode = static_cast<AppMode>(saved_state.current_mode);
        switch_mode(mode); // Y24.27: delegate to unified switch_mode
        EVENT_LOG(fmt::format("Restored mode: {}", static_cast<int>(mode)));
    }

    // Restore the last played title
    if (!saved_state.current_title.empty()) {
        EVENT_LOG(fmt::format("Last played: {}", saved_state.current_title));
    }
}

void App::load_radio_root() {
    EVENT_LOG("Fetching Radio stations...");
    std::string data = Network::fetch("https://opml.radiotime.com/Browse.ashx?formats=mp3,aac");
    if (!data.empty()) {
        auto parsed = OPMLParser::parse(data);
        if (parsed) {
            std::lock_guard<std::recursive_mutex> lock(tree_mutex);
            radio_root = parsed->children;
            radio_loaded = true;
            EVENT_LOG(fmt::format("Radio: {} stations loaded", radio_root.size()));
            Persistence::save_cache(radio_root, podcast_root);
        }
    }
}

void App::spawn_load_radio(TreeNodePtr node, bool force) {
    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        if (node->loading)
            return;
        node->loading = true;
    }

    // Unified print of the full URL
    EVENT_LOG(fmt::format("Loading: [RADIO] {}", node->url));

    if (node->children_loaded && !force) {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        node->loading = false;
        node->expanded = true;
        return;
    }

    // Use thread pool (old code: std::thread([this,node]{...}).detach();)
    pool_.submit([this, node]() {
        std::string url = node->url;
        // Reset XML error state
        reset_xml_error_state();

        std::string data = Network::fetch(url);
        if (!data.empty()) {
            auto parsed = OPMLParser::parse(data);
            if (parsed) {
                std::lock_guard<std::recursive_mutex> lock(tree_mutex);
                node->children = parsed->children;
                node->children_loaded = true;
                node->expanded = true;
                node->parse_failed = false;
                node->is_cached = true; // parsed — episode_cache is the persistence
                EVENT_LOG(fmt::format("Loaded: {} items", node->children.size()));
                Persistence::save_cache(radio_root, podcast_root);
            }
        }
        {
            std::lock_guard<std::recursive_mutex> lock(tree_mutex);
            node->loading = false;
        }

        // Sync LINK node status
        sync_link_node_status(node);
    });
}

void App::spawn_load_feed(TreeNodePtr node) {
    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        if (node->loading)
            return;
        node->loading = true;
        node->parse_failed = false;
    }

    // Reset XML error state
    reset_xml_error_state();

    std::string url = node->url;
    URLType url_type = URLClassifier::classify(url);

    // Unified print of the full URL
    EVENT_LOG(fmt::format("Loading: [{}] {}", URLClassifier::type_name(url_type), url));

    // Use thread pool (old code: std::thread detach)
    // Note: Apple Podcast feed lookup (a network request) runs inside the thread to avoid blocking the UI thread.
    pool_.submit([this, node, url, url_type]() {
        // Reset XML error state (inside thread)
        reset_xml_error_state();

        // Y24.40: parse + commit delegated to focused helpers.
        std::string cur_url;
        bool abort = false;
        TreeNodePtr result = parse_feed_by_type(node, url, url_type, cur_url, abort);
        if (abort)
            return; // Apple lookup failed; node state already set inside parse_feed_by_type
        commit_feed_result(node, result, cur_url);
    });
}

// Apple-lookup + YouTube-cache + ParserRegistry dispatch. Produces the parsed result tree.
//   cur_url_out receives the (possibly Apple-rewritten) URL used for parsing and the episode-cache key.
//   abort_out=true means Apple lookup failed and node state is already set; caller must skip commit.
TreeNodePtr App::parse_feed_by_type(TreeNodePtr node, const std::string &url, URLType url_type,
                                    std::string &cur_url_out, bool &abort_out) {
    abort_out = false;

    // Apple Podcast: look up the feed URL first (network request, in-thread to avoid blocking the UI for 30s)
    std::string cur_url = url;
    URLType cur_type = url_type;
    if (cur_type == URLType::APPLE_PODCAST) {
        std::string feed = Network::lookup_apple_feed(cur_url);
        if (!feed.empty()) {
            cur_url = feed;
            cur_type = URLClassifier::classify(cur_url);
            EVENT_LOG(fmt::format("Apple -> {}", cur_url));
        } else {
            // lookup_apple_feed() already emitted a detailed EVENT_LOG with the reason + retry count.
            std::lock_guard<std::recursive_mutex> lock(tree_mutex);
            node->loading = false;
            node->parse_failed = true;
            node->error_msg = "Apple lookup failed (see LOG)";
            abort_out = true;
            cur_url_out = cur_url;
            return nullptr;
        }
    }
    cur_url_out = cur_url;

    TreeNodePtr result = nullptr;

    switch (cur_type) {
    case URLType::YOUTUBE_CHANNEL:
    case URLType::YOUTUBE_PLAYLIST: {
        // Channels (bare -> enumerate tabs / tab segment -> episode list) and playlists (-> episode list)
        //   are uniformly dispatched by YouTubeChannelParser::parse based on URL form
        //   via ParserRegistry self-registration (YouTube parsing brings its own network/yt-dlp, no pre-fetch needed)
        // BUG5: consult YouTubeCache first — on a cache hit, build the result tree directly
        //   from the cached video list and skip the network/yt-dlp parse entirely. On a miss,
        //   parse normally and feed the parsed children back into the cache.
        // Y23.3: a cache hit with an EMPTY video list is a stale bare-channel entry (pre-Y23.3
        //   bug: tabs were skipped → empty list cached → next load "No items found" → ✗). Treat
        //   an empty cache entry as a miss and re-parse.
        bool cache_hit = false;
        if (YouTubeCache::instance().has(cur_url)) {
            auto cache = YouTubeCache::instance().get(cur_url);
            if (!cache.videos.empty()) {
                result = std::make_shared<TreeNode>();
                result->is_youtube = true;
                result->type = NodeType::PODCAST_FEED;
                result->title = cache.channel_name;
                result->channel_name = cache.channel_name;
                for (const auto &v : cache.videos) {
                    auto ep = std::make_shared<TreeNode>();
                    ep->type = NodeType::PODCAST_EPISODE;
                    ep->title = v.title;
                    ep->url = v.url;
                    ep->is_youtube = true;
                    ep->children_loaded = true;
                    ep->parent = result;
                    result->children.push_back(ep);
                }
                EVENT_LOG(fmt::format("YouTube cache hit: {} videos for {}", cache.videos.size(),
                                      cur_url));
                cache_hit = true;
            }
        }
        if (!cache_hit) {
            cache_youtube_videos(node, cur_type, cur_url, result);
        }
        break;
    } // close case YOUTUBE_CHANNEL/PLAYLIST
    case URLType::YOUTUBE_RSS:
    case URLType::RSS_PODCAST: {
        // Dispatch via ParserRegistry self-registration; fetch still happens here (consistent with original behavior)
        if (auto p = ParserRegistry::instance().create(cur_type)) {
            std::string data = Network::fetch(cur_url);
            if (!data.empty())
                result = p->parse({data, cur_url});
        }
        break;
    }
    case URLType::OPML: {
        if (auto p = ParserRegistry::instance().create(cur_type)) {
            std::string data = Network::fetch(cur_url);
            if (!data.empty())
                result = p->parse({data, cur_url});
        }
        break;
    }
    // Y18: Bilibili uses WBI-signed arc/search API (real titles); Douyin uses yt-dlp flat-playlist.
    case URLType::BILIBILI_CHANNEL: {
        // Y18: Only WBI-signed API (real titles). No flat-playlist fallback.
        //   arc/search doesn't need SESSDATA (public videos), only WBI signing.
        //   SESSDATA is read if available (for member-only content) but not required.
        std::string mid;
        auto pos = cur_url.find("space.bilibili.com/");
        if (pos != std::string::npos) {
            mid = cur_url.substr(pos + 19);
            auto slash = mid.find('/');
            if (slash != std::string::npos)
                mid = mid.substr(0, slash);
        }
        std::string sessdata;
        std::string ck = IniConfig::instance().get_bilibili_cookies_file();
        std::error_code ec;
        if (!ck.empty() && std::filesystem::exists(ck, ec)) {
            std::ifstream cf(ck);
            std::string content((std::istreambuf_iterator<char>(cf)),
                                std::istreambuf_iterator<char>());
            sessdata = BilibiliAPI::extract_sessdata_from_cookies_txt(content);
        }
        result = BilibiliParser::parse_user_videos(sessdata, mid, cur_url, node->title);
        break;
    }
    case URLType::DOUYIN_USER:
        // Y24.16: yt-dlp has no DouyinUserIE — douyin.com/user/<sec_uid> can't be listed.
        //   'a'/'/' reject Douyin user URLs up front, so this is only reachable from legacy
        //   DB rows. Surface a clear error instead of silently retrying into emptiness.
        EVENT_LOG("T: Douyin user video list unsupported (yt-dlp has no DouyinUserIE) — delete the "
                  "node and use a Douyin video URL");
        result = std::make_shared<TreeNode>();
        result->url = cur_url;
        result->type = NodeType::PODCAST_FEED;
        result->title = node->title;
        result->children_loaded = true; // empty — stops the loading spinner
        break;
    case URLType::TIKTOK_USER: {
        // Y24.11: TikTok creator video listing via yt-dlp --flat-playlist + geo-bypass.
        std::string region = tiktok_region_;
        std::string cookies = IniConfig::instance().get_tiktok_cookies_file();
        result = parse_tiktok_user_videos(cur_url, region, cookies, node->title);
        break;
    }
    default: {
        std::string data = Network::fetch(cur_url);
        if (!data.empty())
            result = RSSParser::parse(data, cur_url);
        break;
    }
    }

    return result;
}

// YouTube cache miss: parse via ParserRegistry and backfill YouTubeCache with the parsed videos.
void App::cache_youtube_videos(TreeNodePtr node, URLType cur_type, const std::string &cur_url,
                               TreeNodePtr &result) {
    if (auto p = ParserRegistry::instance().create(cur_type)) {
        result = p->parse({"", cur_url});
        // BUG5: persist the parsed result into the YouTube cache for next time.
        if (result && !result->children.empty()) {
            std::vector<YouTubeVideoInfo> videos;
            videos.reserve(result->children.size());
            for (const auto &child : result->children) {
                if (child->type != NodeType::PODCAST_EPISODE)
                    continue;
                // Extract video id from watch?v=ID URL (best-effort; empty if not found).
                std::string id;
                size_t vp = child->url.find("v=");
                if (vp != std::string::npos) {
                    size_t start = vp + 2;
                    size_t end = child->url.find_first_of("&", start);
                    id = child->url.substr(start, end == std::string::npos ? std::string::npos
                                                                           : end - start);
                }
                videos.push_back({id, child->title, child->url});
            }
            std::string ch_name = result->channel_name;
            if (ch_name.empty())
                ch_name = result->title;
            if (ch_name.empty())
                ch_name = node->title;
            // Y23.3: only cache a NON-EMPTY video list. A bare channel parses to TAB
            //   children (PODCAST_FEED), which the loop above skips → videos empty. Caching
            //   that empty list made the next load hit the cache with 0 videos → "No items
            //   found" → ✗. Bare channels (tabs) are left uncached so they re-parse each time.
            if (!videos.empty()) {
                YouTubeCache::instance().update(cur_url, ch_name, videos);
            }
        }
    }
}

// Commit the parsed result: write children/error state under tree_mutex, then persist
//   episode_cache + full-tree cache OUTSIDE the lock (per-episode DB writes would block UI draw/flatten).
void App::commit_feed_result(TreeNodePtr node, TreeNodePtr result, const std::string &cur_url) {
    // Build the episode snapshot outside the lock first (result->children is local to this task, no lock needed),
    //   to avoid keeping the time-consuming per-episode DB writes inside the lock and blocking UI draw()/flatten().
    bool has_children = result && !result->children.empty();
    json episodes_json = json::array();
    if (has_children) {
        for (const auto &child : result->children) {
            episodes_json.push_back({
                {"url", child->url},
                {"title", child->title},
                {"duration", child->duration},
                {"is_youtube", child->is_youtube},
                {"has_subtitle", child->has_subtitle}, // Y23.10: persist 📜 flag
                {"subtitle_url", child->subtitle_url}  // Y23.10: persist transcript URL
            });
        }
    }

    // Hold the lock only for tree-structure changes + lightweight subscription persistence (subscription list is small, fast),
    //   ensuring the loading state and draw()'s is_loading detection stay in sync.
    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        if (result) {
            if (has_children) {
                node->children = result->children;
                for (auto &c : node->children)
                    c->parent = node; // BUG1: reset parent (result was a temporary)
                node->children_loaded = true;
                node->expanded = true; // auto-expand to show the loaded content
                node->parse_failed = false;
                node->is_cached = true;
                if (result->is_youtube)
                    node->is_youtube = true;
                EVENT_LOG(fmt::format("Loaded: {} items", result->children.size()));
                // Y24: mark the feed node with 📜 when ANY of its episodes has a transcript,
                //   so users can spot transcript-bearing feeds in the collapsed subscription
                //   list. (Y23.10 used "all", which left feeds like DOAC — where most but not
                //   every episode has a transcript — unmarked.) Persisted via save_tree below.
                int sub_count = 0;
                for (const auto &c : node->children)
                    if (c->has_subtitle)
                        ++sub_count;
                node->has_subtitle = (sub_count > 0);
                if (sub_count > 0)
                    LOG(fmt::format("[RSS] feed '{}' marked 📜 ({} of {} episodes have transcript)",
                                    node->title, sub_count, node->children.size()));
                // P2-C13: the full-tree save_cache is moved OUTSIDE the lock (below) so it
                //   doesn't block UI draw()/flatten() on every feed load.
                // Save the subscription list to the database (ensure correct loading after restart)
                Persistence::save_data(podcast_root, fav_root);
            } else if (result->parse_failed) {
                node->parse_failed = true;
                node->error_msg = result->error_msg;
                EVENT_LOG(fmt::format("Parse failed: {}", result->error_msg));
            } else {
                node->parse_failed = true;
                node->error_msg = "No items found";
                EVENT_LOG("No items found in feed");
            }
        } else {
            node->parse_failed = true;
            node->error_msg = "Parser returned null";
            EVENT_LOG("Parser returned null");
        }
        // Set loading=false at the end to ensure correct state-update order
        node->loading = false;
        // Sync LINK node status (after the target finishes loading, sync LINK nodes referencing it)
        sync_link_node_status(node);
    } // release tree_mutex

    // Time-consuming DB writes outside the lock (episode cache: per-episode INSERT loop + json dump).
    //   Does not read the tree; does not block UI draw()/flatten().
    if (has_children) {
        node->is_cached = true; // parsed — episode_cache (saved below) is the persistence
        DatabaseManager::instance().save_episode_cache(cur_url, episodes_json.dump());
        // P2-C13: full-tree cache save outside the lock (best-effort; a concurrent tree
        //   mutation only makes the restart cache slightly stale, never corrupts live state).
        Persistence::save_cache(radio_root, podcast_root);
    }
}

// Built-in global popular podcasts; deleted subscriptions are not auto-restored on restart
void App::load_default_podcasts() {
    // DB query runs outside the lock to avoid blocking other lock-waiting threads while holding tree_mutex
    auto removed = DatabaseManager::instance().load_removed_defaults();
    std::lock_guard<std::recursive_mutex> lock(tree_mutex);
    struct BuiltinPodcast {
        const char *name;
        const char *url;
    };

    // Collect existing URLs to avoid duplicate additions
    std::set<std::string> existing_urls;
    for (const auto &child : podcast_root) {
        if (!child->url.empty()) {
            existing_urls.insert(child->url);
        }
    }

    // TOP200 Apple Podcasts popular podcasts (updated 2026-03-26)
    // Data source: Apple RSS Marketing Tools API
    // Categories: News, Comedy, True Crime, Business,
    //             Society & Culture, History, Sports,
    //             Health & Fitness, Education, Religion, Technology
    std::vector<BuiltinPodcast> defaults = {
        // ═══════════════════════════════════════════════════════════════════
        // [TOP1-20 popular podcasts]
        // ═══════════════════════════════════════════════════════════════════
        {"Call Her Daddy", "https://podcasts.apple.com/us/podcast/call-her-daddy/id1418960261"},
        {"The Joe Rogan Experience",
         "https://podcasts.apple.com/us/podcast/the-joe-rogan-experience/id360084272"},
        {"The Rest Is History",
         "https://podcasts.apple.com/us/podcast/the-rest-is-history/id1537788786"},
        {"Good Hang with Amy Poehler",
         "https://podcasts.apple.com/us/podcast/good-hang-with-amy-poehler/id1795483480"},
        {"The Mel Robbins Podcast",
         "https://podcasts.apple.com/us/podcast/the-mel-robbins-podcast/id1646101002"},
        {"The Daily", "https://podcasts.apple.com/us/podcast/the-daily/id1200361736"},
        {"The Diary Of A CEO", "https://podcasts.apple.com/us/podcast/"
                               "the-diary-of-a-ceo-with-steven-bartlett/id1291423644"},
        {"Crime Junkie", "https://podcasts.apple.com/us/podcast/crime-junkie/id1322200189"},
        {"The Weekly Show with Jon Stewart",
         "https://podcasts.apple.com/us/podcast/the-weekly-show-with-jon-stewart/id1583132133"},
        {"Dateline NBC", "https://podcasts.apple.com/us/podcast/dateline-nbc/id1464919521"},
        {"The Secret World of Roald Dahl",
         "https://podcasts.apple.com/us/podcast/the-secret-world-of-roald-dahl/id1868436905"},
        {"48 Hours", "https://podcasts.apple.com/us/podcast/48-hours/id965818306"},
        {"Love Trapped", "https://podcasts.apple.com/us/podcast/love-trapped/id1878220033"},
        {"Pardon My Take", "https://podcasts.apple.com/us/podcast/pardon-my-take/id1089022756"},
        {"Pod Save America", "https://podcasts.apple.com/us/podcast/pod-save-america/id1192761536"},
        {"20/20", "https://podcasts.apple.com/us/podcast/20-20/id987967575"},
        {"Morbid", "https://podcasts.apple.com/us/podcast/morbid/id1379959217"},
        {"The Ezra Klein Show",
         "https://podcasts.apple.com/us/podcast/the-ezra-klein-show/id1548604447"},
        {"The Bill Simmons Podcast",
         "https://podcasts.apple.com/us/podcast/the-bill-simmons-podcast/id1043699613"},
        {"SmartLess", "https://podcasts.apple.com/us/podcast/smartless/id1521578868"},

        // ═══════════════════════════════════════════════════════════════════
        // [TOP21-50 News & Politics]
        // ═══════════════════════════════════════════════════════════════════
        {"This Past Weekend w/ Theo Von",
         "https://podcasts.apple.com/us/podcast/this-past-weekend-w-theo-von/id1190981360"},
        {"Stuff You Should Know",
         "https://podcasts.apple.com/us/podcast/stuff-you-should-know/id278981407"},
        {"The Bulwark Podcast",
         "https://podcasts.apple.com/us/podcast/the-bulwark-podcast/id1447684472"},
        {"The Shawn Ryan Show",
         "https://podcasts.apple.com/us/podcast/the-shawn-ryan-show/id1492492083"},
        {"The MeidasTouch Podcast",
         "https://podcasts.apple.com/us/podcast/the-meidastouch-podcast/id1510240831"},
        {"The Toast", "https://podcasts.apple.com/us/podcast/the-toast/id1368081567"},
        {"Trace of Suspicion",
         "https://podcasts.apple.com/us/podcast/trace-of-suspicion/id1880711858"},
        {"Up First from NPR",
         "https://podcasts.apple.com/us/podcast/up-first-from-npr/id1222114325"},
        {"Candace", "https://podcasts.apple.com/us/podcast/candace/id1750591415"},
        {"The Megyn Kelly Show",
         "https://podcasts.apple.com/us/podcast/the-megyn-kelly-show/id1532976305"},
        {"Armchair Expert",
         "https://podcasts.apple.com/us/podcast/armchair-expert-with-dax-shepard/id1345682353"},
        {"Giggly Squad", "https://podcasts.apple.com/us/podcast/giggly-squad/id1536352412"},
        {"Real Vikings", "https://podcasts.apple.com/us/podcast/real-vikings/id1876255143"},
        {"Betrayal Season 5",
         "https://podcasts.apple.com/us/podcast/betrayal-season-5/id1615637724"},
        {"My Favorite Murder",
         "https://podcasts.apple.com/us/podcast/my-favorite-murder/id1074507850"},
        {"The Rest Is Politics",
         "https://podcasts.apple.com/us/podcast/the-rest-is-politics/id1611374685"},
        {"The Ben Shapiro Show",
         "https://podcasts.apple.com/us/podcast/the-ben-shapiro-show/id1047335260"},
        {"Bridge of Lies", "https://podcasts.apple.com/us/podcast/bridge-of-lies/id1874641982"},
        {"Global News Podcast",
         "https://podcasts.apple.com/us/podcast/global-news-podcast/id135067274"},
        {"MrBallen Podcast", "https://podcasts.apple.com/us/podcast/mrballen-podcast/id1608813794"},
        {"The Tucker Carlson Show",
         "https://podcasts.apple.com/us/podcast/the-tucker-carlson-show/id1719657632"},
        {"Conan O'Brien Needs A Friend",
         "https://podcasts.apple.com/us/podcast/conan-obrien-needs-a-friend/id1438054347"},
        {"The Rest Is Politics: US",
         "https://podcasts.apple.com/us/podcast/the-rest-is-politics-us/id1743030473"},
        {"The Bible in a Year",
         "https://podcasts.apple.com/us/podcast/the-bible-in-a-year/id1539568321"},
        {"The Ramsey Show", "https://podcasts.apple.com/us/podcast/the-ramsey-show/id77001367"},
        {"REAL AF with Andy Frisella",
         "https://podcasts.apple.com/us/podcast/real-af-with-andy-frisella/id1012570406"},
        {"The Deck", "https://podcasts.apple.com/us/podcast/the-deck/id1603011962"},
        {"Huberman Lab", "https://podcasts.apple.com/us/podcast/huberman-lab/id1545953110"},
        {"The Dan Le Batard Show",
         "https://podcasts.apple.com/us/podcast/the-dan-le-batard-show/id934820588"},
        {"This American Life",
         "https://podcasts.apple.com/us/podcast/this-american-life/id201671138"},

        // ═══════════════════════════════════════════════════════════════════
        // [TOP51-100 more popular]
        // ═══════════════════════════════════════════════════════════════════
        {"Mick Unplugged", "https://podcasts.apple.com/us/podcast/mick-unplugged/id1731755953"},
        {"Matt and Shane's Secret Podcast",
         "https://podcasts.apple.com/us/podcast/matt-and-shanes-secret-podcast/id1177068388"},
        {"Off Duty | The Guardian", "https://podcasts.apple.com/us/podcast/off-duty/id1731314182"},
        {"The News Agents", "https://podcasts.apple.com/us/podcast/the-news-agents/id1640878689"},
        {"The Louis Theroux Podcast",
         "https://podcasts.apple.com/us/podcast/the-louis-theroux-podcast/id1725833532"},
        {"The Romesh Ranganathan Show",
         "https://podcasts.apple.com/us/podcast/the-romesh-ranganathan-show/id1838594107"},
        {"Snapped: Women Who Murder",
         "https://podcasts.apple.com/us/podcast/snapped-women-who-murder/id1145089790"},
        {"Pivot", "https://podcasts.apple.com/us/podcast/pivot/id1073226719"},
        {"The Rest Is Entertainment",
         "https://podcasts.apple.com/us/podcast/the-rest-is-entertainment/id1718287198"},
        {"Parenting Hell", "https://podcasts.apple.com/us/podcast/parenting-hell/id1510251497"},
        {"Page 94: The Private Eye Podcast",
         "https://podcasts.apple.com/us/podcast/page-94/id973958702"},
        {"The Dan Bongino Show",
         "https://podcasts.apple.com/us/podcast/the-dan-bongino-show/id965293227"},
        {"The Rest Is Science",
         "https://podcasts.apple.com/us/podcast/the-rest-is-science/id1853007888"},
        {"Newscast", "https://podcasts.apple.com/us/podcast/newscast/id1234185718"},
        {"The Book Club", "https://podcasts.apple.com/us/podcast/the-book-club/id1876049295"},
        {"Off Menu", "https://podcasts.apple.com/us/podcast/off-menu/id1442950743"},
        {"Freakonomics Radio",
         "https://podcasts.apple.com/us/podcast/freakonomics-radio/id354668519"},
        {"Hanging Out With Ant & Dec",
         "https://podcasts.apple.com/us/podcast/hanging-out/id1867521360"},
        {"Dish", "https://podcasts.apple.com/us/podcast/dish/id1626354833"},
        {"Morning Wire", "https://podcasts.apple.com/us/podcast/morning-wire/id1576594336"},
        {"Sh**ged Married Annoyed",
         "https://podcasts.apple.com/us/podcast/sh-ged-married-annoyed/id1451489585"},
        {"No Such Thing As A Fish",
         "https://podcasts.apple.com/us/podcast/no-such-thing-as-a-fish/id840986946"},
        {"NPR News Now", "https://podcasts.apple.com/us/podcast/npr-news-now/id121493675"},
        {"Foundling | Tortoise", "https://podcasts.apple.com/us/podcast/foundling/id1590561275"},
        {"Elis James and John Robins",
         "https://podcasts.apple.com/us/podcast/elis-james-john-robins/id1465290044"},
        {"Off Air with Jane & Fi", "https://podcasts.apple.com/us/podcast/off-air/id1648663774"},
        {"The Joe Budden Podcast",
         "https://podcasts.apple.com/us/podcast/the-joe-budden-podcast/id1535809341"},
        {"Wolf & Owl", "https://podcasts.apple.com/us/podcast/wolf-owl/id1540826523"},
        {"Focus: Adults in the Room", "https://podcasts.apple.com/us/podcast/focus/id1733735613"},
        {"The Idiot", "https://podcasts.apple.com/us/podcast/the-idiot/id1884735227"},
        {"Happy Place", "https://podcasts.apple.com/us/podcast/happy-place/id1353058891"},
        {"The Dylan Gemelli Podcast",
         "https://podcasts.apple.com/us/podcast/the-dylan-gemelli/id1780873400"},
        {"We Need To Talk", "https://podcasts.apple.com/us/podcast/we-need-to-talk/id1765126946"},
        {"The Viall Files", "https://podcasts.apple.com/us/podcast/the-viall-files/id1448210981"},
        {"Fin vs History", "https://podcasts.apple.com/us/podcast/fin-vs-history/id1790458615"},
        {"My Therapist Ghosted Me",
         "https://podcasts.apple.com/us/podcast/my-therapist-ghosted-me/id1560176558"},
        {"Last Podcast On The Left",
         "https://podcasts.apple.com/us/podcast/last-podcast-on-the-left/id437299706"},
        {"Today in Focus", "https://podcasts.apple.com/us/podcast/today-in-focus/id1440133626"},
        {"RedHanded", "https://podcasts.apple.com/us/podcast/redhanded/id1250599915"},
        {"The Rest Is Classified",
         "https://podcasts.apple.com/us/podcast/the-rest-is-classified/id1780384916"},
        {"Rosebud with Gyles Brandreth",
         "https://podcasts.apple.com/us/podcast/rosebud/id1704806594"},
        {"Staying Relevant", "https://podcasts.apple.com/us/podcast/staying-relevant/id1651140064"},
        {"Unblinded with Sean Callagy",
         "https://podcasts.apple.com/us/podcast/unblinded/id1844970260"},
        {"WW2 Pod: We Have Ways", "https://podcasts.apple.com/us/podcast/ww2-pod/id1457552694"},
        {"Park Predators", "https://podcasts.apple.com/us/podcast/park-predators/id1517651197"},
        {"Pod Save the World",
         "https://podcasts.apple.com/us/podcast/pod-save-the-world/id1200016351"},
        {"Short History Of...",
         "https://podcasts.apple.com/us/podcast/short-history-of/id1579040306"},
        {"Last One Laughing Podcast",
         "https://podcasts.apple.com/us/podcast/last-one-laughing/id1885620207"},
        {"Fresh Air", "https://podcasts.apple.com/us/podcast/fresh-air/id214089682"},
        {"Football Weekly", "https://podcasts.apple.com/us/podcast/football-weekly/id188674007"},

        // ═══════════════════════════════════════════════════════════════════
        // [TOP101-150 News & Business]
        // ═══════════════════════════════════════════════════════════════════
        {"IHIP News", "https://podcasts.apple.com/us/podcast/ihip-news/id1761444284"},
        {"Empire: World History", "https://podcasts.apple.com/us/podcast/empire/id1639561921"},
        {"LuAnna: The Podcast", "https://podcasts.apple.com/us/podcast/luanna/id1496019465"},
        {"Money Rehab with Nicole Lapin",
         "https://podcasts.apple.com/us/podcast/money-rehab/id1559564016"},
        {"British Scandal", "https://podcasts.apple.com/us/podcast/british-scandal/id1563775446"},
        {"Dig It with Jo Whiley", "https://podcasts.apple.com/us/podcast/dig-it/id1825368127"},
        {"Alan Carr's Life's a Beach",
         "https://podcasts.apple.com/us/podcast/lifes-a-beach/id1550998864"},
        {"Habits and Hustle",
         "https://podcasts.apple.com/us/podcast/habits-and-hustle/id1451897026"},
        {"The Rest Is Football",
         "https://podcasts.apple.com/us/podcast/the-rest-is-football/id1701022490"},
        {"Young and Profiting",
         "https://podcasts.apple.com/us/podcast/young-profiting/id1368888880"},
        {"Americast", "https://podcasts.apple.com/us/podcast/americast/id1473150244"},
        {"Proven Podcast", "https://podcasts.apple.com/us/podcast/proven-podcast/id1744386875"},
        {"Stick to Football",
         "https://podcasts.apple.com/us/podcast/stick-to-football/id1709142395"},
        {"Help I Sexted My Boss",
         "https://podcasts.apple.com/us/podcast/help-i-sexted/id1357127065"},
        {"Fraudacious", "https://podcasts.apple.com/us/podcast/fraudacious/id1879610796"},
        {"FT News Briefing", "https://podcasts.apple.com/us/podcast/ft-news-briefing/id1438449989"},
        {"The Matt Walsh Show",
         "https://podcasts.apple.com/us/podcast/the-matt-walsh-show/id1367210511"},
        {"Great Company with Jamie Laing",
         "https://podcasts.apple.com/us/podcast/great-company/id1735702250"},
        {"Spittin Chiclets", "https://podcasts.apple.com/us/podcast/spittin-chiclets/id1112425552"},
        {"Three Bean Salad", "https://podcasts.apple.com/us/podcast/three-bean-salad/id1564066507"},
        {"Casefile True Crime",
         "https://podcasts.apple.com/us/podcast/casefile-true-crime/id998568017"},
        {"Raging Moderates", "https://podcasts.apple.com/us/podcast/raging-moderates/id1774505095"},
        {"Front Burner", "https://podcasts.apple.com/us/podcast/front-burner/id1439621628"},
        {"Dan Snow's History Hit",
         "https://podcasts.apple.com/us/podcast/history-hit/id1042631089"},
        {"Someone Knows Something",
         "https://podcasts.apple.com/us/podcast/someone-knows-something/id1089216339"},
        {"The Vault Unlocked",
         "https://podcasts.apple.com/us/podcast/the-vault-unlocked/id1837193185"},
        {"The Archers", "https://podcasts.apple.com/us/podcast/the-archers/id265970428"},
        {"OverDrive", "https://podcasts.apple.com/us/podcast/overdrive/id679367618"},
        {"Desert Island Discs",
         "https://podcasts.apple.com/us/podcast/desert-island-discs/id342735925"},
        {"All-In Podcast", "https://podcasts.apple.com/us/podcast/all-in/id1502871393"},
        {"The Rewatchables", "https://podcasts.apple.com/us/podcast/the-rewatchables/id1268527882"},
        {"Joe Marler Will See You Now",
         "https://podcasts.apple.com/us/podcast/joe-marler/id1850736713"},
        {"The Therapy Crouch",
         "https://podcasts.apple.com/us/podcast/the-therapy-crouch/id1665665408"},
        {"Bad Friends", "https://podcasts.apple.com/us/podcast/bad-friends/id1496265971"},
        {"Feel Better, Live More",
         "https://podcasts.apple.com/us/podcast/feel-better-live-more/id1333552422"},
        {"Watch What Crappens",
         "https://podcasts.apple.com/us/podcast/watch-what-crappens/id498130432"},
        {"That Peter Crouch Podcast",
         "https://podcasts.apple.com/us/podcast/that-peter-crouch/id1616744464"},
        {"The Bridge with Peter Mansbridge",
         "https://podcasts.apple.com/us/podcast/the-bridge/id1478036186"},
        {"World Report", "https://podcasts.apple.com/us/podcast/world-report/id278657031"},
        {"Chatabix", "https://podcasts.apple.com/us/podcast/chatabix/id1560965008"},
        {"The Cult Queen of Canada",
         "https://podcasts.apple.com/us/podcast/cult-queen/id1364665348"},
        {"The Rest Is Politics: Leading",
         "https://podcasts.apple.com/us/podcast/rest-is-politics-leading/id1665265193"},
        {"The Journal", "https://podcasts.apple.com/us/podcast/the-journal/id1469394914"},
        {"Begin Again with Davina McCall",
         "https://podcasts.apple.com/us/podcast/begin-again/id1773104705"},
        {"The Bible Recap", "https://podcasts.apple.com/us/podcast/the-bible-recap/id1440833267"},
        {"Museum of Pop Culture",
         "https://podcasts.apple.com/us/podcast/museum-pop-culture/id1863943807"},
        {"Frank Skinner Podcast",
         "https://podcasts.apple.com/us/podcast/frank-skinner/id308800732"},
        {"Adam Carolla Show", "https://podcasts.apple.com/us/podcast/adam-carolla/id306390087"},
        {"ZOE Science & Nutrition",
         "https://podcasts.apple.com/us/podcast/zoe-science-nutrition/id1611216298"},
        {"Blair & Barker", "https://podcasts.apple.com/us/podcast/blair-barker/id541972447"},

        // ═══════════════════════════════════════════════════════════════════
        // [TOP151-200 more popular podcasts]
        // ═══════════════════════════════════════════════════════════════════
        {"Digital Social Hour",
         "https://podcasts.apple.com/us/podcast/digital-social-hour/id1676846015"},
        {"The Daily Show: Ears Edition",
         "https://podcasts.apple.com/us/podcast/the-daily-show/id1334878780"},
        {"Table Manners", "https://podcasts.apple.com/us/podcast/table-manners/id1305228910"},
        {"32 Thoughts: The Podcast",
         "https://podcasts.apple.com/us/podcast/32-thoughts/id1332150124"},
        {"How To Fail With Elizabeth Day",
         "https://podcasts.apple.com/us/podcast/how-to-fail/id1407451189"},
        {"The Determined Society",
         "https://podcasts.apple.com/us/podcast/determined-society/id1555922064"},
        {"Real Kyper & Bourne",
         "https://podcasts.apple.com/us/podcast/real-kyper-bourne/id1588452517"},
        {"What Did You Do Yesterday?",
         "https://podcasts.apple.com/us/podcast/what-did-you-do/id1765600990"},
        {"On Purpose with Jay Shetty",
         "https://podcasts.apple.com/us/podcast/on-purpose/id1450994021"},
        {"Canadian True Crime",
         "https://podcasts.apple.com/us/podcast/canadian-true-crime/id1197095887"},
        {"The Learning Leader Show",
         "https://podcasts.apple.com/us/podcast/learning-leader/id985396258"},
        {"ill-advised by Bill Nighy",
         "https://podcasts.apple.com/us/podcast/ill-advised/id1842190360"},
        {"Nothing much happens",
         "https://podcasts.apple.com/us/podcast/nothing-much-happens/id1378040733"},
        {"The High Performance Podcast",
         "https://podcasts.apple.com/us/podcast/high-performance/id1500444735"},
        {"Hidden Brain", "https://podcasts.apple.com/us/podcast/hidden-brain/id1028908750"},
        {"Serial", "https://podcasts.apple.com/us/podcast/serial/id917918570"},
        {"The Daily T", "https://podcasts.apple.com/us/podcast/the-daily-t/id1489612924"},
        {"Get Birding", "https://podcasts.apple.com/us/podcast/get-birding/id1551111133"},
        {"What's My Age Again?",
         "https://podcasts.apple.com/us/podcast/whats-my-age-again/id1806655079"},
        {"Football Daily", "https://podcasts.apple.com/us/podcast/football-daily/id261291929"},
        {"Mike Ward Sous Écoute", "https://podcasts.apple.com/us/podcast/mike-ward/id1053196322"},
        {"Your World Tonight",
         "https://podcasts.apple.com/us/podcast/your-world-tonight/id250083757"},
        {"The Jamie Kern Lima Show",
         "https://podcasts.apple.com/us/podcast/jamie-kern-lima/id1728723635"},
        {"Crime Next Door", "https://podcasts.apple.com/us/podcast/crime-next-door/id1744545000"},
        {"Power & Politics", "https://podcasts.apple.com/us/podcast/power-politics/id337361397"},
        {"Creating Confidence",
         "https://podcasts.apple.com/us/podcast/creating-confidence/id1462192400"},
        {"Football Ramble", "https://podcasts.apple.com/us/podcast/football-ramble/id254078311"},
        {"The Money Mondays",
         "https://podcasts.apple.com/us/podcast/the-money-mondays/id1664983297"},
        {"Strangers on a Bench",
         "https://podcasts.apple.com/us/podcast/strangers-bench/id1770745380"},
        {"Smosh Reads Reddit Stories",
         "https://podcasts.apple.com/us/podcast/smosh-reads/id1697425543"},
        {"Sword and Scale", "https://podcasts.apple.com/us/podcast/sword-and-scale/id790487079"},
        {"Amanda Knox Hosts | DOUBT", "https://podcasts.apple.com/us/podcast/doubt/id1877870463"},
        {"The Prof G Pod", "https://podcasts.apple.com/us/podcast/prof-g-pod/id1498802610"},
        {"The NPR Politics Podcast",
         "https://podcasts.apple.com/us/podcast/npr-politics/id1057255460"},
        {"The Ancients", "https://podcasts.apple.com/us/podcast/the-ancients/id1520403988"},
        {"The Basement Yard",
         "https://podcasts.apple.com/us/podcast/the-basement-yard/id1034354283"},
        {"You're Dead to Me",
         "https://podcasts.apple.com/us/podcast/youre-dead-to-me/id1479973402"},
        {"Ologies with Alie Ward", "https://podcasts.apple.com/us/podcast/ologies/id1278815517"},
        {"Breaking Points", "https://podcasts.apple.com/us/podcast/breaking-points/id1570045623"},
        {"The David Frum Show",
         "https://podcasts.apple.com/us/podcast/david-frum-show/id1305908387"},
        {"Modern Wisdom", "https://podcasts.apple.com/us/podcast/modern-wisdom/id1347973549"},
        {"Two Blocks from the White House",
         "https://podcasts.apple.com/us/podcast/two-blocks/id1866939902"},
        {"Today, Explained", "https://podcasts.apple.com/us/podcast/today-explained/id1346207297"},
        {"Passion Struck", "https://podcasts.apple.com/us/podcast/passion-struck/id1553279283"},
        {"Joe and James Fact Up",
         "https://podcasts.apple.com/us/podcast/joe-james-fact-up/id1838423659"},
        {"Prof G Markets", "https://podcasts.apple.com/us/podcast/prof-g-markets/id1744631325"},
        {"The Decibel", "https://podcasts.apple.com/us/podcast/the-decibel/id1565410296"},
        {"The Martin Lewis Podcast",
         "https://podcasts.apple.com/us/podcast/martin-lewis/id520802069"},
        {"3 Takeaways", "https://podcasts.apple.com/us/podcast/3-takeaways/id1526080983"},
        {"The Good, The Bad & The Football",
         "https://podcasts.apple.com/us/podcast/good-bad-football/id1839425706"},
        {"followHIM", "https://podcasts.apple.com/us/podcast/followhim/id1545433056"},
        {"Behind the Bastards",
         "https://podcasts.apple.com/us/podcast/behind-the-bastards/id1373812661"},

        // ═══════════════════════════════════════════════════════════════════
        // [YouTube channels]
        // ═══════════════════════════════════════════════════════════════════
        {"56BelowTV (YouTube)", "https://www.youtube.com/@56BelowTV"},
    };

    // Load the user's deleted built-in podcast records to avoid re-adding them on restart
    //   (the original code only looked at current subscriptions; after deletion, restart would always revive them, contrary to the comment's promise)
    // removed was queried at the function entry (outside the lock)

    for (auto &p : defaults) {
        // Skip already-existing podcasts to avoid duplicates
        if (existing_urls.count(p.url))
            continue;
        // Skip built-in podcasts the user has deleted
        if (removed.count(p.url))
            continue;

        auto node = std::make_shared<TreeNode>();
        node->title = p.name;
        node->url = p.url;
        node->type = NodeType::PODCAST_FEED;
        // Initially show the subscription, but the episode list is not loaded
        // Parsing is triggered only on enter (expand)
        node->children_loaded = false;
        node->expanded = false;
        node->parent.reset(); // set parent pointer
        // Mark YouTube channels
        if (std::string(p.url).find("youtube.com") != std::string::npos) {
            node->is_youtube = true;
        }
        podcast_root.push_back(node);
    }
    podcast_loaded = true;
}

void App::flatten(const TreeNodePtr &node, int depth, bool is_last, int parent_idx) {
    // E: pure subtree recursion — always pushes the node. The caller passes only displayable
    //   items (the per-mode root is never passed), so the old `title=="Root"` magic is gone.
    int current_idx = display_list.size();
    display_list.push_back({node, depth, is_last, parent_idx});
    if ((node->type == NodeType::FOLDER || node->type == NodeType::PODCAST_FEED) &&
        node->expanded) {
        int count = node->children.size();
        for (int i = 0; i < count; ++i) {
            flatten(node->children[i], depth + 1, i == count - 1, current_idx);
        }
    }
}

// E: flatten the current mode's top-level item list (the root is not in the display domain).
void App::flatten_items(const std::vector<TreeNodePtr> &tops) {
    int count = (int)tops.size();
    for (int i = 0; i < count; ++i) {
        flatten(tops[i], 0, i == count - 1, -1);
    }
}

// E: the active mode's top-level items. (Step 1 bridge: backed by the per-mode root's children;
//   Step 2 migrates ownership to dedicated vectors and removes the root nodes.)
std::vector<TreeNodePtr> &App::items_for_mode(AppMode m) {
    switch (m) {
    case AppMode::RADIO:
        return radio_root;
    case AppMode::PODCAST:
        return podcast_root;
    case AppMode::FAVOURITE:
        return fav_root;
    case AppMode::HISTORY:
        return history_root;
    case AppMode::ONLINE:
        return OnlineState::instance().online_root->children;
    case AppMode::ACCOUNT:
        return account_root;
    case AppMode::BILIBILI:
        return bilibili_root;
    case AppMode::TIKTOK:
        return tiktok_root;
    case AppMode::IPTV:
        return iptv_root;
    }
    return radio_root;
}

std::vector<TreeNodePtr> &App::cur_items() {
    return items_for_mode(mode);
}

} // namespace panicast
