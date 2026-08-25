#!/usr/bin/env python3
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
from fem_smoke import build_bar, expect, SKIP  # noqa: E402


def main():
    binary = sys.argv[1]
    try:
        with launch(binary) as app:
            d = app.designer
            study, _, _, _ = build_bar(d)

            # The synchronous answer, to compare the worker's against.
            sync = d.call("run_study", study=study)
            expect(sync["elements"] > 0, "solved a mesh")
            sigma = sync["max_von_mises_mpa"]["value"]
            expect(sigma > 0, f"a positive von Mises came back: {sigma}")

            # Re-running is what makes the results stale-then-fresh again; the
            # load is unchanged, so the async answer must equal the sync one.
            started = d.call("run_study", study=study, **{"async": True})
            expect(started.get("started") and started.get("running"),
                   f"async static run starts: {started}")

            deadline = time.time() + 120
            refused_a_cancel = False
            while time.time() < deadline:
                st = d.call("study_status", study=study)
                if not st.get("running", False):
                    break
                expect("elapsed_seconds" in st,
                       f"a running status reports elapsed: {st}")
                # A static solve cannot be interrupted mid-factorisation, so
                # cancel must refuse and SAY so rather than accept and lie.
                try:
                    d.call("cancel_study", study=study)
                    raise AssertionError("a static solve must refuse to cancel")
                except RuntimeError as e:
                    expect("cannot be cancelled" in str(e),
                           f"the cancel refusal explains itself: {e}")
                    refused_a_cancel = True
                time.sleep(0.2)
            else:
                raise AssertionError("async solve did not finish in 120s")

            res = d.call("study_results", study=study)
            expect(res["stale"] is False, f"async result cached: {res}")
            got = res["max_von_mises_mpa"]["value"]
            expect(abs(got / sigma - 1.0) < 0.05,
                   f"the worker's answer matches the sync one: {got} vs {sigma}")

            # Not an assertion about the solve: if the mesh is small enough
            # that it finished before the first poll, the refusal above never
            # ran and this test quietly stopped covering it. Say so rather
            # than report a pass that checked less than it claims.
            if not refused_a_cancel:
                print("fem_async_smoke: NOTE the solve finished before the "
                      "first poll, so the cancel refusal was not exercised",
                      file=sys.stderr)

            print("fem_async_smoke: OK")
            return 0
    except Exception as e:
        if "FEM support" in str(e) or "Netgen" in str(e):
            print("fem_async_smoke: SKIP (built without Netgen)")
            return SKIP
        raise


if __name__ == "__main__":
    sys.exit(main())
