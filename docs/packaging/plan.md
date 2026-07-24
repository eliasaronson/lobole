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
      + **Netgen** (crib both from FreeCAD's manifest — see de-risker below),
      **Spectra** (header-only, trivial), **nativefiledialog-extended**, and the
      **small deps the Freedesktop SDK does not ship** — glfw, spdlog, eigen,
      boost, nlohmann_json, glm (mostly header-only / tiny CMake modules).
      Then Designer itself as the final module. Runtime provides
      glibc/X11/Wayland/Mesa/D-Bus; confirm each small dep is actually
      SDK-provided vs. needs-a-module before writing them all.
- [ ] Ensure **glslc is in the sandbox** (shaderc module or SDK extension) — it
      is a hard build-time requirement; shaders are compiled to .spv at build.
- [ ] Finish/permissions: `--device=dri` (GPU), `--socket=wayland` +
      `--socket=fallback-x11`, filesystem access for opening/saving projects
      (portal-based where possible — aligns with existing `FileDialogs`).
- [ ] Bundle `assets/` next to the binary; **relocatability prerequisite is
      already cleared** (assets load relative to the exe, IBL cache writes to
      `$XDG_CACHE_HOME` — done on the flake branch), so the read-only `/app`
      layout is no longer a blocker. Just verify it in the sandbox.
- [ ] Confirm the renderer (SSAO, shadows, clip planes, instancing) works under
      the runtime's Mesa and, separately, with the NVIDIA extension.
- [x] App ID `io.github.eliasaronson.Designer`, `.desktop` file, scalable SVG
      icon, and AppStream metainfo. Both metadata files pass
      `desktop-file-validate` / `appstreamcli validate --no-net`. Installed by
      CMake into the standard locations; Flatpak manifest still to be written.
- [ ] Distribution: Flathub submission (their build farm + auto-updates) or a
      self-hosted Flatpak repo to start.

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
Cheaper interim if a runnable binary is wanted sooner: `nix bundle` → AppImage
off the flake is ~hours (weaker driver story; documented above).

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
