#ifdef HAVE_CAM

// The setup and the job document: fixture frames, the indexed 4th axis,
// the workpiece and where zero is on it, and N parts placed in one sheet.
// One of five files split from test_cam.cpp
// (docs/architecture.md#source-layout).

#include "cam_fixtures.h"
#include "core/cam/CamOtherSetups.h"
#include "core/features/RevolveFeature.h"
#include "ui/DocumentSlots.h"

TEST_CASE("cam stock: the blank is the part's REAL size, not its control "
          "points",
          "[cam][setup]") {
  // OCC bounds a B-spline by its CONTROL POINTS, which lie outside the
  // surface. Stock used to take that bound, so a 200 mm revolved waveguide
  // asked for a 216 mm blank and the facing and profile passes cut air to
  // reach it (docs/cam/README.md#invariants). Stock is the material a person
  // buys and clamps -- and it is what sets the work origin -- so it takes the
  // TIGHT bound.
  //
  // The fixture is a revolved ARC, because that is the shape whose two bounds
  // differ. A box, a cylinder or a sphere is analytic and bounded exactly
  // either way, which is both why the golden programs did not move when this
  // changed and why the first version of this test passed against the bug.
  // The REQUIRE below is what stops that happening again: it fails if the
  // fixture is no longer testing anything.
  Document doc;
  auto sk = std::make_unique<SketchFeature>("Dish profile",
                                            SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Line{{50.0f, 0.0f}, {100.0f, 0.0f}});
  sk->AddEntity(sketch::Line{{100.0f, 0.0f}, {100.0f, 25.0f}});
  sk->AddEntity(sketch::Line{{100.0f, 25.0f}, {92.0f, 25.0f}});
  // Tangent to the front flat at the mouth and through the throat -- the
  // waveguide's own flare, which is where the loose bound came from.
  sk->AddEntity(sketch::Arc{{92.0f, 25.0f - 47.78f},
                            47.78,
                            std::atan2(0.0 - (25.0 - 47.78), 50.0 - 92.0),
                            std::atan2(47.78, 0.0)});
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto rev = std::make_unique<RevolveFeature>("Dish", sid, 360.0,
                                              ExtrudeMode::NewBody);
  rev->axis.kind = AxisRef::Kind::Y;
  doc.AddFeature(std::move(rev));
  REQUIRE(doc.GetBodies().size() == 1);

  glm::vec3 looseLo(0.0f), looseHi(0.0f), tightLo(0.0f), tightHi(0.0f);
  const CADShape& shape = doc.GetBodies()[0].shape;
  shape.GetBoundingBox(looseLo, looseHi);
  shape.GetBoundingBox(tightLo, tightHi, /*tight=*/true);
  INFO("loose " << looseHi.x << " vs tight " << tightHi.x);
  // The fixture must actually have a loose bound, or this test proves
  // nothing -- which is exactly how its first version passed against the bug.
  REQUIRE(looseHi.x > tightHi.x + 0.5);

  cam::Setup setup = BasicSetup();
  setup.stock.margin = {0.0, 0.0, 0.0};
  setup.stock.bottomMargin = 0.0;
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(setup, doc.GetBodies(), lo, hi));
  INFO("stock " << lo.x << ".." << hi.x);
  CHECK_THAT(hi.x, WithinAbs(tightHi.x, 1e-6));
  CHECK_THAT(lo.x, WithinAbs(tightLo.x, 1e-6));
}

TEST_CASE("cam: a setup survives a save and reload", "[cam]") {
  // Setups are document objects (v58), and since v73 they are objects of a
  // JOB document rather than of the part (docs/cam/README.md#invariants). They
  // describe how to cut the model and change nothing about it, so they must
  // NOT touch the regen chain hash — a new operation rebuilding the geometry
  // would be a bug with a long tail. Structural now that the setups live
  // elsewhere entirely, and still the claim worth asserting.
  auto doc = BlockDoc();
  const size_t hashBefore = doc->TipChainHash();

  cam::JobDocument job;
  cam::Setup& setup = job.AddSetup("Side A");
  setup.machine.post = "linuxcnc";
  setup.safeZ = 12.5;
  setup.tools[0].diameter = 3.175;
  setup.tools[0].feedCut = 450.0;
  cam::Operation op;
  op.id = job.AllocateId();
  op.name = "Outline";
  op.type = cam::OpType::Profile;
  op.topZ = 20.0;
  op.bottomZ = 0.0;
  op.stepdown = 4.0;
  op.tabCount = 3;
  op.targets.push_back(cam::Target{0, ElementRef{1, "F:box.side"}});
  setup.operations.push_back(op);

  CHECK(doc->TipChainHash() == hashBefore); // geometry untouched

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_roundtrip.json";
  REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  auto back = DocumentIO::LoadJob(path.string());
  REQUIRE(back.has_value());
  const std::vector<cam::Setup>& setups = (*back)->GetSetups();
  REQUIRE(setups.size() == 1);
  CHECK(setups[0].name == "Side A");
  CHECK(setups[0].machine.post == "linuxcnc");
  CHECK_THAT(setups[0].safeZ, WithinAbs(12.5, 1e-9));
  REQUIRE(setups[0].tools.size() == 1);
  CHECK_THAT(setups[0].tools[0].diameter, WithinAbs(3.175, 1e-9));
  REQUIRE(setups[0].operations.size() == 1);
  const cam::Operation& backOp = setups[0].operations[0];
  CHECK(backOp.type == cam::OpType::Profile);
  CHECK(backOp.tabCount == 3);
  CHECK_THAT(backOp.stepdown, WithinAbs(4.0, 1e-9));
  REQUIRE(backOp.targets.size() == 1);
  CHECK(backOp.targets[0].ref.name == "F:box.side");
  std::filesystem::remove(path);
}

// --- C9: the indexed 4th axis ---------------------------------------------

