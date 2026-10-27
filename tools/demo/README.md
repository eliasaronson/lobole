# Demo scenes

The release video's app footage (docs/plans/demo-video.md), recorded from
the real application. Each scene is a GUI test in the `demo` category
(`src/app/guitests/GuiTestsDemo.cpp`); the suite runs them Fast and records
nothing, so a UI change that breaks one fails the suite the day it lands.

```
tools/demo/record.sh                  # every scene -> scratch/demo/<scene>.mp4
tools/demo/record.sh parametric cam   # just these
tools/demo/record.sh --draft om615    # 1920x1080 at scale 1: a quick look
tools/demo/film.sh                    # the film scenes (strandbeest) -> scratch/demo/
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
| `om615` | the OM615 with its injection pump, seen level from a little above its middle, turned once round, cut down the crank and run through two revolutions in section; a part right-clicked in the viewport and opened; a boss sketched on its largest flat face and extruded, then suppressed and back from the feature tree, and the rollback bar dragged above it and back |
| `assembly` | the printed engine cut down its cylinders, its 68-step assembly sequence played in the Motion window at 1.5x from the block alone to the flywheel, each step's parts lit as they go home (the crank's half turn among them), ending put together |
| `strandbeest` | (`film.sh`, not a window) the Strandbeest's leg pair put together piece by piece by `export_assembly_video`: each bar, plate, pin and cap lit as it goes home along the path its check took, the whole linkage lit as the crank turns 15° before the cap that goes on with it, ending put together |

## Joining them

```
tools/demo/assemble.sh                       # scratch/demo/*.mp4 -> scratch/demo/
tools/demo/assemble.sh --scenes "om615 cam"  # these, in this order
```

`assemble.sh` joins the scenes -- `om615` first, then `strandbeest`, `parametric`,
`history`, `fem`, `cam` -- played at 1.5x (`--speed`: the scenes are
recorded at a pace a viewer can follow live, and a cut wants them quicker),
with a 0.4 s crossfade between each, a caption
over the first few seconds of each (its `CAPTION` table, in the app's own
Roboto), and an end card: the dark lockup on the viewport's colour with the
repository's address, taken from `origin` unless `--url` names another.
Every scene starts and ends on a still view of the app, so a plain fade is
all the joins need.

It writes two files. `lobole-demo.mp4` is the master at the clips' size.
`lobole-demo-1080p.mp4` is for the README: GitHub plays a video inline only
when it is attached through its own editor (the file dropped into the
README on github.com, which gives a `user-attachments` link), at most 10 MB
on a free plan, so the cut is a two-pass encode at the bitrate that fits
`--max-mb` for the video's length, and the script fails if it does not. A
longer video means a lower bitrate: past about 90 s the interface's text
starts to soften.

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

The clips and the generated models are the MAIN checkout's `scratch/`
(`record.sh` finds it through git), so a scene recorded from a worktree
lands beside the others.

## The assembly scene

`assembly` plays the printed engine's sequence
([generators/engine](../../generators/engine/README.md)), registered only
when `LOBOLE_DEMO_ENGINE` names it; `record.sh` sets it to a copy of
`scratch/engine/engine.lob` (`LOBOLE_DEMO_ENGINE_SOURCE` for another). The
sequence is checked off camera (about a minute and a half), the section is
the XZ plane through the bores and the crank, the camera looks at the cut
from in front, a little to the right and above, and Play runs at 1.5x
(`kAssemblySceneSpeed`): about 40 s recorded, under 30 s in the cut. Play
runs on the interface's clock, which a recording fixes at 1/60 s a frame,
so the clip shows the sequence at its own pace however slowly a 4K frame
draws; and it stops at the end, put together, which is the scene's last
still. The cut leaves it out by default (`strandbeest` shows a sequence
instead); `--scenes` puts it back.

## The film scenes

`strandbeest` is not a window recording: `tools/demo/film.sh` opens a
saved project in its own offscreen instance (a scratch port, under an 8 GB
memory cap, stopped when the script exits) and films its assembly sequence
with `export_assembly_video` at 3840x2160 (`--draft`: 1920x1080), through
the generator's own `film_sequence` so the camera is the one the
generator's `--video` uses. The film moves from its first frame and stops
on its last, so `film.sh` holds the first frame 1 s and the last 2 s
(ffmpeg's `tpad`): every scene starts and ends still, and the crossfades
join it like the others. It films the saved stage 1 of a generator run,
`scratch/strandbeest-film/stage1/strandbeest_stage1.lob`
(`LOBOLE_DEMO_STRANDBEEST_SOURCE` for another), made with

```
python3 generators/strandbeest/build.py --port PORT --stage 1 --no-controls \
    --out scratch/strandbeest-film
```

About two minutes at 4K; about 28 s of clip, under 20 s in the cut.

## Writing a scene

A scene sets itself up off camera and records only what the video shows.

- While a scene records, the pointer's base speed is 2.5 times the
  engine's (`kSceneMouseSpeed`). The engine already stretches a move under
  one second of travel to 0.5-1 s, so this cuts only the long crossings.
- Waits are `Hold`, and camera moves `OrbitFor` and `FlyToBodies`: real
  time when recorded, one frame in the suite. Never mouse moves for timing
  -- they glide at Cinematic speed.
- `OrbitFor` is a turntable about the world vertical in exact fractions of
  the angle, so a full turn ends where it began, with the horizon held
  level every frame (moving the eye alone keeps the camera's tilted up, and
  over a turn the model tumbled). `Camera::Orbit` is a
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
