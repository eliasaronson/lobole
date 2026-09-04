# Agent-scale follow-up: three things the engine left behind

The agent-scale round shipped G0–G6 and its acceptance test passed
(docs/agent-scale-plan.md, docs/modeling/single-cylinder-dogfood.md). Three
problems outlived it. Each is measured, each has a repro, and they are
independent — W1 is a defect, W2 is an architecture question with a small
answer, W3 is a performance hole with three separable parts.

Ordered by what it costs to leave alone.

---

## W1 — `check_fasteners` must agree with `fit_fastener` — DONE 2026-09-04

Shipped in da043eae. It was two defects, not one: the grip ignored the on-axis
ray, AND the check applied the through rule ("it must span the stack") to blind
joints, which are snapped DOWN to a stocked length and so are shorter than
their hole is deep by construction. Fixing only the first would still have
called the engine's M10x80 in an 85 mm joint 5 mm short. Both callers now take
the answer from `parts::JointEnd`; a blind screw is judged on bottoming out and
thread engagement instead. `tests/fastener_stack_smoke.py`.

The original statement:


**The defect.** Minimal rig: an 80 mm base with a 25 mm blind hole, a 20 mm
plate on top, one screw.

    fit_fastener    -> M10x45, blind=True, "grip 45: plate 20, base 60"
    check_fasteners -> "is 35 mm too short: its stack is now 80 mm"

Nothing happens between the two calls. Every blind-hole joint reads as too
short by exactly the material under the hole, and a blind hole is the normal
way to bolt into a block — all four of the engine's head bolts are flagged.

**Root cause: two traversals of the same stack.** `FitFastenerToHole`
(AssemblyActions.cpp) walks the stack and knows it bottomed out, which is how
it sets `blind` and trims the grip to 45. The check re-derives by probing the
axis with a ray offset past the loosest clearance and taking every solid span
it crosses, so it counts the base's whole 60 mm. `fit`'s own summary string
has the same blind spot — it says "base 60" beside a correct grip of 45 — so
the decomposition is the raw traversal and only the grip was trimmed.

**The fix is not "make the check stop too".** That leaves two traversals that
happen to agree until someone edits one. Extract the traversal into a single
function that returns the stack, where it ends, and why, and have both callers
use it. Then they agree by construction, and the summary is trimmed once. This
is the same shape as the fixes that worked in the last round: one frame
builder (`FrameMath.h`), one tessellation cache, one list-selection gesture.

**Gate.** A test on that rig asserting `fit_fastener` and `check_fasteners`
return the same grip and the same verdict — plus a through-hole case, so the
fix cannot be "always stop at the first hole".

Size: small. One extraction, one test.

---

## W2 — a READ should not need a tab

### Why the SDK handles tabs at all

Not an SDK invention: the app has exactly **one live document**.
`PartSlot::doc` is a `unique_ptr` that is *moved* into `AppState::document`
when a tab activates and moved back when it leaves (`StashActive` /
`ActivateSlot`, AppActions.cpp:1091–1128). Every other tab's model is stashed.
So "the active part" is a real thing in the document layer, and the RPC
surface inherited it: RemoteServer.cpp:1091 refuses most methods when
`state.doc` is null, with a list of 60-odd exceptions.

### Could parts be handled by references instead?

**For reads, yes — and half of it already is.** `PartDocById(state, id)`
resolves any slot's document whether it is active or stashed, and it is used
39 times outside AppActions, including from the RPC layer. Two of the SDK's
own query RPCs already take `part`:

    list_elements  params: body, max_faces, part
    frame          params: element, feature_id, frame, occurrence, part

Verified: with `beta` active, `list_elements(part="alpha")` returns alpha's
body with its 6 faces and 12 edges, and `get_state` still reports beta's body.
No switch. The SDK calls `activate()` anyway.

**The one real gap is `get_state`**, which takes no parameters and is the only
source of the body LIST. And the two payloads are not nested:

| | fields |
|---|---|
| `get_state` bodies | name, volume, bbox_min, bbox_max, material |
| `list_elements` bodies | name, created_by, body, job_part, faces, edges |

Neither is a superset, which is why the SDK needs both and why `Body` carries
`reference` from one and `created_by` from the other.

**For writes, no, and we should not try.** A part's `CommandStack` is stashed
per slot too, so mutating a non-active part means undo history for a document
that is not on screen — a real question about what Ctrl+Z then means, for very
little gain now that `batch` amortizes a burst of edits. **Reads by reference,
writes on the active tab** is the line.

### Why it is worth doing beyond ergonomics

A tab switch is not a free pointer move. `ResetActiveTabUiState`
(AppActions.cpp:1130) clears the selection, the hidden-body set, the measure
session, simulation mode and the explode state, and forces a renderer resync.

CLAUDE.md tells agents they may read from the human's `dev-watch` instance on
8799 freely and only warns against mutating their document. But under today's
SDK, *reading a part's faces switches their tab and clears their selection.*
A documented-safe operation is not safe.

### The work

1. `get_state` gains a `part` parameter, resolved by the same helper
   `list_elements` and `frame` already use (`ResolvePartDoc`,
   RemoteInspection.cpp:40–75) — name, stable id or slot index.
2. The SDK's read paths (`Part.bodies`, `Body._load`, `faces`, `edges`,
   `Element.frame`, `volume`) pass `part=` instead of calling `activate()`.
   `Part.activate()` stays for the write paths.
3. Audit the rest of `describe` for read-only methods that refuse without a
   part tab and could take `part` the same way — `mass_properties` takes only
   `body`, and is the obvious next one.

