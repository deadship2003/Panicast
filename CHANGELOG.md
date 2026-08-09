# Changelog — Panicast

单一总日志，按 `[模块]` 分节记录。命名方案：基线 `Panicast_V0.1`；修正迭代 `Panicast_V0.1-F01`…`-F99`；帐号线迭代 `Panicast_V0.1-Y01`…`-Y99`；网络控制线迭代 `Panicast_V0.1-N01`…`-N99`。Y/F/N 线并行，互不影响。

---

## 规划 — 2026-08-09 — ASR "本地字幕文件优先" 缺口入计划（D11-3a 收口，延后实现）

> 用户提出 ASR 流程缺本地字幕文件优先逻辑。深挖 4 处不一致（remote `asr_start` 无本地检查 / track-load `find_sidecar` 漏下载目录 ASR SRT / `:asr` 注释与行为边界不清 / L 键优先链内联 UI 未收口）。**决策：不当下打补丁，收口进 D11-3a**——统一 `find_local_srt`+`find_sidecar` 为单一 `resolve_subtitle_source(node)` 解析器，3 个 ASR 入口（L 键/`:asr`/remote）+ track-load 全经此，"有本地字幕源（内嵌/本地SRT/online📜）就不跑 ASR" 统一生效。待 D11-2 视图态迁移解锁。详见 `DECISIONS_LOG.md`（D11-3a 规划）。**纯文档、无代码改动。**

---

## 新架构 D11-1 — 2026-08-09 — PlaybackTrackEnded：切掉字幕最后残留直调（播放↔字幕全解耦 · D11 增量1）

> M1（UI 解耦）第 14 步、D11 第 1 增量。D10-3 Step 2 后 PlaybackService 仅剩 2 处 `subtitle_svc_->stop_realtime()` 直接调用（on_playback_ended/play_current 入口，杀 ASR）。本增量新增 `PlaybackTrackEnded{}` 事件，两处改 publish；SubtitleService 订阅 → `stop_realtime()`。**PlaybackService 彻底删除 `subtitle_svc_` 指针、attach 的 SubtitleService 参数、前向声明、include**——播放域零 SubtitleService 引用，播放↔字幕所有直接耦合切断、全走总线。

### PlaybackTrackEnded（新事件）
- `struct PlaybackTrackEnded{};`（playback_events.h，无 payload）。publish 于 on_playback_ended 入口（曲目结束，含 error/stop 不进阶）+ play_current 入口（前曲被手动替换）——即所有"ASR 不可跨曲携带"的曲目边界。
- SubtitleService `init()` 订阅 → `stop_realtime()`（token 入 `subs_`，shutdown 先 unsubscribe）。

### PlaybackService（彻底去字幕耦合）
- 删 `SubtitleService *subtitle_svc_` 成员、`class SubtitleService` 前向声明、`#include "panicast/app/subtitle_service.h"`。
- `attach(ThreadPool&, SubtitleService&)` → `attach(ThreadPool&)`；app_run `attach(pool_, subtitle_)` → `attach(pool_)`。
- 2 处 `subtitle_svc_->stop_realtime()` → `EventBus::publish(PlaybackTrackEnded{})`。

### 行为等价性
- stop_realtime 触发点不变（同样两处入口、同样 UI 线程、EventBus 同步派发）；保守未折进 begin_track，**时序零变化**。stop_realtime 幂等（realtime_active_ 为假即 return），advance 路径 Ended + 后续无重复杀问题（begin_track 不调 stop_realtime）。

### 验收
- ctest 39/39、构建 0-warning（21/21）、pty 冒烟 exit 0 + clean endwin + quit dialog。
- 字幕/ASR 行为待人工验证（同 D10-3 Step 2，本增量仅改触发通道、逻辑未动）。

---

## 新架构 D10-3 Step 2 — 2026-08-08 — SubtitleController：字幕加载事件化（Option B 统一 · D10-3 增量2/2）

> M1（UI 解耦）第 13 步（续）。字幕加载从 PlaybackService 命令式直调改为**事件驱动**：PlaybackService 只 publish `PlaybackTrackChanged`（加 `has_video` 字段），SubtitleService `init()` 订阅 → `begin_track(node, has_video)`。**Option B（用户拍板）**：自动进阶走与手动播放**相同**的 A/B 分支（按 `has_video` 重新识别节目类型），不再只走 Method B——视频进阶升级到 Method A（mpv 渲染，修了潜在不一致）。= **D9 reactor 通道首个真实消费者**，播放↔字幕加载彻底解耦。

### PlaybackTrackChanged（加 has_video 字段）
- 事件加 `bool has_video = false`（默认初始化，故既有 `{node,mode}` 聚合初始化仍编译——单测 `PlaybackEvents.DeliveredOnBus` 不受影响）。
- `has_video` = **按曲目重新识别**的 A/B 标志（`is_youtube || URLClassifier::is_video(url)`，由 PlaybackService 计算入事件），**非** `node->is_video`（两者不同：PlaylistItem.is_video 源自前者）。

### SubtitleService（订阅事件触发 begin_track）
- init() `EventBus::subscribe<PlaybackTrackChanged>` → `begin_track(e.node, e.has_video)`（token 存 `subs_`）；shutdown() 先 unsubscribe 再拆引擎（防总线回调打到半销毁的服务）。
- 删 `load_transcript`（Step 1 的 advance Method-B-only 路径——Option B 后 advance 也走 begin_track，死方法）。

### PlaybackService（字幕加载改事件、降耦合）
- on_playback_ended advance：`load_transcript` 直调删除；`has_video` 快照上移、`PlaybackTrackChanged{next_node, mode, has_video}` publish（同步派发→订阅方 begin_track 在 play() 前跑，时序等价）。
- play_current：`begin_track` 直调删除；publish 改带 `has_video`。
- **残留（D11 切）**：`subtitle_svc_` 指针保留，仅用于 on_playback_ended 入口 + play_current 入口两处 `stop_realtime`（杀 ASR；"曲目结束但不进阶"路径无 PlaybackTrackChanged，故需直接调用）。直调从 4 处降到 2 处；加 `PlaybackTrackEnded` 事件可彻底拆。

### Option B 行为变化（仅自动进阶，需人工验证）
- 视频自动进阶：Method B（LYRIC 面板）→ **Method A（mpv 渲染烧字幕）**，与手动播放一致。
- 音频自动进阶：仍是 Method B（load_async），仅多一行 log。
- 手动播放：行为零变化（begin_track 逻辑未动，仅触发方式从直调变事件，同步派发时序等价）。

### 验收
- ctest 39/39、构建 0-warning（21/21）、pty 冒烟 exit 0 + clean endwin + quit dialog。
- **字幕正确性冒烟测不出 → 需人工放带字幕的播客/视频验证**：音频 Method B（LYRIC）、视频 Method A（mpv 渲染）、L 键 ASR、自动进阶重识别类型。

---

## 新架构 D10-3 Step 1 — 2026-08-08 — SubtitleController：字幕编排逻辑搬入 SubtitleService（行为等价 · D10-3 增量1/2）

> M1（UI 解耦）第 13 步。把 PlaybackService 里**命令式内联编排**的字幕逻辑（Method A/B 判定 + mpv sub_add + load_async + stop_realtime）**原样搬进** SubtitleService 三个方法（stop_realtime / begin_track / load_transcript）；PlaybackService 改走方法调用（触发方式不变=仍命令式直调）。纯重定位、行为零变化（字幕对象同一：init 传入的 pool/mpv/subtitle_mgr 即 PlaybackService 原用者）。这是 SubtitleController（= 提前 M3）的第一步 strangler-fig。

### SubtitleService（新增编排方法 + 存 pool_/mpv_）
- 新增 `stop_realtime()`（转发 transcription_engine_）、`load_transcript(node)`（advance 路径 Method B：subtitle_mgr_.load_async）、`begin_track(node, has_video)`（play_current 完整 A/B 块：has_video→reset+异步探 sidecar+mpv sub_add(A)/load_async(B)；!has_video→Method B load_async）。
- init() 新存 `pool_`/`mpv_` 指针（begin_track 的 pool 提交 + mpv sub_add 用；与 transcription_engine_ 共用同一对象）。
- `is_mpv_sub_url`/`basename_of` 两 helper 从 playback_service.cpp 匿名 namespace 搬入 subtitle_service.cpp（字幕域专用，随逻辑归位）。

### PlaybackService（删引擎裸指针、改方法调用）
- attach() 签名 `attach(ThreadPool&, SubtitleManager&, TranscriptionEngine&)` → `attach(ThreadPool&, SubtitleService&)`；删 `subtitle_mgr_`/`transcription_engine_` 两裸指针成员，换 `SubtitleService *subtitle_svc_`。
- 4 处字幕调用点改走方法：on_playback_ended 入口 stop_realtime + advance load_transcript；play_current 入口 stop_realtime + 整段 A/B 块塌缩为 `begin_track(pn, has_video)`。
- App::run 的 attach 调用 `attach(pool_, subtitle_.subtitle_mgr(), subtitle_.transcription_engine())` → `attach(pool_, subtitle_)`。
- playback_service.h 用 SubtitleService 前向声明（最小头耦合），.cpp include 全定义。

### 设计依据（主流反应式 vs 命令式反模式）
mpv/VLC/ExoPlayer 字幕都是反应式：媒体换→发事件→字幕组件订阅加载。PaniCast 现状（PlaybackService 命令式内联 + Method A/B 域知识泄漏进播放域）是反模式。Step 1 先把逻辑搬到字幕域（行为等价）；**Step 2** 才换触发方式为事件订阅（届时 PlaybackTrackChanged 终有首个真实消费者、播放↔字幕彻底解耦）。

### 验收
- ctest 39/39、构建 0-warning、pty 冒烟 exit 0 + clean endwin。
- 字幕行为等价性：纯 verbatim 搬迁（同一 pool/mpv/subtitle_mgr 对象）+ 编译/冒烟绿；字幕正确性的最终验证在 Step 2 后由人工放带字幕内容确认。

---

## 新架构 D10-5 + D10 完成 — 2026-08-08 — Account 勘察（无需切割）+ D10 收官（UI 解耦 · D10 增量5/里程碑）

> M1（UI 解耦）第 12 步 / **D10 收官**。勘察帐号域的 App 自有状态以定 AccountService 切割范围——结论：**无 App 自有状态可切**，D10-5 以"无需切割"结案。综合 D10-1～D10-4，D10 所有权切割目标全部达成。

### Account 域勘察（D10-5 结案）
- `AccountsManager` = **单例**（`static AccountsManager& instance()`，accounts.h:68）；app_account.cpp 全 ~25 处经单例调用（list/add/delete/update_tokens/set_active/load_subscriptions/load_history…）。
- `GoogleOAuth` = **静态工具类**（`GoogleOAuth::request_device_code`/`poll_token`/`fetch_identity`/`fetch_channel_videos`），无实例。
- 帐号数据 = DB + 树节点（树节点现归 LibraryService）。App 唯一沾边态 = `tiktok_region_`（一字符串）。
- 结论：帐号域**在 App 层无自有状态**——状态早已天然解耦（单例/静态/DB）。包成 AccountService 只是把单例调用再裹一层，纯 churn。帐号**逻辑**（mode handler）操作单例+树+UI，全方法搬迁属 D11。**不造空壳 Service。**

### D10 收官总结
- **4 个 Application Service 就位**：Playback（D8/D9）、Subtitle（D10-1）、Search（D10-2）、Library（D10-4）。App god-object 的**域数据块全部外迁**——不再持播放/字幕/搜索/库的域私有态。
- **持 App 自有状态的域全部切割完毕**；Account 域无 App 自有状态（天然解耦）。
- **遗留（统一归 D11 UI 纯交互化）**：① 各 Service **方法体/逻辑搬迁**（均直探 tree_mutex/display_list/selected_idx/has_video/player_.sub_add，被 UI 耦合挡住）；② 视图态 display_list/selected_idx/view_start + tree_mutex + pending_select_ 迁入 LibraryService；③ 字幕事件化（SubtitleService 订 PlaybackTrackChanged 自动加载字幕——首个 D9 reactor 真实消费者，作 D11 首步）。
- **验收（每步）**：ctest 39/39、构建 0-warning、pty 冒烟 exit 0 + clean endwin。提交链：b518e5e（D10-1）→ 13ddb2c（D10-2）→ a1d2da6（D10-4）→ 本次（D10-5/D10 收官，文档）。

---

## 新架构 D10-4 — 2026-08-08 — LibraryService 所有权切割：搬树数据模型（UI 解耦 · D10 增量4）

> M1（UI 解耦）第 11 步。把 App god-object 里最大的域数据块——树数据模型（8 个模式根 item 列表 + 6 个 loaded flag）——迁入**第四个 Application Service** LibraryService；`src/app/*.cpp` 全 184 处经 `\b` 词界 sed 统一重定向到 `library_` 访问器。行为零变化（纯所有权搬移 + 机械重定向，复刻 D10-1/D10-2/D8b-1）。

### LibraryService（拥有树数据模型，header-only）
- 新 `include/panicast/app/library_service.h`：私有持 8 个 `std::vector<TreeNodePtr>`（radio/podcast/fav/history/account/bilibili/tiktok/iptv `_root_`）+ 6 个 `bool`（radio/podcast/account/bilibili/tiktok/iptv `_loaded_`，初值 false）；非/常引用访问器（向量返 `vector&`/`const vector&`；flag 返 `bool&`/`bool`）。
- header-only（仅访问器、无逻辑）→ 无需 CMake .cpp 注册。

### App 重定向（184 处）
- App 删 app.h:130-134 的 8 root + 6 flag 裸成员，换 `LibraryService library_` + include。
- `src/app/*.cpp`：8 root token（175 处）+ 6 flag token（9 处）经 GNU sed `\b…\b` → `library_.xxx_root()` / `library_.xxx_loaded()` 统一重定向。词界 `\b` 保证 `load_radio_root()`、`load_accounts_root()`（复数）、`children_loaded`、`OnlineState::history_loaded` 等同名子串/无关标识符不受波及。
- 转换后计数逐 token 与 sed 前完全吻合（radio_root 30 / podcast_root 68 / fav_root 43 / history_root 6 / account_root 8 / bilibili_root 8 / tiktok_root 9 / iptv_root 3；flag 1/3/2/1/1/1）→ 零遗漏、零误伤。

### 边界决策（暂留 App）
- **tree_mutex 暂留 App**：recursive_mutex 同时守树数据（现归 LibraryService）+ 视图态 `display_list`/`selected_idx`（D11 领地）。锁若现迁入 LibraryService 仍要守 App 的视图态，且视图态 D11 会再迁——现迁=双 churn 且无同处收益。故锁待 D11 视图态一并迁入时跟随。当前: `lock_guard<recursive_mutex> lock(tree_mutex); library_.podcast_root().push_back(...)`（锁在 App、数据经访问器，等价、行为零变化）。
- **树构建方法 `load_*_root()` 留 App**：触 parser/storage/UI，全方法搬迁属 D11。
- **`pending_select_` 留 App**：pool 任务设、UI 线程消费设 selected_idx，是 UI handoff → D11。
- **验收**：ctest 39/39、构建 0-warning（22/22 链接）、pty 冒烟 exit 0 + clean endwin + quit dialog。第四个 Application Service 就位（Playback/Subtitle/Search/Library）。

---

## 新架构 D10-2 — 2026-08-08 — SearchService 所有权切割：搬树内搜索状态（UI 解耦 · D10 增量2）

> M1（UI 解耦）第 10 步。把树内搜索运行时状态（搜索词 / 命中列表 / 当前命中序号 / 总命中数）从 App 裸成员迁入**第三个 Application Service** SearchService，暴露访问器 + `reset()`；`app_search.cpp` 全部 4 成员读写改走 `search_` 访问器。行为零变化（纯所有权搬移 + 机械重定向，复刻 D10-1/D8b-1 访问器模式）。

### SearchService（拥有树内搜索状态）
- 新 `include/panicast/app/search_service.h` + `src/app/search_service.cpp`：私有持 `std::string search_query_` / `std::vector<TreeNodePtr> search_matches_` / `int current_match_idx_ = -1` / `int total_matches_ = 0`。
- 非/常访问器 `search_query()` / `search_matches()`（返回引用）+ `current_match_idx()`/`set_current_match_idx(int)` / `total_matches()`/`set_total_matches(int)`；`reset()` 一次清四态。

### App 重定向
- App 删 4 个搜索裸成员（`search_query` / `search_matches` / `current_match_idx` / `total_matches`），换 `SearchService search_` + include。
- `app_search.cpp`：`search_query = q;` → `search_.search_query() = q;`；所有 `search_matches`（push_back / 递归实参 / 循环 / move 赋值 / `.size()` / 下标）→ `search_.search_matches()`；两处 int 读写改 `set_/()` 访问器；`reset_search()` 体塌缩为 `search_.reset()`。
- `app_run.cpp` draw 调用点 4 形参（搜索词 / 当前序号 / 总命中 / …）改走 `search_` 访问器。
- **注意**：`jump_to_match(0)`（start_search 命中后跳首个）传字面量 `0`，非 `current_match_idx`——保留原语义。

### 结论与遗留
- **关键调研**：搜索/字幕/库/帐号的**输入处理与逻辑体**直探 UI/tree 耦合态（`tree_mutex`/`display_list`/`selected_idx`/`view_start`），干净的全方法搬迁被 UI 耦合挡住 → 属 **D11（UI 纯交互化）** 领地。故 D10 收尾策略改为**所有权切割**：逐域把 App 的域私有态外迁到各 Service（状态归位），为 D11 逻辑搬迁腾出干净边界。
- **验收**：ctest 39/39、构建 0-warning（19 文件含新 search_service.cpp）、pty 冒烟 exit 0 + clean endwin + quit dialog。第三个 Application Service 就位（Playback/Subtitle/Search）。

---

## 新架构 D10-1 — 2026-08-08 — SubtitleService 干净一刀：搬字幕/ASR 所有权 + 生命周期（UI 解耦 · D10 增量1）

> M1（UI 解耦）第 9 步。把 `SubtitleManager` + `TranscriptionEngine`（字幕检测/加载/状态机 + whisper.cpp 离线/实时 ASR）从 App 裸成员迁入**第二个 Application Service** SubtitleService，集中生命周期接线；App 的 ~16 处触发点改走访问器。行为零变化（纯所有权搬移 + 机械重定向，复刻 D8b-1）。PlaybackService 一行未动。

### SubtitleService（拥有字幕引擎 + 生命周期）
- 新 `include/panicast/app/subtitle_service.h` + `src/app/subtitle_service.cpp`：私有持 `SubtitleManager subtitle_mgr_` + `TranscriptionEngine transcription_engine_`。
- `init(ThreadPool&, MPVController&)`：内部 `transcription_engine_.init(&subtitle_mgr_, &pool, &mpv)`——把"引擎↔自己的 SubtitleManager"接线收进服务（替代 App 构造期的 `transcription_engine_.init(&subtitle_mgr_, &pool_, &player)`，inter-object 依赖不再外泄）。
- `shutdown()` → `transcription_engine_.shutdown()`；`poll(UI&, bool)` → `subtitle_mgr_.poll(...)`（系统拆除 / 每帧 handoff 入口）。
- 访问器 `subtitle_mgr()` / `transcription_engine()`：D10-1 重定向缝（复刻 D8b-1 队列状态访问器），D10-2/3 用 Action/事件替代。

### App（删引擎成员 + 触发点改访问器）
- `app.h`：删 `SubtitleManager subtitle_mgr_` / `TranscriptionEngine transcription_engine_`；加 `SubtitleService subtitle_`；两 include 合并为 `subtitle_service.h`。
- 触发点重定向（每处一行调用改名，无逻辑变化）：
  - `app_run.cpp`：构造 `subtitle_.init(pool_, player)`（原 `transcription_engine_.init(&subtitle_mgr_,…)`）；析构 `subtitle_.shutdown()`；`run()` 的 `playback_.attach(pool_, subtitle_.subtitle_mgr(), subtitle_.transcription_engine())`；主循环 `subtitle_.poll(...)` + lyric_active 读 `subtitle_.subtitle_mgr().status()` / `subtitle_.transcription_engine().realtime_running()`。
  - `app_input.cpp`（×10）、`app_remote.cpp`（×3）、`app_download.cpp`（×2）：`transcription_engine_.foo()`→`subtitle_.transcription_engine().foo()`、`subtitle_mgr_.foo()`→`subtitle_.subtitle_mgr().foo()`。
- **PlaybackService 不动**：`attach()` 仍收 `SubtitleManager&`+`TranscriptionEngine&`（App 在调用点传访问器），其内部裸指针用法 (`subtitle_mgr_->`/`transcription_engine_->`) 零变化——最小爆炸半径。

### 行为零变化
纯所有权搬移 + 调用重定向：SubtitleService 持有的两对象与原 App 成员构造/析构序等价（构造默认、init 接线时机同、析构前 shutdown 同）；所有调用经访问器转达到同一对象实例。无新逻辑分支、无线程变化。

### 测试
- ctest 39/39（无新增——纯机械搬迁）；构建 0-warning（-Wall -Wextra -Wpedantic，19 文件含新 subtitle_service.cpp）；pty 冒烟 exit 0 + clean endwin。

### 意义
第二个 Application Service 就位（继 PlaybackService）。为 D10-2（字幕按键→SubtitleActions 上总线）、D10-3（订 PlaybackTrackChanged 自动加载字幕 + 发 SubtitleStatusChanged）及 M3 SubtitleController 打底。

---

## 新架构 D9-3 — 2026-08-07 — BUFFERING 手柄 + 状态机迁入 PlaybackService（UI 解耦 · D9 增量2b · D9 完成）

> M1（UI 解耦）第 8 步，**D9 收官**。把 BUFFERING 运行时手柄 `playback_pending_(_start_)` 及其每帧状态机逻辑（pending 生命周期 + 30s 超时 + 一次性 buffering 时长日志）从 app_run 整段迁入 PlaybackService。App 不再持有/直接写任何播放运行时状态。行为零变化（逐帧 5-case 等价）。

### PlaybackService（拥有 buffering 状态 + 状态机）
- 新增私有成员 `playback_pending_`（bool）/ `playback_pending_start_`（steady_clock::time_point，header 加 `<chrono>`）。
- 新增 **`bool advance_buffering(bool mpv_has_media)`**：逐帧 buffering 生命周期 tick。`play_current`/`on_playback_ended` 启动加载时置 pending；mpv 报 has_media → 清 pending + 记一次 buffering 时长（Y24.17）；30s 超时 → 清 + 日志（Y23.9）。返回 true=仍在 pending（App 显 BUFFERING）。逻辑**逐字**自 app_run 状态机搬入，UI 线程跑（D4 不变量保持）。
- 新增私有 **`set_buffering_(bool)`** 单漏斗：写 `playback_pending_(_start_)` + publish `PlaybackBufferingChanged`（reactor 通道）。`play_current`（1×）、`on_playback_ended`（advance 1× + error 1×）原 3 处 `publish(PlaybackBufferingChanged{...})` 改调 `set_buffering_`；`advance_buffering` 的清除也走它。

### App（删最后播放状态成员 + 状态机精简）
- `app.h`：删 `playback_pending_` / `playback_pending_start_`；注释更新为"App 现仅持 play_mode（设置），无任何播放状态成员"。
- `app_run.cpp`：删 `PlaybackBufferingChanged` 订阅块；状态机由原 3 分支（has_media / pending / else）精简为：
  ```
  if (playback_.advance_buffering(state.has_media))      BUFFERING;   // pending, mpv 仍加载中
  else if (state.has_media)                              PLAYING/PAUSED/(idle)BUFFERING;  // 从 mpv 派生
  else                                                  BROWSING;    // 含刚超时的 pending
  ```
  PLAYING/PAUSED/(mpv-idle)BUFFERING 仍从 `state.core_idle`/`state.paused` 派生（非 pending 状态，留 app_run）。`HistoryChanged` 订阅保留（重建历史树）。

### 等价性（逐帧 5-case）
| 场景 | 旧行为 | 新行为 |
|---|---|---|
| has_media + pending | 记时长、清 pending、mpv 派生 | advance(true) 内记时长+清、返 false→mpv 派生 ✓ |
| has_media + !pending | mpv 派生 | advance(true) 返 false→mpv 派生 ✓ |
| !has_media + pending(<30s) | BUFFERING | advance(false) 返 true→BUFFERING ✓ |
| !has_media + pending(≥30s) | 清+日志、BROWSING | advance(false) 内清+日志、返 false→BROWSING ✓ |
| !has_media + !pending | BROWSING | advance(false) 返 false→BROWSING ✓ |

线程同旧（advance_buffering 在 app_run 主循环 = UI 线程；set_buffering_ 的 publish 同步、无订阅方→no-op）。D4 不变量保持。

### D9 收官
- PlaybackService 现**独占全部播放运行时状态**：队列（D8b-1）+ track 手柄（D9-2）+ buffering 手柄/状态机（D9-3）。App 持 play_mode（设置，按调用传入）+ 树逻辑（build_peer_list/is_playable_node/play_episode），订 `HistoryChanged` 重建历史树。track/buffering 事件保留 publish，待 D10+ remote/UI 直订。

### 测试
- ctest 39/39（无新增——逻辑搬迁、无新分支）；构建 0-warning（-Wall -Wextra -Wpedantic）；pty 冒烟 exit 0 + clean endwin（首跑一次 WSL2 启动抖动超时，连跑 3 次全绿）。

---

## 新架构 D9-2 — 2026-08-07 — "在播"手柄迁入 PlaybackService 私有化（UI 解耦 · D9 增量2a）

> M1（UI 解耦）第 7 步。把"在播曲目"运行时手柄 `playback_node`/`playback_mode_` 的所有权从 App 迁入 PlaybackService（私有 + 只读访问器）；App 删除镜像成员与 `PlaybackTrackChanged` 订阅，改经访问器读。行为零变化。BUFFERING 手柄 `playback_pending_(_start_)` 还被 app_run 每帧状态机直写，留 D9-3。

### PlaybackService（拥有"在播"状态）
- 新增私有成员 `playback_node_`（TreeNodePtr）/ `playback_mode_`（AppMode，默认 RADIO）；在 `play_current`、`on_playback_ended`（自动进阶设 next_node 时）直接写入——替代 D9-1 把 `publish(PlaybackTrackChanged)` 当"写回"手段。
- 新增只读访问器 `playback_node() const` / `playback_mode() const`（队列访问器之后的 `Track state` 节）。
- `PlaybackTrackChanged` **保留 publish**：意义从"App 镜像写回"转为"外部 reactor（remote 现在播放推送 / 未来 UI 直订）通道"；单测 `PlaybackEvents.DeliveredOnBus` 仍覆盖。同步语义不变。

### App（删镜像、经访问器读）
- `app.h`：删 `TreeNodePtr playback_node;` 与 `AppMode playback_mode_`；注释更新为"track 手柄已迁入服务、App 只剩 BUFFERING 的 pending_(_start_)"。
- `app_run.cpp`：删 `PlaybackTrackChanged` 订阅块；`ui.draw(...)` 改传 `playback_.playback_node()`；保存 player state 处标题改 `playback_.playback_node()` 读。`PlaybackBufferingChanged`/`HistoryChanged` 订阅保留。
- `app_navigation.cpp`（`jump_to_playing` / `'N'`）：缓存 `auto pn = playback_.playback_node(); auto pm = playback_.playback_mode();` 后用之。
- `app_input.cpp`：`:asr`（1×）、复制 URL 回退（1×）、`'L'` 字幕块（块首缓存 `pn`，含 `find_local_srt` lambda 经 `[&]` 捕获）改访问器。
- `app_remote.cpp`：状态快照 `art_url`、`asr_start` 处理改访问器。

### 等价性 / 线程
- 服务在 `play_current`/`on_playback_ended`（仍只在 UI 线程跑，D4 不变量保持）直接写私有成员——与 D9-1 经同步事件在 UI 线程更新 App 镜像，线程/时机一致。读取点（draw/输入/nav/remote）原本就在 UI 线程读，改访问器不引入新线程访问。行为零变化。

### 范围分拆
- 完整"运行时手柄私有化 + UI 展示经事件"耦合大，且 `playback_pending_(_start_)` 还被 app_run 每帧状态机直写（D4 现场附近）。本刀只私有化**单写者**的两个 track 手柄（干净、低风险）；pending_ + 状态机收尾留 **D9-3**（同 D8b-1/D8b-2 分拆原则）。

### 测试
- ctest 39/39（无新增——访问器是机械改名、无新逻辑；`PlaybackEvents.DeliveredOnBus` 仍守护保留的 track 事件）；构建 0-warning（-Wall -Wextra -Wpedantic）；pty 冒烟 exit 0 + clean endwin。

---

## 新架构 D9-1 — 2026-08-07 — 播放事件上总线，替代 D8b-2 回调缝（UI 解耦 · D9 增量1）

> M1（UI 解耦）第 6 步。建立输出侧（Core→App）事件通道：PlaybackService 经 EventBus 发播放状态事件，App 订阅——删除 D8b-2 的临时回调缝。总线同步派发，行为零变化。

### 新增事件类型（`include/panicast/app/playback_events.h`）
- `PlaybackTrackChanged { TreeNodePtr node; AppMode mode; }`——新轨道成为播放指针（源节点 + 跳回 'N' 用的 mode）。替代 `set_playback_node_` + `set_playback_mode_`（二者总是一前一后）。
- `PlaybackBufferingChanged { bool pending; }`——BUFFERING 标志翻转（true 时订阅方盖 `playback_pending_start_`；false→BROWSING）。替代 `set_pending_`。
- `HistoryChanged {}`——播放历史 DB 变更（订阅方异步重建历史树）。替代 `on_history_changed_`。

### PlaybackService（回调 → publish）
- `attach()` 瘦身：去掉 4 个 `std::function` 回调形参，只留 `(ThreadPool&, SubtitleManager&, TranscriptionEngine&)` 三指针注入。
- `play_current`/`on_playback_ended`：`set_playback_node_+set_playback_mode_` → `publish(PlaybackTrackChanged{...})`；`set_pending_(x)` → `publish(PlaybackBufferingChanged{x})`。
- `record_play_history`：`pool_->submit([this]{ on_history_changed_(); })` → `pool_->submit([this]{ publish(HistoryChanged{}); })`。
- `playback_service.h` 去掉 `<functional>` 与 4 个回调成员。

### App（订阅替代回调）
- `run()`：`playback_.attach(pool_, subtitle_mgr_, transcription_engine_)`（3 参）；新增 3 个 `EventBus::subscribe`（token 入 `action_subs_`）：`PlaybackTrackChanged`→`playback_node`+`playback_mode_`；`PlaybackBufferingChanged`→`playback_pending_(_start_)`；`HistoryChanged`→`load_history_to_root()`。

### 等价性 / 线程
- EventBus `publish` **同步**派发在调用线程 → 与回调的线程/顺序完全一致：`play_current`/`on_playback_ended` 在 UI 线程 publish（订阅方在 UI 线程更新手柄）；`record_play_history` 在 pool 线程 publish `HistoryChanged`（订阅方在 pool 线程跑 `load_history_to_root`，同旧）。D4 不变量保持。

### 测试
- `tests/test_units.cpp` +`TEST(PlaybackEvents, DeliveredOnBus)`：三事件在总线上的 round-trip（delivery + payload）。

### 验收
- ctest 39/39、构建 0-warning（-Wall -Wextra -Wpedantic，含 test_units）、pty 冒烟（启动→订阅接线→主循环渲染→q/y 退出，exit 0 + clean endwin）。

### 意义
- 输出侧 service→App 事件通道建立；D8b-2 的临时回调缝删除（消息总线成 service→App 唯一通道）。为 D9-2（事件驱动 UI 展示 + 把运行时手柄迁入服务私有化）铺路。

## 新架构 D8b-2 — 2026-08-07 — PlaybackService 接管播放/自动进阶逻辑（UI 解耦 · D8 增量2b）

> M1（UI 解耦）第 5 步（D8 增量2 的第二刀）。把播放**逻辑**迁入 PlaybackService（队列状态已在 D8b-1 内）；运行时手柄经回调缝写回 App，读取点留 D9。

### 迁入 PlaybackService（播放逻辑）
- **方法**：`play_current` / `on_playback_ended` / `record_play_history` / `resolve_youtube_url`（4 个）→ PlaybackService。迁入后队列状态是其私有成员，直接访问（不再经 `playback_.` 访问器）。
- **静态助手**：`is_mpv_sub_url` / `basename_of`（play_current 字幕处理用）→ `playback_service.cpp` 文件内匿名命名空间。

### 依赖后注入 `attach()`（解决声明序）
- `pool_` / `subtitle_mgr_` / `transcription_engine_` 在 app.h 中声明于 `playback_`（行 126）**之后**（行 164/405/406），无法作构造期引用 → PlaybackService 持指针，`App::run()` 在 `playback_.init()` 后调 `playback_.attach(pool_, subtitle_mgr_, transcription_engine_, <4 回调>)` 注入。
- 迁移的方法对运行时手柄**只写不读**，故 4 个 `std::function` 回调即够：
  - `set_playback_node(TreeNodePtr)` → `App::playback_node =`
  - `set_pending(bool)`（true 时一并盖 `playback_pending_start_ = now`）→ `App::playback_pending_(_start_)`
  - `set_playback_mode(AppMode)` → `App::playback_mode_ =`
  - `on_history_changed()` → `App::load_history_to_root()`（原 `record_play_history` 直调，现解耦）

### 仍在 App（Option Y：回调缝，D9 事件层替代后迁入私有化）
- 运行时手柄 `playback_node` / `playback_pending_(_start_)` / `playback_mode_` 物理位置不动；读取点（app_input 21×、app_run 每帧状态机、nav/remote）零改动——避免 ~50 个低价值改名。
- `play_mode` 是设置（多点写），留 App，按调用以形参传 `play_current(idx, mode, play_mode)` / `on_playback_ended(reason, mode, play_mode)`。
- `build_peer_list` / `is_playable_node` / `play_episode` 留 App（树逻辑）；`play_episode` 末尾改 `playback_.play_current(idx, mode, play_mode)`。
- 移除 `load_transcript` / `probe_local_sidecar` 两个薄委托（仅被迁移的方法用，PlaybackService 直调 `subtitle_mgr_`）。

### 调用点
- `on_playback_ended`：app_run 的 D4 drain `on_playback_ended(_end_reason)` → `playback_.on_playback_ended(_end_reason, mode, play_mode)`（仍 UI 线程）。
- `play_current`：`App::play_episode` + app_remote next/prev → `playback_.play_current(idx, mode, play_mode)`。

### 注意（lambda 捕获）
- `play_current` 的 YouTube 异步 pool lambda 原靠 `this->play_mode`（成员）读 play_mode；迁入后 play_mode 是形参 → **显式捕获** `play_mode`（`[this, …, play_mode]`）。on_playback_ended 的异步 lambda 不用 play_mode，无需此改动。

### 验收
- ctest 38/38、构建 0-warning、pty 冒烟（启动→attach→主循环渲染→q/y 退出，exit 0 + clean endwin）。

