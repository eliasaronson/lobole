# U10 — Defaults that do something, and refusals you can see (design)

> Status: **SHIPPED 2026-08-14** — one round
> (C1 `9bdb3c4`, C2 `7ae08de`, C3 `e49d238`, C4 `ae9793a`, C5 `bd03f5d`,
> C6 `31fff13`, acceptance fix `79ff834`, docs this commit).
> gui suite 125/125; headless 572/572.
> Decisions recorded at sign-off: (a) a no-op **warns, never blocks**;
> (b) scope covers the defaults work, the refusal gaps and the test
> infrastructure, plus sketch U/V labels and nav-cube hotspots — the
> save-mode file browser for Export stays deferred.
> Parent: [plan.md](plan.md) U10. Predecessor:
> [u9-gui-parity.md](u9-gui-parity.md).

## Problem

Three clicks-only dogfood rounds (U9) kept finding the same two shapes, in
five separate places, and each was fixed one at a time:

**Defaults that are valid but useless.** A hard-coded constant carrying a unit
is correct in the abstract and wrong against any particular part:

| Default | What it did |
|---|---|
| `cbDiameter = 2.0` | a Ø0.5 hole switched to Countersink cut a crater 4× the bore |
| view `scale = 1.0` | a 2 mm part was a 2 mm dot on A4 |
| occurrence `position = (0,0,0)` | every inserted part buried in the first |
| Split's XY plane at the origin | missed any body not at the origin |
| revolve axis = world Y | not in the plane of a top-plane sketch, so the tool opened already failing |
| sweep profile = "the latest sketch" | that is the PATH the user just drew |

**Silence where a refusal belongs.** A fillet with no edges committed a no-op
feature; Shell accepted OCC handing back the input solid; a click on a
disabled button reported success; an armed pick that missed said nothing.

## The gate (C1)

The piece that keeps working after the round. `defaults/*` in
`src/app/GuiTests.cpp` is a table of every creation tool, addressed by its
`###tb_*` id, with one registered test per tool so a failure names the tool.
For each, with its own defaults and no picks, **exactly one** of these must
hold:

- `Expect::Changes` — Accept is offered and the document differs afterwards
  (compared with `FingerprintOf`: body count, per-body volumes, feature
  count), with no error on the new feature;
- `Expect::RefusesUntilPicked` — Accept is disabled and the window names what
  is missing;
- `Expect::ArmsAPick` — not a preview at all (Hole, New Sketch ask for a face
  or plane first);
- `Expect::HarmlessNoOp` — accepts cleanly and legitimately changes nothing
  (only Refine, on an already-clean body).

"Enabled button, accepts, changes nothing" is the failure it exists to catch.
It checks the Accept **button**, not only the outcome, because a tool that
opens already failing is the same bug one step earlier — which is exactly how
it caught the revolve axis and the sweep profile on its first run.

## What the round changed

- **C1 `9bdb3c4`** — the gate, plus the revolve axis (now a world axis that
  lies IN the sketch plane, preferring the one along the plane's V) and the
  sweep seeds (profile = the latest CLOSED sketch; path = the latest open
  chain or curve that is not that profile).
- **C2 `7ae08de`** — `ui/Placement.h` states the placement rule once; the
  occurrence insert and drawing-view placement use it. `NextSpot` keeps its
  ladder (every primitive is 2 units, so it is equivalent) with a comment
  pointing at the rule. Fixed while in there: a fourth drawing view wrapped
  UPWARD and the title-block clamp then pushed it off the paper.
- **C3 `e49d238`** — the tool window says *"Changes nothing - the model would
  be identical without this feature"* in amber, and leaves Accept enabled.
  It costs a comparison, not a rebuild: `Document::CachedBodiesAfter(idx - 1)`
  is the pending feature's input and `CachedBodiesAfter(idx)` its output.
  Features that never touch bodies (sketches, datums, curves) are exempt.
  `Feature::WhyIncomplete()` names a missing INPUT — distinct from
  `GetError()`, a regeneration failure — and is enforced by both
  `AcceptPendingFeature` and the `add_feature` RPC, so the script API refuses
  what the GUI refuses. Draft gained Shell's OCC-returned-the-input guard.
- **C4 `ae9793a`** — the tree's body rows fill an armed Body slot (the plane
  rows already filled a plane pick), and `SlotPickState::miss` explains a pick
  that landed on nothing.
- **C5 `bd03f5d`** — sketch axes carry `U +Y` / `V +Z` labels anchored near the
  origin (a compass, not a ruler); nav-cube corners and edge midpoints are
  clickable; the camera fit keeps the model clear of the tool window.
- **C6 `31fff13`** — the gui-labeled ctest entries run under
  `tools/xvfb-exec.sh`, so a locked screen stops reading as a 600 s timeout.

## Why the no-op warns instead of blocking

Refine on an already-clean body has nothing to merge; a fillet at a radius the
geometry rounds away is a judgement call; a draft of 0° is a parameter the
user is about to change. Blocking those would make correct tools feel broken.
The bug was never that the app allowed a no-op — it was that the app never
said so. Hence: amber line, Accept live.

## The acceptance round, and what it found

The round closes with a clicks-only pass across all three document kinds:
part (sketch → extrude from the model toolbar → fillet → countersunk hole),
assembly (insert two parts → fastened mate), drawing (view → Dist X → hole
callout). All of it worked — the drawing view auto-scaled to 10:1 and its
dimensions read model units, the two occurrences landed clear of each other,
and the countersink was sized from the bore.

It also found one bug in this round's own C5: the fit read
`state.toolWindowRect` to decide how far to pan clear of the panel, but the
remote `fit_view` runs before the UI draws, so the rect is a frame behind —
and nothing cleared it when the window closed. A fit right after accepting a
feature shoved the model a third of a viewport sideways, and the next click,
aimed at the middle of the screen, missed the part. Fixed in `79ff834` by
asking the session state whether a window is open and only then consulting the
rect. The warning from C3 is what made it visible: the hole reported "Changes
nothing" instead of quietly cutting air.

## Deferred, with reasons

- **A save-mode file browser** so Export works without a desktop portal. The
  in-app browser is open-only; humans on a desktop already get a working
  dialog, and scripts use `export_drawing` / `export_bom`. Export by clicks is
  therefore out of reach for a scripted human, by construction.
- **MCP on port 8799** (the human's dev-watch instance) — lives in the user's
  environment, not the repo.

## The rule this round leaves behind

> A default carrying a unit — millimetres, degrees, a scale, a position — must
> be derived from what it acts on: the bore, the body's bounding box, the
> sheet. Never a literal.

Recorded in `CLAUDE.md`, and enforced for creation tools by the `defaults/*`
suite.
