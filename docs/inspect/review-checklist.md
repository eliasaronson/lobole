# Design review checklist

Run this against a model before showing it to anyone — an agent before it
reports done, a person before a review. Every box is one RPC call. The MCP
`modeling-workflow` prompt points agents here. What each RPC returns is in
[README.md](README.md).

## Every model

- [ ] `get_state`: no feature `error`s and no `warning`s. A warning means a
      feature built but likely did nothing — a cut that removed no material.
- [ ] Each mutating response's `digest.delta` matched the intent: the volume
      moved the right way, `added` / `removed` name the bodies you meant, and
      no body's `solids` count silently went to two.
- [ ] `mass_properties`: total mass and centre of mass are physically
      plausible for the object. A render cannot fake these.
- [ ] Every driving dimension is a named variable; no literal where an
      expression belongs.
- [ ] `render_sheet`: one call, four views and the numeric footer. Check the
      pixels against the footer numbers in the same image.
- [ ] The orientation of any swept or revolved feature is read from the
      digest bbox, not from a render tile — small tiles mislead, the bbox
      does not.
- [ ] Multi-body part: `contact_report` on the part tab reports ONE component.
      Overlapping bodies join like a weld at this scope, so do not assert
      `clashes` empty here.

## Assemblies

- [ ] `contact_report`: `connected` is true. Anything in its own component
      is floating.
- [ ] `contact_report`: `clashes` and `near_misses` empty. A 0.1–1 mm gap is
      almost always an arithmetic slip, not a design choice.
- [ ] Its receipt's `standing` is not `unverified` — a truncated sweep is not
      a clean one.
- [ ] Every bolted or fastened joint asserted `"touching"` with
      `interference {a, b}`. A clash-only check passes exactly when nothing
      touches.
- [ ] Every designed clearance asserted by number:
      `state == "clear"` and `|distance - intended| < tol`.
- [ ] With mates: `unrealized_mates` and `unmated_contacts` empty or
      explained.
- [ ] `bom`: no `warnings`, total mass non-null, quantities as intended.

## Against a reference

- [ ] `compare_geometry {golden_path}`: "identical within tolerance" for a
      regression; for a reproduction, read the verdicts — they name over- and
      under-generation and size or offset errors.
- [ ] Reproducing a shape rather than a placement: add `align: true`. IoU is
      computed where the two solids sit, so a correct re-draw built about its
      own origin scores low against a reference placed elsewhere.
- [ ] When the verdict is off: export `missing_path` / `extra_path` (or
      `materialize: true` and a screenshot) and look at where the difference
      lives before changing parameters.

## Symptom → cause → fix

| symptom | likely cause | fix |
| --- | --- | --- |
| A cut or add changed nothing (warning set) | The sketch plane's V axis points away (V = normal × uAxis), or the profile misses the body | Re-plane: normal −Y with uAxis +X gives V = +Z; check the digest delta |
| `unknown field 'x' for T` | A typo, or a field that type never had (a Fillet has no `mode`) | Read `describe` → `feature_types[].params` for T |
| BOM mass null | A body has no material (`bom.warnings` names it) | `set_material` on that body's creating feature |
| `contact_report` shows two or more components | A member's coordinates drifted from the geometry it should touch | The component listing names the floater; fix its placement, prefer a mate onto a named frame |
| A near miss at 0.1–1 mm | An arithmetic slip in a position (an offset applied once instead of twice) | Recompute from the mating face, then assert `touching` |
| A screenshot from inside the model | `set_camera direction` points from the target toward the camera | Pass `eye` instead |
| An occurrence ends up inside a neighbour after rotating | A hand-derived quaternion and rotated-corner arithmetic | Use `transform.rotate {axis, angle_deg}` |
| `render_still` on an assembly refuses with "no occurrences" | The assembly tab is empty | Add occurrences, or render the part tab |