### 意义
- PlaybackService 现拥播放**逻辑**（D8b-1 已拥队列状态）→ App 只剩运行时手柄 + 设置 + 树逻辑。回调缝是临时桥，D9 事件层（PlaybackStateChanged 等）一上即可把手柄迁入服务并私有化、去掉回调。

## 新架构 D8b-1 — 2026-08-07 — PlaybackService 接管播放队列状态（UI 解耦 · D8 增量2a）

> M1（UI 解耦）第 4 步（D8 增量2 的第一刀）。把播放"队列状态"的所有权迁入 PlaybackService；复杂播放逻辑（play_current/on_playback_ended）留 D8b-2。

### 迁入 PlaybackService（队列状态 + 纯队列逻辑）
- **状态**：`current_playlist` / `current_index` / `shuffle_queue_` / `playlist_mutex_`（4 个成员，原 App 私有）→ PlaybackService 私有。
- **方法**：`clear_playlist` / `refill_shuffle_queue` / `random_peer_index`（3 个，纯队列逻辑、无跨切依赖）→ PlaybackService。
- **公开访问 API**：`playlist_mutex()` / `playlist()` / `current_index()` / `set_current_index()` / `shuffle_queue()` / `clear_playlist()` / `refill_shuffle_queue()`——调用方先锁 `playlist_mutex()` 再访问，**锁语义与重构前完全一致**（同一把 mutex、同样的 guard 点；`refill_shuffle_queue` 仍不自锁、由调用方持锁）。

### 仍在 App（故意外延到 D9 事件层再迁）
- `play_mode`（全局设置，多输入点写）、`playback_node`（app_input 21× ASR 读取 + UI 读）、`playback_pending_(_start_)`（app_run 每帧状态机）、`playback_mode_`——运行时手柄。`play_current`/`on_playback_ended`/`build_peer_list` 仍是 App 方法，经 `playback_.` 访问队列。
- **D4 不变量保持**：`on_playback_ended` 仍是 App 方法，调用点（app_run 的 `pending_end_reason_` drain，UI 线程）未动→它绝不跑在 mpv 事件线程，"暂停后 TUI 冻结"修复不回退。

### 调用点
- `clear_playlist()` 两处外部调用（app_input 'C' 键、app_remote "clear_playlist" 命令）→ `playback_.clear_playlist()`。
- app_run 绘制快照、app_remote 状态快照/next-prev、app_playback 三函数：队列访问全改 `playback_.`。

### 验收
- ctest 38/38、构建 0-warning、冒烟正常（pty 下 TUI 全渲染、无崩溃）。

### 意义
- PlaybackService 现为播放队列的**唯一所有者**（App 不再直接持队列状态）——D9 事件层的前置（服务拥状态才能发事件）。

## 新架构 D8a — 2026-08-05 — PlaybackService 接管播放 Action（第一个 Application Service）

> M1（UI 解耦）第 3 步（D8 增量1）。功能抽象层起步：PlaybackService 处理播放类 Action。

### 新增
- **[App] `include/panicast/app/playback_service.h` + `src/app/playback_service.cpp`**：`PlaybackService`（持 `MPVController&`，`init()` 订阅 `PlayPause/VolumeUp/VolumeDownAction` → handler 调 player）。
- App `PlaybackService playback_{player};` 成员；`run()` 调 `playback_.init()` 取代 App 内联的 pause/音量订阅。

### 意义
- pause/音量现经 **UI→Keymap→Action→总线→PlaybackService→player**——首个 **Application Service（功能抽象层）** 就位。复杂播放状态（current_playlist/auto-advance）迁入留待 D8b。

### 验收
- ctest 38/38、构建 0-warning、冒烟正常。

## 新架构 D7 — 2026-08-05 — Keymap + 迁移 pause/音量/导航到 Action（UI 解耦）

> M1（UI 解耦）第 2 步。引入键→Action 映射，5 个常用交互经总线，UI 不再直调 Core。

### 新增
- **[App] `include/panicast/app/actions.h`**（重写）：`Action = std::variant<PlayPause/VolumeUp/VolumeDown/NavUp/NavDown>` + `publish_action`（`std::visit`→`EventBus::publish`）。
- **[App] `include/panicast/app/keymap.h`**：`Keymap`（键 int → Action），`bind/lookup/contains`。键绑定单一来源，可重绑。
- **[App] `build_keymap()`**：绑默认键（space/p→PlayPause、+/-→VolumeUp/Down、k/j→NavUp/Down）。
- **handle_input**：switch 前先查 Keymap（命中→`publish_action`→return）；移除 pause/音量/导航 旧 case。

### 效果
- pause / 音量± / 导航(k/j) 5 个交互现走 **键→Keymap→Action→总线→handler**；UI 不再直调 `player.toggle_pause/set_volume` / `nav_up/down`。键绑定集中在 Keymap（可重绑，`[keys]` INI 覆盖为后续）。

### 验收
- ctest 38/38、构建 0-warning、冒烟正常。
- 注：page/seek/模式切换/复杂流程（搜索/账号/bilibili）仍走 switch，待续。

## 新架构 D6 — 2026-08-05 — Action 类型 + 暂停走消息总线（UI 解耦种子）

> M1（UI 解耦）第一步。UI 输入首次经消息总线，不再直调 Core。

### 新增
- **[App] `include/panicast/app/actions.h`**：`PlayPauseAction`（后续 Action 类型随 D7 迁移加入）。Action = UI→Core 输入方向，复用 EventBus 承载（`publish(PlayPauseAction{})`）。
- **暂停走总线**：`app_input.cpp` Space/'p' 键改为 `EventBus::publish(PlayPauseAction{})`（不再直调 `player.toggle_pause()`）；`App::run()` 订阅 `PlayPauseAction` → 调 `player.toggle_pause()`（已是 worker 异步）。

### 意义
- 首个 **UI→Core 经总线**闭环（输入侧）。UI 暂停路径不再直接碰 player——"UI 纯交互层"解耦的第一步（见 `docs/DESIGN.md` 目标架构）。后续 D7 迁移更多输入、D8 抽 PlaybackService 接管这些 handler。

### 验收
- ctest 38/38、构建 0-warning、冒烟正常；暂停经总线工作。

## robust — 2026-08-05 — mpv 交互命令移到 worker 线程（TUI 永不被 mpv/PA 阻塞）

> 用户要求：不管 mpv 状态，TUI 都要响应交互。根因（前一条日志）：暂停触发 `pa_stream_cork` 卡在挂死的 PulseAudio → mpv 阻塞 → UI 线程直接调 `mpv_set_property` 冻结。

### 改动
- **[Playback] MPVController 新增命令 worker 线程**（`cmd_thread_` + 队列 + condition_variable）：`toggle_pause/set_pause/set_volume/adjust_speed/reset_speed/set_speed` 的 mpv 调用改为 `enqueue_cmd_` 投递到 worker；UI 线程立即返回（`state_` 乐观更新）。
- **效果**：mpv/PA 卡死时，阻塞的只是 worker；**UI 线程继续渲染 + 读输入**（乐观 state_ + event 线程 update_state 刷新）。暂停/音量/速度不再冻 TUI。
- worker 关停：`stop()` bounded-join（≤1.2s）+ detach 回退（与 event 线程同模式，靠 `_exit` 退出）。

### 验收
- ctest 38/38、构建 0-warning、冒烟正常。
- 注：`get_state()` 本就返回缓存（update_state 在 event 线程刷新、mpv 读取在锁外）→ 不阻塞 UI；本次堵的是命令侧（set_pause 等）。

### 局限/后续
- 其余 mpv 调用（play/loadfile、seek、loop/keep_open、sub_add）仍在调用线程；若也阻塞可同法迁 worker。
- 治本仍需修 WSL PulseAudio（`wsl --shutdown`）；本改动让 PA 挂死时 TUI 不再冻死。

## fix — 2026-08-05 — 定位"看视频/暂停时 TUI 输入无响应"= mpv 视频窗口抢键盘焦点

> 用户 panicast.log（看 IPTV 时复现）揭示真正根因，纠正 D4/D5 的"死锁"误判。

### 根因（确认，非死锁）
- 日志 `input: No key binding found for key 'l'/'a'/SPACE/','/MBTN_LEFT'`：mpv 视频窗口（vo=gpu/wlshm）打开时**抢了键盘焦点** → 按键被它接收、丢弃 → ncurses TUI 收不到。看视频即发生（与暂停无关，暂停只是更易察觉）。
- 排除死锁：watchdog 未触发慢帧、ncurses 无跨线程破坏、END_FILE 已 D4 迁 UI 线程。

### 修复（卫生）
- `mpv_set_option_string(ctx, "input-default-bindings", "no")`：TUI 拥有全部输入，mpv 视频窗口不解释键（消除 "No key binding" 吃键日志 + 防 mpv 自带绑定与 TUI 冲突）。

### 限制 + 绕过
- **抢焦点本身是 WSLg/Wayland 窗口行为**，应用内无法强制终端保持焦点。**绕过**：视频窗口打开后点一下终端窗口即恢复 TUI 键盘控制。根治需 WSLg/WM 层（如 focus-new-windows=off）。

### 另（日志独立问题）
- WSL 音频坏（`ao/pulse Init failed: Timeout` / `ao/alsa Connection refused` → AO init failed → 播放失败），与 focus 无关，需修 WSL 音频（PulseAudio/PipeWire）。

## 新架构 D5 — 2026-08-05 — M0 收尾 + UI 帧时间 watchdog（定位"暂停后输入无响应"）

> 新架构增量迁移 M0 第 5 人日（收尾）。D1–D4 已把 EventBus/Connectivity/Media 串通（app 本身即端到端集成）。

### 新增
- **[App] UI 帧时间 watchdog**（`app_run.cpp`）：每帧测 `tree_mutex` / `playlist_mutex_` 等待时长 + 整帧耗时；任一超阈值（锁等待 >80ms / 帧 >150ms）写 `panicast.log`（`[WATCHDOG] ...`）。用于精确定位"暂停播放后 TUI 输入无响应"的剩余卡点——下次复现时日志直接显示卡在拿哪把锁或哪一步。

### M0 达成
- 最小可演进系统就位：EventBus（D1）+ IProxyManager/Connectivity 全消费者接入（D2/D3）+ Media/MediaID（D4）+ 工程基线；D4 已修 END_FILE 跨线程锁争用。后续 M1（Downloader 全覆盖 + 热键 Keymap）起增量扩展。

### 关于"暂停后输入无响应" bug
- D4 已修一处根因（`on_playback_ended` 原在 mpv 线程持 `playlist_mutex_` → 改入队、UI 线程 drain）。已排除：ncurses 跨线程破坏（只在 ui/app）、ASR join 阻塞（`stop_realtime` 非阻塞）。剩余卡点需运行时数据——watchdog 会在复现时记录确切位置。**请复现后把 `panicast.log` 里的 `[WATCHDOG]` 行发我，即可定点修复。**（若你暂停时开着 Shift+L 实时转写，也可能是 ASR worker 在暂停后仍持续转写整段占 CPU——可一并告知。）

### 验收
- ctest 38/38、构建 0-warning、冒烟正常。

## 新架构 D4 — 2026-08-05 — Media/MediaID 骨架 + 修复"暂停后 TUI 无响应"

> 新架构增量迁移 M0 第 4 人日。落地 Media 领域句柄；顺带修一个跨线程锁争用导致的 TUI 卡死 bug。

### 新增
- **[Domain] Media / MediaID**（`include/panicast/domain/media.h`，header-only）：`MediaID`（弱引用 TreeNode 身份，可比较/可过期）+ `Media`（只读视图：id+url+title）+ `media_from_node` adapter。不改 TreeNode；后续里程碑逐步收敛 Media 表面、与树解耦。

### 修复
- **[App] 暂停播放后 TUI 一段时间无响应**（`app.h`/`app_run.cpp`）：根因——`on_playback_ended` 在 **mpv 事件线程**锁 `playlist_mutex_` 执行（`fs::exists`/`classify`/`pool_.submit`/`player.play`），与 UI 主循环 draw 持有的 `playlist_mutex_` 争用；暂停久了直播流空闲断开 → END_FILE → mpv 线程持锁阻塞 UI draw → 卡死（AUDIT P1-4/5/8 同源）。改：mpv 线程 end_file 回调只 `pending_end_reason_.store(reason)`；UI 线程每帧 `exchange(-1)` drain 后在**自己线程**跑 `on_playback_ended` → 消除跨线程锁争用（EventBus 迁移方向）。
- README 去掉全部历史版本注记（Y24.30/Y01/Y15/Y11/Y13/Y24.11 等），公开文档按全新项目呈现。

### 验收
- ctest 35→38（+3：MediaID 身份/过期、Media 视图）；增量构建 0-warning；`build/panicast --version` 正常。

## 新架构 D3 — 2026-08-05 — 全部网络消费者接入 Connectivity（含 Downloader）

> 新架构增量迁移 M0 第 3 人日。apply_network_proxy 改 url-aware，4 个 curl 调用点 + yt-dlp 全部经 IProxyManager。

### 变更
- **[Net] `apply_network_proxy` 改为 `(curl, url, platform)`**：`network.cpp` / `bilibili_api.cpp` / `itunes_search.cpp` / `app_download.cpp` 4 个 curl 调用点传真实 url（启用域名规则）+ platform（bilibili→"bilibili"、itunes→"podcast"、通用→""）。
- **[Net] ytdlp_runner（`--proxy`）接入 ProxyManager**：从直读 `IniConfig::get_proxy()` 改为 `ProxyManager::resolveProxy("").url`——yt-dlp 下载也经 Connectivity（mpv 播放仍直连）。至此所有网络消费者（Parser/Downloader/解析/yt-dlp）统一经 IProxyManager。
- README 去掉内部版本注记（Y05/Y11），公开文档更正式。

### 验收
- ctest 35/35 通过；增量构建 0-warning；`build/panicast --version` 正常；代理路径无回归。

## 新架构 D2 — 2026-08-05 — IProxyManager（Connectivity 统一网络前端）

> 新架构增量迁移 M0 第 2 人日。落地"所有网络消费者经统一代理层"的接口（Downloader 切换在 D3）。

### 新增
- **[Net] IProxyManager + ProxyManager**（`include/panicast/net/proxy_manager.h` / `src/net/proxy_manager.cpp`）：`ProxyConfig{url}` + `resolveProxy(url, platform)` 规则链（平台→域名→全局→直连），线程安全。全局源**可注入**（`setGlobalSource(std::function)`）——proxy_manager.cpp 不依赖 IniConfig，单测可干净链入。
- **[Net] apply_network_proxy 接入 Connectivity**：`network.cpp` 把 `[network] proxy` 注入 ProxyManager 全局源（Ctrl+N 实时生效），apply_network_proxy 改走 `resolveProxy`——首个真实消费者，行为零变化（无平台/域名规则时即返回 [network] proxy）。

### 测试
- test_units +5 用例（全局源、无源直连、平台覆盖全局、域名匹配、平台优先于域名）。ctest 30→35 全过；编译 0-warning；`build/panicast --version` 正常。

## 新架构 D1 — 2026-08-05 — EventBus 核心 + EventLog 首个生产者

> 新架构增量迁移 M0 第 1 人日（开发计划 D1）。引入类型安全事件总线——替换 `pending_select_`+散落回调（竞态 P1-4/5/8 根因）的承重墙。

### 新增
- **[Core] EventBus**（`include/panicast/core/event_bus.h`，header-only）：类型安全 pub/sub，线程安全；`subscribe<E>` 返回 token、`unsubscribe(token)`、`publish<E>` 同步派发（订阅者锁内快照、锁外派发，handler 可递归 publish/unsubscribe 不自死锁）。
- **[Core] EventLog::push 顺带 publish(LogEvent)**（`src/core/event_log.cpp`，`LogEvent` 定义于 `event_log.h`）：首个真实生产者——所有日志行上总线，未来订阅者（远程日志推送/调试覆盖层）订阅 `LogEvent` 而非轮询 EventLog。无订阅者即空操作，行为零变化、无 init-order 风险。

### 测试
- test_units +4 用例（subscribe/publish、多订阅者+unsubscribe、无订阅者安全、LogEvent 路由）。ctest 26→30 全过；增量构建 0-warning；`build/panicast --version` 正常。

## PaniCast-V0.01-F01 — 2026-08-04 — ASR 实时转写修复 + YouTube 播放/下载可靠性

> 修复用户反馈的两个 BUG：①ASR 非异步、播放时卡等 ASR、且未正常唤起 whisper-cli；②YouTube 不能正常播放/下载。

### 修复
- **[ASR] 实时转写重写为「分块 + 可中断」**（`src/subtitle/transcription_engine.cpp` `realtime_worker`）。旧设计在调 whisper-cli **之前**用一次阻塞、不可中断的 `ffmpeg -i <url>` 解码**整段源**：直播流（Bloomberg/FOX 等 tunein 直播）该 ffmpeg 永不返回 → whisper-cli 根本没被唤起；远程播客要先整段下载（长卡顿）。且无停止钩子，`stop_realtime()`（仅 bump gen）杀不掉 ffmpeg → worker 线程泄漏 → `start_realtime()`/`shutdown()` 的 `realtime_thread_.join()` **阻塞 UI 线程**（即"播放时卡等 ASR"）。
  - 新设计按短块捕获：有限/可 seek 媒体用 `ffmpeg -ss <start> -t <chunk>`，直播流用 `-t <chunk>`（不 seek），每块 ffmpeg **自终止**而非无限捕获；chunk 默认 30s，`[transcription] realtime_chunk_sec` 可调（5–120）。
  - ffmpeg 经 `-progress pipe:1 -stats_period 1` 把进度写 stdout，`run_process_streaming` 的 stop_pred 每秒轮询 → `stop_realtime()`/换轨/关停在 ~1s 内杀掉在途 ffmpeg，UI 线程不再阻塞、线程不再泄漏。whisper-cli 本就按行输出 segment，同样可中断。
  - 每块 whisper 转写后，segment 时间戳 **+块起点偏移**映射回源时间线，累加并渐进喂给 LYRIC（音频）/OSD（视频）。有限媒体循环到 ≥duration，直播循环到被停止。
  - 验证：分块模拟（jfk.wav [0,6]s/[6,12]s 两块）→ 段时间戳连续正确（第二块 +6s 后 06.000–11.000）；编译 0-warning。
- **[YouTube] 播放解析超时可配 + 重试**（`src/app/app_playback.cpp` `resolve_youtube_url`、`include/panicast/config/ini_config.h`）。播放解析用固定 **30s** yt-dlp `-g` 超时——经 SOCKS 代理 + quickjs/ejs 解 nsig，单次解析合理耗时 30–60s，30s 上限导致间歇性"YouTube resolve failed"（日志里精确 30s 的 YtdlpRunner timeout；而下载路径正确用了 3600s）。
  - 已核实 nsig solver 本身可用：app 注入 `--js-runtimes quickjs:<qjs>` 后 yt-dlp 报 `JS runtimes: quickjs-ng-0.15.1` 且 `yt_dlp_ejs-0.8.0` 在场（不注入则为 none）；日志 08-01 亦显示解析多次成功。故失败为延迟/抖动，超时+重试是对症修复。
  - 新增 `[youtube] resolve_timeout_sec`（默认 90）、`resolve_retries`（默认 3，含首次）；失败/超时按重试循环再解析，并在 LOG/EVENT_LOG 记录每次尝试。
- **[退出] 退出时杀全部子进程（含孙进程）**（`src/app/app_run.cpp` `run()` 收尾）。`_exit(0)` 跳过 `~App`，而 `kill_all_child_processes()` 原只在 `~App` 里 → 退出时只有**直接子进程**经内核 `PR_SET_PDEATHSIG` 被杀，**孙进程**（如 yt-dlp 的 ffmpeg 合流子进程、下载派生的 ffmpeg）被 init 收养继续跑（用户即观察到"退出后仍有残留进程"）。修复：在 `ui.cleanup()`（终端已恢复）之后、`_exit(0)` 之前显式调 `Utils::kill_all_child_processes()`——被跟踪子进程各自是进程组长(pgid==pid)，`kill(-pgid)` 整组终结含孙进程；~200ms 的 SIGTERM→SIGKILL 宽限不可见，`_exit(0)` 随即回到 shell PROMPT。异步即时退出 + 全子进程回收两全。
- **[退出] 关闭 wlshm 视频窗口（修"退出后 MPV 窗口残留"）**（`src/playback/mpv_controller.cpp` `stop()`）。`stop()` 原本发完 `stop`/`quit` 异步命令后**立即 detach** 事件线程 → VO 的 `wl_surface` 销毁（即关 wlshm/WSLg 窗口的那一步）与随后的 `_exit(0)` 竞态，进程已退但窗口变 WSLg 幽灵窗口——播放视频后最明显。修复：`stop()` 在 detach 前对 `mpv_thread_done_`（event_loop 见到 SHUTDOWN、即 VO/AO 已 uninit 后置位）做**有界等待**（≤~1.2s）——VO 正常 <100ms 即关窗并 join；若 WSLg 上 VO teardown 病态挂起（原 fire-and-forget 的初衷）则超时 detach 不无限阻塞。`stop()` 仅在退出路径调用（非逐曲），故该延迟仅退出时发生。
- 二进制 `build/panicast` 全量编译通过（45/45，-j2，0-warning）。

## Panicast_V0.1-N07 — 2026-08-01 — titlebar-as-root 数据模型重构 + 退出 typeahead 修复

> 消除 per-mode 根节点（数据模型 E 化）：8 个模式根 `TreeNodePtr` → `std::vector<TreeNodePtr>`（根节点不再存在，一级条目直接在 vector 里），`current_root` 移除。`online_root` 作为跨模式 LINK 目标保留。另修退出时 typeahead 残留。

### 重构（E：root → vector）
- **8 个 per-mode 根**（radio/podcast/fav/history/account/bilibili/tiktok/iptv）由 `TreeNodePtr` 容器节点改为 `std::vector<TreeNodePtr>` 顶层条目列表——根节点从数据模型消除（不再有"标题栏 + 根行重复"的遗留容器）。
- `current_root`（TreeNodePtr）**移除**；新增 `items_for_mode(mode)`/`cur_items()` 按模式取条目列表；`flatten_items()` 遍历一级条目（根结构性不进显示域）。
- 容器函数（count/clear/collect/remove）新增 `_current` 助手循环 `cur_items()`；`sort_target` 重写（顶层排序 `cur_items()` + `cur_sort_reversed` 成员）；`search_recursive`/`reveal`/身份比较适配；parent 指针 `=xxx_root`→`reset()`；`*_loaded` 根节点标志改 App 成员 bool；`Persistence::save_cache/load_cache` 改 vector 签名；`get_root_by_mode_string` 返回 `vector*`。
- **`online_root` 保留**（审计确认是跨模式 LINK 目标：收藏的"Online Search"是 LINK 节点指向它 `linked_node`/指针身份/LINK 展开），向量化会破坏 LINK 功能；已不可见（显示遍历其 children），属功能性节点非遗留。
- 删掉 `flatten` 的 `title=="Root"` 魔法字符串（flatten 退化为纯递归）。
- 编译 0-warning；9 模式 smoke + 排序/展开/搜索/删除(go_back/parent/mark/delete DB 62→59) 深度回归全通过，无崩溃。

### 修复
- **退出 typeahead 残留**：关停期间（主循环停止读取后）敲入的键留在终端输入队列，退出后 shell 接续回显/执行（如 `kjkkj`）。修复：`restore_terminal_state()`/`restore_termios_async()` 末尾 `tcflush(STDIN, TCIFLUSH)` 排空输入队列；shell 启动干净。验证：优雅退出 code 0、termios 恢复 ICANON/ECHO/ISIG。
- 版本号 → N07（六处同步）。

## Panicast_V0.1-N06 — 2026-07-31 — MediaType 分类（DB 驱动的显示图标）

> history/favourites 存 `media_type` 列，显示时直接用 DB 的分类图标，取代每次渲染从 URL 推断。核心诉求：**特定平台优先于通用类型**——YouTube 不被当 ONLINE VIDEO、m3u8(IPTV) 不被当 RADIO。

### 新增
- **MediaType 枚举**（`types.h`）：Radio/Youtube/Bilibili/Tiktok/Iptv/OnlineAudio/OnlineVideo/LocalAudio/LocalVideo 共 9 类。
- **classifyMediaType(url)**（`url_classifier.h/.cpp`）：平台优先（复用 URLClassifier）；**非纯 URLType 映射**——因 URLType 把 `.m3u8` 并入 RADIO_STREAM、把本地/在线都并入 VIDEO_FILE/RADIO_STREAM，故在 classify 之上加 2 个前置判断：① `file://`/绝对路径→LocalVideo/LocalAudio；② `iptv:`/`.m3u8`/`.m3u`→Iptv。DOUYIN_* 归 Tiktok（占位）。
- **media_type_icon(MediaType)**（`icons.h`）：9 类图标，**全部 glibc wcwidth=2**（📻📹Ｂ🎵📺🎤🎬🎶🎥）。
- **图标宽度约定 + Bilibili 修复（覆盖重打）**：Bilibili 原 `🅱️`(U+1F171) 是 **glibc wcwidth=1、终端渲染为2** 的字形；而 **ncurses 按 glibc(libc) wcwidth 推进光标渲染**，二者错位 → 该行边框偏移（应用层的 `mk_wcwidth` 改不了 ncurses 的 libc 渲染）。**统一约定（`icons.h` 注释明示）：所有图标一律用 glibc wcwidth=2 的字形**——Bilibili 改用全角 `Ｂ`(U+FF22)、Account 标题改 `Ｙ`(U+FF39)（glibc=终端=2，ncurses 三者一致，必然对齐）；回退之前的 mk_wcwidth U+1F1xx 权宜补丁。实测 `glibc wcwidth(Ｂ)=2`、`utf8_display_width("Ｂ ")=3`=icon_field_width。
- **强制/崩溃退出 termios 恢复（覆盖重打）**：第二次 Ctrl+C（或崩溃信号）走 `_exit` 强制退出路径，原只写转义恢复序列（退副屏/显光标/RIS），**没恢复 termios**（行规程 ISIG/ICANON/ECHO）→ 退出后 shell 把 ESC 显示成 `^[`、Ctrl+C 显示成 `^C`（ISIG 关）。修复：新增 `restore_termios_async()`（async-signal-safe 的 `ioctl(TCSETS, &g_original_termios)`），在两条强制退出路径调用。pty 实测：2× Ctrl+C 后 termios 恢复为 ICANON/ECHO/ISIG（恢复前只剩 ISIG）。
- **Schema 46→47**（`database.cpp`）：history/favourites 加 `media_type INTEGER` 列；**一次性回填**旧行（classifyMediaType(url)）。实测用户库：39 条 m3u8→IPTV、13 bili→Bilibili、5 视频文件→OnlineVideo、5 电台→Radio，零误伤。
- **落库**（`history_repo.cpp`/`tree_repo.cpp`）：add_history/save_favourite 内部计算并写 media_type（无调用方改动）；get_history/load_favourites 取出。
- **显示**（`tree_renderer.cpp`）：history/favourites 叶节点用 `media_type_icon`；容器收藏/feed 仍显示文件夹图标；实时树不变。
- 版本号 → N06（六处同步）。

### 验证
- 编译 0-warning；DB 副本 schema 46→47 + 回填全部正确；Ｂ 三层(glibc/终端/app)宽度均=2 验证通过；版本号六处同步。

## Panicast_V0.1-N05 — 2026-07-31 — 'r' 键跨模式统一刷新（Y/B/T 修复）

> 修复 Y 模式 'r' 无差别全量 resync、B 模式 'r' 长期 no-op、T 模式无法刷新缓存三处问题；'r' 键统一为"按节点刷新最新数据并替换本地缓存"。另修 Q 退出后终端无干净换行（`tui_cleanup` 缺尾换行）。

### 修复
- **Y 模式 'r' 过度重载**（`app_input.cpp` 'r' 分支）：原在任意节点按 'r' 都触发整账号全量 resync（重 + 折叠整树）。改为按节点类型分流：account 节点→`resync_account_node`（保留全量）；Subscriptions/History 容器→新增 `refresh_account_subs/history`（`app_account.cpp:456/470`），仅重抓该子树、保留其余展开。
- **B 模式 'r' 长期 no-op**（`app_bilibili.cpp:313/319/326`）：man/帮助原误标 "Refresh node"，`app_input.cpp:373` 注释称 re-fetches 从未实现。新增 `refresh_bili_followings/refresh_bili_history/refresh_bilibili_account`，'r' 强制重抓关注/历史或重展开账户。
- **T 模式 'r' 无法刷新**（`app_nodes.cpp:526`）：`refresh_node` 新增 T 分支——创作者节点按 'r' → `spawn_load_feed`(TIKTOK_USER) 在线重抓(yt-dlp) → `commit_feed_result` 替换 children + `episode_cache`(DEL+INS)，解决本地缓存过期。
- **account 节点按 mode 消歧**：`is_account` 被 T 创作者复用，'r' 分流中 Y/B account 靠 `AppMode::ACCOUNT/BILIBILI` 区分，避免误匹配。
- **Q 退出无干净换行**（`ui.cpp` `tui_cleanup`）：退出时已正确 `endwin`+恢复副屏/光标，但末尾未输出换行；副屏退出把光标还原到启动前那一行，shell 提示符便粘在该行 → "退出后没有干净换行"。末尾加 `printf("\n")`（恢复 termios 后转 CRLF），光标落新行、提示符干净出现。pty 抓包验证：退出字节结尾由 `\x1b[?25h` 变为 `\x1b[?25h\r\n`；Q→Y / Ctrl+C→Y 均 exit 0、终端完整恢复。
- 文档同步：`man/panicast.1` 'r' 条目 + `ui.cpp` Y/B/T 帮助行修正；版本号 → N05（六处同步）。

### 验证
- 编译 0-warning（Linux 全量重编 18 + 增量 3 通过）；六处版本号同步；功能检验表(Sheet50) + 修复记录(Sheet53) 同步。

## Panicast_V0.1-N04 — 2026-07-29 — PIN 鉴权 + UDP 发现 + WebSocket + 内嵌 BS 客户端

> 浏览器/手机可直接控制 PaniCast：IE 打开 http://地址:端口/ 即控制；APK 扫描网络发现播放器 + PIN 配对。本版含完整服务端 + BS 客户端 + APK 源码工程。

### 新增（服务端）
- **PIN 鉴权**（`remote_server.h/.cpp` + `remote_session.cpp`）：动态 4 位 PIN（`regenerate_pin()`，`:pin` 弹窗显示、`:newpin` 轮换）+ 万能 PIN **6696**（无屏场景）；localhost 连接开放（IE 在本机打开→直接控制）；非本机需 `password <PIN>`。`pin_test.py` 验证：LAN 无 PIN→ACK、错 PIN→ACK、6696→OK、status→OK。
- **UDP 网络发现**（`discovery_loop` + `[remote] discovery_port=18430`）：APK 广播 `PANICAST_DISCOVER`，PaniCast 回 `PANICAST 1 tcp=<port> ws=<port+1>`。`discovery_test` 验证通过。
- **WebSocket 前端**（`remote_ws.h/.cpp` + `panicast_web_index.h`）：RFC6455 握手（SHA1+base64，OpenSSL）+ 帧编解码（掩码处理/ping-pong）。端口 = TCP+1。socketpair 桥接 RemoteSession（PRP 不变）。GET / 返回**内嵌** BS 客户端（单 HTML，无外部文件）。`ws_test.py` 验证：握手 101 + greeting 帧 + ping/status/volume 端到端 PASS。
- **`RemoteSession` 重构**：读写双 fd（`read_fd_`/`write_fd_`）+ `closed_` 标志，使 WS 桥接可经 socketpair 喂入同一 PRP 引擎（TCP 两 fd 相同）。
- **App 接入**：`:pin`/`:newpin` 命令（`app_input.cpp`）；启动时 EVENT_LOG 显示 PIN + 浏览器地址。
- 版本号 → N04（六处同步）。

### 新增（BS 客户端，内嵌）
- 单页 HTML+JS（IE11 兼容，无框架）：PIN 配对遮罩、正在播放（标题/模式/进度条插值/时间）、控制（上/下/进/退/播放暂停/音量/变速/9 模式/repeat/shuffle/cycle/sleep）、idle 订阅实时状态、1.5s status 校准、断线自动重连。GitHub Dark 视觉。

### 新增（APK 源码工程，`apk/`）
- 原生 Kotlin + Jetpack Compose + Material3，minSdk 24。无运行时依赖（Gradle 自带 Compose）。
- UDP 广播扫描发现播放器 → PIN 配对（动态/6696）→ TCP PRP 控制 + 完整查看。
- 4 屏：扫描列表 / PIN / 播放控制 / 状态。含 Gradle 配置 + AndroidManifest + 构建说明（Android Studio 打开即编译签名 APK）。

### 验证
0 warning 编译。`pin_test`/`discovery_test`/`ws_test` 全 PASS。HTTP GET / 返回 200 + 内嵌 BS 客户端。

### 待办
- APK 实机编译（需 Android Studio；本环境无 Android SDK，仅出源码）。
- log/tree 显式 notify；search/mark/edit/download/subtitle/asr/queue 命令覆盖；BS/APK UI 打磨。

---

## Panicast_V0.1-N03 — 2026-07-29 — idle 事件订阅 + 状态同步推送

> 实现"状态同步"核心：远程客户端 `idle` 订阅子系统，服务端在状态变更时推送 `changed: <subsystem>`。路线图精简：N02 已吸收原计划 N03–N06 的控制命令覆盖，故本版序号为 N03（对应原路线图的 N07 idle 里程碑）。

### 新增
- **`RemoteSession::handle_idle`**（`remote_session.h/.cpp`）：MPD `idle [subsystems]` 语义。session 进入订阅态后用 `poll(fd, 100ms)` 多路复用——既等服务端 `notify_change` 推送，又监听客户端 `noidle`/排队命令/断开；命中变更则回 `changed: <subsys>` 行 + `OK`，`noidle` 则回 `OK` 退出。`notify_change(subsys)` 跨线程安全（`idle_mtx_`，去重）。
- **`RemoteServer` 会话注册表 + `notify()`**：`register_session`/`unregister_session`/`notify(subsys)` 广播给所有 idling 订阅者。
- **差分轮询线程 `diff_loop`**：10Hz 拉 `snapshot_state()`，比对 player(state/song/title/url/elapsed)/mixer(volume)/options(speed/play_mode/sleep)/mode/subtitle/art，命中即 `notify()`。100ms 轮询即去抖合并窗口；捕获 mpv 自身变化（seek/pause/换轨）无需 App 插桩。
- **重构 `run()`/`poll_line`**：`recv_buf_` 改为成员，`poll_line(out, timeout_ms)` 共享缓冲区，供 `run()` 阻塞读与 `handle_idle` 多路复用复用。
- 问候语改用 `VERSION` 常量自动跟踪（`OK Panicast_V0.1-N03`，MPD 风格）。
- 版本号 → N03（六处同步）。

