# The development loop: what the chess round cost, and what to change

The chess-set round (82 commits, formats v133 to v138, one main session with
26 agents) spent 27.8 hours of wall clock. The main agent was busy for 16 of
them. The rest was the night, 1 h 49 m dead after a crash, and about 2 h
idle waiting on agents. The numbers below come from the session and agent
transcripts, the task outputs, the earlyoom journal and git. They were
measured, not remembered.

Each change is small enough to do on its own. The saving is this round's
cost of not having it.

## 1. Memory decides how many builds run, not cores

**What happened.** Four worktree agents started at once, each running
`worktree-build.sh` at the default `-j`: one job per core, 22 of them. Each
cc1plus is 0.6 to 1.8 GB, and the machine also runs a kind cluster, Chrome
and Slack. earlyoom killed 58 compilers in the round, 30 of them in four
minutes. Then it killed the Claude process itself: `earlyoom: sending
SIGTERM to process … "claude"`. Nothing ran for 1 h 49 m, until the human
noticed. Agents then retried at `-j6`, then `-j4`, and one spent 24 minutes
in a retry loop.

**Change.**
- A `tools/build-jobs` helper prints a `-j` from `MemAvailable` at about
  1.5 GB per job, capped at the core count.
- `worktree-build.sh`, `tools/test-fast.sh` and the build line in CLAUDE.md
  use it.
- CLAUDE.md says to start worktree agents two at a time. A second pair waits
  for the first pair's first build to finish, because a warm ccache build is
  memory-light.

**Saving.** The crash and about 20 minutes of retries. Also the class of
"compiler Terminated" failures that agents otherwise misread as code
problems.

## 2. A worktree hits the same ccache main fills

**What happened.** CLAUDE.md promises a fresh worktree "~75 s to a working
binary (ccache-warm)". None did. The nine build agents spent 13 to 79
minutes each building, and a docs-only agent spent 23 of its 37 minutes
compiling. Since its stats were last reset, ccache shows a hit rate of 8%:
1,441 hits against 16,219 misses. The store sits at its 40 GB cap after 476
cleanups. Three things keep a worktree from ever hitting main's objects:

- `base_dir` is unset. Every include path is absolute
  (`-I…/.claude/worktrees/agent-…/src`), so it is part of the key.
- `worktree-build.sh` configures with no build type, and `CMakeLists.txt`
  defaults to Debug. Main's `build/` is Release, so the flags differ.
- Debug adds `-g`, and with `hash_dir = true` ccache also hashes the working
  directory.

The same mismatch made `watchdog_smoke` fail in every agent's full suite
(its stall stack differs in Debug): 13 times across 7 agents, each one
investigated and reported. It also made worktree CAM runs about 1.6 times
slower (49.5 minutes against about 30), and it is why four fresh worktrees
together compiled four whole trees and ran the machine out of memory
(item 1).

**Change.**
- `~/.config/ccache/ccache.conf` gets `base_dir = /home/elias/Documents`,
  which covers `build/`, `.claude/worktrees/*` and `../designer-wt*`.
- `worktree-build.sh` configures with main's `CMAKE_BUILD_TYPE`, read from
  `build/CMakeCache.txt`, Release by default.
- `max_size` is raised, or other projects' caches are moved out, so one
  full tree of both configurations fits without eviction.
- `watchdog_smoke` should still pass in a Debug build, because a developer
  debugging a stall builds Debug on purpose. Find out why its stack shows
  `RunHeadless` rather than `StallSelfTest` there.
- Measure it: a fresh worktree's first build and its `ccache -s` hit rate,
  before and after. The CLAUDE.md figure gets corrected to whatever this
  measures.

**Saving.** Most of the agents' 4.9 hours of building (an estimate; with
warm hits a worktree build is the 75 s CLAUDE.md claims). Also the 13 false
failures, and most of the memory pressure behind the crash.

## 3. Wait on a pid or a marker, never on a pattern

**What happened.** The tool caps a call at 600 s. About 400 calls asked for
more and were silently capped. 37 hit the cap and were moved to the
background, where they stayed alive and stacked up: one pipeline run had
three waiters, one agent had five. `pgrep -f` and `pkill -f` matched the
calling shell's own command line in 7 of 16 uses, which killed the caller
(exit 144). A `pgrep -f "ninja designer"` wait loop matched itself and never
ended, costing 33 minutes. The first retro of the round had already written
this lesson down, and it recurred four more times.

**Change.**
- A `tools/wait-for.sh` waits on a pid file or a marker line in a log, with
  a deadline under 600 s. It exits non-zero on timeout, so the next call
  decides.
- `run_all.sh` and the other long scripts write a pid file and a `done`
  marker.
- CLAUDE.md gets one line: long runs go through `run_in_background` and the
  completion notice, never an `until pgrep` loop. Kill by the pid a script
  announced, never by `-f` pattern.

**Saving.** About 1 hour of self-inflicted waiting, and the stacked
notifications.

## 4. A program that cuts nothing is not "ok"

**What happened.** The rook's crenel op picked the notch's end cap, which
is a wall 3 mm past the axis, and it was refused as too deep. Each crenel
setup then posted a seven-line file that cuts nothing. `cam_verify` said
`ok: true` with the line "This program never touches the stock". Every
full run for about 20 hours reported the crenels as clean.

**Change.**
- A setup with an enabled operation whose program touches no stock gets
  `ok: false`, because the job is incomplete. The same goes for an op that
  was refused at `add_cam_op`, where the file never mentions it.
- The verdict carries `cuts_nothing`, following "refuse, don't substitute".
- `cam.py` exits non-zero when any op is refused or any setup is not ok, so
  `run_all.sh` fails loudly instead of printing `done`.

