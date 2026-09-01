# Capability + architecture review (2026-08-31)

> Status: **the testing and tooling halves are partly BUILT — see "What was
> done" at the bottom.** The capability findings (A1-A3, S1, F1) remain
> findings only, and nothing there is scheduled. This is a read of the
> tree against what the mainstream modellers do, taken after the CAM
> projection fix (`286ef6b`) and prompted by "what are we missing, what is
> badly structured, what do we do worse than FreeCAD and Onshape".
>
> It is a different kind of pass from
> [architecture-review-2026-08-24.md](architecture-review-2026-08-24.md),
> which measured layering, duplication, unit sizes and dead code *inside* the
> tree and shipped five stages of cleanup. This one looks outward: capability
> walls, absent feature families, and the two or three decisions that would be
> expensive to revisit later. Where that review asked "is this well built",
> this one asks "is this the right thing, and what can it never do as built".
>
> Everything below is measured against the source, not against the roadmap's
> own summaries — the 08-25 audit's lesson about status lines applies to
> capability claims too.

---

## A1 — The assembly solve is not a solver, and closed loops are unassemblable

**The wall.** `core/AssemblySolve.h` states it without varnish:

> "position each mated occurrence relative to its neighbours by direct frame
> alignment — **no numeric 3D solver**. Each mate fully determines one
> occurrence relative to the other (given its stored DOF pose), so placement
> is a closed-form transform propagated sequentially from the `fixed` anchors
> through the mate graph. Mate *trees* are solved; genuine loops are detected
> and reported (per-mate error), **not solved**."

675 lines, and an honest v1 — A3 shipped all six mate types on it, and for a
tree of parts bolted to each other it is the right amount of machinery. But
it means **any mechanism containing a closed kinematic loop cannot be
assembled at all**: a four-bar linkage, a slider-crank, a toggle clamp, a
scissor lift, a pantograph. The mates go red and there is no setting that
helps, because the limitation is the propagation, not the mate types.

**Why it matters more than it used to.** This is the one place where a mature
open-source competitor is now visibly ahead rather than merely different:
FreeCAD 1.0 ships an Assembly workbench over the OndselSolver, a genuine 3D
constraint solver. Onshape and the commercial modellers have had one forever.
Everything else in this repo is either comparable or better; this is a plain
gap.

**Why it is also the most tractable of the three architectural items.** The
parts that usually make a 3D solver expensive already exist: the six mate
types, connector frames (`ConnectorLocalFrame`), stored DOF poses, the mate
graph, and the drag-within-mate interaction. What is missing is the numeric
core — and this codebase already vendors PlaneGCS for the 2D case and Eigen
for FEM, so neither the dependency nor the numerical idiom would be new.
Nothing about it threatens the determinism guarantees that checkpoints, git
merge and the golden corpus rest on: it changes where an occurrence is placed,
not what a document means.

**Open question before costing it:** whether to grow PlaneGCS's approach into
3D, vendor a solver, or write a small Newton/DOF-analysis core against the
existing mate set. Deciding that is most of the design.

---

## A2 — Regeneration cannot cross a document boundary

**The shape.** A part is a document, and regeneration resolves only that
document's own feature list. Three items deferred separately are all the same
missing edge:

