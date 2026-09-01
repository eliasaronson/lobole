# Placing a fastener by clicking — plan (F1 p6)

> Status: **planned, not built.** Written 2026-09-01 after phases 1-5 shipped
> ([plan.md](plan.md)) and after the first real assembly went through the
> feature over RPC. Everything below was checked against the source rather
> than assumed; the mechanisms it leans on are named by their real symbols so
> the estimate can be argued with.

## Why

F1 built the derivation and left it unreachable from the interface most people
use. `fit_fastener` is RPC-only: there is no toolbar button, no menu item and
no keybinding — checked, not assumed (`Commands.cpp`, `Toolbar.cpp`,
`OccurrencePanel.cpp` mention no fastener at all).

That is not a rounding error on a finished feature. The argument for deriving
a fastener rather than typing one is a **U10** argument — a default carrying a
unit must come from what it acts on — and U10 is about the GUI. Building the
derivation and exposing it only to agents inverts the thing it was for.

The scale is small and worth stating up front, because it is the reason to do
it now rather than schedule it: **the expensive parts are done.** The
geometry, the tables, the refusals, the traversal, the placement and the BOM
all exist and are gated. What is missing is one tool window and a pick slot,
on machinery that already carries three other tool sessions.

## First, a bug — this is not optional and not GUI work

`ui::FitFastenerToHole` takes a bare `ElementRef` and resolves it by walking
the flattened assembly for the **first** instance whose part describes that
element:

```cpp
if (foundRim) continue;
const auto element = Measure::Describe(inst.part->GetBodies(), rim);
```

Every occurrence of one part shares every element name. So on a joint made of
two occurrences of the same plate — which is the ordinary case, and was
exactly the first model this was tried on — picking the rim on the **second**
plate places the fastener on the **first**. Silently, at the wrong end.

Over RPC it has been survivable because a caller passes a name and gets
whichever occurrence answers first. A click-driven verb hits it immediately,
because the whole point of clicking is that you meant *that one*.

So: `FitFastenerToHole` takes the occurrence id alongside the element, and
resolves against that occurrence. The RPC gains an optional `occurrence` on
its `rim` object. Gated headlessly with two occurrences of one part and a pick
on the far one — a test that fails today.

## What the verb is

**One pick and one choice.** Click the rim the head sits against; choose which
standard. Everything else derives, and phase 4 corrected which "everything
else" that is:

| | |
|---|---|
| thread size | derived — the bore, read backwards out of the clearance table |
| length | derived — the stack, snapped to a length that size is made in |
| head | **the standard** (ISO 4762 *is* a socket head cap screw) |
| drive | **the standard** (a torx socket cap screw is an ISO 14579) |
| washers, nut | chosen — geometry cannot imply them, and plenty of joints have neither |

So the window is a row of standards, two checkboxes and a nut toggle. The
plan's original "dialog with thread, length, head and drive" is four fields
that no longer exist.

### It stays open

Holes come in patterns. A flange with eight bolts should be eight clicks, not
eight trips to the toolbar — so a successful placement **re-arms the rim pick**
with the same options rather than closing the session. Escape or Done ends it.

This differs from the mate session (which closes when both connectors are
filled) and matches nothing else currently, which is why it is written down
here rather than left to the implementer: it is a deliberate difference, not
an inconsistency.

### The summary is the product, and it belongs in the window

`FitFastenerToHole(..., dryRun)` already returns the decomposition:

    M8x30 — grip 30: washer 1.6, Part 1 10, Part 1 10, washer 1.6, nut 6.8

That line is what makes a wrong stack visible — a gap that should not be
there, a plate that is not what you thought. It updates live as the options
change and sits above Accept, so the number is read **before** the commit
rather than found afterwards in a BOM.

A refusal takes the same slot: "there is no hole on this axis in Bracket…"
appears where the summary would be, and Accept stays disabled. No modal, no
log-only failure.

### The empty case IS the refusal

