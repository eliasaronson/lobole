#ifdef HAVE_CAM

// The verifier: read the posted G-code back, cut it into the stock, and
// report gouge, leftover and the verdict in words.
// One of five files split from test_cam.cpp
// (docs/architecture.md#source-layout).

#include "cam_fixtures.h"

#include "core/Parallel.h"
#include "core/cam/CamOtherSetups.h"

#include <chrono>
#include <cstring>
#include <nlohmann/json.hpp>

// --- C5: verifying the posted file ---------------------------------------

TEST_CASE("cam verify: the parser READS the A word rather than judging it",
          "[cam]") {
  // An A word used to fall into the parser's `default:` and vanish, so a
  // file whose part turns was simulated as though it stood still. It is
  // recorded now -- modal, like every other word -- and what to DO about a
  // moving A is decided where the stock model is chosen, not here. A parser
  // that refused would also have to know what it was being parsed for.
  const cam::GProgram indexed = cam::ParseGcode("G0 Z25\n"
                                                "G0 A90\n"
                                                "G0 X0 Y0\n"
                                                "G1 Z-1 F300\n"
                                                "G1 X10\n");
  REQUIRE(indexed.errors.empty());
  REQUIRE(indexed.moves.size() >= 3);
  // Modal: the A stated once before the cut is carried by every move after
  // it, and the moves before it state none -- which is a fact about the
  // file, not a zero.
  CHECK_FALSE(indexed.moves.front().hasA);
  CHECK(indexed.moves.back().hasA);
  CHECK_THAT(indexed.moves.back().a, WithinAbs(90.0, 1e-9));
  // One constant A is an INDEX: the part stands still while the tool moves,
  // which is the same question a 3-axis check answers.
  CHECK_FALSE(cam::RotatesWhileCutting(indexed));

  const cam::GProgram turning = cam::ParseGcode("G0 Z25\n"
                                                "G0 A0\n"
                                                "G1 Z-1 F300\n"
                                                "G1 X10 A45\n"
                                                "G1 X20 A90\n");
  REQUIRE(turning.errors.empty());
  CHECK(cam::RotatesWhileCutting(turning));

  // An ARC does not fill the A word with its own parameter. The re-expansion
  // walks an angle it also called `a`, which shadowed the modal one and
  // wrote the arc parameter into every move's rotary axis -- so an ordinary
  // indexed job read as a trunnion sweeping from -pi to pi.
  const cam::GProgram arc = cam::ParseGcode("G0 Z25\n"
                                            "G0 A90\n"
                                            "G0 X0 Y0\n"
                                            "G1 Z-1 F300\n"
                                            "G2 X10 Y10 I10 J0\n");
  REQUIRE(arc.moves.size() > 4);
  for (const cam::GMove& m : arc.moves)
    if (m.hasA)
      CHECK_THAT(m.a, WithinAbs(90.0, 1e-9));
  CHECK_FALSE(cam::RotatesWhileCutting(arc));
}

TEST_CASE("cam verify: the 3-axis check refuses a part that turns", "[cam]") {
  // The height map is z(x, y) and cannot represent a part that rotates, so
  // every cut would be applied as though it stood still and the gouge and
  // leftover would be measurements of a program that does not exist. The
  // planar path says so instead (`InspectRotary` is the model that can).
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  const std::string turning = "(op 1)\nG0 Z25\nG0 X0 Y0 A0\nG1 Z19 F300\n"
                              "G1 X40 A180\nG0 Z25\n";
  const cam::Inspection flat =
      cam::Inspect(turning, setup, doc->GetBodies(), 0.5);
  REQUIRE(flat.ok);
  INFO(flat.verdict.firstFileError);
  CHECK(flat.verdict.fileErrors > 0);
  CHECK(flat.verdict.firstFileError.find("A axis MOVES") != std::string::npos);
  CHECK_FALSE(flat.verdict.Safe(0.01));
}

TEST_CASE("cam verify: a cut that bites deeper than its passes is reported",
          "[cam]") {
  // One slot across a 40 x 30 x 20 block, 8 mm deep: once in a single cut,
  // once in four 2 mm passes. The op asks for 2 mm passes both times, so the
  // first is a cutter buried four stepdowns deep, full width -- the load that
  // breaks tools and shifts the work, and that nothing else in the verdict
  // measures (the part comes out the same either way).
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::On;
  op.topZ = 20.0;
  op.bottomZ = 12.0;
  op.stepdown = 2.0;
  setup.operations.push_back(op);
  // Stock flush with the part, so the first pass takes the 2 mm it says
  // (the default leaves a millimetre on top).
  setup.stock.margin = glm::dvec3(0.0);

  const std::string buried = "(op 1)\nG0 Z25\nG0 X-5 Y15\nG1 Z12 F200\n"
                             "G1 X45 F600\nG0 Z25\n";
  const cam::Verdict bad =
      cam::VerifyGcode(buried, setup, doc->GetBodies(), 0.25);
  INFO("worst bite " << bad.worstBite << " allowed " << bad.worstBiteAllowed);
  CHECK(bad.overBites > 0);
  CHECK_THAT(bad.worstBite, WithinAbs(8.0, 0.5));
  CHECK_THAT(bad.worstBiteAllowed, WithinAbs(2.0, 1e-9));
  CHECK(bad.worstBiteOp == 1);
  CHECK_FALSE(bad.Safe(0.01));
  bool said = false;
  for (const cam::VerdictLine& l : cam::Explain(bad, 0.05, 1.0))
    said = said || (l.level == cam::VerdictLine::Level::Bad &&
                    l.text.find("bite deeper") != std::string::npos);
  CHECK(said);

  std::string stepped = "(op 1)\nG0 Z25\nG0 X-5 Y15\n";
  for (const int z : {18, 16, 14, 12})
    stepped += "G1 Z" + std::to_string(z) + " F200\nG1 X" +
               (z % 4 == 2 ? "45" : "-5") + " F600\n";
  stepped += "G0 Z25\n";
  const cam::Verdict good =
      cam::VerifyGcode(stepped, setup, doc->GetBodies(), 0.25);
  REQUIRE(good.biteByOp.count(1) == 1);
  INFO("stepped bite " << good.biteByOp.at(1).first);
  CHECK(good.overBites == 0);
  CHECK(good.biteByOp.at(1).first < 2.3);

  // A drilling op is not measured: a peck is the whole depth by design.
  setup.operations[0].type = cam::OpType::Drill;
  CHECK(cam::VerifyGcode(buried, setup, doc->GetBodies(), 0.25).overBites == 0);
}

