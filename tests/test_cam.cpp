#ifdef HAVE_CAM

// CAM's core toolpath generation: sections, depth passes, profiles,
// pockets, tabs, adaptive clearing and the arcs they post as.
// One of five files split from test_cam.cpp
// (docs/architecture.md#source-layout).

#include "cam_fixtures.h"

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
  // Held to the plunge feed only where it goes DOWN (EntryFeed): its
  // vertical speed is at most the plunge feed, its feed at most the cutting
  // feed, and the rest of the ramp, at 3 degrees, runs at the cutting feed.
  {
    const glm::dvec3 d = tp.moves[2].to - tp.moves[1].to;
    CHECK(tp.moves[2].feed * -d.z / glm::length(d) <= 200.0 + 1e-9);
    CHECK(tp.moves[2].feed <= 600.0 + 1e-9);
  }
  CHECK_THAT(tp.moves[3].feed, WithinRel(600.0, 1e-9));
  // No vertical plunge anywhere in a profile.
  for (const cam::Move& m : tp.moves)
    CHECK(m.kind != cam::Move::Kind::Plunge);
}

TEST_CASE("cam: an inside profile with nothing to cut inside of refuses",
          "[cam]") {
  // This used to offset the PART'S OWN outline inward by a tool radius and
  // cut a trench a whisker inside the perimeter, full depth, all the way
  // round — a 15 mm gouge, found by asking for a slot this way in the first
  // dogfooding round (docs/cam/operations.md#derivation--what-a-click-means).
  // An inside profile
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
  // for (docs/cam/operations.md#profile).
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

TEST_CASE("cam: coming down off a tab is a PLUNGE, at the plunge feed",
          "[cam]") {
  // The vertical move at a tab's far edge went in as a Cut at the CUTTING
  // feed: the cutter is buried and driving straight down into the material,
  // which is the one move the tool's plunge feed exists to govern. On the
  // speaker baffle that was ten 3 mm descents at 1200 mm/min where the tool
  // asks for 400.
  auto doc = BlockDoc(); // 40 x 30 x 20
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 9;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 5.0;
  op.side = cam::Side::Outside;
  op.tabCount = 4;
  op.tabWidth = 6.0;
  op.tabHeight = 3.0;
  setup.operations.push_back(op);
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  const cam::Tool* tool = setup.FindTool(1);
  REQUIRE(tool != nullptr);
  REQUIRE(tool->feedPlunge < tool->feedCut); // or this test proves nothing

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  INFO(tp.error);
  REQUIRE(tp.error.empty());

  int descents = 0;
  for (size_t i = 1; i < tp.moves.size(); i++) {
    const cam::Move& a = tp.moves[i - 1];
    const cam::Move& b = tp.moves[i];
    if (b.kind == cam::Move::Kind::Rapid)
      continue;
    const bool vertical =
        std::abs(b.to.x - a.to.x) < 1e-9 && std::abs(b.to.y - a.to.y) < 1e-9;
    if (!vertical || b.to.z >= a.to.z - 1e-9)
      continue;
    descents++;
    INFO("descent " << descents << " from z " << a.to.z << " to " << b.to.z);
    CHECK(b.kind == cam::Move::Kind::Plunge);
    CHECK_THAT(b.feed, WithinAbs(tool->feedPlunge, 1e-9));
  }
  // Four tabs, so at least four exits off one.
  CHECK(descents >= 4);
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
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{0, wall});
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

  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, wall});
  INFO(derived.why);
  REQUIRE(derived.ok);
  // Top from the STOCK (a typed part-top once sent entry rapids through the
  // millimetre of stock above the part); bottom from the hole's own extent,
  // which for a through hole goes on through the 1 mm of stock below.
  CHECK_THAT(derived.op.topZ, WithinAbs(21.0, 1e-9));
  CHECK_THAT(derived.op.bottomZ, WithinAbs(-1.0, 1e-4)); // OCC bbox gap
  REQUIRE(derived.op.targets.size() == 1);
  CHECK(derived.op.targets[0].ref.name == wall.name);

  // No target is a teaching refusal, not "not a verb" — that message fired
  // even when the caller HAD passed cylindrical targets.
  const cam::DerivedOp none = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Drill, cam::Target{});
  REQUIRE_FALSE(none.ok);
  CHECK(none.why.find("pass targets") != std::string::npos);

  // A flat face is refused as the wrong KIND, naming where to look.
  const cam::DerivedOp flat =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Drill,
                           cam::Target{0, UpFaceAt(*doc, 20.0)});
  REQUIRE_FALSE(flat.ok);
  CHECK(flat.why.find("cylind") != std::string::npos);
}

TEST_CASE("cam: clicking a pocket floor derives every depth", "[cam]") {
  auto doc = PocketDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());

  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Pocket, cam::Target{0, floor});
  INFO(derived.why);
  REQUIRE(derived.ok);
  // Nothing here was typed: the floor is the face, the top is the stock, the
  // stepdown is half the cutter. This is the `2 -> -0` failure made
  // unexpressible (docs/cam/operations.md#derivation--what-a-click-means).
  CHECK_THAT(derived.op.bottomZ, WithinAbs(12.0, 1e-9));
  CHECK_THAT(derived.op.topZ, WithinAbs(21.0, 1e-9)); // part top + 1 mm stock
  CHECK_THAT(derived.op.stepdown, WithinAbs(3.0, 1e-9)); // half of a 6 mm tool
  REQUIRE(derived.op.targets.size() == 1);
  CHECK(derived.op.targets[0].ref.name == floor.name);

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
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket,
                           cam::Target{0, floor})
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
  op.targets.push_back(cam::Target{0, floor});

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

// A notch OPEN at one end: its floor's edge there has nothing standing above
// the floor beside it, so the cutter may run over it. Every edge treated as a
// wall, the queen's crown notch -- 3.4 wide, its floor 2.3 long before the
// dome fell away -- was refused for a 3 mm cutter as "2.3 mm across". The
// same notch walled at both ends still is: the growth is only where the
// floor's edge is open.
TEST_CASE("cam: a pocket's open edge does not hold the cutter back", "[cam]") {
  const auto notched = [](double endWall) {
    auto doc = std::make_unique<Document>();
    doc->AddFeature(
        std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
    const uint64_t block = doc->GetFeature(0)->GetId();
    // 2.3 mm long along X, 3.4 wide, down to z = 12; with endWall = 0 it
    // runs out of the block's +X face.
    doc->AddFeature(std::make_unique<BoxFeature>(
        "Notch", glm::vec3(37.7f - static_cast<float>(endWall), 13.0f, 12.0f),
        2.3 + (endWall > 0.0 ? 0.0 : 5.0), 3.4, 20));
    const uint64_t notch = doc->GetFeature(1)->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("Cut", block, notch,
                                                     BooleanOp::Subtract));
    return doc;
  };
  cam::Setup setup = BasicSetup();
  cam::Tool three;
  three.number = 2;
  three.diameter = 3.0;
  setup.tools.push_back(three);
  cam::Operation op;
  op.id = 11;
  op.type = cam::OpType::Pocket;
  op.toolNumber = 2;
  op.topZ = 20.0;
  op.bottomZ = 12.0;

  auto open = notched(0.0);
  op.targets = {cam::Target{0, UpFaceAt(*open, 12.0)}};
  REQUIRE(op.targets[0].ref.IsSet());
  const cam::Toolpath cut = cam::Generate(setup, op, open->GetBodies());
  INFO(cut.error);
  REQUIRE_FALSE(cut.Empty());
  // ...and it stays inside the notch: nothing gouged into its walls.
  cam::Setup job = setup;
  job.operations = {op};
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", job, {cut}, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, job, open->GetBodies(), 0.1);
  INFO("gouge " << v.maxGouge << " at " << v.gougeAt.x << "," << v.gougeAt.y);
  CHECK(v.maxGouge < 0.06);

  auto walled = notched(1.0); // a wall 1 mm thick left at the end
  op.targets = {cam::Target{0, UpFaceAt(*walled, 12.0)}};
  REQUIRE(op.targets[0].ref.IsSet());
  const cam::Toolpath refused = cam::Generate(setup, op, walled->GetBodies());
  CHECK(refused.Empty());
  CHECK(refused.error.find("2.3 mm across") != std::string::npos);
}

// The chess bishop's first mitre: a slanted blind slot, drawn as four
// corners -- two ends 3.2 mm long, and sides running 14 mm into the head.
// The ends were square to the direction the script MEANT, not the one it
// drew, so they lie 18 degrees off the sides instead of square to them and
// the slot is 3.2 * sin 18 = 0.99 mm wide. "0.99 mm across" was the truth.
// The slot also runs out of the part, and once a floor's open edges grew by
// the cutter's radius the region a pocket clears is wider there than the
// floor, in air, by an amount the cutter sets: measured on that, the same
// slot read "1.57 mm across" for a 3 mm cutter. The width quoted is the
// floor's own.
TEST_CASE("cam: a slot too thin for the cutter is measured on its floor "
          "and not on the air past its open end",
          "[cam]") {
  const auto slotted = [](double lean) {
    auto doc = std::make_unique<Document>();
    doc->AddFeature(
        std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
    const double ang = 54.0 * std::numbers::pi / 180.0;
    // Along the slot, from outside the block's +X face inward.
    const glm::dvec2 along(-std::cos(ang), -std::sin(ang));
    // Across it: square to `along`, then turned by `lean`.
    const double c = std::cos(lean), s = std::sin(lean);
    const glm::dvec2 across(-along.y * c - along.x * s,
                            along.x * c - along.y * s);
    const glm::dvec2 a(44.0, 20.0);
    const glm::dvec2 b = a + 14.0 * along;
    const double w = 1.6; // half of each 3.2 mm end
    const glm::dvec2 k[4] = {a + w * across, b + w * across, b - w * across,
                             a - w * across};
    // On the block's top, cut 8 mm down: a floor at z = 12.
    auto sk = std::make_unique<SketchFeature>(
        "Mitre sketch",
        SketchPlane(glm::vec3(0.0f, 0.0f, 20.0f), glm::vec3(0.0f, 0.0f, 1.0f),
                    glm::vec3(1.0f, 0.0f, 0.0f)));
    for (int i = 0; i < 4; i++)
      sk->AddEntity(sketch::Line{glm::vec2(k[i]), glm::vec2(k[(i + 1) % 4])});
    doc->AddFeature(std::move(sk));
    const uint64_t sid = doc->GetFeature(1)->GetId();
    doc->AddFeature(std::make_unique<ExtrudeFeature>("Mitre", sid, 8.0,
                                                     ExtrudeMode::Cut, true));
    return doc;
  };
  cam::Setup setup = BasicSetup();
  cam::Tool three;
  three.number = 5;
  three.diameter = 3.0;
  setup.tools.push_back(three);
  cam::Operation op;
  op.id = 15;
  op.type = cam::OpType::Pocket;
  op.toolNumber = 5;
  op.topZ = 20.0;
  op.bottomZ = 12.0;

  // As drawn: the ends turned 72 degrees from square, 18 off the sides.
  const double lean = 72.0 * std::numbers::pi / 180.0;
  auto drawn = slotted(lean);
  op.targets = {cam::Target{0, UpFaceAt(*drawn, 12.0)}};
  REQUIRE(op.targets[0].ref.IsSet());
  for (const cam::OpType type : {cam::OpType::Pocket, cam::OpType::Adaptive}) {
    op.type = type;
    const cam::Toolpath refused = cam::Generate(setup, op, drawn->GetBodies());
    INFO(refused.error);
    CHECK(refused.Empty());
    CHECK(refused.error.find("0.99 mm across") != std::string::npos);
    CHECK(refused.error.find("cannot reach in") != std::string::npos);
  }
  // ...and 0.99 is the width a cutter meets: a 0.8 mm one goes in.
  cam::Tool fine;
  fine.number = 6;
  fine.diameter = 0.8;
  setup.tools.push_back(fine);
  op.type = cam::OpType::Pocket;
  op.toolNumber = 6;
  const cam::Toolpath thin = cam::Generate(setup, op, drawn->GetBodies());
  INFO(thin.error);
  CHECK_FALSE(thin.Empty());

  // Drawn square, the same 3.2 mm ends make a 3.2 mm slot, and the 3 mm
  // cutter clears it.
  auto square = slotted(0.0);
  op.targets = {cam::Target{0, UpFaceAt(*square, 12.0)}};
  REQUIRE(op.targets[0].ref.IsSet());
  op.toolNumber = 5;
  const cam::Toolpath cut = cam::Generate(setup, op, square->GetBodies());
  INFO(cut.error);
  CHECK_FALSE(cut.Empty());
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
  op.targets.push_back(cam::Target{0, floor});

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
  op.targets.push_back(cam::Target{0, floor});

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
      glm::dvec3 c(0.0), n(0.0), u(0.0);
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
    const cam::DerivedOp asPocket = cam::DeriveOperation(
        probe, doc.GetBodies(), cam::OpType::Pocket, cam::Target{0, wall});
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
  op.targets.push_back(cam::Target{0, wall});
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
      glm::dvec3 c(0.0), n(0.0), u(0.0);
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
      glm::dvec3 c(0.0), n(0.0), u(0.0);
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
      glm::dvec3 c(0.0), n(0.0), u(0.0);
      if (b.shape.FacePlane(index, c, n, u))
        continue; // planar
      op.targets.push_back(cam::Target{0, ElementRef{b.createdBy, name}});
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
  op.targets.push_back(cam::Target{0, wall});

  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  REQUIRE(tp.Empty());
  INFO(tp.error);
  CHECK(tp.error.find("opening") != std::string::npos);
  CHECK(tp.error.find("5 mm across") != std::string::npos);
  CHECK(tp.error.find("depths") == std::string::npos);
}

TEST_CASE("cam: a face that points down is not a floor to cut to", "[cam]") {
  // A ceiling is level too, and was pocketed as if the cutter went through
  // the part to reach it -- a rotary mitre indexed the wrong way round.
  auto doc = PocketDoc(/*island=*/false);
  ElementRef under;
  for (const Body& b : doc->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::dvec3 c(0.0), n(0.0), u(0.0);
      if (b.shape.FacePlane(index, c, n, u) && n.z < -0.99f)
        under = ElementRef{b.createdBy, name};
    }
  REQUIRE(under.IsSet());
  const cam::FaceRegion region = cam::PickedFaceRegion(doc->GetBodies(), under);
  CHECK_FALSE(region.ok);
  CHECK(region.why.find("points down") != std::string::npos);
}

TEST_CASE("cam: an island in the picked floor is machined around",
          "[cam][slow]") {
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
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket,
                           cam::Target{0, floor})
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
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket,
                           cam::Target{0, floor})
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
                           cam::Target{0, UpFaceAt(*wide, 12.0)})
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
  const cam::DerivedOp none =
      cam::DeriveOperation(toolless, doc->GetBodies(), cam::OpType::Pocket,
                           cam::Target{0, UpFaceAt(*doc, 12.0)});
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
    const cam::DerivedOp d = cam::DeriveOperation(
        s, doc->GetBodies(), cam::OpType::Pocket, cam::Target{0, floor});
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

