#!/usr/bin/env python3
"""An automatic mechanical watch — time only, with small seconds.

    python3 generators/watch/watch.py [--port N] [--out DIR]
                                      [--stage parts|assembly|all]

A 21,600 vph self-winding movement in a 40 mm case: barrel, centre, third,
fourth and escape wheels, a Swiss lever escapement with a hairspring, motion
works under the dial, a rotor winding the barrel through a reduction wheel,
and a dial, hands and case to put it in (docs/modeling/automatic-watch-plan.md).

Every dimension is a document variable (watch_spec.declare). The going train
and the motion works are coupled by gear relations, so one motion study turns
every hand at its true rate; the clearances that decide whether it runs are
DECLARED on the assembly and re-checked there. The run refuses to save what
it has not checked, and it gates on each check's receipt, not on its `ok`.
"""
import argparse
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import watch_parts as W  # noqa: E402
import watch_spec as spec  # noqa: E402
from designer_sdk import SdkError, connect  # noqa: E402
from watch_spec import *  # noqa: F401,F403,E402 — rebound by declare()

PHASES = []


class phase:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.t = time.perf_counter()

    def __exit__(self, *exc):
        PHASES.append((self.name, time.perf_counter() - self.t))


def report_phases():
    total = sum(t for _, t in PHASES)
    print(f"time: {total:.1f} s")
    for name, t in PHASES:
        print(f"  {name:<22} {t:6.1f} s  {100 * t / max(total, 1e-9):4.0f}%")


# --- parts --------------------------------------------------------------------------

def build_parts(app):
    P = {}
    P["mainplate"] = W.mainplate(app)
    P["barrel"] = W.barrel(app)
    P["barrel_arbor"] = W.barrel_arbor(app)
    P.update(W.train(app))                       # center, third, fourth, escape
    P["pallet"] = W.pallet_fork(app)
    P["balance"] = W.balance(app)
    P["hairspring"] = W.hairspring(app)
    for k, v in W.bridges(app).items():
        P[f"{k}_bridge"] = v
    P["cannon"] = W.cannon_pinion(app)
    P["minute"] = W.minute_wheel(app)
    P["hour"] = W.hour_wheel(app)
    P["dial"] = W.dial(app)
    P["indices"] = W.indices(app)
    for k, v in W.hands(app).items():
        P[f"{k}_hand"] = v
    P["rotor"] = W.rotor(app)
    P["reduction"] = W.reduction_wheel(app)
    P["crown_wheel"] = W.crown_wheel(app)
    P["stem"] = W.stem(app)
    P.update(W.case(app))
    return P


def check_parts(app, P):
    """Every part is a valid solid that built — on its receipt, not its `ok`.
    An empty part's `valid` is vacuously true and its receipt says so."""
    bad = []
    for key, part in P.items():
        part.activate()
        g = app.check_geometry()
        standing = g["receipt"]["standing"]
        if standing != "verified":
            bad.append(f"{part.name}: {standing} — {g['receipt']['summary']}"
                       + (f" {g['feature_errors']}" if g.get("feature_errors")
                          else ""))
    return bad


# --- the assembly -------------------------------------------------------------------

def mesh_phase(pa, pb, za, zb):
    """The relation phase that puts a GAP of b where a has a tooth, on the
    line between them.

    Every arbor is built with tooth 0 along +X and mated so that dof 0 keeps
    it there. With a's teeth at alpha + 2*pi*k/za, b's gaps at
    beta + 2*pi*(j + 1/2)/zb and beta = -(za/zb) * alpha + phase, a tooth of a
    on the centre line (angle phi) meeting a gap of b (at phi + pi) gives

        phase = phi * (1 + za/zb) + pi - pi/zb      (mod 2*pi/zb)

    gear_train.py gets away with pi/24 because its centre line IS +X; a watch
    train turns corners.
    """
    phi = math.atan2(pb[1] - pa[1], pb[0] - pa[0])
    return (phi * (1 + za / zb) + math.pi - math.pi / zb) % (2 * math.pi / zb)