TEST_CASE("cam verify: a shank wider than the cutter is found at the wall",
          "[cam]") {
  // A 3 mm cutter with 12 mm of flute on a 6 mm shank, slotting 15 mm into a
  // block in 2 mm passes. Every pass bites what it should; it is the last
  // two that put the shank below the stock top, between walls 3 mm apart.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  setup.stock.margin = glm::dvec3(0.0);
  setup.tools[0].diameter = 3.0;
  setup.tools[0].fluteLength = 12.0;
  setup.tools[0].shankDiameter = 6.0;
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::On;
  op.topZ = 20.0;
  op.bottomZ = 5.0;
  op.stepdown = 2.0;
  setup.operations.push_back(op);
  std::string g = "(op 1)\nG0 Z25\nG0 X-5 Y15\n";
  bool out = true;
  for (const int z : {18, 16, 14, 12, 10, 8, 6, 5}) {
    g += "G1 Z" + std::to_string(z) + " F200\nG1 X" + (out ? "45" : "-5") +
         " F600\n";
    out = !out;
  }
  g += "G0 Z25\n";
  const cam::Verdict hit = cam::VerifyGcode(g, setup, doc->GetBodies(), 0.25);
  INFO("shank " << hit.shankCollisions << " worst " << hit.worstShankDepth);
  CHECK(hit.shankCollisions > 0);
  // At the 5 mm pass the flutes end at 17: 3 mm of wall stands above them.
  CHECK_THAT(hit.worstShankDepth, WithinAbs(3.0, 0.3));
  CHECK(hit.overBites == 0);
  CHECK_FALSE(hit.Safe(0.01));
  bool said = false;
  for (const cam::VerdictLine& l : cam::Explain(hit, 0.05, 1.0))
    said = said || l.text.find("SHANK") != std::string::npos;
  CHECK(said);

  // The same cut with the shank the cutter's width, or unstated: nothing
  // here -- the depth alone is the reach finding's, at the op.
  for (const double shank : {3.0, 0.0}) {
    setup.tools[0].shankDiameter = shank;
    CHECK(cam::VerifyGcode(g, setup, doc->GetBodies(), 0.25).shankCollisions ==
          0);
  }
}

TEST_CASE("cam verify: a verdict line is never cut short", "[cam]") {
  // At 256 characters the rapid line lost its last words ("...not been cut
  // y"): every sentence Explain writes ends where it was written to.
  cam::Verdict v;
  v.cells = 1000; // something was compared, or there is nothing to explain
  v.rapidsThroughStock = 12;
  v.worstRapidDepth = 30.0;
  v.worstRapidAt = {163.625, 27.375};
  v.worstRapidLine = 14275;
  bool found = false;
  for (const cam::VerdictLine& l : cam::Explain(v, 0.05, 1.0))
    if (l.text.find("rapids through material") != std::string::npos) {
      found = true;
      INFO(l.text);
      CHECK(l.text.size() > 256);
      CHECK(l.text.substr(l.text.size() - 6) == "steep.");
    }
  CHECK(found);
}

