# CAM dogfooding — the coupon, and two rounds

> Status: **round 1 started** (2026-08-21) — findings at the end. Two rounds against one real part:
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

## Round 1 findings (RPC), 2026-08-21

The round got as far as building the coupon and running the whole job over
the RPC. Four findings, one of them the kind this exists to catch.

**1. Setups built over the RPC rapid through the material.** `add_cam_setup`
kept `Setup`'s bare 5 mm Safe Z, which is *inside* an 18 mm plate. The job
posted **68 rapids through stock**, first at line 11. The GUI got this fix
during the G-round (`DeriveSafeZ` when the panel creates a setup); the RPC
door never did, so an agent or script building a job got the tool-breaking
default that a human clicking no longer can. **Fixed** — `add_cam_setup`
derives it and returns it, and the same job now reports 0. This is the third
time a defect has hidden in "the setting whose default makes old and new
behaviour coincide"; it will not be the last.

**2. A through-cut stops at the bottom of the PART, not the stock.** The
outline was asked for `bottom_z = -1` (1 mm into the spoilboard) with the
model sitting on z = 0. `MinZ` came back **5.0**: the final pass is skipped
because `GenerateProfile` sections the *model* at each depth and there is
nothing to section below z = 0. A part cut this way stays welded to the
sheet — the exact failure tabs exist to control, arriving by a different
route. Not yet fixed: the profile should keep the last contour it found and
carry it down to `bottom_z` rather than stopping when the model runs out.

**3. The verbs are not reachable over the RPC.** `cam::DeriveOperation` — the
thing that makes the GUI's "click the floor, get every depth" work — has no
RPC. A script must supply `top_z`/`bottom_z` by hand, which is precisely the
failure mode the G-round removed from the GUI. Given this project's goal that
agents use the same interface as humans, that gap is the wrong way round.

**4. An inside profile with no target silently means "the whole outline".**
Asking for the bolt slot without an element to target cut a 15 mm-deep trench
1.5 mm inside the part perimeter, and nothing said so; the verifier reported
it as a 15 mm gouge, which is how it was found. Two things wanted here: a way
to target the slot (see 3), and a refusal when an inside profile has no
target and would eat the part.

Still to run: the pocket (its adaptive op produced nothing with a target
face, message unhelpfully generic — needs a look), the drilling, both
dialects, `fit_arcs` on and off, the indexed setup, and all of round 2.
