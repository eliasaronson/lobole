# Startup and GPU hardening

## The problem

Loböle ships as an AppImage, a deb, a Flatpak and a Windows zip and
installer, to machines nobody here has seen. The graphics path stops the
process the moment one of them is not like the developer's, and almost
always does it where nobody can see why.

- **`check_vk_result` (`src/app/main.cpp`) aborts on any negative
  `VkResult`**: a failed `vkCreateInstance` (`VK_ERROR_INCOMPATIBLE_DRIVER`
  when no ICD loads), `vkCreateDevice`, the window surface, a swapchain
  recreate inside `ImGui_ImplVulkanH_CreateOrResizeWindow` (ImGui is handed
  the same function as `CheckVkResultFn`), and in the frame loop every
  acquire, fence wait, submit and present. `VK_ERROR_DEVICE_LOST` (a Windows
  TDR, a driver reset, an eGPU unplugged) and `VK_ERROR_SURFACE_LOST_KHR`
  are aborts too. The only message is `[vulkan] Error: VkResult = -4`,
  a number, on stderr.
- **No physical device** leaves `g_PhysicalDevice` null behind an
  `IM_ASSERT` that a Release build compiles out, so the next
  `vkGetPhysicalDeviceProperties` segfaults. `SetupVulkanWindow` has an
  `exit(-1)` ("no WSI support on physical device 0"). `glfwInit` failing
  returns 1 with the reason only in GLFW's stderr callback, and a null
  `glfwCreateWindow` is never checked: the run carries on into Vulkan with
  no window.
- **Every one of these is invisible from a desktop launch.** An AppImage,
  deb or Flatpak started from a menu has no terminal; a Windows shortcut's
  console, if any, closes with the process. The user sees a window flash, or
  nothing. The next start's crash notice ("Loböle closed unexpectedly last
  time", `crash::SetNotice`) says SIGABRT or SIGSEGV with a raw stack: a
  report for us, not a reason for them, and never what to try.
- **The renderers' one-shot submits ignore their results**
  (`render/VulkanCommon.cpp`, `Screenshot.cpp`: `vkQueueSubmit` then
  `vkQueueWaitIdle`, unchecked), so a loss there passes silently and the
  frame loop aborts later.
- **A renderer that fails to initialise is logged and the app runs on**
  (Shadow, SSAO, Mesh, Outline, Sketch, …), with nothing on screen
  saying a part of the picture is missing.
- **A crash loop has no way out but hand-editing.** `settings.json` that
  parses but drives a crash (a `gpuDevice` whose driver dies, a font scale)
  or an `imgui.ini` that trips a docking assert crashes every start. An
  unreadable `settings.json` is renamed to `settings.json.bak`, replacing
  the previous `.bak`, with a log line the user never reads.
- **The watchdog and the crash flow see the symptoms, not the cause.** A GPU
  that hangs without a reset parks the main thread in
  `vkWaitForFences(…, UINT64_MAX)`; the frame watchdog reports a stall in
  `FrameRender` with no label saying it is the GPU. A device loss becomes
  SIGABRT, the session marker survives, and the next start files it as a
  crash, with the unsaved work recoverable only if the last periodic
  autosave happened to be recent.

## The shape of the fix

