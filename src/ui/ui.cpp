// UI rendering layer implementation unit:
//   1) Terminal/signal lifecycle (save/restore terminal attributes, ncurses cleanup, signal handler registration);
//   2) Accommodates future out-of-line definitions and static data member definitions;
//   3) Included as a translation unit in CMakeLists to keep -Wweak-linkage and other checks consistent.
#include "panicast/ui/ui.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <ncurses.h>

namespace panicast
{

// =========================================================
// Terminal state save and restore
// =========================================================
namespace
{
struct termios g_original_termios;
bool g_termios_saved = false;
} // namespace

bool g_ncurses_initialized = false;
int g_original_lines = 0;
int g_original_cols = 0;

// Global exit request flag (used for SIGINT)
std::atomic<bool> g_exit_requested{false};
// Async-signal-safe flag (SIGSEGV/SIGABRT only set the flag; the main loop does cleanup).
// Also set for SIGHUP/SIGTERM/SIGQUIT so the main loop can flush state and exit without a confirm popup.
volatile sig_atomic_t g_crash_sig{0};
// N04-fix: count termination signals; a 2nd one means graceful cleanup is stuck (e.g. a worker
//   blocked in a 600s yt-dlp call) → force _exit so `pkill panicast` run again kills it instantly.
static std::atomic<int> g_term_sig_count{0};

// Save original terminal attributes
void save_terminal_state() {
    if (tcgetattr(STDIN_FILENO, &g_original_termios) == 0) {
        g_termios_saved = true;
    }
    // Save original terminal size
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        g_original_lines = ws.ws_row;
        g_original_cols = ws.ws_col;
    }
}

// Restore original terminal attributes
static void restore_terminal_state() {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
        // NOTE: do NOT clear g_termios_saved. tui_cleanup runs again at atexit (after ~App), and the
        //   destructor's teardown (remote/transcription/kill/pool) or terminal-specific behavior
        //   (WSL2/WSLg + mpv VO window close) can alter the line discipline after run()'s restore;
        //   re-tcsetattr on every call corrects it so the shell gets the original termios.
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
}

// Async-signal-safe termios restore for the force-exit signal paths. tcsetattr() is NOT
// POSIX async-signal-safe; ioctl(TCSETS) is the underlying syscall and works on Linux inside a
// signal handler. Without this, a forced _exit (2nd SIGINT, or a crash signal) leaves the line
// discipline in cbreak/raw mode → the shell then echoes ESC as "^[" and Ctrl+C as "^C" (ISIG off),
// and the terminal feels broken. RIS (\033c) only resets the terminal's internal state, NOT the
// kernel line discipline — so termios must be restored explicitly here.
static void restore_termios_async() {
    if (g_termios_saved) {
        ioctl(STDIN_FILENO, TCSETS, &g_original_termios);
        tcflush(STDIN_FILENO, TCIFLUSH); // discard typeahead so it doesn't carry to the shell
    }
}

// Terminal cleanup function (called by atexit and signal handlers)
void tui_cleanup() {
    static std::atomic<bool> cleaned{false}; // Idempotent for endwin (the costly ncurses call)
    bool first = !cleaned.exchange(true);
    if (first && g_ncurses_initialized) {
        endwin(); // ncurses standard: exits alt-screen + restores cursor. Don't add manual escapes.
        g_ncurses_initialized = false;
    }
    // Restore termios (every call, incl. atexit after ~App) + drain typeahead. NO manual cursor
    //   escapes or newlines — those fight bash's own prompt redraw (cursor glued, no clean prompt).
    //   endwin handled the screen; bash handles the prompt placement naturally.
    restore_terminal_state();
    fflush(stdout);
    fflush(stderr);
}

// Signal handler function
static void signal_handler(int sig) {
    // SIGHUP = controlling terminal closed (user closed the window / SSH dropped). The TUI is gone,
    //   so die IMMEDIATELY. We must NOT just set a flag here: the flag is checked by the main loop,
    //   and if the main thread is blocked in a long operation (heavy feed parse, a join, etc.) the
    //   flag is never checked and the process lingers — holding the wlshm video window open and
    //   forcing the user to kill it manually. Restore the line discipline + leave alt-screen, then
    //   _exit (all async-signal-safe). Terminal-close → process always dies.
    if (sig == SIGHUP) {
        restore_termios_async();
        const char restore[] = "\033[?1049l"; // leave alt-screen; bash redraws the prompt
        (void)write(STDERR_FILENO, restore, sizeof(restore) - 1);
        _exit(128 + sig);
    }
    // N04-fix: a 2nd termination signal means graceful cleanup is stuck (worker blocked in
    //   yt-dlp/whisper). Restore the terminal and force _exit so a repeated `pkill panicast`
    //   kills the process immediately instead of hanging in pool_.shutdown()'s join.
    if (sig != SIGSEGV && sig != SIGABRT) {
        if (++g_term_sig_count >= 2) {
            restore_termios_async(); // restore line discipline before _exit (escape seqs alone can't)
            // Exit alt-screen only — let bash handle the cursor/prompt naturally (no manual cursor
            //   forcing or newlines that fight bash's prompt redraw).
            const char restore[] = "\033[?1049l";
            (void)write(STDERR_FILENO, restore, sizeof(restore) - 1);
            _exit(128 + sig); // async-signal-safe; 128+sig is the conventional exit code
        }
    }
    if (sig == SIGINT) {
        // CTRL+C does not exit directly; set the flag and let the main loop handle it
        g_exit_requested = true;
        return;
    }
    if (sig == SIGSEGV || sig == SIGABRT) {
        // Crash signals only set the flag; do not call any non-async-signal-safe functions
        g_crash_sig = sig;
        restore_termios_async(); // restore line discipline before the crash/core-dump
        // Exit alt-screen only — let bash handle the cursor/prompt naturally.
        const char restore[] = "\033[?1049l";
        (void)write(STDERR_FILENO, restore, sizeof(restore) - 1);
        // Restore the default handler and re-raise so the system generates a core dump
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    // SIGTERM/SIGQUIT: likewise only set the flag; the main loop does unified cleanup on exit
    g_exit_requested = true;
    g_crash_sig = sig;
}

// Register signal handlers
void setup_signal_handlers() {
    // SIGHUP = terminal closed / SSH disconnected: must flush & exit (default action would terminate
    //   immediately, losing in-flight YouTube parse cache and any unflushed state).
    int sigs[] = {SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGSEGV, SIGABRT};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(int); i++) {
        struct sigaction sa;
        sa.sa_handler =
            signal_handler; // sigaction has well-defined semantics (avoids SYSV/BSD differences of signal())
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(sigs[i], &sa, nullptr);
    }
}

