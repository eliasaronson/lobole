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
  std::string kind;    // "face" | "edge"
  double area = 0.0;   // faces
  double length = 0.0; // edges
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
  glm::vec3 centroid{0.0f}; // face center of mass, world space
  glm::vec3 normal{0.0f};   // outward unit surface normal at the centroid UV
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

// Contact probe between two rigidly placed body sets (V1): total pairwise
// intersection volume plus the exact minimum separation. distance is 0
// whenever the sets overlap; a touching pair reports volume 0 and distance
// ~0. Boxes prune both the booleans and the extrema work. nullopt on an OCC
// failure or when either set has no valid shapes.
struct SetContact {
  double volume = 0.0;   // total pairwise intersection volume
  double distance = 0.0; // min body-to-body separation, 0 when overlapping
};
std::optional<SetContact> ContactBetween(const std::vector<Body>& bodiesA,
                                         const glm::mat4& xfA,
                                         const std::vector<Body>& bodiesB,
                                         const glm::mat4& xfB);

// Axis-aligned world bounds of a placed body set — the cheap prefilter the
// assembly contact_report sweep runs before paying for ContactBetween.
// False when the set has no valid shapes.
bool WorldBounds(const std::vector<Body>& bodies, const glm::mat4& xf,
                 glm::vec3& min, glm::vec3& max);

// V4 compare_geometry: boolean diff between two placed body sets. missing =
// vol(A − B) (reference not covered), extra = vol(B − A) (beyond the
// reference); the derived similarity metrics (IoU, precision/recall, ...) are
// arithmetic over these and live at the RPC layer. missingOut/extraOut, when
// non-null, receive the diff SOLIDS so callers can export/render the delta.
struct SetCompare {
  double volumeA = 0.0, volumeB = 0.0;
  double common = 0.0, missing = 0.0, extra = 0.0;
  double surfaceA = 0.0, surfaceB = 0.0;
  glm::dvec3 comA{0.0}, comB{0.0};
  glm::vec3 minA{0.0f}, maxA{0.0f}, minB{0.0f}, maxB{0.0f};
  int facesA = 0, facesB = 0, edgesA = 0, edgesB = 0;
};
std::optional<SetCompare>
CompareBodySets(const std::vector<Body>& bodiesA, const glm::mat4& xfA,
                const std::vector<Body>& bodiesB, const glm::mat4& xfB,
                CADShape* missingOut = nullptr, CADShape* extraOut = nullptr);

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

// B-Rep validity (OCC BRepCheck_Analyzer): whether the shape is a well-formed
// solid and, if not, how many faces/edges fail and the distinct failure
// reasons (e.g. "self-intersecting wire", "invalid curve on surface"). Useful
// on imported STEP and after booleans. Purely diagnostic. `valid` is the
// whole-shape verdict; the counts/reasons enumerate the offending sub-shapes.
struct GeometryReport {
  bool valid = false;
  int badFaces = 0;
  int badEdges = 0;
  int badWires = 0;
  std::vector<std::string> reasons; // distinct human-readable failure statuses
};
GeometryReport CheckGeometry(const CADShape& shape);

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
std::optional<DistanceResult>
DistanceDetailed(const std::vector<Body>& bodiesA, const ElementRef& a,
                 const glm::mat4& xfA, const std::vector<Body>& bodiesB,
                 const ElementRef& b, const glm::mat4& xfB);

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
