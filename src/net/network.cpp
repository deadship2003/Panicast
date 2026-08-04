#include "podradio/net/network.h"

#include <chrono>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <thread>

#include "podradio/core/event_log.h"
#include "podradio/core/logger.h"

namespace podradio
{

using json = nlohmann::json;

void apply_network_proxy(CURL* curl) {
    if (!curl) return;
    std::string proxy = IniConfig::instance().get_proxy();
    if (!proxy.empty()) {
        // Operator-controlled: pass the proxy verbatim. Use socks5h:// (remote DNS) on
        // DNS-poisoned networks (e.g. GFW) where www.youtube.com resolves to a bogus IP and
        // the TLS handshake fails; socks5:// does local DNS. The choice is the operator's.
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    }
}

// Y24.27: shared curl setup — eliminates 5x duplicated setopt boilerplate.
static void configure_curl(CURL* curl, const std::string& url,
                           struct curl_slist* headers, int timeout = DEFAULT_NETWORK_TIMEOUT_SEC) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    apply_network_proxy(curl);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Network::write_cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    bool tls_verify = IniConfig::instance().get_tls_verify();
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, tls_verify ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, tls_verify ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
}

size_t Network::write_cb(void* ptr, size_t size, size_t nmemb, void* data) {
    ((std::string*)data)->append((char*)ptr, size * nmemb);
    return size * nmemb;
}

