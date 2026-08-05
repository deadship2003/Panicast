
## robust — mpv 交互命令移到 worker 线程（TUI 永不被 mpv/PA 阻塞）

**Context:** 用户要求"不管 mpv 状态，TUI 都要响应交互"。前一条日志（看 IPTV/mp3 暂停）+ 诊断确认：WSLg PulseAudio 挂死 → 暂停触发 `pa_stream_cork` 超时 → mpv 阻塞 → UI 线程直接调 `mpv_set_property` 冻结。

**Approach:**
- `get_state()` 已是缓存读（`update_state` 在 event 线程、mpv 读取在 `mtx_` 锁外）→ 不阻塞 UI，无需改。
- 阻塞点在**命令侧**：`set_pause/set_volume/set_speed/...` 在调用线程（UI）直接 `mpv_set_property`。
- 修：MPVController 加**命令 worker 线程**（`cmd_thread_` + `cmd_queue_` + cv）。`toggle_pause/set_pause/set_volume/adjust_speed/reset_speed/set_speed` 改 `enqueue_cmd_` 投递；UI 立即返回（state_ 乐观更新：pause/volume/speed）。worker 顺序执行；mpv 卡死时只 worker 阻塞，UI 继续跑。
- 关停：`stop()` bounded-join（等 `cmd_done_` ≤1.2s）+ detach 回退（与 event 线程同模式，靠 `_exit` 退出）。
- 编译坑：按值捕获的 lambda 在 const operator() 里 `&capture` 是 const 指针 → mpv 的 `void*` 参数报 const void*→void*；三个 lambda 加 `mutable` 解决。

**Verification:** ctest 38/38；构建 0-warning；冒烟正常。

**Followups:** 其余 mpv 调用（play/seek/loop/keep_open/sub_add）同法迁 worker（按需）；治本修 WSL PA。

---

## fix — "看视频/暂停时 TUI 输入无响应" = mpv 视频窗口抢键盘焦点（非死锁）

**Context:** 用户报告"暂停后 TUI 不响应"，看 IPTV 时复现。D4（end_file 入队）+ D5（watchdog）按"死锁"方向未根治。用户提供 panicast.log。

**Root cause（日志确认）：**
- 日志 `input: No key binding found for key 'l'/'a'/SPACE/','/MBTN_LEFT'` —— mpv 的视频窗口（vo=gpu/wlshm）**抢了键盘焦点**，用户按键被该窗口接收、因无绑定被丢弃，ncurses TUI 收不到。看视频即发生，与暂停无关。
- 排除死锁：ncurses 只在 ui/app（无跨线程破坏）、`stop_realtime` 非阻塞、END_FILE 已 D4 迁 UI 线程；watchdog 也未触发慢帧 → 不是 freeze。

**Fix（卫生）：** `mpv_set_option_string(ctx, "input-default-bindings", "no")` —— TUI 拥有输入，mpv 窗口不解释键（消除吃键日志、防绑定冲突）。

**Limitation：** 视频窗口抢焦点是 WSLg/Wayland 窗口行为，应用内无法强制终端保持焦点。绕过：点终端窗口恢复 TUI 控制。根治需 WSLg/WM 层（focus-new-windows 规则）。

**Also（日志）：** WSL 音频坏（PulseAudio timeout / ALSA refused → AO init failed），独立问题。

---

## D5 — M0 收尾 + UI 帧时间 watchdog（新架构 M0 第 5 人日）

**Context:** M0 收尾。D1–D4 已落地 EventBus、IProxyManager（全消费者经 Connectivity）、Media/MediaID。用户报告"暂停后 TUI 输入无响应"在 D4 修复后仍存——静态分析已穷尽明显原因（D4 修了 END_FILE 跨线程 `playlist_mutex_` 争用；排除 ncurses 跨线程破坏[只在 ui/app]、ASR join 阻塞[`stop_realtime` 非阻塞]）。剩余卡点需运行时数据。

**Approach:**
- M0 端到端 = app 本身（EventLog→EventBus、所有网络→ProxyManager、Media 适配器），无需额外样例。
- 加 **UI 帧时间 watchdog**：每帧测 `tree_mutex` / `playlist_mutex_` 等待 + 整帧耗时；超阈值（锁等待 >80ms / 帧 >150ms）写 `panicast.log`。下次复现"暂停后无响应"时，日志直接显示卡点（等哪把锁 / 哪步慢）→ 定点修复。

