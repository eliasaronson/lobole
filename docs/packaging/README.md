# Packaging: building and shipping

How Loböle is built outside a developer's `nix-shell`, and what each
channel delivers. Day-to-day builds (`nix-shell shell.nix --run "cd build &&
ninja …"`) are in the repository's `CLAUDE.md` and `CONTRIBUTING.md`.

| channel | who it is for | recipe | GUI |
|---|---|---|---|
| Nix flake | Nix and NixOS users | `flake.nix` | yes |
| Container image | CI, servers, agents | `flake.nix` `dockerHeadless`, `.github/workflows/docker.yml` | no |
| `nix bundle` AppImage | headless use on any distro | `nix bundle --bundler github:ralismark/nix-appimage .#default` | no |
| Flatpak | desktop users on any distro | `packaging/flatpak/`, `tools/flatpak-build.sh` | yes |
| Windows | Windows users | `packaging/windows/` (cross-compiled on Linux) | yes |

The Vulkan driver (ICD) is always the host's; no channel bundles one, and
every channel needs a working Vulkan driver for the GUI.

## The install layout

CMake's `install()` rules define the layout every packager uses:
`bin/designer`; the shared libraries `libplanegcs.so` and
`libdesigner_adaptive.so` under the library directory, found through an
`$ORIGIN`-relative RUNPATH; runtime data (shaders, fonts, the HDRI, the
standard-parts tables) under `share/designer`; and the `.desktop` file,
AppStream metainfo (`DESIGNER_INSTALL_METAINFO`, default ON) and icons (SVG
and a 256×256 PNG) under their standard `share/` locations. The metadata
sources are in `packaging/`.

**Every shared library the app links needs its own `install()` rule.** Every
test runs from the build tree, where the libraries sit beside the binary, so
a missing rule only shows in an installed build, as `cannot open shared
object file` at start. To check, `cmake --install` to a scratch prefix and
`ldd` the result.

At run time `RuntimePaths` (`src/common/RuntimePaths.{h,cpp}`) resolves:

- **read-only data**: `DESIGNER_DATA_DIR` if set, else the staged build-tree
  directories beside the executable, else `../share/designer` relative to it;
- **per-user directories**, never beside the executable (read-only under
  Nix, Flatpak and an installed prefix): `UserConfigDir` (settings),
  `UserStateDir` (autosaves, recent projects, the instance registry, import
  staging), `UserCacheDir` (the precomputed IBL maps). On Linux these are
  `$XDG_CONFIG_HOME` / `$XDG_STATE_HOME` / `$XDG_CACHE_HOME` + `/designer`,
  falling back to `~/.config`, `~/.local/state`, `~/.cache`. On Windows,
  which has neither `$HOME` nor `XDG_*`, they are
  `%LOCALAPPDATA%\Lobole\{config,state,cache}`, never `%TEMP%`, which
  Windows sweeps.

## Crash reports

