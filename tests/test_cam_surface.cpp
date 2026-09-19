#include "cam_fixtures.h"

#ifdef HAVE_CAM

#include "cad/CADKernel.h"
#include "core/cam/CamDropCutter.h"
#include "core/cam/CamSurface.h"

// The cutter-location surface (docs/cam/surfacing.md, phase 1).
//
// Every case here has an answer a machinist could work out on paper, because
// the failure this file exists to prevent is a finishing pass that gouges: a
// CL surface one cutter radius wrong is a part cut to the wrong shape with
// nothing in the toolpath that looks unusual.

namespace {

cam::Tool FlatMill(double diameter) {
  cam::Tool t;
  t.number = 1;
  t.diameter = diameter;
  t.type = cam::ToolType::Flat;
  return t;
}

cam::Tool BallNose(double diameter) {
  cam::Tool t;
  t.number = 2;
  t.diameter = diameter;
  t.type = cam::ToolType::Ball;
  return t;
}

// The CL height at a point, by nearest cell.
double At(const cam::CLSurface& s, double x, double y) {
  const int i = static_cast<int>(std::lround((x - s.z.x0) / s.z.res - 0.5));
  const int j = static_cast<int>(std::lround((y - s.z.y0) / s.z.res - 0.5));
  REQUIRE(i >= 0);
  REQUIRE(j >= 0);
  REQUIRE(i < s.z.nx);
  REQUIRE(j < s.z.ny);
  return s.z.At(i, j);
}

} // namespace

TEST_CASE("drop cutter: a flat mill rests on a flat top", "[cam][surface]") {
  // The simplest answer there is, and the one that catches a sign error in
  // the dilation: TipHeight is 0 everywhere under a flat mill, so its tip
  // sits exactly on the surface it is standing on.
  auto doc = BlockDoc(40, 30, 20); // top at z = 20
  const cam::Tool tool = FlatMill(6.0);

  cam::DropRequest req;
  req.min = {5.0, 5.0};
  req.max = {35.0, 25.0};
  req.res = 0.5;
  req.floorZ = -10.0;

  const cam::CLSurface s = cam::DropCut(doc->GetBodies(), tool, req);
  INFO(s.why);
  REQUIRE(s.ok);
  CHECK_THAT(At(s, 20.0, 15.0), WithinAbs(20.0, 1e-3));
  CHECK_THAT(At(s, 10.0, 10.0), WithinAbs(20.0, 1e-3));
}

TEST_CASE("drop cutter: a flat mill's rim holds it up past the edge",
          "[cam][surface]") {
  // The dilation's whole point. A flat mill whose CENTRE is outside the block
  // is still resting on it until the centre is one radius clear -- that is
  // what stops a finishing pass diving down the side of the part.
  auto doc = BlockDoc(40, 30, 20); // block spans x 0..40
  const cam::Tool tool = FlatMill(6.0);

  cam::DropRequest req;
  req.min = {-10.0, 10.0};
  req.max = {50.0, 20.0};
  req.res = 0.25;
  req.floorZ = -10.0;

  const cam::CLSurface s = cam::DropCut(doc->GetBodies(), tool, req);
  INFO(s.why);
  REQUIRE(s.ok);
  // Just past the wall: the rim is still on the top face.
  CHECK_THAT(At(s, 42.0, 15.0), WithinAbs(20.0, 1e-3));
  // Just short of a full radius: still held up.
  CHECK_THAT(At(s, 42.8, 15.0), WithinAbs(20.0, 1e-3));
  // Past the radius: nothing under the tool at all.
  CHECK_THAT(At(s, 44.0, 15.0), WithinAbs(-10.0, 1e-3));
}

