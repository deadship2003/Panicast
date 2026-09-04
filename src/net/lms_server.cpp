// mini-LMS server implementation. N08 phase 1: the LMS CLI line protocol (Squeezer).
//   See lms_server.h for the protocol/threading/gating contract.
#include "panicast/net/lms_server.h"

#include "panicast/config/ini_config.h"
#include "panicast/core/logger.h"
#include "panicast/net/remote_command_bus.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace panicast
{

namespace
{
// Same dual-stack listener as remote_server.cpp's make_listen_fd.
int make_listen_fd(const std::string &bind_addr, int port) {
    bool dual = (bind_addr.empty() || bind_addr == "0.0.0.0" || bind_addr == "::");
    int fd = -1;
    if (dual) {
        fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (fd >= 0) {
            int v6only = 0; // dual-stack: accept IPv4-mapped + native IPv6
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
            int yes = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            struct sockaddr_in6 a6{};
            a6.sin6_family = AF_INET6;
            a6.sin6_port = htons(static_cast<uint16_t>(port));
            a6.sin6_addr = in6addr_any;
            if (::bind(fd, reinterpret_cast<struct sockaddr *>(&a6), sizeof(a6)) < 0) {
                LOG(fmt::format("[LMS] bind6(:{}) failed: {}", port, std::strerror(errno)));
                ::close(fd);
                fd = -1;
            }
        }
    }
    if (fd < 0) { // fallback: IPv4 only
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in a4{};
        a4.sin_family = AF_INET;
        a4.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, bind_addr.c_str(), &a4.sin_addr) <= 0)
            a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&a4), sizeof(a4)) < 0) {
            LOG(fmt::format("[LMS] bind(:{}) failed: {}", port, std::strerror(errno)));
            ::close(fd);
            return -1;
        }
    }
    if (::listen(fd, 16) < 0) {
        LOG(fmt::format("[LMS] listen(:{}) failed: {}", port, std::strerror(errno)));
        ::close(fd);
        return -1;
    }
    return fd;
}

// LMS CLI wire encoding: hash tokens are `key:value` with the whole token URL-encoded
//   (space %20, colon %3A, percent %25 — the characters that break token splitting).
std::string cli_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == ' ' || c == ':' || c == '%' || c == '&')
            out += std::string("%") + hex[c >> 4] + hex[c & 0xF];
        else
            out += (char)c;
    }
    return out;
}

