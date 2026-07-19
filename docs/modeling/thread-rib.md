# Thread + Rib — the last two modeling features

Status: SHIPPED 2026-07-19 (rib 098b4d7, cosmetic thread 9cb1167; modeled
thread remains the opt-in stretch flag below).

Scope: the two items keeping workstream 2 from complete ([plan.md](plan.md) M8
tail: "thread wizards, rib"). Both are ordinary features in the M8 mold —
feature class + serialization + editor + tests — with one genuinely risky
corner (modeled helical threads in OCC), which this design fences off behind
an opt-in flag.

---

## Part A — Thread

### Where we actually are

Nothing thread-shaped exists, but every ingredient does: `HoleFeature`
(src/core/features/HoleFeature.{h,cpp}) is the wizard precedent — a parameter
set cut into bodies, expression-driven via `VisitExprs`; face references by
`ElementRef` are routine (draft picks faces, extrude's up-to-face resolves via
`ResolveFacePlane` in ExtrudeFeature.cpp); `SweepFeature` shows the
MakePipeShell sweep path, though it only sweeps sketch-wire spines — there is
no helix path type today. Fillet's dangling-ref error + "Pick edges…" repair is
the model for a thread whose cylinder face vanished.

### Design

**v1 default = cosmetic thread.** This is what SolidWorks/Onshape do by
default, for good reason (see the modeled-thread honesty below).

1. **`ThreadFeature`** (src/core/features/ThreadFeature.{h,cpp}): `ElementRef
   face` (a cylindrical face — external boss or hole wall; internal/external
   inferred from whether the face normal points away from or toward the axis),
   `std::string designation` ("M8", "M8x1"), `double pitch` + `threadClass`,
   expression-driven `length` (or full-face), `bool modeled = false`.
2. **Standard tables** (src/core/ThreadStandards.{h,cpp}): ISO metric coarse +
   fine — designation → pitch, minor diameter, tap-drill. Picking "M8" fills
   pitch/minor; a custom designation leaves them editable. Static data, unit
   tested, no I/O. (UNC/UNF later; the table is just rows.)
3. **Cosmetic rendering**: the geometry is untouched — the feature resolves its
   face each regeneration (validating it is cylindrical and the designation
   fits its diameter, error otherwise) and emits overlay polylines: dashed
   minor-diameter circles at both ends plus a dashed axis-parallel edge pair,
   drawn through the existing sketch-primitive line pipeline (`LineVertex3D`),
   dashing done by segmenting the polyline — no shader work.
