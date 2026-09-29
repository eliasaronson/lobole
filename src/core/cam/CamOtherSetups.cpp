#include "core/cam/CamOtherSetups.h"

#include "core/cam/CamGcode.h"
#include "core/cam/CamToolpath.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cam {

bool SetupStock::Removes(const glm::dvec3& model, double tol) const {
  const glm::dvec3 q = glm::dvec3(modelToSetup * glm::dvec4(model, 1.0));
  if (!rotary) {
    const HeightMap& h = height;
    const int i = static_cast<int>(std::floor((q.x - h.x0) / h.res));
    const int j = static_cast<int>(std::floor((q.y - h.y0) / h.res));
    if (i < 0 || j < 0 || i >= h.nx || j >= h.ny)
      return false;
    if (q.z < zLo - tol || q.z > zHi + tol)
      return false;
    return q.z > h.At(i, j) - tol;
  }
  const RadiusMap& m = radius;
  const int i = static_cast<int>(std::floor((q.x - m.x0) / m.res));
  if (i < 0 || i >= m.nx || m.nTheta <= 0)
    return false;
  const double r = std::hypot(q.y, q.z);
  if (r > m.rMax + tol)
    return false;
  double theta = std::atan2(q.z, q.y);
  if (theta < 0.0)
    theta += 2.0 * std::numbers::pi;
  const int j = std::clamp(
      static_cast<int>(std::floor(theta / (2.0 * std::numbers::pi) * m.nTheta)),
      0, m.nTheta - 1);
  return r > m.At(i, j) - tol;
}

uint64_t OtherSetups::Claims(const glm::dvec3& from, const glm::dvec3& to,
                             double tol) const {
  if (stocks.empty())
    return 0;
  const double len = glm::length(to - from);
  const int n =
      std::clamp(static_cast<int>(std::ceil(len / std::max(tol, 0.1))), 1, 400);
  std::vector<size_t> count(stocks.size(), 0);
  for (int k = 0; k <= n; k++) {
    const glm::dvec3 p = from + (to - from) * (static_cast<double>(k) / n);
    const glm::dvec3 model = glm::dvec3(toModel * glm::dvec4(p, 1.0));
    bool any = false;
    for (size_t s = 0; s < stocks.size(); s++)
      if (stocks[s].Removes(model, tol)) {
        count[s]++;
        any = true;
      }
    if (!any)
      return 0;
  }
  const size_t best = static_cast<size_t>(
      std::max_element(count.begin(), count.end()) - count.begin());
  return stocks[best].id;
}

std::string OtherSetups::NameOf(uint64_t id) const {
  for (const SetupStock& s : stocks)
    if (s.id == id)
      return s.name;
  return "";
}

bool ModelToSetup(const Setup& setup, const std::vector<Body>& rawBodies,
                  glm::dmat4& out, std::string* why) {
  bool have = false;
  for (const Body& b : rawBodies) {
    const glm::dmat4 m = BodyToSetup(setup, b.jobPartId);
    if (!have) {
      out = m;
      have = true;
      continue;
    }
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++)
        if (std::abs(m[c][r] - out[c][r]) > 1e-9) {
          if (why != nullptr)
            *why = "its parts are clamped or placed differently from one "
                   "another, so a point of leftover has no one place on the "
                   "part";
          return false;
        }
  }
  if (!have) {
    if (why != nullptr)
      *why = "it has no part";
    return false;
  }
  return true;
}

OtherSetups OtherSetupsOf(const Setup& self,
                          const std::vector<SetupProgram>& others,
                          const std::vector<Body>& rawBodies, double res) {
  OtherSetups out;
  if (others.empty())
    return out;
  glm::dmat4 mine(1.0);
  std::string why;
  if (!ModelToSetup(self, rawBodies, mine, &why)) {
    out.notChecked.push_back("the job's other setups, because in this one " +
                             why);
    return out;
  }
  out.toModel = glm::inverse(mine);
  for (const SetupProgram& p : others) {
    if (p.setup == nullptr || p.gcode.empty())
      continue; // cuts nothing: its stock is whole, and claims nothing
    const Setup& s = *p.setup;
    const std::string label = "setup '" + s.name + "', because in it";
    SetupStock stock;
    stock.id = s.id;
    stock.name = s.name;
    stock.earlier = p.earlier;
    if (!ModelToSetup(s, rawBodies, stock.modelToSetup, &why)) {
      out.notChecked.push_back(label + " " + why);
      continue;
    }
    if (RotatesWhileCutting(ProgramInSetupSpace(p.gcode, s))) {
      RotaryInspection ri =
          InspectRotary(p.gcode, s, rawBodies, std::max(res, 0.5));
      if (ri.machined.r.empty()) {
        out.notChecked.push_back(label + " " + ri.why);
        continue;
      }
      stock.rotary = true;
      stock.radius = std::move(ri.machined);
    } else {
      glm::dvec3 lo(0.0), hi(0.0);
      Inspection in = Inspect(p.gcode, s, rawBodies, res);
      if (!in.ok || !StockBounds(s, rawBodies, lo, hi)) {
        out.notChecked.push_back(label + " there is no stock to cut");
        continue;
      }
      stock.height = std::move(in.machined);
      stock.zLo = lo.z;
      stock.zHi = hi.z;
    }
    out.stocks.push_back(std::move(stock));
  }
  return out;
}

