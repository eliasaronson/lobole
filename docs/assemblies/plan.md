# Assemblies — plan

Multiple parts, positioned and constrained relative to each other. The last
major workstream — deliberately: it multiplies every cost (regeneration,
memory, rendering, references), so its prerequisites are other plans'
milestones. This doc fixes the architecture direction now so nothing earlier
paints us into a corner.

## Prerequisites (gates — don't start A2+ without them)

> **Status 2026-07-11:** gates 1 (incremental regen), 3 (toponaming), 4
> (diffable format) are DONE. Only gate 2 (renderer instancing) remains, and
> it blocks A2, not A1. See [a1-design.md](a1-design.md).
> **2026-07-12:** instancing design drafted for sign-off —
> [../rendering/instancing-design.md](../rendering/instancing-design.md).

1. **Incremental regeneration** (roadmap "Continuous"): `Document::Regenerate()`
   recomputes every feature per edit. Per-part feature caching (output shape
   keyed by an input hash) is the entry ticket — an assembly edit must not
   re-run every feature of every part.
2. **Renderer instancing**: today `Scene`/`MeshRenderer` assume one body = one
   mesh upload. Assemblies need one tessellation per *part*, shared across N
   occurrences with per-instance transform + material — this, not allocator
   tricks, is the big memory/perf lever (10–100× on repeated parts).
3. **Toponaming (T-series)** for mates that reference faces — mates are
   persistent face/edge references *across documents*, the hardest consumers
   of [modeling/toponaming.md](../modeling/toponaming.md). A1 (multi-part) and
   explicit-transform assemblies (A2) don't need it; real mates (A3) do.
4. **Diffable format (versioning F3)** — an assembly referencing external part
   files interacts with the on-disk layout + git story; settle those first.

### On memory: arenas, virtual memory, and what actually matters

Asked and answered: the first-order wins are architectural — **instancing**
(above), **lazy part loading** (load a part's B-Rep/tessellation on first
need; drop tessellations of parts not visible), and **incremental regen**.
Arena/pool allocators address allocation churn, but most churn happens inside
OpenCASCADE, which has its own allocator layer (`NCollection` allocators /
`Standard::Allocate`, tunable via `MMGT_OPT`) — tune that if profiling ever
says so, don't build our own. Virtual-memory tricks (mmap'd geometry caches)
only matter at thousands-of-parts scale; not a v1 concern. Rule: profile a
real 100-part assembly before any allocator work.

## Milestones

### A1 — Multi-part projects — **shipped (July 2026)** — [design: a1-design.md](a1-design.md)
**Shipped:** a project holds multiple part documents (thin container); the
active part's document/commands live in AppState, inactive parts stash in a
PartSlot; add/switch/delete via bottom part tabs + RPCs/MCP. Persistence:
project.json + parts/<name>/ (single-part stays flat .dsgn). A2 (occurrences)
needs renderer instancing next.
A project holds multiple part documents; UI for switching (document tabs above
the viewport; the feature tree shows the active part). Open/save moves from
"one document" to a project container — coordinate with versioning F3
(directory layout: `parts/<name>/…`). No cross-part references yet. Much of
this is untangling singletons (`AppState` assumes one document).

### A2 — Assembly document + occurrences — [design: a2-design.md](a2-design.md)
> **2026-07-13:** instancing gate cleared (6a99c38); A2 design **signed off**
> ([a2-design.md](a2-design.md)). **Phase 1 SHIPPED** (format spine 0949554 +
> project container): assembly is a distinct document kind, parts have stable
> ids, occurrences (translation+quaternion) round-trip through a v2 project;
> add_assembly/add_occurrence RPCs + MCP + assembly_smoke test. **Phase 2
> SHIPPED** (45ae3e8): assemblies RENDER — shared asset per part body, one
> instance per occurrence; assembly camera-fit; hit→occurrence picking.
> **Phase 3 SHIPPED** (4860eea + ae5c808): editing UX — occurrence panel
> (list + numeric transform/rename/visibility/fixed/delete), click-to-select
> with highlight, occurrence-edit snapshot undo. **A2 is a usable v1 assembly.**
> Deferred to a follow-up: the 3D drag gizmo (numeric-first was chosen).

