#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

// Tiny logging front end: logcat ("th06" tag) on Android, stderr elsewhere, and optionally a
// session log file (the launcher's bug-report log).
namespace touhou::log {

enum class Level { Debug, Info, Warn, Error };

void write(Level level, std::string_view message);
// Starts the session log file; an existing one is first renamed to previousPath (if given).
bool openFile(const std::string& path, const std::string& previousPath = {});
void closeFile();

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace touhou::log
