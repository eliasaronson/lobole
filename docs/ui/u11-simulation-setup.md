# U11 — Simulation setup: directions that come from the part (2026-08-17)

> Status: **SHIPPED**. The first clicks-only round on the FEM workspace, run
> after the user reported "fix direction when adding forces in simulation and
> arrows".

Simulation is the one workspace that emits numbers a person acts on — a factor
of safety, a peak stress, a deflection. Every other workstream has had a
clicks-only round; this one had not, and its setup defaults were world
literals.

## What was wrong

**Gravity pulled sideways.** The "Gravity (self-weight)" checkbox set
`(0, 0, −9.81)`. World up in this app was **+Y** at the time (Z-up since
docs/up-axis.md, 2026-08-28 — the default now derives from
`conv::kGravity`), so self-weight pushed the part
toward −Z: a cantilever sagged in the wrong direction *and* bent about the
wrong axis, which for a 60 × 6 × 10 beam is a 2.8× stiffness difference. The
number that came out was wrong, and it looked like an answer.

**A force ignored the face it was placed on.** The direction defaulted to
`(0, 0, −1)` — a world vector typed into a panel *before* the face is picked,
which can only be right by luck. It now defaults to the negated outward normal
of the face that is clicked: pushing on a face is what placing a force on it
means. `fem::DefaultForceDirection` is one function, called by both the GUI
placement and `add_load` when no `direction` is given, so clicking and
scripting cannot drift apart. A "Custom direction" checkbox restores the
explicit vector for anyone who wants it.

**A force defaulted to 0 N.** Arming the tool and clicking a face placed a load
that solves to zero stress everywhere. Arming now seeds the magnitude from the
part's own weight (mass × g, and weight/area for pressure) — a real number
derived from what the load acts on, which the user can then edit. This is the
`defaults/*` gate's rule (docs/ui/u10-defaults-and-refusals.md) applied to the
one place where a silent no-op is most expensive.

**The first Run of every new part failed, and only said so afterwards.** A new
body carries the DEFAULT material, which deliberately has no stiffness data
(`CADMaterials.h`: fail rather than guess). So Run failed with "material has no
stiffness data" — as a tooltip on a small ⚠, after the click, with the fix
living outside simulation mode in the feature's Material combo. The panel now
says it up front, per body, and offers the material combo in place.

**A fixed support drew as a single red stick.** The glyph computed a second
in-plane axis and threw it away (`(void)other;`), so the hatch fanned along one
axis only. Both axes now draw, and the anchor reads as an anchor.

**The load list hid the direction.** It showed `Force 1/face3 (250.0 N)` — the
magnitude, not the vector that decides the answer. It shows both now.

**`File ▸ New` left the app in simulation mode**, with the simulation toolbar
over a brand-new part and the panel editing the new document's empty study
list. `ReplaceDocument` now exits the mode, the same way it already dropped
assembly and drawing state.

**Four model-toolbar buttons had no ids** — Simulation, Measure, Toggle planes
and Render were addressable only by icon codepoint, the "grep the source"
problem U9 was supposed to end. They are `###tb_simulation`, `###tb_measure`,
`###tb_planes`, `###tb_render`.

## The verification that matters

A 60 × 6 × 10 steel cantilever, fixed at x = 0, gravity on, run through the
GUI: peak displacement **2.05e-4 mm at the free end**, against **2.08e-4 mm**
from beam theory (`wL⁴/8EI`) — 1.4%. Gravity pulls down, with the right
magnitude, about the right axis.

## Two test-surface bugs found while writing the tests

- **`popup_items` could not close what it opened.** It pressed Escape, which
  falls through to the app and cancels the tool session the popup belongs to
  (listing a Loft's sections closed the Loft). The obvious fix — click the
  opener again — cannot work either: a combo popup is drawn *over* its opener,
  so the click lands on the popup. It closes the stack directly now
  (`ImGui::ClosePopupToLevel`).
- **An `ImGui::PushID` scope makes a widget unaddressable by ref.** The first
  version of the material combo was `PushID(bodyId)` + `"###material"`, which
  only a numeric id from `ui_items` could reach. It is `"Material###material_
  <feature>"` with no ID scope — one combo per body, each with its own name.

## Not done

- **The Model dock is too narrow for this panel** at the default layout width:
  "Mesh size (mm, 0=auto)", the hint line and the load list all clip. Widening
  the default dock is a layout change for every panel, so it wants its own
  look.
- **A placed load cannot be edited** — only deleted and replaced. The list now
  shows magnitude and direction, which is enough to *see* a mistake but not to
  fix it in place.
- **Pressure and Fixed are not covered by a GUI test** to the depth Force now
  is (place → check the stored load), only by the arm-and-click path.

## S0 (2026-09-16): a setup you can read

The first outside pass at the visualization, planned as phase S0 of
[../simulation/assembly-studies-plan.md](../simulation/assembly-studies-plan.md).
Before it, a study drew one glyph per load at its face CENTRE, tinted no
face, drew no gravity, and its legend was a colorbar with a maximum and a
zero. What changed, and why:

- **The face is the boundary condition; the glyph is its type.** Every BC
  face is tinted through the mesh renderer's face-highlight channel in its
  family's colour — fixtures green, loads magenta, keep faces cyan (the
  palette in `mesh.frag`, indexed by `MeshRenderer::HighlightTint`). The
  push constant was at its 128-byte ceiling, so the tint rides in the top
  four bits of the highlight range's end. `AppendSimulationFaceTints` is
  part of `SyncSelectionHighlights`, because that projection clears first.
- **A pressure is a field.** `SampleFace` lays a Halton grid over the
  face's triangles — one sample per `spacing²` of area, spacing derived from
  the view-scaled glyph length, at most 160, never fewer than the centre —
  and each sample gets an arrow along its own triangle's inward normal. A
  fixed support repeats its anchor hatch on the same grid; the hatch strokes
  lean OUT of the face, since leaning in they were depth-hidden. A force
  keeps one resultant arrow (the draggable handle) over its tinted face.
- **Gravity is drawn**: an arrow leaving the centre of mass along g, in the
  see-through overlay channel (the centre of mass is inside the solid). The
  centre of mass is cached against the document version.
- **Every BC carries a label** — "1.2 kN", "0.5 MPa", "fixed", "keep",
  "g 9.81 m/s^2" — in the viewport draw list, behind windows, switchable
  off in the panel ("Labels in viewport"). `SimulationLoadLabel` is pure and
  pinned by `test_actions.cpp`.
- **The legend states its claim**: study name and analysis, the deformation
  factor, the quantity and unit, quarter ticks, the mode number and
  frequency for a modal result, and where the maximum is and on which body.

Found on the way: a load placed over RPC with a bare element name
(`feature_id` 0, the study RPCs' documented convention) solved correctly and
drew NOTHING, because `ResolveElementItem` required the feature id to match
while `fem::ResolveFace` searched every body. It now searches every body
too. And the `--offscreen` instance builds no overlays at all — the glyph
projection lives in `DrawUI` — so a study's picture needs the windowed
instance (`headless-preview.sh --gui`).

GUI cases: `sim/pressure_is_a_field_over_the_face`,
`sim/fixed_and_loaded_faces_are_tinted`,
`sim/gravity_is_drawn_at_the_centre_of_mass`.

On an ASSEMBLY tab (S1, 2026-09-16) the same glyphs, tints, labels and
legend draw for the assembly's study: every anchor is occurrence-qualified,
a rigid support is a ghost with its contact faces tinted as a fixture, a
contact clamp is labelled "clamped at «support»", and the field overlay
replaces the meshed occurrence's meshes only.
