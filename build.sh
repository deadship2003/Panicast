#!/bin/bash
# ╔═══════════════════════════════════════════════════════════════════════════╗
# ║                    PaniCast Multi-Platform Build Script                    ║
# ╚═══════════════════════════════════════════════════════════════════════════╝
# Usage: ./build.sh [all|linux|arm64|windows|clean]

set -e
cd "$(dirname "$0")"

# Memory-aware parallel job count: ~1 GiB per compile job (cc1plus on heavy files can exceed
#   800 MiB), leave 1 GiB headroom for the OS, cap at nproc. Prevents OOM kills on low-RAM hosts
#   (e.g. 4 GiB → 2 jobs instead of $(nproc)=16). Override with PANICAST_BUILD_JOBS.
if [ -z "${PANICAST_BUILD_JOBS:-}" ]; then
    _NJ=$(nproc 2>/dev/null || echo 4)
    _GB=$(awk '/MemTotal/{printf "%d", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo 8)
    # ~1 GiB per job, leave ~2 GiB headroom (OS + editor + running panicast). min 1.
    JOBS=$(( _GB > 2 ? _GB - 2 : 1 ))
    [ "$JOBS" -gt "$_NJ" ] && JOBS=$_NJ
else
    JOBS="$PANICAST_BUILD_JOBS"
fi
export JOBS

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}╔═══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║           PaniCast Multi-Platform Build Script                ║${NC}"
echo -e "${BLUE}╚═══════════════════════════════════════════════════════════════╝${NC}"

build_linux() {
    echo -e "\n${BLUE}[Linux x86_64] 编译中...${NC}"
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel "$JOBS"
    echo -e "${GREEN}✓ 完成: build/panicast${NC}"
    # Y01: libqrencode is optional. If absent, cmake warns and Y-mode QR login falls back to text.
    if ! pkg-config --exists libqrencode 2>/dev/null && [ ! -f /usr/include/qrencode.h ]; then
        echo -e "${YELLOW}⚠ 未检测到 libqrencode：Y 模式扫码登录将回退为纯文本 user_code（无 QR 图片）。${NC}"
        echo -e "${YELLOW}  安装：apt install libqrencode-dev  /  pacman -S qrencode  /  vcpkg install qrencode${NC}"
    fi
    # Y05: a JavaScript runtime is a RUNTIME dependency for YouTube playback. yt-dlp 2026.07+
    # needs it to solve YouTube's nsig "n challenge". quickjs-ng (binary `qjs`, ~2MB, ~10× faster
    # cold-start) is the lightweight default; deno (~106MB) is the fallback. Detection accepts the
    # binary under either name `qjs` or `qjsng` — Arch's quickjs-ng and Debian's quickjs both ship
    # `qjs`, but some quickjs-ng builds name it `qjsng`; the C++ resolver (find_qjs_binary) covers
    # both at runtime. Set [youtube] js_runtime in config.ini (quickjs default, deno fallback).
    if ! command -v qjs >/dev/null 2>&1 && ! command -v qjsng >/dev/null 2>&1 && ! command -v deno >/dev/null 2>&1; then
        echo -e "${YELLOW}⚠ 未检测到 qjs/qjsng/deno：YouTube 播放/下载将失败（yt-dlp 求解 n 挑战需要 JS 运行时）。${NC}"
        echo -e "${YELLOW}  推荐 quickjs-ng(2MB，二进制名 qjs)：${NC}"
        echo -e "${YELLOW}    Arch: paru -S quickjs-ng (AUR)  /  Debian: apt install quickjs(注意版本，旧版被 yt-dlp 拒) 或 pip install quickjs-ng${NC}"
        echo -e "${YELLOW}    或从 https://github.com/quickjs-ng/quickjs/releases 取 ≥0.12.0 放到 PATH(命名为 qjs)${NC}"
        echo -e "${YELLOW}  或 deno(106MB)：curl -fsSL https://deno.land/install.sh | sh${NC}"
        echo -e "${YELLOW}  注: quickjs 需 EJS solver——pip install -U \"yt-dlp[default]\"；deno 可自动从 npm 拉 EJS${NC}"
    elif ! command -v qjs >/dev/null 2>&1 && ! command -v qjsng >/dev/null 2>&1; then
        echo -e "${YELLOW}ℹ 仅有 deno，未检测到 qjs/qjsng：建议装 quickjs-ng(2MB，冷启动快约 10×)以消除播放初始卡顿。${NC}"
    else
        echo -e "${GREEN}✓ JS 运行时: $(command -v qjs 2>/dev/null || command -v qjsng 2>/dev/null) (yt-dlp nsig solver)${NC}"
    fi
}

build_arm64() {
    echo -e "\n${BLUE}[Linux ARM64] 编译中...${NC}"
    if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        echo -e "${YELLOW}⚠ 跳过: 需安装 gcc-aarch64-linux-gnu${NC}"
        return
    fi
    cmake -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux.cmake
    cmake --build build-arm64 --parallel "$JOBS"
    echo -e "${GREEN}✓ 完成: build-arm64/panicast${NC}"
}

build_windows() {
    echo -e "\n${BLUE}[Windows x64] 编译中...${NC}"
    if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        echo -e "${YELLOW}⚠ 跳过: 需安装 mingw-w64${NC}"
        return
    fi
    cmake -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-mingw.cmake
    cmake --build build-windows --parallel "$JOBS"
    echo -e "${GREEN}✓ 完成: build-windows/panicast.exe${NC}"
}

case "${1:-all}" in
    all)    build_linux; build_arm64; build_windows ;;
    linux)  build_linux ;;
    arm64)  build_arm64 ;;
    windows) build_windows ;;
    clean)  rm -rf build build-arm64 build-windows ;;
    *)      echo "用法: $0 [all|linux|arm64|windows|clean]" ;;
esac
