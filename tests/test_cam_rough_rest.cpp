#ifdef HAVE_CAM

// Rough, finish, clean up (docs/cam/rough-rest-range-plan.md): a 3D part
// roughed in layers, a smaller ball re-cutting only what a larger one could
// not reach -- over the bed and on the rotary -- and a rotary pass bounded
// along the axis by a picked face. Every generator case goes out through the
// real post and comes back through the real verifier, which shares nothing
// with the generators but the tool's shape.

#include "cad/CADKernel.h"
#include "cam_fixtures.h"
#include "core/cam/CamPicks.h"

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <set>

using Catch::Matchers::WithinAbs;

namespace {

// A dome on a plate: the concave ring where they meet is the corner a 6 mm
// ball cannot get into and a 2 mm one can.
std::vector<Body> DomeOnPlate(double R = 12.0) {
  CADKernel& k = CADKernel::Instance();
  CADShape plate = k.CreateBox(glm::vec3(-25.0f, -25.0f, 0.0f),
                               glm::vec3(25.0f, 25.0f, 4.0f));
  CADShape dome =
      k.CreateSphere(glm::vec3(0.0f, 0.0f, 4.0f), static_cast<float>(R));
  const CADShape below = k.CreateBox(glm::vec3(-R - 1.0f, -R - 1.0f, -R),
                                     glm::vec3(R + 1.0f, R + 1.0f, 4.0f));
  dome = dome.Difference(below);
  Body body;
  body.name = "Dome on a plate";
  body.shape = plate.Union(dome);
  std::vector<Body> bodies;
  bodies.push_back(std::move(body));
  return bodies;
}

cam::Setup TwoBallSetup() {
  cam::Setup setup = BasicSetup();
  setup.safeZ = 30.0;
  setup.stock.margin = {1.0, 1.0, 2.0};
  setup.stock.bottomMargin = 0.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  setup.tools[0].number = 1;
  cam::Tool small = ShapedTool(cam::ToolType::Ball, 2.0);
  small.number = 2;
  setup.tools.push_back(small);
  return setup;
}

cam::Verdict Verify(const cam::Setup& setup, const std::vector<Body>& bodies,
                    const std::vector<cam::Toolpath>& paths, double res) {
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, gcode, &error));
  return cam::VerifyGcode(gcode, setup, bodies, res);
}

} // namespace

TEST_CASE("cam rough: a dome roughed in layers leaves its skin and finishes "
          "clean",
          "[cam][surface][slow]") {
  const std::vector<Body> bodies = DomeOnPlate();
  cam::Setup setup = TwoBallSetup();

  const cam::DerivedOp rough = cam::DeriveSurfaceRough(setup, bodies, 1);
  INFO(rough.why);
  REQUIRE(rough.ok);
  CHECK(rough.op.stepdown == 3.0);
  CHECK(std::abs(rough.op.stockToLeave - 0.6) < 1e-12);
  cam::Operation r = rough.op;
  r.id = 1;
  r.sampleStep = 0.4;
  setup.operations.push_back(r);

  const std::vector<cam::Toolpath> roughed = cam::GenerateAll(setup, bodies);
  REQUIRE(roughed.size() == 1);
  INFO(roughed[0].error);
  REQUIRE(roughed[0].error.empty());
  // Layers: cutting at several distinct depths on the way down.
  std::set<long> depths;
  for (const cam::Move& m : roughed[0].moves)
    if (m.kind == cam::Move::Kind::Cut)
      depths.insert(std::lround(m.to.z));
  CHECK(depths.size() >= 4);

  const cam::Verdict afterRough = Verify(setup, bodies, roughed, 0.5);
  INFO("rough: gouge " << afterRough.maxGouge << " leftover "
                       << afterRough.maxLeftover);
  CHECK(afterRough.maxGouge < 0.05);
  CHECK(afterRough.maxLeftover > 0.5); // the skin, for the finish

  // Then the finish takes the skin.
  cam::DerivedOp finish =
      cam::DeriveOperation(setup, bodies, cam::OpType::SurfaceParallel, {}, 1);
  REQUIRE(finish.ok);
  cam::Operation f = finish.op;
  f.id = 2;
  f.stepover = 0.15;
  f.sampleStep = 0.4;
  setup.operations.push_back(f);
  const std::vector<cam::Toolpath> both = cam::GenerateAll(setup, bodies);
  REQUIRE(both.size() == 2);
  REQUIRE(both[1].error.empty());
  const cam::Verdict done = Verify(setup, bodies, both, 0.5);
  INFO("rough + finish: gouge " << done.maxGouge << " leftover "
                                << done.maxLeftover << " rapids through stock "
                                << done.rapidsThroughStock);
  CHECK(done.maxGouge < 0.05);
  // The finish on raw stock left more than a millimetre everywhere it had
  // not yet been (test_cam_surface.cpp); after a rough, what remains is the
  // corner a 6 mm ball cannot reach, which is the clean-up's job.
  CHECK(done.maxLeftover < afterRough.maxLeftover);
}

