// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string>

struct Body;
namespace printfit {
struct Process;
}
namespace printset {
struct Settings;
}

// The PRINTED mass of a body (docs/tolerances/README.md#printed-mass): what
// a slicer's walls, shells and infill weigh, against the solid the model is.
// An estimate, and said so: the slicer's real paths differ at small
// features, and a wall is taken as walls x line width thick.
//
// The method is the slicer's, on a raster: the body is meshed and cut into
// the profile's layers up the print direction; each layer's section is
// rasterised, and a pixel is
//   - SHELL (solid) within the walls' thickness of the section's outline,
//     or where any of the `top` layers above it or the `bottom` layers below
//     it is not material (the top and bottom shells, as a slicer finds them);
//   - CORE otherwise, weighted by the infill density.
// Mass, centre and inertia are the weighted sums over every pixel of every
// layer. A cube's are its shell and core to the pixel, and the pixel is a
// quarter of the line width, or coarser on a body big enough to need it
// (said in `pixelMm`).
namespace printmass {

struct Input {
  double layerMm = 0.0;     // the profile's layer height
  double lineWidthMm = 0.0; // the profile's wall line width
  int walls = 0;
  int topLayers = 0;
  int bottomLayers = 0;
  double infill = 0.0;          // 0..1
  glm::dvec3 up{0.0, 0.0, 1.0}; // the print direction, body frame, unit
};

struct Result {
  double solidMm3 = 0.0;     // the raster's material volume
  double shellMm3 = 0.0;     // of it, printed solid
  double coreMm3 = 0.0;      // of it, infill
  double effectiveMm3 = 0.0; // shell + infill * core: mass / density
  glm::dvec3 centerOfMass{0.0};
  // About the centre of mass, world axes, per unit density, weighted: the
  // convention Measure::BodyMass uses (OCC's matrix of inertia).
  double inertia[3][3] = {};
  int layers = 0;
  double pixelMm = 0.0;
  // Printed at 100 %: the solid's own properties, exactly (EstimatePrinted
  // takes them off the B-Rep; no layers, no pixels).
  bool exact = false;
};

// nullopt (with `why`) for a body with no shape, an input out of range, or a
// mesh that will not slice.
[[nodiscard]] std::optional<Result> Estimate(const Body& body, const Input& in,
                                             std::string* why);

// The estimate for `body` printed by `proc` with `s`: the layer and line
// width from proc's profile, the rest from `s` (direction unset = +Z).
// nullopt with `why` naming what is missing: no process, a profile that
// states no layer_height_mm or line_width_mm, or walls, top_layers,
// bottom_layers or infill_pct unstated. At 100 % infill the print is the
// solid, and the answer is its exact mass properties (`exact`). Remembered
// per shape and inputs, so a generator asking again pays nothing.
[[nodiscard]] std::optional<Result>
EstimatePrinted(const printfit::Process* proc, const printset::Settings& s,
                const Body& body, std::string* why);

// "slicer raster: walls x line width, top/bottom layers, infill weighted;
// an estimate": what an answer names the method as.
[[nodiscard]] const char* MethodNote();

} // namespace printmass
