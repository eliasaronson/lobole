#!/usr/bin/env python3
"""A Bosch PES4M inline injection pump (Mercedes OM615, L306D) that MOVES:
four roller tappets and plungers lifted by the camshaft in firing order
1-3-4-2, the control rack turning every plunger's helix through its control
sleeve, the delivery valves lifting during the effective stroke, the feed
pump's tappet riding its eccentric, the governor diaphragm carried by the
rack, the hand primer, and the timing device's flyweights advancing the
sprocket.

    tools/headless-preview.sh --start <port>
    python3 generators/pes_pump/pes_pump.py --port <port> [--out scratch/pes_pump]

What it models and its limits: generators/pes_pump/README.md.
"""
import argparse
import json
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools"))
from designer_sdk import connect  # noqa: E402
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pes_spec import *  # noqa: E402,F401,F403
import pes_parts as P  # noqa: E402

ASSEMBLY = "PES4M injection pump"
LOAD_CONFIGS = [("Stop", 0.0), ("Idle", 4.0), ("Full load", RACK_TRAVEL)]
WEIGHT_SWING = 20.0          # flyweight swing, degrees at full advance


def build_parts(app, only=None):
    builders = [
        ("housing", P.housing), ("cover", P.window_cover),
        ("bearing", P.bearing), ("camshaft", P.camshaft),
        ("tappet", P.tappet), ("pin", P.tappet_pin), ("roller", P.roller),
        ("seat", P.spring_seat), ("spring", P.plunger_spring),
        ("plunger", P.plunger), ("barrel", P.barrel),
        ("sleeve", P.control_sleeve), ("vseat", P.valve_seat),
        ("valve", P.delivery_valve), ("vspring", P.valve_spring),
        ("holder", P.valve_holder), ("rack", P.control_rack),
        ("feed_tappet", P.feed_tappet), ("feed_body", P.feed_pump_body),
        ("primer", P.primer), ("primer_tube", P.primer_tube),
        ("banjo", P.banjo), ("z_cover", P.z_cover), ("clamp", P.clamp_plate),
        ("screw", P.slotted_screw), ("gov_housing", P.governor_housing),
        ("gov_cover", P.governor_cover), ("filler_cap", P.filler_cap),
        ("nameplate", P.nameplate), ("stop_lever", P.stop_lever),
        ("membrane", P.membrane),
        ("plate", P.diaphragm_plate), ("gov_spring", P.governor_spring),
        ("hub", P.timing_hub), ("nut", P.camshaft_nut),
        ("flyweight", P.flyweight), ("sprocket", P.drive_flange),
    ]
    parts, times = {}, {}
    for key, fn in builders:
        if only and key not in only:
            continue
        t = time.perf_counter()
        parts[key] = fn(app)
        times[key] = round(time.perf_counter() - t, 2)
        print(f"  {key:12s} {times[key]:6.2f} s", flush=True)
    return parts, times


