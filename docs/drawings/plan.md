# Drawings — 2D drafting output — design

> Status: **draft, for sign-off**. The roadmap has carried "2D drawings /
> drafting output — deliberately out of scope until after assemblies; revisit
> then" since the beginning. Assemblies are done (A1–A3 + A2.5; A4 in
> flight), so this is the revisit. Verdict: build it — the geometric core
> (hidden-line-removed projection) is OCC's `HLRBRep`, not code we write, and
> the supporting machinery (a third document kind, stable edge references,
> screen-space line rendering, staleness stamps, export plumbing) all has a
> shipped pattern in this codebase. The honest cost is assemblies-scale
> (A1–A3 together), with dimensions as the long pole.

## Why

A drawing is the classic fifth pillar (part / assembly / drawing / render /
measure) and the first output artifact this app can produce that other
*people* — machinists, reviewers, suppliers — consume without running CAD.
The LLM angle is unusually strong: "make a dimensioned drawing of the
bracket, front/top/right at 1:2, export PDF" as one RPC chain is a
capability no mainstream CAD exposes headlessly. Views project and export
without a GPU (HLR is pure kernel), so the whole loop short of screenshots
is CI-testable, per the headless-core rule.

## What v1 is — and is not

**v1: one sheet per drawing document, projected views, driven dimensions,
title block, SVG/PDF export.**

- A **drawing** is a third document kind. One sheet (A4–A0 / letter,
  landscape/portrait) per drawing document; multiple drawings per project.
- **Views**: a base view of a source document (part *or* assembly) in a
  named orientation (front/back/left/right/top/bottom/iso), plus projected
  children folded orthographically from a base view and kept aligned to it.
  Per-view scale (sheet-standard ratios), visible edges solid, hidden edges
  dashed and per-view toggleable.
- **Dimensions are driven** — measured from projected geometry, read-only,
  re-measured on view update. The six sketch kinds carry over: distance /
  distance-X / distance-Y / radius / diameter / angle.
- **Title block**: a simple field table (project, document, scale, date,
  author) + border. Template customization deferred.
- **Export**: SVG and vector PDF, plus the sheet in `screenshot` when a
  drawing tab is active.

**Explicitly deferred (decisions, not gaps):**

- **Section, detail, broken, and crop views** — each is real projection +
  annotation work; base+ortho+iso covers the core need. Section views can
  later reuse `Measure::CrossSectionArea`'s cutting machinery.
- **Balloons + BOM table on sheet** — wants assembly drawings to mature
  first; the BOM data source already exists (A4 phase 2).
- **Exploded drawing views** — A4's exploded state applied to a drawing
  view; additive later (a view flag + the explode transform at projection).
- **Multi-sheet drawings** — the model below keeps sheet state on the
  drawing document so a `sheets[]` list is an additive change.
- **GD&T, weld/surface symbols, tolerance fields** — the annotation deep
  end; not v1.
- **Driving dimensions from the sheet** — refused outright, not deferred:
  drawing dims are measurements. Editing geometry happens in the part.
- **DXF output** — the interchange plan defers DXF "same status as 2D
  drawings"; once drawings exist, a DXF writer of sheet line-work becomes a
  small X-milestone there, not here.
- **Templates/styles** (dim text height, arrow style, layer colors) — one
  hardcoded ISO-ish style in v1 (3.5 mm text, existing arrow glyphs).

## 1. Projection: OCC HLR, spike-gated

