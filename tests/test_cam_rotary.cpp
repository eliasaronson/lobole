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
#include "core/cam/CamSim.h"
#include "core/cam/CamSurface.h"

#include <catch2/generators/catch_generators.hpp>

#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <chrono>
#include <cstring>
#include <numbers>

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

TEST_CASE("cam rotary: a cell on the cutter's wall is outside by a decision, "
          "not by rounding",
          "[cam]") {
  // docs/cam/rotary.md#the-cutters-wall. A toolpath lies on the grid's own
  // lattice, so a 6 mm cutter over a 0.25 mm grid puts its wall EXACTLY on a
  // column of cell centres -- and whether |dx| <= R there was the last bit
  // of the blank's x0. On the chess knights a float32 walk counted two
  // rapids 3 mm through stock that the double walk did not. Here the same
  // move is asked with the grid nudged a picometre each way. Everything the
  // cutter reaches is already cut below it except the column on its +X
  // wall, so that column is the ONLY material in play.
  cam::Tool flat;
  flat.diameter = 6.0;
  flat.type = cam::ToolType::Flat;
  const double res = 0.25;
  const double tipX = (40 + 0.5) * res; // over cell 40's centre
  const auto walk = [&](double nudge, bool carve) {
    cam::RadiusMap stock = cam::MakeRotaryStock(nudge, 20.0 + nudge, 10.0, res);
    for (int i = 0; i < stock.nx; i++)
      if ((i + 0.5) * res - tipX < 3.0 - 0.5 * res)
        for (int j = 0; j < stock.nTheta; j++)
          stock.Set(i, j, 5.0f);
    // Turned so one row's ray points straight up the spindle: it stays on
    // y = 0 its whole length, so on the wall it is wall from end to end.
    const int jUp = stock.nTheta / 4;
    cam::RotaryMove m;
    m.from = m.to = glm::dvec3(tipX, 0.0, 7.0);
    m.aFrom = m.aTo =
        glm::degrees(0.5 * std::numbers::pi - stock.CellTheta(jUp));
    m.tool = &flat;
    m.rapid = !carve;
    const std::vector<cam::RotaryRapidHit> hits =
        cam::WalkRotaryProgram({m}, stock, 1);
    if (carve) {
      double sum = 0.0;
      for (float v : stock.r)
        sum += v;
      return sum;
    }
    REQUIRE(hits.size() == 1);
    return hits[0].depth;
  };
  for (const bool carve : {false, true}) {
    INFO((carve ? "carve" : "rapid"));
    const double nominal = walk(0.0, carve);
    CHECK(walk(1e-12, carve) == nominal);
    CHECK(walk(-1e-12, carve) == nominal);
  }
  // ...and the decision is OUTSIDE: the wall is where the cutter stops.
  CHECK(walk(0.0, false) == 0.0);
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
        r = 0.0;                 // off the part
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

namespace {
// Pins parallel::Workers() for one scope, restoring what was there.
struct ScopedThreads {
  explicit ScopedThreads(const char* n) {
    if (const char* was = std::getenv("DESIGNER_SWEEP_THREADS"))
      m_was = was;
    setenv("DESIGNER_SWEEP_THREADS", n, 1);
  }
  ~ScopedThreads() {
    if (m_was.empty())
      unsetenv("DESIGNER_SWEEP_THREADS");
    else
      setenv("DESIGNER_SWEEP_THREADS", m_was.c_str(), 1);
  }
  std::string m_was;
};
} // namespace

TEST_CASE("cam rotary: the model samples the same on one thread and on many",
          "[cam]") {
  // SampleModelRotary splats triangles on every core, raising cells with a
  // compare-and-swap; a cell's answer is a maximum, so the order the samples
  // land in cannot show. The whole pass is then the same moves in the same
  // order whatever the thread count.
  auto doc = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 15.0);
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.stock.margin = {0.0, 0.0, 0.0};
  setup.safeZ = 40.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::SurfaceRotary, cam::Target{});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  op.stepover = 0.25;
  setup.operations = {op};

  const cam::RadiusMap grid = cam::MakeRotaryStock(-3.0, 63.0, 20.0, 0.1);
  cam::RadiusMap one, many;
  std::vector<cam::Toolpath> pathsOne, pathsMany;
  {
    ScopedThreads pin("1");
    one = cam::SampleModelRotary(doc->GetBodies(), grid);
    pathsOne = cam::GenerateAll(setup, doc->GetBodies());
  }
  {
    ScopedThreads pin("8");
    many = cam::SampleModelRotary(doc->GetBodies(), grid);
    pathsMany = cam::GenerateAll(setup, doc->GetBodies());
  }
  REQUIRE(one.r.size() == many.r.size());
  CHECK(one.r == many.r);
  REQUIRE(pathsOne.size() == 1);
  REQUIRE(pathsMany.size() == 1);
  REQUIRE(pathsOne[0].error.empty());
  REQUIRE(pathsOne[0].moves.size() > 500);
  REQUIRE(pathsOne[0].moves.size() == pathsMany[0].moves.size());
  size_t differ = 0;
  for (size_t k = 0; k < pathsOne[0].moves.size(); k++) {
    const cam::Move& a = pathsOne[0].moves[k];
    const cam::Move& b = pathsMany[0].moves[k];
    if (a.kind != b.kind || a.to != b.to || a.a != b.a || a.feed != b.feed)
      differ++;
  }
  CHECK(differ == 0);
}