TEST_CASE("cam: an indexed job still fits arcs", "[cam]") {
  // A round part, posted to a controller that takes G2/G3, has to come back
  // with arcs whether or not the trunnion is turned. It did not: every move
  // of an indexed setup carries an A (CamStrategies.cpp stamps it), and
  // `FitArc`'s usability test required `!m.hasA` -- so arc fitting was off
  // for EVERY indexed job, silently, while the panel still showed "Arcs
  // (G2/G3)" ticked. Nothing caught it because the C9 test below cuts a
  // rectangular block, which has no arc to fit, and no golden .nc carries an
  // A word.
  //
  // The A is not what makes an arc unsafe. A CHANGE in A across the run is,
  // and `LastCollinear` five lines away already compared them that way.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Puck", glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 20.0, 20.0));

  const auto arcs = [](const std::string& g) {
    int n = 0;
    for (size_t at = 0; at < g.size();) {
      const size_t end = std::min(g.find('\n', at), g.size());
      const std::string line = g.substr(at, end - at);
      at = end + 1;
      if (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0)
        n++;
    }
    return n;
  };
  const auto post = [&](double a) {
    cam::Setup s = BasicSetup();
    s.machine.fitArcs = true;
    if (a != 0.0) {
      // Half a turn about X: the puck is still a puck, so the toolpath is the
      // same circle and only the A word differs. That is what makes the two
      // posts comparable.
      RotaryMachine(s);
      s.rotationA = a;
    }
    glm::dvec3 lo(0.0), hi(0.0);
    REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));
    s.safeZ = cam::DeriveSafeZ(s, doc->GetBodies());
    cam::Operation op;
    op.id = 1;
    op.type = cam::OpType::Profile;
    op.toolNumber = 1;
    op.topZ = hi.z;
    op.bottomZ = lo.z;
    op.stepdown = 5.0;
    op.side = cam::Side::Outside;
    s.operations.push_back(op);
    const std::vector<cam::Toolpath> paths =
        cam::GenerateAll(s, doc->GetBodies());
    REQUIRE(paths.size() == 1);
    INFO(paths[0].error);
    REQUIRE(paths[0].error.empty());
    std::string gcode, error;
    REQUIRE(cam::PostProcess("linuxcnc", s, paths, {}, gcode, &error));
    return gcode;
  };

  const std::string flat = post(0.0);
  const int flatArcs = arcs(flat);
  INFO("3-axis arcs: " << flatArcs);
  REQUIRE(flatArcs > 0); // or this case is measuring nothing

  const std::string turned = post(180.0);
  REQUIRE(turned.find("A180") != std::string::npos); // it IS indexed
  INFO("indexed arcs: " << arcs(turned));
  CHECK(arcs(turned) == flatArcs);
}

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
  RotaryMachine(indexed);
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
  RotaryMachine(indexed);
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
  RotaryMachine(indexed);
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
  RotaryMachine(rotary);
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
  // that started this (docs/cam/setup.md#fixturing-frame-and-rotary-index).
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

TEST_CASE("cam: a flip job's sides say what the other cuts, not leftover",
          "[cam]") {
  // docs/cam/simulation.md#leftover-another-setup-removes. Each side cuts the
  // outline half way down; alone, each reports the other half as 10 mm of
  // leftover, which reads exactly like a forgotten operation.
  auto doc = BlockDoc(40, 30, 20);
  const auto side = [&](const char* name, uint64_t id, bool flipped) {
    cam::Setup s = BasicSetup();
    s.id = id;
    s.name = name;
    s.stock.margin = {3.0, 3.0, 0.0};
    s.stock.bottomMargin = 0.0; // the two boxes agree only when these do
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
    REQUIRE(paths[0].error.empty());
    std::string gcode, error;
    REQUIRE(cam::PostProcess("mach3", s, paths, {}, gcode, &error));
    return gcode;
  };
  const std::string frontFile = post(front), backFile = post(back);

  // Alone: the lower half of the outline is left.
  const cam::Verdict alone =
      cam::VerifyGcode(frontFile, front, doc->GetBodies(), 0.5);
  INFO("alone: leftover " << alone.maxLeftover);
  CHECK(alone.maxLeftover > 9.0);
  CHECK(alone.leftoverOtherSetups.empty());

  // With the back consulted, that half is the back's, and the front is
  // complete.
  const cam::OtherSetups others =
      cam::OtherSetupsOf(front, {{&back, backFile}}, doc->GetBodies(), 0.5);
  REQUIRE(others.notChecked.empty());
  const cam::Verdict v = cam::VerifyGcode(frontFile, front, doc->GetBodies(),
                                          0.5, {}, {}, {}, &others);
  INFO("with the back: leftover " << v.maxLeftover);
  CHECK(v.Complete(0.75));
  REQUIRE(v.leftoverOtherSetups.size() == 1);
  CHECK(v.leftoverOtherSetups[0].setup == 2);
  CHECK(v.leftoverOtherSetups[0].name == "Back");
  CHECK(v.leftoverOtherSetups[0].area > 100.0);
  bool said = false;
  for (const cam::VerdictLine& l : cam::Explain(v, 0.75, 0.0, false))
    said = said || l.text.find("cut by setup 'Back'") != std::string::npos;
  CHECK(said);

  // A back that cuts only its top quarter leaves a strip neither side cuts:
  // still leftover, not excused.
  cam::Setup shallow = back;
  shallow.operations[0].bottomZ = -5.0; // its frame runs z -20 to 0
  const cam::OtherSetups partial = cam::OtherSetupsOf(
      front, {{&shallow, post(shallow)}}, doc->GetBodies(), 0.5);
  const cam::Verdict gap = cam::VerifyGcode(frontFile, front, doc->GetBodies(),
                                            0.5, {}, {}, {}, &partial);
  INFO("with a shallow back: leftover " << gap.maxLeftover);
  CHECK(gap.maxLeftover > 4.0);

  // Parts placed differently from one another have no one map to the
  // model: said, not guessed.
  std::vector<Body> two = doc->GetBodies();
  Body other = two.front();
  other.jobPartId = 7;
  two.push_back(other);
  cam::Setup placed = front;
  cam::Placement pl;
  pl.jobPartId = 7;
  pl.pos = {100.0, 0.0};
  placed.placements.push_back(pl);
  const cam::OtherSetups refused =
      cam::OtherSetupsOf(placed, {{&back, backFile}}, two, 0.5);
  REQUIRE(refused.notChecked.size() == 1);
  CHECK(refused.notChecked[0].find("placed differently") != std::string::npos);
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

TEST_CASE("cam job: two parts in one setup, placed apart", "[cam][job]") {
  // The case the whole job document exists for: two brackets cut from one
  // sheet. Before v73 a cam::Setup lived in Document::m_camSetups, so this
  // could not be said at all (docs/cam/README.md#the-model).
  //
  // The bodies arrive already stamped with the job part they came from --
  // that is what ui::JobBodies does -- so this exercises the core rule
  // without a running app: a placement moves the part it names, in setup
  // space, and nothing else.
  Document a;
  a.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 40, 30, 10));
  Document b;
  b.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 40, 30, 10));

  cam::JobDocument job;
  const uint64_t pa = job.AddPart(1, "A");
  const uint64_t pb = job.AddPart(2, "B");
  cam::Setup& setup = job.AddSetup("Sheet");

  std::vector<Body> bodies;
  for (const Body& body : a.GetBodies()) {
    bodies.push_back(body);
    bodies.back().jobPartId = pa;
  }
  for (const Body& body : b.GetBodies()) {
    bodies.push_back(body);
    bodies.back().jobPartId = pb;
  }
  REQUIRE(bodies.size() == 2);

  // Unplaced, the two sit on top of each other: "nobody has said where" is
  // the origin, which is honest and is not a layout.
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(setup, bodies, lo, hi));
  const double stacked = hi.x - lo.x;

  // Place the second 200 mm along, and the stock has to cover both.
  cam::Placement place;
  place.id = job.AllocateId();
  place.jobPartId = pb;
  place.pos = {200.0, 0.0};
  setup.placements.push_back(place);
  REQUIRE(cam::StockBounds(setup, bodies, lo, hi));
  CHECK(hi.x - lo.x > stacked + 190.0);

  // A part on the sheet but not cut this run leaves the stock to the other
  // one (PrusaSlicer's ModelInstance::printable, the same idea).
  setup.placements[0].enabled = false;
  REQUIRE(cam::StockBounds(setup, bodies, lo, hi));
  CHECK_THAT(hi.x - lo.x, WithinAbs(stacked, 1e-6));
}

