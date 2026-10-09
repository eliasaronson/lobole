// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Workholding (docs/cam/setup.md#workholding-fixtures): fixtures the verifier
// can see. Every case posts a real program -- written by hand where the point
// is a move no strategy would emit -- and reads the verdict, so what is
// asserted is what an operator would be told.

#ifdef HAVE_CAM

#include "cam_fixtures.h"

namespace {

// A 40 x 30 x 20 block and the setup the verifier cases share. With the
// default relative stock the box runs X -2..42, Y -2..32, Z -1..21.
struct Bench {
  std::unique_ptr<Document> part = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  glm::dvec3 lo{0.0}, hi{0.0};
  Bench() {
    setup.origin = glm::dvec3(0.0); // the file is written in setup space
    REQUIRE(cam::StockBounds(setup, part->GetBodies(), lo, hi));
  }
};

// A fixture made of one box in its own coordinates (corner at the origin),
// placed where `side` puts it against the bench's stock.
cam::FixtureSolid PlacedBox(const Bench& b, const std::string& name,
                            glm::dvec3 size, cam::FixtureSide side,
                            cam::Fixture* placed = nullptr) {
  cam::Fixture f;
  f.name = name;
  f.box = size;
  Body model;
  model.shape =
      CADKernel::Instance().CreateBox(glm::dvec3(0.0), glm::dvec3(size));
  REQUIRE(cam::PlaceFixture(b.setup, b.part->GetBodies(), {model}, 0.0, side,
                            f.pos));
  if (placed != nullptr)
    *placed = f;
  Body bed = model;
  bed.shape = model.shape.Transformed(glm::dmat4(cam::FixtureTransform(f)));
  return {1, name, {bed}};
}

cam::Verdict Verify(const Bench& b, const std::string& g,
                    const std::vector<cam::FixtureSolid>& fixtures,
                    const cam::Setup* setup = nullptr) {
  const cam::Inspection insp =
      cam::Inspect(g, setup != nullptr ? *setup : b.setup, b.part->GetBodies(),
                   0.5, {}, {}, fixtures);
  REQUIRE(insp.ok);
  return insp.verdict;
}

bool Says(const cam::Verdict& v, const std::string& words) {
  for (const cam::VerdictLine& l : cam::Explain(v, 0.75, 0.0))
    if (l.text.find(words) != std::string::npos)
      return true;
  return false;
}

} // namespace

TEST_CASE("cam workholding: a cutter driven into a clamp is reported",
          "[cam][workholding]") {
  Bench b;
  // A clamp 10 wide, the stock's depth, 25 tall, on the bed against -X: it
  // spans X -12..-2 and stands to Z 24, three above the stock.
  const cam::FixtureSolid clamp =
      PlacedBox(b, "Toe clamp", {10, 30, 25}, cam::FixtureSide::MinusX);
  // A profile pass down Y at X -4: the 6 mm cutter spans -7..-1, two
  // millimetres into the clamp, at Z 10.
  const std::string g = "G0 X-4 Y-10 Z40\nG1 Z10 F300\nG1 Y40\nG0 Z40\n";
  const cam::Verdict v = Verify(b, g, {clamp});
  INFO("collisions " << v.fixtureCollisions << " depth " << v.worstFixtureDepth
                     << " line " << v.worstFixtureLine);
  CHECK(v.fixtureCollisions == 1);
  CHECK(v.worstFixtureName == "Toe clamp");
  CHECK_FALSE(v.worstFixtureByHolder);
  // The clamp's top at 24, the flat cutter's underside at 10.
  CHECK_THAT(v.worstFixtureDepth, WithinAbs(14.0, 0.05));
  CHECK_THAT(v.worstFixtureZ, WithinAbs(24.0, 0.05));
  CHECK(v.worstFixtureLine == 3);
  CHECK(v.fixturesChecked == 1);
  CHECK_FALSE(v.Safe(0.75));
  CHECK(Says(v, "fixture \"Toe clamp\""));

  // The control: the same pass inside the stock, clear of the clamp.
  const cam::Verdict clear =
      Verify(b, "G0 X10 Y-10 Z40\nG1 Z10 F300\nG1 Y40\nG0 Z40\n", {clamp});
  CHECK(clear.fixtureCollisions == 0);
  CHECK(clear.fixturesChecked == 1);
}