- cross-part **linked sketch copies**
  ([sketching/copy-links.md](sketching/copy-links.md) phase 2 — *"regeneration
  would need to resolve another part's document … it widens regen's input
  surface"*),
- **cross-part references in context** (assemblies plan, "remain"),
- **assembly-level features** (assemblies plan, "own design").

One architectural decision, showing up three times on three different lists.
Naming it once is most of the value of this entry.

**What it costs today.** Top-down design is the casualty: you cannot derive
part B's geometry from part A's. The in-context edit shipped in A4 ghosts the
surrounding assembly so you can *see* the neighbours; you still cannot
*reference* them.

**The comparison.** Onshape's **Part Studio** is exactly this solved: several
parts share one regeneration graph, and assemblies reference the results.
Designer is closer to that shape than it first appears — a document already
holds multiple bodies (`GetBodies()`, `BodyMode.cpp`, body-scope
`contact_report`), so the multi-body half is there. The gap is purely that a
body in document 2 cannot derive from geometry in document 1.

**Why I would think hard before building it.** Single-document determinism is
load-bearing for far more than modelling: checkpoints, rollback, the git merge
machinery, the golden corpus, and the "regenerates from scratch, identically"
promise the whole design rests on. A cross-document dependency graph puts
cycle detection and staleness propagation into the middle of that, and the
failure mode is not a crash but a document that regenerates differently on
someone else's machine. **Highest value and highest risk on this list.** If it
is ever done it wants its own design doc and its own gate before any code.

---

## A3 — There is no general "long operation off the frame" mechanism

Four instances, three hand-rolled answers, one still unfixed:

| operation | today |
|---|---|
| STEP import | worker thread + a modal (import-plan, 2026-08-30) |
| FEM solve | `async` + `study_status` polling |
| FEM `mesh_study` | **synchronous**, outlives the client's socket timeout (known-bugs) |
| CAM adaptive clearing | **synchronous on the UI thread, ~7 s per edit** (known-bugs) |

Regeneration itself is synchronous on the calling thread, which for the GUI is
the main thread.

Each case grew its own shape, and the two that have not been done yet are open
bugs rather than missing features. The recommendation is small and concrete:
**when the CAM adaptive freeze is fixed, build the abstraction instead of the
fourth bespoke worker** — a job with a progress state, a cancel, and a row in
the panel that owns it. `mesh_study` is then the first free adopter.

---

## S1 — Two files outran the god-file refactor

The [08-24 review](architecture-review-2026-08-24.md) did real work and its
policy is right ("split what the next feature extends, not big-bang"). Two
files have been extended a lot since:

| file | lines | shape |
|---|---|---|
| `src/ui/AppActions.cpp` | **5,714** | 212 functions, four section comments; FEM mode, project variables, undo routing and drawing view projection in one unit |
| `src/ui/SketchInteraction.cpp` | **4,275** | |

They are the two largest non-vendored files in the tree (`main.cpp` is 2,508,
`CamToolpath.cpp` 2,389). Both live in `ui/`, which is also where test cover
is thinnest — the combination is what makes them worth naming rather than
either fact alone.

Not urgent, and explicitly **not** a big-bang candidate. The trigger should be
the next feature that extends either one.

---

## F1 — Feature families that are simply absent

Ordered by what I would actually care about, not by size.

- **A standard parts / fastener library.** Absent entirely, and the cheapest
  high-value gap here: it is data plus a placement UI, no kernel work. Every
  assembly dogfood round so far (C-clamp, Marsliden dock, W 170 S speaker)
  hand-modelled its screws. FreeCAD has FastenersWB; Onshape ships a large
  standard content library.
- **User-definable feature types.** `kFeatureTypes` (SerializeFeatures.cpp) is
  a compile-time C++ table, so extending the modelling vocabulary means
  recompiling the application. FreeCAD has Python workbenches; Onshape has
  FeatureScript. This one is pointed given goal 5 of the roadmap: an agent can
  *drive* every feature this app has and cannot *define* one.
- **Surfacing / NURBS surface modelling**, and **sheet metal**. Both standard
  elsewhere, both absent. My recommendation is to keep skipping them — each is
  a deep workstream serving users this project does not have, and the breadth
  trap is the specific way an open-source CAD becomes worse than the sum of
  its parts.
- **Drawing section and detail views**, on-sheet BOM/balloons, GD&T/PMI —
  already deferred in the drawings plan, and normal expectations for drafting
  output.
- **Assembly-level features** and **per-occurrence part-config selection** —
  already on the deferred list; both are partly blocked by **A2**.
- Sketch **B-splines with fully-solved poles** (phase L deferral);
  **dimensional analysis + unit display** (variables plan's deliberate
  remainder).

---

## What we do BETTER, and should not trade away

Recorded because it should shape what does *not* get chased.

- **Toponaming designed in from the start** (T1–T5). FreeCAD fought this for a
  decade and shipped mitigations in 1.0. It is why "change a fillet and your
  CAM job survives" is true here — machining selections are `ElementRef`s —
  and no other open CAD gives you that.
- **The CAM verifier.** Post the file, parse it back, simulate it, report
  gouge and leftover, with **one** height map feeding both the picture and the
  verdict. FreeCAD's two disagree.
- **A diffable text format with real git integration**, checkpoints, branches,
  merge with a conflicts panel that names a conflict in domain words.
- **The agent interface as a first-class client**, not a bolt-on: the same
  command layer humans use, `describe` schemas gated against the handlers, and
  a benchmark that grades an agent holding only the MCP surface.
- **The refusal / honesty conventions**, gated by `test_conventions.cpp` — a
  real engineering asset, and the reason the front_wall gouge was findable at
  all rather than being absorbed as "CAM is like that".

---

## Sequencing, if these are ever picked up

Not a schedule — a preference order, with the reasoning:

1. **A1 (assembly solver)** — a hard capability wall rather than a soft one, a
   competitor has passed us on it, the prerequisites are in the tree, and it
   does not endanger the determinism guarantees.
2. **F1's standard parts library** — cheap, immediately useful in every
   assembly round, no kernel risk.
3. **A3** — as a by-product of fixing the CAM adaptive freeze, which is owed
   anyway.
4. **A2** — the biggest unlock and the biggest risk; wants its own design doc
   and its own gate before any code, not a slot in a round.
5. **S1** — opportunistic, when the next feature extends one of those files.

---

# The testing system

A separate pass, same day, over the three suites and the harness around them.
The headline is that the machinery is unusually good and one number in it has
silently gone stale.

## What is genuinely strong, and should not be traded away

Named first because the list below is a list of gaps, and gaps read louder
than they weigh.

- **Convention gates that prevent classes rather than instances.**
  `test_conventions.cpp` (a silent bool parser cannot arrive; a renderer
  cannot ship half-wired), `rpc_params_gate.py` (describe's schema against
  what the handlers actually read, **in both directions**), `kFeatureTypes`
  authoritative with sabotage checked both ways, `config_isolation_lint.py`
  (nothing may launch the binary except `smoke_app.py`). Most codebases have
  none of these.
- **`tests/blindspots/`** — one file per known AGENT mistake (the V-axis
  sketch plane, the away-facing cut, hand-derived quaternions, almost-touching
  assemblies), each asserting that the wrong pattern is caught loudly AND the
  right one works, with "never delete one" as the rule. Session memory made
  executable.
- **`tools/coverage_report.py` asks the right question** — not a percentage,
  but "which functions in `src/core` has nothing ever executed". That is
  precisely the bug class this project keeps finding.
- **`coverage_matrix.py`** generated from the binary's own test list, so the
  tool x tab-kind asymmetry that has cost three bugs cannot be asserted away.
- Golden corpus **current at v68**, gated in both directions; a failing GUI
  shard archived with its seed and a runnable `reproduce.sh`.
- 37.8k lines of test against 117.7k of source (~32%), across 925 headless +
  246 GUI + the python `gui` three.

## T1 — The inner loop is four times slower than it says it is — FIXED

**Measured today: `tools/test-fast.sh` takes 48.7 s.** Its own header says
~8 s and CLAUDE.md says 11 s.

The cause is a stale exclusion list, not a regression. The script skips slow
gates **by name**:

    SLOW='Topo:|Topology|RunTopology|TopologyOptimize|femgpu|fem_smoke|topo_smoke|offscreen_smoke'

That was right when it was written — topology and FEM were the long poles. The
long poles now are four CAM tests that arrived later and are not in it:

| test | CPU |
|---|---|
| Topo: SIMP cantilever (GATE) | 79.4 s |
| cam: an opening the cutter fits into is still an island | 53.8 s |
| cam: clearing the outline still goes around material | 34.9 s |
| cam: clearing the outline passes over holes | 27.1 s |
| cam: a floor with both keeps the boss and drops the hole | 22.1 s |

So the fast loop now saves 30 s off a 79 s suite rather than 70 s, and the
number every doc quotes is wrong.

**The fix worth making is structural rather than an edit to the regex.** A
by-name list in `tools/` cannot know about a slow test added in
`tests/CMakeLists.txt`, which is why it drifted and will drift again. Give
slow tests a ctest **label** (`slow`) where they are declared, and let
`test-fast.sh` be `ctest -LE gui -LE slow`. Then a new heavy test opts itself
out at the moment it is written, by someone who knows it is heavy. Update the
two docs quoting the old figure in the same commit.

Worth asking separately why four CAM tests cost 138 s of CPU between them.

## T2 — The renderer has no pixel-regression gate

The most effective renderer verification this project has ever run was manual:
the 08-24 architecture review re-rendered a fixed three-body scene through the
real GPU after stages 2 and 5 and diffed the PNG — *"0 of 2,764,800 bytes
differ"*. That is the check that pinned a renderer rewrite no test covers, and
it exists nowhere in the suite.

What the suite has instead is `render_ao_smoke.py`, and it is a good design —
build a scene whose correct ambient occlusion is confined to one known place,
then assert there is none anywhere else, so any mapping error fails whatever
its cause. But it covers AO placement, and `main.cpp` plus the renderers are
the acknowledged untested layer.

`--offscreen` already gives a real-GPU render with no window and no display
server, which is most of the work. The remaining design question is the only
hard part: exact byte equality is machine- and driver-specific, so this wants
a tolerance and a **locally recorded** baseline rather than a golden PNG in
git. A perceptual-hash or a bounded per-channel delta against a baseline
recorded by `--record` is the shape.

Best value-for-effort item in this section.

## T3 — `gui_smoke` has to be excluded by hand, every time — FIXED

`tests/CMakeLists.txt:143` gives `gui_smoke` the label `gui` — the same label
the sharded runner's tests carry. So `ctest -L gui` re-runs the entire GUI
suite serially: **365 s against the sharder's 100 s**, for the same tests.
CLAUDE.md warns about it and every documented command carries `-E gui_smoke`.

That is knowledge where a label would do. Give it its own label (or drop `gui`
from it) and the documented command stops needing the exclusion.

## T4 — Coverage is occasional and leaves no trace

`tools/coverage.sh` is deliberately not in CI, and that is right — it is a
20-40 minute instrumented audit. But there is **no record of the last run
anywhere in the repo**, so the one output it exists to produce — a named
0%-covered function in `src/core` — is visible only to whoever ran it, and
only until they close the terminal.

A committed `docs/coverage-<date>.md` holding just the zero-covered list, per
run, would make it a trend instead of an occasion. Both bugs found this
session were in the "nothing has ever executed this path" class.

## T5 — No property-based testing, and one property is worth more than the rest — BUILT, and it found a bug

**Regenerate the same document twice and compare the bodies.** Determinism is
the promise that checkpoints, rollback, git merge, baked geometry and the
whole golden corpus rest on, and nothing asserts it directly — the goldens
compare a document against a recorded expectation, which is a different claim
from "this document regenerates to the same thing every time". Cheap, and it
is the assumption with the most weight on it.

Secondary candidates, in order: the expression evaluator (a small typed input
space, easy to generate); the document parser's refusal behaviour under
malformed input, fuzzed over `tests/golden/` as a seed corpus — the "refuse,
don't substitute" conviction has never been tested against inputs nobody
wrote by hand.

## T6 — Performance regressions have no gate

Three performance rounds have shipped with their numbers recorded in docs
(scale: open 259 s -> 0.04 s; GPU: Arc 40 -> 30 ms; import: resync 5.9 s ->
30 ms). **None of those numbers is asserted anywhere**, so every one of them
can regress silently — and the import win in particular rests on a cache whose
eviction rule was subtle enough to need two attempts.

Two wall-clock gates do exist (`FaceSnapPoints stays linear on a drilled
plate`, and the outline-edge build), and the first is the right pattern to
copy: it asserts the *shape* of the cost rather than an absolute millisecond
count, so it survives a slower machine. Not arguing for perf CI — arguing that
the three biggest wins deserve the FaceSnapPoints treatment.

## T7 — The order-dependent GUI failures are still undiagnosed

Existing entry in [known-bugs.md](known-bugs.md); repeated here only because
this pass is about the harness. The infrastructure is now correct — shuffle
with a recorded seed, and a failing shard archived with its filter and a
`reproduce.sh`. What is missing is the habit: nothing runs the suite shuffled
outside CI, so the seed that would make one reproducible is never generated
locally.

## Order I would take these

1. **T1** — trivial, and it is charging every edit made today.
2. **T3** — a label; smaller than T1 and removes a documented footgun.
3. **T2** — the real coverage hole, and the one that needs actual design.
4. **T5's determinism property** — small, and it guards the largest assumption
   in the codebase.
5. **T4**, **T6**, **T7** — habits and gates rather than code.

---

# The development tooling (build, debug, introspect, worktrees, memory, profiling)

Same day, same shape: what is strong, then the gaps. The short version is that
introspection and build tooling are excellent, sanitizers are well done, and
the two thin areas are **CPU profiling** and **GPU memory**.

## Strong, and unusual

`ci-local.sh` derives its package list, container images and test exclusions
**from ci.yml** so the two cannot drift. Sanitizer runtime options live in the
binary (`SanitizerDefaults.cpp`), so a sanitized build behaves the same however
it is launched — which is how the real findings actually turned up. Leak
detection stays ON; only OCC's container-overflow false positive is disabled.
`worktree-build.sh` knows the empty-submodule trap, and the GUI suite
self-serializes machine-wide by flock so parallel worktrees cannot flake each
other. `CrashHandler` prints a symbolized `std::stacktrace` into the dev-watch
log. `git-sessions.sh` answers "which session wrote this" from a trailer
nobody thought to read. Introspection — `describe`/`help`/`instances`/`rpc`,
the MCP surface, `designer_replay.py`, `coverage_matrix.py`,
`rpc_params_scan.py` — is the best-served area in the repo.

## P1 — There is no CPU profiler workflow, and every perf win so far was found by hand — FIXED

`perf`, flamegraphs, Tracy, callgrind: **none appear anywhere** in `tools/`,
`docs/` or CLAUDE.md. Every performance result this project has recorded was
found by hand-placing `std::chrono::steady_clock` around a suspected phase and
reading a log line — the 259 s open, the 5.9 s assembly resync, the STEP file
parsed twice, the 7 s adaptive pocket.

That method works (the numbers in `performance/` are real) but it only ever
finds what you already suspected, and it is re-typed each round: there is not
even a shared scoped timer — `AssemblyActions.cpp:1234` declares a local
`SyncClock` for the resync instrumentation.

Two cheap pieces: a `tools/profile.sh` wrapping `perf record` + a flamegraph
against a scripted workload (the remote makes the workload trivially
reproducible), and one reusable scoped timer that logs, so phase timing stops
being retyped. The next perf round then starts from a profile instead of a
hypothesis.

## P2 — No GPU memory visibility — RESOLVED, and the ceiling was a phantom

> **Corrected 2026-09-01.** The entry below inferred a limit from the Vulkan
> spec floor and was wrong. Read the correction under it before the original.

`vkutil::CreateBuffer` does **one `vkAllocateMemory` per buffer**
(`VulkanCommon.cpp:67`) — no VMA, no sub-allocation. The header records the
scale: *"a 299-part assembly builds 1196 buffers / 51 MB"*
(`VulkanCommon.h:91`).

Vulkan's `maxMemoryAllocationCount` is commonly **4096** on desktop drivers.
At roughly four allocations per part, the ceiling is around **1000 parts** —
and a real imported machine assembly can exceed that. `StagingBatch` (2026-08-30)
fixed the *submit* count, not the *allocation* count, so this one is untouched.
51 MB across 1196 allocations also wastes whatever each rounds up to.

Underneath that, **nothing reports device memory at all.** `frame_stats` has
per-pass GPU timestamps and no VRAM figure; `kPartRenderCacheBudgetBytes` is an
app-side estimate that is never checked against the device's actual budget. A
device-memory leak is invisible until the driver refuses an allocation.

Two separable fixes: **report VRAM in `frame_stats`** (cheap, immediately
useful, and it makes the cache budget checkable), then **sub-allocate** (bigger,
and the part that lifts the ceiling).

### The correction (2026-09-01)

Two things were wrong with the above, and the second one is the whole item.

**The VRAM half was redundant.** `nvidia-smi` and `nvtop` are both installed
and already report per-process device memory — `build/designer, 71 MiB` of
8188 — live, for free. Putting heap usage into `frame_stats` would have
duplicated an external tool while costing a seam decision, an extension
enablement in `main.cpp`, an RPC and a describe entry. The argument for doing
it anyway was the allocation COUNT, which no external tool reports.

**And the ceiling does not exist.** "commonly 4096 on desktop drivers" was
inferred from the Vulkan *floor* — `maxMemoryAllocationCount` must be at least
4096 — and never checked. Measured on every device this project targets:

| device | maxMemoryAllocationCount |
|---|---|
| NVIDIA RTX 4060 Laptop | 4294967295 |
| Intel Arc (MTL) | 4294967295 |
| llvmpipe (what CI runs) | 4294967295 |

No handle ceiling anywhere. The "around 1000 parts" limit was arithmetic on a
number nobody had read, and **the sub-allocator this entry called "the actual
fix" is not needed.**

**What shipped instead**, about fifteen lines and no seam at all: the limit is
logged beside the device name, so a driver that *does* impose one says so on
the first line of its log rather than at the allocation that fails; and the
assembly sync's existing phase breakdown gained the allocations that sync made.

    Selected GPU: NVIDIA GeForce RTX 4060 Laptop GPU (type 2),
      max device allocations 4294967295
    Assembly synced: 1 occurrences, 1 instances, 1 parts cached (1 tessellated)
      in 9 ms [build 3, instances 0, picking 0, of which parallel
      tessellation 4], 8 device allocations (79 total)

That second number keeps its value with the ceiling gone: it is a work-volume
signal the phase timings cannot give, and a sync that starts making ten times
what it used to has stopped reusing something — visible here before it shows
in the clock.

This is the second finding this session that did not survive contact with a
measurement, after the CAM round's "four fixtures failed to reproduce it". Both
were plausible, both were argued from a number nobody had looked at.

## P3 — No ThreadSanitizer, in a tree that is now genuinely concurrent — BUILD MODE ADDED

`ENABLE_SANITIZERS` is `-fsanitize=address,undefined`. Neither catches a data
race, and the concurrency is no longer hypothetical: STEP import on a worker,
OCC meshing across 8 threads grouped by TShape, async FEM jobs, the remote
server thread. The meshing work reasoned carefully about shared `TShape`s and
grouped to make the race impossible — TSan is the tool that checks that
reasoning rather than trusting it.

A third sanitizer mode is a CMake option and a `ci-local.sh` flag. TSan and
ASan cannot be combined, so it is a separate build, run occasionally like the
coverage pass.

## P4 — No GPU frame-capture workflow — FIXED

RenderDoc / Nsight appear nowhere. `DEV_WATCH_VALIDATE=1` covers the validation
half well (it diagnosed the two-extruded-parts device-lost). What it does not
answer is "what did this frame actually draw", which is the question the SSAO
sub-rect bug needed — and that one shipped, and was caught later by a
hand-written invariant test. A short recipe in `docs/rendering/` would do; the
app is a normal Vulkan target.

## P5 — Worktree scratch ports are assigned by hand — FIXED

`worktree-build.sh` says *"pass a distinct one per worktree"*. Everywhere else
this repo refuses to let a human hold a value a machine could derive — the
`free_port()` races were fixed, and every instance announces itself in
`~/.local/state/designer/instances/<port>.json`. The script could pick a free
port, print it, and print the `git worktree remove` line it currently carries
only as a comment.

Trivial, and it removes the one manual step in the parallel-work setup.

## Order

**P5** is minutes. **P1** next — it multiplies every future performance round.
**P2's `frame_stats` half** is cheap and closes a blind spot; the sub-allocator
waits for a part big enough to need it. **P3** and **P4** are occasional-audit
tools, like `coverage.sh` — worth having, not worth scheduling.

---

# What AADvark teaches us (arXiv 2604.15184v2, read 2026-08-31)

"Agent-Aided Design for Dynamic CAD Models": an agent in a feedback loop
emitting JSON parts and joints, compiled by a 3D assembly constraint solver,
with FreeCAD renderings and compiler errors fed back. Scope is **rectangular
prisms and a single revolute joint type**; the headline demo is a pair of
scissors from two images in 20 iterations, 4.14 hours, $15.85.

Read for what it tells us about our own decisions, not for its results.

## It picked OndselSolver — and hit a trap we have already avoided

The paper's solver is the same one **A1** names, which is independent
confirmation of the target. More useful is what they had to change: the
original used **Euler angles**, which let a part be oriented "anti-parallel".
FreeCAD papers over that for humans with a flip button, and *"this confused the
agent about the correctness of its design"*. They replaced it with quaternions.

Our `Mate` already stores the ambiguity rather than hiding it — an explicit
`flipped` bool, `glm::quat ballRotation`, a `glm::dquat` setup frame, and a
composition stated in the header as `Flip?·Rz(rotation)·Tz(offset)`. **Do not
regress this when A1 is built**: the flip must stay a stored, named parameter,
never a UI affordance that reorients geometry behind the document.

## The one idea worth adopting: deterministic per-instance visual identity

They modified FreeCAD to give **every face and edge of every part instance a
unique colour and texture** (solid vs dotted) plus a text identifier
("Blade1", "Blade2"), specifically so the agent could name them in joint
definitions — *"deterministic, stable identifiers which improved its ability to
reference specific edges"*.

Our dogfooding has recorded this exact failure twice: the "render_sheet
betrayal" (bend direction misjudged from a small tile, while the digest already
carried the answer) and the floating mount plate (a 35 mm gap invisible at
render distance, with confirmation bias filling it in).

