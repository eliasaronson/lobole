"""What the run checks before it is allowed to save.

Four things, and the third is the one that makes this a generator rather than
a script that happened to produce a document once.

1. **It built.** No failed feature, and every part is a valid solid.
2. **Nothing interferes.** Exact, not mesh. Panels that are glued to each
   other touch, and touching is not clashing.
2b. **Everything is MOUNTED.** `contact_report` must come back as ONE
   connected component. This is the check that was missing, and its absence
   let a document ship with eight T-nuts floating in mid-air, touching
   nothing: a part in a cavity clashes with nothing, so an empty clash sweep
   said the assembly was fine. Worse, chasing the clash count actively made
   it wrong — a T-nut whose barrel sat in its hole reported 222 mm3 of
   interference, and turning it over "fixed" that by lifting it out of the
   wood. A clash sweep can only tell you what is in the way; it cannot tell
   you that something is held.
3. **The parameters actually drive it.** The document is rebuilt at a second
   set of values and every part must move. A part whose volume AND bounding
   box are unchanged when the walls got 2 mm thicker and the cabinet 20 mm
   wider is a literal that survived, and the run fails naming it. This is the
   check that would have caught the port's five hard-coded bolt positions.
4. **It is the same cabinet.** Volumes against the parts that already exist —
   the document for the six panels, the STEP for the port and the braces.
   The braces used to be reported as a similarity rather than a match,
   because they were re-drawn; they are now recovered from the export's own
   sections and land at 1.009 and 0.999 (braces.py).

## Use the verb, not the sweep

`asm.check()` gates clashes, connectivity and near misses together, with an
`allow` map for intended contacts. Everything in check 2 was hand-rolled
before that was noticed, and every part of it was worse — see the comment
there. If a check here can be a call to the SDK, it should be.
"""
from pathlib import Path

import params

HERE = Path(__file__).resolve().parent
REFERENCE = HERE.parent.parent / "scratch/smart_speaker/reference"

# A second parameter set for check 3. Chosen so that every part in the
# cabinet has to respond to at least one of them.
# One entry per thing that has its own dimensions, so no part can sit the
# rebuild out: a part that does not read ANY of these would pass by not being
# asked, which is the same hole the gate exists to close.
REBUILD_AT = {
    "wall_thickness": "18.0",
    "cabinet_internal_w": "230.0",
    "port_length_total": "300.0",
    "plug_dia": "10.0",
    "t_nut_size": "16.0",
    "brace_ladder_h": "365.0",
}


# Contacts that are MEANT to happen, and the most overlap that is still fine.
# The port is a slip-fit stack: the tube's outside and both flares' throats
# are all `port_tube_od`, so an exact boolean finds material wherever they
# engage. That is the joint doing its job — declared by name and by number,
# not waved through by loosening the gate.
ALLOW = {
    ("port tube", "port flare (bay)"): 1000.0,
    # The bought driver is a whisker over the cut-out its own datasheet
    # specifies: 20.5 mm3 spread round a d148 wall 143 mm tall is 0.3 MICRONS
    # of mean depth. That is the STEP not being the physical part, and the
    # answer is not to open a cut-out the CAM cuts to a datasheet number.
    ("front_wall", "W170S woofer"): 40.0,
}


def run(app, p, made, asm, deep=False):
    ok = True
    print("\nverify:")

    # 1 -- it built
    bad = []
    for name, part in made.items():
        failed = (part.state().get("regen") or {}).get("failed") or []
        if failed:
            bad.append(f"{name}: features {failed} failed")
        part.activate()
        r = app.check_geometry()
        if not r.get("valid", True):
            bad.append(f"{name}: B-Rep invalid ({r.get('bodies')})")
    print(f"  built            {len(made)} parts, "
          f"{'no failures' if not bad else str(len(bad)) + ' BAD'}")
    for line in bad:
        print(f"    {line}")
    ok = ok and not bad

    # 2 -- nothing clashing, nothing floating, no near miss
    #
    # One call. `asm.check` gates all three together and takes `allow` for
    # contacts that are MEANT to happen — which is what the hand-written
    # version here was, badly: a clash reader that asked `interference` for a
    # key it did not have and so returned [] on a document with 1,068,565
    # mm3 of overlap, a connectivity check added only after eight T-nuts
    # shipped floating in mid-air, and an EXPECTED set that is `allow` under
    # a worse name.
    if asm is not None:
        try:
            # `near_tol` just under the designed flange fit. A driver in a
            # ply cut-out IS meant to be close — 0.5 mm on the radius — and
            # `allow` speaks for clashes, not for near misses, so the
            # threshold is the only way to say which gaps are on purpose. A
            # slip is still caught: they are tenths, not halves.
            asm.check(allow=ALLOW, near_tol=0.45)
            print("  assembly         clean: nothing clashing, floating or "
                  "nearly touching")
        except Exception as exc:
            print(f"  assembly         {exc}")
            ok = False

    # 4 -- the same cabinet (before 3, which moves everything)
    _compare(app, made, deep)

    # 3 -- the parameters drive it
    ok = _parametric(app, p, made) and ok
    return ok