### 验证
0 warning 编译。`idle_test.py`：A 连接 `idle mixer` → B 连接 `volume 42` → A 收到推送 `changed: mixer` + `OK` → PASS。差分轮询→notify→session 推送全链路打通。`prp_test.py` 仍通过（greeting/ping/status/control 端到端）。

### 待办
- N04：WebSocket 前端（自写 RFC6455）+ 静态资源托管 —— 让浏览器能连。
- N05：BS Web 客户端 v1（GitHub Dark、PC 三栏、控制+LOG 流、idle 状态同步）。
- 后续：search/mark/edit/download/subtitle/asr/queue 命令覆盖；log/tree 显式 notify。

---

## Panicast_V0.1-N02 — 2026-07-29 — PRP 协议引擎 + 状态查询 + 控制命令端到端

> N 线协议定稿落地。基于 MPD 风格行协议（PRP），实现"远程终端复刻本地键盘"的控制闭环 + 线程安全状态查询。三份设计文档：`PANICAST_N_LINE_PLAN.md` / `PANICAST_N_LINE_PROTOCOL_DESIGN.md` / `PANICAST_N_LINE_UI_DESIGN.md`。

### 新增
- **协议类型与控制接口**（`include/panicast/net/remote_protocol.h`）：`RemoteStateSnapshot`（player+app 状态 POD）、`RemotePlaylistItem`、`RemoteControlInterface`（App 实现的抽象接口，服务端依赖接口而非 App —— 可组合）。
- **`RemoteSession`**（`include/panicast/net/remote_session.h` + `src/net/remote_session.cpp`）：每连接 PRP 引擎，跑在 worker 线程。行解析（MPD 风格 token 化，支持双引号参数）；查询命令（`status`/`currentsong`/`playlistinfo`/`ping`/`password`）即时从快照应答；控制命令经 `RemoteCommandBus` 转发 UI 线程；`OK`/`ACK [code@0] {cmd} msg` 响应；问候 `OK PaniCast N02`。鉴权：token 非空时除 `password` 外命令回 `ACK [5@0] {cmd} auth required`。`idle`/`noidle` 占位（N07 实装）。Windows stub。
- **`RemoteServer` 升级**：`start()` 增加 `control` + `auth_token` 参数；`handle_client` 改为创建 `RemoteSession` 并 `run()`；`next_client_id_` 原子计数。
- **App 接入**（`app.h` / `app_run.cpp` / `app_remote.cpp`）：`App : public RemoteControlInterface`；`snapshot_state()` 加锁返回快照副本；`update_remote_state_cache()` 主循环每帧在 UI 线程构建快照（player state + mode/play_mode/selected/playlist/art/sleep/subtitle，`playlist_mutex_` 下拷贝）；`dispatch_remote()` 映射核心控制命令到既有方法。
- **控制命令覆盖（N02 核心）**：play/pause/resume/play_pause/stop/next/previous、seek/seekto/seek_percent（mpv 透传）、volume/volume_up/down、speed/speed_up/down/reset、repeat/shuffle/cycle/set_mode、sleep/sleep_cancel、mode/mode_next/mode_prev、nav_up/down/top/bottom/page_up/down/back/enter/select、sort_toggle、mpv 原生透传。search/mark/edit/download/subtitle/asr/queue 标记为 N03–N05。
- **版本号 → N02**（六处同步）。

### 验证
0 warning 编译（`-Wall -Wextra -Wpedantic`）。Python PRP 测试客户端（`prp_test.py`）连 :18421：问候 `OK PaniCast N02`；`ping`→`OK`；`status`→16 字段 key:value；`volume 55` 后 `status` 回显 `volume: 55`（控制端到端生效）；`play_pause`/`nav_down`/`mode PODCAST` 转发 `OK`。默认 `enable=false` 本地 TUI 不受影响。

### 待办
- N03：search/mark/edit/download/subtitle/asr/queue 命令覆盖（对齐键位表全量）。
- N07：idle 事件订阅（显式 notify + 10Hz 差分 + 去抖）。
- N08：WebSocket 前端（自写 RFC6455）+ 静态资源托管。
- N09：BS Web 客户端 v1。

---

## Panicast_V0.1-N01 — 2026-07-29 — 网络控制线立项 + 服务端骨架

> 新增 N 线（Network control），基线 `Panicast_V0.1-Y24.56`。目标：远程终端（先 IE 浏览器 BS，后安卓 APK）实现本地终端控制的全部功能 + 实时监视。本版仅交付地基骨架，命令协议待定稿（参考 MPD/ncmpcpp 路线，待用户确认）。

### 新增
- **版本号切到 N 线**：`PANICAST_FIX_SUFFIX="N01"`，六处同步（CMakeLists / vcpkg.json / constants.h / man / README / version.h.in）。`--version` = `Panicast_V0.1-N01`。
- **`[remote]` 配置段**（`ini_config.h` + 默认模板）：`enable`(默认 false, opt-in) / `port`(8421) / `bind`(127.0.0.1) / `auth_token`(空)。默认关闭 → 本地 TUI 零影响。
- **`RemoteCommandBus`**（`include/panicast/net/remote_command_bus.h` + `src/net/remote_command_bus.cpp`）：网络线程→UI 线程的命令队列，mutex 保护、`drain_all()` 非阻塞 swap、`shutdown()` 原子标志。这是服务端线程与 UI 线程之间唯一的越界点（UI 非线程安全）。
- **`RemoteServer`**（`include/panicast/net/remote_server.h` + `src/net/remote_server.cpp`）：自写 C++ POSIX socket TCP 服务端骨架。accept 线程 + 跟踪式 worker 线程（atomic done 标志 + reap，无 detach 孤儿线程）；`start()`/`stop()` 优雅停机（close listen fd 解阻塞 accept → join accept 线程 + 全部 worker）。N01 仅回 banner 关闭，协议在 N02+。Windows 平台编译为 stub（远程控制 Linux 优先，与 mpv 依赖一致）。
- **App 接入**（`app.h` / `app_run.cpp` / 新增 `app_remote.cpp`）：`run()` 启动前按 `[remote] enable` 启动服务端；主循环每帧 `drain_remote_commands()` 在 UI 线程出队派发；析构先 `remote_server_.stop()` + `remote_bus_.shutdown()` 再拆其余成员。N01 的 `dispatch_remote()` 仅记日志，动作映射在 N02–N06。

### 验证
0 warning 编译（77→51 增量，`-Wall -Wextra -Wpedantic`），`--version`=N01。运行时冒烟：`[remote] enable=true port=18421` → `bash /dev/tcp` 连接成功收到 banner `PaniCast remote control — protocol not implemented yet (N01 skeleton)`；timeout 退出优雅 join 无挂起。默认 `enable=false` 时不启动服务端，本地 TUI 行为不变。

### 待办（N02+）
- 命令协议定稿（MPD/ncmpcpp 风格行协议，待用户解释细节后确认）。
- `dispatch_remote()` 动作映射：播放控制 / 导航 / 模式 / 节目管理 / `:` 命令全集（N03–N06）。
- 状态快照 `App::snapshot_remote_state()`（加锁）+ 实时事件推送（N02/N07）。

---

## Panicast_V0.1-Y24.56 — 2026-07-28 — IPTV 播放诊断：LOG 区事件消息

> 用户报：播放 IPTV 时，地址正确但电视台当前未广播→无视频流→mpv 不开视频窗，用户不知原因；30s 超时只说"stream may have failed"，无法区分地址错/未开播/无音频设备。WSL2 音频设备故障时 PULSE/ALSA 错误溢出致花屏。

### 新功能 / 修复
- **[iptv] 13 条 LOG 区事件消息**（`mpv_controller.cpp` + `app_playback.cpp`）：枚举 IPTV 播放"不开窗/不起播"的全部情况，在屏幕 LOG 区输出简练英文事件（同时落 panicast.log）。
  - 前缀约定：`MPV:` = mpv 行为（原因，沿用既有 `mpv_error_str`）；`IPTV:` = IPTV 语境解释。同一事件同时触发两者时按时间先后各输出一条（MPV: 先、IPTV: 后），均经 `EVENT_LOG` 进屏幕+文件。
  - 覆盖：不可达/404/403/5xx（`-13` 按日志关键词细分）、空播放列表（`-16`）、AO 初始化失败（`-14`）、VO 初始化失败（`-15`）、缺解码器（`-17`）、**已连接但无流数据=可能未开播（轮询检测 #5）**、纯音频频道（#7）、网速过慢（#11）、**播放中途断流（#12，按 `PLAYBACK_RESTART` 是否触发区分）**、30s 超时兜底（#13，去括号）。
  - **#5 与 #13 天然互斥**：#5 要求 `FILE_LOADED`（has_media=true），#13 仅在 has_media 始终不出现时触发，不双发。
- **`[iptv] offair_detect_secs`**（默认 12s，INI 可调）：#5 的"无数据持续多久才报未开播"窗口，慢链路可调大。检测在有数据/编码到达时取消，避免慢启动误判。
- **`set_iptv_context(bool)`**：app 在 `mode==IPTV` 播放前置位，控制器据此决定是否输出 `IPTV:` 消息。

### 验证
0 warning 编译（74/74），`--version` = Y24.56。需用户在 WSL2 实测：未开播频道→#5；404 频道→#2。

### 旧问题（Y24.55 已修，本次一并发布）
- WSL2 PULSE/ALSA stderr 溢出致花屏：`mpv_initialize()` 后永久重定向 stderr→/dev/null（mpv 自身 AO 错误仍经 log 回调进 LOG 区）。

---

## Panicast_V0.1-Y24.50 — 2026-07-28 — 新增 I 模式（IPTV，iptv-org 源）

> 接入 GitHub 最全 IPTV 源 `iptv-org/iptv`（CC0，8000+ 频道，自动更新），按全部/地区/国家/分类/语言/自定义浏览。m3u 实时拉取+缓存（不打包陈旧快照）。

### 新功能
- **新模式 `AppMode::IPTV` + 热键 `I`**（第 9 模式，Tab 循环 `%8→%9`）。状态栏标题 `📺 IPTV` / `[I] IPTV`。
- **内置目录**（`app_iptv.cpp`）：All Channels / By Region / By Country / By Category / By Language / Custom。顶层分组硬编码；分组内国家/分类列表从 iptv-org API JSON（countries.json 等）拉取；展开某项拉对应 `.m3u` → 解析为频道树（按 `group-title` 折叠）。
- **m3u 解析器**（`parsers/m3u_parser`）：解析 `#EXTINF`（name/tvg-logo/group-title）+ 流 URL。
- **实时拉取+缓存**（`<data_dir>/iptv-cache/`，TTL `[iptv] cache_hours` 默认 24h）：数据始终最新，不膨胀 tarball。异步 `pool_` + loading spinner。
- **播放**：频道为可播放叶节点，走现有 mpv 路径（vo=auto 开视频窗；`--quiet` 纯音频）。
- **INI `[iptv]`**：`base_url` / `api_url`（可换镜像）/ `cache_hours` / `custom_urls`（用户自定义 m3u）。
- **法律**：iptv-org 数据 CC0；频道流可能受地区限制——README/man 免责（用户自行负责）。

### 验证
0 warning 编译。需用户实测：`I` 进入 IPTV → 展开 By Country → 选 China → 拉取 cn.m3u → 选频道播放。

---

## Panicast_V0.1-Y24.49 — 2026-07-28 — B 模式：扫码显示用户名 + D 下载修复

> 用户报：① B 站扫码登录后显示 UID 而非用户名；② B 模式 D 下载失败。

### 修复
- **[net] nav 取用户名带 buvid3**（`bilibili_api.cpp`）：`fetch_nav` 原只发 `SESSDATA=...`，被 B 站风控拒（nav 返回 -101 → 回退 `"Bili #<uid>"` 显示 UID）。改为用现成的 `build_bilibili_cookie(sessdata)`（带 `buvid3`/`b_nut`，与 WBI 调用同源）→ nav 成功 → 显示真实 uname。加前置声明（`build_bilibili_cookie` 定义在 `fetch_nav` 之后）。
- **[download] B 站/抖音/TikTok 视频走 yt-dlp**（`app_download.cpp`）：原 BILIBILI_VIDEO 等落入"Normal download"（直接 curl）→ curl 拉的是 HTML 网页非视频流 → 下载失败。新增分支：BILIBILI_VIDEO/DOUYIN_VIDEO/TIKTOK_VIDEO 用 yt-dlp + 站点 cookies 文件（bilibili/tiktok）下载。
- **[download] 抽取 `ytdlp_download` 公共 helper**（DRY）：YouTube 与 B 站/抖音/TikTok 共用同一 yt-dlp 下载核心（参数构建 + 进度解析 + 校验 + 缓存）。下载超时 600→3600s（长视频不致被杀）。

### 验证
0 warning 编译。需用户实测：B 站扫码后账户显示用户名（非 `Bili #<uid>`）；B 站视频 D 下载成功出 mp4。

---

## Panicast_V0.1-Y24.48 — 2026-07-28 — LYRIC 默认不开，仅在可显示字幕/ASR 时自动开

> 用户报：播放 B 站视频（无在线/内嵌字幕）时 LYRIC 默认开了空面板。要求：默认不开，仅在检测到内嵌/在线字幕且已解析可显示、或手动 ASR 成功运行时才自动开。

### 根因
`app_run.cpp` 的自动激活条件 `is_lyric_bar_requested() && has_active_subtitle()`：`requested` 默认 true（INI `lyric`），`has_active_subtitle()` 只判"字幕轨存在"（B 站 CC 轨存在但无内容）→ 误开空 LYRIC。

### 修复
- **自动开 LYRIC 改为"确有可显示内容"**（`app_run.cpp`）：仅当 ① 字幕 transcript READY（在线/本地 SRT 解析完成）② ASR 运行中（`realtime_running`）③ 内嵌字幕本轨已出现过 sub_text（`embedded_sub_confirmed`，置位后本轨粘住避免间隙闪烁）—— 三者之一才自动开。默认（无字幕）**不开**。
- **新增 per-track 三态手动覆盖**（`ui.h` `LyricManual{Auto,Open,Closed}`）：L 开 → `Open`（即使尚无内容，如 ASR 启动瞬间）；L 关 → `Closed`（本轨抑制自动开）；换台重置为 `Auto`。
- **`update_lyric_history` 每帧调用**（即使 LYRIC 未开），以便检测内嵌 sub_text 置位 `embedded_sub_confirmed` 触发自动开。
- VO 开（视频窗口）→ LYRIC 恒关（字幕在视频窗）。

### 验证
0 warning 编译。需用户实测：B 站视频（无字幕）应不再默认开 LYRIC；TED mp4（有内嵌字幕）首个字幕出现时自动开 LYRIC；音频节目 L 启动 ASR 后开 LYRIC 显示进度。

---

## Panicast_V0.1-Y24.47 — 2026-07-28 — 音频模式自动只拉音频流（省带宽）+ 开发脚本

> 用户需求：`--quiet`/VO=null 音频模式播放在线视频时，只拉音频流、不拉视频流以省带宽。

### 优化
- **[playback] YouTube/自适应流音频模式选音频格式**（`resolve_youtube_url`）：新增 `MPVController::is_audio_only_mode()`（判定 `vo=null`/`vid=no`，来自 `--quiet` 或 INI）。音频模式时即使节目是视频，也选 `play_format_audio`（bestaudio）→ yt-dlp 返回纯音频流 URL → **视频流不下载，省带宽**。仍走 `play_video`（vo=null 不开窗 + sub_file 字幕照常加载）。
- **[dev] 个人开发脚本** `~/panicast-dev.sh`（源码树外，不进 Git）：一键 解压→建 secrets+cp client_secret.json→build→install→启动。每次测试从 8 步降到 1 条命令。密钥不进源码（`.gitignore` 已排除 `secrets/`）。

### 限制（无法修）
直接复用的 mp4 URL（如 TED `download.ted.com/...mp4`）：音视频在 `mdat` 交错复用，mpv 必须顺序下载整文件才能取音频 → `--quiet` 只省 CPU/解码，**不省带宽**。需服务端提供独立音频流才行。

### 验证
0 warning 编译。需用户实测：`--quiet` 播放 YouTube 视频应只拉音频流（日志 `audio_only=1, fmt=...audio`）；TED mp4 仍整文件下载（格式限制）。

---

## Panicast_V0.1-Y24.46 — 2026-07-28 — LYRIC 内嵌字幕：合并多行 + 超宽滚动 + 修复历史花屏

> 用户实测：Y24.45 只在"当前行"拆 `\n`，未处理"历史行"——含 `\n` 的多行字幕成为历史上一条时，`\n` 仍留在 prev 字符串里，`mvwprintw` 跳行 → 花屏。用户要求：每个时间戳的多行字幕合并为一行，居中显示，超宽水平滚动；成为历史后也在上一行单行居中/滚动。

### 修复 / 优化
- **[ui] 字幕入历史前合并换行**（`update_lyric_history`）：mpv `sub-text` 的 `\n`/`\r` 在 push 进 `lyric_history_` 前替换为空格 → 每条字幕恒为**一行**。从源头消除 `\n`，根治历史行花屏。
- **[ui] fallback 复用 transcript 渲染**（`draw_lyric_content`）：当前行 + 历史上一条改用与 transcript 路径**相同的 `draw_row`**——超宽 `get_scrolling_text` 水平自动滚动、短则居中。当前行垂直居中（row mid），历史在上一行（row mid-1）。一行一字幕，无 `\n` 到达 `mvwprintw`。
- 一致性：fallback 与 transcript 路径渲染同构（同一 `draw_row`）。

### 验证
0 warning 编译。需用户 WSL2 复测：TED mp4 纯音频 + L 打开 LYRIC，多行字幕合并为一行居中、超宽滚动；切到下一条后，上一条在顶行单行居中/滚动、不花屏。

---

## Panicast_V0.1-Y24.45 — 2026-07-28 — LYRIC 歌词式布局（垂直居中 + 历史上一行）

> 用户优化：单行字幕在 LYRIC 区垂直居中（3 行空间的中间行），上面显示上一条历史字幕，下面留给下一条；长行不换行直接居中（LYRIC 区宽度大）。

### 优化
- **[ui] fallback 字幕布局**（`lyric_renderer.cpp`）：内嵌 mp4 字幕（sub-text）改为歌词式：
  - 当前行**垂直居中**（3 行中的中间行），水平居中加粗。
  - **上一条历史字幕**显示在顶行（非加粗，作为上下文）。
  - **不换行**：每条按 `\n` 拆分后，每段直接 `truncate_by_display_width` 截断到面板宽度 + 居中（LYRIC 区宽，多数不截断）。
  - 单行：顶=上一条，中=当前，底=空；两行：顶=上一条，中下=当前两行；三行：满行当前。
- 限制说明：mpv `sub-text` 仅暴露**当前**字幕条（无前瞻），内嵌字幕底行无法显示"下一条"；SRT/ASR/在线 transcript 走 transcript 路径，已是上一条/当前/下一条三行居中（`idx-1/idx/idx+1`），无需改。

### 验证
0 warning 编译。需用户 WSL2 复测：TED mp4 纯音频 + L 打开 LYRIC，单行字幕应垂直居中、上方显示上一条、不压边框。

---

## Panicast_V0.1-Y24.44 — 2026-07-28 — LYRIC 多行内嵌字幕修复

> 用户实测：TED mp4 纯音频模式 L 打开 LYRIC，含 `\n` 的两行内嵌字幕第二行跑到 LYRIC 边框、不居中。

### 修复
- **[ui] sub-text 多行处理**（`lyric_renderer.cpp`）：mpv mov_text 字幕的 `sub-text` 含内嵌 `\n`；`wrap_text` 不按 `\n` 分行（把 `\n` 当 width=-1 控制字符留在串里），导致 `mvwprintw` 遇 `\n` 跳到下一行 col 0（边框行）→ 第二行压边框、不居中。改为**先按 `\n` 拆分**（去 `\r`），每段独立 `wrap_text` 折行+居中，cap 到 `rows` 行、底部对齐。无 `\n` 进入 `mvwprintw`。

### 验证
0 warning 编译。需用户 WSL2 复测：两行内嵌字幕应各自居中显示在 LYRIC 面板内、不压边框。

---

## Panicast_V0.1-Y24.43 — 2026-07-28 — 字幕流程统一（L / ASR / LYRIC 面板）

> 用户报：TED mp4（内嵌 mov_text 字幕）VO 开时字幕在视频窗口正常；VO 关纯音频时字幕只在 INFO/LOG 区显示且换行居中乱码；Shift+L 打开底部 LYRIC 无反应（误触发 ASR）。重新设计统一 L 逻辑：显示目标与 ASR 模式由 `vo_open`（mpv 视频窗口是否渲染）驱动，本地字幕永远优先于在线。

### 重构 / 修复
- **[subtitle] L 键统一流程**（`app_input.cpp`）：
  - **vo_open**（视频窗口）：一次性确保视频窗口有字幕（mpv 控制显隐）。优先级 内嵌 > 本地 ASR SRT > 在线 📜 > 视频 ASR。内嵌存在则只 LOG 不做 ASR。
  - **!vo_open**（音频 / 视频不开 VO）：切换底部 LYRIC 面板。打开时取源（本地优先）：视频 内嵌 > 本地SRT > 在线 > 音频ASR；音频 本地SRT > 在线 > 音频ASR。
  - **L 不再停止 ASR**；ASR 由换台 `play_current` / 播放结束 `on_playback_ended` 自动停。
  - **ASR `is_video = vo_open`**（非 has_video 轨道标志）→ 视频不开 VO 走音频 ASR 喂 LYRIC，与音频一致。
- **[subtitle] 识别内嵌字幕**：新增 `MPVController::has_active_subtitle()`（查 mpv `track-list` 有 `type=sub`，比 sub-text 可靠——后者在字幕间隔期为空）；新增 `is_video_window_open()`（`current-vo`≠null/空）。
- **[subtitle] English 偏好**：mpv `slang=en`（INI `[mpv] sub_lang` 默认 `en`），多内嵌轨自动选 English。
- **[ui] 删除 INFO/LOG 内嵌 LYRIC 区**（`info_panel.cpp`）；字幕只在底部 LYRIC 面板或 mpv 视频窗口显示。
- **[ui] 修复底部 LYRIC 面板换行居中**（`lyric_renderer.cpp`）：sub-text fallback 由 `get_scrolling_text`（跑马灯）改为 `wrap_text` 折行+每行居中+当前行加粗。
- **[subtitle] LYRIC 面板激活规则**（`app_run.cpp`）：vo_open 时底部栏恒关；!vo_open 且有内嵌字幕（subtitle_mgr 无 transcript）时激活底部栏（经 sub-text fallback 显示）。
- 新增 `UI::set_lyric_bar_requested(bool)`（L 开关用，避免 toggle 翻转歧义）。

### 验证
0 warning 编译。需用户 WSL2 实测：TED mp4 VO 开→视频窗口字幕（内嵌，English）；VO 关→按 L 打开底部 LYRIC 显示内嵌字幕（折行居中）；音频节目→L 加载在线/本地 SRT 或启动 ASR 于 LYRIC。

---

## Panicast_V0.1-Y24.42 — 2026-07-28 — 总体审计修复（数据访问收口 + i18n）

> 按 `DEVELOPMENT_PRINCIPLES.md` 做总体审计并修复全部发现。审计维度：i18n / 并发安全 / 数据访问 / DRY-结构。

### 修复
- **[storage] 全量参数化查询**：消除 `escape_sql` + `fmt::format` 字符串拼接 SQL（违反准则六「只用参数化查询」）。涉及 `tree_repo`/`feed_cache_repo`/`history_repo`/`player_state_repo` 共 ~24 处 → 改为 `sqlite3_prepare_v2` + `bind_text/int/double` + step + finalize；`accounts.cpp` 10 处 `account_id` 整数拼接 → 新增 `exec_locked_int()` 单参 helper。`escape_sql` 现 0 调用方。修复编译器捕获的 bug：`get_progress` 初版漏 bind `url`（unused-parameter 警告暴露）→ 已补。
- **[i18n]** `app_tiktok.cpp` 一条 EVENT_LOG 的中文全角标点 → ASCII。重 grep 确认 src/+include/（除 ini_config.h 双语注释）0 CJK。
- **[build]** CMakeLists 注释中失效的 `src/panicast.cpp` 引用 → 更正为真实的 5 处版本源。

### 验证
0 warning 编译 + DB 往返 smoke test（OPML 导入→`save_tree_node_recursive` INSERT→导出→`load_tree_node_recursive` SELECT）数据一致。并发安全审计通过（无 detach、worker 全 join、共享标志位均 atomic、crash 标志 sig_atomic_t）。

---

## Panicast_V0.1-Y24.41 — 2026-07-28 — 主题修订（fg-only 文字 / Y24.20 进度条 / GitHub Dark 默认 / +7 配色）

> 用户需求：文字只要前景色、字符串后不要背景色块（光标行除外）；进度条用回 Y24.20 前景色；找回 Y24.20 配色（偏好 GitHub Dark）；15 种不够丰富。

### 修复（回归自 Y24.20）
- **[theme] 消除背景色块**：`theme_manager.apply()` 恢复 `assume_default_colors(fg, bg)`（Y24.20 行为），使默认窗口背景=主题背景；文字 pair 的 bg 与窗口 bg 一致 → 不再有色块。选中行仍用 `A_REVERSE` 高亮条（用户要保留的块）。此前误用 `use_default_colors()`（透明默认）→ 文字 pair bg 与窗口 bg 不一致 → 色块。
- **[theme] 边框 + 进度条颜色**：恢复 `init_pair(20, fg, bg)` / `init_pair(21, ...)`。pair 20（`PAIR_BORDER_STD`，INFO 边框 + 播放进度条用）此前未初始化 → 回退为任意 256 色 → 边框/进度条颜色错误。Y24.20 进度条前景色 = 主题 fg = pair 20，现已恢复。
- **[theme] GitHub Dark 默认**：调色板与 Y24.20 完全一致（无需改色）；按用户偏好设为默认主题（`DEFAULT_THEME_INDEX = 11`，原默认 Solarized Dark）。
- **[theme] +7 配色**：`THEME_COUNT` 15→22，新增 One Dark / Rose Pine / Monokai Pro / Night Owl / Tomorrow Night / Edge Dark / Deep Ocean（RGB 由各方案规范 hex 换算）。

---

## Panicast_V0.1-Y24.33–Y24.40 — 2026-07-28 — 审计收尾：god 文件/函数按关注点拆分

> 沿用 Y24.32 模式（成员方法声明留头文件、实现迁至独立 .cpp），机械、低风险，全程 0 warning。决策详见 `DECISIONS_LOG.md`。

### 重构
- **Y24.33 StatusBar**：`draw_status` → `src/ui/status_bar.cpp`（350 行）。
- **Y24.34 InfoPanel**：`draw_info` + OSC8 收集 → `src/ui/info_panel.cpp`（618 行）。
- **Y24.35 Tree**：`draw_line` → `src/ui/tree_renderer.cpp`（198 行）。
- **Y24.36 Lyric**：`update_lyric_history`/`lyric_bar_title`/`draw_lyric_content`/`draw_lyric_bar` → `src/ui/lyric_renderer.cpp`（89 行）。`ui.cpp` 2078→820 行。
- **Y24.37 DatabaseManager 拆分**：`database.cpp`（1524 行 / 60 方法）按域拆为 `history_repo`/`tree_repo`/`feed_cache_repo`/`account_repo`/`player_state_repo`；`database.cpp` 保留 schema init + 连接管理 + 11 个 infra 方法。用大括号深度感知 Python 解析器精确切分方法块。
- **Y24.38 Utils 拆分**：`utils.cpp`（941 行 / 26 实体）→ `path_utils`/`process_utils`/`text_utils`；`class Utils` 声明留 `utils.h`（C++ 无 partial class，无法拆头），`utils.cpp` 删除。API 不变，无调用方改动。
- **Y24.39 enter_node 拆分**：~175 行 god function → dispatcher + `enter_marked`/`enter_folder_expand`/`enter_favourite_folder`/`enter_leaf`。锁顺序与分支条件原样保留。
- **Y24.40 spawn_load_feed 拆分**：~230 行 god lambda → `parse_feed_by_type`/`cache_youtube_videos`/`commit_feed_result`；lambda 仅调度。Apple 失败早退经 `abort_out` 透传，`cur_url` 经 out-param 传递（commit 的 episode-cache key）。

---

## Panicast_V0.1-Y24.32 — 2026-07-28 — 提取 PopupStack（ui.cpp 按关注点拆分第 1 步）

> 审计收尾第二步（机械、低风险）：将弹窗/模态对话框代码从 `src/ui/ui.cpp` 拆出至独立单元 `src/ui/popups.cpp`。`ui.cpp` 从 2518 行降至 2078 行；6 个弹窗方法集中到 456 行的新单元。方法仍为 `UI::` 成员（访问私有几何成员 `w`/`h` 与静态 `INPUT_CANCELLED`，并委托 `draw_help`），声明保留在 `ui.h`，仅实现迁出 —— 与迭代计划中 Y24.33–Y24.36 的拆分模式一致。

### 重构
- **[ui] 提取 popups.cpp**：6 个弹窗/模态方法从 `src/ui/ui.cpp` 迁至 `src/ui/popups.cpp`：
  - `input_box()`（输入框，UTF-8/CJK 输入 + URL 自适应宽度）
  - `is_input_cancelled()`（取消标记判定）
  - `dialog()`（单行输入对话框）
  - `show_url_popup()`（流地址全显示弹窗）
  - `confirm_box()`（Y/N 确认框，带边框标题）
  - `show_help()`（委托 `draw_help`）
- 方法体加 `UI::` 限定原样迁出，逻辑零改动；默认参数仅在 `ui.h` 声明保留（定义已在 Y24.31 剥离）。
- `CMakeLists.txt` 新增 `src/ui/popups.cpp`。
- 相比计划文本的两处偏差（已记入 `DECISIONS_LOG.md`）：(1) 未新建 `include/panicast/ui/popups.h` —— 成员函数声明须留在 `ui.h`，C++ 无 partial class，独立头无意义；(2) 一并迁移 `dialog` + `is_input_cancelled`（同为弹窗关注点，DRY/单一职责）。

### 验证
本机 Linux 编译 0 warning。TUI 实测需用户 WSL2 环境执行（弹窗行为应与 Y24.31 完全一致）。

---

## Panicast_V0.1-Y24.31 — 2026-07-27 — UI .cpp 迁移（header-only → 声明+实现分离）

> 审计收尾第一步（机械、低风险）：`ui.h` 从 header-only（2659 行）拆为声明（299 行）+ 实现（`src/ui/ui.cpp`，2518 行）。多行方法体迁出并加 `UI::` 限定、剥离默认参数（声明保留）；单行 getter/setter 保留 inline；嵌套 struct/成员声明原样保留。编译 0 警告。

### 重构
- **[ui] .cpp 迁移**：22 个非 inline 方法体（init/cleanup/draw/draw_info/draw_status/draw_line/draw_help/input_box/confirm_box/draw_lyric_* 等）从 `include/panicast/ui/ui.h` 迁至 `src/ui/ui.cpp`：
  - `ui.h` 仅留类声明 + 成员变量 + 13 个 inline getter/setter（299 行，原 2659 行）。
  - 方法体加 `UI::` 限定；默认参数仅在 .h 声明保留，.cpp 定义剥离（避免重定义）。
  - 现有终端/信号生命周期代码（tui_cleanup/setup_signal_handlers/g_*）保持不变。
  - `src/ui/ui.cpp` 已在 CMakeLists 注册，无需改动构建。

---

## Panicast_V0.1-Y24.21 — 2026-07-26 — 离线转写 skip/resume（避免重复转写）

> Bug：离线转写已转过字幕的文件仍重复转，浪费算力。修复：先检测已有 SRT → 完整则跳过；不完整则断点续转。

### 修复
- **[transcription] skip/resume**：`transcribe_one` 转写前检查 `<file>.srt` 是否存在：
  - **完整**（last_end >= duration - 5s）→ 跳过，EVENT_LOG "Transcribe skip: 已有完整字幕 N段"
  - **不完整**（last_end < duration - 5s）→ `whisper-cli -ot <last_end_ms>` 断点续转，新段追加到已有 SRT，EVENT_LOG "Transcribe resume: N段到 XXs, 续转"
  - **无 SRT** → 全新转写（原行为）
- 新增 helper：`parse_srt_file`（SrtSubtitleParser 解析已有 SRT）、`get_audio_duration`（ffmpeg -i → parse stderr Duration）、`write_srt_file`（写 SRT）。
- `whisper-cli -ot` 时间戳为**绝对**（从文件起点，实测验证），新段直接追加无需加偏移。

### 验证
本机 Linux 编译 0 warning。需用户实测：已转过的文件再按 L → 应显示 skip；中断后重转 → 应 resume。

---

## Panicast_V0.1-Y24.20 — 2026-07-26 — 实时转写 + 路径处理修复

## Panicast_V0.1-Y24.20 — 2026-07-26 — 实时转写 + 路径处理修复

> Y24.19 离线转写基础上的实时转写（播放中无字幕→`L`→whisper.cpp 渐进段→LYRIC）。含 BTW 路径处理修复。

### 新增
- **[subtitle] 实时转写**（`TranscriptionEngine::start_realtime`/`stop_realtime`）：播放中无字幕按 `L` → 后台 `ffmpeg`（URL/文件→16kHz mono wav）→ `whisper-cli`（进度式 stdout）→ 逐段解析 `[HH:MM:SS.mmm --> ...] text` → `SubtitleManager::set_pending` 喂 `current_transcript_` → LYRIC 实时显示。完成存 SRT（本地同路径 / 流式 `<data_dir>/transcripts/<hash>.srt`）。`L` 再按 / 切曲 → `stop_realtime`（gen 失效 + kill whisper-cli）。
- **[utils] `run_process_streaming`**：spawn + 进度式 stdout 行回调 + `stop_pred`（true→kill 子进程）。供实时 whisper-cli 用。
- **[subtitle] `SubtitleManager::set_pending`**：进度式喂段（worker→UI 经 pending_，线程安全）。
- **[subtitle] `SubtitleManager::reset`**（Y24.18 已加，实时也用）。

### 修复（BTW 路径处理）
- **`resolve_whisper_bin`**：裸名→`which_binary` 搜 PATH；`~`→展开 `$HOME`；绝对路径→`fs::exists`。
- **`resolve_model`**：`~`→展开；绝对/相对路径→`fs::exists`；**裸文件名→`<data_dir>/models/<file>`** 自动补全。
- **INI 默认统一**：`whisper_bin = whisper-cli`，`model = ~/.local/share/panicast/models/ggml-small.en-q5_1.bin`。

### L 状态机（Y24.19+20 合并）
| 场景 | `L` 行为 |
|---|---|
| 实时转写中 | 停止（kill whisper-cli） |
| 播放中 + 无字幕（NONE） | 启动实时转写 |
| F 模式 + 光标/v 标记本地文件 | 离线转写（Y24.19） |
| 其他 | LYRIC 条切换 |

### 验证
本机 Linux 编译 0 warning。需用户 WSL2(7735HS) 实测：播放无字幕节目 → `L` → LYRIC 实时填充段 → 完成存 SRT → 回放自动加载。