def build_assembly(app, P):
    asm = app.assembly("Watch")
    occ = {}
    occ["mainplate"] = asm.place(P["mainplate"].name, fixed=True,
                                 name="Mainplate")
    for k, p in P.items():
        if k != "mainplate":
            occ[k] = asm.place(p.name, name=p.name)
    mp = occ["mainplate"]

    def rev(key, frame):
        return asm.revolute((mp, frame), (occ[key], "axis"),
                            flipped=False)["id"]

    def fasten(a, fa, b, fb="axis"):
        return asm.fasten((occ[a] if isinstance(a, str) else a, fa),
                          (occ[b], fb), flipped=False)["id"]

    m = {}
    for key, frame in (("barrel", "pivot barrel"),
                       ("barrel_arbor", "pivot barrel"),
                       ("center", "pivot center"), ("third", "pivot third"),
                       ("fourth", "pivot fourth"), ("escape", "pivot escape"),
                       ("pallet", "pivot pallet"),
                       ("balance", "pivot balance"),
                       ("minute", "pivot minute"), ("hour", "pivot hour"),
                       ("reduction", "pivot reduction"),
                       ("crown_wheel", "pivot crown_wheel"),
                       ("rotor", "pivot center"), ("stem", "pivot stem")):
        m[key] = rev(key, frame)
    fasten("center", "axis", "cannon")
    fasten("balance", "axis", "hairspring")
    for key in ("train_bridge", "barrel_bridge", "cock_bridge",
                "pallet_bridge", "dial", "indices", "case", "bezel",
                "crystal", "caseback", "window"):
        fasten(mp, "seat", key, "seat")

    pos = POS
    rel = []
    for a, b, za, zb, pa, pb in (
            ("barrel", "center", Z_BARREL, Z_CENTER_PIN, "barrel", "center"),
            ("center", "third", Z_CENTER, Z_THIRD_PIN, "center", "third"),
            ("third", "fourth", Z_THIRD, Z_FOURTH_PIN, "third", "fourth"),
            ("fourth", "escape", Z_FOURTH, Z_ESCAPE_PIN, "fourth", "escape"),
            ("center", "minute", Z_CANNON, Z_MINUTE_W, "center", "minute"),
            ("minute", "hour", Z_MINUTE_P, Z_HOUR, "minute", "center"),
            ("rotor", "reduction", Z_ROTOR_PIN, Z_REDUCTION_W, "center",
             "reduction"),
            ("reduction", "barrel_arbor", Z_REDUCTION_P, Z_RATCHET,
             "reduction", "barrel"),
            ("barrel_arbor", "crown_wheel", Z_RATCHET, Z_CROWN_WHEEL,
             "barrel", "crown_wheel")):
        r = asm.gear(m[a], m[b], za, zb,
                     phase=mesh_phase(pos[pa], pos[pb], za, zb),
                     name=f"{a} {za} -> {b} {zb}")
        if r.get("error"):
            raise SdkError(f"relation {a}->{b}: {r['error']}")
        rel.append(r)

    # The hands. Meshing phases turn every driven arbor by up to a tooth
    # pitch — 22 minutes of hour hand, on a 32-tooth hour wheel — so each hand
    # is fastened at the angle that cancels its arbor's resting dof: the
    # watch reads 12:00:00 with the barrel at zero.
    app.call("set_mate", id=m["barrel"], rotation=0.0)
    app.solve_assembly()
    dof = {x["id"]: x.get("rotation", 0.0) for x in app.list_mates()["mates"]}
    for hand, arbor in (("hour_hand", "hour"), ("minute_hand", "cannon"),
                        ("seconds_hand", "fourth")):
        base = m["center"] if arbor == "cannon" else m[arbor]
        asm.app.add_mate(type="fastened",
                         a={"occurrence": occ[arbor].id, "frame": "axis"},
                         b={"occurrence": occ[hand].id, "frame": "axis"},
                         flipped=False, rotation=-dof.get(base, 0.0))
    solved = app.solve_assembly()
    return asm, occ, m, rel, solved


# --- declared clearances (v100) ------------------------------------------------------

