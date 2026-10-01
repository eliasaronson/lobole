#!/usr/bin/env python3
"""Stage 6: the yoke under load, and what topology made of it. Opens
pantilt.dsgn, saves yoke_topo.dsgn (the head with the studies, the solid
yoke, the envelope and its topology result, and the print pieces), writes
yoke_topo_report.json, the pictures and the pieces' STLs.

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
  wide as the space around them allows (inside the tilt worm's blocks
  above the ledge), the seats and the screw holes kept. Its volume fraction
  is the solid yoke's volume in the envelope: the stiffest layout of the
  same material, which is what the target -- the lens at most a third of
  the solid yoke's at 3 g, level -- is asked of.
- The head's yoke cut into the three pieces it prints as (`print_pieces`),
  each checked against the bed (`BED_MM`) in the orientation it prints in.
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
        "journal": one("the pan journal",
                       lambda f: cyl(f, model("shaft", S["main_brg_d"]) / 2)),
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


def add_loads(app, sid, F, c):
    for k in ("journal", "collar"):
        app.add_load(study=sid, type="fixed", face=F[k].ref)
    for key, face in (("bore_pos", "bore_pos"), ("bore_neg", "bore_neg"),
                      ("shoulder_pos", "shoulder_pos"), ("ledge", "ledge")):
        v = c["loads"][key]
        mag = math.sqrt(sum(x * x for x in v))
        if mag < 1e-9:
            continue
        app.add_load(study=sid, type="force", face=F[face].ref,
                     magnitude=mag, direction=list(v))


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


def static(app, part, S, F, G, c, clock):
    """One static study of `part` under case `c`: the maxima, the factor of
    safety, and the lens."""
    part.activate()
    sid = app.add_study(type="static", name=f"{part.name}: {c['name']}")["id"]
    app.set_study(id=sid, mesh_size=mesh_size(S))
    add_loads(app, sid, F, c)
    app.run_study(id=sid, **{"async": True})
    seconds = wait(app, sid, c["name"])
    r = app.study_results(id=sid)
    probe = {k: app.probe_study(id=sid, face=F[k].ref)
             for k in ("bore_pos", "bore_neg", "shoulder_pos")}
    ledge = app.probe_study(id=sid, face=F["ledge"].ref, at=list(G["worm"]))
    lens = yoke_loads.lens(G, c, probe["bore_pos"]["translation_mm"],
                           probe["bore_neg"]["translation_mm"],
                           ledge["displacement_at_mm"][0],
                           probe["shoulder_pos"]["translation_mm"][1])
    vm = r["max_von_mises_mpa"]["value"]
    out = {
        "part": part.name, "case": c["name"], "study": sid,
        "elements": r["elements"], "seconds": round(seconds, 1),
        "max_displacement_mm": r["max_displacement_mm"]["value"],
        "max_displacement_at": r["max_displacement_mm"]["at"],
        "max_von_mises_mpa": vm,
        "max_von_mises_at": r["max_von_mises_mpa"]["at"],
        "fos_yield": r.get("factor_of_safety"),
        # A print's weak direction is across its layers.
        "fos_layers": S["petg_layer_mpa"] / vm if vm > 0 else None,
        "bore_pos_mm": probe["bore_pos"]["translation_mm"],
        "bore_neg_mm": probe["bore_neg"]["translation_mm"],
        "worm_x_mm": ledge["displacement_at_mm"][0],
        **lens,
    }
    clock(f"static {part.name}: {c['name']}")
    print(f"  {part.name}, {c['name']}: max {out['max_displacement_mm']:.3f}"
          f" mm, lens {out['lens_magnitude_mm']:.3f} mm "
          f"(pitch {out['camera_pitch_arcmin']:.2f}'), vm {vm:.2f} MPa, "
          f"FoS {out['fos_yield']:.1f} (layers {out['fos_layers']:.1f})",
          flush=True)
    return out


def fresh(app, name):
    """Drop a part a previous run of this stage left, so it is rebuilt."""
    for p in app.call("list_parts")["parts"]:
        if p["name"] == name:
            app.call("delete_part", part=p["id"])


def mass_g(app, part):
    return app.mass_properties(part=part.id)["total"]["mass_g"]


def volume(app, part):
    return app.mass_properties(part=part.id)["total"]["volume"]


def topo_step(app, S, c, target_volume, fresh_run, clock):
    """The topology study, or the one `yoke_envelope.dsgn` already holds.

    A run is two hours, so its project is kept: when the envelope built
    now has the volume the saved one has (it is the same envelope), the
    saved result is reused -- pictured and reported again, not re-run.
    Called last, on the session saved as yoke_topo.dsgn: a fresh run adds
    the envelope to it and saves that as yoke_envelope.dsgn."""
    saved = common.OUT / "yoke_envelope.dsgn"
    fresh(app, "yoke_envelope")
    env = build_yoke.yoke(app, dict(S), name="yoke_envelope",
                          design="envelope")
    v_now = volume(app, env)
    if saved.exists() and not fresh_run:
        app.call("open_project", path=str(saved))
        old = app.part("yoke_envelope", create=False)
        v_old = volume(app, old)
        sid = next((s["id"] for s in app.call("list_studies")["studies"]
                    if s["name"] == "yoke envelope" and s["has_results"]),
                   None)
        if sid is not None and abs(v_old - v_now) <= 1e-6 * v_now:
            r = app.study_results(id=sid)
            r.pop("receipt", None)
            prev = REPORT_PREVIOUS.get("topology", {})
            REPORT["topology"] = {
                **{k: prev.get(k) for k in (
                    "envelope_volume_mm3", "envelope_mass_g",
                    "volume_fraction", "mesh_size_mm", "filter_radius_mm",
                    "keep_faces", "case", "seconds")},
                "study": sid, "reused_from": str(saved), **r}
            print(f"  topology: reused {saved.name} (the same envelope, "
                  f"{v_now:.0f} mm3): {r['iterations']} iterations, "
                  f"compliance {r['compliance']:.1f}", flush=True)
            REPORT["topology_pictures"] = pictures(app, old, sid)
            clock("topology (reused)")
            return
        print(f"  topology: {saved.name} is for another envelope "
              f"({v_old:.0f} against {v_now:.0f} mm3): running it",
              flush=True)
        app.call("open_project", path=str(common.OUT / "yoke_topo.dsgn"))
        env = build_yoke.yoke(app, dict(S), name="yoke_envelope",
                              design="envelope")
    env, REPORT["topology"] = topology(app, S, env, c, target_volume, clock)
    REPORT["topology_pictures"] = pictures(app, env,
                                           REPORT["topology"]["study"])
    common.save(app, "yoke_envelope.dsgn")


def topology(app, S, env, c, target_volume, clock):
    """The envelope, optimised for the gear's worst case; returns the study
    and its summary."""
    model = common.printed(env)
    F = faces_of(env, S, model)
    v_env = volume(app, env)
    vf = target_volume / v_env
    env.activate()
    sid = app.add_study(type="topology", name="yoke envelope")["id"]
    # About two tet10s through an arm's 12 mm (quadratic elements bend
    # well with that), and a filter twice the size: the smallest member it
    # leaves is a printable strut. Coarse on purpose: on the CPU this mesh
    # took four minutes an iteration; the GPU, refined to fp64, takes about
    # 2.7 s (docs/topopt/README.md#mixed-precision-refinement).
    h = S["arm_t"] / 1.5
    app.set_study(id=sid, mesh_size=h, volume_fraction=vf,
                  filter_radius=2 * h, max_iterations=TOPO_ITERATIONS)
    add_loads(app, sid, F, c)
    keep = [F[k] for k in ("bore_pos", "bore_neg", "shoulder_pos",
                           "shoulder_neg")] + F["reliefs"] + F["screws"]
    for f in keep:
        app.add_keep_face(study=sid, face=f.ref)
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
        time.sleep(5)
    if st.get("error"):
        raise SystemExit(f"topology: {st['error']}")
    r = app.study_results(id=sid)
    r.pop("receipt", None)
    out = {"study": sid, "envelope_volume_mm3": v_env,
           "envelope_mass_g": mass_g(app, env), "volume_fraction": vf,
           "mesh_size_mm": h, "filter_radius_mm": 2 * h,
           "keep_faces": len(keep), "case": c["name"],
           "seconds": round(time.monotonic() - t0, 1), **r}
    clock("topology")
    print("  topology:", {k: out[k] for k in (
        "iterations", "converged", "volume_fraction", "compliance", "solver",
        "seconds", "elements")}, flush=True)
    return env, out


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
    """The yoke as it prints: three parts, each a Derived copy of the yoke
    cut down to one piece by a box (there is no split-into-parts verb), its
    STL, and whether it fits the bed in the orientation it prints in."""
    zt = S["yoke_zt"]
    big = 1000.0
    pieces = [
        # name, box (corner, size), print orientation, the dimension that
        # stands up from the bed
        ("base", (-big / 2, -big / 2, -big / 2), (big, big, big / 2 + zt),
         "top face down: the hub, collar and journal stand up", 2),
        ("arm_pos", (-big / 2, 0.0, zt), (big, big / 2, big),
         "upright, the foot down: the arm, the web and the ledge are walls "
         "(the ledge outboard of the web on support)", 2),
        ("arm_neg", (-big / 2, -big / 2, zt), (big, big / 2, big),
         "on its inner face: the A-frame and the flange pads flat", 1),
    ]
    out = []
    for name, corner, size, how, up in pieces:
        pname = f"yoke_print_{name}"
        fresh(app, pname)
        part = app.part(pname)
        part.activate()
        d = part._add({"type": "Derived", "name": "Yoke",
                       "source_part": yoke.id, "reference": False})
        b = part._add({"type": "Box", "name": "Piece", "position":
                       list(corner), "width": size[0], "height": size[1],
                       "depth": size[2]})
        r = part._add({"type": "Boolean", "name": "Piece", "op": "intersect",
                       "target_id": d["id"], "tool_id": b["id"]})
        # (the intersect's result is the box's body, which has no material)
        part.material("petg")
        bodies = r["digest"]["bodies"]
        if len(bodies) != 1:
            raise SystemExit(f"{pname}: {len(bodies)} bodies")
        lo, hi = bodies[0]["bbox_min"], bodies[0]["bbox_max"]
        dims = [h - l for l, h in zip(lo, hi)]
        stand = dims[up]
        lay = sorted(d for i, d in enumerate(dims) if i != up)
        fits = max(lay) <= BED_MM and stand <= BED_MM
        stl = str(common.OUT / f"{pname}.stl")
        app.export_stl(path=stl)
        row = {"piece": name, "mass_g": mass_g(app, part),
               "bbox_mm": [round(x, 1) for x in dims],
               "print": how, "height_mm": round(stand, 1),
               "footprint_mm": [round(x, 1) for x in lay],
               "fits_bed": fits, "stl": stl}
        print(f"  print piece {name}: {row['mass_g']:.0f} g, footprint "
              f"{row['footprint_mm']} x {row['height_mm']} high, "
              f"fits {BED_MM:g} bed: {fits}", flush=True)
        if not fits:
            raise SystemExit(f"{pname} does not fit a {BED_MM:g} mm bed")
        out.append(row)
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
                         "yoke_envelope.dsgn holds a result for this "
                         "envelope (two hours on the CPU)")
    a = ap.parse_args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(common.OUT / "pantilt.dsgn"))
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
                           "static": [static(app, solid, S, F, G, c, clock)
                                      for c in CASES]}
    if "yoke" in steps:
        yoke = app.part("yoke", create=False)
        FY = faces_of(yoke, S, model)
        REPORT["yoke"] = {"mass_g": mass_g(app, yoke),
                          "static": [static(app, yoke, S, FY, G, c, clock)
                                     for c in CASES]}
        REPORT["print_pieces"] = print_pieces(app, S, yoke)
        clock("print pieces")
        REPORT["stills"] = stills(app, [solid, yoke])
        clock("stills")
    common.save(app, "yoke_topo.dsgn")
    if "topo" in steps:
        topo_step(app, S, CASES[1], volume(app, solid), a.fresh_topology,
                  clock)
    REPORT["time"] = clock.rows
    (common.OUT / "yoke_topo_report.json").write_text(
        json.dumps(REPORT, indent=1, default=str))


if __name__ == "__main__":
    main()
