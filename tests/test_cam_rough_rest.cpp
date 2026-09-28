#ifdef HAVE_CAM

// Rough, finish, clean up
// (docs/cam/operations.md#rough-a-surface-and-rest-machining): a 3D part
// roughed in layers, a smaller ball re-cutting only what a larger one could not
// reach -- over the bed and on the rotary -- and a rotary pass bounded along
// the axis by a picked face. Every generator case goes out through the real
// post and comes back through the real verifier, which shares nothing with the
// generators but the tool's shape.

#include "cad/CADKernel.h"
#include "cam_fixtures.h"
#include "core/cam/CamPicks.h"
#include "core/cam/CamSurface.h"

#include <catch2/generators/catch_generators.hpp>
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

// How far each plunge of a toolpath descends, feeding: what an entry costs.
std::vector<double> Plunges(const cam::Toolpath& tp) {
  std::vector<double> out;
  for (size_t k = 1; k < tp.moves.size(); k++)
    if (tp.moves[k].kind == cam::Move::Kind::Plunge)
      out.push_back(glm::length(tp.moves[k].to - tp.moves[k - 1].to));
  return out;
}

// What the same plunges would cost fed the whole way from `from` -- the
// stock top, or the blank's radius on the rotary (z is the radius there).
double FedFrom(const cam::Toolpath& tp, double from) {
  double s = 0.0;
  for (const cam::Move& m : tp.moves)
    if (m.kind == cam::Move::Kind::Plunge)
      s += std::max(0.0, from - m.to.z);
  return s;
}

double Sum(const std::vector<double>& v) {
  double s = 0.0;
  for (double x : v)
    s += x;
  return s;
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
  RotaryMachine(setup);
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
  RotaryMachine(setup);
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

TEST_CASE("cam entries: a rotary rest pass comes down just above what its "
          "finish left",
          "[cam][slow]") {
  // docs/cam/operations.md#surface-entries. The rotary sampler's rest pass
  // spent 26 of its 58 minutes plunging from the safe radius at the plunge
  // feed, through air its finish had cut.
  auto doc = NeckedShaftDoc(); // Phi 30, a Phi 22 journal from x 12 to 28
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
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
  cam::Operation c = cam::DeriveRest(setup, doc->GetBodies(), 2).op;
  c.id = 2;
  c.stepover = 0.25;
  setup.operations.push_back(c);

  const auto after = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(after.size() == 2);
  REQUIRE(after[1].error.empty());
  REQUIRE(cam::FinishedBefore(setup, setup.operations[1]) != nullptr);

  // The same rest op cut FIRST knows nothing of what is left, and comes down
  // from the blank.
  cam::Setup first = setup;
  std::swap(first.operations[0], first.operations[1]);
  REQUIRE(cam::FinishedBefore(first, first.operations[0]) == nullptr);
  const auto alone = cam::GenerateAll(first, doc->GetBodies());
  REQUIRE(alone.size() == 2);
  REQUIRE(alone[0].error.empty());
  const double fed = Sum(Plunges(after[1]));
  const double fedFirst = Sum(Plunges(alone[0]));
  INFO("rest plunges " << fed << " mm after its finish, " << fedFirst
                       << " mm before it");
  CHECK(fed < 0.6 * fedFirst);
  // Lowered, not reckless: the whole job still rapids through no stock.
  const cam::Verdict v = Verify(setup, doc->GetBodies(), after, 0.5);
  INFO("rapids through stock " << v.rapidsThroughStock << " worst "
                               << v.worstRapidDepth);
  CHECK(v.rapidsThroughStock == 0);
  CHECK(v.maxGouge < 0.1);
}

TEST_CASE("cam entries: a layered rough enters each layer from just over "
          "the last",
          "[cam][slow]") {
  SECTION("on the rotary") {
    // A flat end too: its entry beside a shoulder is where the floor alone
    // was not enough -- the cell at the wall's foot is never under a tool
    // centre (the rotary sampler's rough).
    const cam::ToolType type =
        GENERATE(cam::ToolType::Ball, cam::ToolType::Flat);
    auto doc = NeckedShaftDoc();
    cam::Setup setup = BasicSetup();
    RotaryMachine(setup);
    setup.stock.margin = {0.0, 2.0, 2.0};
    setup.safeZ = 30.0;
    setup.tools[0] = ShapedTool(type, 6.0);
    cam::Operation f = cam::DeriveOperation(setup, doc->GetBodies(),
                                            cam::OpType::SurfaceRotary, {}, 1)
                           .op;
    f.id = 1;
    f.stepover = 0.25;
    f.stepdown = 1.5;
    setup.operations.push_back(f);
    const auto paths = cam::GenerateAll(setup, doc->GetBodies());
    REQUIRE(paths.size() == 1);
    REQUIRE(paths[0].error.empty());
    // A layer's floor is a stepdown under the one before; the entry is a
    // ridge and a clearance over that.
    // Fed from just over the layer before, not from the blank: beside a
    // shoulder an entry stays at the wall's height, so not every one is a
    // layer's descent, but together they are a fraction of the blank's.
    const double fed = Sum(Plunges(paths[0]));
    const double fromBlank =
        FedFrom(paths[0], cam::RotaryBlankRadius(setup, 15.0) + 1.0);
    INFO("fed " << fed << " mm of plunges; from the blank " << fromBlank);
    CHECK(fed < 0.7 * fromBlank);
    const cam::Verdict v = Verify(setup, doc->GetBodies(), paths, 0.5);
    INFO("rapids through stock " << v.rapidsThroughStock << " worst "
                                 << v.worstRapidDepth);
    CHECK(v.rapidsThroughStock == 0);
    CHECK(v.maxGouge < 0.1);
  }
  SECTION("over the bed") {
    const std::vector<Body> bodies = DomeOnPlate();
    cam::Setup setup = TwoBallSetup();
    cam::Operation r = cam::DeriveSurfaceRough(setup, bodies, 1).op;
    r.id = 1;
    r.sampleStep = 0.4;
    setup.operations.push_back(r);
    const auto paths = cam::GenerateAll(setup, bodies);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths[0].error.empty());
    glm::dvec3 lo(0.0), hi(0.0);
    REQUIRE(cam::StockBounds(setup, bodies, lo, hi));
    const double fed = Sum(Plunges(paths[0]));
    const double fromTop = FedFrom(paths[0], hi.z + 1.0);
    INFO("fed " << fed << " mm of plunges; from the stock top " << fromTop);
    CHECK(fed < 0.7 * fromTop);
    const cam::Verdict v = Verify(setup, bodies, paths, 0.5);
    INFO("rapids through stock " << v.rapidsThroughStock << " worst "
                                 << v.worstRapidDepth);
    CHECK(v.rapidsThroughStock == 0);
    CHECK(v.maxGouge < 0.05);
  }
}