We already own every piece — per-part appearance override (v54), `render_sheet`,
occurrence names, and stable element names from toponaming. An **identify
render mode** — distinct colour per occurrence/body, on-image labels — is a
small addition on top.

**With one caution.** Our answer to that failure class so far has been *numbers,
not pictures*: `contact_report`, `compare_geometry`, digests, "measure before
render". That is the better answer, and the identify mode must not soften it.
Adopt it where a picture is genuinely the medium — choosing a face to reference,
checking an arrangement — not as a substitute for a verifier.

## Their metric set is better than ours in one respect

They report iterations, LLM calls (23.4 per iteration), tokens, wall-clock and
dollars per design. `benchmarks/` reports a score (39/39 for the oracle) and a
sweep cost, but **not iterations-to-success per part**. A part that scores full
marks after 30 iterations is not the same result as one that scores full marks
after 4, and today we cannot tell them apart.

## Where it places us

Their frontier claim — *"none of these systems can build a piston, a pendulum,
or even a pair of scissors"* — is worth measuring ourselves against honestly:

- **Scissors we can already do.** Two blades and a pivot is a revolute *tree*,
  and `set_mate {rotation}` drives the DOF from the API. Our geometry,
  verifiers and API are far past their prism-and-revolute scope.
- **A piston we cannot**, and that is **A1** exactly: a slider-crank is a
  closed loop.
