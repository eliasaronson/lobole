# A fitted view and a rendered still disagree about the frame

## The problem

`fit_view` frames the geometry for the VIEWPORT's aspect
(`FitCameraToBox` reads `cam.GetAspectRatio()`, ui/ViewActions.cpp:114).
`render_still` renders at the size it was asked for, projecting with
`GetProjectionMatrixForAspect(width / height)` (ui/RendererSync.cpp,
`MakeStillDesc`). Both are individually right and nothing connects them, so
fit-then-render crops whenever the image's aspect is narrower than the
viewport's.

Measured on the smart speaker's `bottom_wall` (210 x 320 x 19), fitted from
the top and rendered at 400 x 1100: about a fifth of the panel's width is in
frame, the rest is outside the picture. The part is centred and correctly
projected — it is the FRAMING that was solved for a different picture.

It is worse than it looks for an agent, because:

- the requested size is the only aspect it knows, and it is not the one used;
- `get_state`'s camera block reports `distance`, `position` and `target` and
  no `fov` or `aspect`, so a caller cannot reproduce the projection to check
  its own framing;
- offscreen has no window, so the viewport aspect is whatever the framebuffer
  happens to be — an invisible input to every scripted render.

The GUI's Render dialog has the same mismatch (a 1920x1080 still of a
viewport that is not 16:9), which is the same cause and not a separate bug.

## The fix

1. **`FitCameraToBox` takes the aspect it is framing for**, defaulting to the
   camera's. The fit maths already has `aspect` as a local; it just has one
   source for it.
2. **`fit_view {width?, height?}`** — frame the geometry as it will appear in
   a picture that size. Both or neither; a width without a height is refused
   rather than half-applied.
3. **`render_still {fit?}`** — fit at the still's OWN size before rendering.
   This is the one an agent wants: "a picture of this part, framed", without
   having to know or guess what the viewport is.
4. **`get_state` camera reports `fov` and `aspect`**, so a script can
   reproduce the projection and assert its own framing. Read-only.

**Built.** `FitCameraToBox`/`FitCameraToDocument` take `forAspect`
(ui/ViewActions.{h,cpp}); `fit_view {width, height}` and
`render_still {fit}` in src/remote/RemoteDispatch.cpp; `fov_deg` and `aspect`
in both camera replies. Measured on `bottom_wall` from the top: the furthest
corner sat at 1.719 of the frame for a 400x1100 picture and now sits at
0.952 for every aspect from 0.36 to 4.0 — which is exactly the 1.05 margin
the fit intends, so it is tight rather than merely safe.

**Not built, and known:** the GUI's Render dialog still offers 1920x1080
regardless of the viewport's shape, so a wide part can be cut in the still
while whole on screen. The still is projecting correctly for the size asked
for — the gap is that the dialog neither frames for it nor defaults to the
viewport's aspect. Left because a dialog that re-aims the camera the user
framed by hand is its own surprise, and the right answer there is probably a
default size that matches the viewport.

## Verification

`tests/headless_smoke.py` — not a render test, because the property needs no
picture: fit for three very different picture shapes and project the part's
eight bbox corners through the REPORTED camera at that aspect, asserting
every one lands inside NDC. It needs no GPU, so it runs in the headless
suite rather than skipping on most machines.

It also asserts the negative first: a plain `fit_view` must NOT already
frame a 400x1100 picture. Without that line the whole check would pass
vacuously if the aspect ever stopped being threaded through — both calls
would frame identically and nothing would notice.
