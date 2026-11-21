#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""`fit: "reduce"` on add_feature and modify_feature
(docs/modeling/README.md#reducing-on-the-call): a size that does not fit is
replaced by the size that does, the document stores THAT size, and the
call's own response says both. Checked by what it is for: the stored radius
is the size built, the body is the closed-form solid of that radius, a save
and reopen gives the same body, and a size that fits answers no `reduced`.
The refusals (an unknown `fit`, `fit` on another feature, an expression, a
batch, nothing that fits) are refused_calls_smoke's.

Usage: fit_reduce_smoke.py <path-to-designer-binary>
"""
import math
import os
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def edge_at(d, point):
    """The one edge through `point`, by its stable name."""
    q = d.call("query_geometry", element_type="edge", on_point=point,
               expected_count=1)
    expect(q.get("ok") is True, f"one edge through {point}: {q}")
    return q["matches"][0]["element"]


def volume(d):
    return d.call("get_state")["bodies"][0]["volume_mm3"]


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="lobole_fit_reduce_")
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        blk = box(d, (0, 0, 0), (80, 15, 20), "B")["id"]
        ends = [edge_at(d, [0, 0, 10]), edge_at(d, [0, 15, 10])]
        fillet = {"type": "Fillet", "name": "F", "radius": 10.0,
                  "edges": [{"feature_id": blk, "element": e} for e in ends]}

        # `fit: "exact"` is the default: the same call refuses.
        try:
            d.call("add_feature", feature=fillet,
                   fit="exact")
            expect(False, "fit exact on a size that does not fit refuses")
        except AssertionError:
            raise
        except Exception as e:  # noqa: BLE001
            diag = getattr(e, "diagnostic", None) or {}
            expect(diag.get("code") == "does_not_fit",
                   f"refused as does_not_fit: {e} {diag}")
        # Two rounds on the 15 mm end meet at 7.5: 10 is reduced.
        r = d.call("add_feature", feature=fillet, fit="reduce")
        red = r.get("reduced")
        expect(red is not None, f"a reduced fillet says so: {r}")
        expect(red["param"] == "radius" and red["asked_mm"] == 10.0,
               f"reduced names the field and the size asked: {red}")
        built = red["built_mm"]
        expect(7.4 < built < 7.5, f"the size built is under 7.5: {red}")
        expect(red["basis"] in ("built", "bound"), f"a basis: {red}")
        expect(r["regen"]["ok"], f"and the part is healthy: {r['regen']}")
        # The document holds the number that was built, and nothing
        # remembers 10.
        f = next(x for x in d.call("get_state")["features"]
                 if x["id"] == r["id"])
        expect(f["radius"] == built and "radius_expr" not in f,
               f"the stored radius is the size built: {f}")
        want = 24000.0 - 2.0 * (1.0 - math.pi / 4.0) * built ** 2 * 20.0
        got = volume(d)
        expect(abs(got - want) < 1e-6 * want,
               f"the body is the round of {built}: {got} vs {want}")

        # modify_feature reduces too, and stores the size it built.
        m = d.call("modify_feature", feature_id=r["id"],
                   feature=dict(fillet, radius=12.0), fit="reduce")
        expect(m.get("reduced", {}).get("asked_mm") == 12.0
               and m["reduced"]["built_mm"] < 7.5,
               f"modify_feature reduces: {m}")
        f = next(x for x in d.call("get_state")["features"]
                 if x["id"] == r["id"])
        expect(f["radius"] == m["reduced"]["built_mm"],
               f"and stores it: {f}")

        # A size that fits is built as asked, and answers no `reduced`.
        m = d.call("modify_feature", feature_id=r["id"],
                   feature=dict(fillet, radius=5.0), fit="reduce")
        expect("reduced" not in m and m["regen"]["ok"],
               f"a size that fits is not reduced: {m}")
        want = 24000.0 - 2.0 * (1.0 - math.pi / 4.0) * 25.0 * 20.0
        expect(abs(volume(d) - want) < 1e-6 * want,
               f"and is built as asked: {volume(d)} vs {want}")
        d.call("undo")
        d.call("undo")

        # Saved and reopened, the reduced fillet is the same body.
        path = os.path.join(tmp, "reduced.lob")
        d.call("save_project", path=path)
        d.call("new_project")
        opened = d.call("open_project", path=path)
        expect(opened["regen"]["ok"], f"it reopens healthy: {opened}")
        expect(abs(volume(d) - got) < 1e-9 * got,
               f"and the same body: {volume(d)} vs {got}")

        print("fit_reduce_smoke: OK")
        return 0
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:  # noqa: BLE001
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
