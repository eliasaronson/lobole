#include "core/sketch/SketchInference.h"

#include <cmath>

namespace sketch {

namespace {
constexpr double kPointTolPx = 10.0; // coincident: cursor near a vertex
constexpr double kEdgeTolPx = 8.0;   // point-on-line: cursor near a segment
constexpr double kAlignTolPx =
    8.0; // axis alignment: cursor shares a vertex X/Y
constexpr double kAngleTanTol = 0.0349; // tan(2 deg): H/V slope ratio
constexpr double kAngleSinTol = 0.0349; // sin(2 deg): parallel/perpendicular

double Cross(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }

// Visits every named vertex of an entity: Point(0), Line(0,1), Circle/Ellipse
// centre(0), Arc centre(0)/start(1)/end(2), an ellipse's axis ends (3..6),
// spline poles, a label's anchor and corners — the coincident and alignment
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
  } else if (std::holds_alternative<Ellipse>(e)) {
    // The centre and the four axis ends (which 3..6).
    fn(uint8_t{0}, EntityPointUV(e, 0));
    for (uint8_t w = kEllipseMajorPos; w <= kEllipseMinorNeg; w++)
      fn(w, EntityPointUV(e, w));
  } else if (auto* ea = std::get_if<EllipseArc>(&e)) {
    fn(uint8_t{0}, ea->center);
    fn(uint8_t{1}, EllipseArcPointUV(*ea, 1));
    fn(uint8_t{2}, EllipseArcPointUV(*ea, 2));
    for (uint8_t w = kEllipseMajorPos; w <= kEllipseMinorNeg; w++)
      fn(w, EllipseArcPointUV(*ea, w));
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
double DistToSegment(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b,
                     glm::dvec2& closest) {
  glm::dvec2 ab = b - a;
  double len2 = glm::dot(ab, ab);
  double t =
      len2 > 1e-12 ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
  closest = a + t * ab;
  return glm::length(p - closest);
}
} // namespace

Snap InferPoint(const std::vector<Entity>& entities, glm::dvec2 uv,
                std::optional<glm::dvec2> other, uint32_t skipId,
                double pixelsPerUV) {
  Snap s;
  s.uv = uv;
  const double scale = pixelsPerUV > 1e-6 ? pixelsPerUV : 1.0;
  const double pointTol = kPointTolPx / scale;
  const double edgeTol = kEdgeTolPx / scale;

  // 1. Coincident — nearest existing vertex within tolerance.
  double bestPt = pointTol;
  for (const auto& e : entities) {
    const uint32_t id = EntityId(e);
    if (id == skipId)
      continue;
    ForEachVertex(e, [&](uint8_t which, glm::dvec2 p) {
      double d = glm::length(uv - p);
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
    s.uv = glm::dvec2(0.0);
  }
  if (s.coincident) {
    s.glyphs.push_back(GlyphKind::Coincident);
    return s; // point is pinned; no further snapping
  }

  // 2. Point-on-curve — nearest line body, circle or arc circumference
  // within tolerance (the constraint kind dispatches per target).
  double bestEdge = edgeTol;
  for (const auto& e : entities) {
    const uint32_t id = EntityId(e);
    if (id == skipId)
      continue;
    if (auto* l = std::get_if<Line>(&e)) {
      glm::dvec2 closest;
      double d = DistToSegment(uv, l->a, l->b, closest);
      if (d < bestEdge) {
        bestEdge = d;
        s.pointOnLine = id;
        s.uv = closest;
      }
    } else if (auto* c = std::get_if<Circle>(&e)) {
      const glm::dvec2 off = uv - c->center;
      const double len = glm::length(off);
      if (len < 1e-6)
        continue; // at the center: direction undefined
      const double d = std::abs(len - c->radius);
      if (d < bestEdge) {
        bestEdge = d;
        s.pointOnLine = id;
        s.uv = c->center + off * (c->radius / len);
      }
    } else if (auto* ar = std::get_if<Arc>(&e)) {
      const glm::dvec2 off = uv - ar->center;
      const double len = glm::length(off);
      if (len < 1e-6)
        continue;
      const double d = std::abs(len - ar->radius);
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
        s.uv = ar->center + off * (ar->radius / len);
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
      const double alignTol = kAlignTolPx / scale;
      // Points on the curve where it crosses the axis x=c (vertical=true) or
      // y=c, nearest to the current snap point; nullopt if none in range.
      auto curveAxisPoint = [&](bool vertical,
                                double c) -> std::optional<glm::dvec2> {
        std::vector<glm::dvec2> hits;
        if (auto* l = std::get_if<Line>(&target)) {
          const glm::dvec2 d = l->b - l->a;
          const double den = vertical ? d.x : d.y;
          if (std::abs(den) > 1e-9) {
            const double t = (c - (vertical ? l->a.x : l->a.y)) / den;
            if (t >= 0.0 && t <= 1.0)
              hits.push_back(l->a + t * d);
          }
        } else if (auto* ci = std::get_if<Circle>(&target)) {
          const double off = c - (vertical ? ci->center.x : ci->center.y);
          const double r = ci->radius;
          if (std::abs(off) <= r) {
            const double h = std::sqrt(std::max(0.0, r * r - off * off));
            if (vertical) {
              hits.push_back({c, ci->center.y + h});
              hits.push_back({c, ci->center.y - h});
            } else {
              hits.push_back({ci->center.x + h, c});
              hits.push_back({ci->center.x - h, c});
            }
          }
        } else if (auto* ar = std::get_if<Arc>(&target)) {
          const double off = c - (vertical ? ar->center.x : ar->center.y);
          const double r = ar->radius;
          if (std::abs(off) <= r) {
            const double h = std::sqrt(std::max(0.0, r * r - off * off));
            const glm::dvec2 c1 = vertical ? glm::dvec2(c, ar->center.y + h)
                                           : glm::dvec2(ar->center.x + h, c);
            const glm::dvec2 c2 = vertical ? glm::dvec2(c, ar->center.y - h)
                                           : glm::dvec2(ar->center.x - h, c);
            for (const glm::dvec2& p : {c1, c2}) {
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
        std::optional<glm::dvec2> best;
        for (const glm::dvec2& p : hits)
          if (!best || glm::length(p - s.uv) < glm::length(*best - s.uv))
            best = p;
        if (best && glm::length(*best - s.uv) <= alignTol)
          return best;
        return std::nullopt;
      };
      // Nearest share-X and share-Y vertex candidates (skip the curve's own
      // id for its endpoints, keep its centre — that's the top-of-circle
      // case; the origin joins at lower priority like the plain alignment).
      double bestX = alignTol, bestY = alignTol;
      std::optional<PointRef> refX, refY;
      double cX = 0.0, cY = 0.0;
      for (const auto& e : entities) {
        const uint32_t id = EntityId(e);
        if (id == skipId)
          continue;
        ForEachVertex(e, [&](uint8_t which, glm::dvec2 p) {
          const double dx = std::abs(s.uv.x - p.x);
          if (dx < bestX) {
            bestX = dx;
            cX = p.x;
            refX = PointRef{id, which};
          }
          const double dy = std::abs(s.uv.y - p.y);
          if (dy < bestY) {
            bestY = dy;
            cY = p.y;
            refY = PointRef{id, which};
          }
        });
      }
      if (std::abs(s.uv.x) < bestX) {
        bestX = std::abs(s.uv.x);
        cX = 0.0;
        refX = PointRef{kOrigin, 0};
      }
      if (std::abs(s.uv.y) < bestY) {
        bestY = std::abs(s.uv.y);
        cY = 0.0;
        refY = PointRef{kOrigin, 0};
      }
      // Prefer the closer axis; take the first that the curve can satisfy.
      struct Cand {
        bool vertical;
        double c;
        std::optional<PointRef> ref;
        double d;
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
    const double alignTol = kAlignTolPx / scale;
    double bestX = alignTol, bestY = alignTol;
    double snapX = uv.x, snapY = uv.y;
    for (const auto& e : entities) {
      const uint32_t id = EntityId(e);
      if (id == skipId)
        continue;
      ForEachVertex(e, [&](uint8_t which, glm::dvec2 p) {
        const double dx = std::abs(uv.x - p.x);
        if (dx < bestX) {
          bestX = dx;
          snapX = p.x;
          s.alignShareX = PointRef{id, which};
        }
        const double dy = std::abs(uv.y - p.y);
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
      snapX = 0.0;
      s.alignShareX = PointRef{kOrigin, 0};
    }
    if (std::abs(uv.y) < bestY) {
      bestY = std::abs(uv.y);
      snapY = 0.0;
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
  glm::dvec2 seg = s.uv - *other;
  double segLen = glm::length(seg);
  if (segLen < 1e-6)
    return s;
  glm::dvec2 dir = seg / segLen;

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
  double bestPar = kAngleSinTol, bestPerp = kAngleSinTol;
  for (const auto& e : entities) {
    const uint32_t id = EntityId(e);
    if (id == skipId)
      continue;
    auto* l = std::get_if<Line>(&e);
    if (!l)
      continue;
    glm::dvec2 ld = l->b - l->a;
    double ll = glm::length(ld);
    if (ll < 1e-6)
      continue;
    ld /= ll;
    double par = std::abs(Cross(dir, ld));     // |sin| — 0 when parallel
    double perp = std::abs(glm::dot(dir, ld)); // |cos| — 0 when perpendicular
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
