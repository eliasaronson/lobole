#ifdef HAVE_CAM

// What the CUTTER's own geometry does: tool shapes, edge breaks, drilling
// and pecking, corner relief, lead arcs, and roughing then finishing.
// One of five files split from test_cam.cpp
// (docs/architecture.md#source-layout).

#include "cam_fixtures.h"
#include "core/features/FilletFeature.h"

TEST_CASE("cam: a ball nose steps over by the cusp it leaves", "[cam]") {
  // A flat mill's floor is flat whatever the stepover, so 45% of the cutter
  // stands. A ball nose leaves a cusp between passes, and the height of that
  // cusp is what decides the step. Before this a ball cleared a pocket on
  // exactly the flat mill's path — same length, same everything — and left
  // 160 mm2 of scallop that only the verifier ever mentioned.
  cam::Tool flat;
  flat.number = 1;
  flat.diameter = 6.0;
  cam::Tool ball = flat;
  ball.type = cam::ToolType::Ball;

  CHECK_THAT(cam::DerivedStepover(flat), WithinAbs(0.45, 1e-9));
  const double s = cam::DerivedStepover(ball);
  INFO("ball stepover fraction " << s);
  CHECK(s < 0.45); // finer than a flat mill's
  CHECK(s > 0.02); // and not so fine it never finishes

  // It really is the cusp equation: stepping `s * d` apart on a ball of
  // radius r leaves r - sqrt(r^2 - (step/2)^2), and that must be the 2% of
  // the radius the derivation asks for.
  const double r = ball.diameter * 0.5;
  const double step = s * ball.diameter;
  const double cusp = r - std::sqrt(std::max(0.0, r * r - step * step * 0.25));
  INFO("cusp " << cusp << " vs target " << r * 0.02);
  CHECK_THAT(cusp, WithinRel(r * 0.02, 1e-6));

  // Bigger cutter, bigger step — the rule is in the tool, not in millimetres.
  cam::Tool big = ball;
  big.diameter = 12.0;
  CHECK(cam::DerivedStepover(big) * big.diameter >
        cam::DerivedStepover(ball) * ball.diameter);

  // And a derived operation picks it up rather than hard-coding 0.45.
  auto doc = FlangeDoc(/*holeDia=*/0.0, /*boss=*/false);
  const ElementRef floor = PocketFloorRef(*doc);
  REQUIRE(floor.IsSet());
  cam::Setup setup = BasicSetup();
  setup.tools[0].type = cam::ToolType::Ball;
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Pocket, cam::Target{0, floor});
  REQUIRE(d.ok);
  CHECK_THAT(d.op.stepover,
             WithinAbs(cam::DerivedStepover(setup.tools[0]), 1e-9));
}

TEST_CASE("cam: a cone is refused a floor to clear", "[cam]") {
  // A V-bit or a drill point run along a floor leaves a groove as deep as the
  // pass, not a floor. A ball nose is fine — it leaves a cusp, and the step
  // is sized for it — so this refuses only the shapes that have no answer.
  auto doc = FlangeDoc(/*holeDia=*/0.0, /*boss=*/false);
  const ElementRef floor = PocketFloorRef(*doc);
  REQUIRE(floor.IsSet());
  for (const auto& [type, word] :
       std::vector<std::pair<cam::ToolType, const char*>>{
           {cam::ToolType::VBit, "V-bit"}, {cam::ToolType::Drill, "drill"}}) {
    cam::Setup s = BasicSetup();
    s.tools[0].type = type;
    s.tools[0].tipAngleDeg = 90.0;
    for (const cam::OpType op :
         {cam::OpType::Pocket, cam::OpType::Face, cam::OpType::Adaptive}) {
      const cam::DerivedOp d =
          cam::DeriveOperation(s, doc->GetBodies(), op, cam::Target{0, floor});
      INFO(word << " / op " << static_cast<int>(op) << ": " << d.why);
      CHECK_FALSE(d.ok);
      CHECK(d.why.find(word) != std::string::npos);
    }
  }
  // Flat and ball are both allowed — the ball with a finer step.
  for (const cam::ToolType type : {cam::ToolType::Flat, cam::ToolType::Ball}) {
    cam::Setup s = BasicSetup();
    s.tools[0].type = type;
    const cam::DerivedOp d = cam::DeriveOperation(
        s, doc->GetBodies(), cam::OpType::Pocket, cam::Target{0, floor});
    INFO(d.why);
    CHECK(d.ok);
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
      setup, doc->GetBodies(), cam::OpType::EdgeRound, cam::Target{0, wall});
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

namespace {
// The block of BlockDoc with its top outline broken in the MODEL: a chamfer
// or a round of `size` on every top edge.
std::unique_ptr<Document> BrokenBlockDoc(double size, EdgeBlendOp how) {
  auto doc = BlockDoc(); // 40 x 30 x 20
  const Body& body = doc->GetBodies().at(0);
  std::vector<ElementRef> top;
  const auto polylines = body.shape.GenerateEdgePolylines();
  for (size_t e = 0; e < polylines.size(); e++) {
    bool atTop = polylines[e].size() >= 2;
    for (const glm::vec3& p : polylines[e])
      atTop = atTop && std::abs(p.z - 20.0f) < 1e-3f;
    if (!atTop)
      continue;
    const std::string* name = body.elementMap.EdgeNameOf(static_cast<int>(e));
    REQUIRE(name != nullptr);
    top.push_back(ElementRef{body.createdBy, *name});
  }
  REQUIRE(top.size() == 4);
  doc->AddFeature(std::make_unique<FilletFeature>("Break", top, size, how));
  REQUIRE(doc->Regenerate());
  return doc;
}
} // namespace

TEST_CASE("cam: a small chamfer on a round rim reads at its own angle",
          "[cam]") {
  // The pan/tilt base plate's 0.5 mm chamfer on its Ø73.6 rim read "92.23
  // degrees included (0.52 mm wide)": the probe measured the break across
  // sections flattened to 0.02 mm chords, which is 4% of half a millimetre.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Disc", glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 36.8, 24.7));
  const Body& body = doc->GetBodies().at(0);
  std::vector<ElementRef> rim;
  const auto polylines = body.shape.GenerateEdgePolylines();
  for (size_t e = 0; e < polylines.size(); e++) {
    bool atTop = polylines[e].size() >= 2;
    for (const glm::vec3& p : polylines[e])
      atTop = atTop && std::abs(p.z - 24.7f) < 1e-3f;
    if (!atTop)
      continue;
    const std::string* name = body.elementMap.EdgeNameOf(static_cast<int>(e));
    REQUIRE(name != nullptr);
    rim.push_back(ElementRef{body.createdBy, *name});
  }
  REQUIRE(rim.size() == 1);
  doc->AddFeature(
      std::make_unique<FilletFeature>("Break", rim, 0.5, EdgeBlendOp::Chamfer));
  REQUIRE(doc->Regenerate());
  cam::Setup setup = BasicSetup();
  setup.tools = {ShapedTool(cam::ToolType::VBit, 6.0, 90.0)};
  // The disc's side: the cylinder's own face0 (the chamfer's cone is the
  // Break's).
  ElementRef wall;
  const std::string side =
      std::to_string(doc->GetFeature(0)->GetId()) + "/face0;M";
  for (const auto& [index, name] : doc->GetBodies().at(0).elementMap.names)
    if (name.rfind(side, 0) == 0)
      wall = ElementRef{doc->GetBodies().at(0).createdBy, name};
  REQUIRE(wall.IsSet());
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::EdgeRound, cam::Target{0, wall});
  INFO(d.why);
  REQUIRE(d.ok);
  CHECK_THAT(d.op.topZ, WithinAbs(24.7, 0.005));
  CHECK_THAT(d.op.edgeSize, WithinAbs(0.5, 0.005));
}

