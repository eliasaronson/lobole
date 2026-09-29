#ifdef HAVE_CAM

// The rotary stock: r(x, theta) instead of z(x, y)
// (docs/cam/rotary.md#verifying-a-turning-part).
//
// Every number here is one a machinist could check by hand, because the
// failure this model exists to prevent is a verdict about a program nobody
// checked -- which is what a 3-axis height map gives you on a part that
// turns, and what one surveyed project ships today.

#include "cam_fixtures.h"
#include "core/cam/CamOtherSetups.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamSurface.h"

#include <chrono>

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

TEST_CASE("cam rotary: a leftover ring is said as a ring, a patch at its angle",
          "[cam]") {
  // The two leftovers a turned part shows and what each means: a RING all
  // the way round is a corner no pass reaches; a PATCH at one angle is a
  // pass missing there. The region's angle was the vector average of its
  // cells', which a ring cancels to rounding noise -- so the chess blanks'
  // plug corners read as patches "at A131.5", every one at the same angle,
  // and sent the search after a skipped pass that did not exist.
  cam::RadiusMap designed = cam::MakeRotaryStock(0.0, 20.0, 10.0, 0.5);
  REQUIRE(designed.nx == 40);
  const int n = designed.nTheta;
  REQUIRE(n > 60);
  cam::RadiusMap machined = designed;
  const auto proud = [&](int i, int j) {
    machined.Set(i, (j + n) % n, machined.At(i, (j + n) % n) + 0.5f);
  };
  for (int j = 0; j < n; j++)
    proud(10, j); // a ring, one cell wide
  for (int i = 20; i <= 22; i++)
    for (int j = 20; j <= 24; j++)
      proud(i, j); // a patch, five rows round
  for (int i = 30; i <= 31; i++)
    for (int j = -2; j <= 1; j++)
      proud(i, j); // a patch ACROSS the seam

  const cam::Verdict v = cam::CompareRotary(machined, designed, nullptr);
  REQUIRE(v.leftoverRegions == 3);
  REQUIRE(v.biggest.size() == 3);
  const double step = 360.0 / n;
  int rings = 0;
  for (const cam::Verdict::Region& r : v.biggest) {
    INFO("x " << r.at.x << " at " << r.at.y << " span " << r.spanDeg);
    if (std::abs(r.at.x - designed.CellX(10)) < 1e-6) {
      rings++;
      CHECK(r.ring);
      CHECK(std::abs(r.spanDeg - (360.0)) < 1e-6);
      CHECK(r.at.y == 0.0);
    } else if (std::abs(r.at.x - designed.CellX(21)) < 1e-6) {
      CHECK_FALSE(r.ring);
      CHECK(std::abs(r.spanDeg - (5 * step)) < 1e-6);
      CHECK(std::abs(r.at.y - (glm::degrees(designed.CellTheta(22)))) < 1e-6);
    } else {
      CHECK(std::abs(r.at.x - 0.5 * (designed.CellX(30) + designed.CellX(31))) <
            1e-6);
      CHECK_FALSE(r.ring);
      CHECK(std::abs(r.spanDeg - (4 * step)) < 1e-6);
      // Rows n-2, n-1, 0, 1: the middle is the seam itself.
      const double off = std::min(r.at.y, 360.0 - r.at.y);
      CHECK(off < 1e-6);
    }
  }
  CHECK(rings == 1);
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
  auto doc = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 15.0);
  REQUIRE(doc->GetBodies().size() == 1);

  cam::Setup setup = BasicSetup();
  setup.machine.name = "trunnion";
  RotaryMachine(setup);
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

