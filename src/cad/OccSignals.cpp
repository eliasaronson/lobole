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
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Type.hxx>

#include <mutex>

#if defined(_WIN32)
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

#endif

} // namespace

void ArmOccSignalHandlers() {
  thread_local bool armed = false;
  if (armed)
    return;
  armed = true;
  const std::lock_guard<std::mutex> lock(g_armMutex);
#if defined(_WIN32)
  // OSD::SetSignal installs OCC's unhandled-exception filter over ours; the
  // SE translator it sets is what turns a fault into an exception inside a
  // try block, so the filter can go back to the crash handler's.
  const LPTOP_LEVEL_EXCEPTION_FILTER prior =
      SetUnhandledExceptionFilter(nullptr);
  SetUnhandledExceptionFilter(prior);
  OSD::SetSignal(Standard_False);
  if (prior != nullptr)
    SetUnhandledExceptionFilter(prior);
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
    if (why != nullptr) {
      const char* said = e.GetMessageString();
      *why = std::string(e.DynamicType()->Name()) +
             (said != nullptr && *said ? std::string(": ") + said : "");
    }
    return false;
  }
}

} // namespace occsig
