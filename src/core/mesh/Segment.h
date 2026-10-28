// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A faceted mesh cut into the surfaces it was tessellated from
// (docs/interchange/README.md#recognising-a-faceted-mesh): every triangle
// ends in one region, a plane, cylinder, cone or sphere fitted to it.
//
//  1. Flat patches: triangles coplanar to within the file's precision
//     (k x quantum, as a vertex's distance from the patch's plane, never a
//     normal angle) flood together. Every patch is a plane, at least.
//  2. Curved regions: patches meeting across smooth edges are grown under a
//     hypothesis -- the Gauss map gives the axis, and every vertex must lie
//     on the fitted surface to the same tolerance. A patch that breaks the
//     fit is not taken: the plane a fillet is tangent to has its normal on
//     the fillet's great circle but its vertices far off the cylinder.
//  3. Round, or a polygon: a hex nut trap's corners lie on a circle too, so
//     the fit cannot tell it from a hole. The tessellation can: a curved
//     region is taken only when no facet of it turns by more than
//     360 / min_sides degrees. One that fits but turns more is a POLYGON,
//     listed, and its flats stay planes -- a nut sits on them.
//  4. What fits nothing (a torus corner, a freeform blend) stays as its
//     flat patches, counted as facets left.
//
// Growth is by the fitted surface's residual, not by a normal's deviation
// from a seed: a cylinder's normals turn by design.

#include "core/mesh/Fit.h"
#include "core/mesh/TriMesh.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mesh {

struct SegmentOptions {
  // A curved region's facets turn by at most 360/minSides degrees each.
  int minSides = 12;
  // The on-surface tolerance, in quanta of the file's precision.
  double quanta = 4.0;
  // Polled between seeds; true stops with an empty result.
  std::function<bool()> cancelled;
};

inline constexpr int kMinSidesLowest = 5;
inline constexpr int kMinSidesHighest = 1024;

struct Region {
  FitResult fit;
  std::vector<uint32_t> triangles; // ascending
  uint32_t firstTriangle = 0;      // the lowest, which orders the regions
  double area = 0.0;               // mm²
  // Curved: the most any facet turns (rad), the full turn's side count it
  // implies, and the most the facets stand off the surface (mm).
  double facetTurn = 0.0;
  int sides = 0;
  double sagitta = 0.0;
};

// A ring of flats whose corners fit a circle but that turns too far a facet
// to be one: kept flat and listed.
struct Polygon {
  int sides = 0;           // a full turn's worth
  double circumradius = 0; // mm
  glm::dvec3 centre{0.0};  // on the axis, at the middle of the flats
  glm::dvec3 axis{0.0, 0.0, 1.0};
};

struct Segmentation {
  bool cancelled = false;
  std::vector<uint32_t> regionOf; // per triangle
  std::vector<Region> regions;    // by firstTriangle
  std::vector<Polygon> polygonal;
  // Flats with a smooth neighbour that fitted no curved surface: the facets
  // of something recognition left as it was.
  size_t facetsLeft = 0;
  double tolerance = 0.0; // the on-surface tolerance used (mm)

  [[nodiscard]] size_t Count(Kind k) const;
};

Segmentation SegmentAll(const TriMesh& mesh, const SegmentOptions& options);

// Puts curved region `region` back as the flat patches it was grown from,
// each a plane, counted among the facets left: what the rebuild does with a
// surface it cannot make a face of. Regions are renumbered.
void Demote(Segmentation& s, const TriMesh& mesh, uint32_t region);

// What an import says it recognised (the reply's `recognition`), in the
// mesh's own coordinates until the caller places it.
struct RecognizedRound {
  Kind kind = Kind::Cylinder;
  double radius = 0.0;    // mm (a cone: at its widest)
  double halfAngle = 0.0; // a cone's, rad
  glm::dvec3 axis{0.0, 0.0, 1.0};
  glm::dvec3 centre{0.0}; // a cylinder's or cone's mid-length on its axis,
                          // a sphere's centre
  int sides = 0;          // a full turn's worth
  double sagitta = 0.0;   // mm the facets stood off it
};
struct Recognition {
  int minSides = 12;
  size_t triangles = 0;
  size_t planes = 0;
  std::vector<RecognizedRound> rounds; // cylinders, cones, spheres
  std::vector<Polygon> polygonal;
  size_t facetsLeft = 0;
  double toleranceMm = 0.0;
  // Set when the rebuild failed a check and the mesh was sewn as facets:
  // which check, and why.
  std::string fellBack;
  // Curved regions the rebuild could not make a face of, kept as flats.
  size_t demoted = 0;

  [[nodiscard]] size_t Count(Kind k) const;
  // Moved by `m` (a rigid placement) after scaling by `scale` about the
  // file's origin.
  void Transform(const glm::dmat4& m, double scale);
  // "12 holes, 9 planes; a 6-sided pocket left flat"
  [[nodiscard]] std::string Summary() const;
};

Recognition Summarize(const TriMesh& mesh, const Segmentation& s, int minSides);

} // namespace mesh
