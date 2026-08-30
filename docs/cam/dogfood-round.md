# CAM dogfooding — the coupon, and two rounds

> Status: **both rounds done** (2026-08-22) — findings at the end. Two rounds
> against one real part: once over the RPC, once by clicking. Fourteen defects,
> five papercuts and six pieces of friction in the surfaces used to drive the
> app — all fixed; two reported findings turned out not to be findings.
> **Nothing has been cut yet** — the last section is what to watch for when it
> is.
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

1. Build the part from features (`scratch/holddown_clamp.py`, regenerable — run it against any instance and it rebuilds the part and the job).
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

    ok=True (no gouge, no rapid through stock), complete=False:
    282 mm2 of leftover in 10 places, every one of them nameable
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

### The papercuts, since fixed

Round 2 left five smaller things written down rather than fixed. They were
fixed the same day; each is a line in the panel now rather than a line here.

- **The standard views are named for the CAD frame, not the machine's** — a
  machinist's plan view is filed under "Front", and "Top" shows the part
  edge-on, because CAM's Z is +Z and the app's up is +Y. Renaming the CAD
  frame would be wrong for everything else, so CAM offers the view it means:
  **Look down the spindle**, in the frame *this setup* cuts in. An indexed job
  looks down its own spindle, not the world's.
- **Picking the top face set the facing allowance to zero**, so "Face the top"
  refused immediately after the pick meant to set the job up, and the coupon's
  first operation needed a detour into Advanced to undo it. Worse, it threw
  away an allowance already typed. The pick now says where Z zero is and
  nothing about how much material sits above the part; the default 1 mm is the
  rough board a facing pass exists for.
- **The verb window was placed once and stayed put** — anchored on the frame it
  appeared, against a size `AlwaysAutoResize` had not computed yet, and left
  there. Change the layout and it sat over the middle of the model it was
  asking you to click. It follows the viewport now, and re-anchors on reopen.
  Chasing that turned up the real reason the model kept ending up underneath
  it: `FitCameraToBox` already knows to put the model in the band a tool
  window leaves — and **panned the wrong way**, pushing it under the window
  instead of clear of it. CAM's window was also missing from the list of
  windows it considers open at all. Both fixed; "Look down the spindle" frames
  the whole job in the band that is left.
- **`ui_items` truncates labels at 32 characters** (the test engine's own
  buffer), so for a long label the `###id` is cut off — and the truncated
  string is exactly what one would paste into `item_click`, where it fails.
  The listing now says `label_truncated`, and the numeric `id` always works.
- **The CAM panel's Generate / verdict / Save G-code fell below the fold** at
  720 px with the default layout, and no RPC scrolls a panel into view. They
  sit ABOVE the operations list now: the list is the part that grows with the
  job, and what the job *says* is not the thing that should fall off the bottom
  as the job gets big enough to want checking.

### The friction, since fixed

Six things the rounds noticed about the surfaces used to drive the app rather
than about CAM. All of them either produced a false finding during the rounds
or would have.

- **A screenshot taken mid-animation is a lie.** A camera move animates for
  about a second, so a capture straight after one shows a frame part way
  through the turn. Twice this round that read as a bug in whatever was being
  looked at ("standard views come out rolled", "Fit View doesn't fit"); both
  were tweens. The capture is already deferred until after a frame renders —
  it now also waits for the camera to stop, capped so an animation that never
  ends cannot hold a client.
- **`cam_verify`'s `ok` could never be true for a real job.** One flag answered
  two questions, and only one of them is ever certain. `ok` now means nothing
  scraps the part or breaks the tool — no gouge, no rapid through material —
  and `complete` means nothing is left standing. The coupon reports
  `ok: true, complete: false`, which is the honest answer for a job with four
  tabs and a slug that drops out.
- **File > New left every tool session open** on a document that no longer
  existed, including a feature-editor session naming a feature that had just
  been discarded. The one that bit was `camToolOpen`: it is a toggle, so a
  stale one turned OFF at the next click on the button meant to turn it on,
  and the failure surfaced in an unrelated test.
- **`gui-tests-xvfb.sh -f <case>` exited 0 on a failing test.** It `exec`ed
  straight through, so the status was xvfb-run's — which the script's own
  comment calls unreliable, which is why the sharded path parses the log
  instead. A filter matching nothing also read as a pass. Both are failures
  now, and the final line says "passed" rather than "run", and stale shard
  logs are cleared at the start of a run rather than left to be misread as
  current.
- **An adaptive pocket posted ~30,000 lines** with arc fitting off — the
  setting a controller that mishandles arcs is left with, and those are the
  slow ones. Straight runs now collapse to their endpoints, within the same
  chord tolerance the path was flattened at: 29,860 lines to 15,463. The merge
  runs only where the arc fitter has declined, and stops before any arc — the
  first points of a tangentially-leaving arc are collinear with the line
  feeding into it, and an unchecked run swallowed them and cost 316 arcs. The
  verifier reports the same gouge, the same leftover and the same regions to
  the digit.