# The gaps that decide whether it runs, DECLARED on the assembly: they survive
# the save, list_assertions shows them beside the model, and moving a part
# marks them stale without this script running. (key, key, min mm, mode, why)
CLEARANCES = [
    ("seconds hand to hour hand", "seconds_hand", "hour_hand", 0.04, "exact",
     "the hour hand sweeps over the register at six"),
    ("hour hand to minute hand", "hour_hand", "minute_hand", 0.12, "exact",
     "two blued hands, one stacked over the other"),
    ("minute hand to indices", "minute_hand", "indices", 0.20, "exact",
     "the minute hand passes over every applied baton"),
    ("minute hand to crystal", "minute_hand", "crystal", 0.30, "exact",
     "a crystal pressed by a thumb must not stop the watch"),
    ("escape wheel to third wheel", "escape", "third", 0.10, "exact",
     "they overlap in plan, one level apart"),
    ("fourth wheel to centre wheel", "fourth", "center", 0.20, "exact",
     "they overlap in plan, one level apart"),
    ("balance to balance cock", "balance", "cock_bridge", 0.05, "exact",
     "the balance swings under the cock that holds its pivot"),
    ("rotor to ratchet wheel", "rotor", "barrel_arbor", 0.15, "exact",
     "the rotor passes over the ratchet every turn"),
    ("rotor to caseback", "rotor", "caseback", 0.30, "mesh",
     "a flexed caseback must not brake the rotor"),
    ("crown to case", "stem", "case", 0.05, "exact",
     "the crown turns against the case tube"),
]


def declare_clearances(app, occ):
    for name, a, b, mm, mode, why in CLEARANCES:
        app.add_assertion(name=name, a=occ[a].id, b=occ[b].id, min_mm=mm,
                          mode=mode, why=why)
    return len(CLEARANCES)


# --- verification ---------------------------------------------------------------------

def check_ratios(app, m):
    """Turn the barrel through one minute of the going train and read every
    arbor's dof back. The ratios are the watch; they must be exact."""
    minute_of_barrel = 360.0 / (Z_BARREL / Z_CENTER_PIN) / 60.0    # degrees
    app.call("set_mate", id=m["barrel"], rotation_deg=minute_of_barrel)
    app.solve_assembly()
    got = {x["id"]: x.get("rotation", 0.0) for x in app.list_mates()["mates"]}
    app.call("set_mate", id=m["barrel"], rotation_deg=0.0)
    app.solve_assembly()
    base = {x["id"]: x.get("rotation", 0.0) for x in app.list_mates()["mates"]}
    turn = {k: math.degrees(got[m[k]] - base[m[k]]) for k in
            ("center", "fourth", "escape", "hour")}
    want = {"center": -6.0, "fourth": -360.0, "escape": 12 * 360.0,
            "hour": -0.5}
    bad = [f"{k} turned {turn[k]:+.4f} deg in one minute, want {want[k]:+.1f}"
           for k in want if abs(turn[k] - want[k]) > 1e-6 * max(1, abs(want[k]))]
    return turn, bad


# Every mesh, as (driver key, driven key, the arbor whose mate drives the
# chain, teeth of the smaller gear in the pair, that gear's turns per turn of
# the chain's driver).
MESHES = [
    ("barrel", "center", "barrel", Z_CENTER_PIN, Z_BARREL / Z_CENTER_PIN),
    ("center", "third", "barrel", Z_THIRD_PIN,
     Z_BARREL / Z_CENTER_PIN * Z_CENTER / Z_THIRD_PIN),
    ("third", "fourth", "barrel", Z_FOURTH_PIN,
     Z_BARREL / Z_CENTER_PIN * 60),
    ("fourth", "escape", "barrel", Z_ESCAPE_PIN,
     Z_BARREL / Z_CENTER_PIN * 60 * Z_FOURTH / Z_ESCAPE_PIN),
    ("cannon", "minute", "barrel", Z_CANNON, Z_BARREL / Z_CENTER_PIN),
    ("minute", "hour", "barrel", Z_MINUTE_P,
     Z_BARREL / Z_CENTER_PIN * Z_CANNON / Z_MINUTE_W),
    ("rotor", "reduction", "rotor", Z_ROTOR_PIN, 1.0),
    ("reduction", "barrel_arbor", "rotor", Z_REDUCTION_P,
     Z_ROTOR_PIN / Z_REDUCTION_W),
    ("barrel_arbor", "crown_wheel", "rotor", Z_CROWN_WHEEL,
     Z_ROTOR_PIN / Z_REDUCTION_W * Z_REDUCTION_P / Z_RATCHET * Z_RATCHET
     / Z_CROWN_WHEEL),
]


