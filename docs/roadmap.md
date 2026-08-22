# Designer Roadmap

> Top-level vision + workstream index. Detail lives in the area plans (linked
> below); this doc holds the why, the current state, and the sequencing.

## Vision

An open-source parametric CAD application in the spirit of the commercial parametric
modellers, with five
differentiating goals:

1. **Transformation checkpoints** — named points in the model's regeneration history that
   can be inspected, rolled back to, and branched from.
2. **Variables and expressions** — dimensions defined as expressions over global parameters
   (`length = width * 2`).
3. **Scripting** — a Python REPL / API that can do everything the GUI can.
4. **Git-based versioning** — models stored in a diffable, mergeable text format.
5. **LLM-friendly by design** — AI assistants use the *same interface as humans*: the same
   command layer, the same document format, the same feedback (queries, screenshots). This
   helps both in developing the application and in future AI-assisted modeling.

### The one architectural decision behind all of it

Goals 1–5 are all the same decision: a **headless core with a serializable command/document
layer**, where the GUI is just one client.

- Every UI action becomes a *command* that edits a *document* (a feature list), never the
  geometry directly.
- The document regenerates the B-Rep deterministically from scratch.
- The document serializes to human-readable text.

From this fall out: undo (command replay/inversion), checkpoints (points in the feature
history), variables (an expression pass during regeneration), scripting (the command layer
*is* the API), git (text format), LLM interface (same command layer + headless mode), and
testability (geometry ops unit-testable without a GUI).

## Current state (2026-08-20)

The app is a working parametric CAD with assemblies, simulation and drawings;
**all sixteen workstreams are v1-complete** (see the table below), and the
work since mid-July has been breadth-then-hardening rather than new pillars.
What that means concretely:

**Modeling & core:** the full modeling loop (sketch → extrude → sketch-on-a-face → cut)
plus M1–M8 — datum planes/axes, revolve, loft, sweep, booleans, mirror/linear/circular
patterns, **toponaming** (persistent face/edge references), fillet/chamfer, shell, hole
wizard, draft, split and refine — all of them creatable from the GUI as well as
the script API since UI U9 (2026-08-13); several were script-only before that.
The 2D sketcher is feature-complete. Parametric core: `Document`
feature list with deterministic regeneration + a per-feature output-shape cache
(incremental regen), stable ids, undo/redo, PlaneGCS solving. **Variables & expressions**
everywhere, including unit-suffixed literals. Serialization is at **format v57** (a
diffable directory format, golden-corpus migrations, with built geometry baked
beside the recipe so opening a project is a load, not a rebuild).

**Differentiators:** **checkpoints + rollback**, **git-backed history** (F4/F5)
and the **History panel** that makes both usable without a terminal (F6);
the **scripting stack** — JSON-RPC remote, an **MCP server**, **headless mode**, a Python
client, and record/replay session tests; **CI** runs `ctest -LE gui` on every push.

**Beyond the core:** **assemblies** A1–A5 (occurrences + instancing, all six mate types,
drag gizmo + drag-within-mate, part→assembly propagation, BOM, in-context edit, exploded
views, sub-assemblies); **inspection** (measure, mass properties, section views,
interference, contact reports);
**interchange** (STEP/STL/BREP + glTF/OBJ/PLY export, STEP import) via menu/RPC/MCP;
**FEM** single-part linear static plus modal and self-weight (Netgen meshing,
in-house Eigen/Spectra solvers, colormapped results) and **topology
optimization** on the same mesh; **drawings** (HLR views of parts/assemblies, driven dimensions, SVG + PDF).
**Rendering:** MSAA + ACES + IBL/PBR, per-instance transparency, PCF shadows +
SSAO (persisted as user preferences), an offline path-traced Render button.

**Thin or missing:** the UI/UX visual-polish tail (spacing rhythm, tree badges,
a tooltips audit); assembly-level features; per-occurrence part-config
selection (the one configurations follow-on); real-time hybrid RT (gated on
the `main.cpp` render-path cleanup); assembly/thermal/buckling FEM.

