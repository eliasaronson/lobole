# Reader fuzzing

## The problem

Every file the app reads was written by something other than this build: an
older or newer build, a merge, a hand edit, a sync client, a disk that
filled up mid-write. A reader given damaged bytes must do one of two things:
refuse, naming the file and what is wrong, or load the file correctly.
It must never crash, hang, or load a document that is silently missing
content. That last outcome is the dangerous one, because the user then saves
over the original and the missing content is gone.

No test damages a file on purpose. `tools/fuzz.py` and
`hostile_params_smoke.py` both go through the RPC, so neither reaches a
reader. The hand-written cases (`test_serialization.cpp`,
`test_baked_geometry.cpp`, `test_settings.cpp`, `test_crash_reports.cpp`)
each cover a particular kind of damage. A survey of the readers already finds
the following, and none of it is tested:

- **No file name in the error.** Only open and parse failures name the file.
  A wrong type deep inside a feature (`.at()`, `.value()`, `type_error.302`)
  reaches the user as nlohmann's bare message. So does a version newer than
  `kVersion`, and so does a parse error in `project.json`
  (`ReadProject`, `std::ifstream >> json`).
- **Content dropped with no word to the user:**
  - unknown keys in a document are ignored;
  - a project entry whose `type` is unknown loads as a part (`ReadSlot`);
  - an asset an image references but `assets/` lacks is skipped;
  - a drawing dimension listed in the index but missing on disk is skipped;
  - in a duplicated JSON key, the last value wins.
- **Damage to a derived file fails the whole load.**
  `StudyResultFromJson` and `MotionResultFromJson` are not guarded, so a
  damaged saved result fails the load of its whole document. These results
  are derived data: a saved drawing projection that is damaged is dropped
  with a warning instead.
- **A bad field kills the start.** `settings::WindowFromJson` and
  `CrashReports` `FromJson` call `j.value()` outside any try, so one field of
  the wrong type throws out of `main` before the window opens.
- **The reason for a refusal is lost.** Recovery (`RecoverProject`) and
  compare-with-revision (`ProjectActions.cpp`, after `GitVault::ExtractSubtree`)
  throw the loader's error away and say only that the copy "does not load".

## The shape of the fix