def assemble(app, parts):
    """The pump, mated and coupled, in its own assembly, with the housing
    fixed. An engine places that assembly as one FLEXIBLE occurrence
    (docs/assemblies/README.md#flexible-sub-assemblies), fastens it by the
    housing's `flange` and drives the camshaft's own mate; the rack is held
    by the configuration it runs under (configure()). Returns the rig, the
    mates an engine drives or reads, and the occurrence ids the checks
    use — ids in THIS assembly, which the engine names by path."""
    rig = app.assembly(ASSEMBLY, create=True)
    rig.activate()
    ids = {}
    ho = rig.place(parts["housing"], fixed=True, name="pump housing")
    ids["housing"] = ho.id

    def fixed_to(occ, frame, part, name, pframe=None):
        o = rig.place(part, name=name)
        rig.fasten((occ, frame), (o, pframe or frame))
        return o

    fixed_to(ho, "origin", parts["cover"], "inspection cover")
    zc = fixed_to(ho, "origin", parts["z_cover"], "Z cover")
    for k in range(4):
        fixed_to(zc, f"screw{k + 1}", parts["screw"], f"Z cover screw {k + 1}",
                 "seat")
    for k in range(2):
        cp = fixed_to(ho, f"clamp{k + 1}", parts["clamp"],
                      f"holder clamp plate {k + 1}", "seat")
        fixed_to(cp, "screw", parts["screw"], f"clamp plate screw {k + 1}",
                 "seat")
    for n in ("drive", "governor"):
        o = rig.place(parts["bearing"], name=f"camshaft bearing, {n} end")
        rig.fasten((ho, f"bearing_{n}"), (o, "seat"))
    cam = rig.place(parts["camshaft"], name="pump camshaft")
    cam_m = rig.revolute((ho, "cam_axis"), (cam, "axle"), name="pump camshaft")
    ids["camshaft"] = cam.id
    rack = rig.place(parts["rack"], name="control rack")
    rack_m = rig.slider((ho, "rack"), (rack, "slide"), name="control rack")
    ids["rack"] = rack.id

    rig.activate()
    for i in range(N_EL):
        el, fr = i + 1, f"el{i + 1}"
        tp = rig.place(parts["tappet"], name=f"tappet {el}")
        tm = rig.slider((ho, fr), (tp, "axis"), name=f"tappet {el}")
        rig.cam(cam_m, tm, lift=table(lambda t: plunger_lift(el, t)),
                name=f"cam lobe {el}")
        rig.fasten((tp, "pin"), (rig.place(parts["pin"], name=f"tappet pin {el}"),
                                 "pin"))
        ro = rig.place(parts["roller"], name=f"tappet roller {el}")
        rm = rig.revolute((tp, "pin"), (ro, "axle"), name=f"roller {el}")
        rig.activate()
        # Rolls without slip on the base circle: the approximation the roller
        # keeps over the lobe too.
        app.add_relation(type="gear", a=cam_m["id"], b=rm["id"],
                         ratio=-CAM_BASE_R / ROLLER_R, name=f"roller {el} roll")
        fixed_to(tp, "axis", parts["seat"], f"lower spring seat {el}")
        fixed_to(tp, "axis", parts["spring"], f"plunger spring {el}")
        fixed_to(ho, fr, parts["barrel"], f"barrel {el}", "axis")
        sl = rig.place(parts["sleeve"], name=f"control sleeve {el}")
        sm = rig.revolute((ho, fr), (sl, "axis"), name=f"control sleeve {el}")
        rig.rack(sm, rack_m, PITCH_R, name=f"sleeve {el} on rack")
        pl = rig.place(parts["plunger"], name=f"plunger {el}")
        pm = rig.cylindrical((ho, fr), (pl, "axis"), name=f"plunger {el}")
        rig.cam(cam_m, pm, lift=table(lambda t: plunger_lift(el, t)),
                dof_b="offset", name=f"plunger {el} lift")
        rig.rack(pm, rack_m, PITCH_R, name=f"plunger {el} on rack")
        fixed_to(ho, fr, parts["vseat"], f"delivery valve seat {el}", "axis")
        dv = rig.place(parts["valve"], name=f"delivery valve {el}")
        dm = rig.slider((ho, fr), (dv, "axis"), name=f"delivery valve {el}")
        rig.cam(cam_m, dm, lift=table(lambda t: valve_lift(el, t)),
                name=f"delivery valve {el} lift")
        fixed_to(ho, fr, parts["vspring"], f"delivery valve spring {el}", "axis")
        hd = fixed_to(ho, fr, parts["holder"], f"delivery valve holder {el}",
                      "axis")
        ids[f"holder{el}"] = hd.id
        ids[f"tappet{el}"], ids[f"plunger{el}"] = tp.id, pl.id
        ids[f"sleeve{el}"], ids[f"valve{el}"] = sl.id, dv.id
        ids[f"plunger_mate{el}"], ids[f"sleeve_mate{el}"] = pm["id"], sm["id"]

    ft = rig.place(parts["feed_tappet"], name="feed pump tappet")
    fm = rig.slider((ho, "feed"), (ft, "slide"), name="feed pump tappet")
    rig.cam(cam_m, fm, lift=table(feed_lift), name="feed pump eccentric")
    ids["feed"] = ft.id
    fb = fixed_to(ho, "origin", parts["feed_body"], "feed pump body", "mount")
    fixed_to(fb, "primer", parts["primer_tube"], "hand primer tube", "mount")
    for n in ("inlet", "outlet"):
        fixed_to(fb, n, parts["banjo"], f"feed pump {n} banjo", "seat")
    pr = rig.place(parts["primer"], name="hand primer")
    prime_m = rig.slider((fb, "primer"), (pr, "slide"), name="hand primer")
    ids["primer"] = pr.id

    for key, name in (("gov_housing", "governor housing"),
                      ("gov_cover", "governor cover"),
                      ("nameplate", "nameplate"), ("stop_lever", "stop lever"),
                      ("membrane", "governor diaphragm rim"),
                      ("gov_spring", "governor spring")):
        o = fixed_to(ho, "governor", parts[key], name, "mount")
        if key == "gov_housing":
            fixed_to(o, "filler", parts["filler_cap"], "governor filler cap",
                     "seat")
    fixed_to(rack, "governor", parts["plate"], "governor diaphragm", "mount")

    hub = fixed_to(cam, "axle", parts["hub"], "timing device hub")
    fixed_to(cam, "axle", parts["nut"], "camshaft nut")
    wa = rig.place(parts["flyweight"], name="flyweight A")
    wam = rig.revolute((hub, "pin_a"), (wa, "pivot"), name="flyweight A")
    wb = rig.place(parts["flyweight"], name="flyweight B")
    wbm = rig.revolute((hub, "pin_b"), (wb, "pivot"), name="flyweight B")
    rig.chain(wam, wbm, 1, 1, name="flyweights together")
    sp = rig.place(parts["sprocket"], name="timing device sprocket")
    spm = rig.revolute((hub, "axle"), (sp, "axle"), name="injection advance")
    rig.activate()
    app.add_relation(type="gear", a=wam["id"], b=spm["id"],
                     ratio=-ADVANCE_MAX / WEIGHT_SWING,
                     name="flyweights advance the injection")
    ids["weightA"], ids["sprocket"], ids["hub"] = wa.id, sp.id, hub.id
    mates = dict(cam=cam_m, rack=rack_m, prime=prime_m, weight=wam,
                 advance=spm)
    return rig, mates, ids


