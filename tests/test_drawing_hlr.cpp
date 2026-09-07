#include "ui/CreateActions.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "cad/CADKernel.h"
#include "core/Conventions.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingProjection.h"
#include "core/io/Serialization.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt.hxx>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>

using Catch::Matchers::WithinAbs;
using drawing::CurveKind;
using drawing::InputBody;
using drawing::ProjectedCurve;
using drawing::ProjectionOptions;
using drawing::ProjectView;

namespace {

// The FEM0 spike geometry: a 100 x 40 x 40 block with an 8 mm-radius hole
// bored through it along Z at (50, 20) — planar and cylindrical faces.
CADShape BlockWithHole() {
  CADShape block = CADKernel::Instance().CreateBox(100.0, 40.0, 40.0);
  CADShape hole = CADKernel::Instance().CreateCylinder(
      glm::vec3(50, 20, -1), glm::vec3(0, 0, 1), 8.0, 42.0);
  return block.Difference(hole);
}

// Named orthographic directions used throughout: the viewer LOOKS ALONG
// viewDir. "Front" looks along +Y (sees the XZ face), "top" looks along -Z.
const glm::dvec3 kFrontDir{0, 1, 0}, kFrontUp{0, 0, 1};
const glm::dvec3 kTopDir{0, 0, -1}, kTopUp{0, 1, 0};
const glm::dvec3 kSideDir{1, 0, 0}, kSideUp{0, 0, 1};
const glm::dvec3 kIsoDir = glm::normalize(glm::dvec3(-1, 1, -1));
const glm::dvec3 kIsoUp{0, 0, 1};

std::vector<InputBody> One(const CADShape& shape, uint64_t id = 1) {
  return {InputBody{shape.GetShape(), glm::dmat4(1.0), id}};
}

// Projected bounding box across all curves (optionally visible only).
struct Box2 {
  glm::dvec2 min{1e30}, max{-1e30};
  double W() const { return max.x - min.x; }
  double H() const { return max.y - min.y; }
};
Box2 BoundsOf(const std::vector<ProjectedCurve>& curves, bool visibleOnly) {
  Box2 b;
  for (const auto& c : curves) {
    if (visibleOnly && !c.visible)
      continue;
    for (const auto& p : c.points) {
      b.min = glm::min(b.min, p);
      b.max = glm::max(b.max, p);
    }
  }
  return b;
}

// The 0-based index (TopExp::MapShapes order) of the shape's one cylindrical
// face — what silhouette SourceRefs must point at.
int CylindricalFaceIndex(const TopoDS_Shape& shape) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  int found = -1;
  for (int i = 1; i <= faces.Extent(); ++i) {
    BRepAdaptor_Surface surf(TopoDS::Face(faces.FindKey(i)));
    if (surf.GetType() == GeomAbs_Cylinder) {
      REQUIRE(found == -1); // exactly one expected in these fixtures
      found = i - 1;
    }
  }
  REQUIRE(found >= 0);
  return found;
}

int EdgeCount(const TopoDS_Shape& shape) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  return edges.Extent();
}

} // namespace

#ifdef HAVE_OPENCASCADE