- **Joint limits we do not have.** Their JSON carries "freedom limits"; our
  `Mate` has no min/max on `rotation`, `offset`, `dx`, `dy`. An agent can drive
  a revolute straight through self-intersection and nothing objects. Small,
  concrete, and it pairs naturally with A1 — a solver that handles loops wants
  ranges anyway.

One behaviour they added deliberately, which we already have: *"update parts'
positions even when there was a compilation error"* — our solve keeps prior
transforms and stamps a per-mate error rather than collapsing the assembly.

## Adoption list

1. **Joint limits on `Mate`** — smallest, and it makes driven mechanisms
   checkable. Natural companion to A1.
2. **Identify render mode** — colour + label per occurrence/body, for the picks
   and arrangements where a picture is the medium.
3. **Iterations-to-success in `benchmarks/`** — one more reported number,
   comparable with published work.

---

# What was done (2026-08-31, same night)

The capability findings above (A1-A3, S1, F1) are untouched and unscheduled —
each needs a decision this pass was not the place to make. The testing and
tooling halves were mostly actionable without one, and this is what landed.

| item | state | commit |
|---|---|---|
| **T1** inner loop 48.7 s -> **10.3 s** | fixed | `ae5b624` |
| **T3** `gui_smoke` excluded by name | fixed | `ae5b624` |
| **T5** determinism property — **found a real bug** | built + fixed | `d374679` |
| **P1** sampling profiler | built | `9913136` |
| **P4** GPU frame-capture recipe | written | `9784ff6` |
| **P5** worktree picks its own port | fixed | `073aa31` |
| **P3** TSan build mode | added | (below) |
| **T4** coverage leaves no trace | run recorded, and it needs a fix | [coverage-2026-08-31.md](coverage-2026-08-31.md) |
| **T2** pixel-regression gate | NOT done — needs a decision | — |
| **P2** GPU memory visibility | done — and the finding was WRONG | (below) |
| **T6** perf gates, **T7** shuffled runs | NOT done | — |

