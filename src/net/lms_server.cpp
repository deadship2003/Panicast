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
#include <ctime>
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

// Minimal base64 decode (standard alphabet) for HTTP Basic auth.
std::string b64decode(const std::string &in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto dv = [&](char c) -> int {
        const char *p = std::strchr(tbl, c);
        return (c && p) ? (int)(p - tbl) : -1;
    };
    std::string out;
    int val = 0, bits = 0;
    for (char c : in) {
        if (c == '=')
            break;
        int d = dv(c);
        if (d < 0)
            continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((val >> bits) & 0xFF);
        }
    }
    return out;
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
    char buf[4096];
    while (running_.load()) {
        ssize_t n = ::recv(c->fd, buf, sizeof(buf), 0);
        if (n <= 0)
            break; // EOF / error → connection gone
        pending.append(buf, static_cast<size_t>(n));

        // N08.3 protocol auto-detection: newer Squeezer opens an HTTP/Bayeux (cometd)
        //   connection; older CLI tools send raw lines. The first request line decides,
        //   and the mode sticks for the connection's lifetime.
        if (!c->http_mode) {
            size_t nl = pending.find('\n');
            if (nl != std::string::npos) {
                std::string first = pending.substr(0, nl);
                if (!first.empty() && first.back() == '\r')
                    first.pop_back();
                for (const char *v : {"POST ", "GET ", "PUT ", "DELETE ", "OPTIONS ", "HEAD "})
                    if (first.rfind(v, 0) == 0) {
                        c->http_mode = true;
                        LOG(fmt::format("[LMS] client {} speaks HTTP/cometd", c->client_id));
                        break;
                    }
            }
        }

        if (c->http_mode) {
            // Extract complete HTTP requests (headers + Content-Length body), keep-alive.
            for (;;) {
                size_t hdr_end = pending.find("\r\n\r\n");
                if (hdr_end == std::string::npos)
                    break;
                size_t cl = 0;
                {
                    std::string h = pending.substr(0, hdr_end);
                    std::transform(h.begin(), h.end(), h.begin(),
                                   [](unsigned char ch) { return (char)std::tolower(ch); });
                    size_t p = h.find("content-length:");
                    if (p != std::string::npos)
                        cl = (size_t)std::strtoull(h.c_str() + p + 15, nullptr, 10);
                }
                size_t total = hdr_end + 4 + cl;
                if (pending.size() < total)
                    break; // body not fully buffered yet
                size_t sp1 = pending.find(' ');
                size_t sp2 = pending.find(' ', sp1 + 1);
                std::string method = pending.substr(0, sp1);
                std::string pathq =
                    sp2 != std::string::npos ? pending.substr(sp1 + 1, sp2 - sp1 - 1)
                                             : pending.substr(sp1 + 1);
                std::string path = pathq.substr(0, pathq.find('?'));
                std::string headers = pending.substr(0, hdr_end);
                std::string body = pending.substr(hdr_end + 4, cl);
                pending.erase(0, total);

                std::string resp = handle_http(*c, method, path, headers, body);
                {
                    std::lock_guard<std::mutex> lk(c->wmtx);
                    if (c->fd >= 0)
                        ::send(c->fd, resp.data(), resp.size(), MSG_NOSIGNAL);
                }
            }
            if (pending.size() > (1u << 20))
                pending.clear(); // runaway garbage guard
        } else {
            // CLI line protocol: one command per line.
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
            if (c->kick)
                break;
            if (pending.size() > 65536)
                pending.clear();
        }
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
            if (c->done.load() || c->http_mode) // HTTP clients get JSON responses only (v1)
                continue;
            if (!(c->subscribed || c->listening))
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

// ── N08.3: HTTP/cometd (Bayeux JSON-RPC) — newer Squeezer's transport ──────────
//   POST /cometd with a JSON array of Bayeux messages; /meta/* keep the long-poll
//   session alive, /service/* carry JSON-RPC "slim.request" calls whose params are the
//   same CLI command arrays. Auth = HTTP Basic against lms_user/lms_pass.
std::string LmsServer::handle_http(Conn &c, const std::string &method, const std::string &path,
                                   const std::string &headers, const std::string &body) {
    auto http_resp = [](int code, const char *status, const std::string &b,
                        const char *extra = "") {
        return fmt::format(
            "HTTP/1.1 {} {}\r\nContent-Type: application/json;charset=UTF-8\r\n"
            "Content-Length: {}\r\nConnection: keep-alive\r\n{}\r\n{}",
            code, status, b.size(), extra, b);
    };
    auto header_val = [&](const std::string &key) -> std::string {
        // Search case-insensitively on a lowered COPY, but slice the ORIGINAL string —
        //   the transform is 1:1 so indices align; returning the lowered copy would turn
        //   "Authorization: Basic ..." into "basic ..." and break the scheme match below.
        std::string h = headers;
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        size_t p = h.find(key + ":");
        if (p == std::string::npos)
            return "";
        p += key.size() + 1;
        while (p < h.size() && (h[p] == ' ' || h[p] == '\t'))
            p++;
        size_t e = headers.find("\r\n", p);
        return headers.substr(p, e == std::string::npos ? std::string::npos : e - p);
    };

    // Basic-auth gate → lms_user/lms_pass (Squeezer sends credentials preemptively).
    if (auth_required_ && !c.authed.load()) {
        bool ok = false;
        std::string auth = header_val("authorization");
        if (auth.rfind("Basic ", 0) == 0) {
            std::string dec = b64decode(auth.substr(6));
            size_t colon = dec.find(':');
            if (colon != std::string::npos &&
                ct_equal(dec.substr(0, colon), lms_user_) &&
                ct_equal(dec.substr(colon + 1), lms_pass_)) {
                ok = true;
                c.http_auth_user = dec.substr(0, colon);
            }
        }
        if (!ok) {
            LOG(fmt::format("[LMS-HTTP] 401 {} {} (bad or missing Basic credentials)",
                            method, path));
            return http_resp(401, "Unauthorized", "[]",
                             "WWW-Authenticate: Basic realm=\"panicast\"\r\n");
        }
        c.authed.store(true);
    }

    LOG(fmt::format("[LMS-HTTP] >> {} {} {}", method, path, body.substr(0, 300)));
    if (path.find("/cometd") == std::string::npos && path.find("/jsonrpc") == std::string::npos)
        return http_resp(404, "Not Found", "[]");

    bool is_cometd = path.find("/cometd") != std::string::npos;
    nlohmann::json out = nlohmann::json::array();
    bool direct_rpc = false; // /jsonrpc.js style: single object in, single object out
    try {
        nlohmann::json msgs = nlohmann::json::parse(body, nullptr, false);
        if (msgs.is_object()) {
            direct_rpc = true;
            msgs = nlohmann::json::array({msgs});
        } else if (!msgs.is_array())
            msgs = nlohmann::json::array();
        for (const auto &m : msgs) {
            std::string ch = m.value("channel", std::string());
            nlohmann::json id = m.contains("id") ? m["id"] : nlohmann::json(nullptr);

            if (ch.rfind("/meta/handshake", 0) == 0) {
                static std::atomic<uint64_t> next_cid{1};
                nlohmann::json j;
                j["channel"] = "/meta/handshake";
                j["successful"] = true;
                j["authSuccessful"] = true;
                j["version"] = "1.0";
                j["supportedConnectionTypes"] = nlohmann::json::array({"long-polling"});
                j["clientId"] = fmt::format("lc{:016x}", next_cid.fetch_add(1));
                j["id"] = id;
                out.push_back(j);
            } else if (ch.rfind("/meta/connect", 0) == 0) {
                // Immediate reply + advice.interval keeps the client from busy-spinning
                //   without holding a thread per long-poll (state arrives on requests).
                char ts[32];
                std::time_t now = std::time(nullptr);
                std::strftime(ts, sizeof(ts), "%FT%TZ", std::gmtime(&now));
                nlohmann::json j;
                j["channel"] = "/meta/connect";
                j["successful"] = true;
                j["clientId"] = m.value("clientId", std::string());
                j["timestamp"] = ts;
                nlohmann::json adv;
                adv["reconnect"] = "retry";
                adv["interval"] = 800;
                adv["timeout"] = 25000;
                j["advice"] = adv;
                j["id"] = id;
                out.push_back(j);
            } else if (ch.rfind("/meta/subscribe", 0) == 0 || ch.rfind("/meta/unsubscribe", 0) == 0) {
                nlohmann::json j;
                j["channel"] = ch;
                j["successful"] = true;
                j["clientId"] = m.value("clientId", std::string());
                j["subscription"] = m.value("subscription", std::string());
                j["id"] = id;
                out.push_back(j);
            } else if (ch.rfind("/meta/disconnect", 0) == 0) {
                nlohmann::json j;
                j["channel"] = ch;
                j["successful"] = true;
                j["clientId"] = m.value("clientId", std::string());
                j["id"] = id;
                out.push_back(j);
            } else {
                // Service message: {"channel":"/service/...","data":{id,method,params}}
                const nlohmann::json *rpc = m.contains("data") && m["data"].is_object() ? &m["data"] : &m;
                nlohmann::json result = nlohmann::json::object();
                if (rpc->value("method", std::string()) == "slim.request" &&
                    (*rpc).contains("params") && (*rpc)["params"].is_array() &&
                    (*rpc)["params"].size() >= 2 && (*rpc)["params"][1].is_array()) {
                    std::vector<std::string> cmd;
                    for (const auto &e : (*rpc)["params"][1])
                        cmd.push_back(e.is_string() ? e.get<std::string>() : e.dump());
                    result = json_slim_request(c, cmd);
                }
                nlohmann::json rpc_resp;
                rpc_resp["id"] = rpc->contains("id") ? (*rpc)["id"] : nlohmann::json(nullptr);
                rpc_resp["method"] = rpc->value("method", std::string());
                rpc_resp["result"] = result;
                if (is_cometd && !direct_rpc) {
                    nlohmann::json j;
                    j["channel"] = ch.empty() ? "/service/slim/request" : ch;
                    j["data"] = rpc_resp;
                    j["id"] = id;
                    out.push_back(j);
                } else {
                    out = rpc_resp; // direct JSON-RPC → single object response
                }
            }
        }
    } catch (const std::exception &e) {
        LOG(fmt::format("[LMS-HTTP] body parse error: {}", e.what()));
        return http_resp(400, "Bad Request", "[]");
    }

    std::string dump = direct_rpc ? out.dump() : out.dump();
    LOG(fmt::format("[LMS-HTTP] << {}", dump.substr(0, 300)));
    return http_resp(200, "OK", dump);
}

// JSON-RPC command mapping — same command set as the CLI path, but LMS-JSON shaped:
//   stringly-typed values, players_loop array for player listings, flat status object.
nlohmann::json LmsServer::json_slim_request(Conn &c, const std::vector<std::string> &cmd) {
    if (cmd.empty())
        return nlohmann::json::object();
    const std::string &k = cmd[0];
    auto push = [&](const char *action) {
        if (bus_)
            bus_->push({action, {}, c.client_id});
    };
    auto push_arg = [&](const char *action, const std::string &arg) {
        if (bus_)
            bus_->push({action, {arg}, c.client_id});
    };

    if (k == "players" || k == "serverstatus") {
        nlohmann::json p;
        p["playerindex"] = "0";
        p["playerid"] = player_id();
        p["name"] = player_name();
        p["model"] = "squeezelite";
        p["modelname"] = "SqueezeLite";
        p["isplayer"] = "1";
        p["connected"] = "1";
        p["power"] = "1";
        p["displaytype"] = "graphic-280x16";
        p["seq_no"] = "0";
        nlohmann::json r;
        r["count"] = "1";
        r["player_count"] = "1";
        if (k == "serverstatus") {
            r["version"] = "8.4.0";
            r["sn"] = "0";
        }
        r["players_loop"] = nlohmann::json::array({p});
        return r;
    }
    if (k == "status") {
        for (const auto &a : cmd)
            if (a.rfind("subscribe:", 0) == 0)
                c.subscribed = true; // JSON pushes are v2; marking keeps semantics
        if (!control_)
            return nlohmann::json::object();
        auto s = control_->snapshot_state();
        const char *mode = !s.has_media ? "stop" : (s.paused ? "pause" : "play");
        int idx = std::max(0, s.current_index);
        auto S = [](int v) { return std::to_string(v); };
        nlohmann::json r;
        r["player_name"] = player_name();
        r["player_connected"] = "1";
        r["power"] = "1";
        r["mode"] = mode;
        r["playlist_tracks"] = S((int)s.playlist.size());
        r["playlist_cur_index"] = S(idx);
        r["playlist_repeat"] = "0";
        r["playlist_shuffle"] = "0";
        r["song"] = S(idx);
        r["seq_no"] = "0";
        r["rate"] = "1";
        r["time"] = S((int)s.elapsed);
        r["duration"] = S((int)s.duration);
        r["canseek"] = "1";
        r["digital_volume_control"] = "1";
        r["mixer volume"] = S(s.volume); // LMS keeps the space in the JSON key
        if (s.has_media) {
            r["current_title"] = s.title;
            r["title"] = s.title;
            if (!s.art_url.empty())
                r["art_url"] = s.art_url;
        }
        return r;
    }
    if (k == "login" || k == "listen")
        return nlohmann::json::object();
    if (k == "version") {
        nlohmann::json r;
        r["_version"] = "8.4.0";
        return r;
    }
    if (k == "play") {
        push("play");
    } else if (k == "pause") {
        std::string want = cmd.size() > 1 ? cmd[1] : "";
        if (want == "1")
            push("pause");
        else if (want == "0")
            push("resume");
        else
            push("play_pause");
    } else if (k == "stop") {
        push("stop");
    } else if (k == "next") {
        push("next");
    } else if (k == "prev") {
        push("previous");
    } else if (k == "playlist" && cmd.size() > 1) {
        const std::string &sub = cmd[1];
        if (sub == "index" && cmd.size() > 2) {
            if (cmd[2] == "+1")
                push("next");
            else if (cmd[2] == "-1")
                push("previous");
        } else if (sub == "next") {
            push("next");
        } else if (sub == "prev") {
            push("previous");
        }
    } else if (k == "mixer" && cmd.size() > 1) {
        if (cmd[1] == "volume" && cmd.size() > 2 && cmd[2] != "?")
            push_arg("volume", cmd[2]);
        // muting unsupported → accept silently
    } else if (k == "time") {
        if (cmd.size() > 1 && cmd[1] != "?")
            push_arg("seekto", cmd[1]);
    } else {
        LOG(fmt::format("[LMS-JSON] unhandled command: {}",
                        fmt::join(cmd.begin(), cmd.end(), " ")));
    }
    return nlohmann::json::object();
}

} // namespace panicast
