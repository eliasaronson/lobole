#!/usr/bin/env python3
"""A5 sub-assemblies, end to end through the remote surface.

Builds the shape the workstream exists for — a unit assembly placed inside a
top-level one — and checks the things that are only true if the whole tree is
being walked: the combined BOM counts leaves (a sub placed twice doubles its
parts), the per-unit BOM is just that assembly's own tab, geometry survives a
save/reopen, a cycle is refused before it can render forever, and deleting a
referenced tab is refused rather than leaving a dangling occurrence.

Run: python3 tests/nested_assembly_smoke.py build/designer
"""
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402

FAILURES = []


def expect(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        FAILURES.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main(binary):
    tmp = tempfile.mkdtemp(prefix="designer_nested_")
    port = free_port()
    env = dict(os.environ)
    env["XDG_STATE_HOME"] = tmp
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.STDOUT)
    try:
        d = None
        for _ in range(120):
            try:
                d = Designer(port)
                d.call("ping")
                break
            except Exception:
                time.sleep(0.5)
        if d is None:
            print("app never came up")
            return 1

        # --- a part, and a "Gangway" assembly that places it twice ----------
        d.call("add_part", name="Board")
        d.call("set_active_part", name="Board")
        d.call("add_feature", feature={"type": "Box", "name": "B",
                                       "position": [0, 0, 0],
                                       "width": 100, "height": 10, "depth": 20})
        d.call("add_assembly", name="Gangway")
        d.call("set_active_part", name="Gangway")
        d.call("add_occurrence", part="Board", transform={"position": [0, 0, 0]})
        d.call("add_occurrence", part="Board",
               transform={"position": [0, 0, 30]})
        gang_bom = d.call("bom")
        gang_qty = sum(r["quantity"] for r in gang_bom["rows"])
        expect(gang_qty == 2, f"the unit's own BOM counts its parts ({gang_qty})")

        # --- a top assembly placing the Gangway TWICE plus a loose Board ----
        d.call("add_assembly", name="Dock")
        d.call("set_active_part", name="Dock")
        r = d.call("add_occurrence", part="Gangway",
                   transform={"position": [0, 0, 0]})
        expect(r.get("ref_kind") == "assembly",
               "add_occurrence resolves an assembly by name")
        d.call("add_occurrence", part="Gangway",
               transform={"position": [500, 0, 0]})
        d.call("add_occurrence", part="Board",
               transform={"position": [0, 200, 0]})

        occs = d.call("list_occurrences")["occurrences"]
        kinds = sorted(o.get("ref_kind", "part") for o in occs)
        expect(kinds == ["assembly", "assembly", "part"],
               f"list_occurrences reports the reference kinds ({kinds})")
        # An agent must be able to see INSIDE a placed unit without switching
        # tabs, or it cannot tell an empty sub-assembly from a full one.
        sub = next(o for o in occs if o.get("ref_kind") == "assembly")
        expect(sub.get("leaf_count") == 2 and len(sub.get("leaves", [])) == 2,
               f"a sub-assembly reports its leaves ({sub.get('leaves')})")
        expect(all("/" in p for p in sub.get("leaves", [])),
               "leaf paths name the tree (Gangway/Board)")

        # A face anchor names a face of a PART; a placed assembly has none.
        # Refused at add time, not stored and left failing to solve.
        try:
            d.call("add_mate", type="fastened",
                   a={"occurrence": sub["id"],
                      "face": {"feature_id": 1, "element": "1/face0"}},
                   b={"occurrence": [o for o in occs
                                     if o.get("ref_kind", "part") == "part"][0]["id"],
                      "origin": True})
            expect(False, "a face anchor on a sub-assembly must be refused")
        except RuntimeError as e:
            expect("no faces of its own" in str(e),
                   f"face anchor refused with the way out: {str(e)[:70]}")

        # 2 gangways x 2 boards + 1 loose board = 5 leaves.
        top_bom = d.call("bom")
        top_qty = sum(r["quantity"] for r in top_bom["rows"])
        expect(top_qty == 5,
               f"the top BOM aggregates the whole tree ({top_qty}, want 5)")

        # Mass is the same aggregation: 5 boards of 100x10x20 mm.
        mp = d.call("mass_properties")
        want = 5 * 100 * 10 * 20
        got = mp.get("total", {}).get("volume", mp.get("volume", 0))
        expect(abs(got - want) < 1.0,
               f"mass properties see the whole tree ({got} mm^3, want {want})")

        # A section at x=50 crosses the three boards that live there (the
        # near gangway's two, plus the loose one) and misses the far gangway
        # at x=500: 3 x 10 x 20 = 600 mm^2. An exact number, because "> 0"
        # would also pass if only the top-level board were being cut.
        #
        # (Deliberately NOT the derived mid-scene offset: this model has a gap
        # in the middle, so the honest answer there is zero area.)
        sec = d.call("section", base_plane="yz", offset=50)
        expect(abs(sec["area"] - 600.0) < 1.0,
               f"a section cuts nested geometry ({sec['area']:.0f} mm^2, want 600)")

        # --- reading INTO a unit: the query no tab can answer ---------------
        # The loose Board sits at y=200; the near Gangway's boards are at
        # y=0..10, so an inner board is 190 mm clear of it. Neither the Dock
        # tab nor the Gangway tab can ask this: one side lives in each.
        gang_id = sub["id"]
        inner = sub["leaves"][0].split("/")[-1]
        r = d.call("interference", a=f"{gang_id}/1", b="Board")
        expect(r["state"] == "clear" and abs(r["distance"] - 190.0) < 1.0,
               f"a path query crosses the boundary ({r['state']}, "
               f"{r.get('distance', 0):.0f} mm, want clear 190)")

        # Both Gangways are named "Gangway", so a NAME path is ambiguous by
        # construction — it must say so and point at the id, not guess.
        try:
            d.call("interference", a=f"Gangway/{inner}", b="Board")
            expect(False, "an ambiguous name path must be refused")
        except RuntimeError as e:
            expect("more than one" in str(e),
                   f"ambiguity names the fix: {str(e)[:60]}")
        # A typo INSIDE an unambiguous unit says where it looked. (Reaching
        # for "Gangway/Nope" would report the ambiguity above first, which is
        # also right: the first bad step is the one worth naming.)
        try:
            d.call("interference", a=f"{gang_id}/Nope", b="Board")
            expect(False, "a bad path must be refused")
        except RuntimeError as e:
            expect("no occurrence 'Nope'" in str(e),
                   f"a typo inside the unit says where it looked: {str(e)[:60]}")

        # --- cycles are refused, not rendered -------------------------------
        d.call("set_active_part", name="Gangway")
        try:
            d.call("add_occurrence", part="Dock")
            expect(False, "placing the Dock inside the Gangway must be refused")
        except RuntimeError as e:
            expect("cycle" in str(e).lower(),
                   f"a cycle is refused at add time: {str(e)[:60]}")

        # --- deleting a referenced tab is refused ---------------------------
        parts = d.call("list_parts")["parts"]
        gang_idx = [p["index"] for p in parts if p["name"] == "Gangway"][0]
        try:
            d.call("delete_part", index=gang_idx)
            expect(False, "deleting a placed assembly must be refused")
        except RuntimeError as e:
            expect("placed in" in str(e),
                   f"a referenced tab cannot be deleted: {str(e)[:60]}")

        # --- save, reopen, and the tree is still a tree ---------------------
        proj = os.path.join(tmp, "nested.dsgn")
        d.call("save_project", path=proj)
        d.call("open_project", path=proj)
        d.call("set_active_part", name="Dock")
        reopened = d.call("bom")
        reopened_qty = sum(r["quantity"] for r in reopened["rows"])
        expect(reopened_qty == 5,
               f"the tree round-trips through save/open ({reopened_qty})")
        occs = d.call("list_occurrences")["occurrences"]
        expect(sum(1 for o in occs if o.get("ref_kind") == "assembly") == 2,
               "ref_kind survives the round trip")
    finally:
        app.terminate()

    print(("FAILED: " + "; ".join(FAILURES)) if FAILURES else "nested assembly smoke: OK")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/designer"))
