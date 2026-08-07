# PaniCast 开发计划（人日任务 · 活清单）

> 铁律：**每个人日任务完成后，源码可编译（0-warning）、可执行文件正常运行、ctest 绿。** 不重写，增量演进。
> 准则见 `docs/DESIGN.md`。完成的任务打 `[x]`，并同步 `CHANGELOG.md` + `DECISIONS_LOG.md`(ADR)。每步保绿色可回退 commit。

## 里程碑 M0 — 核心抽象骨架（不破坏现 app）
- [x] **D1 — EventBus 核心 + 首个真实消费者** ✅ 2026-08-05
  - `include/panicast/core/event_bus.h`（**header-only**，类型安全 `subscribe<E>/publish<E>` + `unsubscribe(token)`，线程安全；`post/drain` 待迁 `pending_select_` 时加）+ 单测。
  - 首个真实生产者 = `EventLog::push` 顺带 `publish(LogEvent)`（379 处日志全上总线，零行为变化）。
  - 接入 `CMakeLists.txt`。**验收**：编译 0-warning、ctest 含 EventBus 用例、`./build/panicast` 启动+播放正常。
- [x] **D2 — IProxyManager 接口 + resolveProxy 规则链** ✅ 2026-08-05
  - `include/panicast/net/proxy_manager.h`（`ProxyConfig` + `IProxyManager::resolveProxy(url,platform)` 平台→域名→全局→直连，线程安全）+ `src/net/proxy_manager.cpp` + 单测。全局源**可注入**（`setGlobalSource`，不耦合 IniConfig → 测试可干净链入）；`network.cpp` 把 `[network] proxy` 注入 → Ctrl+N 实时生效。`apply_network_proxy` 已走 ProxyManager（首个消费者，行为零变化）。
  - **验收**：单测 5 例过、app 运行、Ctrl+N 代理仍生效。
- [x] **D3 — 调用点切到带 url 的 resolveProxy + Downloader** ✅ 2026-08-05
  - `apply_network_proxy(curl, url, platform)` 改 url-aware；4 个 curl 调用点（`network.cpp` configure_curl / `bilibili_api` / `itunes_search` / `app_download`）传真实 url+platform；`ytdlp_runner` 的 `--proxy` 改走 `ProxyManager::resolveProxy`。所有网络消费者现均经 Connectivity（mpv 播放仍直连）。
  - **验收**：编译 0-warning、ctest 35/35、冒烟正常；代理路径无回归。（yt-dlp 的 url/platform 感知路由为后续精化项。）
- [x] **D4 — Media/MediaID 骨架 + 修"暂停后 TUI 无响应"** ✅ 2026-08-05
  - `include/panicast/domain/media.h`（header-only：`MediaID` 弱引用 TreeNode 身份 + `Media` 视图 + `media_from_node` adapter，不改 TreeNode）+ 3 单测。
  - **Bug 修复**：`on_playback_ended` 原在 mpv 事件线程锁 `playlist_mutex_` 跑（暂停久了流断开→END_FILE→与 UI draw 锁争用→TUI 卡死）。改为 mpv 线程只入队 `pending_end_reason_`，UI 线程每帧 drain 后在自己线程跑 → 消除跨线程锁争用。
  - **验收**：编译 0-warning、ctest 35→38、冒烟正常。
- [x] **D5 — M0 端到端样例 + 收尾** ✅ 2026-08-05
  - 端到端 = app 本身（EventLog→EventBus、所有网络→IProxyManager/Connectivity、Media 适配器，D1–D4 已串通）。本日加 **UI 帧时间 watchdog**（测 tree_mutex/playlist_mutex_ 等待 + 整帧耗时，超阈值写 panicast.log）用于精确定位"暂停后输入无响应"剩余卡点。
  - **M0 达成**：最小可演进系统就位（EventBus + Connectivity + Media + 工程基线）。后续 M1+ 增量扩展。

## 里程碑 M1 — UI 解耦（消息总线 + 抽象层 + UI 纯交互）【主线】
> 目标（见 `docs/DESIGN.md` 目标架构）：UI 变纯交互层——只发 Action + 订阅事件，不再直接调 Core；消息总线（EventBus/ActionBus）成 UI↔核心唯一通道；抽 Application Services 作功能抽象层。每步 strangler、可编译可运行、有真实消费者（不空跑）。

- [x] **D6 — Action 类型 + 暂停端到端走总线（输入侧种子）** ✅ 2026-08-05
  - `include/panicast/app/actions.h`（`PlayPauseAction` 等 Action 类型，复用 EventBus 承载）；UI 暂停键 → `publish(PlayPauseAction{})`（不再直调 `player.toggle_pause()`）；`App::run()` 订阅 → 调既有 `player.toggle_pause()`（已是 worker 异步）。
  - **验收**：编译 0-warning、ctest 38/38、冒烟正常；UI 暂停路径不再直调 player（经总线）。首个输入侧 UI 解耦闭环。