TEST_CASE("cam verify: what the parser found reaches the verdict", "[cam]") {
  // The parser's errors and warnings were computed and DROPPED. `Inspect`
  // parsed the program, built its findings and returned none of them --
  // tested at the parser (test_cam_machine.cpp's nested comment) and
  // consumed by nobody, so a NaN coordinate, a feed of zero and a file Mach3
  // will not load all verified clean. That is the exact failure this
  // module's own header says it exists to catch.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();

  // A readable file first, so the case is not passing on an empty verdict.
  const std::string clean = "(op 1)\nG0 Z25\nG0 X0 Y0\nG1 Z19 F300\n"
                            "G1 X40\nG0 Z25\n";
  const cam::Verdict ok = cam::VerifyGcode(clean, setup, doc->GetBodies(), 0.5);
  CHECK(ok.fileErrors == 0);
  CHECK(ok.fileWarnings == 0);

  // A NaN coordinate: the bug the header says had already shipped once.
  const cam::Verdict nan =
      cam::VerifyGcode("(op 1)\nG0 Z25\nG1 Xnan Ynan Z19 F300\nG0 Z25\n", setup,
                       doc->GetBodies(), 0.5);
  INFO(nan.firstFileError);
  CHECK(nan.fileErrors > 0);
  CHECK(nan.firstFileError.find("axis word with no number") !=
        std::string::npos);
  // ...and it makes the file unsafe, because everything else in the verdict
  // was measured by simulating the part of the program that WAS readable.
  CHECK_FALSE(nan.Safe(0.01));
  bool said = false;
  for (const cam::VerdictLine& l : cam::Explain(nan, 0.01, 1.0))
    said = said || l.text.find("could not be read") != std::string::npos;
  CHECK(said);

  // A nested comment: the parser survives it and Mach3 does not, so it is a
  // WARNING -- said out loud, and not counted against the file.
  const cam::Verdict nested = cam::VerifyGcode(
      "(tool 1: 6 mm flat (through cuts) D6)\nG0 Z25\nG0 X0 Y0\n"
      "G1 Z19 F300\nG1 X40\nG0 Z25\n",
      setup, doc->GetBodies(), 0.5);
  INFO(nested.firstFileWarning);
  CHECK(nested.fileWarnings > 0);
  // Not counted: a warning is a file that loads HERE and may not load THERE.
  // (This fixture cuts into the block on purpose, so its Safe() is about the
  // gouge and says nothing either way -- `fileErrors` is the assertion.)
  CHECK(nested.fileErrors == 0);
  bool warned = false;
  for (const cam::VerdictLine& l : cam::Explain(nested, 0.01, 1.0))
    warned =
        warned || l.text.find("a CONTROLLER may refuse") != std::string::npos;
  CHECK(warned);
}

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
  // Safe -- and not a job. With no leftover measured it read as complete,
  // which is how the chess rook's crenel setups "verified clean" for a day
  // while posting files that cut nothing.
  CHECK(v.CutsNothing());
  CHECK(v.Safe(0.5));
  CHECK_FALSE(v.Complete(0.5));
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

  // HOW DEEP, because a count cannot be acted on. A ball lifting out of a
  // groove of its own shape grazes a few hundredths; a rapid across a rib
  // buries the cutter. Both were one sentence until the depth was reported.
  cam::Verdict rub;
  rub.cells = 100;
  rub.rapidsThroughStock = 121;
  rub.firstBadLine = 7;
  rub.worstRapidDepth = 0.04;
  rub.worstRapidLine = 9;
  const std::vector<cam::VerdictLine> soft = cam::Explain(rub, 0.1, 0.0);
  REQUIRE_FALSE(soft.empty());
  INFO(soft[0].text);
  CHECK(soft[0].text.find("0.04 mm") != std::string::npos);
  CHECK(soft[0].text.find("line 9") !=
        std::string::npos); // the WORST, not the first
  CHECK(soft[0].text.find("a rub") != std::string::npos);
  CHECK(soft[0].level == cam::VerdictLine::Level::Warn); // not Bad

  rub.worstRapidDepth = 0.79;
  const std::vector<cam::VerdictLine> hard = cam::Explain(rub, 0.1, 0.0);
  REQUIRE_FALSE(hard.empty());
  INFO(hard[0].text);
  CHECK(hard[0].text.find("0.79 mm") != std::string::npos);
  CHECK(hard[0].level == cam::VerdictLine::Level::Bad);
  // And the advice names BOTH causes, because raising Safe Z does nothing
  // for a retract whose rim is in the strip beside it -- measured on the
  // waveguide, where safe Z of 28.7, 35 and 45 all gave the same count.
  CHECK(hard[0].text.find("RETRACT") != std::string::npos);
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
                           cam::Target{0, UpFaceAt(*doc, 12.0)})
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

TEST_CASE("cam verify: a job with tabs is SAFE without being COMPLETE",
          "[cam]") {
  // The two questions have different answers, and one flag could not carry
  // both: tabs are material deliberately left standing, so a job that cuts
  // the part correctly reports leftover forever. Gating on one boolean meant
  // gating on a boolean no real job could ever set
  // (docs/cam/simulation.md#verdict).
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{});
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
      setup, doc->GetBodies(), cam::OpType::EdgeRound, cam::Target{0, wall});
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

TEST_CASE("cam verify: a through cut is not a gouge", "[cam]") {
  // A through cut ends BELOW the workpiece on purpose — into the spoilboard —
  // so the cutter leaves no web holding the part in the sheet. The verifier
  // compared cell to cell against a designed surface whose floor IS the
  // stock's bottom, so every such job reported the overcut as material removed
  // from the part and said "that is scrap" about correct G-code.
  //
  // Measured on the real smart_speaker front_wall: both setups reported
  // 0.50 mm of gouge, which is exactly the -0.5 bottomZ its profile ops carry.
  // A verdict that cries wolf on every through cut teaches an operator to skip
  // the line that would have mattered, which is worse than not checking.
  auto doc = BlockDoc(40, 30, 10);
  cam::Setup setup = BasicSetup();
  // No bottom margin: the stock floor is the part's own bottom face, which is
  // the case that produces the false gouge (and is what a sheet clamped to a
  // spoilboard actually is).
  setup.stock.bottomMargin = 0.0;
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.side = cam::Side::Outside;
  op.topZ = 11.0;
  op.bottomZ = -0.5; // through the part, 0.5 into the spoilboard
  op.stepdown = 5.0;
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v =
      cam::VerifyGcode(gcode, setup, doc->GetBodies(), /*res=*/0.4);
  REQUIRE(v.cells > 0);
  INFO("gouge " << v.maxGouge << " below stock " << v.belowStock);
  // The part is untouched...
  CHECK(v.maxGouge < 0.5); // one cell of quantisation at 0.4 mm
  CHECK(v.Safe(0.1));
  // ...and the overcut is REPORTED rather than dropped. Silently discarding it
  // would be the substitution this codebase refuses; it is a real fact about
  // the program, it is just not a fact about the part.
  CHECK(v.belowStock > 0.4);
  CHECK(v.belowStock < 0.7);

  // The gouge check still bites where it should: the same job cut 3 mm deeper
  // than the part is thick has nowhere to put that depth except the part.
  cam::Setup deep = setup;
  deep.operations[0].side = cam::Side::Inside;
  deep.operations[0].targets.clear();
  const cam::Verdict shallow =
      cam::VerifyGcode("G90 G21\nG0 X20 Y15 Z20\nG1 Z-5 F100\nM30\n", setup,
                       doc->GetBodies(), 0.4);
  INFO("plunge gouge " << shallow.maxGouge);
  CHECK(shallow.maxGouge > 5.0); // straight down through 10 mm of part
  CHECK_FALSE(shallow.Safe(0.1));
}

