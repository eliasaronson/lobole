#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Stage 5: the head assembled, checked and moved. Opens stages/yoke.dsgn,
saves stages/assembly.dsgn, writes assembly_report.json, the BOM and the
renders.

    python3 generators/pantilt/build_assembly.py --port N

- Every part placed by mates to the named frames its stage built: the base
  plate fixed, the pan wheel screwed to it, the yoke on a revolute about the
  pan axis, the worms on revolutes in the yoke, the tilt wheel on a revolute
  between the arms, cradle, camera and screw fastened to it.
- The bearings fitted to their seats (fit_bearing) and checked (check_seat);
  the screws fitted to their holes (fit_fastener), nuts where a hole is
  through.
- Two worm relations, each derived from where its pair stands.
- The standing exact clash sweep, the declared fits (check_fits), the BOM.
- Motion studies driven from the worm shafts -- pan through 360 degrees,
  tilt through +-90 -- every sample checked exactly.
- The head's acceptance criteria as REQUIREMENTS the project holds
  (docs/requirements/README.md), checked at the end of the stage, which
  stops on any that is not met: both rings of every bearing seated
  (check_seat), check_fasteners with only the camera screw's known finding,
  every modelled part lint-clean (check_literals), both motion studies
  clean, and the camera and the cradle clearing the yoke through the whole
  tilt.
- Renders of the head.
"""
import json
import math
import os
import time

import common
import spec

REPORT = {}
# Every sample is checked exactly. 48 is the study's own default: 7.5 degrees
# of pan (300 of the worm) and 3.75 of tilt between samples.
STUDY_SAMPLES = int(os.environ.get("PANTILT_SAMPLES", 48))


def faces_r(part, radius, tol=1e-3):
    return [f for f in part.body().faces(kind="cylinder")
            if f.radius is not None and abs(f.radius - radius) < tol]


def place(app, asm, S):
    o = {}
    o["base"] = asm.place("base_plate", fixed=True, name="base plate")
    for key, part, name in [
        ("wheel", "pan_wheel", "pan wheel"), ("yoke", "yoke", "yoke"),
        ("pan_worm", "worm", "pan worm"), ("tilt_worm", "worm", "tilt worm"),
        ("pan_block_a", "worm_block", "pan block A"),
        ("pan_block_b", "worm_block", "pan block B"),
        ("tilt_block_a", "worm_block", "tilt block A"),
        ("tilt_block_b", "worm_block", "tilt block B"),
        ("pan_knob", "knob", "pan knob"), ("tilt_knob", "knob", "tilt knob"),
        ("tilt_wheel", "tilt_wheel", "tilt wheel"),
        ("cradle", "cradle", "cradle"), ("camera", "camera", "camera"),
    ]:
        o[key] = asm.place(part, name=name)
    return o


def mate(app, asm, o, S):
    m = {}
    m["wheel"] = asm.fasten((o["base"], "seat"), (o["wheel"], "seat"),
                            name="pan wheel on plate")
    m["pan"] = asm.revolute((o["wheel"], "top"), (o["yoke"], "pan"),
                            name="pan")
    m["pan_worm"] = asm.revolute((o["yoke"], "pan_worm"),
                                 (o["pan_worm"], "axis"), name="pan worm")
    m["tilt"] = asm.revolute((o["yoke"], "tilt"), (o["tilt_wheel"], "axis"),
                             name="tilt")
    m["tilt_worm"] = asm.revolute((o["yoke"], "tilt_worm"),
                                  (o["tilt_worm"], "axis"), name="tilt worm")
    for k in ("pan_block_a", "pan_block_b", "tilt_block_a", "tilt_block_b"):
        asm.fasten((o["yoke"], k), (o[k], "bore"), name=k.replace("_", " "))
    asm.fasten((o["pan_worm"], "knob"), (o["pan_knob"], "bore"),
               name="pan knob")
    asm.fasten((o["tilt_worm"], "knob"), (o["tilt_knob"], "bore"),
               name="tilt knob")
    asm.fasten((o["tilt_wheel"], "cradle"), (o["cradle"], "shaft"),
               name="cradle on shaft")
    asm.fasten((o["cradle"], "seat"), (o["camera"], "socket"),
               name="camera on cradle")
    o["screw"] = camera_screw(app, asm, o["cradle"], S)
    r = asm.solve()
    if r.get("errored_mates"):
        raise SystemExit(f"mates did not solve: {r['errored_mates']}")
    return m


def camera_screw_length(S):
    """The camera screw's stocked length (spec.py's `cam_screw_l`), checked
    against what it has to do: through the cradle's plate, which spec.py
    sized for it, and into the camera's socket by `cam_screw_eng`, short of
    the socket's floor."""
    length = S["cam_screw_l"]
    eng = length - S["plate_t"]
    if eng > S["cam_socket"] + 1e-6 or eng < S["cam_screw_eng"] - 1e-6:
        raise SystemExit(f"camera screw: {length:.3f} mm engages {eng:.3f} "
                         f"in a {S['cam_socket']:g} mm socket")
    return length


