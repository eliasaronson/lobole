#ifdef HAVE_CAM

#include "core/Document.h"
#include "core/Feature.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamSim.h"
#include "core/cam/CamStockMesh.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "core/features/BooleanFeature.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <utility>

// Cutting simulation (docs/cam/simulation.md). The load-bearing test in this
// file is the first one: playing the program to its end must leave the SAME
// height map the verdict is read from. That equality is the whole design —
// it is what makes the picture on screen evidence rather than an impression.
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::unique_ptr<Document> BlockDoc(double w = 40, double h = 30,
                                   double d = 10) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), w, h, d));
  return doc;
}

cam::Setup BasicSetup() {
  cam::Setup s;
  s.name = "Job";
  s.safeZ = 25.0;
  cam::Tool t;
  t.number = 1;
  t.name = "6mm flat";
  t.diameter = 6.0;
  t.feedCut = 600.0;
  t.feedPlunge = 200.0;
  s.tools.push_back(t);
  return s;
}

// A block with one profile operation, posted.
struct Job {
  std::unique_ptr<Document> doc;
  cam::Setup setup;
  std::string gcode;
};

Job ProfiledBlock() {
  Job j;
  j.doc = BlockDoc(40, 30, 10);
  j.setup = BasicSetup();
  cam::Operation op;
  op.id = 7;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 11.0;
  op.bottomZ = 0.0;
  op.stepdown = 5.0;
  j.setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(j.setup, j.doc->GetBodies());
  std::string error;
  REQUIRE(cam::PostProcess("mach3", j.setup, paths, {}, j.gcode, &error));
  return j;
}

// The same job on a panel that stands UPRIGHT — thin in Y, tall in Z, like
// every part imported in its assembly orientation — laid flat on the bed by
// the setup's fixture frame rather than by editing the model
// (docs/cam/setup-frame.md). Every other fixture in this file is modelled
// face-up, which means setup space and model space are identical in them and
// nothing here has ever exercised the distinction.
Job LaidFlatPanel() {
  Job j;
  j.doc = BlockDoc(40, 10, 30); // 40 wide, 10 THICK, 30 tall: on its edge
  j.setup = BasicSetup();
  // Clamp it with a Y-facing face up: the frame that maps +Z to -Y.
  j.setup.frame = cam::SolveFaceUp({0.0, -1.0, 0.0}, glm::dquat(1, 0, 0, 0));
  j.setup.safeZ = cam::DeriveSafeZ(j.setup, j.doc->GetBodies());
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(j.setup, j.doc->GetBodies(), lo, hi));
  cam::Operation op;
  op.id = 7;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = hi.z;
  op.bottomZ = lo.z;
  op.stepdown = 5.0;
  j.setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(j.setup, j.doc->GetBodies());
  std::string error;
  REQUIRE(cam::PostProcess("mach3", j.setup, paths, {}, j.gcode, &error));
  return j;
}

} // namespace

TEST_CASE("cam sim: a laid-flat part plays the map its verdict is read from",
          "[cam]") {
  // The load-bearing equality of this whole file, on a setup whose frame is
  // NOT identity. Everything the simulation touches has to agree about which
  // space it is in: StockBounds derives the box in setup space, the post
  // writes the file in setup space, and StockSim carves the one with the
  // other. If any of the three slipped back into model space the block would
  // be the wrong shape and the tool would cut past it — and at identity,
  // where every other test in this file lives, that mistake is invisible.
  Job job = LaidFlatPanel();
  constexpr double kRes = 0.5;

  const cam::Inspection insp =
      cam::Inspect(job.gcode, job.setup, job.doc->GetBodies(), kRes);
  REQUIRE(insp.ok);

  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(job.setup, job.doc->GetBodies(), lo, hi));
  // The panel is lying down: the box is 40 x 30 in the machine's XY and only
  // as deep as the panel is thick. Asserted so this test fails LOUDLY if the
  // frame stops reaching StockBounds, rather than quietly comparing two maps
  // that are both wrong in the same way.
  CHECK_THAT(hi.x - lo.x, WithinAbs(44.0, 1e-6)); // 40 + 2 mm margin each side
  CHECK_THAT(hi.y - lo.y, WithinAbs(34.0, 1e-6)); // 30 + 2 mm
  CHECK_THAT(hi.z - lo.z, WithinAbs(12.0, 1e-6)); // 10 thick + 1 above, 1 below

  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);
  REQUIRE_FALSE(tl.Empty());

  cam::StockSim sim;
  sim.Begin(tl, job.setup, cam::MakeStock(lo, hi, kRes));
  sim.SeekTo(tl.End());

  REQUIRE(sim.Stock().z.size() == insp.machined.z.size());
  CHECK(sim.Stock().z == insp.machined.z);
  // And it actually cut something: an equality between two empty simulations
  // would pass this without the tool ever touching the block.
  CHECK(sim.RemovedVolume() > 100.0);
}

