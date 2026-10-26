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
| `src/core/features/` | one file pair per feature type (`ExtrudeFeature`, `HoleFeature`, `DerivedFeature`, the sheet-metal and tube features, …); `ImportFeature` and the stored primitives an older document carries (`Box`, `Sphere`, `Cylinder`) are inline in `core/Feature.h`; the Box/Cylinder/Sphere tool, which adds a sketch and an extrude, is `core/Primitives.{h,cpp}` |
| `src/core/sketch/` | the 2D sketch solver (PlaneGCS), profiles, inference, selection |
| `src/core/io/` | serialization, split by domain (`Serialize{Features,Assembly,Studies,Configurations,Cam}.cpp`, shared `SerializationInternal.h`), the git vault, merge, autosave, checkpoints, settings, keymap |
| `src/core/{cam,drawing,fem,parts,catalog,sheet,tube}/` | one directory per domain |
| `src/cad/` | the OpenCASCADE wrapper (`CADKernel.h`: `CADShape`), materials, mesh conversion and export, sketch planes |
| `src/common/` | logging (`Logger.h`), runtime paths, build info, the terminal emulator and PTY (they live here so `designer_tests` can link them) |
| `src/render/` | the Vulkan renderers, `VulkanCommon` (the shared buffer/memory/shader helpers), camera, screenshot, IBL |
| `src/ui/` | `AppState` and the actions layer, one file per subject (below), plus the panels |
| `src/remote/` | the JSON-RPC server: one `Remote<Domain>.cpp` per domain plus the dispatcher (below) |
| `src/app/` | entry points: `main.cpp` (window, frame loop, captures), `GpuOwners.cpp` (the Vulkan device and every renderer, as one table of owners, and its rebuild in place), `HeadlessMain.cpp`, `CrashHandler.cpp`, and the ImGui Test Engine GUI suite under `app/guitests/`, one file per domain |
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
| `designer` | `main.cpp`, `GpuOwners.cpp`, `HeadlessMain.cpp`, `CrashHandler.cpp` | links `designer_app`, `designer_core` and (when built) `fem_gpu` |
| `fem_gpu` | `src/femgpu/` | Vulkan compute; `LOBOLE_ENABLE_FEM_GPU` |
| `designer_tests`, `femgpu_tests` | Catch2 | see [Tests](#tests) |

**The targets keep the old name; what ships does not.** Loböle is the
wordmark and `lobole` the identifier everywhere a user or another program
sees it: the binary (`OUTPUT_NAME lobole` on the `designer` target), the
application id `org.lobole.Lobole`, the per-user directories
(`src/common/RuntimePaths.cpp`), the instance-registry handshake
(`{"app": "lobole"}`), the MCP server's name, and every environment
variable and CMake option (`LOBOLE_*`; `test_conventions.cpp` fails on
the old prefix). Never the ö in an identifier. The CMake target names,
the C++ namespaces and the document format (`kFormat`, `.dsgn`) are
invisible outside the build and keep "designer"; the format moves only with
a `kVersion` bump wanted for its own reasons. "designer" in prose is also the
English noun for the person using the app, and stays.

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
  `ui_action` without it), `LOBOLE_ENABLE_FEM_GPU`.
- Netgen, Eigen and Spectra are detected: without Spectra modal analysis
  refuses, without Eigen the FEM solver and the mate-loop solver are absent.

`features` also carries one thing that is not compiled in: `ffmpeg`, whether
that program is on PATH now. It is run, never linked, by
`export_motion_video` and `export_assembly_video` for their video formats
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
  compiler and headers. `tools/fix-stale-nix-cache.sh` reconfigures it,
  from the source its cache names (`CMAKE_HOME_DIRECTORY`), so a
  worktree's tree reconfigured from main stays the worktree's
  (`tests/fix_stale_nix_cache_smoke.py`). A package's directory cached as
  not found (a configure that ran after a GC took the package) counts as
  stale too: no store path names it, and the script called such a tree
  clean. A missing OpenCASCADE now stops the configure, where it used to
  warn and carry on into a build that failed on a missing header.

Measured on 22 cores, `designer` plus `designer_tests` in a fresh worktree:
**2 min 8 s, 404 of 423 compiles from the cache.** It was 11 min 30 s before.
The misses are `BuildInfo.cpp` and the sources that carry the tree's path in
a define (`LOBOLE_SOURCE_DIR`).

**Headers include what they use.** Turning the PCH off found two sources that
compiled only because it supplied their includes.

## The development loop

What builds and tests here run on, and why each piece is the shape it is.
The numbers were measured on this 22-core, 30 GB machine.

- **Memory, not cores, limits parallel work.** A compiler takes 0.6 to
  1.8 GB. Four agent worktrees building at one job per core got the OOM
  killer to take 58 compilers and then the agent host. `tools/build-jobs`
  sizes `-j` from free memory (about 1.5 GB a compile, 700 MB a test that
  starts the app), and worktree agents start two at a time.
