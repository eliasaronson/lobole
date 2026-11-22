#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The worked examples an agent is handed run, and build what they say.

`EXAMPLES` in python/lobole/mcp.py are complete builds, one per recipe the
workflow prompt teaches, each with its stated result
(docs/scripting/mcp.md#worked-examples). Every one is sent here as written:
its `setup` calls one at a time, its `calls` as one batch with its `expect`,
then its `then` (a variable changed) and the result stated after it. A miss
is the app's own `expect_failed`, so the numbers live in one place, the
example. An example with one number changed must fail, or the run proves
nothing.

The first fenced block of docs/scripting/sdk.md, the SDK as the page
introduces it, is run the same way: it states its own volume.

Usage: agent_examples_smoke.py <path-to-designer-binary>
"""
import copy
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch_app  # noqa: E402

from lobole.client import Client  # noqa: E402
from lobole.errors import ExpectationFailed  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def run_example(d, example):
    """Build `example` from a new project; raises ExpectationFailed on a
    miss, as an agent's client would."""
    d.call("new_project")
    for call in example.get("setup", []):
        d.request(call["method"], call.get("params", {}))
    r = d.call("batch", calls=example["calls"], expect=example["expect"])
    expect(r["expect"]["ok"], f"{example['name']}: {r['expect']}")
    then = example.get("then")
    if then:
        for call in then["calls"]:
            d.request(call["method"], call.get("params", {}))
        judged = d.call("check_expect", **then["expect"])
        expect(judged["ok"], f"{example['name']} after then: {judged}")


def nudged(example):
    """The example with its first stated number made 10 % larger."""
    wrong = copy.deepcopy(example)
    spec = wrong["expect"]
    for key in ("volume_mm3", "mass_g"):
        if key in spec:
            spec[key] *= 1.1
            return wrong, key
    if "bbox_mm" in spec:
        spec["bbox_mm"][0] *= 1.1
        return wrong, "bbox_mm"
    spec["body_count"] += 1
    return wrong, "body_count"


def sdk_block(port, state):
    """The first ```python block of docs/scripting/sdk.md, against this app
    alone (the registry is the test's own, so connect() finds only it)."""
    text = open(os.path.join(ROOT, "docs/scripting/sdk.md"),
                encoding="utf-8").read()
    block = re.search(r"```python\n(.*?)```", text, re.S).group(1)
    expect("check_expect" in block,
           "the SDK page's first example states what it builds")
    env = dict(os.environ, XDG_STATE_HOME=state, LOBOLE_PORT=str(port))
    with tempfile.TemporaryDirectory(prefix="lobole-sdk-block-") as cwd:
        r = subprocess.run([sys.executable, "-c", block], cwd=cwd, env=env,
                           capture_output=True, text=True, timeout=120)
    expect(r.returncode == 0,
           f"docs/scripting/sdk.md's first block fails:\n{r.stdout}"
           f"\n{r.stderr}")


def main():
    binary = sys.argv[1]
    import mcp_formats_gate  # the SDK, or a stand-in where it is absent
    m = mcp_formats_gate.load_mcp_constants()
    state = tempfile.mkdtemp(prefix="lobole-examples-")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(state, "config"))
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.sock.settimeout(300)
        expect(len(m.EXAMPLES) >= 5, f"{len(m.EXAMPLES)} examples")
        for example in m.EXAMPLES:
            run_example(d, example)
            # help(example=name) hands out exactly this.
            expect(m._example(example["name"])["calls"] == example["calls"],
                   f"help(example={example['name']!r}) is the example")
        wrong, key = nudged(m.EXAMPLES[0])
        try:
            run_example(d, wrong)
            raise AssertionError(f"an example with {key} changed passed")
        except ExpectationFailed as e:
            expect(key in str(e), f"the changed number is named: {e}")
        d.close()
        sdk_block(app.port, state)
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print(f"agent_examples_smoke: {len(m.EXAMPLES)} examples and the SDK "
          "page's first block build what they state, before and after "
          "their change")
    return 0


if __name__ == "__main__":
    sys.exit(main())