TEST_CASE("cam sim: a laid-flat job plays where its toolpath cuts", "[cam]") {
  // The frame check of "the played path sits where the toolpath does", with a
  // fixture frame in the way. Both sides are in setup space, so the answer is
  // that the frame changes nothing here — which is worth pinning, because the
  // way to get it wrong is to apply the frame once on one side.
  Job job = LaidFlatPanel();
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(job.setup, job.doc->GetBodies());
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);

  const auto span = [](const std::vector<glm::dvec3>& pts) {
    glm::dvec3 lo(pts.front()), hi(pts.front());
    for (const glm::dvec3& p : pts) {
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
    return std::pair<glm::dvec3, glm::dvec3>{lo, hi};
  };
  std::vector<glm::dvec3> generated;
  for (const cam::Toolpath& tp : paths)
    for (const cam::Move& m : tp.moves)
      if (m.kind != cam::Move::Kind::Rapid)
        generated.push_back(m.to);
  std::vector<glm::dvec3> played;
  for (const cam::GMove& m : tl.program.moves)
    if (!m.rapid)
      played.push_back(m.to);
  REQUIRE(generated.size() > 20);
  REQUIRE(played.size() > 20);

  const auto [genLo, genHi] = span(generated);
  const auto [playLo, playHi] = span(played);
  INFO("generated " << genLo.x << "," << genLo.y << "," << genLo.z << " .. "
                    << genHi.x << "," << genHi.y << "," << genHi.z);
  INFO("played    " << playLo.x << "," << playLo.y << "," << playLo.z << " .. "
                    << playHi.x << "," << playHi.y << "," << playHi.z);
  CHECK(glm::length(playLo - genLo) < 0.02);
  CHECK(glm::length(playHi - genHi) < 0.02);

  // And the path is ON the part. Both are in SETUP space and are drawn there
  // — a job tab shows its parts laid out (v73/J6), so the backplot, the stock
  // mesh and the bodies share one space and nothing is mapped back. The
  // property this asserts is unchanged; where it is asserted is not.
  const std::vector<Body> framed =
      cam::BodiesInSetupFrame(job.setup, job.doc->GetBodies());
  glm::vec3 partLo(FLT_MAX), partHi(-FLT_MAX);
  for (const Body& b : framed) {
    glm::vec3 bmin, bmax;
    b.shape.GetBoundingBox(bmin, bmax);
    partLo = glm::min(partLo, bmin);
    partHi = glm::max(partHi, bmax);
  }
  // The panel is 40 x 10 x 30 about the origin, plus the stock margin.
  CHECK(genLo.x > partLo.x - 40.0);
  CHECK(genHi.x < partHi.x + 40.0);
  CHECK(genLo.y > partLo.y - 40.0);
  CHECK(genHi.y < partHi.y + 40.0);
}

TEST_CASE("cam sim: a job zeroed on its corner still plays its own verdict",
          "[cam]") {
  // W5's cross-cutting gate. Since v68 the post writes coordinates relative to
  // the work origin and the file's readers add it back. Both the verifier and
  // the simulator read the FILE, so an offset applied once instead of twice
  // lands here — as a program that cuts somewhere the stock is not. At the
  // origin (0,0,0) every other test in this file lives, and the mistake is
  // invisible.
  Job job = ProfiledBlock();
  job.setup.originAt = cam::OriginAt::StockMin;
  REQUIRE(cam::RecomputeOrigin(job.setup, job.doc->GetBodies()));
  REQUIRE(job.setup.origin != glm::dvec3(0.0));
  {
    const std::vector<cam::Toolpath> paths =
        cam::GenerateAll(job.setup, job.doc->GetBodies());
    std::string error;
    REQUIRE(cam::PostProcess("mach3", job.setup, paths, {}, job.gcode, &error));
  }
  constexpr double kRes = 0.5;

  const cam::Inspection insp =
      cam::Inspect(job.gcode, job.setup, job.doc->GetBodies(), kRes);
  REQUIRE(insp.ok);

  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(job.setup, job.doc->GetBodies(), lo, hi));
  const cam::Timeline tl = cam::BuildTimeline(
      cam::ProgramInSetupSpace(job.gcode, job.setup), job.setup.rapidFeed);
  REQUIRE_FALSE(tl.Empty());

  cam::StockSim sim;
  sim.Begin(tl, job.setup, cam::MakeStock(lo, hi, kRes));
  sim.SeekTo(tl.End());
  REQUIRE(sim.Stock().z.size() == insp.machined.z.size());
  CHECK(sim.Stock().z == insp.machined.z);
  CHECK(sim.RemovedVolume() > 100.0);

  // And the file really is offset: parsed RAW it sits a whole origin away
  // from the stock and ploughs through it, so the two disagree loudly. This
  // is the assertion that would have caught the offset being applied on one
  // side only — the failure mode this phase exists to make impossible.
  const cam::Timeline raw =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);
  cam::StockSim wrong;
  wrong.Begin(raw, job.setup, cam::MakeStock(lo, hi, kRes));
  wrong.SeekTo(raw.End());
  CHECK(wrong.Stock().z != insp.machined.z);
  CHECK(std::abs(wrong.RemovedVolume() - sim.RemovedVolume()) > 100.0);
}

