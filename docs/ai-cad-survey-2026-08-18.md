# What other AI-CAD systems can teach us — survey (2026-08-18)

Companion to `dogfooding.md` (the 2026-08-18 round). Three parallel research
sweeps: the two skills Elias found, the agent-interface landscape
(MCP servers, Zoo, Onshape, startups), and the research literature on
validating LLM-generated CAD. Full agent reports are summarized here;
the strongest signal is how much everything CONVERGES on the same few
mechanisms — and that our dock retro independently found the same gaps.

## Verdict on the two links Elias found

Both are real code, not prompt fluff.

- **flowful-ai/cad-skill** (528★, maintained, parts actually printed;
  CadQuery + trimesh/pyrender). The substantial one. Its contribution is
  closed-loop *discipline*: a strict runner where "script exited 0 but
  produced no STL" and "mesh not watertight" are hard failures; a
  6-view render sheet (iso/front/right/back-iso/top/bottom) on ONE
  canvas with a numeric footer (bbox, volume, watertight, weight); and a
  versioned `design-review.md` checklist the agent must execute against
  the sheet ("No floating or disconnected geometry" is literally a
  checkbox). **License is PolyForm Noncommercial — steal ideas, not
  code.**
- **VibeCAD `model-compare`** (rawwerks/vibecad, 108★, MIT, young).
  Smaller but the most directly portable piece anywhere in the survey:
  B-Rep boolean diff on OUR kernel (OCC) — missing = A−B, extra = B−A,
  common = A∩B — yielding IoU/dice/precision/recall, volume ratio,
  center offset, bbox IoU, topology counts, plus the diff BODIES
  exported for rendering, and ~30 lines of thresholded English verdicts
  ("Under-generating: 17.8% of reference missing").

## The three findings everything agrees on