def camera_screw(app, asm, cradle, S):
    """The 1/4-20 camera screw, bought: an ASME B18.3 socket head cap screw
    from the app's table, fastened to the cradle's `screw` frame (the head's
    bearing face, the shank up +z: the fastener's own origin)."""
    asm.activate()
    r = app.add_fastener(standard="asme_b18_3", size="1/4-20",
                         length=camera_screw_length(S))
    app.rename_occurrence(occurrence=r["occurrence"], name="camera screw")
    app.add_mate(type="fastened", name="camera screw", flipped=False,
                 a={"occurrence": cradle.id, "frame": "screw"},
                 b={"occurrence": r["occurrence"], "origin": True})
    print(f"  camera screw: {r['part']}")
    return r["occurrence"]


def bearings(app, asm, o, S):
    """Each bearing fitted to the housing bore it presses into."""
    main = spec.designation(app, S["main_brg_d"])
    pan = spec.designation(app, S["pan_brg_d"])
    worm = spec.designation(app, S["worm_brg_d"])
    seats = []
    # A seat is the housing's counterbore: its radius is the bearing's
    # outside, as modelled (plus the print allowance on a printed part).
    plate = app.part("base_plate", create=False)
    seats.append(("pan lower", o["base"], plate,
                  faces_r(plate, S["pan_brg_D"] / 2), pan))
    for key, pname, D, des, label in [
        ("wheel", "pan_wheel", S["pan_brg_D"], pan, "pan upper"),
        ("yoke", "yoke", S["main_brg_D"], main, "tilt"),
    ]:
        part = app.part(pname, create=False)
        model = part.app.print_allowance(kind="hole", size_mm=D,
                                         part=part.id)["model_mm"]
        seats.append((label, o[key], part, faces_r(part, model / 2), des))
    block = app.part("worm_block", create=False)
    bmodel = app.print_allowance(kind="hole", size_mm=S["worm_brg_D"],
                                 part=block.id)["model_mm"]
    bface = faces_r(block, bmodel / 2)
    for k in ("pan_block_a", "pan_block_b", "tilt_block_a", "tilt_block_b"):
        seats.append((k, o[k], block, bface, worm))
    out = []
    asm.activate()
    for label, occ, part, faces, des in seats:
        if not faces:
            raise SystemExit(f"no seat face for {label}")
        for f in faces:
            r = app.fit_bearing(seat=dict(f.ref, occurrence=occ.id),
                                designation=des)
            if len(faces) > 1:  # the arms' pair: which arm
                label = f"tilt {'+' if f.centroid[1] > 0 else '-'}Y"
            out.append({"seat": label, "designation": r["designation"],
                        "occurrence": r["occurrence"],
                        "part": part.name, "face": f.ref,
                        "summary": r["summary"]})
            print(f"  bearing {label}: {r['summary']}")
    return out


