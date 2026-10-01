#include "core/sketch/SketchSelection.h"

#include "core/sketch/SketchProfile.h" // TessellateEntity

#include <cmath>

namespace sketch {

namespace {
bool Inside(glm::dvec2 p, glm::dvec2 mn, glm::dvec2 mx) {
  return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y;
}

// Liang–Barsky: does segment [a,b] intersect (or lie within) the AABB [mn,mx]?
bool SegmentHitsRect(glm::dvec2 a, glm::dvec2 b, glm::dvec2 mn, glm::dvec2 mx) {
  double t0 = 0.0, t1 = 1.0;
  const glm::dvec2 d = b - a;
  auto clip = [&](double p, double q) {
    if (std::abs(p) < 1e-9)
      return q >= 0.0; // parallel to this edge: inside iff on the inner side
    const double r = q / p;
    if (p < 0.0) {
      if (r > t1)
        return false;
      if (r > t0)
        t0 = r;
    } else {
      if (r < t0)
        return false;
      if (r < t1)
        t1 = r;
    }
    return true;
  };
  return clip(-d.x, a.x - mn.x) && clip(d.x, mx.x - a.x) &&
         clip(-d.y, a.y - mn.y) && clip(d.y, mx.y - a.y) && t0 <= t1;
}

// Screen-space sample points of an entity: a Point is its single point (which
// TessellateEntity leaves empty), everything else its tessellated polyline.
std::vector<glm::dvec2>
SamplePoints(const Entity& e,
             const std::function<glm::dvec2(glm::dvec2)>& project) {
  std::vector<glm::dvec2> uv;
  if (auto* p = std::get_if<Point>(&e))
    uv = {p->p};
  else if (std::holds_alternative<Text>(e)) // every glyph's outline
    for (const auto& poly : TessellateEntityPolylines(e))
      uv.insert(uv.end(), poly.begin(), poly.end());
  else
    uv = TessellateEntity(e);
  std::vector<glm::dvec2> screen;
  screen.reserve(uv.size());
  for (const auto& q : uv)
    screen.push_back(project(q));
  return screen;
}
} // namespace

std::vector<uint32_t>
BoxSelect(const std::vector<Entity>& entities, glm::dvec2 rectMin,
          glm::dvec2 rectMax, bool crossing,
          const std::function<glm::dvec2(glm::dvec2)>& project) {
  const glm::dvec2 mn(std::min(rectMin.x, rectMax.x),
                      std::min(rectMin.y, rectMax.y));
  const glm::dvec2 mx(std::max(rectMin.x, rectMax.x),
                      std::max(rectMin.y, rectMax.y));
  std::vector<uint32_t> out;
  for (const auto& e : entities) {
    const std::vector<glm::dvec2> pts = SamplePoints(e, project);
    if (pts.empty())
      continue;
    bool keep = false;
    if (!crossing) {
      keep = true; // window: every sample point must be inside
      for (const auto& p : pts)
        if (!Inside(p, mn, mx)) {
          keep = false;
          break;
        }
    } else {
      for (const auto& p : pts) // crossing: any point inside …
        if (Inside(p, mn, mx)) {
          keep = true;
          break;
        }
      for (size_t i = 0; !keep && i + 1 < pts.size();
           i++) // … or an edge crosses
        if (SegmentHitsRect(pts[i], pts[i + 1], mn, mx))
          keep = true;
    }
    if (keep)
      out.push_back(EntityId(e));
  }
  return out;
}

} // namespace sketch
