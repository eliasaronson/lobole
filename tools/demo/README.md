# Demo scenes

The release video's app footage (docs/plans/demo-video.md), recorded from
the real application. Each scene is a GUI test in the `demo` category
(`src/app/guitests/GuiTestsDemo.cpp`); the suite runs them Fast and records
nothing, so a UI change that breaks one fails the suite the day it lands.

```
tools/demo/record.sh                  # every scene -> scratch/demo/<scene>.mp4
tools/demo/record.sh parametric cam   # just these
```

`record.sh` runs them at Cinematic speed (a visible cursor that glides and
pauses) in a 1920x1080 window on its own Xvfb display, with scratch config
and state directories. Recording itself is the test engine's, fed by the
app: docs/ui/testing.md#recording-a-scene.

| scene | shows |
|---|---|
| `parametric` | a rectangle drawn on Top, extruded; its height a variable, changed in the Variables panel, and the block follows |
| `history` | a version named, a cylinder added and saved, the named version restored and the cylinder gone |
| `fem` | a cantilever's static study run, its von Mises stress shown |
| `cam` | a plate pocketed, the cut simulated end to end |
| `print` | the print check on a part with a piece held off the bed; Show points at it |

A scene sets itself up off camera and records only what the video shows.
Its waits are `Hold`/`OrbitFor` (real time when recorded, one frame in the
suite), never mouse moves, which glide at Cinematic speed; it frames the
camera from the bodies' box (`FrameBodies`), since `view.fit` does not frame
bodies added to the document directly. Watch every clip end to end before
it goes into the edit.
