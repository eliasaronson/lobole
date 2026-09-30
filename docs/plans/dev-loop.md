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

- **`watchdog_smoke` in Debug.** A developer debugging a stall builds Debug on
  purpose, and the smoke failed in every Debug tree: its stack shows
  `RunHeadless` rather than `StallSelfTest`.

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

## Verification

Item 2 by `watchdog_smoke` passing in a Debug tree and a Release one. Item 7
comes with the test that would have failed this round, the job tab's idle
frame, failing against a build without the stock-view cache. Item 8 is the
`SolveIndexA` contract test, which is already in, and the audit of the other
solvers.
