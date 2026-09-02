// Global application constants: name, version, author, etc.
// VERSION prefers version.h (CMake single source); falls back to hardcoded value when missing.
#pragma once

#include <cstdlib>

// Pull in the CMake-generated version macros (single source of truth) when available, so VERSION
//   tracks PANICAST_FIX_SUFFIX without a manual fallback sync. Falls back below for direct g++.
#if __has_include("version.h")
#include "version.h"
#endif

namespace panicast
{

constexpr const char *APP_NAME = "Panicast";
constexpr const char *AUTHOR = "Panic";
constexpr const char *EMAIL = "Deadship2003@gmail.com";
// BUILD_TIME only used for print_usage CLI banner; status bar uses real-time clock
constexpr const char *BUILD_TIME = __DATE__ " " __TIME__; // Auto-generated at compile time

// VERSION prefers the macro from version.h (CMake single source)
// If version.h doesn't exist (e.g. direct g++ compile), fall back to hardcoded value
#ifdef PANICAST_VERSION_STRING
constexpr const char *VERSION = PANICAST_VERSION_STRING;
#else
constexpr const char *VERSION = "Panicast-V0.0.1";
#endif

// ─── Player configuration constants (eliminate magic numbers)────────────────────────────────────────
constexpr int MAX_VOLUME = 100;       // Maximum volume
constexpr int VOLUME_STEP = 5;        // Volume adjustment step
constexpr double MIN_SPEED = 0.2;     // Minimum playback speed
constexpr double MAX_SPEED = 3.0;     // Maximum playback speed
constexpr double DEFAULT_SPEED = 1.0; // Default playback speed
constexpr double SPEED_STEP = 0.1;    // Speed adjustment step

// ─── Download/progress configuration constants ────────────────────────────────────────────────
constexpr int DOWNLOAD_COMPLETE_DISPLAY_SEC =
    5; // Display duration after a successful download (seconds)
constexpr int DOWNLOAD_FAILED_DISPLAY_SEC =
    8; // How long a FAILED entry stays visible (seconds; 5–10 range)
constexpr int MAX_CONCURRENT_DOWNLOADS =
    10; // Max simultaneously active/visible download slots; excess are queued

// ─── UI configuration constants ──────────────────────────────────────────────────────
constexpr int PAGE_SCROLL_LINES = 10; // Page scroll line count

} // namespace panicast
