# Architecture

How the code is organised, and the rules that keep it that way. The
conventions every subsystem follows (axes, units, defaults, refusals) are in
[conventions.md](conventions.md).

## The one decision behind the design

A **headless core with a serializable command/document layer**; the GUI is one
client of it and the JSON-RPC remote is another.

- Every UI action is a *command* that edits a *document* (a feature list),
  never the geometry directly.
- The document regenerates its B-Rep deterministically from the feature list.
- The document serializes to human-readable, diffable text.

Undo, checkpoints, variables, scripting, git history and the agent interface
all follow from that: undo replays or inverts commands, a checkpoint is a
point in the feature history, variables are an expression pass during
regeneration, the command layer *is* the scripting API, and the text format is
what git diffs and merges.

## Source layout

Includes are relative to `src/` (`#include "core/Document.h"`).

| directory | what lives there |
|---|---|
| `src/core/` | the document model, flat: `Document`, `Feature`, `Command`, `Expression`, `Occurrence` (assemblies), `Measure`, `Units`, `Angles.h`, `Conventions.h`, `Parallel.h`, `Receipt.h` |
| `src/core/features/` | one file pair per feature type (`ExtrudeFeature`, `HoleFeature`, `DerivedFeature`, the sheet-metal and tube features, …); the four primitives (`Box`, `Sphere`, `Cylinder`, `Import`) are inline in `core/Feature.h` |
| `src/core/sketch/` | the 2D sketch solver (PlaneGCS), profiles, inference, selection |
| `src/core/io/` | serialization, split by domain (`Serialize{Features,Assembly,Studies,Configurations,Cam}.cpp`, shared `SerializationInternal.h`), the git vault, merge, autosave, checkpoints, settings, keymap |
| `src/core/{cam,drawing,fem,parts,catalog,sheet,tube}/` | one directory per domain |
| `src/cad/` | the OpenCASCADE wrapper (`CADKernel.h`: `CADShape`), materials, mesh conversion and export, sketch planes |
| `src/common/` | logging (`Logger.h`), runtime paths, build info, the terminal emulator and PTY (they live here so `designer_tests` can link them) |
| `src/render/` | the Vulkan renderers, `VulkanCommon` (the shared buffer/memory/shader helpers), camera, screenshot, IBL |
| `src/ui/` | `AppState` and the actions layer, one file per subject (below), plus the panels |
| `src/remote/` | the JSON-RPC server: one `Remote<Domain>.cpp` per domain plus the dispatcher (below) |
| `src/app/` | entry points: `main.cpp` (window, Vulkan setup, frame loop), `HeadlessMain.cpp`, `CrashHandler.cpp`, and the ImGui Test Engine GUI suite under `app/guitests/`, one file per domain |
| `src/femgpu/` | the Vulkan-compute topology solver |
| `src/shaders/` | GLSL, compiled to SPIR-V at build time |

**Edit the domain module, not a monolith.** A new RPC goes in its domain's
`Remote<Domain>.cpp`, a new persisted field in the matching `Serialize*.cpp`,
a new feature in `core/features/`.

### Layering

`src/core/` includes nothing from `ui/`, `render/`, `remote/` or `app/`. Two
files in `src/cad/` use render-side vertex and camera types
(`CADMeshConverter.h`, `FaceSelector.cpp`). `designer_core` links no Vulkan
library; the GPU topology solver is its own library, `fem_gpu`, so the core
stays GPU-free.

## Libraries and targets