TEST_CASE("cam rotary: a turning program is a timeline the slider can cross",
          "[cam]") {
  // It used to be REFUSED here, which was the honest answer while the only
  // stock was a height map. It is simulated now, on a radius field, so the
  // timeline has to carry what the picture needs: that it is rotary, where A
  // is part way through a segment, and a LENGTH for a rotation.
  const cam::Timeline turning =
      cam::BuildTimeline(cam::ParseGcode("(op 1)\nG0 Z25 A0\nG1 Z20 F300\n"
                                         "G1 A180\nG1 X10 A270\n"),
                         0.0);
  CHECK(turning.unsupported.empty());
  CHECK(turning.rotary);
  REQUIRE(turning.Segments() == 3);

  // A is interpolated along a segment exactly as the position is.
  CHECK_THAT(turning.AngleAt({1, 0.0}), WithinAbs(0.0, 1e-9));
  CHECK_THAT(turning.AngleAt({1, 0.5}), WithinAbs(90.0, 1e-9));
  CHECK_THAT(turning.AngleAt({1, 1.0}), WithinAbs(180.0, 1e-9));

  // Half a turn with the tool 20 mm off the axis is pi * 20 of metal past the
  // edge, and it is a distance on the slider -- zero here and the slider
  // would jump straight across every rotation.
  const double halfTurn = turning.distance[1] - turning.distance[0];
  CHECK_THAT(halfTurn, WithinAbs(glm::pi<double>() * 20.0, 1e-6));

  // A 3-axis and an indexed file are not rotary: the part stands still.
  CHECK_FALSE(
      cam::BuildTimeline(cam::ParseGcode("G0 Z25 A90\nG1 X10 F300\n"), 0.0)
          .rotary);
  CHECK_FALSE(
      cam::BuildTimeline(cam::ParseGcode("G0 Z25\nG1 X10 F300\n"), 0.0).rotary);
}

namespace {

// The rolling pin's program in miniature: a bar, a ball, passes along the
// axis stepping round it -- generated and posted for real, so the simulator
// is fed exactly what the verifier is.
struct RotaryJob {
  std::unique_ptr<Document> doc;
  cam::Setup setup;
  std::string gcode;
};

RotaryJob SmallRotaryJob() {
  RotaryJob j;
  j.doc = NeckedShaftDoc();
  REQUIRE(j.doc->GetBodies().size() == 1);
  j.setup = BasicSetup();
  RotaryMachine(j.setup);
  j.setup.stock.margin = {0.0, 1.0, 1.0};
  j.setup.safeZ = 30.0;
  j.setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::SurfaceRotary;
  op.toolNumber = 1;
  op.stepover = 0.5;
  j.setup.operations.push_back(op);
  const auto paths = cam::GenerateAll(j.setup, j.doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  std::string error;
  REQUIRE(cam::PostProcess("linuxcnc", j.setup, paths, {}, j.gcode, &error));
  return j;
}

} // namespace

TEST_CASE("cam rotary: playing to the end leaves the verifier's own stock",
          "[cam]") {
  // The load-bearing equality, the rotary twin of the planar simulator's
  // first test. The picture and the verdict come from ONE engine: the same
  // blank (`MakeRotaryBlank`), the same carve (`CarveSweepRotary`), the same
  // posted file. If they ever diverge, what is on screen stops being evidence
  // and becomes an impression -- which is the thing FreeCAD's two simulators
  // are, and the reason this one was built as one.
  RotaryJob job = SmallRotaryJob();
  constexpr double kRes = 0.5;
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ProgramInSetupSpace(job.gcode, job.setup), 0.0);
  REQUIRE(tl.rotary);

  cam::RotaryStockSim sim;
  const std::vector<Body> bodies =
      cam::BodiesInSetupFrame(job.setup, job.doc->GetBodies());
  sim.Begin(tl, job.setup, cam::MakeRotaryBlank(job.setup, bodies, kRes));
  REQUIRE(sim.Ready());
  sim.SeekTo(tl.End());

  const cam::RotaryInspection ri =
      cam::InspectRotary(job.gcode, job.setup, job.doc->GetBodies(), kRes);
  REQUIRE(ri.ok);
  REQUIRE(ri.machined.r.size() == sim.Stock().r.size());
  size_t differ = 0;
  for (size_t k = 0; k < ri.machined.r.size(); k++)
    if (ri.machined.r[k] != sim.Stock().r[k])
      differ++;
  INFO(differ << " of " << ri.machined.r.size() << " cells differ");
  CHECK(differ == 0); // bit-identical, not "close"
  CHECK(sim.RemovedVolume() > 100.0);
}

