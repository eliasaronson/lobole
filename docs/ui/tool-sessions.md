# Tool sessions

Every tool that collects picks and parameters — a feature being created or
edited, measure, section, a mate, a drawing dimension, a sketch operation,
the CAM verbs — runs as a session in **one floating tool window**, driven by
declarative pick slots and the one selection pipeline
([selection.md](selection.md)).

## The preview flow

A creation tool is a `Create*` action (`src/ui/CreateActions.cpp`) ending in
one `BeginPreviewFeature(state, feature)` call (`src/ui/PreviewFeature.cpp`),
which:

1. adds the feature straight to the Document (not through a command), so it
   regenerates, renders and is pickable while the user decides;
2. `SeedSlotsFromSelection` — select-first: whatever was already selected
   fills the tool's slots, greedily in slot order, oldest pick first;
3. `SelectFeature` on the new feature;
4. `ArmNextUnfilledSlot` — the first unfilled **required** slot arms for the
   next viewport click (nothing arms when the seed satisfied everything, so
   Enter commits).

While previewing, parameter edits mutate the feature and regenerate with no
undo entry. `AcceptPendingFeature` removes it and re-adds it through an
`AddFeatureCommand` (id preserved), so accept is one undo step.
`CancelPendingFeature` removes it. A pending preview lives in the document
outside the undo stack, so every tab switch cancels it before stashing the
active document (`CancelPendingFeature` before `StashActive`,
`preview/tab_switch_discards_pending`).

A committed feature is edited by double-clicking its tree row (or the
context menu's Edit): `OpenFeatureEditor` opens the same widgets in the tool
window with `commit=true`, and each edit is an undoable modify. There is no
docked Properties panel; editing is an explicit session.

## Pick slots

`PickSlot` (`src/ui/FeatureEditor.h`) enumerates every slot any tool has —
extrude end face, boolean target and tool, blend edges, shell open faces,
split knife, the CAM picks, fastener rims, sheet-metal edges, mate
connectors, the direct edits' faces (`DeleteFaceFaces`, `OffsetFaceFaces`,
`MoveFaceFaces`, `ReplaceFaceFace`) and Replace Face's target
(`ReplaceFaceTarget`). It is **append-only**: tests address slot rows as
`###slot<enum value>`. `SlotSpecsFor(feature)` returns the tool's slots as
`SlotSpec {slot, label, kinds, multi, prompt, required}`; `SlotFilled` and
`ApplySlotItem` complete the set. The three switches have no `default:`, so a
new enumerator is a `-Wswitch` build break, and that break is the per-tool
checklist.

The armed slot is `AppState::slotPick` (`SlotPickState`: feature, slot,
kinds, multi, prompt, and `miss` — why the last click did not land, shown
under the slot rows). Exactly one slot is armed. A single slot auto-advances
to the next unfilled required slot after a pick; a multi slot stays armed and
toggles items until another row is clicked, Enter accepts, or Escape
disarms. Every slot row is editable — per-item remove, click to re-pick — so a
wrong seed is a one-click fix. The feature tree fills an armed pick too: body
rows fill a Body slot, plane rows a plane pick, and a datum plane's or a
sketch's row fills Replace Face's target (`ReplaceFaceTarget`).

Conditional slots appear only in the mode that uses them (the extrude end
face for Up-to-face, the split knife face for Face-knife mode).

A slot armed by something that is not a feature — a mate, section, a CAM
verb, the fastener tool, the Align and Fit tools' points — carries a sentinel in
`slotPick.featureId`, and
`kToolSessions` (`FeatureEditor.cpp`) says, for each sentinel, whether the
session is still alive (checked every frame, so a dead session disarms) and
who consumes a pick. Adding such a session is one row there;
`test_conventions.cpp` checks every `k*Slot` sentinel appears. A session
that takes a POINT rather than an element reads where the click met the
face, `slotPick.hitPoint`, which the pipeline sets before handing the pick
on (the Align tool's floor and points, the Fit tool's seed).

