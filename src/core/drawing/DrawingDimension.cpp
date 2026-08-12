#include "core/drawing/DrawingDimension.h"

#include "core/io/Format.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace drawing {

namespace {

bool SameRef(const ElementRef& a, const ElementRef& b) {
  return a.IsSet() && b.IsSet() && a.featureId == b.featureId &&
         a.name == b.name;
}

// Every projected curve in the view a ProjRef names — by source edge or by
// silhouette face, occurrence-filtered for assembly sources.
std::vector<const SheetCurve*> Match(const SheetView& v, const ProjRef& r) {
  std::vector<const SheetCurve*> out;
  for (const SheetCurve& c : v.curves) {
    if (r.occurrenceId != 0 && c.sourceId != r.occurrenceId)
      continue;
    if (SameRef(r.edge, c.edge) || SameRef(r.silFace, c.silFace))
      out.push_back(&c);
  }
  return out;
}

// A ref's anchor point for a linear dimension: a circle's centre, an explicit
// endpoint (end 1|2), or the edge's midpoint (end 0).
bool RefPoint(const SheetView& v, const ProjRef& r, glm::dvec2& out) {
  const auto cs = Match(v, r);
  if (cs.empty())
    return false;
  for (const SheetCurve* c : cs)
    if (c->conicRadius > 0.0) {
      out = c->conicCenter;
      return true;
    }

  std::vector<glm::dvec2> pts;
  for (const SheetCurve* c : cs)
    pts.insert(pts.end(), c->points.begin(), c->points.end());
  if (pts.empty())
    return false;
  if (r.end == 1) {
    out = pts.front();
    return true;
  }
  if (r.end == 2) {
    out = pts.back();
    return true;
  }
  glm::dvec2 lo(1e30), hi(-1e30);
  for (const glm::dvec2& p : pts) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  out = 0.5 * (lo + hi); // edge midpoint (bbox centre — exact for a segment)
  return true;
}

// A ref's circle (centre + radius): a matched circle curve, or a pair of
// silhouette lines (a shaft side view) reduced to their midline + half-gap.
bool RefCircle(const SheetView& v, const ProjRef& r, glm::dvec2& c,
               double& rad) {
  const auto cs = Match(v, r);
  for (const SheetCurve* cc : cs)
    if (cc->conicRadius > 0.0) {
      c = cc->conicCenter;
      rad = cc->conicRadius;
      return true;
    }

  // Silhouette pair: two parallel generators 2R apart.
  std::vector<const SheetCurve*> lines;
  for (const SheetCurve* cc : cs)
    if (cc->points.size() >= 2)
      lines.push_back(cc);
  if (lines.size() >= 2) {
    const glm::dvec2 m0 =
        0.5 * (lines[0]->points.front() + lines[0]->points.back());
    const glm::dvec2 m1 =
        0.5 * (lines[1]->points.front() + lines[1]->points.back());
    c = 0.5 * (m0 + m1);
    rad = 0.5 * glm::length(m1 - m0);
    return rad > 0.0;
  }
  return false;
}

// A ref's dominant line: the longest matched segment's foot point + unit dir.
bool RefLine(const SheetView& v, const ProjRef& r, glm::dvec2& p,
             glm::dvec2& dir) {
  const auto cs = Match(v, r);
  const SheetCurve* best = nullptr;
  double bestLen = -1.0;
  for (const SheetCurve* c : cs) {
    if (c->points.size() < 2)
      continue;
    const double len = glm::length(c->points.back() - c->points.front());
    if (len > bestLen) {
      bestLen = len;
      best = c;
    }
  }
  if (!best)
    return false;
  p = best->points.front();
  const glm::dvec2 d = best->points.back() - best->points.front();
  const double L = glm::length(d);
  if (L < 1e-9)
    return false;
  dir = d / L;
  return true;
}

} // namespace

