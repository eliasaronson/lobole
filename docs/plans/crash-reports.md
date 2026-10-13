# Crash reports a user can hand over

## The problem

Every crash so far happened in front of us: the stack goes to stderr, stderr
goes to the dev-watch log, and the crash is reproduced on the spot. On a
user's machine none of that holds.

- **Windows prints only the exception code** (`CrashHandler.cpp`,
  `OnWindowsException`), and prints it to stderr, which a GUI app launched
  from the Start menu does not have. A Windows crash leaves nothing.
- **Linux and macOS print a stack, also only to stderr.** Launched from a
  desktop icon, it goes nowhere.
- **There is no log file.** `Logger.cpp` has one sink, stdout. The watchdog's
  stall reports and every warning a regeneration printed are lost with it.
- **Nothing knows the last run ended badly.** The only trace a crash leaves is
  an instance-registry file, and that exists only with `--remote`. A leftover
  autosave proves nothing either: a clean quit with unsaved changes keeps it
  on purpose.
- **Unsaved work is never offered back after a crash.** The recovery modal
  appears only from Open Project (`FileDialogs.cpp`), so autosaves of a
  project the user does not reopen, and anything in `unsaved.autosave.*`, are
  never found.

So the gap is not finding crashes but getting evidence back from a machine we
do not control. The user should end up with a folder they can zip and send,
and the app should tell them it exists.

## The shape of it

### 1. A log file per session

`Logger` gains a second sink: `<StateDir>/logs/<started>-<pid>.log`, with the
same pattern and flush-on-every-message as stdout, so the file is complete up
to the instant of a crash. The ten newest are kept; older ones are deleted at
startup. stdout is unchanged, so dev-watch and the smokes see what they see
now.

**The log is the activity journal.** Edits are logged at info as they happen,
one line each, so the tail of the log says what the user was doing:

- a part `Command` executed, undone or redone, with its `Description()`
  (`CommandStack`, in `Command.cpp`; no header change);
- an assembly, drawing or job undo snapshot, naming the document kind
  (`SnapshotAssembly`, `SnapshotDrawing`, `SnapshotJob`);