- **One way to build, and one to test, under a lock**
  (`tools/build-lock.sh`). `tools/build.sh` builds with an exclusive `flock`
  on the build directory. `tools/merge-branch.sh` takes the same lock before
  it cherry-picks, numbers a branch's format bump
  ([versioning/format.md](versioning/format.md#a-bump-takes-its-number-when-it-lands)),
  then builds, tests and removes the worktree. A pick that conflicts only
  in the files made from the MethodDoc rows (`python/lobole/rpc.py`,
  `api/rpc-surface.json`) has them made again from the merged rows and goes
  on, since every pair of branches that each add an RPC conflicted there;
  any other conflict stops for a person. `--pick-only` picks and numbers
  without building, testing or removing anything. A merge that
  landed mid-build once linked objects that saw two versions of `AppState.h`,
  and the binary died with a `bad_alloc` that looked like heap corruption.
  `tools/ctest` (ctest, every argument passed through) and the GUI runner
  hold it shared: suites run side by side, and a build or a merge waits for
  them, where one used to relink `designer_tests` mid-suite so a test failed
  once and passed on the rerun. What a suite launches does not inherit the
  lock, so an app a test left running cannot keep it. `build_lock_smoke`
  proves a merge waits for a running suite. Under the lock, both refuse to
  run when the app or a test binary is older than any file it is built from
  (`tools/stale-binaries.sh`, `ninja -t inputs`): `femgpu_tests`, which
  `tools/build.sh` did not build, sat twelve days behind main and would have
  passed on old code had a nix GC not taken its libraries. `tools/build.sh`
  builds it with the pair now.
- **Trees share one ccache** ([precompiled headers](#precompiled-headers)): a
  fresh worktree builds in about 2 minutes, where it took 11.5.
- **`tools/nix-run`** runs a command with what `nix-shell` adds to the
  environment, cached and keyed on `shell.nix`, the OpenCASCADE overlay and
  its patches, and nixpkgs: ~0.1 s against
  ~5 s to enter. It caches `nix-shell`'s own additions, because `nix
  print-dev-env` resolved a different nixpkgs, and it keeps the caller's
  `TMPDIR` and `DISPLAY`. It captures them from a clean environment: a
  `nix-shell` run inside another appends to the outer one's search paths,
  and a worktree whose `shell.nix` patched OpenCASCADE linked the outer
  shell's stock library.
- **Wait on a pid or a marker.** `tools/wait-for.sh` waits with a deadline
  under the tool's 600 s cap. A `pgrep -f` loop matches its own command line
  and never ends, and `pkill -f` kills the shell that ran it.
- **The fast loop skips `slow`.** `tools/test-fast.sh` runs in about 65 s.
  The suite's floor was `watch_smoke` (73 s) until it was tagged.
- **A dear result is kept and keyed by its inputs.** A generator stage that
  runs a long study saves the project that holds its result and reuses it
  while the inputs are the same (the pan/tilt yoke's topology study, kept
  while the envelope's volume is unchanged, `--fresh-topology` to rerun):
  a verification that rebuilt the head from nothing ran a two-hour study a
  second time. `run_all.sh PORT --from STAGE` runs what changed.
- **A generator times its stages** (`Clock` in
  `generators/pantilt/common.py`), so a slow run says where: the pan/tilt
  drawings stage was 146 s, of which most was
  re-cutting section views through a cache too small to hold them (27 s
  once it did).
- **A study outlives its client.** A killed script leaves its study running
  in the app; stop the instance by its pid (`headless-preview.sh --stop`,
  with the same `XDG_STATE_HOME` as its `--start`).
- **clangd** skips `.claude/worktrees/` (`.clangd`). Each worktree is
  another copy of the project, and the index held 5.4 GB.

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

- **Incremental.** The regeneration cache (`RegenCacheEntry`) keys each
  feature evaluation on the chain hash of everything up to and including it,
  and keeps the bodies after it plus a clone of the feature as its Execute
  left it. It remembers more than the current history, so an edit
  re-executes only from the first changed feature, and setting a value back,
  undo and redo execute nothing. A hit restores the feature's derived state
  through `Feature::CopyDerivedState` (pure virtual), since the object may
  last have run in another history
  ([modeling](modeling/README.md#regeneration)). `LastRegenStats()` reports
  `executed`/`cached`, and tests assert on it; `tools/bench.py`'s
  `history/om615-head` times it, and `LOBOLE_TEST_NO_REGEN_CACHE=1` caps it
  at zero to show that the harness sees a miss
  ([performance](performance/README.md#injected-slowdowns)).
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
  `doc` (what `describe` and `help` serve), `params` (the accepted keys —
  typed, on the most-used methods, which makes the row the method's
  contract: [rpc.md](scripting/rpc.md#typed-contracts)), `flags`, and
  `returns` (the response keys, required for `list_*`, `get_*` and
  `check_*`).
- **The `flags` column is the gate**, stated on the row rather than kept in a
  list apart from the handler: `NeedsPart` (refuses unless a part tab is
  active), `NotInBatch` (reaches outside the document, or makes an edit no
  undo history holds, so a `batch` cannot roll it back), `Deferrable` (its result does not need fresh geometry, so a
  batch may defer the regeneration past it), `UiAction` (runs on the ImGui
  queue). An unknown method has no row and no flags, so a typo is answered
  "unknown method" rather than "switch to a part tab".
- An unknown request key is refused by the dispatcher against the row's
  `params`, before the handler runs; `RpcParams` catches the remainder (a
  listed key this call's path did not read), and a refused call's edits are
  rolled back ([rpc.md](scripting/rpc.md#a-refused-call-changes-nothing)).
- Gates over the tables: `tests/rpc_params_gate.py` (the `params` column
  against what each handler reads, both directions, via
  `tools/rpc_params_scan.py`), `tests/rpc_units_gate.py` (units, see
  [conventions.md](conventions.md#units-and-angles)),
  `tests/rpc_returns_gate.py`, `tests/rpc_names_gate.py`,
  `tests/rpc_gate_smoke.py`, `tests/refused_calls_smoke.py` and
  `tests/rpc_contracts_smoke.py`; the shared row parser is
  `tools/method_doc_scan.py`, and `tools/rpc_contract.py` reads a typed
  row's grammar as `src/remote/RpcContract.cpp` does.
- Changing a method's name, a refusal message, a `describe` row or `help()`
  text changes what `benchmarks/` measures; the benchmark is unfinished and
  gates nothing yet ([scripting/benchmark.md](scripting/benchmark.md#status)).

### Requests that wait

The pump (`ProcessPending`, `RemoteUiActions.cpp`) runs one request at a time
on the main thread. Three kinds span frames. A `ui_action`, a `screenshot`
and a `render_sheet` hold the WHOLE queue until they finish, because they
drive the frame itself. A request that would compute for seconds or minutes is
PARKED instead (`RemoteServer::Parked`, `remote::Parking` in
`RemoteParking.h`): the work goes to background jobs, `PollParked` asks it
once a tick, after `ReapBackgroundJobs`, whether it is done, and the answer
-- or a refusal, or the request put back at the head of the queue to run its
handler from the caches -- goes out then. Four domains park: CAM, on its
toolpath passes ([cam/README.md](cam/README.md#requests-that-wait));
drawings, on their views' projection, except on `--headless`, where the
synchronous handler holds no frame
([drawings](drawings/README.md#requests-that-wait)); an exact
`interference` of two named sides, on its boolean, in every mode
([inspect](inspect/README.md#threads)); a synchronous
`check_requirements` with something to measure, on the check's job
([requirements](requirements/README.md#checking-is-a-job)), and a
synchronous `design_sweep`, on its job
([design sweep](requirements/README.md#design-sweep)). The pump sets
`AppState::rpcConnection` while it asks, so a job a parked request starts
belongs to its client.
A parked request holds only its OWN connection's later requests, so a
pipelining client still gets its answers in order while other connections
(the MCP server, a second script) are served. While anything is parked the
pump reports activity, which keeps the idle pacer at full rate. A parked
request whose client disconnects is dropped; its passes land in the cache
anyway. A STUDY job is different: each connection is numbered (never reused,
unlike its fd), the pump sets `AppState::rpcConnection` around a dispatch,
and a study job a connection started is cancelled when it closes, unless it
was started with `outlive_client`
([studies as jobs](simulation/README.md#studies-as-jobs)).

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
- The named views and world axes (`core/Conventions.h`); the light rig's
  placement (`core/LightRig.h`).
- The GPU owners: `kRows` in `app/GpuOwners.cpp`, every object made on the
  device, built forward and torn down backward by the start, the exit and a
  rebuild alike, and `RegisterSceneRenderers()` beside it for the draw
  order ([rendering](rendering/README.md#rebuilding-the-device)); icons: `ui/Icons.cpp`; tool sessions: the tool-session registry
  that `ViewportInput.cpp` asks through `ToolSessionFor`. Each is checked by
  `test_conventions.cpp`.

## Threading

- The UI, regeneration and the renderers run on the main thread; the remote
  server has its own thread and hands work to the main loop; long operations
  run on workers and are applied by their `Poll…` (above).
- `core/Parallel.h` is the one parallel-for: flat batches of independent
  OpenCASCADE work, results collected per index and merged in order so a
  response is reproducible. `LOBOLE_SWEEP_THREADS` pins the thread count.
  A `For` nested in another's body gets its worker's share of the threads
  (`Workers()` is that share inside a `For`), so the other setups of a job
  inspected at once, each split into bands, divide the machine rather than
  multiply it.
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
- **Nothing on a clock may touch the disk or start a process on the
  frame.** A question the frame asks every few seconds — has the project
  changed on disk, what does the history say — is asked on a worker with
  copies of what it needs, and the answer is applied on a later frame, or
  discarded if the project changed meanwhile (`ui/DiskPoller.h`,
  docs/versioning/README.md#the-poll-runs-off-the-frame). On a 4 000-file
  project the walk and the `git` calls were a dropped frame every three
  seconds on Windows. A check that guards a write (the save's) stays
  synchronous: the write blocks anyway.

## App modes

- **Windowed** (`designer`): the GUI.
- **`--offscreen`**: the real renderer with no window and no display server;
  it has no panels, so the viewport is the whole framebuffer.
- **`--headless`** (`HeadlessMain.cpp`): no renderer at all; non-visual RPC
  work.
- `--remote <port>` serves JSON-RPC (`--remote 0` binds a kernel-chosen
  port); every instance announces itself in
  `~/.local/state/lobole/instances/<port>.json`. `ping` reports the port,
  the open project, the renderer and how start-up got to it (`renderer`,
  [the device ladder](rendering/README.md#the-device-ladder)), `safe_mode`,
  the OpenCASCADE version (`occ`) and the patches it carries
  (`occ_patches`), the kernel faults caught (`kernel_faults`), the build's
  optional `features`, and `binary_replaced`
  (Linux: the executable was rebuilt under a running instance).

## Crashes and stalls

- **A crash** (SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL; on Windows an
  unhandled SEH exception or `abort()`) is handled in `CrashHandler.cpp`, in
  an order that matters: it opens this session's crash file
  (`<state>/sessions/<started>-<pid>.crash`, a path copied into a fixed
  buffer at start-up), writes the signal or exception code, the RPC in
  flight (the watchdog's label) and an uncaught exception's message, then
  the **raw frames as module + offset**, from a module table recorded at
  start-up and again once the Vulkan driver is loaded. Nothing up to there
  allocates, because symbolizing can hang in a corrupt heap. Only then, best
  effort, the symbolized `std::stacktrace`, to the file and to stderr as it
  always went, and on Windows a minidump (`MiniDumpNormal`: threads,
  registers, stacks, no heap) beside the crash file. The default action is
  restored first and the signal re-raised last, so the process still dies
  and core-dumps as it would have. A stack overflow runs the handler on an
  alternate stack. `lobole --crash-test` crashes once the session marker
  exists and before there are any edits (or, windowed, a first frame: it is
  how a test makes a start that dies starting up); the
  `debug_crash {kind: segv|abort|exception|system_fault}` RPC crashes after
  them, and is refused unless the app was started with
  `LOBOLE_ALLOW_DEBUG_CRASH=1`; so is `debug_fault {kind}`, which fails the
  ways a handler can without crashing, for the hostile sweep's self-test
  ([rpc.md](scripting/rpc.md#the-hostile-sweep)). OpenCASCADE's own signal
  handler does not replace this one: `ArmOccSignalHandlers` puts it back
  after `OSD::SetSignal` and defers to OCC's only for a fault inside an OCC
  catch scope ([modeling](modeling/README.md#a-reshaping-feature-keeps-only-a-valid-solid)).
- **On Windows none of what `OSD::SetSignal` installs stays**
  (`src/cad/OccSignals.cpp`). Its `signal()` handlers go back to the
  default: mingw's start-up code runs `main` under an `__except` whose filter
  (`_gnu_exception_handler`) calls a `signal()` handler before the
  unhandled-exception filter can run, and OCC's `exit(1)`ed with no report.
  Its unhandled-exception filter goes back to the crash handler's, and the
  conversion of a fault inside an OCC catch scope is a vectored handler of
  ours, first in line on every thread, that acts only when
  `Standard_ErrorHandler::IsInTryBlock()` and long-jumps there with
  `Standard_Failure::Jump` (never for a stack overflow). OCC's own way held
  its signal mutex across the long jump: after one converted fault the next
  thread to arm OCC's handlers waited for good (`fuzz/cases/crash-153`
  hung in `open_project`). A **fault in code we ship** (everything in the
  executable's directory) is reported at first chance by a vectored handler
  registered last, and the process ends with the exception's code: nothing in
  GCC-built code can handle one, and on a `std::thread` the next frame up is
  mcfgthread's, which ends the process with `RaiseFailFastException` before
  any filter runs, so a worker's crash left no report. A fault in the
  system's own code (msvcrt's `memcpy` handed a bad pointer, a driver) is
  reported the same way when nothing in the system would catch it: the
  handler walks the faulting thread's frames from the fault's context as
  the dispatch will (`RtlLookupFunctionEntry`, `RtlVirtualUnwind` with
  `UNW_FLAG_EHANDLER`, which returns each frame's handler, and, for
  Wine's GCC-built DLLs, the TEB's handler list, which Wine's dispatch
  calls too), and reports when it reaches a frame of code we ship before
  any system frame with a handler. One that passes a handler first — the
  system probing memory under its own `__try`, as `IsBadReadPtr` does — or
  a walk that cannot go on (no frame within the limit, a stack pointer off
  the thread's stack) is left to the dispatch as before. The walk allocates
  nothing and takes no lock. `debug_crash {kind: "system_fault"}` (on a new
  thread, msvcrt's `memcpy` from an unmapped address) and
  `{kind: "system_probe"}` (on a new thread, `IsBadReadPtr` on one, which
  answers and lives) are the two cases, which `packaging/windows/smoke.sh`
  runs under Wine (`tools/release-smoke.py --system-faults`); on Linux the
  first is a SIGSEGV like any other and the second is refused. The crash
  file gives the
  address touched (`address: … (read)`), all ones for a general-protection
  fault. OCC's console handler, which swallowed Ctrl+C, is kept behind ours
  (below). `debug_fault {kind: "occ_fault"}` faults inside an OCC scope on
  the handler's thread and on a new one and answers what each became.
- **Every session leaves evidence only if it ends badly**
  (`src/core/io/CrashReports.{h,cpp}`). At start-up, in every mode, the app
  writes `<state>/sessions/<started>-<pid>.json` (version, build, mode, OS,
  log path, the open project and its autosave paths, rewritten when the
  project changes) and holds an **exclusive lock** on it for its whole life
  (`platform::FileLock`: `flock`, `LockFileEx`). A clean exit deletes it,
  and so do SIGTERM, SIGINT and SIGHUP, since a process asked to stop has
  not crashed. On Windows a console control handler does the same for
  Ctrl+C, Ctrl+Break, the console closing, logoff and shutdown: headless and
  offscreen are asked to stop (their `SIGINT` handler) and end the session
  the ordinary way, and the windowed app deletes the marker and ends.
  `TerminateProcess` cannot be caught, so there a session that ended without
  a report reads "ended from outside (Task Manager, taskkill /F) or power
  lost": running out of memory is a failed allocation, which the crash
  handler sees. A marker whose lock can be taken belongs to a process that is
  gone, however it died (a crash, the OOM killer, `kill -9`, a power cut); a
  pid check would not do, since pids are reused. The marker is locked under
  a temporary name and renamed into place, so another instance starting at
  the same moment never sees it unlocked. A windowed session's marker gains
  `first_frame` once its first frame is presented (`crash::NoteFirstFrame`,
  which also stamps `<state>/last-good-start` with that session's start in
  milliseconds): a session that dies without it died starting up.
- **The next start collects it**, before the first frame and in every mode
  (not under `--gui-tests`, which run on defaults): each unlocked marker
  becomes `<state>/crashes/<started>-<pid>/` with `report.txt` (what
  happened — "crashed: SIGSEGV", or "ended without a crash report: killed,
  out of memory, or power lost" — version, build, OS, mode, project, and the
  crash file), `report.json` (the same fields, for `crash_reports`),
  `session.log` (the last 2000 lines of that session's log, taken only from
  the state dir's `logs/`), `crash.dmp` where there is one, and `autosave/`
  (copies of the autosaves that session wrote: nothing older than it, each
  once — its unsaved slot is its own, named by its stem
  ([versioning](versioning/README.md#every-instance-has-its-own-unsaved-slot)) — and only a sidecar path — a damaged marker naming another file gets
  nothing copied). A marker's or a report's fields are read one by one: a
  damaged one loses that field, never the start. Twenty reports are kept,
  newest by the session's start in milliseconds (`crash::NewerReport`; a
  folder with no start recorded falls back to its name's second), then a
  session's "-n" for its later reports. The name is never compared as text:
  "-32" sorted ahead of "-308" in the same second, so a fresh Wine prefix's
  small pids listed the older report first and pruning could delete the
  newest. Two sessions started in the same millisecond fall to pid order,
  which is stable, not true. Nothing is sent
  anywhere, and `report.txt` says at the top that the folder may hold the
  user's design and file paths.
- **The windowed app says so**: a "Loböle closed unexpectedly" modal with
  the reason and Recover unsaved work (when autosaves were copied: the
  project through `ui::RecoverProject`, or the copied unsaved slot for a
  project never saved — a never-saved project of several documents cannot
  be reopened from its sidecars, and the modal says where they are), Open
  report folder (`platform::RevealInFileManager`), Copy report (`report.txt`
  to the clipboard) and Dismiss. `Help ▸ Crash reports…` opens the folder at
  any time; the read-only `crash_reports` RPC lists the reports.
- **The log is the activity journal**: every session's log also goes to
  `<state>/logs/<started>-<pid>.log` (`common/LogFile.h`; same pattern and
  flush-on-every-message as stdout, which is unchanged; the ten newest are
  kept, by name read as numbers (`logfile::NewerName`), never a live
  session's). Each edit is one info line as it happens:
  `edit:`/`undo:`/`redo:` with the command's description, `edit: assembly`
  (drawing, CAM job) for a snapshot, `rpc: <method>` by name only (a run of
  the same call that changed nothing is one line, so a client polling
  `get_state` does not bury the edits), and `open:`, `save:`, `tab:`,
  `tool: <type> started|committed|cancelled`. So a report's `session.log`
  says what the user was doing, and an autosave copy is the document they
  were doing it to.
- **`tools/symbolize-crash.py REPORT BINARY [LIBRARY…]`** names the raw
  frames with `addr2line` against the unstripped binary of the same build
  (the report's `Build:` line), reading a PE image base for a `.exe`, whose
  offsets are RVAs (the mingw `x86_64-w64-mingw32-addr2line`). A release
  build has function names and no line numbers; where addr2line finds no
  debug info to read (a PE always), the name comes from `nm`'s symbol
  table.
  `tests/crash_report_smoke.py` crashes a headless instance in a scratch
  state dir and checks each of the above.
- **A lost graphics device writes a report too**, while the process lives:
  `crash::ReportThisSession` makes the same folder a dead session's would
  (log tail, autosave copies). A loss rebuilt in place writes it under
  `graphics device recovered: …`, evidence the next start never offers as a
  notice ([rendering](rendering/README.md#rebuilding-the-device)); one that
  reaches the backstop writes `graphics device lost: …`, and the restart it
  asks for makes it the next start's notice
  ([rendering](rendering/README.md#a-lost-device)).
- **A stall** — the main thread not coming round its loop — is reported by
  the frame watchdog (`src/common/Watchdog.{h,cpp}`). Each loop (windowed,
  `--offscreen`, `--headless`) beats it once per iteration; a monitor thread
  that sees no beat for 3 s logs `main thread stalled 3.0 s (in <label>)`,
  the main thread's stack, and later `main thread resumed after N s`, once
  per stall. The label says what was running when that is known: the RPC
  being handled, a native file dialog (reported without a stack — the
  wait is the user), or the GPU: the frame loop's acquire and fence waits
  take a 0.1 s timeout in a loop under `waiting on the GPU: swapchain
  image` / `waiting on the GPU: frame fence`, so a GPU that hangs without
  resetting is named as such instead of a stack deep in a driver. Every RPC, `ping` included, runs on the main thread, so
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
- **On Windows** there is no signal to ask a thread for its stack: the
  watchdog suspends the main thread, reads its registers and walks the stack
  with `RtlVirtualUnwind`, as the crash handler walks a fault's, then resumes
  it. The walk takes no lock the stopped thread might hold: functions are
  found in each module's own `.pdata`, from a list of modules read and
  pinned before the suspend, and named after the resume, as module + RVA
  frames that `tools/symbolize-crash.py` names from the log.
- `LOBOLE_WATCHDOG_S` sets the threshold (default 3; 0 turns it off, as a
  debugger stopping the process is not a hang); a value that is not seconds
  is refused at start-up. `lobole --stall-test` stalls the loop for 4 s once,
  and `tests/watchdog_smoke.py` checks the stall line, the label, the stack and
  the resume.

### When the renderer cannot start

A start that cannot get a window and a renderer says why, in words, where
a desktop launch — which has no terminal — can read it, and exits; it does
not abort:

- **A startup failure is a value.** Every startup Vulkan call goes through
  `CheckStartup(step, VkResult)` (`src/app/GpuOwners.cpp`), which throws a
  `startup::StartupError` (`src/app/StartupReport.h`) carrying the step in
  words ("create the Vulkan instance", "set up the swapchain"), the
  result's NAME, the devices seen and each device tried and how it failed;
  `glfwInit` failing, a null window, no Vulkan loader, no device and the
  mesh renderer failing raise the same, quoting GLFW's last error where it
  has one. Within the [device ladder](rendering/README.md#the-device-ladder)
  a failed rung is caught and the next tried; only the last failure reaches
  `main`.
- **`main` ends it through one place**, `EndStartup` →
  `startup::ReportStartupFailure`: the message to stderr, to the session
  log, and to `<state>/last-startup-failure.txt`; then exit code **3** (no
  usable renderer; 1 is a bad flag or a taken port, 2 a refused environment
  variable) by `_Exit`, since what was half built is not torn down. The
  session marker is deleted first, as a clean exit's is: a start that said
  why has not crashed, and leaves no crash report. The message says what
  failed, what Vulkan answered, the devices, and what to try for that step:
  the driver and the Vulkan loader by distribution (`libvulkan1` +
  `mesa-vulkan-drivers`, `vulkan-loader`, `vulkan-icd-loader`), 3D
  acceleration in a VM, `LOBOLE_RENDERER=software`, `--safe-mode`, and
  `--offscreen` / `--headless` for the scripting surface without a window,
  with a link to
  [packaging's troubleshooting](packaging/README.md#when-it-will-not-start).
- **The user sees it.** A windowed start whose stderr is not a terminal
  shows it natively: `MessageBoxW` on Windows; on Linux `zenity --error`,
  else `kdialog --error`, else a desktop notification over D-Bus
  (`org.freedesktop.portal.Notification` inside the Flatpak,
  `org.freedesktop.Notifications` outside; libdbus, which the file-dialog
  portal already links) naming the text file, and with none of them the
  file is where it ends — never a hang. **`LOBOLE_NO_DIALOG=1`** suppresses
  every native box (this one and the lost device's): `tests/smoke_app.py`,
  `tools/xvfb-exec.sh`, `tools/gui-tests-xvfb.sh`,
  `tools/headless-preview.sh` and the CI workflows set it, and
  `--gui-tests` sets it for itself. `--offscreen` never shows one.
- **The next start says it once.** A windowed start finding
  `last-startup-failure.txt` newer than `last-good-start` shows it in a
  modal ("Loböle could not start last time"); `crash_reports` answers it as
  `last_startup_failure` in every mode.
- **Safe mode** follows a crash loop: two windowed starts in a row, newest
  first, that died before their first frame (`crash::EarlyDeathsInARow`,
  over the reports and `last-good-start`) make the third start
  [safe mode](ui/settings.md#safe-mode), as `--safe-mode` or
  `LOBOLE_SAFE_MODE=1` do.
- **The environment is refused, not guessed**: `LOBOLE_RENDERER`
  (`software|hardware`), `LOBOLE_SAFE_MODE` and `LOBOLE_NO_DIALOG` (`0|1`),
  and `LOBOLE_FAULT` (`device_create`, `swapchain`, `rebuild` (a device
  rebuild in place fails on every rung: the backstop),
  `renderer_init:<mesh|outline|sketch|underlay|field|stock|raytracing>`,
  honoured only with `LOBOLE_ALLOW_DEBUG_CRASH=1`) are each checked at start
  and anything else is exit 2 naming what is accepted. `LOBOLE_FAULT` is how
  `tests/startup_failure_smoke.py` makes the failures a machine cannot
  produce on demand; the ones it can are real (`VK_ICD_FILENAMES=/nonexistent`
  for no driver, `LOBOLE_RENDERER=software` on lavapipe), and a stand-in
  `zenity` on `PATH` shows the box was asked. The words, the parsers and the
  ladder's order are unit tests (`test_startup_report.cpp`).

## Tests

| suite | where | runs |
|---|---|---|
| C++ unit + integration | `tests/test_*.cpp` → `designer_tests` (Catch2; tags become ctest labels via `ADD_TAGS_AS_LABELS`) | headless |
| GPU topology solver | `tests/test_femgpu.cpp` → `femgpu_tests` | headless, needs a Vulkan device |
| python smokes and gates | `tests/*_smoke.py`, `tests/*_gate.py` | headless, against a launched app |
| agent blind spots | `tests/blindspots/` (one file per known agent mistake, run by `blindspot_smoke.py`; never delete one) | headless |
| GUI | `src/app/guitests/GuiTests<Domain>.cpp` (ImGui Test Engine) | `tools/gui-tests-xvfb.sh` |
| render pixels | `tests/render_pixel_smoke.py`, byte-exact against a per-device baseline | needs a GPU |
| golden corpus | `tests/golden/` and its `assembly/`, `job/` and `drawing/` directories, gated by `test_golden.cpp` | headless |

- `tests/smoke_app.py` is the only way a test launches the app
  (`launch_app()`); `tests/config_isolation_lint.py` fails any script that
  spawns the binary directly. It keeps the app's log, prints it on failure,
  and archives the log, traceback and a `reproduce.sh` under
  `$XDG_STATE_HOME/lobole/smoke-failures/`. Every launch sets
  `LOBOLE_EXIT_WITH_PARENT=<its pid>`, so the app gets SIGTERM when the
  test dies however it dies — a SIGKILLed smoke used to leave its instance
  serving nothing under init for hours (`src/app/ParentWatch.h`,
  `tests/exit_with_parent_smoke.py`). A launcher that is PID 1 (a
  container's entry point, as in `ci-local.sh --pixel`) sets nothing: the
  app refuses 1, which is also what an orphan's parent reads, and the
  kernel kills everything in a pid namespace when its init exits. It prints
  the app's pid; kill by that.
  The MCP server's autostart arms the same; `headless-preview.sh` and
  `dev-watch.sh` do not, because their instances are meant to outlive them.
- The GUI runner shards, shuffles by default and prints the seed that
  replays the run; a failing shard is archived under
  `$XDG_STATE_HOME/lobole/gui-failures/` with its exact filter and a
  `reproduce.sh`.
- `tests/test_actions.cpp` pins UI logic headlessly (read its header before
  adding to it); `tests/test_conventions.cpp` holds the source-scanning rules
  listed in [conventions.md](conventions.md#rules-the-source-is-scanned-for).
- Heavy tests carry the `[slow]` tag; `tools/test-fast.sh` is
  `tools/ctest -LE 'gui|slow'`. A tag is the ctest label, so a test's cost class
  lives on the test rather than in a list in `tools/`.
- Every test that needs a display carries the `gui` label, which is what the
  headless run excludes. `gui_smoke` runs the same cases as
  `tools/gui-tests-xvfb.sh`, serially; the python ones also carry `pygui`, so
  they run as `tools/ctest -L pygui` rather than by excluding `gui_smoke` by name.
- Four sweeps test from outside what no hand-picked case does:
  `tests/hostile_params_smoke.py` sends right-typed wrong values to every
  RPC key ([rpc.md](scripting/rpc.md#the-hostile-sweep)),
  `tools/fuzz.py` builds random designs and checks each against a rebuild
  from nothing, a reopen, undo and redo and the Boolean identities
  ([modeling](modeling/README.md#random-designs)),
  `tools/action_matrix.py` tries every ordinary action on every face, edge
  and body of a fixed corpus against a committed baseline
  ([modeling](modeling/README.md#action-matrix)), and
  `tests/test_reader_fuzz.cpp` hands every file reader damaged versions of
  the golden corpus and of fixtures for the readers outside documents, and
  each must refuse naming the file or load without losing content
  ([versioning](versioning/README.md#reader-fuzzing)).
- `src/render/`, `app/main.cpp` and `app/GpuOwners.cpp` have no unit tests;
  they are verified by
  the GUI suite, the pixel gate and screenshots. Low line coverage there is
  expected.

Occasional audits, not in CI: `tools/coverage.sh` (reports the `src/core`
functions nothing executed), `tools/bench.py` (the performance harness:
fixed workloads timed on this machine and compared against its own history,
[performance](performance/README.md); only its own working is tested, by
`bench_harness_smoke`), `tools/profile.sh` (sampling profiler),
`-DENABLE_SANITIZERS=ON` (ASan + UBSan, runtime options compiled in via
`src/common/SanitizerDefaults.cpp`) and `-DENABLE_TSAN=ON` (ThreadSanitizer,
a separate build; Netgen's own thread pool is suppressed in
`tools/tsan-suppressions.txt`, OpenCASCADE deliberately is not, since OCC is
called from our workers).

### CI on this machine

`tools/ci-local.sh` runs the CI jobs here, reading the package lists, the
container images and the excluded tests from `.github/workflows/ci.yml`. It
runs them as LANES, concurrently: one for the nix chain (Release build,
headless suite, GUI suite, the no-test-engine and no-CAM builds and their
checks, in that order) and one per stock-Ubuntu image (image, build, suite).
Starting everything at full strength would be wrong twice over, so two rules
bound it:

- **Builds share the compile budget.** A build's `-j` is `tools/build-jobs`
  divided by the lanes still running when it starts. Every build in flight
  took its share when at least as many lanes were alive, so the shares never
  add up to more than one build was allowed, and the last lane standing
  compiles at the full budget.
- **Suites take turns**, under a machine-wide `flock`
  (`/tmp/lobole-ci-local-suite.lock`, the pattern of the GUI runner's GPU
  lock), each at full `-j$(nproc)`, so no deadline-based test sees more load
  than it would alone. A container's build and suite are two `docker run`s
  for this reason. What overlaps is one lane's compile with another lane's
  suite.

Each lane writes its output to `.ci-local/lanes/<lane>.out`, printed whole
at the end in lane order, then a table of every step's seconds and how long
each suite queued for the lock. A failed build ends its own lane and no
other; the run exits non-zero if any step failed. An interrupted run stops
every lane's process group and the containers it named
(`ci-local-<pid>-<lane>-…`). `--serial` runs the lanes one after another
with their output live, in the order the script ran before it had lanes.

`tools/ci-local-selftest.sh` checks those promises in about a minute, on a
copy of the script whose builds, suites and containers are stubs that sleep:
the builds in flight stay within one build's `-j`, one suite runs at a time,
a failed lane leaves the others to finish, `--serial` runs one step at a
time, and a matrix entry's test exclusion and extra packages reach its
container whole (a planted entry carries spaces in both: a lane's command is
kept `printf %q`-quoted and `eval`ed, and stored unquoted it was word-split,
so 24.04's exclusion became apt packages). It is not in any suite, since only an edit to `ci-local.sh` can break
what it checks: run it after one.

## C++ usage

- C++23 (`CMAKE_CXX_STANDARD 23`); the default build type is Debug, which
  compiles at `-Og`. It has to name a level: nix's cc-wrapper puts `-O2` in
  front of every compile for its fortify hardening, and CMake's Debug passes
  none, so a Debug build under nix-shell was silently `-O2`. A level given in
  `CMAKE_CXX_FLAGS` (coverage's `-O0`) is left alone.
- **Logging goes through the `LOG_*` macros** (`common/Logger.h`, spdlog).
  The only direct stdout/stderr writes are `main.cpp`'s command-line usage
  and argument errors, which are program output and must not carry a log
  prefix or follow the log level (`-v`/`--verbose`, `-vv`).
- New fallible functions return `std::expected<T, std::string>`; the
  `bool f(..., std::string* error)` out-parameter form is legacy.
- `std::to_underlying` for enum-to-integer casts.
- Ranges views (`enumerate`, `zip`, `ranges::to`) are fine in cold code; keep
  explicit loops in the solver kernels (`core/fem/`, `femgpu/`), because the
  Debug build does not inline view wrappers.
- Nothing is named `near` or `far`: `windows.h` defines both as empty macros
  and the Windows cross-build would break (gated).
