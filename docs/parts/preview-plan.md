# Seeing the fastener before you place it — plan (F1 p7)

> Status: **BUILT 2026-09-01**, all four phases. Written 2026-09-01 from use: *"different ISO
> standards does not tell me much"*, with a screenshot of four buttons reading
> ISO 4762 / ISO 14579 / ISO 4017 / ISO 7380 and nothing else. Every mechanism
> named below was checked against the source.

## Why

The tool asks one question — which standard — and answers it with a catalogue
number. `ISO 7380` is a button head; `ISO 4017` is a hex head you turn with a
spanner. Nothing on screen says so, and the difference is the only thing the
choice is about, since the size and the length derive.

Worse, the *joint* options are three checkboxes. A washer under the head, a
nut on the far side, a washer under that — the thing they add is precisely the
thing you cannot picture from a checkbox.

## Two halves, and the cheap one is half the complaint

### 1. Say what it is (~15 minutes)

`parts::Profile` already carries `what`: "socket head cap screw", "button head
socket screw", "hex head screw", "hexalobular socket head cap screw". It is
already written, already vendored-standard-accurate, and currently reachable
only by hovering. Put it on the button.

That alone turns an unreadable row of numbers into a readable list, and it
costs nothing but layout. **Ship it first and separately** — it is not the
preview, and holding it behind the preview would be holding the cheap fix
behind the expensive one for no reason.

### 2. Ghost the joint at the rim (~2 hours)

A translucent screw, washers and nut sitting in the hole, updating as the
options change, before Place commits anything.

## What it reuses

| need | what carries it today |
|---|---|
| a translucent instance | `MeshRenderer::SetInstanceGhost(id, alpha)` — an existing alpha-blended, depth-write-off pass |
| building the mesh | `CADShape::Mesh` -> `CADMeshConverter::ConvertCADMeshToVertices` -> `CreateAsset` -> `AddInstance` |
| a worked example of exactly this shape | `SyncInContextGhosts` (AssemblyActions.cpp) — builds assets per part, instances them at a transform, tints and ghosts them |
| building the fastener body without committing | `AddFastener` already does it: the feature is executed in a scratch `Document` as a probe, to let the table refuse before a tab exists |

**No new scene renderer.** It rides the ghost pass that already exists, so
there is no `RegisterSceneRenderers`/`ShutdownSceneRenderers` wiring and
`test_conventions` is not involved (docs/rendering/renderer-wiring.md).

## The two things that are actually work

### The fit keeps its placements to itself

`FitFastenerToHole` computes the rim centre, the axis, and an orientation per
piece — and uses them only at commit time. So a **dry run today tells you the
spec but not where**, which is a slightly dishonest preview even in text.

`FastenerFit` gains the planned placements:

    struct PlannedPart {
      std::string standard, size;   // "iso7089", "M12"
      double length = 0.0;          // 0 for a nut or a washer
      glm::vec3 position;
      glm::quat orientation;
    };
    std::vector<PlannedPart> plan;  // screw first, then washers, then nut

Filled on BOTH paths, and the commit path then just walks it. That removes the
duplication between "work out where everything goes" and "place everything",
which today are the same arithmetic written once and used once.

### The ghost cannot live in the resync

This is the trap, and it is why the estimate is two hours rather than one.

`SyncAssemblyRenderers` and `SyncDocumentRenderers` are **version-gated**:
they run when a document or the assembly changes. The fastener ghost changes
on TOOL state — a rim picked, a checkbox toggled, a standard chosen — and none
of that bumps a version. Hung inside the resync, the ghost would appear only
when something *else* happened to change, which is the kind of bug that reads
as "the preview is flaky".

So it follows `SyncCamPlayback` instead, whose header says the pattern
outright: *"Cheap and idempotent — call it every frame."* One
`SyncFastenerGhost(state)` in the frame loop that:

- does nothing when the tool is closed or has no fit,
- keeps a cache key of `(plan, standard, options)`, rebuilds only when it
  changes, and
- removes its instances when the key goes empty — tracked apart from
  `documentMeshes`, like `topoGhostInstance`, so a stale one cannot survive a
  tab switch and haunt another document (which is a bug the topopt ghost
  already had once and left a comment about).

## Phases

1. **Labels.** The standard buttons say what the screw is. Ships alone.
2. **`FastenerFit::plan`.** The fit reports where every piece goes; the commit
   path consumes it instead of recomputing. Headless gate: a dry run's plan
   has the same positions the wet run places at — which is a claim worth
   pinning whether or not a ghost ever draws it.
3. **The ghost.** `SyncFastenerGhost`, called per frame, cached on the plan.
4. **Gate.** A GUI test that arms the tool, picks a rim, and asserts ghost
   instances exist and their count tracks the joint options (screw alone = 1;
   with two washers and a nut = 4); then that Done clears them.

Phase 2 is worth doing even if 3 is dropped: it makes `fit_fastener`'s
`dry_run` complete, and an agent asking "what would this do" currently gets an
answer missing the position.

## What building it corrected

- **`render_still` cannot see it, and that is correct.** The first attempt to
  eyeball the ghost used `render_still` and came back with the plate and no
  screw, which looked like the preview failing to render. It is the OFFLINE
  RAYTRACER: it draws the model, not the raster overlays. `screenshot` (the
  viewport grab) is the tool for anything living in the raster passes —
  ghosts, highlights, the backplot.
- **The first tint was wrong, and only looking showed it.** A near-neutral
  grey-blue at alpha 0.45 — roughly what steel is — read as washed-out
  geometry against a white plate rather than as a preview. It is now
  distinctly blue, like the topology shape-guide ghost. The instance-count
  test passed either way; the house rule about actually looking at renders is
  what caught it.
- **A tab switch disarms the pick but leaves the session open.**
  `SetActivePart` cancels pending work and clears `slotPick`, so coming back
  to the assembly tab gives a tool window with nothing armed. Harmless — the
  Pick rim button re-arms — but it cost a confused debugging pass.

## Out of scope

- **A 2D thumbnail per standard.** More work than the 3D ghost (it needs
  authored or pre-rendered art) and less useful — the ghost shows the screw in
  *your* hole at *your* size, which no thumbnail can.
- **Ghosting on hover before the pick.** The rim has to be picked before there
  is anything to derive from; previewing on hover means running the traversal
  per frame per hovered edge.
- **Dragging the ghost to reposition.** The placement is derived. A draggable
  preview would invite exactly the typed-in placement U10 exists to prevent.