std::string Network::fetch_once(const std::string& url, int timeout, std::string* err_out) {
    // CurlRAII auto-releases
    CurlRAII curl_raii;
    CURL* curl = curl_raii.handle;
    if (!curl) { if (err_out) *err_out = "curl init failed"; return ""; }
    // URL safety check: protocol whitelist + private network/loopback/cloud metadata host blocking (UrlGuard).
    // Controlled by the network.reject_unsafe_url switch (if the user explicitly disables it, requests are allowed).
    if (IniConfig::instance().get_reject_unsafe_url() && UrlGuard::reject(url, "Network::fetch")) {
        EVENT_LOG(fmt::format("Rejected unsafe URL: {}", url.substr(0, 60)));
        if (err_out) *err_out = "rejected unsafe URL";
        return "";
    }
    std::string data;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/xml,application/xhtml+xml,text/xml;q=0.9,*/*;q=0.8");

    configure_curl(curl, url, headers, timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    // SSL/ALPN extras (fetch_once-specific)
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_DEFAULT);
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 1L);
#if LIBCURL_VERSION_NUM < 0x075600
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_NPN, 1L);
#endif
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, 64L * 1024 * 1024);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);

    // P2-S2: a 4xx/5xx body is typically an HTML error page, not a feed — clear it so callers
    //   don't silently parse garbage as RSS/OPML. (post/fetch_auth keep the body for OAuth error JSON.)
    if (res != CURLE_OK) {
        if (err_out) *err_out = curl_easy_strerror(res);
    } else if (http_code >= 400) {
        if (err_out) *err_out = fmt::format("HTTP {}", http_code);
        LOG(fmt::format("[Network] fetch HTTP {} for {}", http_code, url.substr(0, 90)));
        data.clear();
    }
    return data;
}

std::string Network::fetch(const std::string& url, int timeout) {
    std::string err;
    std::string data = fetch_once(url, timeout, &err);
    if (err.empty()) return data;
    // Preserve fetch()'s historical logging split: HTTP errors are LOG-only (already logged in
    //   fetch_once); transport errors (curl failures) surface to the UI via EVENT_LOG.
    if (err.rfind("HTTP ", 0) != 0 && err != "rejected unsafe URL") {
        EVENT_LOG(fmt::format("Network error: {} (URL: {})", err, url.substr(0, 60)));
    }
    return data;
}

std::string Network::lookup_apple_feed(const std::string& url) {
    static const std::regex id_regex("/id(\\d+)");  // Static, to avoid recompiling on every call
    std::smatch match;
    if (!std::regex_search(url, match, id_regex)) {
        EVENT_LOG("Apple lookup failed: no podcast id in URL");
        return "";
    }
    // iTunes lookup is a tiny JSON endpoint. On flaky proxy/network paths (e.g. WSL2 + transparent
    //   proxy to itunes.apple.com) the TLS handshake fails transiently with "SSL connect error" /
    //   "Could not connect to server" — a retry a moment later usually succeeds. So retry a few
    //   times with backoff. Runs off the UI thread (submitted via pool_), so blocking is fine.
    constexpr int MAX_ATTEMPTS = 3;
    const std::string lookup_url = "https://itunes.apple.com/lookup?id=" + match[1].str() + "&entity=podcast";
    std::string last_err;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
        std::string err;
        std::string response = fetch_once(lookup_url, 10, &err);  // 12s: tiny JSON, don't wait 30s/try
        if (err.empty()) {
            // Transport succeeded — parse. These are deterministic outcomes, not worth retrying.
            try {
                json j = json::parse(response);
                if (j.contains("results") && !j["results"].empty()) {
                    std::string feed = j["results"][0].value("feedUrl", "");
                    if (!feed.empty()) return feed;
                    last_err = "lookup returned no feedUrl";
                } else {
                    last_err = "lookup returned no results";
                }
            } catch (const std::exception& e) {
                last_err = std::string("JSON parse failed: ") + e.what();
                LOG(fmt::format("[Exception] {}", e.what()));
            }
            break;
        }
        // Transient transport error — retry after backoff.
        last_err = err;
        if (attempt < MAX_ATTEMPTS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(800 * attempt));
        }
    }
    EVENT_LOG(fmt::format(
        "Apple lookup failed after {} tries: {} — check [network] proxy / TLS to itunes.apple.com",
        MAX_ATTEMPTS, last_err));
    return "";
}

// Y01: HTTP POST helper (OAuth device-flow token endpoint, YouTube Data API write calls, InnerTube).
//   body is sent verbatim; content_type defaults to application/x-www-form-urlencoded.
std::string Network::post(const std::string& url, const std::string& body,
                          const std::string& content_type,
                          const std::vector<std::string>& extra_headers,
                          int timeout) {
    CurlRAII curl_raii;
    CURL* curl = curl_raii.handle;
    if (!curl) return "";
    // OAuth token + YouTube API endpoints are all https to fixed google domains; the global
    //   reject_unsafe_url guard is about user-supplied feed URLs, not these hard-coded endpoints.
    std::string data;
    struct curl_slist* headers = NULL;
    std::string ct = content_type.empty() ? "application/x-www-form-urlencoded" : content_type;
    headers = curl_slist_append(headers, ("Content-Type: " + ct).c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    for (const auto& h : extra_headers) headers = curl_slist_append(headers, h.c_str());

    configure_curl(curl, url, headers, timeout);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        EVENT_LOG(fmt::format("Network POST error: {} (URL: {})", curl_easy_strerror(res), url.substr(0, 60)));
    } else if (http_code >= 400) {
        std::string snippet = data.size() > 400 ? data.substr(0, 400) : data;
        LOG(fmt::format("[Network] POST HTTP {} for {} : {}", http_code, url.substr(0, 90), snippet));
    }
    return data;
}

// Y01: GET with Authorization: Bearer (YouTube Data API v3 reads).
std::string Network::fetch_auth(const std::string& url, const std::string& bearer, int timeout) {
    CurlRAII curl_raii;
    CURL* curl = curl_raii.handle;
    if (!curl) return "";
    std::string data;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
    configure_curl(curl, url, headers, timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        EVENT_LOG(fmt::format("Network GET auth error: {} (URL: {})", curl_easy_strerror(res), url.substr(0, 60)));
    } else if (http_code >= 400) {
        // Surface the real Google error (403 accessNotConfigured / 401 invalid_token / 403 quotaExceeded
        //   / 403 forbidden for consent-screen testing scope, etc.) — otherwise fetch_subscriptions etc.
        //   silently return empty and the user sees "no data" with no clue why.
        std::string snippet = data.size() > 400 ? data.substr(0, 400) : data;
        LOG(fmt::format("[Network] fetch_auth HTTP {} for {} : {}", http_code, url.substr(0, 90), snippet));
        EVENT_LOG(fmt::format("YouTube Data API HTTP {} (details in podradio.log)", http_code));
    }
    return data;
}

// Y15: GET with Cookie header (Bilibili API auth).
// Y20: added optional Referer header (Bilibili API requires it for anti-scraping).
std::string Network::fetch_cookie(const std::string& url, const std::string& cookie_header,
                                const std::string& referer, int timeout) {
    CurlRAII curl_raii;
    CURL* curl = curl_raii.handle;
    if (!curl) return "";
    std::string data;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!cookie_header.empty())
        headers = curl_slist_append(headers, ("Cookie: " + cookie_header).c_str());
    if (!referer.empty())
        headers = curl_slist_append(headers, ("Referer: " + referer).c_str());
    configure_curl(curl, url, headers, timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        EVENT_LOG(fmt::format("Network GET cookie error: {} (URL: {})", curl_easy_strerror(res), url.substr(0, 60)));
    } else if (http_code >= 400) {
        // P2-S2: clear 4xx/5xx bodies so WBI/nav callers don't parse HTML error pages as JSON.
        LOG(fmt::format("[Network] fetch_cookie HTTP {} for {}", http_code, url.substr(0, 90)));
        data.clear();
    }
    return data;
}

// Y01: DELETE with Authorization: Bearer (YouTube subscriptions.delete).
std::string Network::del(const std::string& url, const std::string& bearer, int timeout) {
    CurlRAII curl_raii;
    CURL* curl = curl_raii.handle;
    if (!curl) return "";
    std::string data;
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
    configure_curl(curl, url, headers, timeout);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) {
        EVENT_LOG(fmt::format("Network DELETE error: {} (URL: {})", curl_easy_strerror(res), url.substr(0, 60)));
    } else if (http_code >= 400) {
        // P2-S2: surface the YouTube API error (body kept — callers may parse the error JSON).
        LOG(fmt::format("[Network] DELETE HTTP {} for {} : {}", http_code, url.substr(0, 90),
                        data.size() > 400 ? data.substr(0, 400) : data));
    }
    return data;
}

} // namespace podradio