TEST_CASE("cam rotary: passes run past a domed end and finish its foot",
          "[cam][slow]") {
  // docs/cam/rotary.md#limits used to say passes stop at the part's ends:
  // the grid was the part's own extent, so no ball centred past the end and
  // the steep foot of a dome was never cut. The chuck and the tailstock clip
  // what would come near them; a free end has neither.
  CADKernel& k = CADKernel::Instance();
  Body body;
  body.name = "Domed bar";
  body.shape =
      k.CreateCylinder(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0, 30.0)
          .Union(k.CreateSphere(glm::vec3(30.0f, 0.0f, 0.0f), 10.0));
  std::vector<Body> bodies;
  bodies.push_back(std::move(body));
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.stock.margin = {2.0, 1.0, 1.0};
  setup.safeZ = 30.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  cam::Operation f =
      cam::DeriveOperation(setup, bodies, cam::OpType::SurfaceRotary, {}, 1).op;
  f.id = 1;
  f.stepover = 0.2;
  setup.operations.push_back(f);
  const auto paths = cam::GenerateAll(setup, bodies);
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0].error.empty());
  double maxX = -1e18;
  for (const cam::Move& m : paths[0].moves)
    if (m.kind == cam::Move::Kind::Cut)
      maxX = std::max(maxX, m.to.x);
  const cam::Verdict v = Verify(setup, bodies, paths, 0.5);
  INFO("last cut at x " << maxX << "; dome leftover " << v.leftoverSurfaceMax
                        << " at x " << v.leftoverSurfaceAt.x << ", gouge "
                        << v.maxGouge);
  CHECK(maxX > 41.5); // a ball centred well past the apex
  // Stopping at the end left 16 mm2 on the dome's steep cap (the "end
  // face" a rotary verdict excuses) and 0.25 mm on its foot.
  INFO("end face leftover " << v.leftoverEndFaceArea << " mm2");
  CHECK(v.leftoverEndFaceArea < 1.0);
  CHECK(v.leftoverSurfaceMax < 0.15);
  CHECK(v.maxGouge < 0.1);
}

#endif // HAVE_CAM

TEST_CASE("cam waterline: the top of a dome is lapped, not left",
          "[cam][surface]") {
  // Evenly spaced levels spread apart where a surface flattens, and the cap
  // above the highest lap was never cut: 0.6 mm left on the sampler part's
  // dome. Levels are added where the band between two -- or over the cap --
  // is wider than the finish's own cusp allows.
  const std::vector<Body> bodies = DomeOnPlate(12.0);
  cam::Setup setup = TwoBallSetup();
  const cam::DerivedOp d =
      cam::DeriveOperation(setup, bodies, cam::OpType::SurfaceWaterline, {}, 1);
  INFO(d.why);
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(setup, bodies);
  REQUIRE(paths[0].error.empty());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, gcode, &error));
  const cam::Inspection insp = cam::Inspect(gcode, setup, bodies, 0.1);
  REQUIRE(insp.ok);
  // The apex, at (0, 0), and the cells round it within 1.5 mm.
  double worst = 0.0;
  for (int j = 0; j < insp.machined.ny; j++)
    for (int i = 0; i < insp.machined.nx; i++)
      if (std::hypot(insp.machined.CellX(i), insp.machined.CellY(j)) < 1.5)
        worst = std::max(worst, static_cast<double>(insp.machined.At(i, j) -
                                                    insp.designed.At(i, j)));
  INFO("left at the apex " << worst);
  CHECK(worst < 0.1);
}
