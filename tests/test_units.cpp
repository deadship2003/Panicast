// V0.05B9n3e5g3r P3-3: Unit test infrastructure
// Tests PaniCast's pure-function modules
// Build: see the BUILD_TESTING option in CMakeLists.txt
//   cmake -DBUILD_TESTING=ON -B build && cmake --build build && ctest --test-dir build

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>

#include "panicast/app/playback_events.h"
#include "panicast/core/event_bus.h"
#include "panicast/core/event_log.h"
#include "panicast/net/proxy_manager.h"
#include "panicast/domain/media.h"
#include "panicast/parsers/feed_parser.h"

// ─── Since panicast.cpp is a single file, we mirror the pure-function logic for independent testing ───
// Real integration tests require splitting the modules (v0.6 work)

namespace panicast_test
{

enum class URLType {
    UNKNOWN,
    OPML,
    RSS_PODCAST,
    YOUTUBE_RSS,
    YOUTUBE_CHANNEL,
    YOUTUBE_VIDEO,
    YOUTUBE_PLAYLIST,
    APPLE_PODCAST,
    RADIO_STREAM,
    VIDEO_FILE
};

struct UrlPattern {
    const char *needle;
    URLType type;
};
static constexpr UrlPattern PATTERNS[] = {
    {"youtube.com/feeds/videos.xml", URLType::YOUTUBE_RSS},
    {"youtube.com/playlist", URLType::YOUTUBE_PLAYLIST},
    {"youtube.com/watch", URLType::YOUTUBE_VIDEO},
    {"youtu.be/", URLType::YOUTUBE_VIDEO},
    {"youtube.com/@", URLType::YOUTUBE_CHANNEL},
    {"youtube.com/channel/", URLType::YOUTUBE_CHANNEL},
    {"youtube.com/c/", URLType::YOUTUBE_CHANNEL},
    {"podcasts.apple.com", URLType::APPLE_PODCAST},
    {".mp4", URLType::VIDEO_FILE},
    {".webm", URLType::VIDEO_FILE},
    {".mkv", URLType::VIDEO_FILE},
    {".avi", URLType::VIDEO_FILE},
    {".mov", URLType::VIDEO_FILE},
    {".m3u8", URLType::RADIO_STREAM},
    {".mp3", URLType::RADIO_STREAM},
    {".aac", URLType::RADIO_STREAM},
    {"Browse.ashx", URLType::OPML},
    {".opml", URLType::OPML},
    {".xml", URLType::RSS_PODCAST},
    {"/feed", URLType::RSS_PODCAST},
    {"/rss", URLType::RSS_PODCAST},
};

URLType classify(const std::string &url) {
    if (url.empty())
        return URLType::UNKNOWN;
    if (url.compare(0, 7, "file://") == 0 || (!url.empty() && url[0] == '/')) {
        return URLType::RADIO_STREAM;
    }
    if (url.find("Tune.ashx") != std::string::npos) {
        if (url.find("c=pbrowse") != std::string::npos ||
            url.find("c=sbrowse") != std::string::npos) {
            return URLType::OPML;
        }
        return URLType::RADIO_STREAM;
    }
    for (const auto &p : PATTERNS) {
        if (url.find(p.needle) != std::string::npos)
            return p.type;
    }
    return URLType::UNKNOWN;
}

} // namespace panicast_test

using namespace panicast_test;

// ─── URLClassifier test cases ───

TEST(URLClassifier, EmptyUrl) {
    EXPECT_EQ(classify(""), URLType::UNKNOWN);
}

TEST(URLClassifier, LocalFile) {
    EXPECT_EQ(classify("file:///home/user/music.mp3"), URLType::RADIO_STREAM);
    EXPECT_EQ(classify("/abs/path/file.mp3"), URLType::RADIO_STREAM);
}

