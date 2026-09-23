#!/usr/bin/env python3
"""The watch generator builds a watch that keeps time, headless.

generators/watch/ is a 21,600 vph automatic with small seconds
(generators/watch/README.md). This builds its parts and its
assembly against a headless app and checks the things that make it a watch:

- every part builds, on its check_geometry RECEIPT (an empty part's `valid`
  is vacuously true, and the receipt is what says so);
- every moving part is mated — nothing in it holds a typed transform;
- the going train and the motion works keep time EXACTLY: one minute of the
  barrel turns the centre wheel 6 deg, the fourth wheel (small seconds) one
  turn, the escape wheel twelve, the hour wheel half a degree;
- the crown winds the ratchet, crown in, and sets the hands without moving
  the train, crown out;
- the declared clearances hold, on their receipt.

The generator's own run does the expensive half — the exact sweep, the motion
study, the renders — and refuses to save without it. This is the part of it
that must never regress quietly.

Usage: watch_smoke.py <path-to-designer-binary>
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "generators", "watch"))
sys.path.insert(0, HERE)
from designer_sdk import App  # noqa: E402
from smoke_app import launch  # noqa: E402


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: watch_smoke.py <designer-binary>")
    import watch  # noqa: E402 — after sys.path
    import watch_parts
    import watch_spec

    with launch(sys.argv[1]) as launched:
        app = App(launched.designer)
        app.call("new_project")
        dims = watch_spec.declare(app)
        watch.__dict__.update(dims)
        watch_parts.__dict__.update(dims)
        n_vars = len(app.list_variables()["variables"])
        expect(n_vars >= 50, f"the spec's dimensions are document variables "
                             f"(W1): only {n_vars} declared")

        P = watch.build_parts(app)
        bad = watch.check_parts(app, P)
        expect(not bad, "every part builds, on its receipt:\n  "
                        + "\n  ".join(bad))

        asm, occ, m, rel, solved = watch.build_assembly(app, P)
        expect(solved.get("errored_mates", 1) == 0,
               f"no mate errors: {solved.get('errored_mates')}")
        expect(solved.get("placed_by_transform") == 0,
               f"every occurrence is placed by a mate, none by a typed "
               f"transform: {solved.get('placed_by_transform')} of "
               f"{solved.get('occurrences')}")

        turn, why = watch.check_ratios(app, m)
        expect(not why, "the train keeps time:\n  " + "\n  ".join(why))

        # The crown winds (crown in) and sets the hands (crown out) — the
        # kinematics only; the generator's run sweeps both positions.
        _, why = watch.check_winding(app, m)
        expect(not why, "the crown winds:\n  " + "\n  ".join(why))
        _, why = watch.check_setting(app, occ, m, rel, sweep=False)
        expect(not why, "the crown sets the hands:\n  " + "\n  ".join(why))

        watch.declare_clearances(app, occ)
        c = app.verify_assertions(_timeout=600)
        expect(c["receipt"]["standing"] == "verified",
               "the declared clearances hold: " + "; ".join(
                   f"{r['name']} {r['standing']} {r.get('measured_mm')}"
                   for r in c["assertions"] if r["standing"] != "holds"))
    print("watch_smoke: ok")


if __name__ == "__main__":
    main()
