# Smart speaker cabinet

A two-way floorstanding loudspeaker, built from one parameter table: a
Visaton W 170 S in an 854 mm reflex chamber, a Markaudio Alpair 7 MS in a
sealed chamber above it, a flared port down the back of the bay under both,
and an ESP32 amplifier and power brick on the bay floor. Six MDF panels, four
3.6 mm plywood coverings, two braces, the port and the dowel and T-nut
hardware are made here; the drivers, amplifier and brick are bought and
arrive as STEP. The run also writes the baffle's and the uprights' CNC jobs
for the 6090 router. It started as a model from another CAD system, ported to
this app from its STEP export, and the generator replaces that port.
`BUILD.md` beside the code is the builder's guide: glue lines, what the model
cannot enforce.

## Running it

```sh
python3 generators/smart_speaker/extract_bought.py [--port N] [--step ~/Downloads/step_files/speaker.step] [--out scratch/smart_speaker/bought]   # once
python3 generators/smart_speaker/smart_speaker.py [--port N] [--out scratch/smart_speaker.lob] \
    [--stage carcass|parts|assembly|cam|all] [--skip-cam] [--no-save] [--deep] [--save-anyway]
python3 generators/smart_speaker/modal.py [--port N] [--mesh 25] [--project P] [--assembly]
```

- `extract_bought.py` slices the original model's 48 MB STEP export into
  small per-part STEPs, once: the driver by its solids' names, the Alpair
  and the brick by the volume and box only they have (a non-unique match is
  refused), the amplifier as its two case halves and board at their assembly
  placements.
  The board's ~200 passives are left out unless `--with-passives`. The
  output lives in `scratch/` and is regenerable, so neither it nor the
  project is tracked.
- `carcass` builds everything that does not need those slices (panels,
  coverings, braces, port, hardware); it is what `tests/generators_smoke.py`
  runs, headless, checking that the dowel holes of joined panels line up.
- `all` (default) adds the bought parts, the assembly, and the CAM jobs,
  posted as `.nc` beside the project. `--skip-cam` leaves the jobs out.
- The run refuses to save what failed its checks; `--save-anyway` saves it
  to look at, marked untrustworthy. `--deep` compares against the reference
  STEPs by shape (IoU) instead of size and census.
- `modal.py` runs panel-mode studies by hand; it takes minutes.

## Structure

| file | what |
|---|---|
| `params.py` | the one parameter table, declared at PROJECT scope in order so a derived entry may name any above it; the frame; Helmholtz tuning |
| `walls.py` | side, front (baffle), back, top, bottom walls and the divider; `silhouette()`, which the coverings reuse; placements |
| `joinery.py` | every dowelled joint from one rule, and the frames each panel carries for it |
| `veneer.py` | the plywood coverings as their own parts |
| `braces.py` | the port-ring brace and the window-brace ladder |
| `port.py` | the port: tube and two flares, revolved |
| `hardware.py` | T-nuts and dowels |
| `bought.py` | imports the sliced STEPs |
| `mates.py`, `seating.py` | the assembly's mates |
| `cam.py`, `cam_back.py` | the baffle's front job; the back of both uprights |
| `verify.py` | what the run refuses to save without |
| `modal.py` | panel modes |
| `smart_speaker.py` | the run |

**Frame.** World Z up, z = 0 the top of the MDF carcass, so the cabinet hangs
into negative z — the ported document's frame, which the CAM job and the
assembly are both written in.

**One table.** The port carried its parameters six times, a part-scoped copy
in every wall, and they had drifted; here each is declared once at project
scope. Driver heights are derived from the floor and the top
(`w170s_center_from_top`, `alpair_center_from_top`), bolt rings are a count
and a diameter looped over, and the W170S chamber height is what is left
after the Alpair chamber and the port bay. Existing expressions
(`wall_thickness`, `plug_length - wall_thickness/2`, …) are written as
expressions.

**The joint is the unit.** Both panels of a dowelled joint ask `joinery.py`
where their holes go, so the holes meet; each joint also names the frames
built from its two bores, and the carcass is fastened along a TREE of those
joints from the left side wall rather than placed at coordinates.

**Coverings** are separate parts (they are cut and glued on, and belong in
the BOM). Each carries a `Derived` reference copy of its wall — a ghost,
absent from mass, BOM, export, CAM and booleans — so it reports `stale` when
its wall moves; its outline comes from the same `walls.silhouette()`, and it
is chamfered 45° with a setback of its own thickness where it meets another
covering, which is a mitre.

**Braces** are read off the reference export's own mid-plane sections and
re-expressed in parameters: the rail, spokes and ring wall are one
`wall_thickness`. The **port** is revolved rather than imported, because its
length and diameter are the tuning knobs; the run reports the Helmholtz
frequency they give, counting what stands in the chamber from the assembly.

**Mates.** A frame on ONE face has no meaningful X, and a fastened mate on it
has eight outcomes the solver would pick between arbitrarily. `asm.join`
(`add_mate derive: "twist"`) gives an exact joint turned the way the part is
already placed, and refuses when the two anchors are not square to one axis.
What the generator keeps is the assertion — the amplifier is above the
floor, inside the bay, lengthways (`_check_laid_down`). The amplifier bolts
through the four holes its own case is bored with, and the floor is drilled
to that pattern; the mate refuses if the case stops matching it.

**CAM.** `cam.py` rebuilds the baffle job: one setup, two tools, fourteen
operations, ten of them screw holes written from the bolt counts. Targets are
found by GEOMETRY (the cylinder at this radius at this centre), never by
element strings, which are valid only for the feature order that made them.
`cam_back.py` cuts the divider dado and the face dowels on the back of both
uprights in two setups each, because the dowels 1006 mm apart do not fit the
900 mm travel; the deep dowels into the panels' end faces are a jig's work.

## What the run checks

1. It built: no failed feature, every part a valid B-Rep.
2. `asm.check(allow=…)`: nothing clashing, nothing floating (one connected
   component — a part in a cavity clashes with nothing, so an empty clash
   sweep does not say it is held), no near miss, with glued contacts named
   in `allow` by pair and volume.
3. The parameters drive it: the document is rebuilt at `wall_thickness`
   16 → 18, `cabinet_internal_w` 210 → 230 and `port_length_total`
   250 → 300, and any part whose volume and bounding box did not change is a
   literal that survived, and fails the run.
4. It is the same cabinet: volumes against the ported document's panels and
   the reference STEP's port and braces.
5. The port tunes where it should.

## Modal studies

`modal.py` clamps each panel on the faces it is glued in by and reports its
six lowest modes — a comparison between variants of one idealisation, not a
prediction: clamped against simply supported edges is a factor of two on the
baffle, and a glued butt joint is between. Both panels' fundamentals are set
by their SHORT span, which the horizontal braces do not subdivide; the lever
is a vertical stiffener at mid-depth, which the ladder's studs are.
`--assembly` studies the side wall in its assembly, its neighbours as rigid
supports clamped where they touch, with and without the braces. It needs a
build with FEM.
