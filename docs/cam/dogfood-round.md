# CAM dogfooding — the coupon, and two rounds

> Status: **both rounds done** (2026-08-22) — findings at the end. Two rounds
> against one real part: once over the RPC, once by clicking. Fifteen defects,
> all but two fixed. **Nothing has been cut yet** — the last section is what to
> watch for when it is.
>
> Everything CAM has shipped — C1–C9 and G1–G4 — had been verified against
> simulated material and nothing had cut anything. That is what this was for.

## The part: a spoilboard hold-down clamp

Chosen so the coupon is also a thing worth keeping, and so every operation
appears for a *reason* rather than because a test needed it.

- **Stock:** 18 mm plywood or MDF, 130 × 70 rough, faced to 17 mm.
- **Tools:** 6 mm flat (T1), 3 mm flat (T2). Feeds are the user's — the app
  does not guess them.
- **Part:** 120 × 60 × 17 mm.

| feature of the part | what it exercises | why it is there |
|---|---|---|
| Skim the top, 1 mm | **Face** (C3), stock margin above the part | A rough board is not flat; this is the pass that makes Z zero mean something |
| 40 × 20 pocket, 6 mm deep, with a 10 mm island | **Pocket → auto-**Adaptive** (C8), islands, **helix entry** (C6) | 6 mm deep needs a helix: the first ring is ~96 mm and a 3° ramp wants ~114 mm. The island is the boss the bolt washer bears on |
| Inside corners of that pocket | **leftover paint** (G4) | A 6 mm cutter leaves 3 mm radii in a square pocket. The verifier should say so and the part should show it |
| 12 × 44 through slot, closed at both ends | **Profile inside** (C2), through-cut into the spoilboard | The bolt slot — the reason a hold-down is adjustable. Closed rather than open-ended: a notch that reaches the edge is part of the part's own outline and gets cut with it, so it would exercise nothing |
| 2 × 6 mm holes | **Drill** (C2) | Alternative bolt positions |
| Outline, through, 4 tabs | **Profile outside** (C2), **tabs**, **arcs** (C7) | The part must not break free under the cutter. Its rounded corners are the arcs |
| Two setups (T1 then T2) | tool change, `M0` pause, per-op tool | The one place a Mach3 file differs most from a LinuxCNC one |

**Not on this part: the 4th axis (C9).** A plywood clamp has no business on a
trunnion, and the mill has no rotary yet. C9 is verified headless in the same
round instead — an indexed setup, posted and verified in its own frame.

## Round 1 — over the RPC (the plan)

Own scratch port, `headless-preview.sh`. The sequence a person actually runs,
in order, because that is where the last three rounds of bugs lived — not in
the features taken one at a time.

1. Build the part from features (regenerable script in `scratch/`).
2. `add_cam_setup`, pick Z zero from the top face, face the top.
3. Pocket from its floor — assert it chose **adaptive** and that no depth was
   typed.
4. Slot, holes, outline with tabs.
5. `cam_verify` after **each** operation, not only at the end.
6. `cam_post` to both dialects; `fit_arcs` off and on; diff the two files.
7. An indexed setup, verified in its own frame.
8. Read the G-code by eye. Every line eventually moves a machine.

**Acceptance:** the posted file cuts the designed part per the verifier, with
no gouge, no rapid through stock, and leftover only in the corners a 6 mm
cutter genuinely cannot reach.

## Round 2 — by clicking (the plan)

The same part and the same job, entirely through the GUI, with screenshots
looked at rather than counted. It will find different things: the RPC cannot
tell me a ghost is drawn in the wrong place, and the GUI cannot tell me the
RPC misreports an operation type. Both of those were real bugs this month.

Particular attention to what only a human hits: whether the verbs are
reachable without reading this document, whether the refusals say what to do
next, and whether the picture matches the file.

## Round 1 — over the RPC, 2026-08-22 (as built)

The whole job, built by pointing at the part, verified after every operation
and read by eye at the end. Nine defects. Four of them break a cutter or throw
a part; all nine are fixed, each with a test (commit `69429af`).

**A tab was a quarter of the height it was asked for.** Tabs were emitted on
the last pass only, so every earlier pass — and the descent ramp, which takes
most of a lap — ran through the tab region and took the top off it. A 3.8 mm
tab came out 1 mm tall. That is the one failure tabs exist to prevent: a part
that breaks free under the cutter and is thrown. `EmitLoop` now treats the ramp
and the lap as one function of arc length, so a tab is a pillar from the bottom
of the cut to its stated height and nothing crosses it.

