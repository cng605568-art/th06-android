#pragma once

#include <string>

namespace th06 {

// Writes a short report (signal, fault address, a backtrace as library+offset) to `path` when
// the process crashes on a fatal signal, then hands the signal on to the previous handler
// (on Android, the system's tombstone writer), so the crash itself proceeds as before. Also
// logs an uncaught C++ exception's message before the program aborts. The launcher offers
// the report (with the session log) for bug reports.
void installCrashHandler(const std::string& path);

}  // namespace th06