TEST_CASE("cam rotary: scrubbing back lands where playing forward does",
          "[cam]") {
  // Backward cannot un-cut, so it restarts from a SNAPSHOT -- and a snapshot
  // is only correct if it holds exactly "every segment before this one
  // carved, and this one untouched". Get that wrong and scrubbing back shows
  // material the cursor has not reached yet. So: play to the end, scrub back
  // to several cursors (whole segments and part way into them), and each must
  // match a fresh simulation played forward to the same place.
  RotaryJob job = SmallRotaryJob();
  constexpr double kRes = 0.5;
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ProgramInSetupSpace(job.gcode, job.setup), 0.0);
  const std::vector<Body> bodies =
      cam::BodiesInSetupFrame(job.setup, job.doc->GetBodies());
  const cam::RadiusMap blank = cam::MakeRotaryBlank(job.setup, bodies, kRes);

  cam::RotaryStockSim scrubbed;
  scrubbed.Begin(tl, job.setup, blank);
  scrubbed.SeekTo(tl.End());
  INFO("snapshots every " << scrubbed.KeyframeEvery() << " segments, "
                          << scrubbed.Keyframes() << " kept");
  REQUIRE(scrubbed.Keyframes() >= 2); // or this is not testing snapshots

  const size_t n = tl.Segments();
  for (const cam::SimCursor at :
       {cam::SimCursor{n / 2, 0.0}, cam::SimCursor{n / 2, 0.37},
        cam::SimCursor{scrubbed.KeyframeEvery(), 0.0},
        cam::SimCursor{scrubbed.KeyframeEvery(), 0.6},
        cam::SimCursor{n / 7, 0.9}, cam::SimCursor{1, 0.0}}) {
    scrubbed.SeekTo(at);
    cam::RotaryStockSim forward;
    forward.Begin(tl, job.setup, blank);
    forward.SeekTo(at);
    size_t differ = 0;
    for (size_t k = 0; k < forward.Stock().r.size(); k++)
      if (forward.Stock().r[k] != scrubbed.Stock().r[k])
        differ++;
    INFO("cursor " << at.seg << "+" << at.t << ": " << differ
                   << " cells differ");
    CHECK(differ == 0);
  }
}

TEST_CASE("cam rotary: the stock mesh is a closed tube facing outward",
          "[cam]") {
  // Two gates the planar block already has, for the tube. Winding: every
  // triangle's geometric normal agrees with its vertices' -- the glyph was
  // inside out for a month and three rounds of screenshots missed it. And
  // closure: by the divergence theorem the mesh encloses the radius field's
  // own volume, which it cannot if a cap is missing or wound backwards.
  cam::RadiusMap map = cam::MakeRotaryStock(0.0, 40.0, 15.0, 0.5);
  for (int i = 0; i < map.nx; i++)
    if (map.CellX(i) > 12.0 && map.CellX(i) < 28.0)
      for (int j = 0; j < map.nTheta; j++)
        map.Set(i, j, 11.0f); // a journal, so there is a shoulder to face
  const cam::StockMesh m = cam::BuildRotaryStockMesh(map);
  REQUIRE(!m.Empty());

  int agree = 0, disagree = 0;
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const glm::vec3& a = m.vertices[m.indices[i]].position;
    const glm::vec3& b = m.vertices[m.indices[i + 1]].position;
    const glm::vec3& c = m.vertices[m.indices[i + 2]].position;
    const glm::vec3 geo = glm::cross(b - a, c - a);
    if (glm::length(geo) < 1e-9f)
      continue;
    const glm::vec3 vn = m.vertices[m.indices[i]].normal +
                         m.vertices[m.indices[i + 1]].normal +
                         m.vertices[m.indices[i + 2]].normal;
    (glm::dot(glm::normalize(geo), glm::normalize(vn)) > 0.0f ? agree
                                                              : disagree)++;
  }
  INFO(agree << " agree, " << disagree << " disagree");
  CHECK(agree > 1000);
  CHECK(disagree == 0);

  // The volume the tube encloses, against the field's own: the solid between
  // the first and last cell CENTRES, where the caps sit.
  const double dTheta = 2.0 * glm::pi<double>() / map.nTheta;
  double field = 0.0;
  for (int j = 0; j < map.nTheta; j++)
    for (int i = 0; i + 1 < map.nx; i++) {
      const double r = 0.5 * (map.At(i, j) + map.At(i + 1, j));
      field += 0.5 * r * r * dTheta * map.res;
    }
  INFO("mesh " << m.Volume() << " field " << field);
  CHECK_THAT(m.Volume(), WithinRel(field, 0.01));
}

