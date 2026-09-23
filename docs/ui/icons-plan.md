# Icons: one table, no look-alikes, and CAD glyphs Font Awesome lacks

## The problem

Every icon in the app is a Font Awesome 6 Free Solid glyph, picked one by one
where each button was written. An audit of all ~300 uses and the whole
1,400-glyph font found three kinds of fault.

**Look-alikes on one toolbar.** A toolbar gets scanned for a shape, so two
similar shapes on the same bar cost a misclick.
- Part bar: Revolve `ROTATE` vs Hem `ROTATE_LEFT` (one arrow, mirrored); Boolean
  `OBJECT_GROUP` vs Corner relief `OBJECT_UNGROUP`; Fit vs Refold (one
  arrow pair, reversed); Datum plane `TABLE_CELLS_LARGE` vs Split
  `TABLE_COLUMNS`.
- Sketch bar: all three arc tools are `BEZIER_CURVE`, which means spline (and
  Spline itself is a pen nib). Constraint glyphs and Slice are two eyes.
  Perpendicular is `ANGLE_RIGHT`, which is a `>` chevron, not a right angle.
- Drawing bar: X distance is a ruler, Y distance is arrows, and the aligned
  distance is a third shape again. Diameter is a half-filled circle.

**The same thing drawn two ways.** Four hand-kept tables repeat the toolbar's
choice, and they have drifted. `MateGlyph` (OccurrencePanel.cpp) disagrees
with the assembly bar on 4 of 6 mates. `OpGlyph` (CamPanel.cpp) says it uses
"the job toolbar's own" and does not for Profile or Drill. `FeatureGlyph`
(FeatureTreePanel.cpp) matches today only because nobody has changed a
button since.

**Not clean at 14 px.** The icon font is merged with a fixed 15 px advance
(Theme.cpp). Twelve glyphs in use are wider than that (16.8 px), so their
buttons are wider than their neighbours. The planes toggle swaps
`EYE` (15.1) for `EYE_SLASH` (16.8) and grows 2 px on every click, which its
own comment says cannot happen. Drum, Explosion, Scroll, Film, Mountain-sun,
Gauge and Box-open are pictures, and at 14 px they turn to noise.

Font Awesome has no CAD vocabulary at all: no fillet, chamfer, shell, loft,
draft, section, mate, dimension, arc or outlined circle. Re-picking inside it
fixes the duplicates. It cannot make Fillet anything but a dotted square
corner.

## The shape of the fix

### 1. One icon per command, in one table

Add `src/ui/Icons.{h,cpp}` with `const char* CommandIcon(std::string_view id)`,
a single table from command id (`part.fillet`, `assembly.mate_revolute`,
`job.profile`, `sketch.arc_center`, ...) to its glyph. Then:
- Toolbar buttons take their icon from it. `CommandButton` (Toolbar.cpp) gains an overload
  that looks the icon up, so Toolbar.cpp no longer names a glyph for any
  command button.
- `FeatureGlyph`, `MateGlyph` and `OpGlyph` return `CommandIcon` of the
  command that creates that thing, so a tree row cannot differ from the
  button that made it.
- The panel titles, status glyphs (warning, check, eye, trash) and playback
  controls are not commands. They stay where they are.

### 2. A small Designer icon font for what Font Awesome cannot draw

- **Source:** `tools/icons/build_icon_font.py`, which defines each glyph as
  geometry (shapely): strokes are buffered into outlines and unioned, so the
  font gets clean filled contours. A TrueType glyph cannot hold an SVG
  stroke, so hand-written SVGs would need an outline-conversion step. Drawn
  solid to match Font Awesome Solid's weight, on a 14-unit grid, the size
  the icons render at: one unit is one pixel at 100% UI scale, so edges land
  on pixels. The baseline sits at row 12 of 14, the box FA glyphs occupy, so
  the two fonts share a baseline and height.
- **Build:** the same script (fontTools, run through `nix-shell -p`) writes
  `deps/fonts/designer-icons.ttf` and `deps/fonts/IconsDesigner.h`
  (`ICON_DS_FILLET`, ...) at U+E900 and up; Font Awesome uses nothing in
  U+E700–EFFF. Glyphs are appended, never reordered, so codepoints stay put.
  Both outputs are committed and byte-reproducible, so a build needs no
  Python; `--check` fails when they do not match what the script would
  write. `--sheet OUT.png --compare` renders the review sheet.
- **Runtime:** Theme.cpp merges `designer-icons.ttf` after `fa-solid-900.ttf`
  with the same config and fixed advance. It is copied and installed exactly
  where `fa-solid-900.ttf` is (the `file(COPY)` and `install(FILES)` in CMakeLists.txt).

**The glyphs to draw**, 64 of them. Simulation and Explode were drawn too and
dropped at review: Font Awesome's gauge and explosion read better.