TEST_CASE("cam sim: playing to the end leaves the verdict's own map", "[cam]") {
  Job job = ProfiledBlock();
  constexpr double kRes = 0.5;

  const cam::Inspection insp =
      cam::Inspect(job.gcode, job.setup, job.doc->GetBodies(), kRes);
  REQUIRE(insp.ok);

  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(job.setup, job.doc->GetBodies(), lo, hi));
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);
  REQUIRE_FALSE(tl.Empty());

  cam::StockSim sim;
  sim.Begin(tl, job.setup, cam::MakeStock(lo, hi, kRes));
  sim.SeekTo(tl.End());

  // Bit-identical, not merely close: both went through CarveSweep, segment
  // for segment, so any difference at all means the two have drifted apart.
  REQUIRE(sim.Stock().z.size() == insp.machined.z.size());
  CHECK(sim.Stock().z == insp.machined.z);
}

TEST_CASE("cam sim: scrubbing back and forward lands in the same place",
          "[cam]") {
  Job job = ProfiledBlock();
  constexpr double kRes = 0.6;
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(job.setup, job.doc->GetBodies(), lo, hi));
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);

  cam::StockSim sim;
  sim.Begin(tl, job.setup, cam::MakeStock(lo, hi, kRes));

  const cam::SimCursor half = tl.CursorAtDistance(tl.totalDistance * 0.5);
  sim.SeekTo(half);
  const std::vector<float> straight = sim.Stock().z;

  // Past it, back to the start, and forward again: a height map cannot be
  // un-cut, so this only works because seeking backwards re-runs from the
  // pristine block. If it ever silently kept the deeper map, this is the
  // test that says so.
  sim.SeekTo(tl.End());
  sim.SeekTo({0, 0.0});
  sim.SeekTo(half);
  CHECK(sim.Stock().z == straight);

  // And half way really is half way: something is cut, and less than at the
  // end.
  cam::StockSim whole;
  whole.Begin(tl, job.setup, cam::MakeStock(lo, hi, kRes));
  whole.SeekTo(tl.End());
  double halfRemoved = 0.0, allRemoved = 0.0;
  const cam::HeightMap pristine = cam::MakeStock(lo, hi, kRes);
  for (size_t i = 0; i < pristine.z.size(); i++) {
    halfRemoved += pristine.z[i] - sim.Stock().z[i];
    allRemoved += pristine.z[i] - whole.Stock().z[i];
  }
  CHECK(halfRemoved > 0.0);
  CHECK(halfRemoved < allRemoved);
}

TEST_CASE("cam sim: the parser keeps the feed, the tool change and the op",
          "[cam]") {
  const cam::GProgram p = cam::ParseGcode("(op 3)\n"
                                          "G0 X0 Y0 Z5\n"
                                          "G1 Z-1 F300\n"
                                          "X10\n"  // modal feed
                                          "F450\n" // feed-only block
                                          "X20\n"  // the new feed
                                          "(op 4)\n"
                                          "T2 M6\n"
                                          "G1 X30 F200\n");
  REQUIRE(p.errors.empty());
  REQUIRE(p.moves.size() == 5);
  CHECK_THAT(p.moves[1].feed, WithinAbs(300.0, 1e-9));
  CHECK_THAT(p.moves[2].feed, WithinAbs(300.0, 1e-9)); // modal
  CHECK_THAT(p.moves[3].feed, WithinAbs(450.0, 1e-9));
  CHECK(p.moves[0].op == 3);
  CHECK(p.moves[3].op == 3);
  CHECK(p.moves[4].op == 4);
  CHECK(p.moves[4].tool == 2);
  CHECK(p.moves[4].toolChange);
  CHECK_FALSE(p.moves[3].toolChange);

  // The post's own way of saying an operation produced NOTHING must not read
  // as that operation starting here.
  const cam::GProgram skipped =
      cam::ParseGcode("(SKIPPED op 9: nothing to cut)\nG0 X1 Y1\nG1 X2\n");
  REQUIRE(skipped.moves.size() == 2);
  CHECK(skipped.moves[1].op == 0);
}

