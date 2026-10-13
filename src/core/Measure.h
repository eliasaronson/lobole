// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"
#include "core/MeshCollide.h"
#include "core/ThreadStandards.h"
#include "core/Units.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Measurement over stable element names (inspection I1, faces/edges v1):
// resolve ElementRefs against the bodies' element maps, then ask OCC.
namespace Measure {

// One threaded span of a body, placed: what the clearance check relieves an
// engagement against, and what tells a fastener's joint where it ENDS when
// no nut does (a screw in a tapped boss is held by the thread, and the axis
// beyond the thread is somebody else's material).
struct ThreadSpan {
  threads::Standard spec;
  glm::dvec3 start, axis;
  double length = 0.0, minimum = 0.0;
  bool external = false;
};

// Every thread `body` carries, in world coordinates.
[[nodiscard]] std::vector<ThreadSpan> ThreadSpansOf(const Body& body,
                                                    const glm::dmat4& world);

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
  glm::dvec3 axis{0.0};
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
// POSITIONS HERE ARE DOUBLE, deliberately: the kernel computes them in
// double and these are answers to "where is this face", not render data, so
// the way out should not add loss the caller cannot see. A box at 164.8 now
// reports 164.8 and not 164.800003.
//
// The model is double end to end as well -- a sketch plane, a primitive's
// position, an occurrence's placement -- so a panel drawn on a plane at
// -172.8 sits at -172.8 (docs/conventions.md#the-model-is-double). A CALLER
// filtering on a COMPUTED position still wants a tolerance suited to the
// magnitude, since arithmetic rounds and an empty pick list looks exactly
// like nothing to do (docs/inspect/README.md#positions-are-double).
struct FaceInfo {
  std::string kind; // "plane"|"cylinder"|"cone"|"sphere"|"torus"|"other"
  double area = 0.0;
  glm::dvec3 centroid{0.0}; // face center of mass, world space
  glm::dvec3 normal{0.0};   // outward unit surface normal at the centroid UV
  // Analytic extras for query_geometry: cylinders/spheres carry a radius,
  // cylinders/cones an axis DIRECTION (sign is arbitrary — compare |dot|).
  double radius = 0.0;
  bool hasRadius = false;
  glm::dvec3 axis{0.0};
  bool hasAxis = false;
};
struct EdgeInfo {
  std::string kind; // "line" | "circle" | "arc" | "other"
  double length = 0.0;
  double radius = 0.0; // circular edges ("circle" / "arc") only
  bool hasRadius = false;
  glm::dvec3 midpoint{0.0}; // curve midpoint (parametric mid), world space
  glm::dvec3 axis{0.0};     // circular edges: the circle's axis direction
  bool hasAxis = false;
};
std::optional<FaceInfo> DescribeFaceAt(const Body& body, int faceIndex);
std::optional<EdgeInfo> DescribeEdgeAt(const Body& body, int edgeIndex);
// The distance (mm) from `point` to the face / edge at that index ITSELF —
// the nearest point of its geometry, not its centroid or midpoint — or
// nullopt when the index does not resolve or the distance is more than
// `cutoff` (a box test skips the extrema then). query_geometry's on_point:
// a full circle is found by any point on it, wherever its seam fell.
std::optional<double> DistanceToElementAt(const Body& body, bool face,
                                          int index, const glm::dvec3& point,
                                          double cutoff);

// Total pairwise intersection volume between two rigidly placed body sets
// (I4 clash, assembly form): every body of A (placed by xfA) is intersected
// with every body of B (placed by xfB) and the volumes summed. The part-tab
// check is the identity-transform special case. nullopt on an OCC failure.
std::optional<double> InterferenceVolume(const std::vector<Body>& bodiesA,
                                         const glm::dmat4& xfA,
                                         const std::vector<Body>& bodiesB,
                                         const glm::dmat4& xfB);

// Contact probe between two rigidly placed body sets (V1): total pairwise
// intersection volume plus the exact minimum separation. distance is 0
// whenever the sets overlap; a touching pair reports volume 0 and distance
// ~0. Boxes prune both the booleans and the extrema work. nullopt on an OCC
// failure or when either set has no valid shapes.
struct SetContact {
  size_t engagedThreadPairs = 0;
  double threadEngagementVolume = 0.0; // deliberately relieved from volume
  // A retaining ring in its groove (RingGrip): the free ring's overlap with
  // the floor is its grip, relieved the same way.
  size_t ringGripPairs = 0;
  double ringGripVolume = 0.0;
  // A PRESS FIT (FitBand, docs/tolerances/README.md#press-fits): two coaxial
  // banded round faces, one inside the other, whose fit allows interference.
  // Their overlap is relieved up to the fit's maximum interference, over the
  // length the two faces engage, and listed here rather than as a clash.
  struct PressFit {
    std::string fit;              // "Ø47 K7 / 6005 outside (ISO 492 normal)"
    double diameter = 0.0;        // the basic size the two bands share, mm
    double interference = 0.0;    // as made, on the diameter, mm
    double maxInterference = 0.0; // the most the two bands allow, mm
    double length = 0.0;          // engaged, mm
    double volume = 0.0;          // relieved, mm3
  };
  std::vector<PressFit> pressFits;
  double pressFitVolume = 0.0;
  // An overlap between two coaxial round faces, at least one of them banded,
  // that is NOT relieved — and why: past the band (`excess` the interference
  // beyond what the fit allows, mm on the diameter), a fit that allows no
  // interference, a side with no band, a band on the wrong side, or two
  // faces not coaxial. Its overlap stays in `volume`, a clash.
  struct FitRefused {
    std::string fit; // as PressFit's, or the one face that has a band
    std::string reason;
    double interference = 0.0, maxInterference = 0.0, excess = 0.0;
  };
  std::vector<FitRefused> fitsRefused;

