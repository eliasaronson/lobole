#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Gate: the RPC surface does not lose a method or a parameter silently.

`describe` publishes `api_version` (src/remote/ApiVersion.h), and a caller
written against one version keeps working while it stands. What breaks a
caller is a method or a parameter that goes away or is renamed — so that is
what raising the version is for, and this holds it to that.

api/rpc-surface.json is the surface as last published: every method and its
parameter names, and the version they belong to. Against the MethodDoc rows
(read from the source, no app needed):

- a method or parameter gone from the rows while `api_version` is unchanged
  fails — raise kApiVersion, or, before the first release (`released:
  false`), refresh the snapshot, because version 1 is still being written;
- a method or parameter the snapshot does not hold fails until the snapshot
  is refreshed, so that the NEXT removal has something to be caught against;
- a method added after version 1 must say so in its row (`since`), and no
  row may claim a version that does not exist yet.

    api_surface_gate.py            check
    api_surface_gate.py --update   rewrite api/rpc-surface.json from the rows
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from method_doc_scan import method_docs  # noqa: E402

SNAPSHOT = os.path.join(ROOT, "api", "rpc-surface.json")
REMOTE = os.path.join(ROOT, "src", "remote")


def api_version():
    with open(os.path.join(REMOTE, "ApiVersion.h")) as f:
        m = re.search(r"kApiVersion\s*=\s*(\d+)\s*;", f.read())
    if not m:
        raise SystemExit("api_surface_gate: no kApiVersion in ApiVersion.h")
    return int(m.group(1))


def surface():
    paths = sorted(os.path.join(REMOTE, f) for f in os.listdir(REMOTE)
                   if f.startswith("Remote") and f.endswith(".cpp"))
    docs = method_docs(paths)
    return ({name: (info["params"] if info["params"] is not None else [])
             for name, info in docs.items()},
            {name: info["since"] for name, info in docs.items()})


def main():
    version = api_version()
    methods, since = surface()
    if "--update" in sys.argv[1:]:
        released = False
        if os.path.exists(SNAPSHOT):
            with open(SNAPSHOT) as f:
                released = json.load(f).get("released", False)
        # One method per line, so a diff of the snapshot reads as the list
        # of what changed.
        rows = ",\n".join(f"  {json.dumps(k)}: {json.dumps(methods[k])}"
                          for k in sorted(methods))
        with open(SNAPSHOT, "w") as f:
            f.write(f'{{\n "api_version": {version},\n'
                    f' "released": {json.dumps(released)},\n'
                    f' "methods": {{\n{rows}\n }}\n}}\n')
        print(f"api surface: wrote {len(methods)} methods at version "
              f"{version} to {os.path.relpath(SNAPSHOT, ROOT)}")
        return 0

    with open(SNAPSHOT) as f:
        snap = json.load(f)
    problems = []
    gone, added = [], []
    for name, keys in snap["methods"].items():
        if name not in methods:
            gone.append(f"method {name}")
            continue
        if keys == "*" or methods[name] == "*":
            if keys != methods[name]:
                gone.append(f"{name}: params changed between free-form and "
                            "a key list")
            continue
        gone += [f"{name}.{k}" for k in keys if k not in methods[name]]
    for name, keys in methods.items():
        if name not in snap["methods"]:
            added.append(f"method {name}")
            if version > 1 and since[name] != version:
                problems.append(
                    f"{name} is new in api_version {version} and its row "
                    f"says since {since[name]}: give the row `since` "
                    f"{version}")
            continue
        old = snap["methods"][name]
        if keys != "*" and old != "*":
            added += [f"{name}.{k}" for k in keys if k not in old]
    for name, v in since.items():
        if v > version:
            problems.append(f"{name} says since {v}, past kApiVersion "
                            f"{version}")

    if gone and version == snap["api_version"]:
        if snap.get("released", False):
            problems.append(
                "removed or renamed with api_version unchanged — a caller "
                "written against it breaks. Raise kApiVersion "
                "(src/remote/ApiVersion.h), then --update:\n    "
                + "\n    ".join(gone))
        else:
            problems.append(
                "removed or renamed since the snapshot (allowed before the "
                "first release; refresh it with --update):\n    "
                + "\n    ".join(gone))
    if added:
        problems.append("not in the snapshot yet — refresh it with --update, "
                        "so a later removal is caught:\n    "
                        + "\n    ".join(added))
    if version != snap["api_version"] and not (gone or added):
        problems.append(f"kApiVersion is {version} and the snapshot "
                        f"{snap['api_version']}: refresh it with --update")

    if problems:
        print("The RPC surface moved without the snapshot "
              "(docs/scripting/rpc.md#the-api-version):\n")
        for p in problems:
            print("  " + p)
        print(f"\n  python3 {os.path.relpath(__file__, ROOT)} --update")
        return 1
    print(f"ok: {len(methods)} methods, api_version {version}, matching "
          "the snapshot")
    return 0


if __name__ == "__main__":
    sys.exit(main())
