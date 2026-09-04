// mini-LMS server — Squeezer remote control (N08, phase 1: the LMS CLI line protocol).
//   Implements the Logitech Media Server "telnet" CLI control plane on TCP :9090 so stock
//   controller apps (Android Squeezer) can drive panicast: newline-terminated requests
//   `[playerid] <command> <params...>`, responses mirror the request; hash-style responses
//   are space-separated URL-encoded key:value tokens (see cli_encode()).
//
//   Phase 1 scope: connection + login echo + version + players (one virtual player,
//   id 00:00:00:00:84:21, name "panicast") + status with subscribe: pushes + transport
//   (play/pause/stop/next/prev) + mixer volume + time (query/seek). Queue browsing
//   (`titles`/`playlist tracks`) is phase 2; SlimProto audio streaming is a future optional
//   extension, out of scope.
//
//   Reads go through RemoteControlInterface::snapshot_state() (thread-safe); writes are
//   pushed as RemoteCommands onto the bus and executed on the TUI main thread — the same
//   single sanctioned network→UI crossing the PRP/WS servers use.
//
// Compile layer: built only when CMake PANICAST_REMOTE_LMS=ON (default). Runtime layer:
//   opt-in — [remote] lms_enable=false by default; nothing listens until the user flips it.
//   Singleton (not an App member) so the compile switch needs no #ifdef in app.h.
//
// Threading model:
//   - start() spawns ONE accept thread + ONE 1Hz poll thread (status-diff → pushes).
//   - Each connection runs a long-lived reader thread that blocks on line reads; pushes
//     from the poll thread serialize through a per-connection write mutex.
//   - stop() shuts the listen fd down to unblock accept(), closes every client fd to
//     unblock the readers, then joins everything. No detached threads.
//
// Platform: POSIX sockets (Linux / macOS).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "panicast/net/remote_protocol.h"

#include <nlohmann/json.hpp>

#include <sys/socket.h> // sockaddr_storage (peer_allowed — POSIX-only module)

namespace panicast
{

class RemoteCommandBus;

// One IPv4 CIDR entry (network order). Bare IPs parse as /32. Namespace scope so the
//   file-local parser in lms_server.cpp can build the list.
struct LmsCidr {
    uint32_t net;
    uint32_t mask;
};

class LmsServer {
public:
    static LmsServer &instance();

    LmsServer(const LmsServer &) = delete;
    LmsServer &operator=(const LmsServer &) = delete;

    // Bind + listen + spawn accept/poll threads. `control` is the thread-safe state
    //   snapshot source; `bus` receives the transport/volume/seek commands (executed on
    //   the TUI main thread). Returns true on success.
    bool start(const std::string &bind_addr, int port, RemoteControlInterface *control,
               RemoteCommandBus *bus);
    void stop();

    bool is_running() const {
        return running_.load();
    }
    int port() const {
        return port_;
    }
    // Stable virtual-player identity Squeezer sees (also answers for any playerid).
    static const char *player_id() {
        return "00:00:00:00:84:21";
    }
    static const char *player_name() {
        return "panicast";
    }

private:
    LmsServer() = default;
    ~LmsServer();

    // One live controller connection: fd + write mutex + subscription/auth state.
    struct Conn {
        int fd = -1;
        int64_t client_id = 0;
        std::mutex wmtx;                       // serializes response writes + pushes
        std::thread reader;                    // blocked in client_loop()
        std::atomic<bool> done{false};         // set by client_loop right before returning
        bool subscribed = false;               // status subscribe:N active
        int sub_interval_sec = 10;             // push at least this often
        bool listening = false;                // listen 1
        std::string last_status_hash;          // suppress no-op pushes
        std::chrono::steady_clock::time_point last_push{};
        std::atomic<bool> authed{false};       // login succeeded (read by the poll thread)
        bool kick = false;                     // reader thread closes after this response
        int unauth_cmds = 0;                   // pre-login commands (probe deterrent)
        // N08.3: HTTP/cometd mode (newer Squeezer speaks Bayeux JSON-RPC, not raw CLI).
        //   Detected from the first request line ("POST /cometd ..."); stays per-connection.
        bool http_mode = false;
        std::string http_auth_user;            // Basic-auth user (logging)
    };

    void accept_loop();
    void client_loop(Conn *c); // read lines/HTTP requests → dispatch → write response
    void poll_loop();          // 1Hz snapshot diff → push to subscribed conns
    std::string handle_line(const std::string &line, Conn &c); // CLI dispatch → response
    // N08.3: HTTP/cometd path — Bayeux handshake/connect/subscribe + JSON-RPC slim.request.
    std::string handle_http(Conn &c, const std::string &method, const std::string &path,
                            const std::string &headers, const std::string &body);
    nlohmann::json json_slim_request(Conn &c, const std::vector<std::string> &cmd);
    std::string status_line() const; // full hash-style status for the virtual player
    void send_line(Conn *c, const std::string &line); // locked write + log
    void reap_done(); // join + drop finished conns (conns_mtx_ held)
    bool peer_allowed(const sockaddr_storage &peer) const; // lms_allow CIDR check

    RemoteControlInterface *control_ = nullptr;
    RemoteCommandBus *bus_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> next_client_id_{1};
    int listen_fd_ = -1;
    int port_ = 0;
    std::string bind_addr_;

    // Access policy, parsed once at start() from [remote] lms_allow / lms_user / lms_pass.
    std::vector<LmsCidr> allow_;    // empty + !allow_all_ = nothing gets in (defensive)
    bool allow_all_ = false;        // lms_allow explicitly empty
    std::string lms_user_;
    std::string lms_pass_;
    bool auth_required_ = false;    // lms_pass non-empty → login gate

    std::thread accept_thread_;
    std::thread poll_thread_;

    std::mutex conns_mtx_;
    std::vector<std::unique_ptr<Conn>> conns_;
};

} // namespace panicast
