// Safe temporary file: creates an anonymous temp file under ~/.local/share/podradio/tmp/.
#pragma once

#include <string>

namespace podradio
{

class SafeTmpFile {
public:
    // Creates an anonymous temp file under ~/.local/share/podradio/tmp/ and returns its path; returns an empty string on failure.
    static std::string create(const std::string& suffix = ".m3u");
    static void remove(const std::string& path);
    static std::string tmp_dir();
    static void ensure_tmp_dir();
};

} // namespace podradio