| target | contents | notes |
|---|---|---|
| `designer_core` | `core/`, `cad/`, `common/`, `remote/InstanceRegistry.cpp` | the unit-testable target; carries OpenCASCADE, PlaneGCS, Eigen/Spectra, Netgen (via `fem_mesher`) |
| `designer_app` | `ui/`, `render/`, `remote/`, the GUI suite, ImGui | everything the app is except its entry points, so `designer_tests` can link the actions layer |
| `designer` | `main.cpp`, `HeadlessMain.cpp`, `CrashHandler.cpp` | links `designer_app`, `designer_core` and (when built) `fem_gpu` |
| `fem_gpu` | `src/femgpu/` | Vulkan compute; `DESIGNER_ENABLE_FEM_GPU` |
| `designer_tests`, `femgpu_tests` | Catch2 | see [Tests](#tests) |

Optional subsystems are CMake options, each printed in the configure summary
and reported by the `ping` RPC's `features`:

- `ENABLE_CAM` (Clipper2). Toolpath generation sits behind it (the
  polygon-offsetting `CamGeometry`, `CamDerive`, `CamStrategies`,
  `CamAdaptive`, and the surfacing, rotary, G-code, verify and simulation
  files that build on them); the job document, its names, frames, picks,
  travel, op fields and post dialects build either way, so a job tab exists
  in a CAM-off build. CI builds and tests `-DENABLE_CAM=OFF`.
- `ENABLE_CAM_ADAPTIVE`: adaptive clearing from libarea (LGPL), linked as a
  separate shared library, `designer_adaptive`, so only the bridge translation
  unit touches its symbols.
- `ENABLE_TERMINAL` (libvterm), `ENABLE_TEST_ENGINE` (no GUI suite and no
  `ui_action` without it), `DESIGNER_ENABLE_FEM_GPU`.
- Netgen, Eigen and Spectra are detected: without Spectra modal analysis
  refuses, without Eigen the FEM solver and the mate-loop solver are absent.

`features` also carries one thing that is not compiled in: `ffmpeg`, whether
that program is on PATH now. It is run, never linked, by
`export_motion_video` for its video formats
([motion video](assemblies/README.md#motion-video)); a missing one refuses
those formats by name.

`src/core/cam/` is compiled with `-Werror=switch`: the CAM generator switch
has no `default:`, so a new operation type without a generator fails the
build. `-Werror` is otherwise off.

## Precompiled headers

**Off by default** (`ENABLE_PCH`, CMakeLists.txt). GCC's `.gch` is not
byte-reproducible, and ccache folds its bytes into the key of every source
that uses one. Each build tree makes its own, so with a PCH no two trees share
an object. A fresh worktree missed 391 of 428 sources with every flag
otherwise identical, and main missed the objects an agent had just built for
the branch it was merging. Under `-O3` compiling dominates and parsing does
not, and the PCH saved only 5-10% on a file being recompiled: 5.8 s against
6.1 s for `ui/CamPanel.cpp`, 29.8 s against 33.3 s over four test files.

What makes a worktree share main's cache:

- **One flag set.** Nothing tree-specific is on the command line of the
  sources. The commit was a PUBLIC define on 409 files, so every reconfigure
  at a new commit recompiled them all. It is now on `common/BuildInfo.cpp`
  alone. A worktree builds main's build type (`tools/worktree-build.sh`).
- **Relative paths.** The compiler launcher sets `CCACHE_BASEDIR` to the
  source tree, so `-I…/src` hashes as `../src` in every tree.
- **One toolchain.** A tree configured before `shell.nix` moved keeps the old
  compiler and headers. `tools/fix-stale-nix-cache.sh` reconfigures it.

Measured on 22 cores, `designer` plus `designer_tests` in a fresh worktree:
**2 min 8 s, 404 of 423 compiles from the cache.** It was 11 min 30 s before.
The misses are `BuildInfo.cpp` and the sources that carry the tree's path in
a define (`DESIGNER_SOURCE_DIR`).

**Headers include what they use.** Turning the PCH off found two sources that
compiled only because it supplied their includes.

## The document model and regeneration

A project holds documents of four kinds (`ui::DocKind`): **Part**
(`Document`, a feature list), **Assembly** (`AssemblyDocument`, occurrences of
parts or sub-assemblies, mates), **Drawing** (`DrawingDocument`) and **Job**
(`cam::JobDocument`, CAM setups over placed parts). Documents reference each
other by stable id.

`Document::Regenerate()` re-evaluates the parameter table and every
expression-driven field, then executes the feature list in order. If a
feature fails, it returns false and the bodies are those of the successful
prefix. It runs on the calling thread.

- **Incremental.** A prefix cache (`RegenCacheEntry`) keys each feature on
  the chain hash of everything up to and including it and keeps the bodies
  after it, so an edit re-executes only from the first changed feature.
  `LastRegenStats()` reports `executed`/`cached`, and tests assert on it.
- **Everything a feature reads must be hash-visible.** A feature exposes its
  expressions through `VisitExprs` and its references to other features
  through `VisitFeatureRefs` (pure virtual, like `Clone`); a field missing
  from either is invisible to the cache or the dependency graph.
  `test_conventions.cpp` checks the `VisitFeatureRefs` half.
- **Configurations** regenerate against an override symbol table and
  `EffectiveSuppressed`, which also folds into the cache key.
- **Baked geometry.** The saved project carries the built bodies beside the
  recipe, tagged with the chain hash, so opening a project is a load rather
  than a rebuild.
- **Bulk edits** (`Document::BulkEdit`) suspend regeneration and run it once
  when the last guard releases; loading a file uses it.
- **Determinism** is what checkpoints, rollback, git merge, baked geometry
  and the golden corpus all rest on. `tests/test_determinism.cpp` loads each
  document twice, independently, and compares the results.
- **Undo** is a `CommandStack` of `Command`s on a part document; assembly tabs
  use snapshot undo of their occurrence list.
- Regeneration never crosses a document boundary. A part that needs another
  part's geometry takes a `Derived` feature: a baked snapshot of the source,
  stale when the source's version moves, refreshed only on request.

## The ui/ actions layer

`AppState` (`ui/AppState.h`) is one concrete struct; the actions are free
functions over `AppState&`. There is deliberately no abstract interface in
front of it: that style is what lets `tests/test_actions.cpp` drive the
actions layer headlessly.

**One file per subject, each with its own narrow header** —
`ProjectActions`, `DocumentSlots`, `ViewActions`, `DrawingActions`,
`MeasureActions`, `RendererSync`, `CreateActions`, `SceneOverlays`,
`SketchOverlay`, `VariableActions`, `ImportActions`, `SelectionPipeline`; the
assembly side as `AssemblyActions`, `MateActions`, `Bom`, `GizmoInput`,
`TessellationCache`, `FastenerActions`, `FlexibleActions`; the sketch session
as `SketchTools`, `SketchDimensions`, `SketchDimensionDraw`, `SketchPick`,
`SketchOps`, `SketchToolOverlay`, with `SketchInteraction.cpp` keeping the viewport event
router. There is no umbrella header: a file includes the header of the
subject it uses, so changing one declaration rebuilds only that subject's
includers. Helpers shared between the action files live in
`AppActionsInternal.h`, which only they include.

**No renderer seam.** Every renderer pointer in `AppState` is null-checked,
because `--headless` runs the whole app without renderers. A test constructs
an `AppState`, leaves the pointers null, and the sync paths no-op exactly as
they do headless. The one thing a test must not do is call an ImGui function:
`designer_tests` links ImGui through `designer_app`, so a call without a
context crashes rather than failing to compile. Widget and click paths belong
to the GUI suite.

**ImGui stays out of the logic files.** The text and pill overlays drawn over
the viewport live in `ui/Overlays.cpp`; the actions layer asks
`AppState::anyWidgetActive` (sampled once per frame by the loop) instead of
calling `ImGui::IsAnyItemActive()`.

**Long operations run off the frame.** Each (STEP import, FEM mesh and solve,
topology optimization, git sync, CAM toolpath passes, verification and the
playback's starting stock, motion studies, the offline render) has a worker and a `Poll…` that applies its result on the
main thread. `ui::PollBackgroundJobs` (`ui/BackgroundJobs.h`) is called once
per iteration by each of the three loops — windowed, `--offscreen` and
`--headless` — and never from a panel, so whether an operation can finish
does not depend on which panel is drawn (`test_conventions.cpp` gates it).
RPC handlers keep their own poll so they answer from a finished result.

## The remote (JSON-RPC)

- `RemoteServer.cpp` is the class; `RemoteSession.cpp` the socket transport
  and threading; `RemoteUiActions.cpp` the queue that runs `ui_action`s inside
  the ImGui frame; `RemoteDispatch.cpp` decides what a call means and routes
  it to the domain modules.
- Each domain module keeps a `MethodDoc` table next to its handler: `name`,
  `doc` (what `describe` and `help` serve), `params` (the accepted keys),
  `flags`, and `returns` (the response keys, required for `list_*`, `get_*`
  and `check_*`).
- **The `flags` column is the gate**, stated on the row rather than kept in a
  list apart from the handler: `NeedsPart` (refuses unless a part tab is
  active), `NotInBatch` (reaches outside the document, so a `batch` cannot
  roll it back), `Deferrable` (its result does not need fresh geometry, so a
  batch may defer the regeneration past it), `UiAction` (runs on the ImGui
  queue). An unknown method has no row and no flags, so a typo is answered
  "unknown method" rather than "switch to a part tab".
- `RpcParams` refuses unknown request keys.
- Gates over the tables: `tests/rpc_params_gate.py` (the `params` column
  against what each handler reads, both directions, via
  `tools/rpc_params_scan.py`), `tests/rpc_units_gate.py` (units, see
  [conventions.md](conventions.md#units-and-angles)),
  `tests/rpc_returns_gate.py`, `tests/rpc_names_gate.py` and
  `tests/rpc_gate_smoke.py`; the shared row parser is
  `tools/method_doc_scan.py`.
- Changing a method's name, a refusal message, a `describe` row or `help()`
  text changes what `benchmarks/` measures (see CLAUDE.md).

### Requests that wait

The pump (`ProcessPending`, `RemoteUiActions.cpp`) runs one request at a time
on the main thread. Three kinds span frames. A `ui_action`, a `screenshot`
and a `render_sheet` hold the WHOLE queue until they finish, because they
drive the frame itself. A CAM request that would compute for minutes is
PARKED instead (`RemoteServer::Parked`, `remote::CamParking`): the work goes
to background jobs, `PollParked` asks it once a tick, after
`ReapBackgroundJobs`, whether it is done, and the answer -- or a refusal, or
the request put back at the head of the queue to run its handler from the
caches -- goes out then ([cam/README.md](cam/README.md#requests-that-wait)).
A parked request holds only its OWN connection's later requests, so a
pipelining client still gets its answers in order while other connections
(the MCP server, a second script) are served. While anything is parked the
pump reports activity, which keeps the idle pacer at full rate. A parked
request whose client disconnects is dropped; its passes land in the cache
anyway.

## One table per kind of thing

Wherever a kind of thing has properties spread over several consumers, one
table states them and every consumer reads it, because separate lists keyed
by the same name drift apart without any test noticing:

- `kFeatureTypes` (`core/io/SerializeFeatures.cpp`) is authoritative:
  `FeatureFromJson` refuses a type with no row, so a new feature cannot load
  and be missing from `describe`.
- The CAM operation types: `core/cam/CamNames.h` (names, serialized strings,
  whether an operation follows one target at a time), with the generator a
  `-Werror=switch` switch in `CamStrategies.cpp`.
- The RPC `MethodDoc` rows and their flags (above).
- The named views, world axes and light rig (`core/Conventions.h`).
- Scene renderers: `RegisterSceneRenderers()`/`ShutdownSceneRenderers()` in
  `main.cpp`; icons: `ui/Icons.cpp`; tool sessions: the tool-session registry
  that `ViewportInput.cpp` asks through `ToolSessionFor`. Each is checked by
  `test_conventions.cpp`.

## Threading

- The UI, regeneration and the renderers run on the main thread; the remote
  server has its own thread and hands work to the main loop; long operations
  run on workers and are applied by their `Poll…` (above).
- `core/Parallel.h` is the one parallel-for: flat batches of independent
  OpenCASCADE work, results collected per index and merged in order so a
  response is reproducible. `DESIGNER_SWEEP_THREADS` pins the thread count.
- **OpenCASCADE is not safe to share between threads.** It fills caches
  lazily on the shape (bounding boxes, tolerances, triangulation), so every
  caller of `parallel::For` warms those single-threaded first. That is not
  enough for booleans: a boolean *writes* to its arguments (the p-curves it
  computes go onto the input edges), and what it writes is specific to the
  pair, so no warming makes two concurrent booleans over a shared body safe.
  Every query boolean in `Measure.cpp` is built through `BuildQuery`, which
  sets `SetNonDestructive`; `test_conventions.cpp` fails on a boolean built
  any other way. `tests/sweep_parallel_smoke.py` diffs a one-thread report
  against an all-thread one byte for byte.
- `CADShape` memoizes its bounding box on the shape, so it replaces its shape
  only through `SetShape` (gated).

## App modes

- **Windowed** (`designer`): the GUI.
- **`--offscreen`**: the real renderer with no window and no display server;
  it has no panels, so the viewport is the whole framebuffer.
- **`--headless`** (`HeadlessMain.cpp`): no renderer at all; non-visual RPC
  work.
- `--remote <port>` serves JSON-RPC (`--remote 0` binds a kernel-chosen
  port); every instance announces itself in
  `~/.local/state/designer/instances/<port>.json`. `ping` reports the port,
  the open project, the build's optional `features`, and `binary_replaced`
  (Linux: the executable was rebuilt under a running instance).

## Crashes and stalls

- **A crash** (SIGSEGV, SIGABRT, SIGBUS, SIGFPE) prints a symbolized stack
  to stderr from `CrashHandler.cpp`, then dies as it would have.
  `designer --crash-test` exercises it.
- **A stall** — the main thread not coming round its loop — is reported by
  the frame watchdog (`src/common/Watchdog.{h,cpp}`). Each loop (windowed,
  `--offscreen`, `--headless`) beats it once per iteration; a monitor thread
  that sees no beat for 3 s logs `main thread stalled 3.0 s (in <label>)`,
  the main thread's stack, and later `main thread resumed after N s`, once
  per stall. The label says what was running when that is known: the RPC
  being handled, or a native file dialog (reported without a stack — the
  wait is the user). Every RPC, `ping` included, runs on the main thread, so
  a stall also silences the remote, and this log line is where it is
  explained. Nothing is watched before the first beat, so start-up is not
  reported.
- **The stack of the stalled thread** comes from a SIGUSR2 sent to it. The
  handler only captures return addresses into preallocated memory (a static
  arena for `std::basic_stacktrace`), because the thread may be slow rather
  than stuck — inside `malloc`, say — and a handler that allocated there
  would deadlock it for good. The watchdog symbolizes the capture on its own
  thread; a release build names functions but has no file:line. With debug
  info, a frame inside an inlined call is named by the innermost function
  (`sleep_for at this_thread_sleep.h:82`), so the executable's symbol table
  adds the function it lies in (`, inlined in StallSelfTest()`); without
  that an optimised build with `-g` (RelWithDebInfo, the TSan tree) showed
  the stall's helper and never the function that stalled.
- `DESIGNER_WATCHDOG_S` sets the threshold (default 3; 0 turns it off, as a
  debugger stopping the process is not a hang); a value that is not seconds
  is refused at start-up. `designer --stall-test` stalls the loop for 4 s once,
  and `tests/watchdog_smoke.py` checks the stall line, the label, the stack and
  the resume.

## Tests

| suite | where | runs |
|---|---|---|
| C++ unit + integration | `tests/test_*.cpp` → `designer_tests` (Catch2; tags become ctest labels via `ADD_TAGS_AS_LABELS`) | headless |
| GPU topology solver | `tests/test_femgpu.cpp` → `femgpu_tests` | headless, needs a Vulkan device |
| python smokes and gates | `tests/*_smoke.py`, `tests/*_gate.py` | headless, against a launched app |
| agent blind spots | `tests/blindspots/` (one file per known agent mistake, run by `blindspot_smoke.py`; never delete one) | headless |
| GUI | `src/app/guitests/GuiTests<Domain>.cpp` (ImGui Test Engine) | `tools/gui-tests-xvfb.sh` |
| render pixels | `tests/render_pixel_smoke.py`, byte-exact against a per-device baseline | needs a GPU |
| golden corpus | `tests/golden/`, `tests/golden/assembly/`, gated by `test_golden.cpp` | headless |

- `tests/smoke_app.py` is the only way a test launches the app
  (`launch_app()`); `tests/config_isolation_lint.py` fails any script that
  spawns the binary directly. It keeps the app's log, prints it on failure,
  and archives the log, traceback and a `reproduce.sh` under
  `$XDG_STATE_HOME/designer/smoke-failures/`.
- The GUI runner shards, shuffles by default and prints the seed that
  replays the run; a failing shard is archived under
  `$XDG_STATE_HOME/designer/gui-failures/` with its exact filter and a
  `reproduce.sh`.
- `tests/test_actions.cpp` pins UI logic headlessly (read its header before
  adding to it); `tests/test_conventions.cpp` holds the source-scanning rules
  listed in [conventions.md](conventions.md#rules-the-source-is-scanned-for).
- Heavy tests carry the `[slow]` tag; `tools/test-fast.sh` is
  `ctest -LE 'gui|slow'`. A tag is the ctest label, so a test's cost class
  lives on the test rather than in a list in `tools/`.
- Every test that needs a display carries the `gui` label, which is what the
  headless run excludes. `gui_smoke` runs the same cases as
  `tools/gui-tests-xvfb.sh`, serially; the python ones also carry `pygui`, so
  they run as `ctest -L pygui` rather than by excluding `gui_smoke` by name.
- `src/render/` and `app/main.cpp` have no unit tests; they are verified by
  the GUI suite, the pixel gate and screenshots. Low line coverage there is
  expected.

Occasional audits, not in CI: `tools/coverage.sh` (reports the `src/core`
functions nothing executed), `tools/profile.sh` (sampling profiler),
`-DENABLE_SANITIZERS=ON` (ASan + UBSan, runtime options compiled in via
`src/common/SanitizerDefaults.cpp`) and `-DENABLE_TSAN=ON` (ThreadSanitizer,
a separate build; Netgen's own thread pool is suppressed in
`tools/tsan-suppressions.txt`, OpenCASCADE deliberately is not, since OCC is
called from our workers).

## C++ usage

- C++23 (`CMAKE_CXX_STANDARD 23`); the default build type is Debug, which
  compiles at `-Og`. It has to name a level: nix's cc-wrapper puts `-O2` in
  front of every compile for its fortify hardening, and CMake's Debug passes
  none, so a Debug build under nix-shell was silently `-O2`. A level given in
  `CMAKE_CXX_FLAGS` (coverage's `-O0`) is left alone.
- **Logging goes through the `LOG_*` macros** (`common/Logger.h`, spdlog).
  The only direct stdout/stderr writes are `main.cpp`'s command-line usage
  and argument errors, which are program output and must not carry a log
  prefix or be silenced by `--log-level`.
- New fallible functions return `std::expected<T, std::string>`; the
  `bool f(..., std::string* error)` out-parameter form is legacy.
- `std::to_underlying` for enum-to-integer casts.
- Ranges views (`enumerate`, `zip`, `ranges::to`) are fine in cold code; keep
  explicit loops in the solver kernels (`core/fem/`, `femgpu/`), because the
  Debug build does not inline view wrappers.
- Nothing is named `near` or `far`: `windows.h` defines both as empty macros
  and the Windows cross-build would break (gated).