### 说明
- 实时管道：`ffmpeg -i <url> -ar 16000 -ac 1 -f wav <tmp>` → `whisper-cli -m <model> -f <tmp> -t N`（whisper-cli 不支持 `-f -` stdin，故用临时 wav）。whisper-cli 转写时逐段打印 stdout，边读边喂 LYRIC。
- i3-5010U 实时跟不上（small.en 8x 慢）；7735HS 实时可行（small.en/large-v3-turbo）。
- 流式节目 SRT 存 `<data_dir>/transcripts/<url_hash>.srt`（djb2 哈希），节点 `subtitle_url` 记录路径。

---

## Panicast_V0.1-Y24.19 — 2026-07-26 — 离线转写（whisper.cpp → SRT sidecar）

> 需求：无字幕节目实时/离线转写生成字幕。分两期：Y24.19 离线（本期），Y24.20 实时。模型选型经实测：i3-5010U 上 small.en-q5_1 8x 慢于实时、base.en-q5_1 2.8x、tiny.en 1.5x；用户 WSL2=AMD 7735HS（实时可行）。

### 新增
- **[subtitle] `TranscriptionEngine`**（`src/subtitle/transcription_engine.{h,cpp}`）：离线 whisper.cpp 转写。`enqueue_offline(nodes)` 入队 → 调度线程按 CPU 负载（getloadavg）动态并发（≤ `[transcription] max_concurrent`，默认 3）→ 每文件 `ffmpeg`（→16kHz mono wav）+ `whisper-cli`（→SRT）→ 存 `<file>.srt` 同路径（`probe_sidecar` 回放自动加载）。无 shell（`Utils::run_process` posix_spawnp）。
- **[utils] `Utils::run_process`**：通用 spawn+捕获 stdout/stderr/exit code（无 shell，安全），供 TranscriptionEngine 用。
- **[app] `L` 键 F 模式转写**：F 模式光标在本地文件（或 `v` 多选）按 `L` → 入队离线转写；否则 `L` 仍是 LYRIC 条切换（现有）。实时转写（播放中无字幕→`L`）留 Y24.20。
- **[ini] `[transcription]`**：`whisper_bin`/`model`/`max_concurrent`。
- **[ui] 帮助** 修正 `L`（原误写"playlist popup"，现为 LYRIC+转写）+ `Ctrl+B`（上下文 cookie）。

### 实测（i3-5010U，jfk.wav 11s）
| 模型 | 耗时 | 倍率 |
|---|---|---|
| tiny.en | 16s | 1.5x 慢 |
| base.en-q5_1 | 31s | 2.8x 慢 |
| small.en-q5_1 | 89s | 8.1x 慢 |

`whisper-cli -m <model> -f <wav> -osrt -of <out>` → `out.srt` 验证可用。7735HS 上 small.en/large-v3-turbo 实时可行（按用户表）。

### 验证
本机 Linux 编译 0 warning。需用户 WSL2(7735HS) 实测：装 whisper-cpp + 模型 → F 模式选本地文件 → `L` → 生成 `.srt` → 回放自动加载字幕。

### 说明 / 待办
- **依赖用户自装**：`whisper-cpp`（Arch `pacman -S whisper-cpp` / Debian `apt install whisper.cpp` 或源码）+ 模型 `ggml-small.en-q5_1.bin`（HuggingFace `ggerganov/whisper.cpp`）。`L` 检测缺失→提示安装。
- **Y24.20 实时转写**：播放中 `L` → `ffmpeg|whisper-stream` 渐进段 → LYRIC 实时显示；完成存 SRT。待用户提供实时流命令。
- 仓库：GitHub `ggml-org/whisper.cpp`（新家），模型镜像 `huggingface.co/ggerganov/whisper.cpp`（同项目）。

---

## Panicast_V0.1-Y24.18 — 2026-07-26 — 切换文件时 LYRIC 区即时清空（消除 stale 旧歌词）

> 反馈：切换播放文件时 LYRIC 打印区要正确处理——切字幕、无字幕要关闭。分析发现切换瞬间 LYRIC 显示旧歌词（stale）。

### 根因
`SubtitleManager::load_async` 切换时只设 `force_log_` 就把工作丢进 pool，**没同步重置 status/pending** → 切换后到 pool 完成前，`status_` 仍是上一曲（如 READY）+ UI `current_transcript_` 仍是上一曲段落 → 用新曲 time_pos 匹配旧段落 → 短暂显示错位/旧歌词。`last_lyric_url_` 只清 `lyric_history_`（渲染行）不清 `current_transcript_`（段落）。视频路径（Y24.17 Method A）没重置 SubtitleManager → 切到视频时 LYRIC 残留旧音频歌词。

### 修复
- **[subtitle] `load_async` 同步清旧字幕**：进 pool 前，加锁 sync 置 `status_=LOADING` + `pending_.clear()` + `pending_url_=新url` + `pending_ready_=true`。→ `poll` 下一帧立即 `set_transcript(空, 新url)` 清掉 `current_transcript_` + `lyric_history_`。**切换瞬间 LYRIC 即清空**，新字幕加载完再填。无字幕路径：sync LOADING → pool NONE → bar 关，衔接正确。
- **[subtitle] 新增 `reset()`**：sync 置 NONE + 清 pending + 标记 pending_ready（poll 清 UI）。视频 Method A（mpv `sub-add`）+ 视频无字幕 调 `reset()` 清 Method B，避免 LYRIC 残留旧音频歌词。视频 JSON 字幕仍走 `load_async`（Method B 填充）。
- `on_playback_ended`（自动切下一曲）走同一 `load_async`，自动受益（同步清）。

### 验证
本机 Linux 编译 0 warning。需用户 WSL2 实测：切文件→LYRIC 立即清空（不显示旧词）→新字幕加载完填充；无字幕→清空+关闭；视频→Method A 渲染视频窗，LYRIC 面板清空不残留。

---

## Panicast_V0.1-Y24.17 — 2026-07-26 — BUFFERING 诊断 + 视频字幕异步 + 日志轮转 + OSC8 日志洪流修复

> 反馈：F 模式本地文件 BUFFERING >5s 异常；"LOG 看不到事件不放心"；视频字幕可异步；OSC8 诊断 LOG 每帧刷屏日志要爆；日志保留 365 天自动清除。

### 修复/增强
- **[ui] OSC8 诊断 LOG 洪流修复**：Y24.13 加的 `[UI] OSC8 emitted N link(s)` 每帧打（状态栏 URL 每帧 OSC8）→ ~30 行/秒日志爆炸。OSC8 已确认可用，**删除该诊断 LOG**。
- **[mpv] 订阅 mpv 日志事件（让你看到 BUFFERING 期间发生了什么）**：`mpv_request_log_messages(ctx, "info")` 订阅 INFO+；事件循环处理 `MPV_EVENT_LOG`，按 `[MPV/log] <prefix>: <msg>` 写日志。**WARN/ERROR 全程记；INFO 仅"加载窗口"（play→FILE_LOADED，`logging_load_` 原子标志）记**，避免刷屏。AO init / demuxer 探测建索引 / cache 填充等事件可见。
- **[playback] play_current 时间戳 + BUFFERING 分解**：`[PLAY] start` → `get_local_file+fs::exists (Xms)` → `player.play` → has_media 时 `[PLAY] BUFFERING cleared: total Yms`（>2s 时还进 UI EVENT_LOG）。一眼看出 >5s 花在"同步 fs::exists/DB"还是"mpv 加载"。
- **[subtitle] 视频字幕异步**：`probe_local_sidecar` 对视频改异步进 pool（原同步查 .vtt/.srt/.ass，/mnt/e 上累加延迟）。pool 探到 mpv-compat sidecar → `player.sub_add(url)`（新增 `MPVController::sub_add`，mpv `sub-add` select）异步加入；非 mpv 格式 → Method B。LOG 详细打印 `[Subtitle] video sidecar probe (async)` + 结果。`is_mpv_sub`/`basename` 提为文件级静态（pool 共用）。音频字幕本就异步。
- **[logger] 日志保留 365 天自动清除**：改按日期分文件 `panicast-YYYYMMDD.log`（原单 `panicast.log`）；init 时按 mtime 删 >365 天的日志文件（含 legacy `panicast.log`）。`Utils::get_log_file()` 返回今日文件；main/?/README/INI 文档同步。mpv 订阅增加日志量，365 天滚动清除防爆。
- **[mpv] cache 保留**（撤回 Y24.17 草稿的 cache=no）：本地文件 cache 填充极快（磁盘吞吐 >>实时），cache 不是 >5s 根因；保留 `cache=yes` + 音频快速起播，靠诊断日志定位真因。

### 验证
本机 Linux 编译 0 warning。需用户 WSL2 实测：本地文件 BUFFERING 时看 `panicast-YYYYMMDD.log` 的 `[PLAY]`/`[MPV/log]` 事件定位 >5s 根因；视频字幕异步加载 + LOG；日志按天分文件 + 365 天清除；OSC8 日志不再刷屏。

### 说明
- T 模式到此为止（Y24.16 终态，不再动）。
- 真因待你测后从日志确认：若是 `[MPV/log]` 显示 mpv 侧（demuxer 索引/AO），是 mpv/挂载问题；若是 `[PLAY]` 同步段，是 fs::exists/DB。

---

## Panicast_V0.1-Y24.16 — 2026-07-26 — T 模式审计清理 + 直入替代搜索 + 抖音单视频 + 全文档

> 先审计 Y24.10–Y24.15 的 T 模式/OSC8/搜索代码（UNIX 哲学：简明、复用、不重复、统一），再迭代。结论：搜索引擎兜底是假功能（Google/Bing 不索引 TikTok 内容页，0 结果）；抖音用户列表是死路（yt-dlp 无 DouyinUserIE）；多个 cookie getter 重复。

### 审计清理
- **[tiktok] 删搜索引擎兜底**：`search_tiktok_creators` + `parse_search_html` + `strip_html` + `url_encode` + `TiktokSearchHit` 全删（实测 Google/Bing `site:tiktok.com` 返回 0 个 /@/video URL，是假功能）。`/` 改直入。
- **[tiktok] 抖音用户列表死路明确化**：`spawn_load_feed` 的 `DOUYIN_USER` 改为明确 EVENT_LOG "暂不支持（yt-dlp 无 DouyinUserIE）"+ 空结果停 spinner（原静默重试 3 次到空）。
- **[config] 抽 `resolve_cookies_path` helper**：youtube/bilibili/tiktok/tiktok_douyin 4 个 cookie getter 原各重复 5 行路径解析，统一为 1 行调用。youtube 默认值由 "" 改 "youtube_cookie.txt"（行为等价，更一致）。
- **[app] `configure_youtube_cookies`→`configure_cookies`**：名实不符（现管 Y/B/T 三种 cookie），重命名统一。
- **[tiktok] 删 `normalize_tiktok_input` 的 `prefer_douyin`** + 裸输入→抖音 sec_uid 分支（抖音不订阅用户，死代码）。`<set>`/`<algorithm>`/`network.h`/`url_classifier.h` 闲置 include 删除。
- **[tiktok] 根节点去区域**：`load_tiktok_root` title 静态 "TikTok"（原 "TikTok [US]"，`b` 切区不同步）。区域唯留状态栏边框。

### 功能（D 方案）
- **[tiktok] `/` 直入**：`tiktok_direct_input` 接受 `@user`/`#tag`/URL。`@user`/URL→`tiktok_subscribe`（与 `a` 共享核心，无重复）；`#tag`→`tag_browse`（yt-dlp 标签列表，复用 `parse_tiktok_user_videos`，当前 `tiktok:tag` 上游禁用故休眠，上游修复自动可用）；纯关键词→明确提示"匿名无法关键词搜索"。
- **[tiktok] `a` 视频订阅**：抽 `tiktok_subscribe(input)` 共享核心。TikTok 视频 URL（含 @user）→自动订阅创作者，展开列全部视频（yt-dlp `tiktok:user`）。抖音视频 URL→存为可播放视频叶节点（option A，`platform="douyin_video"`）；抖音用户 URL→拒绝并提示。

### 文档（全同步）
- `?` 帮助、`man/panicast.1`（新增 TIKTOK/DOUYIN (T MODE) 章节）、`README.md`（新增 T 模式章节）全部记录：`a` 订阅 @user/视频URL、`/` 直入 @user/#tag/URL、抖音仅单视频、区域在状态栏、`#tag` 待上游、cookie/网络要求。

### 验证
本机 Linux 编译 0 warning。TUI 全链路需用户 WSL2 实测：T 进入、`a` TikTok 视频 URL 订阅+展开、`a` 抖音视频 URL 叶节点播放、`/` `#tag` 休眠提示、`b` 切区状态栏更新根节点不变。

### 说明 / 已知限制
- **抖音仅单视频**：yt-dlp 无 DouyinUserIE，列不出用户视频；`a` 抖音用户 URL 被拒。
- **`#tag` 休眠**：yt-dlp `tiktok:tag` `_WORKING=False`（上游显式禁用，无修复迹象），现优雅失败，上游修复后自动可用。
- **无匿名关键词搜索**：搜索引擎不索引 TikTok/抖音内容页 + yt-dlp 无搜索提取器，用 `a` 直接输 @user/视频 URL。

---

## Panicast_V0.1-Y24.15 — 2026-07-26 — OSC 8 下划线同步 + 状态栏完整 URL + 撤回百度

> Y24.14 反馈：(1) 坚决不用百度/搜狗/360；(2) OSC8 续行能识别但悬停第一行只第一行有下划线，应同步；(3) 状态栏缩略 URL Ctrl+点击应打开完整 URL；(4) 'e' 花屏已自行消失。

### 修复
- **[utils] OSC 8 下划线同步（第2条）**：`emit_osc8_link` 加 `id` 参数，序列改 `OSC 8 ; id=<id> ; <url> ST`。同一 URL 的各折行 span 用相同 id → 终端视为一个逻辑链接 → 悬停任意行时**所有行下划线同步显示**。INFO Streaming URL 各行共用 `id="u"`；LOG 各 URL 唯一 id；状态栏 `id="s"`。
- **[ui] 状态栏 URL Ctrl+点击打开完整 URL（第3条）**：`draw_status` 的 mid_display（缩略 URL）加 OSC8，可见=缩略文本，链接目标=完整 `mid_content` → Ctrl+点击在浏览器打开完整 URL（非缩略）。仅 mid_content 为 http(s) URL 时链接。
- **[tiktok] 撤回百度/搜狗（第1条）**：CN 搜索引擎链回到 **Google→DuckDuckGo→Bing**（与 TikTok 一致），不用百度/搜狗/360。

### 验证
本机 Linux 编译 0 warning。OSC8 下划线同步、状态栏完整 URL 需用户 WSL2 实测。

### 待用户确认（未实现）：TikTok/抖音搜索方案
Y24.14 的 CN 百度已撤回。当前 Google→DDG→Bing 对 douyin.com 索引差、对 tiktok.com 也受 bot 拦截，关键词相关视频/博主难搜到。搜索方案待用户提供方向后迭代（不擅自改）。

---

## Panicast_V0.1-Y24.14 — 2026-07-26 — Ctrl+B T 模式 cookie + CN 百度搜索 + OSC 8 确认可用

> Y24.13 测试反馈：(1) OSC 8 全识别 URL + Ctrl+点击打开 IE ✓ 保留；(2) T 模式 Ctrl+B 弹的是 YouTube cookie，应导入 TikTok/Douyin cookie；(3) CN 区搜索结果少（"山泉浓茶" IE 有很多，PaniCast 无）——douyin cookie 没导入 + Google/Bing 对 douyin 索引差；(4) 左侧节点树偶现杂散 'e'，Ctrl+L 切主题上移一行（原因待定位）。

### 修复/增强
- **[app] Ctrl+B T 模式导入对应 cookie（第2条）**：`configure_youtube_cookies` 加 T 模式分支——CN 区设 `[tiktok] douyin_cookies_file`（抖音 cookie），非 CN 设 `[tiktok] cookies_file`（TikTok cookie，可选）。新增 `get_tiktok_cookies_file()` INI 项 + 默认 `tiktok_cookie.txt`。`spawn_load_feed` 的 TIKTOK_USER 分支 + `add_tiktok_user`/`add_tiktok_user_from_node` 都接上 TikTok cookie（匿名仍可用，cookie 为空则跳过）。
- **[tiktok] CN 搜索加百度（第3条）**：`search_tiktok_creators` 引擎链按域名分——CN/douyin 用 **Baidu→Sogou→Bing**（中国引擎索引 douyin.com 远好于 Google/Bing），非 CN 仍 Google→DuckDuckGo→Bing。提升 CN 区搜索命中率。
- **[ui] OSC 8 确认可用**：Y24.13 光标复位修复后，OSC 8 全识别 URL + Ctrl+点击打开，保留。杂散 'e' 待用户 `url_hyperlink=off` 测试定位（OSC 8 经 /dev/tty 直发可见文本，ncurses 增量重绘按内容比对不覆盖残留——疑为残留字符）。

### 验证
本机 Linux 编译 0 warning。Ctrl+B T 模式 cookie、CN 百度搜索、抖音 cookie 列表/播放需用户 WSL2 实测。

### 说明 / 已知限制
- **杂散 'e' 未定**：请用户测试 `url_hyperlink=off` 是否消失。消失则 OSC8 残留，加 `clearok` 强制重绘（可能轻闪）；不消失则非 OSC8，另查。
- **CN 搜索仍非真搜索**：Baidu/Sogou 对 douyin.com 的索引 + 反爬决定命中；搜不到时用 `a` 直接输抖音 sec_uid/URL。
- **抖音需中国出口 + cookie**：TPClash 路由 douyin.com 走 CN + `[tiktok] douyin_cookies_file` 导入登录 cookie。

---

## Panicast_V0.1-Y24.13 — 2026-07-26 — OSC 8 光标错位花屏修复 + T 模式 CN/抖音区

> Y24.12 测试反馈：(a) 开 `url_hyperlink` 后花屏——左侧节点树区出现状态栏艺术字串；(b) OSC 8 未实现 URL 全部识别（仍只有 http 行被识别）；(c) T 模式需加中国抖音支持。

### 修复
- **[ui] OSC 8 光标错位花屏修复**：`emit_osc8_link` 在 `doupdate` 后往 `/dev/tty` 直发 `CUP + 可见文本`，移动物理光标导致 ncurses 增量重绘的相对光标移动失准 → 串屏。新增 `Utils::emit_cup(row,col)`，emit 前 `getyx(curscr,sy,sx)` 保存 ncurses 记录的光标，emit 后 `emit_cup(sy+1,sx+1)` 复位，物理光标与 ncurses 假设重新对齐 → 不再花屏。加诊断 LOG `[UI] OSC8 emitted N link(s)` 便于排查。OSC 8 去留待用户后续测试决定（`[display] url_hyperlink=off` 可关）。
- **[tiktok] T 模式加 CN/抖音区（全球覆盖）**：`TikTokRegion` 列表加 `CN`（13 区），`name("CN")="China (Douyin)"`。CN 区 → T 模式访问 **douyin.com** 域名（其余 12 区仍 tiktok.com），统一 T 模式覆盖 TikTok 全球 + 抖音中国。
  - `add_tiktok_user`：CN 区输入当抖音（`douyin.com/user/<sec_uid>` 或裸 sec_uid），用 `[tiktok] douyin_cookies_file` cookie + `--geo-bypass-country CN`。
  - `search_tiktok_creators`：CN 区搜 `site:douyin.com`，解析 `douyin.com/video/<id>`（可播）+ `douyin.com/user/<sec_uid>`（可订阅）；非 CN 区同前 tiktok。解析函数 `parse_search_html` 改为域名感知。
  - `normalize_tiktok_input` 加 `prefer_douyin`：CN 区裸输入当抖音 sec_uid。
  - `spawn_load_feed` 的 `DOUYIN_USER` 分支传 `--geo-bypass-country CN`（原为空）。
  - CN 区状态栏/根节点标题显示 `🎵 抖音 [CN]` / `Douyin [CN]`，其余 `🎵 TIKTOK [US]`。

### 验证
本机 Linux 编译 0 warning。花屏与 OSC 8 全识别、CN/抖音全链路需用户 WSL2 实测（抖音需中国 IP + cookie）。关键：**不能花屏**——若 Y24.13 仍花屏，立即 `url_hyperlink=off`。

### 说明 / 已知限制
- **OSC 8 去留未定**：用户后续测试决定。WSL2 终端若不支持 OSC 8，续行无法超链接（仅 http 行靠终端自动检测）；`url_hyperlink=off` 彻底关闭不影响其他功能，`Ctrl+Y` 复制完整 URL 兜底。
- **抖音门槛**：需中国 IP 出口（TPClash 路由 douyin.com 走 CN）+ 登录 cookie；匿名可能取不到列表。TikTok 各区匿名可用。

---

## Panicast_V0.1-Y24.12 — 2026-07-26 — T 模式区域标识 + 搜索扩视频/多引擎 + 音频快速起播 + URL OSC 8 超链接

> Y24.11 测试反馈 4 条：(1) T 模式缺地区标识；(2) / 搜索只匹配用户名；(3) P 模式长节目缓冲过久；(4) INFO/LOG 区 URL 折行后鼠标只认第一行。

### 修复/增强
- **[ui] T 模式带区域标识（第1条）**：`TikTokRegion::current()/set_current()` 单例；状态栏标题 `🎵 TIKTOK [US]`（镜像 ONLINE 的 `[US]`）；`tiktok_root->title = "TikTok [US]"` 根节点也带区域。`b` 切区即时更新。
- **[tiktok] / 搜索扩到视频+主页 + 多引擎（第2条）**：查询改 `site:tiktok.com <query>`（去掉 `/@`），解析**视频页** `@user/video/<id>`（可直接 Enter 播放，TIKTOK_VIDEO）+ **主页** `@user`（`a` 订阅）。引擎链 **Google→DuckDuckGo→Bing**，首个有结果的引擎即用。内容/标题匹配由引擎完成（之前仅用户名）。带 anchor 标题解析 + 去重。
- **[mpv] 音频流快速起播（第3条）**：`play_audio()` 设音频专用小缓冲 `cache-secs=5 / demuxer-max-bytes=5MiB / back=2MiB / cache-pause-wait=1`（边缓冲边播，长节目不再等满）；`play_video()` 恢复大缓冲（`cache-secs=10 / 50MiB`），防音频小缓冲串到 YouTube/TikTok 视频导致重缓冲。新增 INI `[mpv] audio_cache_secs / audio_demuxer_max_bytes / audio_demuxer_max_back_bytes / audio_cache_pause_wait`。
- **[ui] INFO/LOG URL OSC 8 超链接（第4条）**：`Utils::emit_osc8_link(row,col,visible,url)` 经 `/dev/tty` 发 OSC 8（CUP 定位 + 链接标记夹可见文本），doupdate 后统一发。INFO Streaming URL **每折行**→链接目标=完整 URL（任意行悬停开完整 URL）；LOG 区每行 URL 子串同样处理。整段折行 URL 识别为一个链接。INI `[display] url_hyperlink=on`（默认开，不支持 OSC 8 的终端可关）。`Ctrl+Y` 复制完整 URL 兜底保留。

### 验证
本机 Linux 编译 0 warning。OSC 8 经 `/dev/tty` 直发（仿既有 OSC 52 剪贴板模式），不依赖 ncurses 转义穿透。TUI 行为需用户 WSL2 实测：T 标题带区域、`/` 搜视频+主页、长播客起播、INFO/LOG URL 悬停。

### 说明 / 已知限制
- **搜索引擎兜底仍非真搜索**：yt-dlp 无 TikTok 搜索提取器不变；Google/DDG/Bing 可能 bot 拦截，结果取决于引擎是否返回 tiktok.com 链接。多引擎链提高命中率。
- **OSC 8 终端依赖**：Windows Terminal/kitty/wezterm/gnome/iTerm 支持；不支持者忽略序列无垃圾，可 `url_hyperlink=off` 关。
- **音频小缓冲权衡**：5s/5MiB 首播快但慢网可能断续；INI 可调。视频保持大缓冲不退化。
- **多重定向播客 URL**（如 pdst.fm→…→flightcast.mp3）的握手延迟是网络固有，降缓冲是唯一杠杆。

---

## Panicast_V0.1-Y24.11 — 2026-07-26 — T 模式 (TikTok/抖音) + 节点树默认常开

> 新增 T 模式：匿名订阅 TikTok/抖音创作者、列视频、播放，b 键循环 12 区域，/ 键搜索引擎兜底发现创作者。释放原 T 键（节点树连接线开关），节点树默认常开。

### 调研结论（驱动设计）
- **yt-dlp 2026.07.04 TikTok 现状**：`tiktok:user`/`tiktok:live`/`tiktok:collection` 可用；`tiktok:tag`/`sound`/`effect` BROKEN；**无搜索提取器**（`tiktok.com/search` 报 Unsupported URL）。
- **匿名可行**：`yt-dlp --flat-playlist "tiktok.com/@user"` 匿名列视频（标题/时长/URL/缩略图/播放数），**无需 US SIM、无需 cookie**。`--geo-bypass-country <CC>` 注入该国家 IP 生效。单视频 `yt-dlp -F` 出 h264/h265 直链，mpv 可播。
- **"需美国手机卡"澄清**：该限制针对浏览器登录态功能（For You/Following/点赞）；对"列某用户公开视频+播放"匿名即可。
- **抖音**：douyin.com 独立，需中国 IP + 常需 cookie。T 模式纳入但门槛高于 TikTok。

### 新增
- **[mode] `AppMode::TIKTOK` + `T` 键进入**：镜像 B 模式。`tiktok_root` + `load_tiktok_root()` 从 DB 列已订阅创作者；`M` 循环纳入第 8 模式。状态栏标题 `🎵 TIKTOK`。
- **[net] `TikTokRegion` 助手**：12 区 (US/JP/GB/DE/FR/KR/ID/TH/VN/MY/BR/MX)，`b` 键循环，INI `[tiktok] region` 持久化。选中 CC 传给所有 yt-dlp 调用 `--geo-bypass-country`。抖音固定不传（需 CN 出口）。
- **[app] `app_tiktok.cpp`**：`load_tiktok_root` / `add_tiktok_user`（输入 @user/URL → yt-dlp 探测频道名 → 存 DB）/ `add_tiktok_user_from_node`（搜索结果直接订阅）/ `cycle_tiktok_region` / `delete_tiktok_user_node` / `search_tiktok_creators`。
- **[db] `tiktok_accounts` 表**（SCHEMA_VERSION 44→45）：platform/handle/url/uname，UNIQUE(platform, handle)，upsert 去重。无凭据需加密。
- **[parser] `parse_tiktok_user_videos`**：yt-dlp `--flat-playlist --dump-json` + geo-bypass + 可选 cookies + 重试 3 次（瞬时空输出/JS challenge）。`DOUYIN_USER` 旧内联代码改为调用此助手（统一 + 修掉占位 cookie）。
- **[net] URL 分类**：`TIKTOK_USER`（`tiktok.com/@`）/ `TIKTOK_VIDEO`（`/video/<id>` + `vm.tiktok.com` 短链）。`is_video()` 加 `TIKTOK_VIDEO`。
- **[playback] TikTok 视频播放**：`TIKTOK_VIDEO` 加入 `BILIBILI_VIDEO||DOUYIN_VIDEO` 分支，复用 mpv ytdl_hook（`player.play(orig_url, true)`），**播放零新代码**。
- **[search] 搜索引擎兜底（B 方案）**：`/` 键 → Bing `site:tiktok.com/@<query>` → 正则提取 @user 去重（≤30）→ 列成节点 → `a` 订阅。匿名无签名，失败明确提示。yt-dlp 无搜索提取器的妥协方案。
- **[ini] `[tiktok]` 段**：`region` / `douyin_cookies_file`。

### 改动
- **[ui] 释放 T 键**：移除 `case 'T': toggle_tree_lines()`；`show_tree_lines` 默认 true 常开（INI 仍可调，无键绑定）；状态栏移除 `[T]` 树线标记（T 现表 TikTok 模式）；帮助弹窗 T 移到 Modes 段并新增 T 模式专节。
- **[ui] 帮助弹窗**：Modes 段补 B/T；新增 "---- T Mode (TikTok / Douyin) [Y24.11] ----" 段。

### 验证
本机 Linux 编译 0 warning。yt-dlp 实测 `@tiktok`/`@khaby.lame` 匿名列视频 + 单视频 `-F` 出直链；geo-bypass US/JP/GB/ID 各注入不同 X-Forwarded-For。TUI 行为（T 进入/`a` 添加/`b` 切区/`/` 搜索/Enter 展开/播放）需用户 WSL2 实测。

### 说明 / 已知限制
- **匿名搜索非真搜索**：yt-dlp 无 TikTok 搜索提取器，`/` 走搜索引擎兜底，结果取决于 Bing 是否返回 @user 链接，不稳定；可靠的"添加创作者"用 `a` 直接输入 @user。
- **区域对创作者自有视频影响有限**：创作者已发视频是全局的；区域主要影响反爬/限流与区域锁定创作者可达性。
- **抖音需中国出口 + cookie**：本机/WSL2 若 TPClash 不把 douyin.com 走 CN，抖音列表会失败。TikTok 匿名即可。
- **TikTok 播放偶发 "No video formats"**：JS challenge cookie 抖动，mpv ytdl_hook 内部重试通常自解；列表侧已加重试。

---

## Panicast_V0.1-Y24.10 — 2026-07-26 — Apple lookup 重试 + 真实错因透出

> Apple Podcast 链接（`podcasts.apple.com/.../id<N>`）解析间歇失败：WSL2 + 透明代理到 `itunes.apple.com/lookup` 的 TLS 握手瞬时断（"SSL connect error" / "Could not connect to server"），多试几次又成功。但失败时只说 "Apple lookup failed"，既不重试也不透出原因，用户无从判断是代理问题还是 URL 问题。

### 修复
- **[net] `fetch` 抽出 `fetch_once(url, timeout, &err_out)`**：单次请求核心返回 body 同时把人话错因（curl 错误串 "SSL connect error" / "Connection timed out" / 或 `HTTP <code>`）填进 `err_out`，不自记 EVENT_LOG。`fetch` 包一层保持原日志行为不变（HTTP 4xx/5xx 仅 LOG，传输错误 EVENT_LOG）。供 `lookup_apple_feed` 复用以捕获重试所需的错因。
- **[net] `lookup_apple_feed` 加重试 + 退避**：瞬时传输错误（SSL/连接）最多重试 3 次，800ms/1.6s 退避；成功拿到响应但无 feedUrl/无 results/JSON 解析失败属确定性失败，不重试直接退出。超时改为 12s/次（小 JSON 不必等 30s）。运行在 pool 线程，不阻塞 UI。
- **[net] 失败透出真实原因**：最终失败发 `Apple lookup failed after 3 tries: <真实原因> — check [network] proxy / TLS to itunes.apple.com`，替换干巴巴的 "Apple lookup failed"。调用方去掉重复的通用 EVENT_LOG，`node->error_msg` 改为 "Apple lookup failed (see LOG)"。
- **[net] 无 podcast id 的 URL** 也给出明确提示（"no podcast id in URL"）而非静默返回空。

### 验证
本机 Linux 编译 0 warning。`itunes.apple.com/lookup?id=1322200189` 实测返回 `feedUrl=https://feeds.simplecast.com/qm_9xx0g`（Crime Junkie）。重试逻辑覆盖瞬时 SSL/连接错误；HTTP 4xx 与 JSON 解析失败不浪费重试次数。

### 说明
- 不改 RSS/feed 抓取的重试策略（只针对 Apple lookup 这条已知抖动路径）；`fetch` 行为对其他调用方完全不变。
- TikTok/T 模式调研报告已出，待用户确认后单独迭代（Y24.11）。

---

## Panicast_V0.1-Y24.9 — 2026-07-26 — 多 transcript URL 优先级选最佳 + TextWithTimestamps 解析器

> omny 节目显示 📜 但加载 0 段：feed 每 episode 有 3 个 `<podcast:transcript>`（srt/vtt/TextWithTimestamps），旧代码盲目覆盖选了最后一个（TextWithTimestamps，自定义格式解析器不认）→ 0 段。实测 curl 3 种格式都抓到了（非 Agent/UA 问题），是选错 URL + 格式不认。

### 修复
- **[subtitle] `detect_from_rss` 按优先级选最佳 transcript URL**：feed 多个标签时按 `vtt(4)>srt(3)>json(2)>lrc(1)>twt(1)>unknown(0)` 保留最高优先级，不盲目覆盖。omny 3 标签 → 最终 VTT（可解析）。优先级按通用性：VTT/SRT 音频(Method B)+视频(Method A)都行；JSON/LRC/twt 仅 Method B。
- **[subtitle] 新增 `TextWithTimestampsParser`**：解析 omny 的 `HH:MM:SS\nSpeaker N: text` 格式（时间戳独立行）。注册到 registry，auto-detect 识别（时间戳独立行 + 无 `-->`/`{`/`[`）。仅当 feed 无 vtt/srt/json/lrc 时兜底用。实测 omny TextWithTimestamps → 260 段。
- **[subtitle] 诊断 LOG**：`[Subtitle] RSS detected: <url> (type=vtt, kept)` / `[Subtitle] RSS skip: <url> (type=twt, rank=1) — keep <url> (type=vtt, rank=4)`，一眼看出选了哪个、跳了哪个。
- **[subtitle] `infer_type` 识别 omny**：URL 含 `format=textwithtimestamps` → type=twt（该格式无 MIME/扩展名）。
- **[subtitle] `load_async` 用 `subtitle_type` 作解析 hint**：RSS 捕获的 type（含 twt）传给 registry；本地 sidecar 仍按扩展名推断。

### 验证
本机 Linux 编译 0 warning。实测 omny TextWithTimestamps（119KB）→ 260 段（auto-detect + twt hint 均通过）；优先级逻辑 srt→vtt→twt(skip) → 最终 VTT。

### 说明
- 经实测确认 0 段非 Agent/UA 问题（curl 3 种格式都抓到），是格式选错 + TextWithTimestamps 不认。
- 建立在 Y24.8 之上（mpv 码人话 + 字幕全异步 + Method A/B 明确措辞 + 失败原因）。

---

## Panicast_V0.1-Y24.8 — 2026-07-26 — mpv 码人话 + 字幕全异步 + 失败原因 + Method A/B 明确措辞

> 两项：(1) mpv 返回码（reason=4/error=-14 等）替换为人类可读表述；(2) 字幕处理完全异步，不阻塞音频播放——本地文件 BUFFERING 慢的根因是 play_current + load_async 在 player.play 前的 ~15 次同步 fs::exists（/mnt/e WSL2 慢）。