TEST_CASE("cam: an entry holds only its vertical speed to the plunge feed",
          "[cam]") {
  cam::Tool t;
  t.feedCut = 1200.0;
  t.feedPlunge = 400.0;
  const glm::dvec3 o(0.0);
  // Level, and rising: the cutting feed. Straight down: the plunge feed.
  CHECK_THAT(cam::EntryFeed(t, o, {10, 0, 0}), WithinRel(1200.0, 1e-12));
  CHECK_THAT(cam::EntryFeed(t, o, {10, 0, 2}), WithinRel(1200.0, 1e-12));
  CHECK_THAT(cam::EntryFeed(t, o, {0, 0, -5}), WithinRel(400.0, 1e-12));
  // A 3 degree ramp comes down at 1200 sin 3 = 63 mm/min at the cutting
  // feed, far under the plunge feed: the cutting feed it is.
  const double a3 = glm::radians(3.0);
  CHECK_THAT(cam::EntryFeed(t, o, {std::cos(a3), 0, -std::sin(a3)}),
             WithinRel(1200.0, 1e-12));
  // At 60 degrees the vertical part governs: plunge / sin 60.
  const double a60 = glm::radians(60.0);
  CHECK_THAT(cam::EntryFeed(t, o, {std::cos(a60), 0, -std::sin(a60)}),
             WithinRel(400.0 / std::sin(a60), 1e-12));
}

TEST_CASE("cam: a closed profile stays down from pass to pass", "[cam]") {
  // With no lead-out to leave by, a pass that lifts only climbs out and
  // feeds back down the slot it just cut -- two minutes of the
  // smart-speaker baffle's screw holes. The next pass ramps on instead.
  auto doc = BlockDoc(); // 40 x 30 x 20
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Outside;
  op.topZ = 20.0;
  op.bottomZ = 0.0;
  op.stepdown = 5.0;
  setup.operations.push_back(op);
  const cam::Toolpath tp = cam::Generate(setup, op, doc->GetBodies());
  REQUIRE(tp.error.empty());
  size_t firstCut = 0, lastCut = 0;
  for (size_t i = 0; i < tp.moves.size(); i++)
    if (tp.moves[i].kind != cam::Move::Kind::Rapid) {
      if (firstCut == 0)
        firstCut = i;
      lastCut = i;
    }
  int rapidsBetween = 0;
  for (size_t i = firstCut; i < lastCut; i++)
    rapidsBetween += tp.moves[i].kind == cam::Move::Kind::Rapid;
  CHECK(rapidsBetween == 0);
  CHECK_THAT(tp.MinZ(), WithinAbs(0.0, 1e-9));
  CHECK_THAT(tp.moves.back().to.z, WithinAbs(setup.safeZ, 1e-9));

  // With a lead-out it still lifts after every pass, arcs stacked.
  setup.operations[0].leadArcs = true;
  const cam::Toolpath led =
      cam::Generate(setup, setup.operations[0], doc->GetBodies());
  int lifts = 0;
  for (const cam::Move& m : led.moves)
    lifts += m.kind == cam::Move::Kind::Rapid && m.to.z >= setup.safeZ - 1e-9;
  CHECK(lifts >= 4); // one per pass, and the approach
}

TEST_CASE("cam: an annular pocket is cleared inside out, walls last", "[cam]") {
  // The baffle's W170S recess in miniature: a flange recess round a through
  // opening, a band of tool centres 14 mm wide. Cut level by level it
  // alternated between its two edges and re-entered by ramp on every ring,
  // walls first, and its last ring swept only what its neighbours had.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Panel", glm::vec3(0, 0, 0),
                                               110.0, 110.0, 16.0));
  const uint64_t panel = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Recess", glm::vec3(55, 55, 13), glm::vec3(0, 0, 1), 45.0, 4.0));
  const uint64_t recess = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Recessed", panel, recess,
                                                   BooleanOp::Subtract));
  const uint64_t recessed = doc->GetFeature(2)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Opening", glm::vec3(55, 55, -1), glm::vec3(0, 0, 1), 25.0, 18.0));
  const uint64_t opening = doc->GetFeature(3)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Baffle", recessed, opening,
                                                   BooleanOp::Subtract));
  cam::Setup s = BasicSetup();
  s.stock.margin = glm::dvec3(0.0);
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Pocket;
  op.side = cam::Side::Inside;
  op.topZ = 16.0;
  op.bottomZ = 13.0;
  op.stepdown = 1.5;
  op.stepover = 0.45;
  op.targets = {cam::Target{0, UpFaceAt(*doc, 13.0)}};
  REQUIRE(op.targets[0].ref.IsSet());
  s.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());

  // The rings of each depth, by tool-centre radius, in the order cut.
  std::map<double, std::vector<double>> rings; // z -> radii
  for (const cam::Move& m : paths[0].moves) {
    if (m.kind == cam::Move::Kind::Rapid)
      continue;
    const double r =
        std::round(std::hypot(m.to.x - 55.0, m.to.y - 55.0) * 10) / 10;
    auto& seq = rings[m.to.z];
    // A lap is many moves at one radius; a link between rings is one short
    // move, whose end is already on the next ring.
    if (seq.empty() || std::abs(seq.back() - r) > 0.5)
      seq.push_back(r);
  }
  const auto isRing = [](double r) { return r > 27.5 && r < 42.5; };
  for (const double z : {14.5, 13.0}) {
    INFO("depth " << z);
    REQUIRE(rings.count(z) == 1);
    std::vector<double> seq;
    for (const double r : rings[z])
      if (isRing(r) && (seq.empty() || std::abs(seq.back() - r) > 0.5))
        seq.push_back(r);
    std::string said;
    for (const double r : seq)
      said += std::to_string(r) + " ";
    INFO("rings cut: " << said);
    // Five rings, not six: of the two that meet in the middle of the band,
    // the one its neighbours already sweep is not cut.
    std::set<double> distinct(seq.begin(), seq.end());
    CHECK(distinct.size() == 5);
    // Walls last on their side: the outer wall (tool centre at 42) after
    // every ring outside the middle, the inner one (28) after every ring
    // inside it.
    const auto at = [&](double r) {
      return std::find_if(seq.begin(), seq.end(),
                          [&](double x) { return std::abs(x - r) < 0.2; }) -
             seq.begin();
    };
    REQUIRE(at(42.0) < static_cast<long>(seq.size()));
    REQUIRE(at(28.0) < static_cast<long>(seq.size()));
    CHECK(at(39.3) < at(42.0));
    CHECK(at(30.7) < at(28.0));
    // It starts in the middle of the band, not at a wall.
    CHECK(std::abs(seq.front() - 35.0) < 2.0);
  }
  // Two entries per depth (one per side of the band), not one per ring.
  int lifts = 0;
  for (const cam::Move& m : paths[0].moves)
    lifts += m.kind == cam::Move::Kind::Rapid && m.to.z >= s.safeZ - 1e-9;
  CHECK(lifts <= 2 * 2 * 2 + 1);

  // And it is still the same pocket: nothing gouged, nothing bitten deeper
  // than a pass -- the middle ring, first now, is a full-width slot at the
  // stepdown, which is exactly the allowance.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", s, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, s, doc->GetBodies(), 0.25);
  CHECK(v.maxGouge < 0.05);
  CHECK(v.overBites == 0);
  // ...and nothing left standing in the band. Dropping a ring is only
  // right when its neighbours sweep ALL of it: at a coarser threshold the
  // baffle kept a hair of MDF 3 mm tall between two rings 0.05 mm apart.
  INFO("leftover " << v.maxLeftover << " at " << v.leftoverAt.x << ","
                   << v.leftoverAt.y);
  CHECK(v.maxLeftover < 0.05);

  // The case that needs the ring: a band 14.15 mm wide, as on the baffle,
  // where the two rings either side of the middle stop 0.05 mm apart. The
  // middle ring adds only that, and it has to be cut.
  auto doc2 = std::make_unique<Document>();
  doc2->AddFeature(std::make_unique<BoxFeature>("Panel", glm::vec3(0, 0, 0),
                                                110.0, 110.0, 16.0));
  const uint64_t panel2 = doc2->GetFeature(0)->GetId();
  doc2->AddFeature(std::make_unique<CylinderFeature>(
      "Recess", glm::vec3(55, 55, 13), glm::vec3(0, 0, 1), 45.15, 4.0));
  doc2->AddFeature(std::make_unique<BooleanFeature>(
      "Recessed", panel2, doc2->GetFeature(1)->GetId(), BooleanOp::Subtract));
  doc2->AddFeature(std::make_unique<CylinderFeature>(
      "Opening", glm::vec3(55, 55, -1), glm::vec3(0, 0, 1), 25.0, 18.0));
  doc2->AddFeature(std::make_unique<BooleanFeature>(
      "Baffle", doc2->GetFeature(2)->GetId(), doc2->GetFeature(3)->GetId(),
      BooleanOp::Subtract));
  cam::Setup s2 = s;
  s2.operations[0].targets = {cam::Target{0, UpFaceAt(*doc2, 13.0)}};
  REQUIRE(s2.operations[0].targets[0].ref.IsSet());
  const std::vector<cam::Toolpath> paths2 =
      cam::GenerateAll(s2, doc2->GetBodies());
  REQUIRE(paths2.size() == 1);
  REQUIRE(paths2[0].error.empty());
  std::string gcode2;
  REQUIRE(cam::PostProcess("mach3", s2, paths2, {}, gcode2, &error));
  const cam::Verdict v2 = cam::VerifyGcode(gcode2, s2, doc2->GetBodies(), 0.25);
  INFO("hair: leftover " << v2.maxLeftover << " at " << v2.leftoverAt.x << ","
                         << v2.leftoverAt.y);
  CHECK(v2.maxLeftover < 0.05);
}

TEST_CASE("cam: a bore climbs straight out before it moves to the next ring",
          "[cam]") {
  // A bore is rings of helices, and each one began with one rapid to its
  // start at safe Z: from the bottom of the last ring that climbed and
  // travelled at once, through the wall of a hole not yet cut to size.
  auto doc = DrilledDoc(); // D16 through a 40 x 30 x 20 block
  cam::Setup s = BasicSetup();
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, BoreWall(*doc)});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  s.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(paths[0].error.empty());
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(s, doc->GetBodies(), lo, hi));
  int sideways = 0;
  for (size_t k = 1; k < paths[0].moves.size(); k++) {
    const cam::Move& m = paths[0].moves[k];
    const glm::dvec3& from = paths[0].moves[k - 1].to;
    if (m.kind == cam::Move::Kind::Rapid &&
        glm::length(glm::dvec2(m.to - from)) > 1e-6 &&
        std::min(from.z, m.to.z) < hi.z - 1e-6)
      sideways++;
  }
  CHECK(sideways == 0);
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", s, paths, {}, gcode, &error));
  CHECK(cam::VerifyGcode(gcode, s, doc->GetBodies(), 0.25).rapidsThroughStock ==
        0);
}

TEST_CASE("cam: adaptive links run at the machine's traverse rate", "[cam]") {
  if (!cam::AdaptiveAvailable())
    SKIP("this build has no adaptive clearing");
  auto doc = PocketDoc();
  cam::Setup s = BasicSetup();
  s.rapidFeed = 2000.0; // the tool cuts at 600
  const cam::DerivedOp d =
      cam::DeriveOperation(s, doc->GetBodies(), cam::OpType::Pocket,
                           cam::Target{0, PocketFloorRef(*doc)});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  op.type = cam::OpType::Adaptive;
  s.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(paths[0].error.empty());
  int links = 0;
  for (const cam::Move& m : paths[0].moves)
    if (m.kind == cam::Move::Kind::Cut && std::abs(m.feed - 2000.0) < 1e-9)
      links++;
  CHECK(links > 0);
}

TEST_CASE("cam: a drill's point in a flat-bottomed blind hole is said",
          "[cam]") {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               40.0, 30.0, 20.0));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::vec3(20, 15, 8), glm::vec3(0, 0, 1), 2.5, 13.0));
  const uint64_t hole = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Drilled", block, hole,
                                                   BooleanOp::Subtract));
  cam::Setup s = BasicSetup();
  s.tools[0] = ShapedTool(cam::ToolType::Drill, 5.0, 118.0);
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, HoleWall(*doc)});
  REQUIRE(d.ok);
  REQUIRE(d.notes.size() == 1);
  CHECK(d.notes[0].find("cone") != std::string::npos);
  // A through hole breaks out and has no floor to speak of.
  const auto through = HoleDoc(5.0);
  const cam::DerivedOp t =
      cam::DeriveOperation(s, through->GetBodies(), cam::OpType::Drill,
                           cam::Target{0, HoleWall(*through)});
  REQUIRE(t.ok);
  CHECK(t.notes.empty());
}

