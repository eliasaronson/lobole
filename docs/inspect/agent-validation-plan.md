# V — Agent feedback & validation — plan

> Status: **V1–V6 ALL SHIPPED (2026-08-18, one session).** What shipped
> matches the plan below with three field notes: compare_geometry FUSES
> each side before the booleans (compound volumes double-count overlapping
> bodies and silently break every metric — caught by the smoke test);
> the strict-field allowlist admits the get_state envelope keys
> (index/error/warning) so read-modify-write of a get_state feature entry
> stays legal; and render_sheet's footer font is a hand-authored 5x7
> bitmap (ASCII-art glyph table in RenderSheet.cpp) rather than anything
> clever. Tests: contact_smoke, strict_inputs_smoke, compare_smoke,
> papercuts_smoke, a render_sheet phase in remote_smoke, ContactBetween
> cases in test_measure.cpp; build_dock.py now gates on
> touching/clearance/connectivity and is the standing acceptance script.
> Deferred list at the bottom unchanged.
>
> Original plan (for the record):

> Consolidates
> `docs/dogfooding.md (2026-08-18 round)` (retro P1–P5) and
> `docs/ai-cad-survey-2026-08-18.md` (external adoption list) into one
> workstream. Theme: the agent loop must be *execute → measure → render*,
> and today the measure rung is missing exactly where it matters — a
> script can prove "not clashing" but not "connected". Externally
> validated: feedback tooling, not model quality, is the measured
> bottleneck (build123d-mcp took a codegen benchmark 88%→100% validity
> with tools alone), and "floating parts" is the canonical LLM failure
> mode everywhere.
>
> Non-goals here: appearance-vs-material (own plan,
> docs/rendering/appearance-plan.md), semantic feature recognition
> (find_holes — deferred, see bottom), ray-traced assembly render_still
> (deferred; V5 makes it error honestly instead of lying).
>
> **No format bumps anywhere in this workstream** — everything is
> RPC/inspection/tooling-level; documents are untouched. GUI surface
> changes are minor (BOM warning tint), so no new defaults/* gates.

## V1 — Contact & connectivity (the headline)

**a. `interference` grows a distance.** Result gains
`distance` (min gap via `BRepExtrema_DistShapeShape`, 0.0 when
touching/clashing) and `state`: `"clash"` (common volume > tol),
`"touching"` (no common volume, distance ≤ 1e-3 mm), `"clear"`.
Existing `clash` bool stays. Scripts can then assert every bolted joint
`touching` and every designed clearance by number.

**b. New `contact_report` RPC (assembly tab).** One call, whole
assembly: AABB prefilter (inflated 1 mm) over all occurrence pairs →
exact distance on candidates → adjacency graph at touch tolerance.
Returns `components` (lists of occurrence names — **len 1 = the
assembly is one connected structure**), `pairs` [{a, b, state,
distance}] for candidates, `near_misses` (0 < d ≤ 1 mm — almost always
an arithmetic slip), `clashes`. When mates exist: `unmated_contacts`
and `unrealized_mates` (intended-vs-realized graph diff naming the
floating part). Core in `src/core/` next to the interference helper;
RPC in RemoteInspection.cpp (+ describe row — tests enforce).

Perf envelope: dock = 57 occurrences → 1596 pairs → prefilter leaves
~10², BRepExtrema on those is sub-second headless. Cache per
(assembly version, transforms hash) if it ever shows up in profiles.

**Tests/acceptance:** unit tests with box pairs (touching / 5 mm gap /
clashing / diagonal near-miss); a deliberately-floating occurrence must
yield 2 components. Acceptance: `scratch/build_dock.py` replaces its
clash-only checks with `state` assertions + a final
`contact_report` gate (1 component, no near-misses) — the v2
floating-stops defect must be reproducible as a red assertion by
reverting its fix in the script.

## V2 — Strict inputs & richer regen digests (retro P2)

- **Strict feature-JSON on the RPC path.** After building a feature
  from `add_feature`/`modify_feature` JSON, serialize it back and error
  on request keys not in (round-tripped keys ∪ per-type allowlist).
  One generic mechanism, no per-feature tables to maintain; the file
  deserializer stays tolerant for old documents. Kills the
  Box-ignores-"mode" class.
- **Digest in every mutating response.** Regen summary gains per-body
  {volume, bbox, solid-component count} and deltas vs before the call.
  "Execute → measure" then happens without a follow-up RPC, and a
  zero-delta add/cut is visible in-line (the existing no-op warning
  stays the error channel).
- **BOM warnings.** `bom` result gains
  `warnings: ["body 2 of 'X' has no material"]` whenever masses go
  null; GUI table shows the row amber. No more silent null totals.
- Document `mass_properties`/result shapes in the describe docs while
  in there.

**Tests:** unknown-field error per feature type (loop the registry);
digest deltas on an extrude add/cut; bom warning on an unassigned body.

## V3 — `render_sheet` (one image, many views, numbers burned in)

New RPC `render_sheet {path, views?, size?}` on GUI/hidden instances:
renders the standard named views (default iso/front/right/top; optional
back/bottom) offscreen at equal tile size, composites into one PNG with
a footer strip: bbox, volume, mass, occurrence count, and — with V1 —
contact components + clash/near-miss counts. Footer text rasterized via
the ImGui font atlas into the strip; tiling CPU-side on the readback
buffers (no new deps). Convergent best practice from cad-skill /
agentcad / CADSmith; kills the set_camera guessing loop and puts the
kernel numbers inside the same image the eyeball pass reads.

Optional V3b (cheap once V3 exists): `sheet: "hlr"` variant that
reuses the drawings HLR pipeline for a 2×2 hidden-line sheet — research
(MUSE) says line art judges better than shaded renders.

**Tests:** gui-labeled test asserting the PNG exists with expected
tile grid size; footer content unit-tested as a string builder.

## V4 — `compare_geometry` (the VibeCAD boolean diff, on our kernel)

`compare_geometry {a, b | golden_path, materialize?}`: boolean
common/cuts (BRepAlgoAPI) → `iou, dice, precision, recall,
volume_ratio, center_offset, bbox_iou, surface_ratio`, topology counts,
and a thresholded English `verdict` ("Under-generating: 17.8% of the
reference is missing" — ~30 lines that make the numbers actionable).
`a`/`b` are bodies, occurrences, or an imported STEP/BREP golden.
`materialize: true` draws the missing (A−B) and extra (B−A) solids over
the part in red/green so one `screenshot` shows the delta instead of two
images to eyeball. SHIPPED 2026-08-23 — as a renderer overlay tracked
apart from the document (the topology-ghost pattern), not as display
bodies: the delta must never be pickable, exportable or saveable, and it
drops itself on any edit or tab switch. See
docs/versioning/f11-seeing-what-changed.md. Idea source is MIT-licensed (rawwerks/vibecad); ours is a
fresh implementation on OCC regardless.

**Tests:** identical-body IoU 1.0; known offset/scale cases; verdict
strings. Follow-up use (separate, small): a `tools/golden-compare.sh`
regression harness for regen/toponaming/import-replay against stored
BREPs.

## V5 — Ergonomics papercut batch (retro P3/P4/P5)

Small, independent, land in any order between phases:
1. `add_occurrence`/`set_occurrence_transform` accept
   `rotate: {axis, angle_deg}` (server-side quaternion conversion) —
   removes the hand-derived-quaternion error class.
2. `set_camera` accepts `eye: [x,y,z]`; document that `direction`
   points target→camera (one sentence that cost three renders).
3. `describe` feature_types gain `params` (name/type/doc/axis
   conventions) — hand-authored next to the registry, with the existing
   "table row per type" test extended to require params.
4. `key_press` gains PageDown/PageUp/Home/End tokens; new
   `scroll {window, dy}` panel scroll (test-engine SetScrollY).
5. `render_still` on an assembly tab errors with "part-gated — use
   screenshot/render_sheet" instead of returning ok:true + an HDRI
   photo.

## V6 — Package the loop as a skill

Ship an `llms.md`/skill with the MCP server (and reference it from the
modeling-workflow prompt): the execute → measure (digest/contact_report)
→ render_sheet loop as the mandated iteration pattern; the review
checklist as a versioned doc (docs/inspect/review-checklist.md) with
cad-skill-style boxes ("no floating or disconnected geometry" is now a
one-call gate, "assert no regen warnings", "verdicts before renders");
a symptom→cause→fix table seeded from the dock retro (V-axis no-op cut,
mixed BOM, target→camera). Cheapest phase, locks the gains in.

## Order & sizing

V1 (M) → V2 (M) → V3 (M, only phase touching the renderer) → V4 (S/M)
→ V6 (S). V5 items (XS/S each) interleave as breaks. V1 first — it is
the acceptance-test-ready headline; V6 last — it documents what shipped.
Each phase lands with its tests green (`ctest -LE gui` + gui suite for
V3) and a `build_dock.py` re-run as the standing acceptance script.

## Deferred (explicitly out)

- `design_audit` parameter perturbation (novel, wants the V2 digest
  machinery first — revisit after V4).
- Semantic feature queries (`find_holes`, hole patterns, "put the bolt
  in hole 3") — real feature-recognition work; own design when a
  dogfood round demands it.
- VLM-checklist judging (CADCodeVerify pattern) — an agent-side
  practice on top of render_sheet, not app code; goes in the V6 skill
  as a recommended pattern only.
- Annotated-screenshot pointing channel; ray-traced assembly stills;
  scan-to-outline import.