// Best-effort percent-decode for incoming tokens (Squeezer encodes params the same way).
std::string cli_decode(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hv = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            int h = hv(s[i + 1]), l = hv(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)(h * 16 + l);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::vector<std::string> split_ws(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

// Parse "a.b.c.d[/n],..." into network-order CIDRs. Bare IP = /32. Invalid entries are
//   logged and skipped (a typo must not silently open the list to everything).
std::vector<LmsCidr> parse_cidrs(const std::string &csv) {
    std::vector<LmsCidr> out;
    std::string cur;
    auto emit = [&](const std::string &entry) {
        std::string e = entry;
        e.erase(0, e.find_first_not_of(" \t"));
        e.erase(e.find_last_not_of(" \t") + 1);
        if (e.empty())
            return;
        int bits = 32;
        std::string ip = e;
        size_t slash = e.find('/');
        if (slash != std::string::npos) {
            ip = e.substr(0, slash);
            bits = std::atoi(e.c_str() + slash + 1);
            if (bits < 0 || bits > 32) {
                LOG(fmt::format("[LMS] lms_allow: bad prefix in '{}', skipped", e));
                return;
            }
        }
        struct in_addr a{};
        if (::inet_pton(AF_INET, ip.c_str(), &a) != 1) {
            LOG(fmt::format("[LMS] lms_allow: bad IP in '{}', skipped", e));
            return;
        }
        uint32_t host_mask = (bits == 0) ? 0u : (0xFFFFFFFFu << (32 - bits));
        out.push_back({a.s_addr & htonl(host_mask), htonl(host_mask)});
    };
    for (char c : csv) {
        if (c == ',') {
            emit(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    emit(cur);
    return out;
}

// Constant-time equality — a byte-wise diff accumulator, so a probing client can't learn
//   the password one character at a time from response timing.
bool ct_equal(const std::string &a, const std::string &b) {
    unsigned char diff = (unsigned char)(a.size() ^ b.size());
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        diff |= (unsigned char)(a[i % a.size()]) ^ (unsigned char)(b[i % b.size()]);
    return diff == 0;
}
} // namespace

LmsServer &LmsServer::instance() {
    static LmsServer s;
    return s;
}

LmsServer::~LmsServer() {
    stop();
}

bool LmsServer::start(const std::string &bind_addr, int port, RemoteControlInterface *control,
                      RemoteCommandBus *bus) {
    if (running_.load())
        return true;

    // Access policy (read once — restart after editing lms_allow/lms_user/lms_pass).
    std::string allow_csv = IniConfig::instance().get_remote_lms_allow();
    allow_all_ = allow_csv.find_first_not_of(" \t") == std::string::npos;
    allow_ = parse_cidrs(allow_csv);
    lms_user_ = IniConfig::instance().get_remote_lms_user();
    lms_pass_ = IniConfig::instance().get_remote_lms_pass();
    auth_required_ = !lms_pass_.empty();

    control_ = control;
    bus_ = bus;
    listen_fd_ = make_listen_fd(bind_addr, port);
    if (listen_fd_ < 0)
        return false;

    bind_addr_ = bind_addr;
    port_ = port;
    running_.store(true);
    accept_thread_ = std::thread(&LmsServer::accept_loop, this);
    poll_thread_ = std::thread(&LmsServer::poll_loop, this);
    LOG(fmt::format("[LMS] mini-LMS CLI (Squeezer control plane) listening on {}:{} — point "
                    "Squeezer at <this host>:{}",
                    bind_addr, port, port));
    LOG(fmt::format("[LMS] allowlist: {} | login auth: {}", allow_all_ ? "ALL sources" : allow_csv,
                    auth_required_ ? fmt::format("on (user '{}')", lms_user_) : "off"));
    return true;
}

void LmsServer::stop() {
    if (!running_.exchange(false))
        return;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR); // unblock accept()
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable())
        accept_thread_.join();
    if (poll_thread_.joinable())
        poll_thread_.join();
    { // closing fds unblocks the readers; then join + drop them
        std::lock_guard<std::mutex> lk(conns_mtx_);
        for (auto &c : conns_) {
            if (c->fd >= 0) {
                ::shutdown(c->fd, SHUT_RDWR);
                ::close(c->fd);
                c->fd = -1;
            }
        }
        for (auto &c : conns_)
            if (c->reader.joinable())
                c->reader.join();
        conns_.clear();
    }
    LOG("[LMS] mini-LMS server stopped");
}

void LmsServer::accept_loop() {
    while (running_.load()) {
        struct sockaddr_storage peer{};
        socklen_t plen = sizeof(peer);
        int fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&peer), &plen);
        if (fd < 0) {
            if (!running_.load())
                break; // listen fd closed by stop()
            continue;
        }
        // Source-IP gate (lms_allow): reject before the protocol layer ever speaks.
        if (!peer_allowed(peer)) {
            char ip[INET6_ADDRSTRLEN] = "?";
            if (peer.ss_family == AF_INET6)
                ::inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, ip, sizeof(ip));
            else if (peer.ss_family == AF_INET)
                ::inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, ip, sizeof(ip));
            LOG(fmt::format("[LMS] rejected connection from {} (not in lms_allow)", ip));
            ::close(fd);
            continue;
        }
        int nodelay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        std::lock_guard<std::mutex> lk(conns_mtx_);
        reap_done();
        auto c = std::make_unique<Conn>();
        c->fd = fd;
        c->client_id = next_client_id_++;
        c->last_push = std::chrono::steady_clock::now();
        Conn *raw = c.get();
        c->reader = std::thread([this, raw] { client_loop(raw); });
        conns_.push_back(std::move(c));
    }
}

