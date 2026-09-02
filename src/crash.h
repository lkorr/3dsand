// crash.h — minimal Windows crash handler.
// Catches unhandled exceptions, writes a stack trace to crash.log,
// shows a summary MessageBox, and terminates.
#pragma once

/// Call once, early in main(). Installs a SetUnhandledExceptionFilter
/// handler that logs to crash.log and shows a dialog before exiting.
void InstallCrashHandler();

/// Total CPU seconds this PROCESS has consumed on every thread, user + kernel
/// (GetProcessTimes). The startup timeline in main.cpp prints its delta per
/// phase: it is the column that tells a GPU wait (wall >> cpu) from a driver
/// thread pool compiling pipelines (cpu >> wall). Lives here because this is
/// the one TU that already includes <windows.h>; main.cpp cannot (it has a
/// variable named `far`, which windows.h defines as a macro).
double ProcessCpuSeconds();