TEST_CASE("cam sim: the timeline is addressable by distance", "[cam]") {
  cam::GProgram p;
  p.moves.push_back({false, {0, 0, 0}, 1, 1});
  p.moves.push_back({false, {10, 0, 0}, 1, 2, 600.0});
  p.moves.push_back({false, {10, 20, 0}, 1, 3, 600.0});
  const cam::Timeline tl = cam::BuildTimeline(p, 0.0);

  REQUIRE(tl.Segments() == 2);
  CHECK_THAT(tl.totalDistance, WithinAbs(30.0, 1e-9));
  // 30 mm at 600 mm/min.
  CHECK_THAT(tl.cuttingMinutes, WithinAbs(0.05, 1e-9));

  const cam::SimCursor at15 = tl.CursorAtDistance(15.0);
  CHECK(at15.seg == 1);
  CHECK_THAT(at15.t, WithinAbs(0.25, 1e-9));
  CHECK_THAT(tl.DistanceAt(at15), WithinAbs(15.0, 1e-9));
  const glm::dvec3 pos = tl.PositionAt(at15);
  CHECK_THAT(pos.x, WithinAbs(10.0, 1e-9));
  CHECK_THAT(pos.y, WithinAbs(5.0, 1e-9));

  // Off both ends, rather than out of bounds.
  CHECK(tl.CursorAtDistance(-5.0) == cam::SimCursor{0, 0.0});
  CHECK(tl.CursorAtDistance(1e6) == tl.End());
}

TEST_CASE("cam sim: the distance axis is what the file actually travels",
          "[cam]") {
  // The slider's whole axis, checked against the file it came from: never
  // going backwards, ending exactly at the total, and carrying the feeds the
  // POST wrote rather than feeds invented here. A scrub bar that disagreed
  // with the program would be a picture of a different job.
  Job job = ProfiledBlock();
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);
  REQUIRE(tl.Segments() > 100);

  double last = 0.0;
  for (size_t k = 0; k < tl.Segments(); k++) {
    CHECK(tl.distance[k] >= last);
    CHECK(tl.minutes[k] >= (k == 0 ? 0.0 : tl.minutes[k - 1]));
    last = tl.distance[k];
  }
  CHECK_THAT(tl.DistanceAt(tl.End()), WithinRel(tl.totalDistance, 1e-12));
  CHECK_THAT(tl.MinutesAt(tl.End()), WithinRel(tl.cuttingMinutes, 1e-12));

  // Every cutting move carries a feed the SETUP states — the post writes
  // feedCut in XY and feedPlunge going down, and nothing else.
  const cam::Tool& tool = job.setup.tools.front();
  int cutting = 0;
  for (const cam::GMove& m : tl.program.moves) {
    if (m.rapid)
      continue;
    if (m.feed <= 1e-9)
      continue; // the first move only establishes position
    cutting++;
    const bool known = std::abs(m.feed - tool.feedCut) < 1e-6 ||
                       std::abs(m.feed - tool.feedPlunge) < 1e-6;
    INFO("line " << m.line << " feed " << m.feed);
    CHECK(known);
  }
  CHECK(cutting > 50);
}

TEST_CASE("cam sim: the played path sits where the toolpath does", "[cam]") {
  // The frame check. The backplot draws `Toolpath::moves`; the playback draws
  // the PARSED FILE. If the post moved, rotated or offset anything on the way
  // out, the two pictures would sit in different places on the model — which
  // is what a user sees as "the positions are off". Same points, same order,
  // or one of the two is lying.
  Job job = ProfiledBlock();
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(job.setup, job.doc->GetBodies());
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);

  // NOT point for point: the post fits arcs and merges collinear runs, so the
  // file is a shorter description of the same path (a rectangle profiled with
  // a round tool comes back as four lines and four G2s). What has to match is
  // the GEOMETRY — where the path is, and how far it travels.
  const auto span = [](const std::vector<glm::dvec3>& pts) {
    glm::dvec3 lo(pts.front()), hi(pts.front());
    for (const glm::dvec3& p : pts) {
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
    return std::pair<glm::dvec3, glm::dvec3>{lo, hi};
  };
  const auto length = [](const std::vector<glm::dvec3>& pts) {
    double d = 0.0;
    for (size_t i = 1; i < pts.size(); i++)
      d += glm::length(pts[i] - pts[i - 1]);
    return d;
  };

  std::vector<glm::dvec3> generated;
  for (const cam::Toolpath& tp : paths)
    for (const cam::Move& m : tp.moves)
      if (m.kind != cam::Move::Kind::Rapid)
        generated.push_back(m.to);
  std::vector<glm::dvec3> played;
  for (const cam::GMove& m : tl.program.moves)
    if (!m.rapid)
      played.push_back(m.to);
  REQUIRE(generated.size() > 100);
  REQUIRE(played.size() > 100);

  // The BOX the cutting lives in. This is the one that catches "the positions
  // are off": a frame that shifted or turned on the way through the post
  // moves this box, however many points describe it.
  const auto [genLo, genHi] = span(generated);
  const auto [playLo, playHi] = span(played);
  INFO("generated " << genLo.x << "," << genLo.y << "," << genLo.z << " .. "
                    << genHi.x << "," << genHi.y << "," << genHi.z);
  INFO("played    " << playLo.x << "," << playLo.y << "," << playLo.z << " .. "
                    << playHi.x << "," << playHi.y << "," << playHi.z);
  // The chord tolerance an arc is re-expanded at (0.02 mm) is the budget: a
  // ramp fitted into a G2 comes back a few microns shallower at its deepest
  // chord. A frame error would be millimetres, not microns.
  CHECK(glm::length(playLo - genLo) < 0.02);
  CHECK(glm::length(playHi - genHi) < 0.02);

  // And it travels the same distance, to within the chord tolerance an arc
  // is re-expanded at.
  INFO("generated " << length(generated) << " mm, played " << length(played)
                    << " mm");
  CHECK_THAT(length(played), WithinRel(length(generated), 0.01));
}