void UI::init(float ratio) {
    (void)ratio; // P3 (Y23.7): layout_ratio_ removed (LayoutGuard reads INI directly)
    //Get terminal size immediately, before any terminal operation
    // This is the key to fixing UI sizing issues in SSH/TTY environments
    int term_rows = 0, term_cols = 0;
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_rows = ws.ws_row;
        term_cols = ws.ws_col;
    }
    // Fallback: read from environment variables
    if (term_rows <= 0 || term_cols <= 0) {
        const char *lines_env = std::getenv("LINES");
        const char *cols_env = std::getenv("COLUMNS");
        if (lines_env)
            term_rows = std::atoi(lines_env);
        if (cols_env)
            term_cols = std::atoi(cols_env);
    }
    // Use the global saved value if it is larger
    if (g_original_lines > term_rows)
        term_rows = g_original_lines;
    if (g_original_cols > term_cols)
        term_cols = g_original_cols;

    //Set locale (no terminal output, log only)
    // Key fix: do not dup2-redirect around setlocale, to avoid breaking the UTF-8 environment
    // WSL2/Debian minimal images often lack a UTF-8 locale; when setlocale falls back to
    //   C/POSIX the wide-char library still garbles. Here we respect the environment locale
    //   first; if the codeset is not UTF-8, fall back to C.UTF-8 (built into glibc, no
    //   locale-gen needed, always available on WSL2/containers).
    //   We do not unconditionally force C.UTF-8 — zh_CN/en_US.UTF-8 users keep their %b
    //   month name/number formatting unchanged.
    char *locale_result = setlocale(LC_ALL, "");
    auto codeset_is_utf8 = [](const char *cs) -> bool {
        if (!cs)
            return false;
        std::string s(cs);
        for (auto &ch : s)
            ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
        return s.find("UTF-8") != std::string::npos || // nl_langinfo usually returns "UTF-8"
               s.find("UTF8") != std::string::npos;    // tolerate "UTF8" spelling
    };
    const char *codeset = locale_result ? nl_langinfo(CODESET) : nullptr;
    if (!codeset_is_utf8(codeset)) {
        // Environment is not UTF-8: fall back to C.UTF-8 (only this branch changes behavior; the original locale was already garbled)
        char *fb = setlocale(LC_ALL, "C.UTF-8");
        if (fb) {
            locale_result = fb;
            codeset = nl_langinfo(CODESET);
        }
    }
    if (locale_result) {
        LOG(fmt::format("[INIT] Locale set to: {} (codeset={})", locale_result,
                        codeset ? codeset : "unknown"));
    }
    if (!codeset_is_utf8(codeset)) {
        // Still not UTF-8 (very old glibc without C.UTF-8 and no generated locale): wide chars will garble
        const char *msg =
            "PaniCast: terminal is not using a UTF-8 locale; CJK/Emoji will be garbled.\n"
            "  Fix (any one of):\n"
            "    1) Set LANG=en_US.UTF-8 or LANG=C.UTF-8 in ~/.bashrc or /etc/locale.conf\n"
            "    2) Run sudo dpkg-reconfigure locales and enable en_US.UTF-8 / zh_CN.UTF-8\n"
            "    3) Temporary: LANG=C.UTF-8 panicast\n";
        fputs(msg, stderr);
        LOG(fmt::format("[INIT] WARNING: non-UTF-8 locale (codeset={}), CJK/Emoji will garble",
                        codeset ? codeset : "unknown"));
    }

    //Initialize ncurses
    initscr();
    g_ncurses_initialized = true;

    //Force the correct terminal size
    if (term_rows > 0 && term_cols > 0) {
        resizeterm(term_rows, term_cols);
        LINES = term_rows;
        COLS = term_cols;
    }
    wrefresh(stdscr);

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(
        30); //30ms poll (≈33FPS) — snappy input response; heavy work is on pool_ (10 workers), not the main loop
    // Enable mouse: left-click to select and expand/play node, wheel to page up/down
    //   Register only button events (not REPORT_MOUSE_POSITION, to avoid event flooding).
    mousemask(BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
    start_color();
    use_default_colors();

    for (int i = 0; i < 256; ++i)
        init_pair(i + 1, i, -1);

    // Node-tree state color pairs: foreground from [colors] config (default if missing), black background

    getmaxyx(stdscr, h, w);
    //Use integer arithmetic to avoid floating-point precision issues
    // F31: use LayoutGuard::compute (reads INI layout_ratio) — consistent with handle_resize().
    //   Previously hardcoded w*40/100, ignoring INI layout_ratio on the first render.
    LayoutDims dims = LayoutGuard::compute(w, h);
    left_w = dims.left_w;
    right_w = dims.right_w;
    top_h = dims.top_h;

    // Bounds protection: ensure minimum window size
    if (left_w < 10)
        left_w = 10;
    if (right_w < 10)
        right_w = 10;
    if (top_h < 5)
        top_h = 5;

    left_win = newwin(top_h, left_w, 0, 0);
    right_win = newwin(top_h, right_w, 0, left_w);
    status_win = newwin(3, w, top_h, 0);
    lyric_win = newwin(lyric_bar_height_, w, top_h, 0); // Y24: full-width LYRIC bar

    //Null-pointer check to prevent crashes
    if (!left_win || !right_win || !status_win || !lyric_win) {
        endwin();
        fprintf(stderr, "Error: Failed to create windows\n");
        exit(1);
    }

    //Disable window auto-wrap and scrolling to prevent content overflow
    scrollok(left_win, FALSE);
    scrollok(right_win, FALSE);
    scrollok(status_win, FALSE);
    scrollok(lyric_win, FALSE);
    immedok(left_win, FALSE);
    immedok(right_win, FALSE);
    immedok(status_win, FALSE);
    immedok(lyric_win, FALSE);

    ThemeManager::instance().statusbar_config() =
        IniConfig::instance().get_statusbar_color_config();
    show_tree_lines_ = IniConfig::instance().get_bool("display", "show_tree_lines", true);
    url_hyperlink_ =
        IniConfig::instance().get_bool("display", "url_hyperlink", true); // Y24.27: cache once
    scroll_mode_ = IniConfig::instance().get_bool("display", "scroll_mode", false); //default false
    // Y24: L-mode (LYRIC bar) — user intent persisted; active is derived per-frame by App.
    lyric_bar_requested_ = IniConfig::instance().get_display_lyric_bar();
    lyric_bar_height_ = IniConfig::instance().get_display_lyric_bar_height();
    if (lyric_bar_height_ < 4)
        lyric_bar_height_ = 4; // need ≥3 lyric lines + 2 borders
    if (lyric_bar_height_ > 12)
        lyric_bar_height_ = 12;
    lyric_bar_lines_ = lyric_bar_height_ - 2; // lyric content rows (excluding top+bottom borders)
    // Whether border title uses Emoji. Default false (ASCII, safe everywhere).
    //   When set to true, the left panel top-border title uses an Emoji badge; the border
    //   switches to a "full ─ base + title overlaid" drawing style, so emoji width mismatch
    //   with the terminal never produces gaps. Requires an emoji font in the terminal,
    //   otherwise emoji render as tofu (a terminal font limitation, not fixable in code).
    title_emoji_ = IniConfig::instance().get_bool("display", "title_emoji", true);

    // Load icon style config (emoji|ascii|auto) + emoji width probing
    {
        // 1. Read INI emoji_width override (0=auto, 1/2=forced)
        int ew_override = IniConfig::instance().get_int("display", "emoji_width", 0);
        if (ew_override == 1 || ew_override == 2) {
            g_emoji_width_override = ew_override;
            LOG(fmt::format("[UI] Emoji width override: {} (from INI)", ew_override));
        }

        // 2. If not overridden, run cursor probing (unconditionally, no longer WT-only)
        //    WT_SESSION is not forwarded over SSH, so only cursor probing can detect narrow-emoji terminals.
        if (g_emoji_width_override == 0 && g_emoji_width_probed == 0) {
            int probed = probe_emoji_width();
            if (probed > 0) {
                g_emoji_width_probed = probed;
                LOG(fmt::format("[UI] Emoji width probed: {} (cursor detection)", probed));
            }
        }

        // 3. Load icon style
        std::string style = IniConfig::instance().get("display", "icon_style", "auto");
        if (style == "emoji") {
            IconManager::set_style(IconStyle::EMOJI);
        } else if (style == "ascii") {
            IconManager::set_style(IconStyle::ASCII);
        } else {
            // auto: driven by cursor-probe result (not env vars), ASCII-first conservative
            //   probe=1 -> narrow-emoji terminal (Windows Terminal, detectable over SSH too) -> ASCII
            //   probe=2 -> wide-emoji terminal (Linux xterm/gnome etc.) -> Emoji
            //   probe=0 -> probe failed/no response/non-interactive -> ASCII (safe, avoids tofu/misalignment)
            //   Note: border title is already fixed to ASCII (see mode_str); this only affects list-item icons.
            int probed = g_emoji_width_probed;
            if (probed == 2) {
                IconManager::set_style(IconStyle::EMOJI);
                LOG(fmt::format("[UI] Auto icon style: Emoji (probed emoji width=2, terminal={})",
                                TerminalDetector::terminal_name()));
            } else {
                IconManager::set_style(IconStyle::ASCII);
                LOG(fmt::format("[UI] Auto icon style: ASCII (probed={}, terminal={})", probed,
                                TerminalDetector::terminal_name()));
            }
        }

        // 4. Log the final emoji-width decision (for debugging)
        LOG(fmt::format("[UI] Emoji width final: {} (override={}, probed={}, recommended={})",
                        get_emoji_width(), g_emoji_width_override, g_emoji_width_probed,
                        TerminalDetector::recommended_emoji_width()));
    }

    // Load persisted theme index and apply (15 schemes; default 0=Dark if missing)
    {
        ThemeManager::instance().init(); // Y24.30: theme + statusbar config
    }

    CacheManager::instance().load();
}

