# 5-inch FPV quadcopter

A 220 mm 5-inch freestyle quad, 6S, analog video, modelled to be built: five
carbon parts to have cut, two to print, every screw, nut and standoff derived
from the hole it goes in, and every bought component at its catalogue
dimensions so the clearances mean something. The run writes dimensioned
sheets with hole tables, STEP, STL, a BOM, a mass budget, load cases on the
parts that carry a load, and G-code to cut the carbon.

## Running it

```sh
python3 generators/fpv_drone/fpv_drone.py --port <n> [--out scratch/fpv-drone] \
    [--stage parts|assembly|all|drawings|shop|fem|cam]
```

- `parts`, `assembly` — build up to that point.
- `all` (default) — build, verify, save `fpv-drone.lob`, then the outputs,
  the FEM studies and the CAM jobs, and save again.
- `drawings` — reopen the saved project and redo the sheets and exports only.
- `shop`, `fem`, `cam` — reopen the saved project and add the load studies
  and/or the cutting jobs. They add STUDIES and JOB DOCUMENTS and touch no
  part, and the project is saved with them: the nesting, tools, depths and
  load cases are what somebody opens to change one, and a `.nc` file cannot
  be edited back into a job.

Outputs in `--out`: the project; five part sheets and a general arrangement
(SVG + PDF) with hole tables read off the model; `fpv-drone.step` and a STEP
per carbon part; STL for the printed parts; `bom.csv`; `mass-budget.csv`;
`views.png` and `render.png`; `fem.csv` and a plot per load case; and
`<sheet>-T<n>.nc`, one G-code file per cutter. The build guide below is copied
beside them as `BUILD.md`.

## Structure