TEST_CASE("cam sim: an ARC-fitted, tabbed job plays where it cuts", "[cam]") {
  // The gap that let a real defect through. The bbox check above uses a
  // rectangular profile: no arcs, no tabs, one pass. A real job derived from
  // a round opening has all three — the post fits G2/G3 across the moves,
  // tabs interrupt them, and the parser re-expands the arcs — and the
  // PLAYBACK draws the parsed file while the backplot draws the toolpath. If
  // those two disagree the picture is of a different job than the one that
  // will be cut, and only a job with arcs can show it.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 200, 150, 20));
  const uint64_t plate = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::vec3(100, 75, -1), glm::vec3(0, 0, 1), 30.0, 22.0));
  const uint64_t bore = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Opening", plate, bore,
                                                   BooleanOp::Subtract));

  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  cam::Operation op;
  op.id = 21;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Inside;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 2.75; // 8 passes, like a derived job
  op.tabCount = 4;    // and tabs, like a derived job
  op.tabWidth = 12.0;
  op.tabHeight = 4.4;
  // The wall of the bore is what "cut the opening" points at.
  for (const Body& b : doc->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (b.shape.FacePlane(index, c, n, u))
        continue; // planar: not the bore
      op.targets.push_back(cam::Target{0, ElementRef{b.createdBy, name}});
      break;
    }
  REQUIRE(op.targets.size() == 1);
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  // The post FITTED arcs: this job is the interesting one precisely because
  // it did.
  INFO(gcode.substr(0, 400));
  CHECK((gcode.find("G2") != std::string::npos ||
         gcode.find("G3") != std::string::npos));

  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(gcode), setup.rapidFeed);
  std::vector<glm::dvec3> generated, played;
  for (const cam::Toolpath& tp : paths)
    for (const cam::Move& m : tp.moves)
      if (m.kind != cam::Move::Kind::Rapid)
        generated.push_back(m.to);
  for (const cam::GMove& m : tl.program.moves)
    if (!m.rapid)
      played.push_back(m.to);
  REQUIRE(generated.size() > 50);
  REQUIRE(played.size() > 50);

  const auto span = [](const std::vector<glm::dvec3>& pts) {
    glm::dvec3 lo(pts.front()), hi(pts.front());
    for (const glm::dvec3& p : pts) {
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
    return std::pair<glm::dvec3, glm::dvec3>{lo, hi};
  };
  const auto [genLo, genHi] = span(generated);
  const auto [playLo, playHi] = span(played);
  INFO("generated " << genLo.x << "," << genLo.y << "," << genLo.z << " .. "
                    << genHi.x << "," << genHi.y << "," << genHi.z);
  INFO("played    " << playLo.x << "," << playLo.y << "," << playLo.z << " .. "
                    << playHi.x << "," << playHi.y << "," << playHi.z);
  CHECK(glm::length(playLo - genLo) < 0.05);
  CHECK(glm::length(playHi - genHi) < 0.05);
}

TEST_CASE("cam sim: a rapid is not counted as time it cannot know", "[cam]") {
  cam::GProgram p;
  p.moves.push_back({false, {0, 0, 0}, 1, 1});
  p.moves.push_back({true, {100, 0, 0}, 1, 2}); // rapid
  p.moves.push_back({false, {100, 6, 0}, 1, 3, 600.0});

  // No stated traverse rate: the rapid contributes nothing, exactly as
  // CuttingMinutes does. Inventing 5 m/min here would be a cycle time the
  // machine never agreed to.
  const cam::Timeline quiet = cam::BuildTimeline(p, 0.0);
  CHECK_THAT(quiet.cuttingMinutes, WithinAbs(0.01, 1e-9));
  // Stated: it counts.
  const cam::Timeline stated = cam::BuildTimeline(p, 6000.0);
  CHECK_THAT(stated.cuttingMinutes, WithinAbs(0.01 + 100.0 / 6000.0, 1e-9));
  // The DISTANCE axis is the same either way — which is why the slider uses
  // it and not the clock.
  CHECK_THAT(quiet.totalDistance, WithinAbs(stated.totalDistance, 1e-12));
}

