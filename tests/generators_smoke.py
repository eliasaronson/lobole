#!/usr/bin/env python3
"""The generators build what they say, headless.

generators/gear.py is the answer to "would we need scripted features for a
gear" (docs/capability-review-2026-08-31.md): a Python generator against the
API, an ordinary part out. This runs it against a headless app and checks
the part by numbers — no display, no GPU.

Usage: generators_smoke.py <path-to-designer-binary>
"""
import math
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
from smoke_app import launch_app  # noqa: E402


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    binary = sys.argv[1]
    state = tempfile.mkdtemp(prefix="designer-generators-")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(state, "config"))
    app = launch_app(binary, env=env)  # headless
    try:
        from designer_remote import Designer
        Designer.wait_for(app.port).close()

        m, z, t, bore = 2.0, 24, 8.0, 6.0
        gear = os.path.join(ROOT, "generators", "gear.py")
        out = subprocess.run(
            [sys.executable, gear, "--port", str(app.port), "--module", str(m),
             "--teeth", str(z), "--thickness", str(t), "--bore", str(bore)],
            capture_output=True, text=True, env=env, timeout=120)
        expect(out.returncode == 0,
               f"gear.py failed:\n{out.stdout}\n{out.stderr}")
        expect("192 sketch entities" in out.stdout,
               f"24 teeth is 8 entities a tooth ({out.stdout.strip()})")

        d = Designer(app.port)
        try:
            s = d.call("get_state")
            errs = [(f["name"], f["error"]) for f in s["features"]
                    if f.get("error")]
            expect(not errs, f"features with errors: {errs}")
            expect(len(s["bodies"]) == 1, f"one body, got {len(s['bodies'])}")
            b = s["bodies"][0]
            rp = m * z / 2
            ra, rf = rp + m, rp - 1.25 * m
            v_bore = math.pi * (bore / 2) ** 2 * t
            lo_v = math.pi * rf ** 2 * t - v_bore
            hi_v = math.pi * ra ** 2 * t - v_bore
            expect(lo_v < b["volume"] < hi_v,
                   f"volume {b['volume']:.1f} outside root..tip "
                   f"({lo_v:.1f}..{hi_v:.1f})")
            # Roughly half the annulus between root and tip is tooth — a
            # profile that came out as petals or as a full disc fails this.
            frac = (b["volume"] - lo_v) / (hi_v - lo_v)
            expect(0.35 < frac < 0.65,
                   f"tooth fill fraction {frac:.2f} is not gear-like")
            for i, (name, want) in enumerate(
                    (("x", ra), ("y", ra), ("z", t / 2))):
                half = (b["bbox_max"][i] - b["bbox_min"][i]) / 2
                expect(abs(half - want) < 1e-3,
                       f"{name} half-extent {half:.4f}, expected {want:.4f}")
            # The live half: thickness is an expression over a variable.
            v = d.call("set_variable", name="gear_thickness", expression="12")
            expect(abs(v["value"] - 12.0) < 1e-9, "variable set")
            s2 = d.call("get_state")
            expect(abs(s2["bodies"][0]["bbox_max"][2] - 12.0) < 1e-3,
                   "the gear thickened with its variable")
        finally:
            d.close()
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print("generators_smoke: gear.py built a gear and it stays parametric")
    return 0


if __name__ == "__main__":
    sys.exit(main())
