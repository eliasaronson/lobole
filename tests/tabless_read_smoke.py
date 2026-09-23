#!/usr/bin/env python3
"""A READ does not switch tabs (docs/scripting/rpc.md#reads-name-their-part).

The app holds exactly one live document: a PartSlot's `doc` is moved into
AppState when the tab activates and moved back when it leaves, so "the active
part" is real in the document layer and most RPCs are gated on it. The SDK
inherited that and activated a part to read it.

That is not free. `ui::ResetActiveTabUiState` clears the selection, the
hidden-body set, the measure session, simulation mode and the explode state on
every switch. CLAUDE.md tells agents they may read from the human's running
instance freely — so a read that switches their tab silently throws away their
selection and un-hides everything they had hidden.

It is also unnecessary: `ui::PartDocById` reaches any slot whether it is active
or stashed. `list_elements` and `frame` already took `part`; `get_state` and
`mass_properties` now do too.

This test builds two parts and an assembly, then exercises every SDK read path
on the FIRST while something else is active, and asserts the active tab never
moved. The UI state that protects has no read-back RPC to assert on; not
switching is the mechanism, and that is what is checked.

Usage: tabless_read_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)
from designer_sdk import connect  # noqa: E402
from smoke_app import launch_app  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_tabless_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        a = connect(app.port)
        a.new()
        alpha = a.part("alpha")
        alpha.box(position=(0, 0, 0), w=10, h=10, d=10, name="A")
        beta = a.part("beta")
        beta.box(position=(0, 0, 0), w=20, h=20, d=20, name="B")

        def active_bodies():
            """What the ACTIVE tab holds — the tab's own identity, read
            without naming a part."""
            return [b["name"] for b in a.get_state()["bodies"]]

        # `beta` is active: every read of `alpha` below must leave it that way.
        expect(active_bodies() == ["B"], f"beta is active: {active_bodies()}")

        reads = {
            "state": lambda: alpha.state()["features"][0]["name"],
            "bodies": lambda: alpha.bodies()[0].name,
            "volume": lambda: round(alpha.volume(), 3),
            "bbox": lambda: alpha.bbox()[1].z,
            "faces": lambda: len(alpha.body().faces()),
            "edges": lambda: len(alpha.body().edges()),
            "frames": lambda: len(alpha.frames()),
            "element frame": lambda: alpha.body().faces()[0].frame().origin.z,
            "check": lambda: alpha.check(),
        }
        for name, read in reads.items():
            value = read()
            expect(active_bodies() == ["B"],
                   f"reading alpha's {name} switched the active tab to "
                   f"{active_bodies()} — a read must not activate")
            # And it really did read ALPHA, not whatever was in front.
            if name == "volume":
                expect(abs(value - 1000.0) < 1e-6,
                       f"alpha's volume is 1000, got {value}")
            if name == "bodies":
                expect(value == "A", f"alpha's body is A, got {value!r}")

        # ---- The same from an ASSEMBLY tab, where there is no part doc ----
        rig = a.assembly("rig")
        rig.place(alpha, fixed=True)
        rig.solve()
        expect(len(alpha.body().faces()) == 6,
               "a part reads the same from an assembly tab")
        expect(abs(alpha.volume() - 1000.0) < 1e-6,
               "and so does its volume")
        occ = a.list_occurrences()["occurrences"]
        expect(len(occ) == 1,
               f"the assembly tab is still in front after those reads: {occ}")
        # With no `part` and no part tab, it refuses rather than guessing.
        try:
            a.get_state()
            raise AssertionError("get_state answered for an assembly tab")
        except RuntimeError as e:
            expect("pass `part`" in str(e),
                   f"the refusal says the way through: {e}")

        # The per-tab UI state this protects — the selection, the hidden
        # bodies, the measure session — has no read-back RPC, so it cannot be
        # asserted here. It does not need to be: it is cleared by
        # ResetActiveTabUiState on a SWITCH, and the assertions above are that
        # no switch happens.
        print("tabless_read_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
