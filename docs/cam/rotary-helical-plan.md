# A rotary path that follows a twist

## The problem

The rolling pin is a 6-start barley twist, a 45° helix with a 157 mm lead.
Neither rotary pattern fits it:

- **Parallel** passes run along X at one A each, so every pass crosses
  the flutes at 45°. That is where its scallops come from.
- **Spiral** measures its pitch along the surface, and a helical flute is
  steep along the axis at every angle, so every turn densifies: 580 minutes
  against parallel's 87 (docs/cam/dogfood-rotary.md).

A twist wants passes that run ALONG the flutes: helices at the part's own
lead, stepped round from one to the next. In the unrolled (x, arc) plane they
are parallel straight lines at the twist's angle. The parallel pattern is the
special case of an infinite lead, and a surface of revolution is cut as well
by one as by the other.

## The shape

### A third pattern: `helical`

`RotaryHelicalPasses(cl, req)` in `CamSurface`. Each pass runs x from one end
of the map to the other with θ = θ0 + 360·(x − x0) / lead, unwound, reading r
from the cutter-location map exactly as the spiral does (bilinear, wrapping in
θ). A cell the tool reaches nothing in ENDS a pass, as it does for parallel
passes, since a pass is not a single helix that must not break.

- **Spacing** is the op's `stepover` as a fraction of the cutter, measured
  PERPENDICULAR to the passes at the widest radius. With the helix angle φ
  (tan φ = 2π·r_max / lead), the angular step is `stepover / (r_max · cos φ)`.
  Measured along θ alone, a 45° pass would sit 30% closer than asked.
- **Surface-true across passes.** The next step shrinks by `1 / sqrt(1 + s²)`,
  with `s` the steepest slope across the passes (∂r/∂arc) anywhere in the band
  the step can cover, from this pass to one base step round. It is floored at
  a sixth of the base step. This is the rule the spiral uses, applied where it
  pays off: a pass that follows a flute has the same cross-slope all along
  it, so the densifying lands on the walls and nowhere else. The spiral could
  not do that, because one helix has to space every angle alike.
- **Zigzag.** Alternate passes run back, so pass k+1 starts one step round
  from where pass k ended. The link is a radial retract, index and plunge, as
  for parallel passes, and a forward pass winds A by as many turns as the
  return unwinds it, so the axis does not wind up.
- **Decimation** in (x, r, arc), shared with the spiral: a pass that follows a
  flute is nearly straight in that space, so most samples go.

### The lead, derived

A number with a unit is derived from what it acts on (CLAUDE.md, Defaults).
`DeriveRotaryTwist(cl)` returns the dominant twist in degrees per mm, signed
for the hand. For each pair of neighbouring columns that is not round (its
radius varies round the part), it finds the θ shift that best lines one up
with the other, with a sub-cell parabola fit, searching ±16 cells. The answer
is the median over those columns, and 0 when too few columns are not round.
On the pin that should come out at 360 / 157 = 2.29 °/mm.

The op field is `lead_mm`: the axial advance per turn of a pass, signed for
the hand, with 0 meaning derive it. A helical op on a part with no twist
cuts as parallel passes, and says so in the toolpath's `strategy` line rather
than quietly.

### A finish measure that is not the end face

The verdict's one leftover number, the worst cell, sits on the pin's end
face for every pattern, because no rotary pass reaches a face perpendicular
to its axis. It hides the comparison this round is for. The rotary verdict
gains `leftoverP95`, the 95th percentile of leftover over the compared
cells: the typical scallop, which the few end-face cells cannot move. It is
reported by `cam_verify` as `leftover_p95_mm`.

## Files

- `CamJob.h`: `RotaryPattern::Helical`, `Operation::leadMm`.
- `CamNames.{h,cpp}`: the `helical` name, `OpField::Lead`.
- `CamOpFields.cpp`: the `lead_mm` row (the table from df1a4d14 is what makes
  this one line in the file, the RPC and the key).
- `CamSurface.{h,cpp}`: `RotaryHelicalPasses`, `DeriveRotaryTwist`, the
  shared decimation.
- `CamSurfaceStrategies.cpp`: the helical branch and its layers.
- `CamRotary.{h,cpp}`, `CamVerify.h`: `leftoverP95`.
- `RemoteCam.cpp`: `leftover_p95_mm`, and `lead_mm` in the op rows (the
  params gate demands it).
- `CamPanel.cpp`: the lead field. The pattern radio already lists every
  pattern.
- `SerializationInternal.h`, `format.md`: v124 and a golden.
- `generators/rotary/rolling_pin_job.py`: `--helical`.

## Verified by

- The twist is recovered from a synthetic twisted map to within 2%, both
  hands, and a round map gives 0.
- Every sample of a helical pass lies on its helix. Passes are spaced as
  asked perpendicular to themselves, and closer where the cross-slope is
  steep.
- On the test shaft, which has no twist, a helical op cuts as parallel passes
  and says so. With a stated lead it verifies clean, and simulating it gives
  the verifier's stock bit for bit.
- The capability poke, the round-trip tests, the v124 golden, and the GUI
  radio.
- **The pin:** helical against parallel and spiral, as cycle time, program
  size, gouge and `leftover_p95`, with the job, the G-code, a picture and the
  report written to `scratch/rolling_pin_helical/`.

## As built

As planned, with one correction the pin forced. The step to the next pass
first took the cross-slope under the CURRENT pass only. A pass along a flat
land then stepped the full width and put the next pass partway down a flute
wall, leaving ridges up to 0.69 mm on the twist where parallel passes left
0.04. The slope is now taken across the whole band the step can cover, and
the ridges are gone: worst 41 µm with layered roughing, 61 µm (over 0.2 mm²)
in the like-for-like job below. It is the same one-sided-window mistake the
spiral made first.

The pin, like for like (parallel's operations, pattern changed; numbers in
docs/cam/dogfood-rotary.md, files in `scratch/rolling_pin_helical/`): the lead
was measured at 157.3 mm (true 157.1). The finish takes 94 minutes against
parallel's 69, and leaves the same scallop on the twist (p99 27 um for both).
The time is the path length: a pass along a 45-degree helix is sqrt(2) as long.
What helical buys on this pin is tool marks along the flutes and a Z axis that
hardly moves, which a kinematic verifier cannot measure.

