# Design review checklist (agent + human)

The V6 companion to the modeling-workflow prompt: run this list against the
model BEFORE showing it to anyone. Every box is one RPC call — none of this
is eyeball-only anymore. Seeded by the floating-dock dogfooding round
(docs/backlog-dock-dogfood-2026-08-18.md), where the unchecked versions of
these boxes each cost a review cycle.

## Every model

- [ ] `get_state`: no feature `error`s, no `warning`s (a warning means a
      feature built but likely did nothing — the silent no-op cut).
- [ ] Mutating responses' `digest.delta` matched intent at every step: the
      volume moved the right direction, `added`/`removed` name the bodies
      you meant, `solids` didn't silently split a body into two lumps.
- [ ] `mass_properties`: total mass and CoM are physically plausible for
      the object (the render cannot fake these).
- [ ] Every driving dimension is a named variable; no literal that should
      be an expression.
- [ ] `render_sheet`: one call, four views + the numeric footer. Check the
      pixels AGAINST the footer numbers in the same image.

## Assemblies

- [ ] `contact_report`: `connected` is true (ONE component) — anything in
      its own component is floating in mid-air, the canonical failure.
- [ ] `contact_report`: `clashes` empty, `near_misses` empty (a 0.1–1 mm
      gap is almost always an arithmetic slip, not a design choice).
- [ ] Every bolted/fastened joint asserted `"touching"` via
      `interference {a, b}` — clash-only checks pass exactly when nothing
      touches.
- [ ] Every designed clearance asserted by number
      (`state == "clear" && |distance - intended| < tol`).
- [ ] With mates: `unrealized_mates` and `unmated_contacts` both empty or
      explained.
- [ ] `bom`: no `warnings`, total mass non-null, quantities match intent.

## Against a reference (regression / reproduction)

- [ ] `compare_geometry {golden_path}`: verdict "identical within
      tolerance" for regressions; for reproductions read the verdicts —
      they name over/under-generation and size/offset errors.
- [ ] When the verdict is off: export `missing_path`/`extra_path` and look
      at WHERE the difference lives before touching parameters.

## Symptom → cause → fix

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Cut/add changed nothing (warning set) | Sketch-plane V axis points away (V = normal × uAxis) or profile misses the body | Re-plane: e.g. normal −Y with uAxis +X for V = +Z; check the digest delta |
| `unknown field 'x' for T` | Typo, or a field that type never had (Box has no `mode`) | Read `describe feature_types[].params` for T's dialect |
| BOM mass null / material "mixed" | A body was created without `set_material` (check `bom.warnings` — it names the body) | `set_material` on that body's creating feature |
| `contact_report` shows 2+ components | A member's coordinates drifted from the geometry it was meant to touch | The component listing names the floater; fix its transform |
| near_miss at 0.1–1 mm | Arithmetic slip in a position (an offset applied once, not twice, …) | Recompute from the mating face, assert `touching` after |
| Screenshot from inside the model | `set_camera direction` points target→camera | Pass `eye` instead |
| render_still shows only the environment | Assembly tab (part-only renderer) | `render_sheet` / `screenshot` |
| Occurrence ends up inside a neighbor after rotation | Hand-derived quaternion + rotated-corner arithmetic | Use `transform.rotate {axis, angle_deg}` |
