# Direct edit: delete, offset, move and replace faces

## The problem

A body changes only by features added on top of it. An imported STEP
solid (a bought part, team CAD, a vendor model) arrives as `Import`, every
face `<import>/face<i>`, with no history behind it, so:

- a hole in the wrong place cannot be moved, only plugged by a Boolean
  and drilled again;
- a boss, fillet or chamfer in the way (a vendor's cosmetic round, a
  chamfer that fouls a mate) cannot be removed;
- a wall cannot be made thicker, a bore larger, or a face extended to a
  datum plane or another body's face without drawing over the old shape;
- the same holds for our own parts when the feature that made a face is
  far back in a history that would take longer to unpick than the change.

Most of our dogfooding starts from STEP, so it hits this on every round.

## The shape of the fix

Four features: `DeleteFace`, `OffsetFace`, `MoveFace` and `ReplaceFace`.
Each is an ordinary history feature (not a mode, not a change made to the
Import): it takes faces by `ElementRef` on one body named by its
`createdBy` (as Shell does), changes that body in place, and is edited,
suppressed, rolled back and undone like any other. One file pair each under
`src/core/features/`; what they share (resolving faces, the checks, the
naming) is `src/core/features/DirectEdit.{h,cpp}`.

**What every one of them holds to:**

1. **It names its output through the builder's history**
   (`topo::HistoryArguments` + `topo::NamesThroughHistory`, as Shell and
   Draft are: docs/modeling/toponaming.md). A face it does not touch keeps
   its name exactly, so whatever references it keeps resolving; a face it
   moves, extends, trims or stands in for is `<name>;M<tag>`; faces it
   merges take one of their names with `;M<tag>`, as Refine does. Never
   `topo::NamePrimitiveElements`.
2. **It refuses what it cannot do, naming the face.** Before a result is
   kept: `Measure::InvalidReasons` is empty; the solid count is the
   input's; every face asked for is gone (Delete) or carries `;M<tag>`;
   the volume moves the way the operation says; the tight bbox grows by
   no more than the operation allows (Shell's "grew past its own outside"
   check). Builders run in `occsig::RunCatchingOccSignals`. A failure is
   the feature's error naming the face and what it is ("planar face
   `7/face12`, 120 mm²"), with the body left as it was.
