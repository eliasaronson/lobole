# 3D roughing, cleaning up what is left, and part of a rotary part

## The problem

The verbs now cover finishing in 3D and 4D (docs/cam/surface-verbs-plan.md),
and a shop's cycle for a curved part is rough, finish, clean up. Three gaps
remain:

- **Nothing roughs a 3D part.** Finish a surface tells the user to rough
  first with Clear a pocket, which accepts only a flat floor, and a domed or
  sculpted part has none. The 3-axis surface ops have no layers; only the
  rotary one reads a stepdown.
- **Nothing cleans up.** A ball leaves material in concave corners smaller
  than itself (the rolling pin's shoulders kept 1.26 mm), and the verifier
  says so. But no operation re-cuts only there with a smaller cutter.
- **A rotary pass is all or nothing along the axis.** Finish on the rotary
  refuses a face pick, so finishing one journal, or the handle alone, means
  finishing the whole part.

## The shape

**1. Rough a 3D part: a raster in layers.** A `SurfaceParallel` op with a
`stepdown` cuts in layers, as the rotary op already does:

- **Floors:** from the stock top down by the stepdown to the lowest point of
  the surface. Each layer is the cutter-location surface, plus the stock to
  leave, held up at that floor. The last layer is the surface itself.
- **No air cutting:** a cell with nothing left above its floor (the surface
  already stands above the previous floor) is marked unreached for that layer,
  so the pass breaks there instead of dragging the tool through cut air. That
  is the difference between this and a naive raster in layers.
- **The region is the part's outline plus a ring one roughing cutter wide.**
  Inside the ring, a cell the model does not cover is cut to the op's bottom
  instead of being skipped. The finishing ball can then reach the part's
  edges, and the rest of the block stays to hold the part.
- **Verb:** "Rough a surface", no pick (`job.surface_rough`, Shift+S). It is
  derived by `cam::DeriveSurfaceRough`: the whole-part finish's derivation,
  then a stepdown of half the cutter and a stock to leave of
  `DefaultFinishStock(tool)`.
- **Format (v125):** the derivation sets a stepdown on every op
  (`CamDerive.cpp`), so raster finishes saved before this carry one, and
  reading it as layers would turn every saved finish into a rough. The finish
  derivations now set 0, and a file older than v125 drops `stepdown` on
  `SurfaceParallel` when it loads. The rotary op needed the same move at v123.
  `SurfaceParallel`'s capability row gains `Stepdown`.

This is the cheap rough. Waterline roughing (clear each level with the pocket
or adaptive clearing between the part's section and the stock) is the
efficient one. It is a larger piece of work, and it is worth doing if this one
proves too slow on real parts.

**2. Clean up what is left: rest machining by a reference tool.** A new op
field, `restFromTool`, holds the number of the larger cutter that went
before. When it is set, a surface op (3-axis raster or rotary) cuts only
where that cutter could not reach:

- **Where to cut:** where the larger tool LEAVES measurably more than the
  smaller one would. Tips are no measure: on any slope a big ball's tip rides
  higher than a small one's while leaving the same surface. So both
  cutter-location surfaces are computed, and each tool's swept envelope (the
  lowest point of its underside over every cell) is taken from its own. Cells
  where the envelopes agree within a tolerance are marked unreached, so passes
  break and lift over them. The same construction works on the rotary with
  radius for height and arc length across the axis.
- **Why geometry, not the verdict:** that is the classic rest-machining
  construction, and it does not depend on the verdict, which may be stale.
  The verdict then confirms the leftover went.
- **Verb:** "Clean up what's left", no pick (`job.rest`, Shift+E). It is
  derived by `cam::DeriveRest`. It finds the job's last surface finish (3-axis
  or rotary) and makes the same kind of op with the verb's tool and
  `restFromTool` set to that finish's tool. It is refused when there is no
  finish to follow, or when the chosen tool is not smaller than that finish's.
- **Format (v125):** a new row in the op-field table, so serialization, the
  RPC, the describe row and the surface cache key all follow.

**3. Part of a rotary part.** Finish on the rotary and Rough on the rotary
take an optional face pick. With one, the pass is limited along the axis to
that face's extent, by marking cells outside it unreached. Without one, the
whole part is cut, as now. The derivation stops refusing a pick. The target is
already a stored field, so there is no format change.