TEST_CASE("cam job: a part placed twice is refused, not half cut",
          "[cam][job]") {
  // The data model holds instances -- a placement per copy, the way every
  // slicer models them -- and the generator does not cut them yet: a region
  // resolves to the first body that answers, so the second copy would be
  // skipped and the posted file would look right. That is the failure mode
  // this project refuses everywhere else.
  cam::JobDocument job;
  const uint64_t part = job.AddPart(1, "Bracket");
  cam::Setup& setup = job.AddSetup("Sheet");
  for (int i = 0; i < 2; i++) {
    cam::Placement p;
    p.id = job.AllocateId();
    p.jobPartId = part;
    p.pos = {i * 100.0, 0.0};
    setup.placements.push_back(p);
  }
  const std::string why = cam::PlacementRefusal(setup, {});
  CHECK(why.find("twice") != std::string::npos);

  // And it reaches the toolpaths, not just a checker nobody calls.
  cam::Operation op;
  op.id = job.AllocateId();
  op.type = cam::OpType::Profile;
  setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(setup, {});
  REQUIRE(paths.size() == 1);
  CHECK(paths[0].error == why);

  // Disabling one is the way out, and it works.
  setup.placements[1].enabled = false;
  CHECK(cam::PlacementRefusal(setup, {}).empty());
}

TEST_CASE("cam job: dropping a part takes its placements and targets",
          "[cam][job]") {
  // A target on a part the job no longer holds would resolve against
  // whatever body answered to the same element name -- the wrong bracket,
  // cut for real. Two parts in one job can easily share both.
  cam::JobDocument job;
  const uint64_t pa = job.AddPart(1, "A");
  const uint64_t pb = job.AddPart(2, "B");
  cam::Setup& setup = job.AddSetup("Sheet");
  for (uint64_t part : {pa, pb}) {
    cam::Placement p;
    p.id = job.AllocateId();
    p.jobPartId = part;
    setup.placements.push_back(p);
    cam::Operation op;
    op.id = job.AllocateId();
    op.targets.push_back(cam::Target{part, ElementRef{7, "1/face0"}});
    setup.operations.push_back(op);
  }
  setup.topFace = cam::Target{pb, ElementRef{7, "1/face5"}};

  REQUIRE(job.RemovePart(pb));
  const cam::Setup& after = *job.FindSetup(setup.id);
  REQUIRE(after.placements.size() == 1);
  CHECK(after.placements[0].jobPartId == pa);
  REQUIRE(after.operations.size() == 2);
  CHECK(after.operations[0].targets.size() == 1);
  CHECK(after.operations[1].targets.empty()); // its only target went with B
  CHECK(!after.topFace.IsSet());              // and so did Z zero
}

TEST_CASE("cam job: a part off the sheet is a state, then a refusal",
          "[cam][job]") {
  // The first draft of the job plan made this a refusal at DRAG time. Both
  // slicers do it the other way -- a state per placement, computed when
  // asked -- and they are right: a sheet is laid out THROUGH the states
  // where a part is half off the edge, and blocking the drag makes the
  // layout impossible to reach. So: three states here, and the refusal at
  // the post, where the mistake becomes G-code.
  Document a;
  a.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 40, 30, 10));

  cam::JobDocument job;
  const uint64_t part = job.AddPart(1, "A");
  cam::Setup& setup = job.AddSetup("Sheet");
  // A sheet you actually own: 200 x 200, the part referenced from its corner.
  setup.stock.mode = cam::StockMode::Fixed;
  setup.stock.size = {200.0, 200.0, 12.0};
  setup.stock.place = {0.0, 0.0, 1.0};

  std::vector<Body> bodies;
  for (const Body& b : a.GetBodies()) {
    bodies.push_back(b);
    bodies.back().jobPartId = part;
  }

  cam::Placement place;
  place.id = job.AllocateId();
  place.jobPartId = part;
  setup.placements.push_back(place);

  auto fitOf = [&]() {
    const std::vector<cam::PlacementFit> f =
        cam::CheckPlacements(setup, bodies);
    REQUIRE(f.size() == 1);
    return f[0].fit;
  };

  CHECK(fitOf() == cam::Fit::Inside);
  CHECK(cam::PlacementRefusal(setup, bodies).empty());

  // Hanging over the edge: the cut runs off the workpiece.
  setup.placements[0].pos = {180.0, 0.0};
  CHECK(fitOf() == cam::Fit::PartlyOutside);
  CHECK(cam::PlacementRefusal(setup, bodies).find("hangs over") !=
        std::string::npos);

  // Not on it at all is a DIFFERENT mistake, and usually a different fix.
  setup.placements[0].pos = {900.0, 0.0};
  CHECK(fitOf() == cam::Fit::FullyOutside);
  CHECK(cam::PlacementRefusal(setup, bodies).find("not on the stock") !=
        std::string::npos);

  // A placement that is not cut this run is not asked to fit.
  setup.placements[0].enabled = false;
  CHECK(fitOf() == cam::Fit::Inside);
  CHECK(cam::PlacementRefusal(setup, bodies).empty());
}