TEST_CASE("cam verify: a cell on the cutter's wall is outside by a decision, "
          "not by rounding",
          "[cam]") {
  // docs/cam/simulation.md#the-cutters-wall. A toolpath on the grid's own
  // lattice puts a 6 mm cutter's wall EXACTLY on cell centres over a
  // 0.25 mm grid, and whether d <= R there was the last bit of the stock's
  // x0: nudged a picometre one way the +X wall cell was cut and the -X one
  // was not, the other way round the reverse. Plunged, and traversed along
  // Y, the carve must take the same metal whichever way the grid rounds.
  cam::Tool flat;
  flat.diameter = 6.0;
  flat.type = cam::ToolType::Flat;
  const double res = 0.25;
  const double c = (40 + 0.5) * res; // over cell (40, 40)'s centre
  const auto carve = [&](double nudge, bool traverse) {
    cam::HeightMap stock =
        cam::MakeStock(glm::dvec3(nudge, 0.0, -10.0),
                       glm::dvec3(20.0 + nudge, 20.0, 0.0), res);
    const glm::dvec3 from(c, c, 5.0);
    const glm::dvec3 to(c, traverse ? c + 2.0 : c, -3.0);
    return cam::CarveSweep(from, to, &flat, stock);
  };
  for (const bool traverse : {false, true}) {
    INFO((traverse ? "traverse" : "plunge"));
    const double nominal = carve(0.0, traverse);
    CHECK_THAT(carve(1e-12, traverse), WithinAbs(nominal, 1e-6));
    CHECK_THAT(carve(-1e-12, traverse), WithinAbs(nominal, 1e-6));
  }
}

// --- The planar check on every core (docs/cam/simulation.md#on-every-core) --

