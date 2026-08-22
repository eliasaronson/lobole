#!/usr/bin/env python3
"""The CAM dogfooding coupon: a spoilboard hold-down clamp (docs/cam/dogfood-round.md).

120 x 60 x 17 mm in 18 mm ply, faced from 130 x 70 rough stock. Regenerable:
run it against any Designer instance and it rebuilds the part and the job.

    python3 scratch/holddown_clamp.py [port]

The job is built the way a person builds one: by pointing at the part. Only
the things a machine cannot know — which tool, how fast — are typed.
"""
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8842

# The part, in mm. The clamp is 120 x 60; the bolt slot runs down its right
# half; the pocket is where a washer bears, with an island the bolt clears.
L, W, T = 120.0, 60.0, 17.0
POCKET = (40.0, 20.0, 6.0)      # w, h, depth from the top
ISLAND = 10.0                    # square boss standing in the pocket
SLOT = (12.0, 44.0)              # bolt slot: width, length
SLOT_X = 84.0
HOLES = [(30.0, 12.0), (30.0, 48.0)]  # alternative bolt positions, 6 mm


def build(d):
    d.call("new_project")
    ids = {}

    def feat(**kw):
        d.call("add_feature", feature=kw)
        st = d.call("get_state")
        return st["features"][-1]["id"]

    ids["plate"] = feat(type="Box", name="Plate", position=[0, 0, 0],
                        width=L, height=W, depth=T)

    # The pocket sits in the LEFT half and the slot runs up the right, so the
    # island stays a real island — the first layout put the slot straight
    # through it, which is a fine clamp and a useless test of islands.
    px, py = 12.0, (W - POCKET[1]) / 2
    ids["void"] = feat(type="Box", name="PocketVoid",
                       position=[px, py, T - POCKET[2]],
                       width=POCKET[0], height=POCKET[1], depth=POCKET[2] + 5)
    ids["pocket"] = feat(type="Boolean", name="Pocket",
                         target_id=ids["plate"], tool_id=ids["void"],
                         op="subtract")

    # The island the washer bears on, standing on the pocket floor.
    ix, iy = px + (POCKET[0] - ISLAND) / 2, (W - ISLAND) / 2
    ids["boss"] = feat(type="Box", name="Boss", position=[ix, iy, T - POCKET[2]],
                       width=ISLAND, height=ISLAND, depth=POCKET[2])
    ids["withboss"] = feat(type="Boolean", name="WithBoss",
                           target_id=ids["pocket"], tool_id=ids["boss"],
                           op="union")

    # The bolt slot, through everything. CLOSED at both ends: an open-ended
    # notch is part of the part's own outline and gets cut with it, so it
    # would exercise nothing. A closed one is an opening with walls of its
    # own — and an opening that goes all the way through has no floor to
    # click, which is the case the round exists to try.
    ids["slotvoid"] = feat(type="Box", name="SlotVoid",
                           position=[SLOT_X, (W - SLOT[1]) / 2, -1.0],
                           width=SLOT[0], height=SLOT[1], depth=T + 2.0)
    ids["slotted"] = feat(type="Boolean", name="Slotted",
                          target_id=ids["withboss"], tool_id=ids["slotvoid"],
                          op="subtract")

    # Two 6 mm bolt holes, through, for the positions the slot does not reach.
    last = ids["slotted"]
    for i, (hx, hy) in enumerate(HOLES):
        bore = feat(type="Cylinder", name=f"Bore{i + 1}",
                    center=[hx, hy, -1.0], direction=[0, 0, 1],
                    radius=3.0, height=T + 2.0)
        last = feat(type="Boolean", name=f"Drilled{i + 1}",
                    target_id=last, tool_id=bore, op="subtract")
    ids["part"] = last
    return ids


# --- pointing at the part -------------------------------------------------

def faces(d):
    for body in d.call("list_elements")["bodies"]:
        for f in body["faces"]:
            yield {"feature_id": body["created_by"], "element": f["name"]}, f


def up_face_at(d, z):
    """The element ref of the up-facing planar face at height z."""
    for ref, f in faces(d):
        n = f.get("normal") or [0, 0, 0]
        c = f.get("centroid") or [0, 0, 0]
        if f.get("kind") == "plane" and n[2] > 0.99 and abs(c[2] - z) < 1e-3:
            return ref
    return None


