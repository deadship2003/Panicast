# ╔═══════════════════════════════════════════════════════════════════════════╗
# ║                    PANICAST CMake Build System                            ║
# ║                   Terminal Podcast/Radio Player                           ║
# ║                 Author: Panic <Deadship2003@gmail.com>                    ║
# ╚═══════════════════════════════════════════════════════════════════════════╝
#
# V0.05B9n3e5g3p: Engineering audit comprehensive fix release
# Cross-platform support: Windows / Linux / macOS
#
# P3-10: Single source of truth for version - authoritative definition here; constants.h and vcpkg.json must stay in sync

cmake_minimum_required(VERSION 3.16)

project(PaniCast
    VERSION 0.1
    DESCRIPTION "Terminal Podcast/Radio Player"
    LANGUAGES CXX
)

# Version information (single source of truth; five locations must stay in sync: CMakeLists.txt / vcpkg.json / include/panicast/core/constants.h / man/panicast.1 / README.md)
# Naming scheme: baseline Panicast_V0.1.
#   - Fix iterations:   Panicast_V0.1-F01 .. -F99  (bug-fix line, parallel)
#   - Y (account) line: Panicast_V0.1-Y01 .. -Y99  (multi-account / Google-login feature line, parallel to F)
#   Empty PANICAST_FIX_SUFFIX = baseline.
set(PANICAST_FIX_SUFFIX "")
set(PANICAST_FULL_VERSION "PaniCast-V0.01")

# V0.05B9n3e5g3p P3-2: Generate version.h via configure_file; source code includes it
# Avoids manually syncing the version in 3 places (old code had CMake=n, vcpkg=n, source=o mismatch)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h
    @ONLY
)

# ── Builtin OAuth client (Y09): auto-detect secrets/client_secret.json at configure time ──
#   If present, bake ITS client_id/client_secret as the builtin default (so other users drop their
#   own client_secret.json into secrets/ and rebuild to replace the builtin). If absent, use the
#   project's fallback client. Runtime <data_dir>/client_secret*.json always overrides the builtin
#   (see google_oauth.cpp client_creds()). secrets/client_secret.json is .gitignored.
# ── Builtin OAuth client (Y09): bake secrets/client_secret.json at configure time ──
#   P1-1 (Y21): the project NO LONGER ships a hardcoded fallback client_secret. The previous
#   fallback secret (committed in Y20 and earlier) MUST be treated as compromised — revoke it in
#   GCP Console and rotate. To get a builtin client, drop your own client_secret.json into
#   secrets/ and rebuild; its client_id/client_secret are baked into the generated header.
#   Without secrets/client_secret.json the builtin is EMPTY and Google login requires a runtime
#   <data_dir>/client_secret*.json (see google_oauth.cpp client_creds()).
#   secrets/client_secret.json and the generated client_secret_builtin.h are both .gitignored.
set(BUILTIN_CLIENT_ID "")
set(BUILTIN_CLIENT_SECRET "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/secrets/client_secret.json")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/secrets/client_secret.json" _cs_json)
    string(REGEX MATCH "\"client_id\"[ \t]*:[ \t]*\"([^\"]+)\"" _cs_id_match "${_cs_json}")
    if(_cs_id_match)
        set(BUILTIN_CLIENT_ID "${CMAKE_MATCH_1}")
    endif()
    string(REGEX MATCH "\"client_secret\"[ \t]*:[ \t]*\"([^\"]+)\"" _cs_secret_match "${_cs_json}")
    if(_cs_secret_match)
        set(BUILTIN_CLIENT_SECRET "${CMAKE_MATCH_1}")
    endif()
    message(STATUS "Builtin OAuth client: baked from secrets/client_secret.json")
else()
    message(WARNING "PaniCast: no secrets/client_secret.json found — builtin OAuth client is EMPTY. "
                    "Google login will require a runtime <data_dir>/client_secret*.json. "
                    "Drop your own client_secret.json into secrets/ and rebuild to bake a builtin client.")
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/panicast/net/client_secret_builtin.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/include/panicast/net/client_secret_builtin.h
    @ONLY
)