**M0 达成：** 最小可演进系统（EventBus + Connectivity + Media + 工程基线）。M1+ 增量。

**Verification:** ctest 38/38；构建 0-warning；冒烟正常。

**Followups：** 据 watchdog 日志定点修"暂停后输入无响应"剩余卡点（疑似 ASR worker 暂停后仍转写整段占 CPU，待确认）；M1（Downloader 全覆盖 D6 + 热键 Keymap D7）。

---

## D4 — Media/MediaID 骨架 + 修复"暂停后 TUI 无响应"（新架构 M0 第 4 人日）

**Context:** D4 落地 Media 领域句柄；用户报告"TUI 暂停播放后过一段时间无响应"，要求在 D4 一并修复。

**Bug 根因分析：**
- `on_playback_ended`（`app_playback.cpp:66`）通过 `end_file_callback`（`app_run.cpp:73` 注册）在 **mpv 事件线程**执行。它锁 `playlist_mutex_`（:68）并做 `fs::exists`/`URLClassifier::classify`/`pool_.submit`/`player.play` 等一串工作。
- UI 主循环（`app_run.cpp:338`）draw 时也持有 `playlist_mutex_`。
- 暂停久了（尤其直播流），流空闲断开 → mpv 触发 END_FILE → mpv 事件线程在 `on_playback_ended` 持 `playlist_mutex_` → UI 线程到 :338 拿不到锁 → 卡在 draw、到不了 :398 读输入 → TUI 无响应。属 AUDIT P1-4/5/8 同源的"跨线程持 UI 锁"问题。

**修复：**
- mpv 事件线程的 end_file 回调改为只入队：`pending_end_reason_.store(reason)`（`app.h` 新增 `std::atomic<int> pending_end_reason_{-1}`）。
- UI 主循环每帧 drain（`app_run.cpp` 紧随 `drain_remote_commands()`）：`exchange(-1)` 取出 reason，非 -1 则在 **UI 线程**调 `on_playback_ended(reason)`。
- 效果：`on_playback_ended` 不再在 mpv 线程持 `playlist_mutex_`，消除与 UI draw 的争用。这正是 EventBus/命令总线把跨线程回调 marshal 到 UI 线程的模式（D1 EventBus 的延伸）。

**Media/MediaID：**
- `include/panicast/domain/media.h`（header-only）：`MediaID`（弱引用 TreeNode 身份，`==`/`!=`/`valid`/`lock`）+ `Media{id,url,title}` 只读视图 + `media_from_node` adapter。不改 TreeNode；后续逐步收敛。

**Verification:** ctest 35→38（+3 MediaID/Media）；增量构建 0-warning；冒烟正常。

**Followups：** 若个别场景仍偶发卡顿，继续把其它 mpv 线程→App 回调（property observers 等）也 marshal 到 UI 线程（EventBus）；Media 表面随 M2 收敛。

---

## D3 — 全部网络消费者接入 Connectivity（url-aware resolveProxy + Downloader）（新架构 M0 第 3 人日）

**Context:** D2 落地 IProxyManager 但 apply_network_proxy 只用 resolveProxy("")（无 url，仅全局）。D3 让所有网络消费者传真实 url + platform（启用域名/平台规则），Downloader（含 yt-dlp）也接入。

**Approach:**
- `apply_network_proxy(CURL*, const std::string &url, const std::string &platform = "")`：body 改 `resolveProxy(url, platform)`。
- 4 个 curl 调用点传值：`network.cpp` configure_curl 传其 `url`（通用 fetch，platform ""）；`bilibili_api.cpp` 传 `url` + "bilibili"；`itunes_search.cpp` 传 `url` + "podcast"；`app_download.cpp` 传 `url`（下载，platform ""）。
- `ytdlp_runner.cpp`：`--proxy` 从 `IniConfig::get_proxy()` 改为 `ProxyManager::instance().resolveProxy("").url`（经 Connectivity；yt-dlp 的 url/platform 感知路由留作精化）。
- 行为零变化：无平台/域名规则时 resolveProxy 返回全局 [network] proxy（与改动前一致）。

**Verification:** ctest 35/35；增量构建 0-warning；`build/panicast --version` 正常。

