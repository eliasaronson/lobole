# The development loop: what the chess round cost, and what to change

The chess-set round (82 commits, formats v133 to v138, one main session with
26 agents) spent 27.8 hours of wall clock. The main agent was busy for 16 of
them. The rest was the night, 1 h 49 m dead after a crash, and about 2 h
idle waiting on agents. The numbers below come from the session and agent
transcripts, the task outputs, the earlyoom journal and git. They were
measured, not remembered.

Each change is small enough to do on its own. The saving is this round's
cost of not having it.

## 2. What is left of sharing ccache

Worktrees now share main's cache (docs/architecture.md#precompiled-headers).
A fresh tree builds in about 2 minutes, where it took 11.5. Still open:

- **The last 19 misses.** Some sources carry the tree's absolute path in a
  define (`DESIGNER_SOURCE_DIR`, for the test fixtures). A path computed at run
  time, or relative to the binary, removes them.
- **`watchdog_smoke` in Debug.** A developer debugging a stall builds Debug on
  purpose, and the smoke failed in every Debug tree: its stack shows
  `RunHeadless` rather than `StallSelfTest`.
- **The 40 GB store is full and evicting** (476 cleanups). Either raise
  `max_size`, or move other projects' caches out.

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

**Change.** `tools/build-jobs`'s callers take an `flock` on the build
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
  on one CAM run. With `tools/wait-for.sh`, an agent waits on the completion notice
  rather than a poll.
- **Rework after a rebase.** Every rebase onto a moved main rebuilt 9 to 12
  minutes, and the stock agent rebased three times. Merging agents in the
  order they finish, rather than asking each to rebase onto the last, saves
  the rebuild. The integrator resolves the one-line conflicts (the
  `kVersion` line, the format row, appended tests) in one pass.
- **Harness rejections.** 44 commands in worktrees were rejected as too
  complex to verify, and 36 more failed with an exit code, mostly builds
  killed for memory. Each costs a round trip.

## 10. What else holds the machine

- **Entering `nix-shell` costs 5 s.** About 400 entries in a round came to
  roughly 33 minutes. Change: `nix print-dev-env` into a cached env file,
  keyed on `shell.nix`'s hash, which the tool scripts source. A second nested
  shell is then also no longer possible to get wrong.

## Smaller, recurring friction

- **zsh glob errors.** "no matches found" came up 87 times, from unquoted
  `--include=*.cpp` in grep. A line in CLAUDE.md: quote globs, or use the
  Grep tool.
- **Commands too complex to verify.** 36 commands in worktrees were
  rejected by the harness as too complex to verify. Splitting a pipeline
  into steps avoids the retry.
- **Idle waiting on agents.** Main waited about 2 hours. A phase that
  delegates everything leaves it waiting, so it should hold back work that
  is file-disjoint from the agents' (the next phase's plan, the generator,
  the doc audit).

## Verification

Items 2 and 6 are measured by the next round with agents: every fresh
worktree near all-hits in `ccache -s`, and no merge that builds against a
half-compiled tree. Item 7 comes with the test that would have failed this
round, the job tab's idle frame. Item 8 is the `SolveIndexA` contract test,
which is already in, and the audit of the other solvers.
