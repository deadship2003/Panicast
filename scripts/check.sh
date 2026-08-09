#!/usr/bin/env bash
# PaniCast 本地预检（与 CI 对齐）：clang-format 自检（咨询）+ 构建 + ctest。
# 用法：./scripts/check.sh      （可设 PANICAST_BUILD_JOBS=N 限制并行度）
# 退出码：configure/build 失败 → 非 0；格式/测试为咨询，不阻断（仅打印）。
set -uo pipefail
cd "$(dirname "$0")/.."

say()  { printf '\033[32m✓\033[0m %s\n' "$1"; }
warn() { printf '\033[33m⚠\033[0m %s\n' "$1"; }
info() { printf '\033[34m…\033[0m %s\n' "$1"; }
fail() { printf '\033[31m✗\033[0m %s\n' "$1"; }

# ── 1. clang-format（咨询；仅看相对 HEAD 改动的 .cpp/.h） ──────────────────
info "clang-format 自检（咨询，仅改动的 C++ 文件）"
if ! command -v clang-format >/dev/null 2>&1; then
  warn "未装 clang-format，跳过（apt install clang-format）"
else
  mapfile -t FILES < <(git diff --name-only HEAD -- '*.cpp' '*.h' '*.hpp' 2>/dev/null || true)
  if [ "${#FILES[@]}" -eq 0 ]; then
    say "无改动的 C++ 文件，跳过格式检查"
  else
    bad=0
    for f in "${FILES[@]}"; do
      [ -f "$f" ] || continue
      if ! diff -q <(clang-format "$f" 2>/dev/null) "$f" >/dev/null 2>&1; then
        warn "  格式差异: $f"; bad=1
      fi
    done
    if [ "$bad" -eq 0 ]; then say "改动文件格式 OK"
    else warn "请对上述文件运行：clang-format -i <file>（不阻断）"; fi
  fi
fi
echo

# ── 2. 构建（必须通过） ───────────────────────────────────────────────────
info "构建（Ninja Release + BUILD_TESTING=ON）"
if ! cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON; then
  fail "configure 失败"; exit 1
fi
if ! cmake --build build --parallel "${PANICAST_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"; then
  fail "build 失败"; exit 1
fi
say "build/panicast OK"
echo

# ── 3. 测试（best-effort） ────────────────────────────────────────────────
info "ctest（best-effort）"
if ctest --test-dir build --output-on-failure; then
  say "测试通过"
else
  warn "ctest 未通过或无测试（若 GTest 未装则测试被禁用：apt install libgtest-dev）"
fi
echo

# ── 4. 层边界自检（咨询）：UI 层零 Core「业务」直调（D11-4 验收门） ──────────
#   UI 是纯呈现层。依赖不变量（见 docs/ARCHITECTURE.md §2.1 / 稳定依赖原则）：
#     允许——横切基础设施：Utils::* 文本/显示工具、LOG/EVENT_LOG（UI 自己的日志面板，
#           见 ARCHITECTURE §3）、get_emoji_width 终端度量。性质等同标准库；mpv/cmus/
#           Qt/LLVM/Chromium 的界面层都直接用日志（横切关注点，不服从"只能往下调"的字面分层）。
#     禁止——Core 业务：Paths（文件系统）/crypto/ThreadPool/EventBus/process_utils/safe_tmp。
#   （net/playback/app 的 singleton 读——URLClassifier/SleepTimer/OnlineState/TikTokRegion——
#    是 D12 IFrontend 的前沿：UI 自查状态而非收视图模型，本轮仅记录、不门控；见 DECISIONS_LOG D11-4。）
info "层边界自检（咨询）：UI 层零 Core 业务直调"
viols=$(grep -rnE 'Paths::|machine_key|token_seal|token_open|Key32|ThreadPool|EventBus::|which_binary|safe_tmp|popen\(' src/ui/ 2>/dev/null || true)
if [ -z "$viols" ]; then
  say "UI 层零 Core 业务直调 ✓"
else
  warn "UI 层出现 Core 业务直调（违反 docs/ARCHITECTURE.md §2.1 分层）："
  printf '%s\n' "$viols"
  warn "  须经 App/服务层；详见 DECISIONS_LOG D11-4"
fi
echo
say "预检完成。冒烟：./build/panicast --version"
