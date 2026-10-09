#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: project tables and lookup() over RPC, driven headless.

What only the RPC and the save path can show (docs/variables/README.md#tables):
a table set by RPC in either row form, a variable and a feature reading it,
the refusals naming what exists, deleting refused while something reads it,
undo restoring it, and the tables surviving a save in BOTH project forms (a
lone part's flat file, and a multi-document project's project.json).

Usage: tables_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, needle, **kw):
    try:
        d.call(method, **kw)
    except RuntimeError as e:
        expect(needle in str(e),
               f"{method} refused, but not naming '{needle}': {e}")
        return
    raise AssertionError(f"{method} was accepted: {kw}")


def variable(d, name):
    return {v["name"]: v for v in d.call("list_variables")["variables"]}[name]


BEARINGS = dict(name="bearings", columns=["d", "D", "B"], rows=[
    {"key": 6000, "values": [10, 26, 8]},
    {"key": 6002, "d": 15, "D": 32, "B": 9},
    {"key": "608", "values": [8, 22, 7]},
])


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_tables_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        d.call("new_project")

        r = d.call("set_table", **BEARINGS)
        rows = r["table"]["rows"]
        expect([row["key"] for row in rows] == [6000, 6002, 608],
               f"numeric keys come back as numbers: {rows}")
        expect(rows[1]["values"] == [15, 32, 9], f"named-column row: {rows}")

        d.call("set_variable", name="size", expression="6002")
        d.call("set_variable", name="od",
               expression='lookup("bearings", size, "D")')
        expect(variable(d, "od")["value"] == 32, variable(d, "od"))

        # A feature reads one too: a box as wide as a 6002 is thick.
        box(d, (0, 0, 0), (1, 10, 10), "Block",
            width_expr="lookup('bearings', 6002, 'B')")
        vol = d.call("mass_properties")["bodies"][0]["volume"]
        expect(abs(vol - 900.0) < 1e-6, f"9 x 10 x 10: {vol}")

        # A miss names what there is.
        d.call("set_variable", name="bad",
               expression='lookup("bearings", 6003, "D")')
        err = variable(d, "bad").get("error", "")
        expect("its keys are 608, 6000 and 6002" in err, err)
        d.call("delete_variable", name="bad")

        # Refusals: a row naming a column that is not there, a table that
        # does not exist, a table something reads.
        refused(d, "set_table", "the columns are d, D, B", name="t",
                columns=["d", "D", "B"], rows=[{"key": 1, "e": 2}])
        refused(d, "set_table", "has two rows keyed '1'", name="t",
                columns=["a"], rows=[{"key": 1, "values": [1]},
                                     {"key": 1.0, "values": [2]}])
        refused(d, "list_tables", "the tables are bearings", name="nuts")
        refused(d, "delete_table", "project variable 'od'", name="bearings")

        # An edit re-evaluates every reader; undo puts it back.
        edited = dict(BEARINGS)
        edited["rows"] = [dict(BEARINGS["rows"][0]),
                          {"key": 6002, "values": [15, 33, 9]},
                          dict(BEARINGS["rows"][2])]
        d.call("set_table", **edited)
        expect(variable(d, "od")["value"] == 33, "re-evaluated")
        d.call("undo")
        expect(variable(d, "od")["value"] == 32, "undo restored the table")

        # A lone part saves flat, and its file carries the tables.
        flat = os.path.join(tmp, "single.dsgn")
        d.call("save_project", path=flat)
        d.call("new_project")
        expect(d.call("list_tables")["tables"] == [], "a new project is empty")
        d.call("open_project", path=flat)
        expect(len(d.call("list_tables")["tables"]) == 1, "flat reopen")
        expect(variable(d, "od")["value"] == 32, "the reader evaluates")

        # A multi-document project keeps them in project.json.
        d.call("add_part", name="Second")
        multi = os.path.join(tmp, "multi.dsgn")
        d.call("save_project", path=multi)
        expect(os.path.isfile(os.path.join(multi, "project.json")),
               "a container project")
        d.call("new_project")
        d.call("open_project", path=multi)
        tables = d.call("list_tables")["tables"]
        expect(len(tables) == 1 and tables[0]["name"] == "bearings",
               f"project.json reopen: {tables}")
        expect(variable(d, "od")["value"] == 32, "the reader evaluates")

        d.call("delete_variable", name="od")
        d.call("delete_variable", name="size")
        # The box still reads it -- through its sketch, whose width
        # dimension carries the lookup (a Box is a sketch and an extrude).
        refused(d, "delete_table", "feature 'Block sketch'", name="bearings")

        print("tables_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
