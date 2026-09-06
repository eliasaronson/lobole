# Architecture review round 2 (2026-09-06) — the oversized units

> Status: **PLAN. Nothing shipped.** Round 1
> (docs/architecture-review-2026-08-24.md) fixed the duplicated Vulkan
> helpers, the dead code, the `GuiTests.cpp` monolith, the feature-type table
> and a contained `ui/` decoupling. It *measured* two problems and scheduled
> neither: §4 "`AppState` is the compile-time bottleneck" and §5 "oversized
> units". Both got worse. This round is those two.

Measured on `13a21cd7`. Every number below came from a command, not an
estimate; where the machine was under load at the time it says so.

---

## 1. What grew

Round 1's §5 table, re-run today:

| Unit | 2026-08-24 | today | change |
|---|---|---|---|
| `ui/AppActions.cpp` | 4,700 (36.3 s) | **6,398** (48.3 s) | +36% |
| `ui/SketchInteraction.cpp` | ~3,400 | **4,278** | +26% |
| `core/cam/CamToolpath.cpp` | — | **3,306** | new since |
| `ui/AssemblyActions.cpp` | ~2,600 | **3,255** | +25% |
| `remote/RemoteServer.cpp` | 2,500 (30.9 s) | **2,856** (38.4 s) | +14% |
| `app/main.cpp` | ~2,100 | **2,525** | +20% |
| `ui/AppState.h` | 1,186 / 214 members | **1,719 / 317 members** | +45% |
| `tests/test_cam.cpp` | — | **5,965** (121 cases) | new since |

`RegisterGuiTests` is the counter-example and the precedent: round 1 split it
by domain and its incremental cost went **40.6 s → 3.7 s**. Nothing regressed;
`--gui-test-list` was byte-identical. That is the move, applied to five more
files.

## 2. Where the compile time actually goes — the result that changes the plan

The obvious hypothesis is "these files are slow because they drag in
OpenCASCADE". Half true, and the half that is false matters.

**Preprocessed line counts** (deterministic, load-independent):

```
ui/AppActions.cpp   FULL          274,974 lines
  ui/AppActions.h   alone         261,675   ← 95% of the TU is the header
  ui/AppState.h     alone         224,066
  core/Document.h   alone         154,782
  cad/CADKernel.h   alone         145,557   ← OCC: 53% of every UI TU
  imgui.h           alone           3,859
```

OCC arrives in all 65 `AppState.h` includers through `Document.h`.
`designer_core` already has a PCH containing `CADKernel.h`; **`designer_app`
has none**, and it is 1,469 s of CPU across 208 files.

**But the header is not what makes `AppActions.cpp` slow.** Serial `g++`,
same flags as the build, machine near-idle:

```
AppActions.cpp, whole TU                    26.1 s
a TU containing only #include AppActions.h   4.6 s   ← headers
                                            -------
                                     body:  21.5 s
```

A PCH of `AppState.h` + `imgui.h` took the whole TU from 26.1 s to 23.8 s —
**9%**. Under load (load avg 30, another build running) a header-only TU went
9.2 s → 2.4 s with a PCH covering `AppActions.h` + OCC, so the PCH is worth
roughly 3–7 s *per TU that includes the umbrella header*, and there are 55 of
them.

Two conclusions, and they point opposite ways:

1. **Splitting `AppActions.cpp` into 12 files raises total CPU**, because each
   new TU re-pays the 4.6 s header parse (12 × 4.6 ≈ 55 s of new work). It
   wins on the two things that are actually felt: the *incremental* cost of
   touching one concern (26 s → ~7 s) and parallelism (one 26 s critical-path
   file becomes twelve that `-j` absorbs).
2. **A `designer_app` PCH is what pays that back**, and it is ~15 lines of
   CMake.

So: **split first, PCH second, in the same round.** A PCH alone buys 9% and
hides the structural problem. A split alone makes `ninja` do more total work
and invites "the refactor made the build slower". Together they are strictly
better than today on both axes.

