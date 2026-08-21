#ifdef HAVE_CAM

#include "core/Document.h"
#include "core/Feature.h"
#include "core/cam/CamAdaptive.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "core/features/BooleanFeature.h"
#include "core/io/Serialization.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  CHECK(tp.moves[1].kind == cam::Move::Kind::Rapid);
  CHECK_THAT(tp.moves[1].to.z, WithinAbs(20.0, 1e-9)); // stock top, not depth
  CHECK(tp.moves[2].kind == cam::Move::Kind::Cut);
  CHECK(tp.moves[2].to.z < 20.0); // descending...
  CHECK(tp.moves[2].to.z > 0.0);  // ...but not all the way in one move
  CHECK_THAT(tp.moves[2].feed, WithinRel(200.0, 1e-9)); // at plunge feed
  // No vertical plunge anywhere in a profile.
  for (const cam::Move& m : tp.moves)
    CHECK(m.kind != cam::Move::Kind::Plunge);
}

TEST_CASE("cam: an inside profile runs inside the part", "[cam]") {
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
  REQUIRE(tp.error.empty());
  // Inward by 3: a 34x24 rectangle, 116 mm — again a ramp lap plus a cut lap.
  CHECK_THAT(tp.CutLength(), WithinRel(2.0 * 2.0 * (34.0 + 24.0), 5e-3));
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

TEST_CASE("cam: tabs lift the tool on the last pass only", "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 4;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 0.0;
  op.stepdown = 10.0; // two passes: z=10 then z=0
  op.tabCount = 4;
  op.tabWidth = 6.0;
  op.tabHeight = 1.5;

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  REQUIRE(tp.error.empty());
  int atTabHeight = 0, atFloor = 0, atMidPass = 0;
  for (const cam::Move& m : tp.moves) {
    if (m.kind != cam::Move::Kind::Cut)
      continue;
    if (std::abs(m.to.z - 1.5) < 1e-6)
      atTabHeight++;
    else if (std::abs(m.to.z) < 1e-6)
      atFloor++;
    else if (std::abs(m.to.z - 10.0) < 1e-6)
      atMidPass++;
  }
  CHECK(atTabHeight > 0); // the tabs exist
  CHECK(atFloor > 0);     // and the rest of the last pass is at depth
  CHECK(atMidPass > 0);   // the earlier pass ran full depth
  // The tabs are on the FINAL pass: no move at the intermediate depth was
  // lifted to tab height.
  CHECK(tp.MinZ() >= -1e-9);
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
  CHECK(v.maxGouge > 4.0); // it cut ~5 mm below the top face
  CHECK_FALSE(v.Ok(0.1));
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
  CHECK_FALSE(v.Ok(1.0));
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

} // namespace

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

#endif // HAVE_CAM