TEST_CASE("cam: an edge break follows the break the model already has",
          "[cam]") {
  // Derived from the wall's own top, a break went below the one modelled
  // above it and as big as the tool allowed: the sampler part's 1.5 mm
  // chamfer came out 6 mm, cut 4.5 mm into the part all round.
  auto doc = BrokenBlockDoc(1.5, EdgeBlendOp::Chamfer);
  cam::Setup setup = BasicSetup();
  setup.tools = {ShapedTool(cam::ToolType::VBit, 12.0, 90.0)};
  const ElementRef wall = SideWall(*doc);
  REQUIRE(wall.IsSet());
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::EdgeRound, cam::Target{0, wall});
  INFO(d.why);
  REQUIRE(d.ok);
  CHECK_THAT(d.op.topZ, WithinAbs(20.0, 0.01));
  CHECK_THAT(d.op.edgeSize, WithinAbs(1.5, 0.02));
  CHECK_THAT(d.op.bottomZ, WithinAbs(18.5, 0.02));
  cam::Setup job = setup;
  cam::Operation op = d.op;
  op.id = 1;
  job.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(job, doc->GetBodies());
  REQUIRE(paths[0].error.empty());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", job, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, job, doc->GetBodies(), 0.25);
  INFO("gouge " << v.maxGouge << " at " << v.gougeAt.x << "," << v.gougeAt.y);
  CHECK(v.maxGouge < 0.05);

  // A stock whose top IS the part's: the top face's own outline sits within
  // the V-bit's reach there, and read as "material all the way up" -- a step
  // -- the break went on below the modelled one (the chess board's 3 mm
  // chamfer, 5.75 mm into its frame).
  {
    auto flush = BrokenBlockDoc(3.0, EdgeBlendOp::Chamfer);
    cam::Setup level = setup;
    level.stock.margin = glm::dvec3(5.0, 5.0, 0.0);
    const cam::DerivedOp f =
        cam::DeriveOperation(level, flush->GetBodies(), cam::OpType::EdgeRound,
                             cam::Target{0, SideWall(*flush)});
    INFO(f.why);
    REQUIRE(f.ok);
    CHECK_THAT(f.op.topZ, WithinAbs(20.0, 0.01));
    CHECK_THAT(f.op.edgeSize, WithinAbs(3.0, 0.02));
    CHECK_THAT(f.op.bottomZ, WithinAbs(17.0, 0.02));
  }

  // A V-bit of another angle would cut another chamfer: refused, naming both.
  setup.tools = {ShapedTool(cam::ToolType::VBit, 12.0, 60.0)};
  const cam::DerivedOp other = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::EdgeRound, cam::Target{0, wall});
  CHECK_FALSE(other.ok);
  CHECK(other.why.find("90 degrees") != std::string::npos);
  CHECK(other.why.find("cuts 60") != std::string::npos);

  // A modelled round: a ball takes its radius, from the block's top.
  auto round = BrokenBlockDoc(2.0, EdgeBlendOp::Fillet);
  setup.tools = {ShapedTool(cam::ToolType::Ball, 6.0)};
  const ElementRef rwall = SideWall(*round);
  const cam::DerivedOp r = cam::DeriveOperation(
      setup, round->GetBodies(), cam::OpType::EdgeRound, cam::Target{0, rwall});
  INFO(r.why);
  REQUIRE(r.ok);
  CHECK_THAT(r.op.topZ, WithinAbs(20.0, 0.01));
  CHECK_THAT(r.op.edgeSize, WithinAbs(2.0, 0.02));
  cam::Setup rjob = setup;
  cam::Operation rop = r.op;
  rop.id = 1;
  rjob.operations.push_back(rop);
  const std::vector<cam::Toolpath> rpaths =
      cam::GenerateAll(rjob, round->GetBodies());
  REQUIRE(rpaths[0].error.empty());
  std::string rgcode;
  REQUIRE(cam::PostProcess("mach3", rjob, rpaths, {}, rgcode, &error));
  const cam::Verdict rv =
      cam::VerifyGcode(rgcode, rjob, round->GetBodies(), 0.25);
  INFO("round gouge " << rv.maxGouge);
  CHECK(rv.maxGouge < 0.05);
}

