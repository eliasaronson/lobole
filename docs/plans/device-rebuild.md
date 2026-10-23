# Device rebuild

## The problem

`VK_ERROR_DEVICE_LOST` is a state
([A lost device](../rendering/README.md#a-lost-device)): nothing draws,
`NeedsRenderer` RPCs and UI actions are refused as `device_lost`, the work
is autosaved, a report written, and a native box offers Restart
(`--after-device-loss`). No work is lost, but a Windows TDR or a driver
reset costs a relaunch, the recovery modal, the open tabs, and a driving
script its connection.

The document is CPU state and survives a loss intact; only the Vulkan
objects are gone. They can be made again in place: the spec's
[Lost Device](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#devsandqueues-lost-device)
section says the lost state belongs to the logical device and the physical
device may be unaffected; that the application destroys the lost `VkDevice`
and creates a new one; that the commands which wait indefinitely
(`vkDeviceWaitIdle`, `vkQueueWaitIdle`, `vkWaitForFences` and
`vkAcquireNextImageKHR` with an infinite timeout) return in finite time on a
lost device, with `VK_SUCCESS` or `VK_ERROR_DEVICE_LOST`; that destroying
objects and freeing memory remain valid on it; and that `vkCreateDevice`
answering `VK_ERROR_DEVICE_LOST` means the physical device is lost too.

What makes an in-place rebuild unsafe today is that nothing knows the whole
set of GPU owners. Construction is written out twice (`RunWindowed` and
`RunOffscreen` in `src/app/main.cpp`), teardown twice more, and only the six
scene renderers have a registry (`RegisterSceneRenderers` /
`ShutdownSceneRenderers`, gated by `tests/test_conventions.cpp`). Outside the
renderers, `AppState` holds mesh and instance ids and upload stamps that
believe the GPU still holds something.

The GPU preference has the same gap from the other side: `gpu_device` is
the one `Tier::Restart` setting, because the device is built once.

## The shape of the fix

1. **One trigger.** `RequestDeviceRebuild(reason)` has exactly two
   reasons: `DeviceLost`, set only by `MarkDeviceLost`, which runs only on a
   `VK_ERROR_DEVICE_LOST` result (`FrameOk`, `check_vk_result`, a swapchain
   recreate, `vkutil::DeviceLostSeen`) or `debug_fault {kind:
   "device_lost"}` (gated by `LOBOLE_ALLOW_DEBUG_CRASH=1`); and
   `GpuPreference`, set by a change to `gpu_device`. Nothing else rebuilds:
   not `VK_ERROR_SURFACE_LOST_KHR` (`RebuildSurface`, as now), not an
   out-of-date or failed swapchain recreate (retried each frame, as now), not
   out-of-memory, a renderer that fails to initialise, a slow frame or a
   watchdog stall. `test_conventions.cpp` fails if `RequestDeviceRebuild`
   gains a caller, or `MarkDeviceLost` a call site that is not beside a
   `VK_ERROR_DEVICE_LOST` test or `DeviceLostSeen`.
2. **One owner table.** The device and everything made on it move out of
   `main.cpp` into `src/app/GpuOwners.{h,cpp}`: a table of rows `{name,
   create, destroy}` in creation order, walked forward to build and backward
   to tear down, for both modes (a row says whether it is windowed-only).
   The rows: the logical device with its queue, descriptor pool and command
   pool; the surface, swapchain, depth and MSAA images and the depth render
   pass (`SetupVulkanWindow`, `SetupDepthBuffer`), or `--offscreen`'s render
   pass; ImGui's Vulkan backend (`ImGui_ImplVulkan_Init` / `Shutdown`, which
   owns its pipeline, its font texture and the descriptor sets taken from
   our pool); `IBLTextures`; `ShadowRenderer`; `GpuTimer`; `SSAORenderer`;
   the six scene renderers; `RayTracingRenderer`; the sky's `IBLTextures`
   with `g_SkyIBLKey` reset to `{-1, -1}`. `FaceSelector` holds no Vulkan
   handle but keys its pick data on mesh ids, so it is a row too. The
   instance and its debug-report callback stay unless
   `vkEnumeratePhysicalDevices` fails or offers nothing, which remakes them.
   Startup and exit walk the same table, so teardown runs on every exit and
   construction on every start: the rebuild path is the start path.
   `RegisterSceneRenderers` stays as the draw-order list, filled from the
   table's scene rows.
3. **The gate covers every owner.** `test_conventions.cpp` extends the
   scene-renderer gate: every source file that calls `vkCreate*`,
   `vkAllocate*` or `ImGui_ImplVulkan_*` is either the file of a row in the
   table or on a named list of transient users whose objects live inside
   one call (`Screenshot.cpp`, `FrameStream.cpp` behind the one-slice
   `ViewportFrameCapture`, `VulkanCommon.cpp`'s helpers). `src/femgpu/` is
   excluded by name: its own instance and device. A new file that makes a
   Vulkan object fails the gate until it is classified.
4. **Every belief about the GPU is forgotten.** The `AppState` fields that
   name something a renderer holds move into one struct,
   `AppState::GpuProjection gpu`: `syncedDocumentVersion`,
   `documentMeshes`, the GPU half of `partRenderCache` and
   `tessellationCache` (their CPU tessellations stay), the section-cap
   instances, `topoGhostInstance`, `topoIsoInstance`,
   `deltaMissingInstance`/`deltaExtraInstance`, `bearingGhostInstance`,
   `camToolInstance`, `camStockDrawnRevision`/`camStockDrawnAt`,
   `fieldBuiltKey`, the sketch renderer's static-line key, `rtSceneValid`.
   `ui::ForgetGpuProjection` (`RendererSync.cpp`) is `state.gpu = {}` plus
   re-pointing the typed renderer pointers, `sceneRenderers`, `rayTracer`,
   `faceSelector`, `ibl` and `skyEnvironment` (`AppState.h` "Non-owning"),
   through the one function startup also uses. A gate fails on a `uint32_t
   …Instance`, `meshId` or `…Drawn…` field declared in `AppState.h`
   outside that struct. The renderers' id counters start above the previous
   generation's, so an id the gate missed cannot alias a new asset, and an
   unknown id is a logged no-op. The document is then projected again the
   way it always is: the windowed loop on its next frame; `--offscreen`
   lazily ([lazy projection](../rendering/README.md#offscreen-mode-and-lazy-projection)),
   because `RenderMeshesCurrent` now answers false. Underlay textures,
   the CAM stock mesh and the FEM field are rebuilt from their CPU sources
   by the same stamps. Renderer state set once at startup (the outline's
   wireframe mode, viewport sizes, shadow-map size, SSAO tier, lights
   locked) is applied from `AppState` after create, by the code that applies
   it at startup.
5. **The order of one rebuild**, all on the main thread at a frame
   boundary: flush the autosave (as now); end a running render job or
   motion video with "the graphics device was rebuilt; run it again"; release
   the part render cache against the old renderers (destroy calls are valid
   on a lost device); `vkDeviceWaitIdle`, its `VK_ERROR_DEVICE_LOST`
   expected and ignored; the owner table backward; re-enumerate the physical
   devices and walk the [device ladder](../rendering/README.md#the-device-ladder)
   with the lost device first for `DeviceLost` (then the usual order, ending
   at the software device), or the usual order for `GpuPreference`; the
   table forward; `ForgetGpuProjection`. Landing below the first rung is
   announced as the ladder announces it now. A watchdog label ("rebuilding
   the graphics device") names a slow one. Remote requests wait in the
   queue meanwhile, as they wait for any frame. `ping`'s `renderer` gains
   `rebuilds` and `last_rebuild {reason, from, to, ms}`; the menu bar says
   "Graphics device restarted" once. A recovered loss writes its report
   under the reason `graphics device recovered`, which the next start does
   not offer as a recovery notice.
6. **The backstop.** A rebuild whose ladder holds no rung, whose mesh
   renderer fails, that throws, or a second `DeviceLost` within 60 s of the
   last rebuild (a device that dies again on its first frames), becomes
   today's lost state: refusals, report, and the Restart box windowed, or
   serving on without captures offscreen. The restart path stays as it is.
   `LOBOLE_FAULT=rebuild` (with `LOBOLE_ALLOW_DEBUG_CRASH=1`) fails the
   rebuild's device creation on every rung, so the backstop is testable.
7. **The preference is a rebuild.** `gpu_device` becomes `Tier::Live`, and
   `Tier::Restart`, with no other user, goes. A change rebuilds only when
   the ladder's first rung differs from the device in use, and waits while a
   render job, a motion video or a test-engine capture runs (the row says
   "switches when the render finishes"). The Preferences row says what is in
   use, or why the chosen device was not; `set_settings {gpu_device}` holds
   its answer until the rebuild has run and returns `renderer` in its own
   response. Every switch in normal use runs the whole path.
8. **femgpu.** Its separate `VkDevice` on the same physical GPU may be lost
   too, independently: it never triggers or waits for the render rebuild.
   `ComputeContext`'s fence waits (`UINT64_MAX` now) wait in 0.1 s slices that
   poll the run's cancel, so a driver that misses the spec's finite-time
   guarantee still lets the run be cancelled. A `VK_ERROR_DEVICE_LOST`
   there ends the topology or static run as failed, "the GPU was lost at
   iteration N; run it again", rather than taking the CPU fallback meant for a
   GPU that cannot do the problem (which can be hours). The next run creates
   a new context, as every run does.

## The risky parts

- **ImGui's backend re-initialised inside a frame.** It never is. The
  rebuild runs at the top of the loop, after the previous frame's present
  (or its abandoned submit) and before `glfwPollEvents` and
  `ImGui_ImplVulkan_NewFrame`. Only the Vulkan backend goes: the ImGui
  context, its windows, docking, font atlas CPU data, `ImGui_ImplGlfw` and
  the test engine are untouched. The font texture is remade by the new
  backend.
- **Objects made mid-frame on a device that is already lost.** A loss seen
  inside a frame (a one-shot upload in `SyncRenderersToDocument`, a
  capture) only sets the flag; the frame's ImGui CPU side finishes and
  nothing is submitted. Whatever a renderer made in that frame belongs to
  that renderer and goes with its row. Handles kept outside an owner are
  what step 4's gate exists for.
- **ImGui's own error reports during teardown.** `check_vk_result` aborts
  on a negative result except while `g_InSwapchainRecreate`. A rebuild
  collects ImGui's results the same way, so `ImGui_ImplVulkanH_DestroyWindow`'s
  wait on a lost device neither aborts nor marks a second loss.
- **A rebuild asked for during a capture or a job.** A real loss ends the
  render job or motion video by name (step 5); a screenshot request queued
  behind it runs after the rebuild, on the new renderers. CAM and FEM jobs
  run on CPU workers and continue; their results are re-uploaded through
  the stamps. A preference change waits for a capture instead of ending it.
- **A different swapchain image count.** Every renderer is remade, so no
  per-frame array outlives it; `init_info.ImageCount`,
  `g_App.swapchainImages` and `FrameIndex = 0` are set from the new
  swapchain, and the `kMaxFramesInFlight` wrap (`render/FramesInFlight.h`)
  stays.
- **An injected loss is not a real one.** `debug_fault` rebuilds a healthy
  device, whose waits succeed. Teardown ignores every wait result either
  way, `LOBOLE_FAULT=rebuild` covers creation failing, and a real TDR is
  checked by hand on Windows.

## Files

- `src/app/GpuOwners.{h,cpp}` (new): the owner table, build, teardown,
  the rebuild and its backstop policy.
- `src/app/main.cpp`: the device globals and both construction and
  teardown paths move to the table; the frame loop and the offscreen tick
  call the rebuild at their boundary; `check_vk_result` during a rebuild.
- `src/app/StartupReport.{h,cpp}`: `DeviceLadder` with a device first;
  the recovered-loss wording.
- `src/ui/AppState.h` (`GpuProjection`, `renderer.rebuilds`),
  `src/ui/RendererSync.{h,cpp}` (`ForgetGpuProjection`, render-job end),
  `src/ui/CamPlayback.cpp`, `SimulationActions.cpp`, `UI.cpp`,
  `MotionVideo.cpp`, the section-cap and ghost code that move their stamps.
- `src/render/MeshRenderer.*`, `OutlineRenderer.*`: id counters, unknown
  ids.
- `src/core/io/Settings.{h,cpp}`, `src/ui/SettingsWindow.cpp`: the tier.
- `src/remote/RemoteDispatch.cpp`: `ping`, `set_settings`, `debug_fault`
  and their `describe` rows.
- `src/core/io/CrashReports.{h,cpp}`: the recovered reason.
- `src/femgpu/ComputeContext.cpp`, `src/core/fem/FemTopo.cpp` and the
  static solve's seam: sliced waits, the loss as a failure.
- `tests/test_conventions.cpp`, `test_startup_report.cpp`,
  `test_settings.cpp`, `src/app/guitests/GuiTestsShell.cpp`,
  `tests/device_loss_smoke.py`, `tests/device_rebuild_smoke.py` (new),
  `tests/render_pixel_smoke.py`, `tests/CMakeLists.txt`.
- `docs/rendering/README.md` (A lost device, the device ladder, scene
  renderers, how it is tested), `docs/ui/settings.md`,
  `docs/architecture.md`, `docs/scripting/rpc.md`, `docs/topopt/README.md`,
  `docs/simulation/README.md`.

## Verified by

- Unit: the ladder with the lost device first; the backstop policy (a
  second loss inside 60 s, a failed rebuild); `gpu_device` is Live and no
  setting is Restart; the three gates, each shown failing on a planted
  violation.
- A GUI test, `app/device_rebuild_keeps_the_session`: a part with a sketch
  underlay, a FEM field and a CAM stock shown; `debug_fault device_lost`;
  then the viewport draws, a face is picked by click, a quick still is
  taken, the underlay, field and stock are on screen, the undo stack and
  open tabs are unchanged. Again through the GPU preference, switching to
  the software device and back; on a machine with one device that can
  present, that half is skipped by name.
- `tests/render_pixel_smoke.py --rebuild`: the fixed scene before and after
  a rebuild on the same device, byte-identical to each other and to the
  baseline.
- `tests/device_rebuild_smoke.py` through `tests/smoke_app.py`, windowed
  (`--hidden`) and `--offscreen`: an edit, `debug_fault device_lost`, then
  `screenshot`, `select_face` and `render_still` answer; `ping` counts the
  rebuild; no `device_lost` refusal is seen; `set_settings {gpu_device}`
  answers with the device it landed on. The offscreen half carries no `gui`
  label, so the sanitized suite runs it.
- `tests/device_loss_smoke.py` keeps the backstop: two losses inside the
  window, and `LOBOLE_FAULT=rebuild`, each end at today's refusals, report
  and Restart.
- `tools/ci-local.sh --asan` with the offscreen rebuild smoke, and the GUI
  test under `tools/run-sanitized-app.sh`: no use-after-free from a handle
  kept across a rebuild.
- Validation layers clean across a rebuild: the GUI test and the smoke run
  with the layer `DEV_WATCH_VALIDATE=1` turns on
  ([gpu-debugging.md](../rendering/gpu-debugging.md)), and the smoke fails
  on a validation error in the app's log.
- femgpu: a topology run whose compute context is told it is lost ends
  failed with the loss named, and a second run completes.
- The headless, GUI and pygui suites.
