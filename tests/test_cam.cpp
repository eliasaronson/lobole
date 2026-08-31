#ifdef HAVE_CAM

#include "core/Document.h"
#include "core/Feature.h"
#include "core/cam/CamAdaptive.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <catch2/catch_test_macros.hpp>

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <sstream>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <numbers>

// CAM's testable half (docs/cam/plan.md): bodies in, moves out, then G-code.
// Every number here is one a machinist could check by hand, because the
// failure mode this suite exists to prevent is scrap metal.
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A 40 x 30 x 20 block at the origin, its top at z = 20.
std::unique_ptr<Document> BlockDoc(double w = 40, double h = 30,
                                   double d = 20) {
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
  t.spindleRpm = 12000.0;
  s.tools.push_back(t);
  return s;
}

} // namespace

TEST_CASE("cam: a section becomes closed loops", "[cam]") {
  auto doc = BlockDoc();
  const std::vector<cam::Loop> loops =
      cam::SectionLoops(doc->GetBodies(), 10.0);
  REQUIRE(loops.size() == 1);
  CHECK_FALSE(loops[0].hole);
  // A 40x30 rectangle: perimeter 140, area 1200. Checking both catches a
  // loop that is closed but wrong, which a point count would not.
  double perimeter = 0.0, area2 = 0.0;
  const std::vector<glm::dvec2>& p = loops[0].pts;
  for (size_t i = 0; i < p.size(); i++) {
    const glm::dvec2& a = p[i];
    const glm::dvec2& b = p[(i + 1) % p.size()];
    perimeter += glm::length(b - a);
    area2 += a.x * b.y - b.x * a.y;
  }
  CHECK_THAT(perimeter, WithinRel(140.0, 1e-6));
  CHECK_THAT(std::abs(area2) * 0.5, WithinRel(1200.0, 1e-6));
}

TEST_CASE("cam: depth passes land exactly on the requested depth", "[cam]") {
  cam::Operation op;
  op.topZ = 0.0;
  op.bottomZ = -10.0;
  op.stepdown = 3.0;
  const std::vector<double> zs = cam::DepthPasses(op);
  // -3, -6, -9, then -10: the last pass is SHORT, never deep. A pass 0.02 mm
  // past the requested depth is a scrapped part.
  REQUIRE(zs.size() == 4);
  CHECK_THAT(zs[0], WithinAbs(-3.0, 1e-9));
  CHECK_THAT(zs[2], WithinAbs(-9.0, 1e-9));
  CHECK_THAT(zs.back(), WithinAbs(-10.0, 1e-9));
  for (double z : zs)
    CHECK(z >= -10.0 - 1e-9);

  // No stepdown = one pass at depth.
  op.stepdown = 0.0;
  CHECK(cam::DepthPasses(op).size() == 1);
  // Stock to leave lifts the floor.
  op.stockToLeave = 0.5;
  CHECK_THAT(cam::DepthPasses(op).back(), WithinAbs(-9.5, 1e-9));
}

TEST_CASE("cam: an outside profile runs one tool radius clear of the part",
          "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 0.0;
  op.stepdown = 0.0; // one pass, so the length is checkable by hand
  op.side = cam::Side::Outside;
  setup.operations.push_back(op);

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  INFO(tp.error);
  REQUIRE(tp.error.empty());
  REQUIRE_FALSE(tp.Empty());

  // A 40x30 rectangle offset outward by r=3 with rounded corners is
  // 2*(40+30) + 2*pi*3 = 158.85. The tool travels that TWICE: once ramping
  // down (20 mm of descent at 3 degrees wants 381 mm of path, so it clamps
  // to one lap) and once cutting at depth.
  const double lap = 140.0 + 2.0 * std::numbers::pi * 3.0;
  // 1%: the ramp ends mid-segment and the lap restarts there, so the seam
  // adds a fraction of a segment to the total. Worth a look someday; not
  // worth a wrong number in a test today.
  CHECK_THAT(tp.CutLength(), WithinRel(2.0 * lap, 1e-2));

  // It never dives below the requested depth, and it starts and ends safe.
  CHECK_THAT(tp.MinZ(), WithinAbs(0.0, 1e-9));
  CHECK(tp.moves.front().kind == cam::Move::Kind::Rapid);
  CHECK_THAT(tp.moves.front().to.z, WithinAbs(setup.safeZ, 1e-9));
  CHECK_THAT(tp.moves.back().to.z, WithinAbs(setup.safeZ, 1e-9));
  // Entry RAMPS rather than plunging: a vertical plunge with a
  // non-centre-cutting end mill rubs instead of cutting. So the tool rapids
  // down to the stock top (air), then descends along the path.
  //
  // 21, not 20. This asserted 20 and called it the stock top; 20 is the
  // operation's topZ, and the stock top is a millimetre above it (the block
  // is 20 tall and the stock carries 1 mm over it). The rapid stops at the
  // real stock top now — the lowest height that cannot hold material — and
  // everything below is fed.
  CHECK(tp.moves[1].kind == cam::Move::Kind::Rapid);
  CHECK_THAT(tp.moves[1].to.z, WithinAbs(21.0, 1e-9)); // stock top, not depth
  CHECK(tp.moves[2].kind == cam::Move::Kind::Cut);
  CHECK(tp.moves[2].to.z < 21.0); // descending...
  CHECK(tp.moves[2].to.z > 0.0);  // ...but not all the way in one move
  CHECK_THAT(tp.moves[2].feed, WithinRel(200.0, 1e-9)); // at plunge feed
  // No vertical plunge anywhere in a profile.
  for (const cam::Move& m : tp.moves)
    CHECK(m.kind != cam::Move::Kind::Plunge);
}

TEST_CASE("cam: an inside profile with nothing to cut inside of refuses",
          "[cam]") {
  // This used to offset the PART'S OWN outline inward by a tool radius and
  // cut a trench a whisker inside the perimeter, full depth, all the way
  // round — a 15 mm gouge, found by asking for a slot this way in the first
  // dogfooding round (docs/cam/dogfood-round.md finding 4). An inside profile
  // needs a contour to be inside OF.
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 2;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 10.0;
  op.side = cam::Side::Inside;
  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  CHECK(tp.Empty());
  REQUIRE_FALSE(tp.error.empty());
  INFO(tp.error);
  CHECK(tp.error.find("pick the wall") != std::string::npos);
}

TEST_CASE("cam: climb and conventional run opposite ways", "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 3;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 15.0;

  const auto signedArea = [](const cam::Toolpath& tp) {
    double a2 = 0.0;
    std::vector<glm::dvec2> pts;
    for (const cam::Move& m : tp.moves)
      if (m.kind == cam::Move::Kind::Cut)
        pts.push_back({m.to.x, m.to.y});
    for (size_t i = 0; i + 1 < pts.size(); i++)
      a2 += pts[i].x * pts[i + 1].y - pts[i + 1].x * pts[i].y;
    return a2;
  };

  op.climb = true;
  const double climbArea =
      signedArea(cam::Generate(setup, op, doc->GetBodies()));
  op.climb = false;
  const double convArea =
      signedArea(cam::Generate(setup, op, doc->GetBodies()));
  // Opposite senses. Which one is "clockwise" is a convention the docs state;
  // that they DIFFER is the thing a test can hold.
  CHECK(climbArea * convArea < 0.0);
  // Climb on an outside profile is clockwise seen from +Z = negative area.
  CHECK(climbArea < 0.0);
}

TEST_CASE("cam: a tab is as tall as it was asked to be", "[cam]") {
  // Tabs used to be emitted on the LAST pass only, so any pass that already
  // ran through the tab region took the top off it: a 3 mm tab came out 1 mm
  // tall. That is a part that breaks free under the cutter and is thrown —
  // the one thing tabs exist to stop. Found by the second dogfooding round,
  // as four 16 mm2 patches of "leftover" 1 mm deep where 3.8 mm was asked
  // for (docs/cam/dogfood-round.md).
  auto doc = BlockDoc(); // 40 x 30 x 20, stock -2..42 x -2..32 x -1..21
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 4;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 5.0; // 16, 11, 6, 1, -1 — the z=1 pass is INSIDE the tab
  op.side = cam::Side::Outside;
  op.tabCount = 4;
  op.tabWidth = 6.0;
  op.tabHeight = 3.0; // so the tabs stand to z = 2
  setup.operations.push_back(op);
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  INFO(tp.error);
  REQUIRE(tp.error.empty());
  CHECK_THAT(tp.MinZ(), WithinAbs(-1.0, 1e-9));

  // The height the tabs actually reach, measured on the simulated stock
  // rather than on the moves: material left OUTSIDE the part is either a tab
  // or a mistake.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, {tp}, {}, gcode, &error));
  const cam::Inspection insp =
      cam::Inspect(gcode, setup, doc->GetBodies(), 0.25);
  REQUIRE(insp.ok);
  double tallest = -100.0;
  for (int j = 0; j < insp.machined.ny; j++)
    for (int i = 0; i < insp.machined.nx; i++) {
      const double x = insp.machined.CellX(i), y = insp.machined.CellY(j);
      if (x > -0.5 && x < 40.5 && y > -0.5 && y < 30.5)
        continue; // on the part, where the tool never went
      tallest = std::max(tallest, static_cast<double>(insp.machined.At(i, j)));
    }
  // 2.0 = bottomZ + tabHeight. It used to come out at 1.0, the depth of the
  // last pass that ran through the tab.
  CHECK_THAT(tallest, WithinAbs(2.0, 0.3));
}

TEST_CASE("cam: refusals are specific, not silent", "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 5;
  op.type = cam::OpType::Profile;
  op.toolNumber = 7; // no such tool
  op.topZ = 20.0;
  op.bottomZ = 0.0;
  cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  CHECK(tp.Empty());
  CHECK(tp.error.find("tool 7") != std::string::npos);

  op.toolNumber = 1;
  op.bottomZ = 25.0; // above the top
  tp = cam::Generate(setup, op, doc->GetBodies());
  CHECK(tp.Empty());
  CHECK(tp.error.find("below its top") != std::string::npos);

  op.type = cam::OpType::Drill;
  op.bottomZ = 0.0;
  op.targets.clear();
  tp = cam::Generate(setup, op, doc->GetBodies());
  CHECK(tp.error.find("target holes") != std::string::npos);
}

TEST_CASE("cam: a setup survives a save and reload", "[cam]") {
  // Setups are document objects (v58). They describe how to cut the model and
  // change nothing about it, so they must NOT touch the regen chain hash — a
  // new operation rebuilding the geometry would be a bug with a long tail.
  auto doc = BlockDoc();
  const size_t hashBefore = doc->TipChainHash();

  cam::Setup& setup = doc->AddCamSetup("Side A");
  setup.post = "linuxcnc";
  setup.safeZ = 12.5;
  setup.tools[0].diameter = 3.175;
  setup.tools[0].feedCut = 450.0;
  cam::Operation op;
  op.id = doc->AllocateId();
  op.name = "Outline";
  op.type = cam::OpType::Profile;
  op.topZ = 20.0;
  op.bottomZ = 0.0;
  op.stepdown = 4.0;
  op.tabCount = 3;
  op.targets.push_back(ElementRef{1, "F:box.side"});
  setup.operations.push_back(op);

  CHECK(doc->TipChainHash() == hashBefore); // geometry untouched

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_roundtrip.json";
  REQUIRE(DocumentIO::Save(*doc, path.string()).has_value());
  auto back = DocumentIO::Load(path.string());
  REQUIRE(back.has_value());
  const std::vector<cam::Setup>& setups = (*back)->GetCamSetups();
  REQUIRE(setups.size() == 1);
  CHECK(setups[0].name == "Side A");
  CHECK(setups[0].post == "linuxcnc");
  CHECK_THAT(setups[0].safeZ, WithinAbs(12.5, 1e-9));
  REQUIRE(setups[0].tools.size() == 1);
  CHECK_THAT(setups[0].tools[0].diameter, WithinAbs(3.175, 1e-9));
  REQUIRE(setups[0].operations.size() == 1);
  const cam::Operation& backOp = setups[0].operations[0];
  CHECK(backOp.type == cam::OpType::Profile);
  CHECK(backOp.tabCount == 3);
  CHECK_THAT(backOp.stepdown, WithinAbs(4.0, 1e-9));
  REQUIRE(backOp.targets.size() == 1);
  CHECK(backOp.targets[0].name == "F:box.side");
  std::filesystem::remove(path);
}

TEST_CASE("cam: the post emits a program that starts and ends safely",
          "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 18.0;
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));

  // The preamble puts the machine in a known state — the absence of this is
  // how a job runs in inches. G91.1 is there because the post fits arcs, and
  // an arc centre read in the wrong mode is a full-radius swing across the
  // table (C7).
  CHECK(gcode.find("G90 G91.1 G21 G17 G40 G49 G80") != std::string::npos);
  CHECK(gcode.find("G54") != std::string::npos);
  CHECK(gcode.find("T1 M6") != std::string::npos);
  CHECK(gcode.find("S12000 M3") != std::string::npos);
  // Mach3 has no tool table: the post pauses for a manual change.
  CHECK(gcode.find("M0 (change tool") != std::string::npos);
  CHECK(gcode.find("M5") != std::string::npos);
  CHECK(gcode.find("M30") != std::string::npos);
  // Feed appears on cutting moves, and modally — not on every line.
  CHECK(gcode.find("F600") != std::string::npos);
  CHECK(gcode.find("F200") != std::string::npos);

  // No coordinate is ever "nan". The first retract happens before anything
  // has moved, and passing the modal X/Y through emitted "G0 Xnan Ynan Z25"
  // — a line a controller may reject, or worse, interpret. Caught by reading
  // a posted file, so it is a test now.
  CHECK(gcode.find("nan") == std::string::npos);
  CHECK(gcode.find("inf") == std::string::npos);

  // LinuxCNC differs where it should and nowhere else. (`linux` is a
  // predefined GCC macro, hence the name.)
  std::string lcnc;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, lcnc, &error));
  CHECK(lcnc.find("M2") != std::string::npos);
  CHECK(lcnc.find("M0 (change tool") == std::string::npos);

  // An unknown post names the ones that exist rather than failing blankly.
  std::string none;
  CHECK_FALSE(cam::PostProcess("haas", setup, paths, {}, none, &error));
  CHECK(error.find("mach3") != std::string::npos);
}

TEST_CASE("cam: a failed operation is skipped LOUDLY in the file", "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Toolpath bad;
  bad.operationId = 9;
  bad.toolNumber = 1;
  bad.error = "targets vanished";
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, {bad}, {}, gcode, &error));
  // A part that comes off the machine missing a feature nobody noticed is
  // the failure this prevents.
  CHECK(gcode.find("SKIPPED op 9: targets vanished") != std::string::npos);
}

// --- C5: verifying the posted file ---------------------------------------

TEST_CASE("cam verify: the parser reads back what the post wrote", "[cam]") {
  const cam::GProgram p = cam::ParseGcode("(a comment with G1 X99 inside)\n"
                                          "G90 G21\n"
                                          "G0 X10 Y20 Z5\n"
                                          "G1 Z-1 F300\n"
                                          "X30\n" // modal G1, modal Y
                                          "G0 Z5 ; trailing comment\n");
  REQUIRE(p.errors.empty());
  REQUIRE(p.moves.size() == 4);
  CHECK(p.moves[0].rapid);
  CHECK_THAT(p.moves[0].to.x, WithinAbs(10.0, 1e-9));
  CHECK_FALSE(p.moves[1].rapid);
  CHECK_THAT(p.moves[1].to.z, WithinAbs(-1.0, 1e-9));
  // Modal: X30 keeps G1, keeps Y20 and Z-1.
  CHECK_FALSE(p.moves[2].rapid);
  CHECK_THAT(p.moves[2].to.x, WithinAbs(30.0, 1e-9));
  CHECK_THAT(p.moves[2].to.y, WithinAbs(20.0, 1e-9));
  CHECK_THAT(p.moves[2].to.z, WithinAbs(-1.0, 1e-9));

  // The NaN line that shipped once: an axis word with no number is reported,
  // not skipped. This is the check that catches a broken post.
  const cam::GProgram bad = cam::ParseGcode("G0 Xnan Ynan Z20\n");
  CHECK_FALSE(bad.errors.empty());
}