def wall_near(d, x, y):
    """The element ref of the vertical face whose centre is nearest (x, y)."""
    best, ref = 1e18, None
    for r, f in faces(d):
        n = f.get("normal") or [0, 0, 1]
        c = f.get("centroid") or [0, 0, 0]
        if f.get("kind") != "plane" or abs(n[2]) > 0.01:
            continue
        d2 = (c[0] - x) ** 2 + (c[1] - y) ** 2
        if d2 < best:
            best, ref = d2, r
    return ref


def bores(d):
    """The cylindrical faces of the two bolt holes."""
    return [r for r, f in faces(d) if f.get("kind") == "cylinder"]


# --- the job --------------------------------------------------------------

def report(d, setup, op, label, res=0.6):
    """What one operation did, and what the verifier makes of the job so far."""
    r = d.call("cam_toolpath", setup=setup)
    this = [o for o in r["operations"] if o["operation"] == op][0]
    v = d.call("cam_verify", setup=setup, resolution=res)
    note = this.get("error")
    if note is None:
        note = (f"{this['moves']:5d} moves {this['cut_length_mm']:7.0f} mm  "
                f"min z {this['min_z']:6.2f}")
    print(f"  {label:22s} {note}")
    print(f"  {'':22s} job so far: gouge {v['max_gouge_mm']:.2f} "
          f"leftover {v['max_leftover_mm']:.2f} mm over "
          f"{v['leftover_area_mm2']:.1f} mm2 in {v['leftover_regions']} "
          f"place(s), rapids {v['rapids_through_stock']}")
    return v


def job(d):
    """The CAM job, over the RPC, by pointing at the part."""
    setup = d.call("add_cam_setup", name="Clamp", post="mach3")["id"]
    d.call("set_cam_setup", id=setup, tool={
        "number": 1, "name": "6 mm flat", "diameter": 6.0,
        "feed_cut": 1800.0, "feed_plunge": 600.0, "spindle_rpm": 18000.0})
    d.call("set_cam_setup", id=setup, tool={
        "number": 2, "name": "3 mm flat", "diameter": 3.0,
        "feed_cut": 1200.0, "feed_plunge": 400.0, "spindle_rpm": 18000.0})

    s0 = d.call("list_cam_setups")["setups"][0]
    stock = s0["stock"]
    print(f"stock {stock['min']} .. {stock['max']}, safe z {s0['safe_z']:.2f}")

    # 1. Skim the top. No pick: the stock above the part IS the thing to face.
    op = d.call("add_cam_op", setup=setup, type="face", tool=1)
    print(f"  face          -> top {op['top_z']:.2f} bottom {op['bottom_z']:.2f}"
          f" stepdown {op['stepdown']:.2f}")
    report(d, setup, op["id"], "skim the top")

    # 2. The pocket, from its floor. Its depth and its islands come from the
    #    face; the strategy comes from the geometry.
    floor = up_face_at(d, T - POCKET[2])
    assert floor, "no pocket floor face"
    op = d.call("add_cam_op", setup=setup, type="pocket", tool=1, target=floor)
    print(f"  pocket        -> {op['type']} side {op.get('side')} "
          f"top {op['top_z']:.2f} bottom {op['bottom_z']:.2f} "
          f"stepdown {op['stepdown']:.2f}")
    assert op["type"] == "adaptive", f"expected adaptive, got {op['type']}"
    report(d, setup, op["id"], "clear the pocket")

    # 2b. The 3 mm cutter goes back over the same floor. A 6 mm cutter cannot
    #     enter the 5 mm gaps between the island and the pocket walls — the
    #     verifier reported exactly that, twice, at 50 mm2 each — so the
    #     pocket needs a second pass with a tool that fits.
    op = d.call("add_cam_op", setup=setup, type="pocket", tool=2, target=floor)
    print(f"  pocket again  -> {op['type']} with tool {op['tool']}, "
          f"stepdown {op['stepdown']:.2f}")
    report(d, setup, op["id"], "finish the pocket")

    # 3. The bolt slot: click its wall. 3 mm cutter, so the tool change lands
    #    here.
    wall = wall_near(d, SLOT_X, W / 2)
    op = d.call("add_cam_op", setup=setup, type="profile", tool=2, target=wall)
    print(f"  slot          -> side {op.get('side')} "
          f"top {op['top_z']:.2f} bottom {op['bottom_z']:.2f} "
          f"stepdown {op['stepdown']:.2f} tabs {op.get('tabs')}")
    assert op.get("side") == "inside", "an opening must be cut from inside"
    report(d, setup, op["id"], "bolt slot")

    # 4. The two bolt holes. 6 mm holes, so the 6 mm cutter — the 3 mm one
    #    plunged into them would leave a 6 mm hole half cut.
    holes = bores(d)
    print(f"  drill         -> {len(holes)} cylindrical faces")
    op = d.call("add_cam_op", setup=setup, type="drill", tool=1,
                top_z=T, bottom_z=stock["min"][2], targets=holes)
    report(d, setup, op["id"], "drill the bolt holes")

    # 5. Cut it out. No pick: "the whole part".
    op = d.call("add_cam_op", setup=setup, type="profile", tool=1)
    print(f"  outline       -> side {op.get('side')} "
          f"top {op['top_z']:.2f} bottom {op['bottom_z']:.2f} "
          f"stepdown {op['stepdown']:.2f} tabs {op.get('tabs')}")
    v = report(d, setup, op["id"], "cut it out")
    return setup, v


