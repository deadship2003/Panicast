#!/bin/bash
# ╔═══════════════════════════════════════════════════════════════════════════╗
# ║          Panicast — build + install (single script, arch-aware)           ║
# ╚═══════════════════════════════════════════════════════════════════════════╝
# Usage:
#   ./build.sh                     # compile for the current host arch (auto-detected)
#   ./build.sh --arch=arm64        # cross-compile aarch64 (needs aarch64-linux-gnu-gcc/g++)
#   ./build.sh --arch=amd64        # force x86_64 build
#   ./build.sh install             # bootstrap: JS runtime + build deps + build + install
#   ./build.sh install --no-deps   #   (skip the system build-deps step)
#   ./build.sh clean               # remove build/ and build-arm64/
#
# Everything installs to /usr/local/bin (system PATH — no ~/.local/bin, no PATH edit).
# The `install` path therefore requires sudo.

set -e
cd "$(dirname "$0")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'
say()  { echo -e "${GREEN}✓${NC} $1"; }
warn() { echo -e "${YELLOW}⚠${NC} $1"; }
info() { echo -e "${BLUE}…${NC} $1"; }
die()  { echo -e "${RED}✗${NC} $1" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

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

# Auto-detect host arch (uname -m → canonical target name).
case "$(uname -m)" in
    x86_64|amd64)   HOST_ARCH=amd64 ;;
    aarch64|arm64)  HOST_ARCH=arm64 ;;
    *)              HOST_ARCH=amd64 ;;  # unknown → assume amd64
esac

# ── Arg parsing ───────────────────────────────────────────────────────────────
MODE=build        # build | install | clean
ARCH=""           # empty = auto-detect
NO_DEPS=0
for arg in "$@"; do
    case "$arg" in
        install|setup) MODE=install ;;
        clean)         MODE=clean ;;
        --no-deps)     NO_DEPS=1 ;;
        --arch=*)      ARCH="${arg#--arch=}" ;;
        arch=*)        ARCH="${arg#arch=}" ;;
        *) echo "未知参数: $arg"; echo "用法: $0 [--arch=amd64|arm64] [install [--no-deps]] [clean]"; exit 1 ;;
    esac
done

# Resolve target arch (aliases accepted).
TARGET="${ARCH:-$HOST_ARCH}"
case "$TARGET" in
    amd64|x86_64)   TARGET=amd64 ;;
    arm64|aarch64)  TARGET=arm64 ;;
    *) die "未知架构: $TARGET (支持 amd64|arm64)" ;;
esac

# ── Install: everything → /usr/local/bin (system dirs, sudo required) ─────────
# First line of `--version` output (build-invariant version string), or empty if unsupported.
bin_version() { "$1" --version 2>/dev/null | head -1; }

install_to_system_bin() {  # $1=src  $2=name
    local dst="/usr/local/bin/$2"
    local new_v old_v
    new_v=$(bin_version "$1"); old_v=$(bin_version "$dst")
    if [ -x "$dst" ] && [ -n "$new_v" ] && [ "$new_v" = "$old_v" ]; then
        say "$2 already installed (same version: ${new_v}) (skip)"
        return 0
    fi
    info "installing $2 to $dst (system-wide — sudo required, may prompt for password)"
    sudo cp -f "$1" "$dst"
    sudo chmod +x "$dst"
    say "$2 -> $dst"
}

# yt-dlp 2026.07+ needs a JS runtime to solve YouTube's nsig "n challenge".
# quickjs-ng (~2MB, ~10× faster cold-start than deno) is the default; deno (~106MB) is the fallback.
# Both come from the vendor/ bundle and install to /usr/local/bin (no ~, no PATH edit).
install_js_runtime() {
    if [ -x vendor/quickjs/qjs ]; then
        install_to_system_bin vendor/quickjs/qjs qjs
        if have yt-dlp && ! python3 -c "import yt_dlp_ejs" 2>/dev/null && ! yt-dlp --version 2>/dev/null | grep -qi ejs; then
            warn "quickjs needs the EJS solver: pip install -U \"yt-dlp[default]\"  (or set [youtube] js_runtime = deno)"
        fi
    elif [ -x vendor/deno/deno ]; then
        install_to_system_bin vendor/deno/deno deno
    else
        warn "no bundled JS runtime (vendor/quickjs/qjs or vendor/deno/deno) — yt-dlp nsig solving will fail"
    fi
}