**A drilled hole was never drilled in the simulation.** A plunge does not
travel in XY, so the closest-point parameter along its path is 0 everywhere and
the simulated tool stayed at the height it started from. Every hole came out
full of stock, and the retract out of it was then reported as a rapid through
material.

**A bounding box grew ~4 µm the first time anything meshed the shape.**
`BRepBndLib::Add` prefers a triangulation when one exists and inflates by its
deflection. CAM derives the stock, the rapid height and every depth from that
box, so a job generated before the first section was verified against a stock
taller than the one it was cut for — and twelve legitimate rapids to the top of
the stock were reported as rapids THROUGH it. The check that must never cry
wolf, crying wolf twelve times.

**A file never turned the spindle back on.** `S…M3` was emitted only when the
speed changed, so two tools at the same rpm produced a manual tool change —
done with the spindle stopped — followed straight by a cutting move. Found by
reading the file, not by any check.

The rest, in one line each:

- a through cut stopped at the bottom of the PART rather than the stock,
  leaving it welded to the sheet (the original finding 2);
- an inside profile with no target silently offset the part's own outline
  inward and cut a 15 mm trench round it (finding 4) — it refuses now;
- a pocket built over the RPC kept `Side::Outside` invisibly, and adaptive is
  the only strategy that reads it, so it cleared outside a boundary with
  nothing beyond it and said "this operation produced no toolpath";
- a 3 mm tool plunged into a 6 mm bore left the hole half cut and said
  nothing;
- the drill fed from Safe Z instead of rapiding to the top of the cut.

Two gaps were closed rather than fixed. **The verbs are reachable over the
RPC**: `add_cam_op` with no `top_z`/`bottom_z` derives them through the same
`cam::DeriveOperation` the GUI's verbs use, and `cam_derive_op` is the dry run.
An agent had a worse interface than a person, which is the wrong way round for
this project. And **an opening that goes all the way through has no floor to
click**, so nothing could target one: `PickedWallContour` makes a click on a
WALL mean the contour it belongs to, the depths it spans, and inside-or-outside
from whether it bounds an opening or the part.

`cam_verify` now says how MUCH is left over and in how many places, lists the
biggest regions, and returns the panel's own sentences. "18 mm at X84 Y8" reads
the same for a corner radius and a forgotten operation.

### The coupon, as it verifies now

    gouge 0.000 mm, rapids through stock 0, 304 mm2 of leftover in 15 places
      228.6 mm2  18.00 deep at X  90.1 Y 29.9   the slot's slug — it drops out
       16.9 mm2   3.80 deep at X  72.7 Y -0.9   a tab, at its asked-for height
       16.8 mm2   3.80 deep at X  -0.8 Y 17.3   a tab
       16.0 mm2   3.80 deep at X  47.4 Y 61.1   a tab
       15.8 mm2   3.80 deep at X 121.1 Y 42.7   a tab
        3.0 mm2   6.00 deep at X  26.9 Y 29.9   a pocket corner, 3 mm radius

A height map cannot watch a slug fall out or know a tab was asked for, so those
show up as leftover and always will. What matters is that the list is now
readable: every entry is something you can name.

**The coupon changed twice, and both times because the app was right.** The
bolt slot is closed at both ends — an open-ended notch is part of the outline
and gets cut with it. And the pocket needs a second pass with the 3 mm cutter:
the 5 mm gaps between the island and the pocket walls are narrower than the
6 mm rougher, which the verifier reported as two 50 mm² patches 6 mm deep
before anyone thought to check.

## Round 2 — by clicking, 2026-08-22 (as built)

Six more defects, none of which the RPC round could have found, and none about
geometry (commit `444b481`).

**The panel and the RPC disagreed about the same job.** The panel said "Cuts
6.00 mm INTO the part at X37 Y35 — that is scrap"; `cam_verify` on the same
document said nothing. Both were right about what they measured: a cell centre
landing on the corner of an island is inside the island for the model and
inside the cut for the tool, because both boundary tests are inclusive, so the
island's whole height came back as a gouge. The panel checks on a 0.4 mm grid
and the RPC defaulted to 0.25, so their grids landed differently. A cell is now
gouged only if it is below everything its neighbours were designed to be, and
left over only if it is above all of them.