TEST_CASE("cam verify: a good job leaves neither gouge nor stock", "[cam]") {
  // The closed loop, on the POSTED FILE: post → parse → simulate → compare
  // against the model. Independent of the toolpath generator, so a bug in
  // the post or in modal state is caught here and nowhere else.
  auto doc = BlockDoc(40, 30, 10);
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  // topZ is the STOCK top (part top 10 + 1 mm margin), not the part top:
  // starting at the part top means the entry rapid drops into the margin
  // still above it. The verifier caught exactly that and reported a rapid
  // through material — a true finding about the job, not about the code.
  op.topZ = 11.0;
  op.bottomZ = 0.0;
  op.stepdown = 5.0;
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));

  const cam::Verdict v =
      cam::VerifyGcode(gcode, setup, doc->GetBodies(), /*res=*/0.4);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover << " rapids "
                << v.rapidsThroughStock);
  REQUIRE(v.cells > 0);
  // Profiling the outside cuts the margin away and must not touch the part.
  CHECK(v.maxGouge < 0.5); // one cell of quantisation at 0.4 mm
  CHECK(v.rapidsThroughStock == 0);
}

TEST_CASE("cam verify: a cut that is too deep is REPORTED", "[cam]") {
  // The test that gives the loop its value: hand-written G-code that plunges
  // through the middle of the part. A geometry test on our own toolpath
  // cannot notice this; simulating the file does.
  auto doc = BlockDoc(40, 30, 10);
  cam::Setup setup = BasicSetup();
  const std::string gouging = "G90 G21\n"
                              "G0 X-15 Y0 Z20\n"
                              "G1 Z-5 F200\n" // straight through the block
                              "G1 X15 F600\n"
                              "G0 Z20\n";
  const cam::Verdict v =
      cam::VerifyGcode(gouging, setup, doc->GetBodies(), 0.4);
  CHECK(v.maxGouge > 4.0);  // it cut ~5 mm below the top face
  CHECK_FALSE(v.Safe(0.1)); // a gouge is never acceptable
  // And it says WHERE, so the report points at a place on the part.
  CHECK(std::abs(v.gougeAt.y) < 5.0);
}

TEST_CASE("cam verify: a rapid through the part is caught", "[cam]") {
  // The check a shape comparison cannot make: the part can come out perfect
  // and the tool still be broken.
  auto doc = BlockDoc(40, 30, 10);
  cam::Setup setup = BasicSetup();
  const std::string crash = "G90 G21\n"
                            "G0 X-25 Y0 Z2\n" // below the 10 mm stock top
                            "G0 X25 Y0\n"     // ...and straight across it
                            "G0 Z20\n";
  const cam::Verdict v = cam::VerifyGcode(crash, setup, doc->GetBodies(), 0.5);
  CHECK(v.rapidsThroughStock > 0);
  CHECK(v.firstBadLine > 0);
  CHECK_FALSE(v.Safe(1.0)); // nor is a rapid through material
}

TEST_CASE("cam verify: a program that cuts nothing is reported as such",
          "[cam]") {
  // An empty program removes nothing. "Leftover" is measured only where the
  // tool actually went — a pocket-only job leaves most of a block untouched
  // and that is CORRECT — so the signal for "an operation was skipped or
  // never added" is the untouched fraction, not a leftover height.
  auto doc = BlockDoc(40, 30, 10);
  cam::Setup setup = BasicSetup();
  const cam::Verdict v =
      cam::VerifyGcode("G90 G21\nM30\n", setup, doc->GetBodies(), 0.5);
  CHECK(v.untouchedFraction == 1.0);
  CHECK(v.cells == 0);      // nothing to compare: the tool never arrived
  CHECK(v.maxGouge == 0.0); // and certainly nothing was cut
  const std::vector<cam::VerdictLine> lines = cam::Explain(v, 0.5, 0.0);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].text.find("cuts nothing") != std::string::npos);
}

// --- G2: the verbs, and the depths a click derives ------------------------

namespace {

// A 40 x 30 x 20 block with a 20 x 14 pocket cleared from its top down to
// z = 12 — the part the GUI's "Clear a pocket" verb is aimed at.
std::unique_ptr<Document> PocketDoc(bool island = false) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(10, 8, 12), 20, 14, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", block, voidId,
                                                   BooleanOp::Subtract));
  if (island) {
    const uint64_t pocket = doc->GetFeature(2)->GetId();
    doc->AddFeature(
        std::make_unique<BoxFeature>("Boss", glm::vec3(18, 14, 12), 5, 5, 8));
    const uint64_t bossId = doc->GetFeature(3)->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("WithBoss", pocket, bossId,
                                                     BooleanOp::Union));
  }
  return doc;
}

// A 120 x 90 x 20 block with an 80 x 60 pocket down to z = 12, and whichever
// inner wires the test needs in that floor: a HOLE through it at (60, 45), a
// BOSS standing on it at (44, 34), or both. Big enough that a 6 mm cutter has
// somewhere to go — the 20 x 14 pocket above leaves a region narrower than
// the tool once anything stands in it.
std::unique_ptr<Document> FlangeDoc(double holeDia, bool boss) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 120, 90, 20));
  uint64_t solid = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(20, 15, 12), 80, 60, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", solid, voidId,
                                                   BooleanOp::Subtract));
  solid = doc->GetFeature(2)->GetId();
  if (boss) {
    doc->AddFeature(
        std::make_unique<BoxFeature>("Boss", glm::vec3(40, 30, 12), 8, 8, 6));
    const uint64_t bossId = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("WithBoss", solid, bossId,
                                                     BooleanOp::Union));
    solid = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
  }
  if (holeDia > 0.0) {
    doc->AddFeature(std::make_unique<CylinderFeature>(
        "Bore", glm::vec3(60, 45, -1), glm::vec3(0, 0, 1), holeDia * 0.5,
        30.0));
    const uint64_t bore = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("Holed", solid, bore,
                                                     BooleanOp::Subtract));
  }
  return doc;
}

// A 40 x 30 x 20 block with a 12 x 14 slot straight through it, closed on
// every side. The case a face pick cannot reach: a through opening has no
// floor, so its walls are the only thing to point at.
std::unique_ptr<Document> SlottedDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(14, 8, -1), 12, 14, 22));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Slot", block, voidId,
                                                   BooleanOp::Subtract));
  return doc;
}

// The vertical planar face whose centroid is nearest (x, y) — a click on a
// wall.
ElementRef WallNear(const Document& doc, double x, double y) {
  ElementRef out;
  double best = 1e18;
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (!b.shape.FacePlane(index, c, n, u))
        continue;
      if (std::abs(n.z) > 0.01f)
        continue;
      const double d = (c.x - x) * (c.x - x) + (c.y - y) * (c.y - y);
      if (d < best) {
        best = d;
        out = ElementRef{b.createdBy, name};
      }
    }
  }
  return out;
}

// The up-facing planar face whose centroid sits at height `z`. This is what
// the GUI hands the engine when the user clicks that face.
ElementRef UpFaceAt(const Document& doc, double z) {
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (!b.shape.FacePlane(index, c, n, u))
        continue;
      if (n.z < 0.99f || std::abs(c.z - z) > 1e-6)
        continue;
      return ElementRef{b.createdBy, name};
    }
  }
  return {};
}

// A vertical face of the block whose outward normal points along -X or +X —
// the wall an edge break follows.
ElementRef SideWall(const Document& doc) {
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (!b.shape.FacePlane(index, c, n, u))
        continue;
      if (std::abs(n.z) < 0.01f)
        return ElementRef{b.createdBy, name};
    }
  }
  return {};
}

// A tool of a given shape, otherwise the setup's own.
cam::Tool ShapedTool(cam::ToolType type, double diameter, double angle = 90.0) {
  cam::Tool t;
  t.number = 1;
  t.diameter = diameter;
  t.type = type;
  t.tipAngleDeg = angle;
  t.feedCut = 600.0;
  t.feedPlunge = 200.0;
  t.spindleRpm = 12000.0;
  return t;
}

// A 40 x 30 x 20 block with a 6 mm through bore at (20, 15) — the part a
// drill operation is aimed at.
std::unique_ptr<Document> DrilledDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::vec3(20, 15, 0), glm::vec3(0, 0, 1), 8.0, 20.0));
  const uint64_t bore = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Drilled", block, bore,
                                                   BooleanOp::Subtract));
  return doc;
}

// The vertical cylindrical face (the bore wall) — what list_elements hands
// an agent that picked the hole.
ElementRef BoreWall(const Document& doc) {
  for (const Body& b : doc.GetBodies()) {
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 o(0.0f), axis(0.0f), x(0.0f);
      if (!b.shape.FaceAxis(index, o, axis, x))
        continue;
      if (std::abs(axis.z) > 0.99f)
        return ElementRef{b.createdBy, name};
    }
  }
  return {};
}

} // namespace

TEST_CASE("cam: a round opening is cut by picking its wall", "[cam]") {
  auto doc = DrilledDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef wall = BoreWall(*doc);
  REQUIRE(wall.IsSet());

  // R10: the pick every round hole has. FacePlane-based resolution used to
  // refuse it ("pick a flat face — that one is curved"), which left the
  // most common opening there is unmachinable by wall pick — the W 170 S
  // baffle needed an onion-skin twin part just to give its Ø73/Ø148
  // openings floors to pocket.
  const cam::DerivedOp derived =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Profile, wall);
  INFO(derived.why);
  REQUIRE(derived.ok);
  CHECK(derived.op.side == cam::Side::Inside); // an opening cuts inside it
  CHECK(derived.op.name == "Cut the opening");
  CHECK_THAT(derived.op.topZ, WithinAbs(21.0, 1e-9));
  CHECK(derived.op.bottomZ < 0.0); // through: carries into the spoilboard

  cam::Setup job = setup;
  cam::Operation op = derived.op;
  op.id = 1;
  job.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(job, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  CHECK(paths[0].error.empty());
  CHECK(paths[0].CutLength() > 0.0);
  // The cutter's centre runs one tool radius INSIDE the Ø16 bore: a circle
  // of radius 5 around (20, 15). Spot-check every cutting move.
  for (const cam::Move& mv : paths[0].moves) {
    if (mv.kind == cam::Move::Kind::Rapid)
      continue;
    const double r = std::hypot(mv.to.x - 20.0, mv.to.y - 15.0);
    CHECK(r < 5.0 + 1e-3);
  }
}

TEST_CASE("cam: a drill derives its depths from the hole it makes", "[cam]") {
  auto doc = DrilledDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef wall = BoreWall(*doc);
  REQUIRE(wall.IsSet());

  const cam::DerivedOp derived =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Drill, wall);
  INFO(derived.why);
  REQUIRE(derived.ok);
  // Top from the STOCK (a typed part-top once sent entry rapids through the
  // millimetre of stock above the part); bottom from the hole's own extent.
  CHECK_THAT(derived.op.topZ, WithinAbs(21.0, 1e-9));
  CHECK_THAT(derived.op.bottomZ, WithinAbs(0.0, 1e-4)); // OCC bbox gap
  REQUIRE(derived.op.targets.size() == 1);
  CHECK(derived.op.targets[0].name == wall.name);

  // No target is a teaching refusal, not "not a verb" — that message fired
  // even when the caller HAD passed cylindrical targets.
  const cam::DerivedOp none = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Drill, ElementRef{});
  REQUIRE_FALSE(none.ok);
  CHECK(none.why.find("pass targets") != std::string::npos);

  // A flat face is refused as the wrong KIND, naming where to look.
  const cam::DerivedOp flat = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Drill, UpFaceAt(*doc, 20.0));
  REQUIRE_FALSE(flat.ok);
  CHECK(flat.why.find("cylind") != std::string::npos);
}

TEST_CASE("cam: clicking a pocket floor derives every depth", "[cam]") {
  auto doc = PocketDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());

  const cam::DerivedOp derived =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, floor);
  INFO(derived.why);
  REQUIRE(derived.ok);
  // Nothing here was typed: the floor is the face, the top is the stock, the
  // stepdown is half the cutter. This is the `2 -> -0` failure made
  // unexpressible (docs/cam/gui.md).
  CHECK_THAT(derived.op.bottomZ, WithinAbs(12.0, 1e-9));
  CHECK_THAT(derived.op.topZ, WithinAbs(21.0, 1e-9)); // part top + 1 mm stock
  CHECK_THAT(derived.op.stepdown, WithinAbs(3.0, 1e-9)); // half of a 6 mm tool
  REQUIRE(derived.op.targets.size() == 1);
  CHECK(derived.op.targets[0].name == floor.name);

  cam::Setup job = setup;
  cam::Operation op = derived.op;
  op.id = 1;
  job.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(job, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  CHECK(paths[0].error.empty());
  CHECK(paths[0].CutLength() > 0.0);
  // It stops ON the floor it was told to clear to, not below it.
  CHECK_THAT(paths[0].MinZ(), WithinAbs(12.0, 1e-6));
}

TEST_CASE("cam: a derived pocket clears its own boundary, not the model's",
          "[cam]") {
  // The distinction the verb exists for: the section-derived pocket clears
  // whatever void the outline encloses, while a clicked floor clears THAT
  // pocket. Here they differ because the tool is asked for one of two voids.
  auto doc = PocketDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  cam::Setup job = setup;
  cam::Operation op =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, floor)
          .op;
  op.id = 1;
  job.operations.push_back(op);
  const cam::Toolpath tp = cam::GenerateAll(job, doc->GetBodies())[0];

  // Every cutting move stays inside the 20 x 14 pocket, one tool radius in.
  for (size_t i = 1; i < tp.moves.size(); i++) {
    if (tp.moves[i].kind != cam::Move::Kind::Cut)
      continue;
    const glm::dvec3& p = tp.moves[i].to;
    INFO("cut at " << p.x << "," << p.y);
    CHECK(p.x >= 10.0 + 3.0 - 1e-3);
    CHECK(p.x <= 30.0 - 3.0 + 1e-3);
    CHECK(p.y >= 8.0 + 3.0 - 1e-3);
    CHECK(p.y <= 22.0 - 3.0 + 1e-3);
  }
}

TEST_CASE("cam: a cut too narrow for the cutter says SO, with the numbers",
          "[cam]") {
  // The first failure a beginner hits, and the one they had no way to
  // diagnose: the region closes up under the first inward offset, every depth
  // silently skips, and the operation lands on the generic "produced no
  // toolpath — check its depths against the model" — which blames the depths
  // of a job whose depths are right and whose cutter is too fat.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  // A 4 mm wide slot: narrower than the 6 mm tool BasicSetup carries.
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(10, 13, 12), 20, 4, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", block, voidId,
                                                   BooleanOp::Subtract));

  cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());
  cam::Operation op;
  op.id = 11;
  op.type = cam::OpType::Pocket;
  op.toolNumber = 1; // 6 mm
  op.topZ = 20.0;
  op.bottomZ = 12.0;
  op.targets.push_back(floor);

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  REQUIRE(tp.Empty());
  INFO(tp.error);
  // What it IS, what the tool is, and what would work — the three things a
  // person needs to act. Not "check its depths".
  CHECK(tp.error.find("4 mm across") != std::string::npos);
  CHECK(tp.error.find("tool 1 is 6 mm") != std::string::npos);
  CHECK(tp.error.find("cannot reach in") != std::string::npos);
  CHECK(tp.error.find("depths") == std::string::npos);

  // A 3 mm cutter fits the same slot and cuts it.
  cam::Tool small;
  small.number = 2;
  small.diameter = 3.0;
  setup.tools.push_back(small);
  op.toolNumber = 2;
  const cam::Toolpath fits = cam::Generate(setup, op, doc->GetBodies());
  INFO(fits.error);
  CHECK_FALSE(fits.Empty());
  CHECK(fits.error.empty());
}

TEST_CASE("cam: stock to leave can be what does not fit, and says so",
          "[cam]") {
  // The tool fits the slot; the tool PLUS the stock being left does not. Two
  // different problems with two different fixes, so they get two messages —
  // telling someone to buy a smaller cutter when they should turn down
  // stock_to_leave is a wasted afternoon.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(10, 12, 12), 20, 6.5, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", block, voidId,
                                                   BooleanOp::Subtract));

  const cam::Setup setup = BasicSetup(); // 6 mm tool
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());
  cam::Operation op;
  op.id = 12;
  op.type = cam::OpType::Pocket;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 12.0;
  op.stockToLeave = 1.0; // 6 + 2 x 1 = 8 mm of room needed in a 6.5 mm slot
  op.targets.push_back(floor);

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  REQUIRE(tp.Empty());
  INFO(tp.error);
  CHECK(tp.error.find("stock") != std::string::npos);
  CHECK(tp.error.find("8 mm of room") != std::string::npos);
  // NOT the "buy a smaller cutter" message: this cutter is fine.
  CHECK(tp.error.find("cannot reach in") == std::string::npos);
}