## What T5 found, and why it is the item worth reading

The determinism test failed on its first run, on `v03-legacy-sketch.json`.

A pre-v5 rigid `"rect"` entity is expanded into four lines at load, and those
lines came from `AddEntity`, which mints **collision-resistant random** ids. A
face's stable name is `<feature>/g<entityId>` — so the same old file loaded
twice produced **different face names for identical geometry**. Same volume,
same bounding box, and every fillet, mate connector, dimension and CAM target
keyed on one of those names pointing somewhere else.

Random is right where it came from: two people ADDING entities to a shared
sketch offline must not mint the same id (C6). A migration is not that — it is
a pure function of the bytes on disk, and two people opening the same old file
must get the same answer.

Two things worth keeping from it. The property that caught it is the one the
others cannot substitute for — **two independent loads**, because both
regenerations of a single document share whatever global state exists. And the
first fix was itself wrong in a way an existing test caught: it handed a new
line the id the rect placeholder had just vacated, which would make an older
reference resolve to a real edge instead of failing. A test written years
earlier for a different reason, catching this one.

## What T4 found: the coverage report is wrong in a correctable way

`src/core` is at **75.0%** line coverage; `src/ui` 8.7% and `src/app` 6.1% by
design (they need a GPU and belong to the GUI suite). 133 `src/core` functions
across 25 files were never executed. Recorded in
[coverage-2026-08-31.md](coverage-2026-08-31.md).

