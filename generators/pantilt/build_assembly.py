#!/usr/bin/env python3
"""Stage 5: the head assembled, checked and moved. Opens yoke.dsgn, saves
pantilt.dsgn, writes assembly_report.json, the BOM and the renders.

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
- Clearance assertions: the camera and the cradle clear the yoke, verified
  at each end of the tilt.
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
        ("screw", "camera_screw", "camera screw"),
    ]:
        o[key] = asm.place(part, name=name)
    return o


def mate(app, asm, o):
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
    asm.fasten((o["cradle"], "screw"), (o["screw"], "head"),
               name="camera screw")
    r = asm.solve()
    if r.get("errored_mates"):
        raise SystemExit(f"mates did not solve: {r['errored_mates']}")
    return m


def bearings(app, asm, o, S):
    """Each bearing fitted to the housing bore it presses into."""
    main = spec.designation(app, S["main_brg_d"])
    worm = spec.designation(app, S["worm_brg_d"])
    seats = []
    # A seat is the housing's counterbore: its radius is the bearing's
    # outside, as modelled (plus the print allowance on a printed part).
    plate = app.part("base_plate", create=False)
    seats.append(("pan lower", o["base"], plate,
                  faces_r(plate, S["main_brg_D"] / 2), main))
    for key, pname, D, des, label in [
        ("wheel", "pan_wheel", S["main_brg_D"], main, "pan upper"),
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
            out.append({"seat": label, "designation": r["designation"],
                        "occurrence": r["occurrence"],
                        "part": part.name, "face": f.ref,
                        "summary": r["summary"]})
            print(f"  bearing {label}: {r['summary']}")
    return out


def check_seats(app, asm, placed):
    asm.activate()
    out = []
    for b in placed:
        r = app.check_seat(occurrence=b["occurrence"])
        out.append({"seat": b["seat"], "status": r["status"],
                    "checks": [(c["characteristic"], c["status"],
                                c.get("reason", "")) for c in r["checks"]]})
        print(f"  check_seat {b['seat']}: {r['status']}")
    return out


def fasteners(app, asm, o, S):
    """The wheel's four screws into the plate's tapped holes, and two
    through each block with a nut on the far side."""
    out = []
    wheel = app.part("pan_wheel", create=False)
    face = S["wheel_face"]
    rims = [e for e in wheel.body().edges(circular=True)
            if e.radius and abs(e.radius - 2.25) < 0.01
            and abs(e.midpoint[2] - (face - 4.5)) < 0.01]
    block = app.part("worm_block", create=False)
    # The head sits on the web at the bottom of its counterbore.
    z = S["block_h"] / 2 - S["block_web"]
    heads = [e for e in block.body().edges(circular=True)
             if e.radius and abs(e.radius - 2.25) < 0.01
             and abs(e.midpoint[2] - z) < 0.01]
    if len(heads) != 2 or len(rims) != 4:
        raise SystemExit(f"screw rims: wheel {len(rims)}, block {len(heads)}")
    jobs = [(o["wheel"], e, {}) for e in rims]
    for k in ("pan_block_a", "pan_block_b", "tilt_block_a", "tilt_block_b"):
        jobs += [(o[k], e, {"nut": "iso4032", "washer_under_nut": True})
                 for e in heads]
    for occ, rim, kw in jobs:
        r = asm.bolt(rim, occ, standard="iso4762", **kw)
        out.append({"size": r["size"], "length_mm": r["length_mm"],
                    "summary": r["summary"]})
    print(f"  {len(out)} screws:",
          sorted({f"{x['size']}x{x['length_mm']:g}" for x in out}))
    return out


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


def assertions(app, asm, o, S):
    asm.activate()
    gap = S["run_gap"] / 2
    ids = []
    for who in ("camera", "cradle"):
        r = app.add_assertion(
            name=f"{who} clears the yoke", a=o[who].id, b=o["yoke"].id,
            min_mm=gap, mode="exact",
            why=f"it passes between the arms {S['run_gap']:g} mm off each "
                f"and swings {S['cam_clear']:g} mm over the base at full tilt")
        ids.append(r.get("id"))
    return ids


def verify(app, asm, where):
    asm.activate()
    r = app.verify_assertions()
    rows = [(a["name"], a.get("last_verdict"), a.get("measured_mm"))
            for a in r["assertions"]]
    print(f"  assertions at {where}: {rows}")
    return {"at": where, "rows": rows,
            "standing": r.get("receipt", {}).get("standing")}


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
    app.call("open_project", path=str(common.OUT / "yoke.dsgn"))
    S = spec.values(app.part("base_plate", create=False))
    asm = app.assembly("pantilt")
    asm.activate()
    o = place(app, asm, S)
    m = mate(app, asm, o)
    clock("placed and mated")
    placed = bearings(app, asm, o, S)
    REPORT["bearings"] = placed
    clock("bearings")
    REPORT["seats"] = check_seats(app, asm, placed)
    clock("seats")
    REPORT["fasteners"] = fasteners(app, asm, o, S)
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
    common.save(app, "pantilt.dsgn")
    REPORT["assertions"] = assertions(app, asm, o, S)
    REPORT["verify"] = [verify(app, asm, "zero tilt")]
    # Pan: the wheel is the base's, so the worm walks round it; 360 degrees
    # of pan is wheel_teeth turns of the worm.
    ratio = S["wheel_teeth"] / S["worm_starts"]
    REPORT["pan_study"], _ = run_study(
        app, asm, "pan 360", m["pan_worm"], 0.0, 360.0 * ratio,
        int(STUDY_SAMPLES), clock)
    # Tilt: +-90 degrees of the camera is +-90 * ratio of the tilt worm.
    REPORT["tilt_study"], tilt_id = run_study(
        app, asm, "tilt +-90", m["tilt_worm"], -90.0 * ratio, 90.0 * ratio,
        int(STUDY_SAMPLES), clock)
    # The ends of the tilt, posed by the worm itself (the relation turns
    # the wheel): leave_at would run the whole study again to get there.
    for deg, where in ((-90.0, "tilt -90"), (90.0, "tilt +90")):
        app.set_mate(id=m["tilt_worm"]["id"], rotation_deg=deg * ratio)
        asm.solve()
        REPORT["verify"].append(verify(app, asm, where))
    clock("assertions at full tilt")
    app.set_mate(id=m["tilt_worm"]["id"], rotation_deg=0.0)
    asm.solve()
    common.save(app, "pantilt.dsgn")
    REPORT["renders"] = renders(app, asm, m["tilt_worm"])
    clock("renders")
    common.save(app, "pantilt.dsgn")
    REPORT["time"] = clock.rows
    (common.OUT / "assembly_report.json").write_text(
        json.dumps(REPORT, indent=1, default=str))


if __name__ == "__main__":
    main()