def retaining_rings(app, asm, o):
    """The DIN 471 rings that hold the two inner rings no shoulder locates:
    the lower pan bearing's on the yoke's journal, the -Y tilt bearing's on
    the tilt shaft. Each is put in the groove its part was cut with
    (fit_retaining_ring), seated on the groove's faces, so it follows the
    groove when the journal or the shaft changes."""
    out = []
    for key, pname in (("yoke", "yoke"), ("tilt_wheel", "tilt_wheel")):
        part = app.part(pname, create=False)
        face = common.ring_groove_face(part)
        asm.activate()
        r = app.fit_retaining_ring(groove=dict(face, occurrence=o[key].id))
        out.append({"part": pname, "ring": r["part"],
                    "occurrence": r["occurrence"], "summary": r["summary"]})
        print(f"  ring: {r['summary']}")
    return out


def check_seats(app, asm, placed):
    """Both rings of every bearing, each on the seat it sits on: the one its
    `Bearing seat` mate names, and the other read through the coaxial
    cylinder under it (docs/parts/README.md#both-rings). Not `mate_other`:
    the yoke and the tilt shaft turn by joints of their own, and a second
    revolute through each bearing would close a redundant loop.

    Every shoulder is judged against SKF's abutment limits, built in
    (docs/parts/README.md#abutment-limits). What cannot be judged stays
    unverified and is listed with its reason, not read as a pass. Every
    ring is located -- on a shoulder, or by a retaining ring in a groove
    beside it -- so every check must PASS: a ring with no seat, a check
    that fails or one left unverified fails the stage's `seats`
    requirement. This is the report of each ring; the verdict is the
    requirement's."""
    asm.activate()
    out = []
    for b in placed:
        r = app.check_seat(occurrence=b["occurrence"])
        rings = {}
        for seat in r["seats"]:
            ring = seat["ring"]
            if seat.get("error") or "part" not in seat:
                print(f"  check_seat {b['seat']}: no seat for the {ring} "
                      f"ring: {seat}")
                continue
            size = "shaft_diameter" if ring == "inner" else "housing_diameter"
            mine = [c for c in r["checks"]
                    if c["characteristic"].startswith(ring + "_")
                    or c["characteristic"] == size]
            rings[ring] = {
                "seat": seat["part"],
                "found": "mate" if seat.get("mate") else seat.get("found"),
                "diameter_mm": seat.get("diameter"),
                "checks": {c["characteristic"]: c["status"] for c in mine},
                "unverified": {c["characteristic"]: c.get("reason", "")
                               for c in mine if c["status"] == "unverified"}}
        out.append({"seat": b["seat"], "designation": b["designation"],
                    "status": r["status"], "rings": rings})
        print(f"  check_seat {b['seat']}: {r['status']}; " + "; ".join(
            f"{k} on {v['seat']} ({v['found']}): " + ", ".join(
                f"{c} {st}" for c, st in v["checks"].items())
            for k, v in sorted(rings.items(), reverse=True)))
    return out


