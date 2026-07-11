# M3 — Richer extrude — design

The last thin spot in the bread-and-butter workflow. Four sub-milestones,
sequenced so the one users hit hardest lands first. Estimated **~3.5 days**.

## What already exists (grounded in code, Jul 2026)

- `ExtrudeFeature` (`src/core/ExtrudeFeature.h`): `{sketchId, distance +
  distanceExpr, reversed, mode NewBody|Add|Cut}`; executes via
  `sketch::BuildPrismNamed(plane, loops, distance, reversed, tag, map)` and
  `ApplyBodyMode` (shared with revolve, propagates the element map through
  Add/Cut booleans — T5).
- `BuildProfileFace` builds ONE outer + holes; since 2026-07-10 it **rejects
  disjoint loops loudly** (containment check via `LoopPolygon` +
  `PolygonContains`) instead of the old silent no-op. That check is the seed
  of the multi-profile classifier.
- Toponaming complete (T1–T5): `ElementMap` face/edge names, propagation
  through booleans (`topo::Propagate`, `BRepTools_History`), `ElementRef`
  consumers shipped (sketch-on-face v16, fillet/chamfer v17), face + edge
  picking in the viewport, `list_elements` over RPC.
- Format `kVersion = 17`. Everything below serializes as **v18** (one bump,
  all fields optional-with-defaults so v17 files load unchanged).

## M3.1 — Multi-profile sketches (~1 day) — the headline

A four-bolt-hole cut in ONE sketch. Classification by containment depth,
using the existing tessellated-polygon machinery:

- `BuildProfileFaces` (plural) replaces the single-outer assumption: compute
  each loop's containment count (how many other loops contain its sample
  point). Even depth → outer; odd depth → hole of its **immediate** container
  (deepest containing outer). Islands-in-holes (depth 2) become outers again
  — Onshape behavior.
- One `TopoDS_Face` per outer (with its holes), one prism each, then fuse:
  reuse the M5 fuse path with `topo::Propagate` so every prism's element
  names survive into the fused solid (bolt-hole cylinder faces stay
  nameable → filletable).
- `BuildPrism`/`BuildPrismNamed`/`BuildRevolve` all route through it, so
  revolve gets multi-profile for free.
- Overlapping (partially intersecting) loops remain an error — same loud
  path as today, message upgraded ("profiles overlap").
- Tests: 2/4 disjoint circles cut (the exact case from the S1 live run),
  rect-with-island (ring), nested 3 deep, multi-profile revolve, element
  names present on all fused faces. The disjoint-rejection unit test flips
  from expecting failure to expecting the correct volume.

## M3.2 — Symmetric + two-sided (~0.5 day)

- New fields: `endMode: blind|symmetric|two_sided` (default `blind`),
  `distance2 + distance2Expr` (the back side for `two_sided`).
- Symmetric = one prism swept `distance` total, centered: translate the
  profile face by `-d/2 · n` before sweeping (the revolve `symmetric`
  precedent). Two-sided = one prism per side, fused (names propagate).
- `reversed` stays orthogonal (flips the whole result), matching revolve.
- Tests: volumes + bbox placement (symmetric straddles the plane; two-sided
  d1 ≠ d2 offsets accordingly).

## M3.3 — Up-to-face (~1 day) — first post-toponaming extrude consumer

- New `endTarget: ElementRef` (feature_id + element name), used when
  `endMode: up_to_face`. Resolve exactly like fillet does (body by
  `createdBy`, face index by name), then `CADShape::FacePlane` for the plane.
- v1 scope: **planar faces only** — distance = signed projection of
  (face plane − sketch plane) onto the extrude direction; a non-planar or
  non-forward target fails the feature with a clear error. True
  trim-to-curved-face (half-space cut) is the v2, noted not promised.
- UI: the face picker from sketch-on-face (v16), reused in the extrude
  editor. RPC/MCP: element names via `list_elements`, same as fillet — the
  FORMATS text gains the field.
- Tests: extrude up to an offset datum-backed face, upstream edit moves the
  target → re-regeneration follows it (the toponaming payoff, asserted).

## M3.4 — Thin/wall extrude (~1 day) + draft (deferred)

- `thin: {thickness, thicknessExpr, side: out|in|both}`: offset each profile
  loop with the existing `BRepOffsetAPI_MakeOffset` wire machinery (the
  sketch `OffsetChain` precedent), build the face **between** the loop and
  its offset, prism as usual. Open profiles stay unsupported (profiles must
  still close) — thin here means hollow section, not sheet-metal.
- **Draft angle: explicitly deferred.** `BRepOffsetAPI_DraftAngle` wants
  face-level surgery that interacts badly with the element maps; it earns
  its own design when a real model needs it. Recorded in the plan, not
  promised in v18.
- Tests: ring volumes (out/in/both), thin + multi-profile combined.

## Cross-cutting

- **Serialization v18** once, at M3.2 (M3.1 changes geometry, not format);
  golden file `v17-full.json` (from F1) guards the migration.
- **Editor**: end-condition combo (Blind/Symmetric/Two-sided/Up-to-face) +
  second distance + face-pick + thin block, following the revolve editor's
  `ExprDim` pattern.
- **MCP FORMATS** + a `remote_smoke`/`headless_smoke` block per sub-milestone
  (standing rule: command layer first — every variant lands RPC-drivable).

## Order & risks

M3.1 → M3.2 → M3.3 → M3.4. Riskiest is M3.1's name propagation through the
multi-prism fuse (mitigation: it reuses the already-shipped M5/T5 fuse +
propagate path, and the tests assert names, not just volumes). If F1 (golden
corpus) hasn't landed before M3.2's v18 bump, pull `v17-full.json` forward —
the corpus should exist before the next format change.
