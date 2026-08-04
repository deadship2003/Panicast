#include "podradio/core/paths.h"

namespace podradio
{

std::string Paths::get_data_dir() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) + DATA_DIR : "";
}

std::string Paths::get_db_file() {
    std::string d = get_data_dir();
    return d.empty() ? "" : d + "/podradio.db";
}

std::string Paths::get_tmp_dir() {
    const char* h = std::getenv("HOME");
    if (!h) h = "/tmp";  // Fall back to /tmp when HOME is unset (preserve old behavior)
    return std::string(h) + DATA_DIR + "/tmp";
}

} // namespace podradio