namespace {
struct ScopedSweepThreads {
  explicit ScopedSweepThreads(const char* n) {
    if (const char* was = std::getenv("DESIGNER_SWEEP_THREADS"))
      m_was = was;
    setenv("DESIGNER_SWEEP_THREADS", n, 1);
  }
  ~ScopedSweepThreads() {
    if (m_was.empty())
      unsetenv("DESIGNER_SWEEP_THREADS");
    else
      setenv("DESIGNER_SWEEP_THREADS", m_was.c_str(), 1);
  }
  std::string m_was;
};

bool SameBytes(const cam::HeightMap& a, const cam::HeightMap& b) {
  return a.nx == b.nx && a.ny == b.ny && a.x0 == b.x0 && a.y0 == b.y0 &&
         a.res == b.res && a.z.size() == b.z.size() &&
         std::memcmp(a.z.data(), b.z.data(), a.z.size() * sizeof(float)) == 0;
}

// Every number a verdict carries that the walk or the compare produces,
// compared exactly.
void CheckSameVerdict(const cam::Verdict& a, const cam::Verdict& b) {
  CHECK(a.cells == b.cells);
  CHECK(a.untouchedFraction == b.untouchedFraction);
  CHECK(a.maxGouge == b.maxGouge);
  CHECK(a.gougeAt == b.gougeAt);
  CHECK(a.belowStock == b.belowStock);
  CHECK(a.belowStockAt == b.belowStockAt);
  CHECK(a.maxLeftover == b.maxLeftover);
  CHECK(a.leftoverAt == b.leftoverAt);
  CHECK(a.leftoverArea == b.leftoverArea);
  CHECK(a.leftoverRegions == b.leftoverRegions);
  REQUIRE(a.biggest.size() == b.biggest.size());
  for (size_t k = 0; k < a.biggest.size(); k++) {
    CHECK(a.biggest[k].area == b.biggest[k].area);
    CHECK(a.biggest[k].at == b.biggest[k].at);
    CHECK(a.biggest[k].depth == b.biggest[k].depth);
  }
  REQUIRE(a.leftoverOtherSetups.size() == b.leftoverOtherSetups.size());
  for (size_t k = 0; k < a.leftoverOtherSetups.size(); k++) {
    CHECK(a.leftoverOtherSetups[k].setup == b.leftoverOtherSetups[k].setup);
    CHECK(a.leftoverOtherSetups[k].area == b.leftoverOtherSetups[k].area);
  }
  CHECK(a.rapidsThroughStock == b.rapidsThroughStock);
  CHECK(a.firstBadLine == b.firstBadLine);
  CHECK(a.worstRapidDepth == b.worstRapidDepth);
  CHECK(a.worstRapidAt == b.worstRapidAt);
  CHECK(a.worstRapidLine == b.worstRapidLine);
  CHECK(a.overBites == b.overBites);
  CHECK(a.worstBite == b.worstBite);
  CHECK(a.worstBiteAt == b.worstBiteAt);
  CHECK(a.worstBiteLine == b.worstBiteLine);
  CHECK(a.worstBiteOp == b.worstBiteOp);
  CHECK(a.biteByOp == b.biteByOp);
  CHECK(a.shankCollisions == b.shankCollisions);
  CHECK(a.worstShankDepth == b.worstShankDepth);
  CHECK(a.worstShankAt == b.worstShankAt);
  CHECK(a.worstShankLine == b.worstShankLine);
  CHECK(a.holderCollisions == b.holderCollisions);
  CHECK(a.worstHolderDepth == b.worstHolderDepth);
  CHECK(a.worstHolderAt == b.worstHolderAt);
  CHECK(a.worstHolderLine == b.worstHolderLine);
  CHECK(a.fixtureCollisions == b.fixtureCollisions);
  CHECK(a.worstFixtureDepth == b.worstFixtureDepth);
  CHECK(a.worstFixtureAt == b.worstFixtureAt);
  CHECK(a.worstFixtureLine == b.worstFixtureLine);
  CHECK(a.worstFixtureName == b.worstFixtureName);
}

// A program that gives every part of the walk something to find: passes
// that bite what they should and one that bites twice that, plunges, arcs
// posted as chords, a tool whose shank and holder reach the walls, a clamp
// beside the block, and then a long wander at random depths with a rapid
// every few moves -- some over air, some through material an earlier line
// left standing, and some through material a later line removes, which is
// what the order of the file decides.
std::string EveryFinding() {
  std::string g = "(op 1)\nT1 M6\nG0 Z25\nG0 X-5 Y4\n";
  char buf[160];
  bool right = true;
  for (const double z : {18.0, 16.0, 12.0, 10.0, 4.0}) {
    std::snprintf(buf, sizeof buf, "G1 Z%.3f F200\nG1 X%.3f F600\n", z,
                  right ? 45.0 : -5.0);
    g += buf;
    right = !right;
  }
  // An arc across the block, and a plunge in place off it.
  g += "G0 Z25\nG0 X5 Y20\nG1 Z17 F200\nG2 X35 Y20 I15 J0 F600\n"
       "G1 Z13 F200\nG0 Z25\n";
  // Rapids into the block where nothing has cut yet.
  g += "G0 X20 Y26\nG0 Z15\nG0 Z25\nG0 X-6 Y12\nG0 Z10\nG0 X3 Z10\nG0 Z25\n";
  // The wander, from a fixed seed.
  uint32_t seed = 12345;
  const auto next = [&seed](double lo, double hi) {
    seed = seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * static_cast<double>(seed >> 8) / 16777216.0;
  };
  for (int k = 0; k < 1400; k++) {
    if (k == 700)
      g += "G0 Z25\n(op 2)\nT2 M6\n";
    const bool rapid = k % 7 == 3;
    std::snprintf(buf, sizeof buf, "%s X%.4f Y%.4f Z%.4f%s\n",
                  rapid ? "G0" : "G1", next(-4.0, 44.0), next(-4.0, 34.0),
                  next(k < 700 ? 6.0 : 12.0, 21.0), rapid ? "" : " F600");
    g += buf;
  }
  g += "G0 Z25\n";
  return g;
}

cam::Setup EveryFindingSetup() {
  cam::Setup setup = BasicSetup();
  setup.stock.margin = glm::dvec3(0.0);
  cam::Tool& t1 = setup.tools[0];
  t1.fluteLength = 7.0;
  t1.shankDiameter = 8.0;
  t1.overallLength = 14.0;
  t1.collarDiameter = 20.0;
  cam::Tool ball;
  ball.number = 2;
  ball.name = "3mm ball";
  ball.type = cam::ToolType::Ball;
  ball.diameter = 3.0;
  ball.feedCut = 600.0;
  ball.feedPlunge = 200.0;
  ball.spindleRpm = 12000.0;
  setup.tools.push_back(ball);
  cam::Operation profile;
  profile.id = 1;
  profile.type = cam::OpType::Profile;
  profile.toolNumber = 1;
  profile.topZ = 20.0;
  profile.bottomZ = 0.0;
  profile.stepdown = 2.0;
  cam::Operation pocket = profile;
  pocket.id = 2;
  pocket.type = cam::OpType::Pocket;
  pocket.toolNumber = 2;
  pocket.stepdown = 1.0;
  setup.operations = {profile, pocket};
  return setup;
}
} // namespace

TEST_CASE("cam verify: the check on every core is the serial check to the bit",
          "[cam]") {
  // docs/cam/simulation.md#on-every-core. The map is split into bands of
  // rows, each walking every move over its own rows. A cut lowers a cell to
  // the lowest the tool reaches, so its order cannot show -- but a RAPID,
  // the shank and the holder are asked about the stock as it stands at that
  // line, and a bite is the volume a stretch of cutting took. So the answer
  // must not be close to the serial one: it must BE it, for any number of
  // bands and threads.
  auto doc = BlockDoc(40, 30, 20);
  const cam::Setup setup = EveryFindingSetup();
  const std::string gcode = EveryFinding();
  Document clampDoc;
  clampDoc.AddFeature(std::make_unique<BoxFeature>(
      "Clamp", glm::vec3(-9.0f, 8.0f, 0.0f), 5.0, 8.0, 24.0));
  const std::vector<cam::FixtureSolid> fixtures = {
      {1, "Clamp", clampDoc.GetBodies()}};
  constexpr double kRes = 0.25;

  cam::Inspection serial;
  {
    ScopedSweepThreads pin("1");
    cam::ScopedInspectBands one(1);
    serial =
        cam::Inspect(gcode, setup, doc->GetBodies(), kRes, {}, {}, fixtures);
  }
  REQUIRE(serial.ok);
  const cam::Verdict& sv = serial.verdict;
  INFO("rapids " << sv.rapidsThroughStock << ", bites " << sv.overBites
                 << ", shank " << sv.shankCollisions << ", holder "
                 << sv.holderCollisions << ", fixture "
                 << sv.fixtureCollisions);
  // Or this is not testing what it says.
  REQUIRE(sv.rapidsThroughStock >= 3);
  REQUIRE(sv.overBites >= 1);
  REQUIRE(sv.shankCollisions >= 1);
  REQUIRE(sv.holderCollisions >= 1);
  REQUIRE(sv.fixtureCollisions >= 1);
  REQUIRE(sv.biteByOp.size() == 2);

  const int rows = serial.machined.ny;
  REQUIRE(rows > 100);
  for (const size_t bands :
       {size_t{0}, size_t{3}, size_t{61}, static_cast<size_t>(rows)}) {
    for (const char* threads : {"1", "8"}) {
      ScopedSweepThreads pin(threads);
      cam::ScopedInspectBands split(bands);
      INFO(bands << " bands (0: derived) on " << threads << " threads");
      const cam::Inspection banded =
          cam::Inspect(gcode, setup, doc->GetBodies(), kRes, {}, {}, fixtures);
      REQUIRE(banded.ok);
      CHECK(SameBytes(banded.machined, serial.machined));
      CHECK(SameBytes(banded.designed, serial.designed));
      CHECK(banded.touched == serial.touched);
      CheckSameVerdict(banded.verdict, sv);
    }
  }
}

