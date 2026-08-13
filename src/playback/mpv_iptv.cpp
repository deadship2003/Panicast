// MPV playback IPTV-diagnostics layer — extracted implementation unit (D20 god-object split).
//   IPTV off-air / audio-only / slow / load-failure detection: the case-insensitive log
//   keyword matcher (log_has, file-local static — used only here) + the -13 sub-classifier
//   + per-track detection reset + the IPTV-context toggle. They remain MPVController members
//   (they read private detection state: last_log_text_/iptv_context_/the one-shot flags);
//   only their implementations live here. Declarations stay in mpv_controller.h.
//   Mechanical verbatim move from mpv_controller.cpp.
#include "panicast/playback/mpv_controller.h"

#include <cctype>
#include <string>

namespace panicast
{

// Y24.55: case-insensitive substring search over the last mpv warn/error log line, used to
//   sub-classify a -13 loading-failed into 404 / 403 / 5xx / unreachable for the IPTV context message.
static bool log_has(const std::string &text, const char *needle) {
    if (!needle || !*needle)
        return false;
    std::string hay = text, nd = needle;
    for (auto &c : hay)
        c = (char)std::tolower((unsigned char)c);
    for (auto &c : nd)
        c = (char)std::tolower((unsigned char)c);
    return hay.find(nd) != std::string::npos;
}

// Y24.55: map a -13 loading-failed to an IPTV context message using the last mpv log line's keywords.
//   mpv/ffmpeg log text usually carries the HTTP status or connection error, so we can tell the user
//   WHY the load failed rather than just "loading failed". Falls back to #1 (unreachable) when no
//   keyword matches — the most common -13 cause and reasonable generic advice.
std::string MPVController::classify_iptv_load_error_() const {
    const std::string &t = last_log_text_;
    // #2 channel not found
    if (log_has(t, "404") || log_has(t, "not found"))
        return "IPTV: channel not found — 404, address invalid or removed";
    // #3 access denied
    if (log_has(t, "403") || log_has(t, "forbidden") || log_has(t, "unauthorized"))
        return "IPTV: access denied — 403, region-restricted or authorization required";
    // #4 server error
    if (log_has(t, "500") || log_has(t, "502") || log_has(t, "503") || log_has(t, "504") ||
        log_has(t, "server error") || log_has(t, "service unavailable"))
        return "IPTV: server error — 5xx, source unavailable; retry later";
    // #1 unreachable (connection-level)
    if (log_has(t, "refused") || log_has(t, "timed out") || log_has(t, "timeout") ||
        log_has(t, "resolve") || log_has(t, "unreachable") || log_has(t, "connection reset") ||
        log_has(t, "no route"))
        return "IPTV: server unreachable — network, DNS, or timeout; check connection or switch "
               "source";
    // generic fallback (still #1 family — address/network/access)
    return "IPTV: server unreachable — network, DNS, or timeout; check connection or switch source";
}

// Y24.55: pick the IPTV context message for an END_FILE r=4 error code. Empty = no IPTV message
//   (for codes with no useful IPTV framing, e.g. -1/-2/-3). The MPV: behavior message is emitted
//   separately and unconditionally by the END_FILE handler; this only adds the IPTV explanation.
std::string MPVController::iptv_message_for_error_(int error) const {
    switch (error) {
    case -13:
        return classify_iptv_load_error_(); // #1/#2/#3/#4 by log keywords
    case -14:
        return "IPTV: audio output init failed — no audio device; check [mpv] ao, PulseAudio, WSLg";
    case -15:
        return "IPTV: video output init failed — no display, falling back to audio";
    case -16:
        return "IPTV: empty playlist — no playable stream for this channel";
    case -17:
        return "IPTV: cannot decode stream — missing decoder; ensure ffmpeg is installed";
    default:
        return std::string(); // no IPTV framing for other codes
    }
}

void MPVController::set_iptv_context(bool on) {
    iptv_context_ = on;
}

// Y24.55: reset per-track IPTV detection state. Called at every load entry point (play/play_list/
//   play_list_from) so one-shots re-arm for each new channel and stale timing from the previous
//   track doesn't carry over.
// D20: dropped the spurious `inline` from the definition (the header declaration was never
//   inline). It only compiled before because the definition + all callers lived in one TU;
//   moving the definition to this sibling TU broke the link until definition matched the
//   (non-inline) declaration.
void MPVController::reset_iptv_detection_() {
    last_log_text_.clear();
    file_loaded_time_set_ = false;
    stuck_timing_ = false;
    offair_reported_ = false;
    audio_only_reported_ = false;
    slow_reported_ = false;
    had_playback_started_ = false;
}
} // namespace panicast