TEST_CASE("cam: adaptive is silent about a shape it cannot enter — we are not",
          "[cam]") {
  // The path a person actually takes: a derived pocket deep enough comes back
  // as type ADAPTIVE, and adaptive returns no moves and no complaint for a
  // region its cutter cannot enter. Without measuring the width here, the one
  // failure a beginner hits first reads as "check its depths".
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(10, 13, 8), 20, 4, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", block, voidId,
                                                   BooleanOp::Subtract));

  const cam::Setup setup = BasicSetup(); // 6 mm tool
  const ElementRef floor = UpFaceAt(*doc, 8.0);
  REQUIRE(floor.IsSet());
  cam::Operation op;
  op.id = 14;
  op.type = cam::OpType::Adaptive;
  op.side = cam::Side::Inside;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 8.0;
  op.targets.push_back(floor);

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  REQUIRE(tp.Empty());
  INFO(tp.error);
  CHECK(tp.error.find("4 mm across") != std::string::npos);
  CHECK(tp.error.find("depths") == std::string::npos);
}

TEST_CASE("cam: an elliptical opening can be cut out", "[cam]") {
  // Openings are not all round. The section-loop machinery that follows a
  // wall is shape-agnostic — it works on polygons out of a Z section — so the
  // only thing that ever decided which openings could be machined was the
  // PICK, which admitted a planar vertical face or a vertical cylinder and
  // nothing else. An extruded ellipse is a surface of extrusion, so it was
  // refused with "pick a wall — a flat vertical face or a vertical cylinder".
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 200, 150, 20));
  auto sk = std::make_unique<SketchFeature>("Cut", SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Ellipse{glm::vec2(100.0f, 75.0f), 40.0, 22.0, 0.0});
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(1)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Opening", sid, 30.0, ExtrudeMode::Cut));
  REQUIRE(doc.GetBodies().size() == 1);

  // The elliptical wall: the one face that is neither planar nor cylindrical.
  ElementRef wall;
  for (const Body& b : doc.GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (b.shape.FacePlane(index, c, n, u))
        continue; // planar: the plate's own sides and faces
      wall = ElementRef{b.createdBy, name};
      break;
    }
  REQUIRE(wall.IsSet());

  const cam::WallContour contour =
      cam::PickedWallContour(doc.GetBodies(), wall, 0.02);
  INFO(contour.why);
  REQUIRE(contour.ok);

  // And the elliptical wall now READS as a wall everywhere, which fixes the
  // message a person actually hits: clicking it with the pocket verb used to
  // stop at "pick a flat face — that one is curved" (true, and no help),
  // because the curved-face check ran before the wall guard could say the
  // useful thing.
  CHECK(cam::IsWallFace(doc.GetBodies(), wall));
  {
    cam::Setup probe = BasicSetup();
    const cam::DerivedOp asPocket =
        cam::DeriveOperation(probe, doc.GetBodies(), cam::OpType::Pocket, wall);
    INFO(asPocket.why);
    CHECK_FALSE(asPocket.ok);
    CHECK(asPocket.why.find("FLOOR") != std::string::npos);
    CHECK(asPocket.why.find("curved") == std::string::npos);
  }

  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc.GetBodies());
  cam::Operation op;
  op.id = 41;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Inside;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 5.0;
  op.targets.push_back(wall);
  setup.operations.push_back(op);

  const cam::Toolpath tp =
      cam::Generate(setup, setup.operations[0], doc.GetBodies());
  INFO(tp.error);
  REQUIRE(!tp.Empty());
  CHECK(tp.error.empty());

  // And it followed the ELLIPSE: the cut path stays a tool radius inside a
  // 80 x 44 opening, so its extents are the ellipse less the cutter, not a
  // circle and not the bounding box.
  double lox = 1e9, hix = -1e9, loy = 1e9, hiy = -1e9;
  for (const cam::Move& m : tp.moves)
    if (m.kind != cam::Move::Kind::Rapid) {
      lox = std::min(lox, m.to.x);
      hix = std::max(hix, m.to.x);
      loy = std::min(loy, m.to.y);
      hiy = std::max(hiy, m.to.y);
    }
  INFO("path spans x " << lox << ".." << hix << "  y " << loy << ".." << hiy);
  CHECK_THAT(hix - lox, WithinAbs(80.0 - 6.0, 1.0)); // major less the tool
  CHECK_THAT(hiy - loy, WithinAbs(44.0 - 6.0, 1.0)); // minor less the tool
}

TEST_CASE("cam: a splined opening can be cut out too", "[cam]") {
  // Same gate, the other curve a sketch can draw. Nothing here is special to
  // ellipses — the wall test asks whether the face stands vertical, not what
  // curve swept it.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 200, 150, 20));
  auto sk = std::make_unique<SketchFeature>("Cut", SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::BSpline{{{70.0f, 60.0f},
                                 {95.0f, 45.0f},
                                 {130.0f, 65.0f},
                                 {135.0f, 95.0f},
                                 {100.0f, 105.0f},
                                 {70.0f, 90.0f},
                                 {70.0f, 60.0f}},
                                3});
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(1)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("Opening", sid, 30.0, ExtrudeMode::Cut));
  REQUIRE(doc.GetBodies().size() == 1);

  ElementRef wall;
  for (const Body& b : doc.GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (b.shape.FacePlane(index, c, n, u))
        continue;
      wall = ElementRef{b.createdBy, name};
      break;
    }
  REQUIRE(wall.IsSet());
  const cam::WallContour contour =
      cam::PickedWallContour(doc.GetBodies(), wall, 0.02);
  INFO(contour.why);
  CHECK(contour.ok);
}

TEST_CASE("cam: a tapered wall is still refused", "[cam]") {
  // The generalisation asks the NORMAL, not the shape, and the whole point is
  // that it keeps every refusal the shape test made. A draughted side is the
  // one that matters: it is curved-ish, it is nearly vertical, and a 3-axis
  // contour cannot follow it — the depth you cut at changes where the wall is.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 60, 40, 20));
  const uint64_t block = doc.GetFeature(0)->GetId();
  // The -Y side of the block.
  const ElementRef side = WallNear(doc, 30.0, 0.0);
  REQUIRE(side.IsSet());
  doc.AddFeature(std::make_unique<DraftFeature>(
      "Taper", block, std::vector<ElementRef>{side}, 8.0, PlaneType::XY));
  REQUIRE(doc.GetBodies().size() == 1);

  // After the draft that face is a plane, but a TILTED one, so FindFace's
  // own |normal.z| test refuses it. Assert the refusal, whichever branch
  // produces it: what must never happen is a contour coming back.
  const ElementRef tapered = WallNear(doc, 30.0, 0.0);
  if (tapered.IsSet()) {
    const cam::WallContour contour =
        cam::PickedWallContour(doc.GetBodies(), tapered, 0.02);
    INFO(contour.why);
    if (contour.ok) {
      // It only counts as a pass if the face really did stay vertical (a
      // draft that OCC declined to apply).
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      REQUIRE(doc.GetBodies()[0].shape.FacePlane(
          *doc.GetBodies()[0].elementMap.Resolve(tapered.name), c, n, u));
      CHECK(std::abs(n.z) < 0.01f);
    }
  }
}

TEST_CASE("cam: a depth pass carries on from where the last one stopped",
          "[cam]") {
  // The lap deliberately overshoots the ramp so the ramped-down stretch still
  // gets cut at full depth — which means every pass ENDS a ramp-length past
  // where it began. A pass that then restarted at pts[0] had to lift, fly
  // back across the opening at safe height and drop again, once per pass:
  // visible as a fan of chords through the middle of every bore, and pure
  // air time on the machine.
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
  op.id = 31;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Inside;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 3.0; // several passes, so there are boundaries to check
  // The bore's wall is CYLINDRICAL, and WallNear only considers planar faces
  // — pointing it at the bore hands back one of the plate's outside walls
  // instead, which is a different job entirely and quietly makes this test
  // prove nothing.
  for (const Body& b : doc->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (b.shape.FacePlane(index, c, n, u))
        continue; // planar
      op.targets.push_back(ElementRef{b.createdBy, name});
      break;
    }
  REQUIRE(op.targets.size() == 1);
  setup.operations.push_back(op);

  const cam::Toolpath tp =
      cam::Generate(setup, setup.operations[0], doc->GetBodies());
  REQUIRE(!tp.Empty());

  // No rapid may travel in XY while inside the bore's footprint. A retract
  // straight up and a plunge straight down are fine; a flight ACROSS is the
  // defect. (The approach to the very first pass comes in from outside.)
  int crossings = 0;
  glm::dvec3 from(0.0);
  bool have = false;
  for (const cam::Move& m : tp.moves) {
    if (have && m.kind == cam::Move::Kind::Rapid) {
      const glm::dvec2 a(from), b(m.to);
      const double travel = glm::length(b - a);
      // Both ends inside the bore, and it moved: that is a chord across it.
      const glm::dvec2 centre(100.0, 75.0);
      if (travel > 1.0 && glm::length(a - centre) < 30.0 &&
          glm::length(b - centre) < 30.0)
        crossings++;
    }
    from = m.to;
    have = true;
  }
  INFO("rapid crossings of the opening: " << crossings);
  CHECK(crossings == 0);
}

TEST_CASE("cam: an opening too narrow to cut out says which, and how narrow",
          "[cam]") {
  // The same failure through the other verb: clicking the wall of a through
  // opening cuts INSIDE it, so the contour offsets inward and a slot narrower
  // than the cutter closes up. Before, this was the same misleading
  // "check its depths".
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  // 5 mm wide, right through — narrower than the 6 mm tool.
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(14, 12, -1), 12, 5, 22));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Slot", block, voidId,
                                                   BooleanOp::Subtract));

  const cam::Setup setup = BasicSetup();
  const ElementRef wall = WallNear(*doc, 14.0, 14.5);
  REQUIRE(wall.IsSet());
  cam::Operation op;
  op.id = 13;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Inside;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 0.0;
  op.targets.push_back(wall);

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  REQUIRE(tp.Empty());
  INFO(tp.error);
  CHECK(tp.error.find("opening") != std::string::npos);
  CHECK(tp.error.find("5 mm across") != std::string::npos);
  CHECK(tp.error.find("depths") == std::string::npos);
}

TEST_CASE("cam: an island in the picked floor is machined around", "[cam]") {
  auto doc = PocketDoc(/*island=*/true);
  const cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());
  const cam::FaceRegion region = cam::PickedFaceRegion(doc->GetBodies(), floor);
  REQUIRE(region.ok);
  // Outer wire plus the boss standing in it.
  REQUIRE(region.loops.size() == 2);
  CHECK_FALSE(region.loops[0].hole);
  CHECK(region.loops[1].hole);

  cam::Operation op =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, floor)
          .op;
  op.id = 1;
  // 9 mm deep with a 6 mm cutter, so the verb picks adaptive clearing (C8).
  // Both strategies have to respect the boss, so both are checked here —
  // with each one's own accuracy, not one number for both.
  if (cam::AdaptiveAvailable())
    CHECK(op.type == cam::OpType::Adaptive);

  // A 6 mm cutter whose centre came within 3 mm of the 5 x 5 boss would eat
  // it. Checked as a distance so a partial overlap fails too, not just a
  // centre inside the island.
  const auto clearsTheBoss = [&](const cam::Toolpath& tp, double tolerance) {
    for (const cam::Move& m : tp.moves) {
      if (m.kind == cam::Move::Kind::Rapid)
        continue;
      const double dx = std::max({18.0 - m.to.x, 0.0, m.to.x - 23.0});
      const double dy = std::max({14.0 - m.to.y, 0.0, m.to.y - 19.0});
      INFO("cut at " << m.to.x << "," << m.to.y);
      CHECK(std::hypot(dx, dy) > 3.0 - tolerance);
    }
  };

  cam::Setup job = setup;
  job.operations.push_back(op);
  const cam::Toolpath tp = cam::GenerateAll(job, doc->GetBodies())[0];
  REQUIRE(tp.error.empty());
  // Adaptive2d plans to a stated 0.02 mm; asking it for micron fidelity
  // would be measuring the tolerance rather than the island.
  clearsTheBoss(tp, op.type == cam::OpType::Adaptive ? 0.02 : 1e-3);

  // Concentric rings, which offset exactly, get the tight bound.
  cam::Operation rings = op;
  rings.type = cam::OpType::Pocket;
  cam::Setup ringJob = setup;
  ringJob.operations.push_back(rings);
  const cam::Toolpath ringPath = cam::GenerateAll(ringJob, doc->GetBodies())[0];
  REQUIRE(ringPath.error.empty());
  clearsTheBoss(ringPath, 1e-3);
}

TEST_CASE("cam: a pocket too small to ramp around descends on a helix",
          "[cam]") {
  // C6. A 14 x 14 pocket 9 mm deep with a 6 mm cutter: the first ring is
  // ~32 mm around, and 9 mm of descent at 3 degrees wants 172 mm of path. On
  // the ring that is a 16-degree dive; the helix keeps it at 3.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 40, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(13, 13, 11), 14, 14, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", block, voidId,
                                                   BooleanOp::Subtract));

  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  const ElementRef floor = UpFaceAt(*doc, 11.0);
  REQUIRE(floor.IsSet());
  cam::Operation op =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, floor)
          .op;
  op.id = 1;
  op.stepdown = 0.0; // one pass, so the whole descent is in one entry
  setup.operations.push_back(op);
  const cam::Toolpath tp = cam::GenerateAll(setup, doc->GetBodies())[0];
  INFO(tp.error);
  REQUIRE(tp.error.empty());

  // No plunge, and no cutting move steeper than the ramp angle.
  double steepest = 0.0;
  for (size_t i = 1; i < tp.moves.size(); i++) {
    CHECK(tp.moves[i].kind != cam::Move::Kind::Plunge);
    if (tp.moves[i].kind != cam::Move::Kind::Cut)
      continue;
    const glm::dvec3 d = tp.moves[i].to - tp.moves[i - 1].to;
    const double flat = glm::length(glm::dvec2(d));
    if (d.z < -1e-9 && flat > 1e-9)
      steepest = std::max(steepest, -d.z / flat);
  }
  INFO("steepest descent " << steepest);
  CHECK(steepest < std::tan(3.0 * std::numbers::pi / 180.0) * 1.05);
  // …and a descent HAPPENED. Without this the check above passes for a
  // toolpath that never went down at all, which is what a helix that
  // silently emitted nothing would produce.
  CHECK(steepest > 0.0);
  CHECK(tp.CutLength() > 0.0);
  // The tool goes down once and stays down: one retract to safe Z at the end
  // of the pass, not one per ring.
  int retracts = 0;
  for (const cam::Move& m : tp.moves)
    if (m.kind == cam::Move::Kind::Rapid &&
        std::abs(m.to.z - setup.safeZ) < 1e-9)
      retracts++;
  CHECK(retracts == 2); // over the helix start, and out at the end

  // And the cutter stayed inside the pocket: every cutting move keeps its
  // full radius off the wall. This is the check that catches a helix sized
  // from the wall instead of from the tool centre.
  for (const cam::Move& m : tp.moves) {
    if (m.kind == cam::Move::Kind::Rapid)
      continue;
    INFO("cut at " << m.to.x << "," << m.to.y);
    CHECK(m.to.x >= 13.0 + 3.0 - 1e-3);
    CHECK(m.to.x <= 27.0 - 3.0 + 1e-3);
    CHECK(m.to.y >= 13.0 + 3.0 - 1e-3);
    CHECK(m.to.y <= 27.0 - 3.0 + 1e-3);
  }
  CHECK_THAT(tp.MinZ(), WithinAbs(11.0, 1e-6));

  // A pocket with room to ramp does NOT get a helix: the entry is still the
  // along-the-path descent, which needs no clear circle in the middle.
  auto wide = PocketDoc(); // 20 x 14 pocket, ~64 mm around
  cam::Setup w = BasicSetup();
  w.safeZ = cam::DeriveSafeZ(w, wide->GetBodies());
  cam::Operation wop =
      cam::DeriveOperation(w, wide->GetBodies(), cam::OpType::Pocket,
                           UpFaceAt(*wide, 12.0))
          .op;
  wop.id = 1;
  w.operations.push_back(wop);
  const cam::Toolpath wtp = cam::GenerateAll(w, wide->GetBodies())[0];
  REQUIRE(wtp.error.empty());
  // The ramp entry retracts to safe Z before every ring; the helix path
  // makes exactly one entry per depth pass and then stays down.
  int entries = 0;
  for (const cam::Move& m : wtp.moves)
    if (m.kind == cam::Move::Kind::Rapid && std::abs(m.to.z - w.safeZ) < 1e-9)
      entries++;
  CHECK(entries > 2);
}