The number that matters is a correction to the tool, not a gap in the code.
**gcov writes its counters when a process exits normally, every smoke test
stops the app with SIGTERM, and nothing handles it — so the app's own runs
contribute nothing.** The tell is `Checkpoints.cpp`, listed with all 8
functions "never called", while `headless_smoke.py`'s Checkpoints phase
creates, lists, restores and error-paths them and PASSED in that same run.

So the list is really "functions `designer_tests` never calls", and anything
reachable only over the RPC surface is missing from it. Left as-is with the
caveat stated rather than quietly trusted: the fix is a SIGTERM handler calling
`__gcov_dump()` in coverage builds, and it wants to be a deliberate change
rather than something bolted on at midnight.

Worth reading past the false positives: the uncovered set is dominated by
**removal and teardown paths** — `RemoveConfigurationCommand`,
`Document::RemoveCamSetup`, `AssemblyDocument::RemoveConfiguration`,
`SketchFeature::RemoveExternalGeometry`, `GitVault::RemoveRemote`. We test that
things can be created and rarely that they can be removed, and removal is where
dangling references live.

## What P3 found: 28 races, all of them Netgen's

Every one has `libnglib.so` on both sides; our frames appear only as the call
site that asked Netgen to mesh (`FemStudy.cpp:182/265/369/444/521`). **Zero
reports name our own code on both sides.** Netgen is not instrumented, so TSan
cannot see `ngcore`'s task-scheduler happens-before edges and reads a buffer
handed between its worker and the calling thread as an unsynchronized write.