void UI::cleanup() {
    delwin(left_win);
    delwin(right_win);
    delwin(status_win);
    delwin(lyric_win);
    //Use the global cleanup function to ensure cursor and terminal attributes are restored
    tui_cleanup();
}

void UI::handle_resize() {
    // Zero cached size so size_changed in draw() evaluates to true
    left_w = right_w = top_h = 0;
    cols_ = 0;
    last_lyric_bar_active_ = !lyric_bar_active_; // Y24: force re-detect → full redraw
    // Use wnoutrefresh instead of wrefresh: avoids a standalone physical refresh causing a blank flicker frame;
    // the actual clear is done uniformly by draw()'s clearok(stdscr,TRUE)+doupdate.
    werase(stdscr);
    wnoutrefresh(stdscr);
}

void UI::toggle_tree_lines() {
    show_tree_lines_ = !show_tree_lines_;
    EVENT_LOG(fmt::format("Tree lines: {}", show_tree_lines_ ? "ON" : "OFF"));
}

void UI::set_transcript(const std::vector<TranscriptSegment> &segs, const std::string &url) {
    current_transcript_ = segs;
    current_transcript_url_ = url;
    lyric_history_.clear(); // new track → fresh lyric history
}

void UI::toggle_scroll_mode() {
    scroll_mode_ = !scroll_mode_;
    EVENT_LOG(fmt::format("Scroll mode: {}", scroll_mode_ ? "ON" : "OFF"));
}

