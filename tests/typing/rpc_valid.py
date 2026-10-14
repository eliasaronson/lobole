# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

from lobole.rpc import Revision
from lobole.sdk import App


def build(app: App, revision: Revision) -> None:
    app.add_part(name="Bracket")
    state = app.get_state(part="Bracket")
    app.add_feature(feature={"type": "Box"}, expect_revision=revision)
    app.modify_feature(feature_id=3, feature={"type": "Fillet"})
    app.set_variable(name="w", expression=12.5, scope="part")
    app.add_mate(type="revolute", a={"occurrence": 1}, b={"occurrence": 2},
                 rotation_deg=90, ball_rotation=[0.0, 0.0, 0.0, 1.0])
    app.set_mate(id=4, path=None, offset=2)
    app.measure(a={"feature_id": 3}, kind="angle", mode="exact")
    app.view(name="iso", fit=False)
    app.list_parts()
    app.batch(calls=[{"method": "undo"}], expect_revision=revision)
    app.solve_assembly(_timeout=30)
    print(state["features"])
    # Answers are typed too (docs/scripting/rpc.md#typed-results).
    names: list[str] = [p["name"] for p in app.list_parts()["parts"]]
    volume: float = app.mass_properties()["total"]["volume_mm3"]
    mass = app.mass_properties()["total"]["mass_g"]
    if mass is not None:
        print(names, volume, mass + 1.0)
