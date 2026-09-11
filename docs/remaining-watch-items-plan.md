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
4. **`render_ao_smoke`.** It fails at 1.004/255 against a 1.0 limit, and
   the change that broke it is the lens narrowing from 45 to 30 degrees: the
   test's fixed camera kept its standoff, the walls filled the frame, and the
   half-resolution upsample straddled far more depth edges than in any real
   view. The camera stands off 804 mm, which frames the scene as before.
5. **The watch's run time.** 80% of a full run is the per-mesh exact sweep:
   nine samples through a pitch, each an exact interference of two WHOLE arbor
   parts, and the interference RPC cannot be narrowed to one body (its fast
   mesh mode reads the gears' 3 µm backlash as contact). A mesh's result
   depends only on the tooth geometry and phases — the generator's own files
   — and the kernel that swept it — the app's source — so a CLEAN result is
   kept, keyed on both, and a run where neither changed does not sweep the
   meshes again. `--recheck` forces it; an instance whose binary was rebuilt
   under it never uses the cache.
6. **The watch uses the app fixes.** Crown out becomes a CONFIGURATION (the
   clutch's relations and the stem's pull held), the clutch slides by a
   relation from the yoke, the contrate teeth are one tooth and a Pattern, and
   the build no longer drives the stem to make its relations apply.
7. **A mainspring**, drawn in the barrel (a cosmetic spiral, as the
   hairspring is), coiled let-down in a hollow drum.
8. **A GUI test for the assembly configuration panel**: suppress a mate and
   hold a DOF by clicking, and see the solve follow.

## Larger — scoped for the next round

Built since: the rotating cam follower that animates the escapement
([rotating-cam-follower-plan.md](rotating-cam-follower-plan.md)), and the
freewheel that lets the crown wind without turning the rotor
([freewheel-plan.md](freewheel-plan.md)), and the reverser that winds
whichever way the rotor swings ([reverser-plan.md](reverser-plan.md)).

- **Sawtooth dogs, drawn.** The clutch rides out over its dogs and the
  rotor no longer turns the crown, but the dogs are square and the lift is
  the whole of it rather than part way up a ramp. A real Breguet tooth ramps,
  and drawing one takes a swept helical tooth.
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
