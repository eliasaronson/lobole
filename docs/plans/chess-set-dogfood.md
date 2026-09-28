# Chess set dogfood: what is left

The set is designed and every program is generated (`scratch/chess/`,
`run_all.sh PORT` rebuilds all of it from nothing): one `piece` part with a
configuration per rank, a Derived part per rank and wood, four rotary blanks,
the board, the base jig and the assembled set. What remains is the machine,
one design change the machining asked for, and the app work the round found
but did not do.

## At the machine

1. **Step 0: the rotary as installed.** Jog A +90 and watch which way a mark
   on the chuck's top moves; read the axis letter and steps/degree from
   Mach3's motor tuning; measure the chuck's diameter, its grip, and the
   chuck-to-centre length. They go into `MACHINE` in `scratch/chess/cam.py`
   (`rotary`, `holding`, `a_max_deg_min`, and `rapid_feed`, whose 1500 mm/min
   is a placeholder every feed check reads). Until then no rotary program is
   for the machine.
2. **The pawn blank in cheap wood**, rough and finish only (`-T1`, `-T2`):
   the first real rotary cut, and the check that the mount is right.
   Measure the first pawn with calipers against the drawing.
3. The rest in the plan's order: rook+bishop, queen+king, board, jig and
   undersides, knights last.

## The design change the machining asked for

Every blank's rest pass (`-T7`, the 1.5 mm ball) is about seven hours: a
Staunton profile is inside corners end to end, and each is a ring the 3 mm
finishing ball cannot reach. `profiles.py` rounds the concave line-to-line
corners to 1.6 mm, but the arc-to-line ones (bead, cove, ball necks) and the
tight ones it lists are still sharp. Give every inside corner at least 1.6 mm
and every groove at least 3.4 mm, and the finish alone makes the part.

## App work found and not done

- **A pocket open on a side is cleared as if walled.** The queen's crown
  notch floor runs 2.3 mm along the axis before the dome falls away; the
  pocket refused a 3 mm cutter by that width. Edges where the floor meets
  air, not a wall, should let the tool pass.
- **Text.** No text sketch entity, no engrave operation: the board's a-h / 1-8
  are not on it.
- **Carry the stock into the playback and the rotary check** as the planar
  check now does (`SeedFromEarlier`), and derive an indexed operation's
  `top_z` from the seeded stock (the crenel and cross ops still warn that
  their entry rapids pass through a bar the turning removed).
- **An arc that ends exactly on a revolve's axis** can cross it by float
  noise at one size in several (`profiles.TOP_R` works round it).
- **A blind slot's floor measured 1 mm across** where the model says 3.2 (the
  bishop's first mitre; redesigned as a notch, not diagnosed).
- **A spiral rest pass** winds the whole length at the rotary's speed limit,
  air included, and its `holding_clipped_mm` reads as nearly all of it.
- **Sixteen pieces in one undersides job**, each clamped face up per
  placement, derived its depths from the wrong end; retest now that
  `FacePlane` is fixed, and fold the per-rank jobs back into one.

## Verification

The pipeline's own: every part rebuilds at king_h 70-110, the set's BOM is
32 pieces and a board, the drawing checks clean, and each job's `cam_verify`
is read, not assumed (`scratch/chess/chess_report.json`). At the machine:
calipers on the first pawn.