// The projector takes a direction and an up vector, and the tests around it
// pass those in by hand — so nothing here ever checked the one table that
// decides which direction each NAMED view means. That table was written
// Z-up in a Y-up app, which made every "front" view a top view and every
// "top" a front, on every drawing anyone had made. Found by drawing a plate
// and seeing an 80x60 "front" of something 8 mm thick.
//
// The plate has three different dimensions on purpose: with a cube, or even
// a square plate, a swapped axis is invisible.
TEST_CASE("Named view orientations agree with the model's up axis",
          "[drawing][hlr]") {
  // Width 80 along X, depth 60 along Y, thickness 8 along Z (up): a plate
  // lying flat on the world's floor.
  CADShape plate = CADKernel::Instance().CreateBox(80.0, 60.0, 8.0);
  REQUIRE(plate.IsValid());

  const auto extentOf = [&](ViewOrientation o) {
    glm::dvec3 dir, up;
    OrientationAxes(o, dir, up);
    std::string err;
    auto result = ProjectView(One(plate), dir, up, {}, &err);
    INFO(err);
    REQUIRE(result.has_value());
    glm::dvec2 lo(1e30), hi(-1e30);
    for (const auto& c : result->curves)
      for (const glm::dvec2& p : c.points)
        lo = glm::min(lo, p), hi = glm::max(hi, p);
    return hi - lo;
  };
  const auto approx = [](const glm::dvec2& v, double w, double h) {
    return std::abs(v.x - w) < 1e-6 && std::abs(v.y - h) < 1e-6;
  };

  using O = ViewOrientation;
  // Looking at the front of a plate: its width across, its thickness up.
  CHECK(approx(extentOf(O::Front), 80.0, 8.0));
  CHECK(approx(extentOf(O::Back), 80.0, 8.0));
  // Looking down on it: the full face.
  CHECK(approx(extentOf(O::Top), 80.0, 60.0));
  CHECK(approx(extentOf(O::Bottom), 80.0, 60.0));
  // From the side: depth across, thickness up.
  CHECK(approx(extentOf(O::Right), 60.0, 8.0));
  CHECK(approx(extentOf(O::Left), 60.0, 8.0));

  // Folding depends on the SHARED axes, not just the sizes: a top view sits
  // above its front view (same screen-right), a right view beside it (same
  // screen-up). Getting the sizes right with a mangled basis is possible;
  // getting these right too is not.
  const auto axes = [](O o) {
    glm::dvec3 dir, up;
    OrientationAxes(o, dir, up);
    return std::pair{glm::normalize(glm::cross(dir, up)), up};
  };
  const auto [frontRight, frontUp] = axes(O::Front);
  CHECK(glm::length(axes(O::Top).first - frontRight) < 1e-9);
  CHECK(glm::length(axes(O::Right).second - frontUp) < 1e-9);
  CHECK(frontUp == conv::kWorldUp); // named views stand up in the world frame
}

TEST_CASE("HLR box front view traces every curve to a source edge",
          "[drawing][hlr]") {
  CADShape box = CADKernel::Instance().CreateBox(100.0, 40.0, 40.0);
  REQUIRE(box.IsValid());

  std::string err;
  auto result = ProjectView(One(box), kFrontDir, kFrontUp, {}, &err);
  INFO(err);
  REQUIRE(result.has_value());

  CHECK(result->stats.unresolvedSources == 0);
  const int nEdges = EdgeCount(box.GetShape());

  int visibleLines = 0;
  std::set<int> visibleSourceEdges;
  for (const auto& c : result->curves) {
    CHECK(c.kind == CurveKind::Sharp);
    CHECK(c.curveType == 0); // GeomAbs_Line
    CHECK(c.source.sourceId == 1);
    CHECK(c.source.edgeIndex >= 0);
    CHECK(c.source.edgeIndex < nEdges);
    if (c.visible) {
      ++visibleLines;
      visibleSourceEdges.insert(c.source.edgeIndex);
    }
  }
  // The front face's 4 edges are visible; OCC documents that superimposed
  // lines are NOT eliminated, so the coincident back-face edges may also
  // appear — assert floors, not exact counts (recorded for the D0 gate).
  CHECK(visibleLines >= 4);
  CHECK(visibleSourceEdges.size() >= 4);

  const Box2 b = BoundsOf(result->curves, true);
  CHECK_THAT(b.W(), WithinAbs(100.0, 1e-6));
  CHECK_THAT(b.H(), WithinAbs(40.0, 1e-6));
}

TEST_CASE(
    "HLR cylinder side view yields two silhouettes on the cylindrical face",
    "[drawing][hlr]") {
  CADShape cyl = CADKernel::Instance().CreateCylinder(
      glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 8.0, 40.0);
  REQUIRE(cyl.IsValid());
  const int cylFace = CylindricalFaceIndex(cyl.GetShape());

  std::string err;
  auto result = ProjectView(One(cyl), kSideDir, kSideUp, {}, &err);
  INFO(err);
  REQUIRE(result.has_value());
  CHECK(result->stats.unresolvedSources == 0);

  // Exactly two visible silhouette lines, both generated by the cylindrical
  // face — the "diameter of a shaft in side view" case drawing dimensions
  // hang on. The seam edge is Sewn (RgNLine), never an outline.
  std::vector<const ProjectedCurve*> outlines;
  for (const auto& c : result->curves) {
    if (c.kind == CurveKind::Outline && c.visible)
      outlines.push_back(&c);
    if (c.kind == CurveKind::Sewn)
      CHECK(c.source.edgeIndex >= 0);
  }
  REQUIRE(outlines.size() == 2);
  for (const auto* c : outlines) {
    CHECK(c->source.sourceId == 1);
    CHECK(c->source.faceIndex == cylFace);
    CHECK(c->source.edgeIndex == -1);
  }
  // The two silhouettes sit 2R apart in the view plane.
  const double x0 = outlines[0]->points.front().x;
  const double x1 = outlines[1]->points.front().x;
  CHECK_THAT(std::abs(x0 - x1), WithinAbs(16.0, 1e-6));
}