namespace {

// A turned part with several passes -- a rough in layers, then a finish --
// and rapids THROUGH the stock spliced in at three points: into the untouched
// blank along the axis (where a cutter buried in a cylinder is equally deep
// at many steps and cells, so ties are real), between the rough and the
// finish, and near the end.
std::string TurningWithBadRapids(const cam::Setup& setup, const Document& doc) {
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc.GetBodies());
  REQUIRE(paths.size() == 2);
  for (const cam::Toolpath& tp : paths) {
    INFO(tp.error);
    REQUIRE(tp.error.empty());
  }
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, gcode, &error));

  std::vector<std::string> lines;
  for (size_t at = 0; at < gcode.size();) {
    const size_t nl = gcode.find('\n', at);
    const size_t end = nl == std::string::npos ? gcode.size() : nl;
    lines.push_back(gcode.substr(at, end - at));
    at = end + 1;
  }
  // Spliced only in front of a line that states its own G1, so the modal
  // motion a following bare coordinate relies on is restated by it.
  std::vector<size_t> g1;
  for (size_t k = 0; k < lines.size(); k++)
    if (lines[k].rfind("G1 ", 0) == 0)
      g1.push_back(k);
  REQUIRE(g1.size() > 10);
  const std::string bad[3] = {
      "G0 Z21 A0\nG0 X2 Y0\nG0 X58\nG0 Z40",
      "G0 Z17 A45\nG0 X10 Y1\nG0 X50 A135\nG0 Z40",
      "G0 Z16 A200\nG0 X30 Y0\nG0 A260\nG0 Z40",
  };
  const size_t where[3] = {g1[1], g1[g1.size() / 2], g1[g1.size() - 3]};
  std::string out;
  for (size_t k = 0; k < lines.size(); k++) {
    for (int b = 0; b < 3; b++)
      if (k == where[b])
        out += bad[b] + "\n";
    out += lines[k] + "\n";
  }
  return out;
}

std::vector<cam::RotaryMove> RotaryMovesOf(const cam::GProgram& p,
                                           const cam::Setup& setup) {
  std::vector<cam::RotaryMove> moves;
  glm::dvec3 from(0.0);
  double a = 0.0;
  for (size_t k = 0; k < p.moves.size(); k++) {
    const cam::GMove& m = p.moves[k];
    const double to = m.hasA ? m.a : a;
    if (k > 0)
      moves.push_back({from, m.to, a, to, setup.FindTool(m.tool), m.rapid});
    from = m.to;
    a = to;
  }
  return moves;
}

bool SameBytes(const cam::RadiusMap& a, const cam::RadiusMap& b) {
  return a.r.size() == b.r.size() &&
         std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0;
}

} // namespace

TEST_CASE("cam rotary: the check on every core is the serial check to the bit",
          "[cam][slow]") {
  // docs/cam/rotary.md#on-every-core. The map is split into bands along the
  // axis, each walking every move and touching only its own cells. A cut is
  // a min(), so its order cannot show -- but a RAPID is asked about the
  // stock as it stands at that line, and a rapid through material is the one
  // failure a shape comparison cannot see. So the answer must not be close
  // to the serial one: it must BE it, for any number of bands and threads.
  auto doc = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 15.0);
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.stock.margin = {0.0, 4.0, 4.0};
  setup.safeZ = 40.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  const cam::DerivedOp rough = cam::DeriveRotaryRough(setup, doc->GetBodies());
  REQUIRE(rough.ok);
  const cam::DerivedOp finish = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::SurfaceRotary, cam::Target{});
  REQUIRE(finish.ok);
  cam::Operation op1 = rough.op, op2 = finish.op;
  op1.id = 1;
  op1.stepover = 0.25;
  op2.id = 2;
  op2.stepover = 0.25;
  setup.operations = {op1, op2};
  const std::string gcode = TurningWithBadRapids(setup, *doc);

  constexpr double kRes = 0.5;
  const std::vector<Body> bodies =
      cam::BodiesInSetupFrame(setup, doc->GetBodies());
  const cam::RadiusMap blank = cam::MakeRotaryBlank(setup, bodies, kRes);
  REQUIRE(blank.nx > 64);
  const std::vector<cam::RotaryMove> moves =
      RotaryMovesOf(cam::ProgramInSetupSpace(gcode, setup), setup);
  REQUIRE(moves.size() > 1000);

  // The serial walk: one band, the whole map.
  cam::RadiusMap serial = blank;
  const std::vector<cam::RotaryRapidHit> serialHits =
      cam::WalkRotaryProgram(moves, serial, 1);
  // ...which carves what the one-move carve carves, move after move.
  {
    cam::RadiusMap byMove = blank;
    for (const cam::RotaryMove& m : moves)
      if (!m.rapid)
        cam::CarveSweepRotary(m.from, m.to, m.aFrom, m.aTo, m.tool, byMove);
    CHECK(SameBytes(byMove, serial));
  }
  size_t buried = 0;
  for (const cam::RotaryRapidHit& h : serialHits)
    if (h.depth > 1e-3)
      buried++;
  INFO(serialHits.size() << " rapids, " << buried << " through stock");
  REQUIRE(buried >= 3); // or this is not testing the rapids

  for (const size_t bands :
       {size_t{3}, size_t{61}, static_cast<size_t>(blank.nx)}) {
    for (const char* threads : {"1", "8"}) {
      ScopedThreads pin(threads);
      cam::RadiusMap banded = blank;
      const std::vector<cam::RotaryRapidHit> hits =
          cam::WalkRotaryProgram(moves, banded, bands);
      INFO(bands << " bands on " << threads << " threads");
      CHECK(SameBytes(banded, serial));
      REQUIRE(hits.size() == serialHits.size());
      size_t differ = 0;
      for (size_t k = 0; k < hits.size(); k++)
        if (hits[k].depth != serialHits[k].depth ||
            hits[k].at != serialHits[k].at)
          differ++;
      CHECK(differ == 0);
    }
  }

  // The whole check, as a caller sees it: one thread against many.
  cam::RotaryInspection one, many;
  {
    ScopedThreads pin("1");
    one = cam::InspectRotary(gcode, setup, doc->GetBodies(), kRes);
  }
  {
    ScopedThreads pin("8");
    many = cam::InspectRotary(gcode, setup, doc->GetBodies(), kRes);
  }
  REQUIRE(one.ok);
  REQUIRE(many.ok);
  CHECK(SameBytes(one.machined, serial));
  CHECK(SameBytes(one.machined, many.machined));
  const cam::Verdict& a = one.verdict;
  const cam::Verdict& b = many.verdict;
  INFO("rapids " << a.rapidsThroughStock << " first line " << a.firstBadLine
                 << " worst " << a.worstRapidDepth << " at line "
                 << a.worstRapidLine);
  CHECK(a.rapidsThroughStock >= 3);
  CHECK(a.rapidsThroughStock == b.rapidsThroughStock);
  CHECK(a.firstBadLine == b.firstBadLine);
  CHECK(a.worstRapidDepth == b.worstRapidDepth);
  CHECK(a.worstRapidAt == b.worstRapidAt);
  CHECK(a.worstRapidLine == b.worstRapidLine);
  CHECK(a.maxGouge == b.maxGouge);
  CHECK(a.maxLeftover == b.maxLeftover);
  CHECK(a.leftoverArea == b.leftoverArea);

  // ...and the playback, on every core, ends on that same stock.
  const cam::Timeline tl =
      cam::BuildTimeline(cam::ProgramInSetupSpace(gcode, setup), 0.0);
  REQUIRE(tl.rotary);
  ScopedThreads pin("8");
  cam::RotaryStockSim sim;
  sim.Begin(tl, setup, blank);
  sim.SeekTo(tl.End());
  CHECK(SameBytes(sim.Stock(), many.machined));
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

