# Drawings for the pan/tilt friction round (E)

**Problem.** The pan/tilt sheets (generators/pantilt/README.md, friction
item 5) carry geometric tolerances as text ("SEAT B PERPENDICULAR TO A
WITHIN 0.02"), so a datum symbol is a letter nothing refers to; a vertical
dimension's value is written horizontally across its line; a small feature
can only be shown by drawing the whole view bigger; the generator places
every balloon and the seat's linear Ø by hand, and its leaders still cross;
the 1/4-20 camera screw is a modelled part listed as MADE; and the parts
list says ALUMINUM where the shop needs 6082-T6.

**The shape of the fix.**

1. **Feature control frames.** A note `style: "fcf"` carrying a structured
   frame: an ISO 1101 symbol (straightness, flatness, circularity,
   cylindricity, profile of a line or surface, parallelism,
   perpendicularity, angularity, position, coaxiality, symmetry, circular
   and total runout), the tolerance in model mm, a Ø zone, an MMC/LMC
   modifier, and up to three datum letters. Each datum it names must be a
   `datum` note on the same sheet, which makes the datum symbols datum
   features rather than letters; a symbol that takes no datum (form) is
   refused one, one that needs a datum (orientation, runout, coaxiality,
   symmetry) is refused without, and a Ø or modifier the symbol cannot
   carry is refused. It is drawn as the ISO frame (the symbol as line
   work, so the PDF's Helvetica needs no glyph for it), with a leader to
   the feature, or stacked under a dimension's value (`dim`). A datum letter
   is unique on a sheet and is not I, O or Q; removing a datum an FCF names
   is refused naming the frame.
2. **Aligned dimension text** (ISO 129-1 method 1). A linear value (the
   three distances and a Ø across two lines) is written parallel to its
   dimension line, above it, readable from the bottom or the right: a
   vertical one turns 90° and stands left of its line. `SheetText` carries
   an angle; the viewport, the SVG and the PDF turn it the same way; the
   label's box (placement, the validator) is the turned text's.
3. **Detail views.** A view with `detail {view, center [x, y, z] world mm,
   radius_mm, label}` draws its parent's projection (source, orientation,
   rotation, section, hidden) at its own scale, clipped to the circle; the
   parent draws the circle in thin line lettered, the detail its boundary
   and `DETAIL B (5:1)` under it. Same HLR as the parent, so a cache hit.
4. **Balloon layout and a placed Ø.** `layout_balloons {views}` puts one
   balloon on every parts-list row, on the view that draws the most of it,
   at slots on a ring around that view clear of everything else on the
   sheet, matched to their parts in angular order and then untangled by
   swapping any two leaders that cross (each swap shortens the total, so it
   ends); leaders end on a visible edge of their part away from other parts'
   lines. `check_drawing` reports crossing balloon leaders. A Ø across two
   lines with no offset is placed in lanes beyond the view along the walls,
   not as a radial leader.
5. **ASME B18.3 socket head cap screws.** A vendored-shape table
   (`asme_b18_3`, inch sizes #4-40 to 3/4-10 UNC, with 1/4-20 and 3/8-16)
   typed from ASME B18.3 Table 1 with the source cited in its header, in
   inches and scaled once on load; an unknown size is refused naming the
   stocked ones; the designation is `ASME B18.3 1/4-20 x 1/2`.
6. **Material grades.** Library rows `aluminum_6061_t6` and
   `aluminum_6082_t6` with their own yield and modulus, and a `grade` on
   every row ("6061-T6", "PETG", "STEEL") that the parts list and the title
   block print. A grade row cuts and feeds as its family (aluminium) in CAM;
   sheet metal and tubes refuse it, since their bend data is for another
   alloy.

**Files.** `src/core/drawing/` (DrawingDocument, DrawingDimension,
DrawingRender, DrawingExport, DrawingValidate, a new DrawingBalloons),
`src/ui/DrawingActions.cpp`, `src/ui/DrawingPanel.cpp`,
`src/remote/RemoteDrawings.cpp`, `src/core/io/Serialization.cpp` (format
v151: `detail` on a view, `fcf` and `dim` on a note),
`src/core/parts/` and `assets/parts/asmeb18_3def.csv`,
`src/cad/CADMaterials.*`, `src/core/cam/CamFeeds.cpp`;
`generators/pantilt/build_drawings.py`.

**Verified by.** Unit tests for each refusal, the FCF and detail graphics,
aligned text and its box, the layout's no-crossing result on a crafted
case, the ASME table against the standard's rows and the grade rows; a
golden at v151; the pan/tilt sheets regenerated with FCF-backed datums, the
balloons laid out by the app (the generator's placement removed), the camera
screw an ASME B18.3 part and the plate's grade in its title block and parts
list, rasterised and inspected.
