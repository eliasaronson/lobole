#ifdef HAVE_CAM

// The rotary stock: r(x, theta) instead of z(x, y)
// (docs/cam/rotary-plan.md, D1).
//
// Every number here is one a machinist could check by hand, because the
// failure this model exists to prevent is a verdict about a program nobody
// checked -- which is what a 3-axis height map gives you on a part that
// turns, and what one surveyed project ships today.

#include "cam_fixtures.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamSurface.h"

namespace {

// A cylinder lying ON the rotary axis: setup-space +X through the origin.
// Radius 20, from x = 0 to x = 60.
std::unique_ptr<Document> BarDoc(double radius = 20.0, double length = 60.0) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Bar", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), radius, length));
  return doc;
}

double MinRadius(const cam::RadiusMap& m) {
  double lo = 1e18;
  for (float v : m.r)
    lo = std::min(lo, static_cast<double>(v));
  return lo;
}
double MaxRadius(const cam::RadiusMap& m) {
  double hi = 0.0;
  for (float v : m.r)
    hi = std::max(hi, static_cast<double>(v));
  return hi;
}

} // namespace

TEST_CASE("cam rotary: the blank is a cylinder, and its grid is derived",
          "[cam]") {
  const cam::RadiusMap stock = cam::MakeRotaryStock(0.0, 60.0, 20.0, 0.5);
  CHECK(stock.nx == 120); // 60 mm at 0.5
  // The angular step is not a number anybody typed: enough cells that one
  // cell's ARC at the blank's own surface is about `res`. 2*pi*20 / 0.5 = 252.
  CHECK(stock.nTheta == 252);
  CHECK_THAT(stock.ArcAt(20.0), WithinAbs(0.5, 0.01));
  // ...so the same res on a bar twice as thick buys twice the cells, which is
  // the whole reason to derive it. A fixed step measures a 200 mm blank four
  // times more coarsely than a 50 mm one and says nothing about it.
  const cam::RadiusMap fat = cam::MakeRotaryStock(0.0, 60.0, 40.0, 0.5);
  CHECK_THAT(fat.ArcAt(40.0), WithinAbs(0.5, 0.01));
  CHECK(fat.nTheta > 1.9 * stock.nTheta);
  CHECK_THAT(MinRadius(stock), WithinAbs(20.0, 1e-6));
  CHECK_THAT(MaxRadius(stock), WithinAbs(20.0, 1e-6));
}

TEST_CASE("cam rotary: a bar on the axis samples as its own radius", "[cam]") {
  // The control for everything below: if the sampler cannot see a plain
  // cylinder as a constant radius, no comparison built on it means anything.
  auto doc = BarDoc(20.0, 60.0);
  const cam::RadiusMap like = cam::MakeRotaryStock(0.0, 60.0, 25.0, 0.5);
  const cam::RadiusMap model = cam::SampleModelRotary(doc->GetBodies(), like);

  // Away from the ends, every cell is the bar's radius. The ends are excluded
  // because a cell there straddles the flat face, where the furthest point
  // along the ray is the rim and the cell's own centre is past it.
  double lo = 1e18, hi = 0.0;
  for (int i = 4; i < model.nx - 4; i++)
    for (int j = 0; j < model.nTheta; j++) {
      lo = std::min(lo, static_cast<double>(model.At(i, j)));
      hi = std::max(hi, static_cast<double>(model.At(i, j)));
    }
  INFO("sampled radius " << lo << " .. " << hi);
  CHECK_THAT(lo, WithinAbs(20.0, 0.05));
  CHECK_THAT(hi, WithinAbs(20.0, 0.05));
}

