# Hostile parameters and random designs

## The problem

Every RPC test here sends a value somebody thought of. The gates hold the
`MethodDoc` rows honest (`rpc_params_gate`, `rpc_units_gate`), and
`refused_calls_smoke` proves a refusal changes nothing: every method with an
unknown key, every method with every key set to one wrong-typed object, and
the 21 calls that reach outside the document, each from a valid baseline. What
nothing sends is a value that is the right type and still wrong: `1e400`
(parsed as infinity), `1e308`, `-0.0`, `2^63`, `-1` for a count, a 1 MB
string, an empty array where a point goes, nesting 10 000 deep. Those values
are the ones that reach a handler and the kernel:

- **A typed row stops only some of them.** 83 of the 314 rows are typed
  contracts (docs/scripting/rpc.md#typed-contracts), and the validator does
  refuse non-finite numbers. The other 231 rows hand the raw JSON to their
  handler, and `p.at("count").get<int>()` on `1e308` is undefined behaviour,
  which a normal build reads as `INT_MIN`.
- **`add_feature`'s `feature` is typed as `object`**, so every field of every
  feature type goes unchecked to `FeatureFromJson` and then to OpenCASCADE. A
  zero-length direction becomes a `gp_Dir` that throws
  `Standard_ConstructionError`.
- **The dispatcher hides internal errors.** The catch ladder in
  `ProcessPending` (`RemoteUiActions.cpp`) turns every `std::exception` and the
  last-resort `Standard_Failure` into an ordinary refusal. That keeps the app
  alive, but `vector::_M_range_check`, `stoi` or a `gp_Dir` throw arrives
  looking like a deliberate "no". A test cannot tell a handler that checked
  its input from one that fell over.
- **A non-finite number in an answer is silently replaced.** nlohmann dumps
  NaN and infinity as `null`, so a NaN volume reaches the caller as a missing
  value: a default put in where the true answer should be, the thing CLAUDE.md's "Refuse, don't substitute" rule forbids.

The geometry has the same gap. Every boolean test was built by a person
who knew the case: `test_boolean_volume.cpp`'s dogfood slot, the spokes
8 µm apart. The kernel-side check (docs/modeling/README.md#a-boolean-is-checked-before-it-is-kept)
refuses a cut that adds material or a union that loses it, and the fuzzy
retry handles near-coincidence. What no test does is generate thousands of
the ordinary designs that cause those cases: faces that coincide by
accident, a cylinder tangent to a box face, a fillet running into a shell,
patterns landing on seams. Nor does anything check, from outside, that the
incremental rebuild, a rebuild from nothing, a save and reopen, and undo/redo
all produce the same solid.

SolidCraft (`crates/engine/src/tests.rs`, `hostile_params_never_panic`;
`docs/robustness.md`) does both, and it found bugs a hand-picked case never
would: about half its random designs failed at first. This plan builds both
for Loböle on what is already here: the `describe` rows, `batch`, `--record`
sessions, `digest` on every mutating answer, `check_geometry`, and
`smoke_app`'s isolated headless instances.

## The shape of it

### 1. The server says when it fell over

Two changes, so the sweeps below have a precise definition of failure. Both
are also useful to any caller.

**An internal error is labelled as one.** The `ProcessPending` catch ladder
puts `diagnostic.code: "internal"` (and the exception's type name in
`diagnostic.observed`) on a refusal caused by:

- `std::logic_error` and its subclasses (`out_of_range`, `invalid_argument`,
  `length_error`), where the handler indexed or parsed past what it had
  checked;
- `std::bad_alloc`, `bad_optional_access`, `bad_variant_access` and
  `bad_cast`;
- nlohmann's `out_of_range`, `invalid_iterator` and `other_error` (only
  `type_error` stays the designed wrong-type path, `TypeErrorMessage`);
- a `Standard_Failure` that reached the last line of defence.

The sentence is unchanged, and the call is still rolled back. Deliberate
refusals already throw `std::runtime_error` (1312 sites). The exception is
`PrintCheck.cpp`, which throws `std::invalid_argument` at a caller
("no bodies to check"); it switches to `runtime_error`, so the rule covers
the whole source. docs/scripting/rpc.md#structured-errors gains the code.

**A non-finite number is reported, not hidden.** `RemoteServer::Send` walks
the message before dumping it. Each non-finite double is still written as
`null`, since JSON has no other spelling, but the response gains a top-level
`nonfinite: ["/result/bodies/0/volume", ...]` listing the JSON pointers, and
the app logs a warning naming the method. The call is not turned into an error
after the fact: a mutating call has already happened, and an error must mean
it did not. `designer_remote.py` raises on `nonfinite` in strict mode, which
the smokes use, and warns otherwise.

### 2. The hostile sweep: every method, every key, every hostile value

**It runs in Python against headless instances, not in-process.** The rules
under test live in `ProcessPending`: the unknown-key check, the contract
validator, the `EditMarks` rollback and the catch ladder. Calling handlers
from `designer_tests` would need a socket-less seam into that function and a
full `AppState`, and a crash would take down the test process with no record
of the case. Through a socket, a crash is a closed connection with
the case in hand, and a hang is a timeout. The app's stack is in its
log (the crash handler, or the watchdog's stall report, which headless also
beats). The cost is a round trip, about 2-3 ms for a refusal, and it is
handled by sharding.

`tests/hostile_params_smoke.py <binary> --shard K/N [--full]` sets things up
as follows:

- **The fixture** is `refused_calls_smoke`'s: a part with two boxes and a
  sketch, an assembly with an occurrence, a drawing, and a CAM job when
  `ping` reports `cam`. The part also gets a representative feature set:
  Sketch, Extrude add and cut, Revolve, Box, Cylinder, Fillet, Chamfer,
  Hole, Shell, linear and circular Pattern, Boolean and Draft. `World`,
  `tree` and `changed` move out of `refused_calls_smoke.py` into
  `tests/smoke_world.py`, which both tests import.
- **The methods** come from `describe`, sharded by a stable hash of the
  method name, so a shard's contents do not move when methods are added
  elsewhere. Each shard is its own ctest, `hostile_params_<k>`, with its own
  instance, so ctest schedules the shards like any other tests.
- **The values.** The compact set, which runs by default, is `1e400`, `1e308`,
  `-1`, `0`, `-0.0`, `2^64`, `""`, a 64 KB string, `[]`, `{}`, `null` and
  nesting 64 deep. `--full`
  adds `-1e308`, `1e-308`, `2^53+1`, `-2^63`, `1.5` for an int, `"nope"`,
  `"1/0"`, `"(((1"`, the strings `"NaN"` and `"inf"`, an emoji, a lone
  surrogate, `"\u0000"`, a 1 MB string, `[1e308, -1e308]`, `[0, 0, 0]`,
  `[[0, 0], [0, 0]]`, `[{}]`, `{"$ref": 1}`, nesting 10 000 deep, index
  `999999`, and fixture ids of the wrong kind (an occurrence id where a
  feature id goes, a sketch id where a body goes). A typed row is also given
  the extremes its own contract accepts. `int>=0` gets `0`, `2^31` and
  `2^63-1`; `number>0` gets `1e-300` and `1e308`; an enum gets each literal
  with one character changed. The validator lets these through, so they test
  the handler and not the validator.
- **Path keys stay inside the scratch directory.** `path`, `missing_path`
  and the rest get `""`, `"."`, `"/"`, a directory, a file under a
  directory that does not exist, a 5000-character name and a NUL byte, all
  relative to the app's cwd or under the scratch dir. Nothing names `$HOME`
  or a system path. `smoke_app` already isolates `XDG_STATE_HOME` and
  `XDG_CONFIG_HOME`.
- **Every method and every key from the row, one at a time.** Each call is
  made two ways:
  1. **alone**: `{key: value}`;
  2. **in a valid baseline**: `{**baseline, key: value}`, wherever the method
     has one. The 21 baselines `refused_calls_smoke` already holds come
     first. The others are built for typed rows from `request_schema` (the
     `sample` that `rpc_contracts_smoke` has), with id-shaped keys
     (`feature_id`, `sketch_id`, `body_name`, `part`, `id`, `setup`, `view`)
     filled from the fixture. A baseline counts only if the unmodified call
     is accepted, as `refused_calls_smoke` already checks. The test prints
     how many methods had one ("hostile values reached N of M
     handlers through a valid baseline"), so the coverage that is missing
     shows in the output.
- **Feature fields.** `modify_feature` on each fixture feature, each field
  of its spec set to each value, then `undo`. `add_feature` gets the same
  for sketch entities: a zero-length line, a zero-radius circle, coincident
  points, coordinates at `1e308`. The test prints which of `describe`'s
  `feature_types` the fixture covers.
- **The whole value as params**, for every method and value. This is already
  refused as "params must be an object", and it is cheap.
- **`batch`**: hostile `calls` (a non-array, 10 000 calls, a call without a
  method) and hostile `$ref`s (`0`, `-1`, `99999`, `"x"`, one pointing
  forward, one with `key` naming nothing).
- **Raw lines** on a bare socket, outside the client: `NaN`, `Infinity`,
  invalid UTF-8, a line of 8 MB, a valid request with no `id`, and two
  requests on one line. The answer to each must be a `parse:` error or a
  normal answer, and the connection and the instance must stay up.

**A failure** is any of:

1. the instance died (the connection closed or the pid is gone): a crash,
   reported with the case and the app log's stack;
2. no answer within 30 s: a hang. The instance is killed **by its pid**
   (`LaunchedApp.pid`, never `pkill -f`) and the watchdog's stall line is
   quoted. A method the row documents as long-running (a study, a toolpath)
   is called with `async: true` in its baseline, so it does not hang the
   sweep, and its job is cancelled by the cleanup;
3. an answer that is not one JSON object with the request's `id` and
   exactly one of `result` or `error`;
4. `diagnostic.code == "internal"` (section 1);
5. a `nonfinite` field on any answer;
6. a refusal that changed the world. Every call gets the cheap check:
   `validation_revision` (covers every tab), undo and redo depth per tab,
   `list_parts`, and the non-CAM `background_jobs`. A `NotInBatch` method also
   gets `World.cheap()`'s file trees and settings. A full `World` comparison
   runs at the end of each shard;
7. a refusal without its measured condition: an empty message, or one that
   says "(see log)" (docs/scripting/rpc.md#refusals).

An accepted hostile call is not a failure: `1e308` is a legal depth. The
world is then put back. The test undoes until each tab's depth is what it
was, switches back to the tab, and cancels the jobs it started. If the tab
list or the scratch files changed, it rebuilds the fixture, the way
`refused_calls_smoke` does. Each shard prints its counts: calls, accepted,
refused with a dispatcher diagnostic, refused by the handler, and resets.

**Cost.** The compact set across all shards is about 15 000 calls. N is
chosen so each shard runs in under 30 s on a loaded machine, and the shards
print their times so N can be adjusted. `hostile_params_<k>` run in the
headless suite (and so in CI), excluded from `test-fast.sh` only if they push
it past its budget. `--full` is a separate set of ctests,
`hostile_params_full_<k>`, labelled `slow`. `tools/ci-local.sh --asan` runs
them as part of the whole suite, which is where UBSan reports a `1e308`
narrowed to `int` that the normal build reads silently as `INT_MIN`.

### 3. Random designs

`tools/fuzz_design.py` contains the generator, the checks and the minimiser;
`tools/fuzz.py` is the command line around them.

**A design** is a seeded sequence of RPC calls, chosen by
`random.Random(seed)` against the current state (`list_elements` sorted by
name, so the same build makes the same choices). It starts from one base:

- a Box on `Top`;
- a sketched profile (rectangle, polygon, or a rectangle closed by an arc,
  sometimes notched), extruded;
- a Revolve of a section, a partial or full turn, sometimes touching the
  axis.

Then up to eight steps from:

- an Extrude `add`, `cut` or `new_body` of a sketch on one of the body's own
  planar faces or on a base plane;
- a Box, Cylinder or Sphere `on` a face;
- Fillet or Chamfer on one to three edges;
- Shell with one open face;
- Hole (simple, blind, drilled) on a planar face;
- a linear or circular Pattern of the last feature, or a mirror of it;
- Draft;
- a Boolean between two bodies.

Placements snap to a 2.5 mm grid and often to the body's own faces and
edges, so coincident faces, tangencies and seams on the grid come up as
often as they do in real parts. Sizes are drawn relative to the body's bbox,
not as literals, so a fillet radius is a fraction of the shortest edge.
Steps refer to earlier steps as `{"$step": k}`, resolved at run time to the
id step k returned. That is what lets the minimiser drop a step without
renumbering the others.

**After every step**, read from the call's own answer (`regen`, `digest`)
plus one `check_geometry`:

- the answer is well formed and has no `nonfinite` field;
- `regen.ok`, or each failed feature carries a message stating the measured
  condition;
- every body has a finite volume above zero and at least one solid;
- `check_geometry` is `valid` (docs/inspect/README.md#geometry-checks);
- **the volume moves the right way.** A `cut`, a Hole or a Shell never adds
  (`digest.delta.volume <= tol`). An `add` never removes. A `new_body` adds
  exactly one body and leaves the others' volumes unchanged. The tolerance is
  1e-6 of the part's volume;
- **a cut stays inside what it cut**: the result's bbox lies within the
  previous one.

**At the end of each design:**

- **incremental equals from scratch.** The accepted steps are replayed
  into a new part tab as one `batch` (one regeneration, from nothing), and
  every body's volume, bbox and solid count match the incremental part to
  1e-9 relative;
- **save and reopen**: `save_project` to the scratch dir, `open_project`,
  and the same digest. This exercises the baked geometry and the format;
- **undo and redo**: undo to the start leaves no bodies, and redo to the end
  gives the same digest again.

**The Boolean identity**, three times per seed. Two random primitives, made
as `new_body` on the same grid (boxes, cylinders, spheres; overlapping,
touching, sharing a face). The test measures `V(A)` and `V(B)`, then runs a
`Boolean` union, intersect and subtract one at a time, undoing between them,
and requires `V(A∪B) + V(A∩B) = V(A) + V(B)` and
`V(A−B) = V(A) − V(A∩B)` within 0.2 %. An intersect refused as an empty
result counts as `V(A∩B) = 0`, and the union must then equal
`V(A) + V(B)`. The kernel-side check judges each boolean by its own bounds.
The identity ties three booleans together, so it catches a result inside
every bound that is still wrong.

**What each seed comes out as:**

| outcome | meaning | a failure? |
|---|---|---|
| `clean` | every step built and every check held | no |
| `limit` | a Fillet, Chamfer, Shell, Draft or Hole failed to build, with a message; the step is undone and the design goes on | no, counted by message |
| `generator` | the RPC refused a call the generator made: a bug in the generator | fails the smoke, not counted as a kernel finding |
| `boolean_failed` | an add, cut or `Boolean` failed to build, including one the boolean check refused honestly | yes: these are ordinary inputs, and a refusal here is the kernel not coping |
| `invalid` | an invalid body, a non-finite or non-positive volume | yes |
| `wrong_way` | the volume or bbox moved the wrong way | yes |
| `identity` | the Boolean identity broke | yes |
| `rebuild`, `reopen`, `undo` | the end-of-design checks disagree | yes |
| `message` | a failure with no measured condition | yes |
| `internal`, `nonfinite` | section 1 | yes |
| `crash`, `hang` | the instance died, or a step took longer than the timeout | yes |
| `leak` | failed in a reused instance, clean in a fresh one | yes: state left behind by the previous seed |

A message is reduced to its **gist** for grouping: the class plus the first
60 characters with digits removed, as in SolidCraft. The report groups
seeds by gist, so 40 seeds with one cause read as one line.

**Isolation.** Each worker owns one headless instance (`smoke_app`, scratch
XDG dirs, `DESIGNER_EXIT_WITH_PARENT`) and runs seeds on it one after
another, with `new_project` between them. An instance is replaced every 50
seeds and after any failure. A seed that fails is run again in a fresh
instance before it is reported; if it is clean there it is `leak`, and the
seed before it is named. A worker runs one design at a time, so no two
booleans share a body (the CLAUDE.md OCC rule). If a crash is a heap
corruption (`malloc(): unaligned tcache chunk`), the first thing to suspect
is a boolean that modified an operand which was also in use somewhere else,
and `fuzz.py one <seed> --asan` replays the seed against the sanitized app
(`tools/run-sanitized-app.sh`).

### 4. Minimising a failure

`fuzz.py one <seed> --minimise` cuts the design down to the steps the
failure needs. Each candidate is replayed into a fresh project (a fresh
instance when the class is `crash` or `hang`):

1. **Drop steps.** Drop one step, together with every step that refers to
   it through `$step`. Keep the shorter design if its last step still fails
   with the same class and gist. Repeat until no step can be dropped.
2. **Round the numbers.** Move each number to a coarser grid (10 mm, 5 mm,
   then integers), and keep each change that preserves the failure.

The result is replayed once more with `--record`, so **the minimised case
is a `--record` session**: literal ids (deterministic in a fresh project),
playable by hand with `tools/designer_replay.py`
(docs/scripting/README.md#recording-and-replay). Its first line is a header,
`{"fuzz": {seed, class, gist, generator}}`, which `designer_replay.py`
learns to skip.

### 5. A found case becomes a test

- **The fix lands with its case** in `tests/fuzz/cases/<class>-<seed>.jsonl`.
  `fuzz_corpus_smoke` replays every case into one headless instance, with
  `new_project` between cases, and runs section 3's checks after each step
  and at the end. A case passes when every check holds: the fixed bug no
  longer happens.
- **A fix in `src/core`** also gets a focused C++ case when the session
  reduces to feature JSON, in the style of `test_boolean_volume.cpp`, which
  is built that way. That case names the mechanism; the corpus case only
  shows the failure is gone.
- **A hostile-sweep finding** is pinned by its exact call in the shard's
  output. The sweep runs on every build, so the sweep itself is the
  regression test. A finding the sweep reaches only through `--full` adds
  its value to the compact set.
- **A class that is not fixed in the same round** goes into
  `docs/known-bugs.md` with its minimised case path. The case stays out of
  the corpus until it is fixed.

### 6. How it runs

- `tools/fuzz.py run --from 0 --count 1000 --jobs N --timeout 90 --out
  scratch/fuzz/<stamp>/` writes one JSON file per seed and a `report.md`:
  the outcome table, the gists by count, and the slowest seeds. `--jobs`
  defaults from free memory, as `tools/build-jobs` does, and never from the
  core count. The output is in untracked `scratch/`, and nothing in it is
  committed except the cases that become tests.
- `tools/fuzz.py one <seed> [--minimise] [--asan] [--keep]` runs one seed;
  `--keep` leaves the instance up on a scratch port for driving by hand.
- `tools/fuzz.py replay <case.jsonl>` runs a case with the checks applied.
- A run of 1000 seeds is longer than the 600 s tool cap. An agent starts it
  with `run_in_background` and waits for the completion notice. The tool
  waits on its workers by pid and kills an instance only by its pid.
- **A failure that depends on timing** is replayed under load with
  `tools/with-cpu-load.sh N -- tools/fuzz.py one <seed>`, never with bare
  `while :; do :; done &` burners.
- **The CI-sized variant** is `fuzz_seeds_smoke`, in the headless suite. It
  runs a fixed list of seeds that must stay clean, sized to about 30 s, and
  fails on any failure class and on `generator`. A seed that fails when this
  lands is fixed in this round, or recorded in `known-bugs.md` and left out
  of the list. A change to the generator changes what each seed builds, so
  the list is reviewed only together with such a change. The report prints
  the generator version.
- `-DENABLE_CAM=OFF` changes nothing here except the hostile sweep's fixture,
  which builds the job only when `ping` reports `cam`.

## Files

- `src/remote/RemoteUiActions.cpp`: the `internal` code in the catch ladder.
- `src/remote/RemoteSession.cpp`: `Send` reports `nonfinite`.
- `src/core/PrintCheck.cpp`: its caller-facing refusals become
  `std::runtime_error`.
- `tools/designer_remote.py`: strict mode for `nonfinite`.
- `tools/designer_replay.py`: skips the case header line.
- `tests/smoke_world.py` (new): `World`, `tree` and `changed`, moved out of
  `tests/refused_calls_smoke.py`, which imports them.
- `tests/hostile_params_smoke.py` (new).
- `tools/fuzz_design.py`, `tools/fuzz.py` (new).
- `tests/fuzz_seeds_smoke.py`, `tests/fuzz_corpus_smoke.py`,
  `tests/fuzz/cases/` (new).
- `tests/CMakeLists.txt`: the shards, the `slow` full shards, the two fuzz
  smokes.
- Docs: in `docs/scripting/rpc.md`, the `internal` code under Structured
  errors, `nonfinite` under What a response carries, and the hostile sweep
  next to `refused_calls_smoke` under A refused call changes nothing.
  `docs/modeling/README.md` gets random designs, their invariants and the
  corpus, under how it is tested. `docs/architecture.md`'s tests section
  points at both. CLAUDE.md's Tests table gets one row for `tools/fuzz.py`.
  This plan is then deleted.

## Verification

- **Section 1, by construction.** A unit case per exception family through
  the catch ladder: `std::out_of_range`, `bad_optional_access`, nlohmann
  `out_of_range` and `Standard_Failure` each come back with `code:
  "internal"`, while `runtime_error` and nlohmann `type_error` do not. Send a
  response holding NaN and infinity, and its `nonfinite` lists both
  pointers. `refused_calls_smoke`, `rpc_contracts_smoke` and
  `papercuts_smoke` still pass. No existing refusal is labelled internal
  (they run under strict mode).
- **The sweep detects what it claims.** A temporary handler that indexes
  past an array, one that throws `Standard_Failure`, one that returns a NaN,
  and one that writes a file and then refuses are each reported by the
  sweep under the right failure, and are then removed. The sweep prints its
  coverage (methods with a valid baseline, feature types covered) and the
  docs say it does, not what it was: a count written down goes stale.
- **The fuzzer detects what it claims.** `fuzz_design.py` has a self-test
  mode that runs against a fake answer source. It feeds a cut whose digest
  adds volume, a union that breaks the identity, and a reopen with a
  different volume, and each must come out as its class. Minimising a
  synthetic failure planted at step 5 of 8, with steps 2 and 5 needed, gives
  exactly those two steps.
- **A first real run**: `tools/fuzz.py run --count 200` on a quiet machine.
  Its report is read, every failure class is either fixed with a corpus case
  or recorded in `known-bugs.md`, and `fuzz_seeds_smoke`'s list is chosen
  from the clean seeds.
- The headless suite, `tools/test-fast.sh` and `tools/ci-local.sh` pass, and
  the shards and `fuzz_seeds_smoke` each stay under their stated time.
  `tools/ci-local.sh --asan` runs the full sweep once before this lands.