TEST(URLClassifier, YouTubeChannel) {
    EXPECT_EQ(classify("https://youtube.com/@SomeChannel"), URLType::YOUTUBE_CHANNEL);
    EXPECT_EQ(classify("https://youtube.com/channel/UC123456"), URLType::YOUTUBE_CHANNEL);
    EXPECT_EQ(classify("https://youtube.com/c/SomeChannel"), URLType::YOUTUBE_CHANNEL);
}

TEST(URLClassifier, YouTubeVideo) {
    EXPECT_EQ(classify("https://youtube.com/watch?v=dQw4w9WgXcQ"), URLType::YOUTUBE_VIDEO);
    EXPECT_EQ(classify("https://youtu.be/dQw4w9WgXcQ"), URLType::YOUTUBE_VIDEO);
}

TEST(URLClassifier, YouTubePlaylist) {
    EXPECT_EQ(classify("https://youtube.com/playlist?list=PL123"), URLType::YOUTUBE_PLAYLIST);
}

TEST(URLClassifier, YouTubeRSS) {
    EXPECT_EQ(classify("https://youtube.com/feeds/videos.xml?channel_id=UC123"),
              URLType::YOUTUBE_RSS);
}

TEST(URLClassifier, ApplePodcast) {
    EXPECT_EQ(classify("https://podcasts.apple.com/us/podcast/example/id123"),
              URLType::APPLE_PODCAST);
}

TEST(URLClassifier, VideoFiles) {
    EXPECT_EQ(classify("https://example.com/video.mp4"), URLType::VIDEO_FILE);
    EXPECT_EQ(classify("https://example.com/video.webm"), URLType::VIDEO_FILE);
    EXPECT_EQ(classify("https://example.com/video.mkv"), URLType::VIDEO_FILE);
    EXPECT_EQ(classify("https://example.com/video.avi"), URLType::VIDEO_FILE);
    EXPECT_EQ(classify("https://example.com/video.mov"), URLType::VIDEO_FILE);
}

TEST(URLClassifier, AudioStreams) {
    EXPECT_EQ(classify("https://radio.example.com/live.m3u8"), URLType::RADIO_STREAM);
    EXPECT_EQ(classify("https://example.com/podcast.mp3"), URLType::RADIO_STREAM);
    EXPECT_EQ(classify("https://example.com/audio.aac"), URLType::RADIO_STREAM);
}

TEST(URLClassifier, OPML) {
    EXPECT_EQ(classify("https://opml.example.com/Browse.ashx"), URLType::OPML);
    EXPECT_EQ(classify("https://example.com/feeds.opml"), URLType::OPML);
}

TEST(URLClassifier, RSSPodcast) {
    EXPECT_EQ(classify("https://example.com/feed.xml"), URLType::RSS_PODCAST);
    EXPECT_EQ(classify("https://example.com/feed"), URLType::RSS_PODCAST);
    EXPECT_EQ(classify("https://example.com/rss"), URLType::RSS_PODCAST);
}

TEST(URLClassifier, TuneInBrowseVsStream) {
    EXPECT_EQ(classify("https://tunein.com/Tune.ashx?id=123&c=pbrowse"), URLType::OPML);
    EXPECT_EQ(classify("https://tunein.com/Tune.ashx?id=123&c=sbrowse"), URLType::OPML);
    EXPECT_EQ(classify("https://tunein.com/Tune.ashx?id=123"), URLType::RADIO_STREAM);
}

TEST(URLClassifier, UnknownUrl) {
    EXPECT_EQ(classify("https://example.com/some/page"), URLType::UNKNOWN);
    EXPECT_EQ(classify("https://example.com/"), URLType::UNKNOWN);
}

TEST(URLClassifier, PatternPriority) {
    // youtube.com/feeds/videos.xml takes priority over youtube.com/
    EXPECT_EQ(classify("https://youtube.com/feeds/videos.xml?channel_id=UC123"),
              URLType::YOUTUBE_RSS);
    // youtube.com/watch will not be mismatched by youtube.com/
    EXPECT_EQ(classify("https://youtube.com/watch?v=abc"), URLType::YOUTUBE_VIDEO);
}