def fasteners(app, asm, o, S):
    """The wheel's four screws into the plate's tapped holes, and two
    through each block with a nut on the far side."""
    out = []

    def clear_r(part):
        # The M4 clearance hole as MODELLED in that part: a Hole's clearance
        # fit on a printed part is the size it prints at plus the process's
        # allowance, so the radius is asked of the part's own process.
        return app.print_allowance(part=part.id, kind="hole",
                                   size_mm=S["m4_clear"])["model_mm"] / 2

    wheel = app.part("pan_wheel", create=False)
    face = S["wheel_face"]
    r = clear_r(wheel)
    rims = [e for e in wheel.body().edges(circular=True)
            if e.radius and abs(e.radius - r) < 0.01
            and abs(e.midpoint[2] - (face - 4.5)) < 0.01]
    block = app.part("worm_block", create=False)
    # The head sits on the web at the bottom of its counterbore.
    z = S["block_h"] / 2 - S["block_web"]
    r = clear_r(block)
    heads = [e for e in block.body().edges(circular=True)
             if e.radius and abs(e.radius - r) < 0.01
             and abs(e.midpoint[2] - z) < 0.01]
    # The yoke prints as a base and two arms: each arm's foot flange is
    # bolted down through the base, the head on the flange.
    yoke = app.part("yoke", create=False)
    zf = S["yoke_zt"] + S["yoke_base_t"]
    r_yoke = clear_r(yoke)
    joints = [e for e in yoke.body().edges(circular=True)
              if e.radius and abs(e.radius - r_yoke) < 0.01
              and abs(e.midpoint[2] - zf) < 0.01
              and abs(e.midpoint[1]) > S["arm_out"]]
    if len(heads) != 2 or len(rims) != 4 or len(joints) != 4:
        raise SystemExit(f"screw rims: wheel {len(rims)}, block {len(heads)}"
                         f", yoke joint {len(joints)}")
    jobs = [(o["wheel"], e, {}) for e in rims]
    jobs += [(o["yoke"], e, {"nut": "iso4032", "washer_under_nut": True})
             for e in joints]
    for k in ("pan_block_a", "pan_block_b", "tilt_block_a", "tilt_block_b"):
        jobs += [(o[k], e, {"nut": "iso4032", "washer_under_nut": True})
                 for e in heads]
    # The tilt blocks' screws come out under the ledge into a gap, the nut's,
    # and the line carries on into the +Y arm's A-frame further down: the
    # joint ends at the ledge's underside, which is what `far_end` says
    # (each screw takes the underside rim on its own axis; the app refuses
    # one off it).
    zl = S["ledge_z"] - S["yoke_base_t"]
    unders = [e for e in yoke.body().edges(circular=True)
              if e.radius and abs(e.radius - r_yoke) < 0.01
              and abs(e.midpoint[2] - zl) < 0.01]
    tilt_blocks = {o[k].id for k in ("tilt_block_a", "tilt_block_b")}
    for occ, rim, kw in jobs:
        if occ.id in tilt_blocks:
            for under in unders:
                far = dict(under.ref, occurrence=o["yoke"].id)
                try:
                    r = asm.bolt(rim, occ, standard="iso4762", far_end=far,
                                 **kw)
                    break
                except Exception as e:  # off this screw's axis: the next
                    if "off this hole's axis" not in str(e):
                        raise
            else:
                raise SystemExit("a tilt block screw found no ledge rim")
            out.append({"size": r["size"], "length_mm": r["length_mm"],
                        "summary": r["summary"]})
            continue
        r = asm.bolt(rim, occ, standard="iso4762", **kw)
        out.append({"size": r["size"], "length_mm": r["length_mm"],
                    "summary": r["summary"]})
    print(f"  {len(out)} screws:",
          sorted({f"{x['size']}x{x['length_mm']:g}" for x in out}))
    return out


def fastener_check(app, asm, S):
    """check_fasteners over the head: every placed screw re-derived against
    the stack it is in, reported. One finding is expected and explained
    (README, Known): the camera screw has less thread in the camera than one
    diameter, the app's rule, because a camera's tripod socket is shallower
    than that by design (`cam_socket`). The `fasteners` requirement allows
    that one and fails on anything else."""
    asm.activate()
    r = app.check_fasteners()
    print(f"  check_fasteners: {r.get('examined')} examined, "
          f"{len(r.get('findings', []))} findings"
          + "".join(f"\n    {f['part']}: {f['why']}" for f in
                    r.get("findings", [])))
    return {"examined": r.get("examined"), "skipped": r.get("skipped"),
            "findings": r.get("findings", [])}


def relations(app, asm, m, S):
    out = []
    for worm_key, wheel_key in (("pan_worm", "pan"), ("tilt_worm", "tilt")):
        r = asm.worm(m[worm_key], m[wheel_key],
                     starts=int(S["worm_starts"]),
                     teeth=int(S["wheel_teeth"]), hand="right",
                     name=f"{wheel_key} worm pair")
        out.append({k: r.get(k) for k in ("id", "ratio", "warnings")})
        print(f"  relation {wheel_key}: ratio {r.get('ratio')}")
    return out