TEST_CASE("cam rest: a smaller ball re-cuts only the corner the larger left",
          "[cam][surface][slow]") {
  const std::vector<Body> bodies = DomeOnPlate(12.0);
  cam::Setup setup = TwoBallSetup();
  // The stock ends at the plate's edges: a 1 mm margin beside the plate's
  // vertical walls is a strip no ball fits, stock BESIDE the part rather than
  // a corner of it, and it would be the largest leftover in the job.
  setup.stock.margin = {0.0, 0.0, 2.0};

  // Refused before there is a finish to follow, and with a tool no smaller.
  CHECK_FALSE(cam::DeriveRest(setup, bodies, 2).ok);

  cam::Operation r = cam::DeriveSurfaceRough(setup, bodies, 1).op;
  r.id = 1;
  r.sampleStep = 0.4;
  setup.operations.push_back(r);
  cam::Operation f =
      cam::DeriveOperation(setup, bodies, cam::OpType::SurfaceParallel, {}, 1)
          .op;
  f.id = 2;
  f.stepover = 0.15;
  f.sampleStep = 0.4;
  setup.operations.push_back(f);

  const cam::DerivedOp same = cam::DeriveRest(setup, bodies, 1);
  CHECK_FALSE(same.ok);
  CHECK(same.why.find("not smaller") != std::string::npos);

  const cam::DerivedOp rest = cam::DeriveRest(setup, bodies, 2);
  INFO(rest.why);
  REQUIRE(rest.ok);
  CHECK(rest.op.restFromTool == 1);
  CHECK(rest.op.toolNumber == 2);
  CHECK(rest.op.type == cam::OpType::SurfaceParallel);
  cam::Operation c = rest.op;
  c.id = 3;
  c.stepover = 0.2;
  c.sampleStep = 0.2;

  const std::vector<cam::Toolpath> before = cam::GenerateAll(setup, bodies);
  setup.operations.push_back(c);
  const std::vector<cam::Toolpath> after = cam::GenerateAll(setup, bodies);
  REQUIRE(after.size() == 3);
  INFO(after[2].error);
  REQUIRE(after[2].error.empty());

  // Only near the ring where the dome meets the plate (r = 12), and only
  // down at the plate: a corner, not the dome all over again.
  double cutLength = 0.0, finishLength = 0.0;
  size_t cuts = 0;
  for (size_t k = 1; k < after[2].moves.size(); k++) {
    const cam::Move& m = after[2].moves[k];
    if (m.kind != cam::Move::Kind::Cut)
      continue;
    cuts++;
    cutLength += glm::length(m.to - after[2].moves[k - 1].to);
    const double rho = std::hypot(m.to.x, m.to.y);
    INFO("cut at r " << rho << " z " << m.to.z);
    CHECK(rho > 9.5);
    CHECK(rho < 17.0);
  }
  for (size_t k = 1; k < after[1].moves.size(); k++)
    if (after[1].moves[k].kind == cam::Move::Kind::Cut)
      finishLength +=
          glm::length(after[1].moves[k].to - after[1].moves[k - 1].to);
  CHECK(cuts > 20);
  INFO("rest " << cutLength << " mm against the finish's " << finishLength);
  // Not a small number by construction: the corner band is about a quarter
  // of the part's area, cut at less than half the finish's stepover, so the
  // clean-up runs ~0.56 of the finish's length. The band check above is what
  // says it stayed in the corner; this says it did not re-cut the part.
  CHECK(cutLength < 0.75 * finishLength);

  const cam::Verdict v0 = Verify(setup, bodies, before, 0.25);
  const cam::Verdict v1 = Verify(setup, bodies, after, 0.25);
  INFO("leftover " << v0.maxLeftover << " -> " << v1.maxLeftover << ", gouge "
                   << v0.maxGouge << " -> " << v1.maxGouge << " at ("
                   << v1.gougeAt.x << ", " << v1.gougeAt.y << ")");
  CHECK(v1.maxGouge < 0.05);
  CHECK(v1.maxLeftover < v0.maxLeftover - 0.2);
}