def check_meshes(app, occ, m, samples=9):
    """Every mesh, EXACT, through one pitch of its smaller gear.

    The whole-assembly motion study cannot do this: in one pitch of the
    escape pinion the barrel's mesh moves a ten-thousandth of a pitch, so the
    slow meshes are sampled at one phase and their interference is never
    seen. And its mesh-mode check reads the 3 um that backlash leaves between
    flanks as contact. So each pair is driven through ITS OWN pitch and asked
    with a boolean — the worst volume over the pitch is the answer.
    """
    worst = {}
    for a, b, driver, z_small, turns in MESHES:
        pitch_deg = 360.0 / z_small / turns
        v_max = 0.0
        for i in range(samples):
            app.call("set_mate", id=m[driver], rotation_deg=pitch_deg * i
                     / (samples - 1))
            app.solve_assembly()
            r = app.interference(a=occ[a].id, b=occ[b].id, mode="exact")
            v_max = max(v_max, r.get("volume", 0.0) or 0.0)
        app.call("set_mate", id=m[driver], rotation_deg=0.0)
        worst[f"{occ[a].name} / {occ[b].name}"] = v_max
    app.solve_assembly()
    return worst


def verify(app, occ, m):
    print("checks:")
    bad = []
    with phase("ratios"):
        turn, why = check_ratios(app, m)
    bad += why
    with phase("meshes"):
        worst = check_meshes(app, occ, m)
    for pair, v in worst.items():
        print(f"  mesh {pair:<36} worst {v:.7f} mm3 over a pitch")
        if v > 0.0:
            bad.append(f"the {pair} mesh interferes: {v:.7f} mm3")
    print("  one minute: " + ", ".join(f"{k} {v:+.3f} deg"
                                        for k, v in turn.items()))

    with phase("clash sweep"):
        r = app.interference(mode="exact", deadline_s=300, max_pairs=20000,
                             _timeout=600)
    standing = r["receipt"]["standing"]
    print(f"  interference: {len(r.get('pairs', []))} clash pair(s), "
          f"receipt {standing}")
    if standing != "verified":
        bad.append(f"the clash sweep is {standing}: {r['receipt']['summary']}")
    for p in r.get("pairs", [])[:12]:
        bad.append(f"clash {p['a']} <-> {p['b']} {p.get('volume', 0):.4f} mm3")

    with phase("clearances"):
        c = app.verify_assertions(_timeout=900)
    for row in c["assertions"]:
        print(f"  {row['name']:<30} {row.get('measured_mm', 0):7.3f} mm "
              f"(min {row['min_mm']}) {row['standing']}")
        if row["standing"] != "holds":
            bad.append(f"{row['name']}: {row['standing']} "
                       f"{row.get('note', '')}".rstrip())
    print(f"  clearances: {c['receipt']['standing'].upper()}")

    with phase("motion study"):
        # One pitch of the escape pinion, the fastest mesh in the train: the
        # escape wheel turns 60 deg while the barrel turns 60/5760 of that.
        ratio = (Z_BARREL / Z_CENTER_PIN) * 60 * (Z_FOURTH / Z_ESCAPE_PIN)
        study = app.add_motion_study(driver=m["barrel"], dof="rotation",
                                     from_=0.0, to=360.0 / Z_ESCAPE_PIN / ratio,
                                     samples=13, check_clash=True,
                                     clash_mode="mesh", name="One escape pitch")
        run = app.run_motion_study(study=study["id"], _timeout=1800)
    appeared = run.get("appeared", [])
    unsolved = sum(1 for s in run.get("samples", []) if not s.get("solved"))
    print(f"  motion: {len(run.get('samples', []))} samples, "
          f"{run.get('seated_contacts', 0)} seated contacts, "
          f"{len(appeared)} new, {unsolved} unsolved, receipt "
          f"{run['receipt']['standing']}")
    for pair in appeared:
        bad.append(f"turning the train brings {pair['a_name']} into "
                   f"{pair['b_name']}")
    if unsolved:
        bad.append(f"{unsolved} motion samples did not solve")
    if bad:
        raise SdkError("VERIFICATION FAILED:\n  " + "\n  ".join(bad))