// lms_allow check. IPv4 and v4-mapped IPv6 go through the CIDR list; native IPv6 only
//   passes for ::1 (loopback) — the default list is v4-shaped by design.
bool LmsServer::peer_allowed(const sockaddr_storage &peer) const {
    if (allow_all_)
        return true;
    const unsigned char *v4 = nullptr;
    if (peer.ss_family == AF_INET) {
        v4 = (const unsigned char *)&((const struct sockaddr_in *)&peer)->sin_addr;
    } else if (peer.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&peer;
        const unsigned char *b = (const unsigned char *)&a6->sin6_addr;
        static const unsigned char v4mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        static const unsigned char v6loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        if (std::equal(b, b + 12, v4mapped_prefix))
            v4 = b + 12;
        else if (std::equal(b, b + 16, v6loop))
            return true;
        else
            return false;
    } else {
        return false;
    }
    uint32_t ip;
    std::memcpy(&ip, v4, sizeof(ip)); // network order
    for (const auto &c : allow_)
        if ((ip & c.mask) == c.net)
            return true;
    return false;
}

void LmsServer::reap_done() { // conns_mtx_ held
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(),
                                [](const std::unique_ptr<Conn> &c) {
                                    if (c->done.load() && c->reader.joinable())
                                        c->reader.join();
                                    return c->done.load();
                                }),
                 conns_.end());
}