TEST_CASE("cam: a verb that cannot apply says why", "[cam]") {
  auto doc = PocketDoc();
  cam::Setup setup = BasicSetup();

  // Facing with the stock top already ON the part (which is what picking the
  // top face does) has nothing to remove.
  setup.stock.margin.z = 0.0;
  const cam::DerivedOp facing =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Face, {});
  CHECK_FALSE(facing.ok);
  CHECK(facing.why.find("no stock above the part") != std::string::npos);

  // With material above it, the same verb derives a facing pass down to the
  // part's own top.
  setup.stock.margin.z = 1.5;
  const cam::DerivedOp ok =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Face, {});
  REQUIRE(ok.ok);
  CHECK_THAT(ok.op.topZ, WithinAbs(21.5, 1e-9));
  CHECK_THAT(ok.op.bottomZ, WithinAbs(20.0, 1e-9));

  // A pocket needs a floor to clear to.
  const cam::DerivedOp nothing =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, {});
  CHECK_FALSE(nothing.ok);

  // And a setup with no tool cannot derive anything, because every depth
  // here is a function of the cutter.
  cam::Setup toolless;
  const cam::DerivedOp none = cam::DeriveOperation(
      toolless, doc->GetBodies(), cam::OpType::Pocket, UpFaceAt(*doc, 12.0));
  CHECK_FALSE(none.ok);
  CHECK(none.why.find("tool") != std::string::npos);
}

TEST_CASE("cam: cutting out the whole part needs no pick", "[cam]") {
  auto doc = BlockDoc(40, 30, 20);
  const cam::Setup setup = BasicSetup();
  const cam::DerivedOp derived =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Profile, {});
  REQUIRE(derived.ok);
  CHECK(derived.op.targets.empty());
  CHECK(derived.op.side == cam::Side::Outside);
  CHECK(derived.op.tabCount > 0); // or the part is thrown by the cutter
  // Through the stock, not down to the part's own bottom — the tolerance is
  // for OCC's bounding box, which is not exact by construction.
  CHECK_THAT(derived.op.bottomZ, WithinAbs(-1.0, 1e-4));
  CHECK_THAT(derived.op.tabWidth, WithinAbs(12.0, 1e-9)); // 2x the cutter
}

TEST_CASE("cam: the time estimate comes from the feeds, not a guess", "[cam]") {
  auto doc = BlockDoc(40, 30, 10);
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 10.0;
  op.bottomZ = 0.0;
  op.stepdown = 5.0;
  setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());

  double byHand = 0.0;
  for (const cam::Toolpath& tp : paths)
    for (size_t i = 1; i < tp.moves.size(); i++)
      if (tp.moves[i].feed > 0.0 && tp.moves[i].kind != cam::Move::Kind::Rapid)
        byHand +=
            glm::length(tp.moves[i].to - tp.moves[i - 1].to) / tp.moves[i].feed;
  CHECK_THAT(cam::CuttingMinutes(setup, paths), WithinRel(byHand, 1e-9));
  CHECK(byHand > 0.0);
  // Rapids are excluded until the setup states a rapid feed: the machine's
  // rapid rate is not ours to invent.
  setup.rapidFeed = 3000.0;
  CHECK(cam::CuttingMinutes(setup, paths) > byHand);
}

// --- G4: the verdict, in words --------------------------------------------

TEST_CASE("cam verify: the verdict is reported in words", "[cam]") {
  auto doc = BlockDoc(40, 30, 10);
  cam::Setup setup = BasicSetup();
  const std::string gouging = "G90 G21\n"
                              "G0 X-15 Y0 Z20\n"
                              "G1 Z-5 F200\n"
                              "G1 X15 F600\n"
                              "G0 Z20\n";
  const cam::Inspection insp =
      cam::Inspect(gouging, setup, doc->GetBodies(), 0.4);
  REQUIRE(insp.ok);
  REQUIRE(insp.verdict.maxGouge > 4.0);

  const std::vector<cam::VerdictLine> lines =
      cam::Explain(insp.verdict, 0.5, 2.0);
  REQUIRE_FALSE(lines.empty());
  // Most severe first, and it names the damage rather than a number the
  // reader has to interpret.
  CHECK(lines[0].level == cam::VerdictLine::Level::Bad);
  CHECK(lines[0].text.find("INTO the part") != std::string::npos);
  CHECK(lines[0].text.find("scrap") != std::string::npos);

  // The clean case says so plainly.
  cam::Verdict clean;
  clean.cells = 100;
  const std::vector<cam::VerdictLine> good = cam::Explain(clean, 0.1, 0.0);
  REQUIRE(good.size() == 1);
  CHECK(good[0].level == cam::VerdictLine::Level::Good);
  CHECK(good[0].text.find("Machines the part") != std::string::npos);

  // A rapid through material points at the line of the file.
  cam::Verdict crash;
  crash.cells = 100;
  crash.rapidsThroughStock = 1;
  crash.firstBadLine = 42;
  const std::vector<cam::VerdictLine> hit = cam::Explain(crash, 0.1, 0.0);
  REQUIRE_FALSE(hit.empty());
  CHECK(hit[0].text.find("line 42") != std::string::npos);
  CHECK(hit[0].text.find("Safe Z") != std::string::npos);
}

TEST_CASE("cam verify: leftover means unreachable, not unprogrammed", "[cam]") {
  // The distinction that decides whether the verdict is worth reading: a job
  // that machines one pocket in a big block must not report the rest of the
  // block as material it failed to remove. (It did exactly that on the first
  // run, and painted the whole part red.)
  auto doc = PocketDoc();
  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  cam::Operation op =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket,
                           UpFaceAt(*doc, 12.0))
          .op;
  op.id = 1;
  setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));

  const cam::Inspection insp =
      cam::Inspect(gcode, setup, doc->GetBodies(), 0.5);
  REQUIRE(insp.ok);
  INFO("leftover " << insp.verdict.maxLeftover << " untouched "
                   << insp.verdict.untouchedFraction);
  // Most of the block is untouched, and none of it is called leftover.
  CHECK(insp.verdict.untouchedFraction > 0.5);
  CHECK(insp.verdict.maxLeftover < 1.0);
  // Nor painted: the marks are for corners a cutter cannot reach.
  CHECK(cam::LeftoverPoints(insp, 0.75).size() < 200);
  // And the derived Safe Z clears the stock, so no rapid crosses it.
  CHECK(insp.verdict.rapidsThroughStock == 0);
}

TEST_CASE("cam verify: leftover comes back as points to paint", "[cam]") {
  // A pass that crosses the part but stops 0.5 mm short of the designed top:
  // the tool WENT there and did not finish, which is exactly what the paint
  // is for. (The stock top is 11; the part's is 10.)
  auto doc = BlockDoc(40, 30, 10);
  const cam::Setup setup = BasicSetup();
  const std::string shallow = "G90 G21\n"
                              "G0 X-25 Y15 Z20\n"
                              "G1 Z10.5 F200\n"
                              "G1 X65 F600\n"
                              "G0 Z20\n";
  const cam::Inspection insp =
      cam::Inspect(shallow, setup, doc->GetBodies(), 1.0);
  REQUIRE(insp.ok);
  const std::vector<glm::dvec3> pts = cam::LeftoverPoints(insp, 0.2, 4000);
  REQUIRE_FALSE(pts.empty());
  for (const glm::dvec3& p : pts) {
    CHECK_THAT(p.z, WithinAbs(10.5, 1e-6));    // where the cutter stopped
    CHECK(std::abs(p.y - 15.0) < 3.0 + 1.001); // inside the swept band
  }

  // The cap is honoured, and it samples the whole band rather than one end.
  const std::vector<glm::dvec3> few = cam::LeftoverPoints(insp, 0.2, 20);
  CHECK(few.size() <= 20);
  double minX = 1e9, maxX = -1e9;
  for (const glm::dvec3& p : few) {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
  }
  CHECK(maxX - minX > 20.0);

  // A part machined to size has nothing to paint.
  cam::Inspection none = insp;
  none.machined = none.designed;
  CHECK(cam::LeftoverPoints(none, 0.2).empty());
}

// --- C9: the indexed 4th axis ---------------------------------------------

TEST_CASE("cam: an indexed setup cuts the face that was turned up", "[cam]") {
  // The strongest test available for a rotary index: a part rotated in the
  // MODEL and machined at A=0 must produce the same toolpath as the
  // unrotated part machined by a setup indexed to the same angle. If the
  // setup frame is wrong, the two disagree.
  auto flat = BlockDoc(40, 30, 20);
  cam::Setup direct = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 10.0;
  op.stepdown = 0.0;
  direct.operations.push_back(op);
  const cam::Toolpath a = cam::GenerateAll(direct, flat->GetBodies())[0];
  REQUIRE(a.error.empty());

  // The same block, rotated 90 degrees about X in the model, cut by a setup
  // indexed 90 degrees the other way — which puts it back.
  auto turned = std::make_unique<Document>();
  turned->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  {
    // Rotate the body itself, so only the setup frame can undo it.
    std::vector<Body>& bodies = const_cast<std::vector<Body>&>(
        static_cast<const Document&>(*turned).GetBodies());
    glm::mat4 m(1.0f);
    m[1][1] = 0.0f;
    m[1][2] = 1.0f;
    m[2][1] = -1.0f;
    m[2][2] = 0.0f; // +90 about X
    for (Body& b : bodies)
      b.shape = b.shape.Transformed(m);
  }
  cam::Setup indexed = BasicSetup();
  // The machine has the axis, and is indexed on it. Both halves are needed:
  // a rotated FIXTURE never posts an A word (S3).
  indexed.hasRotaryAxis = true;
  indexed.rotationA = 90.0;
  indexed.operations.push_back(op);
  const cam::Toolpath b = cam::GenerateAll(indexed, turned->GetBodies())[0];
  INFO(b.error);
  REQUIRE(b.error.empty());

  CHECK_THAT(b.CutLength(), WithinRel(a.CutLength(), 1e-6));
  REQUIRE(b.moves.size() == a.moves.size());
  for (size_t i = 0; i < a.moves.size(); i++) {
    INFO("move " << i);
    CHECK_THAT(b.moves[i].to.x, WithinAbs(a.moves[i].to.x, 1e-6));
    CHECK_THAT(b.moves[i].to.y, WithinAbs(a.moves[i].to.y, 1e-6));
    CHECK_THAT(b.moves[i].to.z, WithinAbs(a.moves[i].to.z, 1e-6));
  }

  // Every move of an indexed setup carries its A, so the file states which
  // way the part is facing rather than assuming.
  for (const cam::Move& m : b.moves) {
    CHECK(m.hasA);
    CHECK_THAT(m.a, WithinAbs(90.0, 1e-9));
  }
  for (const cam::Move& m : a.moves)
    CHECK_FALSE(m.hasA); // a 3-axis job says nothing about A

  // And the posted file turns the table BEFORE it goes down.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", indexed, {b}, {}, gcode, &error));
  const size_t aWord = gcode.find("A90");
  REQUIRE(aWord != std::string::npos);
  const size_t firstCut = gcode.find("\nG1 ");
  REQUIRE(firstCut != std::string::npos);
  CHECK(aWord < firstCut);
  // …and the tool is at safe Z when it turns. Checked as the STATE at that
  // line rather than as "the previous line is a retract": the retract is
  // modal, so when the tool is already clear the post correctly emits
  // nothing, and asserting on the text would fail for the right reason.
  double modalZ = std::nan("");
  bool sawIndexAtSafeZ = false;
  size_t at = 0;
  while (at < gcode.size()) {
    const size_t end = std::min(gcode.find('\n', at), gcode.size());
    const std::string line = gcode.substr(at, end - at);
    at = end + 1;
    const size_t zAt = line.find('Z');
    if (zAt != std::string::npos && line[0] == 'G')
      modalZ = std::stod(line.substr(zAt + 1));
    if (line.find('A') != std::string::npos && line[0] == 'G') {
      INFO("index at Z " << modalZ);
      CHECK_THAT(modalZ, WithinAbs(indexed.safeZ, 1e-9));
      sawIndexAtSafeZ = true;
    }
  }
  CHECK(sawIndexAtSafeZ);
}

TEST_CASE("cam: an indexed job is verified in its own frame", "[cam]") {
  // The gap C9 opened and this closes: the posted file is in SETUP space, so
  // everything it is compared against has to be. Verifying an indexed job
  // against the unrotated part compares a program with a part that is not in
  // front of it — and reports a fictional gouge through the whole block.
  auto turned = std::make_unique<Document>();
  turned->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  {
    std::vector<Body>& bodies = const_cast<std::vector<Body>&>(
        static_cast<const Document&>(*turned).GetBodies());
    glm::mat4 m(1.0f);
    m[1][1] = 0.0f;
    m[1][2] = 1.0f;
    m[2][1] = -1.0f;
    m[2][2] = 0.0f; // +90 about X
    for (Body& b : bodies)
      b.shape = b.shape.Transformed(m);
  }

  cam::Setup indexed = BasicSetup();
  // The machine has the axis, and is indexed on it. Both halves are needed:
  // a rotated FIXTURE never posts an A word (S3).
  indexed.hasRotaryAxis = true;
  indexed.rotationA = 90.0;
  // Derived, not typed: the depths must come from the frame the cut happens
  // in, which is the other half of the same bug.
  indexed.safeZ = cam::DeriveSafeZ(indexed, turned->GetBodies());
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(indexed, turned->GetBodies(), lo, hi));
  op.topZ = hi.z;
  op.bottomZ = lo.z;
  op.stepdown = 8.0;
  indexed.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(indexed, turned->GetBodies());
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0].error.empty());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", indexed, paths, {}, gcode, &error));

  const cam::Verdict v =
      cam::VerifyGcode(gcode, indexed, turned->GetBodies(), 0.5);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover << " rapids "
                << v.rapidsThroughStock);
  REQUIRE(v.cells > 0);
  // Profiling outside the part removes the margin and must not touch it.
  CHECK(v.maxGouge < 0.6);
  CHECK(v.rapidsThroughStock == 0);
}

// --- S1-S3: the fixture frame is not a 4th axis ---------------------------

namespace {

// A block turned 90 degrees about X in the MODEL, so only the setup can put
// it back. Shared by the frame tests below.
std::unique_ptr<Document> TurnedBlockDoc() {
  auto turned = std::make_unique<Document>();
  turned->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  std::vector<Body>& bodies = const_cast<std::vector<Body>&>(
      static_cast<const Document&>(*turned).GetBodies());
  glm::mat4 m(1.0f);
  m[1][1] = 0.0f;
  m[1][2] = 1.0f;
  m[2][1] = -1.0f;
  m[2][2] = 0.0f; // +90 about X
  for (Body& b : bodies)
    b.shape = b.shape.Transformed(m);
  return turned;
}

cam::Operation ProfileOp() {
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 10.0;
  op.stepdown = 0.0;
  return op;
}

} // namespace

TEST_CASE("cam: a fixture frame cuts what a rotary index would", "[cam]") {
  // The composition order, pinned. A setup whose FRAME is 90 degrees about X
  // must cut exactly what a setup indexed to A=90 cuts: they are the same
  // orientation stated two ways, and the only difference is what the file
  // says about the machine (the next test). Getting the order wrong — the
  // index composed before the fixture rather than after — fails here.
  auto turned = TurnedBlockDoc();

  cam::Setup indexed = BasicSetup();
  indexed.hasRotaryAxis = true;
  indexed.rotationA = 90.0;
  indexed.operations.push_back(ProfileOp());
  const cam::Toolpath a = cam::GenerateAll(indexed, turned->GetBodies())[0];
  INFO(a.error);
  REQUIRE(a.error.empty());

  cam::Setup clamped = BasicSetup();
  // The same turn, as a fixture: the quaternion for +90 about X.
  const double h = glm::radians(90.0) * 0.5;
  clamped.frame = glm::dquat(std::cos(h), std::sin(h), 0.0, 0.0);
  clamped.operations.push_back(ProfileOp());
  const cam::Toolpath b = cam::GenerateAll(clamped, turned->GetBodies())[0];
  INFO(b.error);
  REQUIRE(b.error.empty());

  REQUIRE(b.moves.size() == a.moves.size());
  CHECK_THAT(b.CutLength(), WithinRel(a.CutLength(), 1e-9));
  for (size_t i = 0; i < a.moves.size(); i++) {
    INFO("move " << i);
    CHECK_THAT(b.moves[i].to.x, WithinAbs(a.moves[i].to.x, 1e-9));
    CHECK_THAT(b.moves[i].to.y, WithinAbs(a.moves[i].to.y, 1e-9));
    CHECK_THAT(b.moves[i].to.z, WithinAbs(a.moves[i].to.z, 1e-9));
  }
}

