# Tier-1 closeout — finishing four workstreams — plan

> Status: **draft, for sign-off.** The goal is breadth, not depth: close out
> every workstream whose remainder is small — Interchange, Variables,
> Inspection, Versioning — so the board reduces to the four big blocks
> (Drawings, FEM, Assemblies round 2, hybrid RT). Four items plus a docs
> truth pass; each independently shippable, ordered cheapest-first.

## C1 — glTF/OBJ/PLY export (closes Interchange)

Interchange X2 as planned: the visualization interchange (Blender, web
viewers, engines). Verified available: OCC 7.9.3 ships `TKDEGLTF`/`TKDEOBJ`/
`TKDEPLY` + the XCAF stack (`TKXCAF`/`TKLCAF`/`TKCAF`) in the current nix
env; none are linked yet.

- **The work is the shape→XCAF bridge** (`TDocStd_Document` +
  `XCAFDoc_ShapeTool`/`ColorTool` labels), which then serves all three
  writers (`RWGltf_CafWriter`, `RWObj_CafWriter`, `RWPly_CafWriter`).
- **Materials map for real**: glTF is natively PBR metallic-roughness — the
  `CADMaterials` baseColor/metallic/roughness go through
  `XCAFDoc_VisMaterial`, so exported parts keep their actual appearance,
  not just a tint.
- **Binary `.glb` default**; format dispatched on extension through the
  existing export path (menu dialog + `export` RPC gain `.glb`/`.gltf`/
  `.obj`/`.ply`).
- **Assemblies export too** (recommended inclusion): XCAF represents
  assemblies natively (components + locations), and the occurrence loop is
  the same `PartResolver` walk the BOM uses — one glb of the whole assembly
  at solved (not exploded) placements. This is the "view the robot in the
  browser" artifact and costs ~an extra hour on top of the bridge.
- After C1, X3 (IGES/DXF/mesh import) is **declared demand-driven** and the
  workstream is complete.

## C2 — Units algebra (closes Variables)

`10 mm + 1 in` evaluates; `hole_pitch = 0.5in` just works.

- **Unit-suffixed literals only** — a unit token may follow a number
  (`10mm`, `10 mm`, `1.5in`); the literal converts to document base units
  (mm, degrees) at parse. Algebra stays plain doubles after conversion —
  **no dimensional analysis** (`10mm + 5` = 15: bare numbers are base
  units already; `10mm * 2in` is legal and means what the arithmetic says).
  Full unit *checking* stays deferred with per-document display units.
- Length: `mm` `cm` `m` `in` `ft`. Angle: `deg` (identity — angles are
  already degrees) and `rad`. Angle-vs-length is contextual, same as today.
- **Units win after a number**: `10 m` is ten metres even if a parameter
  `m` exists elsewhere; standalone `m` still resolves as an identifier. To
  keep that unambiguous, unit names join function names in
  `IsValidParameterName`'s reject list (a parameter literally named `in` or
  `m` can no longer be *created*; existing ones keep working since units
  only activate after a number token).
- Implementation is one place: the `BuiltinExpressionEngine` tokenizer
  (units are not identifiers — they never enter `Dependencies()`, and
  `RenameIdentifier` must not touch them).
- **Format v28**: unit syntax in stored `_expr` text is a format-contract
  extension — older builds must refuse the file cleanly rather than error
  every feature. No loader migration needed (purely additive syntax); one
  frozen v27 golden + round-trip per the policy.
- Measured/derived variables stay deferred (they need a staleness design
  and belong with a later inspect/variables follow-up). With C2 the
  variables workstream is complete; units *display/switching* is the
  recorded deferral.

## C3 — Result panels (with C4, closes Inspection)

The GUI face for what already works over RPC.

- **Mass properties panel**: a Properties-dock section for the selected
  body (or whole document): volume, surface area, mass (null-taint rule as
  the BOM), center of mass — plus a **CoG marker** in the viewport (overlay
  cross via the existing `LineVertex3D` channel). All reads through
  `Measure::MassOfBodies`; zero new math.
- **Measure tool**: a toolbar mode (part tab). Pick one element → its own
  measure (face area / edge length+radius); pick a second → distance
  between them; readout in a small overlay panel + the Properties section.
  Face picking exists; edge picking reuses the fillet `edgePick` mechanism.
  Escape/re-click clears. Uses `Measure::Describe`/`Distance` verbatim.
- Angle-between-faces and point picking stay out (they were I1's "needs
  unified selection" tail — record as polish, not workstream-blocking).

## C4 — Section caps (I3 v2)

Filled cut faces so a sectioned part reads solid instead of hollow.

- On section-plane change (on release, not per-drag-frame): cut every body
  with the plane (`BRepAlgoAPI_Common` against a big planar face — exactly
  what `Measure::CrossSectionArea` does), tessellate the resulting cap
  faces, upload as ordinary opaque instances nudged epsilon inside the kept
  half-space so the raster clip doesn't discard them.
- Caps render in the body's material/palette color. **Hatching is deferred**
  (cosmetic; wants the drawings hatch style anyway). Caps regenerate on
  document change via the existing version-gated resync.
- With C3+C4, inspection is complete; the recorded deferrals: hatching,
  angle measure, bbox readout, curvature/thickness maps.

## C5 — Docs truth pass (closes Versioning by declaration)

No code. Fix the stale "remaining" claims found in the sizing survey:

- versioning/plan.md: F3/F5 are shipped (memory + roadmap agree); mark them,
  and state v1 complete — semantic merge and insert-at-marker are recorded
  as explicitly-later projects, not open work.
- sketching/plan.md: the closing "Remaining" list still names Phase N,
  which the doc body itself marks shipped; fix, and state the workstream
  complete with B-splines as the one recorded deferral.
- assemblies/plan.md: the "Remaining A3" line still lists the mate types
  that shipped 6c59632; fix.
- roadmap.md rows for interchange/variables/inspection/versioning as each
  item above lands.

## Decisions for sign-off

1. **C1 exports assemblies too** (XCAF components at solved placements),
   not just parts — small marginal cost, large artifact value.
2. **Units = suffixed literals converted at parse; no dimensional
   analysis**; unit names become reserved parameter names; **format v28**
   (additive, golden per policy).
3. **Measure v1 = one- and two-element readouts** (face/edge) through the
   existing pick mechanisms; angle/point measure recorded as polish.
4. **Section caps are solid-filled, not hatched**, rebuilt on
   plane-release + document change.
5. **Closeout by declaration where warranted**: X3 demand-driven, semantic
   merge later-project, measured variables deferred — each recorded in its
   plan doc so absence stays a decision, not a gap.

## Phasing (each independently shippable)

1. **C1 glTF/OBJ/PLY** (~1d) — CMake libs + XCAF bridge + writer dispatch +
   RPC/menu + smoke test (load the .glb back with the OCC reader, compare
   triangle/vertex counts).
2. **C2 units** (~1–1.5d) — tokenizer + reserved names + v28 golden +
   engine unit tests + a units smoke over RPC.
3. **C3 result panels** (~1–1.5d) — mass-props section + CoG marker +
   measure tool; GUI verification over dev-watch.
4. **C4 section caps** (~1d) — cap mesh build + resync wiring; dev-watch
   screenshots (sectioned part reads solid).
5. **C5 docs truth pass** (~0.5d) — plan-doc fixes + roadmap rows; final
   state: 9 of 13 workstreams complete.