// ─── Time format parsing tests ───

int parse_time_string(const std::string &s) {
    if (s.empty())
        return -1;
    if (s.back() == 'h' || s.back() == 'H') {
        try {
            return std::stoi(s.substr(0, s.size() - 1)) * 3600;
        } catch (...) {
            return -1;
        }
    }
    if (s.back() == 'm' || s.back() == 'M') {
        try {
            return std::stoi(s.substr(0, s.size() - 1)) * 60;
        } catch (...) {
            return -1;
        }
    }
    if (s.back() == 's' || s.back() == 'S') {
        try {
            return std::stoi(s.substr(0, s.size() - 1));
        } catch (...) {
            return -1;
        }
    }
    if (s.find(':') != std::string::npos) {
        int h = 0, m = 0, sec = 0;
        if (sscanf(s.c_str(), "%d:%d:%d", &h, &m, &sec) == 3)
            return h * 3600 + m * 60 + sec;
        if (sscanf(s.c_str(), "%d:%d", &m, &sec) == 2)
            return m * 60 + sec;
        return -1;
    }
    try {
        int n = std::stoi(s);
        return n < 100 ? n * 3600 : n * 60;
    } catch (...) {
        return -1;
    }
}

TEST(TimeParser, SuffixFormats) {
    EXPECT_EQ(parse_time_string("5h"), 5 * 3600);
    EXPECT_EQ(parse_time_string("30m"), 30 * 60);
    EXPECT_EQ(parse_time_string("90s"), 90);
    EXPECT_EQ(parse_time_string("5H"), 5 * 3600);
    EXPECT_EQ(parse_time_string("30M"), 30 * 60);
}

TEST(TimeParser, HHMMSS) {
    EXPECT_EQ(parse_time_string("1:25:15"), 1 * 3600 + 25 * 60 + 15);
    EXPECT_EQ(parse_time_string("25:15"), 25 * 60 + 15);
    EXPECT_EQ(parse_time_string("0:0:30"), 30);
}

TEST(TimeParser, PureNumber) {
    EXPECT_EQ(parse_time_string("5"), 5 * 3600);
    EXPECT_EQ(parse_time_string("100"), 100 * 60);
    EXPECT_EQ(parse_time_string("99"), 99 * 3600);
}

TEST(TimeParser, InvalidInput) {
    EXPECT_EQ(parse_time_string(""), -1);
    EXPECT_EQ(parse_time_string("abc"), -1);
    EXPECT_EQ(parse_time_string("h"), -1);
}

// ─── escape_sql tests ───

std::string escape_sql(const std::string &s) {
    std::string result;
    result.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\'')
            result += "''";
        else
            result += c;
    }
    return result;
}

TEST(EscapeSql, NormalString) {
    EXPECT_EQ(escape_sql("hello"), "hello");
    EXPECT_EQ(escape_sql(""), "");
}

TEST(EscapeSql, SingleQuote) {
    EXPECT_EQ(escape_sql("it's"), "it''s");
    EXPECT_EQ(escape_sql("' OR 1=1 --"), "'' OR 1=1 --");
    EXPECT_EQ(escape_sql("a'b'c"), "a''b''c");
}

TEST(EscapeSql, SqlInjectionAttempt) {
    std::string malicious = "'; DROP TABLE users; --";
    std::string escaped = escape_sql(malicious);
    EXPECT_EQ(escaped, "''; DROP TABLE users; --");
}

// ─── Y01: crypto primitives (token at-rest encryption) ───────────────────────
#include "panicast/core/crypto.h"
#include <string>

namespace
{
std::string hex(const panicast::bytes &b) {
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t x : b) {
        s.push_back(d[x >> 4]);
        s.push_back(d[x & 0xf]);
    }
    return s;
}
} // namespace

