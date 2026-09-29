#include "core/sketch/SketchInference.h"

#include <cmath>

namespace sketch {

namespace {
constexpr float kPointTolPx = 10.0f; // coincident: cursor near a vertex
constexpr float kEdgeTolPx = 8.0f;   // point-on-line: cursor near a segment
constexpr float kAlignTolPx =
    8.0f; // axis alignment: cursor shares a vertex X/Y
constexpr float kAngleTanTol = 0.0349f; // tan(2 deg): H/V slope ratio
constexpr float kAngleSinTol = 0.0349f; // sin(2 deg): parallel/perpendicular

float Cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }

// Visits every named vertex of an entity: Point(0), Line(0,1), Circle/Ellipse
// centre(0), Arc centre(0)/start(1)/end(2) — the coincident and alignment
// candidate set (each is a real solver point, so constraints can bind to it).
template <class F> void ForEachVertex(const Entity& e, F fn) {
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
  } else if (auto* el = std::get_if<Ellipse>(&e))
    fn(uint8_t{0}, el->center);
  else if (auto* ea = std::get_if<EllipseArc>(&e)) {
    fn(uint8_t{0}, ea->center);
    fn(uint8_t{1}, EllipseArcPointUV(*ea, 1));
    fn(uint8_t{2}, EllipseArcPointUV(*ea, 2));
  } else if (auto* bs = std::get_if<BSpline>(&e)) {
    const size_t nPoles = std::min<size_t>(bs->poles.size(), 256);
    for (size_t k = 0; k < nPoles; k++)
      fn(static_cast<uint8_t>(k), bs->poles[k]);
  } else if (std::holds_alternative<Text>(e)) {
    // The anchor and the box corners: all five are solver points.
    for (uint8_t k = 0; k <= 4; k++)
      fn(k, EntityPointUV(e, k));
  }
}