TEST_CASE("cam rotary: a rough is planned on the grid its skin needs, and "
          "still leaves an even skin",
          "[cam]") {
  // docs/cam/rotary.md#layers-rest-and-the-rough-verb: a rough's grid comes
  // from its skin, not from the finish's cusp -- coarser, never finer than
  // the finish's, and never wider than half the skin.
  cam::Tool flat = ShapedTool(cam::ToolType::Flat, 6.0);
  cam::Operation finish;
  finish.type = cam::OpType::SurfaceRotary;
  const cam::RotaryGrid fine = cam::RotaryPassGrid(flat, finish);
  cam::Operation rough = finish;
  rough.stepdown = 3.0;
  rough.stockToLeave = 0.5;
  const cam::RotaryGrid coarse = cam::RotaryPassGrid(flat, rough);
  CHECK(coarse.res > fine.res);
  CHECK(coarse.res <= 0.5 * rough.stockToLeave + 1e-12);
  // Derived from the skin, so a thicker skin is a coarser grid...
  rough.stockToLeave = 0.8;
  CHECK(cam::RotaryPassGrid(flat, rough).res > coarse.res);
  // ...and a skin too thin to gain anything keeps the finish's.
  rough.stockToLeave = 0.1;
  CHECK(cam::RotaryPassGrid(flat, rough).res == fine.res);
  // A rest pass is not a rough, whatever it leaves.
  rough.stockToLeave = 0.5;
  rough.restFromTool = 2;
  CHECK(cam::RotaryPassGrid(flat, rough).res == fine.res);

  // And the skin it leaves is the skin the finish would have met. The same
  // op cut as ONE layer (no stepdown) is planned on the finish's grid and
  // ends at the same surface -- its last layer IS that surface -- so the two
  // carved blanks, measured against the part, must leave the same skin: no
  // thinner where the finish plunges in, no thicker where it has to take
  // more than one pass's worth.
  auto doc = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 15.0);
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.stock.margin = {0.0, 4.0, 4.0};
  setup.safeZ = 40.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  const cam::DerivedOp d = cam::DeriveRotaryRough(setup, doc->GetBodies());
  REQUIRE(d.ok);
  const double skin = d.op.stockToLeave;
  const auto skinOf = [&](double stepdown, double& thin, double& median,
                          double& thick) {
    cam::Setup s = setup;
    cam::Operation op = d.op;
    op.id = 1;
    op.stepover = 0.25;
    op.stepdown = stepdown;
    s.operations = {op};
    const std::vector<cam::Toolpath> paths =
        cam::GenerateAll(s, doc->GetBodies());
    REQUIRE(paths.size() == 1);
    REQUIRE(paths[0].error.empty());
    std::string gcode, error;
    REQUIRE(cam::PostProcess("linuxcnc", s, paths, {}, gcode, &error));
    const cam::RotaryInspection in =
        cam::InspectRotary(gcode, s, doc->GetBodies(), 0.5);
    REQUIRE(in.ok);
    // The skin on the turned surfaces, off the shoulders and the ends: a
    // cell whose neighbours along the axis agree with it.
    std::vector<double> skins;
    for (int j = 0; j < in.designed.nTheta; j++)
      for (int i = 2; i + 2 < in.designed.nx; i++) {
        const double want = in.designed.At(i, j);
        bool flat = want > 0.0;
        for (int di = -2; di <= 2 && flat; di++)
          flat = std::abs(in.designed.At(i + di, j) - want) < 0.05;
        if (flat)
          skins.push_back(in.machined.At(i, j) - want);
      }
    REQUIRE(skins.size() > 1000);
    std::sort(skins.begin(), skins.end());
    thin = skins[skins.size() / 100];
    median = skins[skins.size() / 2];
    thick = skins[skins.size() * 99 / 100];
  };
  double roughThin = 0, roughMedian = 0, roughThick = 0;
  double oneThin = 0, oneMedian = 0, oneThick = 0;
  skinOf(d.op.stepdown, roughThin, roughMedian, roughThick);
  skinOf(0.0, oneThin, oneMedian, oneThick);
  INFO("skin " << skin << ": rough grid " << roughThin << " / " << roughMedian
               << " / " << roughThick << ", finish grid " << oneThin << " / "
               << oneMedian << " / " << oneThick);
  // Even: the thinnest percent keeps most of the skin, as the finish's grid
  // does, and the typical skin is the skin.
  CHECK(roughThin > 0.6 * skin);
  CHECK(roughThin > oneThin - skin / 3.0);
  CHECK(std::abs(roughMedian - oneMedian) < 0.25 * skin);
  CHECK(roughThick < oneThick + 0.25 * skin);
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
TEST_CASE("cam rotary: one operation cut at four indices posts four A words "
          "and verifies clean",
          "[cam][rotary]") {
  // A 24 mm square bar on the axis: four faces, and ONE face operation at
  // 0/90/180/270 is what cuts them from the round blank -- the rook's
  // crenels and the queen's crown were a setup per angle before
  // (docs/cam/rotary.md#one-operation-at-several-indices).
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>(
      "Square bar", glm::vec3(0, -12, -12), 60.0, 24.0, 24.0));
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.heldIn = cam::Setup::HeldIn::Rotary;
  setup.stock.margin = {0.0, 1.0, 1.0};
  setup.safeZ = 30.0;
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Face, cam::Target{});
  INFO(d.why);
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  // From the round blank's top (the bar's corner radius, 17, plus the 1 mm
  // margin) down to the face, in layers.
  op.topZ = 18.0;
  op.bottomZ = 12.0;
  op.stepdown = 2.0;
  op.indexA = {0.0, 90.0, 180.0, 270.0};
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  REQUIRE(paths[0].error.empty());
  CHECK(paths[0].skipped.empty());
  std::set<double> angles;
  for (const cam::Move& m : paths[0].moves) {
    REQUIRE(m.hasA);
    angles.insert(m.a);
  }
  CHECK(angles == std::set<double>{0.0, 90.0, 180.0, 270.0});

  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, gcode, &error));
  // The A words the MOTION carries -- not the G92 at the end, which renames
  // the last angle so the next program does not unwind it.
  std::set<std::string> words;
  std::istringstream lines(gcode);
  for (std::string line; std::getline(lines, line);) {
    const size_t at = line.find(" A");
    if (line.rfind("G0", 0) == 0 && at != std::string::npos)
      words.insert(line.substr(at + 1, line.find(' ', at + 1) - at - 1));
  }
  CHECK(words == std::set<std::string>{"A0", "A90", "A180", "A270"});

  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.5);
  INFO("rotary " << v.rotary << " gouge " << v.maxGouge << " leftover "
                 << v.maxLeftover << " rapids " << v.rapidsThroughStock);
  REQUIRE(v.rotary); // the file turns, so it is read as one that does
  CHECK(v.maxGouge < 0.1);
  CHECK(v.maxLeftover < 0.6);
  CHECK(v.rapidsThroughStock == 0);
}