### mpv 码人类可读（方案 C）
- **修正 reason 映射**：此前代码把 `reason==3` 当 error（错），实际 error 是 `reason==4`（mpv enum: 0=EOF/2=stop/3=quit/4=error/5=redirect）→ 之前错误分支从不触发，人话错误信息从未显示。
- **`MPVController::end_file_reason_str`/`mpv_error_str`**：`[MPV] End file: playback error — audio output init failed (AO=null — check [mpv] ao / PulseAudio / WSLg)` 等人话，替换裸 `reason: 4, error: -14`。`-14=AO_INIT_FAILED`（之前 switch 漏了）、`-13=LOADING_FAILED`、`-15=VO_INIT_FAILED` 等全部映射。
- **AO=null 启动警告**：mpv 初始化后若 `current-ao` 为空 → `EVENT_LOG` 醒目警告"无音频输出驱动，播放将失败"。
- **on_playback_ended**：`[AUTOPLAY] End file: stopped (user/script) — not advancing` 替换 `reason=X, ignored`。

### 字幕全异步（不阻塞播放）
- **`SubtitleManager::load_async` 全异步**：probe sidecar + fetch + parse **整体**进 pool；调用方立即返回，零同步 fs::exists。`player.play()` 紧接着立即调用 → 本地文件不再因 fs::exists 阻塞 BUFFERING。
- **音频路径不再同步门控**：音频一律 Method B（`load_async(pn)` 异步探测+加载），移除 play_current 的同步 `probe_local_sidecar`；视频保留同步 Method A/B（需 sub_for_mpv 传 mpv）。
- **扫描期异步**：`expand_local_folder` 的 sidecar 探测（48 文件 × 5 fs::exists）改为提交一个 pool 批任务，📜 稍晚出现，不阻塞展开。
- **timing LOG**：`[MPV] File loaded (X ms after loadfile)` 打 loadfile→File loaded 耗时，定位剩余延迟在 mpv 侧还是 app 侧。

### Method A/B 明确措辞 + 失败原因（你的要求）
- **明确通路**：`[Subtitle] mpv resolves: x.vtt (Method A — mpv renders)` / `[Subtitle] panicast resolves: x.json (Method B — fetching online / local sidecar / audio async)` —— 一眼看出谁在处理。
- **加载结果打 LOG 区（EVENT_LOG）**：`Subtitle loaded: N segments` / `No subtitle for this track` / `Subtitle load failed: <原因>`。
- **失败原因**：字幕存在却加载失败时说明原因——`online fetch returned empty (network/HTTP/proxy?)` / `sidecar file not readable` / `empty content` / `parsed 0 segments (format unrecognized or empty transcript)`，方便用户决定重播/重试。

### 验证
本机 Linux 编译 0 warning。Method A/B 分流：音频→B（panicast 解析 JSON/SRT/VTT/LRC），视频→A（mpv 渲染 VTT/SRT/ASS）。

---

## Panicast_V0.1-Y24.7 — 2026-07-26 — 字幕模块 PARSER 架构重构 + 诊断 LOG + 下载格式保留

> 字幕处理此前散在 rss_parser / app_download / app_playback / app_input 4 处。重构为 PARSER 架构（仿 IFeedParser/ParserRegistry），集中到 `subtitle/` 模块。实测全链路功能不变（DOAC 真实字幕 detect→fetch→parse 311 段 仍通过）。

### 重构（PARSER 架构）
- **新增 `subtitle/subtitle_parser.{h,cpp}`**：`ISubtitleParser` 接口 + `Json/Srt/Vtt/LrcSubtitleParser` 四个格式类 + `SubtitleParserRegistry`（按 type_hint/扩展名/内容派发，含 `[` 歧义、pretty-JSON 等自动识别）+ `find_at`/`find_active`。新增格式 = 加一个 parser 类 + 注册。`TranscriptSegment` 移入此模块。
- **新增 `subtitle/subtitle_manager.{h,cpp}`**：`SubtitleManager` 集中 detect_from_rss(含 type) / probe_sidecar / load_async(抓取+解析+TranscriptStatus 状态机) / poll(UI handoff + L 激活 + offset) / download_sidecar(格式保留) / offset·adjust_offset / status。App 持有 `subtitle_mgr_` 成员，4 处散落逻辑全部委托。
- **`TranscriptParser` 改为薄 facade**：`parse`/`find_at`/`find_active`/`parse_timestamp` 委托 SubtitleParserRegistry/自由函数；`load` 用 registry。`ui.h`/`app_playback.cpp` 调用点不变。
- **App 侧**：`load_transcript`/`probe_local_sidecar` 改为一行委托；`update_lyric_bar_activation` + 旧 handoff 块合并为 `subtitle_mgr_.poll(ui, requested)`（每帧一处）；`adjust_transcript_offset` → `subtitle_mgr_.adjust_offset`；下载 sidecar 块 → `subtitle_mgr_.download_sidecar`。状态机/offset/pending 全部移入 SubtitleManager。
- **RSS 捕获 `type`**：`<podcast:transcript type="application/json">` 的 type 存为 `node->subtitle_type`（新增字段），作解析 hint + sidecar 扩展名，避免 auto-detect 歧义。
- **下载格式保留**：sidecar 按 type/URL 存 `<base>.<ext>`（`.json/.srt/.vtt/.lrc`，不再统一 `.transcript`）；`probe_sidecar`/`find_sidecar` 同步识别这些扩展名（此前漏 `.json`）。
- **全链路诊断 LOG**（集中一处）：`[Subtitle] RSS detected (type=...)` / `local sidecar detected` / `none(has_subtitle=?, subtitle_url=?)` / `using local sidecar` / `fetching online` / `online fetch returned empty` / `loaded N segments (M bytes, type=...)` / `ready: N segments` / `[LYRIC] ...`。load vs parse vs 陈旧缓存一目了然。

### 验证（本机 Linux，独立测试）
- LRC 5 段、compact/pretty/object JSON、find_active 重叠(2)/顺序(1)/LRC(停留) 全通过。
- 真实 DOAC feed：`SubtitleManager::detect_from_rss` 检测到 has_subtitle=true + subtitle_url；真实字幕文件经 registry 解析 311 段。编译 0 warning。

### 说明
- 经实测确认 Y24.6 之前的"字幕不加载"是**陈旧缓存**（非代码 bug）：`r` 重载即恢复。本重构未改解析语义，行为一致。
- method A/B 决策仍留在 `play_current`（与 mpv sub-file 耦合），未移入 SubtitleManager；detect/probe/load/download/offset/status 已集中。

---

## Panicast_V0.1-Y24.6 — 2026-07-26 — 右侧 LYRIC 区复用 L 模式渲染（当前行居中 + 短文本居中）

### 优化
- **[ui] 抽出共享 LYRIC 渲染函数 `draw_lyric_content`**：L 模式底部全宽条与右侧 INFO/LOG 间 LYRIC 区（L 关闭）共用同一渲染逻辑——当前行居中（上一条/当前/下一条）、短文本水平居中、长行 marquee、当前行加粗+绿、重叠多人各一行、z/Z 偏移。只是显示区域不同（全宽底部 vs 右侧窄区），由调用方传窗口/起始行/行数/内宽。
- **[ui] 右侧 LYRIC 区改为"当前行居中"**：此前是"历史行 + 当前行在底部"。现与 L 模式一致：当前行在中间，上下各一行预读。本地文件（F 模式 LRC）等同样受益（走同一渲染路径）。`🎵 LYRIC` 标题与下方 `Event Log` 分隔线保留。
- **[ui] 右侧 LYRIC 区激活条件放宽**：有 transcript 段列表 OR mpv sub-text 历史即激活（此前仅历史非空）。

---

## Panicast_V0.1-Y24.5 — 2026-07-26 — 修复 pretty-printed JSON 字幕被误判为 LRC 不加载

### 修复（回归）
- **[parser] JSON 数组字幕回归（Y24.1 引入）**：Y24.1 改 `parse()` 的 `[` 歧义判断（区分 JSON 数组 vs LRC）时，只看 `[` 紧跟的字符。**带换行/空白的 JSON 数组**（`[\n  {...}`，即 pretty-printed JSON，很多播客 transcript 如此）`[` 后是空白 → 被误判为 LRC → `parse_lrc` 返回 0 段 → 字幕 FAILED 不加载。修复：`[` 后跳过空白再判断下一个非空白字符（`{`/`"` → json，否则 lrc）。已测试：compact/pretty JSON、JSON object、LRC 均正确解析。

---

## Panicast_V0.1-Y24.4 — 2026-07-25 — Ctrl+Y 改为复制光标对象的 URL

### 优化
- **[app] Ctrl+Y 优先复制光标对象 URL**：此前优先复制正在播放项（`playback_node`）的 URL，浏览时无法复制想查验的节目。改为优先复制**光标所在对象**的 URL（feed / episode / local 文件夹 / online 项皆适用），光标无 URL 时才回退到播放项。便于核查"节目前有 📜 但无字幕加载"——直接复制该节目 URL 去查 RSS/transcript。
- **[app] Ctrl+Y 顺带打印 transcript URL**：若光标对象 `has_subtitle`，额外在 LOG 打印其 `subtitle_url`，方便手动抓取排查。
- **[man] 文档补充 Ctrl+Y** 说明。

---

## Panicast_V0.1-Y24.3 — 2026-07-25 — LOG 文案修正

### 修复
- **[app] 精简无字幕 LOG**：`[LYRIC] No subtitle for this track (LYRIC bar is audio-only)` → 删除 `(LYRIC bar is audio-only)` 冗余后缀，改为 `[LYRIC] No subtitle for this track`。

---

## Panicast_V0.1-Y24.2 — 2026-07-25 — 字幕偏移 z/Z 同步 + 重叠多人各一行 + 短文本居中 + 状态栏[]居中

> 问题③剩余两项实现：① 字幕恒定偏移可用 z/Z 实时微调并持久化；② 重叠说话（多人同时）各显示一行。另：L 模式短字幕行水平居中；状态栏 `[]` 改为在左右内容块之间居中（等间隙）。

### 新增
- **[app] 字幕偏移 + z/Z 实时同步（方案1）**：新增 `transcript_offset_`（秒），method-B LYRIC 查找用 `effective_time = time_pos - offset`（正值=字幕延后，同 mpv sub-delay 语义）。直接按 `z`/`Z`（非 `:` 命令窗）±0.1s 实时调整，持久化到 `[transcript] offset`，EVENT_LOG 显示当前值，并在 LYRIC 条标题显示 `🎵 LYRIC +0.3s`。治恒定漂移（片头/广告/ASR 起点偏移）。
- **[parser/ui] 重叠多人各一行（方案3）**：新增 `TranscriptParser::find_active(segs, time)` 返回该时刻所有 active 段（`start≤t<eff_end`，无 end 的段算到下一 cue）。L 条窗口以最新 active 段居中，**所有 active 段都加粗**——两人同时说话时各占一行。顺序/非重叠字幕仍只 1 行（行为不变）。`parse_json` 现按 start 排序（find_at/find_active 假设有序）。

### 修复
- **[ui] L 模式短文本居中**：`draw_lyric_bar` 中短于全宽的字幕行水平居中显示；超长行仍左对齐 + marquee。
- **[ui] 状态栏 `[]` 间隙居中**：`[]`（中间 URL 区）此前按整窗居中，左右内容块宽度不等时间隙不对称、视觉偏移。改为在左右块之间居中（等间隙）；左右等宽时与原行为一致。间隙不足时回退整窗居中。

### 限制（如实反馈）
- 字幕数据本身时间轴错误（ASR 变速漂移、缺段）无法靠代码修复——z/Z 只能治恒定偏移，治不了全程渐变漂移。需换更准的字幕源。

---

## Panicast_V0.1-Y24.1 — 2026-07-25 — LRC 支持 + 本地 sidecar 探测 + feed 📜 改"任意"

> 三个问题修复：① F 模式本地文件同名 .lrc 字幕未识别/未加载；② DOAC 节目带 📜 但订阅根节点不带（feed 级 📜 判定过严）；③ JSON 字幕时间轴不准（待方案确认，本轮未实现，见下）。

### 修复
- **[parser] 新增 LRC 字幕解析（问题①）**：`TranscriptParser::parse_lrc` 解析 `[mm:ss.xx]text`（含多时间戳重复行、跳过 `[ti:]/[ar:]/[offset:]` 元数据）。`parse()` 按扩展名 + 内容自动识别 lrc/json/vtt/srt（解决 `[` 歧义：LRC 时间标签 vs JSON 数组）。`load()` 按扩展名给 type_hint，避免 LRC 以元数据开头时误判。
- **[parser] 修复 JSON 缺 endTime 的段被丢弃（问题③相关）**：`parse_json` 此前要求 `end >= start`，Podcasting 2.0 JSON 常只有 `startTime`（end=0）→ 段被丢弃 → 字幕出现空隙 → 高亮错位（"不同步"）。改为 end < start 时 clamp 到 start（该行停留到下一 cue，同 LRC）。
- **[app] 本地 sidecar 探测（问题①）**：新增 `probe_local_sidecar(node)`，对有 `local_file` 的节点探测同名 `.lrc/.srt/.vtt/.transcript`，命中则置 `has_subtitle + subtitle_url`。三处调用：`expand_local_folder`（F 模式扫描时 → 树里显示 📜）、`play_current`（播放决策前 → 进入 LYRIC 路径）、`load_transcript`（自动换曲路径）。F 模式本地文件补 `local_file` 字段。
- **[app] feed 级 📜 改"任意"（问题②）**：`spawn_load_feed` 此前要求**所有**节目都有 transcript 才标 feed，DOAC（多数但有部分无）因此不标。改为**任意 ≥1** 节目有 transcript 即标 feed，并 LOG 计数 `N of M episodes have transcript`。
- **[ui] L 模式当前字幕居中显示**：`draw_lyric_bar` 改为以当前段为中心的窗口视图——上一条在上、当前（bold/绿）居中、下一条在下，便于预读。3 行默认 = 1 上 + 当前 + 1 下（`lyric_bar_height` 调大可显示更多上下文）。无 transcript 段列表时（仅 mpv sub_text）回退为历史行、当前在底。

### 待确认（问题③，本轮部分修复）
- 已修复：JSON 缺 endTime 段丢弃（空隙→错位）；L 模式当前行居中（预读上下文）。
- 待你拍板：字幕整体偏移校正（`[transcript] offset` + z/Z 实时微调，治恒定漂移）；重叠多人各一行（治同时说话）。详见对话分析。数据本身时间轴错误（ASR 变速漂移/缺段）无法靠代码完全修复——如实反馈。

---

## Panicast_V0.1-Y24 — 2026-07-25 — L 模式：底部全宽 LYRIC 显示条 + 字幕可用性门控

> 新增 `L` 模式：音频播放时把字幕从右侧窄 LYRIC 区移到**底部全宽 LYRIC 条**（3 行字幕，自动滚动），状态栏在 L 激活时隐藏让出空间。仅当字幕就绪(READY)才激活；加载中/无字幕保留状态栏并打 LOG。

### 新增
- **[ui] L 模式 — 底部全宽 LYRIC 条**：新增 `lyric_win`（全宽，默认 5 行 = 顶边框 + 3 行字幕 + 底边框）。`L`(大写)切换，INI `[display] lyric_bar`(默认 off) / `lyric_bar_height`(默认 5)。小写 `l` 不变。
  - 激活时 `top_h = term_h - 5`，左/右窗缩高，`lyric_win` 占底部，**状态栏隐藏**；关闭时恢复原布局（状态栏 3 行）。
  - 3 行字幕 = 2 历史(dim) + 当前行(bold, 绿色)，长行横向 marquee + 纵向自动滚动。全宽 → 长歌词基本无需滚动（解决右侧窄面板痛点）。
  - **右侧 INFO/LOG 间的 LYRIC 区在 L 激活时自动关闭**，避免重复；INFO/LOG 复用空间。
- **[app] 字幕可用性门控（TranscriptStatus 状态机）**：新增 `TranscriptStatus { NONE, LOADING, READY, FAILED }`，`load_transcript` 维护（无字幕→NONE；异步抓取→LOADING；非空→READY；空/0 段→FAILED）。`lyric_bar_active_ = requested && READY`——**仅字幕就绪时才显示 LYRIC 条**；加载中/无字幕/失败时**不占位**，保留状态栏作回退。
- **[ui] 共享 lyric_history 更新**：抽出 `update_lyric_history(state)`，右侧 LYRIC 区与底部 LYRIC 条共用，避免重复逻辑。
- **[ui] 切换强制重绘防花屏**：L 激活/关闭（top_h 变化）触发 `clearok(stdscr, TRUE)` 全屏重绘 + 四窗 werase，避免缓冲区残留；`handle_resize` 同步重置 `last_lyric_bar_active_`。

### LOG（全英文）
- L 激活转换打英文 LOG：`Transcript ready, LYRIC bar activated` / `Subtitle loading, LYRIC bar will activate on completion` / `No subtitle for this track (LYRIC bar is audio-only)` / `Subtitle load failed, LYRIC bar standing by` / `LYRIC bar off`。
- 把 `app_run.cpp` 3 条中文 EVENT_LOG（yt-dlp/JS 运行时警告）改为英文，满足"LOG 全英文"。

### 影响
- 视频播放字幕仍在视频窗口渲染，L 模式仅用于音频（视频按 NONE 处理）。
- 终端过小（<16 行）时 L 条不激活，回退状态栏。

---

## Panicast_V0.1-Y23.10 — 2026-07-25 — 字幕缓存持久化 + feed 级 📜 标记 + 字幕处理全链路 LOG

> Y23.9 的两个字幕问题：① `r` 重载节点信息后缓存到本地数据库，但再次打开节点时节目前的 📜 LYRIC EMOJI 消失；② 播放时字幕的识别/在线加载没有任何 LOG，用户无法掌握行为。新增需求：P 模式下若订阅的所有节目都有 transcript，在订阅 TITLE 前加 📜 便于快速选择。

### 修复
- **[storage] episode_cache 持久化 has_subtitle + subtitle_url（问题①根因）**：`spawn_load_feed` 构建的 `episodes_json` 快照此前只含 `url/title/duration/is_youtube`，漏掉 `has_subtitle`，导致 `save_episode_cache` 写入时 `has_subtitle` 恒为 0 → 重开节点从缓存读出后 📜 丢失。补齐 `has_subtitle` 与 `subtitle_url` 两个字段。
- **[storage] episode_cache 新增 `subtitle_url` 列**：SCHEMA_VERSION 43→44；CREATE TABLE 加 `subtitle_url TEXT`；`add_column_if_missing` 幂等迁移（老库 ALTER ADD COLUMN，episode 行保留）。`save_episode_cache` 写入、`load_episodes_from_cache` 读取（返回 6-tuple，第 6 项为 subtitle_url）。
- **[app] 7 处缓存消费方恢复 subtitle_url**：`app_navigation.cpp`(3) + `app_tree_expand.cpp`(4) 的结构化绑定由 5-tuple 改 6-tuple，新增 `child->subtitle_url = ep_sub_url`。重开后不仅 📜 还在，播放时也能取到 transcript URL 真正加载字幕。
- **[storage] tree_nodes 持久化 feed 级 has_subtitle**：`save_tree_node_recursive` INSERT 与 `load_tree_node_recursive` SELECT 同步 `has_subtitle` 列（列早已存在但此前未读写）。使折叠状态下的订阅节点也能跨重启显示 📜。

### 新增
- **[app] feed 级 📜 标记（新需求）**：`spawn_load_feed` 解析完节目后，若**所有**节目都有 transcript，则置 `feed->has_subtitle = true`，订阅行显示「📁 📜 Feed Title」。通过 save_tree 持久化，重启后折叠列表即可见。`r` 重载会重新计算保持新鲜。老库需对相应订阅按一次 `r` 触发首次计算。
- **[app] 音频字幕路径改为 method B（兼容性修复）**：此前音频节目若字幕 URL 是 `.srt/.vtt`，会走 method A（把远程 URL 作为 `sub-file` 交给 mpv）。mpv 对**远程** sub-file 的加载常静默失败（重定向/SSL/超时）→ LYRIC 面板空白，但节目仍带 📜（如 omny.fm 节目）。现在：**视频**仍用 method A（mpv 在视频窗渲染）；**音频**一律走 method B（TranscriptParser 经 curl `Network::fetch` 抓取，可靠处理 HTTPS/重定向，直接驱动 LYRIC 面板）。本地 sidecar 同样由 method B 解析。
- **[app/parser] 字幕处理全链路 LOG（问题②）**：
  - `rss_parser`：识别到 RSS `<podcast:transcript>` 或本地 sidecar 时 LOG `[RSS] transcript detected for '...': URL` / `transcript sidecar detected`。
  - `play_current`：LOG 字幕路径决策 `[Subtitle] method A (mpv sub-file)` / `method B (TranscriptParser)` / `none for '...'`。
  - `load_transcript`：LOG `[Transcript] none(...)` / `using local sidecar` / `fetching online transcript`（并 EVENT_LOG "Loading transcript"），加载完成 LOG `ready: N segments`。
  - `TranscriptParser::load`：在线抓取前 LOG `fetching online`，失败 LOG `online fetch returned empty`，成功 LOG `loaded N segments (M bytes)`。
- **[storage] favourites 同步 LOG 改用正确措辞**：Y23.9 已删除误导性的 `[DB] Cleared favourites table`（`clear_favourites` 现静默）。改为在 `Persistence::save_data` 记一条 `[DB] Syncing subscriptions (N feeds) + favourites (M entries) to DB`，准确描述「同步（清空+重写，单事务）」而非「删除某表」。`clear_favourites` 注释同步更正。

### 影响
- 老数据库（v43）首次打开自动迁移到 v44（幂等 ALTER），episode/favourites 用户数据保留。
- 老订阅的 feed 级 📜 需 `r` 重载一次后落库。

---

## Panicast_V0.1-Y18 — 2026-07-24 — 修复 B 模式扫码登录(Set-Cookie 捕获) + QR 弹窗去 URL

> Y17 的 B 模式 QR 登录失败：Bilibili API 返回 SESSDATA 在 HTTP Set-Cookie 响应头(不在 JSON body)，Network::fetch() 只返回 body 所以读不到。QR 弹窗太宽因 URL 长度参与宽度计算。

### 修复
- **[net] poll_qrcode 捕获 Set-Cookie 头**：改为直接用 curl(CURLOPT_HEADERFUNCTION 回调)捕获响应头，从 `Set-Cookie: SESSDATA=xxx; ...` 解析 SESSDATA/bili_jct/DedeUserID。同时保留 body 的 cookie_info 作为 fallback。失败时 LOG 打印响应头前 500 字符便于排查。
- **[app] QR 弹窗去掉 URL 显示**：B 模式 QR 弹窗不再显示长 URL 文本；弹窗宽度按 `max(60, qr_w + 4)` 计算(基于 QR 尺寸,不基于 URL 长度),与 Y 模式一致。QR 码全分辨率渲染不截断。

## Panicast_V0.1-Y17 — 2026-07-24 — Y16 剩余 5 项全部完成 + Y16 已完成 3 项

> Y16 全 8 项功能完整交付（#2/#7/#8 在 Y16 已完成，#1/#3/#4/#5/#6 在 Y17 完成）。

### Y17 新增（#1/#3/#4/#5/#6）
- #1 P 模式 a 订阅 B/T：URLClassifier 加 BILIBILI_CHANNEL/BILIBILI_VIDEO/DOUYIN_USER/DOUYIN_VIDEO；spawn_load_feed 路由 B/T URL 到 yt-dlp --flat-playlist（带对应 cookies）。
- #3 字幕检测：RSS parser 解析 <podcast:transcript> URL → TreeNode has_subtitle=true + subtitle_url；本地 sidecar (.srt/.vtt/.lrc) 同名文件检测。
- #4 播放加载字幕：play_current 传 sub-file=<subtitle_url> 给 mpv（per-file option）→ LYRIC 面板自动显示（sub-text 机制，同 YouTube/本地 LRC）。
- #5 统一 Ctrl+B：上下文感知（Y 模式→YouTube cookies / B 模式→Bilibili cookies），同一个 input_box UI。
- #6 episode_cache has_subtitle：SCHEMA_VERSION 42->43，加 has_subtitle 列；save/load 同步；load_episodes_from_cache 返回 5-tuple。

### Y16 已完成（#2/#7/#8）
- #2 📜 emoji（TreeNode has_subtitle + draw_line 标题前渲染）
- #7 LOG 区两行完整 codec（PLAYBACK_RESTART）
- #8 INFO VO/AO 行加解码器（[hwdec] / [codec]）
- Y15 编译告警修复

## Panicast_V0.1-Y16 — 2026-07-24 — INFO VO/AO 解码器 + LOG 两行 codec + 字幕 emoji + Y15 告警修复（部分完成）

> Y16 全 8 项功能中已完成 3 项 + 告警修复，剩余 5 项下一步。

### 已完成
- #8 INFO VO/AO 行加解码器（VO 行加 hwdec，AO 行加 audio-codec）
- #7 LOG 区两行完整 codec 信息（PLAYBACK_RESTART 时，Video+Audio 完整行）
- #2 TreeNode 加 has_subtitle/subtitle_url + draw_line 渲染 emoji（标题前，始终可见）
- Y15 编译告警修复（-Wswitch BILIBILI + -Wmisleading-indentation）

### 待完成
- #1 P 模式 a 订阅 B/T（URLClassifier + yt-dlp）
- #3 字幕检测（RSS podcast:transcript + sidecar）
- #4 播放加载字幕（sub-file -> LYRIC 面板）
- #5 统一 cookie/登录（Ctrl+B 上下文感知）
- #6 episode_cache 缓存 has_subtitle

## Panicast_V0.1-Y15 — 2026-07-23 — B 模式(Bilibili)基础架构：API 类 + QR 登录 + cookie 管理 + 模式/DB/INI

> 用户需求：扩展 B 模式(Bilibili)，支持扫码登录 + Cookie 导入，浏览关注/搜索/播放/下载。

### 变更（基础架构，login/browse/play 接线下一步）
- **[net] BilibiliAPI 类**(`src/net/bilibili_api.cpp`+header)：QR 登录(`request_qrcode`+`poll_qrcode`→SESSDATA cookie)、`fetch_nav`(用户信息)、`fetch_followings`(关注列表)、`build_cookies_txt`(Netscape 格式供 yt-dlp)、`extract_sessdata_from_cookies_txt`(Cookie 导入解析)。无 WBI 签名(搜索/视频列表走 yt-dlp 提取，不需 API)。
- **[net] Network::fetch_cookie**：cookie-based HTTP GET(`Cookie:` 头)，供 BilibiliAPI 用。
- **[db] bilibili_accounts 表**：SCHEMA_VERSION 41→42，存 uid/uname/sessdata/bili_jct/dedeuserid。
- **[app] AppMode::BILIBILI + B 键**：`B` 键上下文相关(ONLINE 模式→切区域，其他模式→Bilibili 模式)；`bilibili_root` 树节点；APP_MODE_COUNT 6→7；restore_player_state 加 BILIBILI case。
- **[config] [bilibili] cookies_file**：INI 可配(默认 `<数据目录>/bilibili_cookie.txt`)，路径解析同 YouTube cookies_file。

### 待实现（Y15 下一步 / Y16）
- 登录 UI：QR 扫码弹窗(复用 Y 模式 qr_login_poll 模式)+ Cookie 导入(文件路径输入)。
- 浏览：展开账号→关注列表(API)→展开 UP 主→yt-dlp flat-playlist 视频列表。
- 搜索：`/` → yt-dlp bilisearch。
- 播放：`resolve_bilibili_url`(yt-dlp -g --cookies bilibili_cookie.txt)→ mpv。
- 下载：yt-dlp --cookies → 本地文件。
- B 站 CC 字幕：sub-text → 歌词面板(同 YouTube/本地 LRC)。

### B 模式 vs Y 模式
- 不需 OAuth2(QR→cookie 更简单)、不需 quickjs/deno(无 nsig)、无 Data API 配额。
- yt-dlp 完整支持 Bilibili(13 个提取器，实测可用)。

## Panicast_V0.1-Y14 — 2026-07-23 — layout_ratio 0.25 + mpv 字幕设置 INI 化 + 歌词当前行绿色高亮

> 左面板缩小(0.25)右面板扩大(0.75)；mpv 字幕设置从硬编码改为 INI 可配；歌词当前行用主题绿色高亮。

### 变更
- **[config] layout_ratio 默认 0.4→0.25**：左面板 25%、右面板 75%（INFO/LOG/歌词区更宽敞）。改 3 处：`DEFAULT_LAYOUT_RATIO`、INI 模板、`ui.h layout_ratio_`。
- **[config/mpv] 字幕设置 INI 化（Y14，原 Y13 硬编码）**：`sub-ass-override`/`sub-align-x`/`sub-align-y`/`sub-visibility` 从 `mpv_set_option_string` 硬编码改为 `[mpv]` INI getter（默认 auto/center/bottom/yes）。INI 模板加 4 个条目+注释。
- **[ui] 歌词当前行绿色高亮**：从 `A_BOLD` 改为 `A_BOLD | COLOR_PAIR(11)`（与树"正在播放"同色，主题自适应——每套主题重定义 GREEN）。

---

## Panicast_V0.1-Y13 — 2026-07-23 — 歌词面板重设计(🎵 LYRIC 分隔线+空行) + 15 套主题(3 新增高对比交错分布)

> Y12 歌词条下方有横线残余；12 套主题太趋同。Y13 重设计歌词区为独立分区 + 扩展到 15 套主题。

### 变更
- **[ui] 歌词面板重设计**：移除 Y12 的 mvwhline 残余。改为三区布局（INFO / LYRIC / LOG），每区用内嵌标题分隔线（与 "INFO & LOG"、"Event Log" 同风格）：lyric 激活时 split_y 分隔线印 "🎵 LYRIC"（替代 "Event Log"），下方画 [空行] [3 行歌词·当前高亮] [空行] [Event Log 分隔线]，EventLog 上界偏移 `lyric_h=ll+3`。无歌词时回退原 INFO / Event Log / LOG 两区。歌词上方新增一空行（用户要求）。
- **[theme] 15 套主题（12 原有 + 3 新增高对比，交错分布）**：新增 Cobalt2（深蓝·高对比亮黄+电青·极客）、Horizon Dark（暗酒红·七彩均衡·绚丽不刺眼）、Material Ocean（近黑紫·鲜艳多色·Material 风）。3 套新高对比交错分布在位置 5/10/15（每切 4 个原有一个新的，风格交替）。THEME_COUNT 12→15。themes.cpp 用 python 脚本生成（hex→0-1000）。`?`/README/man 主题数同步更新。



---

## Panicast_V0.1-Y12 — 2026-07-23 — 歌词面板(C-1 右侧带,自动滚动) + audio-display=no

> 用户需求：F 模式播本地 mp3(+.lrc) 时在 TUI 实时显示歌词；mpv 默认 `audio-display=no`（音频文件不弹专辑封面窗）。

### 变更
- **[ui/playback] 歌词面板（C-1：右侧 INFO/LOG 之间一条带，自动滚动）**：mpv 把 `.lrc` 当外部字幕轨加载，`sub-text` 属性返回当前时间点歌词行。`update_state()` 轮询 `sub-text`（跟随 `state_refresh_ms`）存入 `State.sub_text`；UI 维护 `lyric_history_` deque（sub_text 变化时 push，换曲清空），在右侧面板 `split_y` 下方画 `lyric_lines`（默认 3）行，当前行（最新）`A_BOLD` 高亮、长行截断、下方画分隔线；EventLog 上界由 `split_y` 改 `split_y+lyric_h` 避免重叠。**自动滚动**= 歌词逐行推进（新行入、旧行上移），符合"自动滚动/换行"（换行=逐行推进，非文本折行）。本地 `.lrc` 和 YouTube 软字幕(sub-file)通用（都走 sub-text）。INI `[display] lyric=true`(默认开) + `lyric_lines=3`。无歌词时隐藏整条。audio-only（无视频窗）时 mpv 自己的 OSD 字幕 overlay 不显示，TUI 是唯一途径。
- **[playback] mpv `audio-display=no` 默认**：播放音频文件（mp3 含嵌入封面）时不弹专辑封面图窗。原先 mpv 默认 `audio-display=embedded-first` 会为带封面的音频开一个图片窗，现置 `no` 保持音频播放无窗。



---

## Panicast_V0.1-Y11 — 2026-07-23 — `:` mpv 热键扩展 + Network/Buffering + 统一定时器 + 12 套主题 + YouTube 软字幕 + 修自动播放 INFO 不更新

> 用户需求：扩展 `:` 的 mpv 热键（加 zoom、对齐原生）；INFO 显示网络速率+缓冲；播放状态统一定时器 INI 可调；重设计主题（12 套 GitHub 流行配色、软前景、配色单独 cpp）；YouTube 软字幕（居中/缩放可用）；修自动播放下一首时 INFO 标题不更新。