def configure(app, rig, mates):
    rig.activate()
    cfg = {}
    for name, offset in LOAD_CONFIGS:
        c = app.call("add_configuration", name=name)["id"]
        app.call("set_configuration", id=c, mate_overrides=[
            {"mate": mates["rack"]["id"], "offset": offset}])
        cfg[name] = c
    return cfg


def studies(app, rig, mates, cfg, samples):
    """Each study names the configuration it measures: the camshaft turn is
    a FULL-LOAD turn whatever the tab has active, and the rack sweep runs
    under the base, where nothing holds the rack."""
    rig.activate()
    s = {}
    s["run"] = rig.motion(mates["cam"], dof="rotation", **{"from": 0.0},
                          to=360.0, samples=samples, clash_mode="exact",
                          check_clash=True, configuration=cfg["Full load"],
                          name="camshaft, one turn at full load")
    s["load"] = rig.motion(mates["rack"], dof="offset", **{"from": 0.0},
                           to=RACK_TRAVEL, samples=16, clash_mode="exact",
                           check_clash=True, configuration=0,
                           name="control rack, stop to full load")
    s["advance"] = rig.motion(mates["weight"], dof="rotation", **{"from": 0.0},
                              to=-WEIGHT_SWING, samples=11, clash_mode="exact",
                              check_clash=True, name="timing device, full advance")
    s["prime"] = rig.motion(mates["prime"], dof="offset", **{"from": 0.0},
                            to=15.0, samples=6, clash_mode="exact",
                            check_clash=True, name="hand primer stroke")
    return {k: v["id"] for k, v in s.items()}


def pose(sample, occ_id):
    return next(p for p in sample["poses"] if p["occurrence"] == occ_id)


def mate_value(sample, mate_id, key):
    return next(m[key] for m in sample["mates"] if m["mate"] == mate_id)


def run_study(app, sid, label):
    t = time.perf_counter()
    r = app.run_motion_study(study=sid, include_poses=True, _timeout=7200)
    dt = time.perf_counter() - t
    bad = [x for x in r["samples"] if not x.get("solved", True)]
    clashes = [(x["value"], c["a_name"], c["b_name"], c.get("volume"))
               for x in r["samples"] for c in x.get("clashes", [])]
    print(f"  {label}: {len(r['samples'])} samples, {r.get('pairs_checked')} "
          f"pair checks, {dt:.1f} s, clean={r.get('clean')}", flush=True)
    for c in clashes[:30]:
        print(f"    CLASH at {c[0]:.2f}: {c[1]} x {c[2]} vol={c[3]}")
    if bad:
        print(f"    UNSOLVED samples: {[x['value'] for x in bad]}")
    return r, dt, clashes, bad


