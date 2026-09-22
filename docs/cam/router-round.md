# The router shop's list (R1–R7)

> Status: **R1–R7 SHIPPED 2026-09-02** (formats v78–v82) — see
> [As built](#as-built). Planned the same day. From: *"what are we missing that other
> CAM software has or what can we make better"* → *"Lets do that."* The
> small things a router user hits on their first real part, each a day or
> less, ranked by how often they bite. Surveyed the same day against FreeCAD
> CAM (DogboneII, LeadInOut, RampEntry, Helix, Drilling, the GRBL post) and
> Kiri:Moto (op-area, op-drill, op-helical, export, the GRBL device) in
> `~/Documents/CAD`. Sits after [job-document.md](job-document.md) round two;
> the bigger items — repeats, tiling, nesting, 3D — stay where those plans
> put them.

## What we have

Profile with tabs, pocket (concentric), adaptive (libarea), face, drill,
edge break; a 3° ramp entry with a helix when the loop is too short (C6);
arcs fitted at post time (C7); `stockToLeave` per operation; two post
dialects, **mach3** and **linuxcnc**, a manual tool change posted as a
retract, `T M6`, then `M0 (change tool, set Z zero, then cycle start)`, and
`S M3` again after it; a verifier that runs on its own and a verdict in
words; a job document with machine profiles, placement, per-part fixturing.

## What is missing, and what the survey said

| gap | who has it | what they do | what it decides here |
|---|---|---|---|
| **GRBL post** | FreeCAD `grbl_post.py` (deprecated for a generic `Generic_Grbl.fcm`); Kiri:Moto `Any.Generic.Grbl.json` | FreeCAD: *"Default to not outputting M6 tool changes (comment it) as grbl currently does not handle it"*, no `G43`, arcs as incremental IJ, `--translate_drill` because GRBL has **no canned cycles**. Kiri:Moto emits an inline `M6 T{tool}` and nothing else — which a stock GRBL ignores | **R1**: a `grbl` dialect: `G21 G90 G17 G94`, no `M6`, no `G43`, a tool change is `T<n>` · `M5` · a message · `M0`, arcs IJ (we already write incremental IJ), and nothing to translate because our drill is already plain moves |
| **File per tool** | FreeCAD `SplitOutput` with `OrderOutputBy` | one program per tool, named for it | **R2**: `Setup::splitOutput`; forces tool order; N files beside the project, each complete |
| **Holes bigger than the tool** | FreeCAD `Path/Op/Helix.py`: turns = `ceil(height / min(2πr·tan(angle), pitch))` as two 180° arcs each, then a flat finish circle; annulus of radii from outer to inner by `StepOver`; a hole smaller than the tool is *"Skipped hole at position"*. Kiri:Moto: a separate `helical` op, the drill op never bores | the refusal the user hit ("an 8 mm hole with a 6 mm tool") is a bore, not a refusal | **R3**: the Drill verb derives a **helical bore** when the hole is wider than the tool, and a plunge when it is not |
| **Peck drilling** | FreeCAD G83/G73 via `DrillCycleExpander.py` (G83 retracts fully to R each peck; G73 to `next + 5 % of peck`); Kiri:Moto expands pecks itself (`camDrillDown` 5 mm, dwell, lift) and emits **no G81/G83 anywhere** | pecks as plain moves, since neither GRBL nor our post speaks cycles | **R3**: `Operation::peck`, expanded moves, default derived from the tool |
| **Dogbones** | FreeCAD DogboneII: styles dogbone / T-bone H, V, long, short; kink direction vs `Side`, **length = tool radius** (adaptive by default); Kiri:Moto: true dogbone only, at any turn over 45°, length **`toolDiam / 5` hard-coded** | the relief is the tool's radius along the bisector, or it does not clear the corner — Kiri's fifth does not | **R4**: `cornerRelief` on profile and pocket: none / dogbone / T-bone, inside corners of the material, length = tool radius |
| **Lead-in / out** | FreeCAD LeadInOut: Arc by default, 45°, radius bound to **`Tool.Diameter/2 × 1.5`**, applied to every cutting run; Kiri:Moto: none (a global ease-down spiral instead) | an arc lead is what stops the dwell mark where the cut starts and ends | **R5**: tangent arc lead in and out on profiles, radius from the tool, refused where it would not fit |
| **Rough then finish** | FreeCAD: `OffsetExtra` on Profile, `ExtraOffset` on pockets, `FinishDepth` as one extra Z step; only Adaptive does both in one op, with a TODO that the finish step *"gets applied to each region separately"*. Kiri:Moto: `rough` leaves `camRoughStock`, `outline`/`contour` finish with their own tool | two operations, one intent | **R6**: an operation can name a **finishing tool**: the rough leaves stock, the finish takes it in one full-depth pass |
| **Undo** | FreeCAD: every job/op/dressup edit is a document transaction (`openTransaction("Edit Job")`) | a job that cannot be undone is the odd one out in this app | **R7**: `SnapshotJob` before every mutation, the drawing pattern |

## The fix

```
cam::Post dialect "grbl"                              // R1
cam::Setup   { bool splitOutput; }                    // R2  (v78)
cam::Operation {
  double peck = 0.0;                                  // R3  0 = one plunge
  // Drill on a hole wider than the tool: a helical bore, derived — no field.
  CornerRelief cornerRelief = None;                   // R4  None|Dogbone|TBoneX|TBoneY
  bool leadArcs = false;                              // R5  tangent arc in and out
  int finishTool = 0; double finishStock = 0.0;       // R6  0 = no finishing pass
}
ui::SnapshotJob(state)                                // R7
```

**Every default is derived** (docs/ui/u10-defaults-and-refusals.md): the bore
pitch from the ramp angle and the stepdown, the peck from the tool's
diameter, the relief length from the tool's radius, the lead radius from the
tool's radius, the finish stock from the tool's diameter. Where a value
cannot be derived the verb refuses and says what would let it.

## Decisions

**GRBL gets its own dialect, not a flag on mach3.** Kiri:Moto's token
templates make every controller a device file; FreeCAD moved the same way
(`Generic_Grbl.fcm`). Ours stay a table row with three behaviours, because
three rows is not yet a design. A fourth controller is the moment to revisit.

**A tool change on GRBL is M5, a message, M0.** GRBL ignores M6 unless built
for it, and Kiri's inline `M6 T2` runs straight into the next cut with the
wrong tool. `M0` is a feed hold every GRBL honours; the operator changes the
tool, re-zeroes Z, and presses cycle start — which is exactly the sentence
the mach3 dialect already posts.

**A drill on a hole wider than the tool is a bore, derived, not a fourth
verb.** The user pointed at a hole; the tool decides how it is made — the
same rule Break an edge already follows (ball rounds, V-bit chamfers).

**Relief length is the tool's radius.** FreeCAD's nominal, and the only
length that clears the corner. Kiri:Moto's `toolDiam / 5` leaves four
fifths of the radius uncut. T-bone runs along the longer edge, where the
joint hides it.

**Leads are arcs, tangent, on every pass.** FreeCAD applies its lead to
every cutting run and offers eleven styles; one tangent arc covers the case
that matters on a router (the dwell mark). Refused, not shortened, where the
opening is too small for it — a lead into the wall is a gouge.

**Rough and finish are one operation with two tools.** Two operations that
must agree about a boundary drift apart; one that says "leave 0.6, then
finish with T2" cannot. The generator emits two toolpaths from it, and tool
order groups them.

**Undo is a full snapshot.** A job is a parts list and a few setups; the
drawing's snapshot stack is the pattern and it costs nothing at this size.

## Phases

### R1 — the GRBL post (~½ session)

A `grbl` row in `kDialects`: preamble `G21 G90 G17 G94`, no `G43`, no `M6`;
tool change = retract, `T2`, `M5`, `(change to T2: 3 mm flat — set Z zero,
then cycle start)`, `M0`, then `S… M3`; end `M2`. Nothing to translate: the
drill already posts plain moves. The machine profile's post combo lists it.
Test: a two-tool job posted for grbl has no `M6`/`G43`/`G8x`, one `M0` with
the message, arcs as `G2/G3 … I J`; the verifier reads it back identically
to the mach3 file (the same cut, in two dialects).

**The bare `T<n>` came later** (2026-09-09), and the reason is worth keeping:
M6 is the word GRBL ignores, but T alone it parses and acts on by moving
nothing. Leaving it out made the operator's message the ONLY thing in the
file that named the cutter — so every reader of a grbl program, this
project's verifier included, had to assume tool 1 after the first change.
It simulated a 2 mm cutter's slots with a 3.175 mm one and called a correct
job scrap (docs/modeling/fpv-drone.md, finding 1).

### R2 — file per tool (~½ session, v78)

`Setup::splitOutput`; when set, `order` is forced to `Tool` (the panel says
so) and Save G-code writes `<project> - <setup>-T<n>.nc` per tool, each a
complete program with its own preamble and end and no tool-change block;
the panel and `cam_post` name every file. RPC: `split_output` on
`set_cam_setup`; `cam_post` returns `files: [{tool, path, gcode}]`. Golden
with the flag. Test: a two-tool job splits into two files that each verify
clean and together remove what the one file did.

### R3 — holes bigger than the tool, and pecking (~1 session, v79)

`cam::DeriveOperation` for Drill: hole diameter from the picked cylindrical
face; when `hole > tool + 2·stockToLeave` the operation is a **helical bore**
— radius `hole/2 − tool/2`, pitch `min(2π·r·tan(3°), stepdown)`, turns as
two 180° arcs each, a flat finishing circle at the bottom; when the hole
exceeds twice the tool, an annulus of radii from outer to inner by the
stepover, finishing with the outer. Verified against the model like every
other op (the bore must not gouge; leftover in the hole must be nil).
`Operation::peck`: retract fully to the safe height between pecks (FreeCAD's
G83 reading, the one that clears chips), default `tool.diameter` when the
depth exceeds three diameters, else none. The 8 mm-hole-with-a-6 mm-tool
refusal becomes a bore; a hole *smaller* than the tool stays a refusal.

**The depth a drill derives** (2026-09-09). Top from the STOCK, so an entry
rapid is above the material. Bottom from the bore's own extent — and a bore
that reaches the bottom of the PART goes on down by the point's own height
(`TipHeight`), because a drill is a cone: stopping its lowest point at the
far face leaves a ring of material the shape of that cone and the hole is
not a hole. Zero for a flat mill, the cone for a drill or V-bit, the radius
for a ball; a BLIND hole keeps its floor, where the cone IS the bottom of
the hole. "Reaches the bottom" is measured against the MODEL's bounds: the
stock's bottom plus its bottom margin recovers that only for relative stock,
and on a fixed sheet a few microns of bounding-box difference then decided
whether a hole broke out at all (docs/modeling/fpv-drone.md, finding 8).

**The rapid check knows the same shape.** A rapid is flagged when it passes
through material, and it used to model every tool as a cylinder of its own
diameter while the CUT used `TipHeight` — so a drill retracting straight up
the hole it had just made found its own cone in the way, and every through
hole in a job reported a rapid through stock. Both sides ask one function
now (docs/modeling/fpv-drone.md, finding 3).

### R4 — dogbones (~1 session, v80)

`Operation::cornerRelief` on profile and pocket. A corner qualifies when the
material is on the inside of the turn — the corner a round cutter cannot
reach — computed on the offset path from the turn direction and `side`,
FreeCAD's rule. Dogbone: two points along the bisector at the tool's radius.
T-bone: the same length along the longer adjacent edge. Off by default: a
relief is a decision about the joint, not a fact about the part. Verified:
the leftover the verifier reports at inside corners goes to zero with a
dogbone and the gouge stays zero.

### R5 — arc lead-in and lead-out (~½ session, v81)

`Operation::leadArcs` on profiles: a tangent arc of the tool's radius into
the first cut of every pass and out of the last, the ramp still doing the Z.
Refused, saying why, when the arc would leave the region the cut is allowed
in — an opening narrower than three tool radii. Verified: the verifier's
gouge stays zero with leads on.

### R6 — rough then finish (~1 session, v82)

`Operation::finishTool` and `finishStock` (default `tool.diameter / 10`).
The generator emits the roughing toolpath leaving `finishStock`, then a
finishing toolpath with the finishing tool: one full-depth pass along the
final boundary. The verb window offers it as "then finish with …" under the
tool row; the panel's operation row shows both tools. Verified: the finished
part has no leftover along the walls, and the roughing tool never reaches
the final boundary.

### R7 — job undo (~½ session)

`ui::SnapshotJob` before every mutation of the job — panel fields, verbs,
placement drag and drop, RPC handlers — pushing a full `JobDocument` copy to
`JobHistory` the way `SnapshotDrawing` does; `edit.undo`/`edit.redo` route to
it on a job tab; the RPC `undo` too. Test: a dragged placement and a verb-made
operation each undo and redo.

## Not this

- **Tiling / work offsets** — still the fixture-list feature, still deferred;
  the machine warning (job-document.md) tells the operator where the machine
  stops, which is the half that was free.
- **Clamps and hold-down keep-outs.** Wanted, not in this round: it needs a
  shape on the sheet and a verifier check, which is its own small plan.
  (2026-09-22: fixtures now exist — docs/cam/workholding-plan.md.)
- **Draggable tabs.** Kiri:Moto's are click-to-add on the face; ours are
  counted and spaced. Enough until a job shows otherwise.
- **3D surfacing, nesting, text and V-carving, probing macros.**

## Cost, honestly

~5 sessions, five format bumps (one per phase that adds a key). The
geometry risks are R3 and R4: the bore's arcs must survive the arc fitter
and the verifier's 0.4 mm grid, and dogbones on an offset path are the kind
of polygon surgery that produces a self-intersection on the one part that
was not tested. Both are gated by the verifier, which is the point of
having one.

## As built

All seven, the same day, in order. What the plan did not anticipate:

- **R1.** The Dialect table says *how* a controller takes a tool change
  (Table / Manual / Pause) rather than whether it has a tool table; the
  frozen mach3 and linuxcnc programs did not move by a byte.
- **R2.** Tool order had to become an order of *toolpaths*, not operations,
  once R6 made one operation two of them. The cutting-order combo, RPC-only
  since J5, joined the panel beside the new checkbox.
- **R3.** The bore is inner ring first, the wall last, so a through hole's
  slug goes progressively rather than dropping. The peck retracts to the
  hole's top (G83's reading), then rapids back to half a millimetre above
  the last floor.