**Where the defects actually come from (evidence, 2026-08-19/20).**
`known-bugs.md` is empty and the suite is ~800 tests, yet a single evening of
a person *using* the app surfaced six real defects — the section arrow dead on
assembly tabs, the section drag rebuilding every mesh per frame, point picking
highlighting an adjacent line, the Intel camera wobble (per-frame resources
sized to 2 swapchain images, not 3), a font-size preference that did nothing,
and a GPU preference that could brick startup. **None were found by tests**;
all were in the interactive path, which is the half the RPC-driven smokes
cannot reach. Two structural lessons were folded back in: the section arrow
had a part-tab test and no assembly-tab test (asymmetric coverage hides
whole-tab-kind failures), and "it feels laggy" cost real time twice — once a
genuine bug, once a suspended laptop — which is why frame timing is now
measurable ([rendering/frame-timing.md](rendering/frame-timing.md)).
**The three large logic files were split (July 2026):** `RemoteServer` 2164→663 (seven
`remote::Handle*Rpc` domain modules), `Serialization` 2268→1135 (`Serialize{Features,
Assembly,Studies,Configurations}` TUs + `SerializationInternal.h`), `AppActions`
3191→2056 (`AssemblyActions` + `SimulationActions`). `main.cpp` + the renderers stay
monolithic until 6b forces it (see the refactoring policy below).

## Workstreams

Each workstream has its own plan doc and ships in independent milestones.