def check_run(r, ids):
    """Stroke and firing order off the poses, not off the inputs."""
    out, peaks = {}, {}
    for el in range(1, N_EL + 1):
        zs = [pose(s, ids[f"plunger{el}"])["position"][2] for s in r["samples"]]
        tz = [pose(s, ids[f"tappet{el}"])["position"][2] for s in r["samples"]]
        vz = [pose(s, ids[f"valve{el}"])["position"][2] for s in r["samples"]]
        out[f"plunger {el} stroke"] = round(max(zs) - min(zs), 4)
        out[f"tappet {el} stroke"] = round(max(tz) - min(tz), 4)
        out[f"delivery valve {el} lift"] = round(max(vz) - min(vz), 4)
        k = max(range(len(zs)), key=lambda i: zs[i])
        peaks[el] = r["samples"][k]["value"]
    order = [el for el, _ in sorted(peaks.items(), key=lambda kv: kv[1])]
    first = order.index(1)
    order = order[first:] + order[:first]
    fy = [pose(s, ids["feed"])["position"][1] for s in r["samples"]]
    out["feed pump stroke"] = round(max(fy) - min(fy), 4)
    out["plunger peaks (cam deg)"] = peaks
    out["firing order"] = "-".join(map(str, order))
    return out


def check_load(r, ids):
    out = {}
    first, last = r["samples"][0], r["samples"][-1]
    rx = [pose(s, ids["rack"])["position"][0] for s in (first, last)]
    out["rack travel"] = round(abs(rx[1] - rx[0]), 4)
    expected = math.degrees(RACK_TRAVEL / PITCH_R)
    for el in range(1, N_EL + 1):
        # The mates' own values: what the rack turned, straight off the study.
        for what in ("plunger", "sleeve"):
            m = ids[f"{what}_mate{el}"]
            turn = (mate_value(last, m, "rotation_deg")
                    - mate_value(first, m, "rotation_deg"))
            out[f"{what} {el} turn (deg)"] = round(turn, 3)
    out["expected turn (deg)"] = round(expected, 3)
    out["effective stroke at stop / idle / full (mm)"] = [
        round(effective_stroke(math.degrees(o / PITCH_R)), 3)
        for _, o in LOAD_CONFIGS]
    return out


def occ(app, name):
    return next(o for o in app.list_occurrences()["occurrences"]
                if o["name"] == name)