TEST_CASE("cam rotary: a cutter held still takes a flat off a turning bar",
          "[cam]") {
  // The defining case of a 4th axis, and the one a height map cannot express
  // at all: the tool does not move, the PART does. A cutter parked at Z15
  // over a Ø40 bar, while the bar turns a full revolution, leaves a bar with
  // a 15 mm radius everywhere -- it has been turned down, not flatted.
  cam::Tool tool;
  tool.number = 1;
  tool.diameter = 10.0;
  cam::RadiusMap stock = cam::MakeRotaryStock(0.0, 60.0, 20.0, 0.5);
  const glm::dvec3 at(30.0, 0.0, 15.0);
  cam::CarveSweepRotary(at, at, 0.0, 360.0, &tool, stock);

  // Under the cutter (x within its radius of 30) the bar is 15 mm. The
  // material outside that band is untouched, which is what makes it a turned
  // GROOVE and not a smaller bar.
  const int mid = static_cast<int>((30.0 - stock.x0) / stock.res);
  double grooveLo = 1e18, grooveHi = 0.0;
  for (int j = 0; j < stock.nTheta; j++) {
    grooveLo = std::min(grooveLo, static_cast<double>(stock.At(mid, j)));
    grooveHi = std::max(grooveHi, static_cast<double>(stock.At(mid, j)));
  }
  INFO("groove " << grooveLo << " .. " << grooveHi);
  CHECK_THAT(grooveLo, WithinAbs(15.0, 0.02));
  CHECK_THAT(grooveHi, WithinAbs(15.0, 0.02));
  // Ten millimetres of cutter on a 60 mm bar: the ends are untouched.
  const int end = static_cast<int>((2.0 - stock.x0) / stock.res);
  for (int j = 0; j < stock.nTheta; j++)
    CHECK_THAT(stock.At(end, j), WithinAbs(20.0, 1e-6));
}

TEST_CASE("cam rotary: a flat is a flat, at every angle it was cut", "[cam]") {
  // The other half of the same idea: hold the part still and the cutter
  // leaves a PLANE, not an arc. A Ø40 bar with the cutter at Z15 and no
  // rotation gets one flat 15 mm from the axis -- so the radius at theta
  // must rise as 15 / sin(theta) either side of the top, which is the
  // arithmetic of a chord and is checkable by hand.
  cam::Tool tool;
  tool.number = 1;
  tool.diameter = 30.0; // wide enough to cut the whole flat in one pass
  cam::RadiusMap stock = cam::MakeRotaryStock(0.0, 60.0, 20.0, 0.5);
  const glm::dvec3 at(30.0, 0.0, 15.0);
  cam::CarveSweepRotary(at, at, 0.0, 0.0, &tool, stock);

  const int mid = static_cast<int>((30.0 - stock.x0) / stock.res);
  int checked = 0;
  for (int j = 0; j < stock.nTheta; j++) {
    const double theta = stock.CellTheta(j);
    const double s = std::sin(theta);
    if (s < 0.8) // only where the flat is, not round the back
      continue;
    const double want = std::min(20.0, 15.0 / s);
    INFO("theta " << glm::degrees(theta) << " want " << want << " got "
                  << stock.At(mid, j));
    CHECK_THAT(stock.At(mid, j), WithinAbs(want, 0.05));
    checked++;
  }
  CHECK(checked > 20);
  // Round the back, nothing was touched -- the cutter comes from +Z only.
  const int bottom = static_cast<int>(0.75 * stock.nTheta);
  CHECK_THAT(stock.At(mid, bottom), WithinAbs(20.0, 1e-6));
}

TEST_CASE("cam rotary: the cutter can pass the centreline", "[cam]") {
  // A rotary job really does part a bar off, and then the ray from the axis
  // outward is gone entirely rather than reduced. The bisection cannot find
  // that boundary -- there isn't one -- so it is answered before searching,
  // and this is the case that says so.
  cam::Tool tool;
  tool.number = 1;
  tool.diameter = 4.0;
  cam::RadiusMap stock = cam::MakeRotaryStock(0.0, 60.0, 20.0, 0.5);
  const glm::dvec3 at(30.0, 0.0, -2.0); // tip two millimetres PAST the axis
  cam::CarveSweepRotary(at, at, 0.0, 360.0, &tool, stock);

  const int mid = static_cast<int>((30.0 - stock.x0) / stock.res);
  for (int j = 0; j < stock.nTheta; j++) {
    INFO("theta cell " << j);
    CHECK_THAT(stock.At(mid, j), WithinAbs(0.0, 1e-6));
  }
}

