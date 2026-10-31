#include "core/Log.hpp"

#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#else
#include <cstdio>
#endif

namespace th06::log {

void write(Level level, std::string_view message) {
#if defined(__ANDROID__)
    int priority = ANDROID_LOG_INFO;
    switch (level) {
        case Level::Debug: priority = ANDROID_LOG_DEBUG; break;
        case Level::Info: priority = ANDROID_LOG_INFO; break;
        case Level::Warn: priority = ANDROID_LOG_WARN; break;
        case Level::Error: priority = ANDROID_LOG_ERROR; break;
    }
    const std::string text(message);
    __android_log_write(priority, "th06", text.c_str());
#else
    static constexpr const char* kNames[] = {"D", "I", "W", "E"};
    std::fprintf(stderr, "[%s] %.*s\n", kNames[static_cast<int>(level)],
                 static_cast<int>(message.size()), message.data());
#endif
}

}  // namespace th06::log