TEST_CASE("cam: a blind hole's floor is its own, not its bounding box's",
          "[cam]") {
  // BRepBndLib::Add widens a face's box by its tolerance: the pan/tilt base
  // plate's Ø32 bearing seat was bored 20 µm under its own shoulder, and a
  // chamfer's edge read 0.05 mm over the part's top.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               40.0, 30.0, 20.0));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::vec3(20, 15, 8), glm::vec3(0, 0, 1), 4.0, 13.0));
  const uint64_t hole = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Bored", block, hole,
                                                   BooleanOp::Subtract));
  const cam::Setup s = BasicSetup();
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, HoleWall(*doc)});
  INFO(d.why);
  REQUIRE(d.ok);
  CHECK_THAT(d.op.bottomZ, WithinAbs(8.0, 1e-6));
  // The wall's own top, for an edge break, is the part's top.
  cam::Setup v = s;
  v.tools[0] = ShapedTool(cam::ToolType::VBit, 6.0, 90.0);
  const cam::DerivedOp e =
      cam::DeriveOperation(v, doc->GetBodies(), cam::OpType::EdgeRound,
                           cam::Target{0, HoleWall(*doc)});
  INFO(e.why);
  REQUIRE(e.ok);
  CHECK_THAT(e.op.topZ, WithinAbs(20.0, 1e-6));
}

TEST_CASE("cam: a bore roughed then finished stops on its floor", "[cam]") {
  // The rough leaves finish stock on the WALL. Lowering its bottom by that
  // stock is right where the depth passes lift the floor by it (a profile,
  // a pocket); a bore helixes to its own bottom, and the pan/tilt base
  // plate's bearing seat was roughed 0.2 mm under its shoulder.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               40.0, 30.0, 20.0));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Seat", glm::vec3(20, 15, 8), glm::vec3(0, 0, 1), 10.0, 13.0));
  const uint64_t seat = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Bored", block, seat,
                                                   BooleanOp::Subtract));
  cam::Setup s = BasicSetup();
  cam::Tool finisher = s.tools[0];
  finisher.number = 2;
  finisher.diameter = 4.0;
  s.tools.push_back(finisher);
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, HoleWall(*doc)});
  INFO(d.why);
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  op.finishTool = 2;
  op.finishStock = 0.2;
  s.operations.push_back(op);
  s.safeZ = cam::DeriveSafeZ(s, doc->GetBodies());
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(paths.size() == 2);
  for (const cam::Toolpath& tp : paths) {
    INFO(tp.error);
    REQUIRE(tp.error.empty());
    CHECK(tp.MinZ() >= 8.0 - 1e-6);
  }
  std::string gcode, why;
  REQUIRE(cam::PostProcess("mach3", s, paths, {}, gcode, &why));
  const cam::Verdict v = cam::VerifyGcode(gcode, s, doc->GetBodies(), 0.1);
  INFO("gouge " << v.maxGouge << " at " << v.gougeAt.x << "," << v.gougeAt.y);
  CHECK(v.maxGouge < 0.05);
}

TEST_CASE("cam: a hole that opens downward is refused, not drilled through "
          "the part",
          "[cam]") {
  // Blind from the UNDERSIDE: the bore runs z 0..12 and the block is solid
  // above it. Its axis is vertical, which was all the check asked, so it was
  // derived as a through hole from the stock top -- 21 mm of drilling down
  // through the part to reach it.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               40.0, 30.0, 20.0));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::vec3(20, 15, -1), glm::vec3(0, 0, 1), 4.0, 13.0));
  const uint64_t hole = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Bored", block, hole,
                                                   BooleanOp::Subtract));
  cam::Setup s = BasicSetup();
  const cam::DerivedOp up = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, HoleWall(*doc)});
  REQUIRE_FALSE(up.ok);
  CHECK(up.why.find("opens downward") != std::string::npos);
  // Flipped, it opens toward the spindle and is drilled to its floor: 12 mm
  // up from the underside, which is now the top.
  s.frame = cam::FlipFrame(s.frame);
  const cam::DerivedOp flipped = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, HoleWall(*doc)});
  INFO(flipped.why);
  REQUIRE(flipped.ok);
  CHECK_THAT(flipped.op.bottomZ, WithinAbs(-12.0, 1e-6));
}

TEST_CASE("cam: clearing round a part takes the stock down its outside wall",
          "[cam][slow]") {
  // A Ø20 boss 15 tall: no floor to pick, the waste all round it. A clearing
  // op on its OUTSIDE wall clears from the stock's edge to the wall
  // (docs/cam/operations.md#clear-round); a wall that reaches the part's
  // bottom goes through the stock's bottom with it, as a cut-out does.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Boss", glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 10.0, 15.0));
  cam::Setup s = BasicSetup();
  s.stock.margin = glm::dvec3(6.0, 6.0, 1.0);
  s.stock.bottomMargin = 1.0;
  s.safeZ = cam::DeriveSafeZ(s, doc->GetBodies());
  const ElementRef wall = HoleWall(*doc);
  REQUIRE(wall.IsSet());
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Adaptive, cam::Target{0, wall});
  INFO(d.why);
  REQUIRE(d.ok);
  CHECK(d.op.side == cam::Side::Outside);
  CHECK(d.op.name == "Clear round");
  CHECK_THAT(d.op.topZ, WithinAbs(16.0, 1e-6));
  CHECK_THAT(d.op.bottomZ, WithinAbs(-1.0, 1e-6));
  // Asked as an adaptive only: the pocket verb on a wall is a missed floor,
  // refused with the way to ask for this.
  {
    const cam::DerivedOp pocket = cam::DeriveOperation(
        s, doc->GetBodies(), cam::OpType::Pocket, cam::Target{0, wall});
    REQUIRE_FALSE(pocket.ok);
    CHECK(pocket.why.find("FLOOR") != std::string::npos);
    CHECK(pocket.why.find("adaptive on its outside wall") != std::string::npos);
  }
  // An opening's wall is refused either way: its floor says how deep.
  {
    auto slotted = SlottedDoc();
    const cam::DerivedOp opening = cam::DeriveOperation(
        BasicSetup(), slotted->GetBodies(), cam::OpType::Adaptive,
        cam::Target{0, WallNear(*slotted, 14.0, 15.0)});
    REQUIRE_FALSE(opening.ok);
    CHECK(opening.why.find("FLOOR") != std::string::npos);
  }
  for (const cam::OpType type : {cam::OpType::Pocket, cam::OpType::Adaptive}) {
    if (type == cam::OpType::Adaptive && !cam::AdaptiveAvailable())
      continue;
    cam::Setup job = s;
    cam::Operation op = d.op;
    op.type = type;
    op.id = 1;
    op.stepover = 0.3;
    job.operations.push_back(op);
    const cam::Toolpath tp = cam::GenerateAll(job, doc->GetBodies())[0];
    INFO(tp.error);
    REQUIRE(tp.error.empty());
    REQUIRE_FALSE(tp.moves.empty());
    // Never into the boss, and out to the stock's edge (the stock is 32 mm
    // square: the cutter's centre reaches its edge, 16 mm out).
    double reach = 0.0;
    for (const cam::Move& m : tp.moves) {
      if (m.kind == cam::Move::Kind::Rapid)
        continue;
      const double r = glm::length(glm::dvec2(m.to));
      INFO("cut at " << m.to.x << "," << m.to.y << "," << m.to.z);
      CHECK(r >= 10.0 + 3.0 - 0.05);
      reach = std::max(reach, std::max(std::abs(m.to.x), std::abs(m.to.y)));
    }
    CHECK(reach >= 16.0 - 0.05);
    // And the posted program leaves the boss and nothing else.
    std::string gcode, why;
    REQUIRE(cam::PostProcess("mach3", job, {tp}, {}, gcode, &why));
    const cam::Verdict v = cam::VerifyGcode(gcode, job, doc->GetBodies(), 0.25);
    INFO((type == cam::OpType::Pocket ? "pocket" : "adaptive")
         << " leftover " << v.maxLeftover << " at " << v.leftoverAt.x << ","
         << v.leftoverAt.y << " area " << v.leftoverArea << " gouge "
         << v.maxGouge);
    CHECK(v.Safe(0.375));
    CHECK(v.Complete(0.375));
  }
}

TEST_CASE("cam verify: a sloped move cuts as low as it goes over each cell",
          "[cam]") {
  // Carved at the tool's height at the closest approach, a cell a descending
  // move passes and then keeps under its rim stayed as high as the tool was
  // when it passed -- leftover on every ramp, and a "rapid through material"
  // when the tool lifted out of its own cut.
  cam::Tool flat;
  flat.diameter = 6.0;
  cam::HeightMap map =
      cam::MakeStock(glm::dvec3(-5, -5, 0), glm::dvec3(6, 5, 10), 0.1);
  const glm::dvec3 a(0.0, 0.0, 5.0), b(1.0, 0.0, 4.0);
  (void)cam::CarveSweep(a, b, &flat, map);
  int checked = 0;
  for (int j = 0; j < map.ny; j++)
    for (int i = 0; i < map.nx; i++) {
      const glm::dvec2 c(map.CellX(i), map.CellY(j));
      double lowest = 10.0;
      for (int k = 0; k <= 2000; k++) {
        const double t = k / 2000.0;
        const glm::dvec3 p = a + (b - a) * t;
        if (glm::length(glm::dvec2(p) - c) <= 3.0)
          lowest = std::min(lowest, p.z);
      }
      if (lowest < 10.0) {
        checked++;
        CHECK_THAT(map.At(i, j), WithinAbs(lowest, 2e-3));
      }
    }
  CHECK(checked > 100);
}

TEST_CASE("cam: adaptive clears a face turned over, and falls back when it "
          "cannot start",
          "[cam]") {
  if (!cam::AdaptiveAvailable())
    SKIP("this build has no adaptive clearing");
  // A pocket in the UNDERSIDE of a block, cut at A = 180: the index mirrors
  // Y, so the face arrives turned over, and adaptive still clears it. (The
  // rotary sampler's flats that declined at A = 0 and 180 alike were too
  // tight round a cross hole, not turned over: the fall-back below.)
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               40.0, 30.0, 20.0));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<BoxFeature>("Under", glm::vec3(8, 6, -1),
                                               24.0, 18.0, 9.0));
  const uint64_t under = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocketed", block, under,
                                                   BooleanOp::Subtract));
  ElementRef floor;
  for (const Body& b : doc->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::dvec3 o(0.0), n(0.0), u(0.0);
      if (b.shape.FacePlane(index, o, n, u) && n.z < -0.99f &&
          std::abs(o.z - 8.0f) < 1e-3f)
        floor = ElementRef{b.createdBy, name};
    }
  REQUIRE(floor.IsSet());
  cam::Setup s = BasicSetup();
  RotaryMachine(s);
  s.rotationA = 180.0;
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Adaptive;
  op.side = cam::Side::Inside;
  op.targets = {cam::Target{0, floor}};
  op.topZ = 21.0;
  op.bottomZ = 12.0; // the floor, turned over: 20 - 8
  op.stepdown = 3.0;
  op.stepover = 0.45;
  s.operations.push_back(op);
  const auto paths = cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error << " / " << paths[0].strategy);
  CHECK(paths[0].error.empty());
  CHECK(paths[0].strategy.find("concentric") == std::string::npos);
  CHECK(paths[0].CutLength() > 100.0);

  // A 4.6 mm slot with a 4 mm cutter: adaptive finds nowhere to start, and
  // used to leave a rapid and a plunge at an unset point -- moves, so the
  // concentric fall-back never ran. It runs now, and says so.
  auto slot = std::make_unique<Document>();
  slot->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                                40.0, 30.0, 20.0));
  const uint64_t sb = slot->GetFeature(0)->GetId();
  slot->AddFeature(std::make_unique<BoxFeature>("Slot", glm::vec3(8, 12, 15),
                                                25.0, 4.6, 6.0));
  const uint64_t sk = slot->GetFeature(1)->GetId();
  slot->AddFeature(
      std::make_unique<BooleanFeature>("Slotted", sb, sk, BooleanOp::Subtract));
  cam::Setup ss = BasicSetup();
  ss.tools[0].diameter = 4.0;
  cam::Operation k;
  k.id = 1;
  k.type = cam::OpType::Adaptive;
  k.side = cam::Side::Inside;
  k.targets = {cam::Target{0, UpFaceAt(*slot, 15.0)}};
  REQUIRE(k.targets[0].ref.IsSet());
  k.topZ = 21.0;
  k.bottomZ = 15.0;
  k.stepdown = 2.0;
  k.stepover = 0.45;
  ss.operations.push_back(k);
  const auto kp = cam::GenerateAll(ss, slot->GetBodies());
  REQUIRE(kp.size() == 1);
  INFO(kp[0].error << " / " << kp[0].strategy);
  CHECK(kp[0].error.empty());
  CHECK(kp[0].strategy.find("concentric") != std::string::npos);
  CHECK(kp[0].MinZ() < 15.0 + 1e-6);
}