---

## 3. `ui/AppActions.cpp` — 6,398 lines, thirteen unrelated subjects

Not one god-function; a *bag*. 165 declarations in `AppActions.h` spanning
every domain in the app. The blocks are already contiguous and separated by
anonymous namespaces — this is a cut, not a rewrite:

| Lines | ~size | Subject | Proposed home |
|---|---|---|---|
| 94–201 | 110 | selection helpers (`SelectedFeature`, `PruneSelection`) | fold into **`SelectionPipeline.cpp`** (exists) |
| 202–404 | 200 | plane quads, sketch grid, origin gizmo, CoG marker | **`SceneOverlays.cpp`** (new) |
| 405–905 | 500 | `CreateBox`…`CreateRib`, `StartHoleTool` — the defaults suite's subject | **`CreateActions.cpp`** (new) |
| 905–1005 | 100 | datum/axis/frame/curve line appenders | **`SceneOverlays.cpp`** |
| 1006–1717 | 710 | tab slots: add/duplicate/delete/activate, folders, per-tab undo, `JobUndoGuard` | **`DocumentSlots.cpp`** (new) |
| 1718–1993 | 275 | project variables + global undo routing | fold into **`VariablesPanel.cpp`**'s pair, new `VariableActions.cpp` |
| 1994–2850 | 860 | save/open/autosave/recover/sidecars/git remote/disk stamp | **`ProjectActions.cpp`** (new) |
| 2851–2933 | 85 | occurrence matrices, resolvers, `FlattenActiveAssembly` | fold into **`AssemblyActions.cpp`** (exists) |
| 2934–3596 | 660 | camera fit, section views, section cap meshes | **`ViewActions.cpp`** (new) |
| 3597–4221 | 625 | drawing view projection, placement, export, render pump | **`DrawingActions.cpp`** (new — pairs with `DrawingPanel.cpp`) |
| 4222–4989 | 770 | `SyncDocumentRenderers`, geometry delta, RT scene, still render | **`RendererSync.cpp`** (new) |
| 4990–5219, 6197–6262 | 290 | sketch overlay line/primitive builders | **`SketchOverlay.cpp`** (new) |
| 5220–5349 | 130 | blend-edge pick resolution | fold into **`FeatureEditor.cpp`** (exists) |
| 5350–6196 | 850 | measure: resolve, compute, format, pin, hover | **`MeasureActions.cpp`** (new — pairs with `MeasureOverlay.cpp`) |
| 6263–6398 | 140 | reference images, `ImportAsFeature` | **`ImportActions.cpp`** (new) |

Five of these already have their drawing half in a separate file and only the
*logic* half stranded here — `MeasureOverlay.cpp`, `DrawingPanel.cpp`,
`VariablesPanel.cpp`, `SelectionPipeline.cpp`, `FeatureEditor.cpp`. That is
the "combined functionality" half of the ask: this is not inventing a
structure, it is finishing one. `Overlays.h` even carries the note —
*"the LINE GEOMETRY … stays in AppActions.cpp"* — recording the seam round 1
stopped at.

**The header is where the win is, and where the work is.** If `AppActions.h`
survives as an umbrella that includes the twelve new headers, the 4.6 s
per-TU cost stays and the split buys only incremental latency. Getting the
full win means a mechanical pass over the 55 includers to name the narrow
header, then deleting the umbrella. Budget that pass; don't skip it and call
the round done.

## 4. `remote/RemoteServer.cpp` — 2,856 lines, and the table it already half-has

Three subjects: socket transport + threading (82–380), `HandleSemantic`
(1,118–2,196 — a **44-arm hand-written `if (m == "…")` chain**), and the
`ui_action` queue (2,196–2,856).

The domain split is real and good — nine `Remote*.cpp` modules own their
methods. What is left here is the residue plus the routing.

**The problem is not the chain; it is that a method's name is a key into FOUR
separate string sets, none of them next to the handler:**

