#!/usr/bin/env python3
"""Stage 9: the final renders of the head. Opens drawings.dsgn; writes
scratch/pantilt/renders/ and saves nothing (it poses the head, renders,
and puts it back).

    python3 generators/pantilt/build_renders.py --port N

Path-traced, with the parts' own materials -- the plate aluminium, the
printed parts PETG, the bearings and screws steel:

- **hero**: the head carrying its camera, from in front and above.
- **tilted**: the camera tilted up 35 degrees, posed by turning the tilt
  worm through 35 x the pair's ratio (the worm relation turns the wheel).
- **exploded**: the head without its camera, every part pulled apart along
  the app's automatic explode.
- **low**: from under the pan worm, the side the hero cannot see.
"""
import common
from common import OUT

RENDERS = OUT / "renders"
RENDERS.mkdir(parents=True, exist_ok=True)
W, H, SAMPLES = 1800, 1350, 512


def still(app, name, direction, up=(0, 0, 1), outlines=False):
    app.set_camera(direction=list(direction), up=list(up))
    path = str(RENDERS / f"{name}.png")
    app.render_still(path=path, width=W, height=H, samples=SAMPLES, fit=True,
                     outlines=outlines)
    print("  render", path, flush=True)
    return path


def explode_steps(occs):
    """Where each part goes in the exploded still, mm: the pan stack down
    the pan axis, the tilt shaft out along its own, each worm out of its
    blocks along the worm, the screws out of their holes and their nuts the
    other way. Written out because the app's auto_explode pushes every part
    radially away from the assembly's centre, which scatters an axial stack
    into two clouds (README, the friction list)."""
    def centre(o):
        return [0.5 * (a + b) for a, b in zip(o["bbox_min"], o["bbox_max"])]

    def add(a, b):
        return tuple(x + y for x, y in zip(a, b))
    named = {o["name"]: o for o in occs}
    off = {}

    def put(name, v):
        off[named[name]["id"]] = tuple(v)
    G, T = (75.0, 0.0, 0.0), (0.0, 85.0, 0.0)  # the two worm groups
    put("base plate", (0, 0, -120))
    put("pan wheel", (0, 0, -60))
    put("cradle", (0, 0, 70))
    put("camera screw", (0, 0, 105))
    put("tilt wheel", (0, 120, 0))
    put("pan worm", G)
    put("pan block A", add(G, (0, 42, 0)))
    put("pan block B", add(G, (0, -42, 0)))
    put("pan knob", add(G, (0, 66, 0)))
    put("tilt worm", T)
    put("tilt block A", add(T, (-42, 0, 0)))
    put("tilt block B", add(T, (42, 0, 0)))
    put("tilt knob", add(T, (-66, 0, 0)))
    for o in occs:
        c = centre(o)
        if o["part"] == "ISO 15 6002":
            if abs(c[1]) < 1.0:  # the pan pair: under and over the wheel
                off[o["id"]] = (0, 0, -95) if c[2] < 25.0 else (0, 0, -30)
            else:                # the tilt pair: out of each arm
                off[o["id"]] = (0, 60 if c[1] > 0 else -60, 0)
        elif o["part"] == "ISO 15 609":
            if c[2] < 100.0:     # the pan worm's, along Y
                off[o["id"]] = add(G, (0, 22 if c[1] > 0 else -22, 0))
            else:                # the tilt worm's, along X
                off[o["id"]] = add(T, (22 if c[0] > 0 else -22, 0, 0))
    # Fasteners ride with what they are attached to, then leave their hole:
    # a screw away from its nut, the nut and washer the other way.
    fasteners = [o for o in occs if o["part"].startswith("ISO 4")
                 or o["part"].startswith("ISO 7")]
    for o in fasteners:
        c = centre(o)
        host = off.get(o.get("attached_to"), (0.0, 0.0, 0.0))
        nut = next((n for n in fasteners if n["part"].startswith("ISO 4032")
                    and abs(centre(n)[0] - c[0]) < 1.0
                    and abs(centre(n)[1] - c[1]) < 1.0), None)
        if o["part"].startswith("ISO 4762"):
            up = nut is None or centre(nut)[2] < c[2]
            pull = 30 if up else -30
        else:
            screw = next(s for s in fasteners
                         if s["part"].startswith("ISO 4762")
                         and abs(centre(s)[0] - c[0]) < 1.0
                         and abs(centre(s)[1] - c[1]) < 1.0)
            away = -1 if centre(screw)[2] > c[2] else 1
            pull = away * (24 if o["part"].startswith("ISO 4032") else 14)
        off[o["id"]] = add(host, (0, 0, pull))
    return off


def mate_named(app, name):
    for m in app.call("list_mates")["mates"]:
        if m.get("name") == name:
            return m
    raise SystemExit(f"renders: no mate named {name!r}")


def main():
    a = common.args()
    app = common.connect(a.port)
    clock = common.Clock()
    app.call("open_project", path=str(OUT / "drawings.dsgn"))
    V = {v["name"]: v["value"]
         for v in app.call("list_variables")["variables"]}
    ratio = V["wheel_teeth"] / V["worm_starts"]
    asm = app.assembly("pantilt", create=False)
    asm.activate()
    tilt = mate_named(app, "tilt worm")
    parts = {p["id"]: p["name"] for p in app.call("list_parts")["parts"]}
    occs = app.call("list_occurrences", bounds=True)["occurrences"]
    for o in occs:
        o["part"] = parts.get(o["part_id"], "")
    camera = [o for o in occs if o["name"] == "camera"]
    clock("open")

    still(app, "hero", (1.2, -1.0, 0.32))
    still(app, "low", (1.0, -1.3, -0.3))
    clock("hero and low")

    app.set_mate(id=tilt["id"], rotation_deg=35.0 * ratio)
    asm.solve()
    still(app, "tilted", (0.35, -1.0, 0.3))
    app.set_mate(id=tilt["id"], rotation_deg=0.0)
    asm.solve()
    clock("tilted")

    for o in camera:
        app.call("set_occurrence_visible", id=o["id"], visible=False)
    view = app.call("add_exploded_view", name="renders")["id"]
    for occ, offset in explode_steps(occs).items():
        app.call("set_explode_step", view=view, occurrence=occ,
                 offset=list(offset))
    app.call("set_explode", factor=1.0, view=view)
    still(app, "exploded", (1.15, -1.0, 0.4), outlines=True)
    app.call("set_explode", factor=0.0, view=0)
    for o in camera:
        app.call("set_occurrence_visible", id=o["id"], visible=True)
    clock("exploded")
    print("done")


if __name__ == "__main__":
    main()