TEST_CASE("cam: a hole is through where it comes out, not at the part's "
          "lowest point",
          "[cam]") {
  // A thick block and a thin plate beside it: a hole through the plate ends
  // 20 mm above the block's underside, and read as BLIND -- it stopped at
  // the plate's underside, the drill's cone left in the plate.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Thick", glm::vec3(0, 0, 0),
                                               20.0, 30.0, 30.0));
  const uint64_t thick = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<BoxFeature>("Plate", glm::vec3(20, 0, 20),
                                               20.0, 30.0, 10.0));
  const uint64_t plate = doc->GetFeature(1)->GetId();
  doc->AddFeature(
      std::make_unique<BooleanFeature>("L", thick, plate, BooleanOp::Union));
  const uint64_t l = doc->GetFeature(2)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::vec3(30, 15, 19), glm::vec3(0, 0, 1), 2.5, 12.0));
  const uint64_t hole = doc->GetFeature(3)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Drilled", l, hole,
                                                   BooleanOp::Subtract));
  cam::Setup s = BasicSetup();
  s.tools[0] = ShapedTool(cam::ToolType::Drill, 5.0, 118.0);
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Drill, cam::Target{0, HoleWall(*doc)});
  REQUIRE(d.ok);
  // Past the plate's underside by the point's own height, and no note about
  // a flat floor it does not have.
  CHECK(d.op.bottomZ < 20.0 - 1.0);
  CHECK(d.notes.empty());
}

TEST_CASE("cam: tool order is by first use, and a rest pass follows its "
          "finish",
          "[cam]") {
  auto doc = BlockDoc();
  cam::Setup s = BasicSetup();
  s.order = cam::OrderOutputBy::Tool;
  for (const int n : {2, 3}) {
    cam::Tool t = s.tools[0];
    t.number = n;
    s.tools.push_back(t);
  }
  // Authored: T3, then T1, then a T2 that rests from T3... and another T3.
  const auto profile = [](uint64_t id, int tool) {
    cam::Operation op;
    op.id = id;
    op.type = cam::OpType::Profile;
    op.side = cam::Side::Outside;
    op.topZ = 21.0;
    op.bottomZ = 10.0;
    op.toolNumber = tool;
    return op;
  };
  s.operations = {profile(1, 3), profile(2, 1), profile(3, 2), profile(4, 3)};
  s.operations[2].restFromTool = 1;
  std::vector<int> order;
  for (const size_t i : cam::CuttingOrder(s))
    order.push_back(s.operations[i].toolNumber);
  // T3 first (it is used first), both T3s together; T2 after T1, which it
  // rests from -- not 1, 2, 3 by number.
  CHECK(order == std::vector<int>{3, 3, 1, 2});
  std::vector<int> posted;
  for (const cam::Toolpath& tp : cam::GenerateAll(s, doc->GetBodies()))
    posted.push_back(tp.toolNumber);
  CHECK(posted == std::vector<int>{3, 3, 1, 2});
  // A rest pass authored BEFORE the tool it rests from moves after it.
  s.operations = {profile(1, 2), profile(2, 3)};
  s.operations[0].restFromTool = 3;
  order.clear();
  for (const size_t i : cam::CuttingOrder(s))
    order.push_back(s.operations[i].toolNumber);
  CHECK(order == std::vector<int>{3, 2});
}

TEST_CASE("cam: concentric rings link at depth instead of lifting", "[cam]") {
  // Every ring of a clearing pass used to get a full entry: lift to safe Z,
  // fly back, descend, ramp, cut a lap. Twelve laps of a flange recess is
  // twelve of those — "does one lap, lifts and goes back a bit, then down and
  // to the next circle". A ring's neighbour starts one step-over away through
  // the band that ring is about to cut anyway, so there is nothing to lift
  // over.
  auto doc = FlangeDoc(/*holeDia=*/0.0, /*boss=*/false); // 80 x 60 pocket
  const ElementRef floor = PocketFloorRef(*doc);
  REQUIRE(floor.IsSet());
  cam::Setup s = BasicSetup();
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc->GetBodies(), cam::OpType::Pocket, cam::Target{0, floor});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  op.type = cam::OpType::Pocket; // concentric, not adaptive
  s.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());

  // Retracts to safe height. An 80 x 60 pocket at 2.7 mm step-over takes
  // dozens of rings over two depths; one lift per ring was 63 of them. What
  // is left is the unavoidable ones — a new depth, and any ring far enough
  // from the last that linking at depth would drag the cutter across
  // something.
  int lifts = 0;
  for (const cam::Move& m : paths[0].moves)
    if (m.kind == cam::Move::Kind::Rapid && m.to.z >= s.safeZ - 1e-6)
      lifts++;
  INFO("lifts " << lifts);
  CHECK(lifts <= 8);

  // Linking must not cost safety. The whole point of leaving the tool down is
  // that the link is one step-over through material the next ring cuts
  // anyway — so the verdict has to be the same as before.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", s, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, s, doc->GetBodies(), 0.6);
  INFO("gouge " << v.maxGouge << " rapids " << v.rapidsThroughStock);
  CHECK(v.rapidsThroughStock == 0);
  CHECK(v.maxGouge < 0.6);
}

TEST_CASE("cam: clearing the outline passes over holes", "[cam][slow]") {
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

TEST_CASE("cam: an opening the cutter fits into is still an island",
          "[cam][slow]") {
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

TEST_CASE("cam: clearing the outline still goes around material",
          "[cam][slow]") {
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

TEST_CASE("cam: a floor with both keeps the boss and drops the hole",
          "[cam][slow]") {
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
  // Pointing down: half a turn.
  REQUIRE(cam::SolveIndexA({0.0, 0.0, -1.0}, degrees));
  CHECK_THAT(std::abs(degrees), WithinAbs(180.0, 1e-9));
  // The contract, and the only thing that matters: turned by the solved A,
  // the face points at the spindle IN THE FRAME THE TOOLPATHS ARE CUT IN.
  // The formula was once checked against its own number (+Y -> 90) while
  // the framing turned the part the other way, and "Turn a face up"
  // machined the part's far side. The framing now turns it as the machine
  // does, and "cam rotary: an indexed cut and the rotary check turn the
  // part the same way" holds the two to one A.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  setup.machine.hasRotaryAxis = true;
  for (const glm::dvec2& at :
       {glm::dvec2(20.0, 30.0), glm::dvec2(20.0, 0.0), glm::dvec2(0.0, 15.0)}) {
    const ElementRef wall = WallNear(*doc, at.x, at.y);
    glm::dvec3 n(0.0);
    REQUIRE(cam::FaceNormalOf(doc->GetBodies(), wall, n));
    if (!cam::SolveIndexA(glm::dvec3(n), degrees))
      continue; // the X-facing wall: refused, below
    cam::Setup turned = setup;
    turned.rotationA = degrees;
    glm::dvec3 up(0.0);
    REQUIRE(cam::FaceNormalOf(cam::BodiesInSetupFrame(turned, doc->GetBodies()),
                              wall, up));
    INFO("wall at " << at.x << "," << at.y << " A " << degrees);
    CHECK(up.z > 0.999f);
  }
  // ...and on a part CLAMPED some other way first: the normal is the one the
  // rotary sees (the fixture frame applied, no index), as the index pick
  // reads it, and turned by the answer the face still comes up. Every face
  // of the block, clamped on its end so the old top and bottom are walls.
  cam::Setup clamped = setup;
  clamped.frame = cam::SolveFaceUp({1.0, 0.0, 0.0}, clamped.frame);
  int solved = 0;
  for (const Body& b : doc->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::dvec3 c(0.0), mn(0.0), u(0.0);
      if (!b.shape.FacePlane(index, c, mn, u))
        continue;
      const ElementRef face{b.createdBy, name};
      glm::dvec3 n(0.0);
      REQUIRE(cam::FaceNormalOf(
          cam::BodiesInSetupFrame(clamped, doc->GetBodies()), face, n));
      if (!cam::SolveIndexA(glm::dvec3(n), degrees))
        continue; // the two faces now along the axis
      solved++;
      cam::Setup turned = clamped;
      turned.rotationA = degrees;
      glm::dvec3 up(0.0);
      REQUIRE(cam::FaceNormalOf(
          cam::BodiesInSetupFrame(turned, doc->GetBodies()), face, up));
      INFO("model normal " << mn.x << " " << mn.y << " " << mn.z << " A "
                           << degrees);
      CHECK(up.z > 0.999f);
    }
  CHECK(solved == 4);
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

  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{0, wall});
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
  // (docs/cam/operations.md#profile).
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
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Pocket, cam::Target{0, wall});
  CHECK_FALSE(derived.ok);
  INFO(derived.why);
  CHECK(derived.why.find("FLOOR") != std::string::npos);
}

TEST_CASE("cam: a wall is not a gouge, whatever the grid", "[cam][slow]") {
  // A cell centre landing on the corner of an island is inside the island for
  // the model and inside the cut for the tool — both boundary tests are
  // inclusive — so a clean job reported the island's whole height as a gouge.
  // The panel checks at 0.4 mm and the RPC defaulted to 0.25, so the two
  // disagreed about the SAME job: "cuts 6.00 mm INTO the part, that is scrap"
  // against a verdict of nothing (docs/cam/simulation.md#the-verifier). The
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
  cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Pocket, cam::Target{0, floor});
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

namespace {
bool SameMoves(const cam::Toolpath& a, const cam::Toolpath& b) {
  if (a.moves.size() != b.moves.size())
    return false;
  for (size_t i = 0; i < a.moves.size(); i++) {
    const cam::Move& x = a.moves[i];
    const cam::Move& y = b.moves[i];
    if (x.kind != y.kind || glm::length(x.to - y.to) > 1e-9 ||
        std::abs(x.feed - y.feed) > 1e-9)
      return false;
  }
  return true;
}
} // namespace

TEST_CASE("cam: an adaptive pass is cached, and can be deferred to a worker",
          "[cam]") {
  // docs/ui/long-operations.md#cam-toolpath-passes. The panel's generate must
  // be able to hand the expensive passes to a worker and still come back,
  // later, with EXACTLY what the synchronous generate produces — the cache is
  // keyed by content, so the worker's answer and the inline answer are the
  // same entry.
  if (!cam::AdaptiveAvailable())
    return;
  auto doc = PocketDoc(); // 20 x 14 pocket, floor at z = 12
  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());
  cam::Operation op =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket,
                           cam::Target{0, floor})
          .op;
  op.id = 1;
  op.type = cam::OpType::Adaptive;
  setup.operations.push_back(op);
  const size_t passes = cam::DepthPasses(op).size();
  REQUIRE(passes >= 2); // or "one pass per entry" is not exercised

  // The reference: no context, every pass inline, as it always was.
  const std::vector<cam::Toolpath> ref =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(ref.size() == 1);
  INFO(ref[0].error);
  REQUIRE(ref[0].error.empty());
  REQUIRE_FALSE(ref[0].pending);
  REQUIRE(ref[0].CutLength() > 0.0);

  SECTION("a cache fills on the first generate and answers the second") {
    cam::AdaptiveCache cache;
    cam::GenerateContext ctx;
    ctx.cache = &cache;
    const std::vector<cam::Toolpath> first =
        cam::GenerateAll(setup, doc->GetBodies(), &ctx);
    CHECK(ctx.pending.empty());
    CHECK(cache.misses == passes);
    CHECK(cache.hits == 0);
    CHECK(cache.Size() == passes);
    CHECK(SameMoves(first[0], ref[0]));
    const std::vector<cam::Toolpath> second =
        cam::GenerateAll(setup, doc->GetBodies(), &ctx);
    CHECK(cache.hits == passes);
    CHECK(cache.misses == passes); // no new ones
    CHECK(SameMoves(second[0], ref[0]));
  }

  SECTION("deferred: pending, nothing drawn, one request per pass") {
    cam::AdaptiveCache cache;
    cam::GenerateContext ctx;
    ctx.cache = &cache;
    ctx.defer = true;
    const std::vector<cam::Toolpath> deferred =
        cam::GenerateAll(setup, doc->GetBodies(), &ctx);
    REQUIRE(deferred.size() == 1);
    CHECK(deferred[0].pending);
    CHECK(deferred[0].moves.empty());
    CHECK(deferred[0].error.empty()); // not "produced no toolpath"
    CHECK(ctx.pending.size() == passes);
    // Depth order, top pass first: a worker cuts them in queue order and a
    // person watching the backplot sees the pocket deepen.
    for (size_t i = 1; i < ctx.pending.size(); i++)
      CHECK(ctx.pending[i].z < ctx.pending[i - 1].z);
    for (const cam::AdaptiveRequest& r : ctx.pending) {
      CHECK(r.key != 0);
      CHECK(r.safeZ == setup.safeZ);
      CHECK(r.tool.diameter == 6.0);
    }

    // The worker: each request from its copy alone, no setup in sight.
    for (const cam::AdaptiveRequest& r : ctx.pending)
      cache.Put(r.key, cam::AdaptiveClear(r));

    cam::GenerateContext again;
    again.cache = &cache;
    again.defer = true;
    const std::vector<cam::Toolpath> landed =
        cam::GenerateAll(setup, doc->GetBodies(), &again);
    CHECK(again.pending.empty());
    CHECK_FALSE(landed[0].pending);
    CHECK(landed[0].error.empty());
    CHECK(SameMoves(landed[0], ref[0]));
  }

  SECTION("the key moves with every input it hashes, and only those") {
    const std::vector<cam::Loop> loops =
        cam::SectionLoops(doc->GetBodies(), 12.0 + 1e-3);
    const cam::Tool& tool = setup.tools[0];
    const uint64_t base = cam::AdaptiveKey(setup, op, tool, loops, 15.0, 20.0);
    CHECK(base == cam::AdaptiveKey(setup, op, tool, loops, 15.0, 20.0));
    CHECK(base != cam::AdaptiveKey(setup, op, tool, loops, 14.0, 20.0));
    CHECK(base != cam::AdaptiveKey(setup, op, tool, loops, 15.0, 21.0));
    cam::Setup s2 = setup;
    s2.safeZ += 1.0;
    CHECK(base != cam::AdaptiveKey(s2, op, tool, loops, 15.0, 20.0));
    cam::Operation o2 = op;
    o2.stepover *= 0.5;
    CHECK(base != cam::AdaptiveKey(setup, o2, tool, loops, 15.0, 20.0));
    o2 = op;
    o2.stockToLeave += 0.1;
    CHECK(base != cam::AdaptiveKey(setup, o2, tool, loops, 15.0, 20.0));
    o2 = op;
    o2.side = cam::Side::Outside;
    CHECK(base != cam::AdaptiveKey(setup, o2, tool, loops, 15.0, 20.0));
    cam::Tool t2 = tool;
    t2.diameter = 4.0;
    CHECK(base != cam::AdaptiveKey(setup, op, t2, loops, 15.0, 20.0));
    t2 = tool;
    t2.feedCut += 1.0;
    CHECK(base != cam::AdaptiveKey(setup, op, t2, loops, 15.0, 20.0));
    t2 = tool;
    t2.feedPlunge += 1.0;
    CHECK(base != cam::AdaptiveKey(setup, op, t2, loops, 15.0, 20.0));
    std::vector<cam::Loop> l2 = loops;
    l2[0].pts[0].x += 0.001;
    CHECK(base != cam::AdaptiveKey(setup, op, tool, l2, 15.0, 20.0));
    // Fields a pass does NOT read do not move it: renaming the operation or
    // changing its tool NUMBER (the same tool) is not a new pass.
    o2 = op;
    o2.name = "renamed";
    CHECK(base == cam::AdaptiveKey(setup, o2, tool, loops, 15.0, 20.0));
    t2 = tool;
    t2.number = 7;
    t2.name = "other";
    CHECK(base == cam::AdaptiveKey(setup, op, t2, loops, 15.0, 20.0));
  }

  SECTION("the cache is bounded, oldest out first") {
    cam::AdaptiveCache cache;
    cam::AdaptiveResult r;
    r.moves.resize(1);
    for (uint64_t k = 1; k <= 300; k++)
      cache.Put(k, r);
    CHECK(cache.Size() == 256);
    CHECK_FALSE(cache.Contains(1));
    CHECK(cache.Contains(300));
  }
}