TEST_CASE("cam: a ball nose rounds the edge over an arc", "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  setup.tools = {ShapedTool(cam::ToolType::Ball, 6.0)};
  const ElementRef wall = SideWall(*doc);
  REQUIRE(wall.IsSet());

  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::EdgeRound, cam::Target{0, wall});
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
      setup, doc->GetBodies(), cam::OpType::EdgeRound, cam::Target{0, wall});
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
  op.targets = {{0, wall}};
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
    cam::JobDocument job;
    cam::Setup& setup = job.AddSetup();
    setup.name = "Job";
    setup.tools = {ShapedTool(cam::ToolType::VBit, 12.0, 60.0)};
    cam::Operation op;
    op.id = job.AllocateId();
    op.type = cam::OpType::EdgeRound;
    op.topZ = 20.0;
    op.bottomZ = 16.0;
    op.edgeSize = 2.5;
    setup.operations.push_back(op);
    REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  }
  auto loaded = DocumentIO::LoadJob(path.string());
  REQUIRE(loaded.has_value());
  REQUIRE((*loaded)->GetSetups().size() == 1);
  const cam::Setup& setup = (*loaded)->GetSetups()[0];
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

TEST_CASE("cam drill: a hole wider than the tool is bored, not refused",
          "[cam][drill]") {
  auto doc = HoleDoc(8.0, 12.0);   // 12 thick: under three diameters deep
  cam::Setup setup = BasicSetup(); // one 6 mm flat
  const ElementRef wall = HoleWall(*doc);
  REQUIRE(wall.IsSet());
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, wall});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 7;
  CHECK(op.peck == 0.0); // ~14 mm deep with a 6 mm tool: under three diameters
  setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  CHECK(paths[0].error.empty());
  // A bore walks a circle the tool's radius inside the wall: the cut moves
  // vary in X and Y, and the lowest of them reaches the bottom.
  double lo = 1e9, minX = 1e9, maxX = -1e9;
  for (const cam::Move& m : paths[0].moves)
    if (m.kind == cam::Move::Kind::Cut) {
      lo = std::min(lo, m.to.z);
      minX = std::min(minX, m.to.x);
      maxX = std::max(maxX, m.to.x);
    }
  CHECK_THAT(lo, WithinAbs(op.bottomZ, 1e-6));
  CHECK_THAT(maxX - minX, WithinAbs(2.0, 1e-3)); // (8 - 6) mm of centre travel
  // ...and it makes the hole without cutting the block: no gouge.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover);
  CHECK(v.maxGouge < 0.4);

  // A hole wider than two tools is rings, the wall last.
  auto wide = HoleDoc(20.0);
  cam::Setup rings = BasicSetup();
  const cam::DerivedOp dw =
      cam::DeriveOperation(rings, wide->GetBodies(), cam::OpType::Drill,
                           cam::Target{0, HoleWall(*wide)});
  REQUIRE(dw.ok);
  cam::Operation ringOp = dw.op;
  ringOp.id = 8;
  rings.operations.push_back(ringOp);
  const auto ringPaths = cam::GenerateAll(rings, wide->GetBodies());
  REQUIRE(ringPaths.size() == 1);
  CHECK(ringPaths[0].error.empty());
  // The LAST cut is on the wall's circle: radius 10 - 3 = 7 from the centre.
  const cam::Move& last = ringPaths[0].moves[ringPaths[0].moves.size() - 2];
  CHECK_THAT(glm::length(glm::dvec2(last.to.x - 20.0, last.to.y - 15.0)),
             WithinAbs(7.0, 1e-3));

  // A hole SMALLER than the tool is still a refusal: it would not fit in.
  auto small = HoleDoc(4.0);
  cam::Setup tight = BasicSetup();
  const cam::DerivedOp ds =
      cam::DeriveOperation(tight, small->GetBodies(), cam::OpType::Drill,
                           cam::Target{0, HoleWall(*small)});
  REQUIRE(ds.ok);
  cam::Operation smallOp = ds.op;
  smallOp.id = 9;
  tight.operations.push_back(smallOp);
  const auto smallPaths = cam::GenerateAll(tight, small->GetBodies());
  REQUIRE(smallPaths.size() == 1);
  CHECK(smallPaths[0].error.find("would not fit") != std::string::npos);
}

TEST_CASE("cam drill: a deep hole pecks, retracting fully each time",
          "[cam][drill]") {
  auto doc = HoleDoc(6.0, 30.0); // 30 deep with a 6 mm tool: five diameters
  cam::Setup setup = BasicSetup();
  const cam::DerivedOp d =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Drill,
                           cam::Target{0, HoleWall(*doc)});
  REQUIRE(d.ok);
  CHECK_THAT(d.op.peck, WithinAbs(6.0, 1e-9)); // derived: the tool's diameter
  cam::Operation op = d.op;
  op.id = 3;
  setup.operations.push_back(op);
  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  CHECK(paths[0].error.empty());
  // Plunges step down a peck at a time, and every retract goes back to the
  // hole's top -- the chips have to come out.
  int plunges = 0, retractsToTop = 0;
  for (size_t i = 1; i < paths[0].moves.size(); i++) {
    const cam::Move& m = paths[0].moves[i];
    if (m.kind == cam::Move::Kind::Plunge) {
      plunges++;
      // One peck, plus the half-millimetre of re-entry above the last floor.
      CHECK(paths[0].moves[i - 1].to.z - m.to.z <= 6.5 + 1e-6);
    }
    if (m.kind == cam::Move::Kind::Rapid &&
        m.to.z > paths[0].moves[i - 1].to.z &&
        std::abs(m.to.z - op.topZ) < 1e-6)
      retractsToTop++;
  }
  CHECK(plunges >= 5);
  CHECK(retractsToTop >= 4);
  // The value survives a save.
  const auto j = DocumentIO::CamSetupToJson(setup);
  CHECK_THAT(DocumentIO::CamSetupFromJson(j).operations[0].peck,
             WithinAbs(6.0, 1e-9));
}