// RotaryCL skips the cells under the cutter that cannot raise it (the best
// contact already stands within TipHeight of the highest cell around). The
// skip must never change the answer: here it is checked cell for cell against
// the whole kernel, on a surface with steps, a groove, a ridge and a gap off
// the part, for a ball, a flat and a V-bit -- on a grid whose cells round
// the axis are not a whole number of the pruning's blocks, where a kernel
// wrapping past the last cell once read the wrong block.
TEST_CASE("cam rotary: the pruned CL equals the full kernel's",
          "[cam][rotary]") {
  cam::RadiusMap model = cam::MakeRotaryStock(0.0, 30.0, 12.0, 0.1);
  for (int j = 0; j < model.nTheta; j++)
    for (int i = 0; i < model.nx; i++) {
      const double x = model.CellX(i);
      const double th = model.CellTheta(j);
      double r = 8.0 + 0.8 * std::sin(3.0 * th) + 0.05 * x;
      if (x > 10.0 && x < 11.0)
        r -= 2.5; // a groove narrower than the ball
      if (x > 18.0 && x < 18.4)
        r += 1.5; // a thin ridge
      if (x > 26.0)
        r = 0.0; // off the part
      if (x > 21.0 && x <= 26.0) // a SQUARE section: corners, flat faces
        r = 9.0 / std::max(std::abs(std::cos(th)), std::abs(std::sin(th)));
      model.Set(i, j, static_cast<float>(std::max(0.0, r)));
    }
  const auto brute = [&](const cam::Tool& tool) {
    cam::RadiusMap out = model;
    const double R = tool.diameter * 0.5;
    const int n = model.nTheta;
    for (int j = 0; j < n; j++)
      for (int i = 0; i < model.nx; i++) {
        const double arc = model.ArcAt(std::max<double>(model.At(i, j), 0.01));
        const int di = static_cast<int>(std::ceil(R / model.res)) + 1;
        const int dj = std::min(
            static_cast<int>(std::ceil(R / std::max(arc, 1e-6))) + 1, n / 2);
        double best = 0.0;
        bool touched = false;
        for (int a = -di; a <= di; a++)
          for (int b = -dj; b <= dj; b++) {
            const int ni = i + a;
            if (ni < 0 || ni >= model.nx)
              continue;
            const double d2 = a * model.res * a * model.res + b * arc * b * arc;
            if (d2 > R * R)
              continue;
            const double s = model.At(ni, ((j + b) % n + n) % n);
            if (s <= 0.0)
              continue;
            const double tip = s - cam::TipHeight(tool, std::sqrt(d2));
            if (!touched || tip > best) {
              best = tip;
              touched = true;
            }
          }
        out.Set(i, j, static_cast<float>(touched ? best : 0.0));
      }
    return out;
  };
  for (const auto type :
       {cam::ToolType::Ball, cam::ToolType::Flat, cam::ToolType::VBit}) {
    cam::Tool tool;
    tool.number = 1;
    tool.diameter = 3.0;
    tool.type = type;
    tool.tipAngleDeg = 60.0;
    const cam::RadiusMap fast = cam::RotaryCL(model, tool);
    const cam::RadiusMap full = brute(tool);
    REQUIRE(fast.r.size() == full.r.size());
    size_t differ = 0;
    for (size_t k = 0; k < fast.r.size(); k++)
      if (fast.r[k] != full.r[k])
        differ++;
    INFO("tool type " << static_cast<int>(type));
    CHECK(differ == 0);
  }
}

TEST_CASE("cam rotary: the CL surface at the rolling pin's size",
          "[cam][.bench]") {
  // Not a gate -- hidden by `.` -- a stopwatch. The pin's finishing grid:
  // 300 mm at the cusp-derived 0.149 mm, round a Phi 50 blank, under a 6 mm
  // ball. `perf` put RotaryCL and its hypot at ~100% of that generation.
  cam::RadiusMap model = cam::MakeRotaryStock(0.0, 300.0, 25.0, 0.149);
  cam::Tool ball;
  ball.number = 1;
  ball.diameter = 6.0;
  ball.type = cam::ToolType::Ball;
  const auto t0 = std::chrono::steady_clock::now();
  const cam::RadiusMap cl = cam::RotaryCL(model, ball);
  const double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  WARN("RotaryCL " << model.nx << " x " << model.nTheta << " cells: " << s
                   << " s");
  CHECK(cl.nx == model.nx);
}

