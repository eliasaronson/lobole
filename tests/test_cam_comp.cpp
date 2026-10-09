// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef HAVE_CAM

// Cutter radius compensation in the controller, wear mode
// (docs/cam/post.md#cutter-compensation): the shape of the G-code, the
// verdict of a compensated program against the same program uncompensated,
// and the refusals.

#include "cam_fixtures.h"
#include "core/cam/CamComp.h"
#include "core/cam/CamGcode.h"
#include "core/cam/CamOpFields.h"
#include "core/features/HoleFeature.h"

namespace {

// A 60 x 60 x 12 plate with a Ø`dia` bore 9 deep at its centre, the face's
// size held to K7 when `k7`.
struct Seat {
  std::unique_ptr<Document> doc;
  ElementRef bore;
};
Seat SeatPlate(double dia, bool k7) {
  Seat out;
  out.doc = std::make_unique<Document>();
  Document& doc = *out.doc;
  auto pad =
      std::make_unique<SketchFeature>("Plate", SketchPlane(PlaneType::XY));
  pad->AddRectangle({-30, -30}, {30, 30});
  doc.AddFeature(std::move(pad));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Pad", doc.GetFeature(0)->GetId(), 12.0, ExtrudeMode::NewBody));
  auto centre = std::make_unique<SketchFeature>(
      "Centre",
      SketchPlane(glm::vec3(0, 0, 12), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  centre->AddEntity(sketch::Point{{0, 0}, 1});
  doc.AddFeature(std::move(centre));
  const uint64_t at = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(
      std::make_unique<HoleFeature>("Seat", at, HoleKind::Simple, dia, 9.0));
  doc.Regenerate();
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  const Body& body = doc.GetBodies().at(0);
  for (const auto& [index, name] : body.elementMap.names)
    if (name.find("/bore") != std::string::npos)
      out.bore = ElementRef{body.createdBy, name};
  REQUIRE(out.bore.IsSet());
  if (k7) {
    Tolerance tol;
    tol.ref = out.bore;
    tol.isoClass = "K7";
    doc.SetTolerance(tol);
  }
  return out;
}

std::vector<Body> Stamped(const Document& doc) {
  std::vector<Body> bodies = doc.GetBodies();
  for (Body& b : bodies)
    cam::StampBands(doc, b);
  return bodies;
}

cam::Setup Ready(const std::vector<Body>& bodies) {
  cam::Setup s = BasicSetup();
  s.stock.margin.z = 0.0; // a faced plate: the bore is all there is to cut
  s.safeZ = cam::DeriveSafeZ(s, bodies);
  REQUIRE(cam::RecomputeOrigin(s, bodies));
  return s;
}

// The seat bored with the 6 mm cutter, compensated or not.
cam::Operation Bore(const cam::Setup& s, const std::vector<Body>& bodies,
                    const ElementRef& bore, bool comp) {
  cam::DerivedOp d = cam::DeriveOperation(s, bodies, cam::OpType::Drill,
                                          cam::Target{0, bore}, 1);
  REQUIRE(d.ok);
  d.op.id = 1;
  d.op.cutterComp = comp ? cam::CutterComp::Wear : cam::CutterComp::Off;
  return d.op;
}

std::string Post(const cam::Setup& s, const std::vector<cam::Toolpath>& paths,
                 const char* post = "mach3") {
  std::string gcode, why;
  cam::PostOptions opts;
  opts.fitArcs = s.machine.fitArcs;
  INFO(why);
  REQUIRE(cam::PostProcess(post, s, paths, opts, gcode, &why));
  return gcode;
}

std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);)
    out.push_back(l);
  return out;
}

cam::Verdict Verify(const cam::Setup& s, const std::vector<Body>& bodies,
                    const std::vector<cam::Toolpath>& paths) {
  std::vector<cam::Toolpath::Fit> fits;
  for (const cam::Toolpath& tp : paths)
    fits.insert(fits.end(), tp.fits.begin(), tp.fits.end());
  return cam::VerifyGcode(Post(s, paths), s, bodies, 0.1, {}, {}, {}, nullptr,
                          {}, {}, fits);
}

} // namespace

