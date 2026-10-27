#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Stage 6: the yoke under load, and what topology made of it. Opens
stages/assembly.lob, saves stages/yoke_topo.lob (the head with the
studies, the solid yoke, the envelope and its topology result, and the print
pieces), writes yoke_topo_report.json, the pictures and the pieces' STLs.

    python3 generators/pantilt/build_yoke_topo.py --port N

- The load: the camera at 3 g, followed down its path (yoke_loads.py) --
  camera, cradle, tilt shaft, the arms' bearings, and the worm pair's tooth
  force into the ledge; the yoke fixed where it sits on the pan bearings.
  Three cases: 3 g down level (the working pose), 3 g down at the tilt
  where the load's lever is longest (the gear's worst), 3 g sideways.
- Static studies of the SOLID yoke (`yoke_solid`, the phase-5 design, built
  here from build_yoke.yoke) and of the head's yoke (`yoke`, the design
  re-modelled over the topology result): the maxima, the factor of safety,
  and the lens -- carried from what the two bearing seats and the ledge did
  (probe_study), since the lens is not in the study.
- A topology study of the ENVELOPE (`yoke_envelope`): the base and arms as
  wide as the space around them allows, the tilt and pan worms' blocks
  (and the tilt blocks' nuts under the ledge) as keep-outs, the seats and
  the screw holes kept, and all three cases at once (load cases, equal
  weights). Its volume fraction is the solid yoke's volume in what the
  keep-outs leave: the stiffest layout of the same material, which is what
  the target -- the lens at most a third of the solid yoke's at 3 g, level
  -- is asked of.
- The result checked before anything is modelled from it
  (`check_topology`): a static study of its shape and of the solid design
  domain, every case, and the lens carried from the shape's seats as it is
  for the yokes.
- The head's yoke cut into the pieces it prints as (`split_for_printing`
  at the base's top), each checked against the bed (`BED_MM`).
- Pictures: the density and the shape (viewport), stills of both yokes.
"""
import argparse
import json
import math
import time

import common
import spec
import build_yoke
import yoke_loads

REPORT = {}
# The last run's report: what a reused topology result was run with.
_previous = common.OUT / "yoke_topo_report.json"
REPORT_PREVIOUS = json.loads(_previous.read_text()) if _previous.exists() \
    else {}
# SIMP's design settles in tens of iterations; the CPU pays for each.
TOPO_ITERATIONS = 30
# The printer: a 256 mm cube (Bambu X1/P1 class). The pieces also fit a
# 250 x 210 x 220 bed (Prusa MK4) as they lie; the yoke whole would not
# print well on either (no orientation without supports under a 220 mm U).
BED_MM = 256.0


def faces_of(part, S, model):
    """The faces the load and the supports are on, found by what they are
    (the same on the solid, the envelope and the re-modelled yoke)."""
    fs = part.body().faces()
    D = model("hole", S["main_brg_D"]) / 2
    zt = S["tilt_z"]

    def one(what, pred):
        hits = [f for f in fs if pred(f)]
        if len(hits) != 1:
            raise SystemExit(f"{part.name}: {len(hits)} faces for {what}")
        return hits[0]

    def cyl(f, r):
        return f.kind == "cylinder" and abs(f.radius - r) < 1e-3

    def plane_at(f, axis, v, sign):
        return (f.kind == "plane" and abs(f.centroid[axis] - v) < 1e-2
                and f.normal[axis] * sign > 0.5)
    out = {
        "bore_pos": one("the +Y bearing bore",
                        lambda f: cyl(f, D) and f.centroid[1] > 0),
        "bore_neg": one("the -Y bearing bore",
                        lambda f: cyl(f, D) and f.centroid[1] < 0),
        "shoulder_pos": one(
            "the +Y bearing's shoulder",
            lambda f: plane_at(f, 1, S["arm_out"] - S["main_brg_B"], 1)
            and abs(f.centroid[2] - zt) < 1),
        "shoulder_neg": one(
            "the -Y bearing's shoulder",
            lambda f: plane_at(f, 1, -(S["arm_out"] - S["main_brg_B"]), -1)
            and abs(f.centroid[2] - zt) < 1),
        "ledge": one("the ledge's top",
                     lambda f: plane_at(f, 2, S["ledge_z"], 1)),
        # Above the ring groove's land, where the head's yoke has one.
        "journal": one("the pan journal",
                       lambda f: cyl(f, model("shaft", S["pan_brg_d"]) / 2)
                       and f.centroid[2] > S["base_t"] - S["pan_brg_B"]),
        "collar": one("the pan collar's face",
                      lambda f: plane_at(f, 2, S["base_t"] + S["wheel_face"],
                                         -1)),
    }
    relief = model("hole", S["relief_d"]) / 2
    out["reliefs"] = [f for f in fs if cyl(f, relief)]
    out["screws"] = [f for f in fs if cyl(f, model("hole", S["m4_clear"]) / 2)]
    return out


def cases(S, F):
    G = yoke_loads.geometry(S, F["bore_pos"].centroid[1],
                            F["bore_neg"].centroid[1])
    g = S["g_handling"]
    worst = yoke_loads.worst_tilt_deg(S)
    return G, [
        yoke_loads.case(S, G, "3 g down, level", 0.0, (0, 0, -g)),
        yoke_loads.case(S, G, f"3 g down, tilted {worst:.1f} deg", worst,
                        (0, 0, -g)),
        yoke_loads.case(S, G, "3 g sideways, level", 0.0, (0, -g, 0)),
    ]


def mesh_size(S):
    """Three tet10s through an arm's thickness."""
    return S["arm_t"] / 3


def add_loads(app, sid, F, c, case=None, supports=True):
    """The supports (unless `supports` is False: a second case shares
    them) and case `c`'s forces, in load case `case` when given."""
    if supports:
        for k in ("journal", "collar"):
            app.add_load(study=sid, type="fixed", face=F[k].ref)
    for key, face in (("bore_pos", "bore_pos"), ("bore_neg", "bore_neg"),
                      ("shoulder_pos", "shoulder_pos"), ("ledge", "ledge")):
        v = c["loads"][key]
        mag = math.sqrt(sum(x * x for x in v))
        if mag < 1e-9:
            continue
        extra = {"case": case} if case else {}
        # Per kilogram of camera, times its variable: every force on the
        # yoke is the load's weight times a lever that does not move with
        # the mass (the CoM stands where com_fwd and com_up put it), so a
        # heavier camera is a heavier load on the same study, and the
        # study's input hash moves with it.
        app.add_load(study=sid, type="force", face=F[face].ref,
                     magnitude=f"load_kg * {mag / c['load_kg']:.15g}",
                     direction=list(v), **extra)


def keep_outs(S):
    """The worms' blocks, in the yoke's coordinates (build_yoke's frames:
    a block's bore along its frame's z, its part x along the frame's z, its
    part z along the frame's x), each with the running gap round it: the
    tilt blocks standing on the ledge, the nuts of their screws under it,
    and the pan blocks hanging under the base. Boxes, [min, max] in mm."""
    s, a = S["worm_span"], S["centre_dist"]
    bt, bw, bh, g = S["block_t"], S["block_w"], S["block_h"], S["run_gap"]
    yw, lz, tb = S["tilt_mesh_y"], S["ledge_z"], S["yoke_base_t"]
    zw = S["tilt_z"] - a
    zm = S["z_mesh_pan"]
    out = []
    for sx in (1, -1):
        x0, x1 = sorted((sx * (s - g), sx * (s + bt + g)))
        y0, y1 = yw - bw / 2 - g, yw + bw / 2 + g
        out.append(("tilt block", [x0, y0, lz], [x1, y1, zw + bh / 2 + g]))
        out.append(("tilt block nuts", [x0, y0, lz - 2 * tb],
                    [x1, y1, lz - tb]))
    for sy in (1, -1):
        # No gap above: a pan block is screwed up against the base.
        y0, y1 = sorted((sy * (s - g), sy * (s + bt + g)))
        out.append(("pan block", [a - bw / 2 - g, y0, zm - bh / 2 - g],
                    [a + bw / 2 + g, y1, zm + bh / 2]))
    return out


def under(p, box):
    """Whether point `p` lies in `box`'s footprint (x and y): a screw hole
    there goes down into the block or its nuts."""
    return all(lo <= c <= hi for c, lo, hi in zip(p[:2], box[1], box[2]))


def wait(app, sid, what, poll=2.0):
    t0 = time.monotonic()
    while True:
        st = app.study_status(id=sid)
        if not st.get("running"):
            break
        time.sleep(poll)
    if st.get("error"):
        raise SystemExit(f"{what}: {st['error']}")
    return time.monotonic() - t0


def statics(app, part, S, F, G, cases, clock):
    """One static study of `part` with every case in it (load cases: one
    factorisation, a right-hand side each -- docs/topopt/README.md
    #load-cases): per case the maxima, the factor of safety, and the
    lens."""
    part.activate()
    sid = app.add_study(type="static", name=f"{part.name}: 3 g")["id"]
    app.set_study(id=sid, mesh_size=mesh_size(S),
                  load_cases=[{"name": c["name"]} for c in cases])
    for i, c in enumerate(cases):
        add_loads(app, sid, F, c, case=c["name"], supports=i == 0)
    app.run_study(id=sid, **{"async": True})
    seconds = wait(app, sid, part.name)
    r = app.study_results(id=sid)
    rows = []
    for c, rc in zip(cases, r["cases"]):
        lens = lens_of(app, sid, F, G, c, c["name"])
        probe = {k: app.probe_study(id=sid, face=F[k].ref, case=c["name"])
                 for k in ("bore_pos", "bore_neg")}
        ledge = app.probe_study(id=sid, face=F["ledge"].ref,
                                at=list(G["worm"]), case=c["name"])
        vm = rc["max_von_mises_mpa"]["value"]
        out = {
            "part": part.name, "case": c["name"], "study": sid,
            "elements": r["elements"], "seconds": round(seconds, 1),
            "max_displacement_mm": rc["max_displacement_mm"]["value"],
            "max_displacement_at": rc["max_displacement_mm"]["at"],
            "max_von_mises_mpa": vm,
            "max_von_mises_at": rc["max_von_mises_mpa"]["at"],
            "fos_yield": rc.get("factor_of_safety"),
            # A print's weak direction is across its layers.
            "fos_layers": S["petg_layer_mpa"] / vm if vm > 0 else None,
            "bore_pos_mm": probe["bore_pos"]["translation_mm"],
            "bore_neg_mm": probe["bore_neg"]["translation_mm"],
            "worm_x_mm": ledge["displacement_at_mm"][0],
            **lens,
            # What the study cannot see: the pan journal bending under it.
            "journal": yoke_loads.journal(S, G, c),
        }
        j = out["journal"]
        print(f"  {part.name}, {c['name']}: max "
              f"{out['max_displacement_mm']:.3f} mm, lens "
              f"{out['lens_magnitude_mm']:.3f} mm "
              f"(pitch {out['camera_pitch_arcmin']:.2f}'), vm {vm:.2f} MPa, "
              f"FoS {out['fos_yield']:.1f} (layers {out['fos_layers']:.1f}); "
              f"the journal, by beam: {j['stress_mpa']:.1f} MPa, lens "
              f"{j['lens_mm']:.2f} mm more", flush=True)
        rows.append(out)
    clock(f"statics {part.name}")
    return rows


# The most the lens may move at 3 g, in any of the three cases. The yoke as
# designed moves it 1.5 / 2.9 / 3.2 mm (level, tilted, sideways) on a rigid
# journal, a fifth under this; a camera a third heavier than the 1.5 kg the
# head is sized for is past it sideways.
LENS_MAX_3G_MM = 4.0


def lens_requirements(app, yoke, S, F, cases, sid, rows):
    """The lens at 3 g, one requirement per case, measured by the project
    from the yoke's study (yoke_loads.lens_requirement). Each formula is
    checked here against the lens this script carried from the same probes,
    so the two cannot drift apart."""
    for c, row in zip(cases, rows):
        readings, expression = yoke_loads.lens_requirement(S, c, F)
        app.require(f"lens at 3 g, {c['name']}", {
            "kind": "study_probe", "document": yoke.id, "study": sid,
            "case": c["name"], "readings": readings,
            "expression": expression, "unit": "mm"},
            at_most=LENS_MAX_3G_MM,
            source="a 3 g knock in handling must not move the lens more "
                   "than this; the yoke on a rigid journal (its own bend is "
                   "the README's beam estimate, beside it)")
    out = app.verify_requirements(raise_on_failure=False)["requirements"]
    for c, row in zip(cases, rows):
        got = next(x for x in out if x["name"] == f"lens at 3 g, {c['name']}")
        if "value" not in got or abs(
                got["value"] - row["lens_magnitude_mm"]) > 1e-6 * max(
                    1.0, row["lens_magnitude_mm"]):
            raise SystemExit(f"the lens requirement's formula reads "
                             f"{got.get('value')} mm, this script "
                             f"{row['lens_magnitude_mm']} mm: {got}")


def fresh(app, name):
    """Drop a part a previous run of this stage left, so it is rebuilt."""
    for p in app.call("list_parts")["parts"]:
        if p["name"] == name:
            app.call("delete_part", part=p["id"])


def mass_g(app, part):
    return app.mass_properties(part=part.id)["total"]["mass_g"]


def volume(app, part):
    return app.mass_properties(part=part.id)["total"]["volume_mm3"]


def topo_step(app, S, G, cases, target_volume, fresh_run, clock):
    """The topology study, or the one `stages/yoke_envelope.lob` holds.

    A run is minutes on the GPU (two hours on the CPU when its solver could
    not converge), so its project is kept: when the envelope built now has
    the volume the saved one has (it is the same envelope) and the saved
    study carries the same cases, its result is reused -- pictured,
    checked and reported again, not re-run. Called last, on the session
    saved as stages/yoke_topo.lob: a fresh run adds the envelope to it and
    saves that as stages/yoke_envelope.lob."""
    saved = common.stage("yoke_envelope")
    fresh(app, "yoke_envelope")
    env = build_yoke.yoke(app, dict(S), name="yoke_envelope",
                          design="envelope")
    v_now = volume(app, env)
    names = [c["name"] for c in cases]
    if saved.exists() and not fresh_run:
        app.call("open_project", path=str(saved))
        old = app.part("yoke_envelope", create=False)
        v_old = volume(app, old)
        study = next((s for s in app.call("list_studies")["studies"]
                      if s["name"] == "yoke envelope" and s["has_results"]),
                     None)
        # Stale is not reusable: check_topology refuses it, and so should
        # the pictures (a reopened part that hashes differently reads stale;
        # docs/simulation/README.md#studies-as-jobs).
        boxes = [{"what": w, "min": lo, "max": hi}
                 for w, lo, hi in keep_outs(S)]
        same_cases = study is not None and not study["stale"] and [
            c["name"] for c in study.get("load_cases", [])] == names and \
            REPORT_PREVIOUS.get("topology", {}).get("keep_out_boxes") == boxes
        if same_cases and abs(v_old - v_now) <= 1e-6 * v_now:
            r = app.study_results(id=study["id"])
            r.pop("receipt", None)
            prev = REPORT_PREVIOUS.get("topology", {})
            REPORT["topology"] = {
                **{k: prev.get(k) for k in (
                    "envelope_volume_mm3", "envelope_mass_g",
                    "volume_fraction", "mesh_size_mm", "filter_radius_mm",
                    "keep_faces", "keep_out_boxes", "cases", "weights",
                    "seconds")},
                "study": study["id"], "reused_from": str(saved), **r}
            print(f"  topology: reused {saved.name} (the same envelope, "
                  f"{v_now:.0f} mm3): {r['iterations']} iterations, "
                  f"compliance {r['compliance']:.1f}", flush=True)
            REPORT["topology_pictures"] = pictures(app, old, study["id"])
            REPORT["topology_check"] = check(app, S, old, G, cases,
                                             study["id"])
            clock("topology (reused)")
            return
        print(f"  topology: {saved.name} is for another envelope or other "
              f"cases ({v_old:.0f} against {v_now:.0f} mm3): running it",
              flush=True)
        app.call("open_project", path=str(common.stage("yoke_topo")))
        env = build_yoke.yoke(app, dict(S), name="yoke_envelope",
                              design="envelope")
    env, REPORT["topology"] = topology(app, S, env, cases, target_volume,
                                       clock)
    sid = REPORT["topology"]["study"]
    REPORT["topology_pictures"] = pictures(app, env, sid)
    REPORT["topology_check"] = check(app, S, env, G, cases, sid)
    clock("topology check")
    common.save(app, "yoke_envelope")


def topology(app, S, env, cases, target_volume, clock):
    """The envelope, optimised for all three cases together; returns the
    study and its summary."""
    model = common.printed(env)
    F = faces_of(env, S, model)
    v_env = volume(app, env)
    env.activate()
    sid = app.add_study(type="topology", name="yoke envelope")["id"]
    # Two tet10s through an arm's 12 mm (quadratic elements bend well with
    # that), and a filter twice the size: the smallest member it leaves is a
    # printable strut. At arm_t / 1.5 (8 mm) the members came out one or two
    # tets wide, and the -Y arm's strut met its boss at tet edges: the
    # static of the shape (check_topology) could not reach its bearing seat
    # through a face. On the GPU, refined to fp64, an iteration is a few
    # seconds a case (docs/topopt/README.md#mixed-precision-refinement).
    h = S["arm_t"] / 2
    # The cases start at equal weights; the probe run below rescales them
    # (docs/topopt/README.md#load-cases).
    app.set_study(id=sid, mesh_size=h, filter_radius=2 * h,
                  max_iterations=TOPO_ITERATIONS,
                  load_cases=[{"name": c["name"], "weight": 1.0}
                              for c in cases])
    for i, c in enumerate(cases):
        add_loads(app, sid, F, c, case=c["name"], supports=i == 0)
    boxes = keep_outs(S)
    for _, lo, hi in boxes:
        app.call("add_keep_out", study=sid, box_min=lo, box_max=hi)
    # The kept faces; a screw hole under a keep-out (the tilt blocks'
    # screws, down through the ledge into their nuts' room) is not one: the
    # block and its nuts are what hold there.
    keep = [F[k] for k in ("bore_pos", "bore_neg", "shoulder_pos",
                           "shoulder_neg")] + F["reliefs"] + [
        f for f in F["screws"]
        if not any(under(f.centroid, b) for b in boxes)]
    for f in keep:
        app.add_keep_face(study=sid, face=f.ref)
    # The volume fraction is of the design domain the keep-outs leave
    # (docs/topopt/README.md#keep-outs): one short run measures it.
    app.set_study(id=sid, volume_fraction=0.5, max_iterations=1)
    app.run_study(id=sid)
    while app.study_status(id=sid).get("running"):
        time.sleep(0.5)
    st = app.study_status(id=sid)
    if st.get("error"):
        raise SystemExit(f"topology: {st['error']}")
    probe = app.study_results(id=sid)
    v_out = sum(k["volume_mm3"] for k in probe["keep_outs"])
    vf = target_volume / (v_env - v_out)
    # Each case weighted by the inverse of its compliance on the uniform
    # start, so each counts as much as the others: at equal weights the
    # sideways case (seven times the level one's compliance) is nearly the
    # whole objective, and the level pose -- the working one -- is left to
    # whatever the other two leave it (its lens 5.8 mm, worse than the solid
    # yoke's at the same mass).
    c0 = [c["compliance"] for c in probe["cases"]]
    mean = sum(c0) / len(c0)
    weights = [mean / c for c in c0]
    app.set_study(id=sid, volume_fraction=vf, max_iterations=TOPO_ITERATIONS,
                  load_cases=[{"name": c["name"], "weight": w}
                              for c, w in zip(cases, weights)])
    app.run_study(id=sid)
    t0 = time.monotonic()
    last = -1
    while True:
        st = app.study_status(id=sid)
        if not st.get("running"):
            break
        if st.get("iteration", -1) != last and st.get("iteration", 0) % 5 == 0:
            last = st.get("iteration", -1)
            print(f"  topology: iteration {st.get('iteration')}, compliance "
                  f"{st.get('compliance', 0):.1f}, change "
                  f"{st.get('max_change', 0):.3f}, {st.get('solver')}",
                  flush=True)
            if st.get("solver", "").startswith("cpu"):
                print("  topology: on the CPU -- slow; see "
                      "docs/topopt/README.md#the-gpu-solver", flush=True)
        time.sleep(5)
    if st.get("error"):
        raise SystemExit(f"topology: {st['error']}")
    r = app.study_results(id=sid)
    r.pop("receipt", None)
    out = {"study": sid, "envelope_volume_mm3": v_env,
           "envelope_mass_g": mass_g(app, env), "volume_fraction": vf,
           "mesh_size_mm": h, "filter_radius_mm": 2 * h,
           "keep_faces": len(keep),
           "keep_out_boxes": [{"what": w, "min": lo, "max": hi}
                              for w, lo, hi in boxes],
           "cases": [c["name"] for c in cases], "weights": weights,
           "seconds": round(time.monotonic() - t0, 1), **r}
    clock("topology")
    print("  topology:", {k: out[k] for k in (
        "iterations", "converged", "volume_fraction", "compliance", "solver",
        "seconds", "elements")}, flush=True)
    print("  topology: per case", [(c["name"], round(c["compliance"], 1))
                                   for c in r.get("cases", [])],
          "keep-outs", [(k["what"], k["tets"]) for k in r["keep_outs"]],
          "islands", r["islands"], flush=True)
    return env, out


def lens_of(app, sid, F, G, c, case):
    """What case `c` did to the lens, carried from the seats of the static
    study `sid` (its case `case`) as static() carries it."""
    probe = {k: app.probe_study(id=sid, face=F[k].ref, case=case)
             for k in ("bore_pos", "bore_neg", "shoulder_pos")}
    ledge = app.probe_study(id=sid, face=F["ledge"].ref, at=list(G["worm"]),
                            case=case)
    return yoke_loads.lens(G, c, probe["bore_pos"]["translation_mm"],
                           probe["bore_neg"]["translation_mm"],
                           ledge["displacement_at_mm"][0],
                           probe["shoulder_pos"]["translation_mm"][1])


def check(app, S, env, G, cases, sid):
    """check_topology: the optimised shape and the solid design domain,
    statically, every case, before anything is modelled from it; and the
    lens each carries, from its seats, as the yokes' studies carry it."""
    env.activate()
    model = common.printed(env)
    F = faces_of(env, S, model)
    t0 = time.monotonic()
    # Two static solves (one factorisation each, every case on it): a
    # minute or two on the yoke, as a job the app stays responsive through;
    # once it is done the same call answers from the two results.
    chk = app.call("check_topology", study=sid)
    if chk.get("started"):
        while True:
            st = app.call("study_status", job=chk["job"])
            if not st.get("running"):
                break
            time.sleep(2.0)
        if st.get("outcome") != "done":
            raise SystemExit(f"check_topology: {st}")
        chk = app.call("check_topology", study=sid)
    seconds = time.monotonic() - t0
    out = {"seconds": round(seconds, 1), "threshold": chk["threshold"],
           "threshold_lowered_from": chk["threshold_lowered_from"],
           "dropped_tets": chk["dropped_tets"], "cases": []}
    for c, row in zip(cases, chk["cases"]):
        lens_shape = lens_of(app, chk["shape_study"], F, G, c, c["name"])
        lens_solid = lens_of(app, chk["solid_study"], F, G, c, c["name"])
        out["cases"].append({**row, "lens_shape": lens_shape,
                             "lens_solid": lens_solid})
        print(f"  check {c['name']}: shape {row['shape']['mass_g']:.0f} g, "
              f"max {row['shape']['max_displacement_mm']:.2f} mm, lens "
              f"{lens_shape['lens_magnitude_mm']:.2f} mm, vm "
              f"{row['shape']['max_von_mises_mpa']:.1f} MPa | solid "
              f"{row['solid']['mass_g']:.0f} g, lens "
              f"{lens_solid['lens_magnitude_mm']:.2f} mm", flush=True)
    return out


def pictures(app, env, sid):
    """The density and the shape, as the viewport draws them, from the
    sides that show the arms' and the base's members."""
    env.activate()
    shots = []
    zt = env.value_of("yoke_zt")
    yi = env.value_of("arm_in")
    # Each arm alone (a section just inside it, looked at from outside), the
    # base from above and from below: seen whole, the two arms overlap and
    # the picture says nothing about either.
    views = (("iso", None, [1, -1, 0.7]),
             ("arm_pos", ("xz", yi - 6, True), [0, 1, 0]),
             ("arm_neg", ("xz", -(yi - 6), False), [0, -1, 0]),
             ("base_top", ("xy", zt + 2, False), [0, 0, 1]),
             ("base_under", ("xy", zt + 2, False), [0, 0, -1]))
    for field in ("density", "shape"):
        app.set_result_display(study=sid, field=field)
        for name, cut, d in views:
            if cut:
                app.set_section(base_plane=cut[0], offset=cut[1],
                                reverse=cut[2], enabled=True)
            app.set_camera(direction=d, up=[1, 0, 0] if d[2] and not d[0]
                           else [0, 0, 1])
            path = str(common.OUT / f"yoke_topo_{field}_{name}.png")
            app.screenshot(path=path, fit=True)
            shots.append(path)
            if cut:
                app.set_section(enabled=False)
    app.set_result_display(study=sid, field="none")
    stl = str(common.OUT / "yoke_topo_shape.stl")
    REPORT["topo_stl"] = app.export_topo_stl(study=sid, path=stl)
    return shots


def print_pieces(app, S, yoke):
    """The yoke as it prints: split_for_printing at the base's top
    (docs/topopt/README.md#split-for-printing) -- the base, and each arm
    with its foot flange, a part each -- checked against the bed, and each
    piece's STL."""
    zt = S["yoke_zt"]
    for p in app.call("list_parts")["parts"]:
        if p["name"].startswith("yoke_print_"):
            app.call("delete_part", part=p["id"])
    yoke.activate()
    made = app.call("split_for_printing", bed_mm=[BED_MM] * 3,
                    cuts=[{"axis": "z", "at_mm": zt}], prefix="yoke_print")
    out = []
    for pc in made["pieces"]:
        part = app.part(pc["part"], create=False)
        part.activate()
        stl = str(common.OUT / f"{pc['part']}.stl")
        app.export_stl(path=stl)
        row = {"piece": pc["part"], "mass_g": mass_g(app, part),
               "size_mm": [round(x, 1) for x in pc["size_mm"]],
               "orientation": pc["orientation"],
               "material": pc["material"], "fits_bed": pc["fits"],
               "stl": stl}
        print(f"  print piece {pc['part']}: {row['mass_g']:.0f} g, "
              f"{row['size_mm']}, {pc['orientation']['up']} up, fits "
              f"{BED_MM:g} bed: {pc['fits']}", flush=True)
        out.append(row)
    yoke.activate()
    return out


def stills(app, parts):
    """A path-traced still of each yoke, the same view."""
    shots = []
    for part in parts:
        part.activate()
        app.set_camera(direction=[1.0, -1.2, 0.8], up=[0, 0, 1])
        path = str(common.OUT / f"{part.name}.png")
        app.render_still(path=path, width=1400, height=1200, samples=96,
                         fit=True)
        shots.append(path)
    return shots


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--steps", default="solid,topo,yoke",
                    help="which of solid,topo,yoke to run (default: all)")
    ap.add_argument("--fresh-topology", action="store_true",
                    help="run the topology study even when "
                         "stages/yoke_envelope.lob holds a result for this "
                         "envelope and its cases (minutes on the GPU)")
    a = ap.parse_args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(common.stage("assembly")))
    S = spec.values(app.part("yoke", create=False))
    fresh(app, "yoke_solid")
    solid = build_yoke.yoke(app, dict(S), name="yoke_solid", design="solid")
    model = common.printed(solid)
    clock("solid yoke")
    F = faces_of(solid, S, model)
    G, CASES = cases(S, F)
    REPORT["cases"] = [{k: c[k] for k in ("name", "tilt_deg", "accel_g",
                                          "loads", "tooth_force_n", "lens")}
                       for c in CASES]
    steps = a.steps.split(",")
    if "solid" in steps:
        REPORT["solid"] = {"mass_g": mass_g(app, solid),
                           "static": statics(app, solid, S, F, G, CASES,
                                             clock)}
    if "yoke" in steps:
        yoke = app.part("yoke", create=False)
        FY = faces_of(yoke, S, model)
        REPORT["yoke"] = {"mass_g": mass_g(app, yoke),
                          "static": statics(app, yoke, S, FY, G, CASES,
                                            clock)}
        lens_requirements(app, yoke, S, FY, CASES,
                          REPORT["yoke"]["static"][0]["study"],
                          REPORT["yoke"]["static"])
        clock("lens requirements")
        REPORT["print_pieces"] = print_pieces(app, S, yoke)
        clock("print pieces")
        REPORT["stills"] = stills(app, [solid, yoke])
        clock("stills")
    common.save(app, "yoke_topo")
    if "topo" in steps:
        # The same material as the head's yoke, so the optimiser's layout
        # and the hand re-model compare directly, case by case.
        topo_step(app, S, G, CASES,
                  volume(app, app.part("yoke", create=False)),
                  a.fresh_topology, clock)
    # topo_step opens and saves yoke_envelope.lob when it has to run, so
    # the head's requirements are checked on the head's project.
    app.call("open_project", path=str(common.stage("yoke_topo")))
    REPORT["requirements"], bad = common.requirements_met(app)
    common.save(app, "yoke_topo")
    REPORT["time"] = clock.rows
    (common.OUT / "yoke_topo_report.json").write_text(
        json.dumps(REPORT, indent=1, default=str))
    common.stop_unless_met(bad)


if __name__ == "__main__":
    main()