TEST_CASE("cam rotary: material only ever comes off", "[cam]") {
  // The invariant the whole model rests on, and the one a bug in the
  // bisection would break silently: a carve can lower a cell and can never
  // raise one. Checked over a move that translates AND turns at once, which
  // is the helix a real rotary pass cuts and the only case where the step
  // count derivation matters.
  cam::Tool tool;
  tool.number = 1;
  tool.diameter = 6.0;
  cam::RadiusMap stock = cam::MakeRotaryStock(0.0, 60.0, 20.0, 0.5);
  const std::vector<float> before = stock.r;
  cam::CarveSweepRotary({5.0, 0.0, 17.0}, {55.0, 0.0, 17.0}, 0.0, 720.0, &tool,
                        stock);
  REQUIRE(stock.r.size() == before.size());
  for (size_t k = 0; k < before.size(); k++)
    REQUIRE(stock.r[k] <= before[k] + 1e-6);
  // ...and it did something: two turns along the bar is a helical cut, so
  // the bar is 17 mm where it passed and 20 where it did not.
  INFO("after: " << MinRadius(stock) << " .. " << MaxRadius(stock));
  CHECK_THAT(MinRadius(stock), WithinAbs(17.0, 0.05));
  CHECK_THAT(MaxRadius(stock), WithinAbs(20.0, 1e-6));
}

TEST_CASE("cam rotary: a turned-down bar verifies clean, an overcut does not",
          "[cam]") {
  // The closed loop, which is the only thing that makes any of the above
  // worth having: a G-code file in, a verdict out, measured against the model
  // the part was designed as. The program turns a Phi 40 bar down to Phi 30
  // over its middle, which is what the MODEL is, so the gouge is zero -- and
  // the same program cutting one millimetre deeper is scrap, by one
  // millimetre, which is the number the check has to produce rather than a
  // flag.
  //
  // `VerifyGcode` routes it: the file's A moves, so the rotary stock model
  // answers. Nothing here asks for that, which is the point -- a caller
  // cannot verify a rotary program against a height map by forgetting.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Blank", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 20.0, 60.0));
  const uint64_t blank = doc->GetFeature(0)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Journal", glm::vec3(20, 0, 0), glm::vec3(1, 0, 0), 20.0, 20.0));
  const uint64_t big = doc->GetFeature(1)->GetId();
  doc->AddFeature(
      std::make_unique<BooleanFeature>("Cut", blank, big, BooleanOp::Subtract));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Small", glm::vec3(20, 0, 0), glm::vec3(1, 0, 0), 15.0, 20.0));
  const uint64_t small = doc->GetFeature(3)->GetId();
  const uint64_t cut = doc->GetFeature(2)->GetId();
  doc->AddFeature(
      std::make_unique<BooleanFeature>("Shaft", cut, small, BooleanOp::Union));
  REQUIRE(doc->GetBodies().size() == 1);

  cam::Setup setup = BasicSetup();
  setup.stock.margin = {0.0, 0.0, 0.0};
  setup.stock.bottomMargin = 0.0;
  setup.tools[0].diameter = 6.0;

  // A cutter walking the journal at a fixed height while the bar turns: the
  // classic rotary roughing pass, written by hand because nothing generates
  // one yet.
  const auto program = [](double z) {
    // Approach at X23 too, not X20: the plunge is a cut like any
    // other, and a 6 mm cutter entering at the journal's edge takes 3 mm
    // out of the shoulder beside it. The check said so.
    std::string g = "(op 1)\nG0 Z40\nG0 X23 Y0 A0\n";
    char buf[128];
    std::snprintf(buf, sizeof(buf), "G1 Z%.3f F300\n", z);
    g += buf;
    // The cutter's CENTRE runs 23 to 37, not 20 to 40: it is 6 mm across, so
    // that is exactly what covers the journal and nothing either side. The
    // first version swept the centre to the shoulders and cut 3 mm into
    // them -- 5 mm of gouge on a program that looked right, which is the
    // check doing its job on the test rather than the other way round.
    for (int k = 1; k <= 72; k++) {
      std::snprintf(buf, sizeof(buf), "G1 X%.3f A%.3f\n", 23.0 + 14.0 * (k % 2),
                    5.0 * k);
      g += buf;
    }
    return g + "G0 Z40\n";
  };

  const cam::Verdict good =
      cam::VerifyGcode(program(15.0), setup, doc->GetBodies(), 0.5);
  INFO("rotary? " << good.rotary << " gouge " << good.maxGouge << " leftover "
                  << good.maxLeftover << " cells " << good.cells);
  REQUIRE(good.rotary); // it routed itself
  REQUIRE(good.cells > 1000);
  CHECK(good.maxGouge < 0.2);

  const cam::Verdict deep =
      cam::VerifyGcode(program(14.0), setup, doc->GetBodies(), 0.5);
  INFO("deep gouge " << deep.maxGouge);
  CHECK_THAT(deep.maxGouge, WithinAbs(1.0, 0.1));
  CHECK_FALSE(deep.Safe(0.05));

  // ...and the words say A, not Y. An angle printed under a "Y" is the kind
  // of wrong that reads as right.
  bool labelled = false;
  for (const cam::VerdictLine& l : cam::Explain(deep, 0.05, 1.0))
    labelled = labelled || l.text.find(" A") != std::string::npos;
  CHECK(labelled);
}