TEST_CASE("cam verify: what another setup cuts is the same on every core",
          "[cam]") {
  // The compare runs in bands of rows too, and the part of it that asks the
  // other setups what they remove is the part that got heavy
  // (docs/cam/simulation.md#on-every-core): a flip job, each side cutting
  // half the outline, whose other half the compare hands to the other side.
  cam::ForgetOtherSetups();
  auto doc = BlockDoc(40, 30, 20);
  const auto side = [&](const char* name, uint64_t id, bool flipped) {
    cam::Setup s = BasicSetup();
    s.id = id;
    s.name = name;
    s.stock.margin = {3.0, 3.0, 0.0};
    s.stock.bottomMargin = 0.0;
    if (flipped)
      s.frame = cam::FlipFrame(s.frame);
    s.safeZ = cam::DeriveSafeZ(s, doc->GetBodies());
    glm::dvec3 lo(0.0), hi(0.0);
    REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));
    cam::Operation op;
    op.id = 1;
    op.type = cam::OpType::Profile;
    op.toolNumber = 1;
    op.topZ = hi.z;
    op.bottomZ = 0.5 * (lo.z + hi.z);
    op.stepdown = 5.0;
    s.operations.push_back(op);
    return s;
  };
  const cam::Setup front = side("Front", 1, false);
  const cam::Setup back = side("Back", 2, true);
  const auto post = [&](const cam::Setup& s) {
    const auto paths = cam::GenerateAll(s, doc->GetBodies());
    REQUIRE(paths.size() == 1);
    std::string gcode, error;
    REQUIRE(cam::PostProcess("mach3", s, paths, {}, gcode, &error));
    return gcode;
  };
  const std::string frontFile = post(front), backFile = post(back);
  const auto check = [&] {
    const cam::OtherSetups others =
        cam::OtherSetupsOf(front, {{&back, backFile}}, doc->GetBodies(), 0.25);
    return cam::VerifyGcode(frontFile, front, doc->GetBodies(), 0.25, {}, {},
                            {}, &others);
  };
  cam::Verdict one;
  {
    ScopedSweepThreads pin("1");
    one = check();
  }
  REQUIRE(one.leftoverOtherSetups.size() == 1);
  CHECK(one.leftoverOtherSetups[0].area > 100.0);
  ScopedSweepThreads pin("8");
  CheckSameVerdict(check(), one);
}

TEST_CASE("cam verify: the designed surface samples the same on every core",
          "[cam]") {
  // SampleModel hands each triangle to the bands of rows it covers; a cell
  // is the highest any triangle stands over it, which no order can change.
  auto doc = PocketDoc(true);
  cam::Setup setup = BasicSetup();
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(setup, doc->GetBodies(), lo, hi));
  const cam::HeightMap grid = cam::MakeStock(lo, hi, 0.1);
  cam::HeightMap one, many;
  {
    ScopedSweepThreads pin("1");
    one = cam::SampleModel(doc->GetBodies(), grid, lo.z);
  }
  {
    ScopedSweepThreads pin("8");
    many = cam::SampleModel(doc->GetBodies(), grid, lo.z);
  }
  CHECK(SameBytes(one, many));
  // Not a map of the floor: the part is in it.
  CHECK(*std::max_element(one.z.begin(), one.z.end()) > lo.z + 10.0);
}

