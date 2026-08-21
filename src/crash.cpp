// crash.cpp — minimal Windows crash handler using SetUnhandledExceptionFilter.
// Writes crash details (timestamp, exception, registers, stack trace) to
// crash.log, shows a MessageBox summary, and terminates.

#include "crash.h"

#include <cstdio>
#include <ctime>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

namespace {

const char* ExceptionName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:            return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_IN_PAGE_ERROR:         return "EXCEPTION_IN_PAGE_ERROR";
    default:                              return "UNKNOWN";
  }
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  char timeBuf[64];
  {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &lt);
  }

  DWORD code = ep->ExceptionRecord->ExceptionCode;
  void* addr = ep->ExceptionRecord->ExceptionAddress;

  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(proc, NULL, TRUE);

  constexpr int kMaxFrames = 64;
  void* frames[kMaxFrames];
  USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, frames, NULL);

  FILE* f = nullptr;
  fopen_s(&f, "crash.log", "a");

  char summary[2048];
  int sOff = 0;

  auto emit = [&](const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (f) vfprintf(f, fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    int room = (int)sizeof(summary) - sOff - 1;
    if (room > 0) sOff += vsnprintf(summary + sOff, room, fmt, ap);
    va_end(ap);
  };

  emit("=== CRASH at %s ===\n", timeBuf);
  emit("Exception 0x%08X (%s) at 0x%p\n\n", code, ExceptionName(code), addr);

#ifdef _M_X64
  CONTEXT* ctx = ep->ContextRecord;
  emit("Registers:\n");
  emit("  RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX\n",
       ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
  emit("  RSI=%016llX  RDI=%016llX  RBP=%016llX  RSP=%016llX\n",
       ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
  emit("  R8 =%016llX  R9 =%016llX  R10=%016llX  R11=%016llX\n",
       ctx->R8,  ctx->R9,  ctx->R10, ctx->R11);
  emit("  R12=%016llX  R13=%016llX  R14=%016llX  R15=%016llX\n",
       ctx->R12, ctx->R13, ctx->R14, ctx->R15);
  emit("  RIP=%016llX  EFLAGS=%08X\n\n", ctx->Rip, ctx->EFlags);
#endif

  emit("Stack trace (%u frames):\n", (unsigned)frameCount);

  alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = MAX_SYM_NAME;

  for (USHORT i = 0; i < frameCount; i++) {
    DWORD64 frameAddr = (DWORD64)frames[i];
    DWORD64 displacement64 = 0;

    if (SymFromAddr(proc, frameAddr, &displacement64, sym)) {
      IMAGEHLP_LINE64 line = {};
      line.SizeOfStruct = sizeof(line);
      DWORD displacement32 = 0;
      if (SymGetLineFromAddr64(proc, frameAddr, &displacement32, &line)) {
        emit("  [%2u] %s +0x%llX  (%s:%lu)\n",
             i, sym->Name, displacement64, line.FileName, line.LineNumber);
      } else {
        emit("  [%2u] %s +0x%llX\n", i, sym->Name, displacement64);
      }
    } else {
      emit("  [%2u] 0x%016llX\n", i, frameAddr);
    }
  }

  emit("\n\n");

  if (f) {
    fflush(f);
    fclose(f);
  }

  SymCleanup(proc);

  summary[sizeof(summary) - 1] = '\0';
  MessageBoxA(NULL, summary, "sandvox crashed", MB_OK | MB_ICONERROR);

  TerminateProcess(GetCurrentProcess(), code);
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void InstallCrashHandler() {
  SetUnhandledExceptionFilter(CrashFilter);
}
