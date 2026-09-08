# Dimensioned toolmaker's clamp

`generators/toolmakers_clamp.py` builds an eight-part mechanism from the
[Journeyman's Workshop drawing, dated 18 August 2014](https://journeymans-workshop.uk/images/projects/toolmkrs/toolmkrs.pdf)
and its [author's construction account](https://journeymans-workshop.uk/toolclamps.php).
The PDF was rendered and checked against the model. Source documents are linked,
not vendored; availability on the web is not a redistribution licence.

The main dimensions are known: 75 × 12 × 12 mm jaws, a 30 mm taper to a 2.5 mm
nose, screw axes 8 and 38 mm from the heel, M6 × 1 studding 75 mm long,
11 × 15 mm knobs and the captive knob's 8.2 mm groove. Project variables retain
those dimensions. `dimension-register.json` distinguishes drawing dimensions,
author-prose dimensions and our explicit design choices. Null tolerances mean
**unspecified**, not zero tolerance or verified manufacturing accuracy.

## Source audit and model decisions

- The captive knob detail appears to show the blind bore entering opposite the
  retaining flange. That conflicts with the assembly's screw entering through
  the flange. This model puts the bore through the flange end and records the
  correction on its sheet. This is an interpreted correction, not an author erratum.
- The author used a salvaged M3 washer-head screw, without its head dimensions.
  Our replacement is explicitly dimensioned: 7 × 0.5 mm retaining rim,
  4.8 × 2 mm boss, 5 mm shank, 0.8 × 1 mm screwdriver slot. It is a custom
  detail, not an asserted DIN/ISO purchased fastener. Spacer dimensions
  (3.5 OD, 3 ID, 1.2 long) come from the author's prose.
- Rod insertion is our 11 mm assembly choice within the 12 mm blind thread.
  Knobs are rigidly attached to their rods, representing the author's retained
  threaded assembly. The baseline jaw opening is our 18 mm setup choice.
- The rear tip follows the drawing's flat 5 mm diameter, 6 mm long end. It
  seats at the cylindrical socket shoulder. The socket has its 118° drill
  point. A matching conical thrust tip, mentioned as an option in the prose,
  is not modelled here.
- Blind depths are interpreted as cylindrical depths plus the 118° drill point.
  M6 and M3 tap-drill points use diameters 5 and 2.5 respectively.
- Threads are native **cosmetic Thread features**. Their B-Reps use major-diameter
  envelopes, including tapped bores, to support assembly checks without false
  thread penetration. Use the M6/M3 and tap-drill callouts for machining;
  **do not machine the exported tapped-hole envelope diameters**.
- Knurl and chamfer remain finish callouts: the source gives no sizes. Steel is
  the chosen generic display/BOM material; brass is used for the spacer.
  No strength, heat-treatment, surface-finish or tolerance certification is implied.

## Run and outputs

Run a private CAD instance, never the human's port 8799:

```sh
nix-shell shell.nix --run 'build/designer --headless --remote 8807'
python3 generators/toolmakers_clamp.py --port 8807 --out /tmp/toolmakers-clamp
```

`--preview` skips the drawing/verification work. The full run writes:

- `toolmakers-clamp.dsgn`: native project with live features, frames, two screw
  relations and two closed assembly loops.
- `toolmakers-clamp.step`: solved assembly geometry.
- `bom.csv`: native eight-row bill of materials.
- Eight SVG part sheets: orthographic/isometric views, associative overall
  dimensions and detailed machining/source callouts.
- `dimension-register.json` and `report.json`: provenance, geometry volumes,
  exact collision results, sampled motion poses, retention margins and timings.

The motion is **parallel adjustment**, not a contact simulation or the clamp's
independent tightening/tilting operation. The jaw slider maintains parallelism;
closed loops and M6 screw relations make both screws rotate as opening changes.
The 41-sample study covers 10 turns (90° increments), opening from 18 to 28 mm.
That checks quarter-turn poses, not just visually identical whole revolutions.

The smoke test additionally imports the STEP export, checks eight solids and
conserved total volume, reopens the native project, and changes jaw length and
opening without rebuilding the generator. It independently calculates both
jaw volumes from stock minus cutters, catching missing holes and misplaced
sketch planes. Standing collisions include fixed parts; the motion study checks
moving pairs. Nominal retention margins are 0.4 mm radial clearance, 1.0 mm
flange overlap and 0.2/0.5 mm axial clearances. These are nominal geometry
checks, not worst-case tolerance stacks or load checks.

## Friction discovered

1. Drawing geometry distinguishes edges from face silhouettes, but callers must
   manually map `kind` to `edge`/`sil_face` and remember endpoint indices 1/2
   (0 means midpoint). An SDK projected-reference object would remove this trap.
2. Datum offsets need explicit persistent datum features. Convenience sketch
   planes can capture a value rather than retain its expression.
3. Cosmetic tapped-hole manufacturing geometry and assembly envelopes need a
   first-class distinction. Today the generator must document the choice.
4. Drawing notes are not checked for overlap with the title block. Visual review
   caught that even though `check_drawing` passed. Notes here are baseline
   manufacturing/source callouts; unlike the driven overall dimensions, they
   do not update after interactive parameter edits. Refresh/review the sheets
   before releasing a modified design.
5. Source discrepancies belong in a dimension/source register. A dimensioned
   drawing is better evidence than a photograph but still needs consistency
   checking, particularly bore entry direction and unspecified fastener heads.

A follow-up manufacturing release would assign functional fits and tolerances,
select stock grades and finish requirements, and review the replacement retaining
screw. This project exercises the digital workflow end to end; it is not a
load-qualified production release.

## Measured validation (2026-09-08)

Final standalone run: **0.70 s build, 1.71 s including full checks, drawings,
STEP, BOM and project save**. The dedicated regression passed in 2.05 s.
The full headless suite passed 1149/1150 on its first run; the existing
`cncest6090_smoke` motion check reported not-clean without listed clashes under
parallel load and passed its isolated rerun in 2.41 s. That intermittent result
remains worth investigating; no clamp check was skipped or weakened.
The assembly render and all eight sheet layouts were visually inspected.

`thread_pitch` is recorded as source data, not exposed as an independent live
variable: M6 coarse cosmetic thread designations and screw relations both use
1 mm pitch. Changing the thread standard requires updating both together.