### 变更
- **[app/ui] `:` 命令窗口 mpv 热键全面扩展（对齐 mpv 原生 input.conf）**：重排 `mpv_cmds` 表——保留 `f F G o O i I m`（原生）；`v` 由 cycle vid 改为 **cycle sub-visibility**（原生 v）、`a` 由 cycle aid 改为 **`#` cycle audio**（原生 #）、cycle vid 移除（无原生键）；新增 `z/Z` sub-delay、`r/R` sub-pos、`j/J` cycle sub、`d` deinterlace、`l` ab-loop、`s/S` screenshot(含/不含字幕)、`A` cycle video-aspect、`1-8` 视频 EQ(contrast/brightness/gamma/saturation)；新增 **zoom**：`+`/`-` video-zoom ±0.1、`=` 复位（mpv 原生 zoom 是 Alt++/Alt+-，单字符 `:` 窗口抓不到 Alt，故用裸 +/- 最近似）。`o` 改为 `show-progress`（原生，原 `osd` 非标准命令）。完整热键表写入 man/`?`/README。
- **[ui/playback] INFO 区加 Network/Buffering 行**：`update_state()` 读 mpv `cache-speed`(下载速率 bytes/s)、`demuxer-cache-duration`(缓冲 ahead 秒)、`cache-buffering-state`(缓冲中 0-100)，存入 State；`draw_info` 在 AO 行下加一行 `Network: <速度> | Buffering: <秒或%>`（缓冲中显百分比）。纯文本标签（主题 fg 色，任意主题适配，**无 emoji 无 ⚠**），低缓冲不加标记（数值自明），标签 `Buffering:`（原 Buf）。无 latency 字段（mpv 无 rtt/latency 属性，demuxer-cache-duration 即最接近值）。
- **[playback/config] 播放状态统一刷新定时器（INI 可调）**：`update_state()` 开头按 `[display] state_refresh_ms`（默认 100ms）节流——不到间隔直接 return（保留上次 state_），到点才读全部播放属性（codec/bitrate/network/VO/AO/position/...）。**一个定时器统管所有播放状态**，最简单。原 50ms 每轮全读改为 100ms 节流，降 mpv_get_property 频率；INI 可调。
- **[theme] 重设计 12 套 GitHub 流行终端配色 + 配色单独 cpp**：移除原 9 套（含刺眼的纯 ANSI "Dark"，fg=纯白 #ffffff 致反光）。新增 12 套全深色 + **软前景**（无纯白，直接解决"白太亮"）：Solarized Dark(默认 index0)/Gruvbox Dark/Nord/Dracula/Catppuccin Mocha/Tokyo Night/Rose Pine/One Dark/Everforest/Kanagawa/Ayu Mirage/Monokai Pro。RGB 值取自各家官方仓库（hex→0-1000）。**配色表移到独立 `src/theme/themes.cpp`**（+ `include/panicast/theme/themes.h` 声明 `struct Theme`/`THEME_COUNT=12`/`themes()`），方便调整不动 UI 逻辑；ui.h 仅 include。CMakeLists 加 themes.cpp。Ctrl+L 循环 12 套。
- **[playback] YouTube 软字幕加载（修字幕不居中/不能缩放）**：根因——`resolve_youtube_url` 用 `yt-dlp -g` 只取视频+音频流 URL，**不取字幕**→mpv 无软字幕轨→F/G(sub-scale)无目标；可见字幕是 burned-in 硬字幕（mpv 无法缩放/居中）。修复：resolve 后当 `[youtube] sub_lang` 非空时，追加一次 `yt-dlp --write-subs(--write-auto-sub) --sub-langs <lang> --sub-format vtt --skip-download -o <tmp>` 写出 .vtt，路径作为 `urls[2]` 返回；`play_video` 的 loadfile options 串加 `sub-file=<path>`（与 `audio-file=` 并列，per-file option）。mpv init 加 `sub-ass-override=auto`（让 ASS 字幕也响应 sub-scale/sub-pos）。vtt 软字幕→mpv 默认居中渲染、F/G 缩放、r/R 位移、z/Z 同步、v 显隐全可用。INI `[youtube] sub_lang`(默认空=不加载,opt-in) + `sub_auto=true`(无手动字幕用自动生成)。硬字幕无法处理（视频本身）。
- **[app] 修自动播放下一首时 INFO 标题/信息不更新**：`on_playback_ended` 自动推进下一首时内联播放（不调 `play_current`），而 `playback_node` 只在 `play_current` 里被赋值→INFO 标题停在上一首。修复：`on_playback_ended` 在 `current_index = next` 后设 `playback_node = current_playlist[next].node`（与 current_index 同样的跨线程模式，TreeNode 由树持有故 shared_ptr 重赋值实际安全）。
- **[app] 异步交互审计：所有网络/解析交互改为 pool 异步，UI 不阻塞**：审计发现 3 处同步网络调用阻塞 UI 线程，已全部改 pool_.submit：① `perform_youtube_search`（`GoogleOAuth::search` 原同步 ~1-2s）→ pool 异步搜+建树，新增 `pending_select_` 机制（pool 任务设该节点，UI 线程每帧 flatten 后消费、移动光标到搜索结果）让选中不丢；② `subscribe_youtube_channel`（`subscribe`+`fetch_subscriptions` 原同步）→ pool 异步；③ `start_account_login` 的 `fetch_identity`+建账号+sync（QR 扫码后原同步冻结）→ 全部并入一个 pool 任务，UI 扫码后立即返回。`r`(refresh_node/spawn_load_feed)、`l`/Enter(enter_node/spawn_load/resolve)、`d`/D(download)、Y-mode resync 本就 pool 异步。主循环每帧 flatten 在 tree_mutex 下，pool 任务改树安全。

### 已知
- zoom 的 `+`/`-` 在 `:` 命令模式下是画面缩放，与直接按 `-`/`+`（音量）不冲突（前者需先按 `:` 进命令模式）。
- Network 速率/缓冲仅在有媒体播放时显示。



---

## Panicast_V0.1-Y10 — 2026-07-23 — 修 Y09 DASH 播放无音频（audio-file 属性不存在）

> Y09 1A DASH 播放在 WSL2 上视频有画无声。根因：mpv 无 `audio-file`（单数）运行时属性。

### 修复
- **[playback] DASH 外挂音频改用 loadfile options（修无音频）**：Y09 用 `mpv_set_property_string(ctx_, "audio-file", url)` 挂外部音频流——但 `mpv --list-properties` 证实 mpv **只有 `audio-files`（复数列表）属性，无 `audio-file`（单数）**，故该调用返回 `MPV_ERROR_PROPERTY_NOT_FOUND` 静默失败 → DASH 视频流（video-only）播放无音频。改为 `loadfile` 第 4 参 `<options>` 传 `audio-file=<audio_url>`（per-file 选项，等价 CLI `--audio-file=`），第 3 参 index 设 `-1`（mpv 0.38+ 占位）。非 DASH（单 URL）照旧 `loadfile url replace`。本机 mpv 0.41 `-fsyntax-only` 通过。

---

## Panicast_V0.1-Y09 — 2026-07-23 — 1A DASH 1080p 播放 + client_secret 编译时自动检测 + P/Y 独立说明 + man/?/CLI 全面更新

> 用户确认 1A（yt-dlp 在 mpv 之前预解析 DASH 双流）并要求全面更新 man/?/CLI/GitHub 介绍。

### 变更
- **[playback] 1A DASH 1080p 播放（音视频流分离）**：`resolve_youtube_url(url, has_video)` 按 has_video 选 `[youtube] play_format_video`（默认 `bestvideo[height<=1080]+bestaudio/best`，1080p DASH）/ `play_format_audio`（默认 `bestaudio/best`，最高音质），`-g` 收集**1-2 条流 URL**（DASH=2）。`MPVController::play/play_video` 增 `audio_file` 参数：2 条时 `loadfile(video)` + `audio-file=audio` 让 mpv 合流（即 mpv ytdl_hook 内部做法）。修 F23 以来 `-f best` 单 URL 封顶 720p 的问题——现可达 1080p（YouTube 无 1080p 单文件，必须 DASH）。`[mpv] vo/ao` 注释列可选值。
- **[net/build] OAuth client_secret 编译时自动检测**：CMake `configure` 时 `if(EXISTS secrets/client_secret.json)` 读出 client_id/secret → `configure_file` 生成 `client_secret_builtin.h` 烘焙为内置默认；无则用项目兜底客户端（`781435869525-…`）。`google_oauth.cpp` 改用该头（删硬编码常量）。运行时 `<data_dir>/client_secret*.json` 仍优先。新增 `secrets/client_secret.json.example` 模板 + `.gitignore`（`secrets/client_secret.json`、生成的头）。三种配置方式（运行时文件 / 编译时 bake / 兜底）写入 README+man。
- **[docs] P 模式 vs Y 模式独立工作说明**：README 新增「P 模式 vs Y 模式（独立工作）」节——P 用 cookies 不需登录、Y 用 OAuth 列表但播放仍需 cookies，共享 `ytdlp_youtube_args_parse`+`episode_cache` 但互不依赖。解释了"修 Y 连带修好 P"的原因。
- **[docs] man/?/CLI 全面更新**：man 页 `.TH` 升 Y09、新增 YOUTUBE（P vs Y）+ YOUTUBE CONFIGURATION + FILES（cookies.txt/client_secret）节；`?` 帮助增「Video/Audio Quality (INI)」段（play_format_video/audio + `:` mpv OSD 命令）；`print_usage` 增 YouTube config（cookies/js_runtime/play_format）+ P/Y 模式说明。
- **[docs] GitHub 介绍**：README 顶部 badge 升 Y09、加 quickjs-ng/OAuth badge、介绍行点出 P+Y 双模式 + 顶部导航加「P vs Y 模式」链接。

### 设计决策（记录）
- **1A vs mpv ytdl_hook**：选 1A（panicast 预解析 yt-dlp 取双流 → mpv audio-file 合流），非 mpv ytdl_hook。理由：panicast 完全掌控 argv（cookies/player_client/js_runtime 单一路径）、异步不阻塞、错误透明可日志、与 Y05 单一 resolve 架构一致；mpv ytdl_hook 边角鲁棒（直播/HLS/重选）但对播客场景罕见，且 ytdl-raw-options 配置脆弱、错误不透明、与 Y05 冲突。
- **1080p 必须 DASH**：YouTube 无 1080p 单文件 muxed（最高 720p），1080p 需 `bestvideo+bestaudio` 双流 + ffmpeg 合流——这是现代流媒体主流（YouTube/Netflix/B站均 DASH）。



---

## Panicast_V0.1-Y08 — 2026-07-22 — 默认 quickjs + DB schema 迁移修 YouTube 缓存 + EJS 预检可靠化

> Arch 验证 quickjs-only 播放成功并删了 deno(省 106MB)；WSL2 Debian 装 quickjs-ng+ejs。本版按"现状(quickjs-only)"固化配置与 DB。

### 变更
- **[config] `js_runtime` 默认值 空→`quickjs`**：`get_youtube_js_runtime()` 在键**缺失**时返回 `quickjs`（旧 config 自动用 qjs，删了 deno 也不会断 nsig——Arch 删 deno 后旧 config 无此键正属此坑）。键存在但空=显式不注入（yt-dlp 默认 deno）。INI 模板 `js_runtime = quickjs` 注释同步。
- **[config] cookie 默认路径显式化**：INI 模板 `cookies_file = youtube_cookie.txt`（空/bare → `<数据目录>/youtube_cookie.txt`，Y05 已实现的解析逻辑不变）。
- **[db] 修 YouTube 缓存写不进库（`table episode_cache has no column named is_youtube`）**：`is_youtube` 等列加进 CREATE TABLE 时未 bump SCHEMA_VERSION，旧 DB（已 v40、旧 episode_cache/favourites 结构）永不迁移→列缺失→缓存写失败。SCHEMA_VERSION 40→41，新增幂等 `add_column_if_missing()`（PRAGMA table_info 查列，缺则 ALTER TABLE ADD COLUMN），对 `episode_cache.is_youtube` 与 `favourites.{is_youtube,channel_name,source_type,is_link,link_target_url,is_local_folder}` 补列。每次 init 跑（幂等，新库列已在不动作）。YouTube 频道节目缓存、收藏现在正确落库；登录信息(OAuth tokens)本就在 accounts 表。
- **[app] EJS 预检改用 yt-dlp 自身环境**：启动预检的 EJS 探测从 `python3 -c "import yt_dlp_ejs"`（系统 python3 与 yt-dlp 安装环境不一致会误报）改为 `yt-dlp -v 2>&1 | grep -q yt_dlp_ejs`（问 yt-dlp 自己，可靠，消除 WSL2 "缺 EJS solver" 误报）。

### 现状(quickjs-only)部署要点
- 两台机均装 quickjs-ng(`qjs` 在 PATH)+ `pip install -U "yt-dlp[default]"`(带 ejs)。
- Arch 已 `pacman -Rdd deno` 删 deno；Debian 本就无 deno。
- `js_runtime = quickjs`（Y08 后旧 config 也默认 quickjs）。
- 播放仍需 cookies(默认 `<数据目录>/youtube_cookie.txt`)+代理(GFW 用 socks5h)；节目列表走 Data API(OAuth)。

---

## Panicast_V0.1-Y07 — 2026-07-22 — Y 模式扫码登录后恢复 UI 焦点 + 启动依赖预检

> 用户反馈：Y 模式扫码登录后 UI 失去光标焦点、j/k 不响应；且之前"no yt-dlp output"报错隐晦（实为 yt-dlp 未安装）。本版修 UI 焦点 + 启动即提示缺失依赖。

### 修复
- **[ui/app] QR 登录弹窗返回时恢复主 UI 输入焦点**：`qr_login_poll`（app_account.cpp）此前用独立弹窗 `win`（`keypad(win,TRUE)`+`nodelay(win,TRUE)`）轮询 `wgetch(win)` 数秒等扫码，退出仅 `delwin(win);touchwin(stdscr);refresh()`——既没清 typeahead/半读转义序列，也没重新断言 stdscr 输入态。残留的半截转义序列让主循环 `wget_wch(stdscr)` 把后续 j/k 当转义字节吃掉→j/k 失灵。现 `done:` 段增 `flushinp()` 清缓冲 + 重新断言 `keypad(stdscr,TRUE)/timeout(30)/curs_set(0)/noecho()` 再 `touchwin+refresh`。对比 input_box 弹窗（用 `mvwgetnstr` 干净读到回车+退出恢复 noecho/curs_set）本不残留，QR 的 nodelay 轮询最易留下半截序列。
- **[app] 启动运行时依赖预检**：`App::run` 在 `player.initialize()` 后探测 yt-dlp（`Utils::which_binary`）、qjs/qjsng/deno、以及 quickjs 的 EJS solver（`python3 -c "import yt_dlp_ejs"`，best-effort 非致命）。缺哪个在 LOG 面板一次性提示（如「yt-dlp 未安装/不在 PATH—装 pip install -U "yt-dlp[default]"」），不再等用户播放时才看到隐晦的 "YouTube resolve failed (no yt-dlp output)"。

### 已知限制（沿用 Y05/Y06）
- 播放需 yt-dlp + cookies(Ctrl+B，默认 `<数据目录>/youtube_cookie.txt`)+代理(Ctrl+N socks5h)+JS 运行时(quickjs，需 `yt-dlp[default]` 提供 EJS solver)。
- 节目列表走 Data API(OAuth token)，不需 cookies/yt-dlp。

---

## Panicast_V0.1-Y06 — 2026-07-22 — 轻量 quickjs-ng + 内置 OAuth + cookies/播放单一流程 + quickjs 跨发行版检测

> 用户反馈：Y03 捆绑 106MB deno 二进制过重，且首次播放 YouTube 时有「初始化卡顿 + 屏幕输出不正常」；Y 模式登录被拒（回退用的 SmartTube 公共客户端已被 Google 封）。本版：① 调研后改用 yt-dlp 原生支持的 quickjs-ng（~2MB，冷启动快约 10×）替换 deno，INI 保留 deno 回退；② 把项目自有的 Desktop-app OAuth 客户端内置为默认，登录开箱即用；③ 修复 `-Wswitch` 编译告警。

### 调研结论（基于 yt-dlp 2026.07 master 源码）
- yt-dlp 已废弃内置 jsinterp，nsig 统一走外部 JS 运行时（EJS solver）；无运行时则降级、缺格式。
- 外部运行时优先级：deno(1000,默认) > node≥22(900) > **quickjs(850)** > bun(800,已废弃)。
- Node 20 被标记 unsupported（需 ≥22）；bun 已废弃；**quickjs-ng ~2MB、冷启动比 deno 快约 10×**。
- cookies 场景下 jsless client(android/visionos/android_vr，REQUIRE_JS_PLAYER=False) 全部 SUPPORTS_COOKIES=False 被跳过 → 必有 JS 运行时。故**不能靠选 client 去掉 JS 运行时**，只能换更轻的运行时。
- Data API 不返回播放直链；mpv ytdl_hook 与独立 yt-dlp 子进程在 nsig 上无差别。

### 变更
- **[net/parsers] 新增 `[youtube] js_runtime` 配置 + `YouTubeChannelParser::js_runtime_args()`**：值 `quickjs`(默认) / `deno` / `quickjs:/path` / 空(不注入)。在 `ytdlp_youtube_args_parse()`(覆盖 解析/播放resolve/下载 三条路径) 与 mpv `-13 fallback` 手动参数处注入 `--js-runtimes <runtime>`。空值时回退 yt-dlp 默认(deno)，向后兼容。
- **[build/setup] quickjs-ng 取代 deno 为推荐运行时**：`setup.sh` 新增 `install_quickjs`(装 `vendor/quickjs/qjs` 到 `~/.local/bin`)，回退 `install_deno`；新增 `js-only` 参数，保留 `deno-only`。`build.sh` 检测改为「qjs 或 deno」，缺两者均告警，仅有 deno 时提示换 quickjs。
- **[vendor] 新增 `vendor/quickjs/README.md`**（quickjs-ng 取包/EJS 依赖/验证步骤）；`vendor/deno/README.md` 标注为回退方案。
- **[ini/README] `[youtube]` 段新增 `js_runtime = quickjs`（默认）**及中英双语说明；README 依赖章节改写为「JS 运行时(推荐 quickjs，回退 deno)」。
- **[build] 修复 `-Wswitch` 编译告警**：`app_subscriptions.cpp` 的 `add_favourites_batch`/`add_favourite` 两处 `switch(mode)` 未处理 `AppMode::ACCOUNT`，补 `case AppMode::ACCOUNT: source_mode_name = "ACCOUNT"; break;`。
- **[net/oauth] 内置项目自有 Desktop-app OAuth 客户端为默认**：`google_oauth.cpp` 的 `client_id()`/`client_secret()` 回退值由**已被 Google 封禁的 SmartTube 公共客户端**（`861556708454-…`，实测 `invalid_client`）改为项目自有的 Desktop-app 客户端（`781435869525-…`）。Y 模式扫码登录不再依赖手动放置 `client_secret*.json`，开箱即用。运行时若数据目录存在 `client_secret*.json` 仍优先采用（向后兼容）。注：Desktop-app 客户端的 secret 按 Google 安装型应用模型本就随二进制分发、非真正机密。`google_oauth.h` 注释同步更新。
- **[net/oauth] Data API 错误不再静默吞掉（诊断 Y04 登录后拉不到数据）**：原 `Network::fetch_auth`/`post`/`del` 不检查 HTTP 状态码，`fetch_subscriptions`/`fetch_identity`/`search` 见到 `{"error":...}` 也不记录——Data API 返回 403/401 时静默返回空，用户只看到"无数据"无从排查。现：① `fetch_auth`/`post` 捕获 `CURLINFO_RESPONSE_CODE`，≥400 时把 Google 错误体（前 400 字节）写入 `panicast.log` 并 EVENT_LOG 提示 HTTP 码；② 新增 `log_api_error()` 解析 Data API 错误信封（`error.code/errors[0].reason/message`），在 `fetch_identity`/`fetch_subscriptions`/`search` 中调用。常见 403 `accessNotConfigured`（GCP 项目未启用 YouTube Data API v3）、403 `quotaExceeded`、401 `invalid_token`、403 consent-screen Testing 模式 现均可见。
- **[net/oauth/app] Y 模式节目列表改用 Data API（OAuth token）取，绕开 yt-dlp**：实测 Data API 10 请求 0 错误（订阅已拉到），但"看不到节目列表"卡在展开频道时的 yt-dlp 路径（需 cookies+代理+JS 运行时）。新增 `GoogleOAuth::fetch_channel_videos(access_token, channel_id)`：`channels.list?part=contentDetails` 取 uploads 播放列表 → `playlistItems.list` 分页取视频，**只用 OAuth token，不需 cookies/代理/JS 运行时**。`enter_account_node` 展开 `is_yt_channel` 时优先走 Data API，取不到再回退 `parse_video_list`（yt-dlp）。效果：Y 模式订阅频道的节目列表开箱即用（与订阅同源 token）。**播放仍需 yt-dlp**（Data API 不返回流地址，OAuth 不能用于播放，Y02 已知限制）——播放需 cookies(Ctrl+B)+代理(Ctrl+N)+JS 运行时(quickjs)。
- **[net/parsers/playback] cookies 与播放统一为单一流程，移除所有 fallback（Y05）**：用户原则——唯一正确流程，不要 fallback。① `get_youtube_cookies_file()` 解析为唯一绝对路径：空/bare `youtube_cookie.txt`→`<数据目录>/youtube_cookie.txt`；`~/...`展开；`/abs`原样；其它相对→`<数据目录>/<输入>`。删除 `cookies_from_browser` + `detect_cookies_browser()` 浏览器自动检测链。`ytdlp_youtube_args_parse()` 单一 `--cookies <路径>`（仅当文件存在，前置条件非回退）。② 播放唯一解析点 `resolve_youtube_url`，失败返回 `""`（不再回退原 URL）、调用方跳过不播。③ 移除 mpv `-13` `yt-dlp -g` retry（YouTube URL 已被 resolve_youtube_url 预解析，mpv 不再收到 watch URL，-13 retry 成死代码；保留 `-15` VO 音频回退，无头主机必需）。④ 移除 `AccountsManager::ytdlp_oauth_args()`/`ytdlp_cache_dir()`（注入已被 yt-dlp 移除的 `--username oauth2`，会致错且已无调用方）。⑤ mpv initialize 不再注入 cookies/oauth 到 ytdl-raw-options，仅保留 `proxy`（mpv 取流用）。⑥ 删除死代码 `resolve_play_url`。
- **[build/parsers] quickjs 跨发行版正确检测/调用（Y05）**：Arch 包名 `quickjs-ng`、Debian 包名 `quickjs`，二者功能相同但二进制名可能 `qjs` 或 `qjsng`。① `build.sh` 检测改为 `command -v qjs || qjsng || deno`，提示信息列两发行版包名 + pip + release。② 新增 `find_qjs_binary()`（搜 PATH 的 `qjs`/`qjsng`），`js_runtime_args()` 对 bare `quickjs` 解析出绝对路径并传 `--js-runtimes quickjs:<abspath>`——无论二进制名/是否在 PATH 都能调用；找不到则传 bare `quickjs`（yt-dlp 报清晰错误）。
- **[config] `player_client` 默认 `android,web`→`tv_downgraded,web`（Y05）**：`tv_downgraded,web` 是 yt-dlp 对 cookie 鉴权请求的官方默认组合，最不易触发"Sign in to confirm you're not a bot"校验（research 确认；tv_downgraded/web 都 REQUIRE_JS_PLAYER，需 quickjs 解 nsig）。

### 关于「初始化卡顿 + 屏幕输出不正常」的根因定位
- **deno 不在 app 启动时被调用**：`App()` 构造与 `run()` 启动路径(DB/Cache 初始化、`load_accounts_root` 走 GoogleOAuth HTTP、`restore_player_state` 只恢复音量/进度/模式不自动续播、`load_radio_root` 走 RadioTime HTTP)均不触发 yt-dlp；YouTube 解析仅在展开节点时按需触发。故「启动初始化」阶段的卡顿与 deno 无关，疑似 `mpv_initialize()` 阻塞或 schema 迁移。
- **首次播放 YouTube 的卡顿**才是 deno 相关：yt-dlp 每个视频 fork 一次 deno 解 nsig，deno V8 冷启动 ~100–300ms。**换 quickjs-ng 后冷启动 ~10–30ms（约 10×），直接消除该卡顿**。
- **屏幕输出不正常非 deno/yt-dlp 所致**：`YtdlpRunner` 已捕获子进程 stdout/stderr；mpv 已设 `terminal=no`；libxml2 错误已重定向。deno 的 stderr 经 yt-dlp 捕获后不会泄漏到终端。换 quickjs 进一步缩短阻塞窗口，间接缓解。

### 已知限制
- **quickjs 需 EJS solver**：quickjs 不能从 npm 拉 EJS（deno 可自动拉），须 `pip install -U "yt-dlp[default]"`（带入 yt-dlp-ejs）或加 `--remote-components ejs:github`。装不了则 `[youtube] js_runtime = deno` 回退。
- 只在 Linux x86_64 验证打包路径；ARM64/Windows 需放对应架构 `qjs`。

## Panicast_V0.1-Y03 — 2026-07-20 — Y 模式三处修复（SSL/重复账号/nodejs 依赖）

> Y02 上线后实测 Y 模式仍有三个问题：拉取观看历史时报 `SSL connect error`；同一个 Google 帐号可被重复登录（DB 出现 #1/#3 同号）；yt-dlp 报 `Requested format is not available`。本版逐一修复。

### 修复
- **[app] 同一 Google 帐号不再重复登录**：`start_account_login` 此前无条件 `INSERT` 新账号，导致同一 `channel_id` 可多次登录（实测 DB 中 #1 与 #3 同为 Ye Ma / `UCh32UMDRwrIZklOTqBVZ2sA`）。改为登录时先按 `channel_id` 查重：命中已有账号则 `update_tokens` + `set_label` + 重新激活（复用原 account_id），未命中才新增。Y 模式每个 Google 帐号至多一条记录。
- **[net] 代理选择权交还操作员（不改写 socks5h）**：Y02 曾尝试在 `apply_network_proxy` 里把 `socks5://` 自动改写为 `socks5h://` 以规避本地 DNS 污染。但 `socks5h` 是否被操作员的代理（如 clash）完全支持应由操作员决定，故回退为**原样透传**代理字符串。在 GFW 等 DNS 污染环境下，操作员应在 `Ctrl+N` 填 `socks5h://`（远端解析），否则 `www.youtube.com` 本地解析到污染 IP 会导致拉取观看历史时 `Network POST error: SSL connect error`（订阅走 `googleapis.com` 不受影响）。详见 README 代理说明。
- **[app] Y 模式切换主帐号时 LOG 打印帐号名**：`enter_account_node` 此前只打印 `Y: active account -> #1`（仅帐号 id），改为 `Y: active account -> #1 (Ye Ma)`（附 `node->title` 帐号名），便于日志辨识。
- **[db] 修复 `ON CONFLICT clause does not match any PRIMARY KEY or UNIQUE constraint`**：`history` 表的 `url` 列在老版本建表时为普通 `TEXT`（无 UNIQUE），而 `CREATE TABLE IF NOT EXISTS` 不会迁移已存在的表，导致 `add_history` 的 `ON CONFLICT(url)` UPSERT 失败、历史记录反复插入重复 url。Y03 把 `SCHEMA_VERSION` 39→40，迁移时先按 url 去重（保留最新 id）再 `CREATE UNIQUE INDEX idx_history_url`，使 UPSERT 正常解析。F42 用户数据保留。
- **[db] 修复 `cannot start a transaction within a transaction` / `cannot commit - no transaction is active`**：`Persistence::save_data` 用外层事务包裹 `save_tree` + 收藏清空/重写以求原子性，但 `save_tree`（及 `save_episode_cache`）各自又 `BEGIN`/`COMMIT`，导致嵌套事务失败、且 `save_tree` 的 `COMMIT` 提前关掉了外层事务。改为按 `sqlite3_get_autocommit` 判断：仅当连接不在事务中时才开启/提交自己的事务，被外层事务包裹时复用外层，原子性保持不变。

### 依赖
- **[build/runtime] 新增运行时依赖 `deno`**（非 nodejs）：yt-dlp 2026.07+ 需要一个 JavaScript 运行时来求解 YouTube 的 nsig「n 挑战」，否则只取到缩略图、音视频格式全缺（报 `Requested format is not available` / `n challenge solving failed`）。**yt-dlp 默认只启用 `deno`**，deno 在 `PATH` 中即自动启用，无需额外参数。实测 Debian/Ubuntu `apt install nodejs` 所装 Node 20 被 yt-dlp 内置 EJS 求解器标记为 `unsupported`、**不生效**，故改推 `deno`（`curl -fsSL https://deno.land/install.sh | sh`）。这是**运行时**依赖（yt-dlp 播放时调用），不影响编译。已写入 README 依赖说明与 `build.sh` 的编译后检测（缺 `deno` 时给出安装提示）。

### 已知限制（沿用 Y02）
- 播放/下载需 cookies（`Ctrl+B` 设 cookies.txt）；Y 模式 OAuth 令牌仅用于 Data API，不能用于播放。
- GFW 环境代理请用 `socks5h://`（远端 DNS），见上。
- YouTube 搜索受 Data API v3 配额限制。

---

## Panicast_V0.1-Y02 — 2026-07-20 — 恢复 P 模式 YouTube 解析 + Y 模式 YouTube 搜索/订阅

> Y01 登录打通后：① Y01 引入的 oauth2 注入导致 P 模式无法刷新 YouTube 频道 TABS（回归）；② 需要在 Y 模式搜 YouTube 并订阅。本版先恢复 P 模式解析能力，并加 Y 模式搜索/订阅供体验。

### 修复
- **[parsers] 恢复 P 模式 YouTube 频道 TABS 解析**（Y01 回归）：Y01 的 `ytdlp_youtube_args()` 在有活动帐号时优先注入 `--username oauth2`，而最新 yt-dlp 已移除 oauth2 登录（见下），导致 tab 解析失败。新增 `ytdlp_youtube_args_parse()`：**tab/视频列表解析改走 cookies**（恢复 F42 行为）。`parse_video_list`/`parse_channel_tabs` 改用解析版参数。
- **[parsers] 移除失效的 oauth2 注入**：最新 yt-dlp（2026.07+）实测 `--username oauth2` 报错 `Login with OAuth is no longer supported`，且 YouTube 对匿名播放强制 `Sign in to confirm you're not a bot`。故 `ytdlp_youtube_args()`（播放/下载）也改走 cookies。**Y 模式 OAuth 令牌不再用于 yt-dlp 播放**，仅用于 Data API（搜索/订阅/身份/历史）。

### 功能
- **[app] 主帐号 = 活动帐号（默认 1#），全局生效**：`load_accounts_root()` 在无活动帐号时自动激活第一个（1#）帐号；Y 模式展开其它帐号即切换主帐号（现有 `set_active_account`）。
- **[net] `GoogleOAuth::search`**（Data API v3 `search`）：混合返回 视频/频道/播放列表（`id.kind` 区分），支持 `type` 过滤与音乐分类（`videoCategoryId=10`）。配额 100 units/次。
- **[app] Y 模式 `/` 搜索 YouTube**：`perform_youtube_search()` 在活动帐号下建 `🔍 <query>` 结果节点，每项前加类型标识 `[C]`频道/`[V]`视频/`[P]`播放列表/`[M]`音乐；搜索词可选前缀 `c `/`v `/`p `/`m ` 过滤。`/` 在 ACCOUNT 模式路由到此。
- **[app] `a` 订阅**：ACCOUNT 模式下，`a` 在 `[C]` 频道结果上 → `subscribe_youtube_channel`（`GoogleOAuth::subscribe` → 重拉订阅 → `replace_subscriptions` → 刷新）；其它位置 `a` 仍为登录。错误走 LOG，不弹窗（沿用 Y01 规约）。
- **[app] `Ctrl+B` 设置 YouTube cookies.txt 路径**（持久化到 `[youtube] cookies_file`，即时生效）。播放/下载需 cookies 过 YouTube bot 校验。

### 暂缓（待体验后定方向）
- 频道级帐号绑定（P 模式订阅时绑定主帐号 + `e` 改绑 1#–99# + `Title[loginname]` 显示标记）。
- 无帐号时 P 模式 `a` 弹 QR 兜底登录（方案 B 折叠）。
> 若 Y 模式体验良好，后续可能将 YouTube 订阅整体迁至 Y 模式并移除 P 模式中的 YouTube 订阅功能。

### 已知限制（重要）
- **播放/下载需 cookies**：最新 yt-dlp 移除 oauth2、YouTube 强制登录校验，故播放必须提供 cookies（`Ctrl+B` 设 cookies.txt，或 `[youtube] cookies_from_browser`）。Y 模式扫码登录的 OAuth 令牌**无法用于播放**（yt-dlp 政策 + Google 封堵 oauth 播放；SmartTube TV 客户端 `861556708454` 也已被 Google 封 → `invalid_client`）。OAuth 仅用于 Data API。
- **网络代理**：GFW 环境需 `Ctrl+N` 设代理（socks5h://...）否则 YouTube 请求超时。
- YouTube 搜索受 Data API v3 配额限制（默认 ~100 次/天）。

---

## Panicast_V0.1-Y01 — 2026-07-19 — Y 模式：多 Google 帐号 + SmartTube 式扫码登录

> Y 线迭代（Y01-Y99）独立于 F 修正线（F01-F99），两条线并行。本条置于 F42 之上，F 线记录原样保留。

### 功能要求（来自需求确认）
- 新增 Y 模式（帐号模式）：`a` 登录一个帐号、`A` 登录另外一个帐号；多帐号数据互相独立；
  Y 模式下 `j/k` 移动光标选帐号，`l/Enter` 进入/激活帐号。仅在 Y 模式生效。
- 登录走 SmartTube 方案：Google OAuth 2.0 Device Authorization Grant（client_id `861556708454`），
  `verification_url` 渲染为终端 QR（libqrencode），手机扫码授权；无 libqrencode 时回退纯文本 user_code。
- 既要 yt-dlp 登录态（播放/下载归属当前 Google 帐号），也要同步 YouTube 订阅列表 + 观看记录（一步到位）。
- 全部数据缓存进同一个 `panicast.db`；token 加密存储（本机密钥 + ChaCha20）。
- **不引入"本地帐号"**：现有 podcast/radio/收藏/历史/续播等数据保持全局（F42 行为零改动）；
  仅 YouTube 相关数据（`youtube_cache` / `youtube_subscriptions` / `youtube_history`）按 Google 帐号隔离。
- 每个 Google 帐号作为 Y 模式左侧树的一个节点，"播放历史""订阅列表"为其子节点（融入现有递归树设计）。
- SmartTube 那种完整网格界面在 TUI 下无法实现；右侧 INFO 在现有基础上增加 Google 帐号信息显示。
- 新增 libqrencode 依赖；README/build.sh/man/`?` 帮助均补充说明。
- 基线 F42 重新开发，轻装上阵，不做数据迁移。

### 开发计划
- [build] 版本 suffix `Y01`；CMakeLists/vcpkg 加 libqrencode（可选，无则回退文本）。
- [storage] 单库多帐号：`accounts` / `youtube_subscriptions` / `youtube_history` / `account_sync_state` 新表；
  `youtube_cache` 加 `account_id`；现有表不动。`AccountsManager` + token 加密。
- [crypto] `core/crypto.cpp`：SHA-256 + HMAC-SHA256 + PBKDF2 + ChaCha20（无外部依赖，token at-rest 加密）。
- [net] `google_oauth.cpp`：device flow + token 刷新 + YouTube Data API（身份/订阅 insert-delete-list）+ InnerTube 观看记录拉取。
  `network.cpp` 增加 POST 助手。
- [ui] `qr.cpp`（libqrencode）渲染终端 QR；Y 模式帐号树 UI；INFO 显示帐号信息。
- [playback] yt-dlp/mpv 按 active 帐号注入 oauth2 token（登录态播放/下载）。
- [app] `app_account.cpp`（Y 模式 + a/A 登录）+ `app_sync.cpp`（订阅/观看记录同步）。
- [docs] README/build.sh/man/`?` 更新；`tests` 加帐号隔离 + 加密单测。

### 已知限制（Y01）
- YouTube 观看记录**拉取**走非官方 InnerTube `/browse` history（Data API v3 不提供观看记录）。
- YouTube 无"标记已看 / 回写续播进度"的官方 API，故本地播放仅记录到该帐号 `youtube_history`（本地侧），
  不真正写回 YouTube 服务端观看记录/续播位置；订阅的增删可双向（Data API `subscriptions.insert/delete`）。
- mpv 的 ytdl-hook 在 `mpv initialize()` 时一次性读取活动帐号 token（切换帐号后需重启程序才在 mpv
  主播放路径生效）；yt-dlp 直接下载与 `-g` 回退路径每次动态读取，切换即时生效。
- yt-dlp oauth2 cache 文件名/格式按其默认实现种子化；若 yt-dlp 版本不匹配则回退其自身 device flow，
  cookie 作为无帐号时的兜底。token 用本机密钥（ChaCha20+HMAC，派生自 machine-id）加密存库。

### 修订（2026-07-19）
- [net] OAuth 凭证改为**运行时**从 `~/.local/share/panicast/client_secret*.json` 加载（Google "Desktop
  app" 客户端，支持 device flow），不再硬编码进二进制/源码；文件缺失时回退公共 SmartTube device client。
  修复登录报 `invalid_client`：SmartTube 公共 client `861556708454` 已被 Google 收紧，改用本机自有 client。