TEST_CASE("cam rotary: indices are refused where nothing can turn the part",
          "[cam][rotary]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Face;
  op.topZ = 20.0;
  op.bottomZ = 19.0;
  op.indexA = {0.0, 180.0};
  setup.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  CHECK(paths[0].moves.empty());
  CHECK(paths[0].error.find("no rotary axis") != std::string::npos);
}

namespace {

// A Phi 30 shaft 60 long with two narrow grooves turned in it, 3 mm wide
// and 1 mm deep, at x 15 and x 42: square-cornered coves whose floor a 6 mm
// ball sags only 0.4 mm into and a 2 mm one reaches, far enough apart that
// nothing lies between. Shallow, so their walls do not dominate a spiral.
std::unique_ptr<Document> GroovedShaftDoc(const double (&at)[2], double width) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Blank", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 15.0, 60.0));
  uint64_t shaft = doc->GetFeature(0)->GetId();
  for (double x : at) {
    doc->AddFeature(std::make_unique<CylinderFeature>(
        "Groove", glm::vec3(x, 0, 0), glm::vec3(1, 0, 0), 15.0, width));
    const uint64_t groove = doc->GetFeatures().back()->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("Cut", shaft, groove,
                                                     BooleanOp::Subtract));
    shaft = doc->GetFeatures().back()->GetId();
    doc->AddFeature(std::make_unique<CylinderFeature>(
        "Core", glm::vec3(x, 0, 0), glm::vec3(1, 0, 0), 14.0, width));
    const uint64_t core = doc->GetFeatures().back()->GetId();
    doc->AddFeature(std::make_unique<BooleanFeature>("Shaft", shaft, core,
                                                     BooleanOp::Union));
    shaft = doc->GetFeatures().back()->GetId();
  }
  return doc;
}

} // namespace

