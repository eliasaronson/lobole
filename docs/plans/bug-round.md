# Bug round

## The problem

The round's branches found defects outside what each was building, and left
them in [known-bugs.md](../known-bugs.md) rather than widen their plans.
They fall in two groups.

**The app's own code**, each a few files:

1. **Placing a fastener, bearing or retaining ring cannot be undone**, in
   the GUI or over RPC: neither `add_*` nor `fit_*` (`RemoteAssembly.cpp`),
   nor the GUI paths that place one, take an assembly snapshot
   (`ui::SnapshotAssembly`) first. Ctrl+Z skips the placement and undoes
   whatever came before it.
2. **`add_section` with a bad `plane_id` leaves a section behind.** The
   handler (`RemoteInspection.cpp`) adds the section, then checks the
   datum and throws; `update_section` likewise applies `base_plane` before
   it refuses a bad `plane_id`. A refused call must change nothing
   (docs/scripting/rpc.md#a-refused-call-changes-nothing).
3. **Two values accepted and then dropped on save:** `set_study` takes
   `num_modes` on a study that is not modal, and `add_cam_op` takes
   `finish_stock` without `finish_tool`. The reader now refuses both in a
   file, so the app writes what it would refuse to read. They are refused
   at the call, naming what makes them valid.
4. **The feature limit has paths around it.** `add_feature`, `batch`,
   `derive_occurrence` and the tool window refuse past 50 000 features, but
   beginning a sketch, DXF/SVG import into a sketch, inserting a template
   and `split_for_printing` add features without asking, so the app can
   still save a part it will not open. One check (`CheckFeatureRoom(doc,
   n)`) is called by every path before its first change; a
   `test_conventions.cpp` scan fails a new `AddFeatureCommand` construction
   outside a function that calls it.
5. **`tools/fix-stale-nix-cache.sh` reconfigures a worktree's build from
   main's source.** Its toolchain-moved branch runs `cmake -S .` for every
   build dir it found, so a worktree's `build/` is pointed at the checkout
   the script ran from. It reads each tree's `CMAKE_HOME_DIRECTORY` before
   deleting the cache and passes that as `-S`.

**The kernel**, slower to fix and each needing a profile first:

6. **A Shell's result still follows memory addresses** on the patched
   OpenCASCADE (`action_matrix_full_3`). Carry upstream's OCCT#1600 in
   `nix/patches/`, wired into every build that applies the others (the
   overlay, `packaging/appimage/deps.sh` and release.yml's cache key,
   `packaging/windows/occ-cross.nix`, the Flatpak), and measure whether any
   draw beyond the first still changes an answer.
7. **The worm's 0.02 mm Shell runs past the matrix's minute.** Profile the
   replay of `tests/action_matrix/open/worm-shell-slow.json`, then either
   give that step a progress check in our patch or refuse the wall before
   the builder starts, by a measure taken from the body.
8. **OCC "premature end of file" in `headless_smoke`'s import phase**, seen
   once and not reproduced. Run the import phase in a loop under load
   (`tools/with-cpu-load.sh`) for an hour; if it reproduces, it becomes a
   case and a fix (a file read while still being written is the first
   suspect), and if not, its entry records what was tried.

The older entries in known-bugs.md (the phantom interference clash, the
sphere cut beside a fillet, the loose bbox, the U-edge names, `item_click`
focus, the shuffled-order pick failure, the Windows worker-thread fault) are
not in this round.

## The shape of the fix

Two branches, file-disjoint:

- **`bugs-app`:** items 1 to 5. Each lands with a test that fails before it:
  undo after each placement (a GUI test and a smoke), the refused
  `add_section`/`update_section` added to `refused_calls_smoke`, the two
  refusals in their smokes, a sketch begin and an import at the limit in
  `feature_limit_smoke`, and the script run against a scratch worktree.
- **`bugs-kernel`:** items 6 to 8.

Each fixed entry leaves known-bugs.md in its own commit. An item whose fix
turns out to need a decision is written up there instead.

## Files

- `src/remote/RemoteAssembly.cpp`, the GUI placement paths in `src/ui/`.
- `src/remote/RemoteInspection.cpp`.
- `src/remote/RemoteStudies.cpp` (or wherever `set_study` lives), the CAM
  op RPC.
- `src/core/io/ReadContext.h` or `src/core/Document.h` for the shared check;
  each add path; `tests/test_conventions.cpp`; `tests/feature_limit_smoke.py`.
- `tools/fix-stale-nix-cache.sh`.
- `nix/patches/`, the four builds that apply them; `src/core/features/ShellFeature.cpp`.
- `docs/known-bugs.md`; the RPC `describe` rows of every refusal added.

## Verification

- Every new test fails on main before its fix.
- `bugs-app`: the headless suite, the GUI suite, `tools/ctest -L pygui`.
- `bugs-kernel`: the full action matrix (`action_matrix_full_3` green), the
  worm case answering inside the minute or refused at once, the headless
  suite.
