#ifdef HAVE_CAM

// Rotary passes that FOLLOW a twist (docs/cam/rotary-helical-plan.md):
// helices at the part's own lead, stepped round one from the next, and the
// twist they follow measured from the surface rather than typed.

#include "cam_fixtures.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamSim.h"
#include "core/cam/CamSurface.h"

namespace {

// A six-start twist: r = 18 + cos(6 (theta - rate x)), rate in degrees per mm.
// What a barley twist's cutter-location surface looks like, without building
// one -- 20 mm radius grid, x from 0 to 100.
cam::RadiusMap Twisted(double rateDegPerMm, double amplitude = 1.0) {
  cam::RadiusMap m = cam::MakeRotaryStock(0.0, 100.0, 20.0, 0.5);
  for (int j = 0; j < m.nTheta; j++)
    for (int i = 0; i < m.nx; i++) {
      const double th =
          m.CellTheta(j) - glm::radians(rateDegPerMm) * m.CellX(i);
      m.Set(i, j, static_cast<float>(18.0 + amplitude * std::cos(6.0 * th)));
    }
  return m;
}

cam::Setup ShaftSetup() {
  cam::Setup setup = BasicSetup();
  setup.machine.hasRotaryAxis = true;
  setup.stock.margin = {0.0, 1.0, 1.0};
  setup.safeZ = 30.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  return setup;
}

cam::Operation RotaryOp(uint64_t id, cam::RotaryPattern pattern,
                        double lead = 0.0) {
  cam::Operation op;
  op.id = id;
  op.type = cam::OpType::SurfaceRotary;
  op.toolNumber = 1;
  op.stepover = 0.5;
  op.rotaryPattern = pattern;
  op.leadMm = lead;
  return op;
}

} // namespace

TEST_CASE("cam helical: the twist is measured from the surface", "[cam]") {
  // The pin's own number: a 45-degree lead at radius 25 is 360 / (2 pi 25).
  const double pin = 360.0 / (2.0 * glm::pi<double>() * 25.0);
  for (const double rate : {pin, -pin, 0.8, -4.0}) {
    const double got = cam::DeriveRotaryTwist(Twisted(rate));
    INFO("asked " << rate << " deg/mm, measured " << got);
    CHECK_THAT(got, WithinRel(rate, 0.02));
  }
  // A body of revolution has no twist to find -- not a small one, NONE.
  cam::RadiusMap round = cam::MakeRotaryStock(0.0, 100.0, 20.0, 0.5);
  for (int j = 0; j < round.nTheta; j++)
    for (int i = 0; i < round.nx; i++)
      round.Set(i, j, static_cast<float>(15.0 + 0.05 * round.CellX(i)));
  CHECK(cam::DeriveRotaryTwist(round) == 0.0);
}

TEST_CASE("cam helical: every pass lies on its helix, spaced across itself",
          "[cam]") {
  const double lead = 157.08;
  cam::RadiusMap round = cam::MakeRotaryStock(0.0, 100.0, 15.0, 0.5);
  cam::RotaryHelicalRequest req;
  req.lead_mm = lead;
  req.stepover_mm = 2.0;
  const auto passes = cam::RotaryHelicalPasses(round, req);
  REQUIRE(passes.size() > 10);

  const double rate = 360.0 / lead;
  const double xLo = round.CellX(0);
  std::vector<double> theta0;
  for (const auto& pass : passes) {
    REQUIRE(pass.size() >= 2);
    const double t0 = pass.front().thetaDeg - rate * (pass.front().x - xLo);
    for (const cam::RotaryPathPoint& q : pass)
      REQUIRE_THAT(q.thetaDeg - rate * (q.x - xLo), WithinAbs(t0, 1e-9));
    theta0.push_back(t0);
  }
  // Spaced as asked PERPENDICULAR to the passes at the widest radius, which
  // on a round bar (nothing steep across them) is the base step exactly.
  const double arcRate = 15.0 * 2.0 * glm::pi<double>() / lead;
  const double cosPhi = 1.0 / std::sqrt(1.0 + arcRate * arcRate);
  const double base = glm::degrees(2.0 / (15.0 * cosPhi));
  CHECK_THAT(theta0[1] - theta0[0], WithinAbs(base, 1e-9));
  CHECK(theta0.size() == static_cast<size_t>(std::ceil(360.0 / base)));
  // Zigzag: alternate passes run back.
  CHECK(passes[0].front().x < passes[0].back().x);
  CHECK(passes[1].front().x > passes[1].back().x);

  // Following a TWIST, the passes crowd where the flute walls are steep
  // across them -- and a round bar with the same stepover does not.
  const auto twisted = cam::RotaryHelicalPasses(Twisted(rate, 2.0), req);
  INFO(passes.size() << " passes on a round bar, " << twisted.size()
                     << " on the twist");
  CHECK(twisted.size() > passes.size());
}

