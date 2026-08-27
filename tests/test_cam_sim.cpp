#ifdef HAVE_CAM

#include "core/Document.h"
#include "core/Feature.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamSim.h"
#include "core/cam/CamStockMesh.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"

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

} // namespace

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
