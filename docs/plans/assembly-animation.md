# Assembly animation: readable films, and the demo's assembly scene

What is built is in docs/assemblies/README.md#playing-and-filming-a-sequence:
a sequence's timeline (groups and poses included) laid out from its check,
Play / scrub / Stop in the Motion window, `export_assembly_video`, and the
clock's films (`--video`).

## The problem

- **A section shows parts that are not placed yet.** The printed engine is
  a closed box, so its film only reads in section (cut through the
  cylinders' plane): pistons down their bores, the crank up, caps and pins,
  the crank's half turn, valves and springs. But a hidden occurrence still
  draws its cut caps: at t = 0, with only the block placed, every piston,
  rod, spring and the bedplate show as flat cut-outs. The playback hides
  an occurrence's mesh and outline (`OutlineRenderer::SetMeshVisible`) and
  not its section cap.
- **The moving part does not stand out.** The printed assemblies are one
  PLA colour, so in both clock films and the engine's the part going home
  is the same blue as everything already placed, and a viewer cannot tell
  what this step is.
- **The demo has no assembly scene.** The release video's scenes
  (tools/demo/README.md) are GUI tests recorded at 3840x2160; the sequence
  check and its playback are a strong thing to show and are not in it.
- **The Strandbeest has no film** (its sequence has a pose step: the
  crank at 15° before the back pivot cap goes on).

## The shape of the fix

1. **Hidden means hidden, in section too.** Whatever hides an occurrence
   for the playback hides its section caps as well, through the same
   visibility the renderers already share rather than a second list.
   Check every renderer that draws an occurrence (mesh, outline, section
   caps, shadows, SSAO's depth) so none still draws a hidden one.
2. **The step's parts in a highlight.** While a step's occurrences travel
   (from appearing to seated) they are drawn in the selection/highlight
   colour the viewport already uses, and return to their own material
   when home; a pose step highlights what it moves. On by default in Play
   and in `export_assembly_video`, with a parameter to turn it off.
3. **Pace.** A playback speed (Play and the export), so a long sequence
   can be shown at a pace that fits a cut, without changing the
   timeline's own speed rule.
4. **The `assembly` demo scene.** A GUI test in the `demo` category
   (`src/app/guitests/GuiTestsDemo.cpp`), recorded by `record.sh` like the
   others: the printed engine (`generators/engine/`, from `scratch/`,
   skipped when it is not there, as `om615` is) opened, cut through the
   cylinders, its sequence played in the Motion window from the block
   alone to the flywheel, ending on a still. A row in
   tools/demo/README.md and a caption in `assemble.sh`'s table; where it
   goes in the default order is the demo's call (after `om615`).
   Recorded at 4K and looked at.
5. **The Strandbeest's film.** `generators/strandbeest/build.py --video`,
   as the clock's: each stage's sequence filmed beside the card, off by
   default, refused with a headless instance.

## Files

- The playback's visibility and highlight: `src/ui/SequencePlayback.cpp`,
  `src/ui/RendererSync.cpp`, the section-cap renderer and whichever other
  renderer item 1 finds; `src/ui/MotionVideo.cpp` for the export's
  parameters; the `describe` row in `src/remote/RemoteMotion.cpp`.
- `src/app/guitests/GuiTestsDemo.cpp`, `tools/demo/{record.sh,assemble.sh,README.md}`.
- `generators/strandbeest/build.py` and its README.
- `docs/assemblies/README.md#playing-and-filming-a-sequence`.

## Verified by

- A smoke (or the existing `assembly_video_smoke`): a sequence filmed with
  a section on, its first frame showing no pixel of a part not yet placed
  (compared with a film of the first part alone); the travelling part's
  pixels in the highlight colour mid-step and in its material once home.
- The GUI suite with the new `demo/assembly` scene run Fast.
  `render_pixel_smoke` after the renderer change.
- The engine scene recorded at 3840x2160 and its frames looked at; the
  Strandbeest's film made and looked at.
- The headless, GUI and pygui suites; `generator_strandbeest` without the
  flag.