TEST_CASE("cam workholding: a fixture never changes what is measured",
          "[cam][workholding]") {
  // A fixture is only a keep-out: it cannot grow the stock box or become a
  // surface a gouge is measured against. The same clean program verifies to
  // the same numbers with a clamp beside it.
  Bench b;
  const cam::FixtureSolid clamp =
      PlacedBox(b, "Clamp", {10, 30, 25}, cam::FixtureSide::MinusX);
  const std::string g = "G0 X10 Y-10 Z40\nG1 Z10 F300\nG1 Y40\nG0 Z40\n";
  const cam::Verdict without = Verify(b, g, {});
  const cam::Verdict with = Verify(b, g, {clamp});
  CHECK(with.maxGouge == without.maxGouge);
  CHECK(with.maxLeftover == without.maxLeftover);
  CHECK(with.leftoverArea == without.leftoverArea);
  CHECK(with.cells == without.cells);
  CHECK(without.fixturesChecked == 0);
}

TEST_CASE("cam workholding: a rapid below a tall clamp, and safe Z over it",
          "[cam][workholding]") {
  Bench b;
  // A clamp standing 40 from the bed (Z -1..39), far above the stock's 21.
  const cam::FixtureSolid clamp =
      PlacedBox(b, "Tall clamp", {10, 30, 40}, cam::FixtureSide::MinusX);
  const auto across = [](double z) {
    const std::string zs = std::to_string(z);
    return "G0 X-20 Y15 Z" + zs + "\nG0 X60 Y15 Z" + zs + "\n";
  };
  // Safe Z from the stock alone clears the block and not the clamp.
  const double stockSafe = cam::DeriveSafeZ(b.setup, b.part->GetBodies());
  CHECK(stockSafe < 39.0);
  const cam::Verdict low = Verify(b, across(stockSafe), {clamp});
  CHECK(low.fixtureCollisions == 1);
  // Derived over the fixture too, the same rapid passes.
  const double fixtureSafe =
      cam::DeriveSafeZ(b.setup, b.part->GetBodies(), clamp.bodies);
  CHECK(fixtureSafe > 39.0);
  CHECK(Verify(b, across(fixtureSafe), {clamp}).fixtureCollisions == 0);
}

TEST_CASE("cam workholding: the holder clips a clamp the cutter clears",
          "[cam][workholding]") {
  Bench b;
  const cam::FixtureSolid clamp =
      PlacedBox(b, "Clamp", {10, 30, 25}, cam::FixtureSide::MinusX);
  cam::Setup setup = b.setup;
  setup.tools[0].overallLength = 10.0;  // the nut's underside, above the tip
  setup.tools[0].collarDiameter = 30.0; // 15 mm out from the axis
  // At X 6 the cutter's edge (X 3) is 5 mm clear of the clamp's face at X -2;
  // the collar reaches X -9. Tip at 5, collar at 15, clamp top at 24.
  const std::string g = "G0 X6 Y-10 Z40\nG1 Z5 F300\nG1 Y40\nG0 Z40\n";
  const cam::Verdict v = Verify(b, g, {clamp}, &setup);
  CHECK(v.fixtureCollisions >= 1);
  CHECK(v.worstFixtureByHolder);
  CHECK_THAT(v.worstFixtureDepth, WithinAbs(9.0, 0.05));
  CHECK(Says(v, "HOLDER into fixture"));
}

TEST_CASE("cam workholding: fixtures on a rotary setup are said, not checked",
          "[cam][workholding]") {
  Bench b;
  const cam::FixtureSolid clamp =
      PlacedBox(b, "Clamp", {10, 30, 25}, cam::FixtureSide::MinusX);
  cam::Setup indexed = b.setup;
  RotaryMachine(indexed);
  indexed.rotationA = 90.0;
  const cam::Verdict v = Verify(
      b, "G0 X-4 Y-10 Z40\nG1 Z10 F300\nG1 Y40\nG0 Z40\n", {clamp}, &indexed);
  CHECK(v.fixtureCollisions == 0);
  CHECK(v.fixturesChecked == 0);
  CHECK(v.fixturesSkippedRotary);
  CHECK(Says(v, "Fixtures were not checked"));
}