TEST_CASE("cam: a profile's outlines are cached, and can be deferred to a "
          "worker",
          "[cam]") {
  // docs/cam/README.md#background-work. The projected outlines are the one
  // expensive step of a profile -- an OCC section per depth pass -- and they
  // froze the frame for two minutes on a helically fluted pin. Deferred, the
  // profile comes back pending; the worker's answer, put into the cache,
  // must regenerate EXACTLY what the inline generate cuts.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  cam::DerivedOp derived =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Profile, {});
  REQUIRE(derived.ok);
  cam::Operation op = derived.op;
  op.id = 1;
  setup.operations.push_back(op);
  REQUIRE(cam::DepthPasses(op).size() >= 2);

  const std::vector<cam::Toolpath> ref =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(ref.size() == 1);
  INFO(ref[0].error);
  REQUIRE(ref[0].error.empty());
  REQUIRE(ref[0].CutLength() > 0.0);

  constexpr uint64_t kBodies = 0x5eed;
  SECTION("a cache fills on the first generate and answers the second") {
    cam::OutlineCache cache;
    cam::GenerateContext ctx;
    ctx.outlineCache = &cache;
    ctx.bodiesKey = kBodies;
    const std::vector<cam::Toolpath> first =
        cam::GenerateAll(setup, doc->GetBodies(), &ctx);
    CHECK(ctx.pendingOutline.empty());
    CHECK(cache.Size() == 1);
    CHECK(SameMoves(first[0], ref[0]));
    const std::vector<cam::Toolpath> second =
        cam::GenerateAll(setup, doc->GetBodies(), &ctx);
    CHECK(cache.hits >= 1);
    CHECK(SameMoves(second[0], ref[0]));
  }

  SECTION("no geometry key, no cache: a result cut from other geometry is "
          "never read") {
    cam::OutlineCache cache;
    cam::GenerateContext ctx;
    ctx.outlineCache = &cache;
    ctx.defer = true;
    const std::vector<cam::Toolpath> inline_ =
        cam::GenerateAll(setup, doc->GetBodies(), &ctx);
    CHECK(ctx.pendingOutline.empty());
    CHECK(cache.Size() == 0);
    CHECK(SameMoves(inline_[0], ref[0]));
  }

  SECTION("deferred: pending, no error, and the worker's answer lands") {
    cam::OutlineCache cache;
    cam::GenerateContext ctx;
    ctx.outlineCache = &cache;
    ctx.bodiesKey = kBodies;
    ctx.defer = true;
    const std::vector<cam::Toolpath> deferred =
        cam::GenerateAll(setup, doc->GetBodies(), &ctx);
    REQUIRE(deferred.size() == 1);
    CHECK(deferred[0].pending);
    CHECK(deferred[0].moves.empty());
    CHECK(deferred[0].error.empty()); // not "produced no toolpath"
    REQUIRE(ctx.pendingOutline.size() == 1);
    const cam::OutlineRequest& req = ctx.pendingOutline.front();
    CHECK(req.key == cam::OutlineKey(kBodies, cam::FramingKey(setup), req.topZ,
                                     req.passes, req.deflection));
    CHECK(req.passes == cam::DepthPasses(op));

    cache.Put(req.key, cam::OutlinePass(req));
    cam::GenerateContext again;
    again.outlineCache = &cache;
    again.bodiesKey = kBodies;
    again.defer = true;
    const std::vector<cam::Toolpath> landed =
        cam::GenerateAll(setup, doc->GetBodies(), &again);
    CHECK(again.pendingOutline.empty());
    CHECK_FALSE(landed[0].pending);
    CHECK(SameMoves(landed[0], ref[0]));
  }

  SECTION("the key moves with every argument") {
    const std::vector<double> passes = cam::DepthPasses(op);
    const uint64_t f = cam::FramingKey(setup);
    const uint64_t k = cam::OutlineKey(kBodies, f, op.topZ, passes, 0.02);
    CHECK(cam::OutlineKey(kBodies + 1, f, op.topZ, passes, 0.02) != k);
    CHECK(cam::OutlineKey(kBodies, f + 1, op.topZ, passes, 0.02) != k);
    CHECK(cam::OutlineKey(kBodies, f, op.topZ + 0.5, passes, 0.02) != k);
    CHECK(cam::OutlineKey(kBodies, f, op.topZ, {passes.front()}, 0.02) != k);
    CHECK(cam::OutlineKey(kBodies, f, op.topZ, passes, 0.05) != k);
    cam::Setup turned = setup;
    turned.rotationA = 90.0;
    CHECK(cam::FramingKey(turned) != f);
    turned = setup;
    turned.frame = cam::FlipFrame(setup.frame);
    CHECK(cam::FramingKey(turned) != f);
  }

  SECTION("one geometry key, two framings: neither reads the other's") {
    // Every setup of a job shares the caller's fingerprint, and so does every
    // index of one operation. Keyed on it alone, the block turned onto its
    // side was cut round the outline of the block standing up.
    cam::OutlineCache cache;
    cam::GenerateContext ctx;
    ctx.outlineCache = &cache;
    ctx.bodiesKey = kBodies;
    (void)cam::GenerateAll(setup, doc->GetBodies(), &ctx);
    cam::Setup turned = setup;
    turned.rotationA = 90.0;
    const std::vector<cam::Toolpath> fresh =
        cam::GenerateAll(turned, doc->GetBodies());
    const std::vector<cam::Toolpath> cached =
        cam::GenerateAll(turned, doc->GetBodies(), &ctx);
    REQUIRE(fresh.size() == 1);
    REQUIRE(cached.size() == 1);
    CHECK_FALSE(SameMoves(fresh[0], ref[0]));
    CHECK(SameMoves(cached[0], fresh[0]));
  }
}

TEST_CASE("cam: Add operation follows the setup", "[cam]") {
  // docs/cam/operations.md#derivation--what-a-click-means. The panel's button
  // added a whole-part cut-out to a job cut on the trunnion -- a 3-axis contour
  // round a dowel, plausible and wrong.
  cam::Setup setup = BasicSetup();
  CHECK(cam::AddOperationType(setup) == cam::OpType::Profile);

  // A 4th-axis machine is not the evidence: a plate on its bed is 3-axis.
  RotaryMachine(setup);
  cam::Operation profile;
  profile.id = 1;
  profile.type = cam::OpType::Profile;
  setup.operations.push_back(profile);
  CHECK(cam::AddOperationType(setup) == cam::OpType::Profile);

  // What the setup already cuts on the axis is.
  cam::Operation rotary;
  rotary.id = 2;
  rotary.type = cam::OpType::SurfaceRotary;
  setup.operations.push_back(rotary);
  CHECK(cam::AddOperationType(setup) == cam::OpType::SurfaceRotary);

  // ...and only on a machine that has the axis to cut it with.
  setup.machine.hasRotaryAxis = false;
  CHECK(cam::AddOperationType(setup) == cam::OpType::Profile);
}

TEST_CASE("cam: adaptive clears the same pocket, gently", "[cam]") {
  auto doc = PocketDoc(); // 20 x 14 pocket, floor at z = 12
  cam::Setup setup = BasicSetup();
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());

  cam::Operation base =
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket,
                           cam::Target{0, floor})
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

TEST_CASE("cam: adaptive that cannot lead in falls back whole, and says so "
          "once",
          "[cam]") {
  if (!cam::AdaptiveAvailable())
    SKIP("this build has no adaptive clearing");
  // The chess bishop's mitre: a 3.2 mm notch through the head, derived as a
  // pocket deep enough to come back ADAPTIVE, cut with a 3 mm flat. Its floor
  // runs out into air at both ends, where the region grows by the cutter's
  // radius; adaptive starts there, cuts, and ALSO flags that it could not
  // lead into the cut. The pass kept both: 9229 moves in the file and an
  // ERROR on the op, so the setup counted failed while the verifier said the
  // program it posted machined cleanly. An operation's outcome is one thing:
  // what it posts is what it reports.
  //
  // The region is the one the mitre's floor gave (x shifted to start at 0),
  // cut here as a closed pocket 8 mm deep so that exactly this outline is
  // what the pass is handed.
  const std::vector<glm::dvec2> floor = {
      {1.429, -8.421}, {1.617, -8.396}, {1.800, -8.346}, {1.975, -8.274},
      {2.140, -8.180}, {2.212, -8.130}, {3.179, -7.412}, {3.324, -7.289},
      {3.327, -7.286}, {4.193, -6.449}, {4.321, -6.309}, {4.325, -6.305},
      {5.074, -5.363}, {5.182, -5.207}, {5.187, -5.201}, {5.804, -4.171},
      {5.891, -4.003}, {5.956, -3.824}, {5.999, -3.639}, {6.017, -3.451},
      {6.018, -3.399}, {6.018, -1.337}, {5.943, -1.358}, {5.683, -1.513},
      {5.453, -1.716}, {5.252, -1.951}, {4.937, -2.491}, {4.852, -2.686},
      {4.823, -2.753}, {4.536, -3.378}, {4.534, -3.381}, {4.533, -3.383},
      {4.529, -3.386}, {4.527, -3.387}, {4.524, -3.388}, {4.520, -3.389},
      {4.518, -3.389}, {4.518, 3.389},  {4.521, 3.389},  {4.523, 3.389},
      {4.528, 3.387},  {4.530, 3.385},  {4.532, 3.384},  {4.535, 3.380},
      {4.823, 2.753},  {4.852, 2.686},  {4.937, 2.491},  {5.252, 1.951},
      {5.453, 1.716},  {5.683, 1.513},  {5.943, 1.358},  {6.018, 1.337},
      {6.018, 3.399},  {6.006, 3.589},  {5.970, 3.775},  {5.911, 3.955},
      {5.830, 4.126},  {5.804, 4.171},  {5.187, 5.201},  {5.079, 5.357},
      {5.074, 5.363},  {4.325, 6.305},  {4.197, 6.445},  {4.193, 6.449},
      {3.327, 7.286},  {3.183, 7.409},  {3.179, 7.412},  {2.212, 8.130},
      {2.053, 8.233},  {1.882, 8.315},  {1.703, 8.375},  {1.517, 8.412},
      {1.327, 8.425},  {1.138, 8.415},  {0.952, 8.380},  {0.771, 8.322},
      {0.599, 8.242},  {0.439, 8.141},  {0.292, 8.020},  {0.163, 7.882},
      {0.051, 7.729},  {0.002, 7.639},  {0.118, 7.591},  {0.735, 7.282},
      {0.737, 7.281},  {1.328, 6.928},  {1.331, 6.926},  {1.333, 6.925},
      {1.336, 6.920},  {1.337, 6.916},  {1.338, 6.913},  {1.338, 6.911},
      {1.318, 6.926},  {1.318, -6.926}, {1.338, -6.911}, {1.338, -6.914},
      {1.337, -6.916}, {1.336, -6.919}, {1.335, -6.921}, {1.332, -6.925},
      {1.328, -6.928}, {0.737, -7.281}, {0.735, -7.282}, {0.119, -7.591},
      {0.000, -7.640}, {0.006, -7.653}, {0.108, -7.812}, {0.230, -7.958},
      {0.369, -8.087}, {0.523, -8.197}, {0.690, -8.288}, {0.867, -8.356},
      {1.051, -8.402}, {1.239, -8.423}};
  // The library's side of it, frozen: moves AND a complaint, from one pass.
  cam::Setup s = BasicSetup();
  s.tools[0].diameter = 3.0;
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Adaptive;
  op.side = cam::Side::Inside;
  op.toolNumber = s.tools[0].number;
  op.topZ = 10.0;
  op.bottomZ = 2.0;
  op.stepdown = 1.5;
  op.stepover = 0.45;
  {
    cam::Loop l;
    l.pts = floor;
    const cam::AdaptiveResult raw =
        cam::AdaptiveClear(s, op, s.tools[0], {l}, 8.5, 10.0);
    INFO(raw.error);
    CHECK_FALSE(raw.moves.empty());
    CHECK_FALSE(raw.error.empty());
  }

  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>(
      "Block", glm::vec3(-3.0f, -11.0f, 0.0f), 12.0, 22.0, 10.0));
  auto sk = std::make_unique<SketchFeature>(
      "Floor sketch",
      SketchPlane(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, 1.0f),
                  glm::vec3(1.0f, 0.0f, 0.0f)));
  for (size_t i = 0; i < floor.size(); i++)
    sk->AddEntity(sketch::Line{glm::vec2(floor[i]),
                               glm::vec2(floor[(i + 1) % floor.size()])});
  doc->AddFeature(std::move(sk));
  const uint64_t sid = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<ExtrudeFeature>("Mitre", sid, 8.0,
                                                   ExtrudeMode::Cut, true));
  REQUIRE(doc->GetBodies().size() == 1);
  op.targets = {cam::Target{0, UpFaceAt(*doc, 2.0)}};
  REQUIRE(op.targets[0].ref.IsSet());
  s.operations.push_back(op);
  const auto paths = cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  const cam::Toolpath& tp = paths[0];
  INFO("error: " << tp.error << " / strategy: " << tp.strategy << " / moves "
                 << tp.moves.size());
  // Cut, by the strategy that could, and no error beside it...
  CHECK(tp.error.empty());
  REQUIRE_FALSE(tp.moves.empty());
  CHECK(tp.strategy.find("concentric") != std::string::npos);
  CHECK(tp.strategy.find("lead") != std::string::npos);
  double lo = 1e18;
  for (const cam::Move& m : tp.moves)
    if (m.kind != cam::Move::Kind::Rapid)
      lo = std::min(lo, m.to.z);
  CHECK(std::abs(lo - 2.0) < 1e-3);

  // ...and what it posts verifies as the pocket.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", s, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, s, doc->GetBodies(), 0.25);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover);
  CHECK(v.maxGouge < 0.05);
  CHECK(v.rapidsThroughStock == 0);
}

