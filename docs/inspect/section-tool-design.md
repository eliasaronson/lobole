# Section views — from a menu toggle to a tool — design

> Status: **P1 shipped 2026-08-19**; P2/P3 below still open. The decisions
> carry their outcomes. Supersedes the I3 raster section UI in
> [plan.md](plan.md): `View > Section View` is **deleted**, not kept
> alongside. Two competing UIs for one feature is how the offset in the menu
> and the offset an agent set through `set_section` came to disagree in the
> first place.

## Why

The section view is a three-field menu: one plane, one offset, session-only,
no list, no way to point at the geometry you want cut. It was written as the
visual half of I3 and never grew past it. Three rounds of dogfooding on the
floating dock ended in "I have not gotten it to work properly", and the fixes
that followed (4972533, 3964b8b — a cutter aimed at the world origin, absent
assembly caps, an offset that stepped in 0.1 mm on a 3-metre model) were the
floor, not the feature.

What a section view is *for*: looking inside an assembly while you work, from
more than one direction at once, with the cut where you point rather than
where the coordinate system happens to be.

## What it becomes

A **Section tool**, in the toolbar, opening the shared tool window with a
**list of sections** — the same shape as the Measure tool, which is already a
non-feature tool with a deletable list of picks
(`BeginToolWindowFrame(state, "Measure")`).

```
  Section                                      [x]
  ────────────────────────────────────────────────
   1  Plane [Top (XZ) ▾]   Offset [ 194.93 ]  ⟲ ⨯
      angle U [  0.0 ]     angle V [  0.0 ]
   2  Plane [Front (XY) ▾] Offset [  -12.0 ]  ⟲ ⨯
      angle U [ 30.0 ]     angle V [  0.0 ]
  ────────────────────────────────────────────────
   [+ Add section]        model spans -420 … 420
```

Per section: a source plane, an offset along its normal, two angles, a flip,
and visibility. In the viewport each section draws its plane outline and a
**drag arrow along the normal** that sets the offset directly — the
`PreviewHandles` machinery that already drags extrude depth and fillet
radius.

Everything else stays as it now behaves: the cut is capped per body in the
body's own colour, on part tabs and assembly tabs alike, and the offset
defaults to the middle of what the tab is showing.

## Decisions

**1. Multiple sections intersect.** A fragment survives only if it is on the
kept side of *every* enabled section. That is what makes two planes produce a
quarter-cut, and it is what every CAD does. (Union would mean "cut away where
any plane says so", which reads as one plane winning at random.)

**2. Angles pivot about the plane's own centre.** See the section below; a
tilt must not also translate the cut.

**3. Angles AND datum planes, for different jobs.** The two angle fields are
a throwaway view tilt. A datum plane is a modelled, named, parametric thing
that other features already reference. A section can be built on either; the
angles then apply on top of whatever plane was picked.

**4. `View > Section View` is deleted.** Replaced by the toolbar tool and a
"Section…" command-palette entry. The menu's plane/offset state
(`AppState::sectionBasePlane`, `sectionOffset`) folds into the section list.

**5. Caps stay body-coloured.** A cut in the body's own colour reads as
material; a single "section colour" reads as a decal. (Hatching remains
deferred — it is a drawing convention, and drawings have their own section
views.)

**6. Session state in v1, persisted in P3.** Sections are a view property,
like the camera. Saving them means a `kVersion` bump and a golden pair, which
should not gate the tool.

**7. FOUR sections maximum** (revised from eight while building). The planes
travel as a `vec4[4]` through five passes — mesh, wireframe, SSAO prepass,
field overlay and the shadow map. The shadow pass is the binding constraint:
it carries its planes in push constants next to a `mat4`, and Vulkan only
guarantees 128 bytes, so four planes fill it exactly (there is a
`static_assert`). Eight would have meant either a descriptor rework for the
shadow pass or a different limit there than everywhere else. SolidWorks
offers three.

## Does the angle complicate things? No — and here is the evidence

Everything downstream of the plane is **already normal-agnostic**:

- The clip is `dot(n, worldPos) > d` in five shaders — direction-blind.
- `Measure::BuildSectionCutter` derives the cutter quad's in-plane axes from
  `n` (`u = normalize(x̂ − n(x̂·n))`), so an arbitrary normal already builds a
  correct cutter.
- The drag arrow projects the ray onto the axis it is given.
- The derived default offset and the "model spans a … b" readout both project
  the scene bounds onto `n`.
- `set_section` has always accepted `{origin, normal}` — the axis-aligned
  `base_plane` form is a convenience on top.

Measured, on the 76-occurrence floating dock, sweeping a plane tilted about X
through the model's middle:

| angle | cut area |
|---|---|
| 0° | 5 914 mm² |
| 15° | 676 765 mm² |
| 30° | 498 261 mm² |
| 45° | 236 521 mm² |
| 60° | 223 732 mm² |

Non-degenerate at every angle, through the same code path the caps use. So
the angle costs: two UI rows, the definition→plane math, and tests — a couple
of hours, not a phase.

**The one real decision is the pivot.** Rotating a plane about an axis
through the world origin swings the cut across the model as the angle
changes — the cut runs away from where the user put it. Rotating about the
plane's **own centre** (the scene centre projected onto the plane, which is
already computed for the cutter quad and the default offset) tilts the cut in
place. Recommended, and worth stating in the doc because it is invisible
until someone drags an angle on a part that is a metre from the origin.