TEST_CASE("cam workholding: where a fixture goes is derived from the stock",
          "[cam][workholding]") {
  Bench b;
  Body model;
  model.shape = CADKernel::Instance().CreateBox(glm::dvec3(0.0),
                                                glm::dvec3(10.0, 8.0, 25.0));
  const auto at = [&](cam::FixtureSide side) {
    glm::dvec3 pos;
    REQUIRE(cam::PlaceFixture(b.setup, b.part->GetBodies(), {model}, 0.0, side,
                              pos));
    return pos;
  };
  // On the bed, against each face, centred along it.
  const glm::dvec3 mx = at(cam::FixtureSide::MinusX);
  CHECK_THAT(mx.x + 10.0, WithinAbs(b.lo.x, 1e-6)); // its +X face on the stock
  CHECK_THAT(mx.y + 4.0, WithinAbs((b.lo.y + b.hi.y) * 0.5, 1e-6));
  CHECK_THAT(mx.z, WithinAbs(b.lo.z, 1e-6));
  CHECK_THAT(at(cam::FixtureSide::PlusX).x, WithinAbs(b.hi.x, 1e-6));
  CHECK_THAT(at(cam::FixtureSide::MinusY).y + 8.0, WithinAbs(b.lo.y, 1e-6));
  CHECK_THAT(at(cam::FixtureSide::PlusY).y, WithinAbs(b.hi.y, 1e-6));
  // On top, straddling the -X edge.
  const glm::dvec3 top = at(cam::FixtureSide::Top);
  CHECK_THAT(top.z, WithinAbs(b.hi.z, 1e-6));
  CHECK_THAT(top.x + 5.0, WithinAbs(b.lo.x, 1e-6));
  // Centred on the stock both ways.
  const glm::dvec3 mid = at(cam::FixtureSide::Centre);
  CHECK_THAT(mid.x + 5.0, WithinAbs((b.lo.x + b.hi.x) * 0.5, 1e-6));
  CHECK_THAT(mid.z, WithinAbs(b.lo.z, 1e-6));

  // It follows the stock, not a literal: a longer block moves +X out.
  Bench longer;
  longer.part = BlockDoc(80, 30, 20);
  REQUIRE(cam::StockBounds(longer.setup, longer.part->GetBodies(), longer.lo,
                           longer.hi));
  glm::dvec3 far;
  REQUIRE(cam::PlaceFixture(longer.setup, longer.part->GetBodies(), {model},
                            0.0, cam::FixtureSide::PlusX, far));
  CHECK_THAT(far.x, WithinAbs(longer.hi.x, 1e-6));

  // A clamp smaller than the stock takes a side; one wider goes round it.
  CHECK(cam::DefaultFixtureSide(b.setup, b.part->GetBodies(), {model}, 0.0) ==
        cam::FixtureSide::MinusX);
  Body vise;
  vise.shape = CADKernel::Instance().CreateBox(glm::dvec3(0.0),
                                               glm::dvec3(80.0, 20.0, 30.0));
  CHECK(cam::DefaultFixtureSide(b.setup, b.part->GetBodies(), {vise}, 0.0) ==
        cam::FixtureSide::Centre);

  // A keep-out box's default size is the stock's, not a number.
  const glm::dvec3 box = cam::DefaultKeepOutBox(b.setup, b.part->GetBodies());
  CHECK_THAT(box.x, WithinAbs((b.hi.x - b.lo.x) * 0.25, 1e-9));
  CHECK_THAT(box.z, WithinAbs((b.hi.z - b.lo.z) * 1.25, 1e-9));

  // A row names the derived place a fixture is in, and nothing once moved.
  cam::Fixture f;
  f.box = {10, 8, 25};
  f.pos = mx;
  CHECK(cam::FixturePlaceName(b.setup, b.part->GetBodies(), {model}, f) ==
        "-X side");
  f.pos.x -= 3.0;
  CHECK(
      cam::FixturePlaceName(b.setup, b.part->GetBodies(), {model}, f).empty());

  // The sides by name, and a name that is not one refused.
  cam::FixtureSide side{};
  CHECK(cam::ParseFixtureSide("centre", side));
  CHECK(side == cam::FixtureSide::Centre);
  CHECK_FALSE(cam::ParseFixtureSide("left", side));
}