TEST_CASE("cam rotary: a rapid through the bar is found", "[cam]") {
  // The one failure a shape comparison cannot make: the part comes out
  // perfect and the tool is broken. It is also the check that earned its
  // keep on the planar side -- 121 rapids through material on a waveguide
  // whose gouge was zero, and 16 after the fix -- so a rotary check without
  // it would be the weaker half of the pair.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Bar", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 20.0, 60.0));
  cam::Setup setup = BasicSetup();
  setup.stock.margin = {0.0, 0.0, 0.0};
  setup.tools[0].diameter = 6.0;

  // Clear of the bar all the way across, then turned so the file is rotary:
  // nothing to hit at Z25 over a Ø40 blank.
  const cam::Verdict clear =
      cam::VerifyGcode("(op 1)\nG0 Z25 A0\nG0 X5 Y0\nG0 X55 A90\nG0 Z25 A180\n",
                       setup, doc->GetBodies(), 0.5);
  REQUIRE(clear.rotary);
  CHECK(clear.rapidsThroughStock == 0);

  // The same traverse five millimetres INTO the blank. A rapid at Z15 over a
  // 20 mm radius buries the cutter by five, and the depth is what makes the
  // finding actionable -- a count alone reads the same for a graze and a
  // crash.
  const cam::Verdict crash =
      cam::VerifyGcode("(op 1)\nG0 Z15 A0\nG0 X5 Y0\nG0 X55 A90\nG0 Z25 A180\n",
                       setup, doc->GetBodies(), 0.5);
  INFO("rapids " << crash.rapidsThroughStock << " worst "
                 << crash.worstRapidDepth << " at x" << crash.worstRapidAt.x
                 << " A" << crash.worstRapidAt.y << " line "
                 << crash.worstRapidLine);
  CHECK(crash.rapidsThroughStock > 0);
  CHECK_THAT(crash.worstRapidDepth, WithinAbs(5.0, 0.2));
  CHECK(crash.worstRapidLine > 0);
  CHECK_FALSE(crash.Safe(0.05));
  // ...and it removed NOTHING doing so. A rapid is checked against the stock
  // and never carves it -- which is what lets the check run at all, since
  // the material has to still be there to be asked about. The blank comes
  // out exactly the bar it started as.
  CHECK_THAT(crash.maxLeftover, WithinAbs(0.0, 0.1));
  CHECK_THAT(crash.maxGouge, WithinAbs(0.0, 0.1));
}

TEST_CASE("cam rotary: turning the part takes time", "[cam]") {
  // A pure A move is zero millimetres in XYZ, so it fell out of
  // `CuttingMinutes` entirely and a rotary program's cycle time came back
  // understated -- silently, which is the part that matters.
  //
  // Half a turn of a bar under a cutter standing 20 mm off the axis is
  // pi * 20 = 62.8 mm of metal past the edge. At 600 mm/min that is 6.3
  // seconds, and it used to be none.
  cam::Setup setup = BasicSetup();
  cam::Toolpath tp;
  tp.toolNumber = 1;
  cam::Move start;
  start.kind = cam::Move::Kind::Cut;
  start.to = {10.0, 0.0, 20.0};
  start.feed = 600.0;
  start.hasA = true;
  start.a = 0.0;
  cam::Move turn = start;
  turn.a = 180.0; // same point, half a turn of the part
  tp.moves = {start, turn};

  const double minutes = cam::CuttingMinutes(setup, {tp});
  INFO("minutes " << minutes);
  CHECK_THAT(minutes, WithinRel(glm::pi<double>() * 20.0 / 600.0, 0.01));

  // A 3-axis toolpath is unchanged -- the rotary term only exists when two
  // moves state DIFFERENT angles.
  cam::Toolpath plain = tp;
  plain.moves[1].a = 0.0;
  CHECK_THAT(cam::CuttingMinutes(setup, {plain}), WithinAbs(0.0, 1e-12));
}

