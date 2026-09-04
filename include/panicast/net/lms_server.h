// mini-LMS server — Squeezer remote control (N08). Ships the accept-loop skeleton only:
//   it listens on the configured TCP port (LMS convention 9000), accepts HTTP/JSON-RPC
//   connections, and answers with a "phase-1 pending" JSON-RPC error. The Bayeux/JSON-RPC
//   control plane (handshake → long-poll → publish, single virtual player) lands in the
//   next N08 iteration; SlimProto (audio streaming) is a future optional extension and is
//   intentionally out of scope for this module.
//
// Compile layer: built only when CMake PANICAST_REMOTE_LMS=ON (default). Runtime layer:
//   still opt-in — [remote] lms_enable=false by default; nothing listens until the user
//   flips it. Singleton (not an App member) so the compile switch needs no #ifdef in app.h;
//   the only gated call sites live in app_run.cpp.
//
// Threading model (mirrors remote_server.h N01):
//   - start() spawns ONE accept thread.
//   - Each accepted connection runs on a tracked worker thread; no thread is detached;
//     stop() closes the listen fd to unblock accept(), then joins everything.
//   - Workers self-mark done via an atomic flag; the accept loop reaps finished workers.
//
// Platform: POSIX sockets (Linux / macOS).
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "panicast/net/remote_protocol.h"

namespace panicast
{

class LmsServer {
public:
    static LmsServer &instance();

    LmsServer(const LmsServer &) = delete;
    LmsServer &operator=(const LmsServer &) = delete;

    // Bind + listen + spawn the accept thread. `control` is the thread-safe playback state
    //   surface the future Bayeux command handlers will drive (same interface PRP/WS use).
    //   Returns true on success; on failure logs the error and leaves the server stopped.
    bool start(const std::string &bind_addr, int port, RemoteControlInterface *control);
    void stop();

    bool is_running() const {
        return running_.load();
    }
    int port() const {
        return port_;
    }

private:
    LmsServer() = default;
    ~LmsServer();

    void accept_loop();
    void handle_client(int fd); // one HTTP/JSON-RPC request → 503 phase-1-pending reply
    void reap_workers();        // join finished worker threads (called with workers_mtx_ held)

    RemoteControlInterface *control_ = nullptr;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    int port_ = 0;
    std::string bind_addr_;

    std::thread accept_thread_;

    // Tracked worker threads (same self-marking-done pattern as RemoteServer::Worker).
    struct Worker {
        std::thread t;
        std::atomic<bool> done{false};
        Worker() = default;
    };
    std::mutex workers_mtx_;
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace panicast