TEST(Y01Crypto, Sha256KnownVector) {
    // NIST: SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    auto h = panicast::sha256(std::string("abc"));
    EXPECT_EQ(hex(h), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Y01Crypto, Base64RoundTrip) {
    std::string msg = "Google OAuth refresh_token #861556708454";
    panicast::bytes b(msg.begin(), msg.end());
    std::string enc = panicast::base64_encode(b.data(), b.size());
    panicast::bytes dec;
    ASSERT_TRUE(panicast::base64_decode(enc, dec));
    std::string back(dec.begin(), dec.end());
    EXPECT_EQ(back, msg);
}

TEST(Y01Crypto, TokenSealOpenRoundTrip) {
    panicast::Key32 k = panicast::machine_key();
    std::string plaintext = "{\"refresh_token\":\"ya29.secret\"}";
    std::string sealed = panicast::token_seal(k, plaintext);
    std::string back;
    ASSERT_TRUE(panicast::token_open(k, sealed, back));
    EXPECT_EQ(back, plaintext);
}

TEST(Y01Crypto, TokenTamperRejected) {
    panicast::Key32 k = panicast::machine_key();
    std::string sealed = panicast::token_seal(k, "secret");
    // Flip one character → HMAC tag must fail verification.
    std::string tampered = sealed;
    tampered[0] = (tampered[0] == 'A') ? 'B' : 'A';
    std::string back;
    EXPECT_FALSE(panicast::token_open(k, tampered, back));
}

TEST(Y01Crypto, WrongMachineKeyRejected) {
    // A different key cannot decrypt what machine_key() sealed.
    panicast::Key32 k0 = panicast::machine_key();
    panicast::Key32 k1 = k0;
    k1[0] ^= 0xff;
    std::string sealed = panicast::token_seal(k0, "secret");
    std::string back;
    EXPECT_FALSE(panicast::token_open(k1, sealed, back));
}

// ─── EventBus tests (D1: type-safe synchronous pub/sub) ───────────────────────
namespace {
struct PeEvent {
    int v;
};
}  // namespace

TEST(EventBus, SubscribeAndPublish) {
    panicast::EventBus &bus = panicast::EventBus::instance();
    int got = -1;
    auto tok = bus.subscribe<PeEvent>([&got](const PeEvent &e) { got = e.v; });
    bus.publish(PeEvent{42});
    EXPECT_EQ(got, 42);
    bus.unsubscribe(tok);
}

TEST(EventBus, MultipleSubscribersAndUnsubscribe) {
    auto &bus = panicast::EventBus::instance();
    int a = 0, b = 0;
    auto ta = bus.subscribe<PeEvent>([&a](const PeEvent &e) { a += e.v; });
    auto tb = bus.subscribe<PeEvent>([&b](const PeEvent &e) { b += e.v; });
    bus.publish(PeEvent{10});
    EXPECT_EQ(a, 10);
    EXPECT_EQ(b, 10);
    bus.unsubscribe(ta);
    bus.publish(PeEvent{5});
    EXPECT_EQ(a, 10);  // ta removed, a unchanged
    EXPECT_EQ(b, 15);  // tb still active
    bus.unsubscribe(tb);
}

TEST(EventBus, PublishWithNoSubscribersIsSafe) {
    auto &bus = panicast::EventBus::instance();
    bus.publish(PeEvent{7});  // no PeEvent subscribers here -> safe no-op
    SUCCEED();
}

TEST(EventBus, LogEventRouting) {
    // The production event type EventLog publishes (D1 first consumer).
    auto &bus = panicast::EventBus::instance();
    std::string received;
    auto tok = bus.subscribe<panicast::LogEvent>([&received](const panicast::LogEvent &e) {
        received = e.msg;
    });
    bus.publish(panicast::LogEvent{"hello-bus"});
    EXPECT_EQ(received, "hello-bus");
    bus.unsubscribe(tok);
}

// ─── Playback events (D9: output-side Core→App messages on the bus) ───────────
TEST(PlaybackEvents, DeliveredOnBus) {
    // The D9 event types PlaybackService publishes; App subscribes. A nullptr node is fine here —
    //   we verify bus delivery + payload, not node validity.
    auto &bus = panicast::EventBus::instance();
    panicast::AppMode got_mode = panicast::AppMode::RADIO;
    bool got_pending = false, track_hit = false;
    int history_hits = 0;

    auto t1 = bus.subscribe<panicast::PlaybackTrackChanged>(
        [&](const panicast::PlaybackTrackChanged &e) { track_hit = true; got_mode = e.mode; });
    auto t2 = bus.subscribe<panicast::PlaybackBufferingChanged>(
        [&](const panicast::PlaybackBufferingChanged &e) { got_pending = e.pending; });
    auto t3 = bus.subscribe<panicast::HistoryChanged>(
        [&](const panicast::HistoryChanged &) { ++history_hits; });

    bus.publish(panicast::PlaybackTrackChanged{panicast::TreeNodePtr{}, panicast::AppMode::IPTV});
    bus.publish(panicast::PlaybackBufferingChanged{true});
    bus.publish(panicast::HistoryChanged{});

    EXPECT_TRUE(track_hit);
    EXPECT_EQ(got_mode, panicast::AppMode::IPTV);
    EXPECT_TRUE(got_pending);
    EXPECT_EQ(history_hits, 1);

    bus.unsubscribe(t1);
    bus.unsubscribe(t2);
    bus.unsubscribe(t3);
}

// ─── ProxyManager tests (D2: Connectivity layer rule chain) ───────────────────
TEST(ProxyManager, GlobalSourceIsUsedWhenNoRules) {
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"socks5h://global:1080"}; });
    EXPECT_EQ(pm.resolveProxy("https://anything.com").url, "socks5h://global:1080");
}

