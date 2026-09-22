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
arms/document_mcp.py  the direct-emission arm's server: no RPCs, one document
oracle/          bench_oracle (C++/OCCT) reads a STEP back; grade.py applies the checks
runners/         reference.py — the by-hand build of every task; the harness's self-test
                 villain.py   — a block of each task's own bbox; the harness's floor
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

Static parts in tiers T0–T3, two mechanisms at T4/T5, and one Fit task graded
against a host the agent never sees. Every prompt pins its axes and origin,
states millimetres, and forbids unrequested fillets — so a failed dimension
check is a failure to model, not a failure to guess.

The count is deliberately not written down: `reference.py` prints the tasks and
their checks, and this line has been wrong twice already (eight parts, then
thirteen; 48/48, then 68/68).

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
| `t3-coil-spring` | 3 | a helix that must be SWEPT — stacked rings have no bspline |
| `t3-taper-adapter` | 3 | a real cone; a fine enough disc stack passes every dimension |
| `t3-bent-tube` | 3 | a swept bend, by either route that reaches one |
| `t4-slider-crank` | 4 | three PARTS and an ASSEMBLY: mates that have to place things where the geometry says, not where a number was typed |
| `t5-crank-motion` | 5 | the same, plus **saying what it does** — the stroke over a full turn, and whether anything collides |
| `f1-cap-tube` | fit | a part graded against ANOTHER part: an end cap mated to a tube the agent never sees. Clearance, interference volume and real contact area — the class of error that a dimensionally perfect part can still commit |

`runners/reference.py` builds every task by hand and must score every check. It
costs nothing, it runs in about a minute, and it is the first thing to run when
a sweep looks wrong: **if the reference arm fails, the harness or the app is
broken, not the agent.** It is also where a task's expected numbers get checked
— two of the first eight were wrong when written, and T4's were wrong twice
(the largest solid is the piston, not the crank; and origin-to-origin mates
stack all three parts on the crank axis, which the bbox check caught).

`runners/negative_controls.py` is its counterpart and answers the question
`reference.py` cannot: is a task DISCRIMINATING? It builds each
construction-sensitive part the wrong way — a spring as flat rings, a cone as
a stack of discs — and fails if one of them passes. A check that nothing ever
fails is not a check, and adding one without this only proves that everything
passes. Sixty discs clear `solids`, `bbox`, `volume`, the bore and the centre
of mass; only the surface census stops them.

### The Fit suite: graded against a part the agent never saw

Every other task grades a part against numbers. `f1-cap-tube` grades it
against **another part** — a tube, supplied as an input marked
`agent_visible: false`, which the grader mates the candidate to and the agent
is never handed. The prompt says the tube is 30 across with a 2 wall and its
end at Z = 40; the file that proves it stays with the grader, because a task
that hands over the geometry is asking for a boolean, not a design.

`bench_oracle --host HOST.step` then reports three things a single "does it
clash" cannot:

    distance_mm         how close the two come
    interference_mm3    how much solid is in solid
    contact_area_mm2    how much of the candidate is actually touching

with `mate_clearance`, `interference_volume`, `contact_area` and `envelope` as
the check kinds over them. `run.py` builds the prompt through `prompt_for`,
which refuses to emit a host — the rule, rather than the accident that nothing
put `inputs` in the prompt before.

**Contact area is measured on the shared patch, not the near face.** The first
cut summed every candidate face within epsilon of the host, and read 725.8 mm2
for a cap resting on a 175.93 mm2 annulus: a face counts in full if any part
of it is near, including the part spanning the tube's bore, which touches
nothing. That number cannot tell a seated cap from one grazing at a point,
which is the only thing a contact check is for. Intersecting the face patches
gives 175.93 — the annulus, exactly.

It earns its keep in `negative_controls.py`, where both wrong caps are
dimensionally perfect and **the surface census catches neither**:

    the right cap, 0.5 mm off the tube    4/6  mate_clearance 0.2, contact_area 0
    no clearance — 29.4 bore on a 30 tube 3/6  interference_volume 335.9 mm3

That is the class of error nothing here could see before: a part that is
correct and does not fit.

### The `document` arm: is the RPC surface a help or a tax?

`full`, `rpc-only` and `script-first` all reach the app through the API, so no
arm here could ever answer the question vcad's mecheval answers against
themselves. Their runs, same model, same tiers (claude-opus-4-7):

    tier   direct (emit the .vcad JSON)   through the MCP tool loop
    a1              88%                            82%
    a2             100%                            72%
    a3              56%                            56%
    a4              82%                            42%

