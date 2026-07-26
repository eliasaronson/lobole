# Packaging & distribution plan

Goal: ship Designer as installable Linux binaries. The distribution is
**layered by audience** (tested & settled 2026-07-25):

| Audience | Delivery | Cost | Status |
|---|---|---|---|
| Nix users (NixOS + Nix-on-other-distro) | `nix run github:…/designer` + **Cachix** | near-free (flake exists) | Cachix pending |
| Headless / CI / servers / scripting | **`nix bundle` AppImage** (CLI/engine/RPC, **no GUI**) | near-free (already builds) | ship, labeled |
| Everyone else, **desktop GUI** | **Flatpak** → Flathub | ~1–1.5 wk | the real desktop release |

The two build-effort deliverables, in order:

1. **`flake.nix`** exposing `packages.x86_64-linux.default` — reproducible
   build + the source of truth for the dependency set. Serves Nix/NixOS users
   directly and becomes the reference the Flatpak manifest is written against.
2. **Flatpak** (Flathub-targetable) — the "runs on any distro" public GUI
   release. **Required, not optional**: a `nix bundle` AppImage was tested for
   the GUI and rejected (glibc↔GPU-driver conflict — see Deliverable 1's
   AppImage section); Flatpak's runtime is the only clean owner of that contract.

We are already all-in on Nix (`shell.nix`), so the flake is a low-effort lift
and the Flatpak reuses everything the flake pins.

**Windows** cross-compiled from Linux since 2026-07-26 (Deliverable 3). Still
deliberately **out of scope**: native distro packages (deb/rpm/AUR), Snap, and
macOS. See "Deferred" below for why (and how macOS could be developed without a Mac).

---

## Why these two

- **Flatpak** wins for a GPU-heavy Vulkan app: its runtime provides a glibc that
  matches its Mesa + `GL.nvidia-*` driver extensions, so the app and the host
  GPU driver share one glibc *by construction*. That is the exact wall a
  `nix bundle` AppImage cannot clear (its bundled glibc 2.42 can neither find nor
  co-load a host driver — tested 2026-07-25, see Deliverable 1). It also uses
  `xdg-desktop-portal`, which our `FileDialogs` already targets, and it dedups
  shared runtimes/files across apps (lowest disk cost of the bundle formats).
- **Flake** because the build is already Nix-expressed. It gives us
  reproducible release artifacts + CI builds for free, reaches every Nix user
  (NixOS *and* Nix-on-other-distro), and documents the exact dep versions/flags
  the Flatpak manifest needs.

The GPU driver (Vulkan ICD) is always the host's responsibility — we never
bundle it. Flatpak just matches userspace to it; document "needs a working
Vulkan driver" for every channel.

---

## Deliverable 1 — `flake.nix`

Lift the `shell.nix` dependency list into a proper package derivation.

Build-time deps (from `shell.nix`): cmake, ninja, gcc (C++23), pkg-config,
shaderc (`glslc` — hard requirement), git (submodules).

Runtime/link deps: vulkan-loader, vulkan-headers, glfw, glm, spdlog,
nlohmann_json, opencascade-occt (7.9 — via `find_package(OpenCASCADE)`), eigen,
boost (headers, PlaneGCS), netgen (`HAVE_NETGEN`, LGPL — dynamic link),
spectra (`HAVE_SPECTRA`), dbus + systemd (portal file dialogs), libx11,
catch2_3 (tests only).

Vendored (git submodules, built in-tree — NOT nixpkgs): imgui,
imgui_test_engine, nativefiledialog-extended, PlaneGCS. The flake must
`fetchSubmodules = true` (a fresh checkout has empty submodules — same gotcha
`worktree-build.sh` handles).

Tasks:

- [x] `flake.nix` with `packages.x86_64-linux.default` (+ `devShells.default`
      via `inputsFrom` mirroring `shell.nix` so `nix develop` == `nix-shell`).
      **Drafted & builds** (2026-07-23, ~50s compile on top of cached deps).
- [x] Package derivation: `nativeBuildInputs` vs `buildInputs` split; SPIR-V
      compiled at build time by `shaderc` (glslc), not shipped.
- [x] Submodules pinned as flake inputs (imgui / imgui_test_engine / NFD) —
      chose this over `?submodules=1` for reproducibility. planegcs + deps/fonts
      are in-tree (git-tracked). Keep input SHAs in sync with `.gitmodules`.
