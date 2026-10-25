# Windows: a fault in system code on a worker thread is reported

## The problem

On Windows a fault inside a system DLL on a `std::thread` (msvcrt's
`memcpy` handed a bad pointer, a graphics driver) ends the process with no
`.crash` and no minidump, and the next start reports a session that "ended
from outside" (docs/known-bugs.md). mcfgthread's thread entry
(`__MCF_seh_top`) ends any exception it does not know in
`RaiseFailFastException`, below which no filter runs.
`OnFaultInShippedCode` (src/app/CrashHandler.cpp) reports faults in code we
ship at first chance, but leaves a fault whose address is in system code to
the unhandled-exception filter, which never runs on a worker thread. It
leaves them because system code probes memory under its own `__try`, and
reporting such a fault would end a process that was about to recover.

The only Windows user testing the app is on real hardware with real
drivers, where a crash in a driver on a worker thread is the likeliest kind
and currently leaves nothing to read.

## The shape of the fix

**Ask what the dispatcher will find.** For a fatal fault whose address is
in system code, walk the faulting thread's frames from the fault's context
the way SEH dispatch will: `RtlLookupFunctionEntry` and `RtlVirtualUnwind`
with `UNW_FLAG_EHANDLER`, which returns each frame's language handler.

- A frame in a **system** module with an exception handler comes first:
  that code may catch its own fault. Leave it alone, as today.
- The walk reaches a frame in **code we ship** first (`InShippedCode`)
  without passing one: nothing between the fault and us handles it, and
  nothing of ours can. Report it, as a fault in shipped code is reported,
  and end the process with the exception's code.
- The walk fails (no unwind data, a corrupt stack): leave it alone, as
  today. A wrong report is worse than a missing one only when it ends a
  process that would have lived. Leaving it is today's behaviour.

The walk runs inside a vectored handler, on a possibly broken stack: no
allocation, no locks, the same constraints the existing report walk keeps
(CrashHandler.cpp's frame loop), with a frame limit.

## Files

- `src/app/CrashHandler.cpp` (`OnFaultInShippedCode` and a helper beside the
  report's walk).
- `packaging/windows/smoke.sh` / `tools/release-smoke.py` (the cases below).
- `docs/architecture.md#crashes-and-stalls`, `docs/known-bugs.md` (the
  entry goes).

## Verification

Under Wine, in the Windows smoke (the only place this code runs here):

- A debug-only fault on a worker thread inside a system function (a
  `memcpy` from msvcrt with a bad source pointer, reached through
  `debug_crash` with `LOBOLE_ALLOW_DEBUG_CRASH=1`) leaves a crash file whose
  frames name the system function and our caller, and the next start turns
  it into a report. Fails before the change (no file).
- A fault that system code catches itself (`IsBadReadPtr` on an unmapped
  address, on a worker thread) does not end the process and leaves no
  report.
- The existing crash smokes on Windows and Linux pass unchanged.
- Then a build to the user's brother, to see it on real Windows.
