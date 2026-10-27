# Import with progress and cancel, on every path

## Problem

File › Import runs a STEP or IGES read as an `import` job
([long operations](../ui/long-operations.md#import)), but every other import
runs synchronously on the main thread:

- `StartImport` itself sends `.stl`, `.obj` and `.brep` to the synchronous
  `ImportAsFeature`, on the belief that a mesh "reads in milliseconds";
- the `import_step` RPC (feature and `as_assembly` alike) calls
  `ImportAsFeature` / `ImportStepAsAssembly` inline;
- `--file` at startup, windowed (`src/app/main.cpp`), offscreen (the same
  file) and headless (`src/app/HeadlessMain.cpp`), calls `ImportAsFeature`
  before the loop starts — even for a STEP.

A mesh is the expensive case: every triangle becomes a face and the faces are
sewn (`BRepBuilderAPI_Sewing`), which grows faster than the triangle count.
A 100k-triangle OBJ took 560 s on the main thread through `import_step`, with
no progress, no cancel, and `SIGTERM` ignored until it finished. The mesh
readers take no progress or cancel at all.

`SIGTERM`/`SIGINT` on `--headless` and `--offscreen` only set a flag the
loop reads once per iteration, so a main thread inside one long call (an
import, a boolean in an RPC) never sees it; and after the loop, destroying
the job registry joins every worker, so a worker inside an uninterruptible
OpenCASCADE call holds the exit too. Nothing bounds either wait.

## Shape of the fix

**1. The mesh readers report and stop.** `CADKernel::LoadSTL` and
`LoadOBJStructured` take the `ImportProgress*` the STEP reader takes, with
two more phases, `Building` (faces from triangles) and `Sewing`. The STL read
(OCC ≥ 7.7: `RWStl::ReadFile` + `BRepBuilderAPI_MakeShapeOnMesh`, exactly what
`StlAPI_Reader` does there) and the sewing get a `Message_ProgressRange` from
the existing `ProgressBridge`, whose `UserBreak` is the job's cancel; the OBJ
face loop checks it every few thousand triangles. Output is unchanged
(same faces, same order), so element names do not move.

**2. One read for the feature and the job.** `ImportFeature::ReadModel`
(path, unit, progress) is what `Execute` does today before placing; `Execute`
calls it when it has no seed, and the job's worker calls it to make the seed.
The seed also carries each product's volume, which the worker computes, so
the main thread's remaining share (placement, bounding box, naming) is a few
per cent of the import.

**3. Every import is the `import` job.** `ImportRequest` carries path, unit,
`keep_origin`, up axis and turn, and a mode: `Auto` (File › Import: a STEP
with several instances is an assembly), `Feature`, `Assembly`
(`as_assembly`). `StartImport(state, request, error)` routes DXF/SVG to the
sketch import as now and everything else to the job; the job records the
part slot it was started in and lands there. The modal is titled "Importing"
for every format.

**4. The RPC waits off the frame.** `import_step` parks
(`remote::ParkImportRequest`, the `Parking` interface every waiting RPC
uses): parameters are checked first, the job starts owned by the caller's
connection (a disconnect cancels it), and the reply is built when the job
lands — the same keys as today, plus `mesh {triangles, expected_s}` for a
mesh. A cancel (`cancel_background_job {kind: "import"}`) answers the call
with an error saying it was cancelled; the part is unchanged. Inside a
`batch` it runs inline, as every parked RPC does.

**5. `--file` is the same job.** All three modes call `StartImport` and the
loop polls it; a project still opens as before. A client that connects
during a startup import sees it in `background_jobs` and may cancel it.

**6. Count before building; refuse past a measured limit.**
`interchange::CountMeshTriangles` reads a binary STL's header count (or
counts `facet`s of an ASCII one) and an OBJ's `f` records (n − 2 triangles a
polygon) — a scan, no geometry. `interchange::ExpectedMeshImportSeconds(n)`
is a power law fitted to the reads measured on this machine, in CPU seconds
(the table goes in docs/interchange/README.md). The limit is
the triangle count whose expected time is a minute (`kPatientImportSeconds`,
the wait a person accepts without being asked), solved from that same law —
no literal triangle count. Above it:

- `import_step` refuses, naming the count, the expected time and
  `allow_large_mesh: true`, which overrides it;
- File › Import asks: a modal with the count and time, *Import anyway* or
  *Cancel*;
- `--file` refuses in the log, naming `--allow-large-mesh`, which overrides;
- `add_feature` / `modify_feature` of an `Import` past the limit is refused
  naming `import_step`, which reads it off the frame.

Under the limit a mesh large enough to take a few seconds says what it will
cost in the job's label and status line.

**7. A stop always ends the process.** `stopsignal::Install` (new,
`src/app/StopSignal.{h,cpp}`) replaces the two flag handlers: the handler sets
the flag and writes one byte to a pipe; a watcher thread wakes on it, gives
the ordinary shutdown `kStopGraceSeconds` (3 s, under the 5 s
`tests/smoke_app.py` waits before `SIGKILL`), and if the process is still
alive — the main thread inside one call, or a join on a worker that cannot
stop — logs which, ends the crash session, removes the instance's registry
entry and `_exit`s with 128 + the signal. A second signal ends it at once.
The handlers are installed before `--file` is acted on.

## Files

- `src/cad/CADKernel.{h,cpp}`: phases, the mesh readers' progress, the
  product volume on `ImportedShape`.
- `src/core/io/MeshImportCost.{h,cpp}` (new): counting, the cost law, the
  limit, the sentence that describes a cost.
- `src/core/Feature.{h,cpp}`: `ImportFeature::ReadModel`, the seeded volume.
- `src/ui/ImportJob.{h,cpp}`, `src/ui/ImportActions.cpp`, `src/ui/Toolbar.cpp`
  (the modal and the ask), `src/ui/FileDialogs.cpp`, `src/ui/AppState.h`.
- `src/remote/RemoteProject.cpp` (`import_step`, its park and describe row),
  `src/remote/RemoteUiActions.cpp` (the park call),
  `src/remote/RemoteModeling.cpp` (`add_feature`/`modify_feature` refusal).
- `src/app/main.cpp`, `src/app/HeadlessMain.cpp`, `src/app/StopSignal.{h,cpp}`.

## Verification

- Catch2: counting (binary, ASCII, OBJ polygons, a lying header); the limit is
  where the law reaches the patient time; a cancelled mesh read returns false
  and leaves nothing; a seeded `ImportFeature` matches an unseeded one (bodies,
  names, volume) for STL and OBJ.
- `tests/import_job_smoke.py` (new): `import_step` of a generated mesh is
  listed in `background_jobs` with a label naming its triangles, its fraction
  moves, another connection's `cancel_background_job` answers the call with a
  cancel and leaves the part empty; a small mesh answers with `mesh`; past
  the limit the call is refused naming the count, the time and
  `allow_large_mesh`, and with it the job starts; `--file` of a mesh is a
  cancellable job too.
- `tests/stop_signal_smoke.py` (new): `SIGTERM` during an `import_step` job,
  during a `batch` import (the main thread busy) and during a `--file` import
  each end the process within the grace plus a margin.
- GUI: `import/a_mesh_import_runs_in_background` (the modal, its cancel) and
  `import/a_large_mesh_asks_first`.
- `tests/startup_file_smoke.py` waits for the startup import to land.
- `tools/test-fast.sh`, the headless suite, the GUI suite and
  `tools/ctest -L pygui -j1`.
