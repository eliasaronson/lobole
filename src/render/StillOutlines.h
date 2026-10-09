// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <vector>

// The traced still's outlines, from its primary-ray G-buffer: pure CPU (no
// Vulkan) so it compiles into designer_core and the tests can cover it.
// RayTracingRenderer traces the G-buffer and hands it here strip by strip
// (docs/rendering/still-render.md#outlines-on-a-traced-still).
//
// The viewport gets its edges from mesh adjacency; the still finds the same
// three things as discontinuities between neighbouring G-buffer texels:
//   - a different body id  -> a boundary / silhouette
//   - face normals apart by more than 60 degrees -> a feature edge (the
//     viewport's outline::BuildEdgeAdjacency featureCos = 0.5)
// and averages the edge mask, `scale` x `scale` texels to an output pixel,
// into coverage — which is what makes the line antialiased.
namespace stilloutline {

// One G-buffer texel exactly as raygen.rgen writes it (rgba32ui): the face
// normal's x,y then z as half floats, the hit distance's float bits, and the
// body id (0 = miss or shadow catcher).
struct Texel {
  uint32_t nxy, nz, dist, id;
};

// The line's shape in G-buffer texels, for a width in OUTPUT pixels.
struct Pen {
  int core = 0;       // texels within this distance are fully covered
  float fringe = 0.f; // weight of the ring at core + 1 (the fractional part)
  int reach() const { return fringe > 0.f ? core + 1 : core; }
};
Pen PenFor(float lineWidthPx, uint32_t scale);

// How many G-buffer rows a strip needs beyond the rows it answers for, on
// each side, to compute exactly what one whole-image pass would.
int HaloRows(float lineWidthPx, uint32_t scale);

// Coverage (0..1) for OUTPUT rows [outRow0, outRow1), written row-major to
// `out` ((outRow1 - outRow0) x auxWidth / scale floats).
//
// `strip` holds G-buffer rows [stripRow0, stripRow0 + stripRows) of an image
// auxWidth x auxHeight texels (auxWidth and auxHeight multiples of `scale`).
// It must cover [outRow0 * scale - halo, outRow1 * scale + halo) clipped to
// the image, halo = HaloRows(lineWidthPx, scale).
void Coverage(const Texel* strip, uint32_t auxWidth, uint32_t auxHeight,
              uint32_t stripRow0, uint32_t stripRows, uint32_t scale,
              float lineWidthPx, uint32_t outRow0, uint32_t outRow1,
              float* out);

} // namespace stilloutline
