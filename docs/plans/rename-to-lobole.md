# Rename to Loböle — the product-visible surface

## The problem

"Designer" is generic: unsearchable, unownable as a mark, and indistinguishable
from the dozens of other things with that name. Loböle is distinctive, and
`lobole.org` and `lobole.dev` are held.

The urgency is that the name is about to be baked into identifiers that cannot
be changed afterwards. The Flatpak application id is the sharpest: a changed id
is a *different application* to Flatpak, so existing installs coexist rather
than upgrade. The repository is private and has no users, which is the only
moment every irreversible piece is free.

Loböle is the wordmark; `lobole` is the identifier everywhere — never the ö in
a binary name, a package name, a CMake target or a domain.

## Scope

**In:** everything a user or another program sees — the binary, the application
id, the per-user directories, the instance-registry handshake, the user-visible
strings, the documentation.

**Out:** internal identifiers (the CMake target names `designer_core`,
`designer_app`, `designer_adaptive`, `designer_tests`; the 34 `DESIGNER_*`
environment variables; C++ namespaces), and the document format (`kFormat`, the
`.dsgn` extension, and the 162 goldens carrying the format magic). Those are
invisible outside the build, and the format half would additionally break every
untracked project in `scratch/`. The format moves only alongside a `kVersion`
bump wanted for its own reasons.

## The shape

**The binary.** `set_target_properties(designer PROPERTIES OUTPUT_NAME lobole)`
renames the artifact without renaming the target, which keeps the out-of-scope
identifiers out of this change. The 98 `$<TARGET_FILE:designer>` generator
expressions then resolve on their own; the 22 hardcoded `build/designer` paths
across 17 files do not, and are rewritten.

**The application id.** `io.github.eliasaronson.Designer` → `org.lobole.Lobole`.
The domain form is the durable one: it does not embed a GitHub username, so it
survives a changed handle or a move into an organisation. Five files are *named*
after the id (`.yaml`, `.desktop`, `.metainfo.xml`, `.png`, `.svg`) and are
renamed with it.

**Per-user directories.** Every path funnels through
`src/common/RuntimePaths.cpp`: `$XDG_CONFIG_HOME/designer`,
`$XDG_STATE_HOME/designer`, `$XDG_CACHE_HOME/designer`, the installed
`../share/designer`, and the Windows `AppData/Local/Designer` and temp
fallbacks. One file. Existing directories are moved by hand as part of landing
this — a migration path in code would have no users to serve.

**The instance-registry handshake.** `{"app": "designer"}` is a contract between
the app, `tools/designer_remote.py`, the MCP server and `headless-preview.sh`.
Every site changes in one commit, or `Designer.instances()` silently finds
nothing and reports no error.

**User-visible strings**, 15 sites in `src/`: the Vulkan application name and
the window title (`app/main.cpp`), the About line (`ui/MainMenuBar.cpp`),
`--version` (`common/BuildInfo.cpp`), the glTF and mesh `generator` tags
(`core/io/GlbAnimation.cpp`, `cad/MeshExport.cpp`), the posted G-code comment
(`core/cam/CamPost.cpp`), the GitVault fallback author (`core/io/GitVault.cpp`),
the file-dialog filter (`ui/FileDialogs.cpp`), the password notice
(`ui/HistoryPanel.cpp`), and three Windows paths plus two comments quoting the
name.

**Documentation**, 57 markdown files. Note that "designer" is also the ordinary
English noun for the person using the application — `core/Measure.cpp` has
"interference a designer meant" and there are others. Those stay.

## Files it touches

| what | scale |
|---|---|
| user-visible strings in `src/` | 15 sites |
| `src/common/RuntimePaths.cpp` | all per-user and install paths |
| instance-registry handshake | `src/remote/InstanceRegistry.{h,cpp}`, `src/remote/RemoteDispatch.cpp`, `tools/designer_remote.py`, `tools/designer_mcp.py`, `tools/headless-preview.sh` |
| application id | 17 occurrences, 8 files, 5 of them renamed |
| hardcoded binary paths | 22 occurrences, 17 files |
| `CMakeLists.txt` | one `OUTPUT_NAME` property |
| documentation | 57 `.md` files |

Four case variants are four separate tokens: `Designer` (586), `designer`
(1563), `DESIGNER` (224), `dsgn` (319) — the last two are out of scope.
`benchmarks/results/*/runs.jsonl` is excluded: those are committed records of
runs that happened under the old name, and rewriting them falsifies a record.

## How it is verified

- `tools/test-fast.sh`, then the full headless suite, then
  `./tools/gui-tests-xvfb.sh`.
- `tests/linkage_gate.py` stays green **without being edited** — the library
  names are out of scope, so needing to touch it means the scope boundary was
  crossed.
- The `[docs]` convention test, which fails on any documentation path or
  `#anchor` that stopped resolving.
- Instance handshake end to end: `headless-preview.sh --start`, then
  `Designer.instances()` from the python client finds it, and the MCP server
  attaches to it.
- `tools/flatpak-build.sh` builds under the new id, and the installed desktop
  entry and icon resolve.
- Launch the GUI and read the window title, the About box, and `--version`.
- Export a glTF and post a G-code file; confirm the `generator` tag and the
  program comment.
- Open an existing `scratch/*.dsgn` project: the format is out of scope, so it
  must load untouched.
- The Windows cross-build, which is not in CI and is where a renamed install
  rule has gone wrong before.

## Not in this plan

The GitHub repository rename is a web operation, and GitHub redirects the old
URL. The SPDX header sweep follows this change rather than preceding it,
because the header text names the project.
