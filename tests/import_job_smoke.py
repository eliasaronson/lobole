#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every import is the `import` job: progress, cancel, and the mesh limit.

docs/ui/long-operations.md#import, docs/interchange/README.md#what-a-mesh-costs.
A 100k-triangle mesh through `import_step` used to sew for minutes on the
main thread, with nothing to show for it and nothing to stop it. Now:

- `import_step` of a mesh is listed in `background_jobs` while it reads,
  labelled with its triangles and expected time, its fraction moving, and
  the instance answers other calls meanwhile;
- another connection's `cancel_background_job {kind: "import"}` answers the
  call with a cancel, and the part is unchanged;
- the reply carries `mesh {triangles, expected_s, limit}`;
- past the limit the call is refused naming the count, the expected time and
  `allow_large_mesh`, changing nothing; with it, the job starts;
- `add_feature` of an Import past the limit is refused naming import_step;
- `--file` of a mesh is the same job, cancellable, and past the limit it is
  refused naming `--allow-large-mesh`.

LOBOLE_TEST_IMPORT_HOLD_MS holds the worker before it reads (a cancel still
ends the hold), so "while it runs" is a state the test reaches, not a race.

Usage: import_job_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "python"))
from smoke_app import launch  # noqa: E402
from smoke_meshes import torus  # noqa: E402
from lobole.client import Client  # noqa: E402

HOLD = {"LOBOLE_TEST_IMPORT_HOLD_MS": "60000"}


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def import_jobs(c):
    return [j for j in c.call("background_jobs")["jobs"]
            if j["kind"] == "import"]