def tuning(p, displaced_litres):
    """The port lands on its target, or the run says so.

    This is the one number a listener hears and the one the cabinet cannot
    be talked out of after glue-up, so it is CHECKED rather than printed and
    left to be read. It went unnoticed twice: `helmholtz_fb` reported the
    gross box for a while, and then reported a plain tube's end correction
    for a flared port — 36.9 Hz against a 33 Hz target, which reads as a
    design error and nearly bought a longer port for a port that was right.

    Half a hertz of tolerance, because the volume is measured off the model
    and the flare's effective length is not measured at all.
    """
    import params
    got = params.helmholtz_fb(p, displaced_litres)
    want = float(p["port_target_fb"])
    off = abs(got - want)
    flag = "" if off <= 0.5 else f"   <- {off:.1f} Hz off target"
    print(f"  tuning           fb {got:.1f} Hz against {want:.1f} "
          f"target{flag}")
    return off <= 0.5, got


def _compare(app, made, deep=False):
    """Against the reference STEPs, where there is one.

    Volume and bounding box, NOT IoU. The references were exported from the
    imported STEP's part tabs and imported geometry keeps the source's
    coordinates, so a reference sits where the cabinet puts it (the port tube
    at z -1012..-762) while the part here is built about its own origin. IoU
    between two solids 900 mm apart is a measure of nothing; it read 0.333 on
    a tube whose volume matched to four figures.
    """
    refs = {"ring_brace": "ring_brace", "brace_ladder": "brace_ladder",
            "port_tube": "port_tube"}
    # Where the design has deliberately LEFT the export, and by how much.
    # The port tube is cut 221 mm against the 250 it was drawn at, to put fb
    # back on its 33 Hz target now that the chamber is measured properly
    # (221/250 = 0.884). Without this the run flags a part that is right,
    # every time, and a check that cries wolf gets read past — which is
    # exactly how the export's own 175.8 bolt circle got argued away.
    expected = {"port_tube": 0.884}
    # Volume, bbox and the face/edge census — everything that needs no
    # boolean. `--deep` asks for the overlap too (iou/dice/precision/recall),
    # which is the only thing that can say two solids are the same SHAPE
    # rather than the same size, and costs three OCC booleans: 13.2 s for the
    # ring brace alone, 22 s of a 56 s run for the three of them
    # (docs/inspect/README.md#comparing-against-a-reference).
    metrics = "all" if deep else "volume"
    print(f'  against the export ({"shape, deep" if deep else "size + census"}):')
    for name, stem in refs.items():
        path = REFERENCE / f"{stem}.step"
        part = made.get(name)
        if part is None or not path.exists():
            continue
        try:
            part.activate()
            r = app.compare_geometry(golden_path=str(path), metrics=metrics)
            ratio = r.get("volume_ratio", 0)
            want = expected.get(name, 1.0)
            flag = ("" if abs(ratio - want) <= 0.03
                    else f"   <- {'re-drawn' if want == 1.0 else 'not the '
                                  'expected divergence'}")
            if want != 1.0 and not flag:
                flag = f"   (cut shorter on purpose: {want:.3f} expected)"
            shape = ""
            if deep:
                shape = f"  iou {r.get('iou', 0):5.3f}"
            elif r["faces"]["reference"] != r["faces"]["candidate"]:
                # Stated, not judged. The census catches a shape faked at the
                # right volume (stacked discs against a true cone,
                # docs/scripting/benchmark.md#construction-checks) — but a STEP round
                # trip splits faces too, and these braces come back 95 faces
                # against the export's 45 at an IoU of 0.991. --deep is what
                # answers it.
                shape = (f"  census {r['faces']['candidate']}f vs "
                         f"{r['faces']['reference']}f")
            print(f"    {name:<20} volume {ratio:6.3f} of the export"
                  f"{flag}{shape}")
        except Exception as exc:
            print(f"    {name:<20} could not compare: {exc}")


def _parametric(app, p, made):
    """Rebuild at REBUILD_AT, insist every part moved, and put it back.

    Restoring matters: without it the document that gets SAVED is the one at
    the test values, and the generator would ship a 230 mm cabinet nobody
    asked for.
    """
    before = {}
    for name, part in made.items():
        try:
            lo, hi = part.bbox()
            before[name] = (round(part.volume(), 3),
                            tuple(round(hi[i] - lo[i], 3) for i in range(3)))
        except Exception:
            pass

    # From the TABLE, not from the Param: a Param declared by name carries
    # the NAME as its expression ("wall_thickness"), so restoring from it
    # would write `wall_thickness = wall_thickness`.
    table = {name: expr for name, expr, _c in params.TABLE}
    was = {key: table[key] for key in REBUILD_AT}
    for key, value in REBUILD_AT.items():
        app.set_variable(name=key, expression=value, scope="project")

    frozen = []
    for name, part in made.items():
        if name not in before:
            continue
        # Bought geometry is imported and is SUPPOSED not to follow.
        if name in ("W170S woofer", "Alpair 7MS", "amp module", "PSU brick",
                    "port_flare_top", "port_flare_bottom"):
            continue
        try:
            lo, hi = part.bbox()
            now = (round(part.volume(), 3),
                   tuple(round(hi[i] - lo[i], 3) for i in range(3)))
        except Exception:
            continue
        if now == before[name]:
            frozen.append(name)

    for key, value in was.items():
        app.set_variable(name=key, expression=value, scope="project")

    print(f"  parametric       rebuilt at {REBUILD_AT}")
    if frozen:
        print(f"    !! unchanged, so not driven by anything: "
              f"{', '.join(frozen)}")
    return not frozen
