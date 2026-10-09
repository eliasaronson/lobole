# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Panel modes of the cabinet — what the box itself adds to what you hear.

A loudspeaker panel that resonates radiates, and the frequencies it does that
at are a property of the panel, its thickness, its material and the span
between whatever holds it. This runs a modal study on the two that matter and
reports the six lowest.

## One panel at a time, which is the right unit anyway

This script is PART-scope: a `Study` on a part, each body meshed and solved
separately — one plate problem per panel. It is the panel that radiates and
its modes are a panel property, so that is the unit to reason in.

The app can also study a panel IN the assembly now (S1/S2: a `meshed`
occurrence with its neighbours `rigid` or `bonded`), and that is what answers
the question this idealisation cannot — see "What the braces are actually
worth" below.

## The idealisation, which matters more than anything below

Each panel is CLAMPED on the narrow faces it is glued in by. Real glued butt
joints are somewhere between clamped and simply supported, and that band is
wide: for the baffle, clamped theory gives 844 Hz and simply supported 372.
So these numbers compare VARIANTS of one idealisation honestly, and are not
absolute predictions. Two smaller approximations, both stated rather than
hidden:

  - the side wall's front and back walls actually land about 16 mm inboard
    on its INNER face, not on its edge faces; clamping the edges stretches
    the span 320 -> 352 mm, so its frequencies come out ~20% low;
  - the ring brace butts against a continuous face, so it cannot be fixed
    without splitting that face first. Only the divider's 4 mm dado is a
    real face, and it is what the second variant of each panel clamps.

## What it found

                        glued edges only        + divider fixed
    baffle   210x1122   850.5 878 924 1075...   850.5 924 1017 1075...
    side wall 352x1122  311.0 349 418 521...    316.3 372 473 623...

**Fixing the divider moves neither fundamental** — the baffle's not at all,
the side wall's by 1.7%. The mode shapes say why, and it is not the reason a
first look suggests. Neither fundamental is a whole-panel mode:

  - the BAFFLE's 850 Hz is the skirt BELOW the woofer cut-out, 210 x 560,
    the largest unbroken area left once the two openings are in it. The
    divider sits at the other end of the panel, 83% of the way up, nowhere
    near the antinode.
  - the SIDE WALL's 311 Hz is one central bulge over the whole 352 x 1122,
    and the divider crosses it near the top, where the mode is already
    almost still.

So a horizontal brace is not useless in general — it is useless HERE, at
this spacing. A clamped plate's fundamental goes as lam(aspect)/b^2 with b
the short span, and lam only runs from 22.4 (an infinite strip) to 36.0 (a
square): a brace earns its keep by making the unsupported patch squarer, and
one that shaves 17% off the long side of a 5:1 panel makes it 4:1, which is
nothing. The table reproduces the FEM closely enough to reason with — 311 Hz
for the side wall against 311.0 solved, 889 for the skirt against 850.5.

Two things follow, and the second is the interesting one:

  - a VERTICAL stiffener is worth far more than a horizontal one, because it
    divides b and the term is squared. A full-height stud at mid-depth would
    take the side wall from 311 Hz to about 1244.
  - **the ring brace crosses the baffle's skirt at 57% of its height**, which
    splits it into 210 x 321 and 210 x 239 — both near square — and should
    put that 850 Hz mode at roughly 1000. This study CANNOT measure it: the
    ring brace butts against a continuous face, so there is no face to
    clamp, and only the divider's dado is a real one. That measurement needs
    the baffle's inner face split along the brace line first.

## What the braces are actually worth

An ASSEMBLY study answers it, because the braces come in as their own
occurrences and the fuse imprints the contact patch for us. The side wall,
meshed, with the five walls it is glued to as rigid supports, and the ring
brace and brace ladder in three states (25 mm mesh, `scratch/`):

    braces absent (walls only)        352.5 Hz   15,045 el
    braces BONDED (their own stiffness) 458.9 Hz  103,086 el
    braces RIGID (the idealisation)   562.4 Hz   14,972 el

The bonded figure is the real one and it lands where a bracket says it must,
between the two. So the horizontal braces DO buy the side wall 30% — 353 to
459 Hz — which is more than the plate formula above credits them with,
because they stiffen the panel as ribs rather than only shortening its span.
Reading the rigid number as the answer would have overstated it by 22%.

