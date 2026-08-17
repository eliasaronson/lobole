#!/usr/bin/env python3
"""Remote-control client for Designer (start the app with --remote <port>).

Library:
    from designer_remote import Designer
    d = Designer(8437)
    d.call("add_feature", feature={"type": "Box", "name": "Box 1",
                                   "position": [0, 0, 0],
                                   "width": 2, "height": 2, "depth": 2})
    print(d.call("get_state"))

CLI:
    designer_remote.py <port> <method> ['<params-json>']
    designer_remote.py 8437 get_state
    designer_remote.py 8437 menu_click '{"path": "File/New"}'
    designer_remote.py 8437 screenshot '{"path": "/tmp/shot.png"}'
"""
import json
import socket
import sys
import time


class Designer:
    def __init__(self, port, host="127.0.0.1", timeout=60.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.file = self.sock.makefile("rw")
        self._id = 0

    def call(self, method, **params):
        self._id += 1
        request = {"id": self._id, "method": method, "params": params}
        self.file.write(json.dumps(request) + "\n")
        self.file.flush()
        while True:
            line = self.file.readline()
            if not line:
                raise ConnectionError("designer disconnected")
            msg = json.loads(line)
            if msg.get("id") == self._id:
                if "error" in msg:
                    raise RuntimeError(msg["error"])
                return msg.get("result")

    def close(self):
        # makefile() dups the socket fd — close BOTH, or the server never
        # sees EOF and keeps serving a ghost client (the remote handles one
        # client at a time, so the next connection would hang).
        self.file.close()
        self.sock.close()

    @staticmethod
    def instances():
        """Running instances that announced themselves, newest first.

        Each is {"pid", "port", "mode": "gui"|"headless", "started"}. The app
        writes one file per open remote port under
        $XDG_STATE_HOME/designer/instances (see src/remote/InstanceRegistry.h);
        a crash leaves the file behind, so the pid is the truth. Entries whose
        process is gone are skipped, and removed when we own them.
        """
        import os
        import pathlib
        state = os.environ.get("XDG_STATE_HOME") or os.path.expanduser(
            "~/.local/state")
        directory = pathlib.Path(state) / "designer" / "instances"
        found = []
        for path in sorted(directory.glob("*.json")) if (
                directory.is_dir()) else []:
            try:
                info = json.loads(path.read_text())
                pid = int(info.get("pid", 0))
                os.kill(pid, 0)  # liveness, not a signal
            except PermissionError:
                pass  # someone else's process: alive, just not ours to poke
            except (OSError, ValueError, json.JSONDecodeError):
                try:
                    path.unlink()  # stale or unreadable: it announces nothing
                except OSError:
                    pass
                continue
            found.append(info)
        return sorted(found, key=lambda i: i.get("started", 0), reverse=True)

    @staticmethod
    def wait_for(port, timeout=30.0):
        """Connect, retrying while the app starts up."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                return Designer(port)
            except OSError as e:
                last = e
                time.sleep(0.3)
        raise TimeoutError(f"designer not reachable on port {port}: {last}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    port = int(sys.argv[1])
    method = sys.argv[2]
    params = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
    d = Designer(port)
    try:
        print(json.dumps(d.call(method, **params), indent=2))
    finally:
        d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