TEST_CASE("cam rest: on the rotary only at the shoulders", "[cam][slow]") {
  auto doc = NeckedShaftDoc(); // Phi 30, a Phi 22 journal from x 12 to 28
  cam::Setup setup = BasicSetup();
  setup.machine.hasRotaryAxis = true;
  setup.stock.margin = {0.0, 1.0, 1.0};
  setup.safeZ = 30.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  cam::Tool small = ShapedTool(cam::ToolType::Ball, 2.0);
  small.number = 2;
  setup.tools.push_back(small);

  cam::Operation f = cam::DeriveOperation(setup, doc->GetBodies(),
                                          cam::OpType::SurfaceRotary, {}, 1)
                         .op;
  f.id = 1;
  f.stepover = 0.25;
  setup.operations.push_back(f);
  const cam::DerivedOp rest = cam::DeriveRest(setup, doc->GetBodies(), 2);
  INFO(rest.why);
  REQUIRE(rest.ok);
  CHECK(rest.op.type == cam::OpType::SurfaceRotary);
  cam::Operation c = rest.op;
  c.id = 2;
  c.stepover = 0.25;

  const std::vector<cam::Toolpath> before =
      cam::GenerateAll(setup, doc->GetBodies());
  setup.operations.push_back(c);
  const std::vector<cam::Toolpath> after =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(after.size() == 2);
  INFO(after[1].error);
  REQUIRE(after[1].error.empty());
  size_t cuts = 0;
  for (const cam::Move& m : after[1].moves) {
    if (m.kind != cam::Move::Kind::Cut)
      continue;
    cuts++;
    // Within reach of a shoulder: the 6 mm ball's corner, grown by the 2 mm
    // ball's radius.
    const double toShoulder =
        std::min(std::abs(m.to.x - 12.0), std::abs(m.to.x - 28.0));
    INFO("cut at x " << m.to.x);
    CHECK(toShoulder < 4.5);
  }
  CHECK(cuts > 20);

  const cam::Verdict v0 = Verify(setup, doc->GetBodies(), before, 0.5);
  const cam::Verdict v1 = Verify(setup, doc->GetBodies(), after, 0.5);
  INFO("surface leftover " << v0.leftoverSurfaceMax << " -> "
                           << v1.leftoverSurfaceMax << ", gouge "
                           << v1.maxGouge);
  CHECK(v1.maxGouge < 0.1);
  CHECK(v1.leftoverSurfaceMax < v0.leftoverSurfaceMax - 0.3);
}

TEST_CASE("cam rotary: a picked face bounds the pass along the axis", "[cam]") {
  auto doc = NeckedShaftDoc(); // cylinders: x 0-12, the journal 12-28, 28-40
  cam::Setup setup = BasicSetup();
  setup.machine.hasRotaryAxis = true;
  setup.stock.margin = {0.0, 1.0, 1.0};
  setup.safeZ = 30.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);

  // Each round face in turn: its pass stays within its own length.
  std::vector<std::pair<double, double>> spans;
  for (const Body& b : doc->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (b.shape.FacePlane(index, o, n, u))
        continue;
      cam::Target target;
      target.ref = ElementRef{b.createdBy, name};
      const cam::DerivedOp d = cam::DeriveOperation(
          setup, doc->GetBodies(), cam::OpType::SurfaceRotary, target, 1);
      INFO(d.why);
      REQUIRE(d.ok);
      cam::Operation op = d.op;
      op.id = 1;
      op.stepover = 0.5;
      setup.operations = {op};
      const std::vector<cam::Toolpath> paths =
          cam::GenerateAll(setup, doc->GetBodies());
      REQUIRE(paths.size() == 1);
      INFO(paths[0].error);
      REQUIRE(paths[0].error.empty());
      double x0 = 1e9, x1 = -1e9;
      for (const cam::Move& m : paths[0].moves)
        if (m.kind == cam::Move::Kind::Cut) {
          x0 = std::min(x0, m.to.x);
          x1 = std::max(x1, m.to.x);
        }
      spans.push_back({x0, x1});
    }
  REQUIRE(spans.size() == 3);
  std::sort(spans.begin(), spans.end());
  const double expect[3][2] = {{0.0, 12.0}, {12.0, 28.0}, {28.0, 40.0}};
  for (int s = 0; s < 3; s++) {
    INFO("span " << spans[s].first << " to " << spans[s].second);
    CHECK(spans[s].first > expect[s][0] - 1.0);
    CHECK(spans[s].second < expect[s][1] + 1.0);
    CHECK(spans[s].second - spans[s].first >
          0.5 * (expect[s][1] - expect[s][0]));
  }
}

#endif // HAVE_CAM