// Distance from p to segment [a,b], plus the closest point on it.
float DistToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2& closest) {
  glm::vec2 ab = b - a;
  float len2 = glm::dot(ab, ab);
  float t =
      len2 > 1e-12f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
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
    ForEachVertex(e, [&](uint8_t which, glm::vec2 p) {
      float d = glm::length(uv - p);
      if (d < bestPt) {
        bestPt = d;
        s.coincident = PointRef{id, which};
        s.uv = p;
      }
    });
  }
  // Datum: the origin is always a coincident candidate.
  if (glm::length(uv) < bestPt) {
    bestPt = glm::length(uv);
    s.coincident = PointRef{kOrigin, 0};
    s.uv = glm::vec2(0.0f);
  }
  if (s.coincident) {
    s.glyphs.push_back(GlyphKind::Coincident);
    return s; // point is pinned; no further snapping
  }

  // 2. Point-on-curve — nearest line body, circle or arc circumference
  // within tolerance (the constraint kind dispatches per target).
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
    } else if (auto* c = std::get_if<Circle>(&e)) {
      const glm::vec2 off = uv - c->center;
      const float len = glm::length(off);
      if (len < 1e-6f)
        continue; // at the center: direction undefined
      const float d = std::abs(len - static_cast<float>(c->radius));
      if (d < bestEdge) {
        bestEdge = d;
        s.pointOnLine = id;
        s.uv = c->center + off * (static_cast<float>(c->radius) / len);
      }
    } else if (auto* ar = std::get_if<Arc>(&e)) {
      const glm::vec2 off = uv - ar->center;
      const float len = glm::length(off);
      if (len < 1e-6f)
        continue;
      const float d = std::abs(len - static_cast<float>(ar->radius));
      if (d >= bestEdge)
        continue;
      // Only within the arc's CCW angular span.
      double ang = std::atan2(off.y, off.x);
      double s0 = ar->startAngle, s1 = ar->endAngle;
      while (s1 <= s0)
        s1 += 2.0 * M_PI;
      while (ang < s0)
        ang += 2.0 * M_PI;
      if (ang <= s1) {
        bestEdge = d;
        s.pointOnLine = id;
        s.uv = ar->center + off * (static_cast<float>(ar->radius) / len);
      }
    }
  }
  if (s.pointOnLine) {
    // Combine with an axis alignment when the curve can satisfy both: slide
    // the snapped point ALONG the curve to its intersection with the aligned
    // axis (e.g. the top of a circle = on the rim AND vertically aligned
    // with its centre; a line point aligned with some other vertex).
    const size_t ti = [&] {
      for (size_t i = 0; i < entities.size(); i++)
        if (EntityId(entities[i]) == *s.pointOnLine)
          return i;
      return entities.size();
    }();
    if (ti < entities.size()) {
      const Entity& target = entities[ti];
      const float alignTol = kAlignTolPx / scale;
      // Points on the curve where it crosses the axis x=c (vertical=true) or
      // y=c, nearest to the current snap point; nullopt if none in range.
      auto curveAxisPoint = [&](bool vertical,
                                float c) -> std::optional<glm::vec2> {
        std::vector<glm::vec2> hits;
        if (auto* l = std::get_if<Line>(&target)) {
          const glm::vec2 d = l->b - l->a;
          const float den = vertical ? d.x : d.y;
          if (std::abs(den) > 1e-9f) {
            const float t = (c - (vertical ? l->a.x : l->a.y)) / den;
            if (t >= 0.0f && t <= 1.0f)
              hits.push_back(l->a + t * d);
          }
        } else if (auto* ci = std::get_if<Circle>(&target)) {
          const float off = c - (vertical ? ci->center.x : ci->center.y);
          const float r = static_cast<float>(ci->radius);
          if (std::abs(off) <= r) {
            const float h = std::sqrt(std::max(0.0f, r * r - off * off));
            if (vertical) {
              hits.push_back({c, ci->center.y + h});
              hits.push_back({c, ci->center.y - h});
            } else {
              hits.push_back({ci->center.x + h, c});
              hits.push_back({ci->center.x - h, c});
            }
          }
        } else if (auto* ar = std::get_if<Arc>(&target)) {
          const float off = c - (vertical ? ar->center.x : ar->center.y);
          const float r = static_cast<float>(ar->radius);
          if (std::abs(off) <= r) {
            const float h = std::sqrt(std::max(0.0f, r * r - off * off));
            const glm::vec2 c1 = vertical ? glm::vec2(c, ar->center.y + h)
                                          : glm::vec2(ar->center.x + h, c);
            const glm::vec2 c2 = vertical ? glm::vec2(c, ar->center.y - h)
                                          : glm::vec2(ar->center.x - h, c);
            for (const glm::vec2& p : {c1, c2}) {
              double ang = std::atan2(p.y - ar->center.y, p.x - ar->center.x);
              double s0 = ar->startAngle, s1 = ar->endAngle;
              while (s1 <= s0)
                s1 += 2.0 * M_PI;
              while (ang < s0)
                ang += 2.0 * M_PI;
              if (ang <= s1)
                hits.push_back(p);
            }
          }
        }
        std::optional<glm::vec2> best;
        for (const glm::vec2& p : hits)
          if (!best || glm::length(p - s.uv) < glm::length(*best - s.uv))
            best = p;
        if (best && glm::length(*best - s.uv) <= alignTol)
          return best;
        return std::nullopt;
      };
      // Nearest share-X and share-Y vertex candidates (skip the curve's own
      // id for its endpoints, keep its centre — that's the top-of-circle
      // case; the origin joins at lower priority like the plain alignment).
      float bestX = alignTol, bestY = alignTol;
      std::optional<PointRef> refX, refY;
      float cX = 0.0f, cY = 0.0f;
      for (const auto& e : entities) {
        const uint32_t id = EntityId(e);
        if (id == skipId)
          continue;
        ForEachVertex(e, [&](uint8_t which, glm::vec2 p) {
          const float dx = std::abs(s.uv.x - p.x);
          if (dx < bestX) {
            bestX = dx;
            cX = p.x;
            refX = PointRef{id, which};
          }
          const float dy = std::abs(s.uv.y - p.y);
          if (dy < bestY) {
            bestY = dy;
            cY = p.y;
            refY = PointRef{id, which};
          }
        });
      }
      if (std::abs(s.uv.x) < bestX) {
        bestX = std::abs(s.uv.x);
        cX = 0.0f;
        refX = PointRef{kOrigin, 0};
      }
      if (std::abs(s.uv.y) < bestY) {
        bestY = std::abs(s.uv.y);
        cY = 0.0f;
        refY = PointRef{kOrigin, 0};
      }
      // Prefer the closer axis; take the first that the curve can satisfy.
      struct Cand {
        bool vertical;
        float c;
        std::optional<PointRef> ref;
        float d;
      };
      Cand cands[2] = {{true, cX, refX, bestX}, {false, cY, refY, bestY}};
      if (cands[1].d < cands[0].d)
        std::swap(cands[0], cands[1]);
      for (const Cand& cand : cands) {
        if (!cand.ref)
          continue;
        if (auto p = curveAxisPoint(cand.vertical, cand.c)) {
          s.uv = *p;
          if (cand.vertical) {
            s.alignShareX = cand.ref;
            s.glyphs.push_back(GlyphKind::Vertical);
          } else {
            s.alignShareY = cand.ref;
            s.glyphs.push_back(GlyphKind::Horizontal);
          }
          break; // one axis is enough (both would over-constrain the curve)
        }
      }
    }
    s.glyphs.push_back(GlyphKind::PointOnLine);
    return s; // point is on a curve; no segment-level nudge
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
    // Datum: align to the origin's X / Y too ("vertical / horizontal to the
    // origin"). Lower priority than a real vertex (strict <, so a vertex tie
    // wins), yielding a two-point Vertical/Horizontal to the origin on commit.
    if (std::abs(uv.x) < bestX) {
      bestX = std::abs(uv.x);
      snapX = 0.0f;
      s.alignShareX = PointRef{kOrigin, 0};
    }
    if (std::abs(uv.y) < bestY) {
      bestY = std::abs(uv.y);
      snapY = 0.0f;
      s.alignShareY = PointRef{kOrigin, 0};
    }
    if (s.alignShareX) {
      s.uv.x = snapX;
      alignedX = true;
      s.glyphs.push_back(GlyphKind::Vertical); // shared X = vertical alignment
    }
    if (s.alignShareY) {
      s.uv.y = snapY;
      alignedY = true;
      s.glyphs.push_back(
          GlyphKind::Horizontal); // shared Y = horizontal alignment
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
    return s; // a positional snap already applied; don't also rotate the
              // segment

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
    float par = std::abs(Cross(dir, ld));     // |sin| — 0 when parallel
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