| set | entries | decides |
|---|---|---|
| `kNoPartDoc` | 189 | may run without a part tab |
| `kNotInBatch` | 32 | reaches outside the document, so a batch cannot roll it back |
| `kDeferrable` | 10 | its result does not need freshly built geometry |
| `IsUiAction` | — | goes on the ImGui queue instead |

Four chances to add a method and forget a list. That is the same shape as
every gate-list bug this project has already paid for (the CAM and drawings
gate lists; the part gate that was "a LIST apart from the handlers").
`kNoPartDoc` also has its default backwards: **189 of 230 methods are in it**,
so the column wants to be `needsPart`, true for ~41.

**The registry is already half-built.** Each module exports
`MethodDoc{name, doc, params}` today, `IsKnownMethod` already derives from
those tables, and `tests/rpc_params_gate.py` already fails when a row and its
handler disagree. So this is *add columns to an existing table*, not *invent a
registry*:

```cpp
struct MethodDoc {
  const char* name, *doc, *params;   // today
  bool needsPart, notInBatch, deferrable;   // the four sets, as columns
  json (*fn)(ui::AppState&, remote::RpcParams&);
};
```

**Two facts that make the migration safe:**

- **All 230 method names are claimed by exactly one module** (checked). No
  cross-module shadowing, so name→handler is a *faithful* transformation of
  the chain-of-responsibility, not a behavior change.
- Because the names are disjoint, it can be **strangled rather than swapped**:
  add the handler column beside the live if-chain, assert in a test that the
  two agree for every name, then delete arms one module at a time. Nine small
  reversible commits with a checkable invariant.

Then `rpc_gate_smoke` can read the table instead of parsing C++ — memory
records its extractor is *blinded by a braced `if (m=="…") {`*, so a method
can go ungated today and the gate cannot see it.

**What stays risky.** `batch` re-enters `HandleSemantic` recursively under
`m_inBatch`; its rollback and deferral rules must become the columns above or
the lists have merely moved. And the part-doc gate's precedence is load-bearing
— an unknown method must fall *through* it, or a typo answers "switch to a part
tab" and sends the caller hunting a tab problem that does not exist. A table
makes "known" trivially true, which is better, but it is behavior-adjacent and
pinned by error-message tests.

`rpc_gate_smoke.py` globs `Remote*.cpp`, so new files named that way are picked
up automatically. Splitting the residue into `RemoteSession.cpp` (transport) +
`RemoteUiActions.cpp` (the test-engine queue) + `RemoteDispatch.cpp` leaves
`RemoteServer.cpp` as the class only.
## 5. `ui/SketchInteraction.cpp` — 4,278 lines

Five of the six blocks are plain cuts:

