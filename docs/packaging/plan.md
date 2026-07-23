# Packaging & distribution plan

Goal: ship Designer as installable Linux binaries. Two deliverables, in order:

1. **`flake.nix`** exposing `packages.x86_64-linux.default` — reproducible
   build + the source of truth for the dependency set. Serves Nix/NixOS users
   directly and becomes the reference the Flatpak manifest is written against.
2. **Flatpak** (Flathub-targetable) — the "runs on any distro" public release.

We are already all-in on Nix (`shell.nix`), so the flake is a low-effort lift
and the Flatpak reuses everything the flake pins.

Deliberately **out of scope for now**: native distro packages (deb/rpm/AUR),
Snap, macOS, Windows. See "Deferred" below for why.

---

## Why these two

- **Flatpak** wins for a GPU-heavy Vulkan app: its runtime ships Mesa and
  auto-pulls a matching proprietary NVIDIA userspace extension against the host
  driver — the one thing AppImage/tarball can't solve. It also uses
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
- [x] Submodules pinned as flake inputs (imgui / imgui_test_engine / nftd) —
      chose this over `?submodules=1` for reproducibility. planegcs + deps/fonts
      are in-tree (git-tracked). Keep input SHAs in sync with `.gitmodules`.
- [x] Build type: nixpkgs passes `-DCMAKE_BUILD_TYPE=Release` by default, which
      skips the CMake `Debug` fallback. Only the `designer` target is built
      (`ninjaFlags`); `designer_tests` deferred to a future `checks` output.
- [x] `nix build .#default` → runnable binary. Verified: `result/bin/designer`
      is a wrapper; real binary + `libraries/shaders/` (25 .spv) + `assets/`
      live in `$out/opt/designer`. Ran `--headless --remote` OK (remote server
      up, accepts connections). GPU render path not verified here (no GPU + do
      not launch the human's windowed instance).
- [x] Runtime layout: exe-relative lookup (readlink `/proc/self/exe`) is
      reproduced under `$out/opt/designer`; fonts + HDRI copied from source.

- [x] **Relocatability fixed** (2026-07-23). The studio HDRI was loaded via the
      `DESIGNER_SOURCE_DIR` compile define and the IBL cache written into the
      source tree — neither survives into a relocatable package. Now: `main.cpp`
      resolves the HDRI relative to the executable (`ExecutableDir()`, same
      scheme as shaders/fonts) and writes the IBL cache to
      `$XDG_CACHE_HOME/designer` (else `~/.cache/designer`). CMake stages
      `assets/hdri` next to the binary; the `DESIGNER_SOURCE_DIR` define is
      gone. The flake no longer needs the `substituteInPlace` hack and copies
      the built `assets/` wholesale. Verified: fonts (Roboto + fa-solid) + HDR
      present in the store output, binary runs headless. **Still to verify on a
      GPU:** that IBL now renders for real (not fallback) — check on the human's
      dev-watch instance, not here.
- [ ] `checks` output for `designer_tests`; verify from a clean `$HOME`.

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
  plain LGPL), dynamically linked. ✅ Fine. (Add it to `THIRD_PARTY.md`.)
- **PlaneGCS — DONE.** Was vendored in `deps/planegcs`, **modified**, and
  **statically linked** — the static link triggered LGPL §6's relink duty.
  Now built as **`libplanegcs.so`** (`CMakeLists.txt`), so relink/replacement is
  automatic. The flake installs the `.so` next to the binary with an `$ORIGIN`
  RUNPATH (`-DCMAKE_BUILD_RPATH_USE_ORIGIN=ON`). No perf impact (boundary
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
  nftd MIT/zlib/Apache, fonts OFL/Apache) is permissive.

Tasks:
- [x] Build PlaneGCS as a shared lib (`libplanegcs.so`) — done + packaged.
- [ ] Add OpenCASCADE to `THIRD_PARTY.md`; ship a `LICENSES/` dir with full
      license texts + written offer for LGPL source in release artifacts.
- [ ] Pick and declare the project's own license (`flake.nix` meta is unset).