TEST_CASE("cam rotary: a rest pass cuts only the bands its finish left",
          "[cam][slow]") {
  // docs/cam/rotary.md#rest-bands. A rest spiral wound ONE helix over the
  // whole turned length, through the air where the finish had left nothing:
  // a 1.5 mm ball's on a chess blank was 697 of its 828 minutes in the air.
  // And its mask was grown round the axis by the arc at the map's middle
  // cell, not the leftover's own, which marked far more than was left.
  const double at[2] = {15.0, 42.0};
  const double width = 3.0;
  auto doc = GroovedShaftDoc(at, width);
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.stock.margin = {0.0, 1.0, 1.0};
  setup.safeZ = 30.0;
  setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  cam::Tool small = ShapedTool(cam::ToolType::Ball, 2.0);
  small.number = 2;
  setup.tools.push_back(small);

  cam::Operation f = cam::DeriveOperation(setup, doc->GetBodies(),
                                          cam::OpType::SurfaceRotary, {}, 1)
                         .op;
  f.id = 1;
  f.stepover = 0.25;
  setup.operations.push_back(f);
  const cam::DerivedOp derived = cam::DeriveRest(setup, doc->GetBodies(), 2);
  INFO(derived.why);
  REQUIRE(derived.ok);

  const auto pattern =
      GENERATE(cam::RotaryPattern::Parallel, cam::RotaryPattern::Spiral);
  INFO("pattern " << static_cast<int>(pattern));
  cam::Operation rest = derived.op;
  rest.id = 2;
  rest.stepover = 0.25;
  rest.rotaryPattern = pattern;
  // The same cutter over the whole part: what the rest pass is measured
  // against.
  cam::Operation whole = rest;
  whole.restFromTool = 0;

  cam::Setup withRest = setup;
  withRest.operations.push_back(rest);
  cam::Setup withWhole = setup;
  withWhole.operations.push_back(whole);
  const auto r = cam::GenerateAll(withRest, doc->GetBodies());
  const auto w = cam::GenerateAll(withWhole, doc->GetBodies());
  REQUIRE(r.size() == 2);
  REQUIRE(w.size() == 2);
  INFO(r[1].error);
  REQUIRE(r[1].error.empty());
  REQUIRE(w[1].error.empty());

  // Every move that is not a retract stays within a groove, grown by the
  // small ball's radius and a cell of the big one's grid.
  const double reach = 0.5 * small.diameter + 1.0;
  std::set<int> visited;
  for (size_t k = 1; k < r[1].moves.size(); k++) {
    const cam::Move& m = r[1].moves[k];
    if (m.kind == cam::Move::Kind::Rapid)
      continue;
    int band = -1;
    for (int b = 0; b < 2; b++)
      if (m.to.x >= at[b] - reach && m.to.x <= at[b] + width + reach)
        band = b;
    INFO("cut at x " << m.to.x);
    CHECK(band >= 0);
    visited.insert(band);
  }
  CHECK(visited.size() == 2); // both grooves, and nothing between them

  // A small fraction of the full-length pass: two bands of ~6 mm on 66. The
  // spiral's is the larger share (a third, against a fifth) because its
  // turns bunch up on every wall, and a band is walls either side.
  const double restLen = r[1].CutLength(), wholeLen = w[1].CutLength();
  INFO("rest " << restLen << " mm, whole " << wholeLen << " mm");
  CHECK(restLen < 0.4 * wholeLen);
  CHECK(restLen > 0.0);

  // ...and still cleans the grooves the finish could not.
  std::string gcode, error;
  const std::vector<cam::Toolpath> finishOnly = {r[0]};
  REQUIRE(
      cam::PostProcess("linuxcnc", withRest, finishOnly, {}, gcode, &error));
  const cam::Verdict v0 =
      cam::VerifyGcode(gcode, withRest, doc->GetBodies(), 0.5);
  REQUIRE(cam::PostProcess("linuxcnc", withRest, r, {}, gcode, &error));
  const cam::Verdict v1 =
      cam::VerifyGcode(gcode, withRest, doc->GetBodies(), 0.5);
  INFO("leftover " << v0.maxLeftover << " -> " << v1.maxLeftover << ", gouge "
                   << v1.maxGouge << ", rapids through stock "
                   << v1.rapidsThroughStock);
  CHECK(v1.maxGouge < 0.1);
  CHECK(v1.rapidsThroughStock == 0);
  CHECK(v1.maxLeftover < v0.maxLeftover - 0.3);
}

TEST_CASE("cam rotary: what the chuck clips off a spiral is counted along "
          "the tip's path",
          "[cam]") {
  // docs/cam/rotary.md#chuck-and-tailstock. A spiral's move turns the part
  // while X barely moves, and `holding_clipped_mm` counted the clipped moves
  // in XYZ alone: the turns lying in the chuck came to their axial advance,
  // a millimetre or two for what was metres of cutting.
  auto doc = BarDoc(20.0, 120.0);
  cam::Setup s = BasicSetup();
  RotaryMachine(s);
  s.machine.holding.chuckDiameter = 80.0;
  s.machine.holding.gripLength = 15.0;
  s.stock.margin = {0.0, 1.0, 1.0};
  s.safeZ = 30.0;
  s.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::SurfaceRotary;
  op.rotaryPattern = cam::RotaryPattern::Spiral;
  op.stepover = 0.5; // a 3 mm pitch
  s.operations.push_back(op);

  const auto loose = cam::GenerateAll(s, doc->GetBodies());
  s.chuck = 1;
  s.tailstock = 1;
  const auto held = cam::GenerateAll(s, doc->GetBodies());
  REQUIRE(loose.size() == 1);
  REQUIRE(held.size() == 1);
  REQUIRE(loose[0].error.empty());
  INFO(held[0].error);
  REQUIRE(held[0].error.empty());
  CHECK(loose[0].holdingClippedMm == 0.0);

  // A spiral is its turns: ~2 pi r a turn, a turn every 3 mm.
  const double turn = 2.0 * std::numbers::pi * 20.0;
  CHECK(loose[0].CutLength() > 0.9 * turn * 120.0 / 3.0);

  // What was clipped is what is missing -- less the plunges the clip adds to
  // come back in.
  const auto plunged = [](const cam::Toolpath& tp) {
    double s = 0.0;
    for (size_t k = 1; k < tp.moves.size(); k++)
      if (tp.moves[k].kind == cam::Move::Kind::Plunge)
        s += glm::length(tp.moves[k].to - tp.moves[k - 1].to);
    return s;
  };
  const double missing =
      loose[0].CutLength() -
      (held[0].CutLength() - plunged(held[0]) + plunged(loose[0]));
  INFO("clipped " << held[0].holdingClippedMm << " mm, missing " << missing
                  << " mm");
  CHECK_THAT(held[0].holdingClippedMm,
             WithinAbs(missing, 0.02 * held[0].holdingClippedMm));

  // And it is turns, not an axial advance: at least the chuck's grip of
  // them, 15 mm of a 3 mm pitch.
  CHECK(held[0].holdingClippedMm > 0.8 * turn * 15.0 / 3.0);
}