1. **One mutation library, seeded and reproducible**
   (`tests/mutate/Mutate.{h,cpp}`). It takes a valid input and a seed and
   returns a list of cases. Each case carries a stable id,
   `<corpus>/<file>:<operator>:<n>`. The RNG is xorshift64, seeded by a hash
   of that id, so one case can be replayed without the others. The operators
   come in five levels:
   - **Bytes**, after SolidCraft's `crates/io/src/mutate.rs`:
     - truncation at every length of a file under 1 KB; for a longer file,
       its first and last 64 lengths and 1024 evenly spaced between;
     - bytes flipped, zeroed and set to 0xff at seeded places;
     - runs of bytes dropped, repeated, or swapped with the next run;
     - garbage appended, the file doubled, the file reversed;
     - an empty file.
   - **Text**: lines dropped, repeated and swapped; one line replaced by
     another; one word replaced by another word from the same file; each
     number in turn replaced by a liar, then all numbers lying at once. The
     liars are the values `hostile_params` sends
     ([rpc.md](../scripting/rpc.md#the-hostile-sweep)): `nan`, `inf`,
     `1e400`, `-0`, `-1`, `0`, `2^31`, `2^53+1`, `2^64`, `-2^63`, `1.5`.
   - **JSON**: the file is parsed and changed at a JSON pointer:
     - a key deleted, or written twice with different values;
     - object keys reordered (this one must load identically);
     - a value's type swapped for each other JSON type;
     - a number replaced by a liar;
     - an enum string with one character changed;
     - `version` set to 0, 1, `kVersion`, `kVersion + 1`, -1, `"188"`, or
       removed;
     - a value wrapped 10 000 arrays deep;
     - an array grown to 10^6 copies of its first element.
   - **Model**, using what the format means:
     - an id reference (a feature's sketch or face, a mate's occurrence, a
       drawing view's source, a CAM operation's setup) pointed at an id that
       does not exist, at its own item, or at a later item, which makes a
       cycle;
     - two items given the same id;
     - id 0;
     - features reordered so that a reference points forward.
   - **Directory**, for the `.dsgn` forms:
     - a file deleted, emptied, or duplicated under another id;
     - an index entry removed while its file stays;
     - one document's file swapped for another document of a different kind;
     - a `dir` or asset name that leaves the project
       ([format.md](../versioning/format.md#paths-in-a-project));
     - a symlink that points outside the project;
     - a `.result.bin` or `.projection.bin` whose offset or count field lies.
2. **The corpus.** Every golden (`tests/golden/`, `assembly/`, `drawing/`,
   `job/`) is mutated in its single-file form. Each is also saved by this
   build into a scratch `.dsgn`, so the directory form, `project.json`, the
   baked `geometry/` (`index.json` and the `.brep` blobs) and the result
   arrays are mutated as this build writes them today. One assembly golden
   with a study result and one drawing with a saved projection make sure
   those side files exist. Larger projects are an opt-in for the slow run:
   `LOBOLE_READER_CORPUS=<dir>` points at generator output under `scratch/`.
   That output is untracked, so it never decides whether the suite passes.
   The non-document readers mutate fixtures written in the test itself:
   - a settings file holding every key and a `keys` table;
   - one machine profile and one print profile;
   - a crash marker and a `report.json`;
   - an instance registry entry;
   - an autosave sidecar of each shape;
   - a git vault with two commits (the log parse, `ShowFile` with
     `ExtractSubtree` and the load, and `ProjectMerge`);
   - a PNG and a JPEG for `stbi_info_from_memory` and the underlay's decode
     under `ImageLimits`.

   `imgui.ini` is ImGui's own parser and is outside this plan.
3. **The harness runs in-process** (`tests/test_reader_fuzz.cpp`, inside
   `designer_tests`). It calls each reader's real entry point directly:
   - `DocumentIO::Load`, `LoadAssembly`, `LoadDrawing` and `LoadJob`;
   - `ReadProject`;
   - `LoadGeometry`, through a load that adopts the blob;
   - `settings::Load`;
   - `LoadMachines` and `LoadPrintProfiles`;
   - `CrashReports::Collect` and `ListReports`;
   - `LiveInstances`;
   - the recover path;
   - `ProjectMerge`.

   Going through the RPC would cost one round trip per case and give no
   better answer. The toolchain is GCC, which has no libFuzzer, so the
   harness is a deterministic loop. A reader is wrapped as
   `Outcome Read(const fs::path&)`, which returns either refused (with the
   message) or loaded (with the document or value).

   Before each case, the harness writes the case id to stderr and to
   `build/reader-fuzz-last-case`. If a case crashes or hangs, that names it.
   Each case has a time budget of 2 s, and 8 s in a build with
   `ENABLE_SANITIZERS`, through a compile definition. A reader that returns
   past its budget fails its case. A reader that never returns is stopped by
   ctest's `TIMEOUT`, and the last-case file names it.
4. **The rules applied to every case.** The harness fails a case when:
   - **Crash.** The process dies, or ASan or UBSan reports. A crash inside a
     reader is a test failure here, not a caught exception.
   - **Refused without a name.** The message must contain the path of the
     file the reader was reading, relative to the project, and something
     beyond the path. Directory-level damage names the directory and the
     entry. The fix is a `FileContext` scope in `SerializationInternal.h`
     that every reader opens around each file it reads. Any exception that
     leaves a scope is rethrown with that file's path in front, so
     `type_error.302` arrives as `features/7-fillet.json: radius: type must
     be number, but is string`.
   - **Silently missing content.** This is the hard rule, and it is checked
     in three ways:
     - **Round trip.** A loaded case is saved into scratch. The new tree is
       compared with the mutated input after normalisation. For every case,
       the normalisation is the difference that loading and saving the
       *unmutated* original produces; that difference is the migrations and
       defaults this build applies. Anything else present in the mutated
       input and absent from the save must appear in the load's report (the
       next bullet), or the case fails.
     - **Idempotence.** Load the saved copy again and save it again. The
       second save must match the first byte for byte (the writer is
       already canonical: sorted keys, `dump(2)`).
     - **Counts.** For goldens that have an `.expect.json`, a case that loads
       must keep the counts that file records (features, bodies, occurrences,
       views). If a count changed, the load must have reported it.
   - **Reported loss.** A load that keeps going past damage reports it.
     `DocumentIO` gains a `LoadReport`: one entry per dropped key, skipped
     file or discarded side file, each with a path, a JSON pointer and a
     reason. It travels with the loaded document. `open_project` returns it
     as `dropped`, its `describe` row is updated, and the GUI shows a notice.
     A document that loaded with drops is not saved back over the path it
     came from by default, but the user can always choose to. In the GUI,
     Save opens a warning that lists the drops, with three choices: Save a
     copy… (the default button), Overwrite anyway, and Cancel. Overwrite
     anyway saves and keeps the drops in the log. The RPC `save_project` to
     that path is refused, naming the drops, unless the call passes
     `overwrite_lossy: true`. The save then goes ahead and its reply repeats
     the drops as a warning. Save As to another path is never blocked.
     Derived data (a study result, a projection, a baked blob) that is
     dropped and rebuilt is reported but does not block a save, since
     nothing the user wrote is lost.
5. **The fixes the sweep needs first.** The gaps under *The problem*:
   - an unknown key is refused, naming it and the keys that are accepted. A
     newer build's keys come with a newer version, which is already refused.
   - an unknown project `type` is refused, naming the accepted types;
   - a missing asset or drawing dimension is refused, naming the file;
   - a duplicate JSON key is refused: parse with a callback that sees each
     key;
   - a damaged study or motion result is dropped and reported, as a damaged
     projection already is;
   - the window and crash-report fields are read with a guard per field, as
     the rest of `Settings.cpp` already does;
   - recovery and compare-with-revision pass the loader's error through.

   Whatever else the sweep finds is fixed the same way.
6. **A bug the sweep finds becomes a committed case.** `tools/reader-fuzz.py
   case <id>` writes the mutated file or tree to `tests/reader_cases/<name>/`
   with an `expect.json`. The expectation is either `refused` with the
   substrings the message must hold, or `loads` with the counts, and an
   optional `dropped` list. `test_reader_cases.cpp` loads every case
   directory. A case stores the bytes, not the id, so changing an operator
   never silently retargets an old regression. A crash case also follows
   the CLAUDE.md rule, because it is built from what crashed.
7. **Registration.** There is one `TEST_CASE` per reader family, so ctest
   runs them in parallel through `catch_discover_tests`.
   - `[reader_fuzz]` is a fixed sample: every operator at least once per
     corpus file, about 1/50 of the cases. It is part of `test-fast.sh` and
     must stay under about 10 s in total.
   - `[reader_fuzz][slow]` is every case. It is excluded from the inner loop
     and runs in the headless suite and under `tools/ci-local.sh --asan`,
     whose exclusion is `gui|asan_slow`. It must not carry `asan_slow`,
     because the sanitized run is where a lying count becomes a reported
     overflow.
   - `tools/reader-fuzz.py run --seed S` reruns the full sweep with another
     base seed and prints the cases that fail. That is for a deliberate
     audit; ctest's seed is fixed.

## Files

- New: `tests/mutate/Mutate.{h,cpp}`, `tests/test_reader_fuzz.cpp`,
  `tests/test_reader_cases.cpp`, `tests/reader_cases/`,
  `tools/reader-fuzz.py`. `tests/CMakeLists.txt` gets the sources and a
  `TIMEOUT` on the fuzz tests.
- Readers:
  - `src/core/io/SerializationInternal.h` (`FileContext`, the duplicate-key
    parse);
  - `src/core/io/Serialization.cpp`, `Serialize{Features,Assembly,Studies,DrawingProjections,Configurations,Cam,PrintProfiles}.cpp`;
  - `src/core/io/GeometryCache.cpp`, `Settings.cpp`, `CrashReports.cpp`,
    `Keymap.cpp`;
  - `src/remote/InstanceRegistry.cpp`;
  - `src/ui/ProjectActions.cpp` (`ReadProject`, `ReadSlot`,
    `RecoverProject`, compare-with-revision), `src/core/ProjectMerge.cpp`;
  - `src/core/io/Serialization.h` (`LoadReport`).
- RPC: `src/remote/RemoteProject.cpp` (`open_project` returns `dropped`,
  `save_project` takes `overwrite_lossy` for a lossy load) and their `describe` rows.
  The GUI save prompt is in `src/ui/ProjectActions.cpp`.
- Docs: `docs/versioning/README.md` (*What a load refuses*, *Tests*),
  `docs/versioning/format.md` (no unknown keys, no duplicate keys),
  `docs/architecture.md` (the sweeps paragraph), `docs/scripting/rpc.md`.

## Verified by

- The library's own test: the same seed gives the same cases and another
  seed gives different ones; truncations go down to empty; every operator
  produces at least one case on a small golden; a 100 KB file stays under a
  few thousand cases.
- The harness proves it sees what it claims. A planted reader that drops a
  key fails the round-trip rule. One that throws without a path fails the
  naming rule. One that sleeps fails the budget.
- `[reader_fuzz]` passes in `test-fast.sh`. `[reader_fuzz][slow]` passes in
  the headless suite and under `tools/ci-local.sh --asan`, and the time of
  each is in its output.
- Every gap listed under *The problem* has a case in `tests/reader_cases/`
  that failed before its fix.
- A smoke opens a project with an unknown key removed by hand, reads
  `dropped` from `open_project`, and checks that `save_project` to the same
  path is refused without `overwrite_lossy`, and goes ahead with it. The GUI suite covers the warning and both of its choices.
- `test_golden.cpp` still passes unchanged: no valid file is refused. The
  headless, GUI and pygui suites all pass.
