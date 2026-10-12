# A table thread's pitch, and a Z gap a profile can hold

Two file-disjoint fixes from the print-in-place counter round.

## 1. A table thread with another pitch is refused, not substituted

`ThreadFeature::Execute` writes `pitch = table pitch` whenever the
designation resolves. So `M6` with `pitch: 1.25` loads, regenerates clean and
is an M6x1 — the caller's number is thrown away without a word, which is the
substitution CLAUDE.md's "Refuse, don't substitute" forbids. The SDK's
`thread()` documents it ("a table thread takes the table's whatever is
given"); the JSON loader feeds it (an absent `pitch` is read as 1.25, which
is wrong for every table row but M8 and only works because Execute
overwrites it).

The shape of the fix:

- **One check**, `threads::PitchError(standard, designation, pitch)` in
  `src/core/ThreadStandards.{h,cpp}`: empty when the pitch is the row's
  (within 1e-6 mm), else a refusal naming the row's pitch and the other rows
  of the same diameter and family by their own designations ("M8 is 1.25 mm;
  for 1 mm use M8x1"), and that any other pitch is a custom thread. Fine
  pitches are rows of their own (`M8x1`, `M10x1.25`), so a designation plus
  its own pitch is accepted and nothing else is.
- **Regeneration** (`ThreadFeature::Execute`): refuses the mismatch as the
  feature's error instead of overwriting `pitch` — the same place and form as
  a flank angle on a table designation. A saved document with a mismatch
  still opens and says which feature is wrong and what it accepts.
- **Load** (`SerializeFeatures.cpp`): an absent `pitch` is the table's for a
  table designation; for a custom one it is 0, which regeneration refuses
  ("a custom thread needs its pitch") rather than inventing 1.25.
- **RPC** (`RemoteModeling.cpp`, `add_feature` and `modify_feature`): the
  built feature is checked before anything is committed, so a refused call
  changes nothing (docs/scripting/rpc.md#a-refused-call-changes-nothing). The
  same pre-check refuses the other inputs regeneration would refuse without
  needing the face (an unknown series row, a flank angle on a table thread or
  out of range, a custom thread with no pitch) — one function,
  `ThreadFeature::WhyRefused()`, that Execute also calls.
- **SDK** `thread()` docstring: a table thread's `pitch` may be given only as
  its own; the server refuses another.
- **GUI** (`FeatureEditor.cpp`): the pitch row of a table thread shows the
  stored pitch; when it is not the table's (a loaded file), it says so with a
  *Use the table's* button rather than displaying the table's number over a
  different stored one.
- Describe row (`Thread` in `SerializeFeatures.cpp`), `designer_mcp.py`'s
  Thread line and docs/modeling/holes-and-threads.md#thread.

Not a format change: the stored fields and their meaning are unchanged; a
file that contradicts itself is now reported instead of silently repaired.
No golden carries a mismatch (checked).

Tests: `tests/test_thread.cpp` (mismatch refused with the accepted pitches,
`M8x1` accepted, the absent-pitch load, custom without pitch), existing tests
that relied on the overwrite corrected to the table pitch;
`tests/refused_calls_smoke.py` (add_feature and modify_feature with a
mismatch: refused, document unchanged).

## 2. A Z-gap row on the print coupon, kept in the profile

The coupon's gap row measures sideways clearance. The counter stacks moving
parts with a vertical gap of whole layers — one layer at 0.1 a side, by a
rule (`ceil(gap_side / layer)` layers) nobody has printed — and takes the
layer height from `--layer 0.2`.

**The row.** Each feature is a ring on a post, the ring's underside `N`
whole layers over the plate's top (N = 1, 2, 3 by default; centred on the
profile's Z gap when it has one). The ring clears the post by the profile's
sideways gap (already measured, so the Z gap is the only variable), and a
45-degree conical head on the post captures it with a clearance of the
largest N plus two layers above, so only the underside interface is under
test and nothing overhangs past 45 degrees. Every size derives from `--size`
(ring Ø1.6 size, post Ø0.5 size, ring 0.25 size thick); the label is
engraved beside each. Twist each ring: the smallest N that turns cleanly is
the Z gap.

**A row on the full coupon, and `--z-gap-only`.** The full coupon is what a
new printer or filament gets, so it carries the row. The user's P1S profiles
already have their sideways gap; for them `--z-gap-only` prints the row alone
on a plate a quarter of `size` thick — three rings, about 60 x 20 mm.

**A profile field, so a format change.** A Z gap is a property of one
printer, filament and slicer preset (layer height, interface behaviour) —
exactly what a profile is — and the counter must read the measurement
instead of a flag, the way it reads the sideways gap. It is not a diameter
allowance, so not a row: the profile gets `layer_height_mm` (the preset's
layer, 0 = not stated) and `z_gap_mm` (0 = not measured), written only when
stated, embedded in a printed part with the rest of the profile.
`ValidateProfile` refuses a negative or non-finite value, a Z gap without a
layer height, and a Z gap that is not whole layers (naming the two nearest
that are). Neither drives geometry in the app, so neither joins
`Fingerprint`.

- `src/core/PrintAllowance.{h,cpp}`, `src/core/io/SerializePrintProfiles.cpp`,
  `src/core/io/Serialization.cpp` (the embedded keys), kVersion 175 with a
  golden (a part printed with a profile stating both) and a format.md row.
- `src/remote/RemoteInspection.cpp`: list/save_print_profile and
  get_print_process rows and output carry both.
- `src/ui/SettingsWindow.cpp`: two rows in the profile editor.
- `generators/print_coupon.py`: the row, `--z-gap-only`, `--z-layers`,
  `--layer` (else the profile's, else 0.2 with a line saying so);
  `--record --z-gap-layers N` stores `z_gap_mm = N * layer` and
  `layer_height_mm`; `--seed` states the P1S layer height (0.2) and no Z gap.
- `generators/counter/build.py`: `--layer` defaults to the profile's; the
  vertical gap is the profile's `z_gap_mm` when measured, else the current
  rule, and the receipt says which.
- docs/tolerances/README.md (profiles, the coupon), generators/counter
  README.

Tests: `tests/test_print_allowance.cpp` (validation, round trip, embedded
copy), the golden, `tests/print_allowance_smoke.py` (save and list carry the
fields, a refused one changes nothing); the coupon generated with
`--z-gap-only` and in full, its STL checked for the right shell count.