> **D0 gate: PASSED (2026-07-14).** Shipped as `core/DrawingProjection.{h,cpp}`
> (exact `HLRBRep_Algo`, TKHLR linked) + `tests/test_drawing_hlr.cpp`
> (`[drawing][hlr]`, all green in the 363-test suite). Results, measured on
> the dev machine, Release `-O3`:
> - **Traceability — PASS.** `unresolvedSources == 0` on every fixture (box,
>   cylinder, blind-bore block, two shared-TShape instances, the v17-full
>   part, 25/49-instance grids). Cylinder side-view silhouettes trace to the
>   generating cylindrical face; the "shaft diameter" case works. Key finding:
>   the `HLRAlgo_EdgesBlock::OutLine` flag is **not** the silhouette signal
>   (HLR sets it on a face's real boundary edges when the face is viewed
>   edge-on). The robust rule is *"a silhouette is a DS edge that maps to no
>   source model edge"*, and its generating face is the DS face whose wire
>   block contains it. Edge identity is IsSame, then a shared stored-geometry-
>   handle + location fallback for edges the outliner splits.
> - **Performance — PASS by ~3 orders of magnitude.** Real part (3 bodies):
>   ~1.5 ms/view. 25-instance assembly (~525 DS edges): ~12 ms. 49-instance
>   (~1029 edges): ~26 ms. Thresholds were 5 s (part) / 15 s (assembly). No
>   crashes or hangs on any fixture. **`HLRBRep_PolyAlgo` promotion NOT
>   triggered** — exact HLR is the default for both parts and assemblies;
>   the crash/hang mitigations (`OSD::SetSignal` + `try/catch`, per-view error
>   string) are in the wrapper as insurance, not because these fixtures needed
>   them.
> - **Curve output — documented.** Iso view returns lines (`GeomAbs_Line`) +
>   ellipses (`GeomAbs_Ellipse`, tilted circles) — conics stay symbolic and
>   are recoverable via `Circle()`/`Ellipse()` (the wrapper exports
>   `conicCenter`/`conicRadius`; a flat bore rim comes back a `GeomAbs_Circle`
>   with radius 8.0 exactly). Discretization scales with deflection (82→146
>   points on the top view at 0.05→0.01 mm). SVG mapping for D4: line→`L`,
>   circle/ellipse→`A`, else polyline at the deflection default (0.05 sheet-mm
>   post-scale). No design amendments — `sil/<faceName>` refs (decision 4)
>   stand. **Proceed to D1.**

`HLRBRep_Algo` (exact) takes shapes + a projector and yields visible/hidden
edge curves in view space, categorized by `HLRBRep_HLRToShape`: sharp edges,
smooth edges, **outlines (silhouettes)** — the projection-generated curves a
shaft's side view is made of — each in visible and hidden flavors. TKHLR
ships in our OCC 7.9.3 install; D0 linked it in `OPENCASCADE_LIBS`
(`CMakeLists.txt`, one token).

**D0 is a spike with a decision gate**, exactly like FEM0:

1. **Source traceability** — dimensions need each projected curve mapped
   back to its generating 3D edge (for sharp edges) or generating *face*
   (for silhouettes). HLR keeps this association internally; the spike
   proves we can extract it on 7.9.3 (TechDraw does, so it exists — the
   question is how ugly).
2. **Exact-HLR performance** on a real part and a mid-size assembly —
   seconds is acceptable (updates are explicit, below), tens of seconds
   changes the design (would promote `HLRBRep_PolyAlgo` from preview option
   to default).
3. **Curve output shape** — conics vs. discretized polylines, and what
   discretization SVG/PDF export wants.

