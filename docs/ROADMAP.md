# PaniCast 开发计划（人日任务 · 活清单）

> 铁律：**每个人日任务完成后，源码可编译（0-warning）、可执行文件正常运行、ctest 绿。** 不重写，增量演进。
> 准则见 `docs/DESIGN.md`。完成的任务打 `[x]`，并同步 `CHANGELOG.md` + `DECISIONS_LOG.md`(ADR)。每步保绿色可回退 commit。

## 里程碑 M0 — 核心抽象骨架（不破坏现 app）
- [x] **D1 — EventBus 核心 + 首个真实消费者** ✅ 2026-08-05
  - `include/panicast/core/event_bus.h`（**header-only**，类型安全 `subscribe<E>/publish<E>` + `unsubscribe(token)`，线程安全；`post/drain` 待迁 `pending_select_` 时加）+ 单测。
  - 首个真实生产者 = `EventLog::push` 顺带 `publish(LogEvent)`（379 处日志全上总线，零行为变化）。
  - 接入 `CMakeLists.txt`。**验收**：编译 0-warning、ctest 含 EventBus 用例、`./build/panicast` 启动+播放正常。
- [ ] **D2 — IProxyManager 接口 + resolveProxy 规则链**
  - `include/panicast/net/proxy_manager.h`（`ProxyConfig` + `IProxyManager::resolveProxy(url,platform)` 平台→域名→全局→直连）+ impl（包装现有 `[network] proxy`/`normalize_proxy`）+ 单测。
  - **验收**：单测过、app 运行、`Ctrl+N` 代理配置仍生效。
- [ ] **D3 — 调用点切换到 IProxyManager（含 Downloader）**
  - 把 `apply_network_proxy` 的 3 处（`network.cpp`/`bilibili_api.cpp`/`itunes_search.cpp`）+ `ytdlp_runner`(`--proxy`) 改走 `resolveProxy`；核对 `app_download` 所有下载路径均经 Connectivity。
  - **验收**：编译绿、解析/下载带代理冒烟正常、无回归。
- [ ] **D4 — Media/MediaID 骨架（不改 TreeNode）**
  - `include/panicast/domain/media.h`（`MediaID` + Media 视图/adapter，复用 `TreeNodePtr` 作底层）+ 单测。
  - **验收**：编译绿、app 行为零变化。
- [ ] **D5 — M0 端到端样例 + 收尾**
  - 一条最小路径走新抽象（RSS → parser provider → MediaID → Connectivity 解析 → playback 播放）+ 集成冒烟测试；写 ADR + CHANGELOG。
  - **验收**：样例跑通、全 ctest 绿、app 全功能正常。→ **M0 达成（最小可演进系统）**

## 里程碑 M1 — Connectivity 全覆盖 + 热键统一
- [ ] **D6 — Downloader 全面经 Connectivity**（补齐 D3 未覆盖的下载路径 + 测试）。
- [ ] **D7 — Keymap/Action**：`ui/keymap.h`（键→Action，从 `[keys]` 配置加载）+ 迁移 `app_input.cpp` 主 switch + 修 FX-1 模式循环。**验收**：热键可用、可自定义。

## 里程碑 M2 — Provider 化 + Media 收敛（每 parser 一小步）
- [ ] **D8…** — 各 parser 确认 Provider 化（youtube/bilibili/itunes/rss/m3u/opml/tiktok）；Media 域从 TreeNode 逐步收敛。每个一进步、保持可运行。

## 里程碑 M3 — 字幕/ASR + 拆 god-object
- [ ] **Dn — SubtitleController** 中介者（修 READY/L-key 冲突）；ASR 作 SubtitleProducer。
- [ ] **Dn — 拆 god-object**：`App`→`AppController`+子控制器；split `app_run.cpp`/`ui.cpp`/`mpv_controller.cpp`/`ini_config.h`。每个拆分一进步、保持可运行。

## 规则
- 每个人日任务完成 → 本文件对应项打 `[x]` + `CHANGELOG.md` 一行 + 架构决策入 `DECISIONS_LOG.md`。
- 计划随实际演进更新（任务可拆并、可插入）；不顺就回退到上一个绿色提交。
- 每步必须保 `git` 可回退点（编译绿+可运行才 commit）。