**Saving.** The 20-hour blind spot, and every future pipeline run that
"passes" while doing nothing.

## 5. The acceptance run is not the inner loop

**What happened.** The full chess pipeline ran 10 times, about 5.2 hours of
background time, and main spent 236 minutes of foreground in wait loops on
it. Most runs checked a change to one job.

**Change.** `cam.py --only <job>` already exists. Add `run_all.sh --from
<stage>`, so the design stages are skipped when only CAM changed. CLAUDE.md
under "Benchmark"-style guidance: the full run is the acceptance check
after a phase, and per-job runs are the loop.

**Saving.** Roughly two thirds of the pipeline time (an estimate; 4 of the
10 runs needed the whole set).

## 6. Never merge into a tree another build is compiling

**What happened.** A cherry-pick changed `AppState.h` while the CAM-off
build was compiling. Half the objects saw the old layout and half the new,
and the binary died on the first autosave with a `bad_alloc` that looked
like heap corruption. That cost ten minutes of valgrind and a rebuild.

**Change.** The build helper from item 1 takes an `flock` on the build
directory for the length of a build. A small `tools/merge-branch.sh`
(cherry-pick, then build, then the headless suite, then remove the
worktree) takes the same lock before it touches the tree. It also does the
worktree removal CLAUDE.md spells out by hand, which the round did 11 times.

**Saving.** The incident, plus about 3 minutes of hand-typed commands per
merge.

## 7. Performance has a test

**What happened.** The human found a job tab running at 1 to 10 fps idle.
It was computing OCC's exact bounding box three times a frame on the chess
blanks' tori, and no test measures frame time.

**Change.** A GUI test opens a job on a turned part (a torus-faced blank)
and asserts the idle frame's `ui` phase is under a bound, read from
`frame_stats`. It also switches job tabs and asserts the worst frame stays
under a second, so the same cache regression fails the suite.

**Saving.** One user-found regression, 24 minutes to diagnose, and the next
one.

## 8. A test checks the contract, not the formula

**What happened.** `SolveIndexA`'s test compared the formula with its own
number (+Y gives 90). The framing turns by R_x(−A), so "Turn a face up"
turned the face down, and no test could see it.

**Change.** No tool, one convention in the tests section of CLAUDE.md: a
geometric solver's test applies its answer through the real pipeline
(`BodiesInSetupFrame`, the post, the verifier) and asserts the outcome. An
audit of the other solvers (`SolveFaceUp` already does this, and
`DeriveRotaryTwist`, the lead and index solvers are the ones to check).

**Saving.** One wrong-side-of-the-part program that nothing flagged.

## 9. What the agents spent their time on

The build agents (nine worktree agents, not the fifteen Explore agents,
which averaged 3 minutes and cost 40 minutes between them) were active for
about 11.5 hours. 9.0 hours of that was inside tools, and 2.5 hours was the
model thinking and writing. Their tool time:

| activity | hours | share |
|---|---|---|
| compiling, often with a test run in the same call | 4.9 | 54% |
| tests without a build (suites, `-L cam`, smokes) | 1.7 | 19% |
| waiting on their own background tasks (`until` loops) | 1.3 | 14% |
| driving the app and the generator | 0.55 | 6% |
| reading code | 0.23 | 2.5% |
| git (a commit whose hook built, a rebase) | 0.20 | 2% |
| editing | 0.13 | 1.4% |

So the agents spent three quarters of their time on the build, and a
twentieth reading and writing the code they were sent to change. Item 2 is
most of the answer. The rest:

- **A docs-only change needs no worktree.** The phase-3 docs agent built a
  whole tree (23 minutes) to run the docs gate, and the doc audit that ran
  in main needed only an incremental build. Docs agents should work in main,
  after the code agents have merged.
- **Waiting in a loop is 14%.** One agent kept five `until` waiters alive
  on one CAM run. With item 3, an agent waits on the completion notice
  rather than a poll.
- **Rework after a rebase.** Every rebase onto a moved main rebuilt 9 to 12
  minutes, and the stock agent rebased three times. Merging agents in the
  order they finish, rather than asking each to rebase onto the last, saves
  the rebuild. The integrator resolves the one-line conflicts (the
  `kVersion` line, the format row, appended tests) in one pass.
- **Harness rejections.** 44 commands in worktrees were rejected as too
  complex to verify, and 36 more failed with an exit code, mostly builds
  killed for memory. Each costs a round trip.

## Smaller, recurring friction

- **zsh glob errors.** "no matches found" came up 87 times, from unquoted
  `--include=*.cpp` in grep. A line in CLAUDE.md: quote globs, or use the
  Grep tool.
- **Commands too complex to verify.** 36 commands in worktrees were
  rejected by the harness as too complex to verify. Splitting a pipeline
  into steps avoids the retry.
- **nix-shell entry.** Each entry costs about 3 s, and 396 of them came to
  about 20 minutes. Keep batching; `test-fast.sh` already does.
- **Idle waiting on agents.** Main waited about 2 hours. A phase that
  delegates everything leaves it waiting, so it should hold back work that
  is file-disjoint from the agents' (the next phase's plan, the generator,
  the doc audit).

## Verification

Items 1, 2, 3 and 6 are measured by the next round with agents: no earlyoom
kills of cc1plus or Claude in the journal, no `watchdog_smoke` failure in a
worktree, no exit 144, and no waiter notifications beyond one per run. Items
4 and 7 each come with the test that would have failed this round: the
crenel's refused op, and the job tab's idle frame. Item 8 is the
`SolveIndexA` contract test, which is already in.