namespace {

// The indexed setup the tests below cut after `SmallRotaryJob`'s turning:
// the same bar on the same axis, at A = 0.
cam::Setup IndexedAfterTurn() {
  cam::Setup indexed = BasicSetup();
  RotaryMachine(indexed);
  indexed.id = 2;
  indexed.name = "Indexed";
  indexed.heldIn = cam::Setup::HeldIn::Rotary;
  indexed.rotationA = 0.0;
  indexed.stock.margin = {0.0, 1.0, 1.0};
  return indexed;
}

double TopAt(const cam::HeightMap& h, double x, double y = 0.0) {
  const int i = static_cast<int>((x - h.x0) / h.res);
  const int j = static_cast<int>((y - h.y0) / h.res);
  return static_cast<double>(h.At(i, j));
}

} // namespace

// The playback starts where the verifier starts. It used to arm every setup
// with its FRESH stock, so an indexed setup after a turning one played its
// entries through a square bar the turning had already taken away -- while
// the verdict, seeded from the turning, said they were over air.
TEST_CASE("cam rotary: playback of an indexed setup starts from the turned "
          "stock",
          "[cam][rotary]") {
  RotaryJob turn = SmallRotaryJob(); // Phi 30 bar, Phi 22 neck at x 12..28
  turn.setup.id = 1;
  turn.setup.name = "Turn";
  const cam::Setup indexed = IndexedAfterTurn();
  const std::vector<Body>& bodies = turn.doc->GetBodies();
  // Down to 13 over the neck -- air once it is turned, the bar before -- and
  // a short cut there.
  const std::string gcode = "(op 1)\nG0 Z30\nG0 X20 Y0\nG0 Z13\n"
                            "G1 X22 F300\nG0 Z30\n";
  constexpr double kRes = 0.5;
  const cam::OtherSetups earlier = cam::OtherSetupsOf(
      indexed, {{&turn.setup, turn.gcode, true}}, bodies, kRes);
  REQUIRE(earlier.HasEarlier());

  cam::HeightMap fresh, start;
  REQUIRE(cam::StartingStock(indexed, bodies, kRes, nullptr, fresh));
  REQUIRE(cam::StartingStock(indexed, bodies, kRes, &earlier, start));
  CHECK(TopAt(fresh, 20.0) > 15.5); // the bar
  CHECK(TopAt(start, 20.0) < 12.5); // the turned neck
  CHECK(TopAt(start, 4.0) > 14.9);  // the full diameter, still there

  const cam::Timeline tl =
      cam::BuildTimeline(cam::ProgramInSetupSpace(gcode, indexed), 0.0);
  cam::StockSim sim;
  sim.Begin(tl, indexed, start);
  REQUIRE(sim.Ready());
  CHECK(TopAt(sim.Stock(), 20.0) < 12.5); // the first frame is the turned part

  // ...and the last frame is the verifier's own stock, cell for cell: one
  // start, one carve.
  sim.SeekTo(tl.End());
  const cam::Inspection insp =
      cam::Inspect(gcode, indexed, bodies, kRes, {}, {}, {}, &earlier);
  REQUIRE(insp.ok);
  REQUIRE(insp.machined.z.size() == sim.Stock().z.size());
  size_t differ = 0;
  for (size_t c = 0; c < insp.machined.z.size(); c++)
    differ += std::abs(insp.machined.z[c] - sim.Stock().z[c]) > 1e-4f;
  CHECK(differ == 0);
  // Over air, the rapid down to 13 is no finding; over the bar it was.
  CHECK(insp.verdict.rapidsThroughStock == 0);
  CHECK(cam::Inspect(gcode, indexed, bodies, kRes).verdict.rapidsThroughStock >
        0);
}