**Followups:** 按需填首批规则（youtube→代理、`*.googlevideo.com`→代理、bilibili→直连）；ytdlp_runner 传真实 url/platform（需 run() 加参数）。

---

## D2 — IProxyManager（Connectivity 层）+ apply_network_proxy 首个消费者（新架构 M0 第 2 人日）

**Context:** 落地"统一网络前端"——所有网络消费者（Parser/Downloader/字幕/AI 云端）经一个 IProxyManager 解析代理，mpv 播放直连。D2 做接口 + 规则链 + 首个消费者；Downloader 等调用点切换在 D3。

**Approach:**
- `IProxyManager` + `ProxyConfig{url}` + `ProxyManager`（`resolveProxy(url, platform)` 规则链：平台→域名→全局→直连；`host_of` + `domain_matches` 实现 `*.glob` 域名匹配；`mutable std::mutex` 线程安全）。
- **全局源可注入**（`setGlobalSource(std::function<ProxyConfig()>)`）而非直接读 IniConfig：使 `proxy_manager.cpp` 不依赖 config 系统 → 单测目标可干净链入 proxy_manager.cpp 而不拖入 ini_config 及其依赖。生产由 `network.cpp`（本就依赖 ini_config）在文件作用域静态初始化里把 `[network] proxy`（实时读，Ctrl+N 即时生效）注入。
- `apply_network_proxy(CURL*)` 改走 `ProxyManager::instance().resolveProxy("")`：首个真实消费者。无平台/域名规则时返回 [network] proxy → 行为零变化。

**Verification:** ctest 30→35（5 个 ProxyManager 用例全过：全局源/无源直连/平台覆盖/域名匹配/平台优先）；增量构建 0-warning；`build/panicast --version` 正常；代理路径经 ProxyManager（Ctrl+N 全局源实时生效）。

**Followups:** D3 切换 network.cpp/bilibili_api.cpp/itunes_search.cpp + ytdlp_runner 调用点到带 url 的 `resolveProxy`，并接入 Downloader；按需填平台/域名规则（如 youtube→代理、*.googlevideo.com→代理）。

---

## D1 — EventBus 核心 + EventLog 作为首个生产者（新架构 M0 第 1 人日）

**Context:** 新架构迁移第 1 步（开发计划 D1）。引入类型安全事件总线，作为后续替换 `pending_select_`+散落回调（AUDIT 竞态 P1-4/5/8 根因）的承重墙。

**Approach:**
- header-only `EventBus`（`include/panicast/core/event_bus.h`）：模板 `subscribe<E>/publish<E>`，`type_index` 分桶；订阅者锁内快照、锁外派发（handler 可递归 publish/unsubscribe，不自死锁）。`subscribe` 返回 token，`unsubscribe(token)` 移除。
- **先只上同步 `publish`**（caller 线程派发）；异步 `post/drain`（跨线程→UI）留到迁移 `pending_select_` 时再加（并发敏感，单列后续人日）。
- **首个真实生产者 = `EventLog::push`**（每条日志 `publish(LogEvent)`）。选它：①真实（379 处 EVENT_LOG 全上总线）；②安全（EventLog 本身线程安全、无订阅者即空操作、不依赖总线即可工作 → 零 init-order 风险、零行为变化）；③启用未来订阅者（远程日志推送/调试覆盖层）。

**Verification:** ctest 26→30（4 个 EventBus 用例全过）；增量构建 0-warning（event_log.h 改动触发其依赖重编，均通过）；`build/panicast --version` 正常，EventLog 行为不变。

**Followups:** `post/drain`（异步）+ 迁移 `pending_select_` 到总线（修竞态）；更多信号（playback 状态等）逐步上总线。

---

## N07 — titlebar-as-root data-model refactor (root nodes → vectors) + exit typeahead fix

**User goal:** eliminate the vestigial per-mode root NODE from the data model (option E, "最优雅"). Display redundancy (5-mode border + root row) was already fixed by Step 1 (display iterates items). N07 completes the data-model cleanup.