TEST(ProxyManager, DirectWhenNoSource) {
    panicast::ProxyManager pm;
    EXPECT_FALSE(pm.resolveProxy("https://x.com").enabled());  // direct
}

TEST(ProxyManager, PlatformOverridesGlobal) {
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"socks5h://global:1080"}; });
    pm.setPlatform("youtube", {"http://yt:8080"});
    EXPECT_EQ(pm.resolveProxy("https://x.com", "youtube").url, "http://yt:8080");
    EXPECT_EQ(pm.resolveProxy("https://x.com", "bilibili").url, "socks5h://global:1080");
    pm.clearPlatform("youtube");
    EXPECT_EQ(pm.resolveProxy("https://x.com", "youtube").url, "socks5h://global:1080");
}

TEST(ProxyManager, DomainRuleMatches) {
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"socks5h://global:1080"}; });
    pm.setDomain("*.googlevideo.com", {"http://gv:8080"});
    EXPECT_EQ(pm.resolveProxy("https://r1.googlevideo.com/videoplayback").url, "http://gv:8080");
    EXPECT_EQ(pm.resolveProxy("https://googlevideo.com").url, "http://gv:8080");  // apex matches
    EXPECT_EQ(pm.resolveProxy("https://other.com").url, "socks5h://global:1080");  // no match -> global
}

TEST(ProxyManager, PlatformBeatsDomain) {
    panicast::ProxyManager pm;
    pm.setGlobalSource([] { return panicast::ProxyConfig{"g"}; });
    pm.setDomain("*.googlevideo.com", {"gv"});
    pm.setPlatform("youtube", {"yt"});
    EXPECT_EQ(pm.resolveProxy("https://r1.googlevideo.com/v", "youtube").url, "yt");  // platform wins
}