TEST_CASE("cam stock mesh: an uncut block is six quads and its own volume",
          "[cam]") {
  const cam::HeightMap block =
      cam::MakeStock({0, 0, 0}, {40, 30, 20}, /*res=*/1.0);
  const cam::StockMesh mesh = cam::BuildStockMesh(block, 0.0);

  // Merging is the point: 1200 cells, and the block comes out as a box.
  // A per-cell mesh would be 1200 top quads before the walls.
  CHECK(mesh.indices.size() / 3 == 12);
  CHECK_THAT(mesh.Volume(), WithinRel(40.0 * 30.0 * 20.0, 1e-9));
  CHECK_THAT(static_cast<double>(mesh.bboxMax.z), WithinAbs(20.0, 1e-6));
}

TEST_CASE("cam stock mesh: what was cut away is missing from the volume",
          "[cam]") {
  cam::HeightMap map = cam::MakeStock({0, 0, 0}, {40, 30, 20}, 1.0);
  // A 10 x 10 pocket, 5 mm deep.
  for (int j = 5; j < 15; j++)
    for (int i = 10; i < 20; i++)
      map.Set(i, j, 15.0f);

  const cam::StockMesh mesh = cam::BuildStockMesh(map, 0.0);
  CHECK_THAT(mesh.Volume(),
             WithinRel(40.0 * 30.0 * 20.0 - 10.0 * 10.0 * 5.0, 1e-9));

  // The mesh is the map: whatever merging does to the triangle count, the
  // enclosed volume has to equal the columns the map describes.
  double columns = 0.0;
  for (int j = 0; j < map.ny; j++)
    for (int i = 0; i < map.nx; i++)
      columns += map.res * map.res * map.At(i, j);
  CHECK_THAT(mesh.Volume(), WithinRel(columns, 1e-9));
}

TEST_CASE("cam stock mesh: a cut below the stock bottom still closes",
          "[cam]") {
  // A through cut with a sacrificial pass. The floor has to follow it, or
  // the "solid" is open at the bottom of every cut.
  cam::HeightMap map = cam::MakeStock({0, 0, 0}, {20, 20, 10}, 1.0);
  for (int j = 8; j < 12; j++)
    for (int i = 8; i < 12; i++)
      map.Set(i, j, -1.0f);

  const cam::StockMesh mesh = cam::BuildStockMesh(map, 0.0);
  double columns = 0.0;
  for (int j = 0; j < map.ny; j++)
    for (int i = 0; i < map.nx; i++)
      columns += map.res * map.res * (map.At(i, j) - (-1.0 - 1e-6));
  CHECK_THAT(mesh.Volume(), WithinRel(columns, 1e-6));
}

TEST_CASE("cam stock mesh: the machined block matches the part it cut",
          "[cam]") {
  // End to end at a coarse grid: profile a 40x30x10 block, play the program
  // out, mesh what is left. The stock started at 44 x 34 x 12 (margins), and
  // the profile takes the margin off down to the table — so what remains has
  // to be smaller than the block it started as and no smaller than the part.
  Job job = ProfiledBlock();
  constexpr double kRes = 0.5;
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(job.setup, job.doc->GetBodies(), lo, hi));
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);

  cam::StockSim sim;
  sim.Begin(tl, job.setup, cam::MakeStock(lo, hi, kRes));
  const cam::StockMesh before = cam::BuildStockMesh(sim.Stock(), lo.z);
  sim.SeekTo(tl.End());
  const cam::StockMesh after = cam::BuildStockMesh(sim.Stock(), lo.z);

  const double blockVolume = 40.0 * 30.0 * 10.0;
  INFO("before " << before.Volume() << " after " << after.Volume() << " part "
                 << blockVolume);
  CHECK(after.Volume() < before.Volume());
  CHECK(after.Volume() > blockVolume);
  // And the surface it produced is a surface: cut stock has more faces than
  // a bare block.
  CHECK(after.indices.size() > before.indices.size());
}

TEST_CASE("cam mesh: every triangle faces the way its normals do", "[cam]") {
  // The tool glyph was wound the opposite way to its own normals — every
  // triangle of it. With backface culling that draws the far wall's INSIDE
  // and culls the near one, so the cutter looked transparent and you could
  // see down its own bore. It had always been so; a mirror-bright material
  // and a 49 mm holder hid it, and a matte 6 mm collar did not.
  //
  // Cheap to check and impossible to eyeball, which is why it survived three
  // rounds of looking at screenshots: the geometric normal of each triangle
  // must point the same way as the vertex normals at its corners.
  const auto wound = [](const cam::StockMesh& m) {
    int agree = 0, disagree = 0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
      const glm::vec3 a = m.vertices[m.indices[i]].position;
      const glm::vec3 b = m.vertices[m.indices[i + 1]].position;
      const glm::vec3 c = m.vertices[m.indices[i + 2]].position;
      const glm::vec3 geo = glm::cross(b - a, c - a);
      if (glm::length(geo) < 1e-9f)
        continue; // a ring that closes on the axis
      const glm::vec3 vn = m.vertices[m.indices[i]].normal +
                           m.vertices[m.indices[i + 1]].normal +
                           m.vertices[m.indices[i + 2]].normal;
      (glm::dot(glm::normalize(geo), glm::normalize(vn)) > 0.0f ? agree
                                                                : disagree)++;
    }
    return std::pair<int, int>{agree, disagree};
  };

  for (const cam::ToolType type : {cam::ToolType::Flat, cam::ToolType::Ball,
                                   cam::ToolType::VBit, cam::ToolType::Drill}) {
    cam::Tool t;
    t.number = 1;
    t.diameter = 8.0;
    t.fluteLength = 25.0;
    t.type = type;
    const auto [agree, disagree] = wound(cam::BuildToolMesh(t));
    INFO("tool type " << static_cast<int>(type));
    CHECK(agree > 100);
    CHECK(disagree == 0);
  }

  // The stock is built by a different function; check it the same way rather
  // than assume.
  const cam::HeightMap stock =
      cam::MakeStock({0.0, 0.0, 0.0}, {40.0, 30.0, 20.0}, 1.0);
  const auto [sAgree, sDisagree] = wound(cam::BuildStockMesh(stock, 0.0));
  INFO("stock mesh: " << sAgree << " agree, " << sDisagree << " disagree");
  CHECK(sAgree >= 12); // an uncut block merges down to six quads
  CHECK(sDisagree == 0);
}

