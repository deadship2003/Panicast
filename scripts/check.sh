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
say "预检完成。冒烟：./build/panicast --version"
