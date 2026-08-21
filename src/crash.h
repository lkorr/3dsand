// crash.h — minimal Windows crash handler.
// Catches unhandled exceptions, writes a stack trace to crash.log,
// shows a summary MessageBox, and terminates.
#pragma once

/// Call once, early in main(). Installs a SetUnhandledExceptionFilter
/// handler that logs to crash.log and shows a dialog before exiting.
void InstallCrashHandler();