### Two findings that were not

**The picker and walls.** Round 2 reported that the picker would not return a
wall inside a pocket or a through opening, and that the coupon's bolt slot
therefore could not be programmed by clicking. **That was wrong, and worth
recording as the trap it is.** Thirty clicks into the slot returned nothing
because a through opening is a hole: a ray down the middle of it passes clean
through the part and hits nothing, which is the correct answer. The walls are a
band at one edge of the opening, and how wide that band is depends entirely on
the view angle — from straight above it is zero. Aimed at the wall rather than
at the hole, the picker returns it first time (`face 14`, normal (1,0,0),
centre X84 Y30 Z8.5), and the verb builds the operation: "Cut the opening:
6 mm tool, 19 mm deep in 7 passes" with a verdict of "Machines the part
cleanly."

The lesson for driving the GUI from a script: a click that hits nothing looks
exactly like a click on something that cannot be picked. When a pick is armed
the panel does say "Nothing there", which is the difference — read it.

**Below the fold.** Round 2 also reported that a panel's lower half was
unreachable. It is not: nothing here uses `ImGuiListClipper`, so every widget
is submitted whether or not it is on screen, `ui_items` lists it, and the test
engine scrolls to an item before clicking it. Verified with a panel grown to
20 operations — the bottom item sits at y=742 in a 690-pixel window and clicks
fine. `scroll_panel` was added anyway, for the one thing that genuinely does
need it: a screenshot shows the pixels that exist, so seeing the bottom of a
long panel means scrolling it first.



## Still not done: cutting it

Everything above is simulated. The verifier agrees with the model, the file
reads correctly by eye, and both dialects post. What has not happened is a
cutter touching plywood.

When it does, watch these, in this order:

1. **The first rapid.** Safe Z is derived (20 mm on 18 mm stock); check the
   tool actually clears the clamps, which the app knows nothing about.
2. **Z zero.** The panel says "Z zero at 17.000" — the height of the picked
   face in **SETUP** coordinates: the frame the part is clamped in, which is
   model coordinates only when it is clamped as modelled. The file is in the
   same frame, so the work offset has to put setup Z 17 at the top of the
   material. Since v67 a setup can lay the part flat without touching the
   model (docs/cam/setup-frame.md), and then the two frames differ — read the
   number off the panel, never off the model tree. This is the number to
   check twice before the first plunge.
3. **The tool change.** `M0`, then `S18000 M3` — verify the spindle restarts.
4. **The tabs.** 3.8 mm tall by design, and the backplot draws them. If the
   part breaks free anyway, that is the number to raise.
5. **The slot's slug.** It falls out. Decide beforehand whether that is
   acceptable or whether it wants tabs of its own.

Then write down what surprised you. That list is worth more than another
simulated round.

## Round 3 findings (GUI), 2026-08-23

Driven through the GUI on a scratch instance, looking at screenshots rather
than counting assertions. One finding, and it is the worst kind: the feature
was unreachable, and every automated check was green.

**The model toolbar did not fit, and the tools past the edge were culled.**
At 1280x720 — the app's own default, and the size the GUI suite runs at — the
row needed **1320 px in a 1280 px window**. U4 had chosen a horizontal
scrollbar over clipping so that no button would be lost, but scrolled-out
items are not submitted at all: nothing could click them, and a human would
simply not find them. The four that fell off were the last in the row —
**Simulation, CAM, Section, Measure** — the mode-level tools.

Measured, not inferred:

| toolbar scroll | `tb_box` | `tb_planes` | `tb_cam` | `tb_section` |
|---|---|---|---|---|
| at rest | reachable | reachable | **unreachable** | **unreachable** |
| scrolled right | **unreachable** | reachable | reachable | reachable |

**CAM was the only one with no other route.** Measure, Simulation and Section
all have command-palette entries; CAM had none, and no menu entry either. So
with the toolbar overflowed, an entire workstream had no way in.

Fixed: the toolbar wraps to a second row instead of scrolling (keeping U4's
actual intent — lose nothing — at the cost of one row of height), and CAM
joins its siblings in the palette.

**The guard for it is structural, after the obvious version failed.** A test
that probes each button with `ItemExists` passes with the bug present,
because the test engine SCROLLS to bring an item into view — verified by
removing the wrap and watching it stay green. `toolbar/every_tool_is_reachable`
asserts the row FITS (`ContentSize.x <= InnerRect width`) instead, and was
verified the other way: on the unwrapped build it fails with "toolbar content
1320 px in 1280 px". New tools are appended to that row, which is exactly how
this broke, so the guard matters more than the fix.
