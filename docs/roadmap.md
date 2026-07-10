# Designer Roadmap

> Top-level vision + workstream index. Detail lives in the area plans (linked
> below); this doc holds the why, the current state, and the sequencing.

## Vision

An open-source parametric CAD application in the spirit of Onshape/SolidWorks, with five
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

## Current state (July 2026)

**Working:** the full Onshape loop — sketch → extrude → sketch-on-a-face → cut. The 2D
sketcher is feature-complete (lines/rects/arcs/ellipses; the full constraint set +
dimensions + construction geometry + conflict management; mirror/fillet/pattern/offset;
select-default/Escape/box-select + Onshape keymap). Parametric core: `Document` feature
list with deterministic regeneration, stable feature + sketch-entity ids, undo/redo,
JSON project serialization (v12). PlaneGCS constraint solving with the FreeCAD drag
recipe. **Variables & expressions** (V1–V3): a document parameter table with a swappable
expression engine, expression-driven dimensions/extrude/datum params, a Variables panel,
and `set/rename/delete/list_variables` RPCs. **Datum planes** (M1): offset/angled/
through-points reference geometry, with sketches that track a datum by id so upstream
edits move dependents. **Revolve** (M2): profile revolved about an axis (global / sketch
line / datum axis) through an expression-driven angle, with a symmetric option; plus
standalone datum axes. `--remote <port>` JSON-RPC: semantic commands + human-parity UI
actions + screenshots (`tools/designer_remote.py`). STEP import, face picking with real
B-Rep face mapping, docking dark-theme workbench with icon toolbar and preview→accept.
Rendering: MSAA + ACES tonemap + IBL/PBR from a studio HDR.

**Thin or missing:** solid modeling beyond extrude/revolve + datum geometry (no fillets,
booleans-as-features, 3D patterns); persistent face/edge references (**toponaming**) —
face/edge sketches still bake at creation, upstream edits don't move those dependents;
checkpoints + git; MCP wrapper, Python API, headless mode; measurement + mass properties;
no assemblies; `Document::Regenerate()` recomputes every feature per edit; no CI.

## Workstreams

Each workstream has its own plan doc and ships in independent milestones.

| Workstream | Plan | Status |
|---|---|---|
| 1. Sketching | [sketching/plan.md](sketching/plan.md) | **Complete** (B-splines deferred) |
| 2. 3D modeling | [modeling/plan.md](modeling/plan.md) | **In progress** (M1 datum planes + M2 revolve/datum axes shipped; M3–M8 next) |
| 3. Topological naming | [modeling/toponaming.md](modeling/toponaming.md) | Design study — start now, in parallel |
| 4. Variables & expressions | [variables/plan.md](variables/plan.md) | **Shipped** (V1–V3; units algebra deferred) |
| 5. Scripting & LLM interface | [scripting/plan.md](scripting/plan.md) | Remote shipped; MCP/Python/headless remain |
| 6. Versioning: format, autosave, checkpoints, git | [versioning/plan.md](versioning/plan.md) | Format v9 exists; the rest unstarted |
| 7. UI & UX | [ui/plan.md](ui/plan.md) | Workbench shipped; selection UX + layout + polish remain |
| 8. Inspection: measure, mass properties, sections | [inspect/plan.md](inspect/plan.md) | Unstarted |
| 9. Rendering | [rendering/plan.md](rendering/plan.md) | Planned (normals → AA → PBR → … → RT) |
| 10. Assemblies | [assemblies/plan.md](assemblies/plan.md) | Planned; gated (see plan) |

Workstreams 1–2 make it a CAD program; 3 keeps it alive as one; 4–6 are the
differentiators — the reason this project exists rather than being a worse
SolidWorks. 7–9 make it pleasant; 10 makes it complete.

## Sequencing

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
9. **Rendering** demo-driven: normals + AA are overdue quality-of-life; the rest when
   it hurts or when a release needs to look good.
10. **Assemblies last** — gated on incremental regeneration, renderer instancing, and
    (for face mates) toponaming. See the plan's prerequisites section.

## Continuous

- **Dead code purge** — `Application`, `SimpleApp`, `Viewport`, `ColorPicker`,
  `FaceHighlighter`, the duplicated View menu. *Keep* `RayTracingRenderer` (rendering 6a/6b
  seed).
- **Dismantle `main.cpp`** and the tripled Vulkan boilerplate (extract a `VulkanContext` +
  shared renderer utilities) — opportunistically while touching code, not big-bang.
- **CI** — build + ctest on every push; the test suites exist, they should run
  automatically. GUI tests need a virtual display; headless mode (scripting S2) makes the
  geometry tests trivially CI-able first.
- **Build hygiene** — `-isystem` for OCC/ImGui headers, a PCH for the heavy common set,
  Ninja, ccache (see sketching plan cross-cutting).
- **Incremental regeneration** — per-feature output-shape cache keyed by an input hash.
  The scaling limiter, and the entry ticket to assemblies.

## Explicitly not planned (conscious decisions, not omissions)

- **2D drawings / drafting output** — the classic fifth pillar of CAD (part / assembly /
  drawing / render / measure). Deliberately out of scope until after assemblies; revisit
  then.
- **CAM, simulation/FEA** — out of scope.
- **Sketch text** — niche, out of scope (see sketching plan).