def set_time(app, m, h, mi, s):
    """Pose the train at a time of day (the barrel at zero reads 12:00:00)."""
    minutes = (h % 12) * 60 + mi + s / 60.0
    # Forward time turns the centre wheel clockwise seen from the dial (-Z),
    # so the barrel, which meshes it, turns positive.
    app.call("set_mate", id=m["barrel"],
             rotation_deg=minutes / 60.0 * 360.0 / (Z_BARREL / Z_CENTER_PIN))
    app.solve_assembly()


def renders(app, occ, out):
    """Glass renders opaque, so the crystal and the caseback window are hidden
    for every still; the movement shot takes the case, the caseback and the
    rotor off, as a watchmaker would. Each shot names what it hides."""
    os.makedirs(out, exist_ok=True)
    glass = ("crystal", "window")
    shots = (("dial", dict(eye=[10.0, -44.0, 70.0], target=[0.0, -1.0, 0.0]),
              glass),
             ("case", dict(eye=[62.0, -48.0, 24.0], target=[0.0, 0.0, -3.5]),
              glass),
             ("caseback", dict(eye=[-16.0, -40.0, -66.0],
                               target=[0.0, 0.0, -6.0]), glass),
             ("movement", dict(eye=[-9.0, -23.0, -38.0],
                               target=[0.0, 0.0, -4.0]),
              glass + ("caseback", "rotor", "case", "bezel")))
    for name, cam, hidden in shots:
        for k in hidden:
            app.call("set_occurrence_visible", id=occ[k].id, visible=False)
        app.set_camera(up=[0.0, 0.0, 1.0], **cam)
        app.render_still(path=os.path.join(out, f"{name}.png"), width=1600,
                         height=1200, samples=256, _timeout=900)
        for k in hidden:
            app.call("set_occurrence_visible", id=occ[k].id, visible=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "..", "scratch",
                                                  "watch"))
    ap.add_argument("--stage", choices=("parts", "assembly", "all"),
                    default="all")
    args = ap.parse_args()
    app = connect(args.port)
    app.call("new_project")
    dims = spec.declare(app)
    globals().update(dims)
    W.__dict__.update(dims)

    with phase("parts"):
        P = build_parts(app)
    bad = check_parts(app, P)
    print(f"parts: {len(P)} built, {len(bad)} not verified")
    for b in bad:
        print("  " + b)
    if bad:
        raise SdkError("parts did not build clean")
    if args.stage == "parts":
        report_phases()
        return

    with phase("assembly"):
        asm, occ, m, rel, solved = build_assembly(app, P)
    print(f"assembly: {len(occ)} occurrences, {len(m)} revolute mates, "
          f"{len(rel)} gear relations; {solved.get('placed_by_mates')} of "
          f"{solved.get('occurrences')} placed by mates, "
          f"{solved.get('placed_by_transform')} by transform, "
          f"errored mates {solved.get('errored_mates')}")
    print(f"  {declare_clearances(app, occ)} clearances declared")
    verify(app, occ, m)
    set_time(app, m, 10, 8, 37)
    os.makedirs(args.out, exist_ok=True)
    app.save_project(path=os.path.join(args.out, "watch.dsgn"))
    if args.stage == "all":
        with phase("renders"):
            renders(app, occ, args.out)
    print("saved")
    report_phases()


if __name__ == "__main__":
    main()
