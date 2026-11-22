#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A failed transport says what is safe, and nothing is sent twice.

Against a stand-in server that counts what it is sent, so "sent once" is a
count and not an inference (docs/scripting/sdk.md#errors,
docs/scripting/mcp.md#a-failed-call):

- a call given less time than it takes raises CallTimedOut (a TimeoutError
  and a LoboleError), the server saw it once, and the same client's next
  call gets its own answer (the late one is skipped);
- a connection the server closes while it has the call raises
  ConnectionLost with `sent` true, and the MCP server's _call does not send
  it again;
- a connection found closed before the call is written raises
  ConnectionLost with `sent` false, and _call sends it once on a new one;
- a refusal's tool error ends saying it is safe to send again corrected.

The real app's half (a check held longer than its caller waits) is in
requirements_smoke's off_frame. Needs no app and no mcp SDK.

Usage: client_transport_smoke.py
"""
import json
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lobole.client import Client  # noqa: E402
from lobole.errors import (CallTimedOut, ConnectionLost,  # noqa: E402
                           LoboleError, Refused)


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


class StandIn:
    """A one-port JSON-RPC server. `behaviour(method, conn, msg)` answers a
    request (return a result, or None having done something else to the
    connection); `seen` is every method it was sent, in order."""

    def __init__(self, behaviour):
        self.behaviour = behaviour
        self.seen = []
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(8)
        self.port = self.listener.getsockname()[1]
        self.connections = 0
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            self.connections += 1
            threading.Thread(target=self._serve, args=(conn,),
                             daemon=True).start()

    def _serve(self, conn):
        buf = b""
        while True:
            try:
                chunk = conn.recv(1 << 16)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, _, buf = buf.partition(b"\n")
                msg = json.loads(line)
                self.seen.append(msg["method"])
                result = self.behaviour(msg["method"], conn, msg)
                if result is _CLOSED:
                    return
                if result is not None:
                    try:
                        conn.sendall(json.dumps(
                            {"id": msg["id"], "result": result})
                            .encode() + b"\n")
                    except OSError:
                        return

    def close(self):
        self.listener.close()


_CLOSED = object()


def timed_out():
    def slow(method, conn, msg):
        if method == "run_slowly":
            time.sleep(1.0)
            return {"done": "run_slowly"}
        return {"done": method}
    server = StandIn(slow)
    d = Client(server.port, timeout=5)
    try:
        d.request("run_slowly", {}, timeout=0.3)
        expect(False, "a call given 0.3 s of a 1 s job answered")
    except CallTimedOut as e:
        expect(isinstance(e, TimeoutError) and isinstance(e, LoboleError),
               "CallTimedOut is a TimeoutError and a LoboleError")
        expect("do not send it again" in str(e) and "background_jobs"
               in str(e) and e.method == "run_slowly",
               f"the timeout says not to send it again: {e}")
    # The same client's next call is answered with ITS answer: the late
    # one, which arrives first, is skipped.
    expect(d.call("ping") == {"done": "ping"}, "the next call is answered")
    expect(server.seen == ["run_slowly", "ping"] and
           server.connections == 1,
           f"sent once, on the one connection: {server.seen}")
    d.close()
    server.close()


def closed_mid_call(m):
    def drop(method, conn, msg):
        if method == "add_feature":
            conn.close()
            return _CLOSED
        return {"port": 0, "mode": "headless"}
    server = StandIn(drop)
    d = Client(server.port, timeout=5)
    try:
        d.call("add_feature", feature={"type": "Box"})
        expect(False, "a closed connection answered")
    except ConnectionLost as e:
        expect(e.sent and "not known" in str(e) and "revision" in str(e),
               f"a loss mid-call says the outcome is not known: {e}")
    # The MCP server does not send it again.
    m._port, m._client = server.port, None
    try:
        m._call("add_feature", feature={"type": "Box"})
        expect(False, "the MCP server answered a closed connection")
    except ConnectionLost as e:
        expect(e.sent, f"surfaced as lost after sending: {e}")
    expect(server.seen.count("add_feature") == 2,  # one per client above
           f"_call sent it once: {server.seen}")
    d.close()
    server.close()


def stale_before_send(m):
    state = {"first": True}

    def stale(method, conn, msg):
        if method == "ping" and state["first"]:
            # Answer the attach, then close: the next call finds the
            # socket stale before it is written.
            state["first"] = False
            conn.sendall(json.dumps({"id": msg["id"], "result": {
                "port": 0, "mode": "headless"}}).encode() + b"\n")
            conn.close()
            return _CLOSED
        return {"done": method}
    server = StandIn(stale)
    m._port, m._client = server.port, None
    m._connect()
    time.sleep(0.2)  # the close reaches this side
    out = m._call("stand_in_read")
    expect(out == {"done": "stand_in_read"}, f"resent on a new connection: {out}")
    expect(server.seen.count("stand_in_read") == 1 and server.connections == 2,
           f"sent once, on a new connection: {server.seen}, "
           f"{server.connections} connections")
    m._client = None
    server.close()


def mcp_timeout(m):
    def slow(method, conn, msg):
        if method == "stand_in_check":
            time.sleep(1.0)
        return {"done": method}
    server = StandIn(slow)
    m._port = server.port
    m._client = Client(server.port, timeout=0.3)
    try:
        m._call("stand_in_check")
        expect(False, "answered inside the timeout")
    except CallTimedOut:
        pass
    expect(server.seen == ["stand_in_check"],
           f"the MCP server sent it once: {server.seen}")
    m._client.close()
    m._client = None
    server.close()


def refusal_text(m):
    said = m._after(Refused("unknown parameter 'x' for method undo"))
    expect(said.endswith("safe to send again corrected."), said)
    expect(m._after(CallTimedOut("t", "m", 1.0)) == "",
           "a timeout says what it says, nothing more")


def main():
    import mcp_formats_gate  # the SDK, or a stand-in where it is absent
    m = mcp_formats_gate.load_mcp_constants()
    m._autostart_enabled = False
    timed_out()
    closed_mid_call(m)
    stale_before_send(m)
    mcp_timeout(m)
    refusal_text(m)
    print("client_transport_smoke: a timeout, a loss mid-call and a stale "
          "socket each say what is safe, and nothing was sent twice")
    return 0


if __name__ == "__main__":
    sys.exit(main())
