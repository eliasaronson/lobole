#ifdef HAVE_CAM

// Continuous 4-axis: the spiral, the feed a turning cut is posted at, and the
// G92 that keeps a wound axis from unwinding
// (docs/cam/rotary-continuous-plan.md).
//
// The checking side was continuous before the generator was -- the carve,
// the timeline and the simulator all interpolate A along a move -- so the
// end-to-end cases here hold the new generator to the verifier and the
// simulator that already existed, rather than to numbers of its own.

#include "cam_fixtures.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamSim.h"
#include "core/cam/CamSurface.h"
#include "core/io/Serialization.h"

#include <filesystem>
#include <fstream>

namespace {

// A uniform radius map: 15 mm from x = 0 to 40, every cell.
cam::RadiusMap Uniform(double radius = 15.0) {
  return cam::MakeRotaryStock(0.0, 40.0, radius, 0.5);
}

// The part the rotary tests turn: a Phi 30 bar with a Phi 22 journal turned
// into its middle -- two shoulders, one corner a ball cannot reach.
std::unique_ptr<Document> ShaftDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Blank", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 15.0, 40.0));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Waist", glm::vec3(12, 0, 0), glm::vec3(1, 0, 0), 15.0, 16.0));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Necked", doc->GetFeature(0)->GetId(), doc->GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Journal", glm::vec3(12, 0, 0), glm::vec3(1, 0, 0), 11.0, 16.0));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Shaft", doc->GetFeature(2)->GetId(), doc->GetFeature(3)->GetId(),
      BooleanOp::Union));
  return doc;
}

cam::Setup ShaftSetup() {
  cam::Setup setup = BasicSetup();
  setup.machine.hasRotaryAxis = true;
  setup.stock.margin = {0.0, 1.0, 1.0};
  setup.safeZ = 30.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  return setup;
}

cam::Operation RotaryOp(uint64_t id, cam::RotaryPattern pattern) {
  cam::Operation op;
  op.id = id;
  op.type = cam::OpType::SurfaceRotary;
  op.toolNumber = 1;
  op.stepover = 0.5; // 3 mm: coarse, so each case stays quick
  op.rotaryPattern = pattern;
  return op;
}

cam::Move CutAt(double x, double r, double aDeg, double feed) {
  cam::Move m;
  m.kind = cam::Move::Kind::Cut;
  m.to = {x, 0.0, r};
  m.feed = feed;
  m.hasA = true;
  m.a = aDeg;
  return m;
}

// The F on the first line of `gcode` that carries both `word` and an F.
double FeedOnLineWith(const std::string& gcode, const std::string& word) {
  std::istringstream in(gcode);
  std::string line;
  while (std::getline(in, line))
    if (line.find(word) != std::string::npos) {
      const size_t f = line.find(" F");
      if (f != std::string::npos)
        return std::stod(line.substr(f + 2));
    }
  return -1.0;
}

} // namespace

TEST_CASE("cam spiral: one pitch per turn, with a closed ring at each end",
          "[cam]") {
  const cam::RadiusMap map = Uniform();
  cam::RotarySpiralRequest req;
  req.pitch_mm = 2.0;
  req.airRadius = 20.0;
  const std::vector<cam::RotaryPathPoint> path =
      cam::RotarySpiralPath(map, req);
  REQUIRE(path.size() > 100);

  // Unwound: the angle only ever grows, through every revolution.
  for (size_t k = 1; k < path.size(); k++)
    REQUIRE(path[k].thetaDeg > path[k - 1].thetaDeg);

  const double xLo = map.CellX(0), xHi = map.CellX(map.nx - 1);
  // The first full turn stands at the low end and the last at the high end:
  // the rings that close the ends.
  size_t lowRing = 0, highRing = 0;
  for (const cam::RotaryPathPoint& q : path) {
    if (q.thetaDeg < 360.0 - 1e-9) {
      CHECK_THAT(q.x, WithinAbs(xLo, 1e-12));
      lowRing++;
    }
    if (q.thetaDeg > path.back().thetaDeg - 360.0 + 1e-9) {
      CHECK_THAT(q.x, WithinAbs(xHi, 1e-12));
      highRing++;
    }
  }
  CHECK(lowRing >= 16);
  CHECK(highRing >= 16);
  CHECK_THAT(path.back().thetaDeg - path.front().thetaDeg,
             WithinAbs(360.0 * (2.0 + (xHi - xLo) / req.pitch_mm), 1e-6));

  // Between the rings: x advances exactly one pitch per 360 degrees.
  for (const cam::RotaryPathPoint& q : path) {
    if (q.thetaDeg < 360.0 || q.thetaDeg > path.back().thetaDeg - 360.0)
      continue;
    const double turns = (q.thetaDeg - 360.0) / 360.0;
    CHECK_THAT(q.x, WithinAbs(xLo + turns * req.pitch_mm, 1e-9));
    CHECK_THAT(q.r, WithinAbs(15.0, 1e-5)); // the surface, everywhere
  }
}