TEST_CASE("HLR blind-bore hidden lines attribute to the bore face",
          "[drawing][hlr]") {
  CADShape part = BlockWithHole();
  REQUIRE(part.IsValid());
  const int boreFace = CylindricalFaceIndex(part.GetShape());

  SECTION(
      "front view: the bore shows as hidden lines, silhouette traced to face") {
    std::string err;
    auto result = ProjectView(One(part), kFrontDir, kFrontUp, {}, &err);
    INFO(err);
    REQUIRE(result.has_value());
    CHECK(result->stats.unresolvedSources == 0);

    // The through-bore (radius 8 at x=50) presents two hidden walls at x=42
    // and x=58. One wall is a pure projection silhouette (Outline, traced to
    // the bore face — the traceability this test proves); the other happens
    // to coincide with the cylinder's seam edge, a real model edge. Both are
    // hidden, which is what the drawing shows.
    bool hiddenAt42 = false, hiddenAt58 = false, silhouetteOnBoreFace = false;
    for (const auto& c : result->curves) {
      if (c.visible)
        continue;
      const double x = c.points.front().x;
      if (std::abs(x - 42.0) < 1.0)
        hiddenAt42 = true;
      if (std::abs(x - 58.0) < 1.0)
        hiddenAt58 = true;
      if (c.kind == CurveKind::Outline && c.source.faceIndex == boreFace)
        silhouetteOnBoreFace = true;
    }
    CHECK(hiddenAt42);
    CHECK(hiddenAt58);
    CHECK(silhouetteOnBoreFace);
  }

  SECTION("hidden lines off: no hidden curves are returned") {
    ProjectionOptions opts;
    opts.hiddenLines = false;
    std::string err;
    auto result = ProjectView(One(part), kFrontDir, kFrontUp, opts, &err);
    REQUIRE(result.has_value());
    for (const auto& c : result->curves)
      CHECK(c.visible);
  }

  SECTION("top view: the bore rim is a visible circle with symbolic radius") {
    std::string err;
    auto result = ProjectView(One(part), kTopDir, kTopUp, {}, &err);
    REQUIRE(result.has_value());
    CHECK(result->stats.unresolvedSources == 0);

    bool foundRim = false;
    for (const auto& c : result->curves) {
      if (c.visible && c.curveType == 1 /* GeomAbs_Circle */ &&
          std::abs(c.conicRadius - 8.0) < 1e-6) {
        foundRim = true;
        CHECK(c.source.edgeIndex >= 0); // a real model edge, not a silhouette
      }
    }
    CHECK(foundRim);
  }
}

TEST_CASE("HLR hides one instance behind another with per-instance attribution",
          "[drawing][hlr]") {
  CADShape part = BlockWithHole();
  REQUIRE(part.IsValid());

  // Two instances of the SAME shape (shared TShapes, the assembly case): the
  // second sits directly behind the first along the front view direction, so
  // every one of its curves must come back hidden — and attributed to
  // sourceId 2 despite the shared topology.
  std::vector<InputBody> bodies = One(part, 1);
  bodies.push_back(
      InputBody{part.GetShape(),
                glm::translate(glm::dmat4(1.0), glm::dvec3(0, 60, 0)), 2});

  std::string err;
  auto result = ProjectView(bodies, kFrontDir, kFrontUp, {}, &err);
  INFO(err);
  REQUIRE(result.has_value());
  CHECK(result->stats.unresolvedSources == 0);

  int fromFirst = 0, fromSecond = 0;
  for (const auto& c : result->curves) {
    if (c.source.sourceId == 1)
      ++fromFirst;
    if (c.source.sourceId == 2) {
      ++fromSecond;
      CHECK(!c.visible);
    }
  }
  CHECK(fromFirst > 0);
  CHECK(fromSecond > 0);
}

