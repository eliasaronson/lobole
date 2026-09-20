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

#endif // HAVE_CAM