TEST_CASE("cam rotary: the CL surface is the part plus the cutter", "[cam]") {
  // The drop cutter's arithmetic, in the rotary frame. A BALL nose resting
  // on a plain cylinder sits one tool radius shallower than the surface: its
  // tip is at r_model - 0, and its widest point touches nothing, so the tip
  // radius equals the part's. That is the boring case and it is the control.
  //
  // The case that measures something is a STEP: a ball cannot reach into a
  // corner, so the CL surface rounds it off by the tool's own radius, which
  // is the scallop a finish pass is allowed to leave and the reason the
  // generator plans against this surface instead of the part.
  cam::RadiusMap model = cam::MakeRotaryStock(0.0, 40.0, 20.0, 0.5);
  // Turn the second half down to 15: a 5 mm step at x = 20.
  for (int i = 0; i < model.nx; i++)
    if (model.CellX(i) > 20.0)
      for (int j = 0; j < model.nTheta; j++)
        model.Set(i, j, 15.0f);

  cam::Tool ball;
  ball.number = 1;
  ball.diameter = 6.0;
  ball.type = cam::ToolType::Ball;
  const cam::RadiusMap cl = cam::RotaryCL(model, ball);

  // Far from the step the tip sits on the surface itself.
  const int big = static_cast<int>((5.0 - cl.x0) / cl.res);
  const int small = static_cast<int>((35.0 - cl.x0) / cl.res);
  CHECK_THAT(cl.At(big, 0), WithinAbs(20.0, 0.05));
  CHECK_THAT(cl.At(small, 0), WithinAbs(15.0, 0.05));

  // Approaching the step from the SMALL side the tool is held out by the
  // shoulder: within one radius of x = 20 the tip cannot come down to 15.
  const int near = static_cast<int>((21.5 - cl.x0) / cl.res);
  INFO("near the step: " << cl.At(near, 0));
  CHECK(cl.At(near, 0) > 15.5);
  CHECK(cl.At(near, 0) < 20.0);
  // ...and three millimetres past it, the ball has cleared the corner.
  const int clear = static_cast<int>((24.0 - cl.x0) / cl.res);
  CHECK_THAT(cl.At(clear, 0), WithinAbs(15.0, 0.05));

  // A FLAT mill of the same size is held out further at the same place: its
  // corner reaches the shoulder a whole radius away, where the ball's does
  // not. The two tools disagreeing here is the whole reason `TipHeight` is
  // one function rather than an assumption per call site.
  cam::Tool flat = ball;
  flat.type = cam::ToolType::Flat;
  const cam::RadiusMap flatCL = cam::RotaryCL(model, flat);
  INFO("flat " << flatCL.At(near, 0) << " ball " << cl.At(near, 0));
  CHECK(flatCL.At(near, 0) > cl.At(near, 0));
  CHECK_THAT(flatCL.At(near, 0), WithinAbs(20.0, 0.05));
}