**Gate.** A smoke test that builds two parts, queries every read path on the
first while the second is active, and asserts the active tab never changed —
the assertion the current SDK cannot pass.

Size: small-to-medium. One RPC parameter, a handful of SDK call sites, one
test. The audit in (3) is the open-ended part and can be bounded to the
methods the SDK actually uses.

---

## W3 — a drawing of an assembly — DONE 2026-09-04

Shipped. **The plan's (a) was wrong and is closed without being built**: HLR
runs ONE pass across all bodies so occlusion is global, and each view looks a
different way, so there is no per-part projection to share between them. A
per-VIEW cache already existed (`UpdateDrawingRenders` re-projects only what
went stale). What was missing was a measurement, and it settles the question:

    iso view of the engine: 84.3 s total, of which hide 84245 ms, outline 75 ms

It is all `HLRBRep_Algo::Hide()` — visibility classification. `show_hidden:
false` does not help (84228 ms, it only filters the output). So:

* **`quality: "fast"`** — polygonal HLR over the triangulations. Three views of
  the engine in **3.5 s** against >21 minutes; a plain box gives byte-identical
  bbox and curve count to exact. It is a LOOK: polylines, no source refs,
  nothing to dimension against, and it is never substituted silently — an
  unknown quality is refused by name. Format v89 + golden + history row.
* **`update_drawing {budget_s}`** — bounds the whole update. This is only
  possible because OCC's `Hide()` takes no progress range and no cancel flag,
  and `HLRBRep_Algo` derives from `HLRBRep_InternalAlgo`, which exposes the
  PAIRWISE `Hide(i)` / `Hide(i, j)` the one-shot call performs internally.
  Driving that loop ourselves changes nothing (913 curves before, 913 after)
  and makes stopping possible. A view that runs out keeps its previous curves
  and says which lever to pull.
* **The cost is on the record.** `list_drawing_views` reports
  `{outline_ms, hide_ms, hlr_edges, bodies}` per view, so a slow sheet is a
  number rather than a stopwatch.

Not done, deliberately: **the export is still synchronous.** Putting it on the
A3 job registry needs the worker to read shapes the main thread may be
regenerating — and poly HLR *meshes* them, which writes into the shared TFace.
The safe version means deep copies, and a copy is only safe if
`BRepBuilderAPI_Copy` preserves explorer order, since the projection reports
source EDGE INDICES that `AssembleSheetView` turns into stable names. It holds
for a box (`test_drawing_hlr.cpp` asks outright) but one box is not evidence
for a swept spring, and getting it wrong anchors every dimension on the sheet
to the wrong feature — silently. `budget_s` bounds the damage without betting
on it.

The original statement:


**Measured** on the engine, each figure one export of one sheet:

| sheet | seconds |
|---|---|
| one part (`head`), iso | 0.00 |
| a plain block, iso | 0.00 |
| a block + one four-turn helical spring, iso | 5.5 |
| the assembly, iso | 66.4 |
| the assembly, front / right / top | 84.1 / 85.8 / 73.1 |
| the assembly, three views on one sheet | >21 min, abandoned |

So it is not the springs and not a degenerate direction: every assembly view
lands in the same 66–86 s band. Three separable fixes, in the order they pay:

**(a) Cache the projection per (shape, direction).** Exact HLR
(`HLRBRep_Algo`, DrawingProjection.cpp) is fed the assembly's solved shapes,
so the same eight parts are re-projected for every view with nothing kept
between them. A front and a right view of one part share no work today; two
occurrences of the same part share none either. This is the same fix as the
shape-keyed tessellation cache that took a part tab from 2.6 s to 0.6 s.

**(b) Put the export on the A3 job registry.** Today it runs on the main
thread, so a client sees only its own timeout while the app keeps burning a
core — I lost an instance to exactly this, and a subsequent `describe` from a
fresh client timed out behind it. `motion`, `topo`, `study`, `mesh`, sync,
import and CAM are already job kinds; drawing export is not. That buys
progress, cancellation, and a server that answers while it works.

**(c) `HLRBRep_PolyAlgo` as an explicit mode.** A look is not a deliverable.
Polygonal HLR is the right answer for "show me the sheet" and exact HLR stays
the default for the PDF. It must be a named mode that says which it gave you —
never a silent substitution when the exact one is slow.

**One more, nearly free:** `add_drawing_view` returns in 0.00 s and holds no
geometry — the entire cost lands at export, so nothing tells the caller what a
view just cost. With (b) in place the view can report its own estimate, or the
export can say what it is about to project.

Size: (a) medium and the biggest win, (b) medium and the one that stops a
stall becoming a lost instance, (c) small.

---

## Not in this plan

- **Mutating a part by reference.** The undo question above. Revisit only if
  a real workflow needs it, and `batch` is the cheaper answer until then.
- **Plane-normal ergonomics.** I cut into fresh air twice in one session
  (`Plane.xz`'s normal is −Y; `Plane.xy(62)` cuts upward). Both were caught
  immediately by the app's own "cut removed no material" warning, which is the
  system working. Making the direction explicit in the SDK is an ergonomic
  nicety, not a correctness gap, and it is not obvious that a signed depth
  reads better than a `flip`. Left alone deliberately.
- **A benchmark sweep.** 90 runs, $180 ceiling. Nothing in the last round has
  a benchmark number and benchmarks/REPORT.md says so. Run it when the surface
  changes again — W2 changes the surface, so it is the natural trigger.
