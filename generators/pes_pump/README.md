# Bosch PES4M injection pump

The four-element inline injection pump on the OM615 (Mercedes L306D, W115
200 D), as one assembly that moves the way the real one does: the camshaft
lifts four roller tappets and plungers in firing order 1-3-4-2, the control
rack turns every plunger's helix through its control sleeve, the delivery
valves lift, the feed pump's tappet rides its eccentric, the governor
diaphragm rides the rack, the hand primer strokes, and the timing device's
flyweights advance the sprocket. It is a clearance-checked kinematic model;
no dimension is sourced (see below). It is not yet mounted on
`generators/om615/`, whose block carries an `injection_pump_mount` frame for
it.

## Running it

```sh
tools/headless-preview.sh --start <port>
python3 generators/pes_pump/pes_pump.py --port <port> [--out scratch/pes_pump] \
    [--samples 73] [--parts NAME ...] [--no-studies] [--no-still]
```

It refuses to run against an instance with unsaved changes. `--parts` builds
only those parts; `--no-studies` stops after the build. Outputs:
`pes_pump-unchecked.dsgn` (written first, so a failed run leaves something to
inspect), `pes_pump.dsgn` (only once every check passes, left on Full load),
`pes_pump.glb` (the camshaft study), `report.json` and the renders.

## Structure

| file | what |
|---|---|
| `pes_spec.py` | the few numbers an M-size pump is quoted by, the stations derived from them, and the motion laws that BOTH the drawn cams and the assembly's relations are made from |
| `pes_parts.py` | one builder per part: housing (cam gallery, tappet and element bores, rack bore, fuel gallery, inspection window, feed-pump pad, governor flange) and window cover; camshaft (four lobes, feed-pump eccentric, journals, drive taper); tappet, pin and roller; barrel and plunger with its vertical groove and control helix cut in; control sleeve with a full involute tooth ring; spring and seat; toothed rack; delivery valve, seat, spring and holder; feed pump body, tappet and primer; governor housing, cover, diaphragm and spring; timing hub, flyweights, sprocket, nut |
| `pes_pump.py` | assembly, configurations, studies, checks, renders |

Frame: +X along the camshaft (drive end −X, governor +X; element 1 at the
drive end), +Z up every element's axis (camshaft centreline z = 0), +Y across
(rack on +Y, fuel gallery and feed pump on −Y). Per-element parts are drawn on
their own axis in pump heights, so a part's reported position is its travel.
Every drawn cam sits `CAM_ALLOW` (0.05 mm) inside its law.

## How it moves

Two independent drivers, as in the real pump: engine speed turns the
camshaft; the governor positions the rack.

| mate | coupling |
|---|---|
| camshaft — revolute | driver of the run study (one cam turn = two crank turns) |
| tappets — sliders | CAM relation from the camshaft, each element's lobe phased 0 / 270 / 90 / 180° for elements 1–4 |
| rollers — revolute on the tappet pin | gear relation at the base-circle rolling ratio (14/6) |
| control sleeves — revolute | RACK relation from the rack slider at the sleeve's pitch radius |
| plungers — one cylindrical mate each | CAM relation drives the slide, RACK relation the turn, so the plunger turns exactly with the sleeve whose slots its vanes ride in |
| delivery valves — sliders | CAM relation from the camshaft |
| feed-pump tappet — slider | CAM relation on the eccentric |
| control rack — slider | driver of the load study; the diaphragm is fastened to it |
| flyweights — revolutes | coupled to each other 1:1 and to the sprocket's advance |
| hand primer — slider | its own study |

Three assembly configurations hold the rack: **Stop** (0 mm), **Idle**
(4 mm) and **Full load** (15 mm). Each study NAMES the configuration it
measures, whatever is active: the camshaft turn runs under Full load, the
rack sweep under the base (configuration 0), where nothing holds the rack.
Point the camshaft study at another configuration with
`set_motion_study {configuration}`.

The pump relies on three things the API does: revolving and patterning about
a `custom` axis (the element bores), a helix's `start_deg` (the control helix
placed on its port without turning the plunger's frame), and a motion study
that names its configuration.

## What a run checks

From the study poses: each plunger's stroke equals the cam lift (7 mm); the
order the four reach top dead centre is 1-3-4-2; over the rack's 15 mm every
plunger and every sleeve turns travel ÷ pitch radius (96°); every part is
one valid solid; and the exact clash sweep of all four studies is clean. The
rack and sleeve teeth are real involutes, so a wrong relation sign shows as
tooth clash. The effective stroke at each rack setting is computed from the
same helix law the plunger is cut to.

## Where the numbers come from

The references are eight undimensioned images: four Bosch/Mercedes exploded
views (PES M housing, element stack, delivery-valve holder, element), the
Mercedes catalogue plate with the pneumatic governor and timing device, and
one photograph. Every value is an engineering estimate consistent with a
Bosch M-size element pump, stated once in `pes_spec.py`:

| quantity | value |
|---|---|
| element spacing | 25 mm |
| plunger diameter | 6 mm |
| cam lift (plunger stroke) | 7 mm |
| camshaft speed | ½ crank |
| control-rack travel | 15 mm, stop to full load |
| control-sleeve pitch radius | 8.95 mm (module 25/(8π): the element pitch is 8 rack pitches, so every sleeve meshes at the same phase) |
| delivery-valve lift | 1.5 mm |

## Limits

- Delivery-valve timing is fixed at full load: a cam relation reads the
  camshaft only, so the valve cannot also follow the rack.
- Rollers roll at the base-circle ratio everywhere; on the lobe the true
  ratio changes with the contact radius.
- Springs are rigid, drawn at their shortest and carried by one end. The
  governor diaphragm is a rigid plate inside a fixed rubber rim.
- The governor and flyweights are kinematic only: nothing links vacuum or
  engine speed to the rack or the advance.
- The castings are simplified blocks, feet, ribs and bosses, not the real
  pump's cored shapes.