TEST_CASE("cam job: relative stock is drawn around every placed part",
          "[cam][job]") {
  // The other half of "the stock is the sheet": relative stock spans the
  // whole arrangement, so nothing can be outside it by construction -- and
  // adding a second part grows the workpiece rather than cropping it.
  Document a, b;
  a.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 40, 30, 10));
  b.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 40, 30, 10));

  cam::JobDocument job;
  const uint64_t pa = job.AddPart(1, "A");
  const uint64_t pb = job.AddPart(2, "B");
  cam::Setup& setup = job.AddSetup("Sheet");
  std::vector<Body> bodies;
  for (const Body& body : a.GetBodies()) {
    bodies.push_back(body);
    bodies.back().jobPartId = pa;
  }
  for (const Body& body : b.GetBodies()) {
    bodies.push_back(body);
    bodies.back().jobPartId = pb;
  }
  for (uint64_t part : {pa, pb}) {
    cam::Placement p;
    p.id = job.AllocateId();
    p.jobPartId = part;
    if (part == pb)
      p.pos = {300.0, 0.0};
    setup.placements.push_back(p);
  }

  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(setup, bodies, lo, hi));
  CHECK(hi.x - lo.x > 330.0); // both parts plus their margins
  for (const cam::PlacementFit& f : cam::CheckPlacements(setup, bodies))
    CHECK(f.fit == cam::Fit::Inside);

  // Stacked, though, is scrap either way: whichever is cut second comes out
  // of the first one's offcut.
  setup.placements[1].pos = {0.0, 0.0};
  const std::vector<cam::PlacementFit> fits =
      cam::CheckPlacements(setup, bodies);
  REQUIRE(fits.size() == 2);
  CHECK(fits[0].overlaps.size() == 1);
  CHECK(cam::PlacementRefusal(setup, bodies).find("on top of each other") !=
        std::string::npos);
}

TEST_CASE("cam job: what order it cuts in", "[cam][job]") {
  // With one part, authoring order IS cutting order and this field would
  // never have been noticed. With four it is the difference between one tool
  // change per tool and one per part per tool -- CamPost emits a change on
  // every change of tool number, so the natural authoring order (per part)
  // is the expensive one (docs/cam/setup.md#cutting-order-and-split-output).
  cam::JobDocument job;
  const uint64_t pa = job.AddPart(1, "A");
  const uint64_t pb = job.AddPart(2, "B");
  cam::Setup& setup = job.AddSetup("Sheet");

  // Authored per part, which is how anyone would build it: rough A, finish
  // A, rough B, finish B -- four tool changes for two tools.
  struct Row {
    uint64_t part;
    int tool;
  };
  const Row rows[] = {{pa, 1}, {pa, 2}, {pb, 1}, {pb, 2}};
  for (const Row& r : rows) {
    cam::Operation op;
    op.id = job.AllocateId();
    op.toolNumber = r.tool;
    op.targets.push_back(cam::Target{r.part, ElementRef{1, "1/face0"}});
    setup.operations.push_back(op);
  }

  auto toolsOf = [&]() {
    std::vector<int> out;
    for (size_t i : cam::CuttingOrder(setup))
      out.push_back(setup.operations[i].toolNumber);
    return out;
  };
  auto partsOf = [&]() {
    std::vector<uint64_t> out;
    for (size_t i : cam::CuttingOrder(setup))
      out.push_back(setup.operations[i].targets.front().jobPartId);
    return out;
  };

  CHECK(toolsOf() == std::vector<int>{1, 2, 1, 2}); // authored: as written

  setup.order = cam::OrderOutputBy::Tool;
  CHECK(toolsOf() == std::vector<int>{1, 1, 2, 2}); // one change per tool
  CHECK(partsOf() == std::vector<uint64_t>{pa, pb, pa, pb});

  setup.order = cam::OrderOutputBy::Part;
  CHECK(partsOf() == std::vector<uint64_t>{pa, pa, pb, pb});
  CHECK(toolsOf() == std::vector<int>{1, 2, 1, 2}); // stable within a part
}

TEST_CASE("cam job: grouping keeps first-appearance order", "[cam][job]") {
  // Grouped, not SORTED. A tool table is not ordered by how early each
  // cutter is wanted, so sorting the key would move a roughing pass after a
  // finishing one that happens to carry a lower number.
  cam::JobDocument job;
  cam::Setup& setup = job.AddSetup("Sheet");
  for (int tool : {7, 3, 7, 3}) {
    cam::Operation op;
    op.id = job.AllocateId();
    op.toolNumber = tool;
    setup.operations.push_back(op);
  }
  setup.order = cam::OrderOutputBy::Tool;
  std::vector<int> got;
  for (size_t i : cam::CuttingOrder(setup))
    got.push_back(setup.operations[i].toolNumber);
  CHECK(got == std::vector<int>{7, 7, 3, 3}); // 7 first: it was wanted first
}

TEST_CASE("cam job: an unknown cutting order is refused", "[cam][job]") {
  // The rule this project applies everywhere: a job that silently posts in
  // an order other than the one it names is a broken cutter, and every
  // plausible substitute produces a file that looks right.
  const std::string bad = R"({"format":"designer-document","version":75,
    "kind":"job","cam":[{"id":1,"name":"J","order":"fixture"}]})";
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_bad_order.json";
  std::ofstream(path) << bad;
  auto loaded = DocumentIO::LoadJob(path.string());
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().find("authored, tool or part") != std::string::npos);
  std::filesystem::remove(path);
}

