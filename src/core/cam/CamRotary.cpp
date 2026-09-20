#include "core/cam/CamRotary.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cam {

namespace {

constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Is the part-frame point at (x, theta, rad) inside the tool, with the tool's
// tip at `tip` and the part turned to `aRad`?
//
// The tool is the same solid the planar carve assumes: a body of revolution
// about setup-space Z, opening upward without limit, whose profile is
// `TipHeight`. "Without limit" is what makes a plunge remove the column above
// it, and it is as true here as there.
bool InsideTool(double x, double theta, double rad, double aRad,
                const glm::dvec3& tip, const Tool* tool, double toolR) {
  const double phi = theta + aRad;
  const double y = rad * std::cos(phi);
  const double z = rad * std::sin(phi);
  const double dx = x - tip.x, dy = y - tip.y;
  const double d = std::hypot(dx, dy);
  if (d > toolR)
    return false;
  const double surface = tip.z + (tool != nullptr ? TipHeight(*tool, d) : 0.0);
  return z > surface;
}

// The largest radius at or below `r0` that is NOT inside the tool.
//
// The ray from the axis outward enters the tool at most once for the shapes
// this app models -- they are convex upward about a single axis -- so the
// answer is a single boundary and bisection finds it. Two ends are answered
// without searching: an axis point inside the tool means the whole ray is
// gone (the cutter has passed the centreline, which is a real thing a rotary
// job does and the reason this is not just `min`), and a tip that does not
// reach `r0` leaves it alone.
double CarveRay(double x, double theta, double r0, double aRad,
                const glm::dvec3& tip, const Tool* tool, double toolR) {
  if (!InsideTool(x, theta, r0, aRad, tip, tool, toolR))
    return r0;
  if (InsideTool(x, theta, 0.0, aRad, tip, tool, toolR))
    return 0.0;
  double lo = 0.0, hi = r0; // lo outside, hi inside
  for (int k = 0; k < 40; k++) {
    const double mid = 0.5 * (lo + hi);
    if (InsideTool(x, theta, mid, aRad, tip, tool, toolR))
      hi = mid;
    else
      lo = mid;
  }
  return lo;
}

} // namespace

RadiusMap MakeRotaryStock(double x0, double x1, double radius, double res) {
  RadiusMap map;
  map.res = std::max(0.01, res);
  map.x0 = std::min(x0, x1);
  map.nx =
      std::max(1, static_cast<int>(std::ceil(std::abs(x1 - x0) / map.res)));
  // Derived, not typed: enough steps that a cell's arc AT THE BLANK'S SURFACE
  // is about one `res`. A literal step would be right at one diameter and
  // wrong at every other, which is the defaults rule this project enforces on
  // every other unit-carrying number.
  const double circumference = kTwoPi * std::max(0.01, radius);
  map.nTheta = std::clamp(static_cast<int>(std::ceil(circumference / map.res)),
                          8, 20000);
  map.r.assign(static_cast<size_t>(map.nx) * static_cast<size_t>(map.nTheta),
               static_cast<float>(std::max(0.0, radius)));
  return map;
}