TEST_CASE("cam spiral: an empty cell is crossed in the air, a floor holds out",
          "[cam]") {
  cam::RadiusMap map = Uniform();
  // A band the tool reaches nothing in, all the way round.
  for (int j = 0; j < map.nTheta; j++)
    for (int i = 30; i < 40; i++)
      map.Set(i, j, 0.0f);
  cam::RotarySpiralRequest req;
  req.pitch_mm = 1.0;
  req.airRadius = 21.0;
  req.floorRadius = 17.0;
  size_t inAir = 0, onFloor = 0;
  for (const cam::RotaryPathPoint& q : cam::RotarySpiralPath(map, req)) {
    // Never at the surface's 15: either lifted to the floor or in the air.
    REQUIRE((q.r == 17.0 || q.r == 21.0));
    (q.r == 21.0 ? inAir : onFloor)++;
    // ...and in the air only over the band (and within a cell of it, where
    // the lookup blends with an empty neighbour).
    if (q.r == 21.0) {
      CHECK(q.x > map.CellX(29) - 1e-9);
      CHECK(q.x < map.CellX(40) + 1e-9);
    }
  }
  CHECK(inAir > 0);
  CHECK(onFloor > inAir);
}

TEST_CASE("cam spiral: decimation stays inside its tolerance", "[cam]") {
  // A surface that changes in both directions, so the thinned helix has
  // something to bend round and the check is not vacuous.
  cam::RadiusMap map = Uniform();
  for (int j = 0; j < map.nTheta; j++)
    for (int i = 0; i < map.nx; i++)
      map.Set(i, j,
              static_cast<float>(14.0 + std::sin(map.CellX(i) * 0.3) +
                                 0.5 * std::cos(3.0 * map.CellTheta(j))));
  cam::RotarySpiralRequest req;
  req.pitch_mm = 1.5;
  req.airRadius = 20.0;
  const std::vector<cam::RotaryPathPoint> raw = cam::RotarySpiralPath(map, req);
  req.tolerance_mm = 0.01;
  const std::vector<cam::RotaryPathPoint> thin =
      cam::RotarySpiralPath(map, req);
  INFO(raw.size() << " samples thinned to " << thin.size());
  REQUIRE(thin.size() < raw.size());
  CHECK(thin.front().thetaDeg == raw.front().thetaDeg);
  CHECK(thin.back().thetaDeg == raw.back().thetaDeg);

  // Every sample dropped lies within the tolerance of the segment that
  // replaced it, in the space the controller interpolates in.
  double rMax = 0.0;
  for (float v : map.r)
    rMax = std::max(rMax, static_cast<double>(v));
  const double arcPerDeg = rMax * glm::pi<double>() / 180.0;
  const auto p3 = [&](const cam::RotaryPathPoint& q) {
    return glm::dvec3(q.x, q.r, q.thetaDeg * arcPerDeg);
  };
  size_t seg = 0;
  double worst = 0.0;
  for (const cam::RotaryPathPoint& q : raw) {
    while (seg + 1 < thin.size() - 1 && thin[seg + 1].thetaDeg < q.thetaDeg)
      seg++;
    const glm::dvec3 a = p3(thin[seg]), b = p3(thin[seg + 1]);
    const glm::dvec3 d = b - a;
    const double len = glm::length(d);
    if (len < 1e-12)
      continue;
    worst = std::max(worst, glm::length(glm::cross(p3(q) - a, d)) / len);
  }
  INFO("worst deviation " << worst);
  CHECK(worst <= req.tolerance_mm + 1e-9);
}