TEST_CASE("cam verify: each other setup is inspected once, until it changes",
          "[cam]") {
  // docs/cam/simulation.md#each-other-setup-inspected-once. What a setup
  // leaves depends on its posted file, the setup, the part and the grid,
  // and on nothing about the setup asking -- so a second check reads it
  // back, and an edit to any of the four inspects it again.
  cam::ForgetOtherSetups();
  auto doc = BlockDoc(40, 30, 20);
  const auto side = [&](const char* name, uint64_t id, bool flipped,
                        double depth) {
    cam::Setup s = BasicSetup();
    s.id = id;
    s.name = name;
    s.stock.margin = {3.0, 3.0, 0.0};
    s.stock.bottomMargin = 0.0;
    if (flipped)
      s.frame = cam::FlipFrame(s.frame);
    s.safeZ = cam::DeriveSafeZ(s, doc->GetBodies());
    glm::dvec3 lo(0.0), hi(0.0);
    REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));
    cam::Operation op;
    op.id = 1;
    op.type = cam::OpType::Profile;
    op.toolNumber = 1;
    op.topZ = hi.z;
    op.bottomZ = hi.z - depth;
    op.stepdown = 5.0;
    s.operations.push_back(op);
    return s;
  };
  const auto post = [&](const cam::Setup& s, const std::vector<Body>& b) {
    const auto paths = cam::GenerateAll(s, b);
    REQUIRE(paths.size() == 1);
    std::string gcode, error;
    REQUIRE(cam::PostProcess("mach3", s, paths, {}, gcode, &error));
    return gcode;
  };
  const cam::Setup front = side("Front", 1, false, 10.0);
  cam::Setup back = side("Back", 2, true, 10.0);
  const cam::Setup deep = side("Deep", 3, true, 15.0);
  const std::string backFile = post(back, doc->GetBodies());
  const std::string deepFile = post(deep, doc->GetBodies());
  const auto fresh = [&](const cam::Setup& s, const std::string& file,
                         const std::vector<Body>& bodies, double res) {
    return cam::Inspect(file, s, bodies, res).machined;
  };
  const auto stats = [] { return cam::OtherSetupsMemo(); };

  // The first check inspects; the second reads it back, the same map.
  const cam::OtherSetups first =
      cam::OtherSetupsOf(front, {{&back, backFile}}, doc->GetBodies(), 0.5);
  CHECK(stats().misses == 1);
  CHECK(stats().hits == 0);
  REQUIRE(first.stocks.size() == 1);
  CHECK(SameBytes(first.stocks[0].height,
                  fresh(back, backFile, doc->GetBodies(), 0.5)));
  const cam::OtherSetups again = cam::OtherSetupsOf(
      front, {{&back, backFile, /*earlier=*/true}}, doc->GetBodies(), 0.5);
  CHECK(stats().misses == 1);
  CHECK(stats().hits == 1);
  REQUIRE(again.stocks.size() == 1);
  CHECK(SameBytes(again.stocks[0].height, first.stocks[0].height));
  CHECK(again.stocks[0].earlier); // the asker's, not the memo's
  CHECK_FALSE(first.stocks[0].earlier);

  // Another program: inspected again, and it is that program's map.
  const std::string edited =
      post(side("Back", 2, true, 12.0), doc->GetBodies());
  REQUIRE(edited != backFile);
  const cam::OtherSetups byProgram =
      cam::OtherSetupsOf(front, {{&back, edited}}, doc->GetBodies(), 0.5);
  CHECK(stats().misses == 2);
  CHECK(SameBytes(byProgram.stocks[0].height,
                  fresh(back, edited, doc->GetBodies(), 0.5)));

  // The same file under another stock: the setup is part of the key.
  back.stock.margin.x = 5.0;
  const cam::OtherSetups byStock =
      cam::OtherSetupsOf(front, {{&back, backFile}}, doc->GetBodies(), 0.5);
  CHECK(stats().misses == 3);
  CHECK(SameBytes(byStock.stocks[0].height,
                  fresh(back, backFile, doc->GetBodies(), 0.5)));
  CHECK_FALSE(SameBytes(byStock.stocks[0].height, first.stocks[0].height));
  back.stock.margin.x = 3.0;

  // Another part.
  auto taller = BlockDoc(40, 30, 22);
  const cam::OtherSetups byPart =
      cam::OtherSetupsOf(front, {{&back, backFile}}, taller->GetBodies(), 0.5);
  CHECK(stats().misses == 4);
  CHECK(SameBytes(byPart.stocks[0].height,
                  fresh(back, backFile, taller->GetBodies(), 0.5)));

  // Another grid.
  (void)cam::OtherSetupsOf(front, {{&back, backFile}}, doc->GetBodies(), 0.4);
  CHECK(stats().misses == 5);

  // And back to the first: still kept.
  (void)cam::OtherSetupsOf(front, {{&back, backFile}}, doc->GetBodies(), 0.5);
  CHECK(stats().misses == 5);
  CHECK(stats().hits == 2);

  // Two that miss together are inspected at once, and come back in the
  // job's order, each its own map.
  cam::ForgetOtherSetups();
  const cam::OtherSetups both = cam::OtherSetupsOf(
      front, {{&deep, deepFile}, {&back, backFile}}, doc->GetBodies(), 0.25);
  CHECK(stats().misses == 2);
  REQUIRE(both.stocks.size() == 2);
  CHECK(both.stocks[0].id == 3);
  CHECK(both.stocks[1].id == 2);
  CHECK(SameBytes(both.stocks[0].height,
                  fresh(deep, deepFile, doc->GetBodies(), 0.25)));
  CHECK(SameBytes(both.stocks[1].height,
                  fresh(back, backFile, doc->GetBodies(), 0.25)));
}

// --- The planar check on a real job, timed -------------------------------

namespace cam {
// Defined in CamVerify.cpp; no header declares it, so the bench can time it
// on its own.
Verdict Compare(const HeightMap& machined, const HeightMap& designed,
                double floorZ, const std::vector<uint8_t>* touched,
                double leftoverTolerance,
                const std::vector<Toolpath::Relief>& reliefs,
                const std::vector<Toolpath::Tab>& tabs,
                const OtherSetups* others,
                const std::vector<float>* grooveFloor,
                const std::vector<float>* threadFloor);
} // namespace cam

