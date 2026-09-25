# The Python SDK

`tools/designer_sdk.py` is what generators and agent scripts build parts
with. It has two layers, and the split is the design.

```python
from designer_sdk import connect

app = connect()                         # the one announced instance, or port=
part = app.part("piston")               # create a part tab (create=False opens one)
bore = part.param("bore", 86)           # a variable, and a Param that knows it
body = part.cylinder(d="bore", h=40)    # a Body
part.fillet(body.edges(circular=True), r=2)
seat = part.frame("wrist_pin", body.faces(kind="cylinder")[0])
part.check()                            # raises on any broken feature
```

`connect()` with no port attaches to the one instance in the registry and
refuses, listing them, when there are several or none. It warns when the
instance's binary has been rebuilt since it started (`ping`'s
`binary_replaced`).

## The generated layer

The RPC surface is not written by hand. At connect time `App` reads
`describe` and every documented method becomes an attribute:
`app.add_feature(...)`, `app.contact_report(...)`, `app.frame(...)`. Keyword
arguments are checked against the row's parameter list before the round
trip, and a wrong one is refused naming the keys the method does take
(free-form methods such as `add_constraint` accept anything). Methods with a
typed contract are validated against their schema. Enums are on `app.enums`,
read from the app. A new RPC is callable the day it lands, a renamed one
fails by name, and the client cannot drift from the server.

- `_timeout=` (seconds) on any generated call, for the ones that run for
  minutes (a large STEP import, a study).
- A trailing underscore reaches a parameter that is a Python keyword:
  `add_motion_study(from_=0, to=360)`.
- `app.call(method, **params)` is the raw escape hatch; `app.methods()` and
  `app.help(method)` list and describe.