TEST_CASE("cam: clamping a part flat posts no rotary move", "[cam]") {
  // The bug this whole change exists for: laying a panel flat on the bed used
  // to be spelled `rotationA`, so a 3-axis router was handed `G0 A90`.
  auto turned = TurnedBlockDoc();
  cam::Setup clamped = BasicSetup();
  const double h = glm::radians(90.0) * 0.5;
  clamped.frame = glm::dquat(std::cos(h), std::sin(h), 0.0, 0.0);
  clamped.safeZ = cam::DeriveSafeZ(clamped, turned->GetBodies());
  clamped.operations.push_back(ProfileOp());

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(clamped, turned->GetBodies());
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0].error.empty());
  for (const cam::Move& m : paths[0].moves)
    CHECK_FALSE(m.hasA);

  for (const char* post : {"mach3", "linuxcnc"}) {
    INFO(post);
    std::string gcode, error;
    REQUIRE(cam::PostProcess(post, clamped, paths, {}, gcode, &error));
    // Not one A word, in any line, in either dialect.
    CHECK(gcode.find(" A") == std::string::npos);
    CHECK(gcode.find("\nA") == std::string::npos);
  }

  // And a machine that HAS the axis but is not indexed on it says nothing
  // either: the flag alone is not a reason to move a table.
  cam::Setup rotary = clamped;
  rotary.hasRotaryAxis = true;
  const std::vector<cam::Toolpath> rp =
      cam::GenerateAll(rotary, turned->GetBodies());
  for (const cam::Move& m : rp[0].moves)
    CHECK_FALSE(m.hasA);
}

TEST_CASE("cam: a clamped part verifies in the frame it was cut in", "[cam]") {
  // The verifier frames its own bodies, so this should pass on arrival — and
  // if the frame composes the wrong way it reports a part cut in mid-air.
  auto turned = TurnedBlockDoc();
  cam::Setup clamped = BasicSetup();
  const double h = glm::radians(90.0) * 0.5;
  clamped.frame = glm::dquat(std::cos(h), std::sin(h), 0.0, 0.0);
  clamped.safeZ = cam::DeriveSafeZ(clamped, turned->GetBodies());

  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(clamped, turned->GetBodies(), lo, hi));
  op.topZ = hi.z;
  op.bottomZ = lo.z;
  op.stepdown = 8.0;
  clamped.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(clamped, turned->GetBodies());
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0].error.empty());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", clamped, paths, {}, gcode, &error));

  const cam::Verdict v =
      cam::VerifyGcode(gcode, clamped, turned->GetBodies(), 0.5);
  INFO("gouge " << v.maxGouge << " rapids " << v.rapidsThroughStock);
  REQUIRE(v.cells > 0);
  CHECK(v.maxGouge < 0.6);
  CHECK(v.rapidsThroughStock == 0);
}

TEST_CASE("cam: any flat face can be laid up, and twice is once", "[cam]") {
  const glm::dquat id(1.0, 0.0, 0.0, 0.0);

  // A face already under the spindle asks for no change at all. Without the
  // idempotence the pick would drift the part a little on every click.
  CHECK(glm::length(cam::SolveFaceUp({0.0, 0.0, 1.0}, id) - id) < 1e-12);

  // Every direction works, including the two `SolveIndexA` must refuse: a
  // clamp is not a trunnion, and a panel standing on its edge is the case
  // that started this (docs/cam/setup-frame.md).
  const std::vector<glm::dvec3> normals = {
      {0.0, 1.0, 0.0},  {0.0, -1.0, 0.0}, {1.0, 0.0, 0.0},
      {-1.0, 0.0, 0.0}, {0.0, 0.0, -1.0}, {0.6, -0.48, 0.64}};
  for (const glm::dvec3& n : normals) {
    INFO("normal " << n.x << " " << n.y << " " << n.z);
    const glm::dquat f = cam::SolveFaceUp(n, id);
    // The frame maps the spindle to the face, so undoing it lays the face
    // flat: that is the whole contract, and it is what the pipeline relies on.
    const glm::dvec3 up =
        glm::transpose(glm::dmat3(glm::mat3_cast(f))) * glm::normalize(n);
    CHECK_THAT(up.z, WithinAbs(1.0, 1e-12));
    // Asking again for the same face, from the frame it produced, is a no-op.
    const glm::dquat again = cam::SolveFaceUp(n, f);
    CHECK(glm::length(again - f) < 1e-9);
  }
}

TEST_CASE("cam: a flipped setup cuts the other side", "[cam]") {
  // A flip job is two setups on ONE part. The second's frame is derived from
  // the first, so the two cannot disagree about which part they are cutting.
  const glm::dquat id(1.0, 0.0, 0.0, 0.0);
  const glm::dquat back = cam::FlipFrame(id);

  // What faced the spindle now faces the bed, and the other way round.
  const glm::dmat3 m(glm::mat3_cast(back));
  const glm::dvec3 up = m * glm::dvec3(0.0, 0.0, 1.0);
  CHECK_THAT(up.z, WithinAbs(-1.0, 1e-12));
  // Flipping twice is where it started.
  const glm::dquat there = cam::FlipFrame(back);
  CHECK(std::abs(std::abs(there.w) - 1.0) < 1e-12);

  // And a real cut: the block's underside becomes the top face.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup flipped = BasicSetup();
  flipped.frame = back;
  flipped.safeZ = cam::DeriveSafeZ(flipped, doc->GetBodies());
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(flipped, doc->GetBodies(), lo, hi));
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = hi.z;
  op.bottomZ = lo.z;
  op.stepdown = 8.0;
  flipped.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(flipped, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  CHECK(paths[0].error.empty());
  for (const cam::Move& m2 : paths[0].moves)
    CHECK_FALSE(m2.hasA);
}

// --- W1-W3: the workpiece, and where zero is on it ------------------------

TEST_CASE("cam: the posted file measures from the picked corner", "[cam]") {
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup s = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 5.0;
  s.operations.push_back(op);

  // An origin of (0,0,0) is what every job carried before v68, and it must
  // post EXACTLY what it always did — that is what confines this change.
  cam::Setup zeroed = s;
  zeroed.origin = glm::dvec3(0.0);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(zeroed, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  std::string atZero, error;
  REQUIRE(cam::PostProcess("linuxcnc", zeroed, paths, {}, atZero, &error));

  // Now zero on the stock's min corner, which is where an operator touches
  // off. Same cut, different words for it.
  cam::Setup corner = s;
  corner.originAt = cam::OriginAt::StockMin;
  REQUIRE(cam::RecomputeOrigin(corner, doc->GetBodies()));
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(corner, doc->GetBodies(), lo, hi));
  CHECK_THAT(corner.origin.x, WithinAbs(lo.x, 1e-9));
  CHECK_THAT(corner.origin.y, WithinAbs(lo.y, 1e-9));
  CHECK_THAT(corner.origin.z, WithinAbs(hi.z, 1e-9)); // the top of the material

  std::string shifted;
  REQUIRE(cam::PostProcess("linuxcnc", corner, paths, {}, shifted, &error));
  CHECK(shifted != atZero);

  // Every coordinate moved by exactly the origin, and no further. Compare the
  // CUTTING moves: the leading retract is a bare `G0 Z`, so both files are
  // still at whatever X/Y the machine was left at and the parser starts both
  // from the same nothing.
  const auto cutting = [](const cam::GProgram& p) {
    std::vector<glm::dvec3> out;
    for (const cam::GMove& m : p.moves)
      if (!m.rapid)
        out.push_back(m.to);
    return out;
  };
  const cam::GProgram a = cam::ParseGcode(atZero);
  const cam::GProgram b = cam::ParseGcode(shifted);
  const std::vector<glm::dvec3> ca = cutting(a), cb = cutting(b);
  REQUIRE(ca.size() == cb.size());
  REQUIRE(ca.size() > 20);
  for (size_t i = 0; i < ca.size(); i++) {
    INFO("cut " << i);
    const glm::dvec3 d = ca[i] - cb[i];
    CHECK_THAT(d.x, WithinAbs(corner.origin.x, 2e-4)); // 4 decimals posted
    CHECK_THAT(d.y, WithinAbs(corner.origin.y, 2e-4));
    CHECK_THAT(d.z, WithinAbs(corner.origin.z, 2e-4));
  }

  // And read back through the setup they belong to, the two are the SAME
  // program — which is what every consumer that compares a file to the model
  // relies on.
  const std::vector<glm::dvec3> back =
      cutting(cam::ProgramInSetupSpace(shifted, corner));
  REQUIRE(back.size() == ca.size());
  for (size_t i = 0; i < ca.size(); i++) {
    INFO("cut " << i);
    CHECK(glm::length(back[i] - ca[i]) < 2e-4);
  }

  // Arc centres are INCREMENTAL, so they are immune to the offset. Posting
  // with arcs on and comparing the I/J words proves it rather than assuming.
  cam::PostOptions arcs;
  arcs.fitArcs = true;
  std::string arcZero, arcShift;
  REQUIRE(cam::PostProcess("linuxcnc", zeroed, paths, arcs, arcZero, &error));
  REQUIRE(cam::PostProcess("linuxcnc", corner, paths, arcs, arcShift, &error));
  const auto ijWords = [](const std::string& g) {
    std::vector<std::string> out;
    std::istringstream in(g);
    for (std::string line; std::getline(in, line);)
      for (size_t i = 0; i < line.size(); i++)
        if ((line[i] == 'I' || line[i] == 'J') &&
            (i == 0 || line[i - 1] == ' '))
          out.push_back(line.substr(i, line.find(' ', i) - i));
    return out;
  };
  CHECK(ijWords(arcZero) == ijWords(arcShift));
}

TEST_CASE("cam: a fixed workpiece is the sheet, not a shrink-wrap", "[cam]") {
  auto doc = BlockDoc(200, 120, 18);
  cam::Setup s = BasicSetup();
  s.stock.mode = cam::StockMode::Fixed;
  s.stock.size = {600.0, 400.0, 18.0};
  s.stock.place = {0.5, 0.5, 1.0};

  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));
  CHECK_THAT(hi.x - lo.x, WithinAbs(600.0, 1e-6));
  CHECK_THAT(hi.y - lo.y, WithinAbs(400.0, 1e-6));
  // An 18 mm part in an 18 mm board is a FIT, not a shortfall: OCC's bbox
  // gap makes an exact match measure a hair short every time.
  CHECK_THAT(hi.z - lo.z, WithinAbs(18.0, 1e-3));
  CHECK(cam::StockRefusal(s, doc->GetBodies()).empty());

  // Relative mode is untouched: the box every job had before v68.
  cam::Setup rel = s;
  rel.stock.mode = cam::StockMode::Relative;
  glm::dvec3 rlo(0.0), rhi(0.0);
  REQUIRE(cam::StockBounds(rel, doc->GetBodies(), rlo, rhi));
  CHECK_THAT(rhi.x - rlo.x, WithinAbs(204.0, 1e-6)); // 200 + 2 mm each side

  // A sheet smaller than the part is REFUSED, and says which axis and by how
  // much — "get a bigger board" is a different instruction from "draw
  // something", and both used to arrive as the same sentence.
  cam::Setup small = s;
  small.stock.size = {100.0, 400.0, 18.0};
  glm::dvec3 slo(0.0), shi(0.0);
  CHECK_FALSE(cam::StockBounds(small, doc->GetBodies(), slo, shi));
  const std::string why = cam::StockRefusal(small, doc->GetBodies());
  INFO(why);
  CHECK(why.find("100") != std::string::npos);
  CHECK(why.find("200") != std::string::npos);
  CHECK(why.find(" X ") != std::string::npos);
  // And the verbs refuse with those words rather than "there is no solid".
  const cam::DerivedOp derived =
      cam::DeriveOperation(small, doc->GetBodies(), cam::OpType::Profile, {});
  CHECK_FALSE(derived.ok);
  CHECK(derived.why == why);
}

TEST_CASE("cam: where the part sits in the sheet is a placement", "[cam]") {
  auto doc = BlockDoc(200, 120, 18); // spans 0..200, 0..120, 0..18
  cam::Setup s = BasicSetup();
  s.stock.mode = cam::StockMode::Fixed;
  s.stock.size = {600.0, 400.0, 18.0};

  glm::dvec3 lo(0.0), hi(0.0);
  s.stock.place = {0.0, 0.0, 1.0}; // part's low face on the stock's low face
  REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));
  CHECK_THAT(lo.x, WithinAbs(0.0, 1e-6));
  s.stock.place = {1.0, 0.0, 1.0}; // …and its high face on the stock's high
  REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));
  CHECK_THAT(hi.x, WithinAbs(200.0, 1e-6));
  s.stock.place = {0.5, 0.5, 1.0};
  REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));
  CHECK_THAT((lo.x + hi.x) * 0.5, WithinAbs(100.0, 1e-6)); // part's own centre
}

TEST_CASE("cam: every origin corner is where it says it is", "[cam]") {
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup s = BasicSetup();
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));

  const auto originFor = [&](cam::OriginAt at) {
    cam::Setup c = s;
    c.originAt = at;
    REQUIRE(cam::RecomputeOrigin(c, doc->GetBodies()));
    return c.origin;
  };
  const glm::dvec3 stockMin = originFor(cam::OriginAt::StockMin);
  CHECK_THAT(stockMin.x, WithinAbs(lo.x, 1e-9));
  CHECK_THAT(stockMin.y, WithinAbs(lo.y, 1e-9));
  const glm::dvec3 centre = originFor(cam::OriginAt::StockCentre);
  CHECK_THAT(centre.x, WithinAbs((lo.x + hi.x) * 0.5, 1e-9));
  const glm::dvec3 modelMin = originFor(cam::OriginAt::ModelMin);
  CHECK_THAT(modelMin.x, WithinAbs(lo.x + s.stock.margin.x, 1e-6));
  // Every one of them touches off on the TOP of the material.
  for (const glm::dvec3& o : {stockMin, centre, modelMin})
    CHECK_THAT(o.z, WithinAbs(hi.z, 1e-9));

  // Re-deriving is idempotent: the origin does not walk when the panel calls
  // this on every stock edit.
  cam::Setup c = s;
  REQUIRE(cam::RecomputeOrigin(c, doc->GetBodies()));
  const glm::dvec3 once = c.origin;
  REQUIRE(cam::RecomputeOrigin(c, doc->GetBodies()));
  CHECK(glm::length(c.origin - once) < 1e-12);
}

namespace {

// The floor of the pocket in these fixtures, at z = 12.
ElementRef PocketFloorRef(const Document& doc) {
  ElementRef floor;
  for (const Body& b : doc.GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      if (b.shape.FacePlane(index, c, n, u) && n.z > 0.99f &&
          std::abs(c.z - 12.0f) < 1e-3f)
        floor = ElementRef{b.createdBy, name};
    }
  return floor;
}

// Through the SAME derivation the verb uses, so the job under test is the job
// a click makes — depths, side, stepdown and the pocket/adaptive choice all
// come from the model rather than from numbers typed here.
cam::Setup PocketJob(const Document& doc, const ElementRef& floor,
                     bool useOutline) {
  cam::Setup s = BasicSetup();
  const cam::DerivedOp d =
      cam::DeriveOperation(s, doc.GetBodies(), cam::OpType::Pocket, floor);
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  op.useOutline = useOutline;
  s.operations.push_back(op);
  return s;
}

// Does any cutting move pass within `r` of (x, y)?
bool CutsOver(const cam::Toolpath& tp, double x, double y, double r) {
  for (const cam::Move& m : tp.moves)
    if (m.kind != cam::Move::Kind::Rapid &&
        std::hypot(m.to.x - x, m.to.y - y) < r)
      return true;
  return false;
}

} // namespace