// ---------------------------------------------------------------------------
// R4: corner relief.

TEST_CASE("cam relief: a dogbone clears a pocket's corners, and the verifier "
          "knows it is not a gouge",
          "[cam][relief]") {
  // A 60 x 40 x 20 block with a 30 x 20 pocket 8 deep in its top.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               60.0, 40.0, 20.0));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<BoxFeature>("Void", glm::vec3(15, 10, 12),
                                               30.0, 20.0, 20.0));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocketed", block, voidId,
                                                   BooleanOp::Subtract));
  ElementRef floor;
  for (const Body& b : doc->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::dvec3 o(0.0), n(0.0), u(0.0);
      if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f &&
          std::abs(o.z - 12.0f) < 1e-4f)
        floor = ElementRef{b.createdBy, name};
    }
  REQUIRE(floor.IsSet());

  const auto job = [&](cam::CornerRelief relief) {
    cam::Setup setup = BasicSetup(); // one 6 mm flat
    cam::Operation op;
    op.id = 1;
    op.type = cam::OpType::Pocket;
    op.side = cam::Side::Inside;
    op.topZ = 21.0;
    op.bottomZ = 12.0;
    op.stepdown = 4.0;
    op.cornerRelief = relief;
    op.targets = {cam::Target{0, floor}};
    setup.operations.push_back(op);
    return setup;
  };

  cam::Setup plain = job(cam::CornerRelief::None);
  auto plainPaths = cam::GenerateAll(plain, doc->GetBodies());
  REQUIRE(plainPaths.size() == 1);
  REQUIRE(plainPaths[0].error.empty());
  CHECK(plainPaths[0].reliefs.empty());
  std::string plainG, error;
  REQUIRE(cam::PostProcess("mach3", plain, plainPaths, {}, plainG, &error));
  // The material standing at the pocket's corner: a round cutter following
  // a square ring leaves it. Read off the machined height map at a cell
  // just inside the corner -- the verifier's leftover count would not say,
  // because leftover is measured only where the tool went, and a corner
  // radius is exactly where it did not.
  const auto heightAt = [](const cam::Inspection& insp, double x, double y) {
    const int i = static_cast<int>((x - insp.machined.x0) / insp.machined.res);
    const int j = static_cast<int>((y - insp.machined.y0) / insp.machined.res);
    return static_cast<double>(insp.machined.At(i, j));
  };
  const cam::Inspection before =
      cam::Inspect(plainG, plain, doc->GetBodies(), 0.25);
  REQUIRE(before.ok);
  INFO("before: corner at " << heightAt(before, 15.4, 10.4));
  CHECK(heightAt(before, 15.4, 10.4) > 19.0); // uncut: the block's top

  cam::Setup boned = job(cam::CornerRelief::Dogbone);
  auto bonedPaths = cam::GenerateAll(boned, doc->GetBodies());
  REQUIRE(bonedPaths.size() == 1);
  REQUIRE(bonedPaths[0].error.empty());
  CHECK(bonedPaths[0].reliefs.size() == 4); // one per corner, told once
  // The bites go OUT of the pocket: some cut lies beyond the wall ring, past
  // the pocket's corner by up to the tool's radius.
  double farthest = 0.0;
  for (const cam::Move& m : bonedPaths[0].moves)
    if (m.kind == cam::Move::Kind::Cut)
      farthest = std::max(
          farthest, std::max(std::abs(m.to.x - 30.0) - 12.0,
                             std::abs(m.to.y - 20.0) - 7.0)); // ring half-sizes
  CHECK(farthest > 1.0);
  CHECK(farthest < 3.0 + 1e-6);
  std::string bonedG;
  REQUIRE(cam::PostProcess("mach3", boned, bonedPaths, {}, bonedG, &error));
  // Without being told, the verifier calls the bites a gouge -- they ARE
  // cuts into the model. Told, it does not, and it says how many corners.
  const cam::Verdict naive =
      cam::VerifyGcode(bonedG, boned, doc->GetBodies(), 0.25);
  CHECK(naive.maxGouge > 1.0);
  const cam::Inspection after = cam::Inspect(bonedG, boned, doc->GetBodies(),
                                             0.25, bonedPaths[0].reliefs);
  const cam::Verdict& told = after.verdict;
  INFO("told: gouge " << told.maxGouge << ", corner at "
                      << heightAt(after, 15.4, 10.4));
  CHECK(told.maxGouge < 0.4);
  CHECK(told.reliefs == 4);
  CHECK(heightAt(after, 15.4, 10.4) < 12.5); // the corner is cleared
  bool said = false;
  for (const cam::VerdictLine& line : cam::Explain(told, 0.4, 0.0, false))
    said = said ||
           line.text.find("4 inside corners relieved") != std::string::npos;
  CHECK(said);

  // A T-bone runs along a wall instead of the bisector: its bites lie on the
  // ring's edge lines, so they reach past the corner in ONE axis only.
  cam::Setup tboned = job(cam::CornerRelief::TBone);
  auto tPaths = cam::GenerateAll(tboned, doc->GetBodies());
  REQUIRE(tPaths.size() == 1);
  REQUIRE(tPaths[0].error.empty());
  CHECK(tPaths[0].reliefs.size() == 4);
  // Counted as distinct places, not per depth pass.
  std::set<std::pair<long, long>> bites;
  for (const cam::Move& m : tPaths[0].moves)
    if (m.kind == cam::Move::Kind::Cut &&
        (std::abs(m.to.x - 30.0) > 12.0 + 1e-6 ||
         std::abs(m.to.y - 20.0) > 7.0 + 1e-6)) {
      const bool onX = std::abs(std::abs(m.to.y - 20.0) - 7.0) < 1e-6;
      const bool onY = std::abs(std::abs(m.to.x - 30.0) - 12.0) < 1e-6;
      CHECK((onX || onY)); // every bite lies on a wall line
      bites.emplace(std::lround(m.to.x * 1000.0), std::lround(m.to.y * 1000.0));
    }
  CHECK(bites.size() == 4);

  // The value survives a save, and an unknown one is refused.
  const auto j = DocumentIO::CamSetupToJson(boned);
  CHECK(DocumentIO::CamSetupFromJson(j).operations[0].cornerRelief ==
        cam::CornerRelief::Dogbone);
  nlohmann::json bad = j;
  bad["operations"][0]["corner_relief"] = "vbone";
  CHECK_THROWS_WITH(DocumentIO::CamSetupFromJson(bad),
                    Catch::Matchers::ContainsSubstring("tbone"));
}

