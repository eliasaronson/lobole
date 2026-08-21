# CAM dogfooding — the coupon, and two rounds

> Status: **planned, not run** (2026-08-21). Two rounds against one real part:
> once over the RPC, once by clicking. Findings get fixed, and this doc gets
> an as-built section saying what broke.
>
> Everything CAM has shipped — C1–C9 and G1–G4 — has been verified against
> simulated material. **Nothing has cut anything.** That is what this is for.

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
| 12 × 60 through slot | **Profile inside** (C2), through-cut into the spoilboard | The bolt slot — the reason a hold-down is adjustable |
| 2 × 6 mm holes | **Drill** (C2) | Alternative bolt positions |
| Outline, through, 4 tabs | **Profile outside** (C2), **tabs**, **arcs** (C7) | The part must not break free under the cutter. Its rounded corners are the arcs |
| Two setups (T1 then T2) | tool change, `M0` pause, per-op tool | The one place a Mach3 file differs most from a LinuxCNC one |

**Not on this part: the 4th axis (C9).** A plywood clamp has no business on a
trunnion, and the mill has no rotary yet. C9 is verified headless in the same
round instead — an indexed setup, posted and verified in its own frame.

## Round 1 — over the RPC

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

## Round 2 — by clicking

The same part and the same job, entirely through the GUI, with screenshots
looked at rather than counted. It will find different things: the RPC cannot
tell me a ghost is drawn in the wrong place, and the GUI cannot tell me the
RPC misreports an operation type. Both of those were real bugs this month.

Particular attention to what only a human hits: whether the verbs are
reachable without reading this document, whether the refusals say what to do
next, and whether the picture matches the file.

## What to do with the result

Cut it. Watch the first rapid, check the Z-zero touch-off against what the
panel says, and stop the machine if the tool is not where the backplot said
it would be. Then write down what surprised you — that list is worth more
than another simulated round.