| Lines | Subject | → |
|---|---|---|
| 39–695 | entity commit, drag sessions, sketch begin/paste, tool arming | `SketchTools.cpp` |
| 697–1,296 | dimension placement + inference | `SketchDimensions.cpp` |
| 2,140–2,600 | overlay lines, selection box, snap glyphs, prompts | `SketchOverlay.cpp` (the same file §3's overlay block lands in) |
| 2,604–3,514 | dimension + constraint glyph drawing (ImGui) | `SketchDimensionDraw.cpp` |
| 3,515–4,278 | picking, constraint application, pattern/offset ops | `SketchPick.cpp` + `SketchOps.cpp` |

The sixth is `HandleSketchViewport()` — **843 lines** (1,297–2,140), max brace
depth 7. Round 1 measured it at 804; it grew.

**It is not a switch over `SketchTool`, and a per-tool handler table is the
wrong shape.** It is a pipeline:

```
plane picking → Escape ladder → hole-Enter → snap inference →
delete-constraint → dim-pill gate → op re-pick     ← ~300 lines, EVERY tool
    ↓
Select(106) Trim(56) Mirror Pattern/Offset Image Calibrate Fillet Dimension
    ↓
click handler, with a nested switch for the simple draw tools
```

A tool's behavior is smeared across four to six phases rather than localized:
`Spline` is tested in **six separate places**, `Line` in four, `Select` in
five, across 20 enum values. A table keyed by tool would either duplicate the
300-line preamble twenty times or thread a fat context struct — and would
still leave the escape ladder, the snap-predicate list and the click switch as
per-tool code *outside* the table. **The cut is by phase, not by tool:** each
preamble phase becomes a named `bool` function ("did I consume the event"),
leaving a router that reads as a sequence of guarded phases.

**Why this is the riskiest stage in the round.** The ordering is load-bearing
and recorded only in comments:

- *"Handled BEFORE the pill-hover gate below"*
- *"a binding can apply a constraint, which REPLACES the sketch feature… the
  pointer above is then a dangling free. (Latent use-after-free; it started
  crashing once R3's regeneration allocations reused the freed block.)"*

Extracting phases invites silently reordering them, or losing that re-resolve
across a function boundary. Coverage is **37 GUI tests and no headless ones** —
`test_sketch*.cpp` covers the sketch *model*, not the interaction — so a
regression surfaces as a shuffled GUI failure at best. CLAUDE.md records three
order-dependent failures here that went undiagnosed for exactly that reason.

**What it buys** is not compile time (one 29 s file most UI work never
touches). It buys editability of the sketch tools, which is where the
dogfooding friction rounds keep landing, and it makes the escape ladder and
the snap-predicate list visible instead of buried at depth 7. Do it one phase
per commit with the shuffled GUI suite between each — or defer it until a
sketch feature needs the room.
## 6. `ui/AssemblyActions.cpp` — 3,255 lines

Six subjects, and two of them are not assembly logic at all:

- mates: tool arming, connector picking, overlay (85–875)
- BOM + fastener checks (239–564)
- the drag gizmo, occurrence and job-placement variants (877–1,468)
- **the tessellation cache** (1,546–1,832) — a render-resource LRU
- **`SyncAssemblyRenderers` + in-context ghosts** (1,833–2,261) — belongs with
  `RendererSync.cpp` from §3; these are the same job for a different tab
- fasteners: `AddFastener`, `FitFastenerToHole`, interface joining (2,380–3,255)

Proposed: `MateActions.cpp`, `Bom.cpp`, `GizmoInput.cpp`,
`TessellationCache.cpp`, `FastenerActions.cpp`, with the renderer sync joining
`RendererSync.cpp`. Two of those (`TessellationCache`, `Bom`) are pure
functions of a document and become **unit-testable headlessly**, which they
are not today.

## 7. `core/cam/CamToolpath.cpp` — 3,306 lines

The only one of the five in `core/`, so the only one where the split directly
buys test surface. Structure:

| Lines | Subject | → |
|---|---|---|
| 50–430 | Clipper conversion, offsets, loop orientation, move emission, helix | `CamGeometry.cpp` |
| 647–950 | sections, depth passes, stock/model bounds, fixture frames | `CamSetupFrame.cpp` |
| 989–1,250 | face/wall picking → regions | `CamPicks.cpp` |
| 1,251–1,780 | `DeriveOperation`, retool, reorder, safe-Z | `CamDerive.cpp` |
| 1,786–2,800 | `GenerateProfile/Pocket/Adaptive/Face/Drill/EdgeRound` | `CamStrategies.cpp` |
| 2,808–3,306 | placement fit, cutting order, machine bounds, clipping | `CamMachine.cpp` |

**The interface:** adding an `OpType` is shotgun surgery across **six files** —
`CamToolpath.cpp` (3 sites), `CamPanel.cpp` (4), `CamTool.cpp` (3),
`RemoteCam.cpp`, `SerializeCam.cpp`, `CamJob.h`. This is the exact shape round
1 fixed for feature types in stage 4 by making `kFeatureTypes` authoritative,
and the fix is the same: **one table, a row per `OpType`**, carrying the
generator, the display name, the serialized string, and the
`FollowsOneTargetAtATime` predicate.

The two switches inside `CamToolpath.cpp` are adjacent and exhaustive, so
`-Wswitch` does catch a *new* op type missed there — the table is not needed
to protect those. It is needed for the other five files, where the knowledge
is a string comparison or an `if`, and for the pairing itself: memory records
two silent-omission CAM bugs already shipped from this shape (*ops clipped
away by the work area*, and *a `profile` following only `targets.front()`* —
the second is precisely the predicate and its generator disagreeing). A row is
checkable by a test; six scattered lists are not.

The one wrinkle: `GenerateAdaptive` takes a `GenerateContext*` the other five
ignore. Give every row that signature and let the five drop it.

## 8. `ui/AppState.h` — 317 members, 1,719 lines, 65 includers

Do **not** split this for size, and do not put an abstract interface in front
of it. The free-function-over-`AppState&` style is the reason
`tests/test_actions.cpp` (2,197 lines) can drive the actions layer headlessly;
a virtual seam would buy nothing and cost that.

Two contained changes:

- **Nested tool state moves out.** `SketchEditState` (200 lines),
  `MeasureState` (155), `SimulationState` (63), `CamNewSetup`,
  `CamRegionCache`, `GizmoDrag`, `SectionView`, `FastenerTool`, `DimTool` are
  each one tool's private business, declared in the file everyone includes.
  Move them to `ui/state/*.h` next to the actions file that owns them (§3's
  split names those owners). `AppState` keeps a member of each type. This does
  not shrink the preprocessed size for `AppState.h` includers — it makes the
  ownership legible and lets a panel include one tool's state.
- **The `Document.h` → OCC chain is the 53%,** and it is unavoidable while
  `AppState` holds `std::vector<Body>` by value (`jobBodies`, `jobPlacedBodies`).
  Not worth breaking. The PCH is the answer, not a pimpl.

## 9. `tests/test_cam.cpp` — 5,965 lines, 121 cases, 38.7 s

Split by subject into `test_cam_toolpath.cpp` / `test_cam_verify.cpp` /
`test_cam_setup.cpp` / `test_cam_machine.cpp`, mirroring §7. Tags are ctest
labels, so `-L cam` keeps working unchanged. Same for `test_actions.cpp`
(2,197) and `test_projectmerge.cpp` (39.5 s, the single slowest test TU).

Low risk, and it is 109 s of the test target's 924 s sitting on three
critical-path files (38.7 + 39.5 + 30.5).

---

## 10. The plan

Ordered so that each stage is independently shippable and independently
revertible. Nothing here changes behavior; every stage's success criterion is
**identical behavior**, so the verification is the same each time.

| # | Stage | Files | Risk | Why this order |
|---|---|---|---|---|
| 1 | `tests/` splits (§9) | 3 test files | none | Free, and proves the label/ctest plumbing survives a split before touching `src` |
| 2 | `CamToolpath` → 6 files + strategy table (§7) | `core/cam/` | low | `core/` has real unit tests; the table closes a shipped bug class |
| 3 | `AppActions` → 12 files, umbrella header kept (§3) | `ui/` | low | Pure cuts along existing anonymous-namespace boundaries |
| 4 | Retire the umbrella: 55 includers name the narrow header | `ui/`, `remote/`, `app/` | low, tedious | This is where the compile win is realized |
| 5 | `designer_app` PCH (§2) | `CMakeLists.txt` | low | Pays back stage 3's duplicated header parsing |
| 6 | `AssemblyActions` → 5 files (§6) | `ui/` | low | Two blocks become headless-testable |
| 7 | **8a** — `RemoteServer` → session / ui-actions / dispatch (§4) | `remote/` | low | Mechanical; captures most of §4's size win with none of its risk |
| 8 | **8b** — four string sets → `MethodDoc` columns, one module at a time (§4) | `remote/` | low–medium | Strangled, not swapped: table and chain co-exist behind an agreement test |
| 9 | **7a** — `SketchInteraction`'s five non-router blocks (§5) | `ui/` | low | Plain cuts |
| 10 | **7b** — the 843-line router, by phase, one phase per commit (§5) | `ui/` | **medium** | The only genuinely risky stage; optional |
| 11 | Tool state → `ui/state/*.h` (§8) | `ui/` | low | Cosmetic; do it last or drop it |

**Stages 1–7 and 9 are mechanical.** Stage 8 needs judgement but is
incrementally revertible. **Stage 10 is the only one that can silently break
something**, and it is the one to drop if the round runs long.

The 2026-09-06 revision moved the RPC work *ahead* of the sketch work and
split both. Two findings drove it: §4 turned out to be safer than first
written (the `MethodDoc` tables already exist; no method name is claimed by
two modules), and §5's original proposal — a per-tool handler table — was the
wrong decomposition for a function that is a pipeline, not a switch.

### Verification, every stage

Round 1's protocol, which caught what tests do not:

1. `tools/test-fast.sh` during the work (~10 s), full headless
   (`ctest -LE gui -j$(nproc)`, ~79 s) before each commit.
2. `./tools/gui-tests-xvfb.sh` (~100 s) — shuffled, and record the seed.
3. `ctest -L pygui -j1` (~110 s).
4. **A pixel diff of a fixed scene through the real GPU.** Stages 3 and 6 move
   `SyncDocumentRenderers` / `SyncAssemblyRenderers`, which no test covers.
   Round 1 pinned exactly this with a three-body render: 0 of 2,764,800 bytes
   differed. Render before, render after, diff.
5. Stages 7–8 (the `remote/` ones) additionally: `designer --gui-test-list`
   diff (round 1's trick for its own stage 3), `tests/rpc_gate_smoke.py`, `tests/regen_honesty_smoke.py`, and
   `python3 benchmarks/runners/reference.py` — free, must stay 48/48.

**Stages 7–8's real gate is the benchmark rule.** Changing a tool, a description,
a refusal message or `help()` changes what `benchmarks/` measures — *no
benchmark, no claim*. A registry that preserves every method name, every
refusal string and every `help` text byte-for-byte is a refactor and needs
only `reference.py`. If it changes one message, it needs a sweep, and a sweep
is **$180 and must be asked for first**. Design stages 7–8 to not need one.

### What this costs and what it buys

- ~20,100 lines of `src/` move (plus ~10,400 of tests re-filed); **no behavior changes** and no format bump (nothing here
  touches `kVersion`).
- Incremental build after touching one UI concern: **26 s → ~7 s**.
- Full `designer_app` build: roughly flat after stage 5 (stage 3 costs ~55 s of
  duplicated header parsing; the PCH returns more than that across 208 files),
  and better under `-j` because the 48 s critical-path file is gone.
- Two new headless test surfaces (`Bom`, `TessellationCache`), one closed bug
  class (the CAM two-list problem), and one gate that can finally read data
  instead of parsing C++ (`rpc_gate_smoke`).

### What not to do

- **No unity build.** It would paper over §2 and make the incremental case
  worse, which is the case that is actually felt.
- **No abstract interface / DI over `AppState`.** The concrete-struct +
  free-function style is what makes the actions layer testable without a GPU.
  Round 1 said the layering is already clean — `core/` includes nothing
  up-stack — and it still is. Don't spend the round proving otherwise.
- **No renaming of RPC methods, refusal messages or `help()` text** in stages
  7–8, and no change to the ORDER `help()`/`describe` emits rows in.
  That is a benchmark change wearing a refactor's clothes.
- **Don't split `main.cpp` in this round.** Its 924-line `main()` and ~700
  lines of Vulkan setup are real (§5 of round 1 named it), but it is the one
  file with no test coverage at all, and stages 3–5 already move things it
  includes. Do it as its own round, against a pixel diff.
- **Don't do stage 5 (the PCH) alone** and declare the compile problem solved.
  It is 9%.
