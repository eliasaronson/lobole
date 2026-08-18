# Backlog — floating-dock dogfooding round (2026-08-18)

Source: the Marsliden floating-dock session (`scratch/build_dock.py`, six
design iterations, ~15 script runs, all scripted against a headless
instance with Xvfb preview renders). Two app bugs were found and one was
fixed in-session (the dockspace/part-tabs overlay). The rest, ordered by
how much of the session they cost:

## P1 — Contact is unverifiable (caused both structural review rounds)

The two worst design defects — the v1 cradle that only *kissed* the frame
at a plane, and the v2 barrel stops left floating in mid-air when the
rails moved — passed every scripted check, **because the checks passed
exactly when nothing touched**. `interference` answers "is intersection
volume > 0" and nothing else, so a script can prove not-clashing but
never prove connected. Both defects were caught by human eyes on renders.

- `interference` should return `distance` (min gap between the pair;
  0 = touching, optionally with contact area). Scripts then assert
  `gap == 0` for every bolted joint and `gap == 5` for designed
  clearances — the whole defect class becomes assertable.
- Bigger hammer: an assembly-level `contact_report` that builds the
  touch graph (bbox prefilter, then exact min-distance) and returns
  disconnected components ("these 8 occurrences touch nothing") plus
  near-misses (0 < gap < 1 mm, usually an arithmetic slip). One call
  gates the "floating member" class; ~1.6k pairs at 57 occurrences is
  cheap after the prefilter.

## P2 — Feature JSON accepts unknown fields silently

`add_feature {type: Box, ..., mode: "add"}` was accepted, `mode` ignored,
and a second body silently created — surfacing three calls later as BOM
material `mixed` with `null` masses and a null total. The RPC layer
validates *param* names strictly ("unknown parameter 'topic'"), but the
feature-JSON path does not validate *field* names.

- Validate feature fields strictly on the `add_feature`/`modify_feature`
  path (the file deserializer stays tolerant for old documents).
- `bom` should carry `warnings: ["body 2 of 'Corner mooring' has no
  material"]` when masses go null — and the GUI table should show it.

## P3 — Scripted placement ergonomics

- Occurrence orientation is quaternion-only. Hand-deriving rotated-box
  positions for the ±90° end posts produced a real error (first attempt
  put them inside the end joist; caught by re-derivation, not by the
  tool). `add_occurrence`/`set_occurrence_transform` should accept
  `rotate: {axis: [0,1,0], angle_deg: 90}` alongside quaternions.
- No body/part mirror (`mirror` is sketch-entities only), so the chiral
  corner-mooring bracket needed a second hand-mirrored part. A Mirror
  body feature would collapse "Corner mooring M" into one part.
- `set_camera` `direction` points **target → camera**; undocumented, and
  the intuitive reading (view direction) aims the camera backwards —
  cost three renders from inside a barrel. One doc sentence, or accept
  an `eye` position param.

## P4 — Introspection gaps

- `describe` lists feature types but not their parameter schemas — Box
  semantics (position = min corner; width/height/depth = X/Y/Z) needed
  an empirical probe part. Attach a params list per feature type.
- `mass_properties` nests the summary under `"total"`; the shape is
  undocumented and was guessed wrong twice. Document result shapes in
  the method docs (or `describe {method: ...}` filtered output — which
  was instinctively tried twice and doesn't exist).

## P5 — Remote/render papercuts

- `render_still` on an assembly tab returns ok:true and renders only the
  HDRI environment (known papercut, now cost a second session) — error
  out or support assemblies.
- No way to scroll a *panel* remotely: `scroll` is viewport zoom and
  `key_press` has no PageDown/Home/End tokens. Verifying the BOM fix
  required abusing the test engine's hover-scrolls-into-view. Add
  `scroll {window, dy}` or the missing key tokens.
- FIXED in-session: docked panels ran underneath the bottom part-tabs
  bar (dockspace host covered the full work area; the bar was an
  overlay), which made the BOM section unreachable — the view cube's
  manual "sit above the bar" offset was the tell. The dockspace now
  reserves the strip (`PartTabsBarHeight()`).

## Round 7-8 addendum (gangway + ladder, first rounds through the loop)

What worked: rotate{axis,angle_deg} (rotated ladder placed first-try),
set_camera eye (zero inside-geometry shots), decision-complete errors
(each of three gate-caught hinge mistakes was one edit from its
message), body-level contact_report (shipped mid-round after Elias found
the floating mount plate; caught pin-float, pin-clash, and leaf-reach in
succession before any render).

The floating-plate post-mortem, in layers: (1) the occurrence-level gate
structurally could not see an intra-part break — no tool, so nothing was
asserted, so nothing was caught; (2) the "interesting" contact (arm-tube
tangency) got hand-verified while the "obvious" one (arm-plate, 35 mm
apart) got skipped; (3) at render distance a 35 mm depth gap is
invisible and confirmation bias filled it in. The durable fix was the
tool, not a resolution to look harder.

The render_sheet betrayal: bend direction was misjudged from a small
tile ("arches" that were dips) while the add_feature DIGEST already
carried the answer (the bends body's bbox sat below the axis). Measure
before render applies to orientation too.

New items:
- Revolve params row: document the sweep direction convention
  (right-hand rule about the axis from the sketch plane) — the 50/50
  should not exist.
- render_sheet {focus: occurrence|body}: auto-frame one thing large;
  the close-up should be automatic, not a camera dance.
- query_geometry: add "torus" (+ radii) to the face classifier — bends
  currently classify "other" and cannot be asserted.
- check_geometry: warn by default on disconnected multi-body parts
  (make the floating-part class opt-out, not opt-in).
- Convention, documented: intra-part overlaps are welds — body-scope
  contact_report counts them as connecting "clash" edges; never assert
  clashes == [] at body scope on welded-look parts.

## Standing gap (planned elsewhere)

Barrels render white: no blue material, and color-per-slug was rejected —
per-part appearance override designed in docs/rendering/appearance-plan.md
(its acceptance test is recoloring these barrels).

## What worked — keep doing

Regenerable build script as the single master (parameters at top, re-run
per iteration); interference spot-checks + BOM + mass + buoyancy printed
every run; headless build instance + separate Xvfb preview instance;
`describe` for API discovery; `ui_items`/`item_click` to verify GUI fixes
from the same script surface humans use.