TEST_CASE("cam verify: the holder fouls what the cutter never touched",
          "[cam]") {
  // The one failure the finished part cannot show you. Every other check in
  // this file reads the SHAPE that came out; here the shape is perfect and
  // the collet nut has hit the tower beside the pocket on the way past.
  //
  // The part: a 10 mm base with a 40 mm tower standing at one end of it.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Base", glm::vec3(0, 0, 0), 60, 30, 10));
  doc->AddFeature(
      std::make_unique<BoxFeature>("Tower", glm::vec3(44, 0, 0), 16, 30, 40));

  cam::Setup setup = BasicSetup();
  setup.origin = glm::dvec3(0.0); // the file is written in setup space here
  // T1 roughs and states no holder, so nothing is checked on it -- which is
  // what lets the clearing passes below set the map up without reporting
  // themselves. T2 is the short one that gets into trouble.
  cam::Tool t2 = setup.tools[0];
  t2.number = 2;
  t2.name = "6mm flat, short";
  t2.fluteLength = 20.0;
  t2.overallLength = 25.0;  // tip to the underside of the nut
  t2.collarDiameter = 30.0; // an ER32 nut, near enough
  setup.tools.push_back(t2);

  // Clear the base end down to its top face with T1 (to X41, so the 6 mm
  // cutter reaches the tower's face at X44 and no uncut sliver is left to
  // stand in for it), then take one pass with
  // T2 alongside the tower. Written by hand rather than generated: the point
  // is a tool change between two identical cuts, which no strategy emits.
  std::string g = "T1 M6\n";
  for (int k = 0; k <= 9; k++) {
    const double y = -2.0 + 4.0 * k;
    g += "G1 X-2 Y" + std::to_string(y) + " Z10\n";
    g += "G1 X41 Y" + std::to_string(y) + " Z10\n";
  }
  // Park over cleared ground and lift BEFORE the tool change. A diagonal
  // retract straight from the last pass to the new start passes within four
  // millimetres of the tower at Z10, which the fat collar rightly reports and
  // the slim one below does too -- a real finding, and not the one this case
  // is about.
  g += "G1 X-2 Y-2 Z10\nG0 Z50\n";
  g += "T2 M6\nG0 X38 Y0\nG1 Z10\nG1 Y30\n";

  const cam::Inspection insp = cam::Inspect(g, setup, doc->GetBodies(), 0.5);
  REQUIRE(insp.ok);
  INFO("collisions " << insp.verdict.holderCollisions << " worst "
                     << insp.verdict.worstHolderDepth << " at line "
                     << insp.verdict.worstHolderLine);
  CHECK(insp.verdict.holderCollisions > 0);
  // The tower's uncut stock stands at Z41 (the 1 mm top margin) and the
  // collar's underside rides at 10 + 25 = 35. Six millimetres of nut in the
  // work, which is a number, not a feeling.
  CHECK_THAT(insp.verdict.worstHolderDepth, WithinAbs(6.0, 0.05));
  CHECK(insp.verdict.worstHolderAt.x >= 44.0); // on the tower, not the base
  CHECK(!insp.verdict.Safe(0.01));

  // ...and it says so in words, with the advice that actually helps: the
  // cutter is nowhere near, so grinding the depths will not fix it.
  const std::vector<cam::VerdictLine> says =
      cam::Explain(insp.verdict, 0.01, 0.0);
  bool said = false;
  for (const cam::VerdictLine& l : says)
    said = said || l.text.find("HOLDER") != std::string::npos;
  CHECK(said);

  // TWO controls, because a check that fires on everything is not a check.
  //
  // A slim collar passes where the fat one does not: the band it sweeps
  // (3 to 4 mm out) lies entirely over material T1 already cleared.
  cam::Setup slim = setup;
  slim.tools[1].collarDiameter = 8.0;
  const cam::Verdict sv = cam::Inspect(g, slim, doc->GetBodies(), 0.5).verdict;
  INFO("slim at " << sv.worstHolderAt.x << "," << sv.worstHolderAt.y
                  << " depth " << sv.worstHolderDepth << " line "
                  << sv.worstHolderLine);
  CHECK(sv.holderCollisions == 0);
  // And so does the same nut held higher up: stick the tool out 45 mm and
  // the collar rides at 55, clear over a 41 mm block.
  cam::Setup high = setup;
  high.tools[1].overallLength = 45.0;
  CHECK(cam::Inspect(g, high, doc->GetBodies(), 0.5).verdict.holderCollisions ==
        0);
  // And a tool that states NOTHING is checked against nothing -- every job in
  // the corpus predates these two fields and must stay quiet.
  cam::Setup mute = setup;
  mute.tools[1].overallLength = 0.0;
  mute.tools[1].collarDiameter = 0.0;
  CHECK(cam::Inspect(g, mute, doc->GetBodies(), 0.5).verdict.holderCollisions ==
        0);
}