namespace {

// A step clamp in its own coordinates: a 100 mm bar, a nose PAD under one end
// (its underside, z = 5, is what rests on the work) and a stud at the other
// end reaching down to the bed. Three bodies, as a part with three solids is.
std::unique_ptr<Document> StepClampDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Bar", glm::dvec3(0, 0, 10), 100, 20, 8));
  doc->AddFeature(
      std::make_unique<BoxFeature>("Pad", glm::dvec3(80, 0, 5), 20, 20, 5));
  doc->AddFeature(
      std::make_unique<BoxFeature>("Stud", glm::dvec3(5, 5, -40), 10, 10, 50));
  return doc;
}

// The named face of `body` whose outward normal is `n`.
ElementRef FaceFacing(const Body& body, glm::dvec3 n) {
  for (const auto& [index, name] : body.elementMap.names) {
    glm::dvec3 c(0.0), fn(0.0), u(0.0);
    if (body.shape.FacePlane(index, c, fn, u) && glm::dot(fn, n) > 0.999)
      return ElementRef{body.createdBy, name};
  }
  return {};
}

struct Placed {
  glm::dvec3 padLo{0.0}, padHi{0.0}, allLo{1e30}, allHi{-1e30};
};

// The clamp's bodies at a solved pose, as boxes.
Placed PlaceClamp(const std::vector<Body>& model, const glm::dvec3& pos,
                  double rotZdeg) {
  cam::Fixture f;
  f.pos = pos;
  f.rotZdeg = rotZdeg;
  const glm::dmat4 m(cam::FixtureTransform(f));
  Placed out;
  for (size_t i = 0; i < model.size(); i++) {
    glm::dvec3 lo(0.0), hi(0.0);
    model[i].shape.Transformed(m).GetBoundingBox(lo, hi, /*tight=*/true);
    if (i == 1) {
      out.padLo = glm::dvec3(lo);
      out.padHi = glm::dvec3(hi);
    }
    out.allLo = glm::min(out.allLo, glm::dvec3(lo));
    out.allHi = glm::max(out.allHi, glm::dvec3(hi));
  }
  return out;
}

} // namespace

