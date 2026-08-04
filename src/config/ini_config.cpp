#include "podradio/config/ini_config.h"

namespace podradio
{

IniConfig& IniConfig::instance() {
    static IniConfig ic;
    return ic;
}

} // namespace podradio