TEST_CASE("cam: the vendored adaptive library throws on some vertex layouts",
          "[cam][slow]") {
  // Frozen because it is the reason the fallback below exists. Adaptive2d
  // throws std::out_of_range from its finishing-pass z-tagging (map::at) for
  // particular vertex layouts — not particular SHAPES: a 30 mm circle clears
  // fine at 24, 60, 120 and 200 points and throws at 37. Which is why it
  // looked ellipse-specific from the GUI: every ellipse OCC discretises at the
  // 0.02 mm chord tolerance happened to land in the failing set
  // (docs/cam/operations.md#pocket-and-adaptive).
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
  // it (docs/cam/operations.md#the-capability-table-and-the-field-table).
  cam::JobDocument job;
  cam::Setup& s = job.AddSetup("Job");
  for (const cam::OpType type : {cam::OpType::Pocket, cam::OpType::Adaptive}) {
    cam::Operation op;
    op.id = job.AllocateId();
    op.type = type;
    op.side = cam::Side::Inside;
    op.topZ = 10.0;
    op.bottomZ = 4.0;
    s.operations.push_back(op);
  }
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_clearing_side.json";
  REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  auto back = DocumentIO::LoadJob(path.string());
  REQUIRE(back.has_value());
  const std::vector<cam::Setup>& setups = (*back)->GetSetups();
  REQUIRE(setups.size() == 1);
  REQUIRE(setups[0].operations.size() == 2);
  for (const cam::Operation& op : setups[0].operations)
    CHECK(op.side == cam::Side::Inside);
  std::filesystem::remove(path);

  // And a document written BEFORE the key was saved at all: the default
  // depends on the type, so those files repair themselves rather than asking
  // anyone to re-make a job. (tests/golden/v60-cam-arcs.json is a real one.)
  const std::string old = R"({"format":"designer-document","version":61,
    "kind":"job",
    "cam":[{"id":2,"name":"J","operations":[
      {"id":3,"type":"adaptive","tool":1,"top_z":10,"bottom_z":4},
      {"id":4,"type":"profile","tool":1,"top_z":10,"bottom_z":0}]}]})";
  const std::filesystem::path oldPath =
      std::filesystem::temp_directory_path() / "cam_clearing_side_old.json";
  std::ofstream(oldPath) << old;
  auto migrated = DocumentIO::LoadJob(oldPath.string());
  REQUIRE(migrated.has_value());
  const std::vector<cam::Operation>& ops =
      (*migrated)->GetSetups()[0].operations;
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].side == cam::Side::Inside);  // a clearing op clears inside
  CHECK(ops[1].side == cam::Side::Outside); // a profile's default is its own
  std::filesystem::remove(oldPath);
}

TEST_CASE("cam: cut it out refuses the floor of a pocket", "[cam]") {
  // Clicking a pocket floor and pressing "Cut it out" derived an OUTSIDE
  // profile through the whole part — a full-depth slot routed around the
  // pocket, in the middle of the board. The verifier called it 22 mm of gouge
  // on a 20 mm plate, which is scrap
  // (docs/cam/operations.md#derivation--what-a-click-means). The mirror
  // of this mistake was already guarded: a pocket asked for on a WALL says
  // "click the pocket's FLOOR".
  auto doc = PocketDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{0, floor});
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
  const cam::DerivedOp out = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{0, top});
  INFO(out.why);
  REQUIRE(out.ok);
  CHECK(out.op.side == cam::Side::Outside);
  CHECK(out.op.tabCount > 0);
}

TEST_CASE("cam: asking for adaptive derives the pocket it is", "[cam]") {
  // OpType::Adaptive had no case in the derivation, so it fell through the
  // switch and returned a DEFAULT operation — top 0, bottom -1, no target —
  // and said ok. A script asking for the strategy by name got an operation
  // pointing at nothing
  // (docs/cam/operations.md#derivation--what-a-click-means). Adaptive IS a
  // pocket; only the clearing strategy differs.
  auto doc = PocketDoc();
  const cam::Setup setup = BasicSetup();
  const ElementRef floor = UpFaceAt(*doc, 12.0);
  REQUIRE(floor.IsSet());
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Adaptive, cam::Target{0, floor});
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
  cam::JobDocument job;
  cam::Setup& s = job.AddSetup("Job");
  cam::Operation op;
  op.id = job.AllocateId();
  op.name = "Rough";
  op.type = cam::OpType::Adaptive;
  op.topZ = 10.0;
  op.bottomZ = 4.0;
  op.stepover = 0.25;
  s.operations.push_back(op);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_adaptive_roundtrip.json";
  REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  auto back = DocumentIO::LoadJob(path.string());
  REQUIRE(back.has_value());
  const std::vector<cam::Setup>& setups = (*back)->GetSetups();
  REQUIRE(setups.size() == 1);
  REQUIRE(setups[0].operations.size() == 1);
  CHECK(setups[0].operations[0].type == cam::OpType::Adaptive);
  // The NUMBERS, not just the type. `stepover` was written only for
  // pocket/face/edge_round, and adaptive -- which reads it as
  // `engine.stepOverFactor` -- was not on that list, so 0.25 came back as the
  // 0.45 loader default: the engagement fraction adaptive clearing exists to
  // hold constant, changed by a save. This test passed throughout, because it
  // checked the type and the machine and nothing else.
  CHECK_THAT(setups[0].operations[0].stepover, WithinAbs(0.25, 1e-12));
  CHECK_THAT(setups[0].operations[0].topZ, WithinAbs(10.0, 1e-12));
  CHECK_THAT(setups[0].operations[0].bottomZ, WithinAbs(4.0, 1e-12));
  CHECK(setups[0]
            .machine.fitArcs); // v60, defaulting on for a document without it
  std::filesystem::remove(path);
}