TEST_CASE("cam spiral: the part turns while it cuts, and verifies clean",
          "[cam]") {
  auto doc = ShaftDoc();
  REQUIRE(doc->GetBodies().size() == 1);
  cam::Setup setup = ShaftSetup();
  setup.operations.push_back(RotaryOp(1, cam::RotaryPattern::Spiral));

  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());

  // A moves ON the cutting moves -- which is the whole difference from the
  // parallel pattern, where it moves only on the links.
  size_t turningCuts = 0, rapids = 0;
  for (size_t i = 1; i < paths[0].moves.size(); i++) {
    const cam::Move& a = paths[0].moves[i - 1];
    const cam::Move& b = paths[0].moves[i];
    if (b.kind == cam::Move::Kind::Rapid)
      rapids++;
    else if (std::abs(b.a - a.a) > 1e-9)
      turningCuts++;
  }
  INFO(turningCuts << " turning cuts, " << rapids << " rapids");
  // Thinned: a constant-radius stretch of helix is straight in (x, r, A),
  // so much of it posts as moves of half a turn. The shoulders are not.
  CHECK(turningCuts > 20);
  CHECK(rapids <= 2); // in and out: one helix, no pass links

  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.5);
  INFO("rotary " << v.rotary << " gouge " << v.maxGouge << " leftover "
                 << v.maxLeftover << " rapids " << v.rapidsThroughStock);
  REQUIRE(v.rotary);
  CHECK(v.maxGouge < 0.1);
  CHECK(v.rapidsThroughStock == 0);
  // No worse than the parallel passes on the same part. This is the check
  // that made the pitch follow the surface: at a fixed axial pitch the helix
  // crossed each shoulder in 3 mm steps and left 3.7 mm against the
  // parallel's 1.1, and looking only AHEAD of the cutter for the slope
  // still left 2.2. What is left is the corner a 6 mm ball cannot reach.
  cam::Setup parallelSetup = ShaftSetup();
  parallelSetup.operations.push_back(RotaryOp(1, cam::RotaryPattern::Parallel));
  std::string parallelGcode;
  REQUIRE(cam::PostProcess("mach3", parallelSetup,
                           cam::GenerateAll(parallelSetup, doc->GetBodies()),
                           {}, parallelGcode));
  const cam::Verdict pv =
      cam::VerifyGcode(parallelGcode, parallelSetup, doc->GetBodies(), 0.5);
  INFO("parallel leftover " << pv.maxLeftover);
  CHECK(v.maxLeftover > 0.5);
  CHECK(v.maxLeftover <= pv.maxLeftover + 1e-6);

  // One engine: playing the file to its end leaves the verifier's stock,
  // bit for bit.
  constexpr double kRes = 0.5;
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ProgramInSetupSpace(gcode, setup), 0.0);
  REQUIRE(tl.rotary);
  cam::RotaryStockSim sim;
  const std::vector<Body> bodies =
      cam::BodiesInSetupFrame(setup, doc->GetBodies());
  sim.Begin(tl, setup, cam::MakeRotaryBlank(setup, bodies, kRes));
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