TEST_CASE("cam rotary: carving a rolling-pin-sized program", "[cam][.bench]") {
  // A stopwatch, hidden by `.`. The verify grid the panel uses on the pin
  // (0.5 mm), and a stream of rotary finishing moves shaped like the real
  // ones: runs along X at one A, stepping A between them. 20,000 moves is
  // about 3% of the pin's 644,160, so the full verify is ~30x this.
  cam::RadiusMap stock = cam::MakeRotaryStock(0.0, 300.0, 27.0, 0.5);
  cam::Tool ball;
  ball.number = 1;
  ball.diameter = 6.0;
  ball.type = cam::ToolType::Ball;
  glm::dvec3 from(0.0, 0.0, 26.0);
  double a = 0.0;
  const auto t0 = std::chrono::steady_clock::now();
  int moves = 0;
  for (int pass = 0; pass < 40 && moves < 20000; pass++) {
    for (int k = 0; k < 500 && moves < 20000; k++) {
      const glm::dvec3 to((pass % 2 == 0 ? k : 500 - k) * 0.6, 0.0, 25.0);
      cam::CarveSweepRotary(from, to, a, a, &ball, stock);
      from = to;
      moves++;
    }
    a += 3.0;
  }
  const double s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  WARN("carved " << moves << " moves on " << stock.nx << " x " << stock.nTheta
                 << " cells: " << s << " s  (" << s / moves * 1e6
                 << " us/move)");
  CHECK(moves == 20000);
}

TEST_CASE(
    "cam rotary: leftover says whether it is on the surface or an end face",
    "[cam]") {
  // docs/cam/rotary.md#verifying-a-turning-part. A rotary answer used to say
  // "0 regions" beside its leftover area, and its one worst cell was an end
  // face on every job -- so a band of passes left out altogether read the
  // same as a correct program.
  auto doc = BarDoc(15.0, 40.0);
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.stock.margin = {0.0, 1.0, 1.0};
  setup.safeZ = 30.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::SurfaceRotary;
  op.toolNumber = 1;
  op.stepover = 0.1; // 0.6 mm of arc: a 15 um scallop, under the floor
  setup.operations.push_back(op);
  std::vector<cam::Toolpath> paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths[0].error.empty());

  std::string gcode;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode));
  const cam::Verdict clean =
      cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25);
  INFO("clean: surface max "
       << clean.leftoverSurfaceMax << " p99 " << clean.leftoverSurfaceP99
       << ", max " << clean.maxLeftover << " in " << clean.leftoverRegions
       << " regions");
  REQUIRE(clean.rotary);
  // The surface itself is finished to the scallop...
  CHECK(clean.leftoverSurfaceMax < 0.05);
  // ...and whatever is left over is on the end faces, and says so.
  for (const cam::Verdict::Region& r : clean.biggest)
    CHECK(r.endFace);

  // Lift every cut between x = 15 and 25 clear of the part: a band the
  // program never touches, which is what a missing operation looks like.
  for (cam::Move& m : paths[0].moves)
    if (m.kind == cam::Move::Kind::Cut && m.to.x > 15.0 && m.to.x < 25.0)
      m.to.z = 25.0;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode));
  const cam::Verdict missed =
      cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25);
  INFO("missed: surface max " << missed.leftoverSurfaceMax << " at X"
                              << missed.leftoverSurfaceAt.x << ", "
                              << missed.leftoverRegions << " regions");
  CHECK(missed.leftoverSurfaceMax > 0.8); // the 1 mm margin, left standing
  CHECK(missed.leftoverSurfaceAt.x > 14.0);
  CHECK(missed.leftoverSurfaceAt.x < 26.0);
  bool band = false;
  for (const cam::Verdict::Region& r : missed.biggest)
    band |= !r.endFace && r.at.x > 15.0 && r.at.x < 25.0 && r.area > 100.0;
  CHECK(band);
  // And the sentence says the SURFACE, not the end faces.
  bool said = false;
  for (const cam::VerdictLine& l : cam::Explain(missed, 0.05, 0.0, false))
    said |= l.text.find("on the surface") != std::string::npos;
  CHECK(said);
}

