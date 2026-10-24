// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// OCC's signal handlers, without losing the crash handler
// (cad/CADKernel.h, ArmOccSignalHandlers).

// OCC_CATCH_SIGNALS is real only where this is defined, which is here alone
// (cad/OccSignals.h).
#define OCC_CONVERT_SIGNALS 1

#include "cad/OccSignals.h"
#include "cad/CADKernel.h"

#include <OSD.hxx>
#include <OSD_Exception.hxx>
#include <OSD_Signal.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Type.hxx>

#include <atomic>
#include <mutex>

#if defined(_WIN32)
#include <OSD_Exception_ACCESS_VIOLATION.hxx>
#include <OSD_Exception_ARRAY_BOUNDS_EXCEEDED.hxx>
#include <OSD_Exception_ILLEGAL_INSTRUCTION.hxx>
#include <OSD_Exception_INT_OVERFLOW.hxx>
#include <OSD_Exception_IN_PAGE_ERROR.hxx>
#include <OSD_Exception_PRIV_INSTRUCTION.hxx>
#include <Standard_DivideByZero.hxx>
#include <Standard_NumericError.hxx>
#include <Standard_ProgramError.hxx>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <float.h>
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#endif

namespace {

std::mutex g_armMutex;

#if !defined(_WIN32)

// The faults OCC turns into exceptions inside its catch scopes, and the stop
// requests it also takes over and must not.
constexpr int kFaults[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL};
constexpr int kStops[] = {SIGINT, SIGHUP, SIGQUIT, SIGTERM};

// Per signal: OCC's action and the one that was there before it. Written
// under g_armMutex before the dispatcher is installed, read in the handler.
struct sigaction g_occ[NSIG];
struct sigaction g_prior[NSIG];
bool g_dispatching[NSIG];

void Call(const struct sigaction& a, int sig, siginfo_t* info, void* ctx) {
  if (a.sa_flags & SA_SIGINFO) {
    if (a.sa_sigaction != nullptr) {
      a.sa_sigaction(sig, info, ctx);
      return;
    }
  } else if (a.sa_handler != SIG_DFL && a.sa_handler != SIG_IGN &&
             a.sa_handler != nullptr) {
    a.sa_handler(sig);
    return;
  } else if (a.sa_handler == SIG_IGN) {
    return;
  }
  // The default: die by this signal, as an unhandled fault does.
  signal(sig, SIG_DFL);
  raise(sig);
}

void Dispatch(int sig, siginfo_t* info, void* ctx) {
  // Inside an OCC_CATCH_SIGNALS scope OCC's handler long-jumps to it and
  // the fault becomes a Standard_Failure the algorithm reports. Anywhere
  // else it would print "no catch was found" and exit(1); the crash handler
  // reports it instead.
  if (Standard_ErrorHandler::IsInTryBlock())
    Call(g_occ[sig], sig, info, ctx);
  else
    Call(g_prior[sig], sig, info, ctx);
}

#else // _WIN32

// OSD::SetSignal installs three things on Windows, and none of them stays:
//
// - signal(SIGSEGV|SIGILL|SIGFPE, OCC's). mingw's start-up code runs main --
//   and winpthreads every thread it starts -- under an __except whose filter
//   (_gnu_exception_handler, mingw-w64 crt/crt_handler.c) calls a signal()
//   handler for an access violation BEFORE the exception can reach the
//   unhandled-exception filter. OCC's, outside an OCC catch scope, printed
//   "*** Abort *** an exception was raised, but no catch was found" and
//   called exit(1): no crash report, no stack. So signal() keeps what it had
//   (the default), and mingw's filter passes the exception on.
// - the unhandled-exception filter, OCC's way of turning a fault inside an
//   OCC catch scope into a Standard_Failure. It never sees one on a
//   std::thread: mcfgthread's thread entry (__MCF_seh_top, mcfgthread
//   src/xglobals.c) ends any exception it does not know in
//   RaiseFailFastException, below which no filter runs. So the crash
//   handler's filter goes back, and the conversion is a vectored handler of
//   our own (OnFault, below), which runs first, on every thread, and acts
//   only inside an OCC catch scope.
// - a console control handler, which swallows Ctrl+C and exit(254)s on every
//   other event; the one SetConsoleControlHandler names is put back in front
//   of it.
constexpr int kFaults[] = {SIGSEGV, SIGILL, SIGFPE};

PHANDLER_ROUTINE g_console = nullptr;
bool g_vectored = false;

// What OCC's own filter raises for each code (OSD_signal.cxx, _osd_raise),
// minus what it cannot be handed: a stack overflow, which a long jump would
// leave with no guard page, so the next one would kill the process with no
// handler at all.
[[noreturn]] void JumpToOcc(const EXCEPTION_RECORD& rec, DWORD code) {
  char what[160];
  if (code == EXCEPTION_ACCESS_VIOLATION) {
    std::snprintf(what, sizeof what,
                  "ACCESS VIOLATION at address 0x%p during '%s' operation",
                  reinterpret_cast<void*>(rec.ExceptionInformation[1]),
                  rec.ExceptionInformation[0] ? "WRITE" : "READ");
  } else {
    std::snprintf(what, sizeof what, "Windows exception 0x%08lX",
                  static_cast<unsigned long>(code));
  }
  Handle(Standard_Failure) failure;
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
    failure = new OSD_Exception_ACCESS_VIOLATION(what);
    break;
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    failure = new OSD_Exception_ARRAY_BOUNDS_EXCEEDED(what);
    break;
  case EXCEPTION_ILLEGAL_INSTRUCTION:
    failure = new OSD_Exception_ILLEGAL_INSTRUCTION(what);
    break;
  case EXCEPTION_PRIV_INSTRUCTION:
    failure = new OSD_Exception_PRIV_INSTRUCTION(what);
    break;
  case EXCEPTION_IN_PAGE_ERROR:
    failure = new OSD_Exception_IN_PAGE_ERROR(what);
    break;
  case EXCEPTION_INT_OVERFLOW:
    failure = new OSD_Exception_INT_OVERFLOW(what);
    break;
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    failure = new Standard_DivideByZero(what);
    break;
  case EXCEPTION_DATATYPE_MISALIGNMENT:
    failure = new Standard_ProgramError(what);
    break;
  default: // the floating-point traps
    _clearfp();
    failure = new Standard_NumericError(what);
    break;
  }
  // Built with OCC_CONVERT_SIGNALS (any non-MSVC build of OCC): the long
  // jump to the innermost OCC_CATCH_SIGNALS, which rethrows it there.
  failure->Jump();
  std::abort(); // not reached: Jump returns only when there is no scope
}