TEST_CASE("cam relief: an outside profile relieves the part's inner corner "
          "and nothing else",
          "[cam][relief]") {
  // An L: a 60 x 40 block with a 30 x 20 bite out of one corner.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               60.0, 40.0, 12.0));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<BoxFeature>("Bite", glm::vec3(30, 20, -1),
                                               40.0, 30.0, 20.0));
  const uint64_t bite = doc->GetFeature(1)->GetId();
  doc->AddFeature(
      std::make_unique<BooleanFeature>("L", block, bite, BooleanOp::Subtract));
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Outside;
  op.topZ = 13.0;
  op.bottomZ = -1.0;
  op.stepdown = 5.0;
  op.cornerRelief = cam::CornerRelief::Dogbone;
  setup.operations.push_back(op);
  auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0].error.empty());
  // The L has one inner corner, at (30, 20); the outer corners are the
  // cutter's to make sharp and get no bone.
  REQUIRE(paths[0].reliefs.size() == 1);
  CHECK_THAT(glm::distance(paths[0].reliefs[0].at, glm::dvec2(30.0, 20.0)),
             WithinAbs(3.0 * std::sqrt(2.0),
                       0.05)); // the path corner, r off each wall
}

// ---------------------------------------------------------------------------
// R5: lead arcs.

TEST_CASE("cam lead: a profile leaves the wall on a tangent arc",
          "[cam][lead]") {
  auto doc = BlockDoc(); // 40 x 30 x 20
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Outside;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 5.0; // several passes: a lead on each, the last one checked
  op.leadArcs = true;
  setup.operations.push_back(op);
  auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  // The last cutting moves before the final retract walk AWAY from the
  // block: their distance from the part's outline grows from the tool's
  // radius to twice it, and none of them comes closer than the radius.
  const auto& moves = paths[0].moves;
  REQUIRE(moves.size() > 12);
  REQUIRE(moves.back().kind == cam::Move::Kind::Rapid);
  const auto outlineDistance = [](const glm::dvec3& p) {
    const double dx = std::max({0.0 - p.x, p.x - 40.0, 0.0});
    const double dy = std::max({0.0 - p.y, p.y - 30.0, 0.0});
    return std::hypot(dx, dy);
  };
  const size_t end = moves.size() - 1;
  const double atWall = outlineDistance(moves[end - 9].to);
  const double offWall = outlineDistance(moves[end - 1].to);
  INFO("at wall " << atWall << ", off wall " << offWall);
  CHECK_THAT(atWall, WithinAbs(3.0, 0.05));
  CHECK_THAT(offWall, WithinAbs(6.0, 0.1));
  for (size_t i = end - 9; i < end; i++)
    CHECK(outlineDistance(moves[i].to) > 3.0 - 1e-6);
  // ...and it is still a clean cut: nothing gouged by the arc.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25);
  CHECK(v.maxGouge < 0.4);

  // Inside an opening with no room for the arc, refused in words.
  auto doc2 = std::make_unique<Document>();
  doc2->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                                60.0, 40.0, 12.0));
  const uint64_t block = doc2->GetFeature(0)->GetId();
  // A 10 mm slot: room for a 6 mm tool (offset 3 each side leaves 4), not
  // for the tool plus a 3 mm lead (the arc's end is 6 off the near wall and
  // the cutter reaches 9; with 3 to spare from the far wall that needs 12).
  doc2->AddFeature(std::make_unique<BoxFeature>("Slot", glm::vec3(10, 15, -1),
                                                40.0, 10.0, 20.0));
  const uint64_t slot = doc2->GetFeature(1)->GetId();
  doc2->AddFeature(std::make_unique<BooleanFeature>("Slotted", block, slot,
                                                    BooleanOp::Subtract));
  ElementRef wall;
  for (const Body& b : doc2->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::dvec3 o(0.0), n(0.0), u(0.0);
      if (b.shape.FacePlane(index, o, n, u) && std::abs(n.z) < 0.01f &&
          o.x > 9.0f && o.x < 51.0f && o.y > 14.0f && o.y < 26.0f)
        wall = ElementRef{b.createdBy, name};
    }
  REQUIRE(wall.IsSet());
  cam::Setup inside = BasicSetup();
  cam::Operation in;
  in.id = 2;
  in.type = cam::OpType::Profile;
  in.side = cam::Side::Inside;
  in.topZ = 13.0;
  in.bottomZ = -1.0;
  in.stepdown = 5.0;
  in.leadArcs = true;
  in.targets = {cam::Target{0, wall}};
  inside.operations.push_back(in);
  auto inPaths = cam::GenerateAll(inside, doc2->GetBodies());
  REQUIRE(inPaths.size() == 1);
  INFO(inPaths[0].error);
  CHECK(inPaths[0].error.find("lead arc") != std::string::npos);
  CHECK(inPaths[0].error.find("room") != std::string::npos);
  // Without the lead the same opening cuts.
  inside.operations[0].leadArcs = false;
  auto plainPaths = cam::GenerateAll(inside, doc2->GetBodies());
  REQUIRE(plainPaths.size() == 1);
  CHECK(plainPaths[0].error.empty());
  // The flag survives a save.
  const auto j = DocumentIO::CamSetupToJson(setup);
  CHECK(DocumentIO::CamSetupFromJson(j).operations[0].leadArcs);
}

