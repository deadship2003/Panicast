#include "panicast/app/app.h"

namespace panicast
{

void App::download_node(int marked_count) {
    std::vector<TreeNodePtr> items;
    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        if (marked_count > 0)
            collect_playable_marked_current(items);
        else if (selected_idx < (int)display_list.size()) {
            auto n = display_list[selected_idx].node;
            if (n->type == NodeType::RADIO_STREAM || n->type == NodeType::PODCAST_EPISODE)
                items.push_back(n);
        }
    }

    if (items.empty()) {
        EVENT_LOG("No downloadable items");
        return;
    }

    EVENT_LOG(fmt::format("Downloading {} items...", items.size()));
    // Enqueue all items; start_one_download is throttled to MAX_CONCURRENT_DOWNLOADS slots
    //   so the download list never overflows the INFO panel. Pending items are promoted as
    //   slots free (pump_download_queue is also called from the render loop).
    for (auto &n : items)
        pending_downloads_.push_back(n);
    pump_download_queue(ProgressManager::instance().get_all().size());

    {
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        clear_marks_current();
    }
    Persistence::save_cache(radio_root, podcast_root);
}

// Start one download for node n. Returns true only if a NEW ProgressManager slot was created
//   (NEW); REUSED_ACTIVE and RESET_EXISTING reuse an existing slot and return false, so the
//   throttle counter in pump_download_queue is not double-counted.
// Y24.49: shared yt-dlp download core — run yt-dlp with progress parsing, verify, and cache the
//   result. Used by the YouTube and Bilibili/TikTok/Douyin video branches (DRY). `site_args` is the
//   site-specific prefix (cookies / player_client / js_runtime); the common -f / -o / --progress /
//   url args are appended here.
void App::ytdlp_download(const std::string &url, const std::vector<std::string> &site_args,
                         const std::string &dir, const std::string &base_name,
                         const std::string &title, const std::string &dl_id, TreeNodePtr n) {
    EVENT_LOG(fmt::format("yt-dlp DL: {}", title));
    ProgressManager::instance().update(dl_id, 5, "Fetching info...", "...", 0, 0, 0);

    std::string output_template = fmt::format("{}/{}.mp4", dir, base_name);
    LOG(fmt::format("[yt-dlp DL] launching for: {}", title));

    std::vector<std::string> args = site_args;
    args.push_back("-f");
    args.push_back("bestvideo+bestaudio/best");
    args.push_back("--merge-output-format");
    args.push_back("mp4");
    args.push_back("-o");
    args.push_back(output_template);
    args.push_back("--no-warnings");
    args.push_back("--newline");
    args.push_back("--progress");
    args.push_back("--no-playlist");
    args.push_back("--continue"); // resume from breakpoint (continue .part)
    args.push_back("--retries");
    args.push_back("20");
    args.push_back("--fragment-retries");
    args.push_back("20");
    args.push_back(url);

    {
        std::string args_dbg;
        for (auto &a : args)
            args_dbg += a + " ";
        LOG(fmt::format("[yt-dlp DL] args: {}", args_dbg));
    }
    auto result = YtdlpRunner::run(
        args,
        [&](const std::string &raw_line) {
            std::string line = raw_line;
            // Parse yt-dlp progress output
            if (line.find("[download]") != std::string::npos) {
                size_t pct_pos = line.find('%');
                if (pct_pos != std::string::npos) {
                    size_t num_start = pct_pos;
                    while (num_start > 0 &&
                           (isdigit(line[num_start - 1]) || line[num_start - 1] == '.')) {
                        num_start--;
                    }
                    std::string pct_str = line.substr(num_start, pct_pos - num_start);
                    try {
                        int percent = static_cast<int>(std::stod(pct_str));
                        if (percent > 100)
                            percent = 100;

                        std::string speed = "...";
                        size_t speed_pos = line.find(" at ");
                        if (speed_pos != std::string::npos) {
                            size_t speed_end = line.find(" ETA", speed_pos);
                            if (speed_end != std::string::npos && speed_end > speed_pos + 4) {
                                speed = line.substr(speed_pos + 4, speed_end - speed_pos - 4);
                                while (!speed.empty() && isspace(speed.front()))
                                    speed.erase(0, 1);
                                while (!speed.empty() && isspace(speed.back()))
                                    speed.pop_back();
                            }
                        }

                        int eta_seconds = 0;
                        size_t eta_pos = line.find("ETA");
                        if (eta_pos != std::string::npos) {
                            std::string eta_str = line.substr(eta_pos + 4);
                            while (!eta_str.empty() && isspace(eta_str.front()))
                                eta_str.erase(0, 1);
                            while (!eta_str.empty() &&
                                   (isspace(eta_str.back()) || eta_str.back() == '\n' ||
                                    eta_str.back() == '\r')) {
                                eta_str.pop_back();
                            }
                            int h = 0, m = 0, s = 0;
                            if (sscanf(eta_str.c_str(), "%d:%d:%d", &h, &m, &s) == 3) {
                                eta_seconds = h * 3600 + m * 60 + s;
                            } else if (sscanf(eta_str.c_str(), "%d:%d", &m, &s) == 2) {
                                eta_seconds = m * 60 + s;
                            }
                        }

                        ProgressManager::instance().update(dl_id, percent, "Downloading", speed,
                                                           eta_seconds, 0, 0);
                    } catch (const std::exception &e) {
                        LOG(fmt::format("[Exception] {}", e.what()));
                    }
                }
            } else if (line.find("[Merger]") != std::string::npos) {
                ProgressManager::instance().update(dl_id, 95, "Merging...", "...", 0, 0, 0);
            } else if (line.find("has already") != std::string::npos) {
                ProgressManager::instance().update(dl_id, 100, "Already exists", "", 0, 0, 0);
            }
        },
        3600 // Y24.49: generous timeout for long video downloads (was default 600s)
    );
    // yt-dlp exit code alone is unreliable: it can exit 0 yet produce no/invalid file. Verify
    //   the actual output file matches the episode info before counting as success.
    std::string local_file = dir + "/" + base_name + ".mp4";
    LOG(fmt::format("[yt-dlp DL] launched={}, exit_code={}", result.launched, result.exit_code));
    if (!result.stderr_output.empty()) {
        std::string err_tail = result.stderr_output;
        size_t pos = err_tail.find_last_of('\n');
        if (pos != std::string::npos && pos > 0) {
            size_t prev = err_tail.find_last_of('\n', pos - 1);
            err_tail = err_tail.substr(prev == std::string::npos ? 0 : prev + 1);
        }
        LOG(fmt::format("[yt-dlp DL] stderr: {}", err_tail));
    }
    bool success = false;
    if (result.launched && result.exit_code == 0) {
        auto vr = verify_downloaded_file(local_file, n->duration);
        if (!vr.ok) {
            success = false;
            EVENT_LOG(fmt::format("Verify failed ({}): {}", title, vr.reason));
        } else {
            success = true;
        }
    } else {
        success = false;
    }

    ProgressManager::instance().complete(dl_id, success);
    {
        // Writing node fields and serializing the tree both require holding tree_mutex
        std::lock_guard<std::recursive_mutex> lock(tree_mutex);
        if (success) {
            CacheManager::instance().mark_downloaded(url, local_file);
            n->is_downloaded = true;
            n->local_file = local_file;
            // Y24.7: download subtitle/transcript sidecar alongside the episode
            subtitle_mgr_.download_sidecar(n, local_file, pool_);
            EVENT_LOG(fmt::format("Saved: {}.mp4", base_name));
        } else {
            CacheManager::instance().mark_partial(url); // .part left → mark incomplete
            EVENT_LOG(fmt::format("Failed: {}", title));
        }
        Persistence::save_cache(radio_root, podcast_root);
    }
}