TEST_CASE("cam: a pass never rapids to a depth nothing cleared", "[cam]") {
  // The entry used to rapid straight to the height it starts cutting at, on
  // the reasoning that that height "is either the stock top or the depth the
  // previous pass already cut here, so it is air". True of a closed contour,
  // which returns to its own entry at full depth. False of a POCKET, whose
  // entry sits mid-spiral where the previous pass was still ramping — and
  // false of any operation told to start BELOW the stock top, which is what a
  // finishing skim after a roughing pass is.
  //
  // Both shapes here: a pocket deep enough for two passes, and a skim that
  // starts at the roughing floor. `rapids_through_stock` must be 0 for both.
  auto doc = FlangeDoc(/*holeDia=*/0.0, /*boss=*/false); // 80 x 60 pocket
  const ElementRef floor = PocketFloorRef(*doc);
  REQUIRE(floor.IsSet());

  const auto rapidsFor = [&](double topZ, double stepdown) {
    cam::Setup s = BasicSetup();
    const cam::DerivedOp d =
        cam::DeriveOperation(s, doc->GetBodies(), cam::OpType::Pocket, floor);
    REQUIRE(d.ok);
    cam::Operation op = d.op;
    op.id = 1;
    op.type = cam::OpType::Pocket; // concentric, not adaptive: this is about
                                   // the ENTRY, and both share it
    if (topZ > 0.0)
      op.topZ = topZ;
    op.stepdown = stepdown;
    s.operations.push_back(op);
    const std::vector<cam::Toolpath> paths =
        cam::GenerateAll(s, doc->GetBodies());
    REQUIRE(paths.size() == 1);
    INFO(paths[0].error);
    REQUIRE(paths[0].error.empty());
    std::string gcode, error;
    REQUIRE(cam::PostProcess("linuxcnc", s, paths, {}, gcode, &error));
    return cam::VerifyGcode(gcode, s, doc->GetBodies(), 0.6);
  };

  // Two depth passes: the second enters where the first was still ramping.
  const cam::Verdict deep = rapidsFor(/*topZ=*/0.0, /*stepdown=*/2.0);
  INFO("two passes, first bad line " << deep.firstBadLine);
  CHECK(deep.rapidsThroughStock == 0);

  // A finishing skim told to start 0.3 mm above the floor — below the stock
  // top, so the old entry rapided through everything in between.
  const cam::Verdict skim = rapidsFor(/*topZ=*/12.3, /*stepdown=*/4.0);
  INFO("skim, first bad line " << skim.firstBadLine);
  CHECK(skim.rapidsThroughStock == 0);
}

TEST_CASE("cam: clearing the outline passes over holes", "[cam]") {
  // A hole NARROWER than the cutter: 3 mm against a 6 mm end mill. Nothing
  // stands in it and the tool could never enter it, so crossing is harmless —
  // and going AROUND is pure waste that leaves an uncut sliver wherever the
  // hole nearly touches the pocket wall, a gap the tool can never come back
  // to. (A hole the cutter DOES fit into is a different question, and the
  // next case answers it.)
  auto doc = FlangeDoc(/*holeDia=*/3.0, /*boss=*/false);
  const ElementRef floor = PocketFloorRef(*doc);
  REQUIRE(floor.IsSet());

  const cam::Toolpath a = cam::GenerateAll(
      PocketJob(*doc, floor, /*useOutline=*/false), doc->GetBodies())[0];
  const cam::Toolpath b = cam::GenerateAll(
      PocketJob(*doc, floor, /*useOutline=*/true), doc->GetBodies())[0];
  INFO(a.error << " / " << b.error);
  REQUIRE(a.error.empty());
  REQUIRE(b.error.empty());

  // The hole is at (60, 45). Only the outline path crosses it.
  CHECK(CutsOver(b, 60.0, 45.0, 3.0));
  CHECK_FALSE(CutsOver(a, 60.0, 45.0, 1.0));
  // SHORTER, not longer. Weaving around costs more than crossing: the detour
  // is extra contour on every depth pass, and the sliver it leaves behind is
  // not cut by any of it.
  INFO("around " << a.CutLength() << " over " << b.CutLength());
  CHECK(b.CutLength() < a.CutLength());
}

TEST_CASE("cam: an opening the cutter fits into is still an island", "[cam]") {
  // `useOutline` crosses an opening because going AROUND one leaves a sliver
  // the tool can never come back to. That reasoning only holds for an opening
  // the tool cannot enter. A BIG opening is one the job removes anyway — the
  // driver cutout through the middle of a flange recess — and clearing across
  // it is time spent cutting air over a hole that is about to be there.
  //
  // So size decides: an opening the cutter fits inside stays an island and
  // the clearing goes round it; one it cannot enter is crossed.
  auto doc = FlangeDoc(/*holeDia=*/16.0, /*boss=*/false); // vs a 6 mm cutter
  const ElementRef floor = PocketFloorRef(*doc);
  REQUIRE(floor.IsSet());
  const cam::Toolpath big = cam::GenerateAll(
      PocketJob(*doc, floor, /*useOutline=*/true), doc->GetBodies())[0];
  INFO(big.error);
  REQUIRE(big.error.empty());
  CHECK_FALSE(CutsOver(big, 60.0, 45.0, 6.0)); // round it: the tool fits in

  // …and the same option on an opening NARROWER than the cutter still crosses
  // it, which is the case it was added for.
  auto small = FlangeDoc(/*holeDia=*/3.0, /*boss=*/false);
  const ElementRef sf = PocketFloorRef(*small);
  REQUIRE(sf.IsSet());
  const cam::Toolpath thin = cam::GenerateAll(
      PocketJob(*small, sf, /*useOutline=*/true), small->GetBodies())[0];
  REQUIRE(thin.error.empty());
  CHECK(CutsOver(thin, 60.0, 45.0, 3.0));

  // Crossing the small one is still the cheaper path…
  const cam::Toolpath around = cam::GenerateAll(
      PocketJob(*small, sf, /*useOutline=*/false), small->GetBodies())[0];
  CHECK(thin.CutLength() <= around.CutLength());
  // …and going round the big one is cheaper than crossing it, which is the
  // waste this rule exists to stop.
  const cam::Toolpath crossed = cam::GenerateAll(
      PocketJob(*doc, floor, /*useOutline=*/false), doc->GetBodies())[0];
  INFO("island " << big.CutLength() << " vs all-round " << crossed.CutLength());
  CHECK_THAT(big.CutLength(), WithinRel(crossed.CutLength(), 1e-9));
}

TEST_CASE("cam: clearing the outline still goes around material", "[cam]") {
  // The hazard the option must NOT open. A boss standing up from the floor
  // has the same kind of inner wire as a hole and the opposite meaning:
  // driving over one is a full-width cut at full depth into material nobody
  // asked to remove. No flag may point the cutter at that, so an island
  // survives `useOutline` — which is where FreeCAD's `UseOutline` differs,
  // taking `Part.Face(element.OuterWire)` and losing the bosses with the
  // holes.
  auto doc = FlangeDoc(/*holeDia=*/0.0, /*boss=*/true);
  const ElementRef floor = PocketFloorRef(*doc);
  REQUIRE(floor.IsSet());

  const cam::Toolpath a = cam::GenerateAll(
      PocketJob(*doc, floor, /*useOutline=*/false), doc->GetBodies())[0];
  const cam::Toolpath b = cam::GenerateAll(
      PocketJob(*doc, floor, /*useOutline=*/true), doc->GetBodies())[0];
  INFO(a.error << " / " << b.error);
  REQUIRE(a.error.empty());
  REQUIRE(b.error.empty());

  // The boss is 8 x 8 at (40, 30), so its centre is (44, 34) and going
  // around it keeps the cutter's centre at least 7 mm out. NEITHER path cuts
  // inside 5 mm of it.
  CHECK_FALSE(CutsOver(a, 44.0, 34.0, 5.0));
  CHECK_FALSE(CutsOver(b, 44.0, 34.0, 5.0));
  // And with nothing else to differ about, the two agree exactly.
  INFO("around " << a.CutLength() << " over " << b.CutLength());
  CHECK_THAT(b.CutLength(), WithinRel(a.CutLength(), 1e-9));
}

TEST_CASE("cam: a floor with both keeps the boss and drops the hole", "[cam]") {
  // Both kinds of inner wire at once: the case that proves the test is on the
  // WIRE and not on the count.
  auto doc = FlangeDoc(/*holeDia=*/3.0, /*boss=*/true);
  const ElementRef floor = PocketFloorRef(*doc);
  REQUIRE(floor.IsSet());
  const cam::Toolpath b = cam::GenerateAll(
      PocketJob(*doc, floor, /*useOutline=*/true), doc->GetBodies())[0];
  INFO(b.error);
  REQUIRE(b.error.empty());
  CHECK(CutsOver(b, 60.0, 45.0, 3.0));       // over the hole
  CHECK_FALSE(CutsOver(b, 44.0, 34.0, 5.0)); // around the boss
}

TEST_CASE("cam: retooling carries the derived stepdown, not the typed one",
          "[cam]") {
  cam::Setup s = BasicSetup(); // T1 = 6 mm
  cam::Tool small;
  small.number = 2;
  small.name = "3mm flat";
  small.diameter = 3.0;
  small.feedCut = 600.0;
  small.feedPlunge = 200.0;
  s.tools.push_back(small);

  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.stepdown = cam::DerivedStepdown(s.tools[0]); // nobody has stated one
  std::string why;

  // Untouched: it follows the cutter. Half of 6 becomes half of 3 — the whole
  // point, since a 6 mm tool's 3 mm pass is a full-diameter plunge for a 3 mm
  // one.
  REQUIRE(cam::Retool(s, op, 2, why));
  CHECK(op.toolNumber == 2);
  CHECK_THAT(op.stepdown, WithinAbs(1.5, 1e-9));

  // Typed: it is the user's number and survives the change.
  op.stepdown = 0.4;
  REQUIRE(cam::Retool(s, op, 1, why));
  CHECK(op.toolNumber == 1);
  CHECK_THAT(op.stepdown, WithinAbs(0.4, 1e-9));

  // A tool this setup does not have is REFUSED, naming it — not quietly read
  // as tool 1, which cuts perfectly well with the wrong cutter at the wrong
  // feeds.
  const int before = op.toolNumber;
  CHECK_FALSE(cam::Retool(s, op, 7, why));
  CHECK(why.find("tool 7") != std::string::npos);
  CHECK(op.toolNumber == before);
}

TEST_CASE("cam: operations reorder, and the order is what gets posted",
          "[cam]") {
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup s = BasicSetup();
  cam::Tool second = s.tools[0];
  second.number = 2;
  second.name = "3mm flat";
  second.diameter = 3.0;
  s.tools.push_back(second);

  for (int i = 0; i < 3; i++) {
    cam::Operation op;
    op.id = static_cast<uint64_t>(i + 1);
    op.type = cam::OpType::Profile;
    op.toolNumber = i == 1 ? 2 : 1; // T1, T2, T1 — two tool changes
    op.topZ = 20.0;
    op.bottomZ = 10.0;
    op.stepdown = 0.0;
    s.operations.push_back(op);
  }

  // Move the last to the front.
  REQUIRE(cam::MoveOperation(s, 3, 0));
  CHECK(s.operations[0].id == 3);
  CHECK(s.operations[1].id == 1);
  CHECK(s.operations[2].id == 2);
  // And back again: a move to its own index is a no-op, not a refusal.
  REQUIRE(cam::MoveOperation(s, 3, 2));
  CHECK(s.operations[0].id == 1);
  REQUIRE(cam::MoveOperation(s, 1, 0));

  // Past the end is refused rather than clamped to the last slot.
  CHECK_FALSE(cam::MoveOperation(s, 1, 3));
  CHECK_FALSE(cam::MoveOperation(s, 99, 0));

  // The order reaches the FILE: T1, T2, T1 is two tool changes; grouping the
  // two T1 operations together is one. That is what reordering buys, and it
  // is why the order is not cosmetic.
  const auto toolChanges = [&](const cam::Setup& setup) {
    const std::vector<cam::Toolpath> paths =
        cam::GenerateAll(setup, doc->GetBodies());
    std::string gcode, error;
    REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, gcode, &error));
    int n = 0;
    for (size_t i = gcode.find("M6"); i != std::string::npos;
         i = gcode.find("M6", i + 1))
      n++;
    return n;
  };
  const int scattered = toolChanges(s); // T1, T2, T1
  REQUIRE(cam::MoveOperation(s, 2, 2)); // -> T1, T1, T2
  CHECK(s.operations[2].id == 2);
  const int grouped = toolChanges(s);
  INFO("scattered " << scattered << " grouped " << grouped);
  CHECK(grouped < scattered);
}

TEST_CASE("cam: a face that cannot face the spindle is refused", "[cam]") {
  double degrees = 0.0;
  // Straight up already: A = 0.
  REQUIRE(cam::SolveIndexA({0.0, 0.0, 1.0}, degrees));
  CHECK_THAT(degrees, WithinAbs(0.0, 1e-9));
  // Pointing at +Y: turn the table 90 degrees.
  REQUIRE(cam::SolveIndexA({0.0, 1.0, 0.0}, degrees));
  CHECK_THAT(degrees, WithinAbs(90.0, 1e-9));
  // Pointing down: 180.
  REQUIRE(cam::SolveIndexA({0.0, 0.0, -1.0}, degrees));
  CHECK_THAT(std::abs(degrees), WithinAbs(180.0, 1e-9));
  // Along the rotary axis itself: no rotation about X can bring it up, and
  // the honest answer is to say so rather than pick the nearest angle.
  CHECK_FALSE(cam::SolveIndexA({1.0, 0.0, 0.0}, degrees));
  CHECK_FALSE(cam::SolveIndexA({0.7, 0.7, 0.0}, degrees));
}

// --- clicking a wall (dogfooding round 1) ---------------------------------

TEST_CASE("cam: clicking the wall of a through opening cuts it out", "[cam]") {
  auto doc = SlottedDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef wall = WallNear(*doc, 14.0, 15.0); // the slot's -X wall
  REQUIRE(wall.IsSet());

  const cam::DerivedOp derived =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Profile, wall);
  INFO(derived.why);
  REQUIRE(derived.ok);
  // An opening is cut from INSIDE, with no tabs — the slug is scrap, and
  // tabbing it only leaves stubs on the part.
  CHECK(derived.op.side == cam::Side::Inside);
  CHECK(derived.op.tabCount == 0);
  // Through: the wall reaches the bottom of the part, so the cut carries on
  // into the spoilboard exactly as a cut-out does.
  CHECK_THAT(derived.op.topZ, WithinAbs(21.0, 1e-6));
  CHECK_THAT(derived.op.bottomZ, WithinAbs(-1.0, 1e-6));

  cam::Setup job = setup;
  cam::Operation op = derived.op;
  op.id = 1;
  job.operations.push_back(op);
  job.safeZ = cam::DeriveSafeZ(job, doc->GetBodies());
  const cam::Toolpath tp = cam::GenerateAll(job, doc->GetBodies())[0];
  INFO(tp.error);
  REQUIRE(tp.error.empty());
  // It stays inside the slot, cutter radius and all: 14..26 x 8..22 shrunk
  // by 3 mm.
  for (const cam::Move& m : tp.moves) {
    if (m.kind == cam::Move::Kind::Rapid)
      continue;
    INFO("cut at " << m.to.x << "," << m.to.y);
    CHECK(m.to.x >= 14.0 + 3.0 - 0.05);
    CHECK(m.to.x <= 26.0 - 3.0 + 0.05);
    CHECK(m.to.y >= 8.0 + 3.0 - 0.05);
    CHECK(m.to.y <= 22.0 - 3.0 + 0.05);
  }
  // And it goes all the way through, into the spoilboard.
  CHECK_THAT(tp.MinZ(), WithinAbs(-1.0, 1e-6));
}

TEST_CASE("cam: a through cut carries the contour past the bottom of the part",
          "[cam]") {
  // The model stops at z = 0 and the cut is asked for z = -1. Sectioning
  // finds nothing below the part, so the last pass used to be SKIPPED and the
  // part stayed welded to the sheet — the exact failure tabs exist to control
  // (docs/cam/dogfood-round.md finding 2).
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = -1.0;
  op.stepdown = 7.0; // 13, 6, -1: the last pass is entirely below the model
  op.side = cam::Side::Outside;
  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  INFO(tp.error);
  REQUIRE(tp.error.empty());
  CHECK_THAT(tp.MinZ(), WithinAbs(-1.0, 1e-9));
}

TEST_CASE("cam: clicking a wall for a pocket says what to click instead",
          "[cam]") {
  auto doc = SlottedDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef wall = WallNear(*doc, 14.0, 15.0);
  const cam::DerivedOp derived =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, wall);
  CHECK_FALSE(derived.ok);
  INFO(derived.why);
  CHECK(derived.why.find("FLOOR") != std::string::npos);
}

TEST_CASE("cam verify: a job with tabs is SAFE without being COMPLETE",
          "[cam]") {
  // The two questions have different answers, and one flag could not carry
  // both: tabs are material deliberately left standing, so a job that cuts
  // the part correctly reports leftover forever. Gating on one boolean meant
  // gating on a boolean no real job could ever set
  // (docs/cam/dogfood-round.md).
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, ElementRef{});
  REQUIRE(derived.ok);
  REQUIRE(derived.op.tabCount > 0); // the point of the test
  derived.op.id = 1;
  setup.operations.push_back(derived.op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.4);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover << " over "
                << v.leftoverArea << " mm2 in " << v.leftoverRegions);
  CHECK(v.Safe(0.6));            // nothing scrapped, nothing crashed…
  CHECK_FALSE(v.Complete(0.6));  // …and the tabs are still standing
  CHECK(v.leftoverRegions >= 3); // in several small places, as tabs are
}