TEST_CASE("cam tool mesh: a flat shoulder is shaded flat", "[cam]") {
  // The tool glyph is a revolved profile with three hard corners: flute to
  // shoulder, shoulder to holder, holder to cap. `Revolve` used to take a
  // central difference across every profile point, which averages the two
  // faces meeting at a corner into one normal — so the flat annular shoulder
  // came out shaded as a 45-degree cone, a lit facet that does not answer to
  // the camera the way the rest of the glyph does.
  cam::Tool t;
  t.number = 1;
  t.diameter = 6.0;
  t.fluteLength = 20.0;
  const cam::StockMesh m = cam::BuildToolMesh(t);
  REQUIRE(m.vertices.size() > 100);
  // Measured off the mesh, not copied from the geometry: the holder got
  // shorter and narrower and a hard-coded radius took this test to zero
  // matches without the SHAPE being any different.
  double holderR = 0.0;
  for (const cam::StockVertex& v : m.vertices)
    holderR = std::max<double>(holderR, std::hypot(v.position.x, v.position.y));

  // Somewhere on the shoulder there must be vertices whose normal is the
  // axis itself: that is what "flat annulus facing down" means.
  int flatDown = 0, sideways = 0;
  for (const cam::StockVertex& v : m.vertices) {
    const glm::vec3 n = glm::normalize(v.normal);
    if (v.position.z > 19.0f && v.position.z < 21.0f && n.z < -0.99f)
      flatDown++;
    // And the holder's wall stays a wall: normals perpendicular to the axis.
    const double r = std::hypot(v.position.x, v.position.y);
    if (r > holderR - 0.01 && v.position.z > 20.5f && std::abs(n.z) < 0.01f)
      sideways++;
  }
  INFO("flat-down " << flatDown << " sideways " << sideways);
  CHECK(flatDown > 8);
  CHECK(sideways > 8);

  // Every normal is a unit vector — a smeared one used to come back short.
  for (const cam::StockVertex& v : m.vertices)
    CHECK_THAT(glm::length(v.normal), WithinAbs(1.0, 1e-4));

  // A BALL nose still shades smoothly: the crease rule must not harden a
  // sampled curve into facets.
  cam::Tool ball = t;
  ball.type = cam::ToolType::Ball;
  const cam::StockMesh bm = cam::BuildToolMesh(ball);
  int distinctTipNormals = 0;
  float lastZ = -2.0f;
  for (const cam::StockVertex& v : bm.vertices) {
    if (v.position.z > 3.0f)
      continue;
    const float nz = glm::normalize(v.normal).z;
    if (std::abs(nz - lastZ) > 0.02f) {
      distinctTipNormals++;
      lastZ = nz;
    }
  }
  CHECK(distinctTipNormals > 4); // a curve, not one flat disc
}

TEST_CASE("cam stock mesh: merging keeps a machined block small", "[cam]") {
  // The reason the mesher merges at all. A machined 2.5D block is flats and
  // walls, so the triangle count must stay a small fraction of the cell
  // count — otherwise the picture costs what a per-cell mesh costs and the
  // whole approach is the wrong one.
  Job job = ProfiledBlock();
  constexpr double kRes = 0.25;
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(job.setup, job.doc->GetBodies(), lo, hi));
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ParseGcode(job.gcode), job.setup.rapidFeed);
  cam::StockSim sim;
  sim.Begin(tl, job.setup, cam::MakeStock(lo, hi, kRes));
  sim.SeekTo(tl.End());

  const cam::StockMesh mesh = cam::BuildStockMesh(sim.Stock(), lo.z);
  const size_t cells = sim.Stock().z.size();
  const size_t triangles = mesh.indices.size() / 3;
  INFO("cells " << cells << " triangles " << triangles);
  REQUIRE(cells > 10000);
  CHECK(triangles < cells / 4);
}

#endif // HAVE_CAM
