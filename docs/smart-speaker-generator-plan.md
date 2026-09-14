# smart_speaker generator

## The problem

`~/Documents/smart_speaker.dsgn` is an Onshape port that brought across six
cabinet walls and stopped. Against `~/Downloads/step_files/speaker.step` — the
same design, complete — it is missing every part that is not a flat panel: the
port, both braces, both drivers and the amplifier. The
assembly has eight occurrences and no mates, so nothing holds together when a
dimension moves.

What did come across is genuinely parametric — 43 named parameters, extrudes
driven by `distance_expr`, sketch constraints carrying `expr` — but only in
part. The driver centres are literals (`123.0`, `435.0` in the baffle sketch)
while `w170s_y = 700` and `w170_height = 650` sit in the table unused and
disagreeing with them; the bolt circles are five precomputed points each rather
than a count and a diameter. Those are the CLAUDE.md "Defaults" rule broken:
correct in the abstract, wrong against the part as soon as a chamber height
changes.

Worse, the 44 parameters are not one table but six copies of one, a part-scoped
set in every wall. Correcting the W170S cut-out to the datasheet's 148 mm
changed `front_wall`'s copy; the other five still read `(145.4+1) * 1.0`. That
is harmless only because no other wall uses the number today — the mechanism
that let it drift is the point, and it is one edit away from a rebate cut to a
different diameter than the baffle it meets. The STEP predates that fix and so
has the old opening: **where the two sources disagree the document wins, and
the STEP is a reference for the parts that are missing, not for the parts that
were corrected.**

So the model cannot answer the question it exists to answer — *what happens to
this cabinet if the woofer moves, the walls get thicker, or the port retunes* —
and the parts that would move most are the ones that are not there.

## What to build

One generator, `generators/smart_speaker/`, that builds the whole project from
a parameter table: every wall, every brace, the port, the hardware, the
assembly that mates them, and the baffle CAM job. Bought parts come in as STEP.
It follows the layout the other multi-file generators use (`om615/`,
`fpv_drone/`), so each module adds its own directory and `generators/` to
`sys.path` and reaches `_lib` by bare name.

```
generators/smart_speaker/
  smart_speaker.py     entry point; --port, --out, --skip-cam
  params.py            the one parameter table, and everything derived from it
  walls.py             side, front, back, top, bottom, divider
  braces.py            the port-ring brace and the window-brace ladder
  port.py              the flared port tube
  hardware.py          T-nuts and dowel plugs
  bought.py            STEP import of the drivers and the amp case
  cam.py               the baffle job on the 6090
  extract_bought.py    one-time: slice speaker.step into per-part STEPs
  BUILD.md             what the run produces and what it checked
```

### The parameter table is the authority

`params.py` holds the 44 parameters already in the document and registers them
**once, at project scope** — `scope="project"`, not the per-part copies the
port produced — and adds what the literals are hiding:

- `w170s_center_from_top` and `alpair_center_from_top` replace the baffle's
  `123.0` and `435.0`. The built model puts them 388 mm and 76 mm below the
  cabinet top, which is what the STEP measures and what the owner confirmed;
  `w170s_y`, `w170_height` and `alpair_y` disagree with that and go, rather
  than leaving two answers in the table.
- Bolt circles become `(count, diameter)` and the generator loops. Five points
  written out five times is why `w170s_bolt_count = 5` currently changes
  nothing.
- Chamber heights derive: the W170S chamber is what is left after the Alpair
  chamber and the port bay, not an independent literal.

Every wall keeps the expression it already has where it has one
(`wall_thickness`, `plug_length - wall_thickness/2`, `finish_wall_thickness`);
the generator writes those as expressions, not as the numbers they evaluate to.

### The parts that are missing

- **Port-ring brace** (210 × 320 × 16 at z = −785.2) — a window frame with a
  central ring the port tube passes through, cross members and four screw
  holes. It is what closes the port bay; there is no separate shelf. Drawn from
  `port_tube_od` and the internal cross-section, not from the ring diameter it
  happens to have.
- **Window-brace ladder** (210 × 320 × 345.7) — a four-window horizontal frame
  with three vertical stiffeners and wall tabs, in the W170S chamber.
