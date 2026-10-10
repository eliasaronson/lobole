# Print profiles: calibration as data, and STL export that keeps the gaps

## The problem

`src/core/PrintAllowance.cpp` is a static table per (process, material). A
print allowance belongs to a printer, a filament and a slicer preset, and
the table cannot say so:

- On one P1S, a print-in-place gap that slides is 0.1 mm a side in K5 PLA
  and 0.15 in an ordinary PLA. The `pla` row can hold one of them.
- Correcting the table means editing C++ and rebuilding. The coupon
  (`generators/print_coupon.py`) measures the number, but nothing records
  it.
- The regeneration hash (Document.cpp, `PrintDrivesGeometry`) and the
  requirements hash (RemoteRequirements.cpp) fold in the process and
  material NAMES. Once the numbers can change, geometry baked from the old
  numbers would stay stale.

A printed part's STL loses its gaps on the way out:

- `export_stl` (RemoteProject.cpp) and the GUI export (FileDialogs.cpp,
  `ExportActiveDocument`) mesh at a 0.1 mm chord deviation, hard-coded in
  the GUI.
- Measured on the coupon, that took 0.03 mm a side off every gap. The
  slicer's gap closing then shut the two narrowest.
- The export knows the part is printed and does nothing with it.

## The shape of the fix

### Profiles

A **print profile** is a named, user-owned copy of a table:
`printfit::Profile {name, process, material, conditions,
stl_deflection_mm, rows[]}`. The CAM machine store is the precedent and the
pattern to copy (docs/cam/setup.md, `DocumentIO::LoadMachines`,
`ui::ReloadMachines`).

- **Store:** one file per profile, `~/.config/lobole/print_profiles/<slug>.json`.
  - Load, save and delete as the machines do: a broken file is reported
    and the rest still load.
  - Saving refuses a missing name, two names that fold to one slug, an
    unknown base process, an overlapping or out-of-order size range, and
    an allowance of the wrong sign for its kind.
- **App and RPC:** a `state.printProfiles` mirror;
  `list_print_profiles` / `save_print_profile` / `delete_print_profile`;
  and a Preferences header with the editor.
  - The built-in tables stay, read-only, as the defaults a profile starts
    from.
- **The document carries its numbers.** The part's `print` becomes
  `{process, material, profile?, rows?}`, with an embedded copy of the
  profile's rows, the way a CAM setup embeds its machine.
  - The embedded copy is what every consumer reads.
  - Its state against the user's store (same, differs, missing) is
    reported by `get_print_process`, the Tolerances panel and `check_fit`'s
    notes, with *Use mine* and *Save as mine*.
  - Nothing is substituted and nothing refuses to open: a part printed on
    one machine still carries what it was modelled with.
  - This bumps `kVersion`: a golden at the new version and a row in
    docs/versioning/format.md.
- **The lookups take the resolved table:**
  - `printfit::Find`, `ForBasic`, `ForModelled`, the repeatability and the
    thread floor take the document's table instead of looking one up by
    name.
  - The call sites pass it through: HoleFeature, RetainingRingGroove,
    FitBands, BearingActions, ToleranceActions, TolerancePanel, and
    RemoteInspection's print RPCs.
  - The regeneration and requirements hashes fold in the rows' content.
  - A profile edit re-stamps the fit bands.
- **The coupon records its answer:**
  - `print_coupon.py --profile NAME` builds against that profile.
  - A `--record` step takes the winners (the bore and pin that fit, the
    tightest gap disc that slides, whether the thread went), turns them
    into rows, and calls `save_print_profile`.
  - The rows' `source` names the coupon, and the profile keeps the
    deflection it was exported at.
- **The two measurements we have become the first two profiles:** K5 PLA
  at 0.1 mm a side and plain PLA at 0.15, both on a P1S. The built-in
  `pla` row goes back to the community's number, marked as a default to
  calibrate.

### Export

- One export helper for the RPC and the GUI.
- **On a printed part with no deflection given,** it meshes at the
  profile's `stl_deflection_mm`. A profile without one gets a tenth of its
  smallest gap.