# 0 if every build dep for the active package manager is already installed.
deps_installed() {
    if have apt-get; then
        for p in mpv libmpv-dev libncurses5-dev libncursesw5-dev libcurl4-openssl-dev \
                 libsqlite3-dev libxml2-dev libfmt-dev nlohmann-json3-dev libqrencode-dev \
                 cmake ninja-build g++; do
            dpkg -s "$p" >/dev/null 2>&1 || return 1
        done
        return 0
    elif have pacman; then
        for p in mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc; do
            pacman -Q "$p" >/dev/null 2>&1 || return 1
        done
        return 0
    elif have dnf; then
        for p in mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel libxml2-devel \
                 fmt-devel nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++; do
            rpm -q "$p" >/dev/null 2>&1 || return 1
        done
        return 0
    fi
    return 1  # unknown package manager → assume not installed
}

install_deps() {
    if deps_installed; then
        say "build deps already installed (skip)"
        return 0
    fi
    if have apt-get; then
        info "Installing build deps via apt (sudo)..."
        sudo apt-get update -y
        sudo apt-get install -y \
            mpv libmpv-dev libncurses5-dev libncursesw5-dev \
            libcurl4-openssl-dev libsqlite3-dev libxml2-dev libfmt-dev \
            nlohmann-json3-dev libqrencode-dev cmake ninja-build g++
        say "build deps installed"
    elif have pacman; then
        info "Installing build deps via pacman (sudo)..."
        sudo pacman -S --needed --noconfirm \
            mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc
    elif have dnf; then
        info "Installing build deps via dnf (sudo)..."
        sudo dnf install -y \
            mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel libxml2-devel fmt-devel \
            nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++
    else
        warn "No apt/pacman/dnf detected — skipping build-deps. Install them manually (see README)."
    fi
}

install_panicast() {
    info "installing panicast to /usr/local/bin (system-wide — sudo required, may prompt for password)"
    if sudo cmake --install build 2>/dev/null; then
        say "installed -> /usr/local/bin/panicast"
    else
        warn "cmake --install failed, trying direct copy"
        sudo cp -f build/panicast /usr/local/bin/panicast
        say "installed -> /usr/local/bin/panicast"
    fi
}

do_install() {
    have sudo || die "sudo is required — the install path writes to /usr/local/bin"
    install_js_runtime
    [ "$NO_DEPS" = "0" ] && install_deps
    build_native   # install always builds for THIS machine (host arch)
    install_panicast
    echo
    echo -e "${GREEN}══════════════════════════════════════════════════════${NC}"
    echo -e "${GREEN} Panicast ready.${NC}"
    echo -e " Run: ${BLUE}panicast${NC}"
    echo -e "${GREEN}══════════════════════════════════════════════════════${NC}"
}

# ── Build ─────────────────────────────────────────────────────────────────────
build_native() {  # native build for the host arch
    echo -e "\n${BLUE}[${HOST_ARCH}] 编译中...${NC}"
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

build_arm64_cross() {  # amd64 host → arm64 target
    echo -e "\n${BLUE}[arm64] 交叉编译中...${NC}"
    if ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 || ! command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
        die "缺少交叉编译工具链 aarch64-linux-gnu-gcc/g++。安装：apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu"
    fi
    cmake -B build-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-linux.cmake
    cmake --build build-arm64 --parallel "$JOBS"
    echo -e "${GREEN}✓ 完成: build-arm64/panicast${NC}"
}

build_target() {
    case "$TARGET" in
        amd64)
            [ "$HOST_ARCH" = "amd64" ] || die "cross-compiling amd64 from a $HOST_ARCH host is not supported"
            build_native ;;
        arm64)
            [ "$HOST_ARCH" = "arm64" ] && build_native || build_arm64_cross ;;
    esac
}

# ── Dispatch ──────────────────────────────────────────────────────────────────
case "$MODE" in
    clean)   rm -rf build build-arm64; say "cleaned build/ build-arm64/" ;;
    install) do_install ;;
    build)   build_target ;;
esac
