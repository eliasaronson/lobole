# One table for an operation's fields

## The problem

A field on `cam::Operation` is written out by hand in about ten places: the
member, its capability bit, the save file's write and read, the RPC's owned
list, apply step and answer, its `describe` rows (three of them), the
branches' `// rpc-params:` annotations, the panel, and the surface cache key.
Only the member and the panel carry anything specific to the field. The rest
repeat four facts: the member, its key, its kind, and which types have it.

The copies drift, and the drift is silent: `tabs`, and then `cut_angle_deg`,
`sample_step_mm`, `z_step_mm` and `rotary_pattern` were each accepted by the
server and refused by the generated SDK (fixed in 606a9ac4). The round-trip
tests from 991dc22f now catch a missed copy. This removes the copies.

## The shape

`src/core/cam/CamOpFields.{h,cpp}` (built in every configuration, as the
serializer is): one row per field.

```cpp
struct OpFieldRow {
  const char* key;   // the file key AND the RPC key
  OpField field;     // the capability bit; None = every type
  OpMember member;   // pointer to member: double, bool, or one of the enums
  unsigned flags;    // kByHand: the drivers below skip it
};
```

Default values come from a default-constructed `Operation`, never a second
literal: a field is left out of the file when it equals the struct's default,
which is the rule every field already follows. Enums read and write their
names through the existing name tables (`Sides()`, `RotaryPatterns()`, and a
new `CornerReliefs()`), and an unknown name is refused with one wording
everywhere: "unknown <key> '<value>'; this build accepts <list>".

The drivers, next to the table:

- `WriteOpFields(op, json&)` and `ReadOpFields(json, op&)`. The reader takes
  only the keys present, which is the same thing as reading with defaults,
  because the op starts default-constructed.
- `OpFieldRows()`, which the RPC reads for its owned list, its apply step
  (through `ReadOpFields`) and its answer.
- `HashOpFields(op, hasher)`, for the surface cache key.

**Hand-written, and flagged `kByHand` so the table still names them:**
`tabs` (three members as one object), `finish_tool` and `finish_stock` (saved
only together; the RPC checks the tool against the setup and derives the
stock), and `side`'s load default, which depends on the type. The identity
fields (id, name, type, tool, depths, targets) stay outside the table: every
op has them, and several have rules of their own.

## What changes around it

- `SerializeCam.cpp`: the per-field lines become two calls, plus the
  hand-written ones.
- `RemoteCam.cpp`: `kOwned`, the per-field apply lines and the per-field
  answer lines become loops over the table.
- `tools/rpc_params_scan.py`: `ApplyOpFields` becomes a KNOWN helper. A branch
  that hands its params to it reads the keys the helper's body reads by hand,
  plus every key in the table, parsed from `CamOpFields.cpp`. So the op
  branches lose their field annotations, and the `describe` rows are checked
  against the table itself.
- `CamSurface.cpp`: `SurfaceKey` hashes the table instead of a hand list, and
  `test_conventions`' surface-key gate accepts any `op.` member the table
  covers.
- `test_cam_capability.cpp`: the pokes stay hand-chosen, because they are
  picked to BITE on a generator, but every table row must have one.

Adding a field is then: the member, a table row, the panel widget, and a
poke.

## Verified by

The round-trip tests (both directions), the params gate against the new
scanner, the SDK smoke, and a throwaway save dump: every CAM job golden and a
fully-poked op of each type, re-saved before and after, byte-identical.

## As built

Save files are byte-identical: every CAM job golden and a fully-poked op of
each type, re-saved before and after (31 files). The save/load and RPC
round-trip tests, the SDK smoke and the params gate pass. Removing a key from
a `describe` row now fails the gate by name, because the scanner reads the
table instead of an annotation.

Following the helper found one more lie the annotation had covered:
`set_cam_op`'s row advertised `target`, which only `add_cam_op` reads, so the
SDK let `set_cam_op(target=...)` through and the server's strict-key check
refused it. The row no longer lists it.

It is not a line saving, and was not meant to be. The serializer and the RPC
lost about 220 lines. The table (half of it comments), the corner-relief name
table and the scanner's helper-following added about the same. What changed is
what a field costs: the member, a table row, the panel widget, and a poke.