`tools/tsan-suppressions.txt` covers the library, wired through
`__tsan_default_options` so a `build-tsan/designer` launched straight from a
shell behaves the same as one through a wrapper — the arrangement ASan already
uses. That suppresses a WHOLE library, which the leak list rightly refuses to
do for OpenCASCADE, and the file states why the two differ: a leak in `libTK*`
is usually our object we forgot to free, while a race in `libnglib` is inside a
thread pool we neither create nor feed. Deliberately no OCC entry — OCC *is*
called from our worker threads, a race there would be ours, and checking the
TShape grouping is the main reason this mode exists.

**Re-run with the suppressions in place: 0 warnings**, 910 s for the headless
suite (about 12x the normal 79 s). Two non-findings to expect: `femgpu_tests`
is a separate binary that needs building too, and `offscreen_smoke` fails
because the app takes longer than `smoke_app.py`'s 30 s startup budget to
report its port under instrumentation — a TSan-speed artifact, not a race.

## The two that were left, and why

**T2 (pixel-regression gate)** and **P2 (GPU memory)** both need a decision
rather than more typing.

T2's is where the baseline lives: exact byte equality is driver- and
machine-specific, so a golden PNG in git would fail on anyone else's GPU. It
needs a tolerance and a locally-recorded baseline, and choosing that shape is
the work.