// The rotary check starts from what earlier setups left too: a second
// turning after the first starts on the turned part, not the blank.
TEST_CASE("cam rotary: a turning after a turning starts from what it left",
          "[cam][rotary]") {
  RotaryJob first = SmallRotaryJob();
  first.setup.id = 1;
  first.setup.name = "Turn";
  cam::Setup second = first.setup;
  second.id = 2;
  second.name = "Again";
  const std::vector<Body>& bodies = first.doc->GetBodies();
  const cam::OtherSetups earlier = cam::OtherSetupsOf(
      second, {{&first.setup, first.gcode, true}}, bodies, 0.5);
  REQUIRE(earlier.HasEarlier());
  const cam::RadiusMap blank =
      cam::StartingRotaryStock(second, bodies, 0.5, nullptr);
  const cam::RadiusMap start =
      cam::StartingRotaryStock(second, bodies, 0.5, &earlier);
  REQUIRE_FALSE(start.r.empty());
  REQUIRE(start.r.size() == blank.r.size());
  const int neck = static_cast<int>((20.0 - start.x0) / start.res);
  for (int j = 0; j < start.nTheta; j += start.nTheta / 8) {
    CHECK(blank.At(neck, j) > 15.5f);
    CHECK(start.At(neck, j) < 12.5f);
    CHECK(start.At(neck, j) > 10.9f);
  }
  // And the check reads the same program against it: the second pass of
  // the same file finds nothing left to cut.
  const cam::RotaryInspection again =
      cam::InspectRotary(first.gcode, second, bodies, 0.5, &earlier);
  REQUIRE(again.ok);
  const cam::RotaryInspection alone =
      cam::InspectRotary(first.gcode, second, bodies, 0.5);
  REQUIRE(alone.ok);
  CHECK(again.verdict.maxLeftover <= alone.verdict.maxLeftover + 1e-6);
}

// An operation's derived top on an indexed setup after a turning one is the
// top of the TURNED part over it: the model's largest radius along its span,
// plus what the turning leaves -- not the square bar's top. The chess king's
// cross was typed `top_z=8.0` to stop it cutting 10 mm of air, and was warned
// for it.
TEST_CASE("cam derive: a top after a turning is the turned part's",
          "[cam][rotary]") {
  RotaryJob turn = SmallRotaryJob();
  turn.setup.id = 1;
  turn.setup.name = "Turn";
  cam::Setup indexed = IndexedAfterTurn();
  const std::vector<Body>& bodies = turn.doc->GetBodies();
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::StockBounds(indexed, bodies, lo, hi));
  REQUIRE(hi.z > 15.9); // the bar, with its margin
  const std::vector<const cam::Setup*> earlier = {&turn.setup};

  // Over the whole part: the full diameter, plus the ball's scallop and the
  // tessellation -- well under the bar's top.
  const cam::DerivedOp whole = cam::DeriveOperation(
      indexed, bodies, cam::OpType::Profile, {}, 1, earlier);
  REQUIRE(whole.ok);
  CHECK(whole.op.topZ < hi.z - 0.3);
  CHECK(whole.op.topZ > 15.0);
  // With nothing before it, the bar's top, as ever.
  const cam::DerivedOp alone =
      cam::DeriveOperation(indexed, bodies, cam::OpType::Profile, {}, 1);
  REQUIRE(alone.ok);
  CHECK(alone.op.topZ == hi.z);
  // Listed AFTER the turning, the turning is not before it.
  std::vector<cam::Setup> job = {indexed, turn.setup};
  CHECK(cam::EarlierSetups(job, indexed.id).empty());
  CHECK(cam::EarlierSetups(job, turn.setup.id).size() == 1);

  // Over the neck alone, the neck's radius: 11, not 15.
  ElementRef neck;
  const Body& body = bodies.front();
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
    if (s.GetType() == GeomAbs_Cylinder &&
        std::abs(s.Cylinder().Radius() - 11.0) < 1e-6)
      if (const std::string* name = body.elementMap.NameOf(i)) {
        neck.featureId = body.createdBy;
        neck.name = *name;
      }
  }
  REQUIRE(neck.IsSet());
  cam::Operation op;
  op.type = cam::OpType::Pocket;
  op.toolNumber = 99; // no such cutter: the span is the face's own, unwidened
  op.targets = {cam::Target{0, neck}};
  const double top = cam::StockTopOver(indexed, earlier, bodies, op, hi.z);
  CHECK(top > 11.0);
  CHECK(top < 12.0);
  // A cutter as wide as the shoulders beside the neck reaches over them.
  op.toolNumber = 1;
  CHECK(cam::StockTopOver(indexed, earlier, bodies, op, hi.z) > 15.0);
  // An op cut at several indices is read in each angle's frame, and its
  // one top is the highest of them: the neck is round, so every angle
  // agrees on it.
  op.toolNumber = 99;
  op.type = cam::OpType::Profile; // follows its target at every index
  op.indexA = {0.0, 90.0, 225.0};
  const double indexed3 = cam::StockTopOver(indexed, earlier, bodies, op, hi.z);
  CHECK(indexed3 > 11.0);
  CHECK(indexed3 < 12.0);
  op.indexA.clear();
  // A turning that does not run -- disabled -- leaves the bar.
  turn.setup.operations.front().enabled = false;
  op.toolNumber = 99;
  CHECK(cam::StockTopOver(indexed, earlier, bodies, op, hi.z) == hi.z);
}
TEST_CASE("cam rotary: an op at several indices derives where its floor "
          "faces the spindle",
          "[cam][rotary]") {
  // The rook's crenel floors face round the axis; derived in the setup's own
  // frame (A0) a sideways floor reads as a wall and the pick is refused. The
  // derivation is taken at the index that turns the first target up.
  auto doc = BlockDoc(40, 30, 20);
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  const std::vector<Body>& bodies = doc->GetBodies();
  cam::Target side;
  side.ref = WallNear(*doc, 20.0, 30.0); // the +Y face
  const auto facesUp = [&](const cam::Setup& s) {
    glm::vec3 n(0.0f);
    return cam::FaceNormalOf(cam::BodiesInSetupFrame(s, bodies), side.ref, n) &&
           n.z > 0.99f;
  };
  REQUIRE_FALSE(facesUp(setup));
  const std::vector<double> four = {0.0, 90.0, 180.0, 270.0};
  const cam::Setup at =
      cam::IndexFrameFor(setup, bodies, cam::OpType::Pocket, side, four);
  CHECK(facesUp(at));
  CHECK(at.rotationA != 0.0);
  // No index presents it: the setup itself, and the derivation's refusal.
  CHECK(cam::IndexFrameFor(setup, bodies, cam::OpType::Pocket, side, {0.0})
            .rotationA == 0.0);
  // A contour is followed at every index: the first.
  CHECK(cam::IndexFrameFor(setup, bodies, cam::OpType::Profile, side,
                           {180.0, 90.0})
            .rotationA == 180.0);
  // A type that is not cut at indices is derived as it always was.
  CHECK(
      cam::IndexFrameFor(setup, bodies, cam::OpType::SurfaceRotary, side, four)
          .rotationA == 0.0);
}
TEST_CASE("cam rotary: a picked index joins its operation's list",
          "[cam][rotary]") {
  // docs/cam/rotary.md#one-operation-at-several-indices: the four crenel
  // floors, picked one by one, are one op at 0/90/180/270.
  CHECK(cam::NormalizeIndexDeg(-90.0) == 270.0);
  CHECK(cam::NormalizeIndexDeg(360.0) == 0.0);
  CHECK(cam::NormalizeIndexDeg(89.99999999) == 90.0);
  CHECK(cam::NormalizeIndexDeg(-1e-9) == 0.0);
  CHECK(cam::NormalizeIndexDeg(450.0) == 90.0);

  const auto target = [](const char* name) {
    return cam::Target{7, ElementRef{3, name}};
  };
  cam::Operation op;
  op.targets = {target("f1")};
  // A second floor at the setup's own index: a target, and no list.
  CHECK(cam::AddIndexedTarget(op, target("f2"), 0.0, 0.0));
  CHECK(op.indexA.empty());
  CHECK(op.targets.size() == 2);
  // One at another index: the setup's own and that one.
  CHECK(cam::AddIndexedTarget(op, target("f3"), 270.0, 0.0));
  CHECK(op.indexA == std::vector<double>{0.0, 270.0});
  CHECK(cam::AddIndexedTarget(op, target("f4"), -270.0, 0.0)); // = 90
  CHECK(cam::AddIndexedTarget(op, target("f5"), 180.0, 360.0));
  CHECK(op.indexA == std::vector<double>{0.0, 90.0, 180.0, 270.0});
  // The same floor again is no change.
  CHECK_FALSE(cam::AddIndexedTarget(op, target("f5"), 180.0, 0.0));
  CHECK(op.targets.size() == 5);

  // An op made at an index of its own takes the setup's when a floor there
  // is added.
  cam::Operation turned;
  turned.indexA = {90.0};
  CHECK(cam::AddIndexedTarget(turned, target("g"), 30.0, 30.0));
  CHECK(turned.indexA == std::vector<double>{30.0, 90.0});
}