void UI::toggle_lyric_bar() {
    lyric_bar_requested_ = !lyric_bar_requested_;
    IniConfig::instance().set("display", "lyric_bar", lyric_bar_requested_ ? "true" : "false");
    EVENT_LOG(fmt::format("LYRIC bar: {}", lyric_bar_requested_ ? "requested" : "off"));
}

void UI::set_lyric_bar_requested(bool requested) {
    if (lyric_bar_requested_ == requested)
        return;
    lyric_bar_requested_ = requested;
    IniConfig::instance().set("display", "lyric_bar", requested ? "true" : "false");
    EVENT_LOG(fmt::format("LYRIC bar: {}", requested ? "requested" : "off"));
}

void UI::draw(
    AppMode mode, const std::vector<DisplayItem> &list, int selected,
    const MPVController::State &state, int view_start, AppState app_state,
    TreeNodePtr playback_node, int marked_count, const std::string &search_query, int current_match,
    int total_matches, TreeNodePtr selected_node, const std::vector<DownloadProgress> &downloads,
    bool visual_mode, int visual_start, const std::vector<PlaylistItem> &playlist,
    int playlist_index, //Play mode + INFO play-context (7-line: 3 history + current + 3 next)
    PlayMode play_mode, const std::vector<std::string> &history_titles,
    const std::vector<int> &next_indices, const DisplayContext &dctx) {
    getmaxyx(stdscr, h, w);
    // NULL window guard: safe_wresize may null the window on failure; skip this frame to avoid dereferencing NULL
    if (!left_win || !right_win || !status_win || !lyric_win)
        return;
    // Use LayoutGuard::compute for unified clamping (eliminates init/draw inconsistency)
    // Old code did new_top_h = h - 3 directly; on small terminals this became 0/negative, causing draw_box to return early and borders to disappear
    LayoutDims dims = LayoutGuard::compute(w, h);
    int new_left_w = dims.left_w;
    int new_right_w = dims.right_w;

    // Y24: bottom strip = LYRIC bar (when active) or status bar (otherwise). When the LYRIC
    //   bar is active the status bar is hidden (space efficiency) and the top area shrinks by
    //   lyric_bar_height_. Fall back to the status bar if the terminal is too short for the bar.
    bool lyric_active = lyric_bar_active_ && (h >= 16);
    int bottom_h = lyric_active ? lyric_bar_height_ : LayoutGuard::STATUS_H;
    int new_top_h = h - bottom_h;
    if (new_top_h < 5) {
        new_top_h = 5;
    } // match existing min guard

    //Detect whether window size changed (incl. L-mode active↔inactive transition → full redraw)
    bool size_changed = (new_left_w != left_w || new_right_w != right_w || new_top_h != top_h ||
                         w != cols_ || lyric_active != last_lyric_bar_active_);

    if (size_changed) {
        //When window size changes, first clear all window contents
        // This avoids old border characters remaining on screen
        werase(left_win);
        werase(right_win);
        werase(status_win);
        werase(lyric_win);
        werase(stdscr);

        // Force the next-frame doupdate to clear and redraw the entire physical screen.
        //   After resize, ncurses's virtual screen may be out of sync with the terminal's
        //   physical screen; werase alone is insufficient.
        //   clearok(stdscr, TRUE) makes doupdate clear first then full-redraw,
        //   thoroughly eliminating leftover gaps/broken lines/open corners when dragging the window.
        //   Y24: also fires on L-mode toggle (top_h changes) to avoid buffer artifacts (garbled screen).
        clearok(stdscr, TRUE);

        // Update cached size
        left_w = new_left_w;
        right_w = new_right_w;
        top_h = new_top_h;
        cols_ = w;
        last_lyric_bar_active_ = lyric_active;
    }

    // Use LayoutGuard::safe_wresize; rebuild window on failure
    // Old code did not check wresize's return value; on failure the window size was inconsistent with the cache
    LayoutGuard::safe_wresize(left_win, top_h, left_w);
    LayoutGuard::safe_wresize(right_win, top_h, right_w);
    mvwin(right_win, 0, left_w);
    // Y24: resize BOTH bottom windows so the inactive one is cleared/sized away cleanly;
    //   only the active one is drawn below. mvwin places whichever is active at y=top_h.
    LayoutGuard::safe_wresize(status_win, LayoutGuard::STATUS_H, w);
    LayoutGuard::safe_wresize(lyric_win, lyric_bar_height_, w);
    if (lyric_active) {
        mvwin(lyric_win, top_h, 0);
        werase(status_win); // keep the hidden status bar clear (no stale content)
    } else {
        mvwin(status_win, top_h, 0);
        werase(lyric_win); // keep the hidden lyric bar clear
    }

    //More conservatively compute the title-bar net width, protecting the top-right border
    // Top-right needs to reserve: ┐ and ─, 2 columns total
    int title_max =
        left_w - 6; // left border 1 + space 1 + title + space 1 + top-right protection 3
    if (title_max < 5)
        title_max = 5; // Minimum reservation

    std::string mode_str;
    // Title Emoji is enabled only when "terminal measured width (probed)==2".
    //   Root cause: ncurses advances its internal cursor per glibc wcwidth; the glibc width
    //   of title emoji must match the terminal's actual render width, otherwise the cursor
    //   misaligns -> subsequent corners/borders on the same line are drawn at wrong positions
    //   -> verticals and corners don't line up. The user's terminal probed=2, so only emoji
    //   with glibc wcwidth=2 are used (📻🎤💖📜🔍).
    //   Note: 🎙(U+1F399) has glibc=1 but terminal renders as 2 -> would misalign; replaced
    //   with 🎤(U+1F3A4, glibc=2).
    //   When probed!=2 (narrow-emoji terminal / not probed), fall back to ASCII to keep borders aligned.
    const bool use_emoji_title =
        title_emoji_ && (g_emoji_width_probed == 2 || g_emoji_width_override == 2);
    switch (mode) {
    case AppMode::RADIO:
        mode_str = use_emoji_title ? "📻 RADIO" : "[R] RADIO";
        break;
    case AppMode::PODCAST:
        mode_str = use_emoji_title ? "🎤 PODCAST" : "[P] PODCAST";
        break;
    case AppMode::FAVOURITE:
        mode_str = use_emoji_title ? "💖 FAVOURITE" : "[F] FAVOURITE";
        break;
    case AppMode::HISTORY:
        mode_str = use_emoji_title ? "📜 HISTORY" : "[H] HISTORY";
        break; //new
    case AppMode::ONLINE:
        mode_str =
            use_emoji_title
                ? fmt::format("🔍 ONLINE [{}]", dctx.online_region_name)
                : fmt::format("[O] ONLINE [{}]", dctx.online_region_name);
        break;
    case AppMode::ACCOUNT:
        mode_str = use_emoji_title ? "Ｙ ACCOUNT" : "[Y] ACCOUNT";
        break; // Y01 (fullwidth Y: glibc wcwidth=2)
    case AppMode::BILIBILI:
        mode_str = use_emoji_title ? "Ｂ BILIBILI" : "[B] BILIBILI";
        break; // Y15 (fullwidth B: glibc wcwidth=2)
    case AppMode::TIKTOK: {
        const std::string &r = dctx.tiktok_region;
        // Y24.13: CN region = Douyin (douyin.com); show Douyin in the title.
        if (r == "CN")
            mode_str = use_emoji_title ? fmt::format("🎵 Douyin [{}]", r)
                                       : fmt::format("[T] Douyin [{}]", r);
        else
            mode_str = use_emoji_title ? fmt::format("🎵 TIKTOK [{}]", r)
                                       : fmt::format("[T] TIKTOK [{}]", r);
        break;
    }
    case AppMode::IPTV:
        mode_str = use_emoji_title ? "📺 IPTV" : "[I] IPTV";
        break; // Y24.50
    }
    if (visual_mode)
        mode_str = use_emoji_title ? "§ VISUAL §" : "[V] VISUAL";
    if (marked_count > 0)
        mode_str += use_emoji_title ? " [🔖 x" + std::to_string(marked_count) + "]"
                                    : " [* x" + std::to_string(marked_count) + "]";
    // V2.39-FF: T and S status indicators. Y24.11: T freed for TikTok mode — tree lines are
    //   now always ON (no keybind), so only the S (scroll) indicator remains.
    if (scroll_mode_)
        mode_str += " [S]";

    std::string title_display = Utils::truncate_by_display_width(mode_str, title_max);

    //Incremental redraw - do not call draw_box here

    //Window structure (after box()):
    //   column 0 = left border │
    //   column 1 to column left_w-2 = content area (width = left_w-2)
    //   column left_w-1 = right border │

    //Make full use of the content area width
    //   content area width = left_w - 2
    //   no longer reserve a right-side space for protection; content sits flush against the right border
    //   tree_line_width = left_w - 2

    // Note: content starts printing at column 1
    int tree_line_width = left_w - 2;
    if (tree_line_width < 10)
        tree_line_width = 10; // Minimum width protection

    // Full redraw of the left panel each frame. Incremental redraw has been removed — needs_full_redraw
    // always returns true; its else branch and the last_selected_idx_/last_view_start_/last_mode_/last_display_size_
    // tracking members are write-only dead code.
    int line_num = 1;
    // Full redraw: clear the window and draw all rows. The border is drawn uniformly by protect_border(-1) below.
    werase(left_win);

    for (int i = view_start; i < (int)list.size() && line_num < top_h - 1; ++i) {
        bool is_sel = (i == selected);
        bool in_visual =
            visual_mode && visual_start >= 0 &&
            ((visual_start <= i && i <= selected) || (selected <= i && i <= visual_start));
        draw_line(left_win, line_num, list[i], is_sel, in_visual, tree_line_width,
                  state.current_url);
        line_num++;
    }

    // Draw the full border first (the whole top border as HLINE), then overlay the title on top.
    //   Key point: the top border has a complete ─ base first, and the title only overwrites the
    //   cells it occupies — regardless of whether the emoji width matches the terminal, no gap can
    //   appear (each cell is either ─ or a title character, no empty holes). Emoji width is provided
    //   by probe and is only used for the truncation layout in truncate_by_display_width above.
    //   Combined with per-frame redraw, toggling [T]/[S] leaves no residue. This drawing approach
    //   works for both ASCII/Emoji titles; both go through this unified path.
    protect_border(left_win, left_w, top_h,
                   -1); // -1 = draw the whole top border as HLINE (no skip)
    mvwprintw(left_win, 0, 2, " %s ", title_display.c_str());
    mvwaddch(left_win, 0, left_w - 1,
             ACS_URCORNER); // top-right corner protection (redrawn when title approaches)
    mvwaddch(left_win, 0, left_w - 2, ACS_HLINE);

    wnoutrefresh(left_win); // double-buffer optimization

    // Right Panel
    werase(
        right_win); // just clear; the border is drawn uniformly by protect_border at the end of draw_info
    mvwprintw(right_win, 0, 2, " INFO & LOG ");

    //Right panel structure: column 0 = left border, columns 1~right_w-2 = content area, column right_w-1 = right border
    //content area width = window_width - 3
    int right_inner_width = getmaxx(right_win) - 3;

    //Pass playlist data
    pending_osc8_.clear(); // Y24.12: reset per-frame OSC 8 link list before draw_info populates it
    draw_info(right_win, state, app_state, playback_node, marked_count, search_query, current_match,
              total_matches, selected_node, downloads, visual_mode, right_inner_width, playlist,
              playlist_index, play_mode, history_titles, next_indices);

    // draw_info already used protect_border(2,14,split_y,2,13) to draw the right panel's full border
    // (including the separator line); not redrawn here (previously caused the border to be drawn 3 times per frame).
    wnoutrefresh(right_win); // double-buffer optimization

    // Bottom strip: Y24 — LYRIC bar when L-mode is active (status bar hidden),
    //   otherwise the normal status bar.
    if (lyric_bar_active_ && (h >= 16)) {
        draw_lyric_bar(lyric_win, state);
        wnoutrefresh(lyric_win);
    } else {
        draw_status(status_win, state, selected_node, dctx);
        wnoutrefresh(status_win); // double-buffer optimization
    }

    //Double-buffer refresh - update all windows at once
    doupdate();
    // Y24.12: emit OSC 8 hyperlinks AFTER doupdate so ncurses has written the visible URL
    //   text first; the link markers (zero-width) are then layered on top and survive the
    //   incremental refresh (cell content matches → ncurses won't rewrite). Re-done per frame.
    if (!pending_osc8_.empty()) {
        // Y24.13: emit_osc8_link() moves the physical cursor (CUP + visible text), which
        //   desyncs ncurses' relative-cursor optimization → next doupdate garbles panels.
        //   Save ncurses' assumed cursor (curscr) before, restore it after via raw CUP.
        int sy = 0, sx = 0;
        getyx(curscr, sy, sx);
        for (const auto &e : pending_osc8_)
            Utils::emit_osc8_link(e.row, e.col, e.visible, e.url, e.id);
        Utils::emit_cup(sy + 1, sx + 1); // re-sync (CUP is 1-based)
        pending_osc8_.clear();
    }
}

