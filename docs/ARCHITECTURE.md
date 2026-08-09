# 架构总览（真实状态）

> 本文档描述 PaniCast **当前**的真实架构，作为开发与重构的唯一架构事实来源。
> 决策记录见 `DECISIONS_LOG.md`（N0x/Y24.x 等条目，体例：Context/Approach/Verification/Followups）；
> 缺陷清单见 `AUDIT_REPORT.md`（P1/P2/DB/FX 等）。

## 1. 技术栈

C++17 · ncurses（TUI，强制宽字符 libncursesw）· libmpv（client API，非 IPC）· libcurl · libxml2 · sqlite3 · fmt · nlohmann_json · OpenSSL/libcrypto（Bilibili WBI + token 加密）· libqrencode（OAuth 登录 QR，可选）。
构建：CMake（单 `panicast` 可执行）+ Ninja + vcpkg（`vcpkg.json`）。

## 2. 目录约定

```
include/panicast/<module>/*.h   声明
src/<module>/*.cpp              实现
```
模块（`src/` 1:1 对应）：
| 模块 | 职责 |
|---|---|
| `core` | 基础设施：types/constants/logger/event_log/thread_pool/crypto/paths/terminal/safe_tmp/platform/utils |
| `config` | INI 配置（`ini_config.h`） |
| `net` | 网络：HTTP（`network.cpp` 代理入口）、URL 分类、yt-dlp 运行、Google OAuth、Bilibili API、远程控制（server/session/ws/command_bus/protocol）、TikTok 区域 |
| `parsers` | 解析器（`IFeedParser` + Registry）：rss/itunes/opml/m3u/youtube_channel/bilibili/transcript |
| `playback` | libmpv 封装（`mpv_controller`）、睡眠定时 |
| `storage` | 持久化：database + 各 repo（history/tree/feed_cache/account/player_state/accounts/cache/youtube_cache）+ `persistence` 抽象 |
| `subtitle` | 字幕：`subtitle_parser`（`ISubtitleParser` + Registry）、`subtitle_manager`、`transcription_engine`（ASR/whisper） |
| `theme` | 颜色/主题/字符对 |
| `ui` | ncurses 渲染：ui/popups/lyric_renderer/tree_renderer/status_bar/info_panel/layout/border/icons/art/qr |
| `app` | 应用层：`app_run`（主循环）、`app_input`（键派发）、`app_playback/download/search/subscriptions/navigation/...`、modes/ |

入口：`src/main.cpp` → `App::run()`（`src/app/app_run.cpp`）。

### 2.1 层间依赖规则（UI 解耦不变量 · D11-4 确立）

**核心判据（稳定依赖原则）**：某层能否依赖 X，看 X 是否随业务变化——稳定基础设施可依赖，易变业务/运行时状态不可依赖。

UI（`src/ui/`）是纯呈现层，依赖规则：
- **允许（横切基础设施，性质等同标准库）**：`Utils::*`（文本/显示工具）、`LOG`/`EVENT_LOG`（后者是 UI 右侧日志环形缓冲，见 §3）、`get_emoji_width`（终端度量）、`URLClassifier::classify`/`is_youtube`（**无状态**纯函数：URL 串→`URLType` 枚举、表驱动、无 I/O 无 `instance()`——物理在 `net/` 但性质同 Utils，§3 亦列为基础设施）。同类软件（mpv / cmus / Qt / LLVM / Chromium）的界面层都直接用日志与工具函数——日志是**横切关注点**（cross-cutting concern），不服从"只能往下调"的字面分层。
- **禁止（Core 业务 / 运行时状态）**：`Paths`（文件系统）、`crypto`、`ThreadPool`、`EventBus`、`process_utils`、`safe_tmp`，以及 `storage`/`net`/`playback`/`app` 的**业务/运行时状态查询**（即带 `instance()` 的有状态 singleton——如 `SleepTimer`/`OnlineState`/`TikTokRegion`）。UI 不得直查运行时状态，应经视图模型 / 事件获得数据。**D12-1 已解耦**这 3 个运行时 singleton → App 每帧构建 `DisplayContext`（睡眠定时 + 区域名）推进 `ui.draw()`。

> 由 `scripts/check.sh` §4 grep 门固化：`src/ui/` 出现 Core 业务符号即报警（咨询、不阻断）。Utils / LOG / URLClassifier 不剥离——剥离横切基础设施只给文件改名，不消除任何真实耦合（`DECISIONS_LOG` D11-4）。当前门**绿**（0 命中）。D12-1 后 `src/ui/` 的运行时 singleton 查询（`SleepTimer`/`OnlineState`/`TikTokRegion`）已**归零**——改由 `DisplayContext` 视图模型推入；`URLClassifier`（纯函数）保留。
>
> **ncurses 边界（D12-3a · M1 验收）**：ncurses 收敛在 `ui/`（渲染）+ `theme/`（呈现层）——二者本就是 ncurses 的归属层。`core/` 与 `config/` **零 ncurses 依赖**（`core/win_raii.h` 已归位 `ui/`；`config/ini_config.h` 颜色映射改原生 int）。core 基础设施对终端的操作走原生 termios/ANSI 转义直写 `/dev/tty`、绕过 ncurses（utils/process_utils/terminal 仅注释提及 ncurses）。

