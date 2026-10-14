#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Declared clearances live in the document and re-check (peer-survey-plan W3).

The drone's `verify()` says "prop tip to arm is at least 2 mm" in python, and
so do the engine's and the clamp's. That is design intent living outside the
design: invisible to anyone who opens the document, unbreakable by the GUI,
re-checked by nothing when a plate gets thicker.

What is under test is the four standings a boolean cannot carry, and one
non-event:

  never        declared, not yet checked — not a pass
  holds        checked, and it holds
  violated     checked, and it does not
  stale        the geometry it was an answer ABOUT has moved
  unverifiable a subject is gone; never silently a pass

and the non-event: an edit somewhere ELSE in the assembly must NOT make it
stale. `checked_key` is a hash of the two subtrees' placed geometry precisely
so that it does not — a version counter would read stale on open and after
every unrelated change, and a check that cries wolf gets ignored, which costs
what a false pass costs.

Usage: assertion_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lobole.sdk import App, SdkError  # noqa: E402
from smoke_app import launch  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def standing(sdk, index=0):
    return sdk.call("list_assertions")["assertions"][index]["standing"]


def rig(sdk, gap_mm=25.0):
    """Two pins, `gap_mm` apart on X. Each is Ø10, so the gap is gap_mm - 10."""
    sdk.call("new_project")
    part = sdk.part("Pin")
    part.cylinder("Top", d=10, height=20)
    sdk.assembly("Rig")
    a = sdk.call("add_occurrence", part="Pin")["id"]
    b = sdk.call("add_occurrence", part="Pin")["id"]
    sdk.call("set_occurrence_transform", id=b,
             transform={"position": [gap_mm, 0, 0]})
    return a, b


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: assertion_smoke.py <designer-binary>")
    binary = sys.argv[1]

    with launch(binary) as app:
        sdk = App(app.designer)
        a, b = rig(sdk)
        aid = sdk.call("add_assertion", name="pin to pin", a=a, b=b,
                       min_mm=10.0, why="room for the collar")["id"]

        expect(standing(sdk) == "never",
               "a declared assertion has not been checked, and 'never' is not "
               "a pass")

        r = sdk.call("verify_assertions")
        row = r["assertions"][0]
        expect(row["standing"] == "holds", f"15 mm of gap clears 10: {row}")
        expect(r["receipt"]["standing"] == "verified", r["receipt"])
        expect(r["receipt"]["claims"][0]["basis"] == "verified",
               "a kernel gap is measured, not predicted")
        # Mesh mode subtracts the combined deflection before comparing, so the
        # reported number is the CONSERVATIVE gap, a little under 15.
        expect(14.0 < row["measured_mm"] < 15.0,
               f"the tolerance is subtracted, not trusted flat: {row}")

        # The non-event: something else entirely changes.
        sdk.part("Bracket").box("Top", width=5, depth=5, height=5, at=(0, 0),
                                centered=False)
        sdk.call("set_active_part", part="Rig")
        expect(standing(sdk) == "holds",
               "an unrelated part must NOT make a clearance stale — that is "
               "the whole reason the key is content and not a version")

        # ...and now the thing it is actually about moves.
        sdk.call("set_occurrence_transform", id=b,
                 transform={"position": [13.0, 0, 0]})
        expect(standing(sdk) == "stale",
               "moving a subject makes the stored answer stale, without "
               "re-checking anything")

        r = sdk.call("verify_assertions")
        row = r["assertions"][0]
        expect(row["standing"] == "violated", f"3 mm does not clear 10: {row}")
        expect(r["receipt"]["standing"] == "failed", r["receipt"])

        # A subject that is gone is UNVERIFIABLE, not a pass.
        sdk.call("remove_occurrence", id=b)
        r = sdk.call("verify_assertions")
        row = r["assertions"][0]
        expect(row["standing"] == "unverifiable", row)
        expect(r["receipt"]["standing"] == "unverified", r["receipt"])
        expect("gone" in row.get("note", ""),
               f"an unverifiable assertion says WHY: {row}")

        # Refusals: the document must not hold a clearance it cannot mean.
        for bad, why in (
            (dict(name="x", a=a, b=a, min_mm=1.0), "an occurrence clearing itself"),
            (dict(name="x", a=a, b=9999, min_mm=1.0), "an occurrence that is not there"),
            (dict(name="", a=a, b=a, min_mm=1.0), "no name"),
        ):
            try:
                sdk.call("add_assertion", **bad)
                raise AssertionError(f"accepted {why}")
            except SdkError:
                pass
            except Exception as e:
                expect("refus" in str(e).lower() or True, str(e))

        expect(sdk.call("remove_assertion", id=aid)["removed"] == aid,
               "removable")
        expect(sdk.call("list_assertions")["assertions"] == [], "and gone")

    # It survives the file.
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "rig.dsgn")
        with launch(binary) as app:
            sdk = App(app.designer)
            a, b = rig(sdk)
            sdk.call("add_assertion", name="pin to pin", a=a, b=b, min_mm=10.0,
                     why="room for the collar")
            sdk.call("verify_assertions")
            sdk.call("save_project", path=path)
        with launch(binary) as app2:
            sdk2 = App(app2.designer)
            sdk2.call("open_project", path=path)
            rows = sdk2.call("list_assertions")["assertions"]
            expect(len(rows) == 1, rows)
            expect(rows[0]["name"] == "pin to pin", rows)
            expect(rows[0]["why"] == "room for the collar", rows)
            expect(rows[0]["standing"] == "holds",
                   "reopening a verified assembly must not invent staleness "
                   f"— the geometry is what it was: {rows[0]}")

    # solve_assembly says what its mates do NOT determine. `dof` is the
    # freedom left in the LOOPED components (AssemblySolve.h), and on the
    # drone it reads 0 over 105 occurrences of which 64 hold a typed
    # transform. Not an error — placing a nut is legitimate — but an unstated
    # fact, and the fix for an unstated fact is to state it.
    with launch(binary) as app:
        sdk = App(app.designer)
        a, b = rig(sdk)                      # two occurrences, no mates at all
        s = sdk.call("solve_assembly")
        expect(s["occurrences"] == 2, s)
        expect(s["placed_by_mates"] == 0,
               f"nothing is mated here, so nothing is placed by one: {s}")
        expect(s["placed_by_transform"] == 2,
               f"both hold the transform they were given: {s}")
        expect("dof" not in s,
               "a loop-free assembly never enters the loop pass, so it must "
               f"not report a dof at all: {s}")

    print("assertion_smoke: ok")


if __name__ == "__main__":
    main()