**1. "Floating parts" is THE canonical failure mode, and contact graphs
are the standard answer.** Our dock retro's headline (clash-checkable
but not contact-checkable) is named independently by: a practitioner
case study ("most common failure: detached parts"), CADCLAW ("pytest
for STEP assemblies" — its first check is floating-part detection),
MUSE (diff the intended assembly graph against the realized one),
CAD-MLLM's SegE metric (count connected components), and build123d-mcp
(`clearance` returns distance/containment/overlap status, not just
clash). Recipe: pairwise `BRepExtrema_DistShapeShape` with bbox
prefilter → adjacency graph at tolerance → assert one connected
component, and diff against the mate/connector graph to name the
floating occurrence.

**2. The canonical agent loop is execute → MEASURE → then render.**
Kernel numbers first, pixels second. Query2CAD showed visual feedback
alone cannot fix dimensional errors; CAD-Judge showed kernel-as-judge is
far cheaper than VLM-as-judge; CADSmith pairs an exact kernel digest
(volume/bbox/CoM/face counts/validity) with three fixed renders and has
the judge cross-reference them. The quantified clincher: build123d-mcp
took CADGenBench validity from 88%→100% and score 0.360→0.457 with NO
model change — the feedback tools were the bottleneck. Zoo's KPI is
generation error rate (50%→16%). Property tests beat similarity metrics
for "is this the intended part" (CADTests: AUC 0.928 vs Chamfer's 0.66),
and LLM-authored geometry tests can be kept honest by mutation testing.

**3. Render ergonomics have a converged best practice.** One image, many
views, numbers burned in: cad-skill's 6-view sheet with numeric footer,
agentcad's auto 4-pane, CADSmith's three fixed angles, Zoo's multi-view
snapshots, and MUSE's finding that 2×2 HIDDEN-LINE engineering views
judge better than shaded renders (we already have HLR drawings!).
Plus: neka-nat/freecad-mcp attaches a screenshot to every mutating call
(named-view param, global text-only opt-out) and blender-mcp returns
base64 with a max_size cap. Nobody makes the agent do camera math.

## Other lessons worth keeping

- **Small surface + script escape hatch + introspection wins.**
  blender-mcp: ~4 core tools, 26k★; a 150-tool FreeCAD server:
  rounding-error stars. Validates our 118→25 MCP consolidation and the
  Python-script-as-master workflow. Fusion's official MCP is
  script-centric too, and exposes docs-as-a-tool
  (`fetch_api_documentation`) — the answer to our describe-schema gap.
- **Semantic addressing sidesteps transform pain.** build123d-mcp's
  `find_holes`/`find_hole_patterns` (BCD, arrays) and Fusion's
  `$selection_N` handles: the agent says "the bolt goes in hole 3",
  not a quaternion. Nobody does raw-transform ergonomics well; the
  winners avoid needing them.
- **Everyone credible generates a parametric program** (KCL, OpenSCAD,
  CadQuery, native features) — never meshes. Adam's UX detail: extracted
  dimensions become sliders whose edits are deterministic source edits,
  no LLM round trip — our variables + `set_variable` already enable
  this; agents should build variable-driven models by default.
- **Incumbents (SolidWorks/PTC/Siemens) ship human copilots, not agent
  surfaces.** Onshape's stated direction — agents as permissioned,
  audited teammates — is the only one pointing our way. The
  same-RPC-as-humans thesis has no shipped competitor yet.
- **CADCodeVerify's checklist trick**: derive 2–5 binary questions from
  the design intent, answer them against fixed views — beats open-ended
  "does this look right", needs no ground truth.
- **build123d-mcp's `design_audit`**: perturb each parameter ±ε and
  report which perturbations break regen — robustness of the parametric
  *program*, genuinely novel, very much in our defaults-gate spirit.

## Adoption list for Designer (priority order)

1. **`contact_report` + min-distance in `interference`** (retro P1,
   now with the full recipe and three independent confirmations):
   per-pair {clash | touching | gap d}, assembly connected-components,
   intended-vs-realized graph diff naming the floating occurrence.
2. **`render_sheet`**: one PNG, standard views (+ optional 2×2 HLR
   sheet), numeric footer digest (bbox, volume, mass, occurrence and
   contact-component counts, interference summary). Kills the
   set_camera-guessing loop and gives the eyeball pass ground truth.
3. **Kernel digest on every mutating RPC** — extend regen summaries
   with per-body volume/bbox deltas and solid-component count; a
   `strict` mode where zero-effect features and unknown feature-JSON
   fields are errors (retro P2).
4. **`compare_geometry`**: the VibeCAD boolean-diff metric set against
   a golden body/file, plus materialized missing/extra diff bodies
   rendered red/green. Doubles as a regression gate for
   regen/toponaming/import replay.
5. **Docs-as-a-tool**: `describe {method|feature}` returns full param
   schema + an example; ship a skill/llms.md with the MCP server
   including the review checklist (checklist-as-versioned-asset).
6. **Semantic queries for placement**: `find_holes`-class feature
   recognition + `rotate {axis, angle_deg}` on occurrences (retro P3).
7. Later: `design_audit` parameter perturbation; CADCodeVerify-style
   checklist judging as an agent pattern; annotated-screenshot channel
   (ModelRift) once reference-image rendering lands.

## Follow-up round (same day, after the V workstream shipped)

Four more repos Elias found, assessed against what we NOW have:

- **10-X-eng/vibecad** (FreeCAD fork with an embedded agent runtime, 138★,
  installers shipping, heavily agent-authored — mine for ideas, don't trust
  the engineering). Three keepers, all mapping onto our deferred list:
  1. `tool_notes.md` — a living audit of whether each tool's outcome is
     "decision-complete": a failed write must carry one plain-language
     error + the measured violated condition + one concrete correction in
     the API's vocabulary; first schema rejection should include the FULL
     correct template, not one constraint per round-trip. Plus screenshot
     postconditions: warn when a frame is blank, clipped, or unchanged.
  2. Parameterized geometry queries with `expected_count`
     (VibeCADGeometryInspection.py): declarative predicates
     (geometry_type/normal/radius±tol/area/near_point) instead of named
     finders — and `expected_count` turns a query into an assertion
     ("4 Ø6 cylindrical faces, axis +Z") in one call. This is the shape
     our deferred find_holes should take.
  3. Tool-less, schema-FORCED design reviewer (`submit_design_review`:
     verdict ready|revise + typed findings with severity/category/
     consequence/required_change, fed live facts only) — a concrete middle
     road toward the VLM judging we deferred.
- **rawwerks/VibeCAD** (full repo, beyond the model-compare skill we
  already matched): two small process ideas — an error→example index
  (map each known error string to a runnable known-good recipe) and a
  numbered "blind-spot" example suite kept green in CI (scripts encoding
  known agent mistakes: our V-axis trap, through_all direction, etc. —
  institutionalize what today lives only in memory files). Also a clean
  progressive-disclosure skill layout with CI-enforced semver bumps.
- **polyspectra/cad-recode-repro**: abandoned day-one repro — a
  preprocessing notebook, no model/training/eval. Nothing for us; the
  upstream filaPro/cad-recode is the real reference if ever needed.
- **polyspectra/nice123d**: HF-Spaces dockerization of someone else's
  300-line exec()-box viewer. Nothing for us.

Independent convergence note: 10-X-eng arrived at the same doctrine we
shipped — small turn context, strict schemas, measured postconditions.

Key sources: github.com/rawwerks/vibecad · github.com/flowful-ai/cad-skill
· github.com/pzfreo/build123d-mcp (llms.md) · github.com/ahujasid/blender-mcp
· github.com/neka-nat/freecad-mcp · github.com/sunnyday-technologies/CADCLAW
· zoo.dev/research/introducing-text-to-cad · arxiv 2409.17106 (Text2CAD) ·
2411.04954 (CAD-MLLM SegE/DangEL/FluxEE) · 2410.05340 (CADCodeVerify) ·
2605.07807 (CADTests) · 2603.26512 (CADSmith) · 2605.28579 (MUSE) ·
autodesk Fusion MCP blog · onshape.com AI blog.
