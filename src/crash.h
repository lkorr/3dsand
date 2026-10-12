// crash.h — minimal Windows crash handler.
// Catches unhandled exceptions, writes a stack trace to crash.log,
// shows a summary MessageBox, and terminates.
#pragma once

/// Call once, early in main(). Installs a SetUnhandledExceptionFilter
/// handler that logs to crash.log and shows a dialog before exiting.
void InstallCrashHandler();

/// Print `text` to stderr AND keep it for the crash report, then abort(). A
/// deliberate fatal's reason is otherwise only on stderr, which a game
/// launched from the tuner's Play button does not keep: crash.log said
/// "the reason was printed to stderr" and nothing held it (2026-10-11, a
/// solute store fault).
[[noreturn]] void FatalAbort(const char* text);

/// Total CPU seconds this PROCESS has consumed on every thread, user + kernel
/// (GetProcessTimes). The startup timeline in main.cpp prints its delta per
/// phase: it is the column that tells a GPU wait (wall >> cpu) from a driver
/// thread pool compiling pipelines (cpu >> wall). Lives here because this is
/// the one TU that already includes <windows.h>; main.cpp cannot (it has a
/// variable named `far`, which windows.h defines as a macro).
double ProcessCpuSeconds();
