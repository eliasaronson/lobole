# First result — 2026-08-27, sonnet, 72 runs

Three arms, eight parts, three repetitions each, sequential on one machine
against `b628f9a`. 39 oracle checks per repetition. Total spend $16.55.

| arm | checks | clean runs | over-claims | tool calls | cost | wall |
|---|---|---|---|---|---|---|
| `full` | 117/117 | 24/24 | 0 | 198 | $4.66 | 665 s |
| `rpc-only` | 117/117 | 24/24 | 0 | 324 | $6.65 | 1143 s |
| `script-first` | 117/117 | 24/24 | 0 | 211 | $5.24 | 747 s |

## Every arm is perfect, and that is the most important number here

234 of 234 oracle checks passed. Sonnet, given nothing but this MCP surface —
no shell, no repo, no documentation beyond what the tools describe about
themselves — built all eight parts every time, including a six-hole bolt circle
whose spacing the centre-of-mass check would have caught if it were uneven, and
a bushing that had to follow a variable edit after the fact.

It also **never once over-claimed**. Not one run ended with the agent reporting
success over geometry that failed. Given how much of this project's design
turns on refusing rather than substituting, that is the result worth having.

The cost is that **the benchmark cannot presently detect a correctness
regression.** Every arm sits on the ceiling, so the pass-rate interval is
degenerate — there is no room below 1.000 for a bad change to show up in. As a
gate it measures efficiency today and nothing else. See "What to do next".

## Fewer tool schemas cost 43%, and it is the schemas, not the scripting

Chamfer credited most of its efficiency win to six curated tools replacing 59
generic ones. **That inverts here**, and the two extra arms say precisely why.

`full` → `rpc-only` (schemas removed, strategy free to change):

| metric | full | rpc-only | delta | 95% CI |
|---|---|---|---|---|
| cost / run | $0.194 | $0.277 | +43% | [+0.054, +0.111] |
| tool calls / run | 8.25 | 13.50 | +64% | [+3.33, +7.54] |
| wall / run | 27.7 s | 47.6 s | +72% | [+10.5, +29.4] |

`full` → `script-first` (same schemas, told to script):

| metric | full | script-first | delta | 95% CI |
|---|---|---|---|---|
| cost / run | $0.194 | $0.218 | +12% | [-0.006, +0.070] — straddles zero |
| tool calls / run | 8.25 | 8.79 | +7% | [-0.79, +2.50] — straddles zero |
| wall / run | 27.7 s | 31.1 s | +12% | [-2.03, +10.49] — straddles zero |

So the penalty is **entirely** the missing schemas. Scripting is free: an agent
told to build the part in one `execute_script` call performs the same as one
calling `add_feature` per feature, and the tool log shows it genuinely changed
strategy (64 `execute_script` calls against 18 in `full`) without changing the
outcome. Both approaches work, which is a good property to have measured.

The mechanism behind the `rpc-only` penalty is legible in what it reached for:

```
full          add_feature x79  describe x42  execute_script x18  check_geometry x16  screenshot x15
rpc-only      execute_script x92  rpc x77  describe x60  help x57  check_geometry x25
script-first  execute_script x64  add_feature x39  describe x39  check_geometry x16  screenshot x15
```

`rpc-only` spent **57 `help` and 60 `describe` calls** re-deriving at runtime
what `full` reads for free in the schemas it was handed. These 25 tools are
already curated for this domain; collapsing them to `rpc` defers the work
rather than removing it. Chamfer's finding was about replacing *generic* tool
sprawl, and it does not transfer to a surface that was designed.

One incidental finding: `full` called `screenshot` 15 times unprompted. The
agent looks at the part it is building, on a benchmark where looking earns it
nothing, since the oracle grades the STEP.

## One cell in this sweep was mine

`t1-mount-plate / script-first / r1` recorded 0/5 with zero tool calls and zero
cost, because I deleted the directory it was about to launch in while the sweep
was running. Two fixes rather than a patched-over number: the runner now
creates that directory immediately before launching, and a cell where the agent
never started is marked `harness_error` and **excluded from every total instead
of scoring zero**. Counting a failed `claude` invocation as a failed part
substitutes a plausible number for a missing one. The re-run passes 5/5; the
original sweep directory is left exactly as recorded, with `NOTES.md` beside
it, because "never overwrite results" has to hold when the bad result is mine.

## What to do next

**A T4 tier, or this stays an efficiency meter.** The eight current tasks are
prose-describable parts with analytically computable volumes — that is what
makes them gradeable, and it is also what makes them easy. Harder tasks need to
be built the other way round: take a real part from `scratch/`, and instead of
asserting a volume nobody can derive, assert *relationships* the oracle can
check without a reference number — these faces are parallel, this wall is
uniformly thick, these holes share a diameter and lie on one circle, this
pocket's floor is flat. Candidates from the existing rounds: the c-clamp's
screw and jaw, a dock bracket, the enclosure's port and bracing.

**Then re-run before and after any change to the surface.** That is the whole
point of the directory, and it costs about $17 and 40 minutes at this size.