TEST_CASE("cam job: two parts, clamped differently, one sheet", "[cam][job]") {
  // The case the whole per-part frame exists for: a speaker cabinet's baffle
  // and a rail, each modelled in the orientation it has in the assembly, both
  // cut from one board. They need two fixture frames and one stock, which a
  // per-SETUP frame cannot say
  // (docs/cam/setup.md#fixturing-frame-and-rotary-index).
  Document baffle, rail;
  // 210 wide, 16 thick, 400 tall: standing upright, like a real baffle.
  baffle.AddFeature(
      std::make_unique<BoxFeature>("Baffle", glm::vec3(0, 0, 0), 210, 16, 400));
  // 300 long, 60 tall, 18 thick: on its edge.
  rail.AddFeature(
      std::make_unique<BoxFeature>("Rail", glm::vec3(0, 0, 0), 300, 60, 18));

  cam::JobDocument job;
  const uint64_t pb = job.AddPart(1, "Baffle");
  const uint64_t pr = job.AddPart(2, "Rail");
  cam::Setup& setup = job.AddSetup("Sheet");

  std::vector<Body> bodies;
  for (const Body& b : baffle.GetBodies()) {
    bodies.push_back(b);
    bodies.back().jobPartId = pb;
  }
  for (const Body& b : rail.GetBodies()) {
    bodies.push_back(b);
    bodies.back().jobPartId = pr;
  }

  // Lay the baffle on its big face (normal along Y) and leave the rail as
  // modelled. Only the baffle turns.
  cam::Placement pl;
  pl.id = job.AllocateId();
  pl.jobPartId = pb;
  pl.frame = cam::SolveFaceUp(glm::dvec3(0, 1, 0), glm::dquat(1, 0, 0, 0));
  setup.placements.push_back(pl);

  CHECK(!cam::FrameIsSquare(setup, pb)); // the baffle is clamped
  CHECK(cam::FrameIsSquare(setup, pr));  // the rail is as modelled
  CHECK(cam::FrameIsSquare(setup));      // and so is the setup itself

  const std::vector<Body> framed = cam::BodiesInSetupFrame(setup, bodies);
  REQUIRE(framed.size() == 2);
  const auto extent = [](const Body& b) {
    glm::vec3 lo, hi;
    b.shape.GetBoundingBox(lo, hi);
    return hi - lo;
  };
  // Laid flat, the baffle is 210 x 400 on the bed and 16 thick -- it is the
  // Z extent that says it worked, since upright it was 400 tall.
  const glm::vec3 eb = extent(framed[0]);
  CHECK_THAT(eb.z, WithinAbs(16.0, 1e-3));
  CHECK_THAT(std::max(eb.x, eb.y), WithinAbs(400.0, 1e-3));
  // The rail did not move: 18 thick, as modelled.
  const glm::vec3 er = extent(framed[1]);
  CHECK_THAT(er.z, WithinAbs(18.0, 1e-3));

  // One sheet covers both, and it is 16-to-18 thick rather than 400.
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(setup, bodies, lo, hi));
  CHECK(hi.z - lo.z < 40.0);
}

