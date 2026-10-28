# Heavy features build off the frame, and one cancel rule

## The problem

Regeneration runs on the main thread on every path (an RPC, a GUI edit, an
undo, an open), and stays there: checkpoints, undo and every RPC handler
rest on the document being mutated on one thread. Most features build in
milliseconds. A few do not: Delete Face on a faceted download (the Voron
exhaust fan grill: 916 faces deleted on a 2937-face refined STL) spends
about 14 s in `BRepAlgoAPI_Defeaturing`, and the frame is held for all of
it, twice for an `add_feature` (the trial copy, then the part). The organic
meshes plan's Cradle is the next one, at minutes.

Separately, every cancellable job kind documents its own cancel, and they
read as eighteen rules rather than one.

## The shape of the fix

### Which features are heavy

A feature declares that it MAY build off the frame
(`Feature::MayBuildOffFrame()`, default false) and estimates its own cost on
the bodies it would act on (`Feature::BuildCostS(input)`, seconds). It goes
to a job when the estimate reaches `heavy::kOffFrameS` (0.25 s: fifteen
frames). Delete Face estimates from the faces it deletes, at the measured
rate of the fan grill (14 s / 916 faces); one hole's two faces stay inline.
No list of feature names exists anywhere.

### How it runs: the regeneration stops at it

`Document::Regenerate` is unchanged except at one point. A document that
builds heavy features off the frame (`SetBuildsOffFrame`, set by the app on
every part it holds, and by `ReadProject` on the parts it loads) reaching a
heavy feature that misses its regen cache:

1. looks it up in the process-wide result store (`heavy::Store`, keyed by
   the feature's chain hash: everything up to and including it). A hit is
   installed like a regen-cache hit: the copy, the trial, the undo, the
   reopen in the same session build nothing;
2. otherwise records a build request (the feature's index and chain hash, a
   clone of it and of every feature it references, its input bodies, the
   symbols, configuration, sheet-metal and print context) and STOPS: the
   feature is `building`, everything after it `waiting`, the bodies are those
   before it. Regenerate returns as it does for a failure-free run, the tip
   hash is withheld (no bake, no copy reuses a half-built list).

A document that does not opt in (unit tests, a design sweep's worker, a
merge's or diff's scratch load, every transient copy) builds the feature
inline, as before, and files the result in the store.

The UI's one poll (`PollBackgroundJobs` -> `PollFeatureBuilds`) sees a part
with a request and starts a `feature` job for it, one per part:

- on the main thread, the input bodies are deep-copied
  (`CADShape::DetachedUnlocated`): OpenCASCADE writes to a boolean's
  arguments and the frame meshes the document's shapes;
- the worker executes the cloned feature on those copies, exactly the
  Execute a regeneration would run, with the job's cancel and fraction
  reachable through `heavy::Interrupt` (thread-local), which the direct-edit
  builders' progress indicator already polls for its deadline;
- `finish` files the result in the store and in the part's regen cache,
  and regenerates the part, which now hits it and builds what follows (a
  second heavy feature downstream is the next job). Untouched bodies keep
  the document's own shapes, so the renderers do not re-tessellate them.

### An edit made while one builds

**Restart if it changes what the feature builds from; otherwise wait.**
The request is a content key. An edit upstream of (or to) the building
feature moves its chain hash; the running job's result could only ever be a
cache entry nobody reads, so it is cancelled (superseded, not a user cancel)
and a job for the new key starts. An edit downstream leaves the key alone;
the job keeps running and the edit is built when it lands. An undo is an
edit like any other. A superseded job's thread finishes its current progress
step and is dropped; a job that completed is kept in the store, so a redo
back to it is instant.

GUI edits apply at once (the viewport keeps drawing). RPC requests that read
the part's geometry -- every `NeedsPart` method that is not `Deferrable` --
wait in the queue until the active part has nothing building, so no answer
is built from the bodies before the feature; `get_state`,
`background_jobs` and `cancel_background_job` answer at once and say what
is building.

### What a mutating RPC returns

It waits off the frame, the way `import_step` does. The pump runs the
handler as today; when the call leaves its part with a build pending, the
reply is parked (`remote::BuildWait`) instead of sent. When the build lands
the reply's `regen` and `digest` are recomputed against the part as built,
and sent. A direct edit added or modified by the call whose own build fails
is refused then, the call rolled back, exactly as the synchronous trial
refused it (the trial is skipped for a heavy edit: it would be the same
build on the frame). The watchdog therefore never sees the build: the
frame's share is the deep copy.

### Cancel

The rule, which every job kind follows: **a cancel never applies a
half-finished change to the model; results already paid for are kept as
results.** For a feature build:

- **Cancelling an edit's build undoes the edit.** The build belongs to an
  edit when that edit's push onto the part's undo stack is what made it, and
  the edit is still the last one on that stack. Cancelling it rolls the edit
  back and discards it (`CommandStack::RollbackSince`, what a refused call
  does): no undo step, the same features, the same saved bytes. A parked RPC
  that made it is answered with the refusal "cancelled; the call was rolled
  back".
- **Cancelling the build of an existing feature** (an open that rebuilt, an
  undo, an upstream edit no longer last, a rebuild asked for) leaves it
  `not built (cancelled)`, everything after it `waiting`, in the tree and in
  `regen` (`regen.unbuilt`, `ok: false`). The mark is keyed on the chain
  hash, so any edit that changes its inputs builds it again, and
  `build_feature {feature_id}` (the tree's **Build** item) clears it.

The job's doc-side cancel is applied by the poll the frame after `cancel`
is set, however it was set (the tool window's button, the tree,
`cancel_background_job`, a disconnect of the RPC client that made it), so
every path gets the same outcome.

docs/ui/long-operations.md gets one section, "The cancel rule", with the
principle and one table: each kind, what a cancel keeps, and the test that
exercises it. Kinds that do not follow the principle are reconciled or the
row says why.

### Undo ordering

The edit is committed to the undo stack when it is made, before its build;
undo order is therefore the order of edits, whatever is building. Undoing a
building edit supersedes its build.

### Saving while a feature builds

The part's recipe saves; its geometry is not baked (the tip hash is
withheld), so a reopen in the same session hits the store and a reopen
after a restart rebuilds it off the frame.

## Files

- core: `src/core/HeavyBuild.{h,cpp}` (request, result, store, interrupt,
  threshold), `Feature.h` (two virtuals), `Document.{h,cpp}` (opt-in, build
  states, request, adopt, cancelled marks), `DirectEdit.cpp` (the deadline
  reads the interrupt and reports progress), `DeleteFaceFeature.{h,cpp}`
  (declares and estimates).
- ui: `src/ui/FeatureBuildJob.{h,cpp}` (the `feature` kind, poll, cancel,
  attribution), `BackgroundJobs.cpp` (polls it), `FeatureTreePanel.cpp`
  (building/cancelled rows, Build), `ProjectActions.cpp` (ReadProject opts
  in).
- remote: `RemoteUiActions.cpp` (gate and `BuildWait`), `RemoteModeling.cpp`
  (`build_feature`, the trial skip), `RemoteHelpers.h` (`regen.unbuilt`),
  `RemoteDispatch.cpp` (rows, get_state's per-feature `build`).
- tests: core (`tests/test_heavy_build.cpp`: deferral, store hits, restart,
  cancel marks, byte-identical revert); GUI (`direct/heavy_delete_face_*`:
  the frame keeps running while it builds, cancel of the edit leaves no
  undo step, cancel of an existing build marks it); smoke
  (`tests/heavy_feature_smoke.py`: add_feature parks and answers its regen,
  cancel refuses and changes nothing, open with a rebuild + cancel shows it
  cancelled, build_feature rebuilds); `test_conventions.cpp` ("every job
  kind states its cancel and a test of it"). A test knob forces small edits
  off the frame and slows the worker (`LOBOLE_TEST_HEAVY_*`), as the open
  job's `delayEachMs` does.

## Verification

- The fan grill (`scratch/imported-edits/a1_adapt.py`'s steps on the Voron
  STL): the longest main-thread hold during the Delete Face, measured by a
  second connection pinging, before and after.
- Every suite: headless, GUI, pygui.