## Files

- `src/core/cam/CamSurfaceStrategies.cpp`: layers, the ring and the air mask
  on the raster; the rest mask on both; the axial window on the rotary.
- `src/core/cam/CamDerive.cpp`, `CamToolpath.h`: finish stepdown 0,
  `DeriveSurfaceRough`, `DeriveRest`, the rotary pick.
- `src/core/cam/CamJob.h`, `CamNames.cpp`, `CamOpFields.cpp`, `CamNames.h`:
  `restFromTool`, the capability rows.
- `src/core/io/Serialization.cpp`, `SerializationInternal.h`: v125, the
  stepdown drop; a golden at v125; docs/versioning/format.md.
- `src/remote/RemoteCam.cpp`: the describe row for `rest_from_tool`.
- `src/ui/AppState.h`, `CamTool.cpp`, `Commands.cpp`, `Toolbar.cpp`,
  `core/io/Keymap.cpp`: two verbs; the rotary verbs take a pick.
- Tests:
  - the rough cuts a domed part in layers, does not gouge, leaves its skin,
    and clears the ring;
  - a v124 raster finish still loads as a finish;
  - the rest op cuts only in the corners a larger ball left, on a dome on a
    plate and on the necked shaft, and the verdict's leftover drops;
  - a picked journal limits a rotary pass to it;
  - GUI tests for both verbs and for the rotary pick.

## Verified by

The three suites, the CAM-off build, the golden gate, and the tests above.

## As built

As planned, with four things the tests found.

- **Interpolation undercut masked cells and cliffs.** The raster sampled
  the cutter-location surface bilinearly and ended a pass only where the
  blend reached the floor. A cell masked on the part (a layer's cut-already
  cells, a rest pass's reached-already ones) sits at the floor beside real
  cells, and the blend between them dipped the ball 15 mm into a dome. And
  where the surface falls off a vertical edge within one cell, the blend
  undercut the cliff: a 6 mm ball plunged 2.7 mm from a plate's corner edge,
  1.1 mm into it. `BilinearReached` now ends a pass at any sample with an
  unreached cell among its four, and takes the highest of the four where they
  differ by more than two cells' rise (about 63 degrees): on flanks that
  steep it leaves stock rather than gouging. A plain finish benefits too: a
  finish over a part with a tall step had the same latent dip at the step's
  top edge.
- **The rough's floor is a real floor.** The ring is cut by giving the drop
  cutter the op's bottom as its floor, so it returns "the part on a floor"
  with the ball rolling off the edge continuously, rather than substituting
  the bottom into cells marked unreached (a step beside every edge).
- **Leftover is only counted where the part is.** Off the part both
  envelopes are tool undersides over air, and a big ball rolling off an
  outside edge differs from a small one's there, which put rest passes round
  the plate's rim. A point probe samples the part's own surface (the model's
  radius on the rotary), and a cell is leftover only where the big tool
  leaves more than both the small tool and the part.
- **The envelopes are coarse.** On the small ball's finishing grid the big
  ball's envelope was a minute on a 30 mm shaft. They are taken over blocks
  a third of the small ball's radius across, conservative in the direction
  that only adds cutting (the big tool's tip the highest in a block, the small
  tool's and the part's the lowest), and the mask is mapped back: 59 s to
  14 s on the shaft test.

The rotary verbs' pick is a button in their window, "Limit to a face", which
arms the surface pick and re-derives the verb's operation with that face as
its target; "Whole length" clears it. A clean-up after a waterline finish is
a raster: the rest region, not the pattern, is the point, so the 3-axis rest
code is the raster's. Keys: Rough a surface Shift+S, Clean up what's left
Shift+E.

Tests:
- `tests/test_cam_rough_rest.cpp`:
  - the rough in layers, then the finish;
  - the 3D rest, whose leftover goes from 1.79 to 0.52 mm on a 2 mm ball;
  - the rotary rest, whose leftover at the shoulders drops;
  - the picked-face axial bounds;
- the raster migration test in `test_cam_spiral.cpp`;
- the v125 golden;
- GUI tests `cam/rough_finish_and_clean_up_from_the_toolbar` and
  `cam/a_rotary_pass_limits_to_a_face`.
