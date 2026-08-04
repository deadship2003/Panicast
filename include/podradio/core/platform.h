// Platform-related inline utilities: thread-safe local time conversion.
// POSIX uses localtime_r, Windows MSVC uses localtime_s (parameter order is reversed compared to POSIX).
// Never degenerate to localtime() — it returns a pointer to a shared static buffer, which gets overwritten on multicore concurrency.
#pragma once

#include <ctime>

namespace podradio
{

inline struct tm* localtime_local(const std::time_t* t, struct tm* out) {
#if defined(_WIN32)
    return localtime_s(out, t) == 0 ? out : nullptr;
#else
    return localtime_r(t, out);
#endif
}

} // namespace podradio