**Approach (E = vectors, not D's display-only):** the 8 per-mode roots (`radio_root`...`iptv_root`) were `TreeNodePtr` container nodes whose `->children` held the items. Converted to `std::vector<TreeNodePtr>` — items live directly in the vector, no container node. `current_root` (TreeNodePtr) eliminated → `items_for_mode(mode)`/`cur_items()`. Container functions (`clear_marks`/`collect_marked`/`count_marked_safe`/`remove_node`) gained `_current` helpers that loop `cur_items()`. `sort_target` reworked (top-level sort = sort `cur_items()`; reverse state in new `cur_sort_reversed` member, was on the root node). `flatten` simplified to pure recursion (removed `title=="Root"` magic). `Persistence::save_cache/load_cache` → vector signatures. `get_root_by_mode_string` → `vector*`. Parent pointers for top-level items → `reset()`. `*_root_loaded` (root node's `children_loaded`) → App member bools.

**`online_root` stays TreeNode (key finding):** audit revealed it's a cross-mode LINK TARGET — the favourited "Online Search" is a LINK node pointing to `online_root` (`fn->linked_node = online_root`, pointer-identity check, LINK expand resolves `"online_root"` URL). Vectorizing it would break the LINK feature. It's already invisible (display iterates its children). Conclusion: online_root is a functional LINK-target node, NOT a vestigial container → stays. So E applies to the 8 per-mode display containers; online_root is a documented exception.

**Exit typeahead fix (separate bug):** keys typed during shutdown (after the main loop stops reading) sat in the terminal input queue and carried to the shell (e.g. "kjkkj" at the prompt). Fix: `tcflush(STDIN, TCIFLUSH)` in `restore_terminal_state()` + `restore_termios_async()` drains the input queue on exit.

**Verification:** compiles 0-warning; 9-mode smoke (display+nav) + deeper regression (sort/expand/search/go_back-parent/mark/delete — DB history 62→59 confirms `remove_from_current`) all pass, no crash. Bilibili search now requires a logged-in account (no root to attach anonymous results to — aligns with Y mode).

**Followups:** download/play/remote need user's real-resource testing (same `_current` helper pattern, low risk). Bilibili anonymous search removed (was attaching to the now-gone root).

## N06 — MediaType: DB-stored display category (platform-specific before generic)

**User goal:** history/favourites icons must respect platform specificity — YouTube never "OnlineVideo", radio never "OnlineAudio", and m3u8 must be IPTV (not radio). Store a `media_type` column so the icon comes from the DB instead of being re-inferred from the URL every render.