TEST_CASE("cam spiral: a turning cut is posted at the feed the tip needs",
          "[cam]") {
  // f = 600 at the tip, the tool 20 mm off the axis. The first move is the
  // rapid there, so the clock below counts exactly the three cuts.
  cam::Setup setup = BasicSetup();
  setup.machine.hasRotaryAxis = true;
  setup.rapidFeed = 0.0;
  cam::Toolpath tp;
  tp.operationId = 7;
  tp.toolNumber = 1;
  tp.moves = {CutAt(0.0, 20.0, 0.0, 600.0), CutAt(0.0, 20.0, 90.0, 600.0),
              CutAt(10.0, 20.0, 90.0, 600.0), CutAt(20.0, 20.0, 180.0, 600.0)};
  tp.moves[0].kind = cam::Move::Kind::Rapid;

  const double quarter = glm::pi<double>() * 20.0 / 2.0; // mm of arc
  // A pure turn: both controllers read degrees alone, so F = 600 * 90 /
  // (quarter of the circumference) -- FreeCAD's 360 / 2 pi r.
  for (const char* post : {"mach3", "linuxcnc"}) {
    std::string gcode, error;
    REQUIRE(cam::PostProcess(post, setup, {tp}, {}, gcode, &error));
    INFO(post << "\n" << gcode);
    CHECK_THAT(FeedOnLineWith(gcode, "A90"),
               WithinAbs(600.0 * 90.0 / quarter, 0.05));
    // A plain X move after it: back to 600, exactly.
    CHECK_THAT(FeedOnLineWith(gcode, "X10"), WithinAbs(600.0, 1e-9));
  }
  // A MIXED move -- 10 mm of X and 90 degrees -- is where they differ.
  const double tip = std::hypot(10.0, quarter);
  {
    std::string gcode;
    REQUIRE(cam::PostProcess("mach3", setup, {tp}, {}, gcode));
    CHECK_THAT(FeedOnLineWith(gcode, "A180"),
               WithinAbs(600.0 * std::hypot(10.0, 90.0) / tip, 0.05));
  }
  {
    std::string gcode;
    REQUIRE(cam::PostProcess("linuxcnc", setup, {tp}, {}, gcode));
    CHECK_THAT(FeedOnLineWith(gcode, "A180"),
               WithinAbs(600.0 * 10.0 / tip, 0.05));
  }

  // The cycle time is what the controller will take: the posted file,
  // clocked the controller's way, agrees with CuttingMinutes.
  for (const char* post : {"mach3", "linuxcnc"}) {
    setup.machine.post = post;
    std::string gcode;
    REQUIRE(cam::PostProcess(post, setup, {tp}, {}, gcode));
    const cam::Timeline tl = cam::BuildTimeline(
        cam::ProgramInSetupSpace(gcode, setup), 0.0, cam::FeedModelOf(post));
    INFO(post);
    CHECK_THAT(tl.cuttingMinutes,
               WithinRel(cam::CuttingMinutes(setup, {tp}), 1e-3));
    // ...and, unclamped, that is the tip's path at the feed asked for.
    CHECK_THAT(tl.cuttingMinutes,
               WithinRel((quarter + 10.0 + tip) / 600.0, 1e-3));
  }
}

TEST_CASE("cam spiral: a turning cut is held to each axis's own top speed",
          "[cam]") {
  // 3 mm off the axis a quarter turn is 4.7 mm of arc against 90 "mm" of
  // degrees: F would have to be 19x the feed. The A motor tops out at
  // 1500 deg/min here, and THAT is what holds it -- not the linear rapid,
  // which a pure turn does not touch at all.
  cam::Setup setup = BasicSetup();
  setup.machine.hasRotaryAxis = true;
  setup.machine.rapidFeed = 600.0; // linear: would bite if it were asked
  setup.machine.rotaryMaxFeed = 1500.0;
  setup.rapidFeed = 0.0;
  cam::Toolpath tp;
  tp.operationId = 3;
  tp.toolNumber = 1;
  tp.moves = {CutAt(0.0, 3.0, 0.0, 600.0), CutAt(0.0, 3.0, 90.0, 600.0)};
  tp.moves[0].kind = cam::Move::Kind::Rapid;

  std::string gcode;
  REQUIRE(cam::PostProcess("mach3", setup, {tp}, {}, gcode));
  INFO(gcode);
  CHECK_THAT(FeedOnLineWith(gcode, "A90"), WithinAbs(1500.0, 1e-9));
  CHECK(gcode.find("op 3: 1 turning moves held to the machine's axis speeds") !=
        std::string::npos);

  // The verdict says it too, once, for the operation.
  const std::vector<cam::Toolpath> paths = {tp};
  size_t said = 0;
  for (const cam::VerdictLine& l : cam::MachineLines(setup, {}, &paths))
    if (l.text.find("Op 3: 1 turning moves") != std::string::npos)
      said++;
  CHECK(said == 1);

  // And the clock counts the slower cut, not the feed that was asked for.
  const double atFeed = glm::pi<double>() * 3.0 / 2.0 / 600.0;
  CHECK(cam::CuttingMinutes(setup, {tp}) > atFeed * 1.5);
  CHECK_THAT(cam::CuttingMinutes(setup, {tp}), WithinRel(90.0 / 1500.0, 1e-9));

  // A mixed move is held by whichever axis runs out first: 30 mm of X with
  // 9 degrees at 1000 at the tip, on a 600 mm/min linear axis, is held by X
  // (A would turn at only ~180 deg/min).
  const cam::ScaledFeed f = cam::ControllerFeed(
      {0.0, 0.0, 3.0}, 0.0, {30.0, 0.0, 3.0}, 9.0, 1000.0,
      cam::RotaryFeedModel::Combined, cam::AxisLimitsOf(setup));
  CHECK(f.clamped);
  // X moves at F * 30 / hypot(30, 9): held to 600 exactly.
  CHECK_THAT(f.feed * 30.0 / std::hypot(30.0, 9.0), WithinAbs(600.0, 1e-9));

  // With no A top speed stated, nothing is held -- and the verdict says what
  // the program asks of the axis instead, once for the whole job.
  setup.machine.rotaryMaxFeed = 0.0;
  size_t asked = 0;
  for (const cam::VerdictLine& l : cam::MachineLines(setup, {}, &paths))
    if (l.text.find("does not state its A top speed") != std::string::npos)
      asked++;
  CHECK(asked == 1);
}