TEST_CASE("drop cutter: a ball nose on a cylinder is where the arithmetic says",
          "[cam][surface]") {
  // A ball of radius r rolling over a cylinder of radius R has its CENTRE on
  // a circle of radius R+r, so its tip is at sqrt((R+r)^2 - x^2) - r. This is
  // the case a flat-tool sampler gets wrong everywhere except the apex, and
  // it is what says the tool PROFILE is really being used.
  const double R = 10.0;
  const double r = 2.0;
  Body body;
  body.name = "Cyl";
  // Axis along Y so the section in X is a circle, centred on z = 0.
  body.shape = CADKernel::Instance().CreateCylinder(
      glm::vec3(0.0f, -20.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), R, 40.0);
  REQUIRE(body.shape.IsValid());
  std::vector<Body> bodies;
  bodies.push_back(std::move(body));

  const cam::Tool tool = BallNose(2.0 * r);
  cam::DropRequest req;
  req.min = {-6.0, -5.0};
  req.max = {6.0, 5.0};
  req.res = 0.1;
  req.floorZ = -30.0;

  const cam::CLSurface s = cam::DropCut(bodies, tool, req);
  INFO(s.why);
  REQUIRE(s.ok);
  for (const double x : {0.0, 2.0, 4.0, 6.0}) {
    const double want = std::sqrt((R + r) * (R + r) - x * x) - r;
    INFO("x = " << x << ", expected tip z " << want);
    // Loose enough for the grid and the tessellation, tight enough that the
    // flat-tool answer (sqrt(R^2 - x^2), which is 0.17 mm out at x = 4 and
    // 1.1 mm out at x = 6) cannot pass.
    CHECK_THAT(At(s, x, 0.0), WithinAbs(want, 0.05));
  }
}

TEST_CASE("drop cutter: a cutter too fat for a gap stays out of it",
          "[cam][surface]") {
  // The other half of "the lowest Z it can sit without cutting into the
  // model". Two 20 mm-tall blocks with a 2 mm gap between them: a 6 mm ball
  // cannot enter the gap, so over its middle the tool is still resting on the
  // two tops and the CL surface is 20 -- NOT the floor it can see but cannot
  // reach. A surface that dipped here would be planning a gouge into both
  // walls.
  std::vector<Body> bodies;
  Body left;
  left.name = "L";
  left.shape = CADKernel::Instance().CreateBox(19.0, 30.0, 20.0);
  REQUIRE(left.shape.IsValid());
  bodies.push_back(std::move(left));

  Body right;
  right.name = "R";
  right.shape = CADKernel::Instance().CreateBox(19.0, 30.0, 20.0);
  REQUIRE(right.shape.IsValid());
  right.shape.Translate(glm::vec3(21.0f, 0.0f, 0.0f));
  bodies.push_back(std::move(right));

  const cam::Tool tool = BallNose(6.0);
  cam::DropRequest req;
  req.min = {14.0, 5.0};
  req.max = {26.0, 25.0};
  req.res = 0.2;
  req.floorZ = -10.0;

  const cam::CLSurface s = cam::DropCut(bodies, tool, req);
  INFO(s.why);
  REQUIRE(s.ok);

  // Dead centre of the gap. The answer is NOT 20: a ball is round, so its
  // flank does enter a gap narrower than itself, resting on the two top
  // edges 1 mm either side. Its tip then sits TipHeight(r=3, d=1) below
  // them -- 3 - sqrt(8) = 0.1716 mm -- and not one micron further.
  const double dip = 3.0 - std::sqrt(8.0);
  CHECK_THAT(At(s, 20.0, 15.0), WithinAbs(20.0 - dip, 5e-3));
  // What it must not do is reach the floor it can see between them. A
  // surface that let the tool to z = 0 here would plan a full-depth cut
  // into both walls.
  CHECK(At(s, 20.0, 15.0) > 19.0);
  // On the tops themselves it rests on them exactly.
  CHECK_THAT(At(s, 16.0, 15.0), WithinAbs(20.0, 1e-2));
  CHECK_THAT(At(s, 24.0, 15.0), WithinAbs(20.0, 1e-2));
}

