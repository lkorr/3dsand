// crash.cpp — minimal Windows crash handler using SetUnhandledExceptionFilter.
// Writes crash details (timestamp, exception, registers, stack trace) to
// crash.log, shows a MessageBox summary, and terminates.

#include "crash.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <typeinfo>

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

// The reporter, shared by EVERY fatal path (see InstallCrashHandler).
// `ep` is null on the paths that never produce an EXCEPTION_POINTERS at all —
// abort(), terminate(), a CRT invalid-parameter trap — and `reason` names the
// path for those.
void WriteCrashReport(EXCEPTION_POINTERS* ep, const char* reason) {
  char timeBuf[64];
  {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &lt);
  }

  // No EXCEPTION_POINTERS on the abort()/terminate() paths, so capture the
  // current register state for the stack walk below to unwind from. The top
  // frames are then the reporter itself — a cheap price for having a trace at
  // all where there previously was total silence.
  CONTEXT synth;
  if (!ep) RtlCaptureContext(&synth);
  DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0u;
  void* addr = ep ? ep->ExceptionRecord->ExceptionAddress
                  : (void*)(uintptr_t)synth.Rip;

  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(proc, NULL, TRUE);

  // ---- the module base, so an address survives ASLR ----
  // Every address below is also printed as a MODULE-RELATIVE offset (an RVA).
  // Absolute addresses are worthless across runs: Windows rebases the image
  // each launch, so two dumps of the identical fault in the identical binary
  // print different numbers and look like different bugs. The RVA is stable,
  // matches what a .map or .pdb lists, and is directly comparable run to run.
  HMODULE selfMod = NULL;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&WriteCrashReport, &selfMod);
  const DWORD64 selfBase = (DWORD64)selfMod;

  // ---- WALK THE FAULTING STACK, not the handler's ----
  // CaptureStackBackTrace() unwinds from wherever it is CALLED, which inside an
  // unhandled-exception filter is the tail of the OS exception-dispatch path.
  // That is what filled every historical dump with six frames of
  // KiUserExceptionDispatcher / _C_specific_handler / RtlLocateExtendedFeature
  // noise ahead of the first frame that belonged to this program — and worse,
  // those names are nearest-exported-symbol GUESSES inside ntdll, so they read
  // as though the crash were in strncpy.
  //
  // StackWalk64 seeded from ep->ContextRecord starts at the instruction that
  // actually faulted and unwinds the real call chain. Frame [0] is the fault.
  //
  // The CONTEXT must be COPIED: StackWalk64 mutates the context it is handed,
  // and ep->ContextRecord is what the OS will resume from if the filter ever
  // returns EXCEPTION_CONTINUE_SEARCH.
  constexpr int kMaxFrames = 64;
  DWORD64 frames[kMaxFrames];
  int frameCount = 0;
#ifdef _M_X64
  CONTEXT walk = ep ? *ep->ContextRecord : synth;
  STACKFRAME64 sf = {};
  sf.AddrPC.Offset = walk.Rip;
  sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = walk.Rbp;
  sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = walk.Rsp;
  sf.AddrStack.Mode = AddrModeFlat;
  while (frameCount < kMaxFrames &&
         StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf,
                     &walk, NULL, SymFunctionTableAccess64, SymGetModuleBase64,
                     NULL)) {
    if (sf.AddrPC.Offset == 0) break;
    frames[frameCount++] = sf.AddrPC.Offset;
  }