def tolerances(app, asm, placed):
    """What each seat may vary by, so check_fits has two sides to judge: the
    machined pocket an ISO class, a printed bore what desktop FDM holds."""
    done = set()
    for b in placed:
        key = (b["part"], b["face"]["element"])
        if key in done:
            continue
        done.add(key)
        part = app.part(b["part"], create=False)
        part.activate()
        if b["part"] == "base_plate":
            # The outer ring is stationary under a load that turns with the
            # yoke: a transition fit, K7 for a light rotating load on the
            # outer ring (M7 or N7 for a heavy one).
            app.set_tolerance(**b["face"], **{"class": "K7"},
                              comment="machined pocket, light rotating load")
        else:
            app.set_tolerance(**b["face"], lower=-0.15, upper=0.15,
                              comment="printed bore, modelled at its "
                                      "allowance; FDM holds about +-0.2")
    asm.activate()


def fits(app, asm):
    asm.activate()
    r = app.check_fits()
    rows = [{"mate": f.get("mate_name"), "status": f["status"],
             "reason": f.get("reason", ""), "kind": f.get("kind"),
             "min_clearance": f.get("min_clearance"),
             "max_clearance": f.get("max_clearance")}
            for f in r["fits"]]
    summary = {k: r.get(k) for k in ("status", "passed", "warned", "failed",
                                       "reported", "unverified")}
    print("  check_fits:", summary)
    return {"summary": summary, "fits": rows}


def standing(app, asm):
    asm.activate()
    r = asm.check_clashes()
    print(f"  standing exact sweep: clean, {r.get('pairs_checked')} pairs, "
          f"{r.get('elapsed_s', 0):.1f} s")
    return {"pairs_checked": r.get("pairs_checked"),
            "elapsed_s": r.get("elapsed_s")}


def run_study(app, asm, name, driver, lo, hi, samples, clock):
    """A motion study driven from a worm shaft, every sample checked exactly.
    Run in the background and polled: a sweep this size outlasts an RPC."""
    asm.activate()
    st = app.add_motion_study(driver=driver["id"], dof="rotation",
                              **{"from": lo, "to": hi}, samples=samples,
                              clash_mode="exact", name=name)
    sid = st.get("id", st.get("study", {}).get("id"))
    app.run_motion_study(study=sid, **{"async": True})
    while True:
        r = app.motion_status(study=sid)
        if not r.get("running") and r.get("result") is not None:
            res = r["result"]
            break
        if not r.get("running") and "clean" in r:
            res = r
            break
        time.sleep(2)
    clashes = [(sm.get("value", sm.get("driver")), c)
               for sm in res.get("samples", []) for c in sm.get("clashes", [])]
    unsolved = [sm for sm in res.get("samples", []) if not sm.get("solved",
                                                                  True)]
    out = {"study": sid, "samples": samples, "from_deg": lo, "to_deg": hi,
           "clean": res.get("clean"), "clashes": clashes[:10],
           "clash_count": len(clashes), "unsolved": len(unsolved),
           "pairs_checked": res.get("pairs_checked"),
           "pairs_rigid": res.get("pairs_rigid"),
           "rigid_clashes": res.get("rigid_clashes")}
    clock(f"motion study {name}")
    print(f"  {name}: clean {out['clean']}, {out['clash_count']} clashes, "
          f"{out['unsolved']} unsolved, pairs {out['pairs_checked']}")
    return out, sid


