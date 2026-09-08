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
- Threads are native **cosmetic Thread features on manufacturing tap-drill
  geometry**: M6 bores are 5 mm and M3 bores are 2.5 mm. Explicit bounded
  engagement annotations let exact contact checks accept the threaded overlap
  while retaining other collisions. Five named engagement gates also verify
  that the intended connections actually exist. STEP retains the tap-drill bores.
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

## Workflow improvements

The issues found in the first version are addressed in
[the workflow-fixes implementation](clamp-workflow-fixes.md): native unknown-contact
reporting, live datum and projected-curve handles, associative notes and annotation
layout validation, manufacturing thread geometry and thread-derived screw motion.
Machining note expressions now follow live variable edits and persist on save.
The model still needs selected functional tolerances, stock grades and finishes
before a manufacturing release; the geometry checks do not qualify clamp loads.

## Original baseline validation (2026-09-08)

Final standalone run: **0.70 s build, 1.71 s including full checks, drawings,
STEP, BOM and project save**. The dedicated regression passed in 2.05 s.
The full headless suite passed 1149/1150 on its first run; the existing
`cncest6090_smoke` motion check reported not-clean without listed clashes under
parallel load and passed its isolated rerun in 2.41 s. That intermittent result
remains worth investigating; no clamp check was skipped or weakened.
The assembly render and all eight sheet layouts were visually inspected.

`thread_pitch` is source-register data rather than a second editable pitch.
Screw relations now reference their native Thread feature; its standard
specification is authoritative on every solve.

## Updated workflow validation (2026-09-08)

The migrated generator builds in **0.844 s** and completes in **2.790 s**, including
five explicit thread-engagement checks, the 41-pose study and all exports.
All **1154 headless tests**, the focused drawing GUI test and the full **284-test
GUI suite** passed. The assembly
and all eight exported sheets were visually inspected. The extra contact checks
increase total time from the original baseline; no collision gate was weakened.
