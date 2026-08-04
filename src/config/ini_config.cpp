#include "panicast/config/ini_config.h"

namespace panicast
{

IniConfig& IniConfig::instance() {
    static IniConfig ic;
    return ic;
}

} // namespace panicast
