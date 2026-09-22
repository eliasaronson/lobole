# Bosch PES4M injection pump — what was built and what it measures

`generators/pes_pump/` builds the four-element inline pump on the OM615
(Mercedes L306D, W115 200 D) as one assembly: 29 parts, 67 occurrences.
The plan, and why each motion is coupled the way it is:
[pes-pump-plan.md](pes-pump-plan.md).

```sh
tools/headless-preview.sh --start <port>
python3 generators/pes_pump/pes_pump.py --port <port>   # -> scratch/pes_pump/
```

Output: `pes_pump.dsgn` (saved only once every check passes;
`pes_pump-unchecked.dsgn` is written first so a failed run still leaves
something to inspect), `pes_pump.glb` (the camshaft study, 73 frames),
`report.json`, and the renders.

## What moves, and what drives it

| study | driver | what follows |
|---|---|---|
| camshaft, one turn | camshaft revolute, 0-360 deg | 4 tappets + plungers (cam relations), 4 rollers (rolling ratio), 4 delivery valves, feed-pump tappet, timing hub and flyweights |
| control rack, stop to full load | rack slider, 0-15 mm | 4 control sleeves and 4 plungers turn (rack relations), governor diaphragm |
| timing device, full advance | flyweight A, 0 to -20 deg | flyweight B, sprocket advances 8 deg against the hub |
| hand primer stroke | primer slider, 0-15 mm | — |

The rack is held by three assembly configurations — **Stop** (0 mm),
**Idle** (4 mm) and **Full load** (15 mm) — so the camshaft study can be
run at any load setting. The saved document is left on Full load.

## Measured (from the study poses, not the inputs)

| check | result |
|---|---|
| plunger and tappet stroke, all four | 7.000 mm (cam lift 7) |
| firing order, read off the plunger peaks | 1-3-4-2, at 90 / 0 / 180 / 270 cam deg for elements 1 / 2 / 3 / 4 |
| feed-pump stroke | 5.000 mm (2 x eccentricity) |
| plunger AND sleeve turn over the rack travel | 96.000 deg on all four (15 / 8.95 rad) |
| effective stroke at Stop / Idle / Full load | 0 / 0.64 / 2.40 mm (from the helix law the plunger is cut to) |
| exact-boolean clash sweep, all four studies | CLEAN: 5896 + 637 + 77 + 12 pair checks |
| every part | one valid solid |

Delivery-valve lift reads 1.45 of 1.5 mm: the valve is open for only ~20
cam degrees, and the study samples every 5, so no sample lands on the peak.

A full run takes about a minute on this machine: 4 s to build, 38 s for
the exact camshaft sweep, 13 s for the rack.

## What the checks caught

- The feed-pump eccentric was drawn at exactly its law radius, and its
  roller touched it at ~1e-8 mm^3 at four angles. Every drawn cam now sits
  `CAM_ALLOW` (0.05 mm) inside its law, as the lobes already did.
- The rack's drive end was 16 mm too long: from 5 mm of travel onward it
  ran into its own end cap. The rack sweep found it; the pump at rest was
  clean.
- The feed-pump body's flange was added after its piston bore and filled
  it back in. The standing exact check found it (924 mm^3).

## Limits

- **No dimension is sourced.** The eight reference images are exploded
  views and one photograph; the numbers are M-size-pump estimates held in
  `pes_spec.py`.
- **Delivery-valve timing is fixed at full load.** A cam relation reads the
  camshaft only, so the valve opens at port closure and reseats at the
  full-load spill point whatever the rack does.
- **The rollers roll at the base-circle ratio** (14/6) everywhere; on the
  lobe the true ratio changes with the contact radius.
- **Springs are rigid**, drawn at their shortest and carried by one end.
  The governor diaphragm is a rigid plate on the rack inside a fixed rubber
  rim; the membrane between them is not modelled.
- **The governor is kinematic only.** Nothing links vacuum or speed to the
  rack; the rack is positioned by configuration or by its study. The same
  goes for the flyweights and engine speed.
- **The castings are simplified**: rounded blocks, a foot, ribs and bosses,
  not the real pump's cored shapes. The pump is not yet mounted on
  `generators/om615/`, whose block already carries an
  `injection_pump_mount` frame for it.