// ── protocol dispatch ─────────────────────────────────────────────────────────
// Request:  [playerid] command params...   (playerid = first token containing ':')
// Response: mirrors the request line; hash answers append encoded key:value tokens.
std::string LmsServer::handle_line(const std::string &raw, Conn &c) {
    std::vector<std::string> t = split_ws(raw);
    for (auto &s : t)
        s = cli_decode(s);
    if (t.empty())
        return {};

    // Strip the playerid prefix when present (single virtual player answers for any id).
    size_t i = 0;
    if (t[0].find(':') != std::string::npos && t.size() > 1 &&
        t[0] != "subscribe:") // a leading MAC-shaped token is a playerid
        i = 1;
    const std::string &cmd = t[i];
    std::vector<std::string> p(t.begin() + i + 1, t.end());

    auto snap = [this] { return control_ ? control_->snapshot_state() : RemoteStateSnapshot{}; };
    auto mode_str = [](const RemoteStateSnapshot &s) -> const char * {
        if (!s.has_media)
            return "stop";
        return s.paused ? "pause" : "play";
    };

    // ── login gate: with lms_user/lms_pass set, nothing executes before a successful login.
    //   Failed login gets no echo and the connection is dropped (clear failure signal for
    //   the app, no brute-force window); pre-login commands are silently ignored, and more
    //   than a few of them drops the connection too.
    if (auth_required_ && !c.authed.load()) {
        if (cmd == "login" && p.size() >= 2) {
            if (ct_equal(p[0], lms_user_) && ct_equal(p[1], lms_pass_)) {
                c.authed.store(true);
                LOG(fmt::format("[LMS] client {} logged in as '{}'", c.client_id, lms_user_));
                return raw; // echo = success (what Squeezer waits for)
            }
            LOG(fmt::format("[LMS] auth FAILED (client {})", c.client_id));
            c.kick = true;
            return {};
        }
        if (++c.unauth_cmds > 5) {
            LOG(fmt::format("[LMS] dropping unauthenticated client {} (command flood)", c.client_id));
            c.kick = true;
        }
        return {}; // silent drop
    }
    if (cmd == "login")
        return raw; // auth disabled — accept any credentials (allowlist-only)

    // ── server-level ──
    if (cmd == "version")
        return "version 8.4.0"; // LMS-compatible version Squeezer gates features on
    if (cmd == "listen") {
        c.listening = !p.empty() && p[0] == "1";
        return raw;
    }
    if (cmd == "serverstatus" || cmd == "players") {
        std::string start = p.empty() ? "0" : p[0];
        std::string count = p.size() > 1 ? p[1] : "100";
        std::string resp =
            fmt::format("{} {} {} count:1 playercount:1", cmd, start, count);
        resp += fmt::format(" playerindex:0 playerid:{} name:{}", cli_encode(player_id()),
                            cli_encode(player_name()));
        resp += " model:squeezelite modelname:SqueezeLite";
        resp += " isplayer:1 displaytype:graphic-280x16";
        resp += " connected:1 power:1";
        return resp;
    }

    // ── player-level (transport / mixer / time / status) ──
    auto pid_echo = [&](const std::string &rest) -> std::string {
        return i == 1 ? fmt::format("{} {}", t[0], rest) : rest;
    };

    if (cmd == "status") {
        std::string args;
        for (auto &a : p)
            args += a + " ";
        if (!args.empty())
            args.pop_back();
        // subscribe:<n> → periodic pushes on this connection (Squeezer's poll model)
        for (auto &a : p) {
            if (a.rfind("subscribe:", 0) == 0) {
                c.subscribed = true;
                c.sub_interval_sec = std::max(1, std::atoi(a.c_str() + 10));
                if (c.sub_interval_sec > 60)
                    c.sub_interval_sec = 60;
            }
        }
        std::string resp = fmt::format("{} status {}", i == 1 ? t[0] : player_id(), args);
        resp += " " + status_line();
        return resp;
    }
    if (cmd == "play") {
        if (bus_)
            bus_->push({"play", {}, c.client_id});
        return pid_echo("play");
    }
    if (cmd == "pause") {
        // Forms seen in the wild: `pause` (toggle), `pause 1`, `pause 0`, `pause 1 0`.
        std::string want; // "" = toggle
        if (!p.empty())
            want = p[0];
        if (bus_) {
            if (want == "1")
                bus_->push({"pause", {}, c.client_id});
            else if (want == "0")
                bus_->push({"resume", {}, c.client_id});
            else
                bus_->push({"play_pause", {}, c.client_id});
        }
        return raw; // LMS echoes the request verbatim
    }
    if (cmd == "stop") {
        if (bus_)
            bus_->push({"stop", {}, c.client_id});
        return pid_echo("stop");
    }
    if (cmd == "next" || cmd == "prev") {
        if (bus_)
            bus_->push({cmd == "next" ? "next" : "previous", {}, c.client_id});
        return pid_echo(cmd);
    }
    if (cmd == "playlist" && !p.empty()) {
        // `playlist index +1|-1|<n>` / `playlist next|prev` — queue stepping (jump = phase 2)
        const std::string &sub = p[0];
        if (sub == "index" && p.size() > 1) {
            const std::string &off = p[1];
            if (off == "+1" || sub == "next") {
                if (bus_)
                    bus_->push({"next", {}, c.client_id});
            } else if (off == "-1") {
                if (bus_)
                    bus_->push({"previous", {}, c.client_id});
            }
            return pid_echo(fmt::format("playlist index {}", off));
        }
        if (sub == "next" || sub == "prev") {
            if (bus_)
                bus_->push({sub == "next" ? "next" : "previous", {}, c.client_id});
            return pid_echo(fmt::format("playlist {}", sub));
        }
        if (sub == "tracks" && p.size() > 1 && p[1] == "?") {
            auto s = snap();
            return pid_echo(fmt::format("playlist tracks {}", s.playlist.size()));
        }
        if (sub == "tracks")
            return pid_echo(fmt::format("playlist tracks {}", snap().playlist.size()));
        return pid_echo(fmt::format("playlist {}", sub));
    }
    if (cmd == "mixer" && !p.empty()) {
        const std::string &sub = p[0];
        if (sub == "volume") {
            if (p.size() > 1 && p[1] != "?") {
                if (bus_)
                    bus_->push({"volume", {p[1]}, c.client_id});
                return pid_echo(fmt::format("mixer volume {}", p[1]));
            }
            return pid_echo(fmt::format("mixer volume {}", snap().volume));
        }
        if (sub == "muting") { // no mute support — report unmuted, accept the set silently
            if (p.size() > 1 && p[1] != "?")
                return pid_echo(fmt::format("mixer muting {}", p[1]));
            return pid_echo("mixer muting 0");
        }
        return pid_echo(fmt::format("mixer {}", sub));
    }
    if (cmd == "time") {
        if (!p.empty() && p[0] != "?") {
            if (bus_)
                bus_->push({"seekto", {p[0]}, c.client_id});
            return pid_echo(fmt::format("time {}", p[0]));
        }
        return pid_echo(fmt::format("time {}", (int)snap().elapsed));
    }
    if (cmd == "mode")
        return pid_echo(fmt::format("mode {}", mode_str(snap())));
    if (cmd == "name" || cmd == "player_name")
        return pid_echo(fmt::format("{} {}", cmd, player_name()));
    if (cmd == "power") {
        if (!p.empty() && p[0] != "?")
            return pid_echo(fmt::format("power {}", p[0]));
        return pid_echo("power 1"); // always on
    }
    if (cmd == "songinfo" || cmd == "titles") // queue browsing → phase 2
        return pid_echo(fmt::format("{} 0 0 count:0", cmd));

    // Unknown command: echo it back (LMS behavior) and log loudly for phase-1 fieldwork.
    LOG(fmt::format("[LMS] unhandled command: {}", raw.substr(0, 200)));
    return raw;
}

