#!/bin/bash
# ╔═══════════════════════════════════════════════════════════════════════════╗
# ║   PaniCast — self-contained setup / deploy script                      ║
# ║   Bundles: source + built binary + JS runtime (vendor/quickjs or deno)     ║
# ╚═══════════════════════════════════════════════════════════════════════════╝
# Usage:
#   ./setup.sh              # install build deps (apt) + JS runtime + build + install panicast
#   ./setup.sh --no-deps    # skip the apt build-deps step (you already have them)
#   ./setup.sh --no-sudo    # user-local only: JS runtime -> ~/.local/bin, podradio -> ~/.local/bin
#   ./setup.sh js-only      # only install the bundled JS runtime (quickjs; deno fallback)
#   ./setup.sh deno-only    # legacy: only install the bundled deno runtime
#
# This script is idempotent — safe to re-run.

set -e
cd "$(dirname "$0")"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
say()  { echo -e "${GREEN}✓${NC} $1"; }
warn() { echo -e "${YELLOW}⚠${NC} $1"; }
info() { echo -e "${BLUE}…${NC} $1"; }

NO_DEPS=0; NO_SUDO=0; JS_ONLY=0; DENO_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --no-deps)  NO_DEPS=1 ;;
    --no-sudo)  NO_SUDO=1 ;;
    js-only)    JS_ONLY=1 ;;
    deno-only)  DENO_ONLY=1 ;;
    *) echo "Unknown arg: $arg"; exit 1 ;;
  esac
done

have() { command -v "$1" >/dev/null 2>&1; }
sudo_if() { if [ "$NO_SUDO" = "1" ]; then "$@"; else sudo "$@"; fi; }

# ── 1. Install JS runtime (bundled quickjs preferred; deno fallback) ──────────
# yt-dlp 2026.07+ needs a JS runtime to solve YouTube's nsig "n challenge".
# quickjs-ng (~2MB, ~10× faster cold-start than deno) is the lightweight default.
# deno (~106MB) is kept as a fallback for environments where quickjs's EJS solver
#   can't be installed (quickjs can't fetch EJS from npm — needs `yt-dlp[default]`).
install_to_local_bin() {  # $1=src  $2=name
  mkdir -p "$HOME/.local/bin"
  cp -f "$1" "$HOME/.local/bin/$2"
  chmod +x "$HOME/.local/bin/$2"
  if [ "$NO_SUDO" = "0" ] && have sudo; then
    sudo cp -f "$1" "/usr/local/bin/$2" 2>/dev/null && sudo chmod +x "/usr/local/bin/$2" && say "$2 -> /usr/local/bin/$2" || warn "could not write /usr/local/bin/$2 (using ~/.local/bin only)"
  fi
  case ":$PATH:" in
    *":$HOME/.local/bin:"*) ;;
    *) warn "~/.local/bin is not on PATH. Add 'export PATH=\$HOME/.local/bin:\$PATH' to your ~/.bashrc";;
  esac
}

install_quickjs() {
  info "Installing bundled quickjs-ng (yt-dlp nsig JS runtime; ~2MB, fast cold-start)..."
  local SRC="vendor/quickjs/qjs"
  if [ ! -x "$SRC" ]; then
    warn "vendor/quickjs/qjs missing — skipping quickjs install (will try deno fallback)"
    return 1
  fi
  install_to_local_bin "$SRC" qjs
  say "qjs -> ~/.local/bin/qjs"
  # EJS solver: quickjs can't fetch from npm; ensure yt-dlp[default] (yt-dlp-ejs) is installed.
  if have yt-dlp; then
    if ! python3 -c "import yt_dlp_ejs" 2>/dev/null && ! yt-dlp --version 2>/dev/null | grep -qi ejs; then
      warn "quickjs needs the EJS solver. Install: pip install -U \"yt-dlp[default]\"  (brings yt-dlp-ejs)"
      warn "  (or keep using deno — set [youtube] js_runtime = deno in config.ini)"
    fi
  fi
  return 0
}

install_deno() {
  info "Installing bundled deno runtime (fallback JS runtime for yt-dlp nsig challenge)..."
  local SRC="vendor/deno/deno"
  if [ ! -x "$SRC" ]; then
    warn "vendor/deno/deno missing — falling back to online install: curl -fsSL https://deno.land/install.sh | sh"
    curl -fsSL https://deno.land/install.sh | sh
    return
  fi
  install_to_local_bin "$SRC" deno
  local DVER
  DVER=$("$HOME/.local/bin/deno" --version 2>/dev/null | head -1)
  say "deno -> ~/.local/bin/deno (${DVER})"
}

install_js_runtime() {
  if [ "$DENO_ONLY" = "1" ]; then install_deno; return; fi
  install_quickjs || install_deno
}

[ "$JS_ONLY" = "1" -o "$DENO_ONLY" = "1" ] && { install_js_runtime; exit 0; }

install_js_runtime

# ── 2. Install build dependencies (Debian/Ubuntu) ─────────────────────────────
if [ "$NO_DEPS" = "0" ]; then
  if have apt-get; then
    info "Installing build deps via apt (sudo)..."
    sudo_if apt-get update -y
    sudo_if apt-get install -y \
      mpv libmpv-dev libncurses5-dev libncursesw5-dev \
      libcurl4-openssl-dev libsqlite3-dev libxml2-dev libfmt-dev \
      nlohmann-json3-dev libqrencode-dev cmake ninja-build g++
    say "build deps installed"
  elif have pacman; then
    info "Installing build deps via pacman (sudo)..."
    sudo_if pacman -S --needed --noconfirm \
      mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc
  elif have dnf; then
    info "Installing build deps via dnf (sudo)..."
    sudo_if dnf install -y \
      mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel libxml2-devel fmt-devel \
      nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++
  else
    warn "No apt/pacman/dnf detected — skipping build-deps. Install them manually (see README)."
  fi
fi

# ── 3. Build panicast ─────────────────────────────────────────────────────────
info "Building panicast (Ninja, Release)..."
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
say "built build/panicast ($(./build/panicast --version 2>/dev/null | head -1))"

# ── 4. Install podradio ───────────────────────────────────────────────────────
if [ "$NO_SUDO" = "0" ] && have sudo; then
  sudo cmake --install build 2>/dev/null && say "installed -> /usr/local/bin/podradio" \
    || { warn "cmake --install failed, trying direct copy"; sudo cp -f build/panicast /usr/local/bin/podradio && say "installed -> /usr/local/bin/podradio"; }
else
  mkdir -p "$HOME/.local/bin"
  cp -f build/panicast "$HOME/.local/bin/podradio"
  say "installed -> ~/.local/bin/podradio (user-local)"
fi

echo
echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
echo -e "${GREEN} PaniCast ready.${NC}"
echo -e " Run: ${BLUE}podradio${NC}"
echo -e " YouTube playback needs a JS runtime on PATH for yt-dlp nsig solving:"
echo -e "   ${BLUE}qjs${NC} (quickjs-ng, ~2MB, recommended) or ${BLUE}deno${NC} (~106MB, fallback)."
echo -e " Set [youtube] ${BLUE}js_runtime${NC} in config.ini to quickjs (default) or deno."
echo -e " If 'qjs: command not found', run: ${BLUE}export PATH=\$HOME/.local/bin:\$PATH${NC}"
echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
