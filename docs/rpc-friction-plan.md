# RPC friction: what building a gripper cost

Nine items from a round that designed a parallel-jaw wrist gripper for the
SO-ARM100 through the remote surface alone. Ordered by what they cost to
work around. Every measurement here is from that round.

The first is not a missing feature. It is a feature that exists, is well
documented, and was never found — which cost more than any of the others.

---

## F1. `relation_kinds` is not discoverable, so gears look impossible

`add_relation` has shipped since v83 with `gear|chain|rack|screw|linear|cam`
and a doc that explains every one. A round that needed a gear pair never saw
it: `add_mate` lists six mate types and says nothing about coupling two of
them, `add_motion_study` says "ONE mate's degree of freedom" and stops there,
and `describe` publishes `constraint_kinds`, `extrude_modes`, `axis_kinds`
and `material_slugs` but neither `mate_types` nor `relation_kinds`.

So the round concluded gears were unsupported and stood in a SLIDER between
the two fingers — kinematically the same constraint, with one extra solution
a gear does not have (both fingers sliding the same way). The solver took
that branch the moment a sweep crossed mid-stroke, and drove tooth through
tooth until the exact clash check caught 51 mm³ of tooth-in-tooth.

**The fix.** `describe` publishes both vocabularies, from the SAME tables the
parser refuses against (`DocumentIO::MateTypeNames` / `RelationKindNames`).
`add_mate`'s doc and the unknown-mate-type refusal both name relations as
the thing that couples two DOFs. `add_motion_study` says a driver moves the
whole mechanism through relations, and warns that a sweep through a singular
configuration can come out on the other branch.

*Files:* `core/io/Serialization.h`, `core/io/SerializeAssembly.cpp`,
`remote/RemoteDispatch.cpp`, `remote/RemoteAssembly.cpp`,
`remote/RemoteMotion.cpp`. *Verified:* `tests/remote_smoke.py` asserts both
lists and the cross-reference; the gripper generator now builds two gear
relations and its loops close to 0.0 mm.

## F2. A wrong parameter TYPE is not a refusal

`set_render_options {ssao: true}` answers `[json.exception.type_error.302]
type must be string, but is boolean`. It names neither the method, nor the
parameter, nor that `ssao` takes `off|half|full`. The project's rule —
refuse, naming what IS accepted — was honoured for enums and skipped for
types, in a surface where a caller cannot read the source.

**The fix.** The dispatcher catches `json::exception` and reports which
call, what each key it was handed actually is, which keys the handler read,
and where the vocabulary is. One place, whole surface.

*Files:* `remote/RemoteUiActions.cpp`, `remote/RemoteDispatch.cpp`,
`remote/RemoteServerInternal.h`.

## F3. Two calls did not keep their own promises

- `add_mate`'s doc says `{occurrence: id|name}`. The handler resolves the
  name and then passes the CALLER's value to a parser that wants a number,
  so a name came back as a type error from a call documented to take one.
- A study face demanded `{feature_id, element}` when the element name is
  what `list_elements` hands out. The obvious repair — read the id off the
  name's prefix — is wrong: that prefix is the feature that created the
  ELEMENT, and after one boolean it is not the body's `created_by`. The
  right resolution is to find the body that HAS the element, and to refuse
  ambiguity naming the candidates.

*Files:* `remote/RemoteAssembly.cpp`, `remote/RemoteSimulation.cpp`,
`core/fem/FemStudy.cpp`.

## F4. `Import` centres every file, silently (format v97)

Shapes are translated so their combined bounding box sits at the origin. It
is a good default for a part you are about to look at, and wrong for every
import whose coordinates mean something: a neighbour part from the same
assembly, a fixture in a shared frame, a re-import of this app's own export.

Nothing said it happened. A fit check that baked an arm's wrist into the
tool frame got every part stacked on the origin, and the distance sweep then
reported **0.00 mm at every roll angle** — which reads as an interference,
not as a placement error.

**The fix.** `keep_origin` on the feature (absent = centred, as every older
file was), `recentred_by` in the call that moved it, and the behaviour
stated in the Import row `describe` returns.

*Files:* `core/Feature.h`, `core/Feature.cpp`, `core/io/SerializeFeatures.cpp`,
`core/io/SerializationInternal.h`, `remote/RemoteModeling.cpp`.
*Verified:* `tests/golden/v97-import-keep-origin.json` freezes the flag and
`test_golden.cpp` asserts it survives the round trip.

## F5. `contact_report` starts booleans it cannot finish

The deadline stops new dispatches; a boolean already running cannot be
preempted. Between modelled parts that is fine. An imported STL is one
planar face per TRIANGLE, and a sweep over two 54k-face bodies ran past ten
minutes with `deadline_s` at its default 20 before it was killed — the
caller cannot tell that from a hang.

**The fix.** A pair carrying more than `exact_face_budget` faces (default
20000) is not started and is listed in `not_checked` with its face count and
what to use instead. The same sweep now returns in 0.2 s.

*Files:* `remote/RemoteInspection.cpp`, `cad/CADKernel.{h,cpp}`
(`CADShape::FaceCount`).

## F6. A mesher refusal names a face nobody can look up

`surface meshing failed on face 13 of 90` — the numbering is OCC's explorer
order, no RPC hands it out, and the body name is the last feature that
touched the shape. The defect was real (a boolean cut clipping a gusset edge
into a sliver, which `check_geometry` calls valid and STL export ignores)
and locating it took four rebuilds and a guess.

**The fix.** The message carries the face's kind, area and centroid:

    body 'pad spigot 33': surface meshing failed on face 4 of 54 (a plane,
    area 0.00 mm2, centred at (2.50, 33.80, 7.00))

*Files:* `core/fem/FemMesher.cpp`.

## F7. `solve_assembly` reports no per-mate status

Its doc says it returns "the per-mate status"; the payload carries `id`,
`name`, and `error` only when there is one. A caller filtering on a field
that is sometimes absent reads "no news" as "not satisfied" and reports
every mate as broken.

**The fix.** `status`: `satisfied | redundant | error`, in `solve_assembly`
and `list_mates`.

*Files:* `remote/RemoteAssembly.cpp`.

## F8. `gear.py` could not generate a gear that meshes

Three parameters missing: `--backlash` (a nominal involute pair at nominal
centres has none, so two generated gears are a press fit of tooth into
space), `--phase` (two mirrored sectors whose teeth both point at the line
of centres jam tip to tip), and `--sector` (a drive plate carries teeth over
a wedge; a full gear there is material in the way of the next part).

*Files:* `generators/gear.py`.

## F9. Not fixed: name the mate whose axes are wrong

A revolute whose two connectors are nowhere near coaxial turns its part to
satisfy itself, and a loop three joints away is what fails to close — 44 mm,
naming the wrong mate. The message should name the suspect.

It cannot be measured in the solve: every `add_mate` solves, so by the time
any solve observes the placement, the part has already been turned into
alignment and the angle reads zero. The hook that works is mate CREATION,
before `add_mate`'s own solve — where `add_relation` already warns about
gear axles more than 5 degrees off parallel. Attempted at the solve, backed
out, and left as a comment where the hint belongs.
