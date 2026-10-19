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
- **A web page cannot drive it.** Browsers can send requests to localhost,
  but only as HTTP, and the server closes the connection at an HTTP request
  line or a `Host:` header, before the request's body is read.
- A running instance announces its port in
  `~/.local/state/lobole/instances/`.

So: **do not expose the remote port** to a network, a port forward, or a
container port published beyond a network you trust. Without `--remote` (and
with the terminal panel off, its default) no port is opened at all.

## Opening someone else's project

Opening a shared project or file should be safe, and a way it is not is a
vulnerability. A project cannot name a path outside its own folder
([docs/versioning/format.md](docs/versioning/format.md#paths-in-a-project)),
and git never runs commands from a repository that came inside a project
([docs/versioning/README.md](docs/versioning/README.md#a-shared-projects-history)).
What remains is what any CAD program carries: its geometry is parsed by
OpenCASCADE's readers, and its saved geometry can differ from what its
feature history would rebuild.

Other things to treat as code rather than data: generator scripts in
`generators/`, and any Python you are handed to run against the app.

## Reporting a vulnerability

Please report security problems **privately**, through GitHub's private
vulnerability reporting: on the repository page, open the **Security** tab and
choose **Report a vulnerability**
(<https://github.com/eliasaronson/lobole/security/advisories/new>). There is
no security email address.

Include what you found, how to reproduce it, the Loböle version or commit
(`lobole --version`), and what an attacker gains. You will get an answer on
the advisory; once a fix is available the advisory is published with credit,
unless you ask otherwise.

The behaviour described above (an unauthenticated loopback interface that
touches files) is by design and documented; a way to reach it that the
description above does not cover is a vulnerability.