TEST_CASE("cam rotary: the rough verb cuts in layers and leaves a skin",
          "[cam]") {
  // docs/cam/rotary.md#layers-rest-and-the-rough-verb: Rough on the rotary is
  // the finish's derivation with layers of half the cutter and a skin of a
  // tenth of it. Cut from a blank 4 mm proud of the part, it must not gouge,
  // must leave material for the finish, and must take more than one layer to
  // get there.
  auto doc = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 15.0);
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.stock.margin = {0.0, 4.0, 4.0};
  setup.safeZ = 40.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);

  const cam::DerivedOp d = cam::DeriveRotaryRough(setup, doc->GetBodies());
  INFO(d.why);
  REQUIRE(d.ok);
  CHECK(d.op.type == cam::OpType::SurfaceRotary);
  CHECK(d.op.name == "Rough on the rotary");
  CHECK(d.op.stepdown == 3.0);                      // half of the 6 mm ball
  CHECK(std::abs(d.op.stockToLeave - 0.6) < 1e-12); // a tenth of the cutter

  cam::Operation op = d.op;
  op.id = 1;
  op.stepover = 0.25;
  setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  // Layers: the tool reaches several distinct radii on its cutting moves,
  // where one finishing pass over this part touches two (the diameters).
  std::set<long> radii;
  for (const cam::Move& m : paths[0].moves)
    if (m.kind == cam::Move::Kind::Cut)
      radii.insert(std::lround(m.to.z * 2.0));
  CHECK(radii.size() > 3);

  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.5);
  INFO("gouge " << v.maxGouge << " leftover " << v.maxLeftover);
  REQUIRE(v.rotary);
  CHECK(v.maxGouge < 0.1);
  CHECK(v.maxLeftover > 0.5); // the skin is still there for the finish

  // The finish's refusals are the rough's.
  setup.machine.hasRotaryAxis = false;
  const cam::DerivedOp none = cam::DeriveRotaryRough(setup, doc->GetBodies());
  CHECK_FALSE(none.ok);
  CHECK(none.why.find("4th axis") != std::string::npos);
}

TEST_CASE("cam rotary: the file is written for the rotary as installed, and "
          "read back the same",
          "[cam]") {
  // docs/cam/rotary.md#how-the-rotary-is-installed. A rotary along machine Y
  // posted as one along X moves the machine across a part that lies the
  // other way. Every mount -- either axis, either end, either sign, either
  // letter -- must write a different file that reads back to the SAME
  // setup-space program.
  RotaryJob job = SmallRotaryJob();
  const auto paths = cam::GenerateAll(job.setup, job.doc->GetBodies());
  const auto setupSpace = [&](const cam::Setup& s, std::string& file) {
    std::string error;
    REQUIRE(cam::PostProcess("mach3", s, paths, {}, file, &error));
    return cam::ProgramInSetupSpace(file, s);
  };
  std::string identityFile;
  const cam::GProgram identity = setupSpace(job.setup, identityFile);
  REQUIRE(identity.moves.size() > 20);
  REQUIRE(cam::RotatesWhileCutting(identity));
  for (const char along : {'x', 'y'})
    for (const int end : {-1, 1})
      for (const int topSign : {-1, 1})
        for (const char word : {'A', 'B'}) {
          cam::Setup s = job.setup;
          s.machine.rotary.along = along;
          s.machine.rotary.chuckEnd = end;
          s.machine.rotary.word = word;
          s.machine.rotary.topAxis = along == 'x' ? 'y' : 'x';
          s.machine.rotary.topSign = topSign;
          INFO("along " << along << " end " << end << " top " << topSign
                        << " word " << word);
          std::string file;
          const cam::GProgram back = setupSpace(s, file);
          CHECK(back.warnings.empty());
          REQUIRE(back.moves.size() == identity.moves.size());
          double worst = 0.0;
          for (size_t k = 0; k < back.moves.size(); k++) {
            worst = std::max(
                worst, glm::length(back.moves[k].to - identity.moves[k].to));
            worst = std::max(worst,
                             std::abs(back.moves[k].a - identity.moves[k].a));
          }
          CHECK(worst < 1e-3);
          // Written for THIS machine: its letter, and the part's axis on
          // the axis the rotary lies along.
          CHECK((file.find(std::string(" ") + word) != std::string::npos));
          const char other = word == 'A' ? 'B' : 'A';
          CHECK(file.find(std::string(" ") + other) == std::string::npos);
          const cam::GProgram raw = cam::ParseGcode(file, word);
          double spanX = 0.0, spanY = 0.0;
          double loX = 1e9, hiX = -1e9, loY = 1e9, hiY = -1e9;
          for (const cam::GMove& m : raw.moves) {
            loX = std::min(loX, m.to.x);
            hiX = std::max(hiX, m.to.x);
            loY = std::min(loY, m.to.y);
            hiY = std::max(hiY, m.to.y);
          }
          spanX = hiX - loX;
          spanY = hiY - loY;
          if (along == 'x')
            CHECK(spanX > spanY);
          else
            CHECK(spanY > spanX);
        }
  // The other letter in a file is said, not read.
  const cam::GProgram wrong = cam::ParseGcode(identityFile, 'B');
  CHECK_FALSE(wrong.warnings.empty());
  CHECK_FALSE(cam::RotatesWhileCutting(wrong));
}