TEST_CASE("cutter comp: a bore's wall goes under G41 between a lead-in and a "
          "lead-out line",
          "[cam]") {
  const Seat seat = SeatPlate(32.0, /*k7=*/true);
  const std::vector<Body> bodies = Stamped(*seat.doc);
  cam::Setup s = Ready(bodies);
  s.operations.push_back(Bore(s, bodies, seat.bore, true));
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
  REQUIRE(paths.at(0).error.empty());
  const cam::Toolpath& tp = paths[0];
  // One wall: the last ring of the bore.
  REQUIRE(tp.comps.size() == 1);
  const cam::Toolpath::Comp& c = tp.comps[0];
  // Anticlockwise inside a bore, the wall on the right: G41, which with a
  // positive wear moves the tool away from the wall.
  CHECK(c.left);
  CHECK_THAT(c.checkedWear, WithinAbs(0.6, 1e-12));
  // The lead-in line is longer than the radius the controller is ever asked
  // to offset, and so is the lead-out line.
  const double in = glm::length(glm::dvec2(tp.moves[c.first].to) -
                                glm::dvec2(tp.moves[c.first - 1].to));
  const double out = glm::length(glm::dvec2(tp.moves[c.last].to) -
                                 glm::dvec2(tp.moves[c.last - 1].to));
  CHECK(in > c.checkedWear);
  CHECK(out > c.checkedWear);
  // Every lead point stands inside the bore's wall pass: in the hole the
  // inner rings cleared, never past the wall.
  for (size_t k = c.first - 1; k <= c.last; k++)
    CHECK(glm::length(glm::dvec2(tp.moves[k].to)) <= 13.0 + 1e-6);

  const std::string gcode = Post(s, paths);
  const std::vector<std::string> lines = Lines(gcode);
  int g41 = 0, g40 = 0;
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i] == "G41 D1") {
      g41++;
      // The next line is the lead-in: a straight move, so the controller
      // turns compensation on along a line, never an arc.
      REQUIRE(i + 1 < lines.size());
      CHECK(lines[i + 1].rfind("G1 ", 0) == 0);
    }
    if (lines[i] == "G40") {
      g40++;
      REQUIRE(i + 1 < lines.size());
      CHECK(lines[i + 1].rfind("G1 ", 0) == 0);
    }
  }
  CHECK(g41 == 1);
  CHECK(g40 == 1);
  CHECK(gcode.find("G42") == std::string::npos);
  // G41 comes before G40, and arcs (the wall, the leads) sit between them.
  const size_t on = gcode.find("\nG41 D1\n"), off = gcode.find("\nG40\n");
  REQUIRE(on != std::string::npos);
  REQUIRE(off != std::string::npos);
  CHECK(on < off);
  CHECK(gcode.substr(on, off - on).find("\nG3 ") != std::string::npos);
  // The header tells the machinist what the table's entry holds.
  CHECK(gcode.find("(comp op 1: G41 D1 WEAR - tool 1's diameter in the tool "
                   "table is the wear alone") != std::string::npos);
  CHECK(gcode.find("checked to +-0.6)") != std::string::npos);
  // The parser reads past the words and names the op.
  const cam::GProgram program = cam::ProgramInSetupSpace(gcode, s);
  CHECK(program.errors.empty());
  CHECK(program.compensatedOps == std::vector<uint64_t>{1});
}

TEST_CASE("cutter comp: the compensated bore verifies as the uncompensated "
          "one does",
          "[cam]") {
  // The verifier and the playback read the NOMINAL tool: the file is the
  // tool centre, so with no wear in the table it cuts what it cut before.
  const Seat seat = SeatPlate(32.0, /*k7=*/true);
  const std::vector<Body> bodies = Stamped(*seat.doc);
  cam::Setup plain = Ready(bodies);
  plain.operations.push_back(Bore(plain, bodies, seat.bore, false));
  cam::Setup comp = plain;
  comp.operations[0].cutterComp = cam::CutterComp::Wear;
  const cam::Verdict a = Verify(plain, bodies, cam::GenerateAll(plain, bodies));
  const cam::Verdict b = Verify(comp, bodies, cam::GenerateAll(comp, bodies));
  CHECK(a.compensatedOps.empty());
  CHECK(b.compensatedOps == std::vector<uint64_t>{1});
  CHECK(b.fileErrors == 0);
  CHECK(a.Safe(0.15) == b.Safe(0.15));
  CHECK(a.Complete(0.15) == b.Complete(0.15));
  CHECK_THAT(b.maxGouge, WithinAbs(a.maxGouge, 1e-6));
  CHECK_THAT(b.maxLeftover, WithinAbs(a.maxLeftover, 1e-6));
  CHECK_THAT(b.leftoverArea, WithinAbs(a.leftoverArea, 0.05));
  CHECK(b.rapidsThroughStock == a.rapidsThroughStock);
  CHECK(b.overBites == a.overBites);
  REQUIRE(a.fits.size() == 1);
  REQUIRE(b.fits.size() == 1);
  REQUIRE(b.fits[0].judged);
  CHECK_THAT(b.fits[0].cut, WithinAbs(a.fits[0].cut, 1e-6));
  CHECK(b.fits[0].InBand());
  // And it says so, in the verdict's own words.
  bool said = false;
  for (const cam::VerdictLine& l : cam::Explain(b, 0.15, 0.0))
    said = said ||
           l.text.find("compensated in the controller") != std::string::npos;
  CHECK(said);
}