TEST_CASE("cam rotary: the raster steps by the arc it was asked for", "[cam]") {
  cam::RadiusMap cl = cam::MakeRotaryStock(0.0, 60.0, 20.0, 0.5);
  cam::RotaryParallelRequest req;
  req.stepover_mm = 2.0;
  const std::vector<cam::RotaryPass> passes =
      cam::RotaryParallelPasses(cl, req);
  REQUIRE(passes.size() > 4);

  // 2 mm of arc round a Ø40 bar is 2 / (pi * 40) of a turn: about 57 passes.
  const double want = 2.0 * glm::pi<double>() * 20.0 / 2.0;
  INFO("passes " << passes.size() << " want about " << want);
  CHECK(std::abs(static_cast<double>(passes.size()) - want) < want * 0.1);

  // Each pass runs the length of the bar, and they alternate direction --
  // a finishing raster is a zigzag, which is why `climb` means nothing on
  // one and is refused rather than offered.
  CHECK(passes[0].pts.front().x < passes[0].pts.back().x);
  CHECK(passes[1].pts.front().x > passes[1].pts.back().x);
  for (const cam::RotaryPass& p : passes)
    CHECK(p.pts.size() > 100);

  // Halving the stepover doubles the passes. The one property of a finishing
  // pattern that has to hold, and the one a fixed angular step would break.
  req.stepover_mm = 1.0;
  const size_t finer = cam::RotaryParallelPasses(cl, req).size();
  INFO("finer " << finer << " vs " << passes.size());
  CHECK(finer > 1.8 * passes.size());

  // A gap in the surface BREAKS a pass rather than being cut across. Zero
  // the middle third of the bar and every pass comes back as two.
  for (int i = 0; i < cl.nx; i++)
    if (cl.CellX(i) > 20.0 && cl.CellX(i) < 40.0)
      for (int j = 0; j < cl.nTheta; j++)
        cl.Set(i, j, 0.0f);
  req.stepover_mm = 2.0;
  const std::vector<cam::RotaryPass> broken =
      cam::RotaryParallelPasses(cl, req);
  INFO("broken " << broken.size() << " from " << passes.size());
  CHECK(broken.size() == 2 * passes.size());
  for (const cam::RotaryPass& p : broken)
    for (const glm::dvec2& q : p.pts)
      CHECK((q.x < 20.5 || q.x > 39.5));
}

TEST_CASE("cam rotary: generate, post and verify a turned part", "[cam]") {
  // The closed loop, and the only thing that makes a strategy worth having:
  // the generator plans against a cutter-location surface, the post writes a
  // file, and the VERIFIER -- which shares nothing with the generator but
  // `TipHeight` -- reads that file back and measures what it removed against
  // the model it was supposed to make.
  //
  // The part is a bar with a Phi 30 journal turned into a Phi 40 blank: two
  // diameters and two shoulders, which is the smallest shape with a corner a
  // ball cannot reach, so the leftover it reports is a real number rather
  // than zero by construction.
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Blank", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 20.0, 60.0));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Waist", glm::vec3(20, 0, 0), glm::vec3(1, 0, 0), 20.0, 20.0));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Necked", doc->GetFeature(0)->GetId(), doc->GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Journal", glm::vec3(20, 0, 0), glm::vec3(1, 0, 0), 15.0, 20.0));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Shaft", doc->GetFeature(2)->GetId(), doc->GetFeature(3)->GetId(),
      BooleanOp::Union));
  REQUIRE(doc->GetBodies().size() == 1);

  cam::Setup setup = BasicSetup();
  setup.machine.name = "trunnion";
  setup.machine.hasRotaryAxis = true;
  setup.stock.margin = {0.0, 0.0, 0.0};
  setup.safeZ = 40.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);

  // Derived, not typed -- the same route a click takes.
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::SurfaceRotary, cam::Target{});
  INFO(d.why);
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  op.stepover = 0.25; // 1.5 mm of arc: coarse, so the case stays quick
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  REQUIRE(paths[0].moves.size() > 500);
  // Every move states an A, and consecutive passes state different ones --
  // which is what makes this a 4-axis program rather than an indexed one.
  std::set<double> angles;
  for (const cam::Move& m : paths[0].moves) {
    REQUIRE(m.hasA);
    angles.insert(std::round(m.a * 100.0) / 100.0);
  }
  INFO("distinct A values " << angles.size());
  CHECK(angles.size() > 30);

  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, gcode, &error));
  CHECK(gcode.find(" A") != std::string::npos);

  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.5);
  INFO("rotary " << v.rotary << " gouge " << v.maxGouge << " leftover "
                 << v.maxLeftover << " rapids " << v.rapidsThroughStock
                 << " cells " << v.cells);
  REQUIRE(v.rotary); // it routed itself, because the file's A moves
  REQUIRE(v.cells > 1000);
  // The generator plans against the CL surface, so it must not cut into the
  // part. This is the assertion the whole file exists for.
  CHECK(v.maxGouge < 0.1);
  // ...and it leaves the inside corners a 6 mm ball cannot reach, which is
  // the right answer and not zero: 3 mm of radius in each of two shoulders.
  CHECK(v.maxLeftover > 0.5);
  CHECK(v.maxLeftover < 3.5);
}

#endif // HAVE_CAM