**Emitting the document format in one shot beat driving their own API.** The
`document` arm asks the same of ours: `benchmarks/arms/document_mcp.py` serves
exactly two tools — `document_format()`, which returns the format version and
a worked example the app builds itself (so it cannot drift from what the
loader accepts), and `write_document(json)`, which swallows a whole project
and reports what the kernel made of it.

It deliberately does NOT live in `tools/designer_mcp.py`. A product tool that
takes a whole document would be a second way to author, and the benchmark
would then be measuring a surface that exists because the benchmark wanted it.

### pass^k

`--reps 3` averaged, which reports a one-in-three failure as 67%. That is the
wrong shape for "can an agent do this *reliably*", which is what the arms are
being compared on. The summary now also carries **pass^k**: a task counts only
if every rep of it was clean. On synthetic records with one task clean 3/3 and
one clean 2/3, the old columns read 23/24 checks and 5/6 clean runs; pass^k
reads 1/2. The harsher number is the honest one.

### The other control: `runners/villain.py`

`reference.py` is the ceiling. The villain is the floor, and it asks the
opposite question: **does a task's check set actually distinguish the part
from a block?** It reads each task's own `bbox` check and extrudes exactly
that block — no holes, no steps, no bosses, prompt ignored entirely. Every
check that survives is a check doing no work, and a task the villain passes
outright is broken.

It costs nothing and it earned its keep on the first run:

    BROKEN t0-plate        4/4   bbox, inclined_planes, solids, volume
    weak   t1-l-bracket    3/4   bbox, inclined_planes, solids

`t0-plate` is degenerate rather than broken — the part described IS the block
of its own bbox — so it now declares `villain_passable: true` with a reason.
Declaring it rather than letting the runner infer it means a task that starts
or stops passing the villain for any OTHER reason still fails loudly
(`MOVED`). `t1-l-bracket` was the real finding: an L is not a block, yet
three of its four checks survived one, so an agent that built the wrong
bracket still scored 75%. Its discriminating check was `volume` alone. It now
also asserts fourteen planar faces — a block has six.

That gap existed because `bench_oracle` had emitted `faces_by_kind` and `area`
all along and `grade.py` implemented neither as a check. Both are check kinds
now:

    {"kind": "faces", "of": "plane", "eq": 14}
    {"kind": "area", "value": 17088.0, "tol_pct": 1.0}

`faces` is the cheapest check that needs the actual shape; `area` is sensitive
to every hole, step and pocket that a volume figure can be made to compensate
for. Reach for one of them when a new task's checks are all size-and-mass.

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
Since X4 (docs/interchange/step-export-plan.md) the file is structured — one
named product per part, placed per occurrence, sub-assemblies nested. The
oracle still reads it flat (`STEPControl_Reader`, `OneShape`), which visits
the placed instances, so its solid counts, volumes and per-solid centres are
unchanged: `reference.py`, `negative_controls.py` and `villain.py` were rerun
on the new writer and score exactly as before.

## Arms

| arm | what it is |
|---|---|
| `full` | every MCP tool, no extra prompting — what an agent gets today |
| `rpc-only` | six tools (`describe`, `help`, `rpc`, `execute_script`, `get_state`, `check_geometry`) |
| `script-first` | every tool, told to build the part by writing a script |
| `no-inspect` | the floor: eight tools, none of which can ask an independent question about the part |

`script-first` exists to break a confound. An agent left with `rpc` and
`execute_script` scripts whether or not it wanted to, so `rpc-only` differs from
`full` in **two** ways at once. Holding the surface constant and changing only
the strategy separates "fewer schemas helped" from "scripting helped" — and in
the first sweep it was decisively the schemas (see `REPORT.md`).

`no-inspect` is the only arm that deliberately loses capability. It exists
because this benchmark had a ceiling (`reference.py`, 67/67) and no floor, so
nothing said how much of a perfect score was free — BenchCAD's equivalent, a
blank-image baseline, scores 0.375 against a 0.587 best, and it is the most
useful row on their board. The deprivation is partial on purpose: a mutating
call still answers with its own digest and regen summary, so what the arm
removes is asking about anything the agent did not just cause. It also takes a
default sweep from 117 runs to 156 — `--arms full,rpc-only,script-first` is
the old three.

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