TEST_CASE("cutter comp: a profile picks its side from the direction it cuts",
          "[cam]") {
  // Round a block: climb runs clockwise, the part on the right (G41);
  // conventional anticlockwise, the part on the left (G42).
  auto doc = BlockDoc();
  doc->Regenerate();
  const std::vector<Body> bodies = doc->GetBodies();
  for (const bool climb : {true, false}) {
    cam::Setup s = Ready(bodies);
    cam::Operation op;
    op.id = 7;
    op.type = cam::OpType::Profile;
    op.side = cam::Side::Outside;
    op.climb = climb;
    op.topZ = 20.0;
    op.bottomZ = 14.0;
    op.stepdown = 3.0;
    op.cutterComp = cam::CutterComp::Wear;
    s.operations.push_back(op);
    const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
    INFO("climb " << climb << ": " << paths.at(0).error);
    REQUIRE(paths[0].error.empty());
    // A run per pass, each starting where the last one did.
    REQUIRE(paths[0].comps.size() == 2);
    const cam::Toolpath& tp = paths[0];
    CHECK(glm::length(glm::dvec2(tp.moves[tp.comps[0].first - 1].to) -
                      glm::dvec2(tp.moves[tp.comps[1].first - 1].to)) < 1e-6);
    for (const cam::Toolpath::Comp& c : tp.comps)
      CHECK(c.left == climb);
    const std::string gcode = Post(s, paths);
    CHECK(gcode.find(climb ? "G41 D1" : "G42 D1") != std::string::npos);
    CHECK(gcode.find(climb ? "G42" : "G41") == std::string::npos);
    // The leads stand outside the part.
    for (const cam::Toolpath::Comp& c : tp.comps) {
      const glm::dvec3 p = tp.moves[c.first - 1].to;
      CHECK((p.x < -1e-6 || p.x > 40 + 1e-6 || p.y < -1e-6 || p.y > 30 + 1e-6));
    }
    // Verified clean as programmed.
    const cam::Verdict v = Verify(s, bodies, paths);
    INFO("gouge " << v.maxGouge << " at " << v.gougeAt.x << "," << v.gougeAt.y);
    CHECK(v.Safe(0.15));
  }
}

TEST_CASE("cutter comp: a rough-then-finish op compensates its finish only",
          "[cam]") {
  const Seat seat = SeatPlate(32.0, /*k7=*/true);
  const std::vector<Body> bodies = Stamped(*seat.doc);
  cam::Setup s = Ready(bodies);
  cam::Tool fine = s.tools[0];
  fine.number = 6;
  fine.name = "6 mm finisher";
  s.tools[0].number = 1;
  s.tools[0].diameter = 10.0;
  s.tools.push_back(fine);
  cam::Operation op = Bore(s, bodies, seat.bore, true);
  op.finishTool = 6;
  op.finishStock = 0.2;
  s.operations.push_back(op);
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
  REQUIRE(paths.size() == 2);
  REQUIRE(paths[0].error.empty());
  REQUIRE(paths[1].error.empty());
  CHECK_FALSE(paths[0].finish);
  CHECK(paths[0].comps.empty());
  CHECK(paths[1].finish);
  CHECK(paths[1].comps.size() == 1);
  const std::string gcode = Post(s, paths);
  CHECK(gcode.find("G41 D6") != std::string::npos);
  CHECK(gcode.find("G41 D1") == std::string::npos);
}

