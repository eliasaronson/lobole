# Twisted and herringbone extrudes

## The problem

There is no helical or twisted prism.

**How the planetary works around it.** `generators/planetary/build.py`
builds every herringbone gear as two lofts through eight rotated copies of
the outline, fused at the apex. That costs:

- **Accuracy:** the skin is not the helix. At four sections a half it
  strayed far enough that a sun and planet clearing 0.28 mm at every
  section touched between them. At eight, the closest point still comes
  out about 5 µm short.
- **Speed:** 18 sketches and a 13 s union per gear, and dense B-spline faces.
  Those faces make exact distance between two gears take about 20 s a pair
  (see measure-speed.md).
- **Generality:** any helical gear, twisted vase or spiral flute needs the
  same workaround.

**What already exists.** `HelicalSweep` (src/core/HelicalSweep.cpp, used by
Thread and Worm) builds exact screw-motion solids with
`BRepOffsetAPI_MakePipeShell` in binormal mode.

## The shape of the fix

**Extrude gains `twist_deg`:** the total turn of the profile about the
extrude direction over the extrude's length.

- A non-zero twist sweeps each closed outer loop along a helix about that
  axis, with binormal = the axis (`SweepProfileAlongSpine` with
  `SweepFrame::Binormal`).
  - With a constant binormal, every profile point undergoes the exact
    screw motion, whatever the helix's radius, so the side faces are
    helicoids. There is one per sketch edge, as a prism has, so element
    names follow the same scheme.
  - The lead comes from the length and the twist. `twist_expr` is live like
    `distance_expr`.
- **Inner loops** (a bore in the profile) are swept the same way and cut
  from the outer.
- **End modes:** blind and symmetric only. Up-to-face, through-all and thin
  extrudes with a twist are refused, naming what is accepted.
- **`herringbone: true`** twists by `twist_deg` over the first half and back
  over the second. The second half is the first mirrored about the
  mid-plane: a mirror reverses the twist and meets the first half on an
  exactly coincident face. The halves are fused with glue.

**The planetary moves onto it.**

- `herringbone()` becomes one sketch and one Extrude per gear.
- The twist's sign rule and the phasing stay where they are.
- `SPANS` and the loft comments go, and the clearance gate tightens back to
  the gap itself (the loft's 5 µm shortfall was the only reason for the
  0.01 mm allowance).
- The generators README's "a loft through the outline turned along the
  helix" becomes the Extrude.

Format: `twist_deg` and `herringbone` are new Extrude fields, so `kVersion`
is bumped with a golden and a format.md row.

## UI

**The Extrude editor** (src/ui/FeatureEditor.cpp, the Extrude branch)
gains two controls after Distance:

- **`ExprDim "Twist (deg)"`,** shown for Blind and Symmetric only, where
  Reversed is gated the same way.
- **A `Herringbone` checkbox,** shown when the twist is not zero.

**Refusals and defaults:**

- A refused combination is the feature's own error (`WhyIncomplete`), so
  the tool window shows it in red and disables Accept. The RPC refuses
  through the same path.
- The twist defaults to 0, so the defaults gate's `extrude` case still
  changes the document.

**The viewport handle:**

- **What:** a twist arc about the extrude axis at the end cap, the same
  `HandleKind::Rotate` arc Revolve uses (src/ui/PreviewHandles.cpp), with
  its 45° snapping and Ctrl to free it.
- **What it needs:** the preview layer draws one handle per feature today,
  so `ComputeHandleSpec` returns a list. The distance arrow and the twist
  arc are then both live, and the same change gives a two-sided extrude
  its second arrow (docs/ui/viewport-handles.md lists that as not built).
- **The arc** hides when the twist is an expression, as the arrow does for
  a distance expression.

**GUI test:** the defaults case unchanged. Then set a twist in the field
and drag the arc; both commit, and the arc snaps.

## Files

- `src/core/features/ExtrudeFeature.{h,cpp}`: fields, the twisted branch.
- `src/core/HelicalSweep.{h,cpp}` or `SweepFeature.cpp`: a shared
  straight-axis screw sweep of a planar wire.
- `src/core/io/SerializeFeatures.cpp`, `SerializationInternal.h`, a golden
  in `tests/golden/`, docs/versioning/format.md.
- The RPC's feature dialect (`describe`'s Extrude row): `twist_deg` carries
  its unit (`tests/rpc_units_gate.py`).
- The Extrude tool's GUI fields (`src/ui/` extrude tool session), with
  defaults per docs/ui/tool-sessions.md#defaults-and-refusals.
- `generators/planetary/build.py`, `generators/planetary/README.md`,
  `generators/README.md`.
- docs/modeling/README.md.

## Verification

- **A square twisted 90° over its height:**
  - its volume equals the prism's;
  - its top face is the bottom face turned 90°;
  - its side faces are not planar.
- **A circle twisted:** the volume and faces are a cylinder's.
- **Herringbone:**
  - the mid-plane section equals the profile turned by half the twist;
  - top and bottom match the profile;
  - it is one valid solid.
- **The planetary:**
  - exact clearances at least the gap, with no allowance;
  - the STL is five shells;
  - both motion studies pass;
  - the print part builds faster than the loft version (time it before and
    after);
  - measuring a gear pair takes a fraction of today's 20 s.
- **Refusals:** a twist with an unsupported end mode or a thin wall.
- **GUI suite:** the Extrude tool with a twist.