TEST_CASE("cam helical: a part with no twist is cut as parallel passes, "
          "and says so",
          "[cam]") {
  auto doc = NeckedShaftDoc();
  cam::Setup parallel = ShaftSetup();
  parallel.operations.push_back(RotaryOp(1, cam::RotaryPattern::Parallel));
  cam::Setup helical = ShaftSetup();
  helical.operations.push_back(RotaryOp(1, cam::RotaryPattern::Helical));
  const auto p = cam::GenerateAll(parallel, doc->GetBodies());
  const auto h = cam::GenerateAll(helical, doc->GetBodies());
  REQUIRE(p[0].error.empty());
  REQUIRE(h[0].error.empty());
  INFO(h[0].strategy);
  CHECK(h[0].strategy.find("parallel passes") != std::string::npos);
  REQUIRE(h[0].moves.size() == p[0].moves.size());
  for (size_t k = 0; k < p[0].moves.size(); k++) {
    REQUIRE(h[0].moves[k].to == p[0].moves[k].to);
    REQUIRE(h[0].moves[k].a == p[0].moves[k].a);
  }
}

TEST_CASE("cam helical: a stated lead turns while it cuts, and verifies "
          "clean",
          "[cam]") {
  auto doc = NeckedShaftDoc();
  cam::Setup setup = ShaftSetup();
  setup.operations.push_back(RotaryOp(1, cam::RotaryPattern::Helical, 60.0));
  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths[0].error.empty());
  CHECK(paths[0].strategy.empty()); // it did what it was asked
  size_t turningCuts = 0;
  for (size_t i = 1; i < paths[0].moves.size(); i++)
    if (paths[0].moves[i].kind == cam::Move::Kind::Cut &&
        std::abs(paths[0].moves[i].a - paths[0].moves[i - 1].a) > 1e-9)
      turningCuts++;
  CHECK(turningCuts > 50);

  std::string gcode;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.5);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover << " p95 "
                << v.leftoverP95);
  REQUIRE(v.rotary);
  CHECK(v.maxGouge < 0.1);
  CHECK(v.rapidsThroughStock == 0);
  // The typical scallop is a fraction of the worst cell, which is the
  // shoulder corner a 6 mm ball cannot reach.
  CHECK(v.leftoverP95 > 0.0);
  CHECK(v.leftoverP95 < v.maxLeftover);

  // One engine: the simulator's last frame is the verifier's stock.
  constexpr double kRes = 0.5;
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ProgramInSetupSpace(gcode, setup), 0.0);
  REQUIRE(tl.rotary);
  cam::RotaryStockSim sim;
  sim.Begin(tl, setup,
            cam::MakeRotaryBlank(
                setup, cam::BodiesInSetupFrame(setup, doc->GetBodies()), kRes));
  REQUIRE(sim.Ready());
  sim.SeekTo(tl.End());
  const cam::RotaryInspection ri =
      cam::InspectRotary(gcode, setup, doc->GetBodies(), kRes);
  REQUIRE(ri.ok);
  REQUIRE(ri.machined.r.size() == sim.Stock().r.size());
  size_t differ = 0;
  for (size_t k = 0; k < ri.machined.r.size(); k++)
    if (ri.machined.r[k] != sim.Stock().r[k])
      differ++;
  CHECK(differ == 0);
}

#endif // HAVE_CAM