TEST_CASE("cam workholding: a clamp rests on the stock by its face",
          "[cam][workholding]") {
  // docs/cam/setup.md#seats. PlaceFixture's "top" rests the clamp's
  // BOUNDING BOX on the stock -- the bottom of its stud -- with the nose in
  // the air. A seat rests the named FACE there, reaching in over one edge.
  Bench b;
  auto clamp = StepClampDoc();
  const std::vector<Body>& model = clamp->GetBodies();
  REQUIRE(model.size() == 3);
  cam::FixtureSeat seat;
  seat.face = FaceFacing(model[1], {0, 0, -1}); // the pad's underside
  REQUIRE(seat.face.IsSet());

  SECTION("on the -X edge: face on the top, reaching in, stud off the edge") {
    seat.edge = cam::StockEdge::MinusX;
    seat.alongMm = 15.0;
    seat.overMm = 12.0;
    glm::dvec3 pos(0.0);
    double rot = 0.0;
    std::string why;
    REQUIRE(cam::SolveFixtureSeat(b.lo, b.hi, model, seat, pos, rot, &why));
    const Placed p = PlaceClamp(model, pos, rot);
    CHECK_THAT(p.padLo.z, WithinAbs(b.hi.z, 1e-6)); // resting ON the top
    CHECK_THAT(p.padHi.x, WithinAbs(b.lo.x + 12.0, 1e-6));
    CHECK_THAT((p.padLo.y + p.padHi.y) * 0.5, WithinAbs(b.lo.y + 15.0, 1e-6));
    CHECK(p.allLo.x < b.lo.x - 50.0); // the stud stands off the work
  }

  SECTION("every edge turns the nose to point in") {
    for (const cam::StockEdge edge :
         {cam::StockEdge::MinusX, cam::StockEdge::PlusX, cam::StockEdge::MinusY,
          cam::StockEdge::PlusY}) {
      INFO("edge " << cam::StockEdgeName(edge));
      seat.edge = edge;
      seat.alongMm = 10.0;
      seat.overMm = 8.0;
      glm::dvec3 pos(0.0);
      double rot = 0.0;
      REQUIRE(
          cam::SolveFixtureSeat(b.lo, b.hi, model, seat, pos, rot, nullptr));
      const Placed p = PlaceClamp(model, pos, rot);
      const glm::dvec2 pad((p.padLo.x + p.padHi.x) * 0.5,
                           (p.padLo.y + p.padHi.y) * 0.5);
      const glm::dvec2 mid((p.allLo.x + p.allHi.x) * 0.5,
                           (p.allLo.y + p.allHi.y) * 0.5);
      const glm::dvec2 stock((b.lo.x + b.hi.x) * 0.5, (b.lo.y + b.hi.y) * 0.5);
      // The pad is the end nearer the middle of the stock.
      CHECK(glm::length(pad - stock) < glm::length(mid - stock));
      CHECK_THAT(p.padLo.z, WithinAbs(b.hi.z, 1e-6));
    }
  }

  SECTION("it follows the stock; a bed position does not") {
    seat.edge = cam::StockEdge::MinusX;
    seat.alongMm = 15.0;
    seat.overMm = 12.0;
    glm::dvec3 pos(0.0);
    double rot = 0.0;
    REQUIRE(cam::SolveFixtureSeat(b.lo, b.hi, model, seat, pos, rot, nullptr));
    cam::Setup wider = b.setup;
    wider.stock.margin = {7.0, 7.0, 4.0};
    glm::dvec3 lo(0.0), hi(0.0);
    REQUIRE(cam::StockBounds(wider, b.part->GetBodies(), lo, hi));
    glm::dvec3 moved(0.0);
    REQUIRE(cam::SolveFixtureSeat(lo, hi, model, seat, moved, rot, nullptr));
    const Placed p = PlaceClamp(model, moved, rot);
    CHECK_THAT(p.padHi.x, WithinAbs(lo.x + 12.0, 1e-6));
    CHECK_THAT(p.padLo.z, WithinAbs(hi.z, 1e-6));
    CHECK(glm::length(moved - pos) > 1.0);
  }

  SECTION("a face that cannot rest on anything is refused, by why") {
    seat.edge = cam::StockEdge::MinusX;
    glm::dvec3 pos(0.0);
    double rot = 0.0;
    std::string why;
    seat.face = FaceFacing(model[0], {0, 0, 1}); // the bar's top
    REQUIRE(seat.face.IsSet());
    CHECK_FALSE(cam::SolveFixtureSeat(b.lo, b.hi, model, seat, pos, rot, &why));
    CHECK(why.find("does not face down") != std::string::npos);
    CHECK_FALSE(cam::SeatFaceRefusal(model, seat.face).empty());
    seat.face.name = "no such face";
    CHECK_FALSE(cam::SolveFixtureSeat(b.lo, b.hi, model, seat, pos, rot, &why));
    CHECK(why.find("no longer on the fixture") != std::string::npos);
  }
}

