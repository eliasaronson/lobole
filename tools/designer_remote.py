#!/usr/bin/env python3
"""Remote-control client for Designer (start the app with --remote <port>).

Library:
    from designer_remote import Designer
    d = Designer(8437)
    d.call("add_feature", feature={"type": "Box", "name": "Box 1",
                                   "on": "Top",
                                   "width": 2, "depth": 2, "height": 2})
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


class RemoteError(RuntimeError):
    """A server refusal, preserving the full structured diagnostic when present."""
    def __init__(self, message, diagnostic=None):
        super().__init__(message)
        self.diagnostic = diagnostic


class Designer:
    def __init__(self, port, host="127.0.0.1", timeout=60.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.file = self.sock.makefile("rw")
        self._id = 0

    # `method` is POSITIONAL-ONLY, so an RPC with a parameter of that name —
    # `describe {method}` — is reachable as call("describe", method="x"). It
    # used to be an ordinary argument, the keyword collided with it, and the
    # workaround was to know to use request() instead.
    def call(self, method, /, **params):
        return self.request(method, params)

    def help(self, method=""):
        """What `method` does and takes — `method_help` below, the same
        answer the MCP server's help() gives. With no method, the index.
        `call("help", method=...)` and `request("help", ...)` reach it too:
        the MCP instructions tell an agent to ask help(), and over this
        client that used to be "unknown method"."""
        return method_help(self.call, method)

    # Params as a DICT rather than keywords.
    def request(self, method, params=None, timeout=None):
        """`timeout` (seconds) overrides the connection default for THIS
        call. A modal solve on a fine mesh outlived the 60 s default, the
        client died mid-call, and the server finished anyway — which reads
        as a hang followed by a mystery. mesh_study/run_study want a long
        one (or run_study {async: true} + study_status polling)."""
        # `help` is answered here, from `describe`, rather than being a
        # second copy of the method table in the app (method_help).
        if method == "help":
            extra = set(params or {}) - {"method"}
            if extra:
                raise RemoteError(f"unknown parameter(s) {sorted(extra)} for "
                                  f"'help'. Accepted: method")
            return self.help((params or {}).get("method", ""))
        self._id += 1
        request = {"id": self._id, "method": method, "params": {} if params is None else params}
        previous = self.sock.gettimeout()
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            self.file.write(json.dumps(request, allow_nan=False) + "\n")
            self.file.flush()
            while True:
                line = self.file.readline()
                if not line:
                    raise ConnectionError("designer disconnected")
                msg = json.loads(line)
                if msg.get("id") == self._id:
                    if "error" in msg:
                        raise RemoteError(msg["error"], msg.get("diagnostic"))
                    return msg.get("result")
        finally:
            if timeout is not None:
                self.sock.settimeout(previous)

    def close(self):
        # makefile() dups the socket fd — close BOTH, or the server never
        # sees EOF and keeps serving a ghost client.
        #
        # The server accepts up to EIGHT clients (RemoteServer.cpp's
        # runaway-connection backstop) and drains them through ONE main-thread
        # queue, so it is execution that is serialized, not connection. An
        # earlier note here said it handled one client at a time; it does not,
        # and several agents driving several instances is the point of
        # docs/scripting/README.md#fan-out-several-agents-several-parts-one-project.
        self.file.close()
        self.sock.close()

    @staticmethod
    def instances():
        """Running instances that announced themselves, newest first.

        Each is {"pid", "port", "mode": "gui"|"headless"|"offscreen",
        "started"}. The open project is not here — it changes while the
        instance runs; `ping` reports it. The app
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


def method_help(call, method="", reference=None):
    """help(method) for every client: what the method does, the parameter
    keys it takes, and what it returns, from the app's own `describe` row.

    Shared by this client and the MCP server (tools/designer_mcp.py), so the
    two cannot drift into different answers. `call(name, **params)` reaches
    the app. `reference` is a client's own prose for methods it wraps,
    {method: {"doc": ..., ...}}: the MCP server passes the tool descriptions
    it keeps out of the context, and they win over the app's one-liner. An
    unknown method is refused naming close matches — the app's for its own
    methods, plus the close ones among `reference`.
    """
    reference = reference or {}
    if not method:
        rows = call("describe")["methods"]
        return {"methods": sorted({r["name"] for r in rows} | set(reference)),
                "hint": "help('add_mate') for one method; describe() for "
                        "the app's type tables and enums"}
    if method in reference:
        return {"method": method, **reference[method]}
    try:
        row = call("describe", method=method)["methods"][0]
    except RemoteError as e:
        import difflib
        near = [n for n in difflib.get_close_matches(method, sorted(reference),
                                                     n=5)
                if n not in str(e)]
        raise RemoteError(str(e) + (f"; also {', '.join(near)}" if near
                                    else "")) from None
    out = {"method": method, "doc": row.get("doc", "")}
    if row.get("params_free_form"):
        out["params_free_form"] = True
    else:
        out["params"] = row.get("params", [])
    # A typed method (docs/scripting/rpc.md#typed-contracts): each key's
    # type in words, from the same schema the server validates against.
    schema = row.get("request_schema") or {}
    if schema.get("properties"):
        from designer_contracts import describe
        required = set(schema.get("required", []))
        out["param_types"] = {
            k: describe(s) + ("" if k in required else " (optional)")
            for k, s in schema["properties"].items()}
    for key in ("returns", "requires_part_doc"):
        if key in row:
            out[key] = row[key]
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    port = int(sys.argv[1])
    method = sys.argv[2]
    params = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
    d = Designer(port)
    try:
        print(json.dumps(d.request(method, params), indent=2))
    finally:
        d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
