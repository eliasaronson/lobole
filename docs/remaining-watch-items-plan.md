# What the watch left — the remaining items

The watch round (docs/modeling/watch.md, docs/watch-app-fixes-plan.md) closed
with a list of limits and loose ends. This plans all of them, smallest first:
the small ones are done in this round, the larger ones are scoped here for
the next.

## Small — this round

1. **`list_mates` says what a configuration holds.** While a configuration
   holds a mate's DOF, `list_mates` reports the mate's AUTHORED value next to
   a part placed at the held one. Each mate gains `held` (the held rotation /
   offset, with `rotation_deg`) and `suppressed` when the active configuration
   takes it out, so a caller sees the value the solve used.
2. **Glass in section view.** A cut glass body shows an OPAQUE cap in both
   renderers: the raster cap is a plain mesh and the still's cap material has
   transmission 0. The cap takes its body's glass-ness — see-through in the
   viewport at the body's alpha, transmissive in the still.
3. **Glass glare in the viewport.** The ghost pass multiplies everything a
   glass fragment shows by its low alpha, so a pane's highlights and
   reflections are dimmed with its body and it reads as a flat veil. A glass
   instance is marked (the sign of its alpha: the push-constant block is at
   its 128-byte limit) and shaded as glass — Fresnel-weighted reflection of
   the environment that rises toward grazing angles, and the lights'
   highlights at full strength over the see-through body. The still already
   has both.
4. **`render_ao_smoke`.** It fails at 1.004/255 against a 1.0 limit and did
   before this work. Find whether that is a real change in the AO upsample or
   a limit set on one GPU, and fix whichever it is.
5. **The watch's run time.** 80% of a full run is the per-mesh exact sweep:
   nine samples through a pitch, each an exact interference of two WHOLE arbor
   parts. Check the toothed bodies only where the parts allow it, and fewer
   samples where the tooth count makes a pitch short.
6. **The watch uses the app fixes.** Crown out becomes a CONFIGURATION (the
   clutch's relations and the stem's pull held), the clutch slides by a
   relation from the yoke, the contrate teeth are one tooth and a Pattern, and
   the build no longer drives the stem to make its relations apply.
7. **A mainspring**, drawn in the barrel (a cosmetic spiral, as the
   hairspring is), and **Breguet teeth** on the clutch and winding pinion —
   sawtooth dogs that drive one way and slip the other, in place of square
   ones.
8. **A GUI test for the assembly configuration panel**: suppress a mate and
   hold a DOF by clicking, and see the solve follow.

## Larger — scoped for the next round

- **A one-way relation** (a freewheel): a relation that drives in one
  direction and slips in the other. Hand-winding then leaves the rotor alone,
  and the rotor's reverser can be modelled. About half a day in the solver
  (it needs the previous value of the driving DOF).
- **A rotating cam follower**, so the escapement animates: the lift table in
  degrees driving a revolute. Half a day to a day, a format bump; an
  approximation still, since in a real watch the balance drives the escape
  wheel.
- **A thinner movement** (~5 mm, not ~8): re-stack every depth. A day,
  mostly verification.
- **A per-body transmission** override in `Appearance` (tinted glass that is
  not the `glass` material): a format bump.
- **Viewport glass, fully:** sorted panes (overlapping glass blends in draw
  order), and glass that does not pick up the AO of what is behind it.
- **A benchmark sweep** after the MCP surface changed — it costs money, so it
  is the user's call.

Not planned: an anti-reflection coating on the crystal (a thin-film model for
one pane).