A new document type: an ordered list of **occurrences** `{ partRef, transform,
name, visibility }` — instances of parts placed by explicit transform.
Move/rotate gizmo for positioning. Rendering via the instancing prerequisite.
Picking maps hit → occurrence (then into the part on double-click). This alone
is a usable v1 assembly (many real workflows are "place parts, eyeball it").

### A2.5 — Interactive manipulation (3D drag gizmo) — **planned, land with A3's DOF-drag**
The A2 editing UX shipped **numeric-first** (Elias's call): occurrences are
positioned via panel fields, not a 3D drag manipulator. The drag gizmo is
deliberately deferred here rather than built as a standalone A2 epilogue,
because it **shares infrastructure with A3's payoff feature** (drag-within-mate):
both need a pick-handle → mouse-delta → live-transform drag layer, plus a real
**OutlineRenderer per-mesh model** (replacing the Phase-2 vertex-bake stopgap)
so a live drag updates cheaply instead of a full resync. Building them together
writes that layer once.

Scope when it lands: translate + rotate handles (screen-space sizing, axis/
plane drags) for a free occurrence, and the same drag layer constrained to a
mate's remaining DOF for drag-within-mate. Also folds in the deferred
FaceSelector/OutlineRenderer CPU-geometry-sharing split.

**Trigger:** do it as the interactive-manipulation push *after* A3's mate/
placement core lands (mates position most occurrences by constraint, so free
drag matters mainly for initial/unmated placement — mates are the higher-value
next step). The one piece worth pulling forward on its own if convenient is the
OutlineRenderer per-mesh model (self-contained, needed by both). See
[a2-design.md](a2-design.md) phase 3.

### A3 — Mates — [design: a3-design.md](a3-design.md)
> **2026-07-13:** design **signed off** ([a3-design.md](a3-design.md)) —
> inline connector frames on planar/cylindrical faces + part origin (via the
> T-series `ElementRef`s), fastened/revolute/slider with stored DOF pose,
> sequential direct-frame-alignment solve (loops reported, not solved),
> format v24 (`mates/` file-per-mate).

**Onshape's mate-connector model, not SolidWorks' constraint soup**: a mate
connects two **connector frames** (coordinate frames placed on parts — on
faces via toponaming refs, or on datums), with one mate type per connection
(fastened, revolute, slider, cylindrical, planar, ball). Rationale: most
assemblies solve by **direct frame alignment** — no global numeric solver —
because each mate fully or partially fixes one occurrence relative to another;
a sequential placement pass handles trees of mates, and only genuine loops
(rare in practice) need an iterative solve, which v1 can simply report as
over-constrained. This dodges writing/embedding a 3D constraint solver (PlaneGCS
is 2D-only) for the 95% case. Degrees-of-freedom display + drag-within-mate
(spin a revolute) is the payoff feature.

### A4 — Assembly features & beyond
In-context editing (edit a part with the assembly ghosted around it),
assembly-level features (hole through multiple parts), interference check
([inspect I4](../inspect/plan.md)), bill of materials, exploded views,
configurations. All later; listed so A1–A3 decisions don't preclude them.

## Cross-cutting

- **External references + versions**: an assembly pins which version of a part
  it references (git commit, once versioning F5 lands) with an update-to-latest
  action — otherwise git branching of parts silently changes assemblies.
- **Scripting parity**: occurrences and mates are commands like everything
  else (`add_occurrence`, `add_mate`) — the LLM builds assemblies the same way.

## Recommended order

A1 ✓ → A2 ✓ (usable v1) → **A3 fastened/revolute/slider → A2.5 interactive
drag (gizmo + drag-within-mate, shared layer) → the rest of A3 → A4.** A2.5
lands with A3's DOF-drag rather than before it, because they share the
interactive-drag + OutlineRenderer per-mesh-model infrastructure (build once).
Do not begin A1 until modeling M1–M5 + variables are done and the incremental-
regen cache exists; begin the mate-connector design sketch alongside toponaming
T4/T5 since they share the reference model.