bool IsFault(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case EXCEPTION_DATATYPE_MISALIGNMENT:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_PRIV_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_INT_OVERFLOW:
  case EXCEPTION_FLT_DENORMAL_OPERAND:
  case EXCEPTION_FLT_DIVIDE_BY_ZERO:
  case EXCEPTION_FLT_INEXACT_RESULT:
  case EXCEPTION_FLT_INVALID_OPERATION:
  case EXCEPTION_FLT_OVERFLOW:
  case EXCEPTION_FLT_UNDERFLOW:
    return true;
  default:
    return false;
  }
}

// First in line for every exception on every thread, so it does nothing
// unless the fault is one an OCC catch scope on this thread is waiting for:
// the same first-chance conversion OCC's MSVC build makes through its SE
// translator. Everything else goes on to the crash handler.
LONG WINAPI OnFault(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  if (IsFault(code) && Standard_ErrorHandler::IsInTryBlock())
    JumpToOcc(*info->ExceptionRecord, code);
  return EXCEPTION_CONTINUE_SEARCH;
}

void PutConsoleHandlerFirst() {
  if (g_console == nullptr)
    return;
  // Handlers run last-registered first.
  SetConsoleCtrlHandler(g_console, FALSE);
  SetConsoleCtrlHandler(g_console, TRUE);
}

#endif

} // namespace

#if defined(_WIN32)
void occsig::SetConsoleControlHandler(int(__stdcall* handler)(unsigned long)) {
  const std::lock_guard<std::mutex> lock(g_armMutex);
  g_console = reinterpret_cast<PHANDLER_ROUTINE>(handler);
  PutConsoleHandlerFirst();
}
#endif

#if !defined(_WIN32)
namespace {
// Whether the dispatcher still holds every fault signal. A sigaction is the
// process's, and anything may put another over it after we armed: Catch2
// puts its own in front of each test case and restores what it found when
// the case ends, which dropped the dispatcher for every later case, so an
// OCC fault inside RunCatchingOccSignals there killed designer_tests.
bool DispatcherHeld() {
  for (int sig : kFaults) {
    struct sigaction now{};
    sigaction(sig, nullptr, &now);
    if (!(now.sa_flags & SA_SIGINFO) || now.sa_sigaction != Dispatch)
      return false;
  }
  return true;
}
} // namespace
#endif