(The 352.5 Hz here and the 311.0 Hz in the table are the same panel under
different clamps: this one is held where the walls actually touch it, the
table's on its edge faces. That is the 20% the idealisation note predicts.)

Trust: mesh-converged at 311.0 / 309.9 / 309.4 Hz over 15k / 21k / 34k
elements, and the hand calculation above agrees within 5%.

Needs a build with FEM and Spectra (`ping` reports `fem`). Minutes, not
seconds: the baffle is ~45,000 elements and about 150 s.

    python3 generators/smart_speaker/modal.py --port 8800
"""
import argparse
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent.parent / "tools")]

from designer_sdk import connect  # noqa: E402

PROJECT = HERE.parent.parent / "scratch/smart_speaker.dsgn"

# The divider's housing in each upright, in the panel's OWN frame. A 4 mm
# dado, and the only brace contact in this cabinet that is a face rather than
# a line on a continuous one.
DADO_Z = (359.0, 375.0)

# Each panel and the axis of its thickness: the baffle is a slab across Y,
# the side wall across X.
PANELS = (("front_wall", 1), ("side_wall", 0))


def perimeter(part, axis, dado):
    """The panel's narrow faces — planar, normal perpendicular to its
    thickness. `dado` picks the divider's housing instead of the edges.

    Selected by WHERE THEY LIE, like everything else in this generator: an
    element name is only valid for the feature order that produced it.
    """
    out = []
    for body in part.bodies():
        for f in body.faces(kind="plane"):
            n = f.normal
            if n is None or abs(n[axis]) > 0.1:
                continue
            at_dado = any(abs(f.centroid[2] - z) < 0.5 for z in DADO_Z)
            if at_dado == dado:
                out.append(f)
    return out


def study(app, name, axis, mesh_mm, braced, modes=6):
    """One panel, clamped where it is glued, solved. Returns its frequencies."""
    part = app.part(name, create=False)
    part.activate()
    label = f"{name} {'+ divider' if braced else 'glued edges'}"
    sid = app.add_study(kind="modal", name=f"{label} {mesh_mm:g}mm")["id"]
    app.set_study(study=sid, mesh_size=mesh_mm, num_modes=modes)

    faces = perimeter(part, axis, dado=False)
    if braced:
        faces += perimeter(part, axis, dado=True)
    if not faces:
        raise SystemExit(f"{name}: no edge faces to clamp — has the panel's "
                         f"thickness axis changed?")
    for f in faces:
        app.add_load(study=sid, type="fixed", face=f.ref)

    t0 = time.time()
    app.mesh_study(study=sid)
    app.run_study(study=sid, _timeout=2400)
    r = app.study_results(study=sid)
    hz = r["bodies"][0]["frequencies_hz"]
    print(f"  {label:26} {len(faces):2} faces clamped, {r['elements']:6} el, "
          f"{time.time() - t0:5.1f}s")
    print(f"      f1 = {hz[0]:7.1f} Hz    {[round(v) for v in hz]}")
    return hz


# The supports of the left side wall, by occurrence name, for the ASSEMBLY
# study (docs/simulation/README.md#assembly-studies): the parts that touch
# it, each clamped where it does. The braces are the two the part study could
# not use — they butt against a continuous face.
SIDE_WALL = "side_wall L"
WALL_SUPPORTS = ("front_wall", "back_wall", "top_wall", "bottom_wall",
                 "divider")
BRACE_SUPPORTS = ("brace ladder", "port ring brace")


def assembly_study(app, label, supports, mesh_mm, modes=6):
    """The side wall meshed, the named occurrences rigid and clamped at their
    contact with it. On the assembly tab; the same verbs as `study`."""
    sid = app.add_study(kind="modal", name=f"{label} {mesh_mm:g}mm")["id"]
    parts = [{"occurrence": SIDE_WALL, "role": "meshed"}]
    parts += [{"occurrence": n, "role": "rigid"} for n in supports]
    app.set_study(study=sid, mesh_size=mesh_mm, num_modes=modes,
                  participants=parts)
    for n in supports:
        app.add_load(study=sid, type="fixed", contact_with=n)
    t0 = time.time()
    r = app.run_study(study=sid, _timeout=2400)
    hz = r["bodies"][0]["frequencies_hz"]
    print(f"  {label:34} {len(supports):2} supports, {r['elements']:6} el, "
          f"{time.time() - t0:5.1f}s")
    print(f"      f1 = {hz[0]:7.1f} Hz    {[round(v) for v in hz]}")
    return hz


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--mesh", type=float, default=25.0,
                    help="element size in mm (converged by 25)")
    ap.add_argument("--project", default=str(PROJECT))
    ap.add_argument("--assembly", action="store_true",
                    help="the side wall in its assembly: its neighbours as "
                         "rigid supports clamped where they touch (S1), "
                         "with and without the braces")
    args = ap.parse_args()

    app = connect(args.port)
    if not app.ping().get("features", {}).get("fem"):
        raise SystemExit("this build has no FEM — modal needs it (and Spectra)")
    app.open_project(path=args.project)

    if args.assembly:
        # What the part study could not measure: the braces. Measured
        # 2026-09-16 at 25 mm: walls as contacts 352.5 Hz (the edge-clamped
        # part study's 311 read ~20% low, as it said), with the braces
        # 562.4 Hz — inside the 515-593 Hz hand calculation for the
        # floor-to-brace regions.
        app.assembly("speaker", create=False).activate()
        print(f"modal on the assembly, mesh {args.mesh:g} mm:")
        assembly_study(app, "side wall, walls as contacts", WALL_SUPPORTS,
                       args.mesh)
        assembly_study(app, "side wall, walls + braces",
                       WALL_SUPPORTS + BRACE_SUPPORTS, args.mesh)
        return

    print(f"modal, mesh {args.mesh:g} mm, MDF (E 3.6 GPa, rho 750, nu 0.25):")
    got = {}
    for name, axis in PANELS:
        for braced in (False, True):
            got[(name, braced)] = study(app, name, axis, args.mesh, braced)

    # The comparison this exists to make. Reported as a ratio because the
    # absolute numbers carry the clamped-edge idealisation and the ratio
    # mostly does not. Expect roughly nothing: the divider is at the far end
    # of both panels from where either fundamental actually lives.
    print("what the divider buys, on the fundamental:")
    for name, _axis in PANELS:
        free, braced = got[(name, False)][0], got[(name, True)][0]
        print(f"  {name:11} {free:7.1f} -> {braced:7.1f} Hz  "
              f"({100 * (braced / free - 1):+5.1f}%)")


if __name__ == "__main__":
    main()
