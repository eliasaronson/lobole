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
> add_assembly/add_occurrence RPCs + MCP + assembly_smoke test. Remaining:
> phase 2 (rendering: occurrences via the instancing API + hit→occurrence
> picking) and phase 3 (move/rotate gizmo + occurrence-edit undo).

A new document type: an ordered list of **occurrences** `{ partRef, transform,
name, visibility }` — instances of parts placed by explicit transform.
Move/rotate gizmo for positioning. Rendering via the instancing prerequisite.
Picking maps hit → occurrence (then into the part on double-click). This alone
is a usable v1 assembly (many real workflows are "place parts, eyeball it").

### A3 — Mates
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

A1 → A2 (already useful) → A3 fastened/revolute/slider → the rest of A3 → A4.
Do not begin A1 until modeling M1–M5 + variables are done and the incremental-
regen cache exists; begin the mate-connector design sketch alongside toponaming
T4/T5 since they share the reference model.