TEST_CASE("cam rotary: a rotary nobody said how is installed is not posted",
          "[cam]") {
  RotaryJob job = SmallRotaryJob();
  job.setup.machine.name = "6090";
  job.setup.machine.rotary = cam::RotaryMount{};
  const auto paths = cam::GenerateAll(job.setup, job.doc->GetBodies());
  std::string gcode, error;
  CHECK_FALSE(cam::PostProcess("mach3", job.setup, paths, {}, gcode, &error));
  INFO(error);
  CHECK(error.find("does not say how its rotary is installed") !=
        std::string::npos);
  CHECK(error.find("6090") != std::string::npos);
  // A setup on the bed of the same machine is untouched.
  auto block = BlockDoc();
  cam::Setup bed = BasicSetup();
  bed.machine = job.setup.machine;
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Outside;
  op.topZ = 21.0;
  op.bottomZ = 10.0;
  bed.operations.push_back(op);
  CHECK_FALSE(cam::HeldInRotary(bed));
  CHECK(cam::PostProcess("mach3", bed,
                         cam::GenerateAll(bed, block->GetBodies()), {}, gcode,
                         &error));
  // ...until it says its part is in the rotary: an indexed setup at A = 0 is
  // in the chuck all the same.
  bed.heldIn = cam::Setup::HeldIn::Rotary;
  CHECK_FALSE(cam::PostProcess("mach3", bed,
                               cam::GenerateAll(bed, block->GetBodies()), {},
                               gcode, &error));
}

TEST_CASE("cam rotary: the chuck and the tailstock are kept out of, and "
          "checked",
          "[cam]") {
  // docs/cam/rotary.md#chuck-and-tailstock. The sampler part's first pass ran
  // along the whole part: through the jaws, and down to 5 mm from the axis
  // over the tailstock's centre.
  auto doc = BarDoc(20.0, 120.0); // along X from 0 to 120
  cam::Setup s = BasicSetup();
  RotaryMachine(s);
  s.chuck = 1;
  s.tailstock = 1;
  s.machine.holding.chuckDiameter = 80.0;
  s.machine.holding.gripLength = 15.0;
  s.stock.margin = {0.0, 1.0, 1.0};
  s.safeZ = 30.0;
  s.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::SurfaceRotary;
  op.stepover = 0.5;
  op.stepdown = 2.0;
  s.operations.push_back(op);
  const cam::HoldingEnvelope env =
      cam::HoldingEnvelopeOf(s, cam::BodiesInSetupFrame(s, doc->GetBodies()));
  REQUIRE(env.ok);
  CHECK_THAT(env.chuckTo, WithinAbs(15.0, 1e-6));
  CHECK_THAT(env.RadiusAt(10.0), WithinAbs(40.0, 1e-9));
  CHECK_THAT(env.RadiusAt(60.0), WithinAbs(0.0, 1e-9));
  CHECK(env.RadiusAt(125.0) > 0.0); // the centre's cone
  CHECK(env.RadiusAt(200.0) > env.RadiusAt(121.0));
  // Unmeasured sizes are named as defaults.
  CHECK_FALSE(env.defaulted.empty());

  const auto paths = cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  CHECK(paths[0].holdingClippedMm > 0.0);
  // ...and says so, or the uncut grip reads as leftover with no reason.
  bool said = false;
  for (const cam::VerdictLine& l :
       cam::MachineLines(s, doc->GetBodies(), &paths))
    said = said || l.text.find("chuck or the tailstock") != std::string::npos;
  CHECK(said);
  for (const cam::Move& m : paths[0].moves)
    if (m.kind != cam::Move::Kind::Rapid) {
      CHECK(m.to.x >= env.MinX(3.0) - 1e-6);
      CHECK(m.to.x <= env.MaxX(3.0) + 1e-6);
    }
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", s, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, s, doc->GetBodies(), 0.5);
  INFO("holding " << v.holdingCollisions << " worst " << v.worstHoldingDepth
                  << " " << v.worstHoldingWhat);
  CHECK(v.holdingCollisions == 0);

  // A file that drives into the jaws is not ok, and says which.
  const std::string jaws =
      "(op 1)\nG0 Z30\nG0 X30 Y0\nG1 Z21 F300\nG1 X5 A10 F300\nG0 Z30\n";
  const cam::Verdict bad = cam::VerifyGcode(jaws, s, doc->GetBodies(), 0.5);
  CHECK(bad.holdingCollisions > 0);
  CHECK(bad.worstHoldingWhat == "the chuck");
  CHECK_FALSE(bad.Safe(0.05));
}