- **A coarser deflection asked for explicitly is exported as asked,** and
  the reply (and the GUI's status line) says how much of the smallest gap
  the chords can take.
- `SaveSTL` takes the angular deflection explicitly. The OCC default of
  0.5 rad is why `ChordLossMm` is only an upper bound.

## UI

**Preferences › Print profiles** is a third hand-written section after
Keyboard and Machines, cloned from `DrawMachinesSection`
(src/ui/SettingsWindow.cpp).

- **Layout,** top to bottom:
  - a muted blurb (what a profile is; that a part keeps a copy);
  - `New` / `Duplicate` / `Delete`;
  - the list;
  - one red line for the store's last refusal;
  - the detail form, written on every edit, with no Save button.
- **The form:**
  - Name: a rename refused when taken, as a machine's is.
  - Process and Material combos over the built-in tables.
  - Conditions (text).
  - STL chord deviation, via `ExprInputDouble`.
  - The rows as a table: kind, from, to, allowance (`ExprInputDouble`),
    and source.
- **New** starts from the built-in table of the chosen process and
  material, so a profile is "the defaults, corrected".
- **Editing a gap from a coupon** is typing the winning disc's value into
  the gap row, with the source filled in as "coupon <date>". A coupon
  wizard is not part of this.

**Variables panel › Print process** (`DrawPrintProcessSection`,
src/ui/TolerancePanel.cpp) keeps its place and its single combo.

- **The combo:** "Not printed", then the user's profiles, then the
  built-in tables labelled "(default)".
- **Under the combo:** the match block copied from the CAM setup's machine
  block (src/ui/CamTree.cpp):
  - same: nothing;
  - differs: amber "Modelled with a different '<name>' than yours." with
    `Use mine` / `Save as mine`;
  - missing: amber "'<name>' is not one of your profiles." with `Save it`.
  - Both buttons are undoable commands on the document.
- **The rows** are listed as today, from the document's own copy.
- **No profiles yet:** a muted line with a `Preferences › Print profiles`
  SmallButton, as the CAM setup does for machines.

**Export:**

- File › Export keeps its native save dialog. A printed part's STL picks
  its chord deviation from the profile, so there is nothing to ask.
- The GUI never passes a coarser one, so the warning is the RPC's alone.
- Today an export only logs. It now shows a viewport status pill
  (`DrawPromptPill`, `StatusGood`): "Exported part.stl — chords 0.01 mm".
  A failure shows as `Warning`.

**GUI tests:**

- create, duplicate, rename (and the refused rename), delete;
- pick a profile in the Print process combo;
- edit the profile so the part reads "differs", then `Use mine` and
  `Save as mine`;
- the export pill.

## Files

- `src/core/PrintAllowance.{h,cpp}`: `Profile`, lookups on a table.
- `src/core/io/Serialization.cpp` (`print` envelope), `SerializationInternal.h`
  (`kVersion`), and a new `SerializePrintProfiles.cpp` beside `SerializeCam.cpp`.
- `src/core/Document.{h,cpp}`: the print process carries its rows; hashes.
- The consumers listed above; `src/remote/RemoteRequirements.cpp`.
- `src/ui/PrintProfileActions.{h,cpp}` (new), `src/ui/SettingsWindow.cpp`,
  `src/ui/TolerancePanel.cpp`, `src/ui/FileDialogs.cpp`.
- `src/remote/RemoteInspection.cpp` (the RPCs), `src/remote/RemoteProject.cpp`
  (export), `src/cad/CADKernel.{h,cpp}` (`SaveSTL`).
- `generators/print_coupon.py`, `generators/planetary/build.py` (reads the
  profile's deflection rather than its own tenth-of-the-gap rule).
- `tools/headless-preview.sh` (seeds the scratch config with the profiles,
  as it does the machines), `tests/config_isolation_lint.py` if it needs
  the new directory.
- docs/tolerances/README.md, docs/ui/settings.md, docs/scripting/rpc.md,
  docs/versioning/format.md.

## Verification

- **Unit tests:**
  - A profile round-trips through its file.
  - Each refusal says what it refuses.
  - A document carrying rows that differ from the store's profile reads
    its own and reports `differs`.
  - A missing profile opens with the embedded rows and reports `missing`.
- **Golden:** a printed part at the new version, with embedded rows.
- **Hashes:** editing a profile's gap regenerates a fitted clearance hole
  modelled from it, and changes the requirements hash.
- **Smokes:**
  - `print_allowance_smoke` extended to profiles.
  - A coupon `--record` run against a scratch config produces a profile
    that `print_allowance` then answers from.
- **Export:**
  - On a printed part with no deflection, the coupon's gap row measured in
    the STL is within 0.005 mm of the model (the check this round did by
    hand).
  - A coarse explicit deflection is exported as asked, and the reply warns.
- **Suites:** `config_isolation_lint`, headless, GUI.