TEST_CASE("cutter comp: what cannot be compensated is refused, naming why",
          "[cam]") {
  const Seat seat = SeatPlate(32.0, /*k7=*/false);
  const std::vector<Body> bodies = Stamped(*seat.doc);

  SECTION("a post with no cutter compensation") {
    cam::Setup s = Ready(bodies);
    s.operations.push_back(Bore(s, bodies, seat.bore, true));
    const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
    REQUIRE(paths.at(0).error.empty());
    CHECK_FALSE(cam::PostWritesCutterComp("grbl"));
    CHECK(cam::PostWritesCutterComp("mach3"));
    CHECK(cam::PostWritesCutterComp("linuxcnc"));
    std::string gcode, why;
    CHECK_FALSE(cam::PostProcess("grbl", s, paths, {}, gcode, &why));
    CHECK_THAT(why, Catch::Matchers::ContainsSubstring("grbl") &&
                        Catch::Matchers::ContainsSubstring("cutter_comp") &&
                        Catch::Matchers::ContainsSubstring("mach3"));
    // LinuxCNC takes the same words.
    CHECK(Post(s, paths, "linuxcnc").find("G41 D1") != std::string::npos);
  }

  SECTION("an unknown mode, naming the ones there are") {
    const cam::OpFieldRow* row = cam::FindOpFieldRow("cutter_comp");
    REQUIRE(row != nullptr);
    cam::Operation op;
    CHECK_THROWS_WITH(cam::SetOpField(*row, "control", op),
                      Catch::Matchers::ContainsSubstring("off, wear"));
    cam::SetOpField(*row, "wear", op);
    CHECK(op.cutterComp == cam::CutterComp::Wear);
  }

  SECTION("a hole the cutter's own size: nothing to move") {
    const Seat small = SeatPlate(6.0, false);
    const std::vector<Body> b = Stamped(*small.doc);
    cam::Setup s = Ready(b);
    s.operations.push_back(Bore(s, b, small.bore, true));
    const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, b);
    CHECK_THAT(paths.at(0).error,
               Catch::Matchers::ContainsSubstring("no wall"));
  }

  SECTION("a bore with no room for the leads") {
    const Seat small = SeatPlate(7.0, false);
    const std::vector<Body> b = Stamped(*small.doc);
    cam::Setup s = Ready(b);
    s.operations.push_back(Bore(s, b, small.bore, true));
    const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, b);
    CHECK_THAT(paths.at(0).error,
               Catch::Matchers::ContainsSubstring("no room"));
  }

  SECTION("a profile on its contour, and one with lead arcs") {
    auto doc = BlockDoc();
    doc->Regenerate();
    const std::vector<Body> b = doc->GetBodies();
    cam::Setup s = Ready(b);
    cam::Operation op;
    op.id = 1;
    op.type = cam::OpType::Profile;
    op.topZ = 20.0;
    op.bottomZ = 15.0;
    op.cutterComp = cam::CutterComp::Wear;
    op.side = cam::Side::On;
    s.operations = {op};
    CHECK_THAT(cam::GenerateAll(s, b).at(0).error,
               Catch::Matchers::ContainsSubstring("give it a side"));
    s.operations[0].side = cam::Side::Outside;
    s.operations[0].leadArcs = true;
    CHECK_THAT(cam::GenerateAll(s, b).at(0).error,
               Catch::Matchers::ContainsSubstring("lead_arcs"));
  }

  SECTION("a turn too tight for the controller to offset") {
    // A wall pass that rounds an inside corner on a 0.1 mm radius -- the
    // tool centre's path past a corner 0.1 mm larger than the cutter -- in
    // eight chords. Offset by 0.3 mm toward the corner's centre, each chord
    // is used up before it starts.
    cam::Toolpath tp;
    const auto emit = [&](cam::Move::Kind k, glm::dvec3 p) {
      cam::Move m;
      m.kind = k;
      m.to = p;
      m.feed = k == cam::Move::Kind::Rapid ? 0.0 : 600.0;
      tp.moves.push_back(m);
    };
    emit(cam::Move::Kind::Rapid, {-20, 0.0, 25});
    emit(cam::Move::Kind::Rapid, {-20, 0.0, 5});
    const size_t first = tp.moves.size();
    emit(cam::Move::Kind::Cut, {-10, 0.0, 5});
    emit(cam::Move::Kind::Cut, {0, 0.0, 5});
    for (int i = 1; i <= 8; i++) {
      const double a = -glm::half_pi<double>() + glm::half_pi<double>() * i / 8;
      emit(cam::Move::Kind::Cut,
           {0.1 * std::cos(a), 0.1 + 0.1 * std::sin(a), 5});
    }
    emit(cam::Move::Kind::Cut, {0.1, 10.0, 5});
    emit(cam::Move::Kind::Cut, {0.1, 20.0, 5});
    // The material on the left of travel (inside the corner the path turns
    // round), so the leads go right, out of the way.
    tp.walls.push_back({first, tp.moves.size() - 1, false});
    emit(cam::Move::Kind::Rapid, {0.1, 20.0, 25});
    cam::Tool tool = BasicSetup().tools[0];
    std::string why;
    CHECK_FALSE(cam::CompensateWalls(tp, tool, glm::dvec3(0.0), why));
    CHECK_THAT(why, Catch::Matchers::ContainsSubstring("inside corner") &&
                        Catch::Matchers::ContainsSubstring("0.6"));
    // The same corner on a 2 mm radius compensates.
    cam::Toolpath ok = tp;
    for (int i = 1; i <= 8; i++) {
      const double a = -glm::half_pi<double>() + glm::half_pi<double>() * i / 8;
      ok.moves[first + 1 + i].to = {2.0 * std::cos(a), 2.0 + 2.0 * std::sin(a),
                                    5};
    }
    ok.moves[first + 10].to = {2.0, 10.0, 5};
    ok.moves[first + 11].to = {2.0, 20.0, 5};
    ok.moves.back().to = {2.0, 20.0, 25};
    std::string none;
    CHECK(cam::CompensateWalls(ok, tool, glm::dvec3(0.0), none));
    CHECK(none.empty());
    CHECK(ok.comps.size() == 1);
  }
}

#endif // HAVE_CAM
