# Demo scenes

The release video's app footage (docs/plans/demo-video.md), recorded from
the real application. Each scene is a GUI test in the `demo` category
(`src/app/guitests/GuiTestsDemo.cpp`); the suite runs them Fast and records
nothing, so a UI change that breaks one fails the suite the day it lands.

```
tools/demo/record.sh                  # every scene -> scratch/demo/<scene>.mp4
tools/demo/record.sh parametric cam   # just these
tools/demo/record.sh --draft om615    # 1920x1080 at scale 1: a quick look
```

`record.sh` runs them at Cinematic speed (a visible cursor that glides and
pauses) on its own Xvfb display, with scratch config and state directories.
It records at **3840x2160 with the interface at scale 2** (`--window-size`,
`--ui-scale`): a 1920x1080 layout drawn at the density of a HiDPI display,
since Xvfb reports a scale of 1. The scenes switch ambient occlusion to full
while they record. Recording itself is the test engine's, fed by the app:
docs/ui/testing.md#recording-a-scene. A 4K run takes several minutes a
scene, because every frame is rendered again at full size and encoded at a
quality meant for video; `--draft` is for checking a scene's choreography.

| scene | shows |
|---|---|
| `parametric` | a variable made in the Variables panel; a rectangle drawn on Top and dimensioned 40 x 25; extruded with the variable typed as its distance; the variable changed, and the block follows |
| `history` | a version named; a cylinder stood on the block's top face and added to it; saved; the named version restored, and the cylinder gone |
| `fem` | a cantilever's static study run; von Mises, the deflection magnified 100x, then displacement |
| `cam` | a plate pocketed, the cut simulated end to end |
| `om615` | the OM615 with its injection pump turned once round, cut down the crank and run through two revolutions in section; a part right-clicked in the viewport and opened; a boss sketched on its largest flat face and extruded |

## The OM615 scene

`om615` plays the generated engine, which lives in `scratch/` and is
registered only when `LOBOLE_DEMO_OM615` names it -- `record.sh` sets that
to a copy, since the scene edits a part. Generate it with the app as built,
or its mates may name faces this build no longer does (an engine generated
nine days earlier lost 56 bolted mates that way):

```
python3 generators/om615/om615.py --port PORT --stage preview --no-export \
    --out scratch/demo/om615-gen
```

`LOBOLE_DEMO_OM615_SOURCE` points `record.sh` at another copy.

## Writing a scene

A scene sets itself up off camera and records only what the video shows.

- Waits are `Hold`, and camera moves `OrbitFor` and `FlyToBodies`: real
  time when recorded, one frame in the suite. Never mouse moves for timing
  -- they glide at Cinematic speed.
- `OrbitFor` is a turntable about the world vertical in exact fractions of
  the angle, so a full turn ends where it began. `Camera::Orbit` is a
  trackball in mouse pixels about the camera's own up: neither degrees nor
  a turntable, and a "full turn" with it leaves the camera elsewhere -- a
  point picked before it then lands on another part.
- Frame the camera from the bodies' box (`FrameBodies`, `CentreLeftOf` to
  keep a model clear of a floating tool window); `view.fit` does not frame
  bodies a scene adds to the document directly.
- A click that must hit a face goes where nothing but the face is near
  (`RoomiestPoint`): a face's centre of mass can sit a few pixels from a
  hole's edge, and the edge wins.

Watch every clip end to end before it goes into the edit.