TEST_CASE("cam: a wall is not a gouge, whatever the grid", "[cam]") {
  // A cell centre landing on the corner of an island is inside the island for
  // the model and inside the cut for the tool — both boundary tests are
  // inclusive — so a clean job reported the island's whole height as a gouge.
  // The panel checks at 0.4 mm and the RPC defaulted to 0.25, so the two
  // disagreed about the SAME job: "cuts 6.00 mm INTO the part, that is scrap"
  // against a verdict of nothing (docs/cam/dogfood-round.md, round 2). The
  // numbers below are the coupon's own pocket, because which grids land on a
  // corner depends on the actual coordinates.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 120, 60, 17));
  const uint64_t plate = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(12, 20, 11), 40, 20, 11));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", plate, voidId,
                                                   BooleanOp::Subtract));
  const uint64_t pocket = doc->GetFeature(2)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Boss", glm::vec3(27, 25, 11), 10, 10, 6));
  const uint64_t boss = doc->GetFeature(3)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("WithBoss", pocket, boss,
                                                   BooleanOp::Union));

  cam::Setup setup = BasicSetup();
  setup.stock.margin.z = 1.0;
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  const ElementRef floor = UpFaceAt(*doc, 11.0);
  REQUIRE(floor.IsSet());
  cam::DerivedOp derived =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, floor);
  INFO(derived.why);
  REQUIRE(derived.ok);
  derived.op.id = 1;
  setup.operations.push_back(derived.op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  // 0.4 mm is what the panel uses, and the grid it lands on puts a cell
  // centre a hair inside the boss's corner at X37 Y35.
  for (const double res : {0.25, 0.4, 0.5}) {
    const cam::Verdict v =
        cam::VerifyGcode(gcode, setup, doc->GetBodies(), res);
    INFO("at " << res << " mm: gouge " << v.maxGouge << " at " << v.gougeAt.x
               << "," << v.gougeAt.y);
    CHECK(v.maxGouge <= res * 1.5);
  }
}

TEST_CASE("cam: the spindle is restated after a manual tool change", "[cam]") {
  // Two tools at the same rpm: the S/M3 was emitted only when the SPEED
  // changed, so the file paused for a manual tool change — which is done with
  // the spindle stopped — and the next line was a cutting move. Found by
  // reading the coupon's own file (docs/cam/dogfood-round.md).
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Tool second = setup.tools.front();
  second.number = 2;
  second.diameter = 3.0;
  setup.tools.push_back(second); // same spindleRpm as tool 1
  std::vector<cam::Toolpath> paths;
  for (int t : {1, 2}) {
    cam::Toolpath tp;
    tp.operationId = static_cast<uint64_t>(t);
    tp.toolNumber = t;
    tp.moves = {{cam::Move::Kind::Rapid, {0.0, 0.0, 25.0}, false, 0.0, 0.0},
                {cam::Move::Kind::Cut, {10.0, 0.0, 5.0}, false, 0.0, 600.0}};
    paths.push_back(tp);
  }
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const size_t pause = gcode.find("M0 (change tool", gcode.find("T2 M6"));
  REQUIRE(pause != std::string::npos);
  INFO(gcode.substr(pause, 80));
  CHECK(gcode.find("M3", pause) != std::string::npos);
  // …and before the first move of the new tool, not somewhere after it.
  CHECK(gcode.find("M3", pause) < gcode.find("G1", pause));
}

TEST_CASE("cam: a drilled hole is drilled in the simulation too", "[cam]") {
  // A plunge does not travel in XY, so the closest-point parameter along its
  // path is 0 everywhere and the simulated tool stayed at the height it
  // started from: every drilled hole came out full of stock, and the retract
  // out of it was reported as a rapid through material.
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Drill;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 5.0;
  cam::Toolpath tp;
  tp.operationId = 1;
  tp.toolNumber = 1;
  tp.moves = {{cam::Move::Kind::Rapid, {20.0, 15.0, 25.0}, false, 0.0, 0.0},
              {cam::Move::Kind::Plunge, {20.0, 15.0, 5.0}, false, 0.0, 200.0},
              {cam::Move::Kind::Rapid, {20.0, 15.0, 25.0}, false, 0.0, 0.0}};
  setup.operations.push_back(op);
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, {tp}, {}, gcode, &error));

  const cam::Inspection insp =
      cam::Inspect(gcode, setup, doc->GetBodies(), 0.5);
  REQUIRE(insp.ok);
  // The hole is there…
  const int i = static_cast<int>((20.0 - insp.machined.x0) / insp.machined.res);
  const int j = static_cast<int>((15.0 - insp.machined.y0) / insp.machined.res);
  CHECK_THAT(static_cast<double>(insp.machined.At(i, j)), WithinAbs(5.0, 1e-4));
  // …so climbing back out of it is not a rapid through material.
  CHECK(insp.verdict.rapidsThroughStock == 0);
}

// --- C8: adaptive clearing ------------------------------------------------

TEST_CASE("cam: adaptive clears the same pocket, gently", "[cam]") {
  auto doc = PocketDoc(); // 20 x 14 pocket, floor at z = 12
  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());

  cam::Operation base =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, floor)
          .op;
  base.id = 1;

  cam::Setup concentric = setup;
  concentric.operations.push_back(base);
  cam::Operation adaptiveOp = base;
  adaptiveOp.type = cam::OpType::Adaptive;
  cam::Setup adaptive = setup;
  adaptive.operations.push_back(adaptiveOp);

  const cam::Toolpath a = cam::GenerateAll(adaptive, doc->GetBodies())[0];
  INFO(a.error);
  if (!cam::AdaptiveAvailable()) {
    // -DENABLE_CAM_ADAPTIVE=OFF: the job still cuts, by the other strategy.
    CHECK(a.CutLength() > 0.0);
    return;
  }
  REQUIRE(a.error.empty());
  CHECK(a.CutLength() > 0.0);
  CHECK_THAT(a.MinZ(), WithinAbs(12.0, 1e-6));

  // It stays inside the pocket, cutter radius and all.
  for (const cam::Move& m : a.moves) {
    if (m.kind == cam::Move::Kind::Rapid)
      continue;
    INFO("cut at " << m.to.x << "," << m.to.y);
    CHECK(m.to.x >= 10.0 + 3.0 - 0.05);
    CHECK(m.to.x <= 30.0 - 3.0 + 0.05);
    CHECK(m.to.y >= 8.0 + 3.0 - 0.05);
    CHECK(m.to.y <= 22.0 - 3.0 + 0.05);
  }

  // And it machines the part: posted, simulated, compared against the model.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", adaptive, {a}, {}, gcode, &error));
  const cam::Verdict v =
      cam::VerifyGcode(gcode, adaptive, doc->GetBodies(), 0.5);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover << " rapids "
                << v.rapidsThroughStock);
  CHECK(v.maxGouge < 0.6); // one cell of quantisation at 0.5 mm
  // The link moves are the whole safety story: a link across material this
  // pass has not cut must happen at safe Z, not at depth.
  CHECK(v.rapidsThroughStock == 0);
}

TEST_CASE("cam: the vendored adaptive library throws on some vertex layouts",
          "[cam]") {
  // Frozen because it is the reason the fallback below exists. Adaptive2d
  // throws std::out_of_range from its finishing-pass z-tagging (map::at) for
  // particular vertex layouts — not particular SHAPES: a 30 mm circle clears
  // fine at 24, 60, 120 and 200 points and throws at 37. Which is why it
  // looked ellipse-specific from the GUI: every ellipse OCC discretises at the
  // 0.02 mm chord tolerance happened to land in the failing set
  // (docs/cam/dogfood-round.md).
  if (!cam::AdaptiveAvailable())
    return;
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.type = cam::OpType::Adaptive;
  op.side = cam::Side::Inside;
  op.topZ = 10.0;
  op.bottomZ = 0.0;
  const auto circle = [](int n) {
    cam::Loop l;
    for (int i = 0; i < n; i++) {
      const double a = 2.0 * std::numbers::pi * i / n;
      l.pts.push_back({30.0 * std::cos(a), 30.0 * std::sin(a)});
    }
    return l;
  };
  for (const int n : {24, 60, 120}) {
    const cam::AdaptiveResult r =
        cam::AdaptiveClear(setup, op, setup.tools[0], {circle(n)}, 0.0, 10.0);
    INFO(n << " points: " << r.error);
    CHECK(!r.moves.empty());
  }
  // The one that does not. If this ever starts passing the library was fixed
  // or replaced, and the fallback can be revisited — it is not wrong either
  // way, which is the point of a fallback.
  const cam::AdaptiveResult bad =
      cam::AdaptiveClear(setup, op, setup.tools[0], {circle(37)}, 0.0, 10.0);
  CHECK(bad.moves.empty());
  CHECK(!bad.error.empty());
}

TEST_CASE("cam: a clearing operation keeps its side across a save", "[cam]") {
  // `side` was written for PROFILES only, and adaptive is the one strategy
  // that reads it — so a pocket that cut correctly came back from its own
  // file clearing OUTSIDE its boundary, where there is nothing, and produced
  // no toolpath at all. A job that worked before the save did not work after
  // it (docs/cam/dogfood-round.md).
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 40, 30, 10));
  cam::Setup& s = doc.AddCamSetup("Job");
  for (const cam::OpType type : {cam::OpType::Pocket, cam::OpType::Adaptive}) {
    cam::Operation op;
    op.id = doc.AllocateId();
    op.type = type;
    op.side = cam::Side::Inside;
    op.topZ = 10.0;
    op.bottomZ = 4.0;
    s.operations.push_back(op);
  }
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_clearing_side.json";
  REQUIRE(DocumentIO::Save(doc, path.string()).has_value());
  auto back = DocumentIO::Load(path.string());
  REQUIRE(back.has_value());
  const std::vector<cam::Setup>& setups = (*back)->GetCamSetups();
  REQUIRE(setups.size() == 1);
  REQUIRE(setups[0].operations.size() == 2);
  for (const cam::Operation& op : setups[0].operations)
    CHECK(op.side == cam::Side::Inside);
  std::filesystem::remove(path);

  // And a document written BEFORE the key was saved at all: the default
  // depends on the type, so those files repair themselves rather than asking
  // anyone to re-make a job. (tests/golden/v60-cam-arcs.json is a real one.)
  const std::string old = R"({"format":"designer-document","version":61,
    "features":[{"type":"Box","name":"P","id":1,"position":[0,0,0],
                 "width":40,"height":30,"depth":10}],
    "cam":[{"id":2,"name":"J","operations":[
      {"id":3,"type":"adaptive","tool":1,"top_z":10,"bottom_z":4},
      {"id":4,"type":"profile","tool":1,"top_z":10,"bottom_z":0}]}]})";
  const std::filesystem::path oldPath =
      std::filesystem::temp_directory_path() / "cam_clearing_side_old.json";
  std::ofstream(oldPath) << old;
  auto migrated = DocumentIO::Load(oldPath.string());
  REQUIRE(migrated.has_value());
  const std::vector<cam::Operation>& ops =
      (*migrated)->GetCamSetups()[0].operations;
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].side == cam::Side::Inside);  // a clearing op clears inside
  CHECK(ops[1].side == cam::Side::Outside); // a profile's default is its own
  std::filesystem::remove(oldPath);
}

TEST_CASE("cam: cut it out refuses the floor of a pocket", "[cam]") {
  // Clicking a pocket floor and pressing "Cut it out" derived an OUTSIDE
  // profile through the whole part — a full-depth slot routed around the
  // pocket, in the middle of the board. The verifier called it 22 mm of gouge
  // on a 20 mm plate, which is scrap (docs/cam/dogfood-round.md). The mirror
  // of this mistake was already guarded: a pocket asked for on a WALL says
  // "click the pocket's FLOOR".
  auto doc = PocketDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, floor);
  CHECK_FALSE(derived.ok);
  INFO(derived.why);
  // It must lead with the verb that WORKS here. Saying what is wrong and
  // then offering two alternatives got read as "pockets are picked by their
  // walls", which is the opposite of true.
  CHECK(derived.why.find("floor of a pocket") != std::string::npos);
  CHECK(derived.why.find("Clear a pocket") < derived.why.find("Cut it out"));

  // The part's own top face still means what it always meant.
  const ElementRef top = UpFaceAt(*doc, 20.0);
  REQUIRE(top.IsSet());
  const cam::DerivedOp out =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Profile, top);
  INFO(out.why);
  REQUIRE(out.ok);
  CHECK(out.op.side == cam::Side::Outside);
  CHECK(out.op.tabCount > 0);
}

TEST_CASE("cam: asking for adaptive derives the pocket it is", "[cam]") {
  // OpType::Adaptive had no case in the derivation, so it fell through the
  // switch and returned a DEFAULT operation — top 0, bottom -1, no target —
  // and said ok. A script asking for the strategy by name got an operation
  // pointing at nothing (docs/cam/dogfood-round.md). Adaptive IS a pocket;
  // only the clearing strategy differs.
  auto doc = PocketDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Adaptive, floor);
  INFO(derived.why);
  REQUIRE(derived.ok);
  CHECK(derived.op.side == cam::Side::Inside);
  CHECK_THAT(derived.op.bottomZ, WithinAbs(12.0, 1e-9));
  CHECK(derived.op.topZ > derived.op.bottomZ);
  REQUIRE(derived.op.targets.size() == 1);
}

TEST_CASE("cam: an adaptive operation survives a save and reload", "[cam]") {
  // v59: a new OpType is a format change, so it needs the round trip and the
  // golden pair that go with one.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 40, 30, 10));
  cam::Setup& s = doc.AddCamSetup("Job");
  cam::Operation op;
  op.id = doc.AllocateId();
  op.name = "Rough";
  op.type = cam::OpType::Adaptive;
  op.topZ = 10.0;
  op.bottomZ = 4.0;
  op.stepover = 0.25;
  s.operations.push_back(op);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_adaptive_roundtrip.json";
  REQUIRE(DocumentIO::Save(doc, path.string()).has_value());
  auto back = DocumentIO::Load(path.string());
  REQUIRE(back.has_value());
  const std::vector<cam::Setup>& setups = (*back)->GetCamSetups();
  REQUIRE(setups.size() == 1);
  REQUIRE(setups[0].operations.size() == 1);
  CHECK(setups[0].operations[0].type == cam::OpType::Adaptive);
  CHECK(setups[0].fitArcs); // v60, defaulting on for a document without it
  std::filesystem::remove(path);
}

// --- C7: arcs -------------------------------------------------------------

TEST_CASE("cam: curves post as arcs, and the cut does not change", "[cam]") {
  // The profile of a rectangle offset outward has four round corners. Fitted,
  // each is one G2 about the part corner instead of thirteen G1s.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = 0.0;
  op.stepdown = 0.0;
  setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());

  cam::PostOptions arcs;
  cam::PostOptions lines;
  lines.fitArcs = false;
  // The choice lives on the setup and survives a save, because a controller
  // that mishandles arcs mishandles them every session.
  CHECK(setup.fitArcs);
  std::string withArcs, withLines, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, arcs, withArcs, &error));
  REQUIRE(cam::PostProcess("mach3", setup, paths, lines, withLines, &error));

  const auto count = [](const std::string& text, const std::string& what) {
    size_t n = 0, at = 0;
    while ((at = text.find(what, at)) != std::string::npos) {
      n++;
      at += what.size();
    }
    return n;
  };
  CHECK(count(withArcs, "\nG2 ") == 4); // one per corner
  CHECK(count(withLines, "\nG2 ") == 0);
  // 79 lines against 127. Not half: the entry ramp descends while it turns,
  // and a helical arc is deferred (C7 fits flat arcs only), so that lap
  // stays as line moves.
  const auto lines_of = [](const std::string& t) {
    return std::count(t.begin(), t.end(), '\n');
  };
  CHECK(lines_of(withArcs) < lines_of(withLines) * 3 / 4);
  CHECK(withArcs.size() < withLines.size());
  // The mode word is not optional: incremental centres read as absolute put
  // the arc centre at the work origin.
  CHECK(withArcs.find("G91.1") != std::string::npos);
  CHECK(withLines.find("G91.1") == std::string::npos);

  // The centre is EXACTLY the part corner and the radius exactly the tool's:
  // a least-squares centre that is not equidistant from the endpoints is
  // what a controller rejects as an arc endpoint radius mismatch.
  CHECK(withArcs.find("G2 X40 Y-3 I-3 J0") != std::string::npos);

  // The load-bearing test: the same job, posted both ways, must machine the
  // same part. A fit that is merely tidier shows up here as a difference.
  const cam::Verdict a =
      cam::VerifyGcode(withArcs, setup, doc->GetBodies(), 0.4);
  const cam::Verdict l =
      cam::VerifyGcode(withLines, setup, doc->GetBodies(), 0.4);
  INFO("arcs gouge " << a.maxGouge << " leftover " << a.maxLeftover
                     << " / lines gouge " << l.maxGouge << " leftover "
                     << l.maxLeftover);
  CHECK(std::abs(a.maxGouge - l.maxGouge) < 0.05);
  CHECK(std::abs(a.maxLeftover - l.maxLeftover) < 0.05);
  CHECK(a.rapidsThroughStock == l.rapidsThroughStock);
}