TEST_CASE("cam spiral: the parser reads G92 A as a rename, not a move",
          "[cam]") {
  const cam::GProgram p =
      cam::ParseGcode("G0 Z25 A0\nG1 Z20 A720 F300\nG92 A0\nG0 A10\nG1 X5\n");
  INFO(p.errors.size());
  REQUIRE(p.errors.empty());
  REQUIRE(p.moves.size() == 4); // the G92 line is not a move
  // The axis turned two full turns, was RENAMED zero, then turned ten
  // degrees more: 730, continuous, which is where it physically is.
  CHECK_THAT(p.moves[1].a, WithinAbs(720.0, 1e-9));
  CHECK_THAT(p.moves[2].a, WithinAbs(730.0, 1e-9));
  CHECK_THAT(p.moves[3].a, WithinAbs(730.0, 1e-9));

  // G92 on a linear axis moves the work zero, which nothing here models:
  // said, not silently read as a move.
  const cam::GProgram bad = cam::ParseGcode("G0 Z25\nG92 X0\n");
  CHECK_FALSE(bad.errors.empty());
  CHECK(bad.moves.size() == 1);
}

TEST_CASE("cam spiral: a wound axis is renamed, and the next op does not "
          "unwind it",
          "[cam]") {
  auto doc = ShaftDoc();
  cam::Setup setup = ShaftSetup();
  setup.operations.push_back(RotaryOp(1, cam::RotaryPattern::Spiral));
  setup.operations.push_back(RotaryOp(2, cam::RotaryPattern::Parallel));
  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[0].error.empty());
  REQUIRE(paths[1].error.empty());

  std::string gcode;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode));
  // 40 mm at a 3 mm pitch plus the rings is about fifteen turns: past one,
  // so the post renames it.
  REQUIRE(gcode.find("G92 A") != std::string::npos);

  // Read back, the axis never turns more than a revolution between the two
  // operations -- the rename put the next op's angles on the nearest turn.
  const cam::GProgram p = cam::ProgramInSetupSpace(gcode, setup);
  REQUIRE(p.errors.empty());
  size_t boundary = 0;
  for (size_t k = 1; k < p.moves.size(); k++)
    if (p.moves[k].op == 2 && p.moves[k - 1].op == 1) {
      boundary = k;
      break;
    }
  REQUIRE(boundary > 0);
  // Every move of op 2 up to its first cut: the retract, and the index.
  double spin = 0.0;
  for (size_t k = boundary; k < p.moves.size() && p.moves[k].rapid; k++)
    spin = std::max(spin, std::abs(p.moves[k].a - p.moves[boundary - 1].a));
  INFO("op 2's approach turns " << spin << " degrees");
  CHECK(spin < 360.0);

  // And the file still verifies: the rename keeps every move on the same
  // physical angle the generator planned.
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.5);
  CHECK(v.rotary);
  CHECK(v.maxGouge < 0.1);
}