- **Port** — three revolves, as the STEP has them: the tube (Ø105 over Ø100
  bore, 250 long) and a flare at each end (Ø155 at the top, Ø160 bolted to the
  bottom wall on `outer_port_bolt_circle`). Revolved profiles rather than
  imported solids because port length and diameter are the tuning knobs and
  the run reports the Helmholtz `fb` they give. The flares' moulded mounting
  feet are not reproduced; their bolt holes are, because the assembly and the
  bottom wall both need them.
- **T-nuts and plugs** — the 14.8 × 25 × 14.8 blocks, patterned onto the bolt
  circles they serve rather than placed.

### The parts that are bought

`extract_bought.py` imports `speaker.step` once and exports the W170S (four
solids), the Alpair and the amp case as small per-part STEPs under
`scratch/smart_speaker/bought/`. The generator imports those. The 48 MB source
with its ~200 SMD components is read once, not on every run — the full import
costs 27.5 s and 301 parts, the slices cost neither.

### Two defects the port left behind, which the generator does not reproduce

An exact sweep of the current assembly clashes on ten of twenty-one pairs.

- **`top_wall` is placed twice**, at z −19.6…0 and at z −16…3.6 — 1,068,565
  mm³ of the same panel in the same space. Only the second is right: the first
  has its veneer facing INTO the cabinet. The generator places one.
- **The veneer skins interpenetrate.** Each wall carries its own 3.6 mm skin,
  so at every vertical corner two of them share a 3.6 × 3.6 column over the
  full 1125.6 mm (14,526 mm³ a corner), and the top skin caps the side skins
  again across 19.6 × 359.2 (24,884 mm³). The `Chamfer 1` on each wall is the
  mitre that was meant to resolve this and does not. The generator mitres the
  skins so the sweep is empty.

### The assembly holds itself together

Eight fixed transforms become mates: each wall to the carcass, the drivers
coaxial to their cutouts, the port to its ring brace, the amp case to its
bracket. The point is that a parameter change moves the parts that follow it
instead of leaving them behind.

### CAM

**Built.** `cam.py` rebuilds the 6090 job: the setup frame, the `stock_max` origin, three
tools, and the fourteen operations. Ten of those are one screw hole each, so
the script writes them from `w170s_bolt_count` and `alpair_bolt_count` in a
loop. Targets resolve **by geometry** — the cylinder at this radius at this
centre — never by the `10/cap1;M10` element strings the current job holds,
which are only valid for the feature order that produced them. The `CNC`
configuration that suppresses the veneer, the side plug holes and the chamfer
is rebuilt with it.

## How it is verified

The run refuses to save what it has not checked, as `fpv_drone.py` does:

- regeneration clean — no failed feature, warnings printed.
- **exact interference sweep empty.** Glued panel contacts are the expected
  exception and are declared by name AND by number in `asm.check`'s `allow`
  map, which gates clashes, floating parts and near misses in one call.
  Reading the sweep by hand is what this replaced: `interference` reported
  its findings under `pairs` alone, a check that asked for `clashes` got
  `[]`, and the gate passed on 1,068,565 mm3 of overlap. That method now
  answers to both names.
- **volumes match the reference.** Each generated part is compared against the
  corresponding solid — the port, the braces and the shelf come from
  `speaker.step`, which is the only place they exist; the six walls come from
  the current document, which is ahead of the STEP on the baffle openings. The
  baffle is checked against the document and is expected to differ from the
  STEP by exactly the two corrected cut-outs.
- **the parametric claim is tested, not asserted.** The run rebuilds at a
  second parameter set (`wall_thickness` 16 → 18, `cabinet_internal_w` 210 →
  230, `port_length_total` 250 → 300) and checks that every part moved: any
  part whose volume and bbox are unchanged is a literal that survived, and
  fails the run.
- CAM gates on `cam_verify ok:true`; `complete:false` is correct with tabs.
- a section and an iso render, and the mass budget, into `BUILD.md`.

No format change, so no `kVersion` bump and no golden.

## Gated

`tests/generators_smoke.py` gains a case that builds the cabinet carcass only
(walls plus braces, no STEP imports, no CAM) and checks it by volume, extents
and that `wall_thickness` still drives it — the same shape as the gear case,
and headless, so it stays in `ctest -LE gui`.