**Key design correction to the initial proposal:** classifyMediaType is NOT a pure URLType→MediaType switch. URLClassifier IS platform-priority for platform-vs-generic (YouTube matched before .mp4), BUT URLType conflates two distinctions the user needs:
- `.m3u8` → URLType::RADIO_STREAM (so pure mapping would make m3u8 = Radio — wrong);
- local `file://.mp4` and online `.mp4` both → VIDEO_FILE; local non-video and online radio both → RADIO_STREAM (so URLType can't tell local from online).

Fix: classifyMediaType layers two pre-checks on top of classify(): (1) `file://`/absolute-path → LocalVideo/LocalAudio by extension (reuse classify()'s VIDEO_FILE split); (2) `iptv:` scheme or `.m3u`/`.m3u8` extension → Iptv. THEN switch on URLType for the rest.

**Confirmed decisions:** 9 categories + emoji; DOUYIN_* → Tiktok (CN counterpart, placeholder); m3u8 empirically = IPTV (user's DB: 39/39 m3u8 are IPTV channels, zero radio uses m3u8); emoji de-duped + all glibc wcwidth=2 (▶ U+25B6 is wcwidth=1 → use 📹 for YouTube; 🎶/🎥 for local audio/video to avoid clashing with Tiktok🎵/OnlineVideo🎬).

**Display scope:** only history + favourites DB-driven LEAF nodes use media_type_icon (gated on NodeType::PODCAST_EPISODE/RADIO_STREAM). Folder/feed/link favourites keep the folder icon; live tree (radio/podcast browsing) unchanged — its flag-based icons are already accurate.

**Schema:** SCHEMA_VERSION 46→47; history + favourites gain `media_type INTEGER`; one-time backfill (guarded by stored_version<47) recomputes from url. Write path computes media_type inside add_history/save_favourite (zero caller changes).

## N05 — 'r' key unified per-node refresh (Y/B/T fix)

**User goal:** Y mode's dedicated "resync" on 'r' was not a user-designed feature — diagnose & fix; extend the same mechanism to B and T; then package as N05.

**Diagnosis:** The resync *capability* is legitimate & required (account data only syncs at login + manual 'r'; lazy-expand reads local DB; no periodic sync — deleting it would freeze data). The *wiring* was wrong:
- Y: 'r' on ANY node triggered whole-account resync (heavy + collapsed the tree).
- B: 'r' was a silent no-op (man/help falsely said "Refresh node"; a comment claimed re-fetches, never implemented).
- T: 'r' couldn't refresh stale local cache at all.

**Fix (app_input.cpp 'r' dispatch unified by node type; account nodes disambiguated by AppMode — is_account is reused by T creators):**
- Y account → `resync_account_node`; Y Subs/History → new `refresh_account_subs/history` (subtree-only, preserves expansion).
- B account → `refresh_bilibili_account`; B followings/history → `refresh_bili_followings/history` (force re-fetch).
- T creator (FOLDER) → new `refresh_node` T branch → `spawn_load_feed`(TIKTOK_USER) online re-fetch → `commit_feed_result` replaces children + `episode_cache` (DEL+INS).

**Followups (not in N05):** T single-video leaf 'r' stays a no-op hint (consistent with episodes everywhere). Y channels (`is_yt_channel`) / B UP masters (`is_bili_up`) still no-op on 'r' — their loaders are inline lambdas; extracting them is a later refactor.

## N04 — PIN auth + UDP discovery + WebSocket + embedded BS client + APK source

**User goals:** (1) latest tarball; (2) open the backend address in any IE → control directly; (3) APK: install on modern Android with no dependency issues, auto-scan network for players, PIN pairing (dynamic PIN shown in PaniCast popup + universal 6696 for headless), full control + view.

**Auth model (resolves BS-direct vs APK-PIN tension):**
- PIN-based auth replaces `auth_token`. Dynamic 4-digit PIN (`regenerate_pin()`), shown via `:pin` popup, rotatable via `:newpin`. Universal `6696` always valid (headless pairing).
- **Localhost connections are open** (no PIN) → "open http://127.0.0.1:port/ in IE on the PaniCast host → control directly". Non-localhost → PIN required. The BS HTML shows a PIN overlay only when the server returns ACK auth-required.
- `password <pin>` valid iff pin == dynamic || pin == "6696".

**UDP discovery (N05):** APK broadcasts `PANICAST_DISCOVER` to udp 18430; PaniCast's `discovery_loop` responds `PANICAST 1 tcp=<port> ws=<port+1>`. APK uses the response source IP. Avoids mDNS/ZeroConf complexity + dependencies — plain UDP broadcast (Android `DatagramSocket` + `WifiManager.MulticastLock`).

**WebSocket (N06):** self-written RFC6455 (handshake SHA1+base64 via OpenSSL `SHA1`; frame codec with mask/ping-pong). One HTTP listener on port+1 serves the embedded BS client (GET /) AND upgrades to WS. **socketpair bridge**: RemoteSession was refactored to separate `read_fd_`/`write_fd_`; the WS bridge runs RemoteSession on one end of a socketpair and shuttles WS frames ↔ PRP lines on the other — the PRP engine is unchanged. This keeps ONE protocol (DRY) across raw-TCP and WS transports.

**Embedded BS client:** a single self-contained HTML (inline CSS+JS, IE11-compatible) baked into the binary via `panicast_web_index.h` raw-string include → "open address and control" with zero external files. GitHub Dark, PIN overlay, now-playing + controls + idle live status + progress interpolation + auto-reconnect.

**APK:** native Kotlin + Compose + Material3 (minSdk 24), no runtime deps (Gradle-bundled Compose). UDP scan → PIN → TCP PRP. Source project under `apk/`; this environment has no Android SDK so the APK is built in Android Studio (instructions in `apk/README.md`).

**Verification:** 0-warning build; pin_test (LAN: no-pin ACK / wrong ACK / 6696 OK / status OK), discovery_test (probe→beacon), ws_test (101 handshake + greeting frame + ping/status/volume 60 end-to-end over WS) all PASS; HTTP GET / → 200 + embedded client.

**Open:** APK on-device build (Android Studio); explicit `notify("log"|"tree")`; remaining command coverage (search/mark/edit/download/subtitle/asr/queue); BS/APK UI polish.

---

## N03 — idle event subscription + state sync push

**Goal (user emphasis: "遥控和状态同步"):** let remote clients subscribe to state changes and receive pushed `changed: <subsystem>` events (MPD `idle` semantics), so a remote UI stays live without polling.

**Design:**
- `RemoteSession::handle_idle`: enters subscription mode; multiplexes with `poll(fd, 100ms)` so it simultaneously (a) waits for server-side `notify_change` to populate `idle_pending_`, and (b) watches the socket for `noidle` / a queued command / close. On change → emit `changed: <subsys>` lines + `OK`. This avoids a second thread per session — the single worker thread handles both directions via poll.
- `notify_change(subsys)` is the cross-thread entry (called by `RemoteServer::notify` from the diff thread or App): guards with `idle_mtx_`, dedups, only if subscribed.
- `RemoteServer` keeps a `sessions_` registry; `notify(subsys)` snapshots the set under `sessions_mtx_` then delivers outside the lock (avoids holding the registry lock during per-session idle_mtx_ acquisition — no lock-ordering hazard).
- **Diff poller** (`diff_loop`, 10Hz): the only source of state-change detection for mpv-internal events (seek/pause/track-end). Compares `snapshot_state()` fields → `notify(player|mixer|options|mode|subtitle|art)`. The 100ms cadence IS the coalesce window (no separate debounce needed). `elapsed` is intentionally NOT diffed (would fire every frame) — clients interpolate elapsed; only state/song/title/url changes fire `player`.
- `recv_buf_` became a member + `poll_line(out, timeout_ms)` shared by `run()` (blocking read) and `handle_idle` (multiplexed read) — single buffer, single code path.

**Roadmap condensation:** N02 already absorbed the plan's N03–N06 command-coverage milestones (core controls shipped in N02). So this idle release is sequentially **N03** (was the plan's "N07" milestone). Future plan updates will renumber sequentially.

**Verification:** `idle_test.py` — A `idle mixer`, B `volume 42` → A receives `changed: mixer`+`OK`. Diff→notify→push chain proven. 0-warning build.

**Open:** N04 WebSocket frontend (self-written RFC6455) + static asset hosting → enables browsers; N05 BS web client; deferred explicit `notify("log"|"tree")` hooks + remaining command coverage (search/mark/edit/download/subtitle/asr/queue).

---

## N02 — PRP protocol engine, state snapshot, control command end-to-end

**Goal:** make the remote terminal actually control PaniCast — query state AND issue commands that take effect — over the MPD-style line protocol defined in `PANICAST_N_LINE_PROTOCOL_DESIGN.md`.

**Protocol (PRP — PaniCast Protocol):** MPD-style text line protocol. Greeting `OK PaniCast N02`; request `COMMAND [ARG...]\n` (double-quoted args for spaces); response `key: value` lines ending `OK` or `ACK [code@0] {cmd} msg`. Query commands (`status`/`currentsong`/`playlistinfo`/`ping`/`password`) answered inline; control commands forwarded to the UI thread via the bus. `password <token>` auth gate (MPD semantics). `idle`/`noidle` accepted as no-ops (N07 implements real subscription).

**Threading contract (the two sanctioned crossing points):**
1. WRITE path — `RemoteCommandBus` (N01): server `push()`es control commands; UI thread `drain_remote_commands()` each frame; `dispatch_remote()` maps to existing App methods. UI never touched off-thread.
2. READ path — `RemoteStateSnapshot`: built once per frame on the UI thread (`update_remote_state_cache`) under a dedicated `remote_state_mtx_`; server threads read copies via `snapshot_state()`. This **deliberately avoids cross-locking `tree_mutex`/`playlist_mutex_` from server threads** — only `playlist_mutex_` is taken on the UI thread (same thread that already takes it) to copy playlist titles. Player state uses `MPVController::get_state()` which is already mutex-protected.

**Decoupling:** `RemoteControlInterface` (abstract) — App implements it; `RemoteServer`/`RemoteSession` depend on the interface, not App. Composable + testable.

**Control coverage (N02):** playback / seek / volume / speed / play-mode / sleep / mode-switch / navigation / mpv-passthrough — the demonstrable core. `mpv <cmd>` reuses the `:`-window forwarding. Search/mark/edit/download/subtitle/asr/queue deferred to N03–N05 (some are coupled to TUI input boxes — need non-interactive variants).

**Verification:** 0-warning build; Python PRP client proves end-to-end: `volume 55` → subsequent `status` echoes `volume: 55` (server→bus→UI→player→snapshot). Default `enable=false` leaves local TUI unaffected.

**Open:** N03 full command coverage; N07 idle subscription (notify + 10Hz diff + debounce); N08 WebSocket frontend (self-written RFC6455); N09 BS web client.

---

## N01 — Network control line: foundation skeleton (command bus + TCP server)

**Context:** User requested a network-control feature line (N01–N99) branched from `Panicast_V0.1-Y24.56`, so a remote terminal — first an IE browser (BS), later an Android APK — can exercise **all** local-terminal control functions plus live monitoring. Development must follow `/mnt/e/AI/DEVELOPMENT_PRINCIPLES.md` (i18n / UNIX philosophy / plan-first / async-non-blocking / concurrency-safe / data-layer收敛).

**Key user decisions (confirmed):**
- HTTP/server implementation: **self-written full C++ socket server** (no external lib). Chose POSIX `socket/bind/listen/accept` over adding cpp-httplib — zero new dependency, full control, matches "统一封装" philosophy extended to the server side.
- Real-time state/LOG push: **MPD / ncmpcpp-style technique** — *protocol design deferred*. User will explain the MPD/ncmpcpp approach in detail before N02 protocol is finalized. (MPD uses a persistent TCP line protocol with an `idle` event-subscription command; ncmpcpp is an MPD client. This signals a move away from HTTP/REST toward a daemon TCP protocol + thin clients.)
- Source tree: `/mnt/e/AI/PaniCast/Panicast_V0.1-N01/` (parallel to existing Y/F lines).

**Architecture (this version):**
- The UI (ncurses) is single-threaded and not thread-safe. Therefore the network server NEVER calls App/MPVController methods directly. Two sanctioned crossing points:
  1. **`RemoteCommandBus`** — server thread `push()`es a `RemoteCommand{action,args,client_id}`; the TUI main loop `drain_all()`s once per frame and dispatches on the UI thread. Mutex-protected vector, non-blocking swap, `shutdown()` atomic flag. Minimal lock scope.
  2. **state snapshot** (N02) — server thread reads a mutex-guarded snapshot; `MPVController::get_state()` is already thread-safe; App members need a new `snapshot_remote_state()`.
- **`RemoteServer`** threading (concurrency rules): one accept thread; each connection on a tracked worker thread (`struct Worker{ std::thread t; std::atomic<bool> done; }`), reaped each accept iteration so the worker list stays bounded. **No `detach()`** — `stop()` closes the listen socket to unblock `accept()`, then joins the accept thread AND every worker. SIGPIPE avoided via `MSG_NOSIGNAL`.
- Platform scope: POSIX (Linux/macOS). Windows compiles to stubs (`start()` returns false). Mirrors the `mpv` vcpkg dependency which is `linux|osx` only. Keeps the cross-platform build green.
- Config: opt-in `[remote]` section, **default `enable=false`** so the local TUI is byte-for-byte unaffected unless the user opts in. `bind=127.0.0.1` default (localhost-only; `0.0.0.0` for LAN).

**Scope of N01 (deliberately minimal):** version bump, `[remote]` config, command bus, TCP accept-loop skeleton (banner-only handler), App wiring (start/stop/drain), `dispatch_remote()` log-only. The command **protocol** is intentionally NOT implemented — it depends on the pending MPD/ncmpcpp decision.

**Verification:** 0-warning build (`-Wall -Wextra -Wpedantic`); `--version` = N01; smoke test with `enable=true port=18421` → `bash /dev/tcp` connection receives the banner; `timeout` exit joins cleanly (no orphan/hung threads); default `enable=false` → no server started.

**Open for N02 (needs user input):** finalize the command protocol (MPD-style line protocol? `idle`-style event subscription? how does the BS/IE browser fit a non-HTTP protocol — a small WS/TCP bridge, or a separate HTTP endpoint set?). Map `dispatch_remote()` actions to local control methods (playback/navigation/modes/management/`:`-commands).

---

## Y24.56 — IPTV playback LOG-area event messages (off-air / no-video diagnosis)

**Bug (user trial):** playing an IPTV channel whose address is correct but the station is not currently broadcasting → no video stream → mpv does not open a video window, and the user has no idea WHY. The 30s pending timeout only says "stream may have failed", indistinguishable from a wrong address.

**Design (confirmed by user):** enumerate every situation where IPTV playback does not visibly start / no video window, and emit a concise English event message to the on-screen LOG area (EventLog, which double-writes to panicast.log). Two prefixes:
- `MPV:` = mpv-level behavior (the cause) — existing mpv_error_str / fallback messages, kept as-is.
- `IPTV:` = IPTV-context explanation (the user-facing meaning) — new.
- Same event causing both → print BOTH, in time order (MPV: first, IPTV: second); both reach screen + log file via EVENT_LOG.

**The 13 messages (approved):**
1. `IPTV: server unreachable — network, DNS, or timeout; check connection or switch source`  (END_FILE -13, conn keywords)
2. `IPTV: channel not found — 404, address invalid or removed`  (-13 + "404"/"not found")
3. `IPTV: access denied — 403, region-restricted or authorization required`  (-13 + "403"/"forbidden")
4. `IPTV: server error — 5xx, source unavailable; retry later`  (-13 + "500"/"502"/"503"/"504")
5. `IPTV: connected, no stream data — channel may be off-air; retry later`  (polling: FILE_LOADED + core-idle + no codec + no download, held ≥ offair_detect_secs)
6. `IPTV: empty playlist — no playable stream for this channel`  (END_FILE -16)
7. `IPTV: audio-only channel — no video track, playing as audio`  (polling: audio codec + playing + no video track ≥ 8s)
8. `IPTV: audio output init failed — no audio device; check [mpv] ao, PulseAudio, WSLg`  (END_FILE -14)
9. `IPTV: video output init failed — no display, falling back to audio`  (END_FILE -15)
10. `IPTV: cannot decode stream — missing decoder; ensure ffmpeg is installed`  (END_FILE -17)
11. `IPTV: network too slow — sustained buffering, bandwidth insufficient or unstable`  (polling: core-idle + buffering>0 + <1s ahead ≥ 20s)
12. `IPTV: stream dropped mid-playback — source interrupted; switch channel or retry`  (END_FILE r=4 when PLAYBACK_RESTART already fired = had_playback_started_)
13. `Playback pending timeout 30s — stream may have failed`  (existing, parens removed; only fires when FILE_LOADED never came — naturally disjoint from #5 which requires FILE_LOADED)

**Implementation:**
- `ini_config.h`: `get_iptv_offair_detect_secs()` → `[iptv] offair_detect_secs` (default 12).
- `mpv_controller.h/.cpp`: `set_iptv_context(bool)` (atomic, set by app when mode==IPTV). Detection lives in the controller because it has the mpv error code AND the warn/error log text (needed to sub-classify -13 into 404/403/5xx/unreachable).
  - `MPV_EVENT_LOG_MESSAGE`: remember `last_log_text_` for warn/error (INFO still gated to load window).
  - `FILE_LOADED`: stamp `file_loaded_time_`, re-arm one-shots.
  - END_FILE r=4: after the existing `MPV:` message(s), if `iptv_context_`, emit the matching `IPTV:` message. If `had_playback_started_` (PLAYBACK_RESTART fired) → #12 (mid-playback drop); else `iptv_message_for_error_()` (-13 → `classify_iptv_load_error_()` keyword match; -14/-15/-16/-17 fixed).
  - `update_state()`: #5/#7/#11 polling detection, one-shot per track, gated by `iptv_context_`. #5 timer cancels on any data/codec arrival (no false positive on slow initial fill).
  - `reset_iptv_detection_()` (incl. `had_playback_started_`) called at play()/play_list()/play_list_from() entry points and re-armed on FILE_LOADED.
- `app_playback.cpp`: `player.set_iptv_context(mode == AppMode::IPTV)` in play_current + on_playback_ended (auto-advance).
- #5 and #13 are disjoint (#5 needs has_media/FILE_LOADED; #13 fires only when has_media never appears) → no double-fire, no cross-layer pending clear needed.

**Scope notes:**
- mpv warn/error log lines still go ONLY to panicast.log (not routed to the on-screen LOG area) — avoids noise; the IPTV: messages are the curated screen-side diagnostics.
- `mpv_error_str` (the MPV: behavior text) left untouched per "MPV:的行为就用MPV:".

Build: 0 warnings, 74/74, binary runs.
