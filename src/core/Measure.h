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

// Per-element classification (list_elements enrichment): resolve a face /
// edge by its ElementMap index — the same index conventions ResolveShape and
// the element maps key on (faces = TopExp_Explorer order, edges = MapShapes
// order, both 0-based) — and classify the underlying surface / curve.
struct FaceInfo {
  std::string kind; // "plane" | "cylinder" | "cone" | "sphere" | "other"
  double area = 0.0;
};
struct EdgeInfo {
  std::string kind; // "line" | "circle" | "arc" | "other"
  double length = 0.0;
  double radius = 0.0; // circular edges ("circle" / "arc") only
  bool hasRadius = false;
  glm::vec3 midpoint{0.0f}; // curve midpoint (parametric mid), world space
};
std::optional<FaceInfo> DescribeFaceAt(const Body& body, int faceIndex);
std::optional<EdgeInfo> DescribeEdgeAt(const Body& body, int edgeIndex);

// Total pairwise intersection volume between two rigidly placed body sets
// (I4 clash, assembly form): every body of A (placed by xfA) is intersected
// with every body of B (placed by xfB) and the volumes summed. The part-tab
// check is the identity-transform special case. nullopt on an OCC failure.
std::optional<double> InterferenceVolume(const std::vector<Body>& bodiesA,
                                         const glm::mat4& xfA,
                                         const std::vector<Body>& bodiesB,
                                         const glm::mat4& xfB);

// Watertightness report for mesh export: shell count and free (naked) edge
// count of a shape about to be tessellated. A shape is watertight when it has
// at least one shell and no non-degenerate edge is used by fewer than two
// faces. Purely diagnostic — export proceeds either way.
struct ShellReport {
  bool watertight = false;
  int shells = 0;
  int freeEdges = 0;
};
ShellReport AnalyzeWatertight(const CADShape& shape);

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

// Assembly variant: each element resolves against its own part's bodies and
// is placed by that occurrence's rigid world transform before the extrema —
// distance and closest points come back in world space. The part-tab path is
// the identity-transform special case of this.
std::optional<DistanceResult> DistanceDetailed(
    const std::vector<Body>& bodiesA, const ElementRef& a, const glm::mat4& xfA,
    const std::vector<Body>& bodiesB, const ElementRef& b,
    const glm::mat4& xfB);

// Axis-aligned bounding box of one resolved element (face or edge), in the
// bodies' own (part-local) space — feeds zoom-to-selection. nullopt when the
// ref no longer resolves.
struct Bounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};
};
std::optional<Bounds> ElementBounds(const std::vector<Body>& bodies,
                                    const ElementRef& ref);

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
