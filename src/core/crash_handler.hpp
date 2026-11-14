#pragma once

namespace core::crash_handler {
// Writes a crash report (exception/signal, faulting module + offset, call
// stack) to the log and flushes it when the proxy crashes or an exception
// escapes, so the session log shows what happened.
void install();
}
