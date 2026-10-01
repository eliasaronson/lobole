#!/usr/bin/env python3
"""Fan-out: several instances, several parts, one project
(docs/scripting/README.md#fan-out-several-agents-several-parts-one-project).

Three claims, none of them new machinery and none of them previously run
together:

1. Several instances serve at once — the server takes eight clients, not the
   one its python client's comment used to claim.
2. Parts built in separate instances merge into one project, because the
   directory format is one file per part and two agents on two parts touch
   disjoint files.
3. Two instances on ONE project directory is the case that must be LOUD: the
   second save is refused by the F8 disk stamp, naming what happened, rather
   than clobbering the first. That refusal is why a fan-out uses a worktree
   per agent.

Usage: fanout_smoke.py <path-to-designer-binary>
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def build_part(d, name, size):
    """One agent's share of the work: a part, in its own instance."""
    d.call("add_part", name=name)
    box(d, (0, 0, 0), (size, size, size), name)
    return d.call("mass_properties")["total"]["volume"]


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="designer_fanout_")
    apps, clients = [], []
    try:
        # ---- Three instances at once -----------------------------------
        for i in range(3):
            env = dict(os.environ)
            env.pop("DISPLAY", None)
            env.pop("WAYLAND_DISPLAY", None)
            env["XDG_STATE_HOME"] = os.path.join(tmp, f"state{i}")
            env["XDG_CONFIG_HOME"] = os.path.join(tmp, f"config{i}")
            apps.append(launch_app(binary, env=env))
        for app in apps:
            clients.append(Designer.wait_for(app.port))
        ports = {c.call("ping")["port"] for c in clients}
        expect(len(ports) == 3, f"three instances, three ports: {ports}")

        # ---- Each builds its own part, in its own directory -------------
        # A worktree per agent, in miniature: separate project directories,
        # merged by taking each one's part files.
        volumes = {}
        paths = []
        for i, d in enumerate(clients):
            d.call("new_project")
            name = f"part{i}"
            volumes[name] = build_part(d, name, 10.0 + i)
            path = os.path.join(tmp, f"agent{i}.dsgn")
            d.call("save_project", path=path)
            paths.append(path)
            expect(os.path.isdir(path), f"a directory-format project: {path}")

        # ---- The parts merge -------------------------------------------
        # The format is one file per part, so combining three agents' work is
        # copying three disjoint sets of files and one index — which is what
        # a git merge of three branches does, without reading either side.
        merged = os.path.join(tmp, "merged.dsgn")
        shutil.copytree(paths[0], merged, ignore=shutil.ignore_patterns(".git"))
        index_path = os.path.join(merged, "project.json")
        index = json.load(open(index_path))
        have = {doc["name"] for doc in index["documents"]}
        for path in paths[1:]:
            other = json.load(open(os.path.join(path, "project.json")))
            for doc in other["documents"]:
                if doc["name"] in have:
                    continue
                src = os.path.join(path, "parts", doc["name"])
                if not os.path.isdir(src):
                    continue
                shutil.copytree(src, os.path.join(merged, "parts",
                                                  doc["name"]))
                index["documents"].append(doc)
                have.add(doc["name"])
        with open(index_path, "w") as f:
            json.dump(index, f, indent=2)

        d0 = clients[0]
        d0.call("new_project")
        d0.call("open_project", path=merged)
        names = {p["name"] for p in d0.call("list_parts")["parts"]}
        for name in volumes:
            expect(name in names,
                   f"{name} survived the merge: {sorted(names)}")
        # Every part is the one its own agent built, byte for byte in volume.
        for name, volume in volumes.items():
            d0.call("set_active_part", part=name)
            got = d0.call("mass_properties")["total"]["volume"]
            expect(abs(got - volume) < 1e-6,
                   f"{name} is {got}, its builder made {volume}")

        # ---- Two instances on ONE directory: refused, not clobbered -----
        shared = os.path.join(tmp, "shared.dsgn")
        a, b = clients[0], clients[1]
        a.call("new_project")
        build_part(a, "shared_part", 20.0)
        a.call("save_project", path=shared)
        b.call("new_project")
        b.call("open_project", path=shared)
        # A writes first; B's copy of the directory is now stale.
        box(a, (50, 0, 0), (5, 5, 5), "A's edit")
        a.call("save_project", path=shared)
        expect(b.call("project_changed_on_disk")["changed"],
               "B can see the directory moved under it")
        try:
            b.call("save_project", path=shared)
            raise AssertionError("B's save overwrote A's work silently")
        except RuntimeError as e:
            expect("disk" in str(e) or "changed" in str(e) or "force" in str(e),
                   f"the refusal says what happened: {e}")
        # A's edit is still there — B refused rather than clobbering.
        a.call("new_project")
        a.call("open_project", path=shared)
        parts = {p["name"] for p in a.call("list_parts")["parts"]}
        a.call("set_active_part", part="shared_part")
        feats = {f["name"] for f in a.call("get_state")["features"]}
        expect("A's edit" in feats,
               f"A's edit survived B's refused save: {feats}")
        # And with force, B's save is a deliberate act, not an accident.
        b.call("save_project", path=shared, force=True)
        print("fanout_smoke: ok")
    finally:
        for app in apps:
            app.terminate()
        for app in apps:
            try:
                app.wait(timeout=10)
            except Exception:
                app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
