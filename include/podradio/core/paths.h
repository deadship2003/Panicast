// Path helper: unified getenv("HOME")+DATA_DIR+... concatenation.
// Reused by all sites such as Logger/SafeTmpFile/Utils/main. Returns empty string when HOME is unset (tmp falls back to /tmp).
#pragma once

#include <cstdlib>
#include <string>

namespace podradio
{

// Path constants (constexpr implies inline linkage; safe to place in a header)
constexpr const char* DATA_DIR    = "/.local/share/podradio";
constexpr const char* CONFIG_DIR  = "/.config/podradio";
constexpr const char* DOWNLOAD_DIR = "/Downloads/PodRadio";

struct Paths {
    static std::string get_data_dir();
    static std::string get_db_file();
    static std::string get_tmp_dir();
};

} // namespace podradio