TEST_CASE("cam job: one sheet, two parts that share feature ids, two cutters",
          "[cam][job]") {
  // The job a real sheet is, and the shape of job the corpus did not have.
  // Every other CAM case cuts ONE part with ONE tool, and the multi-part ones
  // use two DIFFERENT documents -- so no feature id collides, and the picks
  // stop at TargetFor's refusal. Five defects lived behind that gap: a grbl
  // file that never named its second tool (so everything after the change was
  // read back as tool 1), a drill retracting up its own hole reported as a
  // rapid through material, and three geometry pickers that answered for the
  // first body carrying a matching feature id -- which is a coin toss here,
  // because every document numbers its own features from 1.
  auto left = HoleDoc(10.0, 12.0);  // 40 x 30 x 12, a 10 mm hole at (20, 15)
  auto right = HoleDoc(10.0, 12.0); // ...and its identical twin
  const ElementRef wallL = HoleWall(*left);
  const ElementRef wallR = HoleWall(*right);
  // The premise, asserted rather than assumed: these two really do collide.
  // If a future change makes ids unique across documents, this test stops
  // testing what it is for and should say so here rather than pass quietly.
  REQUIRE(wallL.featureId == wallR.featureId);
  REQUIRE(wallL.name == wallR.name);

  cam::JobDocument job;
  const uint64_t pl = job.AddPart(1, "Left");
  const uint64_t pr = job.AddPart(2, "Right");
  cam::Setup& setup = job.AddSetup("Sheet");
  cam::Tool drill;
  drill.number = 2;
  drill.name = "10 mm drill";
  drill.diameter = 10.0;
  drill.type = cam::ToolType::Drill;
  drill.tipAngleDeg = 118.0;
  setup.tools.push_back(drill);
  setup.stock.mode = cam::StockMode::Fixed;
  setup.stock.size = {120.0, 42.0, 12.0};
  // Clear of the sheet: a bare AddSetup starts at 5 mm, which is INSIDE a
  // 12 mm board -- add_cam_setup derives this from the stock, a hand-built
  // setup does not.
  setup.safeZ = 20.0;

  std::vector<Body> bodies;
  for (const Body& b : left->GetBodies()) {
    bodies.push_back(b);
    bodies.back().jobPartId = pl;
  }
  for (const Body& b : right->GetBodies()) {
    bodies.push_back(b);
    bodies.back().jobPartId = pr;
  }
  REQUIRE(bodies.size() == 2);

  // Apart on the sheet: the left part where it was modelled, the right one
  // 60 mm along. That gap is what makes a wrong pick VISIBLE -- the same
  // element name resolves to the same local coordinates on both parts, so
  // only WHERE the cut lands says which part answered.
  for (const auto& [id, x] : {std::pair{pl, 0.0}, std::pair{pr, 60.0}}) {
    cam::Placement place;
    place.id = job.AllocateId();
    place.jobPartId = id;
    place.pos = {x, 0.0};
    setup.placements.push_back(place);
  }
  REQUIRE(cam::PlacementRefusal(setup, bodies).empty());

  // Drill the LEFT part's hole, profile the RIGHT one's, then cut both free.
  const auto add = [&](cam::OpType type, cam::Target target, int tool,
                       cam::Side side, bool tabs) {
    const cam::DerivedOp d =
        cam::DeriveOperation(setup, bodies, type, target, tool);
    REQUIRE(d.ok);
    cam::Operation op = d.op;
    op.id = job.AllocateId();
    op.toolNumber = tool;
    op.side = side;
    if (!tabs)
      op.tabCount = 0;
    setup.operations.push_back(op);
  };
  add(cam::OpType::Drill, cam::Target{pl, wallL}, 2, cam::Side::Inside, false);
  add(cam::OpType::Profile, cam::Target{pr, wallR}, 1, cam::Side::Inside,
      false);
  add(cam::OpType::Profile, cam::Target{}, 1, cam::Side::Outside, true);

  const std::vector<cam::Toolpath> paths = cam::GenerateAll(setup, bodies);
  REQUIRE(paths.size() == 3);
  for (const cam::Toolpath& tp : paths) {
    INFO(tp.error);
    CHECK(tp.error.empty());
    CHECK(!tp.moves.empty());
  }

  // Which part each operation actually cut. The hole is at (20, 15) in both
  // parts' own coordinates, so the LEFT one's is at x 20 on the sheet and the
  // RIGHT one's at x 80: a picker that answers for whichever body carries the
  // feature id first puts the second cut 60 mm from where it belongs.
  const auto cutCentreX = [](const cam::Toolpath& tp) {
    double lo = 1e300, hi = -1e300;
    for (const cam::Move& m : tp.moves)
      if (m.kind != cam::Move::Kind::Rapid) { // a drill only ever plunges
        lo = std::min(lo, m.to.x);
        hi = std::max(hi, m.to.x);
      }
    return (lo + hi) * 0.5;
  };
  CHECK_THAT(cutCentreX(paths[0]), WithinAbs(20.0, 1.0)); // left, drilled
  CHECK_THAT(cutCentreX(paths[1]), WithinAbs(80.0, 1.0)); // right, profiled

  // The file names BOTH cutters. Without the T word a reader has to assume
  // tool 1, and the verifier below would simulate a 10 mm drill's holes with
  // a 6 mm end mill -- or, the other way round, miss a gouge.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("grbl", setup, paths, {}, gcode, &error));
  CHECK(gcode.find("\nT1\n") != std::string::npos);
  CHECK(gcode.find("\nT2\n") != std::string::npos);
  const cam::GProgram program = cam::ParseGcode(gcode);
  bool sawOne = false, sawTwo = false;
  for (const cam::GMove& m : program.moves) {
    sawOne |= m.tool == 1;
    sawTwo |= m.tool == 2;
  }
  CHECK(sawOne);
  CHECK(sawTwo);

  // And the whole sheet checks out: nothing cuts into a part, and nothing
  // rapids through material -- including the drill coming back up the hole it
  // has just made, which is a cone and not a cylinder.
  std::vector<cam::Toolpath::Relief> reliefs;
  std::vector<cam::Toolpath::Tab> tabs;
  for (const cam::Toolpath& tp : paths) {
    reliefs.insert(reliefs.end(), tp.reliefs.begin(), tp.reliefs.end());
    tabs.insert(tabs.end(), tp.tabs.begin(), tp.tabs.end());
  }
  const cam::Verdict v =
      cam::VerifyGcode(gcode, setup, bodies, 0.25, reliefs, tabs);
  // Name the line, not just the count: "16 rapids through material" with no
  // line number is a fact nobody can chase.
  std::string badLine;
  for (int at = 1, from = 0; from <= static_cast<int>(gcode.size()); at++) {
    const size_t end = std::min(gcode.find('\n', from), gcode.size());
    if (at == v.firstBadLine)
      badLine = gcode.substr(from, end - from);
    from = static_cast<int>(end) + 1;
    if (end == gcode.size())
      break;
  }
  INFO("gouge " << v.maxGouge << " at " << v.gougeAt.x << "," << v.gougeAt.y
                << "; first bad line " << v.firstBadLine << ": " << badLine);
  CHECK(v.maxGouge < 0.4);
  CHECK(v.rapidsThroughStock == 0);

  // The tabs the outline was asked for -- four per part -- and the toolpath
  // says where it left them. Told that, the verifier stops reading them as
  // material left standing: before this EVERY job that used a tab reported
  // `complete: false`, which made the flag say nothing at all.
  CHECK(v.tabs == 8);
  CHECK(v.Complete(0.25 * 1.5));
  const cam::Verdict blind = cam::VerifyGcode(gcode, setup, bodies, 0.25);
  CHECK_FALSE(blind.Complete(0.25 * 1.5)); // the same job, not told
}