TEST_CASE("cam rotary: a turning program longer than the travel is said",
          "[cam]") {
  // A continuous rotary file had no travel check at all. Generated without
  // a work area -- generation clips to one -- and checked against a machine
  // 20 mm long in X, as a file made elsewhere would be.
  RotaryJob job = SmallRotaryJob();
  const auto paths = cam::GenerateAll(job.setup, job.doc->GetBodies());
  job.setup.machine.travelMin = {0.0, 0.0, 0.0};
  job.setup.machine.travelMax = {20.0, 600.0, 150.0};
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", job.setup, paths, {}, gcode, &error));
  const cam::Verdict v =
      cam::VerifyGcode(gcode, job.setup, job.doc->GetBodies(), 0.5);
  CHECK(v.rotary);
  CHECK(v.outsideTravel > 0);
  CHECK(v.outsideAxis == 'X');
  // Lying along Y the long travel is along the part, and it fits.
  job.setup.machine.rotary.along = 'y';
  job.setup.machine.rotary.topAxis = 'x';
  job.setup.machine.rotary.topSign = 1;
  REQUIRE(cam::PostProcess("mach3", job.setup, paths, {}, gcode, &error));
  CHECK(cam::VerifyGcode(gcode, job.setup, job.doc->GetBodies(), 0.5)
            .outsideTravel == 0);
}

// An indexed setup after a turning one starts from the TURNED part, not the
// bar: checked against the fresh bar, every entry into the chess king's cross
// was a rapid "through material" 12.5 mm deep and every pass a 24 mm "bite",
// all of it air the turning had already taken away.
TEST_CASE("cam rotary: an indexed setup starts from what the turning left",
          "[cam][rotary]") {
  RotaryJob turn = SmallRotaryJob(); // a Phi 30 bar, Phi 22 neck at x 12..28
  cam::Setup indexed = BasicSetup();
  RotaryMachine(indexed);
  indexed.heldIn = cam::Setup::HeldIn::Rotary;
  indexed.rotationA = 0.0;
  indexed.stock.margin = {0.0, 1.0, 1.0};
  const std::vector<Body>& bodies = turn.doc->GetBodies();
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(indexed, bodies, lo, hi));
  cam::HeightMap stock = cam::MakeStock(lo, hi, 0.5);
  const auto topAt = [&](const cam::HeightMap& h, double x) {
    const int i = static_cast<int>((x - h.x0) / h.res);
    const int j = static_cast<int>((0.0 - h.y0) / h.res);
    return static_cast<double>(h.At(i, j));
  };
  REQUIRE(topAt(stock, 20.0) > 15.5); // fresh: the bar's top over the neck

  // Only an EARLIER setup seeds; the same one listed as later does not.
  const cam::OtherSetups later = cam::OtherSetupsOf(
      indexed, {{&turn.setup, turn.gcode, false}}, bodies, 0.5);
  REQUIRE_FALSE(later.HasEarlier());
  const cam::OtherSetups earlier = cam::OtherSetupsOf(
      indexed, {{&turn.setup, turn.gcode, true}}, bodies, 0.5);
  REQUIRE(earlier.HasEarlier());
  cam::SeedFromEarlier(stock, earlier, lo.z, 0.3);
  // Over the neck the turning left about its radius (11, plus the scallop
  // and the seed's margin); over the full bar, the bar.
  CHECK(topAt(stock, 20.0) < 12.5);
  CHECK(topAt(stock, 20.0) > 10.9);
  CHECK(topAt(stock, 4.0) > 14.9);
}
#endif
