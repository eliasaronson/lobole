#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The MCP server's autostart: nothing running → spawn --offscreen, attach,
and the child dies with the server.

The contract worth pinning is the lifecycle, not the modeling (offscreen_smoke
owns that): discovery finding nothing spawns `--offscreen --remote 0`, the
port comes from the child's own registry announcement (no port race), ping
reports mode offscreen, and _stop_autostarted() leaves no process and no
registry entry behind — an orphan sitting on a port forever is the failure
the old autostart rejection named.

Skips (77) without the python mcp SDK (stock CI) or a usable Vulkan device
(the spawned child dies; its log reaches the error message we key on).
"""
import os
import sys
import tempfile



def main():
    tmp = tempfile.mkdtemp(prefix="designer_autostart_")
    # A private registry AND config — the child inherits both, so the test
    # never sees (or kills) the developer's real instances, and set_settings
    # in the child could never touch real preferences.
    os.environ["XDG_STATE_HOME"] = tmp
    os.environ["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    os.environ.pop("DESIGNER_PORT", None)  # a pin would disable autostart
    os.environ.pop("DISPLAY", None)
    os.environ.pop("WAYLAND_DISPLAY", None)
    try:
        import lobole.mcp as m
    except ModuleNotFoundError as e:
        print(f"SKIP: python mcp SDK not installed ({e})")
        return 77
    from lobole.client import Client

    try:
        port = m._resolve_port()
    except RuntimeError as e:
        if "VkResult" in str(e) or "no usable Vulkan device" in str(e):
            print(f"SKIP: no usable Vulkan device here ({e})")
            return 77
        raise
    child = m._autostarted
    assert child is not None and child.poll() is None, \
        f"_resolve_port returned {port} without a live autostarted child"

    d = Client.wait_for(port)
    pong = d.request("ping")
    assert pong["mode"] == "offscreen", f"autostart is offscreen ({pong})"
    assert pong["port"] == port, f"attached where it announced ({pong})"
    d.close()

    m._stop_autostarted()
    assert child.poll() is not None, "the child dies with the server"
    leftover = [i for i in Client.instances()
                if int(i.get("pid", 0)) == child.pid]
    assert not leftover, f"no registry orphan remains ({leftover})"
    print(f"autostart: spawned on {port}, attached offscreen, "
          "died with the server, registry clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