std::string LmsServer::status_line() const {
    if (!control_)
        return {};
    auto s = control_->snapshot_state();
    const char *mode = !s.has_media ? "stop" : (s.paused ? "pause" : "play");
    int idx = std::max(0, s.current_index);
    std::string out = fmt::format(
        "player_name:{} player_connected:1 power:1 mode:{} "
        "playlist_tracks:{} playlist_cur_index:{} playlist_repeat:0 playlist_shuffle:0 "
        "song:{} time:{} duration:{} canseek:1 digital_volume_control:1 "
        "mixer volume:{} rate:1",
        cli_encode(player_name()), mode, s.playlist.size(), idx, idx, (int)s.elapsed,
        (int)s.duration, s.volume);
    if (s.has_media) {
        out += fmt::format(" current_title:{} title:{}", cli_encode(s.title), cli_encode(s.title));
        if (!s.art_url.empty())
            out += fmt::format(" art_url:{}", cli_encode(s.art_url));
    }
    return out;
}

void LmsServer::send_line(Conn *c, const std::string &line) {
    if (line.empty())
        return;
    std::lock_guard<std::mutex> lk(c->wmtx);
    if (c->fd < 0)
        return;
    std::string buf = line + "\n";
    ::send(c->fd, buf.data(), buf.size(), MSG_NOSIGNAL);
    LOG(fmt::format("[LMS] << {}", line.substr(0, 200)));
}

void LmsServer::client_loop(Conn *c) {
    LOG(fmt::format("[LMS] client {} connected", c->client_id));
    std::string pending;
    char buf[2048];
    while (running_.load()) {
        ssize_t n = ::recv(c->fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break; // EOF / error → connection gone
        pending.append(buf, static_cast<size_t>(n));
        // Process every complete line; Squeezer sends one command per line.
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            LOG(fmt::format("[LMS] >> {}", line.substr(0, 200)));
            std::string resp = handle_line(line, *c);
            send_line(c, resp);
            if (c->kick)
                break; // auth failure / unauth flood → close now
        }
        if (pending.size() > 65536) // runaway garbage guard
            pending.clear();
    }
    LOG(fmt::format("[LMS] client {} disconnected", c->client_id));
    if (c->fd >= 0) {
        ::close(c->fd);
        c->fd = -1;
    }
    c->done.store(true);
}

void LmsServer::poll_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (!running_.load())
            break;
        std::string st = status_line();
        std::string hash = st; // full-line compare: any field change triggers a push
        std::lock_guard<std::mutex> lk(conns_mtx_);
        auto now = std::chrono::steady_clock::now();
        for (auto &c : conns_) {
            if (c->done.load() || !(c->subscribed || c->listening))
                continue;
            if (auth_required_ && !c->authed.load())
                continue; // no state pushes before login
            bool changed = hash != c->last_status_hash;
            bool due = now - c->last_push >= std::chrono::seconds(c->sub_interval_sec);
            if (changed || due) {
                send_line(c.get(), fmt::format("{} status - 1 {}", player_id(), st));
                c->last_status_hash = hash;
                c->last_push = now;
            }
        }
    }
}

} // namespace panicast