Known reality check: exact HLR is the slowest, crashiest corner of OCC
(TechDraw's issue tracker is the evidence). The mitigation is architectural:
projection runs per-view on explicit update, failures set a per-view error
string (the `Feature::SetError` pattern), and a failed view keeps its last
good geometry with a stale badge rather than blanking the sheet.

## 2. A third document kind

A drawing is a `DocKind::Drawing` — the A2 precedent applies verbatim, and
the two-kind assumption is shallow (16 `DocKind::` uses in 5 files, surveyed
2026-07-13). The touch list: the enum (`AppState.h:203`), a `drawing` member
in `PartSlot` (`AppState.h:217-229`), `StashActive`/`ActivateSlot`/
`SetActivePart` (`AppActions.cpp:479-500,608`), an `AddDrawing` action + a
`+Dwg` tab button (`UI.cpp:97-101`), panel routing (`UI.cpp:122-129`), a
drawing branch in `SyncDocumentRenderers` (`AppActions.cpp:1674`), project
index `"type":"drawing"` + `LoadDrawing` (`AppActions.cpp:695-780`), and the
RemoteServer tab-guard list (`RemoteServer.cpp:240-259`).

**Model** (`DrawingDocument`, mirroring `AssemblyDocument`'s shape):

```
DrawingDocument {
  sheet:  { size /*A4..A0, Letter*/, landscape, titleBlock fields },
  views:  [ DrawingView { id, name,
                          sourceDocId,          // stable project doc id (part OR assembly)
                          orientation,          // front|back|left|right|top|bottom|iso
                          baseViewId,           // 0 = base; else folded child, kept aligned
                          scale + scaleExpr,    // 1:1, 1:2, ... expression-capable
                          position,             // sheet mm; children: along fold axis only
                          showHidden } ],
  dims:   [ DrawingDim  { id, kind,             // the six sketch dimension kinds
                          viewId,
                          refs,                 // projected-geometry refs, below
                          labelOffset } ] }     // sheet mm, the sketch labelOffset pattern
```

- Ids from a per-document never-reused counter, like features/occurrences.
- **Source references by stable project-document id**, the occurrence rule
  (`Occurrence::partId`, A2 decision 5): renames can't break a view.
  Resolution through the same resolver pattern as `assembly::PartResolver`
  (`AssemblySolve.h:25`).
- **Projected geometry and measured dimension values are derived, never
  stored** — the element-names/BOM/FEM rule. Serialized state is setup only;
  opening a project re-projects on first view of the drawing tab (or on
  `update_drawing`).
- **Staleness = the A4 stamp pattern** (`solvedPartVersions`,
  `AppActions.cpp:859-877`): each view stamps its source's
  `Document::Version()` at projection; a differing version shows a stale
  badge on the view and the tab. **Update is explicit** (button/RPC) —
  exact HLR costs seconds, same never-auto rule as FEM solves, deliberately
  unlike the cheap A4 assembly re-solve.

**Persistence** — format bump (**v30**; FEM shipped as v29, so drawings take
v30), additive per F3:

```
drawings/bracket-dwg/
  index.json               # {format, version, kind:"drawing", sheet:{...}, views:[ids], dims:[ids]}
  views/1-front.json
  dims/7-dist.json
```

One `designer-document` format string, one `kVersion`, `kind` discriminated
— the A2 rule that keeps the golden corpus and migration policy singular.
Project envelope: `"type": "drawing"` in the v2 `documents` list (additive).
Golden + round-trip per the migration policy. Undo via a
`DrawingHistory` snapshot stack, the `AssemblyHistory` pattern.

## 3. Dimension references: toponaming, projected

The design's one genuinely novel reference problem: a dimension attaches to
*projected* geometry that is regenerated on every update. The answer is the
toponaming contract extended by one qualifier:

```
ProjRef { viewId,
          ElementRef edge,     // the source 3D edge's stable name, OR
          ElementRef silFace,  // the silhouette's generating face
          end }                // 0 = whole edge, 1|2 = an endpoint vertex
```

- **Sharp/smooth edges** carry their source edge's `ElementRef`
  (`ElementMap.h:13`) — the same `{featureId, name}` mates and fillets use.
  For assembly sources, the ref is occurrence-qualified
  (`{occurrenceId, featureId, name}`) — a small forward step toward the
  cross-part refs A4 deferred, but confined to drawings.
- **Silhouettes have no source edge** — they are named by their generating
  *face* (`sil/<face's MappedName>`), which D0 proves extractable. This is
  what makes "diameter of a shaft in side view" — the most common dimension
  in existence — attachable and regeneration-stable.
- **Dangling refs are first-class**: after an update, a dimension whose ref
  no longer resolves (edge filleted away, face consumed) renders in the
  error style with the standard dangling-ref message — never silently
  rebound, the T3 rule.
- Measured values recompute from projected geometry on every update (the
  sketch-dimension pattern: display is always live-measured,
  `SketchInteraction.cpp:1368-1371`), formatted through a **new shared
  `core::FormatLength/FormatAngle`** — the survey found `%.3g`/`%.4g`
  scattered ad hoc across dimension text, editors, and readouts; drawings
  centralize it and the sketch/measure call sites migrate opportunistically.

## 4. The sheet viewport: 2D at last

There is no 2D camera today — sketch mode aligns the *perspective* camera
to the plane (`Camera::AlignToFace`). A sheet wants none of that; v1 gives
the drawing tab its own **orthographic pan/zoom state** (`{offset, scale}`
in `AppState`, scroll-to-cursor zoom + middle-drag pan in `ViewportInput`'s
drawing branch) and never touches the orbit camera.

Rendering reuses the sketch stack wholesale:

- **Line work through the `PrimVertex` pipeline** (`SketchRenderer.h:17-23`)
  — screen-space-constant thickness, per-vertex RGBA, AA — fed an ortho
  view-proj built from the sheet state. Visible edges ~1.8 px, hidden edges
  thinner + dashed via a sheet-space generalization of
  `AppendPrimDashedLine` (`AppActions.cpp:1958`, currently world-unit
  dashes), title block/border as lines + a `SetQuads` sheet background.
- **Text via the ImGui foreground draw list**, the sketch-dimension
  mechanism (`SketchInteraction.cpp:1380`) with one change: drawing text is
  **sheet-anchored** (ISO 3.5 mm on the sheet), so it draws through
  `AddText(font, size, ...)` with size = 3.5 mm × zoom. Bilinear font
  scaling at extreme zoom is accepted v1 (the unsorted-ghost-blending kind
  of tradeoff); SDF text is the rendering-plan upgrade if it ever matters.
  Export text is true vector text regardless (§5), so output is always
  crisp.
- Dimension graphics (extension/dimension lines, the `Arrow` glyph,
  label drag via `labelOffset`) port from `DrawDimensions`
  (`SketchInteraction.cpp:1291-1453`) with sheet-mm coordinates in place of
  sketch UV.
- View placement/drag: click a view → outline highlight + drag (children
  constrained to their fold axis); the A2.5 drag-interaction conventions,
  radically simpler in 2D.

## 5. Output: SVG and vector PDF, hand-rolled

Both are line art + text — no dependency warranted:

- **SVG**: polylines/paths + `<text>`, mm user units, ~200 lines of writer.
  The reference output format, trivially diffable and testable.
- **PDF**: a minimal single-page vector writer (path operators + Helvetica
  from the base-14 fonts, no embedding) is a few hundred lines and covers
  "send it to the machine shop". If it grows warts, vendoring `libharu` is
  the escape hatch; start without it.
- Writers live in core (`core/DrawingExport`), GUI-free, driven by the same
  projected-geometry structures the viewport renders — one geometry
  pipeline, two consumers, so the export is headless-testable in CI without
  a single pixel rendered.
- Surface: `export_drawing {path}` RPC (format by extension, the
  `ExportDialog` convention at `FileDialogs.cpp:114-120`), MCP mirror, an
  entry in the Export dialog + menu.

## 6. Surface: panel, RPCs, MCP

**Panel** (drawing tab's dock, replacing part panels per the
`UI.cpp:122-129` routing): sheet settings + title block fields; view list
(add view → source + orientation picker, scale, show-hidden, delete);
per-view stale badge + Update button (and update-all); dimension tool
(pick two projected refs → dim, the mate-tool interaction pattern).

**RPCs** (snake_case branch + `@mcp.tool` mirrors, per convention):

- `create_drawing {name?, sheet?}` / `list_drawings`
- `add_drawing_view {drawing, source, orientation, base_view?, scale?,
  position?}` / `remove_drawing_view`
- `update_drawing {drawing}` — explicit re-projection, returns per-view
  status + timings
- `add_drawing_dim {drawing, view, kind, refs, offset?}` /
  `remove_drawing_dim`
- `export_drawing {drawing, path}` (svg/pdf by extension)
- `screenshot` works when a drawing tab is active (sheet renders through
  the normal frame; `CaptureScreenshot` needs no change).

LLM parity end-to-end: create drawing → three views → update → dims →
`export_drawing bracket.pdf`, fully headless. That chain is the acceptance
test.

**Tests:** `test_drawing.cpp` — HLR unit tests are GPU-free (project a box:
6/4/2 visible edges by view; a cylinder: 2 silhouettes + 2 circles side-on;
hidden-line counts with a blind hole), dimension re-measure after a
parameter edit, dangling-ref behavior after a fillet consumes an edge,
serialization round-trip + golden at the bump. `drawing_smoke.py` headless:
the LLM-parity chain, asserting the SVG parses and contains the expected
element counts (never byte-golden — HLR discretization isn't stable across
OCC versions, the FEM rule).

## Decisions for sign-off

1. **Scope: drawings v1 = one sheet per drawing document, base/projected/iso
   views of parts or assemblies, driven dimensions (six kinds), title
   block, SVG + vector PDF export.** Sections/details, balloons/BOM tables,
   exploded views, multi-sheet, GD&T, templates all deferred; **driving
   dimensions refused permanently** (a drawing measures; the part edits).
   DXF becomes a small interchange milestone once this exists.
2. **Projection = OCC exact HLR (`HLRBRep_Algo`), D0 spike-gated** on
   source-edge/silhouette traceability, real-part performance, and curve
   output — the FEM0 pattern. `TKHLR` added to the link list. Per-view
   errors keep last-good geometry; `PolyAlgo` is the fast-preview fallback
   the spike may promote.
3. **A drawing is the third `DocKind`** (the A2 playbook: enum, `PartSlot`
   member, tab bar, sync branch, project-index type, tab-guard lists —
   touch list surveyed and enumerated). Model mirrors `AssemblyDocument`;
   sources referenced by **stable project-document id** with
   resolver-based lookup; **projected geometry and measured values are
   derived, never stored**; staleness via per-view source-version stamps
   with **explicit update** (HLR is expensive — the FEM rule, not the A4
   auto-solve rule). Format **v30** (FEM shipped v29), `drawings/` dir,
   file-per-view/dim, one kVersion, golden per bump.
4. **Dimension refs extend toponaming by a view qualifier**: sharp edges by
   source-edge `ElementRef` (occurrence-qualified for assembly sources),
   silhouettes by generating face (`sil/<name>`), endpoints by index;
   dangling refs render as first-class errors and never rebind. Values
   re-measure on update and format through a new shared
   `core::FormatLength` (centralizing today's scattered `%.3g`).
5. **Sheet viewport = new 2D ortho pan/zoom state** (no orbit camera);
   line work through the existing `PrimVertex` screen-space pipeline +
   CPU dashing; **text = zoom-scaled ImGui draw-list at ISO 3.5 mm sheet
   height** (bilinear scaling accepted v1; export text is always true
   vector text, so output quality never depends on the viewport).
6. **Export = hand-rolled SVG + minimal vector PDF in core**, GUI-free,
   consuming the same projected structures as the viewport; `libharu`
   only as a later escape hatch. Tested headlessly by parsing, never
   byte-golden.
7. **Headless-first phasing** (below): projection and export close the LLM
   loop before dimensions exist; dimensions (D3) are the long pole and ship
   last before polish.

## Phasing

0. **D0 — HLR spike (gate):** link TKHLR; headless harness projects a
   bracket part + a mid-size assembly; extract categorized edges, prove
   source-edge + silhouette-face traceability, measure timings. **No
   further drawings work until this passes.**
1. **D1 — the sheet spine — SHIPPED (2026-07-14):** `DocKind::Drawing`
   plumbing (the §2 touch list), `DrawingDocument` (+ `DrawingHistory`
   snapshot undo) + serialization (v30, `kind:"drawing"`, `drawings/<name>/`
   dir + golden `tests/golden/drawing/v30-drawing.json`), the sheet viewport
   (2D ortho pan/zoom in `AppState::Sheet2DView`, border + title block via the
   ImGui background draw list — simpler than the PrimVertex path the design
   proposed, and sufficient for D1's static chrome; D2 can migrate view
   line-work to PrimVertex if AA/perf needs it), `+Dwg` tab + `DrawSheetPanel`,
   `create_drawing`/`list_drawings` RPCs + MCP mirrors. Verified headless by
   `tests/drawing_smoke.py` (create → save → reopen → list) and
   `tests/test_drawing.cpp` (model + v30 round-trip + kind-guard). The
   `DrawingView`/`DrawingDim`/`ProjRef` structs are already defined and
   serialized so D2/D3 populate them without a format bump.
2. **D2 — views:** projection core in `core/` (HLR wrapper → categorized
   sheet polylines with refs), view model (orientations, folding alignment,
   scale), visible/hidden rendering, staleness stamps + explicit update,
   view RPCs. **The LLM loop closes here** with D4's exporter stub for SVG
   — a headless dimensionless drawing.
3. **D3 — dimensions (the long pole):** projected-geometry picking, the six
   kinds, `ProjRef` resolution + dangling behavior, dimension rendering
   (ported `DrawDimensions` + sheet-anchored text), label drag,
   `core::FormatLength`, dim RPCs.
4. **D4 — output + polish:** SVG writer (may land early in D2 for the smoke
   test), PDF writer, export dialog/menu/RPC/MCP, `drawing_smoke.py`,
   stale/error UX, docs.

Order rationale: D0 kills the only unknown for a day's work; D1 before D2
because views need somewhere to live and render; SVG export deliberately
straddles D2/D4 so the headless chain is testable before any dimension
exists; D3 last because it is the largest and depends on everything else.
After v1: sections and balloons/BOM-table are the natural D5/D6, informed by
what assembly drawings teach; DXF goes to interchange.