// ─── Media / MediaID tests (D4: domain handle adapter over TreeNode) ──────────
TEST(MediaID, IdentityEquality) {
    auto n1 = std::make_shared<panicast::TreeNode>();
    auto n2 = std::make_shared<panicast::TreeNode>();
    panicast::MediaID a(n1), b(n1), c(n2);
    EXPECT_EQ(a, b);   // same underlying node
    EXPECT_NE(a, c);   // different nodes
    EXPECT_TRUE(a.valid());
    EXPECT_TRUE(a.lock().get() == n1.get());
}

TEST(MediaID, ExpiresWithNode) {
    panicast::MediaID id;
    {
        auto n = std::make_shared<panicast::TreeNode>();
        id = panicast::MediaID(n);
        EXPECT_TRUE(id.valid());
    }
    EXPECT_FALSE(id.valid());  // node destroyed → expired
}

TEST(Media, FromNodeCopiesUrlTitle) {
    auto n = std::make_shared<panicast::TreeNode>();
    n->url = "https://example.com/track";
    n->title = "Track";
    auto m = panicast::media_from_node(n);
    EXPECT_EQ(m.url, "https://example.com/track");
    EXPECT_EQ(m.title, "Track");
    EXPECT_TRUE(m.id.valid());

    auto empty = panicast::media_from_node(nullptr);
    EXPECT_FALSE(empty.id.valid());
    EXPECT_TRUE(empty.url.empty());
}

// ─── ParserRegistry tests (D13: IFeedParser self-registering dispatch contract) ──
// The real feed parsers (RSS/OPML/YouTube) are NOT linked into the test binary, so
// ParserRegistry starts empty here. This isolates the reg()/create() dispatch contract
// from the heavy parser implementations (libxml2/network). Which parsers self-register in
// production is proven by their REGISTER_PARSER macros, not asserted here.
namespace {
class DummyFeedParser : public panicast::IFeedParser {
public:
    explicit DummyFeedParser(panicast::URLType t) : t_(t) {}
    panicast::URLType supports() const override { return t_; }
    panicast::TreeNodePtr parse(const panicast::ParseInput &) override {
        return std::make_shared<panicast::TreeNode>();
    }
private:
    panicast::URLType t_;
};
}  // namespace

TEST(ParserRegistry, SingletonIdentity) {
    auto &a = panicast::ParserRegistry::instance();
    auto &b = panicast::ParserRegistry::instance();
    EXPECT_EQ(&a, &b);
}

TEST(ParserRegistry, CreateIsNullWhenUnregistered) {
    // No real parsers linked -> registry empty. RSS_PODCAST / YOUTUBE_CHANNEL are never
    // registered by any test below, so they stay nullptr regardless of test order.
    EXPECT_EQ(panicast::ParserRegistry::instance().create(panicast::URLType::RSS_PODCAST).get(),
              nullptr);
    EXPECT_EQ(panicast::ParserRegistry::instance().create(panicast::URLType::YOUTUBE_CHANNEL).get(),
              nullptr);
}

TEST(ParserRegistry, RegisterThenDispatch) {
    auto &reg = panicast::ParserRegistry::instance();
    reg.reg(panicast::URLType::OPML,
            []() -> std::unique_ptr<panicast::IFeedParser> {
                return std::make_unique<DummyFeedParser>(panicast::URLType::OPML);
            });
    auto p = reg.create(panicast::URLType::OPML);
    ASSERT_NE(p.get(), nullptr);
    EXPECT_EQ(p->supports(), panicast::URLType::OPML);
    // Registering a second key does not evict the first; each key dispatches its own factory.
    reg.reg(panicast::URLType::YOUTUBE_RSS,
            []() -> std::unique_ptr<panicast::IFeedParser> {
                return std::make_unique<DummyFeedParser>(panicast::URLType::YOUTUBE_RSS);
            });
    EXPECT_EQ(reg.create(panicast::URLType::YOUTUBE_RSS)->supports(),
              panicast::URLType::YOUTUBE_RSS);
    EXPECT_NE(reg.create(panicast::URLType::OPML).get(), nullptr);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
