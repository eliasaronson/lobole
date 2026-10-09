#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Gate: one noun, one name — a method that resolves a FEATURE takes
`feature_id`.

The vocabulary is mostly consistent already, and the rule it follows is worth
writing down because it is not obvious from any single method:

  - `id` is the id of the thing the method is NAMED after. `remove_mate(id)`,
    `set_cam_op(id)`, `remove_study(id)`, `set_occurrence_visible(id)`.
  - `<noun>_id` is a reference to something the method is NOT about:
    `set_material(feature_id)`, `frame(feature_id)`, `project_geometry`
    takes both a `sketch_id` and a `feature_id`.
  - `sketch_id` stays narrower than `feature_id` ON PURPOSE: it says the
    method requires that feature to be a Sketch, which `feature_id` would
    not, and a caller passing an extrude would then fail later and further
    away.

`delete_feature` was the one out of step — it took `id` where every sibling
took `feature_id` — and `set_rollback` offered only an `index`, which is the
one thing a caller holding a feature does not have: it has to count the tree
to roll back to the thing it just made. Both now take `feature_id`.

Sixteen of the fifty-four failed iterations in one modelling round were the
API's shape rather than the model's (docs/scripting/rpc.md#parameters). Most
were guessed methods and guessed values, which no gate can prevent. This one
class can: a caller who has just written `suppress_feature(feature_id=...)`
should not have to learn a second spelling for the next call.

Needs no app, no GPU and no display — it reads the source.
"""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from rpc_params_scan import scan_files  # noqa: E402

# Methods that resolve a feature without saying so in their name. Listed
# rather than guessed, so adding one is a decision somebody makes on purpose.
RESOLVES_A_FEATURE = {
    "frame",
    "project_geometry",
    "set_appearance",
    "set_material",
    "set_rollback",
    "set_tolerance",
    "update_derived",
}

# `feature` on add_feature/modify_feature is the feature ITSELF (its JSON),
# not a reference to one, so it is not a spelling of this noun.
DEFINITION_NOT_REFERENCE = {"add_feature"}


def main():
    scanned = scan_files(sorted(glob.glob(os.path.join(ROOT, "src", "remote",
                                                       "Remote*.cpp"))))
    problems = []
    for method, info in sorted(scanned.items()):
        keys = set(info["keys"])
        named_for_features = ("feature" in method
                              and method not in DEFINITION_NOT_REFERENCE)
        if not (named_for_features or method in RESOLVES_A_FEATURE):
            continue
        if info["opaque"] or info["annotated"]:
            continue  # its vocabulary is not visible to the scanner
        if "feature_id" not in keys:
            problems.append(
                f"{method} resolves a feature but does not accept "
                f"`feature_id` (it reads {sorted(keys) or 'nothing'}). Every "
                "other method naming a feature takes `feature_id`; add it "
                "alongside whatever it takes now.")

    # The rule only means something if the family it describes is real.
    family = [m for m in scanned
              if "feature_id" in set(scanned[m]["keys"])]
    if len(family) < 8:
        problems.append(
            f"only {len(family)} methods take `feature_id` — this gate "
            "assumes it is the house spelling, so check that before "
            "trusting it")

    if problems:
        print("rpc_names_gate: FAILED")
        for p in problems:
            print("  - " + p)
        return 1
    print(f"rpc_names_gate: OK ({len(family)} methods take `feature_id`)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