TEST_CASE("HLR curve output: types and discretization (gate 3 record)",
          "[drawing][hlr]") {
  CADShape part = BlockWithHole();
  REQUIRE(part.IsValid());

  // Type histogram in an iso view — the D0 gate-3 record of what the
  // exporter and dimension measurement will consume.
  std::string err;
  auto iso = ProjectView(One(part), kIsoDir, kIsoUp, {}, &err);
  REQUIRE(iso.has_value());
  std::map<int, int> histogram;
  for (const auto& c : iso->curves)
    ++histogram[c.curveType];
  std::cout << "[hlr gate3] iso curve types (GeomAbs):";
  for (const auto& [type, count] : histogram)
    std::cout << " " << type << "=" << count;
  std::cout << "\n";
  CHECK(histogram.count(0) == 1); // lines exist
  // An iso view of a bore yields conic or bspline images, never only lines.
  CHECK(iso->curves.size() > size_t(histogram[0]));

  // Deflection controls point counts on curved geometry.
  ProjectionOptions coarse, fine;
  coarse.deflection = 0.05;
  fine.deflection = 0.01;
  auto top1 = ProjectView(One(part), kTopDir, kTopUp, coarse, &err);
  auto top2 = ProjectView(One(part), kTopDir, kTopUp, fine, &err);
  REQUIRE((top1.has_value() && top2.has_value()));
  const auto pointsOf = [](const drawing::ProjectionResult& r) {
    size_t n = 0;
    for (const auto& c : r.curves)
      n += c.points.size();
    return n;
  };
  std::cout << "[hlr gate3] top-view points @0.05mm=" << pointsOf(*top1)
            << " @0.01mm=" << pointsOf(*top2) << "\n";
  CHECK(pointsOf(*top2) >= pointsOf(*top1));
}

TEST_CASE("HLR projection rejects bad input cleanly", "[drawing][hlr]") {
  std::string err;
  CHECK(!ProjectView({}, kFrontDir, kFrontUp, {}, &err).has_value());
  CHECK(!err.empty());

  CADShape box = CADKernel::Instance().CreateBox(10, 10, 10);
  err.clear();
  CHECK(!ProjectView(One(box), glm::dvec3(0), kFrontUp, {}, &err).has_value());
  CHECK(!err.empty());
  err.clear();
  // up parallel to the view direction: no view plane basis.
  CHECK(!ProjectView(One(box), kFrontDir, kFrontDir, {}, &err).has_value());
  CHECK(!err.empty());
}

// ---- D0 gate 2: exact-HLR timings (printed; thresholds in the gate note) ---

TEST_CASE("HLR timing: real part (v17-full golden)", "[drawing][hlr][perf]") {
  namespace fs = std::filesystem;
  const fs::path golden =
      fs::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" / "v17-full.json";
  REQUIRE(fs::exists(golden));
  std::string error;
  auto doc = testio::Load(golden.string());
  REQUIRE((doc != nullptr && error.empty()));

  std::vector<InputBody> bodies;
  uint64_t id = 1;
  for (const auto& body : doc->GetBodies())
    bodies.push_back(InputBody{body.shape.GetShape(), glm::dmat4(1.0), id++});
  REQUIRE(!bodies.empty());

  for (const auto& [name, dir, up] :
       {std::tuple{"front", kFrontDir, kFrontUp}, {"iso", kIsoDir, kIsoUp}}) {
    std::string err;
    auto result = ProjectView(bodies, dir, up, {}, &err);
    INFO(err);
    REQUIRE(result.has_value());
    CHECK(result->stats.unresolvedSources == 0);
    std::cout << "[hlr perf] part " << name
              << ": update=" << result->stats.updateMs
              << "ms hide=" << result->stats.hideMs
              << "ms dsEdges=" << result->stats.dsEdges
              << " curves=" << result->curves.size() << "\n";
  }
}

namespace {

// A synthetic assembly-scale input: an NxN grid of BlockWithHole instances
// (~9 faces each), all sharing one TShape — the A2 instancing shape.
std::vector<InputBody> Grid(const CADShape& part, int n) {
  std::vector<InputBody> bodies;
  uint64_t id = 1;
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < n; ++j)
      bodies.push_back(InputBody{
          part.GetShape(),
          glm::translate(glm::dmat4(1.0), glm::dvec3(i * 110.0, j * 50.0, 0)),
          id++});
  return bodies;
}

void TimeGrid(const CADShape& part, int n) {
  auto bodies = Grid(part, n);
  std::string err;
  auto result = ProjectView(bodies, kIsoDir, kIsoUp, {}, &err);
  INFO(err);
  REQUIRE(result.has_value());
  CHECK(result->stats.unresolvedSources == 0);
  std::cout << "[hlr perf] " << n * n
            << "-instance grid iso: update=" << result->stats.updateMs
            << "ms hide=" << result->stats.hideMs
            << "ms dsEdges=" << result->stats.dsEdges
            << " curves=" << result->curves.size() << "\n";
}

} // namespace

