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

Then the same through a wrapper that runs the app as its child, as an
AppImage's runtime and `flatpak run` do: the instance announces the CHILD's
pid, and autostart once waited for the wrapper's until it gave up and killed
it.

Skips (77) without the python mcp SDK (stock CI) or a usable Vulkan device
(the spawned child dies; its log reaches the error message we key on).
"""
import os
import stat
import sys
import tempfile
import time



def main():
    tmp = tempfile.mkdtemp(prefix="designer_autostart_")
    # A private registry AND config — the child inherits both, so the test
    # never sees (or kills) the developer's real instances, and set_settings
    # in the child could never touch real preferences.
    os.environ["XDG_STATE_HOME"] = tmp
    os.environ["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    os.environ.pop("LOBOLE_PORT", None)  # a pin would disable autostart
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

    # Through a wrapper. The trailing `exit` keeps the shell from exec'ing
    # the app in its own place, so the app really is a child.
    app = m._lobole_argv()
    wrapper = os.path.join(tmp, "wrapped-lobole")
    with open(wrapper, "w") as f:
        f.write('#!/bin/sh\n"$@"\nexit $?\n')
    os.chmod(wrapper, os.stat(wrapper).st_mode | stat.S_IEXEC)
    m._lobole_argv = lambda: [wrapper] + app
    wport = m._resolve_port()
    wchild = m._autostarted
    d = Client.wait_for(wport)
    pong = d.request("ping")
    d.close()
    assert pong["mode"] == "offscreen", f"wrapped autostart ({pong})"
    app_pid = [int(i["pid"]) for i in Client.instances()
               if int(i["port"]) == wport]
    assert app_pid and app_pid[0] != wchild.pid, \
        f"the app ran as the wrapper's child ({app_pid} vs {wchild.pid})"
    m._stop_autostarted()
    # The app follows its own parent, the wrapper, out (ParentWatch.h).
    end = time.monotonic() + 20
    while time.monotonic() < end and any(
            int(i["port"]) == wport for i in Client.instances()):
        time.sleep(0.2)
    assert not any(int(i["port"]) == wport for i in Client.instances()), \
        "the wrapped app died with its wrapper"

    print(f"autostart: spawned on {port}, attached offscreen, "
          "died with the server, registry clean; through a wrapper too")
    return 0


if __name__ == "__main__":
    sys.exit(main())