// ---------------------------------------------------------------------------
// R6: rough then finish.

TEST_CASE("cam lead: every pass leaves the wall at one place, so the arcs "
          "stack",
          "[cam][lead]") {
  // The smart-speaker baffle, in miniature: a through opening under a flange
  // recess, cut from the recess floor's openings on tabs with lead-outs.
  // Chained pass to pass, each lead-out landed somewhere new in the slug and
  // slotted through everything from the stock top down to its pass -- on the
  // real board, 16 mm of MDF at the cutting feed, which moved the work.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Panel", glm::vec3(0, 0, 0),
                                               80.0, 80.0, 16.0));
  const uint64_t panel = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Recess", glm::vec3(40, 40, 13), glm::vec3(0, 0, 1), 32.0, 4.0));
  const uint64_t recess = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Recessed", panel, recess,
                                                   BooleanOp::Subtract));
  const uint64_t recessed = doc->GetFeature(2)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Opening", glm::vec3(40, 40, -1), glm::vec3(0, 0, 1), 25.0, 18.0));
  const uint64_t opening = doc->GetFeature(3)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Baffle", recessed, opening,
                                                   BooleanOp::Subtract));
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 7;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Inside;
  op.topZ = 16.0;
  op.bottomZ = -0.5;
  op.stepdown = 2.5;
  op.leadArcs = true;
  op.tabCount = 4;
  op.tabWidth = 12.0;
  op.tabHeight = 3.0;
  op.targets = {cam::Target{0, UpFaceAt(*doc, 13.0)}};
  REQUIRE(op.targets[0].ref.IsSet());
  setup.operations.push_back(op);
  auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  const auto& moves = paths[0].moves;

  // Every lap follows the OPENING, the top ones included. Above the recess
  // floor the only wall in the projection is the recess's, and the top pass
  // used to go round that instead -- in air -- leaving the next pass to take
  // two stepdowns at once. Tool centre on the opening: 25 - 3 = 22.
  double outermost = 0.0;
  for (const cam::Move& m : moves)
    if (m.kind != cam::Move::Kind::Rapid)
      outermost = std::max(outermost, std::hypot(m.to.x - 40.0, m.to.y - 40.0));
  CHECK(outermost < 22.05);

  // Where each pass's lead-out ends: the last cutting move before each
  // retract. All of them in one place.
  std::vector<glm::dvec2> ends;
  for (size_t k = 1; k < moves.size(); k++)
    if (moves[k].kind == cam::Move::Kind::Rapid &&
        moves[k - 1].kind != cam::Move::Kind::Rapid &&
        moves[k].to.z > moves[k - 1].to.z)
      ends.emplace_back(moves[k - 1].to.x, moves[k - 1].to.y);
  REQUIRE(ends.size() >= 6); // 16.5 mm in 2.5 mm passes
  for (const glm::dvec2& e : ends)
    CHECK(glm::length(e - ends.front()) < 0.05);
  // ...a radius into the slug, off the wall it left.
  CHECK_THAT(std::hypot(ends.front().x - 40.0, ends.front().y - 40.0),
             WithinAbs(std::hypot(19.0, 3.0), 0.05));

  // And the simulation agrees: nothing in this op bites deeper than a pass
  // or a tab exit asks of the cutter.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v =
      cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25, {}, paths[0].tabs);
  REQUIRE(v.biteByOp.count(7) == 1);
  INFO("bite " << v.biteByOp.at(7).first << " allowed "
               << v.biteByOp.at(7).second);
  CHECK_THAT(v.biteByOp.at(7).second, WithinAbs(3.0, 1e-9));
  CHECK(v.biteByOp.at(7).first < 3.0 * 1.25 + 0.2);
  CHECK(v.overBites == 0);
}