bool App::start_one_download(TreeNodePtr n) {
    std::string dir = Utils::get_download_dir();
    fs::create_directories(dir);

    // Already downloaded & cached? Don't re-download unless the file is missing/incomplete
    //   (in which case fall through and resume/re-download). is_downloaded is set only after a
    //   previously verified-successful download, so a present valid file means it's complete.
    bool already = n->is_downloaded || CacheManager::instance().is_downloaded(n->url);
    std::string existing = n->local_file;
    if (existing.empty())
        existing = CacheManager::instance().get_local_file(n->url);
    if (already && !existing.empty()) {
        auto vr = verify_downloaded_file(existing, n->duration);
        if (vr.ok) {
            EVENT_LOG(fmt::format("Already downloaded, skip: {}", n->title));
            n->is_downloaded = true;
            n->local_file = existing;
            return false; // nothing to do — no slot, no task
        }
        // Cached file unusable (deleted/truncated/corrupt) → re-download; resume if a partial exists.
        EVENT_LOG(
            fmt::format("Cached file unusable ({}), re-downloading: {}", vr.reason, n->title));
        n->is_downloaded = false;
    }

    std::string base_name = Utils::sanitize_filename(n->title);
    URLType url_type = URLClassifier::classify(n->url);
    bool is_youtube = URLClassifier::is_youtube(url_type);
    bool is_video_file = (url_type == URLType::VIDEO_FILE);

    ProgressManager::StartResult sr = ProgressManager::StartResult::NEW;
    std::string dl_id =
        ProgressManager::instance().start_download(n->title, n->url, is_youtube, &sr);
    if (sr == ProgressManager::StartResult::REUSED_ACTIVE) {
        EVENT_LOG(fmt::format("Already downloading, skip duplicate: {}", n->title));
        return false;
    }

    if (is_youtube) {
        // YouTube download (with progress parsing) — shared yt-dlp core (Y24.49).
        pool_.submit([this, url = n->url, base_name, dir, title = n->title, dl_id, n]() {
            ytdlp_download(url, YouTubeChannelParser::ytdlp_youtube_args(), dir, base_name, title,
                           dl_id, n);
        });
    } else if (url_type == URLType::BILIBILI_VIDEO || url_type == URLType::DOUYIN_VIDEO ||
               url_type == URLType::TIKTOK_VIDEO) {
        // Y24.49: Bilibili/TikTok/Douyin videos must go through yt-dlp (with site cookies) — a
        //   direct curl GET fetches the HTML watch page, not the media stream, so downloads failed.
        pool_.submit([this, url = n->url, base_name, dir, title = n->title, dl_id, n, url_type]() {
            std::vector<std::string> site;
            std::string cf = (url_type == URLType::BILIBILI_VIDEO)
                                 ? IniConfig::instance().get_bilibili_cookies_file()
                                 : IniConfig::instance().get_tiktok_cookies_file();
            std::error_code ec;
            if (!cf.empty() && fs::exists(cf, ec)) {
                site.push_back("--cookies");
                site.push_back(cf);
            }
            ytdlp_download(url, site, dir, base_name, title, dl_id, n);
        });
    } else {
        // Normal download (with progress callback)
        // Use thread pool (old code: std::thread t(...); t.detach();)
        pool_.submit([this, url = n->url, dir, base_name, title = n->title, dl_id, is_video_file,
                      n]() {
            std::string ext = is_video_file ? ".mp4" : ".mp3";
            size_t p = url.find_last_of('.');
            if (p != std::string::npos && p > url.find_last_of('/')) {
                std::string url_ext = url.substr(p);
                if (url_ext.length() <= 5 && url_ext.find("?") == std::string::npos)
                    ext = url_ext;
            }

            std::string filepath = dir + "/" + base_name + ext;
            EVENT_LOG(fmt::format("Downloading: {}", title));

            // Resume: if a partial file exists, resume from its size (not from the beginning).
            // P2-C12: but only if it's a plausible media prefix — a prior 200-with-garbage
            //   (HTML error page) must NOT be appended to. If the existing bytes don't look
            //   like media (e.g. start with '<' = HTML/XML, or are empty), truncate and restart.
            std::error_code ec_fs;
            curl_off_t resume_from = 0;
            if (fs::exists(filepath, ec_fs)) {
                auto fsize = fs::file_size(filepath, ec_fs);
                if (!ec_fs && fsize > 0) {
                    bool garbage = false;
                    {
                        std::ifstream head(filepath, std::ios::binary);
                        char c = 0;
                        if (head.read(&c, 1)) {
                            if (c == '<' || c == '{')
                                garbage = true; // HTML/XML/JSON error page
                        }
                    }
                    if (garbage) {
                        std::error_code rc;
                        fs::resize_file(filepath, 0, rc);
                        EVENT_LOG(fmt::format(
                            "Discarding garbage partial (re-downloading from start): {}", title));
                    } else {
                        resume_from = static_cast<curl_off_t>(fsize);
                        EVENT_LOG(
                            fmt::format("Resuming {} from {} bytes", title, (long long)fsize));
                    }
                }
            }

            // CurlRAII auto-releases (old code: manual curl_easy_cleanup)
            CurlRAII curl_raii;
            CURL *curl = curl_raii.handle;
            // Resume uses "ab" append; new download uses "wb"
            FILE *f = fopen(filepath.c_str(), resume_from > 0 ? "ab" : "wb");
            bool success = false;

            if (curl && f) {
                // Set progress callback data
                CurlProgressData progress_data;
                progress_data.dl_id = dl_id;
                progress_data.title = title;
                progress_data.last_bytes = 0;
                progress_data.resume_offset =
                    resume_from; // overall percentage includes the existing part
                progress_data.last_time = std::chrono::steady_clock::now();

                // URL safety: protocol whitelist (http/https only) + intranet/loopback/cloud-metadata interception.
                bool url_safe = !IniConfig::instance().get_reject_unsafe_url() ||
                                !UrlGuard::reject(url, "download");
                if (!url_safe) {
                    EVENT_LOG(fmt::format("Rejected unsafe download URL: {}", url.substr(0, 60)));
                }

                bool tls_verify = IniConfig::instance().get_tls_verify();
                // Retry loop: transient failures (timeout/disconnect/partial file/5xx) retry 3 times, don't give up easily;
                //   keep the half-file for next resume.
                const int MAX_RETRIES = 3;
                for (int attempt = 1; attempt <= MAX_RETRIES && url_safe; ++attempt) {
                    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
                    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
                    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
                    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
                    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
                    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                                     CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
                    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
                    apply_network_proxy(curl);
                    if (resume_from > 0) {
                        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, resume_from);
                    }
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L); // 300->600, longer tolerance
                    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L); // connection timeout 30s
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, tls_verify ? 1L : 0L);
                    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, tls_verify ? 2L : 0L);
                    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_DEFAULT);
                    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 1L);
                    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_callback);
                    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
                    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

                    ProgressManager::instance().update(
                        dl_id, 0,
                        attempt > 1 ? fmt::format("Retry {}/{}", attempt, MAX_RETRIES)
                                    : "Connecting...",
                        "...", 0, (int64_t)resume_from, (int64_t)resume_from);

                    CURLcode perform_rc = curl_easy_perform(curl);
                    long http_code = 0;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

                    // F23: HTTP 416 (Range Not Satisfiable) + resume_from > 0 = file already
                    //   fully downloaded. Server says "your range starts beyond file size" → complete.
                    //   Must check BEFORE the 2xx check (416 is not 2xx, would be treated as failure).
                    if (perform_rc == CURLE_OK && http_code == 416 && resume_from > 0) {
                        LOG(fmt::format(
                            "[DL] HTTP 416 (range not satisfiable) — file already complete: {}",
                            title));
                        success = true;
                        break;
                    }

                    if (perform_rc == CURLE_OK && http_code >= 200 && http_code < 300) {
                        // Resumed but server returned 200 (range unsupported, returns the full file) ->
                        //   file = old part + full = corrupted; truncate and re-download from 0
                        if (resume_from > 0 && http_code == 200) {
                            fclose(f);
                            f = fopen(filepath.c_str(), "wb");
                            resume_from = 0;
                            progress_data.resume_offset = 0;
                            progress_data.last_bytes = 0;
                            LOG(fmt::format("[DL] server ignored range (200), restart from 0: {}",
                                            title));
                            continue;
                        }
                        success = true;
                        break;
                    }
                    // Failure: determine whether transient (retryable)
                    bool transient =
                        (perform_rc == CURLE_OPERATION_TIMEDOUT ||
                         perform_rc == CURLE_COULDNT_CONNECT ||
                         perform_rc == CURLE_COULDNT_RESOLVE_HOST ||
                         perform_rc == CURLE_PARTIAL_FILE || perform_rc == CURLE_RECV_ERROR ||
                         perform_rc == CURLE_SEND_ERROR || perform_rc == CURLE_GOT_NOTHING ||
                         perform_rc == CURLE_SSL_CONNECT_ERROR ||
                         (http_code >= 500 && http_code < 600));
                    LOG(fmt::format("[DL] attempt {}/{} rc={} http={} transient={} : {}", attempt,
                                    MAX_RETRIES, (int)perform_rc, http_code, transient, title));
                    if (!transient || attempt == MAX_RETRIES)
                        break;
                    fflush(f);
                    // Update resume_from to the current file size for the next resume
                    auto sz = fs::file_size(filepath, ec_fs);
                    if (!ec_fs && sz > 0) {
                        resume_from = static_cast<curl_off_t>(sz);
                        progress_data.resume_offset = resume_from;
                    }
                }
                if (f)
                    fclose(f);
                // On failure, keep the half-file for next resume (no longer deleted)
                if (!success) {
                    EVENT_LOG(fmt::format(
                        "Download failed after retries, partial kept for resume: {}", title));
                }
            } else {
                if (f)
                    fclose(f);
                // curl is auto-released by CurlRAII (safe even if nullptr)
            }

            // Verify the file matches the episode info before counting as success.
            //   A 2xx/CURLE_OK can still yield a truncated/invalid file (server closed early,
            //   no Content-Length). Without this, a real failure would show [OK].
            if (success) {
                auto vr = verify_downloaded_file(filepath, n->duration);
                if (!vr.ok) {
                    success = false;
                    EVENT_LOG(fmt::format("Verify failed ({}): {}", title, vr.reason));
                }
            }

            ProgressManager::instance().complete(dl_id, success);
            {
                // Writing node fields and serializing the tree both require holding tree_mutex, mutually exclusive with UI/other workers
                std::lock_guard<std::recursive_mutex> lock(tree_mutex);
                if (success) {
                    CacheManager::instance().mark_downloaded(url, filepath);
                    n->is_downloaded = true;
                    n->local_file = filepath;
                    // Y24.24: download online transcript alongside the episode (format already
                    //   chosen by detect_from_rss priority: vtt>srt>json>lrc>twt).
                    subtitle_mgr_.download_sidecar(n, filepath, pool_);
                    EVENT_LOG(fmt::format("Saved: {}{}", base_name, ext));
                } else {
                    CacheManager::instance().mark_partial(url); // .part left → mark incomplete
                    EVENT_LOG(fmt::format("Failed: {}", title));
                }
                Persistence::save_cache(radio_root, podcast_root);
            }
        });
        // Old t.detach() removed; pool_ automatically joins on destruction
    }
    return sr == ProgressManager::StartResult::NEW;
}

// Promote pending downloads into free slots. current_slot_count = current number of
//   ProgressManager entries (active + within their completion display window). Main-thread-only.
void App::pump_download_queue(size_t current_slot_count) {
    while (current_slot_count < (size_t)MAX_CONCURRENT_DOWNLOADS && !pending_downloads_.empty()) {
        TreeNodePtr n = pending_downloads_.front();
        pending_downloads_.pop_front();
        if (start_one_download(n))
            ++current_slot_count;
    }
}

} // namespace panicast
