# macOS: what the first CI runs have to show

The macOS port, its CI job (`ci.yml`, `macos`) and its release job
(`release.yml`, `macos`) were written on Linux, where none of them can run
([macOS](../packaging/README.md#macos)). The contributor built and ran the app
on an M4 Pro; nothing has run on a GitHub runner. This plan is what a first
run must prove, and what to change where it does not.

## The problem

Each item below is an assumption in a workflow or script that only a Mac
can test, most doubtful first.

1. **MoltenVK on the runner's virtual GPU.** Whether a `macos-26` runner
   exposes a Metal device to MoltenVK at all. The `Vulkan device` step of the
   CI job prints what it found. With none, every GPU step says so and the
   headless suite is still the gate.
2. **The headless suite passes there.** The contributor ran it on an M4 Pro,
   not on a three-core virtual machine, and 162 commits of main had landed
   since, none of them built for a Mac. A deadline test failing only there is
   a cost check reading a slower machine
   ([tests that time things](../ui/testing.md#tests-that-time-things)), not a
   Mac bug, until shown otherwise.
3. **`packaging/macos/deps.sh` on a clean runner.** It installs `cmake`,
   `libtool` and `python` now (missing from the first version, present on the
   machine it was written on), builds libvterm's Makefile with `glibtool`, and
   builds glslc from shaderc. The source half is cached; a change to either
   deps script, the OCC patches, the macOS major version or Xcode rebuilds it
   (about an hour on the runner, never measured).
4. **`packaging/macos/bundle.py`.** Every `@rpath` reference resolves through
   the image's own rpaths or the `--search` directories (Netgen installs under
   `$LOBOLE_MACOS_DEPS/Contents/MacOS`); no two libraries share a file name;
   `install_name_tool` has the header room to add an rpath; `otool` output is
   thin (a universal library would need `-arch arm64`).
5. **The Vulkan loader reads the bundle's `Resources/vulkan/icd.d`.** With no
   `VK_DRIVER_FILES`, the bundled app must find the bundled MoltenVK. The
   release job's window step tests it where the runner has a device.
6. **`ctest --tests-from-file`** needs CMake 3.29; the runner's CMake is
   printed in `The runner`.
7. **Signing.** The ad-hoc path runs on every release; the Developer ID and
   notarisation paths run only once their secrets exist, and
   `notarytool` rejects any library signed without the hardened runtime or a
   timestamp.

## The shape of the fix

- No device on the runner: keep the GPU steps conditional and the headless
  suite as the gate. The GPU paths are then tested only on a contributor's
  Mac, until a runner with a Metal device (a self-hosted one) is added.
- A device: commit the render the CI job uploads as
  `tests/golden/render/scene__<key>.png`, then drop `continue-on-error` from
  the GPU steps of both jobs.
- A test that fails only on the runner: fix the code, or give the test a
  Mac expectation under `#if defined(__APPLE__)` with its reason
  ([macOS](../packaging/README.md#macos)); never an exclusion in `ci.yml`
  without one.
- `bundle.py` or the loader failing: the failure names the file and the
  reference; fix the script, not the check.

## Files

`.github/workflows/ci.yml`, `.github/workflows/release.yml`,
`packaging/macos/{deps.sh,bundle.py,sign.sh,smoke.sh}`, `tools/macos-env.sh`,
`tests/golden/render/`.

## Verified by

A `workflow_dispatch` run of `ci.yml` and of `release.yml` (which publishes
nothing), each green, the CI job's log naming a Vulkan device or stating that
there is none, and the release job's `.dmg` opened on a Mac that has never had
Homebrew.
