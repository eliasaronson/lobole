#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A roadster bicycle, modelled from three photographs of the owner's.

    python3 generators/bike/bike.py [--port N] [--out DIR]
                                    [--stage parts|assembly|all] [--keep]

A green Scandinavian gents' roadster: lugged diamond frame, 28" wheels laced
three-cross, a 3-speed internal-gear rear hub, one chainring under a teardrop
alloy chainguard, full mudguards, a tubular rear carrier, north-road bars.

Nothing about it was measured. What the photographs establish and what is
assumed are kept apart in `bike_spec.py`, every dimension is a project
variable, and the run refuses to save what it has not checked
(generators/bike/README.md).

It MOVES: the wheels turn, the bars steer, the cranks turn the rear wheel
through a chain relation at 46/18, and a motion study drives all of it.
"""
import argparse
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import bike_cockpit as C  # noqa: E402
import bike_drive as D  # noqa: E402
import bike_fittings as G  # noqa: E402
import bike_frame as F  # noqa: E402
import bike_spec as spec  # noqa: E402
import bike_verify as verify  # noqa: E402
import bike_wheels as W  # noqa: E402
from _lib import connect  # noqa: E402

PHASES = []


class phase:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        self.t = time.perf_counter()
        return self

    def __exit__(self, *exc):
        PHASES.append((self.name, time.perf_counter() - self.t))


def report_phases():
    total = sum(t for _, t in PHASES)
    print(f"time: {total:.1f} s")
    for name, t in PHASES:
        print(f"  {name:<20} {t:6.1f} s")


# --- the parts ----------------------------------------------------------------

def build_parts(app, S):
    P = {}
    P["frame"] = F.frame(app, S)
    P["fork"] = F.fork(app, S)
    P["front wheel"] = W.front_wheel(app, S)
    P["rear wheel"] = W.rear_wheel(app, S)
    P["crankset"] = D.crankset(app, S)
    P["pedal"] = D.pedal(app, S)
    P["chain"] = D.chain(app, S)
    P["chainguard"] = D.chainguard(app, S)
    P["stem"] = C.stem(app, S)
    P["handlebar"] = C.handlebar(app, S)
    P["grip"] = C.grip(app, S)
    P["brake lever"] = C.brake_lever(app, S)
    P["bell"] = C.bell(app, S)
    P["seatpost"] = C.seatpost(app, S)
    P["saddle"] = C.saddle(app, S)
    P["front mudguard"] = G.front_fender(app, S)
    P["rear mudguard"] = G.rear_fender(app, S)
    P["rear rack"] = G.rear_rack(app, S)
    P["reflector"] = G.reflector(app, S)
    P["front brake"] = G.front_brake(app, S)
    return P


# --- the bicycle --------------------------------------------------------------

def assemble(app, S, P):
    """Place every part by a MATE and none by a coordinate.

    The frame is the only fixed occurrence. Everything that steers hangs off
    the FORK — bar, stem, grips, lever, bell, front brake, front mudguard —
    so turning the one revolute at the head tube takes the whole front end
    with it, which is what a bicycle does and what a coordinate would not.
    """
    asm = app.assembly("Bicycle")
    occ, mates = {}, {}
    occ["frame"] = asm.place(P["frame"], fixed=True, name="Frame")
    for key in ("fork", "front wheel", "rear wheel", "crankset", "chain",
                "chainguard", "stem", "handlebar", "brake lever", "bell",
                "seatpost", "saddle", "front mudguard", "rear mudguard",
                "rear rack", "reflector", "front brake"):
        occ[key] = asm.place(P[key], name=P[key].name)
    for side in ("right", "left"):
        occ[f"pedal {side}"] = asm.place(P["pedal"], name=f"Pedal {side}")
        occ[f"grip {side}"] = asm.place(P["grip"], name=f"Grip {side}")

    # The joints. `flipped=False` is what two FRAMES want — the SDK now
    # defaults it that way for frame-to-frame mates, and this says it anyway,
    # because it is the one thing that silently produced a different bicycle:
    # left at the RPC's default of TRUE the fork turned end for end about its
    # own steering axis and took the front wheel to z = 771, the seatpost,
    # saddle and carrier hung underneath, and the chain and its guard were
    # mirrored onto the wrong side. Everything still "assembled".
    square = dict(flipped=False)
    mates["steering"] = asm.revolute((occ["frame"], "head axis"),
                                     (occ["fork"], "head axis"), **square)
    mates["front wheel"] = asm.revolute((occ["fork"], "front axle"),
                                        (occ["front wheel"], "axis"), **square)
    mates["rear wheel"] = asm.revolute((occ["frame"], "rear axle"),
                                       (occ["rear wheel"], "axis"), **square)
    mates["cranks"] = asm.revolute((occ["frame"], "bb"),
                                   (occ["crankset"], "axis"), **square)
    for side in ("right", "left"):
        mates[f"pedal {side}"] = asm.revolute(
            (occ["crankset"], f"pedal {side}"), (occ[f"pedal {side}"], "axis"),
            **square)

    # Everything that does not move relative to the frame, by its "seat"
    # frame: drawn in the bicycle's coordinates, so the mate is an identity
    # and the part lands where it was drawn.
    for key in ("chain", "chainguard", "seatpost", "saddle", "rear rack"):
        asm.fasten((occ["frame"], "seat"), (occ[key], "seat"), **square)
    for key in ("stem", "handlebar", "front brake"):
        asm.fasten((occ["fork"], "seat"), (occ[key], "seat"), **square)
    asm.fasten((occ["frame"], "rear mudguard"), (occ["rear mudguard"], "axle"),
               **square)
    asm.fasten((occ["fork"], "front mudguard"),
               (occ["front mudguard"], "axle"), **square)
    for side in ("right", "left"):
        asm.fasten((occ["handlebar"], f"grip {side}"),
                   (occ[f"grip {side}"], "axis"), **square)
    asm.fasten((occ["handlebar"], "lever"), (occ["brake lever"], "axis"),
               **square)
    asm.fasten((occ["handlebar"], "bell"), (occ["bell"], "axis"), **square)
    asm.fasten((occ["rear rack"], "reflector"), (occ["reflector"], "axis"),
               **square)

    # The drive. A chain turns both sprockets the SAME way, which is why this
    # is `chain` and not `gear` — a gear relation here would roll the bicycle
    # backwards when you pedal forwards. The hub's three speeds are not
    # modelled: this is its direct-drive gear, the one where the cog and the
    # wheel turn together.
    asm.chain(mates["cranks"], mates["rear wheel"], int(S["CHAINRING_T"]),
              int(S["COG_T"]))
    asm.solve()
    return asm, occ, mates


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int)
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(HERE), "..", "scratch", "bike"))
    ap.add_argument("--stage", default="all",
                    choices=("parts", "assembly", "all"))
    ap.add_argument("--keep", action="store_true",
                    help="build into the open project instead of a new one")
    args = ap.parse_args()

    remote = connect(args.port)
    # A wheel is 36 spokes and a rim, and OpenCASCADE's validity check on it
    # outlives the client's 60 s default — the call finishes, the client
    # gives up, and it reads as a hang.
    remote.sock.settimeout(600.0)
    app = _sdk(remote)
    if not args.keep:
        app.new()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    with phase("declare"):
        S = spec.declare(app)
    print(f"a {S['WHEELBASE']:.0f} mm wheelbase, {S['TRAIL']:.1f} mm of "
          f"trail, {S['DEVELOPMENT_M']:.2f} m per crank revolution")

    with phase("parts"):
        P = build_parts(app, S)
    print(f"{len(P)} parts")
    if args.stage == "parts":
        report_phases()
        return 0

    with phase("assemble"):
        asm, occ, mates = assemble(app, S, P)
    print(f"{len(occ)} occurrences, {len(mates)} joints")

    with phase("check"):
        receipts = verify.everything(app, S, asm, occ, mates, P)
    for line in receipts:
        print(f"  {line}")

    with phase("save"):
        app.call("save_project", path=os.path.join(out, "bike.dsgn"))
        app.call("export_step", path=os.path.join(out, "bike.step"))
    with phase("render"):
        verify.portraits(app, asm, out)
    print(f"saved to {out}")
    report_phases()
    return 0


def _sdk(remote):
    """`_lib.connect` hands back the raw client; the SDK wraps it."""
    from lobole.sdk import App
    return App(remote)


if __name__ == "__main__":
    sys.exit(main())
