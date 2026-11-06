#include "core/Log.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace touhou::log {
namespace {
// The session log file (for bug reports): everything that goes to logcat / stderr, with the
// seconds since it was opened. Capped so a runaway message cannot fill the storage.
constexpr long kMaxFileBytes = 2L << 20;
std::mutex g_fileMutex;
std::FILE* g_file = nullptr;
std::chrono::steady_clock::time_point g_opened;
constexpr const char* kNames[] = {"D", "I", "W", "E"};

void writeFile(Level level, std::string_view message) {
    std::lock_guard lock(g_fileMutex);
    if (!g_file || std::ftell(g_file) > kMaxFileBytes) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_opened).count();
    std::fprintf(g_file, "%9.3f [%s] %.*s\n", t, kNames[static_cast<int>(level)], static_cast<int>(message.size()),
                 message.data());
    std::fflush(g_file);  // a crash must not lose the last lines
}
}  // namespace

bool openFile(const std::string& path, const std::string& previousPath) {
    std::lock_guard lock(g_fileMutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
    if (!previousPath.empty()) std::rename(path.c_str(), previousPath.c_str());  // keep the last session
    g_file = std::fopen(path.c_str(), "w");
    g_opened = std::chrono::steady_clock::now();
    return g_file != nullptr;
}

void closeFile() {
    std::lock_guard lock(g_fileMutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

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
    std::fprintf(stderr, "[%s] %.*s\n", kNames[static_cast<int>(level)], static_cast<int>(message.size()), message.data());
#endif
    writeFile(level, message);
}

}  // namespace touhou::log
