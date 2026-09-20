#ifdef HAVE_CAM

// The verifier: read the posted G-code back, cut it into the stock, and
// report gouge, leftover and the verdict in words.
// One of five files split from test_cam.cpp
// (docs/architecture-review-2026-09-06.md, stage 1).

#include "cam_fixtures.h"

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
  // (docs/cam/dogfood-round.md).
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

#endif // HAVE_CAM