Definition → plane, for a section with base normal `n₀`, in-plane axes
`u₀, v₀`, offset `t` and angles `α, β`:

```
  c  = centre of the visible scene, projected onto the base plane
  n  = Rot(u₀, β) · Rot(v₀, α) · n₀
  d  = dot(c, n) + t
```

so `t` stays "distance from the model's middle", whatever the angles are.

## Phases

**P1 — the tool — SHIPPED 2026-08-19.** `vec4 clipPlanes[4]` through all five
passes (an all-zero slot is off, so no count is needed); `std::vector<SectionView>`
in `AppState` replacing `sectionPlane`; `###tb_section` on BOTH the part and
assembly toolbars (one id — only one toolbar draws at a time) opening the
tool window with per-row plane / offset / Flip / visible / delete plus Add,
Clear all and the model's span along the active normal; the drag arrow, which
took a non-feature path through `PreviewHandles` (`ComputeHandleSpec`
resolves a *pending feature* and refuses assembly tabs outright, so a section
drag resolves nothing, regenerates nothing, and is the one handle that works
on an assembly); caps per section, each trimmed by the others;
`View > Section View` deleted, with a "Tool: Section" command-palette entry
in its place.

Reverse shipped in P1 rather than P2 — it is one sign flip on the plane
(`{-n, -d}` is the same surface, the other half-space) and it is what makes a
cut usable when you guessed the direction wrong.

**P2 — plane sources and angles (~half a day).** Base plane / datum plane
(`PlaneRefCombo`, as Split uses) / picked planar face (`PickSlot`), the two
angle fields, flip, per-section visibility.

**P3 — persistence and parity (~half a day).** Sections saved with the
project (`kVersion` bump + golden pair); `add_section` / `list_sections` /
`update_section` / `remove_section` RPCs with MCP coverage, replacing
single-plane `set_section` (kept as an alias that edits section 1, since
scripts and `docs/` reference it).

Angles are in P2 only because P1 is already a day's work; they are ~2 hours
and can move forward if the tilt is wanted first.

## The part that is actually hard

**Caps under multiple planes.** Today a cap is `body ∩ cutter-face` for one
plane. With two, each cap must also be trimmed by the other plane's kept
half-space, or the fill juts out past the neighbouring cut — the classic
"floating lid" artefact. OCC does it with a common against a half-space
solid per other enabled section; the cost is one more boolean per cap per
extra plane, on a rebuild that already only fires when the plane settles
(0.08 s for one plane across the whole dock).

Everything else in P1 is plumbing.

## Verification

- **GUI**: add two sections through the tool, both clip and both cap; delete
  one, its caps go with it; drag the arrow and the offset follows; a section
  on an assembly tab caps every occurrence (the existing
  `section/assembly_cut_is_capped_and_centered` test extends to the list).
- **Unit**: a cap under two planes is trimmed to their intersection (area
  equals the analytic quarter-section); cap area equals `CrossSectionArea` at
  an angle, not just on axis; the definition→plane math keeps `t` measured
  from the scene centre as angles change.
- **Scale**: the floating dock (76 occurrences), one section, 0.08 s for the
  area pass; cap rebuilds only fire when a drag settles.
- Both suites per commit; `tools/ci-local.sh` before pushing.

## Not in scope

- **Per-component exclusions** (Onshape's "parts to exclude", for keeping
  fasteners whole). Wanted eventually; it needs a per-section occurrence set
  and a rendering path that skips clipping for chosen instances.
- **Hatched cut faces** — a drawing convention; drawing views already section
  independently.
- **Section in drawings** — `docs/drawings/` owns that.
