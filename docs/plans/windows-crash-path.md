# The Windows crash path

On Windows (verified under Wine) a crash after the first fillet leaves no
report, one fuzz case kills the app where Linux survives, another hangs,
a stall has no stack, a console stop is reported as a crash, and every
report's `Build:` line says "no test engine".

## 1. A fault after OCC's handlers are armed

`OSD::SetSignal` on Windows installs three things: the unhandled-exception
filter (`ArmOccSignalHandlers` puts the crash handler's back), a console
control handler, and `signal(SIGSEGV|SIGILL|SIGFPE, SIGWntHandler)`. The
last is what kills the report: mingw's startup code runs `main` — and
winpthreads every thread it starts — under an `__except` whose filter is
`_gnu_exception_handler` (mingw-w64 `crt/crtexe.c`, `crt/crt_handler.c`),
which calls a `signal()` handler for an access violation before the
exception can reach the unhandled-exception filter. OCC's handler, outside
an OCC catch scope, prints "*** Abort *** an exception was raised, but no
catch was found" and calls `exit(1)`.

Mirror the Linux design: `signal()` keeps what was there before (the
default, so mingw's filter passes the exception on), and the
unhandled-exception filter becomes ours, a dispatcher that hands a fault
to OCC (`Standard_Failure::Jump`, the long jump to the nearest
`OCC_CATCH_SIGNALS`) only when `Standard_ErrorHandler::IsInTryBlock()`, and
otherwise calls the crash handler with the fault's own context. The filter
is reached from every thread — mcfgthread's `std::thread`s have no CRT
filter at all, so there OCC's conversion had been lost too. A stack
overflow is never OCC's: a long jump out of it leaves no guard page.
The console handler OCC registers (Ctrl+C swallowed, every other event
`exit(254)`) is kept behind ours (item 4).

## 2. crash-181 adding Hole 'H6' on Windows only

With item 1 the death leaves a report; symbolize it, find the cause, and
fix it with a regression test that runs on Linux when the cause is general.

## 3. crash-153 hangs in `open_project`, and a stall has no stack

Give the watchdog a main-thread stack on Windows: `SuspendThread`,
`GetThreadContext`, and the same `RtlVirtualUnwind` walk the crash handler
uses — but looking functions up in each module's `.pdata` from a table
read before the suspend, so the walk takes no loader lock the suspended
thread might hold — then module + RVA frames, which
`tools/symbolize-crash.py` names. Then read the stack of the hang and fix
what it shows.

## 4. A stop is not a crash

`SetConsoleCtrlHandler` (Ctrl+C, Ctrl+Break, console close, logoff,
shutdown), kept in front of OCC's after every arming: where the mode has its
own clean stop (headless and offscreen install a `SIGINT` handler) it is
asked to stop and the session ends the ordinary way; otherwise the marker is
deleted and the process ends, as `SIGINT`/`SIGTERM` do on Linux.
`TerminateProcess` cannot be caught, so a Windows report of a session that
ended without one says "terminated from outside (Task Manager, taskkill /F)
or power lost" rather than naming `kill -9` and the OOM killer.

## 5. `Build:` says "no test engine"

`HAVE_TEST_ENGINE` is a define of `designer_app`; `BuildInfo.cpp` is in
`designer_core`. Give that one file the define, as it has the commit.

## Files

`src/cad/OccSignals.{h,cpp}`, `src/app/CrashHandler.{h,cpp}`,
`src/core/io/CrashReports.cpp`, `src/remote/RemoteDispatch.cpp` (a
`debug_fault` kind that faults inside an OCC scope, on this thread and on a
worker), `CMakeLists.txt`, `tests/hostile_params_smoke.py`, a unit test for
the build line, and whatever items 2 and 3 turn up.

## Verification

Linux: the headless suite, the GUI suite and `pygui`, with
`hostile_params_selftest` covering the new `debug_fault` kind. Windows, the
cross build under Wine: segv after a fillet leaves a `.crash` with module +
RVA frames and a `.dmp`, collected on the next start; `debug_fault
occ_fault` answers on both threads instead of dying; crash-153 and crash-181
replay to the end; `--stall-test` logs a stack that `symbolize-crash.py`
names; a console Ctrl+C on headless leaves no report; the `Build:` line
names the test engine.
