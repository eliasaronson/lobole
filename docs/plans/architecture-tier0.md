# Tone curve: colour that matches the swatch

The rest of architecture tier 0 has landed; this is what is left of it.

## The problem

Appearance colours render lighter than picked. Both renderers ran the
Narkowicz ACES fit without the 0.6 exposure it was fitted for, then a 2.2
power, then a ×1.1 lift (`src/shaders/mesh.frag`, `ReadbackStill` in
`src/render/RayTracingRenderer.cpp`). A linear 0.2 grey card lands near byte
186 under the default lights, where the corrected curve gives 133 (a plain
sRGB encode of 0.2 is 124). generators/house compensates with a 0.55
factor on every colour it picks from a photograph.

## The fix

Exposure 0.6 before the ACES fit, no ×1.1, and the piecewise sRGB encode, in
both paths. The CPU constants are in `src/render/ToneCurve.h` and the shader
ones in `src/shaders/tonecurve.glsl`, and a test checks the two agree. The
studio backdrop is re-solved so it lands on the same bytes as before. Branch
`wtcolour` (worktree ../designer-wtcolour) has it built, with before/after
stills and a README in scratch/colour-curve/.

It changes every picture the app makes, so it lands only on the user's word.
After it lands:
- re-record the pixel baseline (`tools/ci-local.sh --pixel-record`);
- re-tune the wood and plywood colours that were darkened for the old curve;
- drop `EXPOSE` from generators/house.

## Files

`src/shaders/mesh.frag`, `src/shaders/tonecurve.glsl`,
`src/render/ToneCurve.h`, `src/render/RayTracingRenderer.cpp`,
`src/shaders/{miss.rmiss,closesthit.rchit}` (studio backdrop),
`tests/test_tone_curve.cpp`, docs/rendering/README.md#tone-mapping.

## Verification

- The headless and GUI suites.
- `render_pixel_smoke` against the re-recorded baseline.
- The scratch/colour-curve/ comparisons looked at.
- generators/house without `EXPOSE`, compared with the photographs.
