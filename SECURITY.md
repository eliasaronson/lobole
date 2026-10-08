# Security

## The remote interface is unauthenticated

Loböle can serve a JSON-RPC interface (`lobole --remote <port>`, also
opened on demand by the terminal panel) that scripts, the Python SDK, the MCP
server and the test suites drive. Know what it is before you run it:

- **It has no authentication.** Anything that can open a TCP connection to the
  port can drive the application.
- **It can read and write files** as the user running Loböle: open, save,
  import and export take paths, and `execute_script` in the MCP server runs
  Python.
- **It listens on 127.0.0.1 only**, unless you pass `--remote-bind-all`, which
  binds every interface. That flag exists for a container with its own
  network namespace (the headless image uses it). On a shared machine, other
  local users can still reach a loopback port.
- A running instance announces its port in
  `~/.local/state/lobole/instances/`.

So: **do not expose the remote port** to a network, a port forward, or a
container port published beyond a network you trust. Without `--remote` (and
with the terminal panel off, its default) no port is opened at all.

Other things to treat as code rather than data: generator scripts in
`generators/`, and any Python you are handed to run against the app.

## Reporting a vulnerability

Please report security problems **privately**, through GitHub's private
vulnerability reporting: on the repository page, open the **Security** tab and
choose **Report a vulnerability**
(<https://github.com/eliasaronson/Designer/security/advisories/new>). There is
no security email address.

Include what you found, how to reproduce it, the Loböle version or commit
(`lobole --version`), and what an attacker gains. You will get an answer on
the advisory; once a fix is available the advisory is published with credit,
unless you ask otherwise.

The behaviour described above (an unauthenticated loopback interface that
touches files) is by design and documented; a way to reach it that the
description above does not cover is a vulnerability.