- [x] **D7 — Keymap + 迁移 pause/音量/导航到 Action** ✅ 2026-08-05
  - `include/panicast/app/actions.h`（`Action = std::variant<PlayPause/VolumeUp/Down/NavUp/Down>` + `publish_action`）+ `include/panicast/app/keymap.h`（`Keymap` 键→Action）；`build_keymap()` 绑默认键；`handle_input` 先查 Keymap（命中→`publish_action`→return）再 switch。
  - pause/音量±/导航 k-j 现经总线（UI 不再直调 player.toggle_pause/set_volume/nav_up/down）；handler 订阅。Keymap 集中化→可重绑（`[keys]` INI 覆盖为后续）。
  - **验收**：编译 0-warning、ctest 38/38、冒烟正常；5 个交互经总线工作。（page/seek/模式/复杂流程待续。）
- [x] **D8a — PlaybackService 接管播放 Action（第一个 Service，增量1）** ✅ 2026-08-05
  - `include/panicast/app/playback_service.h` + `src/app/playback_service.cpp`：PlaybackService 持 player ref，`init()` 订阅 PlayPause/VolumeUp/VolumeDown → handler。App `playback_{player}` 成员 + `playback_.init()` 取代 App 内联订阅。
  - pause/音量现经 UI→Keymap→Action→总线→**PlaybackService**→player（nav 仍在 App）。首个 Application Service 就位。
  - **验收**：编译 0-warning、ctest 38/38、冒烟正常。
- [x] **D8b-1 — 播放队列状态迁入 PlaybackService（D8 增量2a）** ✅ 2026-08-07
  - `current_playlist`/`current_index`/`shuffle_queue_`/`playlist_mutex_` + `clear_playlist`/`refill_shuffle_queue`/`random_peer_index` 迁入 PlaybackService（私有状态 + 公开访问 API `playlist_mutex()/playlist()/current_index()/set_current_index()/shuffle_queue()`）；锁语义零变化。
  - `play_mode`/`playback_node`/`playback_pending_(_start_)`/`playback_mode_` 故意留 App（运行时手柄，D9 事件层替代直接读时再迁）；`play_current`/`on_playback_ended`/`build_peer_list` 仍是 App 方法经 `playback_.` 访问队列。
  - **验收**：ctest 38/38、构建 0-warning、冒烟正常；PlaybackService 为播放队列唯一所有者（D4 不变量保持：on_playback_ended 仍只在 UI 线程）。
- [x] **D8b-2 — play_current/on_playback_ended 迁入 PlaybackService（D8 增量2b）** ✅ 2026-08-07
  - `play_current`/`on_playback_ended`/`record_play_history`/`resolve_youtube_url` 迁入 PlaybackService（**逻辑迁入**；队列状态已在 D8b-1 内）；`build_peer_list`/`is_playable_node`/`play_episode` 留 App（树逻辑）。
  - **依赖后注入 `attach()`**：`pool_`/`subtitle_mgr_`/`transcription_engine_` 在 App 中声明于 `playback_` 之后、无法构造期引用 → 经 `attach()` 注入指针；运行时手柄（`playback_node`/`playback_pending_(_start_)`/`playback_mode_`）经 **4 个回调**写回 App（set_playback_node / set_pending / set_playback_mode / on_history_changed）。
  - **回调缝（Option Y）**：迁移的方法对运行时手柄只**写**不读 → 回调即够；读取点（app_input 21× playback_node、app_run 状态机读 pending、nav/remote）暂不动，避免 ~50 个低价值改名（D9 事件层替代直接读时再迁入并私有化）。`play_mode` 是设置、留 App、按调用传入（play_current/on_playback_ended 各加 mode+play_mode 形参）。
  - **D4 不变量保持**：`on_playback_ended` 现为 PlaybackService 方法，但仍只在 UI 线程跑（app_run 的 `pending_end_reason_` drain 调 `playback_.on_playback_ended(reason,mode,play_mode)`），绝不跑在 mpv 事件线程。
  - **验收**：ctest 38/38、构建 0-warning、pty 冒烟（启动→attach→主循环渲染→q/y 退出，exit 0 + clean endwin）。
- [x] **D9-1 — 播放事件上总线，替代 D8b-2 回调缝（D9 增量1）** ✅ 2026-08-07
  - 新 `include/panicast/app/playback_events.h`：输出侧（Core→App）事件 `PlaybackTrackChanged{node,mode}` / `PlaybackBufferingChanged{pending}` / `HistoryChanged{}`。
  - PlaybackService 在 `play_current`/`on_playback_ended`/`record_play_history` 处 `EventBus::publish` 这些事件（替代 D8b-2 的 4 个 `attach()` 回调）；`attach()` 瘦身为只注入 pool_/subtitle/transcription 三指针。
  - App 在 `run()` 订阅三事件 → 更新 `playback_node`/`playback_mode_`、`playback_pending_(_start_)`、`load_history_to_root`（订阅 token 入 `action_subs_`）。总线**同步派发** → 线程/顺序与回调完全等价（行为零变化）。
  - **验收**：ctest 39/39（+`PlaybackEvents.DeliveredOnBus` 用例）、构建 0-warning、pty 冒烟 exit 0；输出侧 service→App 事件通道建立，D8b-2 回调缝删除。