- `app.transaction()` / `part.batch()` is a context manager over the `batch`
  RPC ([rpc.md](rpc.md#batch)); handles returned inside it are `$ref`s. On
  `App` it is `transaction` because `app.batch` is the generated RPC.

`sdk_smoke` gates the split: every documented method is reachable, nothing
hand-written on `App` shadows a generated one, the enums are read rather than
copied, and no constraint-kind string is hard-coded in the module.

## The hand-written layer

Only what `describe` cannot express:

- **References, not numbers.** `Face`, `Edge`, `Frame`, `Plane`, `Body`
  carry `.center`, `.axis`, `.normal`, `.radius`, `.plane()`, `.ref`. A
  position stays an object; it is never a float the caller reads off
  `list_elements` and types back in. `body.faces(kind=, normal=, radius=,
  where=)` and `body.edges(kind=, circular=, radius=, tol=)` filter;
  `face(...)` / `edge(...)` demand exactly one match and refuse otherwise.
  `circular=True` means circle or arc, because the kernel may split a hole's
  rim into arcs; `tol` is a real tolerance in millimetres (0.05 by default).
- **Plane arithmetic.** `Plane.xy(z)`, `.xz(y)`, `.yz(x)`, a face's
  `.plane()`, `plane.uv(world_point)` and `plane.world(u, v)`. Sketch UV is
  measured from the plane origin: `world = origin + u*U + v*V`. The sketch
  plane's V axis is normal x U, which is why `Plane.xz` has a normal of −Y.
  See [the principal planes](#the-principal-planes) for each helper's axes
  and `flip=True`.
- **Compositions** of several RPCs: `revolve_profile`, `bolt_circle`,
  `polyline`, `slot`, `polygon`, `arc_through`, `project`, `pattern`,
  `fillet(edges=...)`, `hole`, `thread`, and a sketch builder that
  constrains (`concentric=`, driven by a variable) rather than placing by
  number.
- **Assembly verbs that mate by frame name**: `asm.place`, `fasten`,
  `revolute`, `slider`, `cylindrical`, `planar`, `ball`, `gear`, `chain`,
  `rack`, `screw`, `cam`, `motion`, `bolt`, and `asm.check()`, which raises
  on a floating occurrence, a clash or a near miss.
- **`part.check(solids=)`** — raises on any errored feature, prints
  warnings, and optionally requires a solid count.

Rules the hand-written layer follows: every method is a composition of
documented RPCs and never sends a field the RPC does not have; every method
returns the ids it made; a default with a unit is derived from what it acts
on (CLAUDE.md, "Defaults"). A coordinate or radius given as a string rides
on the entity as its `*_expr` field and the document evaluates it, so there
is no second expression language in the client.

### The principal planes

A positive extrude distance runs along the sketch plane's normal. Each
helper takes `flip=True` for the same plane facing the other way, so a
feature on the far side of a part extrudes a positive distance instead of a
negative one. Because V is normal x U, reversing the normal must reverse U or
V as well; the vertical planes keep V pointing up (world +Z), so a flipped
one is the sketch seen from behind, mirrored left to right:

| helper | normal | U | V |
|---|---|---|---|
| `Plane.xy(z)` | +Z | +X | +Y |
| `Plane.xy(z, flip=True)` | −Z | +X | −Y |
| `Plane.xz(y)` | −Y | +X | +Z |
| `Plane.xz(y, flip=True)` | +Y | −X | +Z |
| `Plane.yz(x)` | +X | +Y | +Z |
| `Plane.yz(x, flip=True)` | −X | −Y | +Z |

Place sketch geometry with `plane.uv(world_point)` rather than by reading the
table, and the mirroring never reaches the caller. `sdk_smoke` extrudes on
each flipped plane and checks the solid lands on the far side.

### Opening a part is a choice

`app.part(name)` creates the part. When a tab of that name already exists it
opens it only if this `App` object created it (a script switching back to a
part it built earlier); any other is refused, naming the two ways to say what
is meant:

- `app.part(name, create=False)` opens an existing part, and refuses when
  there is none — for a script that reads or edits a part it did not build.
- `app.part(name, reuse=True)` opens the part if it exists and creates it if
  not — for a generator that deliberately rebuilds its own part in place.

`app.assembly(name, ...)` follows the same rule. A slot id (`app.part(7)`)
and `app.part()` with no name always open. The rule exists because a pump
generator building into an engine project wrote its camshaft into the
engine's `camshaft` tab: the name matched, and the SDK opened it without
saying so. `new_project` and `open_project` forget which tabs the client
created, since ids are per project.

### Parameters carry their expression

`part.param(name, value)` and `app.param(...)` (project scope) declare a
document variable and return a `Param`: a `float` that also carries `.expr`,
so arithmetic between Params composes the expression. `L - 18` is
`Param(92.0, "(arm_len - 18)")`, and passed to a feature it lands as the
number plus its `*_expr`, so the document receives the formula rather than
the answer and the dimension stays adjustable after the run.
`part.params(table)` / `app.params(table)` declare a whole dimension table at
once and return `{name: Param}`; an entry may name any declared before it.
Where the document cannot hold an expression a Param behaves as its number.
`param_smoke` builds a part from Param arithmetic, nudges a variable, and
asserts exactly the expected fields of the saved document move.

### Reads do not switch tabs

`Part.state()`, `bodies()`, a body's faces and edges, `Element.frame()` and
`volume()` pass `part=` to the read RPCs and never activate the tab
([rpc.md](rpc.md#reads-name-their-part)). Writes activate their part first.
`App` caches which tab is active to skip a round trip; any call that can move
the tab (`add_part`, `create_drawing`, `set_active_part`, …) drops the cache,
and `app.active()` asks the app.

### Angles name their unit

Every hand-written angle keyword ends `_deg`: `revolve(angle_deg=)`,
`revolve_profile(angle_deg=)`, `pattern(step_deg=)`,
`bolt_circle(start_deg=)`, `polygon(rotate_deg=)`, `frame(rotate_deg=)`. The
unit-less spellings (`angle=`, `rotate=`, `start_angle=`) are refused, naming
the replacement (the `_renamed` decorator), never guessed at: the app's own
guard only catches a radians value over a full turn, so a 5-degree value read
as radians would pass as 286 degrees.

- `pattern(step_deg=None)` defaults to `360 / count`, spreading the copies
  evenly round a full turn (the step is per copy, so 360 would stack every
  copy on the original).
- `Sketch.constrain(value=Param)` sends the number and the Param's
  expression, so the constraint follows the variable. An `angle`
  constraint's `value` is radians while an expression evaluates in degrees,
  so a Param there is refused, naming `value_deg=`.

`sdk_smoke` enforces it with a signature scan — every public SDK function
whose parameter names an angle (`angle`, `rotate`, `tilt`, `phase`, …) must
end `_deg` or `_rad` — the SDK-side twin of `rpc_units_gate`.

### Frame-to-frame mates default to `flipped=False`

`add_mate`'s own `flipped` defaults to true: two connectors meet with their
normals opposed, which is right for two touching faces and wrong for two
designed frames. Left at the RPC default, frame-to-frame mates all solve and
turn parts end for end about their own axes. So the SDK's mate verbs pass
`flipped=False` when both connectors are frames, keep the RPC default for
face anchors, and leave alone any call that says `flipped` or `derive`
itself.

## Measurement snapshots

`body.snapshot(circular=, fields=, max_elements=)` (or the typed
`app.inspection.geometry_snapshot(part_id=, body=, ...)`) captures a frozen
record of a body's edges in one RPC. Reading `edge.center`, `edge.radius`,
`edge.length`, `edge.midpoint` or `edge.ref` then costs no RPC.

- Coordinates are **part space**, lengths mm. A circle's or arc's `center` is
  its centre, not a point on the rim. Unrequested fields and inapplicable
  radii are `None` in the SDK and absent on the wire. `ref` returns a fresh
  dict.
- A snapshot needs a stable `part_id` and an explicit body index; it never
  switches tabs and does not take a slot index. Its revision (`session`,
  `part_id`, `instance`, `version`) is invalidated by reopening, replacing or
  regenerating the part — conservatively, even a regeneration that reused
  cached geometry. Moving an occurrence does not change part-space
  coordinates.
- `snapshot.validate()` is one RPC that raises `stale_snapshot`; it is a
  point-in-time check, not a lock, and references taken from a snapshot are
  resolved against the then-current document when passed to a modelling
  call. `snapshot.refresh()` returns a new snapshot of the same body index.
- `max_elements` (default 2000, at most 10000) bounds the edges scanned,
  including those the `circular` filter excludes; a larger body is refused
  with `budget_exceeded`, never answered partially.
- The existing `Edge.center` stays a live query.

Faces, world space, pagination and guarded writes are not covered
([../roadmap.md](../roadmap.md#6-the-agent-interface)).

## Validation stages

`tools/designer_validation.py`:

```python
from designer_validation import validate
report = validate(app, stage="preview")
report = validate(app, stage="local", assembly=rig)
report = validate(app, stage="full", assembly=rig)
report.current(app)          # re-checks the session revision
```

- **preview** — every part has valid B-Reps, no feature errors, at least one
  body, no stale derived parts, and no unresolved occurrence references.
- **local** — preview plus the exact standing clash sweep over the whole
  visible assembly (`interference mode=exact`). It does not narrow to the
  changed part's neighbours.
- **full** — local plus every motion study defined on the assembly. Each must
  be set to exact collision checks; a study that is not is refused rather
  than run weaker. No study at all is `not_checked`, not accepted. A study
  covers its samples, not the continuous trajectory.

A report separates `geometry`, `standing` and `motion`, each `passed`,
`failed`, `incomplete`, `not_checked` or `stale`; only a passing full stage
sets `accepted`. The report is bound to `validation_revision` (every model
tab's revision plus unresolved references); `report.current(app)` marks it
`stale` if anything changed since. The stages activate each part in turn, so
run them on your own instance. Hidden or suppressed occurrences are outside
the sweep. A project with a construction-only part (no bodies) fails the
preview stage.

The motion tool window's Preview and Validate runs are the GUI side of the
motion stage; they are described with motion studies in docs/assemblies/.

## Generator-owned components

`tools/designer_components.py` rebuilds one generator-made part in place,
keeping its part id, feature ids and occurrences:

```python
from designer_components import claim, plan

part = build_housing(app)
claim(part, namespace="my-generator", key="housing", fingerprint=inputs_hash)

with plan(app, namespace="my-generator", key="housing",
          fingerprint=new_inputs_hash, builder=build_housing) as change:
    print(change.report)       # the target is still unchanged
    housing = change.apply()   # one undoable change
```

- The fingerprint must cover every input and dependency of the builder; when
  it matches the stored one, the builder is skipped entirely.
- Ownership (`namespace`, `key`, `fingerprint`, a recipe hash) is saved in the
  part document. An existing part is owned only by an explicit `claim`,
  never by a matching name; a part a human has edited since it was generated
  is `modified` and is refused.
- A changed component is built into a disposable staging part. Feature ids
  are matched by (type, name, ordinal among duplicates), so a builder must
  keep those stable. `update_component` validates the staging geometry,
  solves copies of every dependent assembly, and checks both revisions again
  on apply. Apply and undo reuse the staged geometry (`geometry_reused`).
- Refused: retiring an existing feature, parts carrying assets,
  configurations, studies or a rollback, and mismatched document units.
  Leaving the `with` block deletes the staging part, including after a
  failure.
- The builder proxy is an adapter for trusted generator code, not a sandbox.

`component_update_smoke` gates it.

## Other clients

`tools/designer.py` is a smaller fluent client (`d.sketch(plane="xy")
.line(...).add()`, `d.extrude(sk, depth="w / 8")`) over the same RPCs,
tested by `pyclient_smoke`. New code uses the SDK.
