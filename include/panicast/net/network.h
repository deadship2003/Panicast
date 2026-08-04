// Network layer: CURL RAII wrapper + HTTP fetching (Network).
// CurlRAII is folded into this file (not a separate file); apply_network_proxy is shared by all curl call sites.
#pragma once

#include <curl/curl.h>
#include <regex>
#include <string>
#include <vector>

#include "panicast/config/ini_config.h"
#include "panicast/net/url_guard.h"

namespace panicast
{

constexpr const char* USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
constexpr int DEFAULT_NETWORK_TIMEOUT_SEC = 10;     // Default network timeout (seconds)

// CURL RAII wrapper
class CurlRAII {
public:
    CURL* handle;
    CurlRAII() : handle(curl_easy_init()) {}
    ~CurlRAII() { if (handle) curl_easy_cleanup(handle); }
    CurlRAII(const CurlRAII&) = delete;
    CurlRAII& operator=(const CurlRAII&) = delete;
    operator bool() const { return handle != nullptr; }
    operator CURL*() const { return handle; }
};

// Apply [network] proxy to a curl handle (empty string = do not set, use direct connection / transparent proxy).
// curl natively supports http/https/socks4/socks4a/socks5/socks5h.
void apply_network_proxy(CURL* curl);

class Network {
public:
    // SSL compatibility fix: enable ALPN/NPN and automatic TLS negotiation to resolve CDN handshake failures.
    static std::string fetch(const std::string& url, int timeout = DEFAULT_NETWORK_TIMEOUT_SEC);
    // Y01: POST helper for OAuth token endpoint + YouTube API. body is application/x-www-form-urlencoded
    //   when content_type is empty (default), else raw body with the given content-type (e.g.
    //   application/json for InnerTube). extra_headers appended as "Key: Value".
    static std::string post(const std::string& url, const std::string& body,
                            const std::string& content_type = "",
                            const std::vector<std::string>& extra_headers = {},
                            int timeout = DEFAULT_NETWORK_TIMEOUT_SEC);
    // Y01: GET with an Authorization: Bearer header (YouTube Data API v3 read calls).
    static std::string fetch_auth(const std::string& url, const std::string& bearer,
                                  int timeout = DEFAULT_NETWORK_TIMEOUT_SEC);
    // Y15: GET with Cookie header (Bilibili API auth via SESSDATA cookie).
    // Y20: added optional Referer header (Bilibili API requires it for anti-scraping).
    static std::string fetch_cookie(const std::string& url, const std::string& cookie_header,
                                    const std::string& referer = "",
                                    int timeout = DEFAULT_NETWORK_TIMEOUT_SEC);
    // Y01: DELETE with Authorization: Bearer (YouTube subscriptions.delete).
    static std::string del(const std::string& url, const std::string& bearer,
                           int timeout = DEFAULT_NETWORK_TIMEOUT_SEC);
    // Apple Podcast URL -> RSS feedUrl (via iTunes lookup API)
    static std::string lookup_apple_feed(const std::string& url);

public:
    static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* data);  // Y24.27: public for configure_curl
    // Single-attempt GET core shared by fetch() and lookup_apple_feed(). Returns the body;
    // on failure returns "" (body cleared on HTTP 4xx/5xx) and, if err_out != nullptr, fills it
    // with a short human-readable reason: the curl error string ("SSL connect error", "Connection
    // timed out", ...) or "HTTP <code>". Does NOT EVENT_LOG — callers own logging/retry policy.
    static std::string fetch_once(const std::string& url, int timeout, std::string* err_out);

    // ASR: download a (potentially large) media URL straight to a FILE through the configured proxy.
    //   curl handles SOCKS natively; ffmpeg's HTTP input CANNOT tunnel over SOCKS, which is why an
    //   ffmpeg capture of a CDN URL stalls when the network needs the proxy (the "Shift+L ASR does
    //   nothing" bug). Unlike fetch(), there is no 64MB cap and the body is streamed to disk (not a
    //   string) so multi-hundred-MB podcasts don't blow up RAM. Returns true on 2xx + non-empty file.
    static bool download_to_file(const std::string& url, const std::string& dest, int timeout = 600);
};

} // namespace panicast
