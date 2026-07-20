# Dogfooding backlog — 2026-07-20 (swivel-caster round)

Second self-driven dogfooding round: an agent built a genuine 5-part swivel
caster (wheel/axle/fork/kingpin/plate) + assembly + drawing + FEM against its
OWN headless-preview instance (Tier-1 render loop, screenshot-verified every
step). Model at `CAD_files/swivel-caster.dsgn`, round-trip verified. **No
crashes.** Exercised the surfaces the fidget-spinner round never touched:
revolve, sweep, loft, moving mates (revolute DOF driven), drawings + PDF/SVG,
FEM, assembly interference/BOM/exploded.

Verified against source where load-bearing: `SymbolTable m_symbols` is a
`Document` member (#1 confirmed — variables are genuinely per-part).

## P1 — correctness / blocks parametric workflow

- **#1 Variables are PART-SCOPED, not document-global** (blocks-workflow +
  wrong-geometry). `set_variable` writes the active part's `Document::m_symbols`;
  each part has an isolated set. A part B sketch can't see part A's
  `axle_dia` — the whole point of named variables in an assembly (one
  `bearing_bore` driving axle + wheel bore + fork holes) doesn't work. The
  workflow prompt even calls them "document variables." **Compounding: expr
  fields silently fall back to the literal twin on unknown-variable, no error
  on the feature** — wrong-but-plausible geometry passes unnoticed (only
  Hole surfaced it, via a zero value). Fix is a design call: project-level
  shared symbol table (assembly-scoped or document-tree-scoped) vs. keeping
  per-part + an explicit "link/publish variable" mechanism. The silent-fallback
  half should error regardless (feature carries an error on unresolved ref).
- **#2 Loft twists/pinches when section vertex counts differ**
  (wrong-geometry). square(20)→circle(r5)→circle(r5) gives an hourglass with a
  pinched waist (vol < the top cylinder alone). Two identical coaxial circles
  loft to an EXACT cylinder, so the engine is fine for matched sections — the
  bug is section-pairing / start-vertex correspondence / anti-twist when edge
  counts differ (4 verts vs 1 curve). Lives in the loft section-alignment.
- **#3 Hole through:true silently drills the 1 mm default** when
  `diameter_expr`'s variable is out of scope (removed 6 mm³ not 420, no error);
  a variable resolving to 0 DOES error. Same root as #1's silent fallback +
  a bad default. through:true itself works.

## P2 — feature gaps / inconsistency

- **#4 Extrude hard-requires literal `distance` even with `distance_expr`**
  (friction/inconsistent). `{distance_expr:"plate_thk"}` → "key 'distance'
  not found", no body. Revolve defaults `angle` to 360 with only `angle_expr`
  — Extrude should mirror that (schema marks `distance_expr` optional).
  Deserializer reads `distance` as required.
- **#5 mass_properties on an assembly tab returns total: null**
  (missing). No world-space aggregation of occurrence masses/CoM; `bom` is
  the workaround (it gives total mass). Assembly should report combined
  mass + CoM (transform part inertia by OccurrenceMatrix, like assembly
  interference already transforms bodies).
- **#6 Mate posing is the roughest workflow** (friction — the assembly-motion
  path). The DOF itself is perfect (drove `set_mate(swivel, rotation)` and the
  sub-assembly rotated exactly about the kingpin axis). But building a clean
  pose from face mates is hard: revolute fixes axial position at connector
  CENTROID COINCIDENCE with no `offset` to slide along the axis (offset is
  slider/cylindrical only) → stacks collapse, wheel intersects plate; `flipped`
  on a vertical-axis revolute rotates azimuthally, can't flip up↔down; fastened
  face-mates centroid-snap with no lateral centering (wheel landed on a fork
  leg); and the `offset` SIGN is inverted (offset −16 → +16 world). Wants: an
  axial-offset DOF on revolute, an auto-center/align option, and a sign fix.

## P3 — ergonomics / messages / docs

- **#7 auto_explode with no view** → raw "key 'view' not found" json
  exception; must add_exploded_view first. MCP description reads like it
  creates one. Auto-create or give a real error.
- **#8 list_elements face `normal` is always [0,0,0]** — faces have
  kind/area/centroid/name but no usable normal; agents must infer orientation
  from centroids to place holes/mates on "the +X face." Populate it.
- **#9 `part=<int>` is stable-id, not slot index** — but list_parts shows
  both `index` and `id` prominently; passing the index silently returns a
  different part's geometry. Accept index too, or error on unknown id.
- **#10 No rename_part RPC** — the default first part is stuck "Part 1"
  (leaks into BOM + add_occurrence). rename_occurrence exists; add rename_part.
- **#11 Raw RPC screenshot(view=…) ignores view** — reorientation is
  client-side in the MCP wrapper (view then screenshot). Raw callers get
  byte-identical shots. Align the raw RPC or note it (the MCP tool is fine).

## Solid (confirmed working well)
Revolve (exact volumes, intuitive axis refs); Sweep (auto-mitered corner
fillets, <0.4% vol error); Hole/Extrude (with literal), multi-point sketches;
**FEM the standout** — full mesh→run→von Mises/FoS/colormap headless, believable
numbers (FoS 1.63, max at the fixed bore rim), load glyphs + deformed shape;
Drawing (3 HLR views + truly-driven dims + title block + PDF/SVG); BOM
(per-part + total mass); exploded view; assembly interference (caught every
posing error); measure/section/checkpoints/history; git-backed round-trip.
Minor cosmetic: set_material updates mass but doesn't recolor the shaded body
in the viewport.