namespace {

// Whether an earlier setup left air at `model`, `margin` clear of what it
// left, so a seed errs toward material -- toward a finding.
bool AirAfter(const SetupStock& s, const glm::dvec3& model, double margin) {
  // Beyond a rotary setup's blank there was never any bar: a later setup's
  // box is the bounding box of that round (or square) bar, and its corners
  // past the bar's radius are air. Removes does not claim outside a stock,
  // rightly for a claim, and left the whole seed at the box's top.
  if (s.rotary) {
    const RadiusMap& m = s.radius;
    const glm::dvec3 q = glm::dvec3(s.modelToSetup * glm::dvec4(model, 1.0));
    const int k = static_cast<int>(std::floor((q.x - m.x0) / m.res));
    if (k < 0 || k >= m.nx || m.nTheta <= 0)
      return false; // off its length: not this setup's to say
    double th = std::atan2(q.z, q.y);
    if (th < 0.0)
      th += 2.0 * std::numbers::pi;
    const int t = std::clamp(
        static_cast<int>(std::floor(th / (2.0 * std::numbers::pi) * m.nTheta)),
        0, m.nTheta - 1);
    return std::hypot(q.y, q.z) >
           std::min(static_cast<double>(m.At(k, t)), m.rMax) + margin;
  }
  // Removes() calls a point air when it is `tol` INSIDE what a setup left;
  // a negative tol asks for `margin` of clearance instead.
  return s.Removes(model, -std::abs(margin));
}

std::vector<const SetupStock*> EarlierOf(const OtherSetups& others) {
  std::vector<const SetupStock*> earlier;
  for (const SetupStock& s : others.stocks)
    if (s.earlier)
      earlier.push_back(&s);
  return earlier;
}

bool AirAfterAny(const std::vector<const SetupStock*>& earlier,
                 const glm::dvec3& model, double margin) {
  return std::any_of(earlier.begin(), earlier.end(), [&](const SetupStock* s) {
    return AirAfter(*s, model, margin);
  });
}

} // namespace

void SeedFromEarlier(HeightMap& stock, const OtherSetups& others, double zLo,
                     double margin) {
  const std::vector<const SetupStock*> earlier = EarlierOf(others);
  if (earlier.empty() || stock.res <= 0.0)
    return;
  for (int j = 0; j < stock.ny; j++)
    for (int i = 0; i < stock.nx; i++) {
      const double x = stock.CellX(i), y = stock.CellY(j);
      double z = stock.At(i, j);
      while (z > zLo &&
             AirAfterAny(earlier,
                         glm::dvec3(others.toModel * glm::dvec4(x, y, z, 1.0)),
                         margin))
        z -= stock.res;
      stock.Set(i, j, static_cast<float>(std::max(z, zLo)));
    }
}

void SeedFromEarlier(RadiusMap& stock, const OtherSetups& others,
                     double margin) {
  const std::vector<const SetupStock*> earlier = EarlierOf(others);
  if (earlier.empty() || stock.res <= 0.0 || stock.nTheta <= 0)
    return;
  // The height map's walk, inward along each ray instead of down each
  // column: a turning setup after an indexed one, or after another turning,
  // starts from what those left.
  for (int j = 0; j < stock.nTheta; j++) {
    const double th = stock.CellTheta(j);
    const double c = std::cos(th), s = std::sin(th);
    for (int i = 0; i < stock.nx; i++) {
      const double x = stock.CellX(i);
      double r = stock.At(i, j);
      while (r > 0.0 &&
             AirAfterAny(
                 earlier,
                 glm::dvec3(others.toModel * glm::dvec4(x, r * c, r * s, 1.0)),
                 margin))
        r -= stock.res;
      stock.Set(i, j, static_cast<float>(std::max(r, 0.0)));
    }
  }
}

double SeedMargin(double res) { return std::max(0.3, res); }

bool StartingStock(const Setup& setup, const std::vector<Body>& rawBodies,
                   double res, const OtherSetups* others, HeightMap& out) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, rawBodies, lo, hi))
    return false;
  out = MakeStock(lo, hi, res);
  if (others != nullptr && others->HasEarlier())
    SeedFromEarlier(out, *others, lo.z, SeedMargin(res));
  return true;
}

RadiusMap StartingRotaryStock(const Setup& setup,
                              const std::vector<Body>& rawBodies, double res,
                              const OtherSetups* others, std::string* why) {
  RadiusMap out =
      MakeRotaryBlank(setup, BodiesInSetupFrame(setup, rawBodies), res, why);
  if (!out.r.empty() && others != nullptr && others->HasEarlier())
    SeedFromEarlier(out, *others, SeedMargin(res));
  return out;
}

} // namespace cam
