#!/usr/bin/env python3
"""A printable test coupon that calibrates the print allowance table.

    python3 generators/print_coupon.py --port 8800
    python3 generators/print_coupon.py --port 8800 --size 10 --steps 3 \\
        --thread M10 --process fdm --material petg

The allowance table (docs/tolerances/README.md#printed-parts) is community
practice and CAD-tool defaults, "to confirm with a test coupon". This is the
coupon. One plate, three rows, every feature engraved with the diameter it is
MODELLED at:

- bores at the table's model size for a Ø`size` hole, and `steps` either side
  of it `step` mm apart;
- pins the same way for a Ø`size` shaft;
- a printed internal and external `thread` (default the metric coarse thread
  of `size`, when there is one) at the table's thread allowance, modelled as
  the custom threads print_allowance and a Thread's report advise.

Print it with the settings the parts will get, then push a real Ø`size` pin
(a drill shank, a dowel) into the bores and the pins into a reamed Ø`size`
hole, and screw a real bolt and nut onto the threads. The bore and the pin
that fit the way you want are the model sizes the table should give; correct
PrintAllowance.cpp from them, citing the print.

Every size derives from `--size`: the pitch between features, the plate's
thickness (a bore as deep as it is wide), the label height and the engraving
depth. The numbers come from the app (`print_allowance`), never from here, so
the coupon always tests the table the build carries.

Output (generated, untracked): scratch/print_coupon/<name>.stl, the project,
and <name>.json listing every feature, its modelled diameter and its label.
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _lib import check, circle, connect, fresh_project, plane_at_z  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "scratch" / "print_coupon"


def fmt(v):
    """8.15 -> "8.15", 8.2 -> "8.2": the label a person reads off the plate."""
    s = f"{v:.3f}".rstrip("0").rstrip(".")
    return s


def text(label, at, height):
    return {"type": "text", "text": label, "font": "sans", "height": height,
            "anchor": [float(at[0]), float(at[1])], "halign": "center",
            "valign": "top"}


def feature_id(d, name):
    return {f["name"]: f["id"] for f in d.call("get_state")["features"]}[name]


def round_face(d, diameter, x, y):
    """The cylindrical face of `diameter` centred at (x, y), as a ref."""
    for b in d.call("list_elements")["bodies"]:
        for f in b.get("faces", []):
            if f.get("kind") != "cylinder":
                continue
            c = f["centroid"]
            if (abs(2.0 * f["radius"] - diameter) < 1e-3
                    and abs(c[0] - x) < 1e-3 and abs(c[1] - y) < 1e-3):
                return {"feature_id": b["created_by"], "element": f["name"]}
    raise SystemExit(f"no Ø{diameter} face at ({x}, {y})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int)
    ap.add_argument("--process", default="fdm")
    ap.add_argument("--material", default="petg")
    ap.add_argument("--size", type=float, default=8.0,
                    help="the diameter the fit is at, mm (the printed size)")
    ap.add_argument("--steps", type=int, default=2,
                    help="variants either side of the table's size")
    ap.add_argument("--step", type=float, default=0.05,
                    help="mm on the diameter between variants")
    ap.add_argument("--thread", default=None,
                    help="thread designation (default: the metric coarse "
                         "M<size>, if the tables carry it; 'none' for none)")
    ap.add_argument("--deflection", type=float, default=None,
                    help="STL chord deviation, mm (default: export_stl's "
                         "own, which is what parts get unless told)")
    ap.add_argument("--name", default=None)
    ap.add_argument("--fresh", action="store_true",
                    help="start a new project first (refused on unsaved work)")
    args = ap.parse_args()

    d = connect(args.port)
    if args.fresh:
        fresh_project(d)
    proc = {"process": args.process, "material": args.material}
    size = args.size

    def allowance(kind, **kw):
        return d.call("print_allowance", kind=kind, **proc, **kw)

    hole = allowance("hole", size_mm=size)
    shaft = allowance("shaft", size_mm=size)
    thread = args.thread
    if thread is None:
        guess = f"M{fmt(size)}"
        try:
            allowance("internal_thread", designation=guess)
            thread = guess
        except RuntimeError:
            thread = "none"
    nut = screw = None
    if thread != "none":
        nut = allowance("internal_thread", designation=thread)
        screw = allowance("external_thread", designation=thread)

    # Everything from the size: a feature every 2.5 diameters, a plate a
    # diameter thick, labels under each feature.
    pitch = 2.5 * size
    count = 2 * args.steps + 1
    label_h = min(0.3 * size, 4.0)
    engrave = 0.05 * size
    thick = size
    rows = 3 if nut else 2
    width = count * pitch
    depth = rows * pitch
    name = args.name or f"coupon_{args.process}_{args.material}_{fmt(size)}"

    d.call("add_part", name=name)
    d.call("set_print_process", **proc)
    check(d.call("add_feature", feature={
        "type": "Box", "name": "Plate", "position": [0, 0, 0],
        "width": width, "height": depth, "depth": thick}), "plate")

    def x_at(i):
        return pitch * (i + 0.5)

    def y_at(row):
        return depth - pitch * (row + 0.45)

    report = {"process": proc, "size": size, "step": args.step,
              "confirm": hole["confirm"], "features": []}
    bores, pins, labels = [], [], []
    for i in range(count):
        k = i - args.steps
        for row, what, base in ((0, "bore", hole), (1, "pin", shaft)):
            dia = round(base["model_mm"] + k * args.step, 4)
            (bores if what == "bore" else pins).append(
                circle((x_at(i), y_at(row)), dia / 2.0))
            labels.append(text(fmt(dia), (x_at(i), y_at(row) - 0.62 * size),
                               label_h))
            report["features"].append(
                {"kind": what, "modelled_mm": dia, "offset_mm": k * args.step,
                 "table": k == 0, "at": [x_at(i), y_at(row)]})

    thread_faces = []
    if nut:
        # The advised form: a custom thread on the face that gives the
        # printed major (docs/tolerances/README.md#printed-parts). An
        # internal thread's bore is its basic minor; a boss is its major.
        p = nut["pitch"]
        nut_face = round(nut["model_mm"] - 1.0825 * p, 4)
        screw_face = round(screw["model_mm"], 4)
        xi, xe = x_at(0), x_at(count - 1)
        bores.append(circle((xi, y_at(2)), nut_face / 2.0))
        pins.append(circle((xe, y_at(2)), screw_face / 2.0))
        labels.append(text(f"{thread} {nut['allowance_mm']:+g}",
                           (xi, y_at(2) - 0.62 * size), label_h))
        labels.append(text(f"{thread} {screw['allowance_mm']:+g}",
                           (xe, y_at(2) - 0.62 * size), label_h))
        thread_faces = [("nut", nut_face, xi, nut), ("screw", screw_face, xe,
                                                     screw)]
        report["features"] += [
            {"kind": "internal_thread", "designation": thread,
             "modelled_face_mm": nut_face, "major_mm": nut["model_mm"],
             "pitch": p},
            {"kind": "external_thread", "designation": thread,
             "modelled_face_mm": screw_face, "major_mm": screw["model_mm"],
             "pitch": p}]

    top = plane_at_z(thick)
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Pins", "plane": top, "entities": pins}),
        "pin sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Pins up", "mode": "add",
        "sketch_id": feature_id(d, "Pins"), "distance": size}), "pins")
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Bores", "plane": top, "entities": bores}),
        "bore sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Bores down", "mode": "cut",
        "sketch_id": feature_id(d, "Bores"), "distance": thick,
        "end_mode": "through_all", "reversed": True}), "bores")
    check(d.call("add_feature", feature={
        "type": "Sketch", "name": "Labels", "plane": top,
        "entities": labels}), "label sketch")
    check(d.call("add_feature", feature={
        "type": "Extrude", "name": "Engrave", "mode": "cut",
        "sketch_id": feature_id(d, "Labels"), "distance": engrave,
        "reversed": True}), "engrave")

    for what, dia, x, info in thread_faces:
        r = check(d.call("add_feature", feature={
            "type": "Thread", "name": f"Coupon {what}",
            "designation": f"coupon {what} thread", "pitch": info["pitch"],
            "modeled": True,
            "face": round_face(d, dia, x, y_at(2))}), f"{what} thread")
        if "print" in r:
            print(f"  {r['print']['sentence']}")

    OUT.mkdir(parents=True, exist_ok=True)
    stl = OUT / f"{name}.stl"
    exp = {"path": str(stl)}
    if args.deflection is not None:
        exp["deflection"] = args.deflection
    d.call("export_stl", **exp)
    d.call("save_project", path=str(OUT / f"{name}.dsgn"))
    (OUT / f"{name}.json").write_text(json.dumps(report, indent=1))
    print(f"print_coupon: {len(report['features'])} features -> {stl}")
    print(f"  {hole['confirm']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