TEST_CASE("surface pick: a curved face gives its shadow, not its wire",
          "[cam][surface]") {
  // The pick the other two refuse. A cylinder's curved face has an outer wire
  // that runs over the surface in 3D, so flattening THAT gives the boundary of
  // a curved patch and not the area underneath it. The silhouette of a
  // cylinder of radius 10 lying along Y over a length of 40 is a 20 x 40
  // rectangle, and its area says which of the two was computed.
  Body body;
  body.name = "Cyl";
  body.shape = CADKernel::Instance().CreateCylinder(
      glm::vec3(0.0f, -20.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 10.0, 40.0);
  REQUIRE(body.shape.IsValid());
  std::vector<Body> bodies;
  bodies.push_back(std::move(body));

  const cam::SurfaceRegion whole = cam::WholePartSilhouette(bodies, 0.05);
  INFO(whole.why);
  REQUIRE(whole.ok);
  REQUIRE_FALSE(whole.loops.empty());
  CHECK_FALSE(whole.loops.front().hole);

  // Area of the shadow: 20 wide (the diameter) by 40 long.
  double area = 0.0;
  const std::vector<glm::dvec2>& p = whole.loops.front().pts;
  for (size_t i = 0, n = p.size(); i < n; i++) {
    const glm::dvec2& a = p[i];
    const glm::dvec2& b = p[(i + 1) % n];
    area += a.x * b.y - b.x * a.y;
  }
  area = std::abs(area) * 0.5;
  INFO("silhouette area " << area << " mm2");
  CHECK_THAT(area, WithinRel(800.0, 0.02));

  // The Z extent is the face's own, reported but not machined to.
  CHECK_THAT(whole.minZ, WithinAbs(-10.0, 0.2));
  CHECK_THAT(whole.maxZ, WithinAbs(10.0, 0.2));
}

TEST_CASE("surface pick: what it refuses", "[cam][surface]") {
  std::vector<Body> none;
  const cam::SurfaceRegion empty = cam::WholePartSilhouette(none, 0.05);
  CHECK_FALSE(empty.ok);
  CHECK_THAT(empty.why, Catch::Matchers::ContainsSubstring("nothing"));

  auto doc = BlockDoc(40, 30, 20);
  ElementRef nowhere;
  const cam::SurfaceRegion unset =
      cam::PickedSurfaceRegion(doc->GetBodies(), nowhere, 0.05, 0);
  CHECK_FALSE(unset.ok);
  CHECK_THAT(unset.why, Catch::Matchers::ContainsSubstring("nothing picked"));
}

TEST_CASE("drop cutter: what it refuses, and what it derives",
          "[cam][surface]") {
  auto doc = BlockDoc(40, 30, 20);
  cam::DropRequest req;
  req.min = {5.0, 5.0};
  req.max = {35.0, 25.0};
  req.res = 0.5;

  cam::Tool none = FlatMill(0.0);
  const cam::CLSurface a = cam::DropCut(doc->GetBodies(), none, req);
  CHECK_FALSE(a.ok);
  CHECK_THAT(a.why, Catch::Matchers::ContainsSubstring("no diameter"));

  cam::DropRequest empty = req;
  empty.max = empty.min;
  const cam::CLSurface b = cam::DropCut(doc->GetBodies(), FlatMill(6.0), empty);
  CHECK_FALSE(b.ok);
  CHECK_THAT(b.why, Catch::Matchers::ContainsSubstring("empty"));

  // A rotated frame is the rotary case. REFUSED rather than ignored: a frame
  // silently dropped would plan against the wrong axis of the model and look
  // like a perfectly ordinary surface.
  cam::DropRequest turned = req;
  turned.frame = glm::dquat(glm::dvec3(glm::radians(90.0), 0.0, 0.0));
  const cam::CLSurface c =
      cam::DropCut(doc->GetBodies(), FlatMill(6.0), turned);
  CHECK_FALSE(c.ok);
  CHECK_THAT(c.why, Catch::Matchers::ContainsSubstring("rotary"));

  // The derived numbers, so a caller can report them without running a drop.
  const cam::Tool ball = BallNose(6.0);
  CHECK_THAT(cam::DerivedCusp(ball, 0.0), WithinAbs(0.06, 1e-12));
  CHECK_THAT(cam::DerivedCusp(ball, 0.02), WithinAbs(0.02, 1e-12));
  // Same cusp equation DerivedStepover solves, over eight samples.
  const double step = 2.0 * std::sqrt(2.0 * 3.0 * 0.06 - 0.06 * 0.06);
  CHECK_THAT(cam::DerivedDropRes(ball, 0.0, 0.06),
             WithinAbs(std::clamp(step / 8.0, 0.01, 0.5), 1e-12));
  CHECK_THAT(cam::DerivedDropRes(ball, 0.25, 0.06), WithinAbs(0.25, 1e-12));
}

TEST_CASE("parallel passes: the raster spacing is what was asked for",
          "[cam][surface]") {
  // A flat top is the case where every number is checkable by hand: the
  // passes must be flat, spaced exactly `stepover_mm` apart, and inside the
  // region. Spacing is the one a raster gets wrong when it walks at an angle
  // instead of rotating the geometry -- the step then comes out
  // stepover/cos(angle) and the scallop is wrong everywhere.
  auto doc = BlockDoc(40, 30, 20);
  const cam::Tool tool = FlatMill(6.0);
  cam::DropRequest drop;
  drop.min = {5.0, 5.0};
  drop.max = {35.0, 25.0};
  drop.res = 0.25;
  drop.floorZ = -10.0;
  const cam::CLSurface cl = cam::DropCut(doc->GetBodies(), tool, drop);
  REQUIRE(cl.ok);

  std::vector<cam::Loop> region;
  cam::Loop box;
  box.pts = {{10.0, 10.0}, {30.0, 10.0}, {30.0, 20.0}, {10.0, 20.0}};
  region.push_back(box);

  cam::ParallelRequest req;
  req.stepover_mm = 2.0;
  req.sampleStep_mm = 1.0;
  req.floorZ = -10.0;

  const cam::SurfacePaths p = cam::ParallelPasses(cl, region, req);
  INFO(p.why);
  REQUIRE(p.ok);
  // 10 mm of region at a 2 mm step is six rows counting both edges, but the
  // row lying exactly ON the far boundary is a degenerate intersection that
  // Clipper may or may not keep -- and it would cut nothing a neighbour 2 mm
  // away has not already covered with a 6 mm tool. So the count is a range
  // and the SPACING below is the real assertion.
  CHECK(p.passes.size() >= 5);
  CHECK(p.passes.size() <= 6);

  for (const cam::SurfacePath& pass : p.passes) {
    REQUIRE(pass.size() >= 2);
    for (const glm::dvec3& pt : pass) {
      CHECK_THAT(pt.z, WithinAbs(20.0, 1e-3)); // flat top, flat pass
      CHECK(pt.x >= 10.0 - 1e-6);
      CHECK(pt.x <= 30.0 + 1e-6);
      CHECK(pt.y >= 10.0 - 1e-6);
      CHECK(pt.y <= 20.0 + 1e-6);
    }
  }
  // Spacing between consecutive passes is the stepover, not something
  // scaled by the angle.
  for (size_t i = 1; i < p.passes.size(); i++)
    CHECK_THAT(p.passes[i].front().y - p.passes[i - 1].front().y,
               WithinAbs(2.0, 1e-6));
}

TEST_CASE("parallel passes: an angled raster keeps its spacing",
          "[cam][surface]") {
  // The same region at 45 degrees. The passes are no longer rows of constant
  // Y, so the check is the PERPENDICULAR distance between them, which must
  // still be exactly the stepover.
  auto doc = BlockDoc(40, 30, 20);
  cam::DropRequest drop;
  drop.min = {5.0, 5.0};
  drop.max = {35.0, 25.0};
  drop.res = 0.25;
  drop.floorZ = -10.0;
  const cam::CLSurface cl = cam::DropCut(doc->GetBodies(), FlatMill(6.0), drop);
  REQUIRE(cl.ok);

  cam::ParallelRequest req;
  req.stepover_mm = 2.0;
  req.sampleStep_mm = 1.0;
  req.cutAngleDeg = 45.0;
  req.floorZ = -10.0;

  std::vector<cam::Loop> region;
  cam::Loop box;
  box.pts = {{10.0, 10.0}, {30.0, 10.0}, {30.0, 20.0}, {10.0, 20.0}};
  region.push_back(box);

  const cam::SurfacePaths p = cam::ParallelPasses(cl, region, req);
  INFO(p.why);
  REQUIRE(p.ok);
  REQUIRE(p.passes.size() >= 3);

  // Perpendicular offset of each pass from the origin, along the raster's
  // own normal. Consecutive passes differ by the stepover.
  const double n = std::sqrt(0.5);
  std::vector<double> offsets;
  for (const cam::SurfacePath& pass : p.passes)
    offsets.push_back(-pass.front().x * n + pass.front().y * n);
  std::sort(offsets.begin(), offsets.end());
  offsets.erase(
      std::unique(offsets.begin(), offsets.end(),
                  [](double a, double b) { return std::abs(a - b) < 1e-6; }),
      offsets.end());
  REQUIRE(offsets.size() >= 3);
  // 1e-3 and not tighter: Clipper works in integers at kScale = 1e4, so a
  // coordinate carries 0.1 um of quantisation and a 45-degree rotation
  // through it compounds to about that. The error this guards against is
  // walking at an angle instead of rotating the geometry, which would make
  // the step stepover/cos(45) = 2.83 -- four hundred times the tolerance.
  for (size_t i = 1; i < offsets.size(); i++)
    CHECK_THAT(offsets[i] - offsets[i - 1], WithinAbs(2.0, 1e-3));
}

TEST_CASE("parallel passes: a hole in the region breaks the pass",
          "[cam][surface]") {
  // A pass that ran straight through a hole would cut across whatever lies
  // in it. The hole must END a pass and start another, so the tool lifts.
  auto doc = BlockDoc(40, 30, 20);
  cam::DropRequest drop;
  drop.min = {5.0, 5.0};
  drop.max = {35.0, 25.0};
  drop.res = 0.25;
  drop.floorZ = -10.0;
  const cam::CLSurface cl = cam::DropCut(doc->GetBodies(), FlatMill(6.0), drop);
  REQUIRE(cl.ok);

  std::vector<cam::Loop> region;
  cam::Loop outer;
  outer.pts = {{10.0, 10.0}, {30.0, 10.0}, {30.0, 20.0}, {10.0, 20.0}};
  region.push_back(outer);
  cam::Loop hole;
  hole.hole = true;
  hole.pts = {{18.0, 12.0}, {18.0, 18.0}, {22.0, 18.0}, {22.0, 12.0}};
  region.push_back(hole);

  cam::ParallelRequest req;
  req.stepover_mm = 2.0;
  req.sampleStep_mm = 0.5;
  req.floorZ = -10.0;

  const cam::SurfacePaths p = cam::ParallelPasses(cl, region, req);
  INFO(p.why);
  REQUIRE(p.ok);
  // Six rows, of which the four crossing the hole are split in two.
  CHECK(p.passes.size() > 6);
  // Nothing inside the hole.
  for (const cam::SurfacePath& pass : p.passes)
    for (const glm::dvec3& pt : pass) {
      const bool inHole = pt.x > 18.0 + 1e-6 && pt.x < 22.0 - 1e-6 &&
                          pt.y > 12.0 + 1e-6 && pt.y < 18.0 - 1e-6;
      CHECK_FALSE(inHole);
    }
}

TEST_CASE("parallel passes: what they refuse", "[cam][surface]") {
  cam::CLSurface none;
  cam::ParallelRequest req;
  req.stepover_mm = 1.0;
  const cam::SurfacePaths a = cam::ParallelPasses(none, {}, req);
  CHECK_FALSE(a.ok);
  CHECK_THAT(a.why, Catch::Matchers::ContainsSubstring("cutter-location"));

  auto doc = BlockDoc(40, 30, 20);
  cam::DropRequest drop;
  drop.min = {5.0, 5.0};
  drop.max = {35.0, 25.0};
  drop.res = 0.5;
  drop.floorZ = -10.0;
  const cam::CLSurface cl = cam::DropCut(doc->GetBodies(), FlatMill(6.0), drop);
  REQUIRE(cl.ok);

  cam::ParallelRequest zero;
  zero.stepover_mm = 0.0;
  const cam::SurfacePaths b = cam::ParallelPasses(cl, {}, zero);
  CHECK_FALSE(b.ok);
  CHECK_THAT(b.why, Catch::Matchers::ContainsSubstring("never advance"));
}

TEST_CASE("waterline: laps a cone at the radius the arithmetic says",
          "[cam][surface]") {
  // A truncated cone is the shape whose waterline is checkable by hand. With
  // a FLAT mill the CL surface is the cone offset outward by the tool radius,
  // so the lap at height z encloses R(z) + r where R(z) runs linearly from
  // R0 at the base to R1 at the top. Anything that quantised the contour to
  // the grid, or forgot the tool, lands somewhere else. Truncated rather than
  // pointed because a zero top radius is a degenerate solid.
  const double R0 = 10.0;
  const double R1 = 2.0;
  const double H = 20.0;
  const double r = 1.5;
  Body body;
  body.name = "Cone";
  body.shape = CADKernel::Instance().CreateCone(
      glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), R0, R1, H);
  REQUIRE(body.shape.IsValid());
  std::vector<Body> bodies;
  bodies.push_back(std::move(body));

  cam::DropRequest drop;
  drop.min = {-14.0, -14.0};
  drop.max = {14.0, 14.0};
  drop.res = 0.15;
  drop.floorZ = -5.0;
  const cam::CLSurface cl = cam::DropCut(bodies, FlatMill(2.0 * r), drop);
  INFO(cl.why);
  REQUIRE(cl.ok);

  cam::WaterlineRequest req;
  req.topZ = 18.0;
  req.bottomZ = 4.0;
  req.zStep_mm = 2.0;
  req.sampleStep_mm = 0.5;
  req.floorZ = -5.0;

  const cam::SurfacePaths p = cam::WaterlinePasses(cl, {}, req);
  INFO(p.why);
  REQUIRE(p.ok);
  REQUIRE(p.passes.size() >= 5);

  // Every point of a lap is at that lap's own Z -- that is what "constant Z"
  // means, and it is why a waterline lap may still fit an arc in the post
  // while a raster pass may not.
  for (const cam::SurfacePath& lap : p.passes) {
    const double z = lap.front().z;
    for (const glm::dvec3& pt : lap)
      CHECK_THAT(pt.z, WithinAbs(z, 1e-9));
  }

  // The radius of each lap, against the cone.
  for (const cam::SurfacePath& lap : p.passes) {
    const double z = lap.front().z;
    if (z < 5.0 || z > 17.0)
      continue; // skip the ends, where the tessellated tip is coarse
    double sum = 0.0;
    for (const glm::dvec3& pt : lap)
      sum += std::hypot(pt.x, pt.y);
    const double got = sum / lap.size();
    const double want = R0 + (R1 - R0) * (z / H) + r;
    INFO("lap at z = " << z << ", mean radius " << got << ", expected "
                       << want);
    CHECK_THAT(got, WithinAbs(want, 0.25));
  }
}

