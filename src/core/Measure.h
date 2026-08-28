#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"
#include "core/Units.h"

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
  // What the element IS, in the vocabulary DescribeFaceAt/DescribeEdgeAt and
  // query_geometry already use: "plane"|"cylinder"|"cone"|"sphere"|"torus"|
  // "other" for a face, "line"|"circle"|"arc"|"other" for an edge.
  std::string shape;
  // A radius wherever one is defined — a circular/arc EDGE, and a cylinder,
  // sphere or torus FACE. The face half was computed and dropped for a year:
  // clicking a bore in the Measure tool reported its surface area, where
  // every CAD system reports a diameter.
  double radius = 0.0;
  bool hasRadius = false;
  // The axis direction of a cylinder/cone/torus face or a circular edge. Its
  // SIGN is arbitrary (an axis has no outward), so compare with |dot|.
  glm::vec3 axis{0.0f};
  bool hasAxis = false;
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
  std::string kind; // "plane"|"cylinder"|"cone"|"sphere"|"torus"|"other"
  double area = 0.0;
  glm::vec3 centroid{0.0f}; // face center of mass, world space
  glm::vec3 normal{0.0f};   // outward unit surface normal at the centroid UV
  // Analytic extras for query_geometry: cylinders/spheres carry a radius,
  // cylinders/cones an axis DIRECTION (sign is arbitrary — compare |dot|).
  double radius = 0.0;
  bool hasRadius = false;
  glm::vec3 axis{0.0f};
  bool hasAxis = false;
};
struct EdgeInfo {
  std::string kind; // "line" | "circle" | "arc" | "other"
  double length = 0.0;
  double radius = 0.0; // circular edges ("circle" / "arc") only
  bool hasRadius = false;
  glm::vec3 midpoint{0.0f}; // curve midpoint (parametric mid), world space
  glm::vec3 axis{0.0f};     // circular edges: the circle's axis direction
  bool hasAxis = false;
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

// The direction an element defines — the only thing an angle can be taken
// between. A planar face's outward normal, a line edge's tangent, a
// cylinder/cone/torus axis, a circular edge's axis. nullopt for a sphere, a
// spline, a general surface: those define no direction, and inventing a
// plausible one is how a measurement comes back confidently wrong.
struct Direction {
  glm::dvec3 dir{0.0};
  // False when the SIGN is arbitrary. A line edge has no "way" — OCC's
  // parameterisation decides which end is first — so an angle taken against
  // it is only meaningful up to 180°. A face normal points out of the solid
  // and is oriented; so is nothing else here.
  bool oriented = false;
  // How the direction was taken, for a readout that states its basis rather
  // than leaving the reader to assume one. Both forms, because the sentence
  // needs one or the other: "between the edges" when the pair matches,
  // "between the edge and the cylinder axis" when it does not.
  std::string from;   // singular: "the face normal", "the cylinder axis"
  std::string plural; // "face normals", "edges", "cylinder axes"
};
std::optional<Direction> DirectionOf(const std::vector<Body>& bodies,
                                     const ElementRef& ref);

// The angle between two elements' directions.
//
// CONVENTION, stated because both choices are defensible and a silent one is
// a trap: two ORIENTED directions (two planar faces) are measured between
// the outward normals, so two adjacent faces of a box read 90° and two
// opposite faces read 180° — what a draftsman means by "the angle between
// these faces". When either direction's sign is arbitrary (any line edge),
// the result is folded into [0, 90] and `basis` says so.
struct AngleResult {
  double degrees = 0.0;       // the angle between the directions, [0, 180]
  double acute = 0.0;         // min(degrees, 180 - degrees), always [0, 90]
  bool parallel = false;      // within tolerance — the ANGLE is degenerate
  bool perpendicular = false; // within tolerance
  std::string basis; // "between the face normals", "between the edges", ...
};
std::optional<AngleResult> Angle(const std::vector<Body>& bodies,
                                 const ElementRef& a, const ElementRef& b);
// Assembly variant: each element's direction is rotated by its occurrence's
// world transform before the angle. Translation cannot change an angle, so
// only the rotation part is used.
std::optional<AngleResult> Angle(const std::vector<Body>& bodiesA,
                                 const ElementRef& a, const glm::mat4& xfA,
                                 const std::vector<Body>& bodiesB,
                                 const ElementRef& b, const glm::mat4& xfB);

// WHICH distance between two elements. Minimum is the one every CAD system
// defaults to and the only one this used to offer; the other two answer
// questions minimum cannot.
//
//   Minimum  — the closest approach. "Do these clash / what is the gap."
//   Maximum  — the furthest approach. "Will this still fit at the worst
//              point", which is the clearance question, and the one a
//              minimum-only tool silently answers wrong.
//   Centres  — anchor to anchor (face centroid, edge midpoint). Hole
//              spacing, and the only one of the three that is stable as the
//              features around it change shape.
enum class DistanceKind { Minimum, Maximum, Centres };

// Serialization / RPC strings, and the refusal vocabulary. Unknown strings
// are REFUSED naming what is accepted, never defaulted to Minimum: a
// substituted minimum is plausible and wrong exactly where it matters.
const char* ToString(DistanceKind k);
[[nodiscard]] bool DistanceKindFromString(const std::string& s,
                                          DistanceKind& out);
std::vector<const char*> DistanceKindNames();

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

// The same, for a chosen DistanceKind. Minimum is DistanceDetailed. Maximum
// walks the tessellated boundary of each element for the furthest pair —
// OCC's extrema solver only minimises, and the furthest points of two curved
// faces are not in general vertices, so this is sampled and honest about it
// rather than exact and wrong. Centres is the two anchors.
std::optional<DistanceResult>
DistanceOfKind(const std::vector<Body>& bodiesA, const ElementRef& a,
               const glm::mat4& xfA, const std::vector<Body>& bodiesB,
               const ElementRef& b, const glm::mat4& xfB, DistanceKind kind);

// MEASURABLES: a named element, or a whole placed shape.
//
// Everything above measures a face or an edge. But "how far apart are these
// two parts" is the most natural question in an assembly, ContactBetween has
// answered it since I4 for the interference report, and the measure tool
// could not accept the pick — the armed kinds were Face|Edge and nothing
// else.
//
// A Target names either. An EMPTY `ref.name` means the whole placed set
// rather than one element of it; `ref.featureId` then filters to a single
// body by its createdBy (0 = every body in the set, which is what an
// occurrence means). Elements and wholes measure through one function, so a
// mixed pair — this face to that part — is not a case anyone has to
// remember to write.
struct Target {
  const std::vector<Body>* bodies = nullptr;
  glm::mat4 xf{1.0f};
  ElementRef ref; // name empty => the whole set
  bool Whole() const { return ref.name.empty(); }
};

// What a whole placed shape measures as, for a one-element readout: the
// volume and surface area a part actually has, and the box it needs.
struct SetMeasure {
  double volume = 0.0;
  double surfaceArea = 0.0;
  glm::vec3 size{0.0f}; // world-axis-aligned bounding box extents
  int solids = 0;
};
std::optional<SetMeasure> DescribeTarget(const Target& t);

// Distance between any two measurables, elements or wholes.
std::optional<DistanceResult> DistanceBetween(const Target& a, const Target& b,
                                              DistanceKind kind);

// The point on a target closest to (or, for Maximum, furthest from) `p` —
// what a POINT measurable measures against. A corner is a point, so its side
// of a measurement is exact and only the other side needs solving.
std::optional<glm::vec3> NearestPointOn(const Target& t, const glm::vec3& p,
                                        DistanceKind kind);

// Whether two WHOLE measurables overlap, and by how much — so a pair of
// parts that interpenetrate reports as a clash rather than as distance 0,
// which is what the extrema solver says about any two overlapping solids and
// is the least useful true thing available. nullopt when either side is an
// element (a face has no volume to overlap with).
std::optional<SetContact> OverlapBetween(const Target& a, const Target& b);

// Total length of a set of edges, or total area of a set of faces — what a
// third and fourth pick should add up to instead of being refused. Mixing
// kinds is REFUSED (nullopt): the sum of an area and a length is not a
// quantity, and returning one of them would be picking a winner in silence.
struct SetTotal {
  std::string kind; // "face" | "edge"
  double total = 0.0;
  int count = 0;
};
std::optional<SetTotal> TotalOf(const std::vector<Body>& bodies,
                                const std::vector<ElementRef>& refs);

// How a PERSON names this element: "top face", "600 mm edge", "Ø25 bore".
// The measure tool's rows read `Edge 1/face3;U0` — the internal stable name,
// which is the thing that makes a measurement survive a regen and exactly
// the thing nobody should have to read.
//
// Derived from the classification and the geometry, never stored: a name
// that went stale would be worse than the toponame it replaced. The stable
// name stays the identity and the tooltip; this is only for display.
// Empty when the ref does not resolve.
std::string HumanName(const std::vector<Body>& bodies, const ElementRef& ref,
                      const units::System& sys);

// "top" / "bottom" / "side" for an axis-aligned unit direction (a planar
// face's outward normal), in the words a person uses about a part on a
// bench; nullptr when the direction is not near an axis. Split out of
// HumanName so the world-up gate (tests/test_world_up.cpp) can check the
// words against conv::kWorldUp directly.
const char* FacingWord(const glm::dvec3& unitDir);

// The corner where two named edges meet, as a measurable point (phase 4c).
// nullopt when they share no vertex, or — the case a cylinder's wall hits on
// the first try — more than one, since two edges that meet twice name two
// points and therefore neither.
//
// A corner has no stable name of its own: the ElementMap names faces and
// edges, not vertices. Two named edges do, so their shared vertex does too.
std::optional<glm::vec3> CornerOf(const std::vector<Body>& bodies,
                                  const ElementRef& edgeA,
                                  const ElementRef& edgeB);

// A representative point ON an element: a face's centre of mass, an edge's
// parametric midpoint. Where a dimension should attach when it has a choice.
std::optional<glm::vec3> AnchorOf(const std::vector<Body>& bodies,
                                  const ElementRef& ref);

// The element's CENTRE, which for anything round is not a point on it.
//
// This is the hole-spacing measurement, and getting it from AnchorOf gives a
// point on the RIM: two holes of the same radius then cancel their offsets
// and answer correctly by luck, while two holes of DIFFERENT radii report
// 102.6 mm where the spacing is 104.4 — wrong, and plausible. A circle's
// centre is its centre; a cylinder's, cone's or torus's is the point on its
// axis nearest the face; a sphere's is the sphere's. Everything else has no
// centre distinct from its anchor and falls back to it.
std::optional<glm::vec3> CentreOf(const std::vector<Body>& bodies,
                                  const ElementRef& ref);

// Where to attach a DIMENSION between two elements, as opposed to where the
// minimum distance happens to be measured.
//
// For a general pair those are the same: the extrema pair is the honest
// answer and the only one. For a PARALLEL pair they are not — every point
// pair is the same distance apart, so the extrema solver returns whichever
// it found first, which is typically a far corner. Dimensioning that draws a
// 45 mm stub at one end of a 600 mm board and reads as a rendering artifact.
// Here the dimension is anchored on A's own centre and taken to the nearest
// point to it on B, which is what a draftsman would have drawn.
//
// `parallel` comes from Angle(); pass false when there is no angle to take.
std::optional<DistanceResult>
DimensionAnchors(const std::vector<Body>& bodiesA, const ElementRef& a,
                 const glm::mat4& xfA, const std::vector<Body>& bodiesB,
                 const ElementRef& b, const glm::mat4& xfB, bool parallel);

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
// The filled cut faces where `normal`/`origin`'s plane crosses each body,
// one entry per body (invalid where that body is not cut).
//
// `trimPlanes` are the OTHER active sections, in the same space as `bodies`,
// each {xyz = normal, w = distance} keeping dot(n,p) <= w. A cap must be cut
// back to their kept side or it juts out past the neighbouring cut — the
// "floating lid" you get when two sections meet.
std::vector<CADShape>
SectionCaps(const std::vector<Body>& bodies, const glm::vec3& origin,
            const glm::vec3& normal,
            const std::vector<glm::vec4>& trimPlanes = {});

} // namespace Measure