# =============================================================================
# C++ standard settings
# =============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Build type
# =============================================================================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "PaniCast version: ${PANICAST_FULL_VERSION}")

# =============================================================================
# Platform detection
# =============================================================================
# V0.05B9n3e5g3p P3-9: Use target_compile_definitions instead of deprecated add_definitions
# Platform macros will be defined on the panicast target (see below)
if(WIN32)
    set(PANICAST_PLATFORM "Windows")
    set(PANICAST_PLATFORM_DEFINE PANICAST_WINDOWS)
elseif(APPLE)
    set(PANICAST_PLATFORM "macOS")
    set(PANICAST_PLATFORM_DEFINE PANICAST_MACOS)
elseif(UNIX)
    set(PANICAST_PLATFORM "Linux")
    set(PANICAST_PLATFORM_DEFINE PANICAST_LINUX)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

message(STATUS "Target platform: ${PANICAST_PLATFORM}")

# =============================================================================
# Find dependencies
# =============================================================================
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# MPV
if(PkgConfig_FOUND)
    pkg_check_modules(MPV IMPORTED_TARGET mpv QUIET)
endif()
if(NOT MPV_FOUND)
    find_library(MPV_LIBRARY NAMES mpv)
    if(MPV_LIBRARY)
        set(MPV_FOUND TRUE)
    endif()
endif()

# SQLite3 - prefer pkg-config to avoid deprecated-target warnings
if(PkgConfig_FOUND)
    pkg_check_modules(SQLITE3_PKG IMPORTED_TARGET sqlite3 QUIET)
    if(SQLITE3_PKG_FOUND)
        set(SQLite3_FOUND TRUE)
        set(SQLITE3_LINK_LIB PkgConfig::SQLITE3_PKG)
    endif()
endif()
if(NOT SQLite3_FOUND)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        # Prefer the new target name SQLite3::SQLite3 to avoid deprecation warnings
        if(TARGET SQLite3::SQLite3)
            set(SQLITE3_LINK_LIB SQLite3::SQLite3)
        elseif(TARGET SQLite::SQLite3)
            set(SQLITE3_LINK_LIB SQLite::SQLite3)
        endif()
    endif()
endif()
if(NOT SQLite3_FOUND)
    find_library(SQLITE3_LIBRARY NAMES sqlite3)
    if(SQLITE3_LIBRARY)
        set(SQLite3_FOUND TRUE)
        set(SQLITE3_LINK_LIB ${SQLITE3_LIBRARY})
    endif()
endif()

# CURL
find_package(CURL QUIET)

# ── libqrencode (Y01: render Google OAuth verification URL as a terminal QR) ──
# Optional: if absent, the Y-mode login falls back to a plain-text user_code (no QR image).
#   Searched via pkg-config first, then a plain find_library/find_path.
set(QRENCODE_FOUND FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(QRENCODE IMPORTED_TARGET libqrencode QUIET)
endif()
if(QRENCODE_FOUND)
    set(HAVE_QRENCODE TRUE)
elseif(UNIX)
    find_path(QRENCODE_INCLUDE_DIR qrencode.h)
    find_library(QRENCODE_LIBRARY NAMES qrencode)
    if(QRENCODE_INCLUDE_DIR AND QRENCODE_LIBRARY)
        set(HAVE_QRENCODE TRUE)
    endif()
endif()
if(HAVE_QRENCODE)
    message(STATUS "libqrencode: Found (QR login enabled)")
else()
    message(WARNING "libqrencode not found - Y-mode QR login falls back to text user_code")
endif()
if(NOT CURL_FOUND)
    find_library(CURL_LIBRARY NAMES curl)
    if(CURL_LIBRARY)
        set(CURL_FOUND TRUE)
    endif()
endif()

# libxml2
find_package(LibXml2 QUIET)
if(NOT LibXml2_FOUND)
    find_library(XML2_LIBRARY NAMES xml2)
    if(XML2_LIBRARY)
        set(LibXml2_FOUND TRUE)
    endif()
endif()

# fmt
find_package(fmt QUIET)

# nlohmann_json
find_package(nlohmann_json QUIET)

# ncurses (Linux/macOS)
# V0.05B9n3e5g3z: Force the wide-character library (libncursesw). The narrow library (libncurses),
#   even under a UTF-8 locale, causes CJK/Emoji garbled output (waddstr does no multi-byte combining).
#   CURSES_NEED_WIDE makes find_package(Curses) pick libncursesw; the fallback also prefers ncursesw.
if(UNIX)
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses QUIET)
    if(NOT CURSES_FOUND)
        find_library(NCURSES_LIBRARY NAMES ncursesw ncurses)
        if(NCURSES_LIBRARY)
            set(CURSES_FOUND TRUE)
            set(CURSES_LIBRARIES ${NCURSES_LIBRARY})
        endif()
    endif()
