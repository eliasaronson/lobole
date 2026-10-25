#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A part holds at most 50 000 features, and the app never writes one it
cannot open.

A file listing more is refused on load, naming the count and the limit
(docs/versioning/README.md#what-a-load-refuses), so `add_feature` refuses
the feature that would take a part past it -- in a batch too, and an
import as well -- and the refused call changes nothing
(docs/scripting/rpc.md#a-refused-call-changes-nothing). A part AT the limit
saves and opens again.

The part is 49 999 datum planes, which open in about a second.

Usage: feature_limit_smoke.py <path-to-designer-binary>
"""
import json
import os
import shutil
import sys
import tempfile

from lobole.errors import Refused  # noqa: E402
from smoke_app import launch  # noqa: E402

LIMIT = 50000
FIXTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "fixtures", "import")
PLANE = {"type": "DatumPlane", "name": "P", "kind": "offset",
         "base_plane": "xy", "offset": 1}


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def refused(d, method, params):
    try:
        d.request(method, params, timeout=120)
    except Refused as e:
        return str(e)
    raise AssertionError(f"{method} was not refused")


def main():
    binary = os.path.abspath(sys.argv[1])
    work = tempfile.mkdtemp(prefix="lobole_feature_limit_")
    try:
        with launch(binary, extra_args=["--autosave", "0"]) as app:
            d = app.client
            d.call("new_project")
            d.call("add_feature", feature=PLANE)
            one = os.path.join(work, "one.json")
            d.call("save_project", path=one)
            with open(one) as f:
                part = json.load(f)
            plane = part["features"][-1]
            features = []
            for i in range(LIMIT - 1):
                g = dict(plane)
                g["id"] = i + 1
                g["name"] = f"P{i + 1}"
                features.append(g)
            part["features"] = features
            near = os.path.join(work, "near.json")
            with open(near, "w") as f:
                json.dump(part, f, indent=2)
            d.request("open_project", {"path": near}, timeout=300)
            expect(len(d.call("get_state")["features"]) == LIMIT - 1,
                   "the part did not open whole")

            def unchanged(label, msg):
                st = d.call("get_state")
                expect("at most 50 000 features" in msg,
                       f"{label}: the refusal does not name the limit: {msg}")
                expect(len(st["features"]) == count,
                       f"{label}: the refused call changed the features")
                expect((st.get("undo"), st.get("redo")) == undo,
                       f"{label}: the refused call changed undo/redo")
                expect(not d.call("ping").get("dirty"),
                       f"{label}: the refused call left the part dirty")

            # A primitive adds its sketch too: two do not fit in one.
            d.call("save_project", path=near)
            count, st = LIMIT - 1, d.call("get_state")
            undo = (st.get("undo"), st.get("redo"))
            unchanged("a primitive", refused(d, "add_feature", {
                "feature": {"type": "Box", "on": "Top", "width": 1,
                            "height": 1, "depth": 1}}))

            # One more fits; it is the last.
            d.call("add_feature", feature=PLANE)
            at = os.path.join(work, "at.json")
            d.call("save_project", path=at)
            count, st = LIMIT, d.call("get_state")
            undo = (st.get("undo"), st.get("redo"))
            expect(len(st["features"]) == LIMIT, "the last feature was lost")
            unchanged("add_feature", refused(d, "add_feature",
                                             {"feature": PLANE}))
            unchanged("a batch", refused(d, "batch", {"calls": [
                {"method": "add_feature", "params": {"feature": PLANE}}]}))
            # The paths that built their feature without asking: a DXF or
            # SVG imported as a new sketch, and a model imported into the
            # part. Every one asks readctx::CheckFeatureRoom first
            # (tests/test_conventions.cpp holds them to it).
            unchanged("import_sketch", refused(d, "import_sketch", {
                "path": os.path.join(FIXTURES, "plate-ezdxf.dxf")}))
            unchanged("import_step", refused(d, "import_step", {
                "path": os.path.join(FIXTURES, "box-lobole.step")}))

            # At the limit, the part opens again.
            d.request("open_project", {"path": at}, timeout=300)
            expect(len(d.call("get_state")["features"]) == LIMIT,
                   "a part at the limit did not open")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("feature_limit_smoke: ok")


if __name__ == "__main__":
    main()
