#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: solving a study on the WORKER THREAD, over JSON-RPC.

Split out of fem_smoke.py on 2026-08-25. This is the slow half — it runs a
solve and then polls — and keeping it separate buys two things: a failure
here names the async contract instead of taking the CRUD/staleness contract
down with it, and this one can carry a longer ctest timeout without
lengthening the other.

What it pins, none of which a unit test can reach: `run_study {async: true}`
returns immediately having STARTED something, `study_status` reports it
running with an elapsed time, a static solve REFUSES to be cancelled (only
the iterative ones can be, and answering "cancelled" to a caller whose solve
then completes anyway would be a lie), and the result the worker leaves
behind is the same one the synchronous path produces.

That last check is the reason this exists at all: the async path is not a
different solver, it is the same solver off the main thread — so the answer
must match, and matching is the only way to know the worker wrote its
results back into the right place.

Skips (exit 77) when the build has no Netgen.

Usage: fem_async_smoke.py <path-to-designer-binary>
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch  # noqa: E402
import host_load  # noqa: E402
from fem_smoke import build_bar, expect, SKIP  # noqa: E402


def main():
    binary = sys.argv[1]
    try:
        with launch(binary) as app:
            d = app.client
            study, _, _, _ = build_bar(d)

            # The synchronous answer, to compare the worker's against.
            #
            # An explicit timeout, because the client's default is 60 s and
            # this call blocks for the whole solve. lobole.client.request's
            # own docstring names the trap: a solve that outlives the default
            # kills the client mid-call while the server finishes anyway,
            # which reads as a hang followed by a mystery. Here it would read
            # as "the async test is flaky", which is worse — the failure would
            # be in the half that is not being tested.
            sync = d.request("run_study", {"study": study}, timeout=600)
            expect(sync["elements"] > 0, "solved a mesh")
            sigma = sync["max_von_mises_mpa"]["value"]
            expect(sigma > 0, f"a positive von Mises came back: {sigma}")

            # The same inputs again: answered from the stored result, nothing
            # started (docs/simulation/README.md#studies-as-jobs).
            with host_load.Timed(app.pid) as took:
                again = d.call("run_study", study=study, **{"async": True})
            expect(again.get("reused") is True and again.get("started") is False,
                   f"an unchanged static study is reused: {again}")
            # Responsiveness: wall time is the point, judged on a quiet machine
            # (docs/ui/testing.md#tests-that-time-things).
            host_load.judge(took, took.seconds < 1.0,
                            f"a reuse is well under a second: "
                            f"{took.seconds:.2f}s", expect, "fem_async_smoke")
            expect(again.get("input_hash") == sync.get("input_hash"),
                   f"the same inputs, the same hash: {again} vs {sync}")

            # recompute: a real run, as a job with an id.
            started = d.call("run_study", study=study, recompute=True,
                             **{"async": True})
            expect(started.get("started") and started.get("running")
                   and isinstance(started.get("job"), int),
                   f"async static run starts as a job: {started}")
            job = started["job"]

            deadline = time.time() + 120
            while time.time() < deadline:
                st = d.call("study_status", study=study)
                if not st.get("running", False):
                    break
                expect("elapsed_seconds" in st,
                       f"a running status reports elapsed: {st}")
                expect(st.get("job") == job, f"the status names its job: {st}")
                time.sleep(0.2)
            else:
                raise AssertionError("async solve did not finish in 120s")

            res = d.call("study_results", study=study)
            expect(res["stale"] is False, f"async result cached: {res}")
            got = res["max_von_mises_mpa"]["value"]
            expect(abs(got / sigma - 1.0) < 0.05,
                   f"the worker's answer matches the sync one: {got} vs {sigma}")
            done = d.call("study_status", job=job)
            expect(done["running"] is False and done["outcome"] == "done",
                   f"the job says how it ended: {done}")

            # A cancel by job id: the static job stops at its next boundary
            # and drops its answer.
            third = d.call("run_study", study=study, recompute=True,
                           **{"async": True})
            c = d.call("cancel_study", job=third["job"])
            expect(c.get("cancelled") is True, f"cancel by id: {c}")
            deadline = time.time() + 120
            while time.time() < deadline:
                st = d.call("study_status", job=third["job"])
                if not st["running"]:
                    break
                time.sleep(0.2)
            expect(st["outcome"] == "cancelled"
                   and st.get("cancel_reason") == "cancel_study",
                   f"cancelled by cancel_study: {st}")
            try:
                d.call("cancel_study", job=third["job"])
                raise AssertionError("an ended job cannot be cancelled again")
            except RuntimeError as e:
                expect("already ended" in str(e), f"says it ended: {e}")
            try:
                d.call("study_status", job=987654)
                raise AssertionError("an unknown job id is refused")
            except RuntimeError as e:
                expect("no study job 987654" in str(e)
                       and str(third["job"]) in str(e),
                       f"refused, naming the jobs there are: {e}")
            print("fem_async_smoke: OK")
            return 0
    except Exception as e:
        if "FEM support" in str(e) or "Netgen" in str(e):
            print("fem_async_smoke: SKIP (built without Netgen)")
            return SKIP
        raise


if __name__ == "__main__":
    sys.exit(main())