#endif
  // Fall back to the old capture if the walk produced nothing (a corrupt stack
  // defeats StackWalk64, and a noisy trace still beats no trace).
  if (frameCount == 0) {
    void* raw[kMaxFrames];
    USHORT n = CaptureStackBackTrace(0, kMaxFrames, raw, NULL);
    for (USHORT i = 0; i < n; i++) frames[frameCount++] = (DWORD64)raw[i];
  }

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
  if (reason)
    emit("%s at 0x%p", reason, addr);
  else
    emit("Exception 0x%08X (%s) at 0x%p", code, ExceptionName(code), addr);
  if (selfBase && (DWORD64)addr >= selfBase)
    emit("  (sandvox+0x%llX)", (DWORD64)addr - selfBase);
  emit("\n");
  emit("Module base 0x%llX\n", selfBase);

  // ---- WHAT the access violation actually was ----
  // ExceptionInformation carries the two facts that decide the whole diagnosis,
  // and the handler used to discard both: [0] is the operation (0 read, 1
  // write, 8 DEP/execute) and [1] is the address that was touched. "Read of
  // 0x0" is a null dereference; "write to 0x00000000000000C8" is a null `this`
  // with a member at offset 0xC8; "read of 0xFFFFFFFFFFFFFFFF" is a poisoned or
  // freed pointer. Those are three unrelated bugs that all report as
  // 0xC0000005, and without this line they are indistinguishable.
  if (ep &&
      (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
      ep->ExceptionRecord->NumberParameters >= 2) {
    const ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
    const ULONG_PTR at = ep->ExceptionRecord->ExceptionInformation[1];
    const char* what = op == 0 ? "READ from" : op == 1 ? "WRITE to"
                     : op == 8 ? "EXECUTE at" : "ACCESS of";
    emit("  %s 0x%016llX%s\n", what, (unsigned long long)at,
         at < 0x10000 ? "   <-- NULL POINTER (offset = member offset)" : "");
  }
  emit("\n");

#ifdef _M_X64
  CONTEXT* ctx = ep ? ep->ContextRecord : &synth;
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

  for (int i = 0; i < frameCount; i++) {
    DWORD64 frameAddr = frames[i];
    DWORD64 displacement64 = 0;

    // The RVA prefix is printed for EVERY frame, symbolized or not. A frame
    // that dbghelp cannot name is still fully identifiable by `sandvox+0xRVA`:
    // it is stable across runs, and
    //     llvm-symbolizer --obj=build/Release/sandvox.exe 0xRVA
    // resolves it offline from the .pdb long after the process is gone.
    // SymGetModuleBase64, not `addr >= selfBase`: the executable is not the
    // highest-loaded module, so a bare lower-bound test labels every ntdll and
    // kernel32 frame "sandvox+" with a nonsense 28-bit offset. Asking dbghelp
    // which module OWNS the address is the range test, and it costs one call.
    char where[64];
    if (selfBase && SymGetModuleBase64(proc, frameAddr) == selfBase)
      snprintf(where, sizeof(where), "sandvox+0x%-8llX", frameAddr - selfBase);
    else
      snprintf(where, sizeof(where), "0x%016llX", frameAddr);

    if (SymFromAddr(proc, frameAddr, &displacement64, sym)) {
      IMAGEHLP_LINE64 line = {};
      line.SizeOfStruct = sizeof(line);
      DWORD displacement32 = 0;
      if (SymGetLineFromAddr64(proc, frameAddr, &displacement32, &line)) {
        emit("  [%2d] %s  %s +0x%llX  (%s:%lu)\n",
             i, where, sym->Name, displacement64, line.FileName,
             line.LineNumber);
      } else {
        emit("  [%2d] %s  %s +0x%llX\n", i, where, sym->Name, displacement64);
      }
    } else {
      emit("  [%2d] %s\n", i, where);
    }
  }

  emit("\n\n");

  if (f) {
    fflush(f);
    fclose(f);
  }

  SymCleanup(proc);

  summary[sizeof(summary) - 1] = '\0';

  // Mirror the summary to stderr so a captured console run (selftest, agents,
  // CI) contains the stack trace without anyone having to find crash.log.
  fputs(summary, stderr);
  fflush(stderr);

  // The modal dialog hangs an unattended run until someone clicks it.
  // SANDVOX_NO_CRASH_DIALOG=1 (any value but "0") skips it; crash.log and
  // stderr carry the full report either way.
  char noDialog[8];
  DWORD nd = GetEnvironmentVariableA("SANDVOX_NO_CRASH_DIALOG", noDialog,
                                     sizeof(noDialog));
  if (nd == 0 || noDialog[0] == '0') {
    MessageBoxA(NULL, summary, "sandvox crashed", MB_OK | MB_ICONERROR);
  }

  TerminateProcess(GetCurrentProcess(), code ? code : 3u);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  WriteCrashReport(ep, nullptr);
  return EXCEPTION_EXECUTE_HANDLER;  // unreachable: WriteCrashReport terminates
}

// ---- the paths SetUnhandledExceptionFilter does NOT catch ------------------
// A filter installed with SetUnhandledExceptionFilter only ever sees SEH
// exceptions. It does not see abort(), it does not see terminate(), and it does
// not see the CRT's own fatal traps — those call ExitProcess directly. So the
// engine's LOUDEST failures were its most silent ones:
//
//   * PageTable::Alloc's "FATAL: page pool exhausted" ends in std::abort(),
//     which is exactly the crash the page pool is DESIGNED to produce when it
//     is mis-sized (PLAN_page_table.md §3.8). It wrote nothing to crash.log.
//   * Any uncaught C++ exception — std::bad_alloc from a large reserve, a
//     .at() out of range, a throw across a callback — reaches terminate().
//   * A bad printf/strcpy_s argument reaches the invalid-parameter handler.
//
// Measured 2026-08-29: a run died leaving crash.log untouched and last_run.json
// stale, and there was NO artefact anywhere saying what happened. Launched from
// the tuner's Play button, where stderr is not attached to a console, such a
// death is completely invisible. Every one of these now writes the same report,
// with a stack trace, to the same file.
void OnTerminate() {
  // Recover the in-flight exception's type and message — for an uncaught throw
  // that text IS the diagnosis, and it is otherwise lost entirely.
  char detail[512];
  snprintf(detail, sizeof(detail), "std::terminate (uncaught exception)");
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      snprintf(detail, sizeof(detail), "std::terminate: uncaught %s: %s",
               typeid(x).name(), x.what());
    } catch (...) {
      snprintf(detail, sizeof(detail),
               "std::terminate: uncaught non-std exception");
    }
  }
  WriteCrashReport(nullptr, detail);
}

void OnAbort(int) {
  WriteCrashReport(nullptr,
                   "abort() — a deliberate fatal assertion; the reason was "
                   "printed to stderr just above this report");
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*,
                        unsigned int, uintptr_t) {
  WriteCrashReport(nullptr, "CRT invalid parameter");
}

void OnPureCall() { WriteCrashReport(nullptr, "pure virtual function call"); }

}  // namespace

void InstallCrashHandler() {
  SetUnhandledExceptionFilter(CrashFilter);

  // Route the non-SEH fatal paths into the same reporter.
  std::set_terminate(OnTerminate);
  signal(SIGABRT, OnAbort);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);

  // Keep the CRT from popping its own "abort() has been called" dialog and
  // from racing us to ExitProcess — our SIGABRT handler must run first, and on
  // an unattended run (selftest, tuner, agent) a modal dialog is a hang.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
