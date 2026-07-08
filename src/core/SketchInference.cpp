#include "core/SketchInference.h"

#include <cmath>

namespace sketch {

namespace {
constexpr float kPointTolPx = 10.0f; // coincident: cursor near a vertex
constexpr float kEdgeTolPx = 8.0f;   // point-on-line: cursor near a segment
constexpr float kAlignTolPx = 8.0f;  // axis alignment: cursor shares a vertex X/Y
constexpr float kAngleTanTol = 0.0349f; // tan(2 deg): H/V slope ratio
constexpr float kAngleSinTol = 0.0349f; // sin(2 deg): parallel/perpendicular

float Cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

// Visits every named vertex of an entity: Point(0), Line(0,1), Circle centre(0),
// Arc centre(0)/start(1)/end(2) — the same candidate set as coincident.
template <class F>
void ForEachVertex(const Entity& e, F fn) {
  if (auto* p = std::get_if<Point>(&e))
    fn(uint8_t{0}, p->p);
  else if (auto* l = std::get_if<Line>(&e)) {
    fn(uint8_t{0}, l->a);
    fn(uint8_t{1}, l->b);
  } else if (auto* c = std::get_if<Circle>(&e))
    fn(uint8_t{0}, c->center);
  else if (auto* a = std::get_if<Arc>(&e)) {
    fn(uint8_t{0}, a->center);
    fn(uint8_t{1}, ArcPointUV(*a, 1));
    fn(uint8_t{2}, ArcPointUV(*a, 2));
  }
}

// Distance from p to segment [a,b], plus the closest point on it.
float DistToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2& closest) {
  glm::vec2 ab = b - a;
  float len2 = glm::dot(ab, ab);
  float t = len2 > 1e-12f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f)
                          : 0.0f;
  closest = a + t * ab;
  return glm::length(p - closest);
}
} // namespace