TEST_CASE("waterline: what it refuses", "[cam][surface]") {
  auto doc = BlockDoc(40, 30, 20);
  cam::DropRequest drop;
  drop.min = {5.0, 5.0};
  drop.max = {35.0, 25.0};
  drop.res = 0.5;
  drop.floorZ = -10.0;
  const cam::CLSurface cl = cam::DropCut(doc->GetBodies(), FlatMill(6.0), drop);
  REQUIRE(cl.ok);

  cam::WaterlineRequest zero;
  zero.zStep_mm = 0.0;
  zero.topZ = 20.0;
  zero.bottomZ = 0.0;
  const cam::SurfacePaths a = cam::WaterlinePasses(cl, {}, zero);
  CHECK_FALSE(a.ok);
  CHECK_THAT(a.why, Catch::Matchers::ContainsSubstring("never descend"));

  cam::WaterlineRequest inverted;
  inverted.zStep_mm = 1.0;
  inverted.topZ = 0.0;
  inverted.bottomZ = 20.0;
  const cam::SurfacePaths b = cam::WaterlinePasses(cl, {}, inverted);
  CHECK_FALSE(b.ok);
  CHECK_THAT(b.why, Catch::Matchers::ContainsSubstring("no depth"));

  // A flat top has no wall rising through the band, so a waterline has
  // nothing to lap -- and says so, naming the strategy that does suit it.
  cam::WaterlineRequest flat;
  flat.zStep_mm = 1.0;
  flat.topZ = 19.0;
  flat.bottomZ = 15.0;
  flat.floorZ = -10.0;
  const cam::SurfacePaths c = cam::WaterlinePasses(cl, {}, flat);
  CHECK_FALSE(c.ok);
  CHECK_THAT(c.why, Catch::Matchers::ContainsSubstring("raster"));
}