TEST_CASE("cam job: the layout says where each part is, and when they are too "
          "close to cut",
          "[cam][job]") {
  // Two answers the check already had to work out and threw away. "Does it
  // fit" and "where is it" are the same measurement, and a caller nesting a
  // sheet had to reconstruct the second one part at a time -- shrink-wrap the
  // stock, disable every other placement, read the stock box back, restore.
  auto a = HoleDoc(10.0, 12.0);
  auto b = HoleDoc(10.0, 12.0);
  cam::JobDocument job;
  const uint64_t pa = job.AddPart(1, "A");
  const uint64_t pb = job.AddPart(2, "B");
  cam::Setup& setup = job.AddSetup("Sheet");
  setup.stock.mode = cam::StockMode::Fixed;
  setup.stock.size = {120.0, 42.0, 12.0};

  std::vector<Body> bodies;
  for (const Body& body : a->GetBodies()) {
    bodies.push_back(body);
    bodies.back().jobPartId = pa;
  }
  for (const Body& body : b->GetBodies()) {
    bodies.push_back(body);
    bodies.back().jobPartId = pb;
  }
  const auto place = [&](uint64_t part, double x) {
    cam::Placement pl;
    pl.id = job.AllocateId();
    pl.jobPartId = part;
    pl.pos = {x, 0.0};
    setup.placements.push_back(pl);
  };
  place(pa, 0.0);
  place(pb, 60.0); // 40 wide each, so a 20 mm gap

  std::vector<cam::PlacementFit> fits = cam::CheckPlacements(setup, bodies);
  REQUIRE(fits.size() == 2);
  // The box is in SETUP space: where the material is once the part is laid
  // the way it is clamped, which is what a nesting caller needs.
  CHECK_THAT(fits[0].lo.x, WithinAbs(0.0, 1e-6));
  CHECK_THAT(fits[0].hi.x, WithinAbs(40.0, 1e-6));
  CHECK_THAT(fits[1].lo.x, WithinAbs(60.0, 1e-6));
  CHECK_THAT(fits[1].hi.x, WithinAbs(100.0, 1e-6));
  CHECK_THAT(fits[1].hi.y - fits[1].lo.y, WithinAbs(30.0, 1e-6));

  // 20 mm apart, 6 mm cutter: room. Nothing to say.
  CHECK(cam::LayoutWarnings(setup, bodies).empty());

  // 4 mm apart, same cutter: cutting one out cuts into the other. A WARNING,
  // not a refusal -- the parts do not overlap, and a narrower tool between
  // them would be fine, so this is the operator's call and not the app's.
  setup.placements[1].pos = {44.0, 0.0};
  const std::vector<std::string> warn = cam::LayoutWarnings(setup, bodies);
  REQUIRE(warn.size() == 1);
  INFO(warn[0]);
  CHECK(warn[0].find("4 mm apart") != std::string::npos);
  CHECK(warn[0].find("6 mm") != std::string::npos);
  CHECK(cam::PlacementRefusal(setup, bodies).empty()); // still cuttable

  // Diagonal neighbours are not "between" anything: nothing cuts there.
  setup.placements[1].pos = {44.0, 34.0};
  CHECK(cam::LayoutWarnings(setup, bodies).empty());
}

TEST_CASE("cam job: a per-part frame survives a save and reload",
          "[cam][job]") {
  // v76. A frame that reloads as "as modelled" is a part cut in an
  // orientation it was not clamped in -- the failure setup-frame.md's own
  // golden exists for, one level down.
  cam::JobDocument job;
  const uint64_t part = job.AddPart(1, "Baffle");
  cam::Setup& setup = job.AddSetup("Sheet");
  cam::Placement pl;
  pl.id = job.AllocateId();
  pl.jobPartId = part;
  pl.frame = cam::SolveFaceUp(glm::dvec3(0, 1, 0), glm::dquat(1, 0, 0, 0));
  setup.placements.push_back(pl);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_part_frame.json";
  REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  auto back = DocumentIO::LoadJob(path.string());
  REQUIRE(back.has_value());
  const cam::Setup& read = (*back)->GetSetups()[0];
  REQUIRE(read.placements.size() == 1);
  REQUIRE(read.placements[0].frame.has_value());
  CHECK(!cam::FrameIsSquare(read, part));
  CHECK_THAT(read.placements[0].frame->w, WithinAbs(pl.frame->w, 1e-12));
  CHECK_THAT(read.placements[0].frame->x, WithinAbs(pl.frame->x, 1e-12));

  // And a placement WITHOUT one still means "as the setup says", rather than
  // reloading as an identity that quietly un-clamps the part.
  cam::JobDocument plain;
  const uint64_t p2 = plain.AddPart(1, "Panel");
  cam::Setup& s2 = plain.AddSetup("Sheet");
  s2.frame = cam::SolveFaceUp(glm::dvec3(0, 1, 0), glm::dquat(1, 0, 0, 0));
  cam::Placement bare;
  bare.id = plain.AllocateId();
  bare.jobPartId = p2;
  s2.placements.push_back(bare);
  REQUIRE(DocumentIO::SaveJob(plain, path.string()).has_value());
  auto back2 = DocumentIO::LoadJob(path.string());
  REQUIRE(back2.has_value());
  const cam::Setup& read2 = (*back2)->GetSetups()[0];
  CHECK_FALSE(read2.placements[0].frame.has_value());
  CHECK(!cam::FrameIsSquare(read2, p2)); // the setup's frame still applies
  std::filesystem::remove(path);
}

TEST_CASE("cam job: the same element name on two parts is refused, not guessed",
          "[cam][job]") {
  // Found by driving the RPC, not by reading the code: two boxes in two
  // documents both have feature id 1 and both have a face called "1/face5",
  // so a target of {feature_id: 1, element: "1/face5"} names TWO faces. The
  // qualifier exists for this, and resolving it by searching for the first
  // body that answers threw the qualifier away again — it cut the first
  // part, silently, whichever one you meant.
  //
  // This exercises the resolver's rule directly; ui::TargetFor is where the
  // app applies it, over both doors.
  const auto owners = [](const std::vector<Body>& bodies,
                         const ElementRef& ref) {
    std::vector<uint64_t> out;
    for (const Body& b : bodies) {
      if (b.createdBy != ref.featureId || !b.elementMap.Resolve(ref.name))
        continue;
      if (std::find(out.begin(), out.end(), b.jobPartId) == out.end())
        out.push_back(b.jobPartId);
    }
    return out;
  };

  Document a, b;
  a.AddFeature(
      std::make_unique<BoxFeature>("A", glm::vec3(0, 0, 0), 40, 30, 10));
  b.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0, 0, 0), 60, 50, 10));
  std::vector<Body> bodies;
  for (const Body& body : a.GetBodies()) {
    bodies.push_back(body);
    bodies.back().jobPartId = 1;
  }
  for (const Body& body : b.GetBodies()) {
    bodies.push_back(body);
    bodies.back().jobPartId = 2;
  }
  REQUIRE(bodies.size() == 2);
  // Two independently modelled boxes really do collide on both halves of an
  // ElementRef. That is the premise, so assert it rather than assume it.
  CHECK(bodies[0].createdBy == bodies[1].createdBy);

  const std::string name = bodies[0].elementMap.names.begin()->second;
  const ElementRef shared{bodies[0].createdBy, name};
  CHECK(owners(bodies, shared).size() == 2); // ambiguous: must be refused

  // A name only one part owns still resolves without help. (Boxes of
  // different sizes still share face names, so make one unique by hand.)
  bodies[1].elementMap.names.begin()->second = "unique/face";
  const ElementRef only{bodies[1].createdBy, "unique/face"};
  const std::vector<uint64_t> one = owners(bodies, only);
  REQUIRE(one.size() == 1);
  CHECK(one[0] == 2);
}