- [app] Y 模式 `a` 登录失败不再弹 `confirm_box`（"Login failed: invalid_client"）；错误统一写
  `panicast.log`（`LOG`）并在右侧 LOG 区打印（`EVENT_LOG`）。仅 QR 二维码弹窗显示；登录成功的弹窗亦移除
  （成功信息已在 LOG 区输出）。

---

## Panicast_V0.1-F42 — 2026-07-19 — add_local_files 改递归扫描 + 清除死代码

### [app] add_local_files 用 expand_local_folder 替代扁平扫描（递归一致）
- **问题**：F 模式 `a` 收集本地文件夹时首次显示扁平列表（所有文件直接挂在 folder 下，无子文件夹），重启后展开才显示递归树——不一致。
- **根因**：`add_local_files` 用 `recursive_directory_iterator` 但只建文件 leaf（不建子文件夹 FOLDER 节点）；`expand_local_folder`（重启后展开用）建递归子文件夹。两套扫描逻辑不一致。
- **修复**：`add_local_files` 创建 folder 节点后调 `expand_local_folder(folder)`（复用递归逻辑），取代手写扁平扫描。删约 25 行扁平扫描代码。首次收集即递归树，与重启后一致。
- **附带**：URL 方案统一（expand_local_folder 用 raw path，旧 add_local_files 用 file://——现统一为 raw path，与 expand_local_folder 一致）。

### [cleanup] 清除死代码
- **`App::add_local_folder`**：声明+定义，从未被调用（`A` 键已 freed，`a` 调 `add_local_files`）。删 `app.h` 声明 + `app_subscriptions.cpp` 定义（约 50 行）。
- **`DatabaseManager::clear_tree`**：声明+定义，从未被调用（`save_tree` 内部自行 DELETE，不调 `clear_tree`）。删 `database.h` 声明 + `database.cpp` 定义（5 行）。

- 版本同步 4 处。

## Panicast_V0.1-F41 — 2026-07-19 — 修 play_current 日志：file:// 误标 "online streaming"

### [playback] file:// URL 日志修正
- **问题**：`play_current` else 分支对所有非缓存/非 YouTube 的 URL 都打 `"Play online streaming"`，包括 `file:///mnt/e/...`（WSL2 挂载的本地文件）——明显是本地文件却标"online"。
- **修复**：else 分支检查 `orig_url` 是否以 `file://` 开头 → 是则打 `"Play local file"`，否则 `"Play online streaming"`。

## Panicast_V0.1-F40 — 2026-07-19 — 修 ao 空值覆盖默认（-14 AO init 失败）+ 本地文件缓存路径

### [config][playback] Bug 2：旧 INI 空 `ao =` 覆盖 `pulse,alsa` 默认 → AO init 失败 (-14)
- **现象**：需显式 `--ao=pulse`；不加则播放报错 `-14`（AO_INIT_FAILED）。
- **根因**：`IniConfig::get()` 键存在就返回存储值（空也返回空），默认值仅键缺失时生效。旧 config.ini（F29-F36 生成）有 `ao =`（空值，键存在）→ `get_mpv_ao()` 返回 `""` → ao 不设 → mpv auto → WSLg pipewire 探测失败 → AO init 失败 → -14。F38 的 SCHEMA_VERSION 只重建 DB，不重建 INI。
- **修复**：
  - `get_mpv_ao()`：空值视为默认 `pulse,alsa`（`v.empty() ? "pulse,alsa" : v`）。
  - `initialize()`：启动时若 INI `ao` 空/缺失 → 自动写入 `ao = pulse,alsa` 并保存（一次性修复，用户可手改）。

### [playback] Bug 1b：F 模式 a 本地文件缓存路径 + 本地优先播放（带续播）
- **现象**：F 模式 `a` 收集的本地文件播放走"online streaming"路径（无续播、日志误导）。
- **根因**：`add_local_files` / `expand_local_folder` 创建的本地文件节点 url（file:// 或 raw path）**不在 media_cache** → `play_current` 的 `get_local_file()` 返回空 → 走 else 分支（"Play online streaming"），不走本地分支（无续播）。
- **修复**：收集/扫描本地文件时 `CacheManager::mark_downloaded(url, path)` 记入 media_cache（缓存路径）。播放时 `get_local_file` 命中 → 走本地分支（`"Play local file"` + 续播 + resume position）。
  - `add_local_files`：每个 leaf `mark_downloaded(leaf->url, entry.path())` + `is_downloaded=true`。
  - `expand_local_folder`：每个文件 child `mark_downloaded(child->url, child->url)`。
- **说明**：用户日志的 `-14` 是 Bug 2（AO init 失败）所致，非"在线播放"——Bug 2 修好后 -14 消失；Bug 1b 额外提供本地路径 + 续播。

- 版本同步 4 处。

## Panicast_V0.1-F39 — 2026-07-18 — 清除死代码（legacy json 持久化助手）

### [storage] 删除 4 个 dead legacy json 方法
- **清理**：`Persistence::save_tree` / `load_tree` / `save_node` / `load_node`（json 版，private static）——旧设计（F38 之前）播客树存嵌套 JSON 时用于 TreeNode↔JSON 转换。F38 改递归 `tree_nodes` 行存后无人调用（仅 save_tree/load_tree 自递归），属死代码。
- 删 `persistence.h` 4 处声明 + `persistence.cpp` 4 处定义 + 未用的 `using json` 别名。
- 保留 `persistence.h` 的 `nlohmann/json` include（app.h 等经它传递依赖，移除有风险）。
- 同步清理过时注释（database.cpp "Persistence::save_node calls this method"、persistence.h 头注释 "migration from legacy data.json"）。
- **DB 清理由用户手动**（不加自动清理功能，按用户确认）：F38 的 `SCHEMA_VERSION` 已自动处理 schema 升级；完全重置数据时用户自行删 `~/.local/share/panicast/panicast.db`。
- 原则：不保留偶尔才用一次的功能（dead/rarely-used 代码即删）。
- 版本同步 4 处。验证：编译零告警。

## Panicast_V0.1-F38 — 2026-07-18 — 数据库重构：统一树表 + history 去重 + 去 data_json 双存 + radio_cache 冗余清理

### [storage] #2 统一 nodes + radio_cache → tree_nodes（递归 parent_id）
- **根因**：`nodes`（播客树）存顶层行 + 子节点嵌在 `data_json.children[]`；`radio_cache`（电台树）用递归 `parent_id` 行。两套平行 schema + 两套 save/load。
- **重构**：新表 `tree_nodes(id, root_type, parent_id, title, url, type, expanded, children_loaded, is_youtube, channel_name, is_cached, sort_order, updated_at)`——递归 parent_id，`root_type ∈ {podcast,radio}` 区分。播客树从"JSON 嵌套子节点"改为递归行。TreeNode 内存模型不变（已是递归树）。统一方法 `save_tree/load_tree/clear_tree` + `save_tree_node_recursive/load_tree_node_recursive` 替换 `save_node/load_nodes/save_radio_cache/load_radio_cache`。索引 `idx_tree_parent(root_type,parent_id)`、`idx_tree_url`。

### [storage] #1 radio_cache 的 is_downloaded/local_file 冗余清理
- radio_cache（现并入 tree_nodes）原存 `is_downloaded`/`local_file` 快照，与 `media_cache`（单一真相源）重复且会陈旧。tree_nodes **不存**这两列；节点下载状态由 `media_cache` 实时提供（draw_line 已查 `CacheManager::is_downloaded(url)`）。避免 db→cache 锁序倒置（不在 load_tree 持 db 锁时调 CacheManager）。

### [storage] #3+#4 history 去重（url UNIQUE）+ 重播置顶
- `history.url` 改 `UNIQUE`；`add_history` 改 UPSERT（`ON CONFLICT(url) DO UPDATE timestamp=now`）——同 URL 仅一条，重播刷新时间戳 → `ORDER BY timestamp DESC` 自动置顶。UNIQUE 索引即 url 索引，`delete_history(url)` 受益（#4 免做）。

### [storage] #5 去 data_json 双存（列即单一来源）
- `episode_cache`：删 data_json，加 `is_youtube` 列（原 data_json 仅解析此字段）。
- `favourites`：删 data_json，加 `is_link`/`link_target_url`/`is_local_folder` 列（原 data_json 存 link 元数据）。
- `podcast_cache`：删 data_json（列即元数据）。
- `tree_nodes`：无 data_json（children 是行、状态是列）。
- 调用处同步：app_subscriptions（5 处 save_favourite 传 link 列）、itunes_search（save_podcast_cache 去 data_json）、app_tree_expand+app_navigation（7 处 load_episodes 用 is_youtube 列，去 json::parse）。

### [storage] 无迁移：schema version（PRAGMA user_version）
- `SCHEMA_VERSION=38`；init 时 `PRAGMA user_version` 不符则 DROP 受影响表（nodes/radio_cache/tree_nodes/history/episode_cache/favourites/podcast_cache）重建——历史数据不保留（按设计，不给迭代加迁移负担）。CREATE IF NOT EXISTS 重建新 schema。

- 版本同步 4 处。验证：Ninja Release 编译零错误零告警。

## Panicast_V0.1-F37 — 2026-07-18 — `:` 命令窗口转发 mpv 交互命令 + 默认 ao=pulse,alsa

### [input][playback] `:` 命令窗口新增 mpv 交互命令转发
- **问题**：打开视频窗后 mpv 收不到 f/o/i 等热键。**根因**：F25 设 `terminal=no`（必须，否则 mpv 与 ncurses 抢终端）同时关了 mpv 终端输入；wlshm 视频窗在 Wayland 上要被点击才获键盘焦点，ncurses 又占着终端 → mpv 交互键到不了 mpv。
- **方案（TUI+libmpv 正道）**：TUI 统管输入，经 mpv 命令 API 转发。`open_command_window()`（`:`）在原 r/s/c 播放模式之外，新增**大小写敏感**单字符 mpv 命令，`mpv_command_string(player.get_handle(), cmd)` 转发：
  - `f`→`fullscreen`、`F`→`add sub-scale -0.1`、`G`→`add sub-scale 0.1`
  - `o`→`osd`、`O`→`cycle-values osd-level 3 1`（持久 OSD）
  - `i`→`script-binding stats/display-stats`、`I`→`script-binding stats/display-stats-toggle`（持久统计）
  - `v`→`cycle vid`、`a`→`cycle aid`、`m`→`cycle mute`
  - mpv 命令查证自 mpv 官方 input.conf（F=sub-scale -0.1 步进，非"0.5"；O/I 持久切换）。
- r/s/c 保持原样（不冲突）。`:` 提示与 `?` 帮助同步更新。

### [playback][config] 默认 ao 改 pulse,alsa（消除 auto 的 pipewire 探测噪声）
- **问题**：不加 `--ao`（默认 auto）播放时 mpv 探测声音系统的输出污染主窗口；`--ao=pulse` 干净。
- **根因**：mpv 0.41 `ao=auto` 顺序 **pipewire→pulse→alsa**。WSLg 有 pulse 但无原生 pipewire → auto 先探 pipewire 失败 → 库级 stderr 噪声污染 ncurses → 之后 pulse 才成功。`terminal=no` 压不住库级 stderr（F23 曾用 dup2 压，F25 因 VO/AO 风险移除）。
- **方案（Linux 哲学：用对后端，不 hack stderr）**：默认 `ao` 从 `""`(auto) 改 **`pulse,alsa`**——WSLg pulse 首先成功→不探 pipewire/alsa→无噪声；无 pulse 机器回退 alsa（不跳过 alsa）。无需 dup2；`--ao` 覆盖保留（纯 alsa 机器 `--ao=alsa`）。
- **改动**：`get_mpv_ao()` 默认 `""`→`"pulse,alsa"`；INI 模板 `ao =`→`ao = pulse,alsa` + 注释；man/`?`/`-?` 的 `--ao` 描述同步。
- 版本同步 4 处。

## Panicast_V0.1-F36 — 2026-07-18 — LOG 压缩阈值可配置（终端高度 23）+ 移除无意义的 min_w/min_h

### [ui][config] 窗口缩小时逐行压缩 LOG、优先 INFO；阈值可配置
- **背景**：F31 在 `top_h < min_h(26)` 时把 LOG 钉 6 行地板，`top_h<13` 时 LOG>INFO，INFO（进度条）反被挤。LOG 多是 resize 噪音、INFO 更重要。
- **方案**：`safe_split_y` 改为——终端高度 ≥ 阈值（默认 23）时 70:30 比例成立；低于阈值时 LOG **逐行让位**（每降 1 行高度 LOG 减 1，INFO 保持阈值处的行数不变），LOG<2 行时隐藏、INFO 独占右面板。
- **可配置**：新增 `[display] log_compress_height`（终端高度单位，含状态栏，默认 23）——换屏/分辨率可调。代码换算 `compress_top_h = 值 − STATUS_H(3)`。
- **效果**（默认 23）：终端 23/top_h 20 → LOG 5/INFO 14（比例）；22/19 → LOG 4/INFO 14；21/18 → 3/14；20/17 → 2/14；≤19/≤16 → LOG 隐藏、INFO 独占。
- **移除 min_w/min_h**：核实 `min_w` 全代码零调用（死定义）、`min_h` 仅 safe_split_y 用（已被本阈值取代），且二者都不能限制窗口大小（终端模拟器控制）→ 无意义，移除 INI 条目/getter/man 描述/黄金比例注释，由 `log_compress_height` 取代功能角色。
- **改动**：`ini_config.h` 删 `get_min_w/get_min_h` 加 `get_log_compress_height`（默认 23）+ INI 模板替换；`layout_guard.h` `safe_split_y` 改写（可配置阈值 + 1:1 LOG 收缩 + <2 隐藏）；man LAYOUT 段"Minimum size"→"LOG compression"。
- 版本同步 4 处。

## Panicast_V0.1-F35 — 2026-07-18 — R模式电台TITLE显示电台名 + 三处跟随主题色 + 改名 Playlist Index

### [playback] R 模式播放电台时 INFO Title 显示电台名称（根因：playback_node 从未赋值）
- **现象**：R 模式播电台，INFO 区 Title 不显示电台名（显示 mpv media-title=流地址/ICY）。
- **根因**：`playback_node`（App 成员）在每次播放被清空（`app_playback.cpp` 三处 `playback_node = nullptr`），**从未赋值为播放节点**。故 INFO Title（`ui.h`：`playback_node ? playback_node->title : state.title`）恒走 `state.title`。电台名其实就在 `current_playlist[idx].title`（build_peer_list 从 `node->title` 拷贝）。
- **修复（恢复 playback_node 设计意图）**：
  - `types.h`：`PlaylistItem` 加 `TreeNodePtr node`（携带源节点）。
  - `build_peer_list`：4 处构建点（单条/兄弟/递归扫描/兜底）设 `it/pi.node = <节点>`。
  - `play_current`：锁内读 `pn = current_playlist[idx].node`，**主线程**置 `playback_node = pn`（if/else 之前，含 YouTube 也在 submit 前设——标题是节点标题，与异步取流无关）；删 3 处 `playback_node = nullptr`。
  - **线程安全改进**：原 YouTube 路径在 pool 线程写 `playback_node`（潜在竞态），现统一主线程写、主线程读。
  - **效果**：INFO Title 显示节点标题——R 模式=电台名、PODCAST=episode 标题（保留）、YouTube=视频标题。INFO 播放块在停止时由 `has_media` 门控隐藏，故无需额外 stop-clear。
  - 附带修复 `save_player_state` 的 `current_title`（原恒空，现保存节点标题）。

### [ui] 三处固定色改跟随主题 + "Playing Context"→"Playlist Index"
- **现象**：INFO 区 `=== PLAYER STATUS ===`、进度条、`--- Playing Context [Cycle] ---` 三处颜色固定，不随 Ctrl+L 主题变化。
- **根因**：这三处都用 `A_BOLD`（无 color pair）。很多终端把 `A_BOLD` 渲染为**亮色**（色索引 8-15），而 9 主题只经 `init_color` 重定义**标准色**（0-7），不重定义亮色 → A_BOLD 恒为终端默认亮色（固定）。
- **修复**：三处 `A_BOLD` → `COLOR_PAIR(PAIR_BORDER_STD)`（pair 20 = `init_pair(20, fg, bg)`，fg 标准色索引被主题 `init_color` 重定义 → 跟随主题前景色）：PLAYER STATUS 标题、进度条整条、Playlist Index 标题。
- **改名**：`Playing Context` → `Playlist Index`。`[%s]` 保持**动态**——反映当前播放模式（Repeat/Shuffle/Cycle），非硬编码。
- 不动 `▶ Playing`/`⏸️ Paused`（状态对 11/14，经 init_color 已随主题）及其它非 A_BOLD 元素。

## Panicast_V0.1-F34 — 2026-07-18 — 修复状态栏右下角时间不刷新

### [ui] 状态栏右下角系统时间停滞（秒不跳）
- **现象**：状态栏右下角时间（`AUTHOR@<时间>`，格式 `%b %d %Y %H:%M:%S` 含秒）不更新。
- **根因**：`draw_status` 的时间字符串缓存按**分钟**粒度失效（`current_minute = now_t / 60`），但格式含**秒**（`:%S`）→ 秒被冻结最多 60 秒才跳一次，看起来"不更新"。缓存粒度与显示粒度不匹配。
- **修复**：缓存粒度改为**秒**（`now_t`），格式不变。`strftime` 仍每秒只跑一次（非每帧 ~30ms），秒数每秒可见地跳动。
- **改动**：`ui.h draw_status`：`cached_minute/current_minute` → `cached_second/now_t`。

## Panicast_V0.1-F33 — 2026-07-18 — INI 模板注释改为中英双语（全局唯一双语处）

### [config] config.ini 默认模板注释：中文 → 中英双语
- **范围**：`ini_config.h` `create_default` 写入 `config.ini` 的默认模板注释（F32 留待本轮的 151 行中文）。
- **规则**：每条中文注释追加英文对照（`# 中文 / English`），参数（键/值）保持英文不变。此为**全局唯一**保留双语的位置（其余源码 F32 已全英文化）。
- **改动**：分块编辑 `[display]`/`[colors]`(含颜色代码参考)/快捷键/`[network]`/`[storage]`/`[playback]`/`[mpv]` 段头/`[youtube]`/`[statusbar_color]`/`[search]`/艺术颜色参考；proxy 段与 mpv vo/vid/ao 段已是英文，保持不动。
- **验证**：中文行数仍 151（未删，已双语）；所有 `key = value` 原样（layout_ratio/min_w/vo/ao/proxy/mode/custom_colors 等抽查无误）；无"纯中文无英文"残留行。版本同步 4 处。

## Panicast_V0.1-F32 — 2026-07-18 — INFO 区 Title/URL 分组 + 进度条主题色 + 运行时中文转英文

### [ui] INFO 区 Title/URL 分组（播放节目与光标节目分离）
- **问题**：播放节目的 Title 在上方播放块，但其 Streaming URL 却挤在下方光标块里（与光标节点自身 URL 混杂）。
- **修复**：把 `Streaming URL`（`state.current_url`）从光标块移到播放块、紧挨播放 Title 之下；光标块只保留光标节点自身的 `Title:` + `URL:`。上方=播放节目 Title+Streaming URL，下方=光标节目 Title+URL，各自成对。

### [ui] 进度条改用主题前景色（删除静态 green/blue）
- **问题**：INFO 区播放进度条 playhead 用硬编码 `COLOR_PAIR(11)`(green)/`COLOR_PAIR(14)`(blue)，不随主题。
- **修复**：删除 playhead 的静态色，整条进度条（▓/░/▶/⏸/时间）统一用 `A_BOLD` over 默认前景=主题前景（`apply_theme` 经 `assume_default_colors` 设默认 fg=主题 fg）。播放/暂停由字形(▶/⏸)与上方状态行表达，不再靠颜色。
- **颜色架构核查**：9 主题经 `init_color` 重定义 8 个 ANSI 色；状态对(10-16)/边框(20-21)/默认 fg 全部引用这些色索引，故**默认已随主题动态**。进度条 playhead 是唯一的静态例外，本轮修复。256 色调色板对(1-256)不随主题，但 UI 内容未使用（仅彩虹状态栏刻意用）。

### [i18n] 运行时用户可见中文→英文（INI 模板注释留 F33）
- **范围**：`src/` 与 `include/` 中**非** `ini_config.h` 模板的全部中文（运行时字符串+代码注释）译为英文；中英并存处只保留英文。`ini_config.h` 的 151 行 INI 模板注释留 F33。
- **改动**：`main.cpp` `-?` Database Tables 块；`mpv_controller.cpp` END_FILE 错误码文案；`app_input.cpp`/`ui.h` 播放模式名(Repeat/Shuffle/Cycle，去中文)；`ui.h` UTF-8 locale 警告；`app_subscriptions.cpp`/`app_tree_expand.cpp` 本地文件夹 EVENT_LOG；`app_run.cpp` 待下载提示；`itunes_search.cpp` 国家名；`database.cpp` SQL 注释。
- **验证**：`grep -rlP '[\x{4e00}-\x{9fff}]' src/ include/` 排除 ini_config.h → **0**；ini_config.h 仍 151（F33）。
- 版本同步 4 处。

## Panicast_V0.1-F31 — 2026-07-18 — 布局比例/最小尺寸：修 init 硬编码 + 黄金比例最小尺寸阈值

### [ui][config] 缩放时三区比例不固定 + init 硬编码 40% + 最小尺寸阈值
- **现象**：缩放主窗口时左:右不恒为 4:6、INFO:LOG 不恒为 70:30。
- **根因**：
  1. 整数取整（±1 字符）——字符网格物理限制，不可避免。
  2. 可用性地板（左/右 ≥10 列、LOG ≥6 行、LOG ≤ top_h/2）在小尺寸覆盖比例——去掉会花屏，必须保留。
  3. **潜伏 bug**：`ui.h init()` 用硬编码 `w*40/100`，**不读 INI `layout_ratio`**；而 `handle_resize()` 用 `LayoutGuard::compute()`（读 INI）。首屏用 40%，缩放后才切 INI。`LayoutGuard` 注释明说要修但 init 漏改。
- **方案**：
  1. **修 init() bug**：改用 `LayoutGuard::compute(w,h)`，首屏也读 INI，与 resize 一致。
  2. **新增 `min_w`/`min_h` INI 参数**（`[display]`，默认 **42/26**），作为"按比例精确渲染"的净空间阈值：≥ 阈值时 4:6 与 70:30 干净成立；< 阈值时压缩 LOG（优先 INFO，保留现状降级，不加提示弹层）。`safe_split_y` 改用 `min_h` 作 ratio-vs-floor 分界（≥min_h 用 log_height_ratio；<min_h LOG 钳到 6 行硬地板）。
  3. **黄金比例取值**：min_h=26 是 70:30(LOG=7 行)干净成立的最小净高；min_w = min_h×φ(≈1.618) = 42。实际终端窗口最小 42 列 × 29 行（26+3 状态栏）。
  4. **语义**：两参数 = 净显示空间（三区内容区，不含 3 行状态栏），与 layout_ratio/log_height_ratio 同框架。状态栏固定 3 行附加。
- **窗口硬锁说明**：终端窗口大小由终端模拟器控制，程序无法锁定/阻止缩小；min_w/min_h 仅为降级阈值，非硬锁。
- **改动**：`ini_config.h` 加 `get_min_w()/get_min_h()`（默认 42/26）+ INI 模板加 `min_w=42/min_h=26`（含黄金比例注释）；`layout_guard.h` `safe_split_y` 用 min_h 阈值；`ui.h init()` 用 `compute()`；man 加 LAYOUT 段（比例 + 最小尺寸 + 黄金 + 硬锁说明）+ config.ini `[display]` 描述补 min_w/min_h。
- 版本同步 4 处。

## Panicast_V0.1-F30 — 2026-07-18 — LOG 补全音频 codec（与 INFO 对称，V/A 双行）

### [playback] LOG 文件 + LOG 区只记视频 codec、缺音频 codec — 与 INFO 区不对称
- **现象**：INFO 区同时显示 `Audio: AAC` 与 `Video: H.264 [software]`；但 LOG 文件与 LOG 区的一次性日志只有 `Video decode: ...`，无音频 codec。
- **根因**：F27/F28 的一次性日志（PLAYBACK_RESTART 处）只读 `video-codec`+`hwdec-current` 并记视频行；`audio-codec` 仅读进 `state_` 供 INFO 显示，从未记入日志。
- **方案（对称补全，simple and right）**：在同一 `PLAYBACK_RESTART` 一次性块（同一 `restart_info_logged_` 守卫）里追加读 `audio-codec`，多记一行 `Audio decode:`。
  - 两行分开（mpv/ffmpeg 惯例，与 INFO 区 `Audio:`/`Video:` 两行对称）；音频无 hwdec 概念，其行仅 codec。
  - LOG 文件（`LOG`）+ LOG 区（`EVENT_LOG`）**都记**（与视频行一致）。
  - 仍每曲一次（F28 守卫不变，无双记回归）；视频轨缺则跳过视频行、音频轨缺则跳过音频行。
- **格式**：
  - LOG 文件：`[MPV] Video decode: codec=H.264, hwdec=software` + `[MPV] Audio decode: codec=AAC`
  - LOG 区：`Video decode: H.264 [software]` + `Audio decode: AAC`

## Panicast_V0.1-F29 — 2026-07-18 — 新增 --ao 选项 + 修正帮助面（a/A 键、--ao 文档）

### [cli][playback] 新增 `--ao` 音频输出覆盖选项
- **背景**：原支持 `--vo`/`--vid`/`--quiet` 覆盖，但 `ao` 根本不设（mpv 默认 auto），无 `--ao` 旋钮。vo/vid/ao 是 mpv 三大输出选择，缺一是不对称残缺。
- **方案（对称补全，非 fallback）**：完全照搬 `--vo` 实现，默认行为不变（ao 仍 auto，pulse 优先）。
  - `ini_config.h`：加 `get_mpv_ao()`（默认空=auto）；默认 INI 模板 `[mpv]` 加 `ao =`（空）+ 注释。
  - `mpv_controller.h`：`set_cli_overrides(vo,vid,ao)`；加 `cli_ao_override_`。
  - `mpv_controller.cpp`：initialize() 读 `get_mpv_ao()`，CLI 覆盖优先，**非空才设** `ao`（空=不设=mpv auto）；Init 日志加 `ao={}`。
  - `main.cpp`：getopt `{"ao",required_argument,0,'A'}` + `case 'A'`；传给 `set_cli_overrides`；`print_usage` 加 `--ao` 行。
- **净效果**：`panicast --ao=pulse/alsa/pipewire` 可用；不传则行为完全不变。

### [ui][man][help] 修正 `?` 弹窗 `A` 键错误描述 + 补全 `a` 键 + 同步 `--ao` 文档
- **问题**：`?` 帮助弹窗与 man 手册中 `A` 键描述为"Add local folder (FAVOURITE)"，但实参 `app_input.cpp` 中 **`A` 已 freed（未绑定）**；实际是 **`a`** 在 FAVOURITE 模式下递归扫描本地文件夹（`add_local_files()`），且 `a` 同时在 PODCAST=add feed、ONLINE=subscribe。原描述漏了 `a` 的 FAVOURITE 行为、且把功能错挂到 `A` 上。
- **修正**（IT 规范：不文档化未绑定键；一个键的多模式行为并列列出）：
  - `?` 弹窗：`a` 行改为 `Add feed (PODCAST) / Subscribe (ONLINE) / Add local folder (FAVOURITE)`；**删除 `A` 行**（未绑定，文档化会误导）。
  - man 手册：`.B a` 改为 `Context action: add feed (Podcast mode), subscribe to podcast (Online mode), or add a local media folder (Favourite mode).`；**删除 `.B A` TP**。
- **`--ao` 文档同步**：`?` 弹窗 Command Line 段、`--help`/`-?`（print_usage）、man 手册 OPTIONS 段均加 `--ao <value>` 说明（auto/pulse/alsa/pipewire，空=auto）。
- 不改 vo/vid 相关注释（不在本次范围）。

## Panicast_V0.1-F28 — 2026-07-18 — 修复 Video decode 日志双记（改绑 PLAYBACK_RESTART 事件）

### [playback] `Video decode:` 日志每曲记两次 — 一次性日志触发点错误
- **现象**：F27 播放视频时 `Video decode: {codec} [{hwdec|software}]` 在 LOG/LOG区 每首曲目出现两次。
- **根因**：F27 一次性日志守卫 `decode_info_logged_` 在 `FILE_LOADED` 里重置为 false，而 `FILE_LOADED` 同时清空 `video_codec`。清空→mpv 重新填充这一瞬形成"video_codec 再次变非空"，守卫已被重置→**第二次触发**。即"FILE_LOADED 重置守卫"与"FILE_LOADED 清空字段导致重新就绪"叠加，必然双记。本质是把"记一次"建立在 `update_state` 轮询 + 标志位上，标志位被事件重置，时序不可控。
- **方案（simple and right：日志绑到 mpv 离散事件，而非轮询标志位）**：一次性日志改在 `MPV_EVENT_PLAYBACK_RESTART` 触发，守卫 `restart_info_logged_` 由 `FILE_LOADED` 重置。
  - 选 PLAYBACK_RESTART 的关键原因：`hwdec-current` 在**解码器初始化时**才确定，发生在 `FILE_LOADED` 之后、首帧渲染时。`loadfile` 调用瞬间 mpv 还没探测（空）；`FILE_LOADED` 时 codec 已知但解码器未起、hwdec 空（硬解会被误报成 software）；`PLAYBACK_RESTART` 时 demuxer 已开（codec/码率/尺寸齐）+ 解码器已起（hwdec 已定）→ 信息齐且准。
  - 不重复的原因：日志只在 `PLAYBACK_RESTART` 事件到达时触发，该事件在 `FILE_LOADED` 之后；没有"轮询在 FILE_LOADED 之前撞到残留 codec"的路径（F27 的 #1 过早日志消失）。每曲：FILE_LOADED（重置守卫）→ PLAYBACK_RESTART（记一次、置守卫）→ 曲内 seek 触发的 PLAYBACK_RESTART 被守卫挡掉 → 下一首再重置。每曲恰一次。
- **改动**（`mpv_controller`）：删 `decode_info_logged_` 换 `restart_info_logged_`，`FILE_LOADED` 重置之；删 `update_state()` 一次性日志块（双记根源移除，但仍连续读 `hwdec-current` 写 `state_` 供 INFO 显示）；`MPV_EVENT_PLAYBACK_RESTART` 处理新增一次性日志（读 `video-codec`+`hwdec-current`，纯音频跳过）。
- **解耦**：INFO 显示=连续轮询（永远准）；日志=离散事件（每曲一次、时机准、hwdec 不误报）。codec 格式不变。

## Panicast_V0.1-F27 — 2026-07-18 — INFO/LOG/LOG区 显示视频 codec + hwdec-current（软/硬解）

### [ui][playback] 新增视频解码方法（hwdec-current）显示
- **需求**：在 INFO 区、LOG 文件、屏幕 LOG 区三处显示视频 codec 与 hwdec-current（软解/硬解及方法）。
- **背景**：`vo` 只代表视频**输出**（gpu/wlshm/null），不代表是否在**解码**；解码与否由 `vid`/`video-codec` 决定，软/硬解由 `hwdec-current` 决定（硬解时返回 `vaapi`/`vaapi-copy`/`d3d11va`/`nvdec` 等，软解时空或 `no`）。此前 INFO 只显 codec 名，看不出软/硬解。
- **改动**（仅此功能，不改 vo/vid/hwdec 行为，只读和显示）：
  - `mpv_controller.h`：`State` 新增 `hwdec_current` 字段；新增私有 `decode_info_logged_`（每曲目一次性日志守卫）。
  - `mpv_controller.cpp`：
    - `update_state()` 连续读 `hwdec-current`，按当前值写入 `state_.hwdec_current`（`"no"`/空 → 清空=软解）——与 F26 同通路，无新竞态。
    - 一次性日志：`video_codec` 首次非空时触发一次（`decode_info_logged_` 守卫，`FILE_LOADED` 重置），锁外 `LOG`+`EVENT_LOG` 记 `codec + hwdec`，避免每 50ms 刷屏。纯音频曲目（无视频轨）不记。
    - `FILE_LOADED`：清 `hwdec_current` + 重置 `decode_info_logged_`。
  - `ui.h draw_info`：Video 行由 `Video: h264` 改为 `Video: h264 [vaapi-copy]`（硬解）或 `Video: h264 [software]`（软解）。Audio 行不动。
- **显示格式**：
  - INFO：`Video:  {codec} [{hwdec|software}]`
  - LOG 文件（panicast.log，每曲目一次）：`[MPV] Video decode: codec={codec}, hwdec={hwdec|software}`
  - LOG 区（EVENT_LOG，每曲目一次）：`Video decode: {codec} [{hwdec|software}]`
- **时序说明**：一次性日志在 `video_codec` 首次非空（解码刚启动）时触发；hwdec-current 通常在解码器初始化时即定，若极端情况下稍晚就绪，INFO 区因连续轮询会自行刷新到最终值，仅那一行 LOG 取启动瞬间值——对日志可接受。

## Panicast_V0.1-F26 — 2026-07-18 — INFO 区格式/码率识别不稳定修复（属性读取竞态）

### [playback] INFO 区视频/音频格式与码率时有时无、重播偶尔能识别 — 属性读取竞态
- **现象**：INFO 区的 VO/AO/尺寸/码率/采样率/声道 等识别不稳定，无规律可重现；重新播放有时能识别。
- **根因**：这些字段**只在 `MPV_EVENT_FILE_LOADED` 那一刻读一次**，之后再不读。而 `FILE_LOADED` 触发时 mpv 才刚打开 demuxer，**VO/AO/尺寸/码率/audio-params 是异步填充的**——`FILE_LOADED` 那一瞬往往还没就绪（要等解码/缓冲推进后才报）。一次性读取踩在就绪窗口之前 → 读到 0/空 → `state_` 永久停留错误值，直到下次 `FILE_LOADED`。`FILE_LOADED` 触发时刻相对于 demuxer 就绪的先后是随机的，故不可重现；"重播"= 重触发一次 `FILE_LOADED`，换时刻 → 偶尔成。本质是**一次性读取 vs 异步属性就绪的竞态**。网络流（YouTube 经 yt-dlp 的直链）尤其严重。`audio-codec`/`video-codec` 之所以相对正常，是因为它们走 `update_state()` 每 50ms 连续轮询——这恰好印证了修复方向。
- **必然选择此方案的原因（Unix 哲学：simple and right，找本质、不打补丁）**：竞态的本质是"读太早且只读一次"。唯一正确的修法是把读取移到**连续轮询**，让 INFO 区在若干轮询周期内收敛到正确值，与时机无关。`update_state()` 本就每 50ms 跑、且已对 codec 连续轮询——把其余 7 个字段并入同一通路即可，零新增机制。不引入属性变更观察（`MPV_EVENT_PROPERTY_CHANGE`）——那需注册观察+处理事件队列，是更重的机制，且本程序已是轮询架构，连续轮询最简最一致。last-known-good 语义（只在 mpv 报有效值时覆盖）避免瞬时不可用让显示闪回 0，且 `FILE_LOADED` 清空字段避免上首信息残留。
- **改动**（`src/playback/mpv_controller.cpp`）：
  - `FILE_LOADED` 处理：删属性读取块，改为**清空** INFO 字段（vo/ao/dim/bitrate/samplerate/channels/codec）+ 保留 LOG 与 resume 逻辑。
  - `update_state()`：新增对 `current-vo/current-ao/width/height/video-bitrate/audio-bitrate/audio-params/samplerate/audio-params/channel-count` 的连续读取，last-known-good 写入 `state_`（vo/ao 取当前值——纯音频时 `"null"` 让 UI 隐藏 VO 行；尺寸/码率只在 `>0` 时覆盖；samplerate/channels 非空时覆盖）。
