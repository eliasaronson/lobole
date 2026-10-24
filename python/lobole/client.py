#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Remote-control client for Loböle (start the app with --remote <port>).

Library:
    from lobole.client import Client
    d = Client(8437)
    d.call("add_feature", feature={"type": "Box", "name": "Box 1",
                                   "on": "Top",
                                   "width": 2, "depth": 2, "height": 2})
    print(d.call("get_state"))

CLI:
    python3 -m lobole.client <port> <method> ['<params-json>']
    python3 -m lobole.client 8437 get_state
    python3 -m lobole.client 8437 menu_click '{"path": "File/New"}'
    python3 -m lobole.client 8437 screenshot '{"path": "/tmp/shot.png"}'
"""
import json
import os
import socket
import sys
import time

from lobole.errors import (ConnectionLost, LoboleError, NonFiniteError,
                           Refused, UnknownArgument, refusal)


def _check_result(method, result):
    """A strict client holds an answer to its row's typed `returns` column
    (docs/scripting/rpc.md#typed-results): every smoke runs strict, so a
    key the handler adds, drops or retypes without its row fails the test
    that saw it, instead of reaching a caller as a KeyError."""
    from lobole.rpc import RESULT_SCHEMAS
    schema = RESULT_SCHEMAS.get(method)
    if schema is None:
        return
    from lobole.contracts import validate
    try:
        validate(schema, result, method, "/result")
    except Refused as e:
        raise LoboleError(f"{method}: the answer breaks its returns "
                          f"contract — {e}") from None



def plain_params(value):
    """`value` with every os.PathLike in it (a `pathlib.Path` given as a path
    parameter) replaced by the string it names, at any depth, so a typed
    contract sees the string it asks for."""
    if isinstance(value, os.PathLike):
        return os.fspath(value)
    if isinstance(value, dict):
        return {k: plain_params(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return type(value)(plain_params(v) for v in value)
    return value


def _jsonable(value):
    """A path parameter may be a `pathlib.Path` (or any os.PathLike): it goes
    over the wire as the string it names. Anything else JSON cannot hold is
    still an error."""
    if isinstance(value, os.PathLike):
        return os.fspath(value)
    raise TypeError(f"Object of type {type(value).__name__} is not JSON "
                    "serializable")

class Client:
    """`strict`: an answer holding a non-finite number raises NonFiniteError
    instead of warning. Unset, it follows LOBOLE_REMOTE_STRICT=1, which
    tests/smoke_app.py sets, so every smoke runs strict."""

    def __init__(self, port, host="127.0.0.1", timeout=60.0, strict=None):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.file = self.sock.makefile("rw")
        self._id = 0
        if strict is None:
            import os
            strict = os.environ.get("LOBOLE_REMOTE_STRICT") == "1"
        self.strict = strict

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
            extra = sorted(set(params or {}) - {"method"})
            if extra:
                raise UnknownArgument(
                    f"unknown parameter(s) {extra} for 'help'. Accepted: "
                    "method",
                    {"operation": "help", "code": "unknown_argument",
                     "path": f"/params/{extra[0]}", "param": extra[0],
                     "expected": ["method"],
                     "observed": (params or {})[extra[0]],
                     "accepted": ["method"]})
            return self.help((params or {}).get("method", ""))
        self._id += 1
        request = {"id": self._id, "method": method, "params": {} if params is None else params}
        previous = self.sock.gettimeout()
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            self.file.write(json.dumps(request, allow_nan=False,
                                       default=_jsonable) + "\n")
            self.file.flush()
            while True:
                line = self.file.readline()
                if not line:
                    raise ConnectionLost(f"{method}: the app disconnected "
                                         "before answering")
                msg = json.loads(line)
                # A line the server could not parse is answered with a null
                # id (it cannot know ours); with one request in flight it is
                # this one's, and waiting for our id would wait forever.
                if msg.get("id") is None and str(
                        msg.get("error", "")).startswith("parse:"):
                    raise refusal(msg["error"], msg.get("diagnostic"))
                if msg.get("id") == self._id:
                    if "nonfinite" in msg:
                        said = (f"{method}: the answer held non-finite "
                                f"numbers, sent as null, at "
                                f"{', '.join(msg['nonfinite'][:5])}")
                        if self.strict:
                            raise NonFiniteError(said, msg["nonfinite"],
                                                 msg.get("result"))
                        import warnings
                        warnings.warn(said, RuntimeWarning, stacklevel=3)
                    if "error" in msg:
                        raise refusal(msg["error"], msg.get("diagnostic"))
                    if self.strict:
                        _check_result(method, msg.get("result"))
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
        $XDG_STATE_HOME/lobole/instances (see src/remote/InstanceRegistry.h);
        a crash leaves the file behind, so the pid is the truth. Entries whose
        process is gone are skipped, and removed when we own them.
        """
        import os
        import pathlib
        state = os.environ.get("XDG_STATE_HOME") or os.path.expanduser(
            "~/.local/state")
        directory = pathlib.Path(state) / "lobole" / "instances"
        found = []
        for path in sorted(directory.glob("*.json")) if (
                directory.is_dir()) else []:
            try:
                info = json.loads(path.read_text())
                pid, port = info.get("pid"), info.get("port")
                # A whole pid above 0 and a port in range, as the app reads
                # them: kill(0) and kill(-1) ask about a whole process group
                # and answer "alive".
                if (not isinstance(pid, int) or isinstance(pid, bool)
                        or pid <= 0 or not isinstance(port, int)
                        or isinstance(port, bool) or not 0 < port < 65536):
                    raise ValueError("not an announcement")
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
    def wait_for(port, timeout=30.0, strict=None):
        """Connect, retrying while the app starts up."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                return Client(port, strict=strict)
            except OSError as e:
                last = e
                time.sleep(0.3)
        raise TimeoutError(f"Loböle not reachable on port {port}: {last}")


def method_help(call, method="", reference=None):
    """help(method) for every client: what the method does, the parameter
    keys it takes, and what it returns, from the app's own `describe` row.

    Shared by this client and the MCP server (lobole.mcp), so the
    two cannot drift into different answers. `call(name, **params)` reaches
    the app. `reference` is a client's own prose for methods it wraps,
    {method: {"doc": ..., ...}}: the MCP server passes the tool descriptions
    it keeps out of the context, and they win over the app's one-liner;
    the parameters, their types and the answer are always the app's row. An
    unknown method is refused naming close matches — the app's for its own
    methods, plus the close ones among `reference`.
    """
    reference = reference or {}
    if not method:
        rows = call("describe")["methods"]
        return {"methods": sorted({r["name"] for r in rows} | set(reference)),
                "hint": "help('add_mate') for one method; describe() for "
                        "the app's type tables and enums"}
    try:
        row = call("describe", method=method)["methods"][0]
    except Refused as e:
        # A client's own name for something that is not an RPC (the MCP
        # server's ui_action) has only its prose.
        if method in reference:
            return {"method": method, **reference[method]}
        import difflib
        near = [n for n in difflib.get_close_matches(method, sorted(reference),
                                                     n=5)
                if n not in str(e)]
        raise type(e)(str(e) + (f"; also {', '.join(near)}" if near else ""),
                      e.diagnostic) from None
    out = {"method": method,
           "doc": reference.get(method, {}).get("doc") or row.get("doc", "")}
    if row.get("params_free_form"):
        out["params_free_form"] = True
    else:
        out["params"] = row.get("params", [])
    # A typed method (docs/scripting/rpc.md#typed-contracts): each key's
    # type in words, from the same schema the server validates against.
    schema = row.get("request_schema") or {}
    if schema.get("properties"):
        from lobole.contracts import describe
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
    d = Client(port)
    try:
        print(json.dumps(d.request(method, params), indent=2))
    finally:
        d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
