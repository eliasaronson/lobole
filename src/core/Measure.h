#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"

#include <optional>
#include <string>
#include <vector>

// Measurement over stable element names (inspection I1, faces/edges v1):
// resolve ElementRefs against the bodies' element maps, then ask OCC.
namespace Measure {

struct Element {
  std::string kind; // "face" | "edge"
  double area = 0.0;    // faces
  double length = 0.0;  // edges
  // A circular/arc edge additionally reports its radius (0/false otherwise).
  double radius = 0.0;
  bool hasRadius = false;
};

// Resolve + describe one element (face area / edge length). nullopt when the
// ref no longer resolves.
std::optional<Element> Describe(const std::vector<Body>& bodies,
                                const ElementRef& ref);

// Minimum distance between two elements (either kind). nullopt when a ref
// doesn't resolve or the extrema computation fails.
std::optional<double> Distance(const std::vector<Body>& bodies,
                               const ElementRef& a, const ElementRef& b);

// Distance with the closest point pair and axis deltas — feeds the measure
// visualization (the nearest-distance line drawn between the elements) and
// the dx/dy/dz readout.
struct DistanceResult {
  double distance = 0.0;
  glm::vec3 pointA{0.0f}; // closest point on element a
  glm::vec3 pointB{0.0f}; // closest point on element b
};
std::optional<DistanceResult> DistanceDetailed(const std::vector<Body>& bodies,
                                               const ElementRef& a,
                                               const ElementRef& b);

// Total cross-section area where the plane (through `origin`, unit `normal`)
// cuts through all bodies (inspection I3): the sum over bodies of the area of
// their intersection with the plane. 0 when the plane misses everything;
// nullopt on an OCC failure.
std::optional<double> CrossSectionArea(const std::vector<Body>& bodies,
                                       const glm::vec3& origin,
                                       const glm::vec3& normal);

// The section-cut cap faces (I3 v2, C4): one CADShape per body — the body's
// intersection with the section plane, lying on the plane — for rendering a
// filled cut face so a sectioned part reads solid. Same plane as
// CrossSectionArea. The returned vector aligns with `bodies` (a body that
// doesn't meet the plane yields an empty/invalid CADShape); empty on failure.
std::vector<CADShape> SectionCaps(const std::vector<Body>& bodies,
                                  const glm::vec3& origin,
                                  const glm::vec3& normal);

} // namespace Measure