Opening the tool with nothing picked gives the prompt ("Pick the rim of the
hole the fastener passes through") and a disabled Accept. That satisfies U10 —
a creation tool must either act on its defaults or refuse visibly — and it is
the honest answer, because a screw with no hole has nothing to derive from.

## What it reuses (all of it already exists)

| need | what carries it today |
|---|---|
| armed pick on an **assembly** tab | `state.slotPick` + `ArmMateConnector` (U3/U7c) — mates are made exactly this way |
| a rim from a viewport click | `SelectionPipeline` already returns `{Kind::Edge, occurrenceId, ElementRef}` on an assembly tab |
| a non-feature tool session | the `kMateSessionSlot` / `kSectionToolSlot` / `kCamToolSlot` sentinel pattern in `SlotPickClicked` |
| the floating window | `BeginToolWindowFrame(state, "Fastener")` — tests address `//Fastener###ToolWindow/###id` |
| button, keybinding, palette entry | one `CommandButton` + one `add(...)` row in `Commands.cpp` |
| the tab-bar folder for what it creates | `kStandardPartsFolder`, already used |

`DrawCamToolWindow` is the structural template: a tool session for something
that is not a feature, with its own picks and its own Done.

## Where the button goes

The assembly toolbar, beside the mate verbs (`###tb_mate_*`) — the fastener
verb is the same kind of thing: an assembly-level action driven by picking
geometry. `###tb_fastener`.

Notably it does **not** join `kToolCases`, the 24-entry table behind the
`defaults/*` suite: that table is part-tab tools, and the mate verbs are not
in it either. Assembly verbs are covered by their own tests. Worth saying,
because "add a row to kToolCases" is the wrong instinct here and would fail in
a confusing way.

## Phases

1. **The occurrence fix.** `FitFastenerToHole` resolves against the picked
   occurrence; the RPC's `rim` gains an optional `occurrence`. Headless gate
   with two occurrences of one part. **This ships on its own** — it is a
   correctness fix, and it should not wait behind a tool window.
2. **The verb.** `PickSlot::FastenerRim`, `kFastenerToolSlot`, an
   `ArmFastenerRim` mirroring `ArmMateConnector`, a routing branch in
   `SlotPickClicked`, `state.fastenerTool`, `DrawFastenerToolWindow`, the
   toolbar button and command row.
3. **Gates.** A GUI test that arms, picks a rim, accepts, and finds the
   occurrences; one for the no-pick refusal; one for a rim whose diameter is
   not a stocked clearance (the refusal must render, not just log).
4. **`check_fasteners` where it can be seen.** A button beside the Bill of
   materials and its findings listed under it, the way the BOM's mass
   warnings already are. Cheap — an hour — and arguably worth more than the
   placement verb: placing a fastener has a workable script path, a wrong one
   going unnoticed does not.

Phases 1 and 4 are each independently worth shipping. Phase 2 is the bulk.

## Cost

About a day, and the shape of it matters more than the number:

| | |
|---|---|
| the occurrence fix + its test | ~30 lines |
| pick slot, sentinel, arming, routing | ~30 lines |
| the tool window | ~200 lines |
| toolbar + command row | ~10 lines |
| GUI tests | ~120 lines |

Half a day for the code, the rest for the tests and the ImGui id papercuts
this codebase has already paid for once each.

## Traps already paid for elsewhere

- **A bare `Combo` cannot be gated.** It gathers with an empty label, so a
  test cannot address it (cost a round in the CAM work-origin work). The
  standard picker is buttons, or a combo with an explicit `###` id.
- **`PushID(int)` is not addressable** either — every row carries a stable
  name (the Variables panel's comment says why at length).
- **A disabled main-menu item is invisible to the test engine**, so the
  refusal must be a disabled *button in the tool window*, which is
  addressable, rather than a greyed menu entry.
- **A toolbar button that is enabled and silently does nothing** is the bug
  the `defaults/*` suite exists for — and the same bug that made File > Save a
  no-op on an assembly tab until 2026-09-01. Whatever this button does when
  clicked with nothing selected, it must be visible.

## Out of scope

- **A ghost of the screw before Accept.** CAM's `camGhostOp` is the precedent
  and it would be nice; the live summary already states the answer in numbers
  before the commit, which is the part that matters.
- **The second pick that overrides the far end.** Designed in
  [plan.md](plan.md), unbuilt, and no real model has wanted it yet.
- **Folding hardware in the Occurrences tree.** Two bolted joints put eight
  fastener rows in a ten-row list — real, and a tree change rather than a
  verb change. Its own item.
- **A parts browser.** Still the wrong answer: the pick-a-hole verb is the
  interface, and a searchable catalogue is what you build when the verb is not
  enough.