1. **A startup failure is a value, not an abort.** Startup Vulkan calls go
   through `CheckStartup(step, VkResult)`, which throws a `StartupError`
   carrying the step ("create the Vulkan instance", "open the window
   surface"), the result's name (`string_VkResult`, never the number alone)
   and the devices seen. `main` catches it and ends through one
   `ReportStartupFailure` with a distinct exit code (3: no usable renderer).
   The no-device, no-WSI, `glfwInit` and null-window cases raise the same
   error. The GLFW error callback keeps its last message so the report can
   quote it.
2. **The user sees it.** `ReportStartupFailure` writes, every time, the
   message to stderr, the session log and `<state>/last-startup-failure.txt`,
   and when stderr is not a terminal shows it natively: `MessageBoxW` on
   Windows; on Linux `zenity --error`, else `kdialog --error`, else a
   desktop notification over D-Bus (`org.freedesktop.portal.Notification`
   inside the Flatpak, `org.freedesktop.Notifications` outside; libdbus is
   already linked for the file-dialog portal) naming the text file.
   `LOBOLE_NO_DIALOG=1` suppresses the box for tests and CI, which would
   otherwise block on it. The text says what failed, in words, and what to
   try for that step: install or update the GPU driver (with the Vulkan
   loader package by distribution: `libvulkan1` + `mesa-vulkan-drivers`,
   `vulkan-icd-loader`), enable 3D acceleration in a VM, `--safe-mode`,
   `LOBOLE_RENDERER=software`, and that `--offscreen`/`--headless` serve the
   scripting surface without a window. The marker of that session is deleted
   as a clean exit's is, and the next start's notice names the startup
   failure rather than "crashed".
3. **A fallback ladder that announces every rung.** Device selection
   already scores devices by presentation support and type and honours the
   `gpuDevice` preference with a note when it cannot. It extends to: a device
   whose `vkCreateDevice` or swapchain setup fails is dropped and the next
   eligible one tried (each failure logged with its result); then the
   software device (`VK_PHYSICAL_DEVICE_TYPE_CPU`, lavapipe or llvmpipe) if
   the loader offers one; then the failure report above. Landing on a lower
   rung than the best device present is announced: a one-time modal at the
   first frame ("Rendering in software (llvmpipe): the NVIDIA RTX 4060
   failed: VK_ERROR_INITIALIZATION_FAILED at device creation"), a lasting
   status-bar entry, and Help ▸ About and `ping` (`renderer: {device,
   fallback, reason}`). A machine whose only device IS a CPU one (CI, a VM)
   is not a fallback: the status bar states it, no modal. The ladder never
   drops to `--offscreen` or headless by itself; the report offers them.
   `LOBOLE_RENDERER=software|hardware` forces a rung; any other value is
   refused at start, naming those two. Windows has no software Vulkan on a
   stock system, so its ladder ends at the message box.
4. **A renderer that fails to initialise is named.** Each failed
   `Initialize` appends to `g_App` a missing-renderer list shown in the same
   status entry and in `ping`; the mesh renderer failing is a
   `StartupError`, since nothing can be drawn without it.
5. **Safe mode.** `--safe-mode` (or `LOBOLE_SAFE_MODE=1`, a
   `[[nodiscard]]` bool parser that refuses anything but `0`/`1`) starts
   with default settings without writing `settings.json`, the default dock
   layout with `io.IniFilename` pointed at a scratch path so `imgui.ini` is
   untouched, the GPU preference and ray tracing off, and no file from the
   command line or recent list opened (the recovery modal still offers
   autosaves). It is entered **automatically** when the last two windowed
   sessions died before their first presented frame: the session marker
   (`CrashReports.h`) gains a `first_frame` field, written once by
   `crash::NoteFirstFrame()` after the first successful present, and
   `CollectDeadSessions` reports how many dead windowed sessions in a row,
   newest first, lack it, the run stopping at `<state>/last-good-start`, a
   stamp the first-frame note also writes. A safe-mode window shows a banner
   saying why (flag, or "the last 2 starts crashed before the window drew"),
   where the user's own files are, and two buttons: restart normally, and
   set the settings and layout aside (renamed `*.bad-<stamp>`, then
   restart). Never in `--gui-tests`, which already run on defaults.
6. **Bad settings set aside, said once.** `settings::Load` renames an
   unreadable file to `settings.json.bad-<stamp>`, never over an earlier
   one, and returns a note the windowed app shows as a one-time notice with
   the path. The layout file gets the same treatment from the safe-mode
   button; ImGui cannot report that it failed to parse one.
7. **Device loss is a state.** In the frame loop
   `VK_ERROR_DEVICE_LOST` from acquire, fence, submit or present (and from the
   renderers' one-shot submits, which now return their result) sets
   `g_App.deviceLost`. From then on: no recording, submitting or presenting;
   every renderer call is skipped; the remote server keeps serving, and an
   RPC that needs a renderer (`screenshot`, `render_still`, face picking that
   resolves a mesh) is refused naming `device_lost`. The document is CPU
   state and is intact, so the app immediately flushes autosave for every
   dirty document (`ui::AutosaveProject` forced), writes a report folder
   whose reason is "graphics device lost" with the log tail, and shows the
   native message box: what happened, that the work is saved for recovery,
   Restart or Quit. Restart re-executes the binary with its arguments plus
   `--after-device-loss`, so the new start offers the autosaves through the
   existing recovery modal and says why it restarted. A second loss within
   ten minutes restarts on the software rung, announced as in 3.
   `VK_ERROR_SURFACE_LOST_KHR` recreates the surface and swapchain; a
   swapchain recreate that fails is logged and retried next frame instead of
   aborting. `--offscreen` has no window to restart: it keeps serving, its
   captures refused, and `ping` says the renderer is lost.
8. **The watchdog names the GPU.** Fence and acquire waits take a finite
   timeout in a loop under a watchdog label ("waiting on the GPU: frame
   fence"), so a hung GPU's stall report says so instead of pointing into
   `FrameRender`. `check_vk_result` stays for the rest, printing the
   result's name, and its abort still becomes a crash report.

The topology optimiser's compute device (`femgpu/ComputeContext.cpp`) is
its own `VkDevice` and not part of this.

## Files

- `src/app/main.cpp`: `CheckStartup`, `StartupError`, device ladder in
  `SetupVulkan`, `SetupVulkanWindow`, the GLFW and window checks, the
  frame loop's loss state, the swapchain retry, safe-mode wiring,
  `--safe-mode` / `--after-device-loss` in `parseCommandLineArgs` and
  `printUsage`.
- A new `src/app/StartupReport.{h,cpp}`: the message text per step, the
  dialog chain (Windows, zenity, kdialog, D-Bus), `last-startup-failure.txt`.
- `src/render/VulkanCommon.cpp`, `src/render/Screenshot.cpp`: return the
  submit's result.
- `src/core/io/CrashReports.{h,cpp}`: `first_frame`, `NoteFirstFrame`,
  the consecutive-failure count, the "startup failure" and "graphics device
  lost" report reasons.
- `src/core/io/Settings.cpp`: `.bad-<stamp>` and the returned note.
- `src/common/Watchdog.*` only if a label needs a new entry point.
- `src/ui/FileDialogs.cpp` (the notices and the safe-mode banner),
  `src/ui/AppState.h` (`deviceLost`, renderer status, safe mode), the status
  bar, Help ▸ About.
- `src/remote/RemoteDispatch.cpp` and the capture RPCs' modules: the
  `device_lost` refusal, `ping`'s `renderer`, `debug_fault {kind:
  "device_lost"}`, and their `describe` rows.
- `tests/smoke_app.py` (`LOBOLE_NO_DIALOG=1` for every launch),
  `packaging/windows/smoke.sh`, `packaging/appimage/smoke.sh`.
- `docs/architecture.md#crashes-and-stalls`, `docs/rendering/README.md`,
  `docs/packaging/README.md` (a troubleshooting section the message links
  to), `docs/scripting/rpc.md`.

## Verified by

- **Fault injection, two ways.** Real ones where the environment can
  produce them: `VK_ICD_FILENAMES=/nonexistent` (no instance),
  `VK_ICD_FILENAMES=<lavapipe icd>` (only a CPU device). The rest through
  `LOBOLE_FAULT=device_create|swapchain|renderer_init:<name>`, honoured only
  with `LOBOLE_ALLOW_DEBUG_CRASH=1` and refused, naming the accepted kinds,
  for anything else. Mid-session loss through `debug_fault {kind:
  "device_lost"}`, which marks the device lost as the frame loop would.
- **Unit tests**: the message text names the step, the result's name, the
  log path and `--safe-mode`; the consecutive-failure count over markers
  with and without `first_frame` and across `last-good-start`; the
  `.bad-<stamp>` rename never replacing an earlier one;
  `LOBOLE_RENDERER` and `LOBOLE_SAFE_MODE` refusing unknown values.
- **A smoke (`tests/startup_failure_smoke.py`, through `smoke_app.py`)**:
  no ICD exits 3 with the step on stderr and in
  `last-startup-failure.txt`, leaves no crash report, and the next start's
  notice names it; `LOBOLE_FAULT=device_create` on a machine with two
  devices lands on the second and `ping` reports the fallback and reason;
  two startup crashes (`--crash-test` before the first frame) make the third
  start safe mode, with `settings.json` and `imgui.ini` byte-identical
  afterwards.
- **A pygui smoke for loss**: an edit, `debug_fault {kind: "device_lost"}`,
  then the autosave on disk holds the edit, `screenshot` is refused with
  `device_lost`, other RPCs still answer, the report folder exists with
  that reason, and the process is still alive (no SIGABRT).
- **A GUI test**: the safe-mode banner and the software-fallback modal are
  shown, and the status entry stays; `app/boots_and_renders` unchanged, and
  `render_pixel_smoke.py` unchanged, since nothing is drawn over the
  viewport on a CPU-only machine.
- **CI's Windows smoke** (`packaging/windows/smoke.sh`) adds
  `VK_ICD_FILENAMES=/nonexistent wine lobole.exe` with
  `LOBOLE_NO_DIALOG=1`, expecting exit 3 and the failure file under
  `%LOCALAPPDATA%\Lobole\state`; the AppImage smoke does the same, and once
  without `LOBOLE_NO_DIALOG` and without zenity or kdialog, to show the
  fallback chain ends in the text file rather than a hang.
- The headless, GUI and pygui suites.