void ArmOccSignalHandlers() {
#if defined(_WIN32)
  thread_local bool armed = false;
  if (armed)
    return;
  armed = true;
#else
  // Not once per thread but on every call (four sigaction reads): the
  // handlers are the process's, and can be replaced under us.
  if (DispatcherHeld())
    return;
#endif
  const std::lock_guard<std::mutex> lock(g_armMutex);
#if defined(_WIN32)
  // What signal() has for the faults (the default, unless something set
  // one), to put back over OCC's.
  using Handler = void (*)(int);
  Handler before[NSIG] = {};
  for (int sig : kFaults) {
    before[sig] = signal(sig, SIG_DFL);
    signal(sig, before[sig]);
  }
  const LPTOP_LEVEL_EXCEPTION_FILTER filter =
      SetUnhandledExceptionFilter(nullptr);
  SetUnhandledExceptionFilter(filter);
  OSD::SetSignal(Standard_False);
  for (int sig : kFaults)
    signal(sig, before[sig]);
  if (filter != nullptr)
    SetUnhandledExceptionFilter(filter);
  if (!g_vectored)
    g_vectored = AddVectoredExceptionHandler(1, OnFault) != nullptr;
  PutConsoleHandlerFirst();
#else
  struct sigaction before[NSIG];
  for (int sig : kFaults)
    sigaction(sig, nullptr, &before[sig]);
  for (int sig : kStops)
    sigaction(sig, nullptr, &before[sig]);
  OSD::SetSignal(Standard_False);
  for (int sig : kStops)
    sigaction(sig, &before[sig], nullptr);
  for (int sig : kFaults) {
    struct sigaction occ{};
    sigaction(sig, nullptr, &occ);
    const bool wasOurs = before[sig].sa_sigaction == Dispatch &&
                         (before[sig].sa_flags & SA_SIGINFO);
    if (!g_dispatching[sig] || !wasOurs)
      g_prior[sig] = before[sig];
    g_occ[sig] = occ;
    g_dispatching[sig] = true;
    struct sigaction dispatch{};
    dispatch.sa_sigaction = Dispatch;
    sigemptyset(&dispatch.sa_mask);
    // On the alternate stack where the crash handler ran on one: a stack
    // overflow is a SIGSEGV with no stack left.
    // SA_NODEFER: OCC leaves its handler by longjmp, not siglongjmp, so a
    // deferred signal would stay blocked on that thread, and the next fault
    // there would kill the process with no handler at all.
    dispatch.sa_flags =
        SA_SIGINFO | SA_NODEFER | (before[sig].sa_flags & SA_ONSTACK);
    sigaction(sig, &dispatch, nullptr);
  }
#endif
}

namespace occsig {

namespace {
std::atomic<unsigned> g_faults{0};
} // namespace

unsigned FaultCount() { return g_faults.load(std::memory_order_relaxed); }

bool CallCatchingSignals(void (*fn)(void*), void* ctx, std::string* why) {
  ArmOccSignalHandlers();
  try {
    OCC_CATCH_SIGNALS
    fn(ctx);
    return true;
  } catch (const Standard_Failure& e) {
#if !defined(_WIN32)
    // Left by longjmp: make sure the fault that brought us here is not
    // still blocked on this thread.
    sigset_t faults;
    sigemptyset(&faults);
    for (int sig : kFaults)
      sigaddset(&faults, sig);
    pthread_sigmask(SIG_UNBLOCK, &faults, nullptr);
#endif
    // A signal OCC turned into an exception (OSD_SIGSEGV and the rest; on
    // Windows OSD_Exception_ACCESS_VIOLATION and the rest), not a failure
    // the algorithm threw itself.
    if (e.IsKind(STANDARD_TYPE(OSD_Signal)) ||
        e.IsKind(STANDARD_TYPE(OSD_Exception)))
      g_faults.fetch_add(1, std::memory_order_relaxed);
    if (why != nullptr) {
      const char* said = e.GetMessageString();
      *why = std::string(e.DynamicType()->Name()) +
             (said != nullptr && *said ? std::string(": ") + said : "");
    }
    return false;
  }
}

} // namespace occsig