ResolvedDim ResolveDim(const DrawingDim& dim, const SheetView& view) {
  ResolvedDim rd;
  rd.id = dim.id;
  rd.kind = dim.kind;
  const glm::dvec2 off = dim.labelOffset;

  switch (dim.kind) {
  case DrawingDimKind::Distance:
  case DrawingDimKind::DistanceX:
  case DrawingDimKind::DistanceY: {
    if (dim.refs.size() < 2)
      return rd;
    glm::dvec2 a, b;
    if (!RefPoint(view, dim.refs[0], a) || !RefPoint(view, dim.refs[1], b))
      return rd;
    rd.ext1 = a;
    rd.ext2 = b;
    if (dim.kind == DrawingDimKind::DistanceX) {
      rd.value = std::abs(b.x - a.x);
      const double dy = std::min(a.y, b.y) + off.y;
      rd.dim1 = {a.x, dy};
      rd.dim2 = {b.x, dy};
    } else if (dim.kind == DrawingDimKind::DistanceY) {
      rd.value = std::abs(b.y - a.y);
      const double dx = std::min(a.x, b.x) + off.x;
      rd.dim1 = {dx, a.y};
      rd.dim2 = {dx, b.y};
    } else {
      rd.value = glm::length(b - a);
      rd.dim1 = a + off;
      rd.dim2 = b + off;
    }
    rd.text = core::FormatLength(rd.value);
    rd.labelPos = 0.5 * (rd.dim1 + rd.dim2);
    rd.resolved = true;
    break;
  }
  case DrawingDimKind::Radius:
  case DrawingDimKind::Diameter: {
    if (dim.refs.empty())
      return rd;
    glm::dvec2 c;
    double rad = 0.0;
    if (!RefCircle(view, dim.refs[0], c, rad))
      return rd;
    rd.radial = true;
    rd.center = c;
    rd.radius = rad;
    rd.value = dim.kind == DrawingDimKind::Diameter ? 2.0 * rad : rad;
    rd.text = (dim.kind == DrawingDimKind::Diameter ? "\xC3\x98" : "R") +
              core::FormatLength(rd.value);
    const glm::dvec2 dir = glm::length(off) > 1e-9 ? glm::normalize(off)
                                                   : glm::dvec2(0.7071, 0.7071);
    rd.rim = c + dir * rad;
    rd.labelPos = rd.rim + off;
    rd.resolved = true;
    break;
  }
  case DrawingDimKind::Angle: {
    if (dim.refs.size() < 2)
      return rd;
    glm::dvec2 pa, da, pb, db;
    if (!RefLine(view, dim.refs[0], pa, da) ||
        !RefLine(view, dim.refs[1], pb, db))
      return rd;
    const double d = std::clamp(std::abs(glm::dot(da, db)), 0.0, 1.0);
    rd.value = std::acos(d); // acute angle between the two edges
    rd.text = core::FormatAngle(rd.value);
    rd.ext1 = pa;
    rd.ext2 = pb;
    rd.labelPos = 0.5 * (pa + pb) + off;
    rd.resolved = true;
    break;
  }
  }
  return rd;
}

glm::dvec2 DefaultDimOffset(const DrawingDim& dim, const SheetView& view) {
  constexpr glm::dvec2 kFallback{0.0, -12.0}; // the historical GUI default
  if (!view.hasGeometry)
    return kFallback;
  // Probe-resolve with a zero offset to learn the anchors, then compute the
  // offset that puts the laid-out geometry outside the bbox per this kind's
  // exact offset semantics in ResolveDim above.
  DrawingDim probe = dim;
  probe.labelOffset = {0.0, 0.0};
  const ResolvedDim rd = ResolveDim(probe, view);
  if (!rd.resolved)
    return kFallback;
  const double below = view.bboxMin.y - kDimOffsetGapMM;
  const double left = view.bboxMin.x - kDimOffsetGapMM;
  switch (dim.kind) {
  case DrawingDimKind::Distance: // dim line = anchors + off
    return {0.0, below - std::max(rd.ext1.y, rd.ext2.y)};
  case DrawingDimKind::DistanceX: // line at min(a.y, b.y) + off.y
    return {0.0, below - std::min(rd.ext1.y, rd.ext2.y)};
  case DrawingDimKind::DistanceY: // line at min(a.x, b.x) + off.x
    return {left - std::min(rd.ext1.x, rd.ext2.x), 0.0};
  case DrawingDimKind::Radius:
  case DrawingDimKind::Diameter: {
    // Leader direction away from the view centre; offset length = gap.
    const glm::dvec2 centre = 0.5 * (view.bboxMin + view.bboxMax);
    const glm::dvec2 away = rd.center - centre;
    const glm::dvec2 dir = glm::length(away) > 1e-9
                               ? glm::normalize(away)
                               : glm::dvec2(0.7071, 0.7071);
    return dir * kDimOffsetGapMM;
  }
  case DrawingDimKind::Angle:
    return {0.0, below - 0.5 * (rd.ext1.y + rd.ext2.y)};
  default: // new kinds (callouts) fall back until they add an arm
    return kFallback;
  }
}

} // namespace drawing
