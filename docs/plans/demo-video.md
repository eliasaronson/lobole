# The release demo video

A 60-90 second video for the first release: what Loböle does, shown in the
real app, re-recordable whenever the interface changes before (or after) the
release.

## The problem

A hand-driven screen recording costs a retake per mistake and goes stale with
every UI change. The app can already be driven by script, and the GUI test
engine it bundles has a Cinematic speed (pauses between actions, a software
mouse cursor drawn into the frame) and a video recorder that pipes frames to
ffmpeg, now fed with frames by the app. What is missing is the scenes, the
script that records them, and the footage around them.

## The shape of it

1. **The app footage is built**: the capture, five demo scenes in the
   suite, and `tools/demo/record.sh`, which records them at 1920x1080
   (tools/demo/README.md).
4. **Rendered footage** from what exists: `export_motion_video` for the
   OM615 in section and the planetary gearbox, `render_still` for still
   shots, and the branded log end (`generators/logo/`) for the close.
5. **The agent segment is recorded live**, not scripted: a real Claude Code
   session building a part over MCP beside the app, sped up. A scripted
   one would not show what it claims.
6. **The edit** (Kdenlive or Resolve) is by hand: captions burned in, no
   voice, music only with a licence that allows redistribution.

## The shot list

| # | scene | length | source |
|---|---|---|---|
| 1 | the OM615 turning over in section, rendered clean (no interface) | 5 s | `export_motion_video` |
| 2 | a sketch, constrained, extruded; one variable changed and the part follows | 15 s | demo scene `parametric` |
| 3 | the history panel: a checkpoint, a rollback, the diff of the text format | 10 s | demo scene `history` |
| 4 | an agent builds a part from a prompt | 20 s | live recording |
| 5 | an FEM study's stress plot; a CAM toolpath simulating its cut | 15 s | demo scenes `fem`, `cam` |
| 6 | the OM615 in the app: turned, cut open, run; a part opened from it and a boss added | 20 s | demo scene `om615` |
| 7 | the log end, the lockup, the URL | 5 s | `generators/logo/`, `assets/brand/` |

## Files

- `tools/demo/footage.sh` (new): the rendered shots into `scratch/demo/` --
  the OM615 and planetary gearbox motion studies through
  `export_motion_video`, and the log end from `generators/logo/build.py` --
  on a scratch instance.
- `tools/demo/assemble.sh` is built: the scenes joined with crossfades,
  captions and the end card, as a master and a README cut. The rendered
  shots join it as scenes once `footage.sh` makes them.
- `tools/demo/README.md`: the shot list, what each clip is, and where the
  live agent segment and the edit come in.

## Verification

- Every clip in `scratch/demo/` watched end to end before it goes into the
  edit; the finished video watched once at full size and once muted on a
  phone.