## The tool window

`BeginToolWindowFrame(state, title)` opens `"<Title>###ToolWindow"` through
the shared floating-window helper (`ui::BeginFloatingWindow`,
[panels.md](panels.md#floating-windows)): anchored top-right of the 3D
viewport, re-anchored when the viewport moves, clamped inside the app, with a
border and drawn shadow. **Never bottom-centre** — a window there covers the
clicks the viewport receives. The id is the same for every tool, so tests
address rows as `//<Title>###ToolWindow/###slotN`; a ref that looks specific
(`//Hole###ToolWindow`) drives whichever session is actually open.

Only one session draws at a time. `DrawToolWindow` takes, in order: a sketch
session (or plane pick), a mate session, a drawing dimension, a drawing
annotation (a datum or a feature control frame), a detail view, the drawing's
Add view, then a feature preview; with none of those it shows the committed-
feature editor, or measure, section, the CAM verbs, the fastener tool, an
assembly editor, exploded views, motion, or simulation.

For a feature preview the window shows the slot rows and parameters, then:

- the pending feature's own **error** in red, with **Accept disabled**
  (button and Enter) — unless the failure only happened because an earlier
  feature failed, which is not this dialog's fault;
- "**Pick <slot> to continue**" and Accept disabled while a required slot is
  empty. `Feature::WhyIncomplete()` names a missing input (distinct from
  `GetError()`, a regeneration failure) and is enforced by
  `AcceptPendingFeature` and by the `add_feature` RPC, so a script is refused
  exactly what the GUI refuses;
- the feature-count refusal, Accept disabled, when accepting would take the
  part past 50 000 features (`kMaxPartFeatures`,
  [../versioning/README.md](../versioning/README.md#what-a-load-refuses)),
  which `add_feature` refuses the same way;
- "**Changes nothing - the model would be identical without this feature**"
  in amber, with **Accept left enabled**. It compares
  `CachedBodiesAfter(idx - 1)` with `CachedBodiesAfter(idx)` (a comparison,
  not a rebuild); features that never touch bodies are exempt. It warns
  rather than blocks because a no-op is sometimes right — Refine on an
  already-clean body, a draft about to be given an angle — and blocking would
  make correct tools feel broken. The silence was the bug, not the
  permissiveness.

Accept and Cancel are `###tw_accept` / `###tw_cancel`, Accept in the accent
colour (Cancel is not destructive, so it is not red). A window that applies
as you go (measure, section, the fastener tool, the editors) has **Close**
instead.

## Defaults and refusals

**A default carrying a unit — a length, an angle, a scale, a position — is
derived from what it acts on (the bore, the body's bounding box, the sheet,
the document's reach), never a literal.** A literal is correct in the
abstract and wrong against any real part: a countersink four times the bore,
a 2 mm part drawn as a dot on A4, every inserted occurrence buried in the
first, a revolve axis not in the sketch plane, a split plane through the
origin that misses the body. Examples in the code:

- `CreateSplit` puts the knife mid-body along the plane's normal, and
  re-centres it whenever the plane changes (`RecenterSplitOffset`).
- The revolve axis defaults to a world axis lying in the sketch plane.
- Sweep's profile is the latest *closed* sketch and its path the latest open
  chain or curve that is not that profile.
- Switching a Hole's type sizes the counterbore/countersink from the bore
  unless the user already set those numbers.
- Fillet and Chamfer open at a tenth of the target body's thinnest extent,
  rounded to 1/2/5 (`SizeFromBody`); Shell at a tenth of its thinnest wall
  (`SizeFromWall`, `ThinnestWall`: on a body already hollow the extent is
  not the wall), the extent where no wall measures. The target is a
  pre-selected element's body, else the newest producer's, else the newest
  valid body whatever made it -- a part made only by a sweep is measured
  too (`defaults/blend_and_wall_scale_with_body`,
  `defaults/blend_and_wall_on_a_swept_body`). With nothing to measure the
  tool does not open (the toolbar gates it on a body); Rib takes its
  thickness the same way.
- Extrude opens at the profile's smaller in-plane extent (the picked
  regions', when some are), rounded to 1/2/5 x 10^n. Switching its Mode to
  Cut while the distance is still the default takes the bodies' depth along
  the sketch's normal, so a cut opens through the part
  (`defaults/extrude_cut_goes_through_the_part`).
- A datum axis runs along X through the middle of the newest body (the
  world origin in an empty part) (`defaults/datum_axis_through_the_part`).
  A helix stands up the middle of it, its radius half the body's smaller
  XY extent and its pitch a quarter of its height, rounded, four turns. A
  pipe's outside diameter is a tenth of its path's length, rounded.
- Pattern's spacing is 1.5 times the source's extent along the pattern's
  direction, with no floor. With no source body there is nothing to repeat:
  the spacing stays 0 and the preview names the missing source
  (`defaults/pattern_without_a_source_body`).
- Picking a cylindrical face into the Thread tool sizes it: the ISO metric
  coarse size whose major diameter (a boss) or tap drill (a hole) is
  nearest the face's diameter (`defaults/thread_takes_the_faces_size`).
- Edge Flange, Hem and Jog take their lengths from the sheet-metal rules.
  With none declared they open at 0 and the preview refuses naming the
  rules, as Base Flange does (`defaults/sheet_tools_without_a_gauge`).
- Offset Face opens at a tenth of the smaller of the face's inset (how far
  its inside reaches from its edges) and the body's thinnest extent, rounded
  the same way, outward (`SizeFromFace`), derived again from the first face
  picked into an empty tool. Move Face moves a tenth of the body's largest
  extent in the plane of the face the picked ones sit in
  (`DefaultFaceMove`). Delete and Replace Face have no number to default:
  they refuse until their faces (and Replace's target) are picked
  ([modeling](../modeling/README.md#direct-edit)).
- A Cradle's clearance is the print process's gap a side and its
  resolution half of that, coarsened to the cost budget; with no process it
  opens refused, naming `set_print_process` and `clearance_mm`
  (`defaults/cradle`; [modeling](../modeling/README.md#cradle)). Ticking
  *Own clearance* seeds it from the last build, else a five-hundredth of
  the scan's diagonal.
- The Align tool's floor tolerance is a five-hundredth of the import's
  bounding diagonal, as `align_import`'s; ticking *Own tolerance* seeds the
  field with it. Its scale's true distance starts as the distance measured
  between the two picks, so nothing scales until it is typed
  ([placing an import](../interchange/README.md#placing-an-import)).
- The Fit tool's tolerance is the picked body's own noise
  (`mesh::ScanTolerance`), as `fit_primitive`'s; ticking *Own tolerance*
  keeps the value it last used. With no point it arms the pick, Accept
  disabled and the window asking for it (`defaults/fit_primitive`,
  `RefusesUntilPicked`); a sphere it fits is shown and
  refused as a datum, saying why
  ([datums on a scan](../modeling/README.md#datums-on-a-scan)).
- The placement rule (`src/ui/Placement.h`, `ClearOffset`) keeps a new item's
  span clear of the ones already taken, with a gap scaled to the item; the
  occurrence insert and drawing-view placement use it.
- A simulation force defaults to the negated outward normal of the face it is
  placed on and a magnitude from the part's own weight.
- Reference geometry is sized from the document's reach and drawn over the
  part ([README.md](README.md#viewport)).

Angles that are the same on any part are not scales, and stay literal:
Draft's 5 degrees, Edge Flange's 90. So does Mirror's world YZ plane
through the origin, which is what a symmetric part drawn there wants.

The companion rule is **refuse visibly**: a tool that cannot do anything with
its inputs says so where the user is looking, rather than committing a no-op
or failing silently.

**The `defaults/*` GUI gate** enforces both for creation tools. `kToolCases`
(`src/app/guitests/GuiTestsCommon.cpp`) lists every creation tool by its
`###tb_*` id with what it needs set up first, and registers one test per
tool, so a failure names the tool. With the tool's own defaults and no picks,
exactly one of these must hold:

- `Expect::Changes` — Accept is **offered** (the button, not only the
  outcome: a tool that opens already failing is the same bug one step
  earlier), and accepting changes the document (`FingerprintOf`: body count,
  per-body volumes, feature count) with no error on the new feature;
- `Expect::RefusesUntilPicked` — Accept is disabled and the window says what
  is missing;
- `Expect::ArmsAPick` — not a preview at all; the tool asks for a face or a
  plane first (Hole, New Sketch);
- `Expect::PickThenChanges` — asks for a face or a plane first, and given
  the Top plane of an empty part (the row in the tree), opens a preview that
  `Changes` (Box, Cylinder, Sphere: their sizes come from the scene's reach
  when the reference has nothing of its own to measure);
- `Expect::HarmlessNoOp` — accepts cleanly and legitimately changes nothing.

"Enabled button, accepts, changes nothing" is the failure it exists to catch.

**`defaults_scale/*`** judges what the gate cannot: whether a default suits
the part in front of it. One case per tool in `kToolCases` that opens a
preview on its defaults sets the same document up at two sizes (`SetupFor`'s
`k`: a 2 mm part and a 300 mm one), fits the view to the part, opens the
tool and measures the preview -- the material it adds or removes (the
bodies it made, where it only re-cuts), and the reference geometry drawn
for it. It must lie within 2.5 half-widths of the fitted view's centre (a
pattern's third copy reaches 2.05) and be no thinner than a hundredth of
the part (a solid by its thinnest side, a line or plane by its longest).
Both sizes are measured and logged before a case fails. A literal default
passes the gate at any one size and fails here at the other: the 1 mm
extrude, rib and 2 mm pipe were slivers at 300 mm, the 3-unit helix and the
datum quad drawn round the world origin out of view at 2 mm.
The drawing tab's sessions (datum, frame, detail) pick on the sheet rather
than in the model, so they are not in `kToolCases`;
`defaults/drawing_sessions_wait_for_a_pick` holds them to `RefusesUntilPicked`
(and Enter does not take a refused Accept), and their derived defaults are
asserted where they are driven ([../drawings/README.md](../drawings/README.md#in-the-gui)).
`defaults/no_op_warns_without_blocking` pins the amber warning.

## The Hole tool

A sketch session and a feature preview cannot coexist (the sketch session has
priority in the tool window), so Hole is staged:

```
[idle] --tb_hole----------------------------> A: face pick
       --tb_hole + planar face selected-----> B            (A skipped)
A --click planar face--> B      (a point sketch on that face)
A --click curved face--> A      (stays armed: "that face is curved - pick a flat one")
B --click--> B                  (Point tool places centres)
B --Enter / check / Shift+H--> C (BeginSketchHole; refuses with no points)
B --Esc--> [idle]               (CancelHoleSession, tiered)
C --Accept--> committed         (one undo step)
```

- `SketchPurpose` (`Freeform` / `Hole`) is a **parameter** of
  `BeginSketchOn{Plane,Datum,Face}`, applied after `BeginSketch` resets
  `SketchEditState`, because a flag set before the call would be wiped. Being
  inside `SketchEditState` means every existing reset (tab switch, undo of
  the sketch, File > New) ends the Hole session with no extra bookkeeping.
- **The face must be genuinely planar.** `BeginHoleSketchOnFace` gates on
  `CADShape::FacePlane`, the B-Rep answer; the tessellation's averaged normal
  would happily build a tangent plane on a cylinder.
- **Cancel is tiered.** Escape discards the sketch the tool made only when
  `HasUserGeometry(sketch)` is false (it ignores entities projected from the
  face); anything the user drew is kept. The discard is a
  `RemoveFeatureCommand`, so Ctrl+Z brings it back.
- `###tb_sk_hole` (Shift+H in a sketch) drills an existing point sketch and
  does not set the purpose, which is what makes `purpose == Hole` proof that
  the session created its own sketch.

Hole matters as a feature rather than an extruded cut: its tool solid is
built from primitives, so counterbore and countersink are expressible, and it
stamps its id into every element it creates, which drawing hole callouts
read.

## Primitives

Box, Cylinder and Sphere stand on something
([../modeling/README.md#primitives](../modeling/README.md#primitives)): what
they make is a dimensioned sketch on a face or a plane and an Extrude (Box,
Cylinder) or a Revolve (Sphere) of it, never a stored solid at a world
position. So the toolbar tool starts the way that model does, with the
reference (`src/ui/PrimitiveTool.{h,cpp}`):

```
[idle] --tb_box-----------------------------------> A: face/plane pick
       --tb_box + planar face / datum selected-----> B      (A skipped)
A --click flat face / base plane / datum (or its tree row)--> B
A --click curved face--> A    (stays armed: "that face is curved - pick a flat one")
B --edit a field--> B         (primitive::Refill + regenerate, no undo entry)
B --drag the ghost--> B       (offset u, v in the reference's frame; snaps)
B --Accept / Enter--> committed  (sketch + solid, ONE undo step)
B --Esc / Cancel / Ctrl+Z--> [idle] (both removed; the document as it was)
```

- **The pick is the plane pick**, with `SketchPurpose::Primitive`: the
  viewport, the base-plane rows and the datum rows answer it exactly as they
  answer New Sketch, and the same prompt pill and tool window name the step.
  A face must be planar by the B-Rep (`ExactFacePlane`) and named (a stable
  ref is what the sketch's `face_ref` holds); a refusal stays armed and says
  why.
- **Defaults** are `primitive::ApplyDefaults`: half the picked face's
  smaller span, a quarter of the part on a plane, and, on an empty part's
  plane, half the base planes' reach (`ScenePlaneHalfExtent`) — never a
  literal.
- **The preview is two features.** The sketch and the solid are both in the
  document; the solid is the pending feature and the sketch its
  **companion** (`AppState::pendingCompanionId`). `AcceptPendingFeature`
  re-adds both through `AddFeatureCommand`s and coalesces them
  (`CommandStack::CoalesceSince`) into one step; `CancelPendingFeature` and
  every tab switch remove both.
- **The window edits the spec** (`AppState::primitive.spec`): width, depth,
  height or diameter, offset u/v, centered (Box: the offset is the centre or
  the low corner, and toggling it keeps the box where it is), mode
  (new body / add / cut) and reversed. Each field takes an expression and
  keeps its text. The Extrude's own height arrow
  ([viewport-handles.md](viewport-handles.md#preview-handles)) still works;
  the window reads the height back from it.
- **Dragging the ghost** moves the offset in the reference's frame: a press
  on its footprint (on the plane it stands on, or on its far cap) grabs, and
  the offset follows the cursor's point on the plane. Each axis snaps on its
  own within 10 px: to the reference's centre (u, v = 0), and on a face to
  its outline — a side flush with the face's edge, or the centre on the edge
  line. A ring and a label ("centre", "flush", "on edge") mark it; Ctrl frees
  it (the app's one polarity, [viewport-handles.md](viewport-handles.md#snapping-one-polarity));
  Escape mid-drag puts the offset back.

**Insert template** (`###tb_template`) borrows the same pick with
`SketchPurpose::Template`: a template stands where a primitive does, and
`BeginPlacedOn` / `BeginPlacedOnFace` (`src/ui/PrimitiveTool.cpp`) answer the
pick for whichever tool armed it. A pick opens a preview instance with the
template's own saved values; with an empty library the pick's window says so
and every pick refuses ([templates](../modeling/templates.md#in-the-gui)).

The stored `BoxFeature` / `CylinderFeature` / `SphereFeature` remain only so
old documents load and as test fixtures (`AddFixtureBox` in the GUI suite);
the feature editor still edits one that a file brought in.

## Sketch sessions

The sketcher's tool sessions use the same tool window: Mirror, Sketch Fillet,
Sketch Pattern, Offset, Linked Copy, Reference Image, Calibrate Image,
Dimension and the Hole's point sketch. Select and the drawing tools need no
window; the prompt pill is enough. There is no Sketch panel: its toggles
(auto-constrain, constraint glyphs) are on the sketch toolbar, Extrude,
Revolve and the finish check mark (`###tb_sk_finish`) are on the sketch
toolbar, and two sections sit at the bottom of the Model panel while a sketch
is edited (`DrawSketchModelSections`): **Operations** (reference images,
patterns and linked copies as rows; a double-click opens the operation's edit
session) and **Constraints**.

Three families of sketch tools share one interaction grammar:

- **collect** tools (Mirror, Pattern, Offset): click toggles, drag
  box-selects, a tap in empty space or Enter continues;
- **act-per-click** tools (Fillet, Trim): each click acts; a tap in empty
  space or Enter finishes; Trim also takes a press-and-drag from empty space
  that trims every span the path crosses, one undo step
  ([trim](../sketching/README.md#trim));
- **draw** tools chain until Escape.

Applying returns to Select. A preselected sole construction line is the
mirror axis, whether Mirror is armed from the toolbar or the keyboard (both go
through `SetSketchTool`).

**Select-profile-first.** `BeginSketchExtrude` / `BeginSketchRevolve` seed
the Profiles slot from the sketch selection: loops containing any selected
entity become the feature's `regions` (nothing selected = the whole sketch).
Extrude and Revolve are also on the model toolbar for a finished sketch; they
take the selected sketch, else the newest with a closed profile.

One prompt-pill renderer serves both the sketch (DOF status, the staged step
prompt, action messages) and model space (the armed prompt). An **empty**
sketch reports no DOF status at all (`dof = -1`), rather than "Fully
defined": zero degrees of freedom is arithmetically true of an empty system,
but the pill answers "am I done constraining?".

The sketch's own entity selection stays in `SketchEditState`
([selection.md](selection.md#one-selection-set)).

## Tests

- `defaults/*` (one per creation tool), `defaults_scale/*` (the same tools at
  2 mm and 300 mm), the derived-default and refusal cases named above,
  `defaults/no_op_warns_without_blocking`,
  `defaults/extrude_symmetric_from_the_tool` (End = Symmetric in the tool
  window sweeps half the distance each way).
- `primitive/face_pick_drag_snap_accept` (the ghost on the face, a free
  drag, flush and centre snaps, Accept: a DOF-0 face-attached sketch and a
  27 mm³ extrude, one Ctrl+Z takes both), `primitive/escape_leaves_document_unchanged`,
  `primitive/curved_face_refused`, `primitive/selected_face_is_the_reference`,
  `primitive/fields_rewrite_the_preview`.
- `hole/face_pick_to_accept`, `hole/preselected_face_skips_pick`, and the
  other `hole/*` cases (Enter opens the params without accepting, the tiered
  Escape and its undo, curved-face refusal, drilling existing points).
- `shell/open_face_pick_hollows_the_box` (error line and disabled Accept),
  `draft/face_pick_names_the_body`, `split/default_plane_cuts_the_body_in_two`,
  `split/switching_the_plane_re_centers_the_knife`,
  `refine/merges_the_seam_left_by_a_union`,
  `sketch/model_toolbar_extrudes_finished`, `preview/tab_switch_discards_pending`.
- `tests/test_actions.cpp`: an empty sketch reports no DOF status.