- [x] Build type: nixpkgs passes `-DCMAKE_BUILD_TYPE=Release` by default, which
      skips the CMake `Debug` fallback. The distributable package builds only
      `designer`; the separate `checks` derivation builds all test binaries.
- [x] `nix build .#default` → runnable binary. Verified:
      `result/bin/designer` is the real executable, PlaneGCS is under `lib/`,
      and 25 shaders plus fonts/HDRI live under `share/designer`. The packaged
      GUI boot smoke initializes IBL, shadows, SSAO, raster, outlines, and ray
      tracing on NVIDIA.
- [x] Standard install layout (2026-07-24): CMake `install()` owns the
      contract used by every packager — `bin/designer`, `lib/libplanegcs.so`,
      runtime data under `share/designer`, and desktop/AppStream/icon metadata
      under their standard `share/` locations. RuntimePaths prefers the staged
      build-tree data beside the executable, then resolves an installed
      `../share/designer`; `DESIGNER_DATA_DIR` is an explicit override. The
      flake now runs the normal CMake install phase instead of manually copying
      an `$out/opt/designer` tree and wrapper.

- [x] **Relocatability fixed** (2026-07-23). The studio HDRI was loaded via the
      `DESIGNER_SOURCE_DIR` compile define and the IBL cache written into the
      source tree — neither survives into a relocatable package. Now: shared
      `RuntimePaths` resolves HDRI/fonts/shaders from the development staging
      tree or the installed `share/designer` directory and writes the IBL cache to
      `$XDG_CACHE_HOME/designer` (else `~/.cache/designer`). The
      `DESIGNER_SOURCE_DIR` define is gone. Verified from both a temporary
      CMake prefix and `nix build`: fonts, icons, shaders, and HDR are present;
      `designer --version` reports 0.1.0; the installed/Nix GUI boot smoke loads
      the real IBL and all render pipelines on NVIDIA.
- [x] Nix `checks.x86_64-linux.headless` output (2026-07-24): builds
      `designer`, `designer_tests`, and `femgpu_tests`, then runs
      `ctest -LE gui` in a sandbox with a clean `$HOME`, Python, Git, and GPU
      topology disabled for determinism. Verified 538/538 passing; unavailable
      sandbox GPU cases skip normally. The installed Nix binary separately
      rebuilt its IBL cache and passed `boots_and_renders` on a real GPU.

---

## Licensing — compliance for open-source distribution

Findings against the current tree, for shipping binaries under an OSI
open-source license:

- **LGPL is *not* whole-project copyleft.** LGPL (OCCT, Netgen, PlaneGCS) does
  not force our own `src/` under LGPL. The obligations are: keep the LGPL libs'
  source (+ our modifications) available under LGPL, allow users to relink
  against a modified version, and give notice.
- **Netgen** — LGPL, dynamically linked, unmodified. ✅ Correct as-is.
- **OpenCASCADE** — LGPL-2.1 *with the OCCT exception* (more permissive than
  plain LGPL), dynamically linked. ✅ Inventoried with both license texts.
- **PlaneGCS — DONE.** Was vendored in `deps/planegcs`, **modified**, and
  **statically linked** — the static link triggered LGPL §6's relink duty.
  Now built as **`libplanegcs.so`** (`CMakeLists.txt`), so relink/replacement is
  automatic. CMake installs the `.so` under the standard library directory and
  gives the executable an `$ORIGIN`-relative install RUNPATH. No perf impact (boundary
  crossed once per solve; inner loops + header-only Eigen stay inlined in the
  .so). Still owed: publish our PlaneGCS modifications under LGPL (satisfied
  while the repo is public).
- **imgui_test_engine — fine as-is; leave it in.** Its license (LICENSE.txt
  v1.04) is free for, among others, *derivatives released publicly under an OSI
  license* — which we are, so we qualify. It is also **not test-only**: the
  remote/MCP `ui_action` GUI-driving mechanism is built on `ImGuiTestContext`
  (`RemoteServer.cpp` registers a persistent "remote/action" test; `main.cpp`
  creates the engine unconditionally), so it powers `ui_action` GUI automation
  at runtime, not just the `--gui-tests` suite. No action needed.
- Everything else (Eigen/Spectra MPL-2.0, Boost BSL, glfw/spdlog/json/glm/imgui/
  NFD MIT/zlib/Apache, fonts OFL/Apache) is permissive and inventoried.

Tasks:

- [x] Build PlaneGCS as a shared lib (`libplanegcs.so`) — done + packaged.
- [x] Distribution notices complete (2026-07-24): `THIRD_PARTY.md` inventories
      direct runtime components and linkage; `LICENSES/` contains their full
      texts, the OCCT exception, and corresponding-source/replacement notice.
      CMake installs the bundle with every binary artifact.
- [x] Project license chosen: **AGPL-3.0-or-later** (© Elias Högbom Aronsson),
      with a GPL/AGPL §7 additional permission to link Dear ImGui Test Engine.
      `LICENSE` (verbatim AGPL text), `ADDITIONAL_PERMISSION.md`, README License
      section, and `flake.nix` `meta.license = agpl3Plus` are all installed.

### Binary cache (do NOT skip)

Nix builds from source by default; nobody should rebuild OpenCASCADE + Netgen.

- [ ] Push artifacts to **Cachix** so `nix run github:…/designer` substitutes
      prebuilt binaries. Flake + Cachix together = the real "download & run"
      experience for Nix users.

### AppImage from the flake — HEADLESS ONLY (GUI path tested & rejected)

`nix bundle --bundler github:ralismark/nix-appimage .#default` closure-packs the
derivation into a self-contained AppImage (~620 MB). **Tested end-to-end
2026-07-25.** Verdict: usable **only for the headless artifact** (CLI / document
engine / `--remote` RPC). The **windowed GUI does not work from this AppImage on
any distro**, and the reason is fundamental, not a config bug.

What the test showed:

- **Headless (`--headless`, `--gui-tests` engine paths, RPC): ✅ works
  everywhere** — verified on the NixOS host and in a clean `ubuntu:24.04`
  container. `--version`, `new_project`, OCC load all succeed.
- **Windowed GUI on the NixOS build host: ❌** `VK_ERROR_INCOMPATIBLE_DRIVER`
  (`VkResult -9`) at `vkCreateInstance`. `nix-appimage`'s AppRun bind-mounts the
  bundle's `/nix/store` over the host's, which **shadows** the NixOS GPU driver
  (`/run/opengl-driver → /nix/store/…-graphics-drivers`). The loader finds
  `icd.d` but every ICD symlink dangles → no drivers. (The *same bundled binary*
  run directly, without AppRun's namespace, brings up full Vulkan incl. ray
  tracing on NVIDIA — so the build is correct; only the sandbox is at fault.)
- **Windowed GUI on clean Ubuntu 24.04: ❌** and this is the decisive one. ICD
  **discovery succeeds** (`/usr/share/vulkan/icd.d` is found), but every host
  driver `.so` fails to load: with the bundle's glibc in charge, the linker only
  searches the bundle's `/nix/store` and can't find `libvulkan_lvp.so`; forcing
  `LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu` then pulls in the host glibc 2.39,
  and the bundle's own libs (spdlog, netgen, planegcs, python…) require
  `GLIBC_ABI_GNU2_TLS` from the bundled **glibc 2.42** → everything collapses.