// One A, one way round. An index is cut through the setup framing
// (BodiesInSetupFrame), and a file that turns is checked and played by the
// rotary carve, whose part turns as the mount says: right-handed, the top
// toward -Y on a positive A (docs/cam/rotary.md#how-the-rotary-is-installed).
// If the two turn the part opposite ways, a face op at the index that
// presents the +Y face cuts the -Y face instead -- invisible on a square bar,
// whose sides are all alike, so this bar is not square: its +Y face is 8 from
// the axis and its -Y face 12.
TEST_CASE("cam rotary: an indexed cut and the rotary check turn the part the "
          "same way",
          "[cam][rotary]") {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("Bar", glm::vec3(0, -12, -10),
                                               60.0, 20.0, 20.0)); // y -12..8
  const std::vector<Body>& bodies = doc->GetBodies();
  cam::Setup setup = BasicSetup();
  RotaryMachine(setup);
  setup.heldIn = cam::Setup::HeldIn::Rotary;
  setup.stock.margin = {0.0, 1.0, 1.0};
  setup.safeZ = cam::DeriveSafeZ(setup, bodies);

  // The +Y face, turned up the way "Turn a face up" turns it.
  const ElementRef plusY = WallNear(*doc, 30.0, 8.0);
  glm::vec3 n(0.0f);
  REQUIRE(cam::FaceNormalOf(bodies, plusY, n));
  REQUIRE(n.y > 0.999f);
  double a = 0.0;
  REQUIRE(cam::SolveIndexA(glm::dvec3(n), a));
  a = cam::NormalizeIndexDeg(a);

  // Face the top at A0 (the model's +Z face), then face the +Y face at its
  // index: two angles, so the file is read by the rotary check.
  const auto faceAt = [&](uint64_t id, double index) {
    cam::Setup at = setup;
    at.rotationA = index;
    const cam::DerivedOp d =
        cam::DeriveOperation(at, bodies, cam::OpType::Face, cam::Target{});
    INFO(d.why);
    REQUIRE(d.ok);
    cam::Operation op = d.op;
    op.id = id;
    op.indexA = {index};
    return op;
  };
  setup.operations = {faceAt(1, 0.0), faceAt(2, a)};
  CHECK_THAT(setup.operations[1].bottomZ, WithinAbs(8.0, 1e-6));

  const std::vector<cam::Toolpath> paths = cam::GenerateAll(setup, bodies);
  REQUIRE(paths.size() == 2);
  for (const cam::Toolpath& tp : paths) {
    INFO(tp.error);
    REQUIRE(tp.error.empty());
  }
  std::string gcode, error;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, bodies, 0.5);
  INFO("A " << a << " gouge " << v.maxGouge << " at x " << v.gougeAt.x
            << " theta " << v.gougeAt.y);
  REQUIRE(v.rotary);
  CHECK(v.maxGouge < 0.1);
}
#endif