TEST_CASE("cam: an unknown operation type in a job file is REFUSED", "[cam]") {
  // It used to load as a PROFILE. `OpTypeFromName` ended in
  // `return cam::OpType::Profile`, so "pocekt" -- or a merged file, or a
  // hand-edited one, which the one-file-per-item directory format invites --
  // came back as a different strategy against a different region, and was
  // posted and cut without a word. Same class as the mate type that loaded as
  // Fastened and solved perfectly well with the wrong degrees of freedom
  // (docs/conventions.md#refuse-dont-substitute).
  cam::JobDocument job;
  cam::Setup& setup = job.AddSetup("Side A");
  cam::Operation op;
  op.id = job.AllocateId();
  op.name = "Clear it";
  op.type = cam::OpType::Pocket;
  setup.operations.push_back(op);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_bad_optype.json";
  REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());

  // Corrupt exactly one character of the type, as a typo would.
  std::string text;
  {
    std::ifstream in(path);
    text.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
  }
  REQUIRE(text.find("\"pocket\"") != std::string::npos);
  text.replace(text.find("\"pocket\""), 8, "\"pocekt\"");
  {
    std::ofstream out(path);
    out << text;
  }

  // The refusal names the value AND what would have been accepted, so the
  // typo is fixable from the message alone.
  auto back = DocumentIO::LoadJob(path.string());
  REQUIRE_FALSE(back.has_value());
  const std::string why = back.error();
  INFO("refusal: " << why);
  CHECK(why.find("pocekt") != std::string::npos);
  CHECK(why.find("pocket") != std::string::npos);
  CHECK(why.find("edge_round") != std::string::npos);
}

TEST_CASE("cam: every operation type round-trips through its name", "[cam]") {
  // The table is the one place a type's name is written down. If a row is
  // ever added with a name the reader does not accept -- or removed while a
  // writer still emits it -- this fails rather than a job file silently
  // changing strategy on reload.
  for (const cam::OpTypeRow& row : cam::OpTypes()) {
    cam::OpType back{};
    INFO("row " << row.name);
    REQUIRE(cam::TryOpTypeFromName(row.name, back));
    CHECK(back == row.type);
    CHECK(std::string(cam::OpTypeName(row.type)) == row.name);
    CHECK(cam::OpTypeNameList().find(row.name) != std::string::npos);
  }
  cam::OpType unused{};
  CHECK_FALSE(cam::TryOpTypeFromName("no_such_op", unused));
  CHECK_FALSE(cam::TryOpTypeFromName("", unused));
}

TEST_CASE("cam: an unknown tool type or side in a job file is REFUSED",
          "[cam]") {
  // The two CAM enums the 2026-08-24 enum sweep never reached, because the
  // CAM loader was written after it. Both had the shape that sweep existed to
  // remove, and both matter to the metal:
  //
  //   tool_type -> Flat  : a ball nose steps over by the cusp it leaves and a
  //                        V-bit chamfers by its angle. Read either as a flat
  //                        mill and the path is a square-shouldered cut --
  //                        which the verifier, modelling the same wrong tool,
  //                        then calls clean.
  //   side      -> Outside: a pocket clearing outside its own boundary has
  //                        nothing to remove. This exact result already
  //                        shipped once, when `side` was not serialized at
  //                        all (the v62 fix).
  const auto save = [](cam::JobDocument& job) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "cam_bad_enum.json";
    REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
    return path;
  };
  const auto corrupt = [](const std::filesystem::path& path,
                          const std::string& from, const std::string& to) {
    std::string text;
    {
      std::ifstream in(path);
      text.assign(std::istreambuf_iterator<char>(in),
                  std::istreambuf_iterator<char>());
    }
    REQUIRE(text.find(from) != std::string::npos);
    text.replace(text.find(from), from.size(), to);
    std::ofstream out(path);
    out << text;
  };

  SECTION("tool type") {
    cam::JobDocument job;
    cam::Setup& setup = job.AddSetup("Side A");
    setup.tools[0].type = cam::ToolType::Ball;
    const auto path = save(job);
    corrupt(path, "\"ball\"", "\"bal\"");
    auto back = DocumentIO::LoadJob(path.string());
    REQUIRE_FALSE(back.has_value());
    INFO("refusal: " << back.error());
    CHECK(back.error().find("bal") != std::string::npos);
    CHECK(back.error().find("vbit") != std::string::npos);
  }

  SECTION("side") {
    cam::JobDocument job;
    cam::Setup& setup = job.AddSetup("Side A");
    cam::Operation op;
    op.id = job.AllocateId();
    op.name = "Clear it";
    op.type = cam::OpType::Pocket;
    op.side = cam::Side::Inside;
    setup.operations.push_back(op);
    const auto path = save(job);
    corrupt(path, "\"inside\"", "\"insde\"");
    auto back = DocumentIO::LoadJob(path.string());
    REQUIRE_FALSE(back.has_value());
    INFO("refusal: " << back.error());
    CHECK(back.error().find("insde") != std::string::npos);
    CHECK(back.error().find("outside") != std::string::npos);
  }
}

TEST_CASE("cam: every tool type and side round-trips through its name",
          "[cam]") {
  for (const cam::ToolTypeRow& row : cam::ToolTypes()) {
    cam::ToolType back{};
    INFO("tool " << row.name);
    REQUIRE(cam::TryToolTypeFromName(row.name, back));
    CHECK(back == row.type);
    CHECK(std::string(cam::ToolTypeName(row.type)) == row.name);
  }
  for (const cam::SideRow& row : cam::Sides()) {
    cam::Side back{};
    INFO("side " << row.name);
    REQUIRE(cam::TrySideFromName(row.name, back));
    CHECK(back == row.side);
    CHECK(std::string(cam::SideName(row.side)) == row.name);
  }
  cam::ToolType t{};
  cam::Side sd{};
  CHECK_FALSE(cam::TryToolTypeFromName("endmill", t));
  CHECK_FALSE(cam::TrySideFromName("left", sd));
}

#endif // HAVE_CAM