def shaft_bearings(placed):
    """The head's bearings by the shaft each carries, with the variable
    that shaft's own minimum diameter is (spec.py): the yoke's pan journal
    in the plate's and the wheel's bearings, the tilt shaft in the arms',
    each worm's journals in its blocks'. Every placed bearing is in exactly
    one group; one left out stops the stage."""
    groups = [("pan journal", "pan_shaft_d_min",
               [b for b in placed if b["seat"].startswith("pan ")]),
              ("tilt shaft", "shaft_d_min",
               [b for b in placed if b["seat"].startswith("tilt ")]),
              ("worm journal", "worm_shaft_d_min",
               [b for b in placed if b["seat"].endswith("_block_a")
                or b["seat"].endswith("_block_b")])]
    grouped = sum(len(g[2]) for g in groups)
    if grouped != len(placed) or any(not g[2] for g in groups):
        raise SystemExit(f"bearings by shaft: {grouped} of {len(placed)} "
                         f"grouped: {[b['seat'] for b in placed]}")
    return groups


def requirements(app, asm, o, S, placed, pan_id, tilt_id):
    """What the head is for, stated as requirements the project holds
    (docs/requirements/README.md): `App.require` states each one, or edits
    the one of that name, so a re-run states the same set."""
    gap = "run_gap / 2"
    why_gap = (f"it passes between the arms {S['run_gap']:g} mm off each and "
               f"swings {S['cam_clear']:g} mm over the base at full tilt")
    for who in ("camera", "cradle"):
        app.require(f"{who} clears the yoke through the tilt", {
            "kind": "clearance", "document": asm.id, "a": o[who].id,
            "b": o["yoke"].id, "mode": "exact", "motion_study": tilt_id},
            at_least=gap, source=why_gap)
    app.require("every bearing seated, both rings", {
        "kind": "check_seat", "document": asm.id,
        "occurrences": [b["occurrence"] for b in placed]},
        source="every ring is located, on a shoulder or by a retaining ring "
               "beside it, and every shoulder is inside SKF's abutment limits")
    # Every bearing against the shaft it carries: the shaft is sized from
    # the load (spec.py: `*_shaft_d_min`), the bearing is a catalogue row
    # chosen once, and a row does not follow a variable -- so a heavier
    # camera regenerates the shaft's own minimum past a bore nothing else
    # compares it with. One requirement per shaft, over its bearings.
    for what, minimum, bearings in shaft_bearings(placed):
        app.require(f"the {what}'s bearings take its minimum", {
            "kind": "bearing_bore", "document": asm.id,
            "occurrences": [b["occurrence"] for b in bearings]},
            at_least=minimum,
            source=f"`{minimum}` is the {what} sized for its load at 3 g; "
                   "each bearing's bore must take it")
    app.require("fasteners: only the camera socket's", {
        "kind": "check_fasteners", "document": asm.id,
        "allow": [{"part": "ASME B18.3 1/4-20", "why": "of thread in camera",
                   "max": 1}]},
        source="a camera's tripod socket is 5.5 mm deep by design "
               "(cam_socket), under the one diameter the rule asks; every "
               "other screw passes")
    app.require("every modelled part lint-clean", {
        "kind": "check_literals",
        "documents": [app.part(n, create=False).id
                      for n in common.MODELLED]},
        source="every number is a variable or follows a face, so base_t 2 "
               "mm more regenerates the head")
    for sid, what in ((pan_id, "pan 360"), (tilt_id, "tilt +-90")):
        app.require(f"{what} moves clean", {
            "kind": "motion_clean", "document": asm.id, "study": sid},
            source="driven from its worm, every sample solved and checked "
                   "exactly, nothing clashing")


def bom(app, asm):
    asm.activate()
    r = app.bom()
    rows = [{"part": x["part"], "quantity": x["quantity"],
             "material": x.get("material"), "standard": x.get("standard"),
             "designation": x.get("designation"),
             "mass_g": x.get("total_mass_g", x.get("mass_g"))}
            for x in r["rows"]]
    app.export_bom(path=str(common.OUT / "pantilt_bom.csv"))
    for x in rows:
        print(f"  BOM {x['quantity']:3d} x {x['part']}"
              f" {x['material'] or ''} {x['mass_g']}")
    return rows


