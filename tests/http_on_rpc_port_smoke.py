#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A web page cannot drive the app through its JSON-RPC port.

A browser POSTs to localhost without a preflight when the body is text/plain,
so any open site can send this. The server read one request per line and
refused only the lines that failed to parse: the HTTP headers were refused one
by one and the JSON in the body RAN. It now hangs up on the request line.

Usage: http_on_rpc_port_smoke.py <path-to-designer-binary>
"""
import socket
import sys

from smoke_app import launch  # noqa: E402

BODY = ('{"id":1,"method":"add_feature","params":{"feature":{"type":"Box",'
        '"name":"FromBrowser","on":"Top","width":5,"depth":5,"height":5}}}\n')


def send_raw(port, data):
    """Send `data`, then read until the server closes or goes quiet."""
    s = socket.create_connection(("127.0.0.1", port), 5)
    s.sendall(data.encode())
    s.settimeout(3)
    got = b""
    try:
        while chunk := s.recv(4096):
            got += chunk
    except socket.timeout:
        raise AssertionError("the server kept the HTTP connection open")
    finally:
        s.close()
    return got.decode(errors="replace")


def feature_names(client):
    return [f.get("name") for f in client.call("get_state")["features"]]


def main():
    with launch(sys.argv[1]) as app:
        for name, request in [
            ("POST", "POST / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                     "Origin: https://example.com\r\n"
                     "Content-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s"
                     % (app.port, len(BODY), BODY)),
            # A WebSocket upgrade starts with a GET; a DNS-rebinding page
            # sends its own Host. Neither may reach the body either.
            ("GET", "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n" + BODY),
            ("Host first", "Host: 127.0.0.1\r\n\r\n" + BODY),
        ]:
            answer = send_raw(app.port, request)
            if "FromBrowser" in feature_names(app.client):
                raise AssertionError(f"{name}: the JSON in an HTTP body ran")
            if answer:
                raise AssertionError(f"{name}: answered {answer[:200]!r}")

        # A real client on its own connection is unaffected.
        app.client.call("add_feature", feature={
            "type": "Box", "name": "Plain", "on": "Top",
            "width": 5, "depth": 5, "height": 5})
        if "Plain" not in feature_names(app.client):
            raise AssertionError("a plain JSON-RPC client stopped working")
    print("http_on_rpc_port_smoke: OK")


if __name__ == "__main__":
    main()
