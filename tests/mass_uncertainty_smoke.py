#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""An uncertain volume says so, in the reply that reports it.

The face-by-face integrator gives every figure an uncertainty
(docs/inspect/README.md#the-face-by-face-integrator). On a sound body it
converges and the reply carries none; this test makes it fail to converge
with the test seam LOBOLE_TEST_INTEGRATE_HALVINGS=0 (no halving allowed), on
a cylinder joined by a cross bar, whose faces are trimmed by intersection
curves. Then:

- each `mass_properties` row carries `volume_uncertainty_mm3` and
  `area_uncertainty_mm2`, and `warnings` names the body and its faces;
- `compare_geometry` repeats the warning beside its volumes;
- `get_state`'s digest stays a bare `volume_mm3`.

With the seam off the same body converges and does not warn, and a box,
which has no face to doubt, carries no uncertainty key at all.

Usage: mass_uncertainty_smoke.py <path-to-designer-binary>
"""
import os
import sys

from smoke_app import launch  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def build(d):
    d.call("new_project")
    cylinder(d, (0, 0, 0), "z", 10, 40, "Post")
    cylinder(d, (-25, 0, 20), "x", 4, 50, "Bar", mode="add")
    bodies = d.call("get_state")["bodies"]
    expect(len(bodies) == 1, f"one body expected: {bodies}")
    return bodies


def main():
    binary = sys.argv[1]
    seam = dict(os.environ, LOBOLE_TEST_INTEGRATE_HALVINGS="0")
    with launch(binary, env=seam) as app:
        d = app.client
        bodies = build(d)
        expect("volume_uncertainty_mm3" not in bodies[0],
               f"get_state's digest gained an uncertainty: {bodies[0]}")
        mp = d.call("mass_properties")
        row = mp["bodies"][0]
        vu = row.get("volume_uncertainty_mm3")
        au = row.get("area_uncertainty_mm2")
        expect(isinstance(vu, float) and vu > 0.0,
               f"no volume uncertainty with no halving allowed: {row}")
        expect(isinstance(au, float) and au > 0.0,
               f"no area uncertainty with no halving allowed: {row}")
        warnings = mp.get("warnings") or []
        expect(any("body 0" in w and "face" in w for w in warnings),
               f"mass_properties does not warn of body 0's faces: {mp}")
        cmp = d.call("compare_geometry", a=0, b=0,
                     metrics="volume")
        cw = cmp.get("warnings") or []
        expect(any("reference" in w and "face" in w for w in cw),
               f"compare_geometry does not repeat the warning: {cmp}")

    with launch(binary) as app:
        d = app.client
        build(d)
        mp = d.call("mass_properties")
        row = mp["bodies"][0]
        # Converged: what is left is rounding, far inside the 1e-9 the
        # warning starts at.
        expect(row.get("volume_uncertainty_mm3", 0.0)
               <= 1e-9 * row["volume_mm3"],
               f"a converged body is uncertain: {row}")
        expect(not mp.get("warnings"), f"a converged body warns: {mp}")
        # A body with no suspect face keeps the fixed rule's figure, with
        # no uncertainty at all, and the row carries no key.
        d.call("new_project")
        box(d, (0, 0, 0), (10, 20, 30))
        row = d.call("mass_properties")["bodies"][0]
        expect("volume_uncertainty_mm3" not in row
               and "area_uncertainty_mm2" not in row,
               f"a box carries an uncertainty: {row}")
    print("mass_uncertainty_smoke: ok")


if __name__ == "__main__":
    main()