## 3. 已有核心抽象（重构须复用，勿另起炉灶）

- **Provider 模式（自注册）**：
  - `IFeedParser` + `ParserRegistry` + `REGISTER_PARSER(XxxParser)` 宏（`include/panicast/parsers/feed_parser.h`）。新增解析器 = 实现接口 + 末尾一行宏，零改 switch。
  - `ISubtitleParser` + `SubtitleParserRegistry`（`include/panicast/subtitle/subtitle_parser.h`），按格式（json/srt/vtt/lrc/twt）注册。
- **远程控制解耦**：`RemoteControlInterface`（`include/panicast/net/remote_protocol.h`）—— App 实现该接口，`RemoteServer`/`RemoteSession` 依赖接口而非 App。可组合、可测。
- **持久化抽象**：`include/panicast/storage/persistence.h` + 各 `*_repo`。
- **基础设施**：`thread_pool`（worker 池）、`EventLog`（UI 右侧日志环形缓冲，`EVENT_LOG` 宏；**不是**发布/订阅总线）、`URLClassifier`、`crypto`（EtM ChaCha20+HMAC token 加密）。

## 4. 领域模型

- `TreeNode`（`include/panicast/core/types.h`）—— **事实上的 Media 对象**：订阅树/收藏/播放列表的统一载体，80+ 字段（类型/URL/缓存/字幕/账号/链接…）。
- `TreeNodePtr = std::shared_ptr<TreeNode>` —— **事实上的 MediaID**，模块间传指针身份。
- 枚举：`NodeType`、`AppMode`（RADIO/PODCAST/FAVOURITE/HISTORY/ONLINE/ACCOUNT/BILIBILI/TIKTOK/IPTV）、`PlayMode`、`MediaType`（9 类显示分类）、`URLType`、`TranscriptStatus`、`AppState`。

## 5. 线程模型（三线程）

| 线程 | 职责 |
|---|---|
| **UI**（主） | ncurses 单线程，非线程安全；所有 App/渲染/输入在此 |
| **worker 池**（`thread_pool`，10） | 网络/解析/下载等异步任务 |
| **mpv event** | libmpv 事件回调（播放状态/结束） |

**三个跨线程穿越点**（见 `DECISIONS_LOG` N01/N02）：
1. `RemoteCommandBus`（写路径）—— server 线程 `push` 命令，UI 帧每轮 `drain` 派发。点对点队列。
2. `RemoteStateSnapshot`（读路径）—— UI 帧在 `remote_state_mtx_` 下构建快照，server 线程读副本。**刻意避免** server 线程交叉锁 `tree_mutex`/`playlist_mutex_`。
3. `pending_select_` + 散落回调（worker→UI 的异步通知）—— **竞态根因**（AUDIT P1-4/P1-5/P1-8）。这是后续 `EventBus` 要替换的目标（重构计划 Tier 2 / R1.1–R1.2）。

> 注：`EventBus` 目前**尚不存在**；`RemoteCommandBus` 是点对点，`EventLog` 是日志缓冲，二者都不是通用 pub/sub。

## 6. 已知技术债（重构输入，详见 AUDIT_REPORT）

- **上帝对象/文件**：`App`（`app.h` 605 行声明）、`app_run.cpp`(1195)、`mpv_controller.cpp`(1149)、`ini_config.h`(957，配置+默认+注释混杂)、`app_input.cpp`(746，硬编码 `switch(ch)`)。
- **竞态**：`pending_select_` + 跨线程裸回调（P1-4 向量竞态、P1-5 两锁一树、P1-8 `~App` 缺失致 UAF）。
- **SQL 卫生**：多处忽略 `exec_sql` 返回、Bilibili 一处 `fmt::format` 拼接（注入面）、`atoll` 解 TEXT 时间戳恒为 0、`StmtRAII` 预编译缓存造好但零使用。
- **热键硬编码**：`app_input.cpp` `handle_input` 的 `switch(ch)`，不可配置（`ini_config.h` 注释自承）。
- **代理**：`apply_network_proxy(CURL*)`（`net/network.cpp`）已统一灌 curl+yt-dlp+mpv，但未抽象为带"分平台/域名规则"的 `IProxyManager`。
- **Windows 构建**：posix_spawn/HOME 路径致坏（P2-X）。

## 7. 构建/版本约束

- `CMakeLists.txt` 是**显式源列表**（非 glob）：新增 `.cpp` 必须手动加入 `add_executable(panicast ...)`（`:242` 起）。
- **版本单一事实来源**：5 处须同步——`CMakeLists.txt` / `vcpkg.json` / `include/panicast/core/constants.h` / `man/panicast.1` / `README.md`。
- 数据迁移：sqlite3 WAL + 幂等 `add_column_if_missing` + 自有事务 + `sqlite3_close_v2`；`SCHEMA_VERSION` 驱动（现 47）。用户数据跨版本必须无损。