- [x] **D9-2 — "在播"手柄私有化（D9 增量2a）** ✅ 2026-08-07
  - `playback_node`/`playback_mode_` 迁入 PlaybackService 作私有状态（`playback_node_`/`playback_mode_`），在 `play_current`/`on_playback_ended` 直接写入；新增只读访问器 `playback_node()`/`playback_mode()`。App 删除这两个镜像成员 + `PlaybackTrackChanged` 订阅，改经访问器读（draw / `'N'` jump / remote / ASR 各读取点）。`PlaybackTrackChanged` 保留 publish（未来 remote/UI 直订的 reactor 通道，单测覆盖）。
  - `playback_pending_(_start_)`（BUFFERING 手柄，还被 app_run 每帧状态机直写 + 30s 超时）暂留 App → **D9-3**（同 D8b-1/D8b-2 的"干净一刀 + 敏感一刀分拆"原则）。
  - **验收**：ctest 39/39、构建 0-warning、pty 冒烟 exit 0；PlaybackService 为"在播曲目"状态唯一所有者，App 经访问器读、不再镜像。
- [x] **D9-3 — BUFFERING 手柄 + 状态机迁入 PlaybackService（D9 增量2b）** ✅ 2026-08-07
  - `playback_pending_(_start_)` 迁入 PlaybackService 私有；新增 **`advance_buffering(bool mpv_has_media)→bool`**——把 app_run 每帧状态机的 pending 生命周期（置位 / has_media 清除 / 30s 超时 / 一次性 buffering 时长日志）**整段逻辑**搬进服务，逐帧等价（5-case 验证）。私有 `set_buffering_(bool)` 单漏斗：写成员 + publish `PlaybackBufferingChanged`（reactor 通道）。`play_current`/`on_playback_ended` 的 3 处 publish 改走 `set_buffering_`。
  - App 删 `playback_pending_(_start_)` 成员 + `PlaybackBufferingChanged` 订阅；app_run 状态机精简为 `if (advance_buffering(has_media)) BUFFERING; else if (has_media) PLAYING/PAUSED/(idle)BUFFERING; else BROWSING`。PLAYING/PAUSED 仍从 mpv 派生（非 pending 状态）。
  - **D9 完成**：PlaybackService 持**全部**播放运行时状态（队列 + track 手柄 + buffering 手柄/状态机），App 不再直接写任何播放手柄、不再订 track/buffering 事件（只留 `HistoryChanged` 重建历史树）。track/buffering 事件保留 publish 作 D10+ remote/UI reactor 通道。
  - **验收**：ctest 39/39、构建 0-warning、pty 冒烟 exit 0 + clean endwin。
- [ ] **D10 — 抽更多 Services + UI 订阅其事件**
  - `LibraryService` / `SearchService` / `SubtitleService` / `AccountService` … 从 App 抽出（持状态、处理 Action、发事件）；UI 订阅对应事件。（每 Service 一进步、可多日）
  - **验收**：对应功能经 Service+事件工作。
- [ ] **D11 — UI 纯交互化：移除 UI 对 Core 的全部直接调用**
  - UI 只发 Action + 订阅事件；不再直调 player/parse/storage。grep 验证 UI 层无 Core 直调。
  - **验收**：UI 层零 Core 直调；全功能正常。
- [ ] **D12 — IFrontend 抽象 + 验证可换 UI**
  - 定义 `IFrontend`（订阅事件渲染 + 输入→Action）；ncurses UI 实现之。确认 Core 不依赖 ncurses、UI 可换（Qt 可后接）。
  - **验收**：ncurses 经 IFrontend；UI 可换性就位。→ **M1 达成（UI 解耦）**

## 里程碑 M2 — Provider 化 + Media 收敛（每 parser 一小步）
- [ ] **Dn** — 各 parser 确认 Provider 化（youtube/bilibili/itunes/rss/m3u/opml/tiktok）；Media 域从 TreeNode 逐步收敛。每个一进步、保持可运行。

## 里程碑 M3 — 字幕/ASR
- [ ] **Dn** — `SubtitleController` 中介者（修 READY/L-key 冲突）；ASR 作 SubtitleProducer。

## Connectivity 跟进（低优先，按需插入）
- Downloader 全面经 Connectivity（补 D3 未覆盖路径）+ 首批代理规则（youtube→代理、`*.googlevideo.com`→代理、bilibili→直连）。

## 规则
- 每个人日任务完成 → 本文件对应项打 `[x]` + `CHANGELOG.md` 一行 + 架构决策入 `DECISIONS_LOG.md`。
- 计划随实际演进更新（任务可拆并、可插入）；不顺就回退到上一个绿色提交。
- 每步必须保 `git` 可回退点（编译绿+可运行才 commit）。