void UI::draw_help(WINDOW *win, const MPVController::State &state, int cw) {
    (void)win;
    (void)state;
    (void)cw; // keep params for future use
    //Help content definition, including all key definitions
    //Update hotkey descriptions
    static const std::vector<std::string> help_lines = {
        fmt::format("{} {}★ - Help", APP_NAME, VERSION),
        "",
        "---- Navigation ----",
        "  k / j      Move up/down",
        "  g / G      Go to first/last item",
        "  PgUp/PgDn  Page up/down",
        "  h          Collapse/Go back",
        "  l / Enter  Expand/Play (marked: add to playlist)",
        "",
        "---- Playback ----",
        "  Space / p  Play/Pause",
        "  - / +      Volume down / up",
        "  [ / ]      Speed slower/faster",
        "  \\          Reset speed to 1.0x",
        "",
        "---- Video/Audio Quality (INI) ----",
        "  play_format_video  = bestvideo[height<=1080]+bestaudio/best (1080p DASH, default)",
        "  play_format_audio  = bestaudio/best (highest audio, default)",
        "  state_refresh_ms   = 100 (playback-state/Network refresh interval, ms)",
        "",
        "---- Playlist ----",
        "  (implicit) The peers (siblings) of the playing episode are the playlist; auto-advance "
        "per play mode",
        "  C          Clear playlist (keep playing) — stop auto-advance, current track keeps "
        "playing",
        "  L          LYRIC bar toggle; in F mode → transcribe selected/v-marked "
        "(whisper.cpp→.srt)",
        "  Ctrl+L     Cycle theme (22 palettes: Solarized "
        "Dark/Gruvbox/Nord/Dracula/Catppuccin/...)",
        "  Ctrl+N     Configure network proxy (socks5h:// / http:// ...)",
        "  Ctrl+B     Set cookies.txt (context: YouTube / Bilibili / TikTok-Douyin)",
        "",
        "---- Actions ----",
        "  a          Add feed (PODCAST) / Subscribe (ONLINE) / Add local folder (FAVOURITE)",
        "  d          Delete node/record (all modes)",
        "  D          Download (single, or batch if marks set)",
        "  b          Switch region (ONLINE/TIKTOK mode)",
        "  e          Edit node title/URL",
        "  f          Add to Favourites",
        "  m          Toggle mark",
        "  v          Enter Visual mode",
        "  V          Clear all marks",
        "  C          Clear playlist (keep playing)",
        "  r          Refresh node",
        "  o          Toggle sort order (asc/desc)",
        "",
        "---- Modes ----",
        "  R          Radio mode",
        "  P          Podcast mode",
        "  F          Favourite mode",
        "  H          History mode",
        "  O          Online (iTunes) mode",
        "  Y          Account (Google) mode  [Y01]",
        "  B          Bilibili mode  [Y15]",
        "  T          TikTok / Douyin mode  [Y24.11]",
        "  M          Cycle through all modes",
        "",
        "---- UI Settings ----",
        "  S          Toggle scroll mode",
        "  U          Toggle icon style (ASCII/Emoji)",
        "  Ctrl+Y     Copy playing stream URL to clipboard (cursor node if idle)",
        "",
        "---- Search ----",
        "  /          Search (local) / Search iTunes (ONLINE) / Search YouTube (Y mode)",
        "             Y-mode YouTube search: c/v/p/m prefix filters channel/video/playlist/music",
        "  J / K      Jump to next/prev match",
        "",
        "---- Y Mode (Google Accounts) [Y01] ----",
        "  a          Login (scan QR) / Subscribe [C] channel result to active account",
        "  A          Login ANOTHER Google account",
        "  l / Enter  Activate account / expand History/Subscriptions/Search",
        "  j / k      Select account",
        "  r          Re-sync account / refresh Subs·History node",
        "  d          Delete selected account",
        "  :secret    Import Google client_secret.json into data dir (Y-mode OAuth client)",
        "  (active account = primary, default 1#; expand another to switch)",
        "",
        "---- Playback ----",
        "  l / Enter  Play selected episode (its peers become the list)",
        "  Space / p  Pause / resume",
        "  :          Command window — play mode (≥2-char prefix) or an mpv hotkey (single char)",
        "    :re/:sh/:cy or :repeat/:shuffle/:cycle (≥2 chars, prefix match)",
        "    mpv hotkeys (aligned to mpv native; type the char after `:`):",
        "      video:  + zoom in | - zoom out | = reset zoom | f fullscreen | A aspect | d "
        "deinterlace",
        "      sub:    F/G size -/+ | z/Z sync -/+ | r/R pos up/down | v hide-show | j/J track "
        "next/prev",
        "      audio:  # cycle track | m mute",
        "      osd:    o progress | O level | i stats | I stats-toggle",
        "      other:  l ab-loop | s screenshot | S screenshot(no sub) | 1-8 "
        "contrast/bright/gamma/sat",
        "  INFO area shows: 3 history ^ / current / 3 next v  +  Network: speed | Buffering: sec",
        "",
        "---- B Mode (Bilibili) [Y15] ----",
        "  a          Login (QR scan → SESSDATA cookie, like Y mode OAuth)",
        "  A          Login another Bilibili account (QR scan)",
        "  Ctrl+B     Set cookie file path (context-aware, same as Y mode)",
        "  l / Enter  Expand account (following list, WBI API) / UP master (video list) / play",
        "  /          Search Bilibili (yt-dlp bilisearch)",
        "  r          Re-fetch followings / history / account",
        "  (QR=cookie; followings need cookie; video list uses WBI; HD playback needs cookie)",
        "  (no quickjs/deno needed — Bilibili has no nsig)",
        "",
        "---- T Mode (TikTok / Douyin) [Y24.11/16] ----",
        "  T          Enter T mode (region shown in the status-bar border, not the root)",
        "  a          Add: @user / tiktok.com video URL / douyin.com video URL",
        "             TikTok video URL → auto-subscribe its @user (expand → all their videos)",
        "             Douyin video URL → saved as a playable leaf (Douyin can't list users)",
        "  /          Open: @user / #tag / URL  (NO keyword search — anonymous infeasible)",
        "             #tag → yt-dlp tag list (dormant; yt-dlp tiktok:tag disabled upstream)",
        "  b          Cycle region: US/JP/GB/DE/FR/KR/ID/TH/VN/MY/BR/MX/CN (CN=Douyin)",
        "  l / Enter  Expand creator → video list (yt-dlp) / play video",
        "  r          Re-fetch creator video list (replace local cache)",
        "  d          Delete the item under the cursor",
        "  Ctrl+B     Import cookie (CN→douyin; else tiktok) — Netscape cookies.txt",
        "  (TikTok anonymous; Douyin needs cookies.txt + CN network exit)",
        "  (playback via mpv ytdl_hook; yt-dlp has no DouyinUserIE so Douyin is single-video only)",
        "",
        "---- Command Line ----",
        "  -a <url>   Add feed from URL",
        "  -i <file>  Import OPML subscriptions",
        "  -e <file>  Export podcasts to OPML",
        "  -t <time>  Sleep timer (5h/30m/1:25:15)",
        "  --purge    Clear cache (preserves subscriptions/history/favourites)",
        "  --quiet    Pure audio mode (vo=null, vid=no)",
        "  --vid <val> Override video track (auto/no)",
        "  --vo <val> Override video output (auto/null/gpu/wlshm)",
        "  --ao <val> Override audio output (default pulse,alsa; or pulse/alsa/pipewire/auto)",
        "",
        "---- Data Storage ----",
        "  Database: ~/.local/share/panicast/panicast.db",
        "  Config:   ~/.config/panicast/config.ini",
        "  Downloads: ~/Downloads/PaniCast/",
        "  Log:      ~/.local/share/panicast/panicast-YYYYMMDD.log (daily, kept 365 days)",
        "",
        "  Note: All data in SQLite database.",
        "",
        "---- Contact ----",
        "  Email:  Deadship2003@gmail.com",
        "",
        "Press 'q' or '?' to close"};

    // V0.03: compute the required window size
    int content_height = help_lines.size();
    int content_width = 0;
    for (const auto &line : help_lines) {
        int w = Utils::utf8_display_width(line) + 4; // border + margin
        if (w > content_width)
            content_width = w;
    }

    // V0.03: limit max size to 90% of the screen
    int max_h = (int)(h * 0.9);
    int max_w = (int)(w * 0.9);
    int help_h = std::min(content_height + 2, max_h); // +2 for border
    int help_w = std::min(content_width, max_w);

    // Ensure minimum size
    if (help_h < 10)
        help_h = 10;
    if (help_w < 40)
        help_w = 40;

    int help_y = (h - help_h) / 2;
    int help_x = (w - help_w) / 2;
    if (help_y < 0)
        help_y =
            0; // when terminal is too short, stick to the top, to avoid newwin returning NULL on negative coords

    WINDOW *help_win = newwin(help_h, help_w, help_y, help_x);
    if (!help_win)
        return; // NULL guard: avoid box(NULL) crash
    keypad(help_win, TRUE);

    // Draw the border
    box(help_win, 0, 0);

    // V0.03: if content overflows the window, add scrolling
    int scroll_offset = 0;
    bool needs_scroll = content_height > help_h - 2;

    auto draw_content = [&]() {
        werase(help_win);
        box(help_win, 0, 0);
        if (scroll_offset < 0)
            scroll_offset = 0; // defensive clamp
        int y = 1;
        int visible_lines = help_h - 2;

        for (int i = scroll_offset; i < content_height && y < help_h - 1; ++i) {
            const std::string &line = help_lines[i];
            std::string display = Utils::truncate_by_display_width(line, help_w - 4);

            if (i == 0) {
                //Title centered
                int title_width = Utils::utf8_display_width(display);
                int x_pos = (help_w - title_width) / 2;
                if (x_pos < 2)
                    x_pos = 2;
                wattron(help_win, A_BOLD);
                mvwprintw(help_win, y++, x_pos, "%s", display.c_str());
                wattroff(help_win, A_BOLD);
            } else if (line.find("----") == 0) {
                // Section heading
                wattron(help_win, A_DIM);
                mvwprintw(help_win, y++, 2, "%s", display.c_str());
                wattroff(help_win, A_DIM);
            } else if (line == "Press 'q' or '?' to close") {
                // Bottom hint
                wattron(help_win, A_DIM);
                mvwprintw(help_win, y++, 2, "%s", display.c_str());
                wattroff(help_win, A_DIM);
            } else {
                mvwprintw(help_win, y++, 2, "%s", display.c_str());
            }
        }

        // V0.03: show scroll indicators
        if (needs_scroll) {
            if (scroll_offset > 0) {
                mvwprintw(help_win, 0, help_w - 6, "▲");
            }
            if (scroll_offset + visible_lines < content_height) {
                mvwprintw(help_win, help_h - 1, help_w - 6, "▼");
            }
        }

        wrefresh(help_win);
    };

    draw_content();

    // V0.03: scrolling key handling
    //Add g/G jump to top/bottom
    int ch;
    while ((ch = wgetch(help_win)) != 'q' && ch != '?' && ch != 27) {
        if (ch == 'k' || ch == KEY_UP) {
            if (scroll_offset > 0) {
                scroll_offset--;
                draw_content();
            }
        } else if (ch == 'j' || ch == KEY_DOWN) {
            if (scroll_offset + help_h - 2 < content_height) {
                scroll_offset++;
                draw_content();
            }
        } else if (ch == KEY_PPAGE) {
            scroll_offset = std::max(0, scroll_offset - 5);
            draw_content();
        } else if (ch == KEY_NPAGE) {
            // Only allow paging when scrolling is needed, and clamp the lower bound with std::max —
            //   the original code's content_height-help_h+2 went negative when content fit in the window,
            //   making scroll_offset negative and draw_content access help_lines with a negative index, crashing
            if (needs_scroll) {
                scroll_offset =
                    std::max(0, std::min(content_height - help_h + 2, scroll_offset + 5));
                draw_content();
            }
        } else if (ch == 'g') {
            //Jump to top
            scroll_offset = 0;
            draw_content();
        } else if (ch == 'G') {
            //Jump to bottom
            scroll_offset = std::max(0, content_height - help_h + 2);
            draw_content();
        }
    }

    delwin(help_win);
}

} // namespace panicast