| bar | glyphs |
|---|---|
| part | extrude, revolve, sweep, loft, pipe, helix/3D curve, cylinder, sphere, datum plane, datum axis, boolean, mirror, fillet, chamfer, rib, thread, shell, draft, split, section |
| sheet metal | base flange, edge flange, hem, jog, corner relief, normal cut, flat pattern, unfold, refold |
| sketch | circle, ellipse, rectangle, centre rectangle, 3-point arc, centre arc, elliptic arc, horizontal, vertical, coincident, parallel, perpendicular, construction, offset, project |
| assembly | fastened, revolute, slider, cylindrical, planar, ball mates |
| drawing | aligned / X / Y distance, radius, diameter, angle |
| CAM | pocket, profile, face, walls, surface rough, surface finish, rotary rough, rotary finish |

Icons Font Awesome already does well stay FA: box (`CUBE`), line (`SLASH`),
spline (`BEZIER_CURVE`, moved from the arcs), point, trim (`SCISSORS`), pattern
(`GRIP`), hole (`BULLSEYE`), fit, measure, render, BOM, insert, fastener,
play, gears, and the simulation gauge and explode burst.

### 3. Width and centring

Every icon is loaded at ONE advance, min and max alike: a 14 px slot, the
grid Designer's glyphs are drawn on. Nothing can change a button's width any
more (the planes toggle grew 2 px per click, eye to eye-slash).

Measured on screenshots of all six bars, icons were NOT centred in their
buttons, and for three reasons, each fixed where it arises:
- **2 px low, every icon, both fonts:** a `GlyphOffset` of +2 px from the
  first toolbar on. Removed: unshifted, FA's glyph box is centred 5.0 px above
  the baseline, where the middle of Roboto's capitals is, so an icon inline
  with text lines up with it too.
- **~0.8 px left, every FA icon:** with `PixelSnapH`, ImGui TRUNCATES the
  offset that centres a glyph in its slot. Off for Font Awesome (its edges
  never landed on pixels at this size; it is oversampled like text), on for
  Designer's font, whose glyphs fill the slot exactly and need no offset.
- **Up to 2.5 px, some Designer glyphs:** drawn off-centre in their own box.
  The build now centres each glyph's ink in the 14 x 14 box by whole pixels,
  so grid edges stay on the grid; an odd ink extent lands half a pixel off.

Result, measured: every toolbar button's icon within 0.5 px of its frame's
centre, both ways, on all six bars.

## Gates

- **No duplicate on a toolbar:** the GUI test `toolbar/no_duplicate_glyph`,
  which checked the part bar only, now opens each toolbar in turn (part,
  sketch, assembly, drawing, CAM job, simulation). It gathers every `tb_*`
  button and flyout row and fails if two different commands show the same
  glyph, or if a button fell through to the unknown-command glyph. It checks
  what is drawn, so it cannot drift from the code the way a source scan
  could.
- **One glyph, one meaning, across bars:** the existing convention case "a
  toolbar glyph means one thing and a feature has one glyph" parsed the
  toolbar and tree sources. It now reads the table instead: a glyph shared by
  two commands must be one idea (an explicit same-idea list: hole, fillet,
  pattern, show/hide, ...), and every command a panel asks `CommandIcon` for
  must be in the table.
- **One table:** `test_conventions.cpp` fails if Toolbar.cpp,
  FeatureTreePanel.cpp, OccurrencePanel.cpp or CamPanel.cpp names an
  `ICON_FA_`/`ICON_DS_` glyph for something `CommandIcon` covers. In
  practice: no glyph literal in a toolbar button, and none in the three glyph
  tables.
- **Font is current:** the build-font script's `--check` mode, run from
  ctest, fails when the committed TTF or header differs from what the script
  would write.

## Verification

1. **Review sheet before wiring (you sign off here).** A PNG of every new
   glyph at 14 px and 28 px on the toolbar's own background, next to the icon
   it replaces, plus each toolbar laid out as a row. Nothing is wired into
   the app until the drawings are accepted.
2. **Real screenshots of every toolbar and the three trees**, from the built
   app. The windowed `--hidden` instance does not answer RPC, so this goes
   through a GUI-test capture (`--gui-tests` with a case that opens each bar
   and takes the `screenshot` RPC), not through `headless-preview --gui`.
3. **Suites:** the headless suite, the GUI suite (shuffled), and
   `render_pixel_smoke.py` (the viewport does not change, so it must still
   match byte for byte).
4. **Benchmark:** no RPC, tool description or refusal changes, so no
   benchmark run is needed.
5. **Docs:** update docs that name a toolbar icon (grep `docs/` for the
   `ICON_FA_` names that change) and the comments that describe the old
   glyphs (Toolbar.cpp's planes-eye note, `OpGlyph`'s claim).

## Files

- new: `src/ui/Icons.{h,cpp}`, `tools/icons/build_icon_font.py`,
  `deps/fonts/designer-icons.ttf`,
  `deps/fonts/IconsDesigner.h`, the GUI test, this plan
- changed: `src/ui/Toolbar.cpp` (its `CommandButton` gains the lookup),
  `Theme.cpp`, `FeatureTreePanel.cpp`, `OccurrencePanel.cpp`, `CamPanel.cpp`,
  `tests/test_conventions.cpp`, `CMakeLists.txt`, the packaging that lists
  `fa-solid-900.ttf`