RadiusMap SampleModelRotary(const std::vector<Body>& bodies,
                            const RadiusMap& like) {
  RadiusMap out = like;
  out.r.assign(out.r.size(), 0.0f);
  if (out.nx <= 0 || out.nTheta <= 0)
    return out;

  const double deflection = like.res > 0.0 ? like.res * 0.25 : 0.0;
  const auto splat = [&](const glm::dvec3& p) {
    const double rad = std::hypot(p.y, p.z);
    const int i = static_cast<int>((p.x - out.x0) / out.res);
    if (i < 0 || i >= out.nx)
      return;
    double theta = std::atan2(p.z, p.y);
    if (theta < 0.0)
      theta += kTwoPi;
    int j = static_cast<int>(theta / kTwoPi * out.nTheta);
    j = std::clamp(j, 0, out.nTheta - 1);
    if (rad > out.At(i, j))
      out.Set(i, j, static_cast<float>(rad));
  };

  for (const Body& body : bodies) {
    if (!body.shape.IsValid())
      continue;
    // Detached, for the same reason the planar sampler is: the triangulation
    // rides on the shared TShape, so meshing the body itself at the CHECK's
    // tolerance would make the renderer's mesh of the same part fine too.
    const CADShape::Mesh mesh = body.shape.GenerateMeshDetached(deflection);
    for (size_t tri = 0; tri + 2 < mesh.indices.size(); tri += 3) {
      const glm::dvec3 p0(mesh.vertices[mesh.indices[tri]].position);
      const glm::dvec3 p1(mesh.vertices[mesh.indices[tri + 1]].position);
      const glm::dvec3 p2(mesh.vertices[mesh.indices[tri + 2]].position);
      // How finely to walk this triangle: half a cell, measured in the
      // coarser of the two directions it spans. A triangle near the axis
      // needs fewer angular samples than one at the rim, which is exactly
      // what measuring the ARC rather than the angle gives.
      const double rad =
          std::max({std::hypot(p0.y, p0.z), std::hypot(p1.y, p1.z),
                    std::hypot(p2.y, p2.z)});
      const double step = std::max(0.25 * out.res, 0.5 * out.ArcAt(rad));
      // One count per EDGE, not one for the triangle. A cylinder's side
      // meshes as long thin triangles -- 60 mm by a fraction of one -- and a
      // single count from the longest edge either samples the short direction
      // absurdly finely or, once clamped, leaves whole cells between samples
      // along the long one. That is how a plain bar first sampled as a
      // radius of ZERO in places.
      const int nu = std::clamp(
          static_cast<int>(std::ceil(glm::length(p1 - p0) / step)), 1, 4096);
      const int nv = std::clamp(
          static_cast<int>(std::ceil(glm::length(p2 - p0) / step)), 1, 4096);
      for (int a = 0; a <= nu; a++)
        for (int b = 0; b <= nv; b++) {
          const double u = static_cast<double>(a) / nu;
          const double v = static_cast<double>(b) / nv;
          if (u + v > 1.0)
            continue; // outside the triangle
          splat(p0 + (p1 - p0) * u + (p2 - p0) * v);
        }
    }
  }
  return out;
}

void CarveSweepRotary(const glm::dvec3& from, const glm::dvec3& to,
                      double aFrom, double aTo, const Tool* tool,
                      RadiusMap& stock) {
  if (stock.nx <= 0 || stock.nTheta <= 0)
    return;
  const double toolR = tool != nullptr ? tool->diameter * 0.5 : 1.0;
  double rMax = 0.0;
  for (float v : stock.r)
    rMax = std::max(rMax, static_cast<double>(v));
  if (rMax <= 0.0)
    return;

  // A rotary move is a HELIX at the surface, not a straight line, so it is
  // walked in steps small enough that neither the tip nor the material under
  // it moves more than half a cell. The angular term uses the arc at the
  // stock's widest point, because that is where a given rotation moves the
  // most metal past the cutter -- stepping by the ANGLE instead would sample
  // a 200 mm blank as finely as a 20 mm one.
  const double travel = glm::length(to - from);
  const double turn = std::abs(aTo - aFrom) / 360.0 * kTwoPi * rMax;
  const int steps = std::clamp(
      static_cast<int>(std::ceil(std::max(travel, turn) / (0.5 * stock.res))),
      1, 100000);

  for (int s = 0; s <= steps; s++) {
    const double t = static_cast<double>(s) / steps;
    const glm::dvec3 tip = from + (to - from) * t;
    const double aRad = glm::radians(aFrom + (aTo - aFrom) * t);

    // Only the cells the tool can touch. Along X that is its own radius; in
    // theta it is every cell whose ray can rise into the cutter, which is the
    // half turn facing the spindle -- the other half is below the axis and
    // out of reach of a tool that only comes down.
    const int i0 =
        std::max(0, static_cast<int>((tip.x - toolR - stock.x0) / stock.res));
    const int i1 =
        std::min(stock.nx - 1,
                 static_cast<int>((tip.x + toolR - stock.x0) / stock.res) + 1);
    for (int i = i0; i <= i1; i++) {
      const double x = stock.CellX(i);
      if (std::abs(x - tip.x) > toolR)
        continue;
      for (int j = 0; j < stock.nTheta; j++) {
        const double theta = stock.CellTheta(j);
        // Cheap rejection before the bisection: a ray pointing away from the
        // spindle cannot meet a tool that only descends.
        if (std::sin(theta + aRad) <= 0.0)
          continue;
        const double r0 = stock.At(i, j);
        if (r0 <= 0.0)
          continue;
        const double cut = CarveRay(x, theta, r0, aRad, tip, tool, toolR);
        if (cut < r0)
          stock.Set(i, j, static_cast<float>(cut));
      }
    }
  }
}

} // namespace cam
