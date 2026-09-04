// panicastd — the headless daemon binary (N09/S1). Same engine as the TUI binary with a
//   NullFrontend instead of ncurses: playback, queue, subscriptions, downloads,
//   transcription and the LMS/PRP remote-control servers all run without a terminal —
//   Squeezer (or the web remote) is the UI. Managed by systemd (panicastd.service) or
//   the `panicast start/stop/restart/status` subcommands.
//
// Exit semantics: SIGTERM (systemctl stop / panicast stop) takes the existing
//   termination-signal path — flush caches, persist player state, exit immediately
//   (see App::check_exit_requests). The pid file lives at <data_dir>/panicastd.pid and
//   is removed on the way out (stale files are detected by kill(pid, 0) callers).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <unistd.h>

#include "panicast/app/app.h"
#include "panicast/core/paths.h"
#include "panicast/parsers/xml_helpers.h"
#include "panicast/ui/ui.h" // setup_signal_handlers / tui_cleanup (curses-guarded no-op here)

#if __has_include("version.h")
#include "version.h"
#endif

static std::string pidfile_path() {
    return panicast::Paths::get_data_dir() + "/panicastd.pid";
}

static void write_pidfile() {
    std::string p = pidfile_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
    std::ofstream f(p);
    if (f.is_open())
        f << getpid() << "\n";
}

static void remove_pidfile() {
    std::remove(pidfile_path().c_str());
}

int main() {
    using namespace panicast;

    // Same boot sequence as the TUI main (minus the terminal save — no terminal here).
    Paths::migrate_legacy();
    curl_global_init(CURL_GLOBAL_ALL);
    xmlInitParser();
    xmlSetGenericErrorFunc(NULL, xml_error_handler);
    xmlSetStructuredErrorFunc(NULL, (xmlStructuredErrorFunc)xml_structured_error_handler);

    setup_signal_handlers(); // SIGTERM/SIGHUP/SIGINT → the clean flush-and-exit path
    atexit(tui_cleanup);     // no-op until ncurses initializes (never does here)

    write_pidfile();
    int rc = 0;
    {
        App app;
        app.set_headless();
        app.set_exit_hook(remove_pidfile);
        try {
            app.run();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "panicastd: fatal: %s\n", e.what());
            rc = 1;
        } catch (...) {
            std::fprintf(stderr, "panicastd: fatal: unknown exception\n");
            rc = 1;
        }
    } // ~App joins everything before the pid file disappears
    remove_pidfile();

    curl_global_cleanup();
    xmlCleanupParser();
    return rc;
}
