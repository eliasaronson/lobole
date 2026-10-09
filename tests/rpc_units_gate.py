#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Gate: a dimensional RPC parameter says what unit it is in.

Every unit bug this project has shipped is a BOUNDARY bug — a number crossing
between JSON and a struct field, where nothing carries the unit but the name
and the prose beside it:

  - `set_mate(rotation=60)` pinned a mate to 60 RADIANS and was diagnosed as a
    loop-solver branch bug twice.
  - `end_angle: 90` swept 116.6 degrees: a plausible arc and the wrong part.
  - `pattern(angle=90)` was documented as a total sweep in degrees and read as
    a per-step angle in radians.
  - `add_load(magnitude=…)` named no unit at all, and every FEM number scales
    linearly with it, so a wrong guess is invisible in the answer.

A type system does not reach any of these: the JSON number becomes a typed
quantity by fiat. What reaches them is saying so, in the one place an agent
looks — the MethodDoc row that `describe` serves. Forty-two parameters carried
a dimensional name and no unit anywhere when this was written.

So: a dimensional key must either

  - carry a unit SUFFIX (`angle_deg`, `touch_tol`... no — `tolerance_mm`), or
  - be named in its row's prose, in a row that names a unit.

The second is the weaker half deliberately. It cannot prove the unit beside
the key is the right one; it can prove the author thought about it and wrote
it down, which is the difference between `add_load`'s row and the forty-two. The
point of a scanning test is the NEXT row, not the current ones.

Needs no app, no GPU and no display — it reads the source.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from method_doc_scan import method_docs  # noqa: E402

REMOTE = os.path.join(ROOT, "src", "remote")

# Key stems that name a physical quantity. Matched against the whole key and
# against its trailing word, so `max_distance` and `near_point_distance` count
# as `distance` without listing every prefix.
DIMENSIONAL = {
    # length
    "distance", "radius", "diameter", "depth", "thickness", "offset",
    "height", "width", "length", "spacing", "stepover", "stepdown",
    "clearance", "pitch", "gap", "deflection", "margin", "travel", "grip",
    "overshoot", "resolution", "elevation", "extent", "inset", "lift", "size",
    # angle
    "angle", "rotation", "tilt", "tilt_u", "tilt_v", "phase", "twist",
    # force / stress / mass / density
    "magnitude", "force", "pressure", "load", "mass", "density", "gravity",
    "stress", "modulus", "yield",
    # rate / time
    "feed", "plunge", "speed", "rpm", "duration", "timeout", "budget",
    "deadline", "frequency",
    # tolerances, which are lengths wearing a different hat
    "tol", "tolerance",
}

# Suffixes that state the unit in the name itself. This is the preferred fix
# and the convention the codebase already spells: angle_deg, mass_g,
# max_von_mises_mpa, feed_mm_min, deflection_mm, budget_s.
UNIT_SUFFIX = re.compile(
    r"_(mm|cm|m|in|deg|rad|g|kg|lb|s|ms|n|mpa|gpa|hz|rpm|mm_min|m_s|px|pct)$")

# Unit words a row's prose can use, in any case — rows shout MILLIMETRES and
# RADIANS where the confusion has actually bitten, and that has to count.
UNIT_WORD = re.compile(
    r"\b(mm|cm|millimetres?|millimeters?|centimetres?|centimeters?|"
    r"metres?|meters?|inch|inches|deg|degrees?|radians?|rad|"
    r"newtons?|kg|grams?|tonnes?|pounds?|"
    r"rpm|hz|hertz|seconds?|minutes?|pixels?|percent|"
    r"unitless|dimensionless|fraction|ratio|normalized)\b"
    r"|mm/min|m/s\^?2?|kg/m\^?3?|g/cm\^?3?|%",
    re.IGNORECASE)

# The symbols, which are only themselves in the right case: a lowercase `n`
# is the English article's neighbour, and `pa` is half of "parallel".
UNIT_SYMBOL = re.compile(r"\bN\b|\bMPa\b|\bGPa\b|\bPa\b")

# Keys that match a dimensional stem but carry no unit, per method. Each is a
# deliberate exemption, not a backlog: the value is a NAME, an INDEX, a count
# or a pure ratio.
ALLOW = {
    # A fastener `size` is a designation ("M6"), a sheet `size` is "A4".
    ("add_fastener", "size"),
    ("set_drawing_sheet", "size"),
    # Drawing/view scale and render scale are ratios, not lengths.
    ("set_drawing_view", "scale"),
    ("add_drawing_view", "scale"),
    ("set_result_display", "deform_scale"),
    # A pixel raster, sized by the caller in pixels, named as such.
    ("render_still", "width"),
    ("render_still", "height"),
    ("screenshot", "width"),
    ("screenshot", "height"),
    # Homographs, all three of them integers or booleans: `load` is the load's
    # INDEX, `force` is "save over it anyway", `depth` is tree nesting.
    ("remove_load", "load"),
    ("save_project", "force"),
    ("ui_items", "depth"),
    # One handler branch serves fillet/offset/mirror/pattern, so rpc_params
    # attributes every key in it to all four and each row must list them.
    # `radius` is fillet's alone; the other three say so in their prose rather
    # than pretending to a unit for a parameter they do not take.
    ("offset", "radius"),
    ("mirror", "radius"),
    ("pattern", "radius"),
    # A quaternion [x, y, z, w]: an orientation, with no unit to state. Its
    # sibling `rotation` IS an angle and says so.
    ("add_mate", "ball_rotation"),
    ("set_mate", "ball_rotation"),
    # A paging offset, in records, beside `limit`.
    ("query_catalog", "offset"),
}


def dimensional(key):
    """Does this key name a physical quantity?"""
    if key in DIMENSIONAL:
        return True
    parts = key.split("_")
    # Trailing word (`max_distance` -> `distance`), or a two-word tail
    # (`angle_tol` -> `tol`); a leading `n_`/`num_` makes it a count.
    if parts[0] in ("n", "num", "count"):
        return False
    return parts[-1] in DIMENSIONAL


def main():
    paths = sorted(os.path.join(REMOTE, f) for f in os.listdir(REMOTE)
                   if f.startswith("Remote") and f.endswith(".cpp"))
    docs = method_docs(paths)

    problems = []
    for name, info in sorted(docs.items()):
        params = info["params"]
        if not params or params == "*":
            continue  # free-form spec: its own dialect documents it
        doc = info["doc"]
        row_names_a_unit = bool(UNIT_WORD.search(doc)
                                or UNIT_SYMBOL.search(doc))
        for key in params:
            if not dimensional(key) or (name, key) in ALLOW:
                continue
            if UNIT_SUFFIX.search(key):
                continue
            if row_names_a_unit and key in doc:
                continue
            problems.append((name, key, os.path.basename(info["file"])))

    if problems:
        print("A dimensional RPC parameter must say what unit it is in —")
        print("either in the KEY (`angle_deg`, `tolerance_mm`) or by naming")
        print("the key and its unit in the row's own text. A wrong guess at a")
        print("unit is invisible in the answer, which is why it belongs in")
        print("the row an agent reads and not in the handler.\n")
        for name, key, where in problems:
            print(f"  {where}: {name}.{key}")
        print(f"\n{len(problems)} undocumented dimensional parameters.")
        return 1

    print(f"ok: {len(docs)} methods, every dimensional parameter names a unit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