**Root cause (fundamental):** a Vulkan app and the host GPU driver must share
one glibc in one process. `nix bundle` ships its *own* (newer) glibc, so the
host driver can neither be found (bundle glibc in charge) nor loaded (host glibc
in charge breaks the app's newer libs). No `LD_LIBRARY_PATH` middle ground
exists. This is exactly the glibc↔driver contract Flatpak's runtime owns — see
Deliverable 2. It also **refutes the earlier "nearly-free AppImage covers most
of it" framing**: nearly-free, yes, but only for the *headless* role.

Alternatives considered and rejected as the GUI path:
- **Traditional (non-Nix) AppImage** built against an old glibc, not bundling
  glibc (the Blender/games recipe) — works for GPU, but requires rebuilding the
  entire OCC/Netgen/Python stack in a separate old-toolchain container. ~Flatpak
  effort, fewer benefits (no store, no updates, own the glibc floor forever).
- **`nix-gl-host`-wrapped bundle** (numtide) — injects the host driver the
  correct direction while keeping the Nix glibc; can run Nix Vulkan binaries on
  non-NixOS. But NVIDIA-centric, adds a fragile runtime probe, and still no
  store/updates/sandbox — reimplementing what Flatpak does for free.
- **Snap** — runtime-container like Flatpak but Ubuntu-centric, worse GPU
  sandbox story. Strictly dominated here.

- [ ] Wire `nix bundle` into CI as the **headless** release artifact (label it
      clearly: CLI / engine / RPC, no GUI). Falls out of the flake for ~free.
- [x] GUI-AppImage path tested and rejected (2026-07-25); Flatpak is the desktop
      release path. See Deliverable 2.

---

## Deliverable 2 — Flatpak — **BUILT + GPU-TESTED (2026-07-25)**

Manifest: `packaging/flatpak/io.github.eliasaronson.Designer.yaml`. Build/install
locally with `tools/flatpak-build.sh` — user-mode via `nix-shell`, so it needs
**no** `services.flatpak.enable` and no root. `flatpak-builder` builds in its own
sandbox against the Freedesktop 24.08 runtime (it does **not** consume the Nix
derivation); the flake told us every version/flag, which was ~80% of the effort.

**Result:** builds from source (OCC + Netgen + the math stack), installs, and —
the headline — **runs on the real NVIDIA RTX 4060 with hardware ray tracing**
(`--gui-tests --gui-test-filter boots_and_renders` → `1/1 passed`, GPU selected =
"NVIDIA GeForce RTX 4060", not lavapipe). This is the exact GPU story the
nix-bundle AppImage could not deliver — Flatpak's runtime owns the glibc↔driver
contract, and on this NixOS host the sandbox still reached the host NVIDIA
userspace. Headless engine + `--remote` RPC also verified reachable from the host
(needs `--share=network`; see finish-args).

Done:

- [x] `org.freedesktop.Platform`/`Sdk` **24.08**. No KDE/GNOME layer needed.
- [x] Modules from source: **OpenCASCADE `V7_9_3`** (Draw off — no `tk` in the
      SDK; Visualization **on** because we link `TKVCAF`→`TKV3d`; RapidJSON on
      for the `TKDEGLTF` reader we link; exceptions kept enabled), **Netgen
      `v6.2.2605`** (OCC on; GUI/Python/**MPI** off), plus **glm, glfw
      (X11-only), spdlog, nlohmann_json, eigen, spectra, rapidjson, boost**.
      `nativefiledialog-extended` + imgui + PlaneGCS are in-tree (built with the
      app module). The header-only cmake modules need `builddir: true`
      (Eigen refuses in-source builds).
- [x] **Boost** must be a real install (not a header copy): `find_package(Boost)`
      is QUIET-gated and silently disables the PlaneGCS sketch solver if absent,
      and CMake 3.30+ needs `BoostConfig.cmake` — so we `b2 install` the tiny
      `system` lib to emit the config + the header target.
- [x] **glslc / Vulkan headers / Mesa GL / freetype / dbus / x11 all ship in the
      SDK** — no module needed (verified by probing the SDK). gcc is **14.3**.
- [x] **`std::stacktrace` portability fix (CrashHandler):** the SDK's gcc ships
      **without `libstdc++exp`**, so CMake now probes for it and defines
      `HAVE_STDCXX_STACKTRACE` only when it links; otherwise CrashHandler falls
      back to glibc `backtrace()`. Improves portability beyond just Flatpak.
- [x] **`DESIGNER_BUILD_TESTS` option** (default ON) — the app module passes OFF
      so the offline sandbox doesn't `FetchContent` Catch2 during configure.
- [x] Finish-args: `--device=dri`, `--socket=fallback-x11` + `--socket=wayland`,
      `--filesystem=host`, `--share=network` (keeps the `--remote` interface
      reachable from host Python/MCP/agent clients).
- [x] App ID / `.desktop` / icon: added a **256×256 PNG** beside the scalable
      SVG (raster fallback). Installed by CMake into standard locations.

Remaining before Flathub submission:

- [ ] **Re-enable AppStream metainfo + the SVG icon.** Both are gated off for the
      *local* build only, because a bare `nix-shell` toolchain lacks gdk-pixbuf
      image loaders, so `appstreamcli compose` (metainfo) and
      `flatpak build-export`'s icon validator (SVG) both fail with file-read
      errors. Flathub's builders have the loaders. Fix: drop
      `-DDESIGNER_INSTALL_METAINFO=OFF` and the SVG-stripping `post-install` from
      the manifest (or set `GDK_PIXBUF_MODULE_FILE` in the build env to test the
      full metadata path locally). The metainfo itself passes `appstreamcli
      validate`.
- [ ] **Pin sources for reproducibility:** switch git `tag:` refs to
      `commit:` + `sha256` (Flathub requirement); currently tags for brevity.
- [ ] Confirm the renderer (SSAO, shadows, clip planes, instancing) on the
      runtime's **Mesa/llvmpipe** floor too (only NVIDIA verified so far).
- [ ] Tighten `--filesystem=host` toward portal-only where the FileDialogs allow.
- [ ] Distribution: Flathub submission (their build farm + auto-updates) or a
      self-hosted repo to start.

### The big de-risker — crib OCC + Netgen from FreeCAD

FreeCAD ships on Flathub (`org.freecad.FreeCAD`) on nearly the same stack —
OpenCASCADE + Netgen + an LGPL kernel. Its published manifest already builds
both from source. Lift its OCC and Netgen module definitions, strip the rest,
add our smaller deps + our app module. This turns the two biggest time sinks
from "port a CAD stack from scratch" into "adapt a proven recipe."

### Known risk areas

- **Renderer on Mesa is the real unknown.** SSAO / soft shadows / SSAO prepass /
  instancing / clip planes have only ever run on our (NVIDIA) dev rig. First
  contact with the runtime's Mesa (llvmpipe as the floor) could be clean or
  could eat a couple of days on a validation/format issue. This is the main
  estimate variance. Also verify with the NVIDIA userspace extension.
- OpenCASCADE build iteration is slow (~20–40 min from source), so the
  manifest fiddle loop drags until `flatpak-builder`'s cache warms.
- Portal file dialogs already fall back to the ImGui browser if no portal runs
  (see `FileDialogs`) — good, but test the portal path inside the sandbox.
- Netgen is LGPL — keep it a separate module, note it in `THIRD_PARTY.md`.

### Effort estimate (post-flake, prerequisites cleared)

~**1 to 1.5 weeks** to a Flatpak that installs and runs locally, *cribbing
OCC/Netgen from FreeCAD* — with the Mesa renderer verification being what pushes
it toward the high end. From scratch (no FreeCAD reference) add ~1 week. Rough
split: OCC ~1d, Netgen ~0.5d, small-dep modules ~1d, app module + finish args
~0.5d, Flathub metadata (AppStream/.desktop/icon) ~1d, GPU verification 0.5–3d.
A `nix bundle` AppImage is *not* a cheaper interim for the GUI — it was tested
and cannot run the windowed app on any distro (glibc↔driver; documented above).
It remains the near-free **headless** artifact only.

---

## Deliverable 3 — Windows (cross-compiled from Linux) — **BUILT + WINE-VERIFIED (2026-07-26)**

The whole Windows toolchain lives in `packaging/windows/` and runs **on Linux** via
nixpkgs cross (`pkgsCross.mingwW64`, gcc 15.2) — no Windows machine, no MSVC.

**Result (all verified from Linux):** Designer cross-compiles to a native
`designer.exe`, and under **Wine** it runs headless (OpenCASCADE engine + PlaneGCS
constraint solver both work — a radius constraint solves end-to-end), **renders on the
real NVIDIA RTX 4060 with hardware ray tracing** (winevulkan passes the RT extensions
through; `boots_and_renders` gui-test 1/1), and ships as an NSIS installer + portable
zip. That's a stronger position than the AppImage ever reached — and, unlike the AppImage
glibc wall, nothing here was an architectural blocker.

Build:
- App:  `nix-build packaging/windows/cross-app.nix`  (→ self-contained `bin/` + `share/`)
- Dist: `nix-build packaging/windows/installer.nix`  (→ portable `.zip` + NSIS `setup.exe`)
- Test: assemble `bin/`+`share/`, `DISPLAY=:0`, `wine designer.exe …` (use cached
  `wine64` for the 64-bit app; `pkgs.wine` (32-bit) for the 32-bit installer — see below).

What it took (the code was the easy part — the deps were the work):
- **Platform shims**, ~6 files, all `#ifdef`-guarded with the Linux path unchanged
  (native build + 538 tests + crash-test stay green): RuntimePaths→`GetModuleFileNameA`,
  CrashHandler→`SetUnhandledExceptionFilter`, RemoteServer→Winsock (+ `ws2_32`),
  GitVault→`nul`/`_pclose`, main.cpp→`std::this_thread::sleep_for`. Zero GNU compiler
  extensions in the tree kept MSVC-style churn near-nil; no fork/exec.
- **OCC cross needs derivation surgery** (`occ-cross.nix`): stock nixpkgs opencascade-occt
  drags tcl/tk→bash (unbuildable for Windows) — disable Draw + Visualization → 48 clean
  Windows DLLs.
- **Boost headers only** (`BoostConfig.cmake`) — Boost's own cross-build fails on its MPI
  component; a hand-written config exposes the `Boost::boost` interface target so PlaneGCS
  builds.
- Small nix-cross quirks: `config.allowUnsupportedSystem` for header libs (eigen/spectra);
  spdlog's packaged tests don't link (turned off); imgui_test_engine needs
  `mingw_w64_pthreads`.
- **MVP scope, deferred (not blockers):** Netgen (FEM meshing) and FEM-GPU (Vulkan
  compute) omitted to keep the build light.

Remaining before a *polished* Windows release:
- [ ] **Netgen** cross-build if FEM meshing is wanted on Windows.
- [ ] **Real-hardware QA + code signing** — needs a Windows box / cloud Windows; Wine
      proves function but isn't sign-off, and unsigned installers trip SmartScreen.
- [ ] Trim the bundle (strip DLLs), and confirm portal-less file dialogs on Windows (NFD
      uses the native Win32 backend — should be automatic).
- Note: the NSIS installer is 32-bit (standard) so it needs WoW64 Wine to test
  install→run in one prefix; verified in halves here (`pkgs.wine` runs the installer,
  `wine64` runs the app).

---

## Sequencing

1. `flake.nix` package + `devShells` → `nix build` green. **[done]**
2. Cachix cache wired into CI (the "download & run" path for Nix users).
3. `nix bundle` **headless** AppImage artifact (falls out of step 1; label it
   CLI/engine/RPC, no GUI — the GUI path is tested-rejected).
4. Flatpak manifest (OCC/Netgen from source) → local `flatpak run` green.
5. Renderer verification on Mesa + NVIDIA-extension.
6. Flathub metadata (.desktop/icon/AppStream) → submission.

Steps 1–3 are the cheap Nix-native wins and unblock reproducible CI releases
**for Nix users and headless consumers**. Steps 4–6 are the Flatpak investment
that delivers the desktop GUI to everyone else — no longer optional, since the
AppImage GUI path is closed.

---

## Deferred (documented, not planned)

- **Native deb/rpm/AUR** — permanent per-release tax across N distro versions,
  and Debian/Ubuntu ship OCC 7.6–7.7 (we need 7.9). Flatpak avoids both. Revisit
  only for official-repo discoverability or users who refuse Flatpak.
- **Snap** — Ubuntu-centric, single-vendor store, historically more GPU/dialog
  sandbox friction than Flatpak. Only if Snap Store presence becomes a goal.
- **Windows** — **DONE** (see Deliverable 3): cross-compiled from Linux, Wine-verified,
  installer built. Only real-hardware QA + signing remain.
- **macOS / MoltenVK** — the genuinely hard one, *because there is no Wine-for-Metal*:
  MoltenVK translates Vulkan→Metal, and Metal only exists on Apple's GPU stack, so unlike
  Windows (which we test end-to-end on Linux via Wine+winevulkan) the **GPU path cannot be
  exercised off a Mac at all**. How to develop it anyway, in order of how much Mac you need:
  1. **Port on Linux with `#ifdef` discipline** — the code is already portable (the same
     ~6-file shim surface as Windows; `<execinfo.h>` even works on macOS, so CrashHandler
     is nearly free there). NFD has a native Cocoa backend; OCC/Netgen build on macOS.
  2. **`osxcross` compile-lint on Linux/CI** — cross-compile to Mach-O to catch macOS
     build breaks fast, no Mac needed. Compile-only (can't run/Metal); the macOS SDK
     license restricts it to Apple hardware, so it's a dev aid, not a distribution path.
  3. **GitHub Actions macOS runners** — real Macs in CI: build + run headless/logic tests
     on every push (catches link/runtime/MoltenVK-init regressions). GPU/Metal on hosted
     runners is limited/headless, so this validates *everything except* real-GPU rendering.
  4. **Periodic real-Mac QA** — a cloud Mac (EC2 mac / MacStadium / MacinCloud) or a cheap
     Mac mini, used occasionally for the Metal render pass + notarization — the two things
     that truly require Apple hardware. Expect RT to be **unavailable** (MoltenVK doesn't
     bridge Vulkan ray tracing), so the offline ray-traced view is macOS's known feature
     gap (the renderer already gates RT and falls back to raster).

  Net: day-to-day macOS dev needs no Mac (Linux `#ifdef` + osxcross lint + macOS CI);
  reserve real/cloud Mac time for the GPU-render + notarization sign-off. It's more effort
  than Windows purely because the GPU can't be tested off-platform.