endif()

# =============================================================================
# Compiler options
# V0.05B9n3e5g3p P3-9: Use target_compile_options + generator expressions
# Old code: add_compile_options(-O3) unconditionally overrode build type, so Debug was effectively -O3 -g
# =============================================================================
if(MSVC)
    set(COMPILER_FLAGS /W4 /utf-8)
    set(COMPILER_DEFS _CRT_SECURE_NO_WARNINGS)
else()
    set(COMPILER_FLAGS -Wall -Wextra -Wpedantic)
    # Distinguish build type via generator expressions
    set(COMPILER_FLAGS ${COMPILER_FLAGS}
        $<$<CONFIG:Release>:-O2>
        $<$<CONFIG:RelWithDebInfo>:-O2;-g>
        $<$<CONFIG:Debug>:-O0;-g>
    )
endif()

# =============================================================================
# Executable
# V0.05B9n3e5g3p P3-1: Keep a single source file (splitting into multiple files is v0.6 work)
# But add generated/ to the include path to support version.h injection
# =============================================================================
add_executable(panicast
    # Panicast: renamed binary output (target stays 'panicast' internally)
    src/main.cpp
    src/core/paths.cpp
    src/core/logger.cpp
    src/core/event_log.cpp
    src/core/thread_pool.cpp
    src/core/safe_tmp.cpp
    src/core/terminal.cpp
    src/core/path_utils.cpp
    src/core/process_utils.cpp
    src/core/text_utils.cpp
    src/net/url_classifier.cpp
    src/config/ini_config.cpp
    src/net/url_guard.cpp
    src/net/ytdlp_runner.cpp
    src/net/network.cpp
    src/net/proxy_manager.cpp
    src/net/remote_command_bus.cpp
    src/net/remote_server.cpp
    src/net/remote_session.cpp
    src/net/remote_ws.cpp
    src/net/google_oauth.cpp
    src/core/crypto.cpp
    src/storage/database.cpp
    src/storage/history_repo.cpp
    src/storage/tree_repo.cpp
    src/storage/feed_cache_repo.cpp
    src/storage/account_repo.cpp
    src/storage/player_state_repo.cpp
    src/storage/accounts.cpp
    src/storage/cache.cpp
    src/storage/youtube_cache.cpp
    src/storage/persistence.cpp
    src/parsers/feed_parser.cpp
    src/parsers/xml_helpers.cpp
    src/parsers/itunes_search.cpp
    src/parsers/opml_parser.cpp
    src/parsers/rss_parser.cpp
    src/parsers/youtube_channel_parser.cpp
    src/parsers/bilibili_parser.cpp
    src/parsers/transcript_parser.cpp
    src/parsers/m3u_parser.cpp
    src/subtitle/subtitle_parser.cpp
    src/subtitle/subtitle_manager.cpp
    src/playback/mpv_controller.cpp
    src/playback/sleep_timer.cpp
    src/ui/icons.cpp
    src/ui/art.cpp
    src/ui/layout_metrics.cpp
    src/ui/layout_guard.cpp
    src/ui/border.cpp
    src/ui/ui.cpp
    src/ui/popups.cpp
    src/ui/lyric_renderer.cpp
    src/ui/tree_renderer.cpp
    src/ui/status_bar.cpp
    src/ui/info_panel.cpp
    src/theme/colors.cpp
    src/theme/themes.cpp
    src/net/bilibili_api.cpp
    src/net/tiktok_region.cpp
    src/subtitle/transcription_engine.cpp
    src/app/progress.cpp
    src/app/online_state.cpp
    src/app/app_playback.cpp
    src/app/playback_service.cpp
    src/app/subtitle_service.cpp
    src/app/search_service.cpp
    src/app/library_service.cpp
    src/app/app_download.cpp
    src/app/app_search.cpp
    src/app/app_subscriptions.cpp
    src/app/app_navigation.cpp
    src/app/app_tree_expand.cpp
    src/app/app_input.cpp
    src/app/app_nodes.cpp
    src/app/app_run.cpp
    src/app/app_remote.cpp
    src/app/app_account.cpp
    src/app/app_bilibili.cpp
    src/app/app_tiktok.cpp
    src/app/app_iptv.cpp
    src/app/app_sync.cpp
    src/ui/qr.cpp
)
set_target_properties(panicast PROPERTIES OUTPUT_NAME "panicast")