3. **`add_feature` and `modify_feature` refuse a direct edit whose own
   Execute fails**: the command is undone and the call answers with the
   error, `revision` unchanged, as `split_for_printing` does
   (docs/scripting/rpc.md#a-refused-call-changes-nothing). An upstream edit
   that breaks a stored one cannot be refused after the fact: it is a
   broken feature, named in that edit's own `regen`.
4. **A face gone upstream is an error, never skipped**
   (`topo::UnresolvedReason`: "no longer exists", or the split message
   naming the pieces): deleting nothing is not deleting the face. The
   tree's context menu gains **Pick faces…**, as Pick edges… for a blend.
5. **A positional name is checked against what was picked.** An Import's
   `<tag>/face<i>` is an index; a changed file read again renumbers it and
   would point the edit at another face. Each face ref stores a
   fingerprint from the pick (surface kind, area, centroid), compared to
   1e-6 relative on names `topo::DecodeElementName` calls positional: "face
   `7/face12` is no longer the face that was picked (was a plane of
   120 mm² at …, now …); re-pick it". Names that say why a face exists are
   not fingerprinted, since an upstream edit legitimately resizes them.
6. **Faces belong to one body**; refs on two are refused, as Shell's are.

### Stage 1: Delete Face

`DeleteFace {body_id, faces[]}` removes the faces and heals the gap by
extending the faces around it (`BRepAlgoAPI_Defeaturing`, history on,
single-threaded): a hole's bore and floor fill, a boss levels, a fillet or
chamfer face gives back the sharp edge.

`BOPAlgo_RemoveFeatures` reports a feature it could not remove as a
**warning** (`BOPAlgo_AlertUnableToRemoveTheFeature`, with its shape) and
returns success with that feature still there: the silent substitute
exactly. So any warning refuses, its shape mapped back to names: "Delete
Face could not heal around `7/face12`, `7/face13`: the faces around them
do not meet when extended (parallel or tangent)", the two causes the
algorithm documents. A set whose removal would leave no closed solid is
refused before the builder runs.

A hole lifted out of a face only takes a loop from the host, which keeps
its own surface; on imported parts whose edges lie 1e-5 mm off their plane,
a refitted plane fails. Defeaturing trims the host's own surface; a test
pins it.

GUI: `###tb_delete_face` in a new **Direct edit** toolbar group (command
`part.delete_face`, an icon), a preview whose **Faces** slot (a new
appended `PickSlot`, multi) is seeded from the selection, `RefusesUntilPicked`
in `kToolCases`, reopened by double-clicking its row. The slot row offers
**Add the feature**: it grows the picks to the connected set bounded by
one loop of edges on a host face (a hole's bore and floor), shown before
it is added, never a silent expansion of a pick.

Format: `vNEXT`, golden `vNEXT-delete-face`.

### Stage 2: Offset Face

`OffsetFace {body_id, faces[], distance_mm (+_expr)}` moves each face
along its own normal, positive outward (adding material); its neighbours
follow along their own surfaces, so a drafted wall stays drafted and a
cylinder changes radius. Built by `BRepOffset_MakeOffset` (skin mode,
intersection joins, zero offset on the shape, `SetOffsetOnFace` per face)
and named from its `Generated`/`Modified`/`IsDeleted` history.

The builder shares Shell's `MakeShells`, whose face order follows
addresses (`nix/patches/occt-shell-cap-order.patch`), so Offset takes
Shell's guard for the unpatched apt OCCT: up to five draws on a
`BRepBuilderAPI_Copy` when a result fails the checks. Its checks: outward
never removes volume, inward never adds; the bbox grows by at most |d|;
an inward offset through the body is refused ("offsetting `4/cap1` by
−20 mm consumes the body: it is 20 mm thick there"); a face with a
tangent neighbour (a fillet) is refused naming it and suggesting both be
offset together.

Default: a tenth of the smaller of the face's `inset_mm` (the
`list_elements` field of docs/plans/action-matrix.md item 3, added here if
that has not landed) and the body's thinnest extent,
rounded 1/2/5, outward (`SizeFromFace` beside `SizeFromBody`). The window
has a flip button; a viewport handle on the face drags the distance.

Format: `vNEXT`, golden `vNEXT-offset-face`.

### Stage 3: Move Face

`MoveFace {body_id, faces[], translate_mm [x,y,z] (+_expr each),
rotate_deg (+_expr), axis (AxisRef)}` moves a set of faces forming one
feature (a hole, boss, pocket, slot) rigidly; the body closes behind it
and opens where it lands. Built from Stage 1 and named booleans:

1. `H` = the body `B` with the faces deleted (Stage 1's builder and its
   refusal).
2. The feature's material: `cut = H − B` (a hole's) and `add = B − H` (a
   boss's), through `topo::BooleanNamed`.
3. The result is `(H − T(cut)) ∪ T(add)`, `T` a `BRepBuilderAPI_Transform`
   whose history carries each moved face back to the one it was, so each
   moved wall is `<original>;M<tag>` and a thread on a moved bore or a
   mate on a moved boss follows it. The histories compose into one, as
   `topo::RefineResult` composes a clean-up onto a boolean.

The checks: volume unchanged to 1e-9 relative; a move that takes the
feature out of the body is refused ("moving `9/p3/bore` 25 mm along X
takes it 4 mm past `2/g1`"); a moved `add` must keep its contact area
with `H` or it would float. A face that does not lift out (a box's end
face) is refused, pointing to Offset Face; a slide within the face's own
surface changes nothing and is refused saying so.

Default: in the host face's plane along the body's largest extent, a tenth
of it, rounded; no rotation. The occurrence gizmo's handles
(`GizmoInput.cpp`) drive it.

Format: `vNEXT`, golden `vNEXT-move-face`.

### Stage 4: Replace Face

`ReplaceFace {body_id, face, target}` puts a face onto another surface,
its neighbours extended or trimmed along their own surfaces to meet it.
`target` is a base or datum plane, a sketch's plane, or any body's face
by `ElementRef` (its whole surface). A parallel planar target is Stage 2's
offset by the signed distance. Otherwise `BOPAlgo_MakerVolume` runs over:

- the body's faces less the replaced one;
- its neighbours extended past the target (`BRepLib::ExtendFace`, by the
  body's bbox diagonal);
- the target surface, bounded to that size.

The cells on the inner side of the untouched faces are kept and fused
(Defeaturing's own method, the target in place of the gap); the new face
is `<replaced>;M<tag>`. Checks: untouched faces keep name and geometry;
one solid; a target the extended neighbours do not reach, or one cutting
an untouched face, is refused naming that face. The target slot (a new
`PickSlot`, face or plane, fillable from the tree) is required: no numeric
default, `RefusesUntilPicked`.

Format: `vNEXT`, golden `vNEXT-replace-face`.

### Every stage also lands

- Its row in `kFeatureTypes` (the `describe` `feature_types` row and
  `add_feature`'s typed contract). Every length is `_mm` and every angle
  `_deg`, so `rpc_units_gate.py` holds them. `python/lobole/mcp.py`'s
  feature dialect and the SDK's typed specs gain it, and `rpc.py` and
  `rpc-surface.json` are remade.
- Its `VisitFeatureRefs` (the body and every face) and `VisitExprs` (each
  `_expr`), so the regen cache sees an edit, and `CopyDerivedState`.
- Its action-matrix rows (docs/plans/action-matrix.md item 4), at the
  derived defaults: every face `face.delete` and `face.delete_feature`
  (grown by Add the feature); every face whose kind offsets
  `face.offset_out`, `face.offset_in`; each such set `face.move`; every
  planar face `face.replace` onto the parallel plane through the body's
  far extent. A direct edit is never in the **fail** class: it passes or
  refuses, and the baseline is blessed with that held.

## Files

- `src/core/features/{DirectEdit,DeleteFace,OffsetFace,MoveFace,ReplaceFace}Feature.{h,cpp}`
  (new; `DirectEdit` without the suffix); `src/core/ElementHistory.{h,cpp}`
  (composed histories, an alert's shape to names); `src/core/ElementOrigin.h`.
- `src/core/io/SerializeFeatures.cpp`, `SerializationInternal.h`
  (`kVersionPending`), `tests/golden/vNEXT-*.json`.
- `src/remote/RemoteModeling.cpp` (the refusal, the rows);
  `python/lobole/{mcp,sdk}.py`.
- `src/ui/CreateActions.cpp` (`Create*`, `SizeFromFace`),
  `FeatureEditor.{h,cpp}` (slots, Add the feature), `Toolbar.cpp`,
  `Commands.cpp` and the icon table, `FeatureTreePanel.cpp` (Pick faces…),
  the viewport handle and gizmo; `src/app/guitests/GuiTestsModeling.cpp`,
  `GuiTestsCommon.cpp` (`kToolCases`).
- `tests/test_direct_edit.cpp` (new), `tests/reference_parts/44-*` on,
  `tests/fixtures/step/`, `tools/action_matrix.py` and its baseline.
- `docs/modeling/README.md`, `docs/modeling/toponaming.md`,
  `docs/ui/tool-sessions.md`, `docs/interchange/README.md` (editing an
  import), `docs/versioning/format.md`.

## Verified by

- **Unit tests** (`tests/test_direct_edit.cpp`), volumes to 1e-9
  relative, on a 40×30×20 block:
  - Delete: a Ø10 through bore, a Ø10×8 blind hole (bore and floor) and a
    10×10×5 boss each give 24000; an R3 fillet or 2 mm chamfer gives the
    sharp block; the block's own top is refused naming it, body unchanged.
  - Offset: the top at +5 gives 30000, at −5 18000, at −20 refused; a
    Ø20×20 cylinder's side at +2 gives π·12²·20; a bore at −1 becomes Ø12;
    `22-draft-box`'s top at +5 gives the drafted frustum's closed form
    (neighbours follow their own surfaces, not walls extruded square); a
    face beside a tangent fillet is refused naming it, and both together
    build.
  - Move: the bore +15 in X keeps 24000 − π·5²·20 with its axis measured
    at the new place; a move past the side is refused naming the side.
  - Replace: onto a parallel datum at z=25, 30000; onto a plane tilted 10°
    about the top's centre line along Y, 24000 with the bbox top at
    20 + 20·tan 10°; onto an R50 cylinder across the block, the rectangle
    less the circular segment, times 30.
  - Naming: untouched faces keep their names; sketches added after the
    edit on an untouched and on the edited face resolve after an upstream
    dimension change; `test_determinism` holds over the new goldens; a
    face removed upstream fails with `UnresolvedReason`; an Import whose
    positional face no longer matches its fingerprint fails naming both.
  - A pocket in a face whose edges lie 5e-5 mm off its plane deletes and
    the host keeps its surface.
- **Reference parts** (`-L reference`), one per operation: picked by
  point, a closed-form `expected.py`, checked again after a STEP round trip.
- **Real imported STEP**, committed under `tests/fixtures/step/` with
  their licences (BenchCAD and SolidCraft are MIT, OCCT's data LGPL):
  - BenchCAD's `topup_ball_knob_axial_hole_rev_orig.step` with its axial
    hole deleted must equal `…_rev_gt.step` read by the same importer
    (volume to 1e-6, the same face count);
  - `battery_holder_box_y_f130_orig.step` and `ball_knob_cylinder_radius_f130_orig.step`
    offset to match their `…_gt.step` (a Y face; the cylinder's radius
    ×1.3), each pair checked by eye first to differ by only that edit;
  - SolidCraft's `plate_100_holes.step` with all 100 bores deleted in one
    feature equals its bbox volume, within a time budget written in the
    docs;
  - a smoke beside `reference_parts`: every face of OCCT's `screw.step`
    and of the kicad demos' tact button, `SHTC3` and crystal packages
    (`kicad-source-mirror/demos/`), deleted alone and offset out and in at
    its default, passes the checks or is refused naming a face; none is a
    broken feature.
- **GUI tests**: `defaults/*` holding the four tools; a preselected bore
  deleted and accepted gives the filled block's volume; Add the feature
  growing a bore pick to bore and floor; a refused delete disabling Accept
  with the face named; Pick faces… repairing a dangling ref; Offset's
  handle drag.
- **The action matrix** rows with no direct edit in the fail class; the
  headless, GUI and pygui suites.

Not in this plan: deleting faces without healing, which would leave an
open surface body. Loböle has no surface bodies.