TEST_CASE("cam: every op field survives a save and reload", "[cam]") {
  // The general form of the bug above. A field written only when its owner's
  // TYPE is on a list is silently dropped for every type somebody forgot, and
  // the only thing that catches it is comparing the numbers back.
  cam::JobDocument job;
  cam::Setup& s = job.AddSetup("Job");
  cam::Operation op;
  op.id = job.AllocateId();
  op.name = "Everything";
  op.type = cam::OpType::Profile;
  op.topZ = 12.0;
  op.bottomZ = -0.5;
  op.stepdown = 1.5;
  op.stockToLeave = 0.3;
  op.stepover = 0.31;
  op.side = cam::Side::Inside;
  op.climb = false;
  op.tabCount = 3;
  op.tabWidth = 7.5;
  op.tabHeight = 2.25;
  op.cornerRelief = cam::CornerRelief::Dogbone;
  op.leadArcs = true;
  op.drilledBores = 3.2;
  op.peck = 1.1;
  op.edgeSize = 0.8;
  op.useOutline = true;
  op.finishTool = 2;
  op.finishStock = 0.2;
  s.operations.push_back(op);

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cam_all_fields_roundtrip.json";
  REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  auto back = DocumentIO::LoadJob(path.string());
  REQUIRE(back.has_value());
  REQUIRE((*back)->GetSetups().size() == 1);
  REQUIRE((*back)->GetSetups()[0].operations.size() == 1);
  const cam::Operation& r = (*back)->GetSetups()[0].operations[0];
  CHECK_THAT(r.topZ, WithinAbs(12.0, 1e-12));
  CHECK_THAT(r.bottomZ, WithinAbs(-0.5, 1e-12));
  CHECK_THAT(r.stepdown, WithinAbs(1.5, 1e-12));
  CHECK_THAT(r.stockToLeave, WithinAbs(0.3, 1e-12));
  CHECK_THAT(r.stepover, WithinAbs(0.31, 1e-12));
  CHECK(r.side == cam::Side::Inside);
  CHECK(r.climb == false);
  CHECK(r.tabCount == 3);
  CHECK_THAT(r.tabWidth, WithinAbs(7.5, 1e-12));
  CHECK_THAT(r.tabHeight, WithinAbs(2.25, 1e-12));
  CHECK(r.cornerRelief == cam::CornerRelief::Dogbone);
  CHECK(r.leadArcs);
  CHECK_THAT(r.drilledBores, WithinAbs(3.2, 1e-12));
  CHECK_THAT(r.peck, WithinAbs(1.1, 1e-12));
  CHECK_THAT(r.edgeSize, WithinAbs(0.8, 1e-12));
  CHECK(r.useOutline);
  CHECK(r.finishTool == 2);
  CHECK_THAT(r.finishStock, WithinAbs(0.2, 1e-12));
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
  CHECK(setup.machine.fitArcs);
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

TEST_CASE("cam: one operation at a time cuts what all of them cut",
          "[cam][slow]") {
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
      cam::DeriveOperation(setup, doc->GetBodies(), cam::OpType::Pocket,
                           cam::Target{0, floor})
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

// --- the front_wall section probe -------------------------------------------
//
// docs/cam/operations.md#profile (a profile follows the projection, not the
// section): four synthetic fixtures failed to reproduce
// the 8 mm gouge, so the entry's own next step is to stop rebuilding the part
// from a description and read the REAL one. This is that read.
//
// Hidden, and machine-bound by nature — the part is not in the repo:
//     build/designer_tests "[frontwall]"
//     DESIGNER_FRONT_WALL=<part dir> build/designer_tests "[frontwall]"
//
// It answers one question. If the outer loop of the section carries a notch
// at the cross-dowel height and not above or below it, the fixtures were
// missing something and "one contour at every depth" is the mechanism. If the
// outer loop is the same at every height, the section is innocent and the
// fault is downstream of it.
namespace {

struct LoopStats {
  double minX = 0, maxX = 0, minY = 0, maxY = 0;
  double area = 0; // signed *2, absolute value taken
  size_t npts = 0;
};

LoopStats StatsOf(const cam::Loop& l) {
  LoopStats s;
  s.npts = l.pts.size();
  double a2 = 0.0;
  for (size_t i = 0; i < l.pts.size(); i++) {
    const glm::dvec2& a = l.pts[i];
    const glm::dvec2& b = l.pts[(i + 1) % l.pts.size()];
    a2 += a.x * b.y - b.x * a.y;
    s.minX = i == 0 ? a.x : std::min(s.minX, a.x);
    s.maxX = i == 0 ? a.x : std::max(s.maxX, a.x);
    s.minY = i == 0 ? a.y : std::min(s.minY, a.y);
    s.maxY = i == 0 ? a.y : std::max(s.maxY, a.y);
  }
  s.area = std::abs(a2) * 0.5;
  return s;
}

} // namespace

TEST_CASE("cam probe: what the front_wall section does by depth",
          "[.][frontwall]") {
  // The part is the smart speaker's saved front_wall, which is not in the
  // repo: point DESIGNER_FRONT_WALL at its part directory.
  const char* env = std::getenv("DESIGNER_FRONT_WALL");
  if (!env)
    SKIP("DESIGNER_FRONT_WALL is not set (the front_wall part directory of a "
         "saved smart_speaker project; the part is not in the repo)");
  const std::string dir = env;
  if (!std::filesystem::exists(std::filesystem::path(dir) / "index.json"))
    SKIP("no front_wall at " << dir << " (DESIGNER_FRONT_WALL)");
  auto loaded = DocumentIO::Load(dir);
  REQUIRE(loaded.has_value());
  std::unique_ptr<Document>& doc = *loaded;

  // The saved job runs the "CNC" configuration, which SUPPRESSES the
  // cross-dowels (19/20/21/22) precisely to dodge this bug. Configuration 0
  // is every feature present — the bisect's "everything present" row, the one
  // that gouges.
  std::printf("active configuration as saved: %llu\n",
              (unsigned long long)doc->ActiveConfiguration());
  REQUIRE(doc->SetActiveConfiguration(0));
  std::printf("features: %zu, bodies: %zu\n", (size_t)doc->FeatureCount(),
              doc->GetBodies().size());

  // Since v73 the setups live in a JOB document, not in the part, so this
  // probe needs to be pointed at one. Warn rather than iterate nothing: a
  // diagnostic that silently examines an empty list is worse than one that
  // says it has nothing to examine.
  const char* jobEnv = std::getenv("DESIGNER_FRONT_WALL_JOB");
  if (jobEnv == nullptr) {
    WARN("set DESIGNER_FRONT_WALL_JOB to a job document to probe its setups");
    return;
  }
  auto loadedJob = DocumentIO::LoadJob(jobEnv);
  REQUIRE(loadedJob.has_value());
  const cam::Setup* setup = nullptr;
  for (const cam::Setup& s : (*loadedJob)->GetSetups())
    if (s.name == "Back")
      setup = &s;
  REQUIRE(setup != nullptr);
  std::printf("setup \"%s\": frame (xyzw) %.4f %.4f %.4f %.4f, origin %.3f "
              "%.3f %.3f\n",
              setup->name.c_str(), setup->frame.x, setup->frame.y,
              setup->frame.z, setup->frame.w, setup->origin.x, setup->origin.y,
              setup->origin.z);

  const std::vector<Body> framed =
      cam::BodiesInSetupFrame(*setup, doc->GetBodies());
  REQUIRE(!framed.empty());
  glm::dvec3 lo(0.0), hi(0.0);
  bool any = false;
  for (const Body& b : framed) {
    if (!b.shape.IsValid())
      continue;
    glm::dvec3 bmin(0.0), bmax(0.0);
    b.shape.GetBoundingBox(bmin, bmax);
    lo = any ? glm::min(lo, bmin) : bmin;
    hi = any ? glm::max(hi, bmax) : bmax;
    any = true;
  }
  REQUIRE(any);
  std::printf("framed bounds: x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f\n",
              (double)lo.x, (double)hi.x, (double)lo.y, (double)hi.y,
              (double)lo.z, (double)hi.z);

  // Sweep the whole thickness. The outer loop is the one the profile follows;
  // a notch shows as a point count and an AREA that move with depth while the
  // bounding box may not (a slot running inward from an edge keeps the box).
  std::printf("\n%8s %6s %6s %12s %10s %10s %10s %10s\n", "z", "loops", "npts",
              "outer area", "minX", "maxX", "minY", "maxY");
  const double z0 = lo.z, z1 = hi.z;
  const int kSteps = 34;
  for (int i = 0; i <= kSteps; i++) {
    const double t = (double)i / kSteps;
    // Never section exactly on a face: a plane flush with the top or bottom
    // is a degenerate cut, and the answer there says nothing about the part.
    const double z = z0 + (z1 - z0) * t;
    const double zc = std::clamp(z, z0 + 0.01, z1 - 0.01);
    const std::vector<cam::Loop> loops = cam::SectionLoops(framed, zc);
    const cam::Loop* outer = nullptr;
    double best = 0.0;
    for (const cam::Loop& l : loops) {
      const LoopStats s = StatsOf(l);
      if (outer == nullptr || s.area > best) {
        best = s.area;
        outer = &l;
      }
    }
    if (outer == nullptr) {
      std::printf("%8.3f %6zu   (no loops)\n", zc, loops.size());
      continue;
    }
    const LoopStats s = StatsOf(*outer);
    std::printf("%8.3f %6zu %6zu %12.2f %10.3f %10.3f %10.3f %10.3f\n", zc,
                loops.size(), s.npts, s.area, s.minX, s.maxX, s.minY, s.maxY);
  }

  // Then the outer loop itself, at the cross-dowel height and at a height
  // clear of them, near the x extremes — where a 22 mm slot in from x = +-105
  // would live. Printed as points so a notch is visible as a shape rather
  // than inferred from a number.
  const char* zenv = std::getenv("DESIGNER_PROBE_Z");
  const double zProbe = zenv ? std::atof(zenv) : -8.0;
  for (const double z : {zProbe, zProbe + 6.0}) {
    const std::vector<cam::Loop> loops = cam::SectionLoops(framed, z);
    const cam::Loop* outer = nullptr;
    double best = 0.0;
    for (const cam::Loop& l : loops) {
      const LoopStats s = StatsOf(l);
      if (outer == nullptr || s.area > best) {
        best = s.area;
        outer = &l;
      }
    }
    std::printf("\n--- outer loop at z = %.3f (%zu loops) ---\n", z,
                loops.size());
    if (outer == nullptr)
      continue;
    const LoopStats s = StatsOf(*outer);
    std::printf("    %zu points, area %.2f, x %.3f..%.3f, y %.3f..%.3f\n",
                s.npts, s.area, s.minX, s.maxX, s.minY, s.maxY);
    // Every vertex that is NOT on the four sides of the rectangle: exactly
    // what a notch is made of, and nothing else survives the filter.
    const double eps = 0.05;
    size_t off = 0;
    for (const glm::dvec2& p : outer->pts) {
      const bool onEdge =
          std::abs(p.x - s.minX) < eps || std::abs(p.x - s.maxX) < eps ||
          std::abs(p.y - s.minY) < eps || std::abs(p.y - s.maxY) < eps;
      if (!onEdge) {
        std::printf("    off-rectangle vertex  %10.3f %10.3f\n", p.x, p.y);
        off++;
      }
    }
    std::printf("    %zu of %zu vertices are off the bounding rectangle\n", off,
                s.npts);
    // The same loop UNFILTERED around one cross-dowel, in path order: whether
    // the notch opens onto the edge or floats inside is the whole question,
    // and only the vertices either side of it answer it.
    std::printf("    in path order, every vertex with y in -480..-440:\n");
    for (size_t i = 0; i < outer->pts.size(); i++)
      if (outer->pts[i].y > -480.0 && outer->pts[i].y < -440.0)
        std::printf("      [%3zu] %10.3f %10.3f\n", i, outer->pts[i].x,
                    outer->pts[i].y);
  }

  // The control, and the bisect's second row: the job's own configuration,
  // which suppresses the cross-dowels. If the notch is theirs it goes away
  // here, and the probe is measuring what it thinks it is.
  REQUIRE(doc->SetActiveConfiguration(43));
  const std::vector<Body> ctl =
      cam::BodiesInSetupFrame(*setup, doc->GetBodies());
  const std::vector<cam::Loop> ctlLoops = cam::SectionLoops(ctl, zProbe);
  const cam::Loop* ctlOuter = nullptr;
  double ctlBest = 0.0;
  for (const cam::Loop& l : ctlLoops) {
    const LoopStats st = StatsOf(l);
    if (ctlOuter == nullptr || st.area > ctlBest) {
      ctlBest = st.area;
      ctlOuter = &l;
    }
  }
  REQUIRE(ctlOuter != nullptr);
  const LoopStats cs = StatsOf(*ctlOuter);
  std::printf("\n--- CONTROL: configuration 43 (cross-dowels suppressed), "
              "z = %.3f ---\n",
              zProbe);
  std::printf("    %zu bodies, %zu loops, outer %zu points, area %.2f, "
              "x %.3f..%.3f\n",
              ctl.size(), ctlLoops.size(), cs.npts, cs.area, cs.minX, cs.maxX);
  size_t ctlNear = 0;
  for (const glm::dvec2& p : ctlOuter->pts)
    if (p.y > -480.0 && p.y < -440.0)
      ctlNear++;
  std::printf("    %zu outer vertices with y in -480..-440\n", ctlNear);
}

// The synthetic half of the same question, and the reason the 2026-08-31 fix
// was reverted: four fixtures "failed to reproduce" the notch. This builds the
// one that matters — a bore that opens onto a side face — in SETUP SPACE with
// a square setup, so no framing stands between the model and the section.
TEST_CASE("cam probe: a bore that opens onto an edge", "[.][frontwall]") {
  auto doc = std::make_unique<Document>();
  // The panel: 210 x 100 x 16, top at z = 0, exactly the front_wall's section.
  doc->AddFeature(std::make_unique<BoxFeature>(
      "Panel", glm::vec3(-105.0f, -50.0f, -16.0f), 210.0, 100.0, 16.0));
  // The cross-dowel: a sketch on the x = +105 side face, normal +X, uAxis +Y,
  // so V is +Z. The circle at (u, v) = (0, -8) is a bore on the mid-plane,
  // 8 mm below the top — the real part's geometry, in the real part's way.
  auto sk = std::make_unique<SketchFeature>(
      "Dowel",
      SketchPlane(glm::vec3(105.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                  glm::vec3(0.0f, 1.0f, 0.0f)));
  sk->AddEntity(sketch::Circle{glm::vec2(0.0f, -8.0f), 4.0});
  doc->AddFeature(std::move(sk));
  const uint64_t sid = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<ExtrudeFeature>("Bore", sid, 22.0,
                                                   ExtrudeMode::Cut, true));
  REQUIRE(doc->GetBodies().size() == 1);
  glm::dvec3 bmin(0.0), bmax(0.0);
  doc->GetBodies()[0].shape.GetBoundingBox(bmin, bmax);
  std::printf("fixture bounds: x %.3f..%.3f  y %.3f..%.3f  z %.3f..%.3f\n",
              (double)bmin.x, (double)bmax.x, (double)bmin.y, (double)bmax.y,
              (double)bmin.z, (double)bmax.z);

  for (const double z : {-8.0, -2.0, -14.0}) {
    const std::vector<cam::Loop> loops = cam::SectionLoops(doc->GetBodies(), z);
    const cam::Loop* outer = nullptr;
    double best = 0.0;
    for (const cam::Loop& l : loops) {
      const LoopStats st = StatsOf(l);
      if (outer == nullptr || st.area > best) {
        best = st.area;
        outer = &l;
      }
    }
    REQUIRE(outer != nullptr);
    const LoopStats st = StatsOf(*outer);
    // maxX is what the reverted round measured. Print it beside the two
    // numbers that can actually see a slot cut INTO an edge.
    std::printf("z %7.3f: %zu loops, outer %2zu points, area %10.3f, "
                "maxX %.3f (width %.3f)\n",
                z, loops.size(), st.npts, st.area, st.maxX, st.maxX - st.minX);
    for (const glm::dvec2& p : outer->pts)
      if (p.x < 104.95 && p.x > -104.95)
        std::printf("        inboard vertex %10.3f %10.3f\n", p.x, p.y);
  }
}

// The end of the ladder: the real job, posted and verified, gouge and all.
// Hidden for the same reason as the section probe — the part is not in the
// repo.  build/designer_tests "[frontwall]"
TEST_CASE("cam probe: the front_wall job's verdict", "[.][frontwall]") {
  // The part is the smart speaker's saved front_wall, which is not in the
  // repo: point DESIGNER_FRONT_WALL at its part directory.
  const char* env = std::getenv("DESIGNER_FRONT_WALL");
  if (!env)
    SKIP("DESIGNER_FRONT_WALL is not set (the front_wall part directory of a "
         "saved smart_speaker project; the part is not in the repo)");
  const std::string dir = env;
  if (!std::filesystem::exists(std::filesystem::path(dir) / "index.json"))
    SKIP("no front_wall at " << dir << " (DESIGNER_FRONT_WALL)");
  auto loaded = DocumentIO::Load(dir);
  REQUIRE(loaded.has_value());
  std::unique_ptr<Document>& doc = *loaded;
  // Configuration 0 is every feature present — the cross-dowels the saved job
  // suppresses precisely to dodge this, and the bisect's "everything present"
  // row.
  REQUIRE(doc->SetActiveConfiguration(0));

  const char* jobEnv = std::getenv("DESIGNER_FRONT_WALL_JOB");
  if (jobEnv == nullptr) {
    WARN("set DESIGNER_FRONT_WALL_JOB to a job document to post its setups");
    return;
  }
  auto loadedJob = DocumentIO::LoadJob(jobEnv);
  REQUIRE(loadedJob.has_value());
  for (const cam::Setup& setup : (*loadedJob)->GetSetups()) {
    const std::vector<cam::Toolpath> paths =
        cam::GenerateAll(setup, doc->GetBodies());
    std::string gcode, error;
    if (!cam::PostProcess(setup.machine.post, setup, paths, {}, gcode,
                          &error)) {
      std::printf("setup \"%s\": post failed: %s\n", setup.name.c_str(),
                  error.c_str());
      continue;
    }
    const cam::Verdict v =
        cam::VerifyGcode(gcode, setup, doc->GetBodies(), /*res=*/0.5);
    std::printf("setup \"%s\": gouge %.3f at (%.1f, %.1f), leftover %.3f, "
                "rapids through stock %d, cells %zu\n",
                setup.name.c_str(), v.maxGouge, v.gougeAt.x, v.gougeAt.y,
                v.maxLeftover, v.rapidsThroughStock, v.cells);
    for (const cam::Toolpath& t : paths)
      if (!t.error.empty())
        std::printf("    op %llu: %s\n", (unsigned long long)t.operationId,
                    t.error.c_str());
  }
}

// --- a profile follows the projection, not the section ----------------------
//
// docs/cam/operations.md#profile: a bore that opens onto a side face notches
// the part's cross-section at its own height and nowhere else. A profile that
// follows that notch cuts the bore's own 8 mm plus the 4 mm of solid above it
// and the 4 mm below — measured on the real part as an 8.00 mm gouge, and
// reproduced here in a fixture small enough to read.
//
// Two directions, deliberately. "Never cut a notch" would pass the first of
// these and is wrong: a notch that reaches the operation's top IS reachable
// and must be cut. What separates them is whether the material above it is
// gone by the time the cutter arrives, which is exactly what the projection
// from topZ down to the pass says.
namespace {

// The panel both fixtures start from: 210 x 100 x 16, top at z = 0.
std::unique_ptr<Document> PanelDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>(
      "Panel", glm::vec3(-105.0f, -50.0f, -16.0f), 210.0, 100.0, 16.0));
  return doc;
}

// A 3 mm cutter, so an 8 mm notch stays open under the outward offset an
// outside profile applies and the question is about the contour rather than
// about whether the tool fits.
cam::Setup PanelSetup() {
  cam::Setup s;
  s.name = "Panel";
  s.safeZ = 5.0;
  cam::Tool t;
  t.number = 1;
  t.name = "3mm flat";
  t.diameter = 3.0;
  t.feedCut = 900.0;
  t.feedPlunge = 300.0;
  t.spindleRpm = 18000.0;
  s.tools.push_back(t);
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.side = cam::Side::Outside;
  op.topZ = 1.0; // the stock top: 16 mm panel, 1 mm relative margin above
  op.bottomZ = -16.5;
  op.stepdown = 4.0;
  s.operations.push_back(op);
  return s;
}

// Moves whose XY lands in the notch — inside x = +-103 and within 3 mm of the
// centre line. The profile's own path runs at x = +-106.5 there (the panel
// plus the tool radius), so nothing but an excursion into the notch can
// satisfy both.
size_t MovesInTheNotch(const cam::Toolpath& tp) {
  size_t n = 0;
  for (const cam::Move& m : tp.moves)
    if (std::abs(m.to.x) < 103.0 && std::abs(m.to.y) < 3.0)
      n++;
  return n;
}

} // namespace

