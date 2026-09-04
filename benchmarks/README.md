# Agent benchmark — does Designer's API survive contact with an agent?

This directory answers one question, repeatably and in numbers:

> **Given only Designer's MCP surface and a description of a part, can an agent
> build that part — and does a change we just made to the API make that better
> or worse?**

Nothing else in the repo can answer it. The C++ suites test functions;
`rpc_gate_smoke` and the `describe` table test that the surface is *documented*;
the `defaults/*` GUI suite tests that a tool either acts or refuses. None of
them can see a rename, a reworded refusal or a thinned-out `help()` string make
the API harder to *discover*. Dogfooding rounds do see that — one expensive,
unrepeatable afternoon at a time. This is the repeatable version.

Prior art: [SmartAI/Chamfer](https://github.com/SmartAI/Chamfer) benchmarks a
CAD agent the same way — identical prompts across arms, a geometry oracle that
never asks the agent whether it succeeded, over-claims counted separately. The
shape here is deliberately theirs. What differs is the subject: Chamfer tunes an
agent against a fixed kernel; **here the arms are Designer configurations and
the agent is the fixed instrument.**

## Run it

```sh
nix-shell shell.nix --run "cd build && ninja bench_oracle"      # once
nix-shell shell.nix --run "python3 benchmarks/runners/reference.py"   # free
nix-shell shell.nix --run "python3 benchmarks/run.py --reps 3"  # costs money
```

`run.py --dry-run` prints the plan and its cost ceiling without spending
anything. `--tasks t2-flange --arms full --reps 1` is the cell-sized loop for
iterating on a task or an arm.

## The rule

**No benchmark, no claim.** A change to the tool surface — a new tool, a
removed one, a reworded description, a different refusal message, a change to
`help()` or `describe` — is a change to the thing this measures. Claiming it
helps means running a sweep before and after. Results are committed under
`results/<timestamp>-<model>/` and are **never overwritten**: the point of
keeping the bad ones is being able to see the line move.

## How it works

```
tasks/*.json     a part described in prose, plus the checks its geometry must pass
arms/*.json      a Designer configuration under test (tool surface, extra prompting)
oracle/          bench_oracle (C++/OCCT) reads a STEP back; grade.py applies the checks
runners/         reference.py — the by-hand build of every task; the harness's self-test
run.py           the sweep: arm x task x rep, sequential, one fresh instance each
results/         committed run records, one directory per sweep
```

### The oracle never asks the app

`oracle/oracle.cpp` links OpenCASCADE and **not one line of designer_core**. It
reports only facts a feature name cannot fake: solid count, bounding box,
volume, area, centre of mass, cylindrical faces grouped into real holes and
bosses (internal vs. external decided by the face's outward normal), and planar
faces that are not axis-aligned. A tree that reads `Extrude → "4x M6 clearance"`
over a solid with no holes in it scores zero.

This is not fussiness. The GPU topology solver once returned zero displacement
and reported convergence; anything that grades itself will eventually do that.
The oracle shares the kernel — there is only one on this machine — but none of
the feature, regeneration or toponaming code, which is where the defects this
is meant to catch actually live.

### The agent gets Designer and nothing else

Each run launches its own `designer --headless` on a kernel-chosen port and
pins that port into the agent's MCP config, so a sweep can never wander onto
the developer's instance on 8799. The agent runs with `--tools ""` — **no
built-in tools at all**: no shell, no file reads, no repo. Whatever it builds,
it built through the MCP surface alone. It runs from an empty directory outside
the repo with `--setting-sources ""`, so no CLAUDE.md, settings or memory reach
it; the probe for that is in this directory's history and answers
`NO PROJECT CONTEXT`.

Three things are taken out of the agent's hands on purpose:

- **The export.** The runner exports the STEP when the agent stops. Grading an
  agent that forgot to export measures the agent, not the API.
- **The verdict.** The oracle decides, always.
- **The port.** See above.

### Over-claims are counted separately

A run where the checks fail and the agent's last message says it succeeded is
an **over-claim**, tallied in its own column. This is the metric this project
should care about most: "refuse, don't substitute" and `regen_honesty_smoke.py`
enforce honesty one RPC at a time, and an over-claim count is the same question
asked of a whole session — did the agent notice it had failed? A silent
substitution upstream shows up here as an agent that cheerfully reports a part
it did not build.

### App-reported checks are labelled, never scored

`t3-parametric-bushing` asks whether geometry is *driven by a variable*, which
no STEP file records. Those checks read the app's own `get_state` and carry
`"source": "app"`. They are evidence, kept out of the oracle's score.

## Tasks

Eight parts, tiers T0–T3, 39 oracle checks per repetition. Every prompt pins
its axes and origin, states millimetres, and forbids unrequested fillets — so a
failed dimension check is a failure to model, not a failure to guess.

| id | tier | what it exercises |
|---|---|---|
| `t0-plate` | 0 | a single prism, the floor of the benchmark |
| `t0-spacer` | 0 | a bore that must read as a hole, not a boss |
| `t1-mount-plate` | 1 | four holes at named coordinates — the count is the claim |
| `t1-l-bracket` | 1 | two features fusing into **one** solid |
| `t2-flange` | 2 | a six-hole bolt circle; the centre of mass proves the spacing |
| `t2-stepped-shaft` | 2 | coaxial steps, and the centre of mass proves their order |
| `t3-gusset-bracket` | 3 | a genuinely inclined face (the sketch-plane V-axis trap) |
| `t3-parametric-bushing` | 3 | variables that drive geometry through a later edit |
| `t4-slider-crank` | 4 | three PARTS and an ASSEMBLY: mates that have to place things where the geometry says, not where a number was typed |
| `t5-crank-motion` | 5 | the same, plus **saying what it does** — the stroke over a full turn, and whether anything collides |

`runners/reference.py` builds all ten by hand and must score 48/48. It costs
nothing, it runs in about a minute, and it is the first thing to run when a
sweep looks wrong: **if the reference arm fails, the harness or the app is
broken, not the agent.** It is also where a task's expected numbers get checked
— two of the first eight were wrong when written, and T4's were wrong twice
(the largest solid is the piston, not the crank; and origin-to-origin mates
stack all three parts on the crank axis, which the bbox check caught).

### Why T4 and T5 exist

The first two sweeps put every arm at 1.000, and the second one showed the
other half of that ceiling: after three workstreams of new surface — named
frames, a batch RPC, an SDK — the benchmark could not detect an improvement
either, because **not one of the eight tasks used any of it.** T0–T3 are
single parts. Nothing there has a mate, so nothing there can be made better
or worse by anything to do with assemblies.

T4 is the smallest task that cannot be done without placing one part against
another, and T5 is the smallest that cannot be answered without moving it.
Writing them found a capability gap on the first run: `export_step` refused
on an assembly tab ("switch to a part tab"), so the one document kind a
customer asks for could not be exported to the interchange format at all.

## Arms

| arm | what it is |
|---|---|
| `full` | every MCP tool, no extra prompting — what an agent gets today |
| `rpc-only` | six tools (`describe`, `help`, `rpc`, `execute_script`, `get_state`, `check_geometry`) |
| `script-first` | every tool, told to build the part by writing a script |

`script-first` exists to break a confound. An agent left with `rpc` and
`execute_script` scripts whether or not it wanted to, so `rpc-only` differs from
`full` in **two** ways at once. Holding the surface constant and changing only
the strategy separates "fewer schemas helped" from "scripting helped" — and in
the first sweep it was decisively the schemas (see `REPORT.md`).

`rpc-only` loses **no capability** — `rpc` reaches every RPC there is. It loses
only schema bulk in the context window. Chamfer credited most of its efficiency
win to that exact trade (six curated tools against 59 schemas); this arm tests
whether it holds against an API that was not designed around it. The narrowing
is real, not a permission filter: `designer_mcp.py --only-tools` unregisters the
rest, and refuses loudly if it cannot, because an arm that silently served the
full surface would look like a valid comparison and be a duplicate.

## Reading a result

`REPORT.md` carries the current headline finding. 

`summary.md` has the headline table; `runs.jsonl` has one record per run with
its checks, tool calls by name, cost, tokens and the agent's final message;
`runs/<task>__<arm>__r<n>/` keeps that run's STEP, its MCP config and the full
agent stream.

Two cautions on the numbers:

- **Which tools an arm reached for** (`tools_used`) usually explains a result
  better than the totals do. "It never called `describe`" is a finding; "it
  used 40 tool calls" is a symptom.
- **Three repetitions is enough for the efficiency gap and thin for
  correctness.** A one- or two-check difference between arms is noise. Buy
  confidence with more *tasks* before more reps — 39 binary checks over eight
  parts separates arms better than the same spend on three.
