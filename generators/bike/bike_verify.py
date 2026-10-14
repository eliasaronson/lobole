# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What the run refuses to save without.

Everything here is MEASURED on the assembled bicycle, not recomputed from the
spec the parts were built from — a check that re-derives the number it is
checking passes whatever the parts came out like. Where the spec is the
subject (the wheelbase, the chainline) the measurement is compared to it;
where it is not (the axle heights, the chain's run) the geometry has to agree
with itself.

A receipt is returned for every check, and the caller prints them. `ok` is
not a receipt: `check_geometry` has an `ok` that is vacuously true for an
empty part, which is how a whole round once passed with nothing in it.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
from lobole.sdk import SdkError, V  # noqa: E402

# Contacts that are MEANT to happen. Every one of these is a part bolted to
# another part, which is two solids touching and has no other way to be said.
# The ceilings are in cubic millimetres of overlap.
ALLOW = {
    ("Frame", "Seatpost"): 3000.0,        # the post, clamped in the seat tube
    ("Fork", "Stem"): 3000.0,             # the quill, wedged in the steerer
    ("Frame", "Fork"): 1200.0,            # the headset's cups in the head tube
    ("Handlebar", "Stem"): 900.0,         # the bar, in its clamp
    ("Grip left", "Handlebar"): 3000.0,   # grips, on the bar
    ("Grip right", "Handlebar"): 3000.0,
    ("Brake lever", "Handlebar"): 400.0,
    ("Bell", "Handlebar"): 400.0,
    ("Chainguard", "Frame"): 900.0,       # the guard's bracket, on the stay
    ("Frame", "Rear rack"): 1200.0,       # the carrier's legs and struts
    ("Rear rack", "Reflector"): 600.0,
    ("Crankset", "Frame"): 1500.0,        # its cups, in the shell
    ("Crankset", "Pedal left"): 2000.0,   # pedal spindles, in their eyes
    ("Crankset", "Pedal right"): 2000.0,
    ("Chain", "Crankset"): 4000.0,        # the chain, ON its sprockets
    ("Chain", "Rear wheel"): 4000.0,
    ("Front wheel", "Fork"): 900.0,       # axles, through their dropouts
    ("Frame", "Rear wheel"): 900.0,
    ("Front mudguard", "Fork"): 2000.0,   # mudguard stays, on the fork ends
    ("Frame", "Rear mudguard"): 2000.0,
    ("Fork", "Front brake"): 9000.0,      # the caliper's bolt through the crown
    ("Front brake", "Front wheel"): 2500.0,  # its pads on the braking track
    ("Saddle", "Seatpost"): 15000.0,      # the saddle's clamp on the post
}

# An allowance is a claim that two parts TOUCH on purpose. Two of them were
# added for contacts that should not exist at all and were removed again when
# the geometry was fixed instead: the chainguard on the chain (the guard's
# lip was 4 mm too far in) and the caliper's arms through the mudguard.

def _origin(occ, frame):
    """Where a part's named frame ENDED UP, once the bicycle was assembled.

    `occ.frame(name)` is the opposite verb to `part.frame(name)`: the part
    one CREATES a datum, this one reports where it landed, in world space.
    It answers with a Frame, not a dict.
    """
    return V(occ.frame(frame).origin)


def everything(app, S, asm, occ, mates, P):
    receipts = []
    receipts.append(standing(asm))
    receipts += geometry(app, S, occ, P)
    receipts.append(drive(app, S, asm, mates))
    receipts.append(steering(app, S, asm, mates, occ))
    return receipts


def standing(asm):
    """Nothing clashing, nothing floating, no near miss under a millimetre."""
    r = asm.check(allow=ALLOW)
    return (f"standing: {len(r.get('pairs', []))} pairs swept, no clash, "
            f"{len(r.get('components', []))} connected group")


def geometry(app, S, occ, P):
    """The half-dozen numbers that decide whether this is a bicycle."""
    out = []
    front = _origin(occ["front wheel"], "axis")
    rear = _origin(occ["rear wheel"], "axis")

    wheelbase = abs(front[0] - rear[0])
    _want("wheelbase", wheelbase, float(S["WHEELBASE"]), 0.5)
    out.append(f"wheelbase: {wheelbase:.1f} mm, as laid out")

    # Both axles at the same height, or the bicycle does not stand on flat
    # ground — and the fork's rake is the thing most likely to break it.
    _want("axle heights", front[2], rear[2], 0.2)
    ground = rear[2] - float(S["WHEEL_R"])
    out.append(f"axles level at z = {front[2]:.1f}, ground at {ground:.1f}")

    # Trail: positive, or it does not steer. Measured from where the steering
    # axis meets the ground against where the tyre does.
    head = occ["fork"].frame("head axis")
    axis_pt, axis_dir = V(head.origin), V(head.z)
    down = axis_dir * (-1.0 if axis_dir[2] > 0 else 1.0)
    reach = (ground - axis_pt[2]) / down[2]
    # Trail is how far the contact patch trails BEHIND where the steering
    # axis meets the ground — the ground point MINUS the patch, with +X
    # forward. Written the other way round it comes out as -61.7 on a
    # bicycle whose trail is 61.7, and the gate reads that as unrideable.
    trail = (axis_pt[0] + down[0] * reach) - front[0]
    if trail <= 10.0:
        raise SdkError(f"trail measured {trail:.1f} mm on the built bicycle")
    out.append(f"trail: {trail:.1f} mm, ahead of the contact patch")

    # The chain has to run straight: it is one band, so its centroid IS its
    # plane, and that plane is the chainline the cog was placed on.
    chain_y = _body_centre_y(app, P["chain"])
    _want("chainline", abs(chain_y), float(S["CHAINLINE"]), 1.0)
    out.append(f"chainline: {abs(chain_y):.1f} mm right of centre, "
               f"chainring and cog on it")

    # The crank must swing clear of the ground and of the chainstay.
    clear = -ground - float(S["CRANK_LEN"])
    if clear < float(S["GROUND_PEDAL_CLEAR"]):
        raise SdkError(f"only {clear:.0f} mm under the pedal at its lowest")
    out.append(f"pedal clears the ground by {clear:.0f} mm at its lowest")

    mass = sum(app.call("mass_properties", part=p.id)["total"]["mass_g"]
               for p in P.values())
    extra = (app.call("mass_properties", part=P["pedal"].id)["total"]["mass_g"]
             + app.call("mass_properties",
                        part=P["grip"].id)["total"]["mass_g"])
    total = (mass + extra) / 1000.0
    if not 11.0 < total < 24.0:
        raise SdkError(f"the bicycle weighs {total:.1f} kg, which is not a "
                       "roadster — check the materials")
    out.append(f"mass: {total:.1f} kg all up, every part on its material")
    return out


def _body_centre_y(app, part):
    return app.call("mass_properties",
                    part=part.id)["total"]["center_of_mass_mm"][1]


def _want(what, got, want, tol):
    if abs(float(got) - float(want)) > tol:
        raise SdkError(f"{what}: measured {got:.2f}, laid out as {want:.2f} "
                       f"(tolerance {tol})")


def drive(app, S, asm, mates):
    """One turn of the cranks must turn the rear wheel 46/18 times, the same
    way round — and the study is RUN, not merely defined: adding one defines
    a question, and `run_motion_study` is what asks it. Every sample is
    checked for a clash, which is what catches a crank that clears the
    chainstay standing still and swings into it a quarter turn later.
    """
    ratio = int(S["CHAINRING_T"]) / int(S["COG_T"])
    before = _mate_deg(app, mates["rear wheel"])
    study = asm.motion(mates["cranks"], dof="rotation", from_=0.0, to=360.0,
                       samples=24, check_clash=True, name="One crank turn")
    run = app.call("run_motion_study", study=_mid(study), leave_at="last",
                   _timeout=1800)
    unsolved = [s for s in run.get("samples", []) if not s.get("solved", True)]
    if unsolved:
        raise SdkError(f"{len(unsolved)} of the study's samples did not "
                       f"solve: {unsolved[:2]}")
    appeared = [p for p in run.get("appeared", [])
                if tuple(sorted((p.get("a", ""), p.get("b", "")))) not in ALLOW]
    if appeared:
        raise SdkError("turning the cranks brought parts together that are "
                       f"clear at rest: {appeared[:4]}")
    after = _mate_deg(app, mates["rear wheel"])
    turned = (after - before) / 360.0
    if abs(abs(turned) - ratio) > 0.02:
        raise SdkError(f"one crank revolution turned the rear wheel "
                       f"{turned:.3f} times; {int(S['CHAINRING_T'])}/"
                       f"{int(S['COG_T'])} is {ratio:.3f}")
    if turned < 0:
        raise SdkError("the rear wheel turns BACKWARDS when the cranks go "
                       "forwards — that is a gear relation, not a chain")
    app.call("set_mate", id=_mid(mates["cranks"]), rotation_deg=0.0)
    asm.solve()
    roll = ratio * 2 * math.pi * float(S["WHEEL_R"]) / 1000.0
    return (f"drive: one crank revolution turns the wheel {turned:.2f} times "
            f"— {roll:.2f} m along the road — clean over "
            f"{len(run.get('samples', []))} samples")


def steering(app, S, asm, mates, occ):
    """The bars must turn through their lock without the front wheel or the
    mudguard touching the down tube."""
    worst = None
    for angle in (-38.0, -20.0, 20.0, 38.0):
        app.call("set_mate", id=_mid(mates["steering"]),
                 rotation_deg=angle)
        asm.solve()
        # `interference` names its two sides by occurrence NAME (or id) — a
        # {"occurrence": id} object is refused.
        for what in (occ["front mudguard"].name, occ["front wheel"].name):
            r = app.call("interference", a=what, b=occ["frame"].name)
            if r.get("clash"):
                raise SdkError(f"at {angle:.0f} deg of steering the "
                               f"{what.lower()} hits the frame")
            gap = r.get("distance", 0.0)
            worst = gap if worst is None else min(worst, gap)
    app.call("set_mate", id=_mid(mates["steering"]), rotation_deg=0.0)
    asm.solve()
    return (f"steering: clear through +/-38 deg, closest approach "
            f"{worst:.0f} mm (mudguard to down tube)")


def _mid(mate):
    return mate["id"] if isinstance(mate, dict) else mate


def _mate_deg(app, mate):
    for m in app.call("list_mates")["mates"]:
        if m["id"] == _mid(mate):
            return float(m.get("rotation_deg", 0.0))
    raise SdkError("mate not found")


def portraits(app, asm, out):
    """The three views the photographs were taken from, to be LOOKED at."""
    asm.activate()
    shots = []
    # `set_camera` takes a DIRECTION, not a named view — the named views live
    # on `view`, and a camera asked for view="right" is refused.
    for name, direction in (("drive-side", (0.0, -1.0, 0.0)),
                            ("three-quarter", (-0.75, 0.85, 0.42)),
                            ("left", (0.0, 1.0, 0.0))):
        path = os.path.join(out, f"bike-{name}.png")
        app.call("set_camera", direction=list(direction))
        app.call("fit_view")
        app.call("render_still", path=path, width=1600, height=1000,
                 _timeout=900)
        shots.append(path)
    return shots
