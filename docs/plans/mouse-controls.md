# Mouse controls in preferences

## The problem

Keyboard shortcuts can be rebound (docs/ui/keybindings.md); the mouse
cannot. Orbit, pan, zoom-by-drag and roll are inline conditionals in
`HandleViewportInput` (src/ui/ViewportInput.cpp:159-225): middle drag, right
drag and Alt+left drag orbit, Ctrl turns any of them into pan, Shift into
zoom, Alt+middle into roll. Someone coming from SolidWorks finds it close to
home; someone from Fusion 360, Onshape or Blender has to relearn their hand
before they can use anything else in the app. The only navigation setting is
`zoom_to_cursor`. The speeds are constants in `Camera` (orbit 0.005 rad/px,
pan a hard-coded factor that ignores `m_panSensitivity`, zoom 0.1), and the
wheel's direction is fixed.

Two texts are already wrong about the mouse: the controls cheat-sheet
(`DrawControlsHelpOverlay`, src/ui/UI.cpp:451-478) still lists "Z: zoom",
and ViewportInput.h's header says Ctrl+click aligns to a face (it is Shift).

## The shape of the fix

**Navigation actions as settings rows.** Four gestures, each a `Field` row
in the Navigation section (src/core/io/Settings.cpp `kFields`), string-valued
with a fixed list of accepted values, so the settings page draws each as a
combo with no new widget:

| row | default (today's behaviour) |
|---|---|
| `nav_orbit` | `middle`, `right`, `alt+left` |
| `nav_pan` | `ctrl+middle`, `ctrl+right`, `ctrl+alt+left` |
| `nav_zoom` | `shift+middle`, `shift+right`, `shift+alt+left` |
| `nav_roll` | `alt+middle` |

A gesture is a button (`middle`, `right`, or `alt+left`, the left button
with Alt held) and at most one more held modifier (`ctrl`, `shift`, or
`alt` on the other two); a row holds one or more. The value is a
list in the JSON. The accepted set is the product of those, minus the bare
left button: left is picking, box select and every handle, so it navigates
only with Alt held. A value outside the set is refused, naming the accepted
ones, in the settings file as in `set_settings`. Two actions claiming one
gesture are refused the same way: there is no order between them to settle
it.

**Presets as one more row.** `nav_preset`: `lobole`, `solidworks`,
`fusion`, `onshape`, `blender`, `custom`. Choosing one writes the four
gesture rows; editing any of them by hand shows `custom`. Each preset's
gestures are written from that program's own documentation when this is
built, and cited in a comment beside its row, not from memory.

**A gesture table in code, not conditionals.** A small `core/io/MouseNav`
(no ImGui, unit-testable like `Keymap`): `Gesture {button, mod}`,
`ParseGesture`/`FormatGesture`, `struct NavMap` built from the four rows,
and `NavMap::ActionFor(button, ctrl, shift, alt)` returning a
`ViewportDrag`. `HandleViewportInput`'s drag block asks it instead of
testing buttons. The behaviour that is not navigation stays fixed and wins
where it already does: a right RELEASE before the drag threshold is still a
click (the assembly occurrence menu, the sketch dimension menu), Shift+left
click still aligns to a face, Ctrl+left still multi-selects. A modifier
pressed mid-drag still switches the action (asked again every frame, as
today).

**The drawing sheet takes the same table.** `HandleSheetInput`
(src/ui/DrawingPanel.cpp:622-666) pans on what is bound to pan and on what
is bound to orbit (a sheet has nothing to orbit), so a hand that pans in 3D
pans the sheet too.

**Speeds and the wheel.** `nav_orbit_speed`, `nav_pan_speed`,
`nav_zoom_speed`: dimensionless multipliers on the camera's own rates, 1 by
default, refused outside 0.25-4. `nav_invert_wheel` (bool) flips wheel zoom
in the viewport and on the sheet. `Camera` gets setters, and pan uses its
pan rate instead of the inline factor.

**The words follow the table.** The cheat-sheet is generated from `NavMap`
and the keymap, so it cannot go stale again; the header comment is fixed.

**Scripts can drive every gesture.** The remote `viewport_drag` action gains
`alt`, and `scroll` gains `ctrl`/`shift`, so a test can make any bound
gesture.

## Files

- `src/core/io/MouseNav.{h,cpp}` (new); `src/core/io/Settings.{h,cpp}`
  (the rows, `operator==`).
- `src/ui/ViewportInput.{h,cpp}`, `src/ui/DrawingPanel.cpp`,
  `src/render/Camera.{h,cpp}`, `src/ui/UI.cpp` (cheat-sheet),
  `src/ui/SettingsWindow.cpp` (the preset row writing the four rows).
- `src/remote/RemoteUiActions.cpp`, the `get_settings` result row in
  `src/remote/RemoteDispatch.cpp`, the regenerated `python/lobole/rpc.py`.
- `tests/test_mousenav.cpp` (new), `tests/test_settings.cpp`,
  `src/app/guitests/GuiTestsViewport.cpp`, the drawing GUI tests.
- `docs/ui/keybindings.md` (the "not bindable" line and the not-built list
  go), `docs/ui/settings.md` (the rows), `docs/ui/README.md` (viewport).

## Verification

- Unit: every accepted gesture parses and formats back to itself; every
  preset is free of clashes and never binds a bare left button; an unknown
  gesture, a bare `left`, a clash and an out-of-range speed are each refused
  with the accepted values named; the settings round trip keeps each row.
- GUI: the existing `viewport/*` navigation tests pass unchanged under the
  default. For each preset, a test makes every bound gesture and asserts the
  camera did that action (orbit turns the view direction, pan moves the
  target, zoom changes the distance, roll turns the up vector) and that an
  unbound one does nothing. Under a preset that orbits on the right button,
  a right click on an occurrence still opens its menu without moving the
  camera. Invert wheel and each speed change the camera the other way or by
  the factor. The sheet pans on the pan gesture.
- The headless suite, the GUI suite, `tools/ctest -L pygui`.