- every semantic RPC, by method name (not its params: it can be large, and
  it names the user's files);
- open, save, tab switch, tool session started and committed.

A second journal format would only drift from the log; one stream with a
timestamp per line is enough.

### 2. A session marker that a dead process leaves behind

At startup, in every mode, the app creates
`<StateDir>/sessions/<started>-<pid>.json` holding the version, the build
(git hash), mode, OS, the log path and the open project path (rewritten when
that changes). It **holds an exclusive lock on it** for its whole life:
`flock` on Linux and macOS, `LockFileEx` on Windows. A clean exit deletes it.

A marker whose lock can be taken belongs to a process that is gone. That
covers every way to die: a crash, the OOM killer, `kill -9`, power loss. A pid
check would not, since pids are reused; a lock dies with its process.

### 3. The crash handler writes a file, raw addresses first

At startup, while allocating is safe, the handler is given the path
`<StateDir>/sessions/<started>-<pid>.crash` as a fixed buffer, plus the
executable's load base. On a fatal signal or exception it:

1. opens that path with `open()`/`CreateFileW`, both safe in a handler;
2. writes the signal or exception code, then the **raw frames as module +
   offset** (`backtrace()` + the recorded base on POSIX; on Windows the
   exception context, walked with `RtlCaptureStackBackTrace`, each address
   given its module and RVA via `GetModuleHandleExW`);
3. then, best effort, the symbolized stack it already prints, written to the
   file as well as stderr;
4. on Windows, also writes `<…>.dmp` with `MiniDumpWriteDump`
   (`MiniDumpNormal`: threads, registers, stacks; a few hundred KB, no heap).

Raw frames come first because symbolizing allocates and can hang in a corrupt
heap; whatever happens after step 2, the file already holds what is needed.
A release build carries no line numbers and the Windows build no in-process
symbolizer, so **`tools/symbolize-crash.py REPORT BINARY` turns module +
offset into function and line** with `addr2line` against the unstripped
binary of the same build (nix's mingw `addr2line` for a `.exe`). The report's
build hash says which binary that is.

The Linux handler keeps everything it does now: restore the default action,
re-raise, core-dump as before.

### 4. The next launch collects it

At startup, before the first frame, every unlocked marker in `sessions/` is
turned into a report folder `<StateDir>/crashes/<started>-<pid>/`:

- `report.txt`: what happened ("crashed: SIGSEGV" / "Windows exception
  0xC0000005" / "ended without a crash report: killed, out of memory, or
  power lost"), version, build, OS, mode, project path, time, and the stack;
- `session.log`: the last 2000 lines of that session's log;
- `crash.dmp` where there is one;
- `autosave/`: copies of that session's autosave sidecars, if any.

The marker and `.crash` file are then deleted. Twenty reports are kept;
older ones go. This step is in all three modes, so a headless crash is
collected by the next headless start too.

Nothing is sent anywhere. The folder holds the user's design (in the autosave
copies) and their file paths (in the log), so `report.txt` says so at the top.

### 5. The user is told

When step 4 produced a report in the GUI, a modal says
"Loböle closed unexpectedly last time", the one-line reason, and:

- **Recover unsaved work** (only when autosaves were copied): reopens that
  project through the existing `ui::RecoverProject`, or loads the unsaved
  sidecar for a project that was never saved;
- **Open report folder**: through a new `platform::RevealInFileManager`
  (`xdg-open` / `ShellExecuteW` / `open`);
- **Copy report**: puts `report.txt` on the clipboard, ready to paste into a
  message;
- **Dismiss**.

`Help ▸ Crash reports…` opens the reports folder at any time.

For agents and tests, a read-only RPC `crash_reports` lists the reports (path,
time, reason, whether there are autosaves). An env-gated RPC
`debug_crash {kind: "segv"|"abort"|"exception"}` crashes the app after edits
have been made (`--crash-test` crashes before there are any), and is refused
unless `LOBOLE_ALLOW_DEBUG_CRASH=1`.

### 6. A crash fix starts from its report

The rule that makes the folder worth having: **a crash fix lands with a
regression test built from the report** — the autosave copy is the starting
document, the log's edit lines are the steps, and the test reproduces the
crash before the fix. CLAUDE.md's Crash debugging section says so, beside the
existing stack-trace note.

## Files

- `src/app/CrashHandler.{h,cpp}`: the file, raw frames, the Windows stack
  walk and minidump; the path and load base are set at startup.
- `src/app/CrashReports.{h,cpp}` (new): the session marker and its lock,
  collecting reports, retention.
- `src/common/Logger.cpp`: the file sink and log retention.
- `src/common/Platform.{h,cpp}` (new, or beside `RuntimePaths`):
  `RevealInFileManager`, `FileLock`.
- `src/core/Command.cpp`: log executed, undone and redone commands.
- `src/ui/AssemblyActions.cpp`, `src/ui/DocumentSlots.cpp`: log undo
  snapshots.
- `src/remote/RemoteDispatch.cpp`: log the method; `crash_reports` and
  `debug_crash` in their domain module's `MethodDoc` table.
- `src/ui/FileDialogs.cpp` (the modal), `src/ui/MainMenuBar.cpp` (the menu
  item).
- `src/app/main.cpp`, `src/app/HeadlessMain.cpp`: marker at start, collect,
  delete the marker on clean exit.
- `tools/symbolize-crash.py` (new).
- `CLAUDE.md`: the rule in section 6.
- Docs: `docs/architecture.md#crashes-and-stalls` takes the description;
  `docs/packaging/README.md` says where a user finds the folder on each OS.

## Verification

- **`tests/crash_report_smoke.py`**, with a scratch `XDG_STATE_HOME`:
  1. start headless, make two edits by RPC, wait for an autosave
     (`--autosave 1`), `debug_crash segv`; the process dies with SIGSEGV;
  2. start again: `crash_reports` lists one report whose `report.txt` says
     SIGSEGV and whose stack names the `debug_crash` handler; `session.log`
     contains both edits' descriptions; `autosave/` holds the sidecar;
  3. the same with `kill -9` gives "ended without a crash report";
  4. a clean quit gives no report, and no marker is left in `sessions/`;
  5. `tools/symbolize-crash.py` on the report names the `debug_crash` handler
     from the raw frames alone.
- **A GUI test**: plant a dead session with an autosave, start the app, the
  modal appears; Recover unsaved work brings the edit back and leaves the
  project dirty.
- **Unit tests** for the lock (a held marker is not collected, a released one
  is) and retention (the 21st report deletes the oldest).
- **Windows, by hand under Wine** (no CI job cross-builds): the cross-built
  exe with `debug_crash segv` writes `.crash` with module + RVA frames and a
  `.dmp`; the next start collects them; `symbolize-crash.py` against the
  cross-built `lobole.exe` names the function. Then the same from the
  extracted portable zip, the artifact a tester runs.
- **macOS is untested**: the POSIX path compiles there and should work, but
  nothing here can run it. The packaging README says so.
- The headless suite, `tools/test-fast.sh` and the GUI suite pass, and
  dev-watch's log is unchanged apart from the new edit lines.