TEST_CASE("cam: a profile never follows a notch it cannot reach", "[cam]") {
  auto doc = PanelDoc();
  // The cross-dowel: a bore on the x = +105 side face, 8 mm across, 22 mm in,
  // its axis 8 mm below the top. It notches the section for z in (-12, -4)
  // and nowhere else — 4 mm of solid above it, 4 mm below.
  auto sk = std::make_unique<SketchFeature>(
      "Dowel",
      SketchPlane(glm::vec3(105.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                  glm::vec3(0.0f, 1.0f, 0.0f)));
  sk->AddEntity(sketch::Circle{glm::vec2(0.0f, -8.0f), 4.0});
  doc->AddFeature(std::move(sk));
  doc->AddFeature(std::make_unique<ExtrudeFeature>(
      "Bore", doc->GetFeature(1)->GetId(), 22.0, ExtrudeMode::Cut, true));
  REQUIRE(doc->GetBodies().size() == 1);

  // The section really does carry the notch at the bore's height and not
  // above it — the premise, checked here so a fixture that stops reproducing
  // fails as a fixture rather than as the rule below.
  const auto notchedAt = [&](double z) {
    for (const cam::Loop& l : cam::SectionLoops(doc->GetBodies(), z))
      for (const glm::dvec2& p : l.pts)
        if (p.x > 82.0 && p.x < 84.0)
          return true;
    return false;
  };
  REQUIRE(notchedAt(-8.0));
  REQUIRE_FALSE(notchedAt(-2.0));

  const cam::Setup setup = PanelSetup();
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0].error.empty());
  // The rule: 4 mm of solid stands over the bore, so no pass may enter it.
  CHECK(MovesInTheNotch(paths[0]) == 0);

  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v =
      cam::VerifyGcode(gcode, setup, doc->GetBodies(), /*res=*/0.4);
  INFO("gouge " << v.maxGouge << " at (" << v.gougeAt.x << ", " << v.gougeAt.y
                << ")");
  REQUIRE(v.cells > 0);
  CHECK(v.maxGouge < 0.5); // one cell of quantisation at 0.4 mm
}

TEST_CASE("cam: a profile DOES follow a notch open from the top", "[cam]") {
  auto doc = PanelDoc();
  // The same notch, reached the other way: a bore straddling the top face at
  // the x = +105 edge, cut 8 mm down. Nothing stands over it, so a profile
  // must go in — the assertion that stops the rule above degenerating into
  // "never cut a notch".
  auto sk = std::make_unique<SketchFeature>(
      "Slot", SketchPlane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 0.0f)));
  sk->AddEntity(sketch::Circle{glm::vec2(105.0f, 0.0f), 4.0});
  doc->AddFeature(std::move(sk));
  doc->AddFeature(std::make_unique<ExtrudeFeature>(
      "Notch", doc->GetFeature(1)->GetId(), 8.0, ExtrudeMode::Cut, true));
  REQUIRE(doc->GetBodies().size() == 1);

  const cam::Setup setup = PanelSetup();
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0].error.empty());
  CHECK(MovesInTheNotch(paths[0]) > 0);

  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v =
      cam::VerifyGcode(gcode, setup, doc->GetBodies(), /*res=*/0.4);
  INFO("gouge " << v.maxGouge << " at (" << v.gougeAt.x << ", " << v.gougeAt.y
                << ")");
  CHECK(v.maxGouge < 0.5);
}

TEST_CASE("cam: a picked wall is re-resolved at every depth", "[cam]") {
  // The same defect through the other door. `PickedWallContour` names the
  // contour at the WALL'S OWN MID-HEIGHT, which for a 16 mm panel is z = -8 —
  // the bore's axis, the one height whose section is notched. Picking that
  // wall used to cut its notch at every depth.
  auto doc = PanelDoc();
  auto sk = std::make_unique<SketchFeature>(
      "Dowel",
      SketchPlane(glm::vec3(105.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f),
                  glm::vec3(0.0f, 1.0f, 0.0f)));
  sk->AddEntity(sketch::Circle{glm::vec2(0.0f, -8.0f), 4.0});
  doc->AddFeature(std::move(sk));
  doc->AddFeature(std::make_unique<ExtrudeFeature>(
      "Bore", doc->GetFeature(1)->GetId(), 22.0, ExtrudeMode::Cut, true));

  // The panel's own -Y side wall: far from the notch, so the pick is
  // unambiguous and the contour it names is the whole outline.
  ElementRef wall;
  for (const Body& b : doc->GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::dvec3 centre(0.0), normal(0.0), x(0.0);
      if (!b.shape.FacePlane(index, centre, normal, x))
        continue;
      if (std::abs(normal.y) > 0.99f && centre.y < -49.0)
        wall = ElementRef{b.createdBy, name};
    }
  REQUIRE(wall.IsSet());

  cam::Setup setup = PanelSetup();
  setup.operations[0].targets.push_back(cam::Target{0, wall});
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  CHECK(MovesInTheNotch(paths[0]) == 0);
}

TEST_CASE("cam profile: a one-pass through cut follows the model's bottom "
          "outline",
          "[cam]") {
  // Found by the finishing pass, which IS one pass at the full depth: a
  // single pass below the model had no pass above it to inherit an outline
  // from, and produced nothing.
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Outside;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 0.0;
  setup.operations.push_back(op);
  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  CHECK(paths[0].error.empty());
  CHECK(paths[0].CutLength() > 100.0);
}

// ---------------------------------------------------------------------------
// Every target of an operation is cut.

namespace {
std::unique_ptr<Document> ThreeHoleDoc(double dia = 8.0, double height = 12.0) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0),
                                               60.0, 30.0, height));
  uint64_t solid = doc->GetFeature(0)->GetId();
  for (int i = 0; i < 3; i++) {
    doc->AddFeature(std::make_unique<CylinderFeature>(
        "Hole" + std::to_string(i),
        glm::vec3(15.0f + 15.0f * static_cast<float>(i), 15.0f, -1.0f),
        glm::vec3(0, 0, 1), dia * 0.5, height + 2.0));
    const uint64_t cut = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>(
        "Drilled" + std::to_string(i), solid, cut, BooleanOp::Subtract));
    solid = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
  }
  return doc;
}

// ONE wall reference per bore, in X order. OCC may split a cylindrical face
// in two, and a test that machined the same hole twice would pass while
// proving nothing.
std::vector<ElementRef> HoleWalls(const Document& doc) {
  std::vector<std::pair<double, ElementRef>> found;
  for (const Body& b : doc.GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::dvec3 c(0.0), n(0.0), u(0.0);
      if (b.shape.FacePlane(index, c, n, u))
        continue; // a floor or a wall of the block, not a bore
      glm::dvec3 o(0.0), axis(0.0), x(0.0);
      if (!b.shape.FaceAxis(index, o, axis, x))
        continue;
      bool seen = false;
      for (const auto& [at, ref] : found)
        if (std::abs(at - static_cast<double>(o.x)) < 1.0)
          seen = true;
      if (!seen)
        found.emplace_back(o.x, ElementRef{b.createdBy, name});
    }
  std::sort(found.begin(), found.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<ElementRef> out;
  for (auto& [at, ref] : found)
    out.push_back(ref);
  return out;
}
} // namespace

TEST_CASE("cam profile: every target is cut, not just the first",
          "[cam][profile]") {
  // Five bolt holes picked into one operation used to bore the first and
  // leave four standing, with the file posting at full length and `failed: 0`
  // -- TargetPaths read op.targets.front() and stopped.
  auto doc = ThreeHoleDoc();
  const std::vector<ElementRef> walls = HoleWalls(*doc);
  REQUIRE(walls.size() == 3);

  cam::Setup setup = BasicSetup(); // one 6 mm flat
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{0, walls[0]});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 3;
  op.targets.clear();
  for (const ElementRef& w : walls)
    op.targets.push_back(cam::Target{0, w});
  setup.operations.push_back(op);

  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  CHECK(paths[0].error.empty());
  CHECK(paths[0].skipped.empty());
  // A cut near every bore centre, not just the first.
  const double centres[3] = {15.0, 30.0, 45.0};
  for (const double cx : centres) {
    bool cutHere = false;
    for (const cam::Move& m : paths[0].moves)
      if (m.kind == cam::Move::Kind::Cut &&
          glm::length(glm::dvec2(m.to.x - cx, m.to.y - 15.0)) < 2.5)
        cutHere = true;
    INFO("bore at X" << cx);
    CHECK(cutHere);
  }
  // Three bores cost about three times one.
  cam::Setup single = BasicSetup();
  cam::Operation one = op;
  one.targets = {cam::Target{0, walls[0]}};
  single.operations.push_back(one);
  const auto onePath = cam::GenerateAll(single, doc->GetBodies());
  REQUIRE(onePath.size() == 1);
  REQUIRE(onePath[0].CutLength() > 0.0);
  const double ratio = paths[0].CutLength() / onePath[0].CutLength();
  INFO("ratio " << ratio);
  CHECK(ratio > 2.5);
  CHECK(ratio < 3.5);
  // ...and the holes come out without cutting the block.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25);
  INFO("gouge " << v.maxGouge);
  CHECK(v.maxGouge < 0.4);
}

TEST_CASE("cam profile: a target that cannot be followed is NAMED, and the "
          "others are still cut",
          "[cam][profile]") {
  auto doc = ThreeHoleDoc();
  const std::vector<ElementRef> walls = HoleWalls(*doc);
  REQUIRE(walls.size() == 3);
  cam::Setup setup = BasicSetup();
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{0, walls[0]});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 4;
  op.targets = {cam::Target{0, walls[0]},
                cam::Target{0, ElementRef{walls[0].featureId, "no/such/face"}},
                cam::Target{0, walls[2]}};
  setup.operations.push_back(op);

  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  // The operation still ran: one bad pick must not scrap the two good ones,
  // and `error` would make the post drop the whole operation.
  CHECK(paths[0].error.empty());
  CHECK(paths[0].CutLength() > 0.0);
  REQUIRE(paths[0].skipped.size() == 1);
  INFO(paths[0].skipped[0]);
  CHECK(paths[0].skipped[0].find("target 2 of 3") != std::string::npos);
  CHECK(paths[0].skipped[0].find("no/such/face") != std::string::npos);
  // The file says it too -- the operator reads the file, not the RPC.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  CHECK(gcode.find("SKIPPED on op 4") != std::string::npos);

  // Every target unusable IS a failed operation, and says which.
  cam::Setup none = BasicSetup();
  cam::Operation bad = op;
  bad.id = 5;
  bad.targets = {cam::Target{0, ElementRef{walls[0].featureId, "no/such/a"}},
                 cam::Target{0, ElementRef{walls[0].featureId, "no/such/b"}}};
  none.operations.push_back(bad);
  const auto nothing = cam::GenerateAll(none, doc->GetBodies());
  REQUIRE(nothing.size() == 1);
  CHECK(nothing[0].moves.empty());
  INFO(nothing[0].error);
  CHECK(nothing[0].error.find("no target of this operation") !=
        std::string::npos);
}

TEST_CASE("cam: a section is remembered for one generate and never confused",
          "[cam]") {
  // SectionMemoScope: the same (bodies, z) answered once per generate --
  // and a moved copy of the same shape, which shares its TShape, is a
  // different question with a different answer.
  auto doc = BlockDoc(40, 30, 20);
  const std::vector<Body>& bodies = doc->GetBodies();
  const std::vector<cam::Loop> fresh = cam::SectionLoops(bodies, 10.0);
  REQUIRE(!fresh.empty());
  std::vector<Body> moved = bodies;
  moved[0].shape = bodies[0].shape.Transformed(
      glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 0.0f, 0.0f)));
  const cam::SectionMemoScope scope;
  const std::vector<cam::Loop> first = cam::SectionLoops(bodies, 10.0);
  const std::vector<cam::Loop> again = cam::SectionLoops(bodies, 10.0);
  const std::vector<cam::Loop> other = cam::SectionLoops(moved, 10.0);
  REQUIRE(first.size() == fresh.size());
  REQUIRE(again.size() == fresh.size());
  CHECK(again.front().pts == fresh.front().pts);
  REQUIRE(!other.empty());
  double minX = 1e9;
  for (const glm::dvec2& p : other.front().pts)
    minX = std::min(minX, p.x);
  CHECK(minX > 99.0); // the moved block's section, not the memo's
  // ...and a different height is a different section.
  const std::vector<cam::Loop> higher = cam::SectionLoops(bodies, 25.0);
  CHECK(higher.empty()); // above the block
}

#endif // HAVE_CAM
