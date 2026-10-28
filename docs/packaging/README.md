# Packaging: building and shipping

How Loböle is built outside a developer's `nix-shell`, and what each
channel delivers. Day-to-day builds (`nix-shell shell.nix --run "cd build &&
ninja …"`) are in the repository's `CLAUDE.md` and `CONTRIBUTING.md`.

| channel | who it is for | recipe | GUI |
|---|---|---|---|
| Nix flake | Nix and NixOS users | `flake.nix` | yes |
| Container image | CI, servers, agents | `flake.nix` `dockerHeadless` | no |
| GUI AppImage | desktop users on glibc 2.39 or newer | `packaging/appimage/` | yes |
| `nix bundle` AppImage | headless use on any distro | `nix bundle --bundler github:ralismark/nix-appimage .#default` | no |
| Flatpak | desktop users on any distro | `packaging/flatpak/`, `tools/flatpak-build.sh` | yes |
| Windows | Windows users | `packaging/windows/` (cross-compiled on Linux) | yes |

The Vulkan driver (ICD) is always the host's; no channel bundles one, and
every channel needs a working Vulkan driver for the GUI.

A release is built from one tag by `.github/workflows/release.yml`, which
publishes both AppImages, the Ubuntu `.deb`, the Windows zip and
installer, the source tarball, the Python package and the container image ([Releasing](#releasing)). The flake and the
Flatpak are built from the source.

## One version

`VERSION` at the repository root is the version, and the only place it is
written: CMake's `project(VERSION)`, the flake, the Windows packaging and
`pyproject.toml` all read it. The AppStream metainfo's newest `<release>`
must name the same version, which `test_conventions.cpp` checks along with
the absence of a literal anywhere else.

## Releasing

1. Raise `VERSION`, and add the release to the top of `<releases>` in
   `packaging/org.lobole.Lobole.metainfo.xml` with its date and a
   `<description>`. That description IS the release notes: software
   centres show it, and the GitHub release takes the same text
   (`tools/release-notes.py`).
2. Run the full [action matrix](../modeling/README.md#action-matrix) here,
   against the committed baseline (`tools/ctest -C action_matrix -L
   action_matrix -j4`): CI runs only its sample, and a pass that became a
   fail on any face of the corpus is a regression a release should not
   carry. `tools/ci-local.sh --asan` belongs here too (CLAUDE.md).
3. Run the release workflow by hand (`workflow_dispatch`): it builds and
   smokes everything and publishes nothing.
4. Push the tag `v<VERSION>`.

On the tag, `release.yml` refuses a tag that is not `v` + `VERSION` or whose
metainfo release is missing or has no notes. It then builds, in parallel:

- the headless AppImage from the flake (`nix bundle`, the bundler pinned by
  revision);
- the GUI AppImage in `ubuntu:24.04` ([The GUI AppImage](#the-gui-appimage)),
  then run in fresh Ubuntu 24.04, Debian 13 and Fedora images, windowed
  as well as headless;
- the container image (`.#dockerHeadless`);
- the Windows portable zip and installer, cross-built against the flake's
  nixpkgs (`NIX_PATH` is pinned to it, rather than a channel);
- the Ubuntu 26.04 `.deb` ([The Debian package](#the-debian-package)),
  smoked after a clean `apt-get install` of it;
- the source tarball ([The source tarball](#the-source-tarball)), and from
  it the AUR package, built, installed and smoked in Arch
  ([Arch Linux (AUR)](#arch-linux-aur));
- the Python sdist and wheel.

**Every one is started before it is published.** `tools/release-smoke.py`
starts it headless (the Windows one under Wine), waits for `ping`, then
makes it model: a 4 mm box must be 64 mm³ (OpenCASCADE works) and a circle
drawn at r 1 and constrained to r 2 must extrude to 8π mm³ (the sketch
solver is linked; a build that loses Boost configures PlaneGCS off without
a word). With `--version` it also checks that the binary's `--version` is
the release's. The two GUI builds also open a window and render one frame
(`app/boots_and_renders`, on Xvfb with Mesa's lavapipe): the AppImage in
fresh images (`packaging/appimage/smoke.sh`), the Windows zip under Wine
from a folder named `Loböle`, as the installer names it
(`packaging/windows/smoke.sh`). Nix's Xvfb and Wine look for GL and Vulkan
drivers only at `/run/opengl-driver`, so on the runner that job links Nix's
Mesa there, as NixOS does; without it Xvfb has no GLX and Wine no Vulkan. The wheel goes through `tests/packaging_smoke.py`, which
builds it from the sdist, as pip does with a PyPI release.

Only when all of that passes does the tag publish: a GitHub release with
every file, `SHA256SUMS`, and the notes; the wheel and sdist to PyPI by
trusted publishing (configured once on pypi.org for project `lobole`,
workflow `release.yml`, environment `pypi`; no token is stored); the image
to GHCR as `lobole-headless:<version>` and `:latest`.

**Cachix.** With the repository variable `CACHIX_CACHE` (the cache's name)
and the secret `CACHIX_AUTH_TOKEN` set, every nix job in the workflow pulls
from and pushes to that cache. Without them it builds OpenCASCADE and
Netgen from source, which is slow but correct.

## The install layout

CMake's `install()` rules define the layout every packager uses:
`bin/lobole`; the shared libraries `libplanegcs.so` and
`libdesigner_adaptive.so` under the library directory, found through an
`$ORIGIN`-relative RUNPATH; runtime data (shaders, fonts, the HDRI, the
standard-parts tables) under `share/lobole`; and the `.desktop` file,
AppStream metainfo (`LOBOLE_INSTALL_METAINFO`, default ON) and icons (SVG
and a 256×256 PNG) under their standard `share/` locations. The metadata
sources are in `packaging/`; the icons are made from the master mark by
`generators/logo/brand.py` ([assets/brand](../../assets/brand/README.md)).

The desktop entry launches `lobole %f`: a lone path on the command line is
opened as `--file` opens it (a project, or a CAD file imported), so "Open
With" and a file dropped on the launcher reach the app. No MIME type is
registered for projects. A project is a `.lob` FOLDER, and file managers type
every directory `inode/directory` whatever its name, so no shared-mime-info
glob can make one open on a double-click
([versioning](../versioning/README.md#the-dsgn-extension)).

**Every shared library the app links needs its own `install()` rule.** Every
test runs from the build tree, where the libraries sit beside the binary, so
a missing rule only shows in an installed build, as `cannot open shared
object file` at start. To check, `cmake --install` to a scratch prefix and
`ldd` the result.

At run time `RuntimePaths` (`src/common/RuntimePaths.{h,cpp}`) resolves:

- **read-only data**: `LOBOLE_DATA_DIR` if set, else the staged build-tree
  directories beside the executable, else `../share/lobole` relative to it;
- **per-user directories**, never beside the executable (read-only under
  Nix, Flatpak and an installed prefix): `UserConfigDir` (settings),
  `UserStateDir` (autosaves, recent projects, the instance registry, import
  staging), `UserCacheDir` (the precomputed IBL maps). On Linux these are
  `$XDG_CONFIG_HOME` / `$XDG_STATE_HOME` / `$XDG_CACHE_HOME` + `/lobole`,
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

## When it will not start

A start that cannot get a window and a graphics device says why, in words,
and exits with code **3** (docs/architecture.md#when-the-renderer-cannot-start):
on stderr, in the session log, in `last-startup-failure.txt` in the state
directory above (`%LOCALAPPDATA%\Lobole\state\` on Windows), and — for a
desktop launch, which has no terminal — in a native box: `MessageBoxW` on
Windows; `zenity`, else `kdialog`, else a desktop notification naming the
file on Linux. The next start that draws a window says it once. The message
is what to read first; by what it names:

- **"create the Vulkan instance", "load Vulkan", "find a graphics device"**:
  no Vulkan driver works here. Install or update the GPU driver and the
  Vulkan loader — Debian and Ubuntu `libvulkan1 mesa-vulkan-drivers`, Fedora
  `vulkan-loader mesa-vulkan-drivers`, Arch `vulkan-icd-loader` and
  `vulkan-intel`, `vulkan-radeon` or `nvidia-utils`; on Windows the driver
  from the GPU's maker, since the one Windows installs often has no Vulkan.
  In a virtual machine, turn on 3D acceleration.
- **"create the graphics device", "set up the swapchain"**: every device was
  tried and failed (the message lists each). A driver update usually fixes
  it; `LOBOLE_RENDERER=software lobole` renders on Mesa's lavapipe where it
  is installed (Linux only: nothing is bundled, and stock Windows has none).
- **"start the windowing system", "open the main window"**: no desktop
  session reached the app (`DISPLAY` / `WAYLAND_DISPLAY`).
- **Anything that crashes before the window draws** — a preference, a
  layout: `lobole --safe-mode` starts on default settings and layout and
  writes neither, and two such crashes in a row start it by themselves
  (docs/ui/settings.md#safe-mode).
- **No window needed?** `lobole --headless --remote 0` serves the scripting
  surface with no GPU at all; `--offscreen` renders without a window.

Landing on a lesser device than the best one present is not a failure: the
app says so once, keeps a notice in the menu bar, and Help ▸ About names the
device. A device that stops working mid-session (a driver reset, an eGPU
unplugged) is rebuilt in place and the session goes on
(docs/rendering/README.md#rebuilding-the-device); only when that fails, or
the device is lost again within 60 s, is the work saved, a report written
and a restart offered (docs/rendering/README.md#a-lost-device).

The release smokes check the first case on every package:
`packaging/appimage/smoke.sh` and `packaging/windows/smoke.sh` start the app
with `VK_ICD_FILENAMES=/nonexistent` and `LOBOLE_NO_DIALOG=1`, expecting
exit 3 and the failure file; the AppImage smoke runs it once more without
`LOBOLE_NO_DIALOG`, `zenity`, `kdialog` or a session bus, to show the chain
ends at the file rather than a hang.

## The Nix flake

`flake.nix` pins nixpkgs (`nixos-unstable`, OpenCASCADE 7.9.3) and the five
git submodules (imgui, imgui_test_engine, nativefiledialog-extended, clipper2,
manifold) as `flake = false` inputs, so `nix build` needs no `?submodules=1`.
`tests/flake_submodules_gate.py` fails when one of those revisions differs
from the commit the repository records for the submodule; a missing
clipper2 does not fail the build, it configures CAM off. PlaneGCS, libarea
and the fonts are vendored in-tree.

- `packages.x86_64-linux.default` (= `lobole`): a Release build of the app
  only, tests off, installed through the CMake install phase. Netgen is
  `nix/netgen-lean.nix`: built without MPI, Python and its GUI, which Loböle
  never uses and which would otherwise pull numpy and a Fortran compiler into
  the runtime closure through metadata references.
- `packages.x86_64-linux.dockerHeadless`: a layered OCI image running
  `lobole --headless --remote 8437 --remote-bind-all`. The RPC server is
  unauthenticated and loopback-only by default; `--remote-bind-all` is right
  inside a container's namespaced network, and the port should not be
  published to a network you do not trust. A release tag builds it, smokes
  it and pushes it to GHCR ([Releasing](#releasing)).
- `checks.x86_64-linux.headless`: builds the app, `designer_tests` and
  `femgpu_tests` and runs `ctest -LE gui` in the sandbox with a clean
  `$HOME` and GPU topology disabled (`LOBOLE_TOPO_GPU=0`).
- `devShells.x86_64-linux.default`: `nix develop`, the same toolchain as
  `nix-shell shell.nix` plus validation layers and the Python tooling.

OpenCASCADE is built with the patches in `nix/patches/`, through the
overlay in `nix/occt-overlay.nix` that `shell.nix` and the flake both
apply, so netgen links the same patched library. The Flatpak manifest and
`packaging/windows/occ-cross.nix` apply the same files, and
`packaging/appimage/deps.sh` too; a new patch goes in all four, and into
the list each writes to the install's `share/lobole/occt-patches` (what
`occpatch::Carries` and `ping`'s `occ_patches` read). Two make a Shell's
result independent of memory addresses (one is upstream's OCCT#1600,
backported) and one lets its deadline stop the offset's intersections
([modeling](../modeling/README.md#a-reshaping-feature-keeps-only-a-valid-solid));
the other three turn a fillet that never returned and three faults inside
the kernel into ordinary failures
([modeling](../modeling/README.md#a-caught-kernel-fault)), each written up
for upstream in `nix/patches/upstream/`. The seventh is upstream's
OCCT#980 without its header changes: a per-thread error-handler stack, so
that booleans on many threads stop queueing on one mutex
([performance](../performance/README.md#the-kernels-error-handler-lock)).
The Debian package builds against Ubuntu's own OpenCASCADE and carries
none.
Patching OpenCASCADE means nixpkgs' binary cache no longer has it: the first
`nix-shell` on a machine compiles OpenCASCADE and netgen.

On NixOS the loader finds the host driver through `/run/opengl-driver`.
Elsewhere a Nix-built Vulkan app cannot see the system's driver, so the
window needs [nixGL](https://github.com/nix-community/nixGL)
(`nixGL lobole`); headless runs anywhere.

**The binary cache** is `eliasaronson.cachix.org`, which the flake's
`nixConfig` names: Nix asks before using it, or `--accept-flake-config`
skips the question. The release workflow fills it (the repository variable
`CACHIX_CACHE` and the secret `CACHIX_AUTH_TOKEN`, [Releasing](#releasing)),
so a released commit downloads in a minute; a later `main` still finds
OpenCASCADE and Netgen there and builds only the app.

## GCC 16 and IPA-CP clones

GCC 16.2 at `-O3` miscompiles OpenCASCADE's `BRepClass3d_SClassifier.cxx`,
natively and in the mingw cross build alike. It clones
`NCollection_UBTree::Select` for the line selector, devirtualized, and sends
the point selector's recursion into that clone, so
`BRepClass3d_BndBoxTreeSelectorLine::Accept` runs on the point selector and
reads a curve from past its end. Every point-in-solid test can call through
a garbage vtable in `Extrema_ExtCC::Perform`: on Windows a Hole crashed
(`tests/fuzz/cases/crash-181.jsonl`, 'H6'), on Linux every boolean's
refusal check did, and 66 headless tests with it. `-O2`, or `-O3` without
the cloning, is right; GCC 15 is right.

So OpenCASCADE is built with `-fno-ipa-cp-clone` (`nix/occt-overlay.nix`,
`packaging/windows/occ-cross.nix`), and so is the app under GCC 16 or newer
(`CMakeLists.txt`; `cross-app.nix` for Windows), because it instantiates
the same templates from OpenCASCADE's headers. The Flatpak's SDK compiler is
older and needs neither. nixpkgs moved to GCC 16 under a build tree
configured before the move: CMake keeps the library paths it found, so such
a tree still links the GCC 15 OpenCASCADE and passes, and the next fresh
configure is the one that crashes. The miscompile is not reported to GCC.

## OpenCASCADE 7.6

Ubuntu 24.04 ships 7.6; development is against 7.9. CI builds a stock
`ubuntu:24.04` container on every push, which pins what differs on 7.6:

- **No PLY export**: `RWPly_CafWriter` arrived in 7.7, so PLY refuses and
  names the version.
- **Two tests fail** where the kernel changed by 7.9 (`Assembly import
  plan…`, `Shell that removes no material…`); the features work.
- **Some booleans are wrong**, such as a sphere touching a cylinder's floor
  at one point. `fuzz_seeds_smoke` lists those seeds by the `occ` version
  `ping` reports and expects them to keep failing.

For full parity use Ubuntu 26.04, Nix or the Flatpak.

## The GUI AppImage

A conventional AppImage, not a Nix bundle: built on the oldest glibc it
supports, with every library inside except glibc and the graphics stack,
which come from the host. That is the split the [headless
AppImage](#the-headless-appimage) cannot make, and why this one opens a
window. It has no sandbox, so the terminal panel, git and ffmpeg are the
user's own, unlike in the [Flatpak](#the-flatpak).

- `packaging/appimage/build.sh VERSION OUTDIR` runs as root in a fresh
  `ubuntu:24.04` (glibc 2.39, gcc 13). It runs on that glibc and newer:
  Ubuntu 24.04+, Mint 22, Debian 13, Fedora 40+, Arch.
- Everything comes from apt except OpenCASCADE (24.04 has 7.6) and Netgen,
  which `packaging/appimage/deps.sh` builds into `/opt/lobole-deps`: OCC
  `V7_9_3` with the patches in `nix/occt-overlay.nix`, Netgen `v6.2.2605`,
  each checked against its commit, with the Flatpak manifest's flags. The
  release caches that prefix on `deps.sh` and the patches.
- linuxdeploy (a pinned release, checksum checked) copies the libraries and
  sets rpaths; its excludelist leaves glibc, libGL, X11 and xcb to the host.
  We also leave out the Vulkan loader, so the host's finds the host's
  drivers, and D-Bus: bundled, it brought `libsystemd` and `libgcrypt`
  without the `libgpg-error` the excludelist keeps out, and Debian 13 failed
  to start it. appimagetool (pinned) packs it with the static runtime, so
  the host needs FUSE but not `libfuse2`, and no user namespaces.
- `packaging/appimage/smoke.sh APPIMAGE VERSION` installs only what a
  desktop already has (the Vulkan loader with Mesa, X11, fontconfig,
  D-Bus) plus Xvfb and python3, runs `tools/release-smoke.py`, then opens
  the window on Xvfb for `app/boots_and_renders`. Containers have no FUSE,
  so it runs with `APPIMAGE_EXTRACT_AND_RUN=1`.
- NixOS runs no generic AppImage without `appimage-run`; Nix users have the
  flake.

To build one here: `docker run --rm -v "$PWD:/src:ro" -v "$PWD/dist:/out"
ubuntu:24.04 /src/packaging/appimage/build.sh <version> /out` (about 20
minutes, most of it OpenCASCADE; mount a directory at `/opt/lobole-deps`
to keep that between runs).

## The headless AppImage

`nix bundle --bundler github:ralismark/nix-appimage .#default` closure-packs
the flake's package into a self-contained AppImage. It runs `--headless`,
`--remote` and the document engine on any distro. **It cannot run the
windowed GUI, on any distro**: a Vulkan app and the host GPU driver must
share one glibc in one process, and the bundle carries its own newer glibc.
With the bundle's glibc in charge the host driver's `.so` cannot be found;
with the host's, the bundle's own libraries fail on their newer glibc
symbols. On NixOS the bundle's `/nix/store` mount also shadows
`/run/opengl-driver`, so every ICD dangles. The [GUI
AppImage](#the-gui-appimage) leaves glibc to the host instead. This one
stays for where that one cannot run: a server with no Vulkan loader, or a
glibc older than 2.39. A release builds it and smokes it headless
([Releasing](#releasing)).

What it needs from the host, measured in a stock `ubuntu:24.04` container:
`/dev/fuse` (no `libfuse2`: the runtime is static) and unprivileged user
namespaces, because its launcher bind-mounts the bundle's `/nix` in one.
Without FUSE, `APPIMAGE_EXTRACT_AND_RUN=1` extracts instead, but still
needs the namespace. So it does not run in a default Docker container (use
the container image there), and on Ubuntu 23.10 and later, which restrict
unprivileged user namespaces through AppArmor, it needs
`sysctl kernel.apparmor_restrict_unprivileged_userns=0` — what the release
workflow sets on its Ubuntu runner before the smoke. No Ubuntu desktop has
run it yet.

## The Debian package

A `.deb` for **Ubuntu 26.04 only**, the first release whose OpenCASCADE
(7.9.2) matches ours; 24.04's 7.6 has different library names and the
faults [above](#opencascade-76). It is the [install layout](#the-install-layout)
under `/usr`, built by CPack (`-DLOBOLE_PACKAGE_DEB=ON
-DCMAKE_INSTALL_PREFIX=/usr`, then `cpack`) against Ubuntu's own libraries,
so it uses the system's Vulkan driver and is the full GUI, FEM meshing
and modal analysis included (Netgen and the header-only Spectra).

- **Depends** comes from `dpkg-shlibdeps`, read off what the binary links,
  so it cannot drift from the link line. By hand only what is run rather
  than linked: Recommends `mesa-vulkan-drivers, git`; Suggests
  `ffmpeg, python3`.
- **Netgen** is Ubuntu's `libnglib-dev`. Its `NetgenConfig.cmake` checks
  that every target's file exists, the `netgen` binary and the Python
  modules included, and stops configure without them, so the build also
  installs `netgen` and `python3-netgen`; the package links `libnglib`
  and `libngcore` alone. The `ubuntu:26.04` CI job installs the same, so
  the FEM tests run against the Netgen the package ships with.
- A release builds it in one `ubuntu:26.04` container and installs it in
  a fresh one with `apt-get install ./lobole_<version>-1_amd64.deb`, which
  resolves Depends as a user's machine does, then runs
  `tools/release-smoke.py` on `/usr/bin/lobole`.

It is a download, not an apt repository: no updates arrive by themselves.
A PPA or a repository of our own would be the next step.

## The source tarball

`tools/source-tarball.sh <version> <dir>` writes
`lobole-<version>-src.tar.gz`: the repository at HEAD with each submodule
at the commit HEAD records for it, from `git archive` (never the working
tree), gzipped without a timestamp, so one commit gives the same bytes. It
has no `.git`, so it carries `.lobole-commit`, which CMake reads for About's
build commit when git cannot answer. A release attaches it, so a packager
names one file and one checksum instead of pinning every submodule again.

## Arch Linux (AUR)

`packaging/aur/PKGBUILD` builds the release's source tarball against Arch's
own packages (OpenCASCADE 7.9 is in its repositories) with
`-DLOBOLE_BUILD_TESTS=OFF`, as every packaging build does: the test tree
would fetch Catch2 during the build. A release fills in `pkgver` and the
tarball's checksum, builds it with `makepkg` in an `archlinux` container,
installs it and runs `tools/release-smoke.py`, and keeps the `PKGBUILD` and
`.SRCINFO` as the `aur` artifact. Arch has neither Netgen nor Spectra in its
repositories, so the package has no FEM meshing or modal analysis.

Publishing is pushing those two files to `aur.archlinux.org/lobole.git`
from an AUR account with an SSH key; nothing does that yet.

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
- The app module passes `LOBOLE_BUILD_TESTS=OFF` so configure does not try
  to fetch Catch2 inside the offline sandbox.
- The SDK's gcc ships without `libstdc++exp`, so CMake defines
  `HAVE_STDCXX_STACKTRACE` only when `std::stacktrace` links, and the crash
  handler otherwise falls back to glibc `backtrace()`.
- finish-args: `--share=ipc`, `--socket=x11`, `--device=dri`,
  `--filesystem=host`, and `--share=network` so the `--remote` RPC server
  is reachable from host clients.
- **X11, not `fallback-x11`.** GLFW is built X11-only. `fallback-x11` grants
  X11 only when no Wayland session is running, so on a Wayland desktop the
  app got no `DISPLAY` and never opened its window; `--socket=x11` reaches
  Xwayland there.
- The local build turns off AppStream metainfo
  (`-DLOBOLE_INSTALL_METAINFO=OFF`) and deletes the SVG icon in
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
- **No IPA-CP clones** (`-fno-ipa-cp-clone`, OpenCASCADE and the app):
  [GCC 16 and IPA-CP clones](#gcc-16-and-ipa-cp-clones).
- **Boost is headers only**: Boost's own cross-build fails on its MPI
  component, so a hand-written `BoostConfig.cmake` exposes the
  `Boost::boost` interface target.
- `allowUnsupportedSystem` for header-only libraries that do not list mingw;
  spdlog's tests are off; `mingw_w64_pthreads` for imgui_test_engine.
- **`installer.nix` copies with `cp -rL`**: several DLLs nix puts in `bin/`
  are symlinks into other store paths. A dangling one makes a DLL that is
  present report itself "not found", because what is actually missing is a
  DLL it imports.
- **The icon** (`assets/icons/org.lobole.Lobole.ico`, 16-256 px) is compiled
  into `lobole.exe` as the resource `GLFW_ICON` (`lobole.rc.in`): GLFW loads
  that name for every window's title bar and taskbar button, and as the
  executable's only icon it is also the one Explorer and a shortcut show.
  The installer and uninstaller carry it too (`Icon` in `installer.nsi`).
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

Verifying a build, all from Linux: `wine lobole.exe --version`; a
headless `--remote` instance driven from the host over 127.0.0.1 in which a
sketch constraint actually SOLVES (a circle whose radius is constrained from
1 to 2 changes the extruded volume accordingly — the check that separates
"OCC loaded" from "PlaneGCS is linked in"); and `wine lobole.exe --gui-tests
--gui-test-filter boots_and_renders --hidden` on the real GPU through
winevulkan, from the extracted portable zip, which is what a tester gets. A
crash report: started with `LOBOLE_ALLOW_DEBUG_CRASH=1` and
`WINEDLLOVERRIDES=winedbg.exe=d` (so Wine's debugger does not take the
crash), `debug_crash {kind: "segv"}` after a fillet (which arms
OpenCASCADE's handlers) leaves a `.crash` with module + RVA frames and a
`.dmp`, the next start collects them, and `tools/symbolize-crash.py`
against that `lobole.exe` names `remote::DebugCrash`. Then, on the same
instance kind: `debug_fault {kind: "occ_fault"}` answers an
`OSD_Exception_ACCESS_VIOLATION` for both threads instead of dying; the
fuzzer's `tests/fuzz/cases/crash-153.jsonl` and `crash-181.jsonl` replay to
the end (`python/lobole/replay.py`); `--stall-test` logs a main-thread
stack that `symbolize-crash.py` names (`StallSelfTest`); and a Ctrl+C
(`SIGINT` to the Wine process, which Wine turns into `CTRL_C_EVENT`) after a
fillet stops headless and leaves no report. `smoke.sh` itself checks faults
in the system's own code on a worker thread (`tools/release-smoke.py
--system-faults`): `debug_crash {kind: "system_probe"}` (`IsBadReadPtr`,
which catches its own fault) answers and leaves no report, and
`{kind: "system_fault"}` (msvcrt's `memcpy` from an unmapped address)
leaves one whose frames are msvcrt's, then `lobole.exe`'s
([architecture](../architecture.md#crashes-and-stalls)). All of it runs
under Wine; the
crash path has not run on real Windows, though the app has been tested
there by hand.

Rules for code, since nothing builds this target on a push (a release
cross-builds it and runs `packaging/windows/smoke.sh` on it under Wine):

- **A narrow string is UTF-8.** libstdc++'s `std::filesystem` reads and
  writes a narrow path as UTF-8 on Windows, and `lobole.manifest` makes
  UTF-8 the process's code page too, so `getenv`, `fopen` and every `…A`
  API agree with it. Without the manifest the install folder,
  `C:\Program Files\Loböle`, came back from `GetModuleFileNameA` as
  Windows-1252, and the first `path::string()` (the font, as the window
  opened) threw. Headless never loads the font, which is why the smoke
  runs the window from a folder with an "ö" in it. Where a wide API exists
  (`GetModuleFileNameW`), use it anyway.

- **Nothing in `src/` is named `near` or `far`.** `windows.h` defines both as
  empty macros, so `double far = -1.0;` becomes `double = -1.0;` on the one
  platform nothing tests; a central `#undef` does not survive the next
  include. `tests/test_conventions.cpp` scans for the names.
- **Platform code is `#ifdef _WIN32`-guarded with the Linux path unchanged**:
  `RuntimePaths` (`GetModuleFileNameW`, `%LOCALAPPDATA%`), `CrashHandler`
  (`SetUnhandledExceptionFilter`, the stack walked with `RtlVirtualUnwind`,
  `MiniDumpWriteDump` from `dbghelp`), `common/Platform` (`LockFileEx`,
  `ShellExecuteW`), the remote server (Winsock), `GitVault` (`nul`,
  `_pclose`), `_getpid`.
- `shutdown()` does not wake a `recv` blocked on another thread on Windows;
  `RemoteServer::Stop` cancels the socket's I/O (`CancelIoEx`) too, or a
  headless instance asked to stop with a client connected waited for good.
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
  Clipper 1 BSL-1.0, manifold Apache-2.0, glfw, spdlog, nlohmann_json, glm, imgui,
  nativefiledialog-extended, libvterm, the fonts) is permissive.
- The standard-parts tables in `assets/parts/` are data under their own
  licences: the fastener tables GPL-2.0-or-later, the bd_warehouse hole tables
  Apache-2.0, with texts and the attribution notice in `LICENSES/`.

## The agent surface

The Python package `lobole` — the client, the SDK and the MCP server — ships
separately from the app: the repository's `pyproject.toml` builds it from
`python/lobole/` with `lobole-mcp` / `lobole-remote` entry points
(`uv tool install <repo>`, then `claude mcp add lobole -- lobole-mcp`).
`tests/packaging_smoke.py` builds the wheel and imports every module from
it alone. The app also stages the package beside its assets and installs it
under `share/lobole/python/`, so the terminal panel can start the MCP server
(`python3 -m lobole.mcp`) against its own instance
([terminal](../ui/terminal.md)). See
[../scripting/README.md](../scripting/README.md).

## Build options that change what ships

| option | default | effect |
|---|---|---|
| `LOBOLE_BUILD_TESTS` | ON | configure the test targets |
| `LOBOLE_INSTALL_METAINFO` | ON | install AppStream metainfo |
| `ENABLE_CAM` | ON | milling toolpaths and G-code; OFF is a supported configuration CI builds and tests |
| `ENABLE_CAM_ADAPTIVE` | ON | adaptive clearing (LGPL libarea, shared) |
| `ENABLE_TERMINAL` | ON | the embedded terminal (libvterm); never on Windows |
| `LOBOLE_ENABLE_FEM_GPU` | ON | the Vulkan-compute topology solver |
| `ENABLE_TEST_ENGINE` | ON | the ImGui test engine (`--gui-tests`, `ui_action`); CI also builds without it |

## Not done

- Flathub: pin every source by `commit:` + `sha256`, re-enable metainfo and
  the SVG icon, confirm the renderer on the runtime's Mesa / llvmpipe (only
  NVIDIA has run it), narrow `--filesystem=host` toward the portal, and
  submit.
- Windows: Netgen, regular real-hardware QA (it has been tested by hand,
  not per release), code signing (an unsigned installer
  trips SmartScreen) and a stripped bundle. A release cross-builds and
  smokes it under Wine; nothing builds it on a push.
- Publishing the AUR package (an AUR account, an SSH key, and a release
  job that pushes the `aur` artifact).
- macOS (MoltenVK; no ray tracing there, and the GPU path cannot be tested
  off a Mac), and rpm packages (the GUI AppImage covers Fedora meanwhile).
