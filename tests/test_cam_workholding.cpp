// Workholding (docs/cam/workholding-plan.md): fixtures the verifier can see.
// Every case posts a real program -- written by hand where the point is a
// move no strategy would emit -- and reads the verdict, so what is asserted is
// what an operator would be told.

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
      CADKernel::Instance().CreateBox(glm::vec3(0.0f), glm::vec3(size));
  REQUIRE(cam::PlaceFixture(b.setup, b.part->GetBodies(), {model}, 0.0, side,
                            f.pos));
  if (placed != nullptr)
    *placed = f;
  Body bed = model;
  bed.shape = model.shape.Transformed(glm::mat4(cam::FixtureTransform(f)));
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
  indexed.machine.hasRotaryAxis = true;
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
  model.shape = CADKernel::Instance().CreateBox(glm::vec3(0.0f),
                                                glm::vec3(10.0f, 8.0f, 25.0f));
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
  vise.shape = CADKernel::Instance().CreateBox(glm::vec3(0.0f),
                                               glm::vec3(80.0f, 20.0f, 30.0f));
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
