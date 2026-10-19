// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A solid cut into bands along an axis (docs/inspect/README.md#insertion):
// spans of the axis over which its cross-section does not change, each with
// that cross-section as a 2D region. An extrusion, a bore, a straight tooth
// band is ONE band whatever its length; where a face is not along the axis
// (a cone, a fillet, a helical flank) the span is sliced `sliceMm` thick on
// a grid anchored at the axis origin, so two solids sliced with one
// thickness have aligned slices where they sit. A prismatic band's section
// is the B-rep's own (an exact planar section, its curves discretized to
// `deflectionMm`); slices are cut from one fine triangulation of the solid
// (`meshDeflectionMm`), as a slice already stands for its thickness.
//
// What two placed solids do as one slides along the axis is then answered
// band pair by band pair: whether their regions come near (a 2D polygon
// intersection, independent of the slide) and over which slide offsets
// their spans overlap (an interval from the band ends) -- no 3D boolean.

#include <clipper2/clipper.h>

#include <TopoDS_Shape.hxx>

#include <atomic>
#include <limits>
#include <vector>

namespace Measure::sections {

// Clipper's integer grid: one unit is 10 nm.
constexpr double kScale = 1.0e5;

struct Band {
  double z0 = 0.0, z1 = 0.0;     // along the axis, z0 < z1
  Clipper2Lib::Paths64 region;   // the section; holes wound against outers
  Clipper2Lib::Rect64 box{};     // of the region
  double rMin = 0.0, rMax = 0.0; // radial extent about the axis, mm
  double perimeterMm = 0.0;
  bool sliced = false; // a slice of a span whose section varies
  double zSection = 0.0; // where the section was cut
  double noiseMm = 0.0;  // how far its outline may be off: a mesh's deflection
  int solid = 0;       // which solid of its side it came from
};

struct BandOptions {
  // Slice thickness where the section varies along the axis, mm.
  double sliceMm = 0.25;
  // Keep only what lies within this radius of the axis, mm.
  double reachMm = std::numeric_limits<double>::infinity();
  // The chord's furthest from an edge it stands for, mm: an exact
  // section's edges, discretized.
  double deflectionMm = 2.0e-4;
  // The same for a slice, which is cut from a triangulation of the solid
  // (a twisted flank's boolean section is seconds a slice).
  double meshDeflectionMm = 5.0e-4;
};

// One solid, placed so the axis is the Z axis through the origin, with
// each face's box (read once: both the reach and the bands need them).
struct Solid {
  TopoDS_Shape shape;
  struct Face {
    int index = 0; // in the solid's face map
    double z0 = 0.0, z1 = 0.0, rMin = 0.0;
  };
  std::vector<Face> faces;
  double half = 0.0; // the furthest any face reaches from the axis in x or y
};
Solid Prepare(const TopoDS_Shape& solidInAxisFrame);

// The span along the axis of the solid's faces that come within `reachMm`
// of the axis; false when none does.
bool ReachSpan(const Solid& s, double reachMm, double& lo, double& hi);

// The solid cut into bands. Empty regions are dropped. Throws
// std::runtime_error when the kernel fails to section it; `cancel` is
// polled between sections.
std::vector<Band> SolidBands(const Solid& s, const BandOptions& opt,
                             int solidIndex,
                             const std::atomic<bool>* cancel = nullptr);

// The region turned about the axis by `deg`.
Clipper2Lib::Paths64 Rotated(const Clipper2Lib::Paths64& p, double deg);

// Box, radial extent and perimeter, recomputed from `region`.
void Refresh(Band& b);

// The least distance between two regions' boundaries, mm, looking only at
// edges with a point within `window` (grown by `reachMm`); +inf if none.
double BoundaryGap(const Clipper2Lib::Paths64& a, const Clipper2Lib::Paths64& b,
                   const Clipper2Lib::Rect64& window, double reachMm);

} // namespace Measure::sections