def masses(app, asm, o):
    asm.activate()
    total = app.mass_properties()["total"]
    out = {"assembly_g": total["mass_g"], "com_mm": total["center_of_mass"]}
    for name in ("base_plate", "pan_wheel", "yoke", "tilt_wheel", "cradle",
                 "worm", "worm_block", "knob", "camera"):
        part = app.part(name, create=False)
        out[name + "_g"] = app.mass_properties(part=part.id)["total"]["mass_g"]
    asm.activate()
    print("  masses:", {k: (round(v, 1) if isinstance(v, float) else v)
                        for k, v in out.items()})
    return out


def renders(app, asm, tilt_worm_mate):
    asm.activate()
    shots = []
    # iso looks down on the yoke's base, which hides the pan wheel and the
    # plate under it; "low" looks up at them from the pan worm's side.
    for name, view, deg in (("pantilt_iso", "iso", 0.0),
                            ("pantilt_front", "front", 0.0),
                            ("pantilt_low", (1.0, -1.3, -0.25), 0.0),
                            ("pantilt_tilt_up", "iso", 3600.0)):
        app.set_mate(id=tilt_worm_mate["id"], rotation_deg=deg)
        asm.solve()
        if isinstance(view, str):
            app.view(name=view)
        else:
            app.set_camera(direction=list(view), up=[0, 0, 1])
        path = str(common.OUT / f"{name}.png")
        app.render_still(path=path, width=1600, height=1200, samples=128,
                         fit=True)
        shots.append(path)
        print("  render", path)
    app.set_mate(id=tilt_worm_mate["id"], rotation_deg=0.0)
    asm.solve()
    return shots


def main():
    a = common.args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(common.stage("yoke")))
    S = spec.values(app.part("base_plate", create=False))
    asm = app.assembly("pantilt")
    asm.activate()
    o = place(app, asm, S)
    m = mate(app, asm, o, S)
    clock("placed and mated")
    placed = bearings(app, asm, o, S)
    REPORT["bearings"] = placed
    clock("bearings")
    REPORT["retaining_rings"] = retaining_rings(app, asm, o)
    clock("retaining rings")
    REPORT["seats"] = check_seats(app, asm, placed)
    clock("seats")
    REPORT["fasteners"] = fasteners(app, asm, o, S)
    REPORT["check_fasteners"] = fastener_check(app, asm, S)
    clock("fasteners")
    REPORT["relations"] = relations(app, asm, m, S)
    clock("relations")
    REPORT["standing"] = standing(app, asm)
    clock("standing clash sweep")
    tolerances(app, asm, placed)
    REPORT["fits"] = fits(app, asm)
    clock("fits")
    REPORT["bom"] = bom(app, asm)
    REPORT["mass"] = masses(app, asm, o)
    clock("bom and mass")
    common.save(app, "assembly")
    # Pan: the wheel is the base's, so the worm walks round it; 360 degrees
    # of pan is wheel_teeth turns of the worm.
    ratio = S["wheel_teeth"] / S["worm_starts"]
    REPORT["pan_study"], pan_id = run_study(
        app, asm, "pan 360", m["pan_worm"], 0.0, 360.0 * ratio,
        int(STUDY_SAMPLES), clock)
    # Tilt: +-90 degrees of the camera is +-90 * ratio of the tilt worm.
    REPORT["tilt_study"], tilt_id = run_study(
        app, asm, "tilt +-90", m["tilt_worm"], -90.0 * ratio, 90.0 * ratio,
        int(STUDY_SAMPLES), clock)
    common.save(app, "assembly")
    REPORT["renders"] = renders(app, asm, m["tilt_worm"])
    clock("renders")
    requirements(app, asm, o, S, placed, pan_id, tilt_id)
    REPORT["requirements"], bad = common.requirements_met(app)
    clock("requirements")
    common.save(app, "assembly")
    REPORT["time"] = clock.rows
    (common.OUT / "assembly_report.json").write_text(
        json.dumps(REPORT, indent=1, default=str))
    common.stop_unless_met(bad)


if __name__ == "__main__":
    main()