  double volume = 0.0;   // total pairwise intersection volume
  double distance = 0.0; // min body-to-body separation, 0 when overlapping
  // WHERE the overlap is, when there is one: the volume-weighted centre of
  // the intersection solid and its bounds, in the same world the bodies were
  // placed in. Only the volume used to come back, and "throttle lever x
  // bellcrank, 66.26 mm3" between two plates carrying three bosses and a pin
  // each is a guessing game — it cost four build-and-look cycles on the
  // carburettor round alone. The intersection is already built to weigh it.
  bool overlaps = false;
  // ...and whether `distance` was MEASURED. A caller that only asks "do
  // these overlap" does not pay for the separation, and 0.0 is what an
  // OVERLAP reports, so an unmeasured distance must not be readable as one.
  bool distanceKnown = true;
  glm::dvec3 centroid{0.0};
  glm::dvec3 overlapMin{0.0}, overlapMax{0.0};
};
// `wantDistance` false answers only "do these overlap, by how much, and
// where" and skips the separation pass entirely. That pass is not the cheap
// half: measured on a 241-occurrence engine it is 299 ms a pair against the
// boolean's 165 ms — 64% of a whole sweep — because
// `BRepExtrema_DistShapeShape` on two complex solids is dearer than
// intersecting them. A motion study reads only the volume, so for 20,403 checks
// it was paying about two thirds of its time for a number it threw away.
std::optional<SetContact> ContactBetween(const std::vector<Body>& bodiesA,
                                         const glm::dmat4& xfA,
                                         const std::vector<Body>& bodiesB,
                                         const glm::dmat4& xfB,
                                         bool wantDistance = true);

// The distance pass alone, without the boolean: for two sets a mesh screen
// (contact_report {mode: "mesh"}) found not crossing. It answers only when
// the answer PROVES there is no overlap — the exact distance is positive
// and no solid of either set lies wholly inside the other (one vertex of
// each solid, classified exactly) — and is nullopt otherwise, for the
// caller to ask ContactBetween. When it answers, `volume` is 0 and
// `distance` the separation, by the same extrema ContactBetween's second
// pass takes, so the two agree to the bit.
std::optional<SetContact> ClearanceBetween(const std::vector<Body>& bodiesA,
                                           const glm::dmat4& xfA,
                                           const std::vector<Body>& bodiesB,
                                           const glm::dmat4& xfB);

// Axis-aligned world bounds of a placed body set — the cheap prefilter the
// assembly contact_report sweep runs before paying for ContactBetween.
// False when the set has no valid shapes.
bool WorldBounds(const std::vector<Body>& bodies, const glm::dmat4& xf,
                 glm::dvec3& min, glm::dvec3& max);

// True when the two placed body sets are further apart than `gap`, judged by
// their ORIENTED boxes — the second prefilter, for the case an axis-aligned
// one cannot help with.
//
// A long slanted member's axis-aligned box is mostly air: a space frame's
// diagonals each box half the car, so almost every pair survives the cheap
// test and pays for an exact boolean. A 32-member chassis took 84 s that way,
// over the default RPC timeout, and the answer was 63 touching pairs out of
// 496. An oriented box follows the member.
//
// Conservative in the only direction that matters: it returns true only when
// the boxes are provably apart, so a pair it rejects cannot be touching.
[[nodiscard]] bool SeparatedByOBB(const std::vector<Body>& a,
                                  const glm::dmat4& xfA,
                                  const std::vector<Body>& b,
                                  const glm::dmat4& xfB, double gap);

// V4 compare_geometry: boolean diff between two placed body sets. missing =
// vol(A − B) (reference not covered), extra = vol(B − A) (beyond the
// reference); the derived similarity metrics (IoU, precision/recall, ...) are
// arithmetic over these and live at the RPC layer. missingOut/extraOut, when
// non-null, receive the diff SOLIDS so callers can export/render the delta.
struct SetCompare {
  // False when the caller asked for no overlap: `common`, `missing` and
  // `extra` were never computed and are not zero-because-they-are-zero.
  // Three OCC booleans is what they cost, and on a part carrying toroidal
  // fillets that is seconds — 13.2 s for one brace of the smart speaker,
  // against 30 ms for a tube with no fillets
  // (docs/inspect/README.md#comparing-against-a-reference).
  bool hasOverlap = true;
  double volumeA = 0.0, volumeB = 0.0;
  double common = 0.0, missing = 0.0, extra = 0.0;
  double surfaceA = 0.0, surfaceB = 0.0;
  glm::dvec3 comA{0.0}, comB{0.0};
  glm::dvec3 minA{0.0}, maxA{0.0}, minB{0.0}, maxB{0.0};
  int facesA = 0, facesB = 0, edgesA = 0, edgesB = 0;
};
// `overlap` false skips the three booleans — everything else (volumes,
// surfaces, centres, bounds, the face/edge census) is measured from the two
// shapes directly and costs milliseconds.
std::optional<SetCompare>
CompareBodySets(const std::vector<Body>& bodiesA, const glm::dmat4& xfA,
                const std::vector<Body>& bodiesB, const glm::dmat4& xfB,
                CADShape* missingOut = nullptr, CADShape* extraOut = nullptr,
                bool overlap = true);

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
                                 const ElementRef& a, const glm::dmat4& xfA,
                                 const std::vector<Body>& bodiesB,
                                 const ElementRef& b, const glm::dmat4& xfB);

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
  glm::dvec3 pointA{0.0}; // closest point on element a
  glm::dvec3 pointB{0.0}; // closest point on element b
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
                 const glm::dmat4& xfA, const std::vector<Body>& bodiesB,
                 const ElementRef& b, const glm::dmat4& xfB);

