// mini-LMS server implementation. N08: accept-loop skeleton only — listens, accepts
//   HTTP/JSON-RPC connections, replies with a phase-1-pending JSON-RPC error, closes.
//   See lms_server.h for the threading model, compile/runtime gating, and scope.
#include "panicast/net/lms_server.h"

#include "panicast/core/logger.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
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
// Same dual-stack listener as remote_server.cpp's make_listen_fd: IPv6 socket with
//   V6ONLY=0 accepts IPv4-mapped + native IPv6 when binding "0.0.0.0"/"::"; falls back
//   to plain IPv4 for specific address binds.
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

// Read until the end of the HTTP request headers ("\r\n\r\n"), bounded so a stray
//   connection can't pin a worker forever.
bool read_http_head(int fd, std::string &req) {
    char buf[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return false;
        req.append(buf, static_cast<size_t>(n));
    }
    return req.find("\r\n\r\n") != std::string::npos;
}
} // namespace

LmsServer &LmsServer::instance() {
    static LmsServer s;
    return s;
}

LmsServer::~LmsServer() {
    stop();
}

bool LmsServer::start(const std::string &bind_addr, int port, RemoteControlInterface *control) {
    if (running_.load())
        return true;

    control_ = control;
    listen_fd_ = make_listen_fd(bind_addr, port);
    if (listen_fd_ < 0)
        return false;

    bind_addr_ = bind_addr;
    port_ = port;
    running_.store(true);
    accept_thread_ = std::thread(&LmsServer::accept_loop, this);
    LOG(fmt::format("[LMS] mini-LMS (Squeezer control plane) listening on {}:{} — phase-1 "
                    "Bayeux/JSON-RCP protocol pending (N08)",
                    bind_addr, port));
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
    std::lock_guard<std::mutex> lk(workers_mtx_);
    for (auto &w : workers_)
        if (w->t.joinable())
            w->t.join();
    workers_.clear();
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
        // One short-lived worker per connection (phase-1 reply is one request → close).
        //   handle_client touches only this + fd, and stop() joins every worker before
        //   destruction, so a bare `this` capture is safe.
        std::lock_guard<std::mutex> lk(workers_mtx_);
        reap_workers();
        auto w = std::make_unique<Worker>();
        w->t = std::thread([this, fd] { handle_client(fd); });
        workers_.push_back(std::move(w));
    }
}

void LmsServer::reap_workers() { // called with workers_mtx_ held
    workers_.erase(
        std::remove_if(workers_.begin(), workers_.end(),
                       [](const std::unique_ptr<Worker> &w) {
                           if (w->done.load() && w->t.joinable())
                               w->t.join();
                           return w->done.load();
                       }),
        workers_.end());
}

void LmsServer::handle_client(int fd) {
    // Bound the request read so idle/malicious peers can't pin the worker.
    struct timeval tv{5, 0}; // 5s recv timeout
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string req;
    if (read_http_head(fd, req)) {
        // N08 phase-1 skeleton: every request gets a well-formed JSON-RPC error so Squeezer
        //   (or curl) sees a live-but-unimplemented endpoint instead of a dead port. The real
        //   Bayeux handshake/connect/publish handlers land in the next iteration.
        static const char *body =
            "{\"id\":null,\"result\":null,\"error\":\"mini-LMS phase-1 protocol not "
            "implemented yet (N08)\"}";
        std::string resp = fmt::format(
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: {}\r\n"
            "Connection: close\r\n\r\n{}",
            std::strlen(body), body);
        ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
    }
    ::close(fd);

    std::lock_guard<std::mutex> lk(workers_mtx_);
    for (auto &w : workers_)
        if (w->t.get_id() == std::this_thread::get_id()) {
            w->done.store(true);
            break;
        }
}

} // namespace panicast
