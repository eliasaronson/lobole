#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A name names one thing (docs/scripting/rpc.md#a-name-names-one-thing).

Placing a part twice gives two occurrences with its name, and nothing stops
two configurations sharing one. Each reader that takes an occurrence or a
configuration by name took whichever its loop stopped at -- the first in
some, the last in others -- and answered about it as if asked. Each is now
refused, naming both ids; the id still reaches the one meant, and once the
name is unique it resolves again.

Usage: name_ambiguity_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def refused(d, method, ids, **params):
    try:
        r = d.call(method, **params)
    except RuntimeError as e:
        text = str(e)
        expect("does not say which" in text,
               f"{method} names the ambiguity: {text}")
        for i in ids:
            expect(str(i) in text, f"{method} names id {i}: {text}")
        return
    raise AssertionError(f"{method} {params} answered for one of two: {r}")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_names_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        box(d, (0, 0, 0), (10, 10, 10), "Block")
        # Two configurations given one name.
        c1 = d.call("add_configuration", name="Long")["id"]
        c2 = d.call("add_configuration", name="Long")["id"]
        d.call("rename_part", part=0, new_name="Block")
        d.call("add_assembly", name="pair")
        a = d.call("add_occurrence", part="Block",
                   transform={"position": [0, 0, 0],
                              "orientation": [0, 0, 0, 1]})["id"]
        b = d.call("add_occurrence", part="Block",
                   transform={"position": [40, 0, 0],
                              "orientation": [0, 0, 0, 1]})["id"]
        names = {o["id"]: o["name"]
                 for o in d.call("list_occurrences")["occurrences"]}
        expect(names[a] == names[b],
               f"placing a part twice names both after it: {names}")
        name = names[a]

        refused(d, "measure", [a, b], a={"occurrence": name},
                b={"occurrence": b}, distance_kind="minimum")
        refused(d, "interference", [a, b], a=name, b=b)
        refused(d, "check_insertion", [a, b], a=name, b=b, entry="either",
                axis={"origin_mm": [0, 0, 0], "direction": [0, 0, 1]})
        refused(d, "set_assembly_sequence", [a, b],
                steps=[{"occurrence": name}])
        refused(d, "add_occurrence", [c1, c2], part="Block",
                configuration="Long")

        # The id still says which, and a unique name resolves.
        r = d.call("measure", a={"occurrence": a}, b={"occurrence": b},
                   distance_kind="minimum")
        expect(abs(r["distance"] - 30.0) < 1e-6, f"by id: {r}")
        d.call("rename_occurrence", occurrence=b, name="Right")
        r = d.call("measure", a={"occurrence": name},
                   b={"occurrence": "Right"}, distance_kind="minimum")
        expect(abs(r["distance"] - 30.0) < 1e-6, f"by unique names: {r}")
        print("name_ambiguity_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