def indexed(d):
    """C9: the same part, clamped on a trunnion, cut from the side."""
    setup = d.call("add_cam_setup", name="Indexed", post="linuxcnc")["id"]
    d.call("set_cam_setup", id=setup, tool={
        "number": 1, "name": "6 mm flat", "diameter": 6.0,
        "feed_cut": 1800.0, "feed_plunge": 600.0, "spindle_rpm": 18000.0})
    d.call("set_cam_setup", id=setup, rotation_a=90.0)
    s = [x for x in d.call("list_cam_setups")["setups"] if x["id"] == setup][0]
    print(f"  A{s['rotation_a']:.0f}, stock {s['stock']['min']} .. "
          f"{s['stock']['max']}, safe z {s['safe_z']:.2f}")
    op = d.call("add_cam_op", setup=setup, type="face", tool=1)
    r = d.call("cam_toolpath", setup=setup)
    this = [o for o in r["operations"] if o["operation"] == op["id"]][0]
    print(f"  face in frame -> {this['moves']} moves, min z {this['min_z']:.2f}"
          f" {this.get('error', '')}")
    nc = "/tmp/clamp-indexed.ngc"
    post = d.call("cam_post", setup=setup, path=nc)
    text = open(nc).read()
    a = [ln for ln in text.splitlines() if " A" in ln]
    print(f"  posted {post['lines']} lines, {len(a)} carry an A word")
    print(f"    {a[0] if a else '(none)'}")
    v = d.call("cam_verify", setup=setup, resolution=0.6)
    print(f"  verified in its own frame: gouge {v['max_gouge_mm']:.3f} mm, "
          f"leftover {v['max_leftover_mm']:.3f} mm over "
          f"{v['leftover_area_mm2']:.1f} mm2, rapids {v['rapids_through_stock']}")
    return setup


def main():
    d = Designer(port=PORT)
    build(d)
    bodies = d.call("get_state")["bodies"]
    print(f"part: {len(bodies)} body, {bodies[0].get('volume', 0):.0f} mm3\n")

    setup, verdict = job(d)

    print("\nposting")
    files = {}
    for post in ("mach3", "linuxcnc"):
        for arcs in (True, False):
            d.call("set_cam_setup", id=setup, fit_arcs=arcs)
            path = f"/tmp/clamp-{post}-{'arcs' if arcs else 'lines'}.nc"
            r = d.call("cam_post", setup=setup, path=path, post=post)
            files[(post, arcs)] = path
            n2 = sum(1 for ln in open(path) if ln.startswith(("G2 ", "G3 ")))
            print(f"  {post:9s} arcs={str(arcs):5s} {r['lines']:5d} lines, "
                  f"{n2:4d} arcs, {r['failed']} failed ops -> {path}")
    d.call("set_cam_setup", id=setup, fit_arcs=True)

    print("\nverdict")
    for res in (0.6, 0.3):
        v = d.call("cam_verify", setup=setup, resolution=res)
        print(f"  at {res} mm: gouge {v['max_gouge_mm']:.3f} mm, "
              f"leftover {v['max_leftover_mm']:.3f} mm over "
              f"{v['leftover_area_mm2']:.1f} mm2 in {v['leftover_regions']} "
              f"places, rapids {v['rapids_through_stock']}, ok={v['ok']}")
        for line in v["says"]:
            print(f"      {line}")
        for r in v["leftover_biggest"][:8]:
            print(f"      {r['area_mm2']:7.1f} mm2 {r['depth_mm']:6.2f} deep "
                  f"at X{r['at'][0]:6.1f} Y{r['at'][1]:5.1f}")

    print("\nindexed setup (C9)")
    indexed(d)

    d.call("save_project", path="/tmp/holddown-clamp")
    print("\nsaved /tmp/holddown-clamp")


if __name__ == "__main__":
    main()