TEST_CASE("surfacing closes the loop: generate, post, verify a dome",
          "[cam][surface][slow]") {
  // The check that matters, and the one the whole workstream is for: the
  // toolpath goes out through the REAL post and comes back through the REAL
  // verifier, which carves a height map with the tool's own profile. A bug in
  // the drop cutter, the pattern, the linking or the post is invisible to
  // anything that inspects a vector<Move>, and shows up here as a gouge.
  //
  // A sphere, because a curved surface is precisely what the 2.5D pipeline
  // cannot finish -- and because a ball nose on a sphere is where a flat-tool
  // CL surface would be wrong everywhere but the apex.
  const double R = 18.0;
  Body body;
  body.name = "Dome";
  body.shape = CADKernel::Instance().CreateSphere(glm::vec3(0.0f), R);
  REQUIRE(body.shape.IsValid());
  // Cut it in half so the dome sits on the bed rather than floating.
  const CADShape below = CADKernel::Instance().CreateBox(
      glm::vec3(-R - 1.0f, -R - 1.0f, -R - 1.0f),
      glm::vec3(R + 1.0f, R + 1.0f, 0.0f));
  REQUIRE(below.IsValid());
  body.shape = body.shape.Difference(below);
  REQUIRE(body.shape.IsValid());

  std::vector<Body> bodies;
  bodies.push_back(std::move(body));

  cam::Setup setup = BasicSetup();
  setup.safeZ = R + 6.0;
  setup.tools[0] = BallNose(6.0);
  setup.tools[0].number = 1;
  setup.tools[0].feedCut = 600.0;
  setup.tools[0].feedPlunge = 200.0;
  setup.tools[0].spindleRpm = 12000.0;
  setup.stock.margin = {1.0, 1.0, 0.0};
  setup.stock.bottomMargin = 0.0;

  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::SurfaceParallel;
  op.toolNumber = 1;
  op.topZ = R;
  op.bottomZ = 0.0;
  op.stepover = 0.12;  // a fine finish: 0.72 mm across with a 6 mm ball
  op.sampleStep = 0.4; // and the CL grid to match
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths = cam::GenerateAll(setup, bodies);
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  REQUIRE(paths[0].moves.size() > 100);

  std::string gcode, error;
  INFO(error);
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  REQUIRE(gcode.find("G1") != std::string::npos);

  const cam::Verdict v = cam::VerifyGcode(gcode, setup, bodies, /*res=*/0.5);
  INFO("gouge " << v.maxGouge << " at (" << v.gougeAt.x << ", " << v.gougeAt.y
                << "), leftover " << v.maxLeftover << ", rapids through stock "
                << v.rapidsThroughStock << ", outside travel "
                << v.outsideTravel);
  REQUIRE(v.cells > 0);

  // THE assertion: a finishing pass does not cut into the designed surface.
  // Zero, not merely within tolerance -- the drop cutter's whole job is that
  // the tool never goes below the part, and this is the only check that sees
  // the real posted file rather than a vector<Move>.
  CHECK_THAT(v.maxGouge, WithinAbs(0.0, 1e-9));
  CHECK(v.outsideTravel == 0);

  // `rapidsThroughStock` is NOT asserted zero, and the reason is a fact about
  // the JOB rather than about the toolpath. This job finishes raw stock: no
  // roughing pass has run, so every column the cutter has not yet swept is
  // still 18 mm of material. Retracting at the end of a pass lifts a 6 mm
  // ball whose rim overhangs the neighbouring, uncut passes, and the verifier
  // is right to say so. A real job roughs first -- the plan says as much, and
  // Adaptive and Pocket already do it -- and the leftover figure below says
  // the same thing from the other side.
  INFO("leftover " << v.maxLeftover << " over " << v.leftoverArea << " mm2");
  CHECK(v.maxLeftover > 1.0); // the bulk nobody roughed away
}