// The same, for a chosen DistanceKind. Minimum is DistanceDetailed. Maximum
// walks the tessellated boundary of each element for the furthest pair —
// OCC's extrema solver only minimises, and the furthest points of two curved
// faces are not in general vertices, so this is sampled and honest about it
// rather than exact and wrong. Centres is the two anchors.
std::optional<DistanceResult>
DistanceOfKind(const std::vector<Body>& bodiesA, const ElementRef& a,
               const glm::dmat4& xfA, const std::vector<Body>& bodiesB,
               const ElementRef& b, const glm::dmat4& xfB, DistanceKind kind);

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
  glm::dmat4 xf{1.0};
  ElementRef ref; // name empty => the whole set
  bool Whole() const { return ref.name.empty(); }
};

// What a whole placed shape measures as, for a one-element readout: the
// volume and surface area a part actually has, and the box it needs.
struct SetMeasure {
  double volume = 0.0;
  double surfaceArea = 0.0;
  glm::dvec3 size{0.0}; // world-axis-aligned bounding box extents
  int solids = 0;
};
std::optional<SetMeasure> DescribeTarget(const Target& t);

// Distance between any two measurables, elements or wholes.
std::optional<DistanceResult> DistanceBetween(const Target& a, const Target& b,
                                              DistanceKind kind);