TEST_CASE("cam finish: one operation roughs with one tool and finishes with "
          "another",
          "[cam][finish]") {
  auto doc = BlockDoc(); // 40 x 30 x 20
  cam::Setup setup = BasicSetup();
  cam::Tool fine = setup.tools[0];
  fine.number = 2;
  fine.diameter = 3.0;
  setup.tools.push_back(fine);
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Outside;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 5.0;
  op.finishTool = 2;
  op.finishStock = cam::DefaultFinishStock(setup.tools[0]); // 0.6
  setup.operations.push_back(op);

  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 2); // two toolpaths from one operation
  CHECK(paths[0].operationId == 1);
  CHECK(paths[1].operationId == 1);
  CHECK(paths[0].toolNumber == 1);
  CHECK(!paths[0].finish);
  CHECK(paths[1].toolNumber == 2);
  CHECK(paths[1].finish);
  INFO(paths[1].error);
  CHECK(paths[1].error.empty());
  const auto outlineDistance = [](const glm::dvec3& p) {
    const double dx = std::max({0.0 - p.x, p.x - 40.0, 0.0});
    const double dy = std::max({0.0 - p.y, p.y - 30.0, 0.0});
    return std::hypot(dx, dy);
  };
  // The rough never comes nearer the final wall than its radius plus what
  // it leaves; the finish runs at exactly its own radius from the wall,
  // in ONE pass at the full depth.
  double roughNearest = 1e9, finishNearest = 1e9, finishFarthest = 0.0;
  double finishLowest = 1e9, finishHighest = -1e9;
  for (const cam::Move& m : paths[0].moves)
    if (m.kind == cam::Move::Kind::Cut)
      roughNearest = std::min(roughNearest, outlineDistance(m.to));
  for (const cam::Move& m : paths[1].moves)
    if (m.kind == cam::Move::Kind::Cut) {
      finishNearest = std::min(finishNearest, outlineDistance(m.to));
      finishFarthest = std::max(finishFarthest, outlineDistance(m.to));
      finishLowest = std::min(finishLowest, m.to.z);
      finishHighest = std::max(finishHighest, m.to.z);
    }
  INFO("rough nearest " << roughNearest << ", finish " << finishNearest << ".."
                        << finishFarthest);
  CHECK(roughNearest > 3.0 + 0.6 - 0.02); // within the offset's chord tolerance
  CHECK_THAT(finishNearest, WithinAbs(1.5, 1e-3));
  CHECK(finishFarthest < 1.5 + 0.01); // no corner rounding beyond the tool
  CHECK_THAT(finishLowest, WithinAbs(-1.0, 1e-6));
  CHECK(finishHighest <= 21.0 + 1e-6);
  // Together: a clean part.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover << " at "
                << v.leftoverAt.x << "," << v.leftoverAt.y << " regions "
                << v.leftoverRegions << " area " << v.leftoverArea);
  CHECK(v.maxGouge < 0.4);
  CHECK(v.maxLeftover < 0.5); // the rough's 0.6 is gone

  // A file per tool splits the two: tool order sorts TOOLPATHS.
  setup.splitOutput = true;
  const auto ordered = cam::GenerateAll(setup, doc->GetBodies());
  std::vector<cam::PostFile> files;
  REQUIRE(cam::PostProcessSplit("mach3", setup, ordered, {}, files, &error));
  REQUIRE(files.size() == 2);
  CHECK(files[0].tool == 1);
  CHECK(files[1].tool == 2);

  // A finishing tool the setup does not have is said, on the finish path.
  cam::Setup missing = setup;
  missing.operations[0].finishTool = 9;
  const auto bad = cam::GenerateAll(missing, doc->GetBodies());
  REQUIRE(bad.size() == 2);
  CHECK(bad[1].error.find("finishing tool 9") != std::string::npos);

  // The fields survive a save.
  const auto j = DocumentIO::CamSetupToJson(setup);
  const cam::Setup back = DocumentIO::CamSetupFromJson(j);
  CHECK(back.operations[0].finishTool == 2);
  CHECK_THAT(back.operations[0].finishStock, WithinAbs(0.6, 1e-9));
}

TEST_CASE("cam profile: cutting INSIDE a face cuts its openings", "[cam]") {
  // "Cut inside this face" can only mean its openings. It used to mean the
  // face's OUTER wire offset inward -- a trench a whisker inside the part,
  // full depth, all the way round, which is the failure the strategy's own
  // refusal describes and which this pick walked straight into, because the
  // refusal fires only when the contour list is EMPTY and a face pick always
  // handed back one contour.
  auto plate = std::make_unique<Document>();
  plate->AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 40, 5));
  uint64_t solid = plate->GetFeature(0)->GetId();
  const glm::dvec2 slots[2] = {{15.0, 20.0}, {45.0, 20.0}};
  for (const glm::dvec2& at : slots) {
    plate->AddFeature(std::make_unique<BoxFeature>(
        "Slot", glm::vec3(at.x - 4.0, at.y - 4.0, -1.0), 8.0, 8.0, 7.0));
    const uint64_t voidId =
        plate->GetFeature(plate->FeatureCount() - 1)->GetId();
    plate->AddFeature(std::make_unique<BooleanFeature>("Cut", solid, voidId,
                                                       BooleanOp::Subtract));
    solid = plate->GetFeature(plate->FeatureCount() - 1)->GetId();
  }
  const auto topFace = [](const Document& doc, double z) {
    ElementRef out;
    for (const Body& b : doc.GetBodies())
      for (const auto& [index, name] : b.elementMap.names) {
        glm::dvec3 o(0.0), n(0.0), u(0.0);
        if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f &&
            std::abs(o.z - z) < 1e-3)
          out = ElementRef{b.createdBy, name};
      }
    return out;
  };
  const ElementRef top = topFace(*plate, 5.0);
  REQUIRE(top.IsSet());

  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Inside;
  op.topZ = 5.0;
  op.bottomZ = -1.0;
  op.stepdown = 3.0;
  op.targets = {cam::Target{0, top}};
  setup.operations.push_back(op);
  const auto paths = cam::GenerateAll(setup, plate->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());

  // Every cutting move belongs to one of the two openings, and none of them
  // goes near the part's own edge -- which is where the trench used to be.
  int near[2] = {0, 0};
  for (const cam::Move& m : paths[0].moves) {
    if (m.kind == cam::Move::Kind::Rapid)
      continue;
    const glm::dvec2 at(m.to.x, m.to.y);
    const double d0 = glm::distance(at, slots[0]);
    const double d1 = glm::distance(at, slots[1]);
    INFO("cut at " << at.x << ", " << at.y);
    CHECK(std::min(d0, d1) < 6.0);
    near[d0 < d1 ? 0 : 1]++;
  }
  CHECK(near[0] > 0); // both openings, in ONE operation
  CHECK(near[1] > 0);

  // A face with nothing in it says what to pick instead of trenching.
  auto blank = BlockDoc(40, 30, 10);
  cam::Setup none = BasicSetup();
  cam::Operation solidOp = op;
  solidOp.topZ = 10.0;
  solidOp.targets = {cam::Target{0, topFace(*blank, 10.0)}};
  none.operations.push_back(solidOp);
  const auto refused = cam::GenerateAll(none, blank->GetBodies());
  REQUIRE(refused.size() == 1);
  INFO(refused[0].error);
  CHECK(refused[0].error.find("nothing to cut in that face") !=
        std::string::npos);
  CHECK(refused[0].moves.empty());

  // A BOSS has an inner wire too, and it is not an opening: cutting round the
  // inside of one removes the boss. The same test `useOutline` uses to tell
  // them apart -- is anything standing in it? -- decides here.
  auto flange = FlangeDoc(10.0, /*boss=*/true);
  cam::Setup floorSetup = BasicSetup();
  cam::Operation onFloor = op;
  onFloor.topZ = 12.0;
  onFloor.bottomZ = 10.0;
  onFloor.targets = {cam::Target{0, topFace(*flange, 12.0)}};
  floorSetup.operations.push_back(onFloor);
  const auto floorPaths = cam::GenerateAll(floorSetup, flange->GetBodies());
  REQUIRE(floorPaths.size() == 1);
  INFO(floorPaths[0].error);
  REQUIRE(floorPaths[0].error.empty());
  for (const cam::Move& m : floorPaths[0].moves) {
    if (m.kind == cam::Move::Kind::Rapid)
      continue;
    // The bore at (60, 45) is an opening; the boss at (44, 34) is not.
    INFO("cut at " << m.to.x << ", " << m.to.y);
    CHECK(glm::distance(glm::dvec2(m.to.x, m.to.y), glm::dvec2(60.0, 45.0)) <
          6.0);
  }
}

