# Dogfooding backlog — 2026-07-19 (fidget-spinner session)

Collected from modeling a three-lobe 608-bearing fidget spinner end-to-end
through the MCP interface, plus Elias's hands-on GUI round, plus a comparison
against [flowful-ai/cad-skill](https://github.com/flowful-ai/cad-skill)
(CadQuery-driving Claude skill). Owner areas link to the plan docs.

## Sketching / dimensions (GUI) — the big UX cluster

- **Center rectangle tool** (rectangle from center + corner; today only
  corner-corner). Small; SketchToolbar + AddRectangle variant.
- **Dimension tool rework — make it a real tool** (docs/sketching/plan.md):
  today's flow fights every CAD habit. Wanted: arm a Dimension tool, then
  click targets, tool infers the dimension kind (traditional CAD order):
  - one line → its length; two parallel lines → the perpendicular distance
    (today two lines yield the DIAGONAL endpoint-to-endpoint distance);
  - point+line → perpendicular distance; two points → distance;
  - circle/arc → radius/diameter (already good).
  Selection precision underneath it: lines are currently hard to pick, and
  clicking a point grabs both the line and the point (need pick priority +
  disambiguation like the model-space same-spot cycling).
- **Dimension input = expression field**: arithmetic ("22/2 + 0.15") and
  variable references must evaluate in the inline dimension editor — today it
  apparently only takes plain numbers (the constraint model supports exprs;
  the GUI editor is the gap).
- **Dimension-on-create** (SolidWorks/Onshape habit): while a drawing tool is
  active, typing a number applies it as a driving dimension to the entity
  just drawn (line length, circle radius) — "sketch by numbers". Big
  ergonomic win; pairs with the U-backlog "dimension-first sketching" item.
- **Dimension label placement must stay on the dimension's own axis**: the
  witness/extension-line geometry should constrain the label drag —
  perpendicular offset only — instead of free 2D dragging that shears the
  indicator away from what it measures.

## Patterns

- **Linear sketch pattern shows no preview** (circular does). Verify whether
  the op even applies or silently no-ops; fix the preview path either way.
- **SEGFAULT: circular pattern on a part body** (dev-watch log 14:54:54):
  repeated per-frame "Document synced" lines with fresh OutlineRenderer mesh
  ids (597/598/599…) then SIGSEGV — looks like a per-frame version bump
  (preview param drag?) racing the outline resync. Repro attempt + fix in
  flight; if it survives this session it is the top bug here.

## Rendering (fold into S3, docs/rendering/ssao-soft-shadows.md)

- **Dark dielectrics wash out**: plastic_black (0.05 albedo) renders nearly
  identical to the unassigned palette grey — the IBL specular term
  (albedo-independent for dielectrics) dominates. Gold/brass/white fine.
  Screenshots in the session record. Likely needs specular-AO/horizon
  attenuation or prefilter energy check.
- **SSAO imperceptible** at default strength in concave junctions
  (plastic_white spinner: lobe/hub creases show no visible darkening).
- **Expose the new View toggles over RPC/MCP** (Shadows, Ambient Occlusion)
  so agents can A/B them for visual verification.

## MCP / scripting interface

- **Expression-driven sketch geometry**: entity coordinates/radii are
  literal-only through add_feature; only constraints carry exprs. Either
  accept `*_expr` on sketch entities or provide an "anchor + distance
  constraint" helper — without it, in-plane parameters (arm_len, lobe_r)
  don't actually drive.
- **list_elements should return edge kind/length/radius (and face kind/area)**
  inline — identifying the 8 bore rims took measure-probing opaque names
  ("6/g1;U0;M6" turned out to be a cylinder seam, the real rims split across
  two naming families).
- **set_material → renderer**: confirmed working (early confusion was the
  plastic_black wash-out above), but while probing: consider returning the
  applied baseColor in the reply for agent-side sanity checks.
- **Headless preview**: screenshot needs a GUI instance; an STL-based or
  offscreen render fallback would make agent/CI flows self-sufficient
  (cad-skill renders 6-view orthographic PNGs from the STL headlessly).
- **Watertightness/manifold check on export_stl** (cheap with OCC; report
  shells/free-edges count in the reply).
- **Cross-part element queries**: while assembling, resolving the spinner's
  face names required set_active_part round-trips (list_elements only reads
  the active tab). Let list_elements/measure take an optional part name/id.

## Workflow prompt (tools/designer_mcp.py WORKFLOW) — cad-skill adoptions

- **Manufacturing section**: FDM minimums (walls ≥1.2mm, hole clearance
  0.2–0.3mm, min feature 0.4mm), print-orientation note in the final answer,
  one-line print recipe convention (material/layer/walls/infill + why).
- **"Research real dimensions first"** rule: when interfacing with real
  products (bearings, inserts, connectors), web-search the datasheet numbers
  before sketching; never guess.
- **Phased delivery cadence**: base shape → show the user → features → show →
  finish (chamfers/fillets) → deliver STL + parameter table + "want me to
  adjust anything?" closing offer.

## Assemblies (full-path exercise: spinner + 4 bearings, fastened mates)

The path WORKS end-to-end (part → assembly → occurrences → face-connector
mates → solve → save → per-part STL export). Findings:

- **Assembly-level interference is missing**: the `interference` RPC (and the
  underlying I4 check) is part-tab only — "the active document is an
  assembly" error. Clash checks matter MOST in assemblies; extend it to
  occurrence pairs (transform the part bodies like assembly measure does).
- **Stale-toponame trap on mates**: bore faces listed BEFORE a chamfer kept
  their old names ("6/g1;M6"); after the chamfer they are "6/g1;M6;M7", and
  the mate errored "connector reference could not be resolved". Correct
  behavior (the history really changed the face), but the error should name
  WHICH connector and ideally suggest near-miss names ("6/g1;M6;M7 exists —
  face was modified by feature 7").
- add_mate flipped=true default seats a bearing in a bore flush and
  coaxial — good default, worked first try once names were fresh.

## Crash-debugging infrastructure (added this session)

- Fatal-signal backtraces (3284986): SIGSEGV/SIGABRT now print a stack to
  stderr → dev-watch log. glibc backtrace now; std::stacktrace when C++23.
- Circular-pattern segfault: headless param sweep (count 0..100, angle
  -90..720, per-frame modify) does NOT reproduce — core exonerated, crash is
  GUI/render-side (log shows per-frame resyncs then SIGSEGV). Next GUI repro
  will carry a backtrace.