TEST_CASE("cam workholding: the seat is read back from where it was put",
          "[cam][workholding]") {
  // docs/cam/gui.md#gizmos: a clamp dragged onto the stock rests
  // there, so a seat is READ from a pose -- and must solve straight back to
  // it, or a clamp put down would jump when the next sync re-solves it.
  Bench b;
  auto clamp = StepClampDoc();
  const std::vector<Body>& model = clamp->GetBodies();
  const ElementRef pad = FaceFacing(model[1], {0, 0, -1});
  REQUIRE(pad.IsSet());
  for (const cam::StockEdge edge :
       {cam::StockEdge::MinusX, cam::StockEdge::PlusX, cam::StockEdge::MinusY,
        cam::StockEdge::PlusY})
    for (const double spin : {0.0, 17.5, -30.0}) {
      INFO("edge " << cam::StockEdgeName(edge) << " spin " << spin);
      cam::FixtureSeat seat;
      seat.face = pad;
      seat.edge = edge;
      seat.alongMm = 11.0;
      seat.overMm = 7.5;
      seat.spinDeg = spin;
      glm::dvec3 pos(0.0);
      double rot = 0.0;
      REQUIRE(
          cam::SolveFixtureSeat(b.lo, b.hi, model, seat, pos, rot, nullptr));
      // The face that lies on the top is the pad's underside...
      const ElementRef on = cam::FaceOnStockTop(b.lo, b.hi, model, pos, rot);
      CHECK(on.name == pad.name);
      // ...and the seat read from the pose is the one it was solved from.
      cam::FixtureSeat back;
      REQUIRE(
          cam::SeatFromPose(b.lo, b.hi, model, pad, pos, rot, back, nullptr));
      CHECK(back.edge == edge);
      CHECK_THAT(back.spinDeg, WithinAbs(spin, 1e-9));
      CHECK_THAT(back.overMm, WithinAbs(7.5, 1e-6));
      CHECK_THAT(back.alongMm, WithinAbs(11.0, 1e-6));
    }

  // Lifted off the top, nothing rests there.
  cam::FixtureSeat seat;
  seat.face = pad;
  glm::dvec3 pos(0.0);
  double rot = 0.0;
  REQUIRE(cam::SolveFixtureSeat(b.lo, b.hi, model, seat, pos, rot, nullptr));
  CHECK_FALSE(
      cam::FaceOnStockTop(b.lo, b.hi, model, pos + glm::dvec3(0, 0, 5), rot)
          .IsSet());
  // Beside the stock at the top's height, nothing either: over the top only.
  CHECK_FALSE(
      cam::FaceOnStockTop(b.lo, b.hi, model, pos - glm::dvec3(60, 0, 0), rot)
          .IsSet());

  // A face square to each axis, by which way it faces: the pad's underside
  // is one of the downward Z planes.
  const auto planes = cam::AxisFacePlanes(model);
  bool padUnder = false;
  for (const cam::AxisFacePlane& p : planes[2])
    padUnder = padUnder ||
               (p.body == 1 && p.facing < 0 && std::abs(p.at - 5.0) < 1e-9);
  CHECK(padUnder);
  CHECK(planes[0].size() == 6); // two X faces on each of three boxes

  // Edges by name, and one that is not refused.
  cam::StockEdge edge{};
  CHECK(cam::ParseStockEdge("+y", edge));
  CHECK(edge == cam::StockEdge::PlusY);
  CHECK_FALSE(cam::ParseStockEdge("left", edge));
}

TEST_CASE("cam workholding: fixtures survive a save, and ids stay fresh",
          "[cam][workholding]") {
  cam::JobDocument job;
  const uint64_t partId = job.AddPart(7, "Bracket");
  cam::Setup& setup = job.AddSetup("Side A");
  cam::Placement pl;
  pl.id = job.AllocateId();
  pl.jobPartId = partId;
  setup.placements.push_back(pl);
  cam::Fixture vise;
  vise.id = job.AllocateId();
  vise.sourceDocId = 9;
  vise.name = "Vise";
  vise.pos = {1.5, -2.0, -1.0};
  vise.rotZdeg = 90.0;
  setup.fixtures.push_back(vise);
  cam::Fixture box;
  box.id = job.AllocateId();
  box.box = {10.0, 8.0, 25.0};
  box.name = "Clamp 2";
  box.enabled = false;
  setup.fixtures.push_back(box);
  // The placement holds the highest id: exactly what the counter's rebuild
  // used to miss.
  cam::Placement last;
  last.id = job.AllocateId();
  last.jobPartId = partId;
  setup.placements.push_back(last);

  const auto path =
      std::filesystem::temp_directory_path() / "designer_workholding_job.json";
  REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  auto back = DocumentIO::LoadJob(path.string());
  REQUIRE(back.has_value());
  const cam::Setup& s = (*back)->GetSetups().front();
  REQUIRE(s.fixtures.size() == 2);
  CHECK(s.fixtures[0].sourceDocId == 9);
  CHECK(s.fixtures[0].name == "Vise");
  CHECK(s.fixtures[0].pos == glm::dvec3(1.5, -2.0, -1.0));
  CHECK(s.fixtures[0].rotZdeg == 90.0);
  CHECK(s.fixtures[0].enabled);
  CHECK_FALSE(s.fixtures[0].IsBox());
  CHECK(s.fixtures[1].IsBox());
  CHECK(s.fixtures[1].box == glm::dvec3(10.0, 8.0, 25.0));
  CHECK_FALSE(s.fixtures[1].enabled);
  CHECK((*back)->AllocateId() > last.id);
  std::filesystem::remove(path);

  // A job with no fixtures writes no `fixtures` key: every older job
  // re-saves byte-identically.
  cam::JobDocument plain;
  plain.AddSetup("Plain");
  CHECK(DocumentIO::JobFingerprint(plain).find("fixtures") ==
        std::string::npos);
}

#endif // HAVE_CAM
