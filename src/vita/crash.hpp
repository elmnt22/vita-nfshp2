// Crash records for unattended device runs (kubridge v0.3+ exception
// handlers; kubridge.skprx in the user's taiHEN config). A data abort,
// prefetch abort or undefined instruction writes a short record to
// ux0:data/NFSHP2N/crash.txt (registers, fault address, thread, stack words)
// and ends the process, so no crash-dump popup waits to be closed. Without
// kubridge, the system's crash dump remains as before.
//
// crash.txt is truncated at start-up: an empty file after a run means no crash.
// A file watchdog_dump.txt in the data directory makes the stall watchdog
// leave a full system core dump (every thread) instead of exiting.
#pragma once

namespace vita_crash {
using LogFn = void (*)(const char* event, const char* detail);
// Opens <dir>/crash.txt and registers the handlers and std::terminate hook.
bool Init(const char* dir, LogFn log);
// A fatal condition found by the port (not a fault): records msg and exits.
[[noreturn]] void Fatal(const char* msg);
// The stall watchdog: records msg and exits, or (with watchdog_dump.txt)
// steps the handler aside and faults so the system writes a core dump.
[[noreturn]] void Stalled(const char* msg);
} // namespace vita_crash