Snap InferPoint(const std::vector<Entity>& entities, glm::vec2 uv,
                std::optional<glm::vec2> other, uint32_t skipId,
                float pixelsPerUV) {
  Snap s;
  s.uv = uv;
  const float scale = pixelsPerUV > 1e-6f ? pixelsPerUV : 1.0f;
  const float pointTol = kPointTolPx / scale;
  const float edgeTol = kEdgeTolPx / scale;

  // 1. Coincident — nearest existing vertex within tolerance.
  float bestPt = pointTol;
  for (const auto& e : entities) {
    const uint32_t id = EntityId(e);
    if (id == skipId)
      continue;
    auto consider = [&](uint8_t which, glm::vec2 p) {
      float d = glm::length(uv - p);
      if (d < bestPt) {
        bestPt = d;
        s.coincident = PointRef{id, which};
        s.uv = p;
      }
    };
    if (auto* p = std::get_if<Point>(&e))
      consider(0, p->p);
    else if (auto* l = std::get_if<Line>(&e)) {
      consider(0, l->a);
      consider(1, l->b);
    } else if (auto* c = std::get_if<Circle>(&e))
      consider(0, c->center);
    else if (auto* a = std::get_if<Arc>(&e)) {
      consider(0, a->center);
      consider(1, ArcPointUV(*a, 1)); // start
      consider(2, ArcPointUV(*a, 2)); // end
    }
  }
  if (s.coincident) {
    s.glyphs.push_back(GlyphKind::Coincident);
    return s; // point is pinned; no further snapping
  }

  // 2. Point-on-line — nearest line body within tolerance.
  float bestEdge = edgeTol;
  for (const auto& e : entities) {
    const uint32_t id = EntityId(e);
    if (id == skipId)
      continue;
    if (auto* l = std::get_if<Line>(&e)) {
      glm::vec2 closest;
      float d = DistToSegment(uv, l->a, l->b, closest);
      if (d < bestEdge) {
        bestEdge = d;
        s.pointOnLine = id;
        s.uv = closest;
      }
    }
  }
  if (s.pointOnLine) {
    s.glyphs.push_back(GlyphKind::PointOnLine);
    return s; // point is on a line; no segment-level nudge
  }

  // 3. Axis alignment — snap X and/or Y to an existing vertex sharing it,
  //    independently per axis. Yields a two-point Vertical (shared X) /
  //    Horizontal (shared Y) constraint on commit. Checked before the segment
  //    snaps so aligning to existing geometry claims that axis first.
  bool alignedX = false, alignedY = false;
  {
    const float alignTol = kAlignTolPx / scale;
    float bestX = alignTol, bestY = alignTol;
    float snapX = uv.x, snapY = uv.y;
    for (const auto& e : entities) {
      const uint32_t id = EntityId(e);
      if (id == skipId)
        continue;
      ForEachVertex(e, [&](uint8_t which, glm::vec2 p) {
        const float dx = std::abs(uv.x - p.x);
        if (dx < bestX) {
          bestX = dx;
          snapX = p.x;
          s.alignShareX = PointRef{id, which};
        }
        const float dy = std::abs(uv.y - p.y);
        if (dy < bestY) {
          bestY = dy;
          snapY = p.y;
          s.alignShareY = PointRef{id, which};
        }
      });
    }
    if (s.alignShareX) {
      s.uv.x = snapX;
      alignedX = true;
      s.glyphs.push_back(GlyphKind::Vertical); // shared X = vertical alignment
    }
    if (s.alignShareY) {
      s.uv.y = snapY;
      alignedY = true;
      s.glyphs.push_back(GlyphKind::Horizontal); // shared Y = horizontal alignment
    }
  }

  // 4. Segment-level (needs the fixed other end). Skip an axis already claimed
  //    by an alignment above; skip parallel/perp entirely once aligned.
  if (!other)
    return s;
  glm::vec2 seg = s.uv - *other;
  float segLen = glm::length(seg);
  if (segLen < 1e-6f)
    return s;
  glm::vec2 dir = seg / segLen;

  // Horizontal / vertical by slope ratio.
  if (!alignedY && std::abs(dir.y) <= std::abs(dir.x) * kAngleTanTol) {
    s.horizontal = true;
    s.uv.y = other->y; // snap flat
    s.glyphs.push_back(GlyphKind::Horizontal);
    return s;
  }
  if (!alignedX && std::abs(dir.x) <= std::abs(dir.y) * kAngleTanTol) {
    s.vertical = true;
    s.uv.x = other->x;
    s.glyphs.push_back(GlyphKind::Vertical);
    return s;
  }
  if (alignedX || alignedY)
    return s; // a positional snap already applied; don't also rotate the segment

  // Parallel / perpendicular to the nearest matching line direction.
  float bestPar = kAngleSinTol, bestPerp = kAngleSinTol;
  for (const auto& e : entities) {
    const uint32_t id = EntityId(e);
    if (id == skipId)
      continue;
    auto* l = std::get_if<Line>(&e);
    if (!l)
      continue;
    glm::vec2 ld = l->b - l->a;
    float ll = glm::length(ld);
    if (ll < 1e-6f)
      continue;
    ld /= ll;
    float par = std::abs(Cross(dir, ld));  // |sin| — 0 when parallel
    float perp = std::abs(glm::dot(dir, ld)); // |cos| — 0 when perpendicular
    if (par < bestPar) {
      bestPar = par;
      s.parallelTo = id;
      s.perpendicularTo.reset();
    }
    if (perp < bestPerp) {
      bestPerp = perp;
      s.perpendicularTo = id;
    }
  }
  // Prefer the stronger of the two if both matched.
  if (s.parallelTo && s.perpendicularTo) {
    if (bestPar <= bestPerp)
      s.perpendicularTo.reset();
    else
      s.parallelTo.reset();
  }
  if (s.parallelTo)
    s.glyphs.push_back(GlyphKind::Parallel);
  else if (s.perpendicularTo)
    s.glyphs.push_back(GlyphKind::Perpendicular);
  return s;
}

} // namespace sketch