| file | what |
|---|---|
| `fpv_drone_spec.py` | every dimension, in three kinds kept apart: CLASS (what a 5" freestyle quad is: 220 mm wheelbase, 5.1×4.3×3 props, 2207 motors, 6S), CATALOGUE (bought parts' mounting dimensions: the 30.5 mm stack, 16×16 motor pattern, 19 mm micro camera, 20×20 VTX), DESIGN (this frame's choices). The layout is derived; `declare(app)` makes every dimension a project variable |
| `fpv_drone_parts.py` | one builder per part, each leaving named frames to mate to |
| `fpv_drone.py` | assembly, fasteners, mates, declared clearances, `verify`, sheets, exports |
| `fpv_drone_shop.py` | the FEM load cases and the CAM jobs |

Bought parts are envelopes carrying their real MOUNTING features: a motor has
its 16×16 tapped holes at depth, the boards the 30.5 stack pattern, the VTX
its 20×20, the camera its M2 tilt taps. Nothing internal is modelled. Carbon
is `carbon_fibre` (1550 kg/m³).

The layout follows from the wheelbase, the propeller and the pack: 15 mm
standoffs put the top plate under the propeller disc, which is why the camera
looks out through a notch in it; the battery sits between the props with its
lead out the back; the camera cage drops straight down through slots in the
mid plate and is locked by two M2 screws into a 19.5 mm tapped spacer; the
antenna is on the centreline; the arm roots stop at r = 30 to clear the stack
and the VTX nuts.

## What the run asserts

`verify` refuses to save anything it has not checked:

- every part is one valid solid;
- the exact interference sweep over the whole assembly is empty;
- `check_fasteners` re-derives every screw against its stack and reports
  nothing;
- seven clearances DECLARED on the assembly (`list_assertions` shows them;
  moving a plate makes them stale): prop to prop adjacent and across, prop to
  top plate, prop to battery, prop to antenna, prop to camera plate, battery
  to camera, each at least 2 mm. Mesh-measured clearances subtract the SUM of
  the two tessellation deflections, so the figure is a lower bound;
- a motion study turns all four props through a blade pitch (the four
  revolutes are coupled by relations, so one driver turns them all) with no
  new contact;
- the centre of mass, re-weighed from catalogue masses (a battery and four
  motors are envelopes), within 15 mm fore-and-aft and 5 mm across of the
  middle of the propeller square.

## Loads and cutting

`fpv_drone_shop.py`, against the saved project.

**FEM** — static studies on the arm at full thrust (one 2207 on 6S, about
1.5 kgf), the arm on a 15 g one-corner landing, and the top plate under the
pack at 15 g. Landing, not thrust, designs a 5 mm arm. Results go to `fem.csv`
and a plot each, because a solved result is not persisted in the project.
Read them as indicative: the solver is isotropic and a carbon laminate is
not — a real arm fails by delamination at the root, which no isotropic model
sees. The caveat is printed and written as the last CSV row.

**CAM** — one job per sheet thickness (3 mm bottom plate; 2 mm mid, top and
both camera plates; 5 mm four arms), nested on a 320 × 320 sheet for a
600 × 400 grbl router with four tools: T1 3.175 mm diamond-cut, T2 3.2 mm
drill (M3), T3 2.2 mm drill (M2), T4 2 mm diamond-cut (the 2.2 mm camera
slot is the narrowest milled cut). Drill, then open up, then cut free on 4 × 0.6 mm
tabs; with one program per tool the tools run in the order they are first
wanted, so the outline is the last thing asked of the big cutter. The camera
plates are modelled upright and clamped face up (`lay_face_up`). Nesting is
read back from `check_layout`, and each part's openings are cut from the
model's own faces (`drilled_bores` leaves the round ones to the drills).
Every sheet must pass `cam_verify` (`ok`: no gouge, no rapid through
material). The machine definition is embedded in each job.

## Building the aircraft

The sheets are the authority for dimensions; this is what to order and the
order to put it together in.

**Cut** (SVG or STEP to a cutting service, or the `.nc` files; cut carbon wet
or with extraction, and wear a mask): bottom plate 3 mm ×1, mid plate 2 mm
×1, top plate 2 mm ×1, arm 5 mm ×4, camera plate 2 mm ×2 (one part, not a
mirrored pair). For the router: X0 Y0 at the sheet's near corner, Z0 on its
top face, and run each sheet's files in the order **T2, T3, T4, T1**. Part the
tabs with a knife; catch the slug each arm's shaft bore drops.

**Print** in TPU 95A, 0.2 mm layers, 4 perimeters: antenna mount ×1, battery
pad ×1.

**Buy** — 4 × 2207 1750 KV motors (M5 shaft, 16×16 M3 mounting, 5 mm of
usable thread in the base); 2 CW + 2 CCW 5.1×4.3×3 props and M5 nuts; a 55 A
6S 4-in-1 ESC and an F7 flight controller, both 30.5 × 30.5 on 36 × 36
boards; a 1000 µF 35 V low-ESR capacitor; a 6S 1300 mAh pack (72 × 36 × 42
max, XT60); a 19 mm micro camera with M2 side mounting; a 5.8 GHz VTX on
20 × 20 (25 × 25 board); an RHCP SMA antenna; an ELRS receiver; a 5 V buzzer.
The hardware is in `bom.csv`: socket caps M3 × 16, 14, 10, 8, 25 and M2 × 12,
8, 6, nylocs, M3 × 15 aluminium standoffs, nylon stack and VTX spacers, the
M2 × 19.5 tapped spacer (cut it to 19.5 — it sets the camera cage's width) and
a 20 mm battery strap.

**Assemble**, top-down:

1. Arms between bottom and mid plate: M3 × 16 up through each outer hole into
   a standoff, M3 × 14 through each inner hole with a nyloc. Square the
   diagonals to 220 mm motor to motor, then tighten opposite corners first.
2. Motors: four M3 × 10 each through the arm pad.
3. Stack: M3 × 25 up from under the bottom plate, 4 mm spacer, ESC, 8 mm
   spacer, flight controller, nyloc; it stands through the mid plate's
   opening without touching it.
4. Solder motors, capacitor (short legs) and the XT60 lead out the back.
5. Camera cage: each plate through its slot until its shoulder rests on the
   mid plate and its foot on the bottom plate; camera on the two M2 × 6; M2 × 8
   lock screws through both plates into the 19.5 mm spacer.
6. Top plate on the standoffs, four M3 × 8; its notch clears the camera only
   if the cage is fully down.
7. VTX on the mid plate's tail on M2 × 5 spacers with M2 × 12 and nylocs;
   antenna mount beside it; receiver and buzzer beside the stack.
8. Battery pad on the top plate, strap through its slots.

Props: front-left and rear-right CW, front-right and rear-left CCW; "front"
is where the camera looks. The camera is drawn at 20° tilt. Different props,
a longer pack or a moved antenna change the clearances above — re-run the
generator's checks.