def renders(app, cfg, out, still):
    """The pictures the write-up uses, all from the saved document."""
    app.call("set_active_configuration", id=cfg["Full load"])
    shots = []

    def shot(name, eye, target, **section):
        app.set_camera(up=[0, 0, 1], eye=eye, target=target)
        path = os.path.join(out, name)
        app.screenshot(path=path)
        shots.append(path)

    shot("exterior_drive_end.png", [-260, -300, 240], [10, 0, 50])
    shot("exterior_governor_end.png", [220, 330, 230], [10, 0, 50])
    # The element row cut along its axes, seen from the feed-pump side.
    app.set_section(base_plane="xz", offset=0.0, enabled=True, reverse=True)
    shot("section_element_row.png", [0, -420, 60], [0, 0, 55])
    # One element across its axis, stop and full load: the helix at the port.
    for name, c in (("stop", "Stop"), ("full", "Full load")):
        app.call("set_active_configuration", id=cfg[c])
        app.set_section(base_plane="yz", offset=EL_X[0], enabled=True,
                        reverse=True)
        shot(f"element_1_{name}.png", [EL_X[0] - 80, -40, 100],
             [EL_X[0], 0, 92])
    app.set_section(enabled=False)
    app.call("set_active_configuration", id=cfg["Full load"])
    cover = occ(app, "inspection cover")
    app.set_occurrence_visible(occurrence=cover["id"], visible=False)
    shot("control_rack_window.png", [60, 200, 120], [0, 10, 77])
    app.set_occurrence_visible(occurrence=cover["id"], visible=True)
    if still:
        app.set_camera(up=[0, 0, 1], eye=[-260, -300, 240], target=[10, 0, 50])
        path = os.path.join(out, "hero.png")
        app.render_still(path=path, width=1600, height=1200, samples=256,
                         fit=True, _timeout=1800)
        shots.append(path)
    return shots


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "scratch",
        "pes_pump"))
    ap.add_argument("--samples", type=int, default=73)
    ap.add_argument("--parts", nargs="*", help="build only these parts, then stop")
    ap.add_argument("--no-studies", action="store_true")
    ap.add_argument("--no-still", action="store_true",
                    help="skip the path-traced hero render")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    app = connect(a.port)
    ping = app.call("ping")
    if ping.get("dirty"):
        raise SystemExit(f"refused: the project on port {ping.get('port')} has "
                         "unsaved changes; use a scratch instance")
    app.new()
    t0 = time.perf_counter()
    print("parts:", flush=True)
    parts, times = build_parts(app, a.parts)
    if a.parts:
        return
    first = app.call("list_parts")["parts"][0]
    if first.get("name") == "Part 1":
        app.call("delete_part", part=first["id"])
    print("assembly:", flush=True)
    rig, mates, ids = assemble(app, parts)
    solved = rig.solve()
    print(f"  solve: dof={solved.get('dof')} residual={solved.get('residual_mm')} "
          f"errored={solved.get('errored_mates')}", flush=True)
    cfg = configure(app, rig, mates)
    sids = studies(app, rig, mates, cfg, a.samples)
    t_build = time.perf_counter() - t0
    app.save_project(path=os.path.join(out, "pes_pump-unchecked.dsgn"))
    print(f"  built in {t_build:.1f} s", flush=True)
    if a.no_studies:
        return
    report = {"build_s": round(t_build, 1), "part_s": times}
    failures = []

    # The standing sweep: every pair, rigid ones included, which a motion
    # study skips by design — the clamp plates, their screws and the
    # holders are all fastened to the housing, and only this sees them.
    rig.activate()
    t = time.perf_counter()
    st = app.interference(mode="exact", max_pairs=20000, deadline_s=300,
                          _timeout=900)
    stand = [(c.get("a_name"), c.get("b_name"), c.get("volume"))
             for c in st.get("pairs", [])]
    report["standing"] = dict(seconds=round(time.perf_counter() - t, 1),
                              pairs_checked=st.get("pairs_checked"),
                              truncated=bool(st.get("truncated")),
                              clashes=stand)
    print(f"  standing: {st.get('pairs_checked')} pairs, "
          f"{len(stand)} clashes", flush=True)
    for c in stand:
        print(f"    CLASH {c[0]} x {c[1]} vol={c[2]}")
    if st.get("truncated") or stand:
        failures.append("standing: clashes or truncated")

    r, dt, cl, bad = run_study(app, sids["run"], "run (full load)")
    report["run"] = dict(check_run(r, ids), seconds=round(dt, 1),
                         clashes=len(cl), unsolved=len(bad),
                         pairs_checked=r.get("pairs_checked"))
    failures += cl and ["run: clashes"] or []
    for key, label in (("load", "load"), ("advance", "advance"),
                       ("prime", "prime")):
        r2, dt, cl, bad = run_study(app, sids[key], label)
        rep = dict(seconds=round(dt, 1), clashes=len(cl), unsolved=len(bad),
                   pairs_checked=r2.get("pairs_checked"))
        if key == "load":
            rep.update(check_load(r2, ids))
        report[key] = rep
        failures += cl and [f"{key}: clashes"] or []

    run = report["run"]
    for el in range(1, N_EL + 1):
        if abs(run[f"plunger {el} stroke"] - STROKE) > 0.01:
            failures.append(f"plunger {el} stroke {run[f'plunger {el} stroke']}")
    if run["firing order"] != "1-3-4-2":
        failures.append(f"firing order {run['firing order']}")
    ld = report["load"]
    for el in range(1, N_EL + 1):
        for what in ("plunger", "sleeve"):
            got = abs(ld[f"{what} {el} turn (deg)"])
            if abs(got - ld["expected turn (deg)"]) > 0.05:
                failures.append(f"{what} {el} turned {got}")
    report["failures"] = failures
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    print(json.dumps(report, indent=2))
    if failures:
        raise SystemExit(f"FAILED: {failures}")
    rig.activate()
    app.call("set_active_configuration", id=cfg["Full load"])
    app.save_project(path=os.path.join(out, "pes_pump.dsgn"))
    ex = app.export_motion(study=sids["run"], path=os.path.join(out, "pes_pump.glb"),
                           _timeout=1800)
    print(f"  GLB: {ex}")
    for path in renders(app, cfg, out, not a.no_still):
        print(f"  render: {path}")


if __name__ == "__main__":
    main()