P2 was left for a seam decision and then **turned out not to need one** — see
its correction above. Asking whether it beat an external monitor was the right
question: most of it did not, and the part that was unique to us was answering
a ceiling that measurement showed does not exist.

## Notes for whoever picks these up

- The `slow` label is **self-maintaining**: tag a heavy test `[slow]` where you
  write it and `test-fast.sh` skips it. Do not add names to a list in `tools/`
  again — that is precisely what drifted.
- Two things fell out of `ADD_TAGS_AS_LABELS` for free: `ctest -L cam`,
  `-L sketch`, `-L solver` all work now; and `fem_smoke`/`topo_smoke` turned
  out to be running in 1.5 s and 1.2 s while still sitting in the old
  exclusion list, which had been excluding tests that stopped being slow.
- `tools/profile.sh` overrides two perf defaults that fail *silently* here: the
  machine is hybrid, so the default hardware `cycles` event samples one PMU
  (a 27 s run gave **42 samples**; `-e cpu-clock` gives 13k), and -O2 omits
  frame pointers, so `--call-graph dwarf` is the only usable stack mode. If a
  profile ever looks thin, check those two before believing it.
- `tools/coverage.sh` now enters `nix-shell` itself. It previously assumed the
  caller had, which nothing enforced and no doc stated — run outside, it died
  at configure with "could not find glfw3".