TEST_CASE("cam: an arc is never fitted across a straight edge", "[cam]") {
  // The bug the first fit had: deviation checked only AT the sample points
  // let a 1.5 mm straight segment be absorbed into a 10 mm-radius arc that
  // passed within 5 um at every sample and bulged 27 um in between — a curve
  // cut where the part wants a flat.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = 0.0;
  op.stepdown = 0.0;
  setup.operations.push_back(op);
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup,
                           cam::GenerateAll(setup, doc->GetBodies()), {}, gcode,
                           &error));

  // Re-expand the file and check every point against the true toolpath
  // shape: the offset outline is a 46 x 36 rectangle with 3 mm corners, so
  // every cutting point is exactly 3 mm from the part's boundary.
  const cam::GProgram program = cam::ParseGcode(gcode);
  REQUIRE(program.errors.empty());
  int checked = 0;
  for (const cam::GMove& m : program.moves) {
    if (m.rapid || m.to.z > 19.0)
      continue;
    // Distance from the 0..40 x 0..30 rectangle, which must be the radius.
    const double dx = std::max({0.0 - m.to.x, 0.0, m.to.x - 40.0});
    const double dy = std::max({0.0 - m.to.y, 0.0, m.to.y - 30.0});
    const double d = std::hypot(dx, dy);
    INFO("point " << m.to.x << "," << m.to.y << " at " << d);
    CHECK(std::abs(d - 3.0) < 0.03);
    checked++;
  }
  CHECK(checked > 20); // the loop actually ran
}

// --- C4: frozen G-code, per dialect --------------------------------------

namespace {

// The job the goldens freeze: one part, one tool, two operations, chosen to
// exercise a tool change comment, a tabbed profile and a facing spiral.
cam::Setup GoldenJob() {
  cam::Setup setup = BasicSetup();
  setup.name = "Golden";
  setup.safeZ = 20.0;
  cam::Operation face;
  face.id = 1;
  face.name = "Face";
  face.type = cam::OpType::Face;
  face.toolNumber = 1;
  face.topZ = 21.0;
  face.bottomZ = 20.0;
  face.stepover = 0.5;
  setup.operations.push_back(face);
  cam::Operation profile;
  profile.id = 2;
  profile.name = "Profile";
  profile.type = cam::OpType::Profile;
  profile.toolNumber = 1;
  profile.topZ = 20.0;
  profile.bottomZ = 0.0;
  profile.stepdown = 8.0;
  profile.tabCount = 4;
  setup.operations.push_back(profile);
  return setup;
}

} // namespace

TEST_CASE("cam: the emitted program is frozen, per dialect", "[cam][golden]") {
  // Substring checks prove the pieces are present; only a byte-for-byte
  // golden proves the FILE stopped changing. Regenerate deliberately with
  // DESIGNER_REFRESH_CAM_GOLDEN=1 and read the diff before committing it —
  // every line of this file eventually moves a machine.
  auto doc = BlockDoc(40, 30, 20);
  const cam::Setup setup = GoldenJob();
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());

  for (const std::string& post : cam::PostIds()) {
    std::string gcode, error;
    REQUIRE(cam::PostProcess(post, setup, paths, {}, gcode, &error));
    const std::filesystem::path golden =
        std::filesystem::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
        ("cam-block-" + post + ".nc");

    if (std::getenv("DESIGNER_REFRESH_CAM_GOLDEN") != nullptr) {
      std::ofstream(golden) << gcode;
      continue;
    }
    INFO("golden: " << golden.string());
    REQUIRE(std::filesystem::exists(golden));
    std::ifstream in(golden);
    const std::string want((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    // Point at the first differing line rather than dumping two programs.
    size_t line = 1, at = 0;
    while (at < want.size() && at < gcode.size() && want[at] == gcode[at]) {
      if (want[at] == '\n')
        line++;
      at++;
    }
    INFO("first difference at line " << line);
    CHECK(gcode == want);
  }
}

// --- R15: tool shapes and the edge break -------------------------------------

TEST_CASE("cam: a tool's shape is what the cutter leaves", "[cam]") {
  // TipHeight is the one place the tool's profile is written down; the
  // verifier's material removal and the edge geometry both read it, so they
  // cannot disagree about the shape of the cutter.
  const cam::Tool flat = ShapedTool(cam::ToolType::Flat, 6.0);
  CHECK_THAT(cam::TipHeight(flat, 0.0), WithinAbs(0.0, 1e-12));
  CHECK_THAT(cam::TipHeight(flat, 3.0), WithinAbs(0.0, 1e-12));

  // A Ø6 ball: flat at the tip, a full radius up at its edge.
  const cam::Tool ball = ShapedTool(cam::ToolType::Ball, 6.0);
  CHECK_THAT(cam::TipHeight(ball, 0.0), WithinAbs(0.0, 1e-12));
  CHECK_THAT(cam::TipHeight(ball, 3.0), WithinAbs(3.0, 1e-9));
  CHECK_THAT(cam::TipHeight(ball, 1.8), WithinAbs(3.0 - 2.4, 1e-9)); // 3-4-5

  // A 90 degree V-bit rises one millimetre per millimetre out from the axis;
  // a 60 degree one rises faster.
  CHECK_THAT(cam::TipHeight(ShapedTool(cam::ToolType::VBit, 6.0, 90.0), 2.0),
             WithinAbs(2.0, 1e-9));
  CHECK_THAT(cam::TipHeight(ShapedTool(cam::ToolType::VBit, 6.0, 60.0), 2.0),
             WithinAbs(2.0 / std::tan(std::numbers::pi / 6.0), 1e-9));
}

TEST_CASE("cam: a V-bit chamfers the edge it is pointed at", "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  setup.tools = {ShapedTool(cam::ToolType::VBit, 12.0, 90.0)};
  const ElementRef wall = SideWall(*doc);
  REQUIRE(wall.IsSet());

  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::EdgeRound, wall);
  INFO(derived.why);
  REQUIRE(derived.ok);
  // Nothing typed: the edge is the wall's own top (z = 20), and the bevel is
  // the biggest a Ø12 90-degree bit can cut — its radius, 6 mm — which at
  // 45 degrees is 6 mm deep.
  CHECK_THAT(derived.op.topZ, WithinAbs(20.0, 1e-6));
  CHECK_THAT(derived.op.edgeSize, WithinAbs(6.0, 1e-6));
  CHECK_THAT(derived.op.bottomZ, WithinAbs(14.0, 1e-6));

  cam::Setup job = setup;
  cam::Operation op = derived.op;
  op.id = 1;
  op.edgeSize = 2.0; // a 2 mm chamfer instead
  job.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(job, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  // One lap, its axis ON the contour, two millimetres down: the cone's flank
  // is the bevel, so the tip lands exactly on the wall at its bottom.
  CHECK_THAT(paths[0].MinZ(), WithinAbs(18.0, 1e-6));
  double onContour = 0.0, cuts = 0.0;
  for (const cam::Move& mv : paths[0].moves) {
    if (mv.kind == cam::Move::Kind::Rapid)
      continue;
    cuts++;
    // The block is 40 x 30 at the origin: a point on its outline has x or y
    // on one of the four sides.
    const bool onX =
        std::abs(mv.to.x) < 1e-6 || std::abs(mv.to.x - 40.0) < 1e-6;
    const bool onY =
        std::abs(mv.to.y) < 1e-6 || std::abs(mv.to.y - 30.0) < 1e-6;
    if (onX || onY)
      onContour++;
  }
  CHECK(cuts > 4);
  CHECK(onContour == cuts);
}

TEST_CASE("cam: a ball nose rounds the edge over an arc", "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  setup.tools = {ShapedTool(cam::ToolType::Ball, 6.0)};
  const ElementRef wall = SideWall(*doc);
  REQUIRE(wall.IsSet());

  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::EdgeRound, wall);
  INFO(derived.why);
  REQUIRE(derived.ok);
  CHECK(derived.op.name == "Round the edge");
  CHECK_THAT(derived.op.edgeSize, WithinAbs(3.0, 1e-6)); // the ball's radius

  cam::Setup job = setup;
  cam::Operation op = derived.op;
  op.id = 1;
  job.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(job, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());

  // The passes walk the arc: pass i has its centre R + r from the round's
  // own axis (3 mm in from the wall, 3 mm below the top) at angle t, and its
  // leftmost lap therefore runs at a known x and a known Z. Assert the
  // programme contains each of them — the ramp into a pass shares the XY and
  // sits above it, so looking for the depth itself is the honest check.
  const double R = 3.0, r = 3.0;
  const int passes =
      static_cast<int>(std::ceil(R * std::numbers::pi * 0.5 / (0.45 * 6.0)));
  REQUIRE(passes >= 2);
  for (int i = 0; i <= passes; i++) {
    const double t = std::numbers::pi * 0.5 * (1.0 - double(i) / passes);
    const double x = R - (R + r) * std::cos(t); // the lap's left side
    const double z = 20.0 - R + (R + r) * std::sin(t) - r; // the ball's TIP
    int at = 0;
    for (const cam::Move& mv : paths[0].moves)
      if (mv.kind != cam::Move::Kind::Rapid && std::abs(mv.to.x - x) < 1e-4 &&
          std::abs(mv.to.z - z) < 1e-4)
        at++;
    INFO("pass " << i << " expected x " << x << " z " << z);
    CHECK(at >= 2); // both ends of that side, at depth
  }
  // It grazes the flat top on the first pass and hangs its own radius below
  // the deepest contact on the last.
  CHECK_THAT(paths[0].MinZ(), WithinAbs(20.0 - R - r, 1e-6));
}

TEST_CASE("cam: a flat mill is refused an edge break", "[cam]") {
  auto doc = BlockDoc();
  const cam::Setup setup = BasicSetup(); // a 6 mm FLAT end mill
  const ElementRef wall = SideWall(*doc);
  REQUIRE(wall.IsSet());

  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::EdgeRound, wall);
  REQUIRE_FALSE(derived.ok);
  // Names the tool it has and the two that would work — substituting one
  // would cut a square step where a chamfer was asked for.
  CHECK(derived.why.find("flat end mill") != std::string::npos);
  CHECK(derived.why.find("ball") != std::string::npos);
  CHECK(derived.why.find("V-bit") != std::string::npos);

  // Asked for a bevel wider than the cutter can reach, it says how wide it
  // can go rather than cutting a smaller one silently.
  cam::Setup vee = setup;
  vee.tools = {ShapedTool(cam::ToolType::VBit, 6.0, 90.0)};
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::EdgeRound;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 10.0;
  op.edgeSize = 8.0; // the tool's radius is 3
  op.targets = {wall};
  vee.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(vee, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  CHECK(paths[0].moves.empty());
  CHECK(paths[0].error.find("3 mm bevel at most") != std::string::npos);
}

TEST_CASE("cam: a tool's shape survives a save and reload", "[cam]") {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "designer_cam_toolshape.json";
  {
    auto doc = BlockDoc();
    cam::Setup& setup = doc->AddCamSetup();
    setup.name = "Job";
    setup.tools = {ShapedTool(cam::ToolType::VBit, 12.0, 60.0)};
    cam::Operation op;
    op.id = doc->AllocateId();
    op.type = cam::OpType::EdgeRound;
    op.topZ = 20.0;
    op.bottomZ = 16.0;
    op.edgeSize = 2.5;
    setup.operations.push_back(op);
    REQUIRE(DocumentIO::Save(*doc, path.string()).has_value());
  }
  auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded.has_value());
  REQUIRE((*loaded)->GetCamSetups().size() == 1);
  const cam::Setup& setup = (*loaded)->GetCamSetups()[0];
  REQUIRE(setup.tools.size() == 1);
  // A V-bit that reloads as a flat mill is a chamfer that reloads as a
  // square step — the substitution this project refuses everywhere else.
  CHECK(setup.tools[0].type == cam::ToolType::VBit);
  CHECK_THAT(setup.tools[0].tipAngleDeg, WithinAbs(60.0, 1e-9));
  REQUIRE(setup.operations.size() == 1);
  CHECK(setup.operations[0].type == cam::OpType::EdgeRound);
  CHECK_THAT(setup.operations[0].edgeSize, WithinAbs(2.5, 1e-9));
  std::filesystem::remove(path);
}

TEST_CASE("cam verify: an unmodelled edge break says why it gouges", "[cam]") {
  // The verifier compares the cut stock with the DESIGNED part, so a round
  // the toolpath makes and the model does not have is, correctly, material
  // removed below the designed surface. Left at that the verdict reads as
  // scrap on every edge-break job — so the explanation names the cause and
  // the fix (model the break) instead of leaving the operator to guess.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  setup.tools = {ShapedTool(cam::ToolType::Ball, 6.0)};
  const ElementRef wall = SideWall(*doc);
  REQUIRE(wall.IsSet());
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::EdgeRound, wall);
  REQUIRE(derived.ok);
  cam::Operation op = derived.op;
  op.id = 1;
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  // The shape of the tool rides in the file's own comments: "T1 D6" alone is
  // the same line for an end mill and a ball nose, and the operator standing
  // at the machine is the one who has to tell them apart.
  CHECK(gcode.find("ball") != std::string::npos);

  const cam::Verdict v =
      cam::VerifyGcode(gcode, setup, doc->GetBodies(), /*res=*/0.4);
  CHECK(v.maxGouge > 0.6); // the round itself, honestly measured
  bool explained = false;
  for (const cam::VerdictLine& line : cam::Explain(v, 0.6, 0.0,
                                                   /*breaksAnEdge=*/true))
    if (line.text.find("not in the MODEL") != std::string::npos)
      explained = true;
  CHECK(explained);
  // And it is only said when the job actually breaks an edge.
  for (const cam::VerdictLine& line : cam::Explain(v, 0.6, 0.0, false))
    CHECK(line.text.find("not in the MODEL") == std::string::npos);
}

TEST_CASE("cam: one operation at a time cuts what all of them cut", "[cam]") {
  // The panel regenerates ONLY the operation a user just touched — adding,
  // deleting or editing one no longer re-cuts the whole job, which used to
  // cost the slowest operation in it on every edit (one adaptive pocket
  // measured 7.1 s). That is only safe while a single Generate and a whole
  // GenerateAll agree, which is what this pins.
  auto doc = PocketDoc();
  cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());

  cam::Operation pocket =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket, floor)
          .op;
  pocket.id = 1;
  cam::Operation profile =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Profile, {})
          .op;
  profile.id = 2;
  cam::Operation disabled = profile;
  disabled.id = 3;
  disabled.enabled = false;
  setup.operations = {pocket, profile, disabled};

  const std::vector<cam::Toolpath> all =
      cam::GenerateAll(setup, doc->GetBodies());
  // A disabled operation is not in the job — the incremental path drops its
  // entry for exactly this reason.
  REQUIRE(all.size() == 2);
  for (const cam::Toolpath& whole : all) {
    const cam::Operation* op = nullptr;
    for (const cam::Operation& candidate : setup.operations)
      if (candidate.id == whole.operationId)
        op = &candidate;
    REQUIRE(op != nullptr);
    const cam::Toolpath one = cam::Generate(setup, *op, doc->GetBodies());
    INFO("operation " << op->id);
    CHECK(one.error == whole.error);
    CHECK(one.toolNumber == whole.toolNumber);
    REQUIRE(one.moves.size() == whole.moves.size());
    CHECK_THAT(one.CutLength(), WithinRel(whole.CutLength(), 1e-12));
    CHECK_THAT(one.MinZ(), WithinAbs(whole.MinZ(), 1e-12));
    for (size_t i = 0; i < one.moves.size(); i++) {
      REQUIRE(one.moves[i].kind == whole.moves[i].kind);
      REQUIRE_THAT(one.moves[i].to.x, WithinAbs(whole.moves[i].to.x, 1e-12));
      REQUIRE_THAT(one.moves[i].to.y, WithinAbs(whole.moves[i].to.y, 1e-12));
      REQUIRE_THAT(one.moves[i].to.z, WithinAbs(whole.moves[i].to.z, 1e-12));
    }
  }
}

#endif // HAVE_CAM