// The point on a target closest to (or, for Maximum, furthest from) `p` —
// what a POINT measurable measures against. A corner is a point, so its side
// of a measurement is exact and only the other side needs solving.
std::optional<glm::dvec3> NearestPointOn(const Target& t, const glm::dvec3& p,
                                         DistanceKind kind);

// Whether two WHOLE measurables overlap, and by how much — so a pair of
// parts that interpenetrate reports as a clash rather than as distance 0,
// which is what the extrema solver says about any two overlapping solids and
// is the least useful true thing available. nullopt when either side is an
// element (a face has no volume to overlap with).
//
// `apart`, when the caller already measured the exact minimum distance
// between the two (as `measure` does first), spares the boolean: surfaces
// that are apart meet nowhere, so the solids overlap only if one is wholly
// inside the other, and one point of each solid classified against the
// other settles that exactly. Between two meshing gears the boolean was
// most of a `measure`.
std::optional<SetContact> OverlapBetween(const Target& a, const Target& b,
                                         double apart = -1.0);

// The minimum distance between two whole targets, off their TRIANGLES: the
// same answer `interference mode=mesh` decides a clash with, and the same
// caveat — it is the distance to the tessellation, +/- the deflection it
// reports. For when the exact one is pathological.
collide::Gap MeshGap(const Target& a, const Target& b);

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
std::optional<glm::dvec3> CornerOf(const std::vector<Body>& bodies,
                                   const ElementRef& edgeA,
                                   const ElementRef& edgeB);