- **R4.** The verifier has to be *told* about reliefs: each toolpath carries
  its bites and `Inspect` skips gouge inside them, or every dogbone reads as
  scrap. The test reads the machined height map at the pocket's corner,
  because the verifier's leftover is measured only where the tool went and
  a corner radius is exactly where it did not.
- **R5.** Lead-*out* only. The ramp is the entry: the tool arrives at the
  wall descending and already moving along it, so there is no dwell to
  lead into. A 14 mm slot has room for a 6 mm tool's lead; a 10 mm one does
  not, and the test had that backwards the first time.
- **R6.** Stock-to-leave is read in Z as well as on the wall, so the rough
  copy keeps the full depth or it leaves a step in the stock beyond the
  finishing tool's reach. And a one-pass through cut produced nothing —
  found because a finishing pass *is* one — fixed for every stepdown-0 cut.
- **R7.** One `JobUndoGuard`, holding a copy and the job's fingerprint at
  construction and pushing the copy at destruction if the fingerprint
  moved, covers the panel's forty-nine edit sites and every mutating RPC
  without touching them; it coalesces a drag while an ImGui item is active.
  The verbs, the picks and the gizmo snapshot explicitly.

**The next morning: cut what fits.** The machine warning said the machine
"cuts as far as it reaches and stops", and that was wrong about every
controller surveyed: GRBL with soft limits on refuses the line and alarms,
without them it drives into the stop. So the file has to stop for it.
`cam::ClipToMachine` clips every move to the travel box in X and Y — a cut
that runs out is cut to the boundary and retracts, one that comes back
re-enters at safe height and plunges to where it was — applied once in
`GenerateAll` so the backplot, the simulation, the verifier and the file
show the same shortened cut, and the verdict says how many millimetres
were left out. `cam::MachineBounds` is the one anchoring the viewport draws
and the clip cuts to.

