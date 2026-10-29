// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What a mesh body measures as (docs/inspect/README.md#mesh-bodies): its
// statistics, a point snapped onto it, its distance to anything else, and
// a plane's cut through it. Its triangles ARE its geometry, so every answer
// is exact to the mesh; where the other side is a B-Rep, that side is
// tessellated, and its deflection is the only tolerance the answer has.

#include "core/Feature.h"
#include "core/Measure.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

namespace meshmeasure {

// Does the target hold a mesh body? (Its whole set, or the body its one
// element is on.)
bool HasMesh(const Measure::Target& t);

// Is `ref` a mesh body's one element ("<id>/b<k>/mesh")? Then it names the
// whole body.
bool IsMeshElement(const std::vector<Body>& bodies, const ElementRef& ref);

struct Stats {
  size_t triangles = 0;
  size_t vertices = 0;
  size_t components = 0;
  bool closed = false;
  std::vector<double> loops;    // boundary loop lengths, mm, longest first
  double area = 0.0;            // mm²
  std::optional<double> volume; // mm³, only when closed
  glm::dvec3 lo{0.0}, hi{0.0};
  double medianEdge = 0.0; // mm
};
Stats Describe(const mesh::TriMesh& m);
// "open: 1 boundary loop, the longest 212.4 mm -- close it in MeshLab,
// Blender or the scanning app": why a volume is refused.
std::string OpenWhy(const Stats& s);

// The point on body `featureId`'s bodies (0: any) nearest `p`, and how far
// it moved. A mesh body's by its tree; a B-Rep's exactly.
struct Snap {
  glm::dvec3 point{0.0};
  double moved = 0.0;
};
std::optional<Snap> SnapPoint(const std::vector<Body>& bodies,
                              uint64_t featureId, const glm::dvec3& p,
                              std::string& why);

// The nearest point of a target to `p` (a mesh body by its tree, anything
// else by Measure::NearestPointOn).
std::optional<glm::dvec3> NearestPoint(const Measure::Target& t,
                                       const glm::dvec3& p);

// The minimum distance between two targets, at least one holding a mesh
// body. A B-Rep side is tessellated at a thousandth of its own size, which
// is `toleranceMm`; between two meshes it is 0, exact. Defined in
// Measure.cpp, where a target's B-Rep is resolved.
struct Gap {
  double distance = 0.0;
  glm::dvec3 pointA{0.0}, pointB{0.0};
  double toleranceMm = 0.0;
};
std::optional<Gap> Distance(const Measure::Target& a, const Measure::Target& b,
                            std::string& why);

// Whether two body sets, at least one holding a mesh body, overlap and by
// how much: their triangles intersected by manifold (a B-Rep side
// tessellated as for Distance), so the volume is exact to the mesh. Both
// must close; an open mesh is refused, naming its holes. Defined in
// Measure.cpp.
struct Overlap {
  double volume = 0.0;   // mm³, 0 when they do not overlap
  double distance = 0.0; // 0 when they meet
  double toleranceMm = 0.0;
};
std::optional<Overlap> Interference(const std::vector<Body>& a,
                                    const std::vector<Body>& b,
                                    std::string& why);

// A plane's cut through every mesh body of `bodies`: the summed area of the
// closed loops, and the regions (an outer loop and the holes inside it),
// biggest first, with their loops as line segments. `open` counts the
// bodies whose cut did not close (an open scan): their area is not summed,
// and `openBodies` names them.
struct Cut {
  double area = 0.0;
  std::vector<Measure::SectionRegion> regions;
  std::vector<std::string> openBodies;
};
Cut Section(const std::vector<Body>& bodies, const glm::dvec3& origin,
            const glm::dvec3& normal, bool withLoops);

} // namespace meshmeasure