TEST_CASE("surfacing: a finer stepover leaves a smaller scallop",
          "[cam][surface][slow]") {
  // The one property of a finishing pass no other test pins. A ball of
  // radius r stepping over by s leaves a ridge of about s^2/(8r) BETWEEN the
  // passes, so a finer step must leave less of it -- and a pass that merely
  // drew more lines without following the surface any closer would sail
  // through every other check in this file.
  //
  // Measured through the real verifier, because the scallop is not in the
  // cutter-location surface: that surface is smooth, and the ridge is
  // material standing between two positions of a round tool. Only carving
  // the stock shows it. The grid is 0.05 mm, fine enough to see a cusp that
  // the 0.25 mm default would round away entirely.
  const double R = 30.0;
  Body body;
  body.name = "Dome";
  body.shape = CADKernel::Instance().CreateSphere(glm::vec3(0.0f), R);
  REQUIRE(body.shape.IsValid());
  const CADShape below = CADKernel::Instance().CreateBox(
      glm::vec3(-R - 1.0f, -R - 1.0f, -R - 1.0f),
      glm::vec3(R + 1.0f, R + 1.0f, 0.0f));
  body.shape = body.shape.Difference(below);
  REQUIRE(body.shape.IsValid());
  std::vector<Body> bodies;
  bodies.push_back(std::move(body));

  const auto scallopFor = [&](double stepoverFrac) {
    cam::Setup setup = BasicSetup();
    setup.safeZ = R + 6.0;
    setup.tools[0] = BallNose(6.0);
    setup.tools[0].number = 1;
    setup.tools[0].feedCut = 600.0;
    setup.tools[0].feedPlunge = 200.0;
    setup.stock.margin = {0.0, 0.0, 0.0};
    setup.stock.bottomMargin = 0.0;
    cam::Operation op;
    op.id = 1;
    op.type = cam::OpType::SurfaceParallel;
    op.toolNumber = 1;
    op.topZ = R;
    op.bottomZ = 0.0;
    op.stepover = stepoverFrac;
    op.sampleStep = 0.2;
    setup.operations.push_back(op);

    const std::vector<cam::Toolpath> paths = cam::GenerateAll(setup, bodies);
    REQUIRE(paths.size() == 1);
    INFO(paths[0].error);
    REQUIRE(paths[0].error.empty());
    std::string gcode, error;
    REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
    const cam::Verdict v = cam::VerifyGcode(gcode, setup, bodies, /*res=*/0.05);
    REQUIRE(v.cells > 0);
    return v.maxLeftover;
  };

  const double coarse = scallopFor(0.40); // 2.4 mm step
  const double fine = scallopFor(0.10);   // 0.6 mm step
  INFO("scallop at a 2.4 mm step: " << coarse << " mm,  at 0.6 mm: " << fine);
  // s^2/(8r) on a 6 mm ball: 2.4 -> 0.24 mm, 0.6 -> 0.015 mm. The RATIO is
  // the assertion -- the absolute figures carry the grid's own 0.05 mm and
  // the dome's curvature on top of the cusp.
  CHECK(coarse > fine);
  CHECK(coarse - fine > 0.05);
}

#endif // HAVE_CAM