A session that ends badly leaves a folder the user can zip and send, and the
next start says so (docs/architecture.md#crashes-and-stalls). Nothing is
sent anywhere; `Help ▸ Crash reports…` opens the folder, which is under the
per-user state directory:

| OS | where |
|---|---|
| Linux | `$XDG_STATE_HOME/lobole/crashes/`, else `~/.local/state/lobole/crashes/` (the Flatpak's own `~/.var/app/<id>/.local/state/lobole/crashes/`) |
| Windows | `%LOCALAPPDATA%\Lobole\state\crashes\` |
| macOS | `~/.local/state/lobole/crashes/`, as Linux without `XDG_STATE_HOME` |

Each report holds `report.txt` (what happened, the build, the raw stack),
`session.log`, copies of that session's autosaves and, on Windows,
`crash.dmp`. A release build ships no line numbers and the Windows build no
in-process symbolizer, so the raw frames are module + offset:
`tools/symbolize-crash.py report.txt BINARY` names them against the
unstripped binary of the build the report's `Build:` line names (the mingw
`x86_64-w64-mingw32-addr2line` for `lobole.exe`). **macOS is untested**:
the POSIX handler and the dyld module table compile for it by design, but
nothing here can build or run it.

## The Nix flake

`flake.nix` pins nixpkgs (`nixos-unstable`, OpenCASCADE 7.9.3) and the four
git submodules (imgui, imgui_test_engine, nativefiledialog-extended, clipper2)
as `flake = false` inputs, so `nix build` needs no `?submodules=1`. Keep those
revisions in sync with `.gitmodules` when a submodule moves; a missing
clipper2 does not fail the build, it configures CAM off. PlaneGCS, libarea
and the fonts are vendored in-tree.

- `packages.x86_64-linux.default` (= `designer`): a Release build of the app
  only, installed through the CMake install phase. Netgen is
  `nix/netgen-lean.nix`: built without MPI, Python and its GUI, which Loböle
  never uses and which would otherwise pull numpy and a Fortran compiler into
  the runtime closure through metadata references.
- `packages.x86_64-linux.dockerHeadless`: a layered OCI image running
  `lobole --headless --remote 8437 --remote-bind-all`. The RPC server is
  unauthenticated and loopback-only by default; `--remote-bind-all` is right
  inside a container's namespaced network, and the port should not be
  published to a network you do not trust. CI builds it, smokes it and
  pushes it to GHCR (`.github/workflows/docker.yml`).
- `checks.x86_64-linux.headless`: builds `designer`, `designer_tests` and
  `femgpu_tests` and runs `ctest -LE gui` in the sandbox with a clean
  `$HOME` and GPU topology disabled (`DESIGNER_TOPO_GPU=0`).
- `devShells.x86_64-linux.default`: `nix develop`, the same toolchain as
  `nix-shell shell.nix` plus validation layers and the Python tooling.

On NixOS the loader finds the host driver through `/run/opengl-driver`.
There is no binary cache yet, so `nix run` builds OpenCASCADE and Netgen from
source unless the user's store already has them.

## The headless AppImage

`nix bundle --bundler github:ralismark/nix-appimage .#default` closure-packs
the flake's package into a self-contained AppImage. It runs `--headless`,
`--remote` and the document engine on any distro. **It cannot run the
windowed GUI, on any distro**: a Vulkan app and the host GPU driver must
share one glibc in one process, and the bundle carries its own newer glibc.
With the bundle's glibc in charge the host driver's `.so` cannot be found;
with the host's, the bundle's own libraries fail on their newer glibc
symbols. On NixOS the bundle's `/nix/store` mount also shadows
`/run/opengl-driver`, so every ICD dangles. The Flatpak runtime is what owns
that glibc-to-driver contract. Nothing in CI builds the AppImage.

## The Flatpak

`packaging/flatpak/org.lobole.Lobole.yaml`, built and
user-installed by `tools/flatpak-build.sh` (user mode through `nix-shell`, no
root and no system Flatpak service needed; `--run` launches the result). It
builds against the Freedesktop 24.08 runtime and SDK, not the Nix
derivation. The first build compiles OpenCASCADE from source (tens of
minutes); the script passes `--ccache`, so reruns are fast. Work and state
directories live outside the repository, because the app module's `type:
dir` source would otherwise copy them into itself.

- Modules built from source: OpenCASCADE `V7_9_3` (Draw off, since the SDK
  has no Tk; Visualization on, because the app links `TKVCAF` → `TKV3d`;
  RapidJSON on for the glTF writer), Netgen `v6.2.2605` (OCC on; GUI, Python
  and MPI off), and glm, glfw (X11 only), spdlog, nlohmann_json, Eigen,
  Spectra, RapidJSON and Boost. imgui, nativefiledialog-extended and PlaneGCS
  build with the app. The SDK itself provides gcc, glslc, Vulkan headers,
  freetype, dbus and X11.
- **Boost must be a real install**, not a header copy: `find_package(Boost)`
  is quiet, and without it the PlaneGCS sketch solver is silently disabled.
  The manifest `b2 install`s the `system` library to get `BoostConfig.cmake`.
- The app module passes `DESIGNER_BUILD_TESTS=OFF` so configure does not try
  to fetch Catch2 inside the offline sandbox.
- The SDK's gcc ships without `libstdc++exp`, so CMake defines
  `HAVE_STDCXX_STACKTRACE` only when `std::stacktrace` links, and the crash
  handler otherwise falls back to glibc `backtrace()`.
- finish-args: `--share=ipc`, `--socket=fallback-x11`, `--socket=wayland`,
  `--device=dri`, `--filesystem=host`, and `--share=network` so the
  `--remote` RPC server is reachable from host clients.
- The local build turns off AppStream metainfo
  (`-DDESIGNER_INSTALL_METAINFO=OFF`) and deletes the SVG icon in
  `post-install`: a bare `nix-shell` lacks the gdk-pixbuf loaders that
  `appstreamcli compose` and `flatpak build-export`'s icon check need.
- File dialogs go through `xdg-desktop-portal`, which the app already uses.
- The host's `ffmpeg` is not on `PATH` inside the sandbox, so motion video
  export to mp4, webm or gif needs an `ffmpeg` inside it; without one the
  export refuses by name (`ping`'s `features.ffmpeg` is false) and PNG frames
  still work. If ffmpeg is ever bundled, it must be an LGPL build shipped
  with a source offer.

## Windows

The whole toolchain is in `packaging/windows/` and runs on Linux through
nixpkgs cross (`pkgsCross.mingwW64`), taken from `<nixpkgs>` on the
`NIX_PATH` rather than the flake's pin. No Windows machine or MSVC is
involved.

```
nix-build packaging/windows/cross-app.nix --cores 8   # bin/ + share/
nix-build packaging/windows/installer.nix             # portable .zip + NSIS setup.exe
```

`--cores 8`: at full parallelism the cross build runs out of memory mid-link
on a 30 GB machine.

- **`occ-cross.nix`** builds OpenCASCADE with Draw and Visualization off:
  stock nixpkgs pulls Tcl/Tk (and through them bash) for Draw, which do not
  build for Windows.
- **Boost is headers only**: Boost's own cross-build fails on its MPI
  component, so a hand-written `BoostConfig.cmake` exposes the
  `Boost::boost` interface target.
- `allowUnsupportedSystem` for header-only libraries that do not list mingw;
  spdlog's tests are off; `mingw_w64_pthreads` for imgui_test_engine.
- **`installer.nix` copies with `cp -rL`**: several DLLs nix puts in `bin/`
  are symlinks into other store paths. A dangling one makes a DLL that is
  present report itself "not found", because what is actually missing is a
  DLL it imports.
- The NSIS installer is 32-bit; testing install-then-run under Wine needs
  WoW64 (or the 32-bit `pkgs.wine` for the installer and `wine64` for the
  app).

What a Windows build has: modeling, the PlaneGCS solver, Spectra modal
analysis, CAM including adaptive clearing, the GPU topology solver, and the
windowed renderer with ray tracing where the driver offers it. What it does
not have: **Netgen meshing**, so FEM studies that need a mesh are
unavailable (Netgen's source assumes MSVC — `/bigobj`-style flags, `FARPROC`
casts; `netgen-cross.nix` holds the attempt and its blocker notes), and the
terminal panel (`ENABLE_TERMINAL` is `NOT WIN32`). `ping` reports which
optional subsystems a build has.

Verifying a build, all from Linux: `wine designer.exe --version`; a
headless `--remote` instance driven from the host over 127.0.0.1 in which a
sketch constraint actually SOLVES (a circle whose radius is constrained from
1 to 2 changes the extruded volume accordingly — the check that separates
"OCC loaded" from "PlaneGCS is linked in"); and `wine designer.exe --gui-tests
--gui-test-filter boots_and_renders --hidden` on the real GPU through
winevulkan, from the extracted portable zip, which is what a tester gets. A
crash report: started with `LOBOLE_ALLOW_DEBUG_CRASH=1` and
`WINEDLLOVERRIDES=winedbg.exe=d` (so Wine's debugger does not take the
crash), `debug_crash {kind: "segv"}` after a couple of edits leaves a
`.crash` with module + RVA frames and a `.dmp`, the next start collects
them, and `tools/symbolize-crash.py` against that `lobole.exe` names
`remote::DebugCrash`.

Rules for code, since nothing in CI builds this target:

- **Nothing in `src/` is named `near` or `far`.** `windows.h` defines both as
  empty macros, so `double far = -1.0;` becomes `double = -1.0;` on the one
  platform nothing tests; a central `#undef` does not survive the next
  include. `tests/test_conventions.cpp` scans for the names.
- **Platform code is `#ifdef _WIN32`-guarded with the Linux path unchanged**:
  `RuntimePaths` (`GetModuleFileNameA`, `%LOCALAPPDATA%`), `CrashHandler`
  (`SetUnhandledExceptionFilter`, the stack walked with `RtlVirtualUnwind`,
  `MiniDumpWriteDump` from `dbghelp`), `common/Platform` (`LockFileEx`,
  `ShellExecuteW`), the remote server (Winsock), `GitVault` (`nul`,
  `_pclose`), `_getpid`.
- `std::filesystem::path::native()` is a `wstring` on Windows; compare path
  components, not string prefixes.
- A second non-inline definition of a function that also exists inline links
  silently on ELF and fails on PE/COFF.

## Licensing

The project is AGPL-3.0-or-later (`LICENSE`), with a section-7 additional
permission to link Dear ImGui Test Engine (`ADDITIONAL_PERMISSION.md`); the
flake's `meta.license` says the same. `THIRD_PARTY.md` inventories every
runtime component and how it is linked, and `LICENSES/` holds the full
texts, the OCCT exception and the corresponding-source notice; CMake
installs them with every binary.

- LGPL libraries (OpenCASCADE with its exception, Netgen, PlaneGCS, libarea
  for adaptive clearing) are linked dynamically, so a user can relink against
  a modified version. PlaneGCS and libarea are modified, which is why they
  are built as `libplanegcs.so` and `libdesigner_adaptive.so` rather than
  linked statically; their modifications are published with the source
  (libarea's as the dated patches in `deps/libarea/patches/`).
- imgui_test_engine is not test-only: the `ui_action` RPC drives the GUI
  through it at run time. Its licence permits derivatives released under an
  OSI licence.
- Everything else (Eigen and Spectra MPL-2.0, Boost, Clipper2 and libarea's
  Clipper 1 BSL-1.0, glfw, spdlog, nlohmann_json, glm, imgui,
  nativefiledialog-extended, libvterm, the fonts) is permissive.
- The standard-parts tables in `assets/parts/` are data under their own
  licences: the fastener tables GPL-2.0-or-later, the bd_warehouse hole tables
  Apache-2.0, with texts and the attribution notice in `LICENSES/`.

## The agent surface

The MCP server and the Python client ship separately from the app: the
repository's `pyproject.toml` packages `tools/designer_mcp.py` and
`tools/designer_remote.py` with `designer-mcp` / `designer-remote` entry
points (`uv tool install <repo>`, then `claude mcp add lobole --
designer-mcp`). `tests/packaging_smoke.py` gates the metadata. See
[../scripting/README.md](../scripting/README.md).

## Build options that change what ships

| option | default | effect |
|---|---|---|
| `DESIGNER_BUILD_TESTS` | ON | configure the test targets |
| `DESIGNER_INSTALL_METAINFO` | ON | install AppStream metainfo |
| `ENABLE_CAM` | ON | milling toolpaths and G-code; OFF is a supported configuration CI builds and tests |
| `ENABLE_CAM_ADAPTIVE` | ON | adaptive clearing (LGPL libarea, shared) |
| `ENABLE_TERMINAL` | ON | the embedded terminal (libvterm); never on Windows |
| `DESIGNER_ENABLE_FEM_GPU` | ON | the Vulkan-compute topology solver |
| `ENABLE_TEST_ENGINE` | ON | the ImGui test engine (`--gui-tests`, `ui_action`); CI also builds without it |

## Not done

- A binary cache (Cachix) for the flake, so `nix run` substitutes instead of
  building.
- Building the headless AppImage in CI.
- Flathub: pin every source by `commit:` + `sha256`, re-enable metainfo and
  the SVG icon, confirm the renderer on the runtime's Mesa / llvmpipe (only
  NVIDIA has run it), narrow `--filesystem=host` toward the portal, and
  submit.
- Windows: Netgen, real-hardware QA, code signing (an unsigned installer
  trips SmartScreen), a stripped bundle, and any CI job that cross-builds.
- macOS (MoltenVK; no ray tracing there, and the GPU path cannot be tested
  off a Mac), and native deb / rpm / AUR packages (distributions ship older
  OpenCASCADE than 7.9).