**A new document did not take the CAM job with it.** The panel kept claiming
"Z zero at 17.000" and offering to Save G-code for a part that no longer
existed — and the armed face pick came along too, into a document where the
button that would re-arm it is disabled while a pick is live. The panel was
stuck with no way out.

**The Verbs button was clipped off the edge of the panel.** It sat third on the
"Pick top face" row, after a status message long enough ("Z zero is not set
yet") to overflow the 217 px the default dock layout gives this panel. The
advertised one-click way into CAM could not be clicked at all on a default
layout. It has its own line now.

**Refusals were not wrapped.** In the floating window one long sentence
stretched it across the whole app, over the panel and the model both; in the
docked panel it was clipped mid-sentence, losing the half that says what to do
instead. And that sentence pointed at "Advanced", which is not what the section
is called. The panel also told you to click the wrong thing: the armed prompt
read "click the top face…" whatever pick was actually live.

Two things an agent could not do at all, and now can:

- **`focus_panel {name}`.** A docked panel behind another is unreachable —
  `item_click` cannot hover what is not in front — so a script could only drive
  whichever panel the saved layout left on top, and CAM is tabbed behind Model.
  This round could not start until this existed.
- **The window layout moved out of the working directory.** `headless-preview.sh`
  gives a scratch instance its own `XDG_CONFIG_HOME` precisely so an agent
  cannot touch the human's preferences — and the agent then rewrote their
  window layout anyway, because `imgui.ini` lives in the directory the app was
  launched from and both run from the repo root.

`tools/designer_remote.py` could not call any RPC with a parameter named
`method` — `help` among them — because it collided with `call`'s own argument.
`request(method, params_dict)` is the way in.

### Left open

**The viewport picker will not return a wall inside a pocket or a through
opening.** Thirty-odd clicks into the slot and around the pocket returned
nothing at all, while the part's own outer wall and every level face pick
first time. So "click the wall" — the only route to machining a through opening
— works over the RPC and not by clicking, and the coupon's bolt slot cannot be
programmed in the GUI. This is a picking bug rather than a CAM one, and it is
the next thing to fix.

Smaller, and written down rather than fixed:

- **The standard views are named for the CAD frame, not the machine's.** A
  machinist's plan view — looking down the spindle — is "Front" here, and
  "Top" shows the part edge-on. CAM's Z is +Z; the app's up is +Y.
- **Picking the top face sets the facing allowance to zero**, so "Face the top"
  then refuses and the coupon's first operation needs a detour into Stock and
  safety. The refusal says exactly that, which is why it is a papercut and not
  a defect.
- **The verb window is placed once and stays put.** It anchors top-right of the
  viewport when it opens; change the layout, or let it grow as operations are
  added, and it can end up over the model it is asking you to click. It is
  draggable, so a person recovers.
- **`ui_items` truncates labels at 32 characters** (the test engine's own
  limit), so for a long label the `###id` is cut off — and the truncated string
  is exactly what one would paste into `item_click`, where it fails. The
  numeric `id` in the same listing always works.
- **The CAM panel's Generate / verdict / Save G-code fall below the fold** at
  720 px with the default layout, and no RPC scrolls a panel into view.

## Still not done: cutting it

Everything above is simulated. The verifier agrees with the model, the file
reads correctly by eye, and both dialects post. What has not happened is a
cutter touching plywood.

When it does, watch these, in this order:

1. **The first rapid.** Safe Z is derived (20 mm on 18 mm stock); check the
   tool actually clears the clamps, which the app knows nothing about.
2. **Z zero.** The panel says "Z zero at 17.000" — the height of the picked
   face in MODEL coordinates. The file is in model coordinates too, so the
   work offset has to put model Z 17 at the top of the material. This is the
   number to check twice before the first plunge.
3. **The tool change.** `M0`, then `S18000 M3` — verify the spindle restarts.
4. **The tabs.** 3.8 mm tall by design, and the backplot draws them. If the
   part breaks free anyway, that is the number to raise.
5. **The slot's slug.** It falls out. Decide beforehand whether that is
   acceptable or whether it wants tabs of its own.

Then write down what surprised you. That list is worth more than another
simulated round.