### Binary cache (do NOT skip)

Nix builds from source by default; nobody should rebuild OpenCASCADE + Netgen.
- [ ] Push artifacts to **Cachix** so `nix run github:…/designer` substitutes
      prebuilt binaries. Flake + Cachix together = the real "download & run"
      experience for Nix users.

### Nearly-free bonus — AppImage from the flake

`nix bundle --bundler github:ralismark/nix-appimage .#default` closure-packs the
derivation into a self-contained AppImage. Relies on the host Vulkan ICD (no
NVIDIA-userspace matching like Flatpak). Cheap "hand someone a file" artifact.
- [ ] Wire `nix bundle` into CI as an optional release artifact once the flake
      builds.

---

## Deliverable 2 — Flatpak

`flatpak-builder` builds in its own sandbox against the Freedesktop runtime; it
will **not** consume the Nix derivation. So the manifest is separate work — but
the flake tells us every version/flag, which is ~80% of the effort.

Plan:
- [ ] `org.freedesktop.Platform`/`Sdk` runtime (pick current stable, e.g.
      24.08). KDE/GNOME layer only if we need their libs — likely not.
- [ ] Manifest modules built from source inside the sandbox: **OpenCASCADE 7.9**
      (the big one), **Netgen**, **Spectra**, **nativefiledialog-extended**,
      then Designer itself as the final module. Runtime provides glibc/X11/
      Wayland/Mesa/D-Bus.
- [ ] Finish/permissions: `--device=dri` (GPU), `--socket=wayland` +
      `--socket=fallback-x11`, filesystem access for opening/saving projects
      (portal-based where possible — aligns with existing `FileDialogs`).
- [ ] Bake SPIR-V shaders + bundle `assets/`; verify asset lookup under the
      sandbox's read-only `/app` layout.
- [ ] Confirm the renderer (SSAO, shadows, clip planes, instancing) works under
      the runtime's Mesa and, separately, with the NVIDIA extension.
- [ ] App ID `org.<domain>.Designer`, `.desktop` file, icon, AppStream
      metainfo (Flathub requires these).
- [ ] Distribution: Flathub submission (their build farm + auto-updates) or a
      self-hosted Flatpak repo to start.

### Known risk areas
- OpenCASCADE 7.9 is new — building it in the Flatpak sandbox is the single
  biggest time sink. Reuse the exact configure flags nixpkgs uses.
- Netgen is LGPL — dynamic-link, keep it a separate module, note it in
  `THIRD_PARTY.md`.
- Portal file dialogs already fall back to the ImGui browser if no portal runs
  (see `FileDialogs`) — good, but test the portal path inside the sandbox.
- Push-constant / shader assumptions are host-GPU-independent, but re-verify on
  Mesa (llvmpipe as a floor) since our dev rig is likely NVIDIA.

---

## Sequencing

1. `flake.nix` package + `devShells` → `nix build` green.
2. Cachix cache wired into CI.
3. `nix bundle` AppImage artifact (falls out of step 1).
4. Flatpak manifest (OCC/Netgen from source) → local `flatpak run` green.
5. Renderer verification on Mesa + NVIDIA-extension.
6. Flathub metadata (.desktop/icon/AppStream) → submission.

Steps 1–3 are the cheap Nix-native wins and unblock reproducible CI releases.
Steps 4–6 are the deliberate public-release investment.

---

## Deferred (documented, not planned)

- **Native deb/rpm/AUR** — permanent per-release tax across N distro versions,
  and Debian/Ubuntu ship OCC 7.6–7.7 (we need 7.9). Flatpak avoids both. Revisit
  only for official-repo discoverability or users who refuse Flatpak.
- **Snap** — Ubuntu-centric, single-vendor store, historically more GPU/dialog
  sandbox friction than Flatpak. Only if Snap Store presence becomes a goal.
- **macOS / Windows** — separate ports (MoltenVK; MSVC/vcpkg for OCC+Netgen;
  `#ifdef` out the libx11/dbus/systemd Linux integration). Real projects, not
  "also build." Measure the Linux-specific-assumption surface first.
