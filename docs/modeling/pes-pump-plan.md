# Bosch PES4M injection pump (OM615 / L306D) — plan

## The problem

The OM615 generator (`generators/om615/`) has a half-speed "injection timer"
relation but no pump behind it. The user's L306D carries a Bosch PES4M
inline pump, and they want a model of it whose mechanism MOVES the way the
real one does: the camshaft lifting four plungers in firing order, the
control rack turning the plungers to meter fuel, and the delivery valves,
feed pump and governor following.

## Sources and their limits

The references are the user's eight images: four Bosch/Mercedes exploded
views (PES M housing, element stack, delivery valve holder, element
assembly), the Mercedes catalogue plate with the pneumatic governor and
timing device, and one photograph of a complete pump. None is dimensioned.
Everything below is an engineering estimate consistent with a Bosch
**M-size** element pump, stated once in `pes_spec.py`:

| quantity | value | why |
|---|---:|---|
| element spacing | 25 mm | M-size pump |
| plunger diameter | 6 mm | M-size, 55 mm^3/stroke class |
| cam lift (plunger stroke) | 7 mm | M-size |
| camshaft speed | 1/2 crank | four-stroke |
| lobe phase, cyl 1..4 | 0, 270, 90, 180 deg cam | firing order 1-3-4-2 at 90 deg pump intervals |
| control-rack travel | 15 mm | stop to full load |
| control-sleeve pitch radius | 8.95 mm | module 25/(8 pi): the element pitch is 8 rack pitches, so every sleeve meshes at the same phase; 15 mm of rack turns the plunger 96 deg |
| delivery-valve lift | 1.5 mm | valve with retraction collar |

This is a clearance-checked kinematic model, not OEM geometry. No
manufacturing claim is made for any dimension.

## What is built

Directory `generators/pes_pump/`, the same layout as `om615/`:

- `pes_spec.py` — the table above and the derived stations (cam
  centreline, barrel seat, rack axis). Every other dimension is derived from
  these, never a literal repeated in two files.
- `pes_parts.py` — one builder per part:
  - **housing**: the aluminium body — cam gallery with bearing bores at both
    ends, four tappet bores, four element bores through the top deck, the
    rack bore across the element row, the fuel gallery drilled lengthways,
    the side inspection window (with its cover plate as a separate part),
    the feed-pump mounting pad, the governor flange at the rear.
  - **camshaft**: four lobes (profiled from the lift law, phased 1-3-4-2),
    the feed-pump eccentric, bearing journals, taper and key at the drive end.
  - **roller tappet** x4: body, roller, pin (roller as its own part so it
    can turn).
  - **element** x4: barrel (fixed, with spill/inlet port) and plunger with
    its vertical groove and the **control helix** actually cut into it, plus
    the two drive vanes at the foot that ride in the control sleeve's slots.
  - **control sleeve** x4 with its toothed ring (a full involute gear), and the
    **plunger spring** with its lower seat (the upper seat is a web of the
    housing).
  - **control rack**: toothed along its length, with the governor link at
    the rear end.
  - **delivery valve** x4: valve body/seat, the valve with its retraction
    collar, spring, and the **delivery-valve holder** (the union nut the
    injector line threads onto).
  - **feed pump** (the side-mounted piston pump): body, piston, tappet
    and the hand primer.
  - **governor**: pneumatic diaphragm housing, diaphragm, main spring, cover
    — the diaphragm is rigid to the rack.
  - **timing device** on the drive end: hub, two flyweights on their pins,
    and the drive flange with the duplex sprocket.
- `pes_pump.py` — the assembly and its checks.

## How it moves

Two independent drivers, which is how the pump works: engine speed turns the
camshaft; the governor positions the rack.

1. **Camshaft** — revolute on the housing; the driving DOF of the "run"
   study (0..360 deg cam = two crank revolutions).
2. **Tappets** — slider in their bores, **cam relation** from the camshaft
   with each cylinder's lift table and phase.
3. **Control sleeve** — revolute about the element axis, **rack relation**
   from the control rack's slider (pitch radius from the spec).
4. **Plunger** — a cylindrical mate on the element axis carrying both of its
   real motions, each from its own cause: the cam relation drives the slide,
   and a rack relation with the sleeve's pitch radius drives the turn, so it
   turns exactly with the sleeve whose slots its vanes ride in.
5. **Delivery valve** — slider in its holder, cam relation from the camshaft
   lifting it from port closure to spill at the full-load setting. A cam
   relation cannot also read the rack, so the valve keeps full-load timing
   at every rack position.
6. **Feed pump** — its tappet follows the eccentric (cam relation, exact for
   a circle); roller and piston are rigid to that tappet. The hand primer is
   its own slider.
7. **Control rack** — slider along its bore; the driving DOF of the "load"
   study (stop to full load); the diaphragm is fastened to it.
8. **Timing flyweights** — revolute on their pins, coupled to each other
   (ratio 1) and to the sprocket's advance angle (8 deg over a 20 deg swing);
   a third study sweeps the advance.
9. **Tappet rollers** — revolute on their pins, turned by the camshaft at the
   base-circle rolling ratio (14/6); over the lobe that ratio is an
   approximation.

The springs do not stretch: the assembly has no flexible parts. Each is drawn
at its SHORTEST length and carried by the part it rides with (the plunger
spring by its tappet, the valve and governor springs by their holders), so it
never clashes and opens a gap at the other end of the travel.

## How it is verified

The generator fails loudly, not with a picture that looks right:

- Every part passes `check_geometry` (valid, closed, one solid).
- **Run study**, 73 samples over a cam turn: each plunger's stroke measured
  from the poses equals the cam lift (7.000 mm); the order the four reach top
  dead centre is 1-3-4-2 at 90 deg spacing; exact-boolean clash sweep CLEAN.
- **Load study**: rack from stop to full load; every plunger AND every sleeve
  turns the same angle, equal to travel / pitch radius; exact sweep CLEAN.
  Rack and sleeve teeth are real involutes, so a wrong relation sign shows as
  tooth clash.
- The helix is real geometry, placed by construction: the vertical groove is
  on the port at the stop position. A cross-section through element 1 at stop
  and at full load shows it; the effective stroke each rack setting gives is
  computed from the same helix law the plunger is cut to.
- Advance and primer studies: exact sweep CLEAN.
- Renders: iso exterior, a longitudinal section through the element row, the
  element-1 cross-sections, the rack through the window, a path-traced still,
  and an animated GLB of the run study, all under `scratch/pes_pump/`.

Nothing in the app changes unless the build finds a defect; one that does is
fixed in the app with a regression test, as in the OM615 rounds.