- **边界**：码率用 last-known-good 意味着自适应流中途变码率不刷新显示——对播客播放器无所谓（标称码率即所需），切轨/重播会重置。

## Panicast_V0.1-F25 — 2026-07-17 — VO/AO 初始化失效修复（删除 F23 的 stderr 劫持）

### [playback] VO/AO 设备不能正常初始化 — 删除 F23 的进程级 `dup2(stderr→/dev/null)`
- **现象**：F24 编译测试发现 VO/AO 不能正常初始化。
- **根因**：F23 在 `mpv_initialize()` 前后用 `dup2` 把进程级 fd 2 重定向到 `/dev/null`（整个 mpv 生命周期，`stop()` 里再 `dup2` 还原）。这次 fd/TTY 状态篡改**恰好落在 `mpv_initialize()` 内部 VO/AO 后端探测的窗口上**；在受限桌面会话（Wayland / 容器 / systemd 服务，无前台控制 TTY）里，进程级 fd 篡改落在探测窗口上会干扰图形/音频后端拿句柄 → VO/AO 初始化失败。这是一个为"压 ALSA/Pulse 库级 stderr 噪声"而引入的补丁，本质是用一个全局 fd 劫持去掩盖一个环境相关的库输出问题，副作用比收益大。
- **必然选择此方案的原因（Unix 哲学：simple and right，找本质、不打补丁）**：
  1. mpv 自身的终端输出已由 `terminal=no` 完整关闭（状态行 + 日志 + 输入）。`terminal=no` 还同时关闭 mpv 的终端**输入**——这是刚需，否则 mpv 会抢 stdin、把终端设 raw 模式与 ncurses 争键盘。因此 `terminal=no` 是唯一正确且必要的那个选项；在其之下，原先的 `msg-level=all=error` 与 `quiet=yes` 都是纯冗余，一并删除。
  2. `dup2` 真正吞掉的只有 ALSA/Pulse **C 库直接写 fd 2** 的噪声，而这类噪声只在 mpv 探测到**不可用**的后端时才产生。`ao` 维持不设（`auto`）时，mpv 先探 pulse——本机 `AO=pulse` 成功（见 `panicast.log`），ALSA 根本不被探测，所以该噪声在本机**不发生**。`dup2` 是在解一个不存在的问题，同时却在制造 VO/AO 失效。
  3. 在 PipeWire 系统上 `ao=pulse` 经 `pipewire-pulse` 兼容层透明运行在 PipeWire 之上，故"优先 pulse、不跳过 alsa"由 `ao=auto` 自然满足，无需任何额外配置或 fallback 代码。显式写 `ao=pipewire,...` 只会在无 PipeWire 的机器上多一次失败探测、反而可能吐 stderr 噪声，属于补丁打补丁，不采用。
  4. 综上，F25 是**纯减法**：删 `dup2` 全套、删冗余的 `msg-level`/`quiet`、`ao` 不动。无新增选项、无 fallback、无 fd/TTY 状态改动。
- **改动**：
  - `src/playback/mpv_controller.cpp`：删除 `initialize()` 里 `dup2` 重定向块与 `stop()` 里还原块；mpv 终端选项只留 `terminal=no`，删 `msg-level`/`quiet`；移除仅此处使用的 `<fcntl.h>`/`<unistd.h>`。
  - `include/panicast/playback/mpv_controller.h`：删除成员 `saved_stderr_fd_`。
  - 注释注明 `VO/AO=null`（init 后）是 `vo=auto`+`idle=yes` 懒初始化的**正常表现**，非故障。
- **说明**：`--msg-level=all=no`（参考消息提议）在本方案中不采用——它要做到的"让 mpv 不写终端"已被 `terminal=no` 覆盖，属冗余；且它对 ALSA/Pulse 库级 stderr 同样无效，非本质杠杆。本质杠杆就是删掉 F23 的 `dup2`。

## Panicast_V0.1-F19 — 2026-07-17 — 4 BUG 修复（连续播放/IME/YouTube下载/DSF卡顿）

### [playback] BUG 1：播放完当前节目后自动 PAUSE → 修复为连续播放
- 根因：mpv `keep-open=yes` → 曲目结束后 mpv 暂停在 EOF；`on_playback_ended` 加载下一首时未显式取消暂停。
- 修复：新增 `MPVController::set_pause(bool)`；`on_playback_ended` 在 `player.play()` 后调用 `player.set_pause(false)`。

### [ui/input] BUG 2：中文输入法预编辑文本污染右下角
- 根因：`leaveok(stdscr, TRUE)` 只影响 stdscr，子窗口仍把光标定位到右下角；IME 预编辑跟随光标。
- 修复：移除 `leaveok`；主循环 `ui.draw()` 后加 `move(0,0); refresh()` 把物理光标移到左上角 → IME 预编辑出现在左上角（下一帧 draw 覆盖），不再污染右下角。

### [download] BUG 3：YouTube tab 节目下载失败
- 根因：`start_one_download` 的 YouTube 分支**缺少 `ytdlp_youtube_args()`**（cookies + player_client），只有硬编码格式参数 → YouTube 风控拒绝 → 下载失败。
- 修复：下载参数前加 `YouTubeChannelParser::ytdlp_youtube_args()`（与解析阶段一致：cookies + player_client=android,web）。

### [playback] BUG 4：F 模式本地 DSF 文件播放卡顿
- 根因：mpv `demuxer-max-bytes=50MiB` 对 DSD 高码率太小；本地文件不需要缓存，缓存管理反而增加开销。
- 修复：`play_audio` 中对 `file://` URL 设 `cache=no`（本地磁盘 I/O 足够快），网络 URL 保持 `cache=yes`。

---

## Panicast_V0.1-F18 — 2026-07-17 — 6 BUG 修复 + IME/本地文件/无peers 三项新功能

### [playback] BUG 1：parent 指针未重置 → peers 退化为单条（一行修复）
- `app_run.cpp` spawn_load_feed 中 `node->children = result->children;` 后新增 `for (auto& c : node->children) c->parent = node;`。首次解析后播放不再"Built 1 peers"，CYCLE/SHUFFLE 可正常推进。

### [playback] BUG 2：YouTube -17 UNKNOWN_FORMAT
- mpv `ytdl-raw-options` 加 `player_client=android`（单值无逗号），与解析阶段一致。
- END_FILE error 码翻译到 LOG/EVENT_LOG（-13/-15/-16/-17 各有中文提示）。

### [playback] BUG 3：未下载节目直连 CDN 失败
- mpv 设浏览器 User-Agent（`Mozilla/5.0 ... Firefox/128.0`）。
- -13(LOADING_FAILED) 兜底：YtdlpRunner 跑 `yt-dlp -g <url>` 取真实直链再 loadfile（防重试循环）。

### [playback] BUG 4：stale DISPLAY → vo=gpu 失败 -15
- 新增 `Utils::has_usable_display()`：X11 检查 `/tmp/.X11-unix/X<n>` socket；Wayland 检查 `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`。
- mpv 初始化判定改用 `!has_usable_display()`。
- VO 错误回退：-15(VO_INIT_FAILED) → `vo=null; vid=no; bestaudio`，重试 loadfile 一次，INFO 提示。

### [storage] BUG 5：youtube_cache 死代码接入
- spawn_load_feed YouTube 分支：先 `YouTubeCache::has(url)` → 命中则重建 children（设 parent）；未命中则解析 → `YouTubeCache::update()`。避免每次展开全量 --flat-playlist。

### [ui] BUG 6：feed 缓存指示误导
- feed 着色两级：有 `is_downloaded` 子节点 → 绿(pair15)；仅 `children_loaded` 无下载 → 青(pair10)；无 → 无色。

### [ui/input] IME 污染修复
- UI 初始化加 `leaveok(stdscr, TRUE)`：ncurses 不再在 refresh 后移动物理光标 → IME 预编辑不跟随到右下角。

### [ui/input] F 模式 'a' 递归收集本地文件
- 'a'（小写）= 输入文件夹路径 → 递归扫描音视频文件 → 加入收藏。'A'（大写）释放（未来他用）。

### [playback] 无 PEERS 时扫描父文件夹
- `build_peer_list` 中 `peers.empty()` → 取 `node->parent.lock()`，用 `collect_playable_items` 递归扫描父文件夹 → 形成 peers。无文件则 LOG 记录 + 单条回退。

---

## Panicast_V0.1-F17 — 2026-07-17 — IME 修复 + import_feed 标题 + app.h 全量拆分 + 响应优化

### [arch] app.h 全量拆分为 9 个 .cpp（4361→415 行）
- `include/panicast/app/app.h` 从 4361 行降至 **415 行**（仅类声明 + 成员 + 短方法 inline）。
- 9 个 out-of-line `.cpp`：`app_playback`(232) / `app_download`(360) / `app_search`(235) / `app_subscriptions`(604) / `app_navigation`(360) / `app_tree_expand`(559) / `app_input`(327) / `app_nodes`(494) / `app_run`(895)。
- 纯机械搬迁（声明留头、定义移 cpp），行为不变；CMakeLists 已加入 9 个源文件。

### [ui/input] IME 非英文输入静默丢弃（#1）
- 主循环 `getch()` → `wget_wch(stdscr, &wch)`：`KEY_CODE_YES`→特殊键；`OK`+`wch<128`→ASCII；`OK`+`wch≥128`→静默丢弃。`input_box` 等文本弹窗仍接受全部 UTF-8。

### [app] import_feed 标题修复（#2）
- 非 YouTube 用 `"Loading..."`（与 `add_feed` 一致），不再误标 "YouTube Channel"。

### [perf] 响应优化（#3）
- 主循环 poll 间隔 50ms→**30ms**（≈33FPS），输入更跟手。
- 线程模型：主线程=ncurses draw+input（单线程，ncurses 限制）；`pool_`(10 worker)=解析/下载/加载（多核）；mpv 事件线程=播放回调。主要"假死"源（每帧深拷贝、播放列表填充）已在 F12 修复；剩余 `save_data` 调用 <100ms。
- handle_input 链中的阻塞操作（save_data/save_cache/delete_episode_cache/clear_download）审计完成——均在 tree_mutex 下、<100ms；后续可按需异步化。

### [已知]
- #2 全部无法播放 / #3 解析失败 / #4 搜索无果：代码逻辑正确，根因是旧 DB schema 不匹配 → 删旧 DB 全新重建后恢复（用户确认）。
- save_data 异步化（pool_.submit）：作为后续优化，当前 <100ms 不构成主要卡顿。

---

## Panicast_V0.1-F16 — 2026-07-17 — media_cache 单status列 + 去cached冗余 + 输入/复制优化

### [storage] media_cache 表简化为单 `status` 列
- 表结构：`media_cache(url PK, status INT, local_file, updated_at)`，`status`：0=无缓存、1=完整缓存、2=部分缓存(.part)。取代 F15 的 `cached`+`downloaded` 双标志。
- API：`DatabaseManager::media_cache_set(url, status, local_file)`（status=0 删行）、`load_media_cache()` 返回 `(url,status,local_file)`。删除 `media_cache_mark/media_cache_clear/media_cache_bulk_set`。
- CacheManager：`mark_downloaded`→status=1、`mark_partial`→status=2（**partial 现已持久化**，重启仍标记）、`clear_download`→status=0；内存 `downloaded_`/`partial_`/`local_files_` 从 status 载入。
- 不迁移、重建表（用户数据不保留）。

### [storage] 删 `cached` 冗余 → 懒查 `episode_cache`
- 删 `CacheManager::mark_cached/is_cached/clear_cache`。"节目列表已解析"改由 `episode_cache` 表（`is_episode_cached`）表达，**不整表载入内存**。
- 展开节点时即时查 `is_episode_cached`（实测单次 <5ms，1万行 1000 次查询 38ms），结果写 `node->is_cached`；UI 绘制只读节点标志（零 DB）。
- `mark_cached_nodes` 启动只设 `is_downloaded`（内存态）；`clear_cache` 调用点 → `delete_episode_cache_by_feed`。
- UI `is_stream_cached` 判据改为 `node->is_cached || (PODCAST_FEED && children_loaded)`，仅限 feed 节点，避免误染 episode。

### [ui/input] CTRL+Y 直接复制 + 英文键盘态
- CTRL+Y：复制当前节目的**源 URL**（`playback_node->url`，如 watch?v=…，非 file:// 本地路径）到系统剪贴板，**不弹窗**；无剪贴板工具时仅 LOG。
- 浏览态（`handle_input`）只处理 ASCII 按键，忽略输入法(IME)多字节输入（`128..255` 字节丢弃）；仅文本输入弹窗（input_box/搜索/`:`命令）接受 UTF-8。避免浏览时 IME 拦截 j/k/l 等。

### [playback] 修复 headless 非 SSH 主机 YouTube 卡 Navigating + mpv 两场景配置
- mpv 纯音频判定由 `is_ssh_session()` 改为 `!has_local_display()`：覆盖 **SSH + headless 非 SSH**（无 DISPLAY/WAYLAND）。原逻辑下 headless 非 SSH 走本地分支取 `bestvideo+bestaudio` 却经 `play_audio` 播、headless 下 loadfile 失败 → 状态恒为 Navigating。
- 两场景配置精确化：
  - 无显示器/SSH 纯音频：`vo=null + vid=no + bestaudio`（零解码零 GPU）。
  - 有显示器音画：`vid=auto + hwdec=auto-safe + bestvideo+bestaudio`；`vo=gpu` 由 `play_video` 在播视频时设置（音频保持 vo=null，避免 GPU-less 主机音频挂、避免 vo gpu→null 切换 segfault）。
- 类似 bug 排查：全仓 `is_ssh_session()` 现仅用于 `has_local_display()` 内部，line 78 是唯一误用点（已修）；`play_video`/`play()` 路由本就用 `has_local_display`，无其它类似问题。
- 次要场景（有 DISPLAY 但无 GPU → vo=gpu 失败）需 VO 错误事件回退，留作后续。

### [已知/待办]
- `app.h` 仍 ~4350 行，out-of-line 拆分为下一步（分步、每步提示）。
- 次要：有 DISPLAY 无 GPU 主机的 vo=gpu 失败需 VO 错误回退。

---

## Panicast_V0.1-F15 — 2026-07-17 — 清理/去迁移/统一DB/缓存颜色/对齐

### [storage] 清理 + 去迁移
- 删死表 `url_cache`（被 `media_cache` 取代后仅剩 purge 里一行 DELETE）。
- 删全部迁移代码：`cache.cpp` 的 `migrate_from_json`、`persistence.cpp` 的 `migrate_from_json`（data.json）、`Paths::get_cached_urls_file`/`get_data_file`。`CacheManager::load()` 只读 `media_cache` 表，不再回退 JSON。DB 为唯一数据源。
- 删死方法 `CacheManager::save()`（write-through 后已无人调用）。

### [storage] 统一数据库 + 解决竞争
- `youtube_cache` 收敛到 `DatabaseManager` 单连接：新增 `DatabaseManager::youtube_cache_load/save`（参数化、共享 `mtx_`），`youtube_cache.cpp` 删除自开的第二条 `sqlite3_open` 连接（消除双连接写同一 DB 的锁竞争）。

### [playback] 指针模型 + 本地直播
- 指针模型（F14 已具）：`current_index` + `shuffle_queue_` 前瞻队列 = 动态指针表；INFO 用指针转换出"前3+当前+3未来"。
- 已下载节目经 `resolve_play_url` 直接 `file://` 播放（不走在线解析）；`load()` 修复后重启也能取到本地路径。

### [ui] 对齐 + 缓存状态色
- INFO 7 行：当前行前缀 `🔊 `(3列) 改为 ` 🔊 `(4列)，与 `  ↑ `/`  ↓ `(4列) 对齐，TITLE 列左对齐。
- 新增"不完整下载(.part)"第三态：`CacheManager::mark_partial/is_partial`（内存瞬态，下载失败时标记、成功时清除）；`draw_line` 用 pair16 着色。
- 新增 INI `[colors] partial = yellow`（默认黄），颜色代码参考（8标准/8亮/216立方/24灰）已在注释中列明。

### [已知/待办]
- `app.h` 仍 4343 行，拆分（out-of-line 到 app_run/playback/input/... .cpp）作为下一步、分步进行。

---

## Panicast_V0.1-F14 — 2026-07-17 — 下载缓存迁入数据库

- **新增 `media_cache` 表**：URL 的 cached/downloaded 双标志 + 本地文件路径，替代 `cached_urls.json`。所有写入用 `INSERT OR IGNORE` + `UPDATE`（兼容老 SQLite，不用 `ON CONFLICT`），按位合并标志、互不覆盖。`media_cache_mark` / `media_cache_clear` / `media_cache_bulk_set` / `load_media_cache` / `clear_media_cache`。
- **CacheManager 改为 DB 直写**：内存结构作为快速读路径，每次 `mark_cached`/`mark_downloaded`/`clear_cache`/`clear_download` 即时写库；移除 `save()`/`save_locked()` 及其两处调用（app 退出、清 feed 缓存后）。`load()` 从 DB 读入。
- **一次性安全迁移**：启动时若 `media_cache` 为空且存在 `cached_urls.json`，则读 JSON → 事务批量写库 → **仅当写库提交成功后**才把 JSON 改名为 `.migrated` 备份（迁移失败保留原 JSON，下次重试）。已实测 4 条记录迁移成功。
- **清理死代码**：删除从未写入的 `cache` 表（DDL + purge 里的 `DELETE FROM cache` 改为 `DELETE FROM media_cache`）。
- **动机**：全部缓存数据集中入库，维护更便利，避免 JSON 文件的隐私/一致性问题；同时规避了"老 SQLite 不支持 `ON CONFLICT` 导致缓存写不进 → 回退取网流 → 一直缓冲"的隐患。

---

## Panicast_V0.1-F13-wsl — 2026-07-17 — 播放模型重构为 peers 隐式列表

- **取消持久化播放列表与 L 弹窗**：不再维护独立的播放列表，删除 L 弹窗及其全部管理代码（添加/删除/清空/移动/排序/去重/持久化），删除 DB `playlist` 表与相关方法、`SavedPlaylistItem` 类型、`AppState::LIST_MODE`、`get_playlist_fill_limit` 配置项。
- **播放列表 = 当前节目的 peers**：按 `l`/Enter 播放一个节目时，其父节点（订阅博客/电台/收藏节点）下的兄弟 episode 快照为 `current_playlist`，`current_index` 指向当前节目。F 模式下直接收藏的节目以 `fav_root` 为父，peers 即全部收藏节目。指针驱动：mpv 单轨播放，`on_playback_ended` 按模式推进指针（REPEAT=loop_file、CYCLE=+1 回绕、SHUFFLE=取前瞻队列首元素）。
- **`:` 命令窗口**：随时按 `:` 输入 `r`/`s`/`c` 或 `repeat`/`shuffle`/`cycle` 设置全局播放模式，持久化到 INI `[playback] mode`。
- **INFO 区 7 行播放上下文**：3 条全局播放历史 ↑ + 当前节目 + 3 条即将播放 ↓。SHUFFLE 的未来 3 个随机指针预先生成（`shuffle_queue_`），INFO 预告即真实即将播放的曲目。
- **键位变更**：`l`/Enter 改为播放所选节目（peers 成为列表）；移除 `=`（追加）、`C`（清空）、`L`（弹窗）绑定。

---

## Panicast_V0.1 — 2026-07-15 — 架构重构（单文件 → 多模块）

**作者**: Panic <Deadship2003@gmail.com>

将原 ~15000 行单文件 `src/panicast.cpp` 按"一个源文件只做好一件事"拆分为 **34 个 .cpp + 39 个 .h** 的模块化工程，行为 100% 保留（man / `?` 帮助 / 所有交互热键与功能不变）。原 `panicast.cpp` 已删除。

### [build]
- CMake 子目录 + `include/panicast/<mod>/` 与 `src/<mod>/` 对称布局；`add_executable(panicast src/main.cpp …)` 聚合 34 个源文件。
- 版本号单一来源：CMake `PROJECT_VERSION 0.1` + `PANICAST_FIX_SUFFIX`（空=基线，`F01`..`F99`=修正），经 `configure_file` 生成 `version.h`。
- Linux x86_64 全新编译零错误（仅 nlohmann/json 既有 `-Warray-bounds` 误报）。

### [core]
- `types.h`（NodeType/URLType/AppMode/AppState/PlayMode/TreeNode/PlaylistItem/SavedPlaylistItem）
- `paths`/`logger`/`event_log`/`thread_pool`/`safe_tmp`/`string_utils`/`clipboard`/`text_width`(emoji/CJK 宽度，雷区隔离)/`terminal`(TerminalDetector+emoji 宽度探针)/`constants`/`platform`/`win_raii`

### [net]
- `network`(curl 抓取+proxy，CurlRAII 折入)/`url_classifier`/`url_guard`(SSRF)/`ytdlp_runner`(posix_spawn 安全执行)

### [config]
- `ini_config`（INI 读写 + 153 行默认生成 + `[network]/[youtube]/[colors]/[display]` getter）

### [storage]
- `database`(DatabaseManager，StmtRAII 折入)/`cache`/`youtube_cache`/`persistence`(树↔nodes 序列化)

### [parsers]
- `feed_parser`：`IFeedParser` 接口 + `ParserRegistry`（Meyers 单例）+ `REGISTER_PARSER` 宏 —— Linux module_init 风格自注册，新增格式仅加一文件 + 一行宏。
- `rss_parser`/`youtube_atom`(RSSParser 内)/`opml_parser`/`itunes_search`/`youtube_channel_parser`(多 TAB)/`xml_helpers`
- `spawn_load_feed` 已改用 `ParserRegistry` 分发（RSS/OPML/YouTube channel/playlist/atom），行为不变。

### [theme]
- `pairs`(color-pair 编号常量)/`theme`(Theme+9 调色板+apply/cycle，仍为 UI 成员)/`colors`(StatusBarColorRenderer+状态色对)

### [ui]
- `ui`(主 TUI，全内联)/`border`(draw_box+protect_border)/`icons`/`art`/`terminal`/`layout_metrics`/`layout_guard`
- 终端/信号生命周期(tui_cleanup/signal_handler/g_exit_requested 等)归入 `ui.cpp`。

### [playback]
- `mpv_controller`(libmpv + ytdl-raw-options 注入 proxy/cookies)/`sleep_timer`

### [app]
- `app`(class App，全内联 ~5300 行)/`progress`(ProgressManager+下载进度)/`online_state`
- `main.cpp`(入口，从原文件末尾抽出)

### [youtube]（功能保留，自 B9n3f12 沿用）
- 频道多 TAB 解析：`yt-dlp --flat-playlist -J` 枚举频道实际全部 tab → 各 tab 为可再展开惰性子节点；展开 tab → `--dump-json` 取节目；Playlists tab 内播放列表可逐层展开。
- 唯一 yt-dlp 方案（curl+RSS 已移除，本环境 RSS 404）。结构化缓存(episode_cache)按 URL 存，R 键刷新。
- 播放：MPV 经 ytdl hook 调 yt-dlp 解析真实流地址后播放，注入与解析一致的 proxy/cookies。

### 验证
- `panicast -v` → `PANICAST Panicast_V0.1`；`-h` 显示用法；`--purge` 正常。
- `panicast -a @56BelowTV` → 枚举 3 tab(Videos 352/Live 53/Shorts 778)；playlist → 100 节目；RSS 播客(NPR)经注册表 → 4 items。

### 已知后续（拟 F01+）
- App 内部深度拆分（tree_manager/playback_flow/modes/input）、UI 方法体由内联迁入 ui.cpp、theme 方法从 UI 抽出为独立 Theme 类、local_folder 独立文件、OnlineState 并入 mode_online。

---

## Panicast_V0.1-F01 — 2026-07-15 — 播放与解析加固

### [playback]
- **SSH 远程自动纯音频**：新增 `Utils::is_ssh_session()`（探测 `SSH_CONNECTION`/`SSH_TTY`/`SSH_CLIENT`，OpenSSH sshd 可靠设置）与 `Utils::has_local_display()`（= `has_gui() && !is_ssh_session()`，取代旧 `want_video_window`）。
  - SSH 会话：`MPVController::initialize` 走 `vo=null`+`vid=no`+`ytdl-format=bestaudio/best`——不开视频设备、不解码视频、只取音频流，省资源且无显示器不崩；`play()` 也不开视频窗。
  - 本地/WSL2（非 SSH）：自动音视频，视频开窗(`vo=gpu`) + `hwdec=auto-safe`（GPU 硬解优先，不可用回退软解）。
  - 修复：SSH 到无显示器主机播放视频节目时 `vo=gpu` 无 GL 上下文 → `Aborted(core dumped)`。
- `play()`/`play_video`/`play_list`/`play_list_from` 的开窗判断由 `has_gui()` 改为 `has_local_display()`（排除 SSH）。

### [parsers]
- **YouTube 解析对 null/非对象 -J 输出容错**：`parse_channel_tabs` 在 `json::parse` 后加 `is_object()` 守卫——yt-dlp 失败/被限流时 `-J` 可能输出 `null`，此前 `.value()` 抛 `type_error.306`（虽被 try/catch 接住但消息晦涩）；现给出清晰诊断"yt-dlp 返回非对象输出(null)，可能被限流/失败 + stderr"。`parse_video_list` 亦加 `is_object()` 跳过 null 行。

### [build]
- 版本号 → `Panicast_V0.1-F01`（`PANICAST_FIX_SUFFIX=F01`，CMake/vcpkg/constants/man/README 同步）。

---

## Panicast_V0.1-F02 — 2026-07-15 — 进度条占宽饱满

### [ui]
- **下载进度条**：改为"速率+ETA 右对齐，进度条填满剩余宽度"（与播放进度条一致），不再封顶 30 格——整行占宽饱满美观。移除不再使用的 `MIN/MAX_PROGRESS_BAR_WIDTH`/`PROGRESS_BAR_RESERVED_SPACE` 常量。
- 播放进度条本已"时间右对齐、bar 填满剩余"（无封顶），沿用。

### [build]
- 版本号 → `Panicast_V0.1-F02`。

---

## Panicast_V0.1-F03 — 2026-07-16 — 输入框回显与进度条时间对齐

### [ui]
- **input_box 回显修复**：`echo()`→`noecho()`。input_box 手动管理输入显示（update_input_display 全权重绘），不应让 ncurses 自动回显。此前 `echo()` 下 backspace 在 ncurses echo 层左移+擦除光标处字符，与手动重绘冲突——空输入 backspace 时光标漂移左移、字符嵌入边框、中心残留打印字符。改 noecho 后空输入 backspace 真正 no-op，光标固定居中。（`dialog()` 用 `mvwgetnstr` 需 echo，不动。）
- **播放进度条时间右对齐**：时间按真实时长动态宽度右对齐到行末（`time_x = 2 + available - time_w`），进度条填左侧剩余；先清行再画。修复此前 `timeline+time` 左对齐 + 播放头(▶)宽度误差导致时间右侧留空。时间现贴右边框，无尾随空格。

### [build]
- 版本号 → `Panicast_V0.1-F03`。

---

## Panicast_V0.1-F04 — 2026-07-16 — 下载断点续传与重试

### [app]
- **curl 下载断点续传**：若目标文件已存在部分内容，按其大小设 `CURLOPT_RESUME_FROM_LARGE`，以 `"ab"` 追加续写（不再 `"wb"` 从头）；失败时**保留半文件**供下次 D 继续续传（不再删除）。
- **重试不轻言放弃**：瞬时失败(超时/断连/部分文件/5xx/SSL 等)重试最多 3 次，每次更新续传偏移；超时 300s→600s，加连接超时 30s。
- **续传百分比**：`CurlProgressData` 加 `resume_offset`，进度回调按 `(偏移+本次已下载)/(偏移+本次总量)` 算整体百分比。
- **200-vs-206 处理**：续传时若服务器返回 200(不支持 range，返回完整文件)，自动截断从 0 重下，避免半文件+完整=损坏。
- **YouTube 下载**：yt-dlp 显式加 `--continue`(续 .part) + `--retries 20 --fragment-retries 20`(更持久，不轻易放弃)。

### [build]
- 版本号 → `Panicast_V0.1-F04`。

---

## Panicast_V0.1-F05 — 2026-07-16 — INFO 区占满宽度

### [ui]
- **INFO 面板内容占满宽度**：各标签行截断宽度由 `safe_cw - (前缀+1)` 调为 `safe_cw - 前缀`（总宽=cw，填到右边框前 1 列 right_w-2，不留右余量），仍左对齐。涉及 Audio/Video/Error/Title(下载列表)/URL换行/Streaming URL/Podcast-Date subtext/[DOWNLOADED 路径]/播放列表标题共 9 处。N=前缀长度保证不溢出边框（总宽=cw，结束于 right_w-2，边框在 right_w-1）。

### [build]
- 版本号 → `Panicast_V0.1-F05`。

---

## Panicast_V0.1-F06 — 2026-07-16 — Ctrl+Y 复制真实 URL（OSC 52）

### [core]
- **Ctrl+Y 复制到系统剪贴板**：`copy_to_clipboard` 工具(wl-copy/xclip/xsel/pbcopy/clip.exe)失败时回退 **OSC 52 终端剪贴板协议**——发送 `\033]52;c;<base64>\007` 到 `/dev/tty`，由终端模拟器写入本地系统剪贴板(Ctrl+V 可粘贴)。经 SSH 透传，解决远程无 DISPLAY 时 xclip/xsel 不可用、复制不到剪贴板的问题。base64 编码 payload。OSC 52 无可见输出，写 /dev/tty 绕过 ncurses 屏幕缓冲，不花屏。
- 弹窗(show_url_popup)仅作最后兜底（工具+OSC 52 均失败时），OSC 52 支持的终端下不再触发——消除"弹窗多行只能选第一行"问题。

### [build]
- 版本号 → `Panicast_V0.1-F06`。

---

## Panicast_V0.1-F07 — 2026-07-16 — 播放视频花屏修复

### [playback]
- **播放视频(如 .mp4)终端花屏修复**：`MPVController::initialize` 显式设 `terminal=no`——禁止 mpv 向终端写任何输出(状态行/窗口标题/转义序列)，此前未设该选项，libmpv 在 TTY 环境下可能默认 terminal=yes，播放时输出污染 ncurses 缓冲致花屏。同时设 `input-terminal=no`(不读终端输入) + `input-default-bindings=no`(不绑定默认键)，避免与 ncurses 键派发冲突。
- 终端尺寸变化已由 KEY_RESIZE→`resizeterm(0,0)`+`handle_resize`(清缓存+werase+wnoutrefresh) 处理，draw() 每帧 `clearok(stdscr,TRUE)`+`doupdate` 全量重绘，窗口移动/尺寸变化后自动恢复。

### [build]
- 版本号 → `Panicast_V0.1-F07`。

---

## Panicast_V0.1-F08 — 2026-07-16 — YouTube 解析日志英文化

### [parsers]
- **YouTube 频道解析输出统一为英文**：`youtube_channel_parser.cpp` 中所有面向用户的输出（`EVENT_LOG` 事件日志、节点 `error_msg`、子节点 `subtext`）由中文改为英文，与同位置已有的英文 `LOG` 风格对齐。
  - 涉及 11 处：`"{} 个视频"`→`"{} videos"`；`"yt-dlp 返回非对象输出(...)，可能被限流/失败"`→`"yt-dlp returned non-object output (...), possibly rate-limited/failed"`；`"[YouTube] 解析频道/tab/列表/枚举..."` 等事件日志；`"tab 解析失败"/"yt-dlp 解析失败"` 节点错误信息。
  - 同文件内中文代码注释一并译为英文。其余文件（`ytdlp_runner.cpp`/`youtube_cache.cpp`/`feed_parser.cpp` 等）的中文注释不在本轮范围，保持不动。
  - `diag_tail` 等本就英文的输出不变。

### [build]
- 版本号 → `Panicast_V0.1-F08`（`PANICAST_FIX_SUFFIX=F08`，CMake/vcpkg/constants/man/README 同步）。

---

## Panicast_V0.1-F09 — 2026-07-16 — 代码注释全面英文化

### [core]
- **全代码库中文注释译为英文**：将 F08 仅 `youtube_channel_parser.cpp` 的注释英文化扩展到全部源码与构建文件——共 **73 个文件、约 2774 行中文注释**，覆盖 `app.h`/`ui.h`/`ini_config.h`/`mpv_controller.*`/`database.*`/`persistence.*`/`utils.*`/`network.*`/`ytdlp_runner.*`/`rss_parser.*`/`opml_parser.*`/`itunes_search.*`/`url_classifier.*`/`url_guard.*`/`cache.*`/`youtube_cache.*`/`feed_parser.*`/`xml_helpers.*`/`types.h`/`terminal.*`/`thread_pool.*`/`paths.*`/`logger.*`/`event_log.*`/`constants.h`/`platform.h`/`safe_tmp.*`/`win_raii.h`/`pairs.h`/`colors.*`/`icons.h`/`art.*`/`border.*`/`layout_metrics.h`/`layout_guard.*`/`progress.*`/`online_state.*`/`sleep_timer.*`/`main.cpp`/`youtube_channel_parser.h`/`CMakeLists.txt`/`build.sh`/`.github/workflows/build.yml`/`tests/test_units.cpp`/`version.h.in` 等。
  - 仅翻译 `//`、`/* */`、`#` 注释内的中文；**不改动任何字符串字面量**（UI 文本/默认 INI 配置/SQL 等 `R"(...)"` 与 `"..."` 内的中文保留，面向中文用户）与**任何代码逻辑**。
  - **强校验**：注释剥离 + 尾随空格归一后 diff，证明仅注释与尾随空格变动；`strings` 二进制对比证明无字符串字面量改动；GCC16 `-O2` 全新编译零错误零告警。过程中发现并还原了一处子代理误删的 FOLDER 缓存加载代码块（`app.h`）与一处误译的 SQL 注释（`database.cpp` raw string 内）。
  - README.md / AUDIT_REPORT.md / CHANGELOG.md 为文档/历史记录，保持中文不动。

### [build]
- 版本号 → `Panicast_V0.1-F09`（`PANICAST_FIX_SUFFIX=F09`，CMake/vcpkg/constants/man/README 同步）。