def wait_for(cond, what, timeout=60.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        got = cond()
        if got:
            return got
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


class Call:
    """One RPC on its own thread, so the test can watch while it waits."""

    def __init__(self, client, method, params):
        self.result = None
        self.error = None
        self.seconds = None

        def go():
            t = time.monotonic()
            try:
                self.result = client.request(method, params, timeout=600)
            except Exception as e:  # noqa: BLE001 -- the message is the point
                self.error = str(e)
            self.seconds = time.monotonic() - t

        self.thread = threading.Thread(target=go)
        self.thread.start()

    def join(self):
        self.thread.join(timeout=600)
        expect(not self.thread.is_alive(), "the call never answered")
        return self


def features(c):
    return c.call("get_state")["features"]


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="lobole-import-job-") as tmp:
        small = os.path.join(tmp, "small.stl")
        mid = os.path.join(tmp, "mid.stl")
        n_small = torus(small, 2000)
        n_mid = torus(mid, 40000)

        # A mesh imports off the frame; its progress moves; it answers with
        # what it cost.
        with launch(binary) as app:
            d = app.client
            rep = d.call("import_step", path=small)
            mesh = rep.get("mesh") or {}
            expect(mesh.get("triangles") == n_small,
                   f"reply mesh {mesh}, not {n_small} triangles")
            expect(mesh.get("expected_s", -1) >= 0 and mesh.get("limit", 0) >
                   n_mid, f"reply mesh {mesh}: no expected time, or a limit "
                   f"under {n_mid}")
            limit = mesh["limit"]
            expect(rep.get("bodies") == 1, f"small import: {rep}")

            d.call("new_project")
            other = Client.wait_for(app.port)
            call = Call(d, "import_step", {"path": mid})
            seen = wait_for(lambda: import_jobs(other), "the import job")
            label = seen[0]["label"]
            expect("40,000 triangles" in label or f"{n_mid:,}" in label,
                   f"the job's label {label!r} does not name its triangles")
            expect(seen[0]["cancellable"], f"not cancellable: {seen[0]}")
            fractions = []
            while call.thread.is_alive():
                jobs = import_jobs(other)
                if jobs:
                    fractions.append(jobs[0]["fraction"])
                # The instance answers while the import runs.
                other.call("ping")
                time.sleep(0.1)
            call.join()
            expect(call.error is None, f"mid import failed: {call.error}")
            expect(len(set(f for f in fractions if f is not None)) >= 2,
                   f"the fraction never moved: {fractions}")
            expect(call.result["mesh"]["triangles"] == n_mid,
                   f"mid reply: {call.result}")
            other.close()

        # A cancel from another connection answers the call, and nothing is
        # imported.
        with launch(binary, env=HOLD) as app:
            d = app.client
            other = Client.wait_for(app.port)
            before = features(other)
            call = Call(d, "import_step", {"path": small})
            wait_for(lambda: import_jobs(other), "the held import job")
            asked = other.call("cancel_background_job", kind="import")
            expect(asked.get("asked") == 1, f"cancel: {asked}")
            call.join()
            expect(call.error and "cancelled" in call.error,
                   f"the cancelled call answered {call.result!r} / "
                   f"{call.error!r}, not a cancel")
            wait_for(lambda: not import_jobs(other), "the job to go")
            expect(features(other) == before,
                   "a cancelled import changed the part")
            # A second import is refused while one runs, naming it.
            call = Call(d, "import_step", {"path": small})
            wait_for(lambda: import_jobs(other), "the second held job")
            try:
                other.call("import_step", path=small)
                second = ""
            except Exception as e:  # noqa: BLE001
                second = str(e)
            expect("already running" in second,
                   f"a second import_step: {second or 'accepted'}")
            other.call("cancel_background_job", kind="import")
            call.join()
            other.close()

        # Past the limit: refused, naming the count, the time and the
        # override; nothing changes. With the override the job starts.
        big = os.path.join(tmp, "big.stl")
        n_big = torus(big, limit + 2000)
        expect(n_big > limit, f"{n_big} is not past the limit {limit}")
        with launch(binary, env=HOLD) as app:
            d = app.client
            before = features(d)
            try:
                d.call("import_step", path=big)
                refused = ""
            except Exception as e:  # noqa: BLE001
                refused = str(e)
            expect(f"{n_big:,} triangles" in refused and "allow_large_mesh"
                   in refused and "may take" in refused,
                   f"import_step past the limit: {refused or 'accepted'}")
            expect(not import_jobs(d), "the refused import started a job")
            expect(features(d) == before, "the refused import changed the part")
            try:
                d.call("add_feature", feature={"type": "Import", "path": big})
                added = ""
            except Exception as e:  # noqa: BLE001
                added = str(e)
            expect("import_step" in added and f"{n_big:,} triangles" in added,
                   f"add_feature of the big Import: {added or 'accepted'}")
            expect(features(d) == before, "the refused add_feature changed it")
            other = Client.wait_for(app.port)
            call = Call(d, "import_step", {"path": big,
                                           "allow_large_mesh": True})
            jobs = wait_for(lambda: import_jobs(other), "the overridden job")
            expect(f"{n_big:,} triangles" in jobs[0]["label"],
                   f"label {jobs[0]['label']!r}")
            other.call("cancel_background_job", kind="import")
            call.join()
            expect(call.error and "cancelled" in call.error,
                   f"overridden call: {call.result!r} / {call.error!r}")
            other.close()

        # --file is the same job: listed, cancellable...
        with launch(binary, ["--file", mid], env=HOLD) as app:
            d = app.client
            jobs = wait_for(lambda: import_jobs(d), "the --file import job")
            expect(jobs[0]["cancellable"], f"--file job: {jobs[0]}")
            d.call("cancel_background_job", kind="import")
            wait_for(lambda: not import_jobs(d), "the --file job to go")
            expect(not any(f.get("type") == "Import" for f in features(d)),
                   "the cancelled --file import left an Import")
        # ...lands when left alone...
        with launch(binary, ["--file", small]) as app:
            d = app.client
            wait_for(lambda: any(f.get("type") == "Import"
                                 for f in features(d)),
                     "the --file import to land")
        # ...and past the limit is refused in the log, naming the flag.
        with launch(binary, ["--file", big]) as app:
            d = app.client
            log = wait_for(lambda: "Could not import" in app.log() and
                           app.log(), "the --file refusal")
            expect("--allow-large-mesh" in log and f"{n_big:,} triangles"
                   in log, "the --file refusal does not name the count and "
                   "--allow-large-mesh")
            expect(not import_jobs(d) and not features(d),
                   "the refused --file import did something")
    print("import_job_smoke: ok")


if __name__ == "__main__":
    main()