// A representative point ON an element: a face's centre of mass, an edge's
// parametric midpoint. Where a dimension should attach when it has a choice.
std::optional<glm::dvec3> AnchorOf(const std::vector<Body>& bodies,
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
std::optional<glm::dvec3> CentreOf(const std::vector<Body>& bodies,
                                   const ElementRef& ref);

// The whole circle a circular EDGE lies on, read off every edge of the same
// body that lies on it (same centre, axis and radius, to a tolerance relative
// to the radius). A rim is often not one edge: the faces around a hole split
// it into arcs — a seat face that was two faces before the hole was cut, a
// slot crossing it — and each arc is still the rim. `arcs` is how many edges
// were found and `coveredDeg` how much of the circle they cover together, so
// a caller that needs a closed rim can refuse one that is not, in those
// numbers. nullopt when `ref` does not resolve to a circular edge.
struct RimCircle {
  glm::dvec3 centre{0.0};
  glm::dvec3 axis{0.0, 0.0, 1.0}; // the picked edge's circle axis
  double radius = 0.0;
  int arcs = 0;
  double coveredDeg = 0.0;
  [[nodiscard]] bool Closed() const { return coveredDeg >= 360.0 - 1e-3; }
};
std::optional<RimCircle> RimCircleOf(const std::vector<Body>& bodies,
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
                 const glm::dmat4& xfA, const std::vector<Body>& bodiesB,
                 const ElementRef& b, const glm::dmat4& xfB, bool parallel);

// Axis-aligned bounding box of one resolved element (face or edge), in the
// bodies' own (part-local) space — feeds zoom-to-selection. nullopt when the
// ref no longer resolves.
struct Bounds {
  glm::dvec3 min{0.0};
  glm::dvec3 max{0.0};
};
std::optional<Bounds> ElementBounds(const std::vector<Body>& bodies,
                                    const ElementRef& ref);

// Total cross-section area where the plane (through `origin`, unit `normal`)
// cuts through all bodies (inspection I3): the sum over bodies of the area of
// their intersection with the plane. 0 when the plane misses everything;
// nullopt on an OCC failure.
std::optional<double> CrossSectionArea(const std::vector<Body>& bodies,
                                       const glm::dvec3& origin,
                                       const glm::dvec3& normal);

// The same cut, decomposed into its CONNECTED REGIONS — one entry per island
// of material the plane crosses, with that island's area and centroid.
//
// The total alone cannot answer the question that matters about a section,
// which is whether the material is where you think it is. An air-cooled
// barrel with four bolt bosses hanging off its deck flange sections at
// 3293.76 mm^2 where the sound one sections at 2586.90 — the difference is
// 706.9, exactly pi*15^2, the boss standing as its OWN island in a number
// that cannot say so
// (generators/README.md#single-cylinder-and-air-cooled-engines).
//
// It is not a verdict: real parts have islands in section, and a fork has two
// tines. It makes "is this feature actually joined to the rest here?" a
// question somebody can ask for the price of a section they were computing
// anyway — the regions come out of the same boolean, and were being thrown
// away.
//
// PER BODY, and two overlapping bodies are two regions. The document holds
// them as two solids and joining solids is what a Boolean (an extrude's
// `add`) is for; a caller asking whether a feature is joined is asking about
// a part that has been through that already.
// One boundary curve of a cut region. `kind` is "line", "arc" (a circle or a
// piece of one, which then carries centre and radius) or "other" for anything
// else, whose ends still bound it.
struct SectionSegment {
  std::string kind;
  glm::dvec3 from{0.0};
  glm::dvec3 to{0.0};
  glm::dvec3 center{0.0}; // arc only
  double radius = 0.0;    // arc only
};
struct SectionLoop {
  bool outer = false;
  std::vector<SectionSegment> segments;
};
struct SectionRegion {
  double area = 0.0;
  glm::dvec3 centroid{0.0};
  // The region's OUTLINE, only when asked for: an area and a centroid say a
  // brace is 34,466 mm2 somewhere near the middle, and recovering a part you
  // did not draw needs the shape. Off by default because it walks every edge
  // of the cut.
  std::vector<SectionLoop> loops;
};
std::optional<std::vector<SectionRegion>>
CrossSectionRegions(const std::vector<Body>& bodies, const glm::dvec3& origin,
                    const glm::dvec3& normal, bool withLoops = false);

// The area the cut ENCLOSES rather than the area it crosses: the plane's
// regions that are not material but are ringed by it, after the bodies' cut
// faces are joined (docs/inspect/README.md#enclosed-area). Four separate wall
// bodies that meet at the corners enclose the floor between them; a plate
// with a bore encloses the bore. A ring with a gap in it encloses nothing,
// and so answers 0 -- the honest answer when the walls do not close.
//
// Material of `bodies` standing inside the ring is not enclosed area (a
// column in a room takes its own footprint off the floor); leave a body out
// of `bodies` to count the area under it. 0 when the plane misses
// everything; nullopt on an OCC failure.
std::optional<double> SectionEnclosedArea(const std::vector<Body>& bodies,
                                          const glm::dvec3& origin,
                                          const glm::dvec3& normal);

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
// Caps are recomputed for every body on every frame the scene rebuilds,
// which makes SCRUBBING a mechanism with a section on cost an OCC half-space
// boolean per body per step: measured at 9.5 ms/step without a section and
// 913.6 with one on the air-cooled engine, 96x. Most of an assembly does not
// move between two poses of a mechanism, and a body that has not moved has
// the same cap.
//
// The key is `CADShape::PlacementId()` — geometry AND placement — plus the
// planes. Note what an entry HOLDS: the source shape as well as the cap.
// PlacementId mixes the TShape's address, and a freed TShape's address can
// be recycled by the next one allocated; a cache that kept only the answer
// would then serve it for a different shape. That is exactly how an earlier
// bbox cache came to fail a determinism test in parallel and pass alone.
// Holding the source keeps the address reserved for as long as the key
// naming it exists, and the cache is CLEARED WHOLESALE when it grows rather
// than evicting entries one at a time, so no key ever outlives its shape.
struct SectionCapCache {
  struct Entry {
    CADShape source;
    CADShape cap;
  };
  std::unordered_map<std::string, Entry> entries;
  uint64_t hits = 0, misses = 0;
  void Clear() {
    entries.clear();
    hits = 0;
    misses = 0;
  }
};

std::vector<CADShape>
SectionCaps(const std::vector<Body>& bodies, const glm::dvec3& origin,
            const glm::dvec3& normal,
            const std::vector<glm::dvec4>& trimPlanes = {},
            SectionCapCache* cache = nullptr);

} // namespace Measure
