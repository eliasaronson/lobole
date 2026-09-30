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

Item 2 by `watchdog_smoke` passing in a Debug tree and a Release one. Item 8 is the
`SolveIndexA` contract test, which is already in, and the audit of the other
solvers.