TEST_CASE("cam spiral: a stepdown cuts in layers from the blank inward",
          "[cam]") {
  auto doc = ShaftDoc();
  cam::Setup setup = ShaftSetup();
  // Parallel, because a raster's finish layer is directly comparable: the
  // spiral's is shifted to carry on from the turn the layer before ended on.
  cam::Operation op = RotaryOp(1, cam::RotaryPattern::Parallel);
  setup.operations.push_back(op);
  const auto finish = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(finish[0].error.empty());

  setup.operations[0].stepdown = 1.5;
  const auto layered = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(layered[0].error.empty());
  const auto& lm = layered[0].moves;
  const auto& fm = finish[0].moves;
  // Blank Phi 32 (margin 1), journal Phi 22: the layers are at 14.5, 13 and
  // 11.5 of CL radius, then the surface -- so several times the moves.
  REQUIRE(lm.size() > fm.size() * 2);
  // The last layer IS the unlayered finish, move for move.
  for (size_t k = 0; k < fm.size(); k++) {
    const cam::Move& a = lm[lm.size() - fm.size() + k];
    const cam::Move& b = fm[k];
    REQUIRE(a.kind == b.kind);
    REQUIRE(a.to == b.to);
    REQUIRE(a.a == b.a);
  }
  // No layer before it cuts below its floor: the first cut of the whole op
  // is at least one stepdown under the blank and never deeper than that.
  const double blankR = cam::RotaryBlankRadius(setup, 15.0);
  double firstLayerMin = 1e18;
  for (size_t k = 0; k < lm.size() - fm.size(); k++)
    if (lm[k].kind == cam::Move::Kind::Cut)
      firstLayerMin = std::min(firstLayerMin, lm[k].to.z);
  CHECK(firstLayerMin >= blankR - 3.0 * 1.5 - 1e-6);

  // The spiral layers carry the angle on rather than restarting it: no rapid
  // anywhere in the op turns the part more than a revolution.
  setup.operations[0].rotaryPattern = cam::RotaryPattern::Spiral;
  const auto spiral = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(spiral[0].error.empty());
  for (size_t k = 1; k < spiral[0].moves.size(); k++)
    if (spiral[0].moves[k].kind == cam::Move::Kind::Rapid)
      CHECK(std::abs(spiral[0].moves[k].a - spiral[0].moves[k - 1].a) < 360.0);
}

TEST_CASE("cam spiral: layers are asked for, never inherited", "[cam]") {
  // The derivation gives every 2.5D op half the cutter as a stepdown, and
  // until v123 it gave one to rotary ops too, which ignored it. Reading it
  // now would turn every rotary op -- derived, retooled, or reopened from a
  // v122 file -- into a rough from the blank that nobody asked for. Found
  // on the rolling pin: 114 m of cutting became 310.
  auto doc = ShaftDoc();
  cam::Setup setup = ShaftSetup();
  cam::Tool second = ShapedTool(cam::ToolType::Ball, 4.0);
  second.number = 2;
  setup.tools.push_back(second);
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::SurfaceRotary, cam::Target{});
  REQUIRE(d.ok);
  CHECK(d.op.stepdown == 0.0); // a finish, unless asked
  setup.operations.push_back(d.op);
  setup.operations[0].id = 1;
  std::string why;
  REQUIRE(cam::Retool(setup, setup.operations[0], 2, why));
  CHECK(setup.operations[0].stepdown == 0.0); // and a new cutter keeps it one

  // A v122 file that stored the derivation's 3 mm reloads as a finish; a
  // v123 file keeps the layers it asked for.
  const std::filesystem::path golden =
      std::filesystem::path(DESIGNER_SOURCE_DIR) /
      "tests/golden/job/v122-cam-rotary.json";
  std::ifstream in(golden);
  REQUIRE(in.good());
  nlohmann::json j = nlohmann::json::parse(in);
  j["cam"][0]["operations"][0]["stepdown"] = 3.0;
  for (const int version : {122, 123}) {
    j["version"] = version;
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() /
        ("spiral_stepdown_v" + std::to_string(version) + ".json");
    std::ofstream(file) << j.dump();
    auto job = DocumentIO::LoadJob(file.string());
    std::filesystem::remove(file);
    REQUIRE(job.has_value());
    const double got = (*job)->GetSetups()[0].operations[0].stepdown;
    INFO("v" << version);
    CHECK(got == (version < 123 ? 0.0 : 3.0));
  }
}

#endif // HAVE_CAM