4. **Drawings callout**: views of a part expose its threads; a callout ("M8x1.25
   – 6H") attaches by the same toponaming ref the drawing dims already use,
   rendered/exported like a driven dim (DrawingPanel.cpp + SVG/PDF path). If
   this drags, it splits off cleanly — the feature is useful without it.
5. **Modeled thread (opt-in)**: when `modeled` is set, build a helix — OCC has
   no helix primitive; the standard construction is a `Geom2d_Line` on a
   `Geom_CylindricalSurface` turned into an edge — sweep the ISO 60° V-profile
   along it with `BRepOffsetAPI_MakePipeShell`, and Cut the groove from the
   body (external) or the hole wall (internal). Result gets fresh
   primitive-style names via `topo::NamePrimitiveElements` (the
   sweep/draft/shell convention) — per-flank naming is explicitly out.

**Honesty about modeled threads**: a helical sweep-cut is the most
failure-prone boolean OCC does — seconds per thread at normal deflection,
self-intersection at the runout, seam-edge tolerance fights on the cylindrical
face. That is *why* real CADs default cosmetic. So: no thread runout (ends are
square), the helix is chained per-turn if MakePipeShell balks at many turns,
and any OCC throw is caught into the per-feature error state (regen-failure UX,
already shipped) with the cosmetic overlay still drawn. `modeled` failing must
never take the document down.

### Tests

- `tests/test_thread.cpp` (model: test_hole.cpp): table lookups (M8 → pitch
  1.25, minor Ø 6.647); cosmetic thread on a cylinder leaves volume bit-exact;
  non-cylindrical / dangling ref → feature error; round-trip. Modeled: one
  M8×1.25 through a short boss, volume strictly below the blank and within a
  loose band of blank − groove estimate (no exact helical-volume asserts —
  they'd be flaky), regenerate twice → identical volume.
- GUI: arm the thread tool, click a cylindrical face, assert the feature holds
  a resolving `ElementRef`. Overlay look = dev-watch screenshot review.

---

## Part B — Rib

### Where we actually are

Every mechanism exists under other names: `sketch::BuildOpenPathWire`
(src/core/sketch/SketchProfile.*) already validates an open chain for sweep;
M3's thin extrude already offsets wires via `BRepOffsetAPI_MakeOffset`;
`CADShape::SplitSolids` (M5 patterns) splits a cut result into components;
hole's through-all already computes a bbox-diagonal "far enough" length. Rib is
a composition, not new machinery.

### Design

**`RibFeature`** (src/core/features/RibFeature.{h,cpp}): `uint64_t sketchId`
(open profile), expression-driven `thickness` (symmetric about the profile —
per-side variants later), `bool flipDirection`, `uint64_t bodyId` (the M4
`Body::createdBy` combo, like boolean/pattern operands). Execute:

1. Open profile → wire (`BuildOpenPathWire`), offset both ways by thickness/2
   (`MakeOffset` in open-wire mode yields the closed slab contour), face it.
2. Prism the slab along the sketch normal (flip per `flipDirection`) by the
   bbox-diagonal length so it fully passes through the target body.
3. **Up-to-body trim**: `Cut(slab, body)`, `SplitSolids`, keep the component
   touching the sketch profile — that is exactly the material between the
   profile and the body wall, conforming to curved walls for free. If that
   component still reaches the slab's far end, the profile never hit the body:
   per-feature error "rib profile does not reach the body" (existing
   regen-error UX; same tone as sweep/hole messages).
4. Fuse the kept component into the target via `ApplyBodyMode`-style merge;
   result gets fresh primitive-style names (rib faces come from the offset
   wire, not sketch entities — per-entity naming is not meaningful here).

**Deferred**: draft angle on the rib (same face-surgery fight that deferred
extrude draft, [plan.md](plan.md) M3), multi-profile rib networks, per-side
thickness.

### Tests

- `tests/test_rib.cpp`: straight profile spanning a box channel → exact
  thickness × span × height volume assert; profile stopping short of the wall
  → error, document still regenerates past it; flipDirection; curved wall
  (rib up to a cylinder) with a loose volume band; round-trip + regen
  determinism.
- GUI: create a rib from a selected open sketch via the dialog, assert a body
  volume increase.

---

## Shared plumbing (one pass, both features)

- **Serialization**: both in src/core/io/SerializeFeatures.cpp, **one bump**
  v35 → v36 (`kVersion`, SerializationInternal.h), landed with whichever
  feature merges first so the schema is reserved; **one golden entry**
  `tests/golden/v36-thread-rib.json` exercising both, finalized when the
  second merges (per-kVersion-bump golden policy, F1 / test_golden.cpp).
- **GUI**: editors in src/ui/FeatureEditor.cpp using the U3 `LeftLabel` /
  `ExprDim` patterns; creation via toolbar/menu with the preview/accept flow
  (PreviewFeature.cpp). Thread's face pick reuses the fillet pick flow with a
  cylindrical-face filter (U2 selection filters); rib's dialog is sketch combo
  + body combo + thickness + flip.
- **RPC/MCP**: the generic `add_feature`/`modify_feature`
  (src/remote/RemoteModeling.cpp) carry both for free once
  `FeatureFromJson/ToJson` know the types; add a `remote_smoke.py` block each
  and update the MCP `add_feature` schema notes.

## Order & effort

**Rib first** (~1 session): pure geometry over five existing helpers, no
tables, no overlay rendering, no new pick filter — it also lands the v36 bump
that thread then rides. **Cosmetic thread second** (~1–2 sessions: table +
face-ref + overlay + callout). **Modeled thread last**, strictly as the opt-in
flag on an already-shipped feature — the riskiest OCC work goes where a
failure costs a flag, not a milestone. Workstream 2 reads **Complete** when
cosmetic thread ships; modeled thread is the stretch goal of the same design.
