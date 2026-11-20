# Start on software rendering when the GPU will not

A start that cannot get a hardware renderer should open its window on the
host's own software Vulkan (Mesa's lavapipe) when that is installed, and
say so for as long as it lasts. Where lavapipe is absent too, today's
report stays as it is: it already names the packages.

## The problem

The device ladder (docs/rendering/README.md#the-device-ladder) already
falls back to a software device in process, announces it and reports it
in `ping`. It only works when the failure is one a rung can catch and
lavapipe is among the devices the loader lists. Four cases still end the
start, or the process, on a machine where lavapipe would have drawn:

- **The failure is above the ladder.** `CreateInstance`
  (src/app/GpuOwners.cpp) throws at "create the Vulkan instance" when a
  broken implicit layer or driver manifest fails `vkCreateInstance`, and
  `BuildRows` throws "find a graphics device" when the loader lists
  nothing. A stale `VK_ICD_FILENAMES` or `VK_DRIVER_FILES` in the
  environment does both while a working lavapipe manifest sits in
  `/usr/share/vulkan/icd.d`.
- **The failure is below it.** The rows after the rung rows (the mesh
  renderer) are made once, on the device the ladder landed on. A hardware
  device that creates and presents but cannot build the mesh pipeline
  ends the start; the software rung is never tried.
- **The driver crashes.** A segfault inside the loader or a driver during
  start is a crash, not a `StartupError`. Nothing remembers which device
  it was, so the next start does the same; two of them reach safe mode
  (docs/ui/settings.md#safe-mode), which changes settings and not the
  device.
- **There is no loader.** The app links the Vulkan loader
  (`Vulkan::Vulkan`, CMakeLists.txt) and the GUI AppImage leaves
  `libvulkan.so.1` to the host (packaging/appimage/build.sh), so on a host
  with no Vulkan packages the dynamic linker ends it before `main`: no
  report, no exit code 3, no box, and `--headless` does not start either.
  The AppImage smoke does not see it, because every image it runs in
  installs the loader first (packaging/appimage/smoke.sh).

## The shape of the fix

### 1. Finding lavapipe

`startup::FindSoftwareDriver()` (src/app/StartupReport.cpp, Linux only)
returns the path of a driver manifest whose `library_path` names
`libvulkan_lvp`, searching where the loader does:
`$XDG_CONFIG_HOME/vulkan/icd.d`, `/etc/vulkan/icd.d`,
`$XDG_DATA_HOME/vulkan/icd.d`, each of `$XDG_DATA_DIRS` (default
`/usr/local/share:/usr/share`) and NixOS's
`/run/opengl-driver/share/vulkan/icd.d`. It reads files and loads
nothing. `LOBOLE_SOFTWARE_DRIVER` overrides it: a manifest path, or `none`
for no automatic fallback; anything else is refused at start (exit 2),
as `LOBOLE_RENDERER` is. Nothing is bundled.

### 2. The restart

`EndStartup` (src/app/main.cpp) restarts instead of reporting when all of
these hold:

- the failed step is one of "create the Vulkan instance", "find a
  graphics device", "create the graphics device", "open the window
  surface", "set up the swapchain", "initialise the mesh renderer" (not
  the windowing system, the window or the loader, which software
  rendering does not cure);
- this process is not itself the restart (no `--software-fallback`);
- `LOBOLE_RENDERER` is unset: `hardware` is the user saying no, and
  `software` has already been tried;
- the failure was not on a software device reached through a clean
  loader (this start's own attempts list it), and
  `FindSoftwareDriver()` finds a manifest.

It writes `<state>/renderer-fallback.json` (the failure as
`ReportStartupFailure` would have worded it: step, result, devices,
attempts; the manifest; the build), ends the session as a clean exit does,
and calls `startup::Relaunch` with this start's arguments plus
`--software-fallback`, in an environment where `VK_DRIVER_FILES` and
`VK_ICD_FILENAMES` are that one manifest, `VK_ADD_DRIVER_FILES` is unset
and `VK_LOADER_LAYERS_DISABLE=~implicit~`: a loader that sees lavapipe
and no layer that could have been the cause. `execv` keeps the pid, so a
launcher watching it keeps watching. Both the window and `--offscreen`
do this.

`--software-fallback` is the restart's own flag, like
`--after-device-loss`: it keeps only the software rungs, and it is the
loop guard. A `StartupError` in that process goes to the report, which
then leads with the first failure and says software rendering was tried
and how it failed. The app's own later restarts (safe mode's buttons, a
lost device) drop the flag, so each fresh start tries the hardware
first and a repaired driver is picked up with nothing to reset.

### 3. A driver that crashed at start

Before the instance is created, and again before each rung, the start
writes `<state>/graphics-attempt.json`: the stage (`instance`, or the
device's name and driver version) and the build. It is removed at the
first frame (`crash::NoteFirstFrame`), on a `StartupError`, and on a
clean exit. A start that finds one left behind by a dead process knows
where the last one died:

- at `instance`: it starts as the restart above would, on the software
  driver, if there is one;
- on a device: that device is left off this start's ladder
  (`startup::DeviceLadder` gains a skip list), and recorded in
  `<state>/graphics.json` under its name, driver version and this build.

A skipped device stays skipped until its driver version or the build
changes, or the user picks "Try this device again" in Preferences, which
clears the entry and rebuilds the device. Neither file is read under
`--headless`. Safe mode counts early deaths as it does now.

### 4. Saying so

A start that came through the restart, or that skipped a device, is a
fallback in every place the ladder's own fallback is announced:

- `renderer.fallback` true with `reason` the recorded failure, `forced`
  empty (it was not asked for), and two new keys in `ping`'s `renderer`:
  `restarted` (this process is the software restart) and `skipped`
  (`[{device, driver, why}]`). `crash_reports` answers the recorded
  failure as it answers `last_startup_failure`.
- The modal at the first frame and the lasting menu-bar notice
  (`###renderer_software_notice`, src/ui/MainMenuBar.cpp) gain what to do
  about it, chosen by the failed step from the table `Tips`
  (src/app/StartupReport.cpp) already holds: the driver and loader
  packages by distribution, 3D acceleration in a virtual machine, the
  environment variable or layer that was in the way when one was.
  Clicking the notice opens that text again; Help ▸ About carries the
  same rows. It shows on every start while the condition lasts, and
  `LOBOLE_SOFTWARE_DRIVER=none` is named in it for a user who would
  rather have the report.

### 5. The loader in the AppImage

The AppImage carries the Khronos loader (Apache-2.0, a few hundred
kilobytes) in `usr/lib/runtime-vulkan`, outside every rpath, and
`packaging/appimage/AppRun` puts that directory on the library path only
when the host has no `libvulkan.so.1`, as it already does for
`libstdc++`. The host's loader is used wherever there is one. With the
bundled one and no driver the app reaches `main`, `--headless` works, and
a windowed start ends at exit 3 with the report and its `apt` line.

## Files

- `src/app/StartupReport.{h,cpp}` (`FindSoftwareDriver`, the attempt and
  skip files, `DeviceLadder`'s skip list, the texts), `src/app/main.cpp`
  (`EndStartup`, `--software-fallback`, `LOBOLE_SOFTWARE_DRIVER`, the
  relaunch argument filter, `--help`), `src/app/GpuOwners.cpp` (writing
  the attempt, the status), `src/ui/AppState.h` (`RendererStatus`),
  `src/ui/MainMenuBar.cpp`, the About and Preferences windows,
  `src/remote/RemoteDispatch.cpp` (`ping`, `crash_reports` rows).
- `packaging/appimage/build.sh`, `AppRun`, `smoke.sh`;
  `.github/workflows/release.yml` (the smoke's second lane).
- `tests/startup_failure_smoke.py`, `tests/test_startup_report.cpp`.
- `docs/rendering/README.md#the-device-ladder`,
  `docs/architecture.md#when-the-renderer-cannot-start`,
  `docs/packaging/README.md` (*When it will not start*, *The GUI
  AppImage*), `docs/ui/settings.md` (the Preferences row).

## Verified by

- `startup_failure_smoke.py`, on lavapipe:
  - `VK_ICD_FILENAMES=/nonexistent` with `LOBOLE_SOFTWARE_DRIVER` naming
    the host's manifest: the app answers `ping` with `renderer.software`,
    `fallback` and `restarted` true, `forced` empty and the instance
    failure in `reason`; the pid is the launched one; no
    `last-startup-failure.txt`, no crash report.
  - the same with `LOBOLE_SOFTWARE_DRIVER=none`: exit 3 and the report,
    as today's first case. With `LOBOLE_RENDERER=hardware`: exit 3.
  - `LOBOLE_FAULT=renderer_init:mesh`, which the fault now applies to
    the first start only: restarted, and `reason` names the mesh
    renderer. A new `LOBOLE_FAULT=software_restart` fails the restart
    too: exit 3, one report naming both failures, and exactly two
    process starts in the session log (no loop).
  - a planted `graphics-attempt.json` naming the first device, on a
    two-device run: that device is in `renderer.skipped` and not used;
    after "try again" (`set_settings`) it is used. One naming `instance`:
    the start is the software restart.
  - `--crash-test` with the attempt file present leaves it; a start that
    reaches its first frame removes it.
- A unit test of `FindSoftwareDriver` over a scratch tree of manifests
  (none, lavapipe among others, a malformed file, the override), and of
  `DeviceLadder` with a skip list.
- `packaging/appimage/smoke.sh`, in two kinds of container:
  - **Mesa and no GPU** (the images it runs in now, with
    `mesa-vulkan-drivers`): today's checks, and then the app started with
    `VK_ICD_FILENAMES=/nonexistent` on Xvfb must answer `ping` as the
    first case above and pass `app/boots_and_renders`. The no-renderer
    check that follows sets `LOBOLE_SOFTWARE_DRIVER=none`.
  - **No Vulkan at all** (a second lane that installs X11, fontconfig,
    D-Bus and python, and neither `libvulkan1` nor Mesa's drivers):
    `tools/release-smoke.py` passes over `--headless`; a windowed start
    is exit 3 with `last-startup-failure.txt` naming the driver packages,
    with and without a way to show a box. The lane fails if a loader or
    a driver manifest is present in the image, so it cannot pass by
    having quietly become the first lane.
- The GUI suite, which runs on lavapipe as the only device: no modal, no
  restart, `renderer.fallback` false, as now.