| Workstream | Plan | Status |
|---|---|---|
| 1. Sketching | [sketching/plan.md](sketching/plan.md) | **Complete** (B-splines deferred) |
| 2. 3D modeling | [modeling/plan.md](modeling/plan.md) | **Complete** (M1–M8 + rib + cosmetic thread, format v36 — see [modeling/thread-rib.md](modeling/thread-rib.md); modeled helical thread = the design’s stretch flag) |
| 3. Topological naming | [modeling/toponaming.md](modeling/toponaming.md) | **Complete** (T1–T5; consumers: sketch-on-face, fillet/chamfer, up-to-face extrude) |
| 4. Variables & expressions | [variables/plan.md](variables/plan.md) | **Complete** (V1–V3 + C2 unit-suffixed literals, format v28; dimensional analysis + unit display deferred) |
| 5. Scripting & LLM interface | [scripting/plan.md](scripting/plan.md) | **Complete** (S1 MCP, S2 headless+CI, S3 python client, S4 record/replay) |
| 6. Versioning: format, autosave, checkpoints, git | [versioning/plan.md](versioning/plan.md) | **Complete v1** (F1 spec+corpus, F2 autosave, F3 directory format, F4 checkpoints+rollback, F5 git engine, **F6 the history UI** — [versioning/f6-history-ui.md](versioning/f6-history-ui.md): a History panel of named versions + restore, checkpoints promoted from part-only JSON copies to project-wide named commits); **merge/conflict handling shipped 2026-08-21** — [versioning/f7-merge-and-conflicts.md](versioning/f7-merge-and-conflicts.md): F7 A+B+C: local branches + merge in the History panel, a Conflicts panel that names a conflict as "Extrude \"Boss\" in part Stand — distance 12 vs 18", a three-way union merge for index order (a pick there loses the other side's features), a renumber for the duplicate ids git merges cleanly and never reports, and an integrity pass that refuses to conclude a merge the project cannot open. Remotes deliberately out. Its scoping surfaced **F8**, also shipped 2026-08-21 ([versioning/f8-external-changes.md](versioning/f8-external-changes.md)): nothing compared memory with disk, so a `git` command in the app's own terminal panel was overwritten by the next Ctrl+S. A `{fileCount, maxMtime, totalSize}` stamp of the project directory — skipping `.git/` (churns on its own) and `geometry/` (the build, not the recipe) — is recorded wherever the app and disk are known to agree and checked at the one place every save funnels through; a moved project refuses, offering Reload or Save-anyway, and the History panel says so before you get there. **F9** ([versioning/f9-collaboration.md](versioning/f9-collaboration.md), **shipped 2026-08-21**) adds what a small team who already use git needed on top: remotes/clone/push/pull with fetch+push on a worker thread (merge stays on the main thread — it moves the working tree under an open document), commits authored by the person who made them, duplicate ids attributed by merge side, built geometry that stops travelling once shared, and named versions that survive two people choosing one name. Auth is deliberately git's job. Proven without a network by `tests/two_collaborators.py`. **C6 (2026-08-22)** closed the last silent class: a face's stable name is `<feature>/g<entityId>`, and sketch entity ids came from a per-sketch counter, so two people editing one sketch offline minted the same id and made that name ambiguous — ids are collision-resistant now and a duplicate is refused at load. Handoff: [versioning/f7-handoff.md](versioning/f7-handoff.md). **Planned next:** [F10](versioning/f10-autosave-the-project.md) — autosave still snapshots only the ACTIVE TAB (and nothing at all on an assembly tab), the same class F6 fixed for checkpoints, so a crash recovery on a multi-part project is more destructive than the crash; and [F11](versioning/f11-seeing-what-changed.md) — a history that says what each version changed, then a diff view |
| 7. UI & UX | [ui/plan.md](ui/plan.md) | **U1–U9 shipped** — unified selection, dock-home enforcement, feature-dialog slots, sketch-region picking, command palette, one floating Tool window for every tool session (right Properties panel deleted), and U9 GUI parity ([ui/u9-gui-parity.md](ui/u9-gui-parity.md)): hole/shell/draft/split/refine reachable by click. **2026-08-20: user preferences** ([ui/settings.md](ui/settings.md)) — Edit > Preferences over a persisted settings file (GPU device, font size, render quality, autosave), which took render quality and font size out of the View menu where they reset every launch. Also 2026-08-20: **one hover for every viewport drag handle** ([ui/viewport-handles.md](ui/viewport-handles.md)) — one `ViewportHandle` list shared by the preview-handle, section, gizmo and simulation-load owners, which immediately exposed the section arrow being dead on assembly tabs; a project saved from the GUI is always a `.dsgn` directory (so git history survives a Save As); and point picking no longer highlights an adjacent line. **2026-08-20: a terminal panel** ([ui/terminal-panel.md](ui/terminal-panel.md)) — libvterm + a PTY behind a dockable right-hand panel, off by default, so Claude Code (or a shell, or git) runs beside the model and is bound to THIS instance's remote port with no wiring. **2026-08-21:** the terminal gained drag-select, double-click word select and Ctrl+Shift+C copy-out. Remain: visual-polish tail (spacing rhythm, tree badges, tooltips audit) |
| 8. Inspection: measure, mass properties, sections | [inspect/plan.md](inspect/plan.md) | **v1 complete** — I1 measure (RPC + GUI tool), I2 mass properties (panel + CoG marker), I3 section (raster clip + filled caps), I4 interference. 2026-08-20: contact checks run the boolean with a 1 µm fuzz, so parts *designed* to touch report contact instead of a sliver-sized "clash" (`scratch/floating-dock.json`'s six sub-0.14 mm³ lap-joint overlaps were this, not bad geometry). **2026-08-21: the section tool completed (P2+P3)** — datum/face sources resolved live, tilt angles, and cuts that persist with the project instead of vanishing on save, plus `add/list/update/remove_section` ([inspect/section-tool-design.md](inspect/section-tool-design.md)). Deferred polish: hatching, angle/point measure, face-sourced cuts on an assembly tab |
| 9. Rendering | [rendering/plan.md](rendering/plan.md) | **Raster polish shipped** (showcase arc R1–R4 + instancing + ghosts; 2026-07-19: key-light PCF shadows + SSAO, [rendering/ssao-soft-shadows.md](rendering/ssao-soft-shadows.md) — S3 quality settings now PERSIST as user preferences ([ui/settings.md](ui/settings.md)); visual tuning remains). 2026-08-20: per-frame GPU resources are sized to the actual swapchain image count ([rendering/frames-in-flight.md](rendering/frames-in-flight.md)) — the Intel camera wobble was 2-vs-3 aliasing — and the loop is now measurable: `--perf-hud` / Ctrl+Shift+F / the `frame_stats` RPC report cpu-vs-display and an avg/p50/p95/max split, off by default ([rendering/frame-timing.md](rendering/frame-timing.md)). Real-time hybrid RT (6b) still gated on the `main.cpp` render-path cleanup |
| 10. Assemblies | [assemblies/plan.md](assemblies/plan.md) | **A1–A5 v1 shipped** — multi-part, occurrences (instanced), all six mate types (connector frames, direct-frame-alignment solve, mate tool + list), interactive manipulation (drag gizmo, drag-within-mate, hole-center snap), part→assembly propagation, BOM, in-context editing (ghosted assembly), exploded views (v27), and **sub-assemblies** (A5, format v55: an occurrence references a part *or* an assembly, with `assembly::Flatten` as the one tree walk). Remain: assembly-level features + configurations (own designs), cross-part refs in-context, snap-marker UX polish |
| 11. Interchange: export/import | [interchange/plan.md](interchange/plan.md) | **v1 complete** — X1 STEP/STL/BREP + X2 glTF/OBJ/PLY (parts + assemblies, PBR materials, instanced), all via menu/RPC/MCP. X3 (IGES/mesh-import/DXF) + 3MF demand-driven |
| 12. Simulation: FEM | [simulation/plan.md](simulation/plan.md) | **v1 + v2 shipped** — v1: single-part linear static (Netgen tet10, in-house Eigen solver, face BCs via toponaming, von Mises + displacement + factor of safety, Simulation-mode GUI, MCP, `fem_smoke`). v2: **modal analysis** (Spectra generalized eigensolver, consistent mass matrix, natural-frequency list + mode-shape overlay; cantilever f₁ 0.18%) and **gravity/self-weight** (0.03% vs beam theory), format v32. **Setup round U11 (2026-08-17)**: gravity pulled along -Z while world up is +Y, a force took a world literal instead of the face it was placed on, the default magnitude was 0 N, and the first Run of every new part failed on the unassigned material — all fixed, verified against beam theory ([ui/u11-simulation-setup.md](ui/u11-simulation-setup.md)). 2026-08-20: force arrows are **draggable** (relative magnitude, one undo entry per drag) and a test asserts the drawn arrow runs along `load.direction`. Remain: assembly/contact, thermal, buckling, nonlinear, VTU export (deferred by design; v2 modal requires a fixed support — free-free later) |
| 13. Drawings: 2D drafting | [drawings/plan.md](drawings/plan.md) + [guide.md](drawings/guide.md) | **v1 complete (D0–D4)** — drawing doc kind (format v30), HLR-projected views of parts + assemblies with projected-child folding, six driven dimensions via toponaming refs, title block, SVG + PDF export; GUI picking + headless/MCP. Deferred to own designs: section/detail views, on-sheet BOM/balloons, exploded drawing views, multi-sheet, GD&T, DXF (→ interchange) |
| 14. Configurations | [configurations/plan.md](configurations/plan.md) | **v1 complete (C0–C4; format v31)** — parametric families: part configs (variable overrides as expressions + feature suppression, applied by regenerating against an override symbol table) and assembly configs (occurrence suppression); persisted active config; RPC/MCP (`remote/RemoteConfigurations.cpp`) + GUI panels. Follow-on: per-occurrence part-config selection |
| 15. Topology optimization | [topopt/plan.md](topopt/plan.md) + [guide.md](topopt/guide.md) | **v1 complete + GPU0–GPU4** — SIMP density optimization on the tet10 mesh, multigrid V-cycle by default, GPU energies and a warm-solve cap (0.55 s/iter at 13.6k tets, ~6x the CPU path); results persist (format v33) with a re-modeling ghost overlay. GPU design notes in `gpu-plan.md`, `gpu3-plan.md`, `gpu4-plan.md` |
| 16. Packaging & distribution | [packaging/plan.md](packaging/plan.md) | **Flatpak shipped** (built and GPU-tested on a real NVIDIA card, `packaging/flatpak/` + `tools/flatpak-build.sh`); `nix run` + Cachix for developers; nix-bundle AppImage is HEADLESS-ONLY (the GUI build was tested and rejected — glibc vs GPU driver). deb/rpm/Snap/macOS/Windows deferred; the Windows port itself is a proven spike (`packaging/windows/`) |

Workstreams 1–2 make it a CAD program; 3 keeps it alive as one; 4–6 are the
differentiators — the reason this project exists rather than being a worse
version of a commercial modeller. 7–9 make it pleasant; 10 makes it complete.
11–14 are the breadth a real CAD tool is expected to have; 15–16 arrived
after those and are listed last for that reason, not because they matter
less — 16 is what lets anyone else run this at all.

## Sequencing

> **Executed** (this was the plan as of early 2026; kept as the rationale
> record). Everything in it has shipped — variables, modeling M1–M8,
> toponaming, scripting, versioning, inspection, interchange, rendering's
> showcase arc, assemblies A1–A5, FEM, drawings, configurations — in roughly
> this order. Sequencing is no longer the open question; see **What's next**
> below, which is driven by use rather than by a pre-planned order.

The recommended interleaving, with the rationale:

1. **Variables & expressions first (V1–V2)** — small now that the document model exists,
   and every new modeling feature (revolve angle, datum offset, pattern count) should be
   expression-driven from day one rather than retrofitted.
2. **Modeling M1 datum planes → M2 revolve** — highest-value solid features, both
   toponaming-free.
3. **Toponaming design study (T1) in parallel** — it shapes the reference model every
   later feature uses; the study costs reading + a memo, not code.
4. **Modeling M4 booleans → M5 3D mirror/pattern → M3 richer extrude**, with the
   **regeneration-failure UX** (modeling cross-cutting) landing before the first
   failure-prone feature (fillets, up-to-face).
5. **UI passes interleaved throughout** — U-milestones are small and independently
   shippable; U2 (sketch-region selection for extrude) and U3 (feature-dialog pattern)
   directly serve the modeling work.
6. **Scripting completion (MCP, Python client, headless)** after variables — the API
   should speak expressions. Headless mode also unlocks geometry tests in CI.
7. **Versioning F1–F2 (format spec + autosave)** early — cheap insurance once real
   models exist. Checkpoints + git (F3–F5) after the schema settles.
8. **Inspection I2 (mass properties)** almost any time — OCC gives it nearly for free
   and it's very LLM-useful. I1 measure wants edge picking (modeling cross-cutting).
   Same goes for **interchange X1 (STEP/STL export)**: the kernel writers already
   exist, it's ~a day including RPCs, and it's the first real-world need (3D
   printing, sending a part to another tool).
9. **Rendering** demo-driven: ~~normals + AA~~ the showcase arc (materials/MSAA/ACES/
   IBL/offline render) shipped; SSAO/soft shadows/hybrid RT when it hurts or when a
   release needs to look good.
10. **Assemblies last** — gated on incremental regeneration, renderer instancing, and
    (for face mates) toponaming. See the plan's prerequisites section. A1+A2 are
    shipped (usable v1: multi-part, occurrences, numeric editing). Next is **A3
    mates**; the deferred **3D drag gizmo (A2.5)** is planned to land *with* A3's
    drag-within-mate, since they share the interactive-drag + per-mesh-model
    infrastructure — build it once, not before A3.

## What's next (2026-08-20)

There is no unshipped pillar, so priority comes from evidence rather than
from the sequencing list above:

1. **Keep dogfooding through the GUI, on real objects.** Every defect found in
   the last two rounds came from a person or an agent *building something*,
   not from the suite — and the GUI-driven half found the ones the RPC-driven
   half structurally cannot. [dogfooding.md](dogfooding.md) records each
   round; the practice is the deliverable, not any single round.
2. **Close the coverage asymmetries it keeps exposing.** A feature tested on a
   part tab and not an assembly tab is the recurring shape (the section arrow,
   twice). Worth a deliberate pass: for each interactive tool, does a test
   exercise it on every tab kind it claims to support?
3. **The visual-polish tail** (workstream 7) — spacing rhythm, tree badges,
   tooltips audit. Small, independently shippable, and the thing a new user
   sees first.
4. **Then the deferred follow-ons**, in demand order: per-occurrence
   part-config selection, assembly-level features, drawing section/detail
   views, FEM assembly contact, deb/rpm packaging.

The only *designed but unbuilt* work left is **git branching and conflict
resolution** ([versioning/plan.md](versioning/plan.md)): `GitVault` is
deliberately a linear local history — no branch, merge or remote — so a
conflict UI has nothing to sit on yet. Branch + switch is worth having on its
own (trying a design direction); merge and the pick-mine/theirs UI can wait
until there is something to merge. Everything else on the lists above is a
deliberate deferral with a reason, not an unfinished project.

Deliberately *not* raised in priority: hybrid RT (still gated on the
`main.cpp` cleanup, no forcing function) and semantic merge (the directory
format already merges cleanly line-by-line).

## Continuous

### Refactoring policy: split what the next feature extends, not big-bang

The large files fall into two kinds, and the "add tests before you refactor" rule
points opposite ways for each:

- ~~**Testable-logic files — refactor freely, guarded by the existing smokes.**~~
  **Done (July 2026), as the policy prescribed — at the moment before extending
  them, not as a standalone pass.** `RemoteServer` 2164→663 (seven
  `remote::Handle*Rpc` domain modules), `Serialization` 2268→1135
  (`Serialize{Features,Assembly,Studies,Configurations}` + an internal header),
  `AppActions` 3191→2056 (`AssemblyActions` + `SimulationActions`). The rule
  the split leaves behind: **edit the domain module, not the monolith** — a new
  RPC goes in `Remote<Domain>.cpp`, a new persisted field in the matching
  `Serialize*.cpp`. Tests were backfilled opportunistically per extracted
  chunk rather than in a separate sprint.
- **Untestable files — leave them until a feature forces it.** `main.cpp` (~1330 LOC
  Vulkan) and the renderers (~4000) can't be unit-tested (need a GPU), so "test first"
  isn't a real gate and a refactor is pure regression risk. There is **no forcing
  function now**: the `main.cpp` dismantle only matters for real-time hybrid RT (6b),
  which isn't scheduled. Do it *when 6b lands*, not before.

### Standing items

- **Dead code purge** — `Application`, `SimpleApp`, `Viewport`, `ColorPicker`,
  `FaceHighlighter`, the duplicated View menu. *Keep* `RayTracingRenderer` (rendering 6a/6b
  seed).
- **Dismantle `main.cpp`** and the tripled Vulkan boilerplate (extract a `VulkanContext` +
  shared renderer utilities) — opportunistically while touching code, not big-bang; gated
  on 6b hybrid RT (no forcing function until then — see the policy above).
- ~~**CI**~~ — **done** (scripting S2): `.github/workflows/ci.yml` builds + runs
  `ctest -LE gui` on every push.
- **Build hygiene** — `-isystem` for OCC/ImGui headers, a PCH for the heavy common set,
  Ninja, ccache (see sketching plan cross-cutting).
- ~~**Incremental regeneration**~~ — **done**: `Document` keeps a per-feature
  output-shape cache keyed by an input hash (`RegenCacheEntry` + `RegenStats`); it was
  the assemblies gate-1 and cleared before A1.
- **Scale** — first measured 2026-08-14, [performance/scale-round.md](performance/scale-round.md):
  a 148-feature part and a 50-occurrence assembly, with the benchmarks kept in
  `scratch/scale_*.py`. Fixed there: hole cutters fused one at a time (480 holes
  324 s → 7.9 s), interference without a bbox reject (7.5 s → 282 ms), drawing export
  re-projecting the sheet (6.1 s → 19 ms). ~~Opening a project rebuilds it from
  scratch~~ — **done** 2026-08-14: one regeneration per load instead of one per
  feature, plus baked geometry beside the recipe
  ([versioning/baked-geometry.md](versioning/baked-geometry.md)); **259 s → 0.04 s**.
  Still slow at size: an edit early in a long history is a full rebuild by definition
  (~28 s for that part), and check_geometry / list_elements are ~3 s.
- ~~**CI: GUI tests under a virtual display**~~ — **done** 2026-08-14: the gui-labeled
  ctest entries run through `tools/xvfb-exec.sh`.
- ~~**GUI suite too slow to run per commit**~~ — **done** 2026-08-20: the suite
  is CPU-bound in one process, so `tools/gui-tests-xvfb.sh` partitions it into
  four sharded processes by recorded per-test time (335 s → ~70 s; `--fast`
  drops the handful of multi-second cases). The partition comes from the
  suite's own `--gui-test-list`, so a new test cannot be silently dropped.
- **Performance measurement is now first-class**: `frame_stats` / the hidden
  frame-time HUD ([rendering/frame-timing.md](rendering/frame-timing.md)) for
  the interactive loop, `scratch/scale_*.py` for model size. Reach for a
  number before a refactor — one "regression" this month was a laptop lid
  closed mid-benchmark.

### Test coverage (state 2026-08-20)

**660 tests pass under `ctest -LE gui` in ~90 s** (636 C++ Catch2 cases + 24
headless Python integration smokes) and **161 ImGui-test-engine GUI cases in
~80 s** via `tools/gui-tests-xvfb.sh`; CI runs the headless subset every push. **Strong:** the
headless core — geometry/features, format+golden-corpus migrations, expression engine,
and the full RPC surface via smokes (every feature since assemblies shipped with a test).
**By-construction untested:** renderers + `main.cpp` (need a GPU — verified via gui_smoke
+ dev-watch screenshots) and much of the UI interaction layer (partial via gui_smoke +
`viewport_drag` smokes). **The real exposure is the GPU/GUI gap** — bugs like the A2.5
device-lost and the C4 back-face-culling surfaced on-screen, not in tests, and
the 2026-08-19/20 round confirmed it with six more. No coverage
gate in CI; new-feature coverage is by author discipline, not enforced threshold.
One trap worth knowing when adding GUI cases: a test whose category is exactly
`perf` lands in the test engine's Perfs group, which this suite never queues —
it will list and never run.

## Explicitly not planned (conscious decisions, not omissions)

- ~~**2D drawings / drafting output**~~ — was "deliberately out of scope until after
  assemblies; revisit then". Assemblies shipped, revisited July 2026: now workstream 13,
  see [drawings/plan.md](drawings/plan.md).
- ~~**CAM**~~ — **reversed and BUILT 2026-08-21** (C1–C5, then G1–G4): 2.5D milling
  — profile with tabs, pocket, face, drill — on Clipper2, posting to Mach3 and
  LinuxCNC, with a backplot, setups that persist with the part (format v58),
  and a dependency-free verifier that simulates the POSTED file and reports
  gouge/leftover. Reached by three verbs on a tool session rather than by a
  parameter editor: two clicks make a pocket, every depth is derived from the
  face you clicked, the removed volume is ghosted before you commit, and
  Generate verifies without being asked ([cam/gui.md](cam/gui.md)). See
  [cam/plan.md](cam/plan.md). **Phase 2 (C6–C9) also shipped**: helix entry
  for small pockets, arcs fitted at post time, Kresimir Tusek's adaptive
  clearing (LGPL, deliberately linked as a separate shared library so this
  app's own licence stays a choice), and an indexed 4th axis. Only 3-axis
  surfacing and continuous 4-axis remain, both unscheduled. **Nothing has cut
  a real part yet** — that is the check simulation cannot stand in for. The proposal that preceded it, kept because the
  survey numbers are the reason the scope is narrow:
  [cam/plan.md](cam/plan.md) costs it out (2026-08-21, after the user bought a
  mill), having surveyed the alternatives by reading them — FreeCAD CAM is
  ~25k lines of ops over 14k lines of libarea, Kiri:Moto's whole CAM mode is
  ~12k lines with individual ops at 47–135. The prerequisites that made FEA
  reversible are present (toponaming makes machining selections survive
  edits; `Measure::SectionCaps` is already the Z-slicer; `compare_geometry` +
  CAMotics' `camsim` close a headless verification loop), and Clipper2 and
  Tusek's adaptive clearing are licence-compatible with this AGPL app. It
  stays **unscheduled** until the mill has been used enough to know what it
  needs. (Simulation/FEA was listed here until July 2026;
  reversed once its real prerequisites — toponaming, materials, headless
  testing — had shipped. See [simulation/plan.md](simulation/plan.md).)
- **Sketch text** — niche, out of scope (see sketching plan).