namespace {
std::string BenchEnv(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return v != nullptr && *v != '\0' ? std::string(v) : fallback;
}
std::string BenchRead(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
template <class F> double BestMs(int repeat, F&& f) {
  double best = 1e300;
  for (int k = 0; k < repeat; k++) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    best = std::min(best,
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  return best;
}
} // namespace

// Hidden: it needs a project on disk. The chess board's check, and its parts:
//   PLANAR_BENCH_DIR    directory holding chess.dsgn and "<job> - <setup>.nc"
//   PLANAR_BENCH_JOB    job directory (default Board)
//   PLANAR_BENCH_PART   part directory (default board)
//   PLANAR_BENCH_SETUP  the setup checked (default Top)
//   PLANAR_BENCH_RES    grid resolution, mm (default 0.25, the RPC's)
//   PLANAR_BENCH_REPEAT runs per timing, the best kept (default 3)
TEST_CASE("cam verify: the board's planar check, timed", "[cam][.bench]") {
  const std::string dir = BenchEnv("PLANAR_BENCH_DIR", "");
  if (dir.empty())
    SKIP("PLANAR_BENCH_DIR is not set");
  const std::string jobName = BenchEnv("PLANAR_BENCH_JOB", "Board");
  const std::string partName = BenchEnv("PLANAR_BENCH_PART", "board");
  const std::string setupName = BenchEnv("PLANAR_BENCH_SETUP", "Top");
  const double res = std::stod(BenchEnv("PLANAR_BENCH_RES", "0.25"));
  const int repeat = std::stoi(BenchEnv("PLANAR_BENCH_REPEAT", "3"));

  auto job = DocumentIO::LoadJob(dir + "/chess.dsgn/jobs/" + jobName);
  REQUIRE(job.has_value());
  // The project's variables first: the board is sized from the king.
  std::vector<Parameter> projectParams;
  {
    const auto index = nlohmann::json::parse(
        BenchRead(dir + "/chess.dsgn/project.json"), nullptr, false);
    if (index.is_object())
      for (const auto& pj : index.value("parameters", nlohmann::json::array()))
        projectParams.push_back({pj.at("name").get<std::string>(),
                                 pj.at("expression").get<std::string>(),
                                 pj.value("comment", std::string{}),
                                 0.0,
                                 {}});
  }
  SymbolTable projectSymbols;
  EvaluateParameterTable(projectParams, projectSymbols, nullptr, {});
  auto part = DocumentIO::Load(dir + "/chess.dsgn/parts/" + partName,
                               {&projectSymbols, &projectParams});
  REQUIRE(part.has_value());
  std::vector<Body> bodies;
  for (const Body& b : (*part)->GetBodies()) {
    if (b.reference)
      continue;
    bodies.push_back(b);
    bodies.back().jobPartId = (*job)->GetParts().front().id;
  }
  REQUIRE(!bodies.empty());
  const cam::Setup* setup = nullptr;
  std::vector<cam::SetupProgram> programs;
  std::string gcode;
  {
    bool before = true;
    for (const cam::Setup& s : (*job)->GetSetups()) {
      const std::string text =
          BenchRead(dir + "/" + jobName + " - " + s.name + ".nc");
      if (s.name == setupName) {
        setup = &s;
        gcode = text;
        before = false;
        continue;
      }
      if (!text.empty())
        programs.push_back({&s, text, before});
    }
  }
  REQUIRE(setup != nullptr);
  REQUIRE(!gcode.empty());

  cam::OtherSetups others;
  cam::ForgetOtherSetups();
  const double othersCold = BestMs(
      1, [&] { others = cam::OtherSetupsOf(*setup, programs, bodies, res); });
  const double othersWarm = BestMs(repeat, [&] {
    others = cam::OtherSetupsOf(*setup, programs, bodies, res);
  });
  cam::Inspection insp;
  const double inspect = BestMs(repeat, [&] {
    insp = cam::Inspect(gcode, *setup, bodies, res, {}, {}, {}, &others, {});
  });
  REQUIRE(insp.ok);
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(*setup, bodies, lo, hi));
  const std::vector<Body> framed = cam::BodiesInSetupFrame(*setup, bodies);
  cam::HeightMap designed;
  const double sample = BestMs(repeat, [&] {
    designed = cam::SampleModel(framed, insp.machined, lo.z);
  });
  // Of which, meshing the part detached at the check's tolerance.
  const double mesh = BestMs(repeat, [&] {
    for (const Body& b : framed)
      (void)b.shape.GenerateMeshDetached(res * 0.25);
  });
  std::printf("  (SampleModel's mesh alone: %.1f ms)\n", mesh);
  cam::Verdict v;
  const double compare = BestMs(repeat, [&] {
    v = cam::Compare(insp.machined, designed, lo.z, &insp.touched, 0.0, {}, {},
                     &others, nullptr, nullptr);
  });
  std::printf("\n== %s / %s at %.2f mm, %zu other setups, map %d x %d, %zu "
              "workers\n"
              "  other setups: %.1f ms cold, %.1f ms memoized\n"
              "  Inspect: %.1f ms (SampleModel %.1f, Compare %.1f, the rest "
              "-- the carve, the start -- %.1f)\n"
              "  whole check (other setups cold + Inspect): %.1f ms; "
              "memoized: %.1f ms\n",
              jobName.c_str(), setupName.c_str(), res, programs.size(),
              insp.machined.nx, insp.machined.ny, parallel::Workers(),
              othersCold, othersWarm, inspect, sample, compare,
              inspect - sample - compare, othersCold + inspect,
              othersWarm + inspect);
  std::printf("  verdict: gouge %.6f, leftover %.6f in %d regions, rapids %d, "
              "bites %d\n",
              insp.verdict.maxGouge, insp.verdict.maxLeftover,
              insp.verdict.leftoverRegions, insp.verdict.rapidsThroughStock,
              insp.verdict.overBites);
}

#endif // HAVE_CAM