TEST_CASE("cam drill: a through hole breaks out by the point, whatever shape "
          "the point is",
          "[cam][drill]") {
  // The clearance a through hole needs is a fact about the CUTTER, so it is
  // read off the same TipHeight the cut and the verifier use rather than
  // typed here: zero for a flat mill (it cuts flat, so its lowest point IS
  // its diameter), the cone for a drill or a V-bit, the radius for a ball.
  auto doc = HoleDoc(8.0); // a 20 mm block, drilled through
  const ElementRef wall = HoleWall(*doc);
  struct Shape {
    const char* what;
    cam::ToolType type;
    double diameter;
    double angle;
    double breakout; // how far past the far face the tool must go
  };
  const Shape shapes[] = {
      {"flat", cam::ToolType::Flat, 6.0, 0.0, 0.0},
      {"drill 118 deg", cam::ToolType::Drill, 6.0, 118.0,
       3.0 / std::tan(glm::radians(59.0))},
      {"v-bit 90 deg", cam::ToolType::VBit, 6.0, 90.0, 3.0},
      {"ball", cam::ToolType::Ball, 6.0, 0.0, 3.0},
  };
  for (const Shape& shape : shapes) {
    INFO(shape.what);
    cam::Setup setup = BasicSetup();
    setup.tools[0].type = shape.type;
    setup.tools[0].diameter = shape.diameter;
    setup.tools[0].tipAngleDeg = shape.angle;
    const cam::DerivedOp d = cam::DeriveOperation(
        setup, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, wall});
    REQUIRE(d.ok);
    // Top from the STOCK, not the part: entry rapids belong above the
    // material. Bottom from the stock's far face (the 1 mm margin under the
    // part is drilled through, as a cut-out goes through it), plus the point.
    CHECK_THAT(d.op.topZ, WithinAbs(21.0, 1e-6));
    CHECK_THAT(d.op.bottomZ, WithinAbs(-1.0 - shape.breakout, 1e-6));
  }

  // A BLIND hole keeps its floor: there the point's cone is the bottom of the
  // hole, and going deeper would be cutting past what was asked for.
  auto blind = std::make_unique<Document>();
  blind->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                                 40.0, 30.0, 20.0));
  const uint64_t block = blind->GetFeature(0)->GetId();
  blind->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::vec3(20, 15, 12), glm::vec3(0, 0, 1), 4.0, 10.0));
  const uint64_t hole = blind->GetFeature(1)->GetId();
  blind->AddFeature(std::make_unique<BooleanFeature>("Drilled", block, hole,
                                                     BooleanOp::Subtract));
  cam::Setup setup = BasicSetup();
  setup.tools[0].type = cam::ToolType::Drill;
  setup.tools[0].tipAngleDeg = 118.0;
  const cam::DerivedOp d =
      cam::DeriveOperation(setup, blind->GetBodies(), cam::OpType::Drill,
                           cam::Target{0, HoleWall(*blind)});
  REQUIRE(d.ok);
  CHECK_THAT(d.op.bottomZ, WithinAbs(12.0, 1e-6));

  // And it does not depend on the WORKPIECE. "Reaches the bottom of the part"
  // used to be measured from the stock's bottom plus its bottom margin, which
  // recovers the part's bottom only for relative stock; on a fixed sheet it
  // landed a millimetre high, and with a zero margin it landed exactly on the
  // model's bottom -- where two OCC bounding boxes differ by a few microns and
  // that decided it. A sheet job's holes stopped breaking out, silently.
  for (const double bottomMargin : {0.0, 1.0, 3.0}) {
    INFO("bottom margin " << bottomMargin);
    cam::Setup sheet = BasicSetup();
    sheet.tools[0].type = cam::ToolType::Drill;
    sheet.tools[0].tipAngleDeg = 118.0;
    sheet.stock.mode = cam::StockMode::Fixed;
    sheet.stock.size = {300.0, 200.0, 20.0};
    sheet.stock.bottomMargin = bottomMargin;
    const cam::DerivedOp on = cam::DeriveOperation(
        sheet, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, wall});
    REQUIRE(on.ok);
    CHECK_THAT(on.op.bottomZ,
               WithinAbs(-3.0 / std::tan(glm::radians(59.0)), 1e-3));
  }
}

#endif // HAVE_CAM
