# The first public release

## The problem

Every channel builds, and none reaches a user. There is no tag, no workflow
that turns a tag into downloadable artifacts, no binary cache, and nothing on
PyPI ([packaging](../packaging/README.md#not-done)). Three things make it
worse than "not published yet":

- **The packages launch a binary that no longer exists.** The rename made the
  executable `lobole` (`OUTPUT_NAME` in CMakeLists.txt), but the Flatpak
  manifest says `command: designer`, the container image's `Entrypoint` is
  `${designer}/bin/designer`, and the NSIS installer's shortcut points at
  `bin\designer.exe`. Nothing builds or starts any of them in CI, so nothing
  noticed.
- **The public surface still says `designer`.** Environment variables a user
  sets (`DESIGNER_PORT`, `DESIGNER_AUTOSTART`, `DESIGNER_WATCHDOG_S`,
  `DESIGNER_DATA_DIR`, …), CMake options a packager sets
  (`DESIGNER_BUILD_TESTS`, `DESIGNER_INSTALL_METAINFO`,
  `DESIGNER_ENABLE_FEM_GPU`), the flake's package names, the image name
  `designer-headless`, the installer's file names and its `APPNAME`. Each is
  free to rename before the first release and a breaking change after it.
- **`nix run` compiles OpenCASCADE and Netgen.** The OCCT shell patch
  (`nix/occt-overlay.nix`) and the lean Netgen are custom derivations, so
  neither is in cache.nixos.org; a first `nix run` takes tens of minutes.

The version is also written in four places that nothing holds together:
`project(VERSION)` in CMakeLists.txt, the flake, the AppStream metainfo's
`<release>`, `pyproject.toml`, and `packaging/windows/installer.nix`.

## Out of scope

Flathub (review is slow and needs pinned sources, metainfo, a Mesa run and a
narrower filesystem grant — it follows the release), Windows on real hardware
and code signing (needs a machine and a certificate; the first Windows
download is labelled untested and unsigned), macOS, distro packages.

## Before anything here is built: the user's decisions

The public repository's name and history must be settled first, because
Cachix, PyPI and GHCR all key on them:

- the FS-chassis team CAD in the git history — keep, or rewrite it out;
- the GitHub repository renamed to `lobole`;
- CLA or DCO (CONTRIBUTING.md states it);
- the stray PNGs at the repository root.

Steps 1-3 below do not depend on these and can land first.

## 1. Every package launches `lobole`

- `packaging/flatpak/org.lobole.Lobole.yaml`: `command: lobole`, module
  name `lobole`.
- `flake.nix`: the derivation becomes `lobole` (`packages.default`,
  `packages.lobole`), `checks.headless` keeps its name, the image is
  `lobole-headless` with `Entrypoint = [ "${lobole}/bin/lobole" ]`.
- `packaging/windows/installer.{nix,nsi}`: `APPNAME "Loböle"` (NSIS takes
  UTF-8 with `Unicode true`), `lobole-<ver>-win64-{portable.zip,setup.exe}`,
  the shortcut to `bin\lobole.exe`.
- `.github/workflows/docker.yml`: push `lobole-headless`.

**Verified by starting each one**: `nix run . -- --headless --remote PORT`
answering `ping`; the image under `docker run` answering `ping`; the Flatpak
via `tools/flatpak-build.sh --run`; the Windows zip's `lobole.exe --headless`
under Wine answering `ping`. Step 4's workflow keeps the first three true.

## 2. No `DESIGNER_` name survives

Rename every `DESIGNER_*` environment variable, CMake option and compile
definition to `LOBOLE_*`, in one mechanical pass over src/, tests/, tools/,
python/, packaging/, CMakeLists.txt, flake.nix, shell.nix, .github/ and
docs/. Internal test variables go too: two prefixes is the confusion the
rename exists to end, and a rule "user-facing ones only" is a rule someone
has to judge per variable. The CMake target `designer` and `designer_tests`
stay — they are not shipped names, and renaming them touches every script and
doc that says `ninja designer` for no user's benefit.

`tests/test_conventions.cpp` gains a scan that fails on `DESIGNER_` anywhere
in the tree (tracked files, outside git history and `deps/`), so a new one
cannot arrive.

**Verified by** the full headless suite, the GUI suite (both read several of
these variables) and `tools/ci-local.sh` (CI reads them in ci.yml).

## 3. One version

`project(VERSION)` in CMakeLists.txt is the source:

- the flake reads it (`builtins.match` over CMakeLists.txt) instead of
  restating it;
- `pyproject.toml` takes it with a dynamic version read from a file CMake
  does not need — simplest is a top-level `VERSION` file that both CMake
  (`file(READ)`) and pyproject (`[tool.setuptools.dynamic]`) read, and the
  flake and installer.nix read with `builtins.readFile`;
- the metainfo's newest `<release version>` must equal it.

A `[conventions]` test checks the metainfo against `VERSION`, and that no
other file spells a version literal for the app.

## 4. A tag builds the release

`.github/workflows/release.yml`, on `push: tags: ["v*"]`:

1. Refuse unless the tag equals `v$(cat VERSION)` and the metainfo has that
   release — a tag that disagrees with what it builds is refused, not shipped.
2. In parallel, each from the flake or its nix file, each started before it
   is attached:
   - the headless AppImage (`nix bundle --bundler
     github:ralismark/nix-appimage .#default`), smoke: `--headless --remote`
     answers `ping`;
   - the Windows zip and installer (`nix-build
     packaging/windows/installer.nix`), smoke: `lobole.exe --headless`
     under Wine answers `ping` (Wine is already how the crash path was
     checked);
   - the wheel and sdist (`python -m build`), smoke: `packaging_smoke`'s
     import check against the built wheel;
   - the container image — `docker.yml`'s job moves in here, so one tag
     produces one set.
3. A GitHub release for the tag with the AppImage, both Windows files, the
   wheel and sdist and a SHA256SUMS file, its notes taken from the metainfo
   `<release>`'s description. Windows is marked untested on hardware and
   unsigned in those notes.
4. Publish the wheel to PyPI through trusted publishing (an OIDC
   `pypa/gh-action-pypi-publish` step; no token stored), and push the image to
   GHCR.

Publishing (steps 3-4) runs only when every build and smoke passed. The same
workflow runs on `workflow_dispatch` with publishing switched off, which is
how it is tested before the first tag.

## 5. Cachix

A public `lobole` cache. `ci.yml`'s nix job and `release.yml` push to it
(`cachix/cachix-action`, auth token as a repository secret) — only the custom
derivations and the app need pushing, well inside the free tier.
`flake.nix` gets `nixConfig.extra-substituters` / `extra-trusted-public-keys`
so `nix run github:<owner>/lobole` offers to use it, and the packaging
README's flake section says how to accept it.

**Verified by** `nix run github:<owner>/lobole -- --version` on a machine
that has never built it, downloading rather than compiling.

## 6. Docs

- `docs/packaging/README.md`: the release workflow, Cachix, PyPI, the
  renamed packages; "Not done" keeps Flathub, Windows hardware/signing,
  macOS and distro packages.
- The top-level README's install section: `nix run`, the AppImage
  (headless), Windows (untested, unsigned), `uv tool install lobole` /
  `pip install lobole`, the container image.
- Every doc that names a `DESIGNER_` variable or option (step 2's scan
  covers docs too).

## Files

`packaging/flatpak/org.lobole.Lobole.yaml`, `packaging/org.lobole.Lobole.metainfo.xml`,
`packaging/windows/installer.{nix,nsi}`, `flake.nix`, `shell.nix`,
`CMakeLists.txt`, a new `VERSION`, `pyproject.toml`,
`.github/workflows/{ci,docker,release}.yml`, `tests/test_conventions.cpp`,
every file with a `DESIGNER_` name, `docs/packaging/README.md`, `README.md`.

## Order

1 → 2 → 3 land on main now, each through the full headless suite (step 2
also the GUI suite). 4 is built and dry-run with `workflow_dispatch` while
the repository is still private. After the user's decisions and the
repository going public: 5, then the first tag.
