# Assemblies — plan

> **Status: A1–A5 shipped** — multi-part projects, instanced occurrences, all
> six mate types, the drag gizmo, propagation, BOM, in-context editing,
> exploded views, and sub-assemblies
> ([a5-subassemblies-plan.md](a5-subassemblies-plan.md)). The prose below is
> the original architecture argument, which is why it still reads as
> forward-looking.

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

### A2.5 — Interactive manipulation (3D drag gizmo) — **SHIPPED (July 2026)** — [design: a2_5-design.md](a2_5-design.md)
**Shipped** all four phases: (1) OutlineRenderer per-mesh model push constant
(deleted the A2 vertex-bake stopgap; a live drag is a matrix update); (2) a
world-aligned translate/rotate gizmo on the selected occurrence, drawn in a
no-depth channel, live drag → snapshot-at-grab/solve-at-release; (3)
drag-within-mate (a mated occurrence's gizmo shows only the mate's free DOF,
oriented by the connector frame, and drives the mate DOF); (4) connector
inference snap — a mate connector snaps to the nearest hole (circular-edge)
center on pick. Verified via ImGui-test-engine `viewport_drag` over dev-watch.
Format v26. The hover snap-marker UI shipped 2026-07-24 (phase 4b) for edge
midpoints and hole centres. **Corner snapping shipped 2026-08-25**
(phase 4c, format v65). Its recorded
blocker — "`ElementMap` does not yet provide persistent vertex names" — was
mis-stated: a corner is where two NAMED boundary edges meet, so it needed a
second `ElementRef` on the anchor and no toponaming change at all. A2.5 is
now complete.
The same measurement found a **live performance defect in the shipped snap
hover** — `EdgeSnapPoint` re-mapped every edge of the body per call, so
hovering a 144-hole plate cost 10 ms/frame and a 400-hole one 88 ms.
**Fixed first, 2026-08-25** (`CADShape::FaceSnapPoints`: one walk, one map,
10 ms -> 0.097 ms), so corners land on a loop that is no longer the
bottleneck.


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
> format v24 (`mates/` file-per-mate). **Phase 1 SHIPPED** (e94a776): the
> mate model, `CADShape::FaceAxis`, the `AssemblySolve` placement pass, and
> v24 serialization — all headless, with the payoff loop (edit base part →
> mated occurrence follows) as a unit test. **Phase 2 SHIPPED** (048b0aa):
> mate RPCs (`add_mate`/`set_mate`/`remove_mate`/`list_mates`/`solve_assembly`)
> + MCP, `ui::SolveActiveAssembly` wired into mate/occurrence edits, and the
> `ui::PickAssemblyFace` join (an assembly click resolves a persistent face
> `ElementRef`). Headless `assembly_mates_smoke`. **Phase 3 SHIPPED**
> (4578518): the GUI — an assembly-mode toolbar (Fasten/Revolute/Slider), a
> two-click mate tool with a live connector-triad preview + Accept/Cancel/Flip,
> and a mate list + editor in the Model dock (DOF drag = numeric mechanism
> motion, error badges, delete). **Rest of A3 SHIPPED** (`6c59632`):
> cylindrical/planar/ball mate types (format v25) + connector inference snap
> points (hole-center snap, format v26, landed with A2.5). **A3 is complete —
> all six mate types.** (The drag gizmo was A2.5, also shipped.)

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

### A4 — Assembly features & beyond — **v1 SHIPPED (July 2026)** — [design: a4-design.md](a4-design.md)
> **2026-07-13:** design signed off and **all four phases shipped**:
> (1) part→assembly change propagation — per-part version stamps at solve,
> auto re-solve on tab activation/open (d2e1e6e); (2) bill of materials —
> derived rows by part, shared `Measure::MassOfBodies`, panel + `bom`/
> `export_bom` RPCs + CSV (278ba4c); (3) in-context editing (visual v1) —
> double-click an occurrence to edit its part with the assembly ghosted
> (part at identity, ghosts inverse-transformed; the renderer's first
> transparency: alpha-blend/depth-write-off second pass), live re-solve so
> ghosts track mates (4ff371c); (4) exploded views — persistent named views
> as pure view state, gizmo-authored offsets + radial auto-explode, format
> v27 (3b33bc1). **Deferred to their own designs:** assembly-level features
> (per-occurrence geometry breaks instancing; assemblies have no feature
> list) and configurations (needs occurrence suppression + assembly
> variables + part-config pinning); cross-part references in-context
> (occurrence-qualified ElementRefs) build on the shipped ghost frame math.

In-context editing (edit a part with the assembly ghosted around it),
assembly-level features (hole through multiple parts), interference check
([inspect I4](../inspect/plan.md)), bill of materials, exploded views,
configurations.

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