target_compile_options(panicast PRIVATE ${COMPILER_FLAGS})
target_compile_definitions(panicast PRIVATE ${PANICAST_PLATFORM_DEFINE} ${COMPILER_DEFS})

target_include_directories(panicast PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# =============================================================================
# Link libraries
# =============================================================================
target_link_libraries(panicast PRIVATE
    Threads::Threads
)

# MPV
if(MPV_FOUND)
    if(TARGET PkgConfig::MPV)
        target_link_libraries(panicast PRIVATE PkgConfig::MPV)
    elseif(MPV_LIBRARY)
        target_link_libraries(panicast PRIVATE ${MPV_LIBRARY})
    endif()
    message(STATUS "MPV: Found")
else()
    message(WARNING "MPV not found - media playback disabled")
endif()

# SQLite3
if(SQLITE3_LINK_LIB)
    target_link_libraries(panicast PRIVATE ${SQLITE3_LINK_LIB})
    message(STATUS "SQLite3: Found (${SQLITE3_LINK_LIB})")
elseif(SQLite3_FOUND)
    message(WARNING "SQLite3 found but no link target available")
else()
    message(WARNING "SQLite3 not found - database features disabled")
endif()

# CURL
if(CURL_FOUND)
    if(TARGET CURL::libcurl)
        target_link_libraries(panicast PRIVATE CURL::libcurl)
    elseif(CURL_LIBRARY)
        target_link_libraries(panicast PRIVATE ${CURL_LIBRARY})
    endif()
    message(STATUS "CURL: Found")
else()
    message(WARNING "CURL not found - network features disabled")
endif()

# libxml2
if(LibXml2_FOUND)
    if(TARGET LibXml2::LibXml2)
        target_link_libraries(panicast PRIVATE LibXml2::LibXml2)
    elseif(XML2_LIBRARY)
        target_link_libraries(panicast PRIVATE ${XML2_LIBRARY})
    endif()
    message(STATUS "libxml2: Found")
else()
    message(WARNING "libxml2 not found - RSS parsing disabled")
endif()

# fmt
if(fmt_FOUND)
    target_link_libraries(panicast PRIVATE fmt::fmt)
    message(STATUS "fmt: Found")
else()
    message(STATUS "fmt: Using bundled version")
endif()

# nlohmann_json
if(nlohmann_json_FOUND)
    target_link_libraries(panicast PRIVATE nlohmann_json::nlohmann_json)
    message(STATUS "nlohmann_json: Found")
else()
    message(STATUS "nlohmann_json: Using bundled version")
endif()

# ncurses
if(UNIX)
    if(CURSES_FOUND)
        target_link_libraries(panicast PRIVATE ${CURSES_LIBRARIES})
        target_include_directories(panicast PRIVATE ${CURSES_INCLUDE_DIR})
        message(STATUS "ncurses: Found")
    elseif(NCURSES_LIBRARY)
        target_link_libraries(panicast PRIVATE ${NCURSES_LIBRARY})
        message(STATUS "ncurses: Found (${NCURSES_LIBRARY})")
    else()
        message(WARNING "ncurses not found - TUI disabled")
    endif()
endif()

# libqrencode (Y01: QR rendering for Google OAuth device-flow login)
if(HAVE_QRENCODE)
    if(TARGET PkgConfig::QRENCODE)
        target_link_libraries(panicast PRIVATE PkgConfig::QRENCODE)
    else()
        target_include_directories(panicast PRIVATE ${QRENCODE_INCLUDE_DIR})
        target_link_libraries(panicast PRIVATE ${QRENCODE_LIBRARY})
    endif()
    target_compile_definitions(panicast PRIVATE HAVE_QRENCODE=1)
endif()

# Y18: OpenSSL libcrypto (MD5 for Bilibili WBI signing in bilibili_api.cpp)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_link_libraries(panicast PRIVATE OpenSSL::Crypto)
else()
    target_link_libraries(panicast PRIVATE crypto)
endif()

# =============================================================================
# Install
# =============================================================================
install(TARGETS panicast
    RUNTIME DESTINATION bin
)

install(FILES README.md LICENSE
    DESTINATION share/doc/panicast
)

# Install man page
if(UNIX)
    install(FILES man/panicast.1
        DESTINATION share/man/man1
    )
endif()

# =============================================================================
# Packaging
# =============================================================================
set(CPACK_PACKAGE_NAME "PaniCast")
set(CPACK_PACKAGE_VERSION ${PANICAST_FULL_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Terminal Podcast/Radio Player")
set(CPACK_PACKAGE_VENDOR "Panic")
set(CPACK_PACKAGE_CONTACT "Deadship2003@gmail.com")

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "TGZ;DEB")
    # V0.05B9n3e5g3r P1-10: Fill in missing DEB dependencies (libfmt/nlohmann-json3)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "mpv,libsqlite3-0,libcurl4,libxml2,libncursesw6,libfmt9,nlohmann-json3")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
endif()

include(CPack)

# =============================================================================
# V0.05B9n3e5g3r P3-3: Unit test infrastructure
# Enable with: cmake -DBUILD_TESTING=ON -B build && cmake --build build && ctest --test-dir build
# =============================================================================
option(BUILD_TESTING "Build unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    # Find GoogleTest (vcpkg provides GTest::gtest_main)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        # crypto.cpp is dependency-free (no OpenSSL) but transitively needs
        # paths.cpp (Paths::get_data_dir) + logger.cpp (Logger) + fmt.
        add_executable(test_units
            tests/test_units.cpp
            src/core/crypto.cpp
            src/core/paths.cpp
            src/core/logger.cpp
            src/net/proxy_manager.cpp
            src/parsers/feed_parser.cpp)  # D13: ParserRegistry dispatch contract test
        # D14-test: link the real URLClassifier (pure, dep-free) so URLClassifier tests stop
        #   mirroring classify/PATTERNS by hand (the copy had already drifted from production).
        target_sources(test_units PRIVATE src/net/url_classifier.cpp)
        target_link_libraries(test_units PRIVATE GTest::gtest_main Threads::Threads)
        if(fmt_FOUND)
            target_link_libraries(test_units PRIVATE fmt::fmt)
        endif()
        target_include_directories(test_units PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_compile_options(test_units PRIVATE -Wall -Wextra -Wpedantic)
        include(GoogleTest)
        gtest_discover_tests(test_units)
        message(STATUS "Unit tests: enabled (test_units)")
    else()
        message(WARNING "GoogleTest not found - unit tests disabled. Install with: vcpkg install gtest")
    endif()
endif()

# =============================================================================
# Configuration summary
# =============================================================================
message(STATUS "")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "  PaniCast ${PANICAST_FULL_VERSION} Configuration Summary")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "  Platform:     ${PANICAST_PLATFORM}")
message(STATUS "  Build Type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:     ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "")
message(STATUS "  Dependencies:")
message(STATUS "    MPV:       ${MPV_FOUND}")
message(STATUS "    SQLite3:   ${SQLite3_FOUND}")
message(STATUS "    CURL:      ${CURL_FOUND}")
message(STATUS "    libxml2:   ${LibXml2_FOUND}")
message(STATUS "    ncurses:   ${CURSES_FOUND}")
message(STATUS "    fmt:       ${fmt_FOUND}")
message(STATUS "    json:      ${nlohmann_json_FOUND}")
message(STATUS "═══════════════════════════════════════════════════════════════")
message(STATUS "")
