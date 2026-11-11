//
// EngineLog.h — the engine's plain log file, ~/Library/Logs/MScopes/engine.log:
// capture start/stop, tap re-opens and why, display-link restarts, and a
// heartbeat every 10 s with the counters, so a freeze that happened while
// nobody was looking can still be explained afterwards. Truncated at 2 MB.
// (os_log from the app was unreliable to read back on this Mac.)
//
#pragma once
#include <os/log.h>

void wvLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
os_log_t wvEngineLog(void);