TEST_CASE("HLR timing: 25-instance assembly grid", "[drawing][hlr][perf]") {
  CADShape part = BlockWithHole();
  REQUIRE(part.IsValid());
  TimeGrid(part, 5);
}

// Headroom number for the gate note; hidden — run explicitly with
//   designer_tests "[hlr-perf50]"
TEST_CASE("HLR timing: 49-instance assembly grid", "[.][hlr-perf50]") {
  CADShape part = BlockWithHole();
  REQUIRE(part.IsValid());
  TimeGrid(part, 7);
}

#endif // HAVE_OPENCASCADE

TEST_CASE("HLR marks full-sweep conic intervals closed",
          "[drawing][hlr][closed]") {
  // Top view of the block-with-hole: the bore rim projects as full circles.
  const CADShape shape = BlockWithHole();
  std::string err;
  const auto res = ProjectView(One(shape), kTopDir, kTopUp, {}, &err);
  INFO(err);
  REQUIRE(res.has_value());
  bool sawClosedCircle = false;
  for (const ProjectedCurve& c : res->curves)
    if (c.conicRadius > 0.0 && c.closed)
      sawClosedCircle = true;
  CHECK(sawClosedCircle);

  // A tall slab burying half the bore splits the rim into partial visible /
  // hidden intervals - none of them is a full sweep on the visible side.
  CADShape slab = CADKernel::Instance().CreateBox(50.0, 40.0, 60.0);
  std::vector<InputBody> two = One(shape);
  two.push_back(InputBody{slab.GetShape(), glm::dmat4(1.0), 2});
  const auto occluded = ProjectView(two, kTopDir, kTopUp, {}, &err);
  REQUIRE(occluded.has_value());
  // The visible rim arcs of the half-buried bore are partial sweeps.
  int partialVisibleArcs = 0;
  for (const ProjectedCurve& c : occluded->curves)
    if (c.conicRadius > 0.0 && c.visible && !c.closed)
      ++partialVisibleArcs;
  CHECK(partialVisibleArcs > 0);
}

TEST_CASE("A deep-copied shape projects identically, edge names included",
          "[drawing][hlr]") {
  // The precondition for taking HLR off the main thread
  // (docs/agent-scale-followup-plan.md, W3). A worker cannot read the live
  // documents' shapes — poly HLR MESHES them, which writes triangulation into
  // the shared TFace — so it would have to work on copies.
  //
  // Copies are only safe if BRepBuilderAPI_Copy preserves EXPLORER ORDER.
  // The projection reports its source edges as indices, and AssembleSheetView
  // turns those indices into stable element names through the ElementMap. If
  // a copy reorders the edges by even one, every dimension on the sheet
  // silently anchors to the wrong feature — which looks entirely correct.
  //
  // So this asks the question outright rather than assuming the answer.
  BRepPrimAPI_MakeBox mk(40.0, 30.0, 20.0);
  const TopoDS_Shape original = mk.Shape();
  const TopoDS_Shape copy = BRepBuilderAPI_Copy(original).Shape();

  TopTools_IndexedMapOfShape a, b;
  TopExp::MapShapes(original, TopAbs_EDGE, a);
  TopExp::MapShapes(copy, TopAbs_EDGE, b);
  REQUIRE(a.Extent() == b.Extent());
  REQUIRE(a.Extent() == 12);

  // Same index, same edge — compared by the geometry, since the copy shares
  // no pointers with the original by definition.
  int matched = 0;
  for (int i = 1; i <= a.Extent(); ++i) {
    TopoDS_Vertex a1, a2, b1, b2;
    TopExp::Vertices(TopoDS::Edge(a.FindKey(i)), a1, a2);
    TopExp::Vertices(TopoDS::Edge(b.FindKey(i)), b1, b2);
    if (a1.IsNull() || b1.IsNull())
      continue;
    const gp_Pnt pa = BRep_Tool::Pnt(a1);
    const gp_Pnt pb = BRep_Tool::Pnt(b1);
    INFO("edge " << i << ": original starts at (" << pa.X() << ", " << pa.Y()
                 << ", " << pa.Z() << "), copy at (" << pb.X() << ", " << pb.Y()
                 << ", " << pb.Z() << ")");
    CHECK(pa.Distance(pb) < 1e-9);
    ++matched;
  }
  CHECK(matched == 12);
}
