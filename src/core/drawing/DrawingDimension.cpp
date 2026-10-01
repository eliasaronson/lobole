#include "core/drawing/DrawingDimension.h"

#include "core/Angles.h"
#include "core/Document.h"
#include "core/drawing/DrawingCallout.h"
#include "core/features/FastenerFeature.h"
#include "core/features/HoleFeature.h"
#include "core/io/Format.h"
#include "core/sheet/Unfold.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
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
  std::vector<glm::dvec2> pts;
  for (const SheetCurve* c : cs)
    pts.insert(pts.end(), c->points.begin(), c->points.end());
  // An END asked for is that end, whatever the curve is. A circle answered
  // with its centre first, and a circle seen EDGE-ON is drawn as a line: the
  // two ends of a turned chess piece's underside measured its radius.
  // An extreme asked for is that extreme, circle or not: a balloon on a
  // bearing seen end-on landed on the bore's empty centre.
  if (r.end == 0 && r.extreme.empty())
    for (const SheetCurve* c : cs)
      if (c->conicRadius > 0.0) {
        out = c->conicCenter;
        return true;
      }
  if (pts.empty())
    return false;
  // The point furthest toward one side of the sheet, over every segment of
  // the curve, hidden ones too: an extreme is the shape's, not what shows.
  if (!r.extreme.empty()) {
    const bool alongX = r.extreme == "left" || r.extreme == "right";
    const double sign = r.extreme == "left" || r.extreme == "bottom" ? -1 : 1;
    out = pts.front();
    for (const glm::dvec2& p : pts)
      if (sign * (alongX ? p.x - out.x : p.y - out.y) > 0.0)
        out = p;
    return true;
  }
  // An end is an end of the curve list_drawing_geometry REPORTS: the first
  // visible segment of the ref (else its first), whose front and back are
  // p1 and p2 there. Taken off every segment joined, the ends of a circle
  // seen edge-on -- a visible half and a hidden half, back where it began --
  // were the same point, and a base's diameter measured 0.
  if (r.end == 1 || r.end == 2) {
    const SheetCurve* rep = cs.front();
    for (const SheetCurve* c : cs)
      if (c->visible && !c->points.empty()) {
        rep = c;
        break;
      }
    if (rep->points.empty())
      return false;
    out = r.end == 1 ? rep->points.front() : rep->points.back();
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

// A ref that IS a straight edge: foot point + unit direction. A circle's
// polyline must not pass for one — a conic match disqualifies, and the
// points must lie on their own chord. Distinguishing this matters because a
// plain `distance` involving an edge measures PERPENDICULAR to it; anchoring
// an edge at its midpoint measured the diagonal instead (an edge-to-hole
// distance on a 144-centred hole read "170", and two parallel window edges
// with offset midpoints read "78" for a 45 mm rail — both real drawings).
bool RefStraightLine(const SheetView& v, const ProjRef& r, glm::dvec2& p,
                     glm::dvec2& dir) {
  const auto cs = Match(v, r);
  if (cs.empty())
    return false;
  for (const SheetCurve* c : cs)
    if (c->conicRadius > 0.0)
      return false;
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
  if (!best || bestLen < 1e-9)
    return false;
  const glm::dvec2 a = best->points.front();
  const glm::dvec2 d = (best->points.back() - a) / bestLen;
  const glm::dvec2 n(-d.y, d.x);
  for (const glm::dvec2& q : best->points)
    if (std::abs(glm::dot(q - a, n)) > std::max(1e-6, bestLen * 1e-6))
      return false; // curved (an arc or ellipse projected without a conic)
  p = a;
  dir = d;
  return true;
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

// Two parallel straight curves of one ref, apart -- a cylinder seen from the
// side: {a0, a1, b0, b1}, each pair a line's ends. Nothing when the ref
// matches a conic (a circle is measured round, not across).
std::optional<std::array<glm::dvec2, 4>> RefLinePair(const SheetView& v,
                                                     const ProjRef& r) {
  std::vector<const SheetCurve*> lines;
  for (const SheetCurve* c : Match(v, r)) {
    if (c->conicRadius > 0.0)
      return std::nullopt;
    if (c->points.size() < 2)
      continue;
    const glm::dvec2 a = c->points.front(), b = c->points.back();
    const double len = glm::length(b - a);
    if (len < 1e-9)
      continue;
    const glm::dvec2 n(-(b - a).y / len, (b - a).x / len);
    bool straight = true;
    for (const glm::dvec2& q : c->points)
      straight = straight && std::abs(glm::dot(q - a, n)) < 1e-6 + len * 1e-6;
    if (straight)
      lines.push_back(c);
  }
  const auto length = [](const SheetCurve* c) {
    return glm::length(c->points.back() - c->points.front());
  };
  std::sort(lines.begin(), lines.end(),
            [&](const SheetCurve* x, const SheetCurve* y) {
              return length(x) > length(y);
            });
  for (size_t i = 0; i < lines.size(); ++i)
    for (size_t j = i + 1; j < lines.size(); ++j) {
      const glm::dvec2 a0 = lines[i]->points.front(),
                       a1 = lines[i]->points.back();
      const glm::dvec2 b0 = lines[j]->points.front(),
                       b1 = lines[j]->points.back();
      const glm::dvec2 u = glm::normalize(a1 - a0);
      if (std::abs(glm::dot(u, glm::normalize(b1 - b0))) < 0.9999)
        continue;
      const glm::dvec2 n(-u.y, u.x);
      if (std::abs(glm::dot(b0 - a0, n)) < 1e-6)
        continue; // collinear: two pieces of one wall
      return std::array<glm::dvec2, 4>{a0, a1, b0, b1};
    }
  return std::nullopt;
}

} // namespace

bool RefPointOf(const SheetView& view, const ProjRef& ref, glm::dvec2& out) {
  return RefPoint(view, ref, out);
}

namespace {
// An aligned value's text stands this far above its line (centre to line):
// its knockout's lower edge then clears the line by half a millimetre.
constexpr double kAlignedBelowMM = 1.7;  // centre to the knockout's bottom
constexpr double kAlignedAboveMM = 1.85; // centre to the knockout's top
constexpr double kAlignedLiftMM = kAlignedBelowMM + 0.5;
constexpr double kKnockoutPadMM = 0.6;
} // namespace

void AlignedLabel(const glm::dvec2& a, const glm::dvec2& b, glm::dvec2& centre,
                  double& angle) {
  glm::dvec2 u = b - a;
  const double L = glm::length(u);
  u = L > 1e-12 ? u / L : glm::dvec2(1.0, 0.0);
  // Readable from the bottom or the right: the turn lies in (-90, 90].
  double t = std::atan2(u.y, u.x);
  constexpr double kHalfPi = 1.5707963267948966, kPi = 3.141592653589793;
  if (t > kHalfPi + 1e-9)
    t -= kPi;
  else if (t <= -kHalfPi + 1e-9)
    t += kPi;
  angle = t;
  // "Above" the text is its own up: the line's normal turned with it.
  const glm::dvec2 up(-std::sin(t), std::cos(t));
  centre = 0.5 * (a + b) + up * kAlignedLiftMM;
}

std::array<glm::dvec2, 4> DimTextQuad(const ResolvedDim& d) {
  if (!d.aligned) {
    glm::dvec2 mn, mx;
    TextKnockout(d.labelPos, d.text, mn, mx);
    return {glm::dvec2(mn.x, mn.y), glm::dvec2(mx.x, mn.y),
            glm::dvec2(mx.x, mx.y), glm::dvec2(mn.x, mx.y)};
  }
  const double half = 0.5 * TextWidthMM(d.text) + kKnockoutPadMM;
  const glm::dvec2 along(std::cos(d.textAngle), std::sin(d.textAngle));
  const glm::dvec2 up(-along.y, along.x);
  const glm::dvec2 c = d.labelPos;
  return {c - along * half - up * kAlignedBelowMM,
          c + along * half - up * kAlignedBelowMM,
          c + along * half + up * kAlignedAboveMM,
          c - along * half + up * kAlignedAboveMM};
}

void DimLabelBox(const ResolvedDim& d, glm::dvec2& mn, glm::dvec2& mx) {
  if (!d.aligned) {
    LabelBox(d.labelPos, d.text, mn, mx);
    return;
  }
  mn = glm::dvec2(1e300);
  mx = glm::dvec2(-1e300);
  for (const glm::dvec2& p : DimTextQuad(d)) {
    mn = glm::min(mn, p);
    mx = glm::max(mx, p);
  }
}

ResolvedDim ResolveDim(const DrawingDim& dim, const SheetView& view,
                       const DimContext& ctx) {
  ResolvedDim rd;
  rd.id = dim.id;
  rd.kind = dim.kind;
  const glm::dvec2 off = dim.labelOffset;
  const double vs = ctx.viewScale > 0.0 ? ctx.viewScale : 1.0;

  switch (dim.kind) {
  case DrawingDimKind::Distance:
  case DrawingDimKind::DistanceX:
  case DrawingDimKind::DistanceY: {
    if (dim.refs.size() < 2)
      return rd;
    glm::dvec2 a, b;
    if (!RefPoint(view, dim.refs[0], a) || !RefPoint(view, dim.refs[1], b))
      return rd;
    // Plain `distance` with a straight edge on either side measures
    // PERPENDICULAR to that edge — an edge names a direction, not a point,
    // and its midpoint is an accident of its length. Point↔point (two hole
    // centres: a bolt circle) stays centre-to-centre. The axis kinds below
    // impose their own direction and need none of this.
    if (dim.kind == DrawingDimKind::Distance) {
      glm::dvec2 lp, ld, lp2, ld2;
      const bool aLine = RefStraightLine(view, dim.refs[0], lp, ld);
      const bool bLine = RefStraightLine(view, dim.refs[1], lp2, ld2);
      if (aLine && bLine && std::abs(glm::dot(ld, ld2)) > 0.999) {
        // `a` (edge A's midpoint) lies on the edge; measure straight across.
        const glm::dvec2 n(-ld.y, ld.x);
        b = a + n * glm::dot(lp2 - a, n);
      } else if (aLine != bLine) {
        const glm::dvec2& fp = aLine ? lp : lp2;
        const glm::dvec2 fd = aLine ? ld : ld2;
        const glm::dvec2 n(-fd.y, fd.x);
        const glm::dvec2 pt = aLine ? b : a;
        a = pt - n * glm::dot(pt - fp, n); // the foot on the edge
        b = pt;
      }
    }
    rd.ext1 = a;
    rd.ext2 = b;
    // Geometry stays in sheet millimetres (that is where the dimension is
    // drawn); the VALUE is a measurement of the part, so it divides by the
    // view scale — a 2 mm box drawn at 20:1 is 2 mm, not the 40 mm of paper
    // it covers. (HoleCallout already did this; distances and radii did not,
    // which stayed invisible while every view was 1:1.)
    if (dim.kind == DrawingDimKind::DistanceX) {
      rd.value = std::abs(b.x - a.x) / vs;
      const double dy = std::min(a.y, b.y) + off.y;
      rd.dim1 = {a.x, dy};
      rd.dim2 = {b.x, dy};
    } else if (dim.kind == DrawingDimKind::DistanceY) {
      rd.value = std::abs(b.y - a.y) / vs;
      const double dx = std::min(a.x, b.x) + off.x;
      rd.dim1 = {dx, a.y};
      rd.dim2 = {dx, b.y};
    } else {
      rd.value = glm::length(b - a) / vs;
      rd.dim1 = a + off;
      rd.dim2 = b + off;
    }
    rd.text = core::FormatLength(rd.value, ctx.units);
    rd.aligned = true;
    AlignedLabel(rd.dim1, rd.dim2, rd.labelPos, rd.textAngle);
    rd.resolved = true;
    break;
  }
  case DrawingDimKind::Radius:
  case DrawingDimKind::Diameter: {
    if (dim.refs.empty())
      return rd;
    // The name a tolerance is looked up by: the edge's, or for a wall seen
    // from the side (a silhouette, a bore in section) the face's.
    const std::string& refName = dim.refs[0].edge.IsSet()
                                     ? dim.refs[0].edge.name
                                     : dim.refs[0].silFace.name;
    // A diameter seen from the side -- a shaft's two silhouettes, a bore's
    // two walls in section -- is a LINEAR dimension across them with a Ø:
    // the drafting convention, and the only one a section can carry. A
    // radial leader from a midline that is not drawn anywhere read as a
    // radius of nothing.
    if (dim.kind == DrawingDimKind::Diameter) {
      if (const auto pair = RefLinePair(view, dim.refs[0])) {
        const auto& [a0, a1, b0, b1] = *pair;
        const glm::dvec2 u = glm::normalize(a1 - a0);
        const glm::dvec2 n(-u.y, u.x);
        const double width = std::abs(glm::dot(b0 - a0, n));
        // The dimension line crosses the walls at the offset's position
        // along them, measured from the walls' common midpoint; the
        // extension lines start at the wall ends on that side.
        const glm::dvec2 mid = 0.25 * (a0 + a1 + b0 + b1);
        const double level = glm::dot(mid, u) + glm::dot(off, u);
        const auto endToward = [&](glm::dvec2 p, glm::dvec2 q) {
          return std::abs(glm::dot(p, u) - level) <
                         std::abs(glm::dot(q, u) - level)
                     ? p
                     : q;
        };
        rd.ext1 = endToward(a0, a1);
        rd.ext2 = endToward(b0, b1);
        rd.dim1 = rd.ext1 + u * (level - glm::dot(rd.ext1, u));
        rd.dim2 = rd.ext2 + u * (level - glm::dot(rd.ext2, u));
        rd.value = width / vs;
        rd.text = "\xC3\x98" + core::FormatLength(rd.value, ctx.units);
        if (ctx.sourceDoc)
          if (const Tolerance* tol = FindToleranceForRef(
                  *ctx.sourceDoc, refName, 0.5 * width / vs))
            rd.text += FormatToleranceSuffix(*tol, ctx.units, rd.value);
        // Written along the line, slid along it by the offset's component
        // across the walls.
        rd.aligned = true;
        AlignedLabel(rd.dim1, rd.dim2, rd.labelPos, rd.textAngle);
        rd.labelPos += n * glm::dot(off, n);
        rd.resolved = true;
        break;
      }
    }
    glm::dvec2 c;
    double rad = 0.0;
    if (!RefCircle(view, dim.refs[0], c, rad))
      return rd;
    rd.radial = true;
    rd.center = c;
    rd.radius = rad;
    rd.value = (dim.kind == DrawingDimKind::Diameter ? 2.0 * rad : rad) / vs;
    rd.text = (dim.kind == DrawingDimKind::Diameter ? "\xC3\x98" : "R") +
              core::FormatLength(rd.value, ctx.units);
    // A toleranced bore says so on the sheet. Only a DIAMETER: an ISO class
    // states how a diameter may vary, so "R10 H7" would be a band twice the
    // size of the one written — the sort of plausible-looking wrong number
    // this project spends its refusals on.
    if (dim.kind == DrawingDimKind::Diameter && ctx.sourceDoc) {
      if (const Tolerance* tol =
              FindToleranceForRef(*ctx.sourceDoc, refName, rad / vs)) {
        rd.text += FormatToleranceSuffix(*tol, ctx.units, rd.value);
      }
    }
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
    rd.text = core::FormatAngle(rd.value, ctx.units);
    rd.ext1 = pa;
    rd.ext2 = pb;
    rd.labelPos = 0.5 * (pa + pb) + off;
    rd.resolved = true;
    break;
  }
  case DrawingDimKind::HoleCallout: {
    // One circle ref, resolved back to the HoleFeature that drilled it via
    // the feature ids in its MappedName + a radius cross-check. No source
    // document (assembly view) or no provenance match -> dangling, never a
    // guessed label.
    if (dim.refs.empty() || !ctx.sourceDoc)
      return rd;
    glm::dvec2 c;
    double rad = 0.0;
    if (!RefCircle(view, dim.refs[0], c, rad))
      return rd;
    const std::string& name = dim.refs[0].edge.name;
    const HoleFeature* hole = FindHoleForRef(*ctx.sourceDoc, name, rad / vs);
    if (!hole)
      return rd;
    rd.leader = true;
    rd.center = c;
    rd.radius = rad;
    rd.value = hole->diameter; // model mm
    rd.text = FormatHoleCallout(*hole, ctx.units,
                                FindThreadForHole(*ctx.sourceDoc, *hole));
    const glm::dvec2 dir = glm::length(off) > 1e-9 ? glm::normalize(off)
                                                   : glm::dvec2(0.7071, 0.7071);
    rd.rim = c + dir * rad;
    rd.labelPos = rd.rim + off;
    rd.resolved = true;
    break;
  }
  case DrawingDimKind::FastenerCallout: {
    // The one ref names an occurrence, and the occurrence's own document says
    // what it is: a part whose whole feature list is one Fastener carries its
    // designation ("ISO 4762 M6x50"), which is exactly what belongs on the
    // leader. No occurrence (a PART view), no resolver, or a part that is not
    // a bought fastener -> dangling, never a guessed label. That is the same
    // rule the hole callout follows: a drawing that quietly labels the wrong
    // screw is worse than one with an obvious gap in it.
    if (dim.refs.empty() || !ctx.occurrenceDoc)
      return rd;
    if (dim.refs[0].occurrenceId == 0)
      return rd;
    const FastenerFeature* screw =
        SoleFastenerOf(ctx.occurrenceDoc(dim.refs[0].occurrenceId));
    if (screw == nullptr)
      return rd;
    glm::dvec2 anchor;
    if (!RefPoint(view, dim.refs[0], anchor))
      return rd;
    // Anchored on whatever edge was picked rather than on a circle: a screw
    // is a circle from above and two lines from the side, and a callout that
    // only worked in a top view would be a callout nobody could place on the
    // section they actually drew.
    rd.leader = true;
    rd.center = anchor;
    rd.rim = anchor;
    rd.labelPos = anchor + off;
    rd.text = screw->Designation();
    rd.resolved = true;
    break;
  }
  case DrawingDimKind::PatternCallout: {
    // 2+ circle refs of equal radius: "N× <base>", plus " PITCH p" when the
    // centres form a uniform collinear row. Base text upgrades to the full
    // hole callout when the first circle's provenance resolves.
    if (dim.refs.size() < 2)
      return rd;
    std::vector<glm::dvec2> centers;
    double rad = -1.0;
    for (const ProjRef& ref : dim.refs) {
      glm::dvec2 c;
      double r = 0.0;
      if (!RefCircle(view, ref, c, r))
        return rd;
      if (rad < 0.0)
        rad = r;
      else if (std::abs(r - rad) > 1e-3 * std::max(rad, 1.0))
        return rd; // mixed radii: not one pattern
      centers.push_back(c);
    }
    std::string base =
        "\xC3\x98" + core::FormatLength(2.0 * rad / vs, ctx.units);
    if (ctx.sourceDoc)
      if (const HoleFeature* hole =
              FindHoleForRef(*ctx.sourceDoc, dim.refs[0].edge.name, rad / vs))
        base = FormatHoleCallout(*hole, ctx.units,
                                 FindThreadForHole(*ctx.sourceDoc, *hole));
    rd.text = std::to_string(dim.refs.size()) + "\xC3\x97 " + base; // "N× "
    const auto pitch = UniformCollinearPitch(centers);
    if (pitch) {
      rd.value = *pitch / vs;
      rd.text += " PITCH " + core::FormatLength(rd.value, ctx.units);
    } else {
      rd.value = 2.0 * rad / vs;
    }
    rd.leader = true;
    rd.center = centers.front();
    rd.radius = rad;
    const glm::dvec2 dir = glm::length(off) > 1e-9 ? glm::normalize(off)
                                                   : glm::dvec2(0.7071, 0.7071);
    rd.rim = centers.front() + dir * rad;
    rd.labelPos = rd.rim + off;
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
    // A vertical measure belongs LEFT of the view: the below-the-view base
    // shifts BOTH anchors down by the offset, which for a tall span pushed
    // the label half a view-height past the border and onto the title block
    // (caught by the R12 overlap gate on its first run).
    if (std::abs(rd.ext1.y - rd.ext2.y) > std::abs(rd.ext1.x - rd.ext2.x))
      return {left - std::max(rd.ext1.x, rd.ext2.x), 0.0};
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

std::string BendTableText(const Document& doc, const std::string& caption,
                          const glm::dvec3& viewDir) {
  // A drawing is written in its SOURCE document's units, like every other
  // number on the sheet — so the document answers that, not the caller.
  const units::System units = doc.Units();
  const glm::dvec3 reader = -glm::normalize(viewDir);
  std::string rows;
  int n = 0;
  for (const Body& body : doc.GetBodies()) {
    if (body.reference)
      continue;
    for (const BendRegion& bend : body.bends) {
      std::string row = "B" + std::to_string(++n) + "  ";
      if (bend.flat && sheet::SquareTo(bend, reader))
        row += sheet::FoldsToward(bend, reader) ? "UP " : "DOWN ";
      row += units::FormatAngle(angles::ToDegrees(bend.angle), units) + "  R " +
             units::FormatLength(bend.radius, units);
      char k[32];
      std::snprintf(k, sizeof k, "  K %.3f", bend.kFactor);
      row += k;
      if (bend.allowance > 0.0)
        row += "  BA " + units::FormatLength(bend.allowance, units);
      rows += "\n" + row;
    }
  }
  if (n == 0)
    return {};
  return (caption.empty() ? std::string("BEND TABLE") : caption) + rows;
}

ResolvedNote ResolveNote(const DrawingNote& note, const SheetView* view,
                         const SymbolTable& symbols,
                         const units::System& units) {
  ResolvedNote rn;
  rn.id = note.id;
  if (note.style == NoteStyle::Fcf) {
    // The frame says what it is; a list reads it back as a line.
    rn.fcf = note.fcf;
    rn.fcfTolerance = core::FormatDeviation(note.fcf.tolerance, units);
    std::string line = GeoSymbolToString(note.fcf.symbol);
    line += std::string(" ") + (note.fcf.diameterZone ? "\xC3\x98" : "") +
            rn.fcfTolerance;
    if (note.fcf.modifier != MaterialModifier::None)
      line += note.fcf.modifier == MaterialModifier::Mmc ? " (M)" : " (L)";
    for (const std::string& d : note.fcf.datums)
      line += " | " + d;
    DrawingNote frame = note;
    frame.text = line;
    frame.style = NoteStyle::Text; // resolved below as plain words...
    ResolvedNote out = ResolveNote(frame, view, symbols, units);
    out.style = NoteStyle::Fcf; // ...and drawn as the frame
    out.fcf = rn.fcf;
    out.fcfTolerance = rn.fcfTolerance;
    return out;
  }
  size_t cursor = 0;
  while (cursor < note.text.size()) {
    const auto begin = note.text.find("{{", cursor);
    if (begin == std::string::npos) {
      rn.text += note.text.substr(cursor);
      break;
    }
    rn.text += note.text.substr(cursor, begin - cursor);
    const auto end = note.text.find("}}", begin + 2);
    if (end == std::string::npos) {
      rn.resolved = false;
      rn.error = "unterminated note expression";
      break;
    }
    const auto value = ExpressionEngine::Default().EvaluateText(
        note.text.substr(begin + 2, end - begin - 2), symbols);
    if (!value) {
      rn.resolved = false;
      rn.error = value.error().message;
      break;
    }
    char number[64];
    std::snprintf(number, sizeof number, "%.10g", *value);
    rn.text += number;
    cursor = end + 2;
  }
  if (!rn.resolved)
    rn.text = "[UNRESOLVED: " + rn.error + "]";
  rn.pos = note.pos;
  rn.style = note.style;
  if (view != nullptr &&
      (note.leader.edge.IsSet() || note.leader.silFace.IsSet())) {
    glm::dvec2 a(0.0);
    if (RefPoint(*view, note.leader, a)) {
      rn.hasLeader = true;
      rn.anchor = a;
    }
  }
  if ((note.leader.edge.IsSet() || note.leader.silFace.IsSet()) &&
      !rn.hasLeader) {
    rn.resolved = false;
    rn.error = "note leader no longer resolves";
  }
  return rn;
}

void LabelBox(const glm::dvec2& labelPos, const std::string& text,
              glm::dvec2& mn, glm::dvec2& mx) {
  // Per LINE, and as many lines as there are: a note is one string with
  // newlines in it, and boxing the whole thing as a single line made every
  // multi-line note read as running off the sheet (a 200-character hole
  // table came out 440 mm wide). Lines grow DOWNWARD from `labelPos`, which
  // is where the exporter draws them.
  //
  // Count glyphs, not bytes: Ø is two bytes of UTF-8 and one glyph wide.
  size_t widest = 0, glyphs = 0, lines = 1;
  for (const unsigned char c : text) {
    if (c == '\n') {
      widest = std::max(widest, glyphs);
      glyphs = 0;
      lines++;
      continue;
    }
    glyphs += (c & 0xC0) != 0x80;
  }
  widest = std::max(widest, glyphs);
  const double w = std::max<size_t>(widest, 1) * 2.2;
  const double h = kLabelLineMM;
  mn = {labelPos.x - w * 0.5, labelPos.y - h * (lines - 0.5)};
  mx = {labelPos.x + w * 0.5, labelPos.y + h * 0.5};
}

void TextKnockout(const glm::dvec2& labelPos, const std::string& text,
                  glm::dvec2& mn, glm::dvec2& mx) {
  double w = 0.0;
  size_t lines = 1;
  size_t start = 0;
  for (;;) {
    const size_t nl = text.find('\n', start);
    w = std::max(w, TextWidthMM(text.substr(
                        start, nl == std::string::npos ? nl : nl - start)));
    if (nl == std::string::npos)
      break;
    start = nl + 1;
    ++lines;
  }
  constexpr double pad = 0.6, half = 0.5 * kLabelLineMM;
  mn = {labelPos.x - 0.5 * w - pad,
        labelPos.y - half - kLabelLineMM * static_cast<double>(lines - 1)};
  mx = {labelPos.x + 0.5 * w + pad, labelPos.y + half};
}

glm::dvec2 LeaderEnd(const glm::dvec2& from, const glm::dvec2& labelPos,
                     const std::string& text) {
  glm::dvec2 mn, mx;
  TextKnockout(labelPos, text, mn, mx);
  if (from.x < mn.x)
    return {mn.x, labelPos.y};
  if (from.x > mx.x)
    return {mx.x, labelPos.y};
  return {labelPos.x, from.y < mn.y ? mn.y : mx.y};
}

namespace {

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t at = s.find(sep, start);
    out.push_back(s.substr(start, at == std::string::npos ? at : at - start));
    if (at == std::string::npos)
      return out;
    start = at + 1;
  }
}

// A ruled table's layout, in sheet mm from its bottom-right corner.
constexpr double kTableRowMM = 6.0;
constexpr double kTablePadMM = 2.0;

struct TableLayout {
  std::vector<std::vector<std::string>> rows;
  std::vector<double> colW;
  double width = 0.0, height = 0.0;
};

TableLayout LayoutTable(const std::string& text) {
  TableLayout t;
  for (const std::string& line : Split(text, '\n'))
    t.rows.push_back(Split(line, '\t'));
  size_t cols = 1;
  for (const auto& r : t.rows)
    cols = std::max(cols, r.size());
  t.colW.assign(cols, 2.0 * kTablePadMM);
  for (const auto& r : t.rows)
    if (r.size() > 1)
      for (size_t c = 0; c < r.size(); ++c)
        t.colW[c] = std::max(t.colW[c], TextWidthMM(r[c]) + 2.0 * kTablePadMM);
  for (double w : t.colW)
    t.width += w;
  // A spanning row (a caption) wider than the columns widens the last one.
  for (const auto& r : t.rows)
    if (r.size() == 1) {
      const double need = TextWidthMM(r[0]) + 2.0 * kTablePadMM;
      if (need > t.width) {
        t.colW.back() += need - t.width;
        t.width = need;
      }
    }
  t.height = kTableRowMM * static_cast<double>(t.rows.size());
  return t;
}

// A closed polygon approximating a circle.
std::vector<glm::dvec2> Circle(glm::dvec2 c, double r, int n) {
  std::vector<glm::dvec2> out;
  for (int i = 0; i < n; ++i) {
    const double a = 2.0 * 3.14159265358979323846 * i / n;
    out.push_back(c + r * glm::dvec2(std::cos(a), std::sin(a)));
  }
  return out;
}

constexpr double kDatumBoxHalfMM = 3.25;

double BalloonRadius(const std::string& text) {
  return std::max(4.0, 0.5 * TextWidthMM(text) + 1.8);
}

} // namespace

namespace {

// A feature control frame's compartments, sheet mm (ISO 1101 / ISO 7083:
// frame height twice the 3.5 mm lettering).
constexpr double kFcfHeightMM = 7.0;
constexpr double kFcfPadMM = 1.2;
constexpr double kFcfModifierR = 1.6;

struct FcfLayout {
  double symbolW = kFcfHeightMM;
  double toleranceW = 0.0;
  std::vector<double> datumW;
  double width = 0.0;
};

FcfLayout LayoutFcf(const FeatureControlFrame& f, const std::string& tol) {
  FcfLayout l;
  std::string value = (f.diameterZone ? "\xC3\x98" : "") + tol;
  l.toleranceW = TextWidthMM(value) + 2.0 * kFcfPadMM;
  if (f.modifier != MaterialModifier::None)
    l.toleranceW += 2.0 * kFcfModifierR + 0.6;
  for (const std::string& d : f.datums)
    l.datumW.push_back(
        std::max(kFcfHeightMM, TextWidthMM(d) + 2.0 * kFcfPadMM));
  l.width = l.symbolW + l.toleranceW;
  for (double w : l.datumW)
    l.width += w;
  return l;
}

void Polyline(SheetGraphics& g, std::initializer_list<glm::dvec2> pts, double w,
              bool closed = false) {
  const std::vector<glm::dvec2> v(pts);
  for (size_t i = 1; i < v.size(); ++i)
    g.lines.push_back({v[i - 1], v[i], w});
  if (closed && v.size() > 2)
    g.lines.push_back({v.back(), v.front(), w});
}

void Arc(SheetGraphics& g, glm::dvec2 c, double r, double a0, double a1,
         double w) {
  constexpr int kSteps = 32;
  glm::dvec2 prev = c + r * glm::dvec2(std::cos(a0), std::sin(a0));
  for (int i = 1; i <= kSteps; ++i) {
    const double t = a0 + (a1 - a0) * i / kSteps;
    const glm::dvec2 q = c + r * glm::dvec2(std::cos(t), std::sin(t));
    g.lines.push_back({prev, q, w});
    prev = q;
  }
}

// A slanted arrow for the runout symbols: shaft from `tail` to `tip`, a
// filled head at the tip.
void SymbolArrow(SheetGraphics& g, glm::dvec2 tail, glm::dvec2 tip, double w) {
  const glm::dvec2 d = glm::normalize(tip - tail);
  const glm::dvec2 n(-d.y, d.x);
  const glm::dvec2 base = tip - d * 1.4;
  g.lines.push_back({tail, base, w});
  g.fills.push_back({tip, base + n * 0.55, base - n * 0.55});
}

// The ISO 1101 symbol as line work, centred on `c`, about 4 mm across: drawn
// rather than set in a font, because the PDF's Helvetica has none of them.
void DrawGeoSymbol(SheetGraphics& g, GeoSymbol s, glm::dvec2 c) {
  constexpr double a = 2.0, w = 0.3, kPi = 3.141592653589793;
  const auto P = [&](double x, double y) { return c + glm::dvec2(x, y); };
  switch (s) {
  case GeoSymbol::Straightness:
    Polyline(g, {P(-a, 0), P(a, 0)}, w);
    break;
  case GeoSymbol::Flatness:
    Polyline(g,
             {P(-a, -0.6 * a), P(0.5 * a, -0.6 * a), P(a, 0.6 * a),
              P(-0.5 * a, 0.6 * a)},
             w, true);
    break;
  case GeoSymbol::Circularity:
    Arc(g, c, 0.9 * a, 0.0, 2.0 * kPi, w);
    break;
  case GeoSymbol::Cylindricity:
    Arc(g, c, 0.7 * a, 0.0, 2.0 * kPi, w);
    Polyline(g, {P(-1.15 * a, -a), P(-0.25 * a, a)}, w);
    Polyline(g, {P(0.25 * a, -a), P(1.15 * a, a)}, w);
    break;
  case GeoSymbol::ProfileLine:
    Arc(g, P(0, -0.5 * a), a, 0.0, kPi, w);
    break;
  case GeoSymbol::ProfileSurface:
    Arc(g, P(0, -0.5 * a), a, 0.0, kPi, w);
    Polyline(g, {P(-a, -0.5 * a), P(a, -0.5 * a)}, w);
    break;
  case GeoSymbol::Parallelism:
    Polyline(g, {P(-0.9 * a, -a), P(-0.1 * a, a)}, w);
    Polyline(g, {P(0.1 * a, -a), P(0.9 * a, a)}, w);
    break;
  case GeoSymbol::Perpendicularity:
    Polyline(g, {P(-a, -a), P(a, -a)}, w);
    Polyline(g, {P(0, -a), P(0, a)}, w);
    break;
  case GeoSymbol::Angularity:
    Polyline(g, {P(a, -a), P(-a, -a), P(a, -a + 2.0 * a * 0.577)}, w);
    break;
  case GeoSymbol::Position:
    Arc(g, c, 0.6 * a, 0.0, 2.0 * kPi, w);
    Polyline(g, {P(-a, 0), P(a, 0)}, w);
    Polyline(g, {P(0, -a), P(0, a)}, w);
    break;
  case GeoSymbol::Coaxiality:
    Arc(g, c, 0.45 * a, 0.0, 2.0 * kPi, w);
    Arc(g, c, 0.95 * a, 0.0, 2.0 * kPi, w);
    break;
  case GeoSymbol::Symmetry:
    Polyline(g, {P(-a, 0), P(a, 0)}, w);
    Polyline(g, {P(-0.6 * a, 0.55 * a), P(0.6 * a, 0.55 * a)}, w);
    Polyline(g, {P(-0.6 * a, -0.55 * a), P(0.6 * a, -0.55 * a)}, w);
    break;
  case GeoSymbol::CircularRunout:
    SymbolArrow(g, P(-0.6 * a, -a), P(0.6 * a, a), w);
    break;
  case GeoSymbol::TotalRunout:
    SymbolArrow(g, P(-a, -0.8 * a), P(-0.1 * a, a), w);
    SymbolArrow(g, P(0.1 * a, -0.8 * a), P(a, a), w);
    Polyline(g, {P(-a, -0.8 * a), P(0.1 * a, -0.8 * a)}, w);
    break;
  }
}

} // namespace

glm::dvec2 FcfSize(const FeatureControlFrame& fcf, const std::string& tol) {
  return {LayoutFcf(fcf, tol).width, kFcfHeightMM};
}

bool IsTableText(const std::string& text) {
  return text.find('\t') != std::string::npos;
}

SheetGraphics NoteGraphics(const ResolvedNote& n) {
  SheetGraphics g;
  // Text on the line: a 3.5 mm font's cap height is about 2.5 mm, so a
  // baseline 1.25 below the centre centres it.
  constexpr double kCentreToBaseline = 1.25;
  if (IsTableText(n.text)) {
    const TableLayout t = LayoutTable(n.text);
    const double x0 = n.pos.x - t.width, y0 = n.pos.y;
    const double x1 = n.pos.x, y1 = n.pos.y + t.height;
    g.lines.push_back({{x0, y0}, {x1, y0}, 0.35});
    g.lines.push_back({{x1, y0}, {x1, y1}, 0.35});
    g.lines.push_back({{x1, y1}, {x0, y1}, 0.35});
    g.lines.push_back({{x0, y1}, {x0, y0}, 0.35});
    for (size_t r = 0; r < t.rows.size(); ++r) {
      const double top = y1 - kTableRowMM * static_cast<double>(r);
      const double bottom = top - kTableRowMM;
      if (r > 0)
        g.lines.push_back({{x0, top}, {x1, top}, r == 1 ? 0.35 : 0.18});
      const auto& cells = t.rows[r];
      if (cells.size() == 1) {
        g.texts.push_back(
            {{0.5 * (x0 + x1), bottom + 1.8}, cells[0], true, 3.5});
        continue;
      }
      double x = x0;
      for (size_t c = 0; c < t.colW.size(); ++c) {
        if (c > 0)
          g.lines.push_back({{x, top}, {x, bottom}, 0.18});
        if (c < cells.size() && !cells[c].empty())
          g.texts.push_back(
              {{x + kTablePadMM, bottom + 1.8}, cells[c], false, 3.5});
        x += t.colW[c];
      }
    }
    return g;
  }
  if (n.style == NoteStyle::Balloon) {
    const double r = BalloonRadius(n.text);
    const auto ring = Circle(n.pos, r, 48);
    for (size_t i = 0; i < ring.size(); ++i)
      g.lines.push_back({ring[i], ring[(i + 1) % ring.size()], 0.35});
    g.texts.push_back(
        {{n.pos.x, n.pos.y - kCentreToBaseline}, n.text, true, 3.5});
    if (n.hasLeader && glm::length(n.anchor - n.pos) > r) {
      const glm::dvec2 d = glm::normalize(n.anchor - n.pos);
      g.lines.push_back({n.pos + d * r, n.anchor, 0.25});
      g.fills.push_back(Circle(n.anchor, 0.7, 16)); // a dot on the part
    }
    return g;
  }
  if (n.style == NoteStyle::Fcf) {
    const FcfLayout l = LayoutFcf(n.fcf, n.fcfTolerance);
    const double h = kFcfHeightMM;
    const double x0 = n.pos.x - 0.5 * l.width, x1 = n.pos.x + 0.5 * l.width;
    const double y0 = n.pos.y - 0.5 * h, y1 = n.pos.y + 0.5 * h;
    g.knockouts.push_back({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
    Polyline(g, {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, 0.35, true);
    double x = x0;
    DrawGeoSymbol(g, n.fcf.symbol, {x + 0.5 * l.symbolW, n.pos.y});
    x += l.symbolW;
    g.lines.push_back({{x, y0}, {x, y1}, 0.25});
    const std::string value =
        (n.fcf.diameterZone ? "\xC3\x98" : "") + n.fcfTolerance;
    g.texts.push_back(
        {{x + kFcfPadMM, n.pos.y - kCentreToBaseline}, value, false, 3.5});
    if (n.fcf.modifier != MaterialModifier::None) {
      const glm::dvec2 mc(
          x + kFcfPadMM + TextWidthMM(value) + 0.6 + kFcfModifierR, n.pos.y);
      const auto ring = Circle(mc, kFcfModifierR, 32);
      for (size_t i = 0; i < ring.size(); ++i)
        g.lines.push_back({ring[i], ring[(i + 1) % ring.size()], 0.25});
      g.texts.push_back({{mc.x, mc.y - 0.85},
                         n.fcf.modifier == MaterialModifier::Mmc ? "M" : "L",
                         true,
                         2.4});
    }
    x += l.toleranceW;
    for (size_t i = 0; i < n.fcf.datums.size(); ++i) {
      g.lines.push_back({{x, y0}, {x, y1}, 0.25});
      g.texts.push_back({{x + 0.5 * l.datumW[i], n.pos.y - kCentreToBaseline},
                         n.fcf.datums[i],
                         true,
                         3.5});
      x += l.datumW[i];
    }
    if (n.hasLeader) {
      // From the end of the frame nearer the feature, square off it, then
      // to the feature with an arrow on it (ISO 1101 8.1).
      const bool right = n.anchor.x > n.pos.x;
      const glm::dvec2 start(right ? x1 : x0, n.pos.y);
      const glm::dvec2 elbow = start + glm::dvec2(right ? 3.0 : -3.0, 0.0);
      const bool straight =
          right ? n.anchor.x <= elbow.x : n.anchor.x >= elbow.x;
      const glm::dvec2 from = straight ? start : elbow;
      if (!straight)
        g.lines.push_back({start, elbow, 0.25});
      if (glm::length(n.anchor - from) > 1e-6) {
        const glm::dvec2 d = glm::normalize(n.anchor - from);
        const glm::dvec2 across(-d.y, d.x);
        const glm::dvec2 base = n.anchor - d * 2.2;
        g.lines.push_back({from, base, 0.25});
        g.fills.push_back({n.anchor, base + across * 0.8, base - across * 0.8});
      }
    }
    return g;
  }
  if (n.style == NoteStyle::Datum) {
    const double h = kDatumBoxHalfMM;
    const glm::dvec2 lo = n.pos - glm::dvec2(h), hi = n.pos + glm::dvec2(h);
    g.lines.push_back({{lo.x, lo.y}, {hi.x, lo.y}, 0.35});
    g.lines.push_back({{hi.x, lo.y}, {hi.x, hi.y}, 0.35});
    g.lines.push_back({{hi.x, hi.y}, {lo.x, hi.y}, 0.35});
    g.lines.push_back({{lo.x, hi.y}, {lo.x, lo.y}, 0.35});
    g.texts.push_back(
        {{n.pos.x, n.pos.y - kCentreToBaseline}, n.text, true, 3.5});
    if (n.hasLeader && glm::length(n.anchor - n.pos) > 2.0 * h) {
      // The triangle stands on the feature, its base square to the leader;
      // the leader runs from its apex to the box.
      const glm::dvec2 u = glm::normalize(n.pos - n.anchor);
      const glm::dvec2 across(-u.y, u.x);
      constexpr double side = 3.4;
      const glm::dvec2 apex = n.anchor + u * (side * 0.866);
      g.fills.push_back({n.anchor + across * (side * 0.5), apex,
                         n.anchor - across * (side * 0.5)});
      const double toEdge = h / std::max(std::abs(u.x), std::abs(u.y));
      g.lines.push_back({apex, n.pos - u * toEdge, 0.25});
    }
    return g;
  }
  return g;
}

void NoteBox(const ResolvedNote& n, glm::dvec2& mn, glm::dvec2& mx) {
  if (IsTableText(n.text)) {
    const TableLayout t = LayoutTable(n.text);
    mn = {n.pos.x - t.width, n.pos.y};
    mx = {n.pos.x, n.pos.y + t.height};
    return;
  }
  if (n.style == NoteStyle::Balloon) {
    const double r = BalloonRadius(n.text);
    mn = n.pos - glm::dvec2(r);
    mx = n.pos + glm::dvec2(r);
    return;
  }
  if (n.style == NoteStyle::Datum) {
    mn = n.pos - glm::dvec2(kDatumBoxHalfMM);
    mx = n.pos + glm::dvec2(kDatumBoxHalfMM);
    return;
  }
  if (n.style == NoteStyle::Fcf) {
    const glm::dvec2 half = 0.5 * FcfSize(n.fcf, n.fcfTolerance);
    mn = n.pos - half;
    mx = n.pos + half;
    return;
  }
  LabelBox(n.pos, n.text, mn, mx);
}

namespace {

// Where ResolveDim would put the LABEL for this offset — the per-kind offset
// semantics of ResolveDim above, replayed without a full resolve per lane.
glm::dvec2 LabelPosFor(DrawingDimKind kind, const ResolvedDim& rd,
                       const glm::dvec2& off) {
  switch (kind) {
  case DrawingDimKind::DistanceX:
    return {0.5 * (rd.ext1.x + rd.ext2.x),
            std::min(rd.ext1.y, rd.ext2.y) + off.y};
  case DrawingDimKind::DistanceY:
    return {std::min(rd.ext1.x, rd.ext2.x) + off.x,
            0.5 * (rd.ext1.y + rd.ext2.y)};
  case DrawingDimKind::Radius:
  case DrawingDimKind::Diameter:
  // Callouts resolve their label the same way — rim + offset — and their
  // ext1/ext2 are never set, so the linear formula below placed the probe
  // box at the sheet origin and the search "found" a lane that was nowhere
  // near where the label would actually land.
  case DrawingDimKind::HoleCallout:
  case DrawingDimKind::PatternCallout:
  case DrawingDimKind::FastenerCallout: {
    const glm::dvec2 dir = glm::length(off) > 1e-9 ? glm::normalize(off)
                                                   : glm::dvec2(0.7071, 0.7071);
    return rd.center + dir * rd.radius + off;
  }
  default:
    return 0.5 * (rd.ext1 + rd.ext2) + off;
  }
}

bool BoxClear(const glm::dvec2& mn, const glm::dvec2& mx,
              const DimPlaceContext& ctx) {
  if (mn.x < ctx.borderMin.x || mn.y < ctx.borderMin.y ||
      mx.x > ctx.borderMax.x || mx.y > ctx.borderMax.y)
    return false;
  constexpr double pad = 1.0;
  for (const auto& [amn, amx] : ctx.avoid)
    if (mn.x < amx.x + pad && mx.x > amn.x - pad && mn.y < amx.y + pad &&
        mx.y > amn.y - pad)
      return false;
  return true;
}

} // namespace

glm::dvec2 PlaceDimOffset(const DrawingDim& dim, const SheetView& view,
                          const DimPlaceContext& ctx,
                          const ResolvedDim* resolved) {
  const glm::dvec2 base = DefaultDimOffset(dim, view);
  DrawingDim probe = dim;
  probe.labelOffset = {0.0, 0.0};
  const ResolvedDim rd = resolved ? *resolved : ResolveDim(probe, view);
  if (!rd.resolved)
    return base;
  const std::string text =
      rd.text.empty() ? std::string("00000") : rd.text; // width estimate
  const auto clearAt = [&](const glm::dvec2& off) {
    glm::dvec2 mn, mx;
    if (rd.aligned) {
      // An aligned value turns with its line, so its box is the turned
      // text's at that offset: resolved again rather than replayed. The
      // text is the caller's (a tolerance suffix widens it).
      DrawingDim at = dim;
      at.labelOffset = off;
      ResolvedDim moved = ResolveDim(at, view);
      if (!moved.resolved)
        return false;
      moved.text = text;
      DimLabelBox(moved, mn, mx);
    } else {
      LabelBox(LabelPosFor(dim.kind, rd, off), text, mn, mx);
    }
    return BoxClear(mn, mx, ctx);
  };
  constexpr double kLane = 7.0;
  if (dim.kind == DrawingDimKind::Diameter && !rd.radial) {
    // A Ø across two lines -- a shaft's sides, a bore's walls in section --
    // is a linear dimension whose line runs ACROSS the walls; it goes
    // beyond the view along them, on the side their ends are nearer, in
    // lanes outward. Its offset moves the line along the walls from their
    // common midpoint.
    const glm::dvec2 across = rd.dim2 - rd.dim1;
    if (glm::length(across) > 1e-9) {
      const glm::dvec2 n = glm::normalize(across);
      const glm::dvec2 u(-n.y, n.x); // along the walls
      const double level = glm::dot(0.5 * (rd.dim1 + rd.dim2), u);
      double lo = 1e300, hi = -1e300;
      for (const glm::dvec2& c : {view.bboxMin, view.bboxMax,
                                  glm::dvec2(view.bboxMin.x, view.bboxMax.y),
                                  glm::dvec2(view.bboxMax.x, view.bboxMin.y)}) {
        lo = std::min(lo, glm::dot(c, u));
        hi = std::max(hi, glm::dot(c, u));
      }
      // How far the walls themselves reach: both ends of both lines.
      double wallStart = glm::dot(rd.ext1, u), wallEnd = wallStart;
      if (const auto pair = RefLinePair(view, dim.refs[0]))
        for (const glm::dvec2& q : *pair) {
          wallStart = std::min(wallStart, glm::dot(q, u));
          wallEnd = std::max(wallEnd, glm::dot(q, u));
        }
      // The side the walls reach nearer to the view's edge goes first.
      const bool upFirst = hi - wallEnd <= wallStart - lo;
      for (int pass = 0; pass < 2; ++pass) {
        const bool up = (pass == 0) == upFirst;
        for (int lane = 0; lane < 8; ++lane) {
          const double target = up ? hi + kDimOffsetGapMM + kLane * lane
                                   : lo - kDimOffsetGapMM - kLane * lane;
          const glm::dvec2 off = u * (target - level);
          if (clearAt(off))
            return off;
        }
      }
      return u *
             ((upFirst ? hi + kDimOffsetGapMM : lo - kDimOffsetGapMM) - level);
    }
  }
  switch (dim.kind) {
  case DrawingDimKind::Radius:
  case DrawingDimKind::Diameter:
  // A callout is a leader too — its offset is a free vector from the thing
  // it points at, exactly like a radius'. Placing it as a LINEAR dimension
  // gave it two lanes to try, and on a crowded sheet both were taken: the
  // fallback then dropped the label 12 mm below its own hole, which is on
  // the part. Found on a quad's mid plate, where a hole table's notes had
  // taken the lanes.
  case DrawingDimKind::HoleCallout:
  case DrawingDimKind::PatternCallout:
  case DrawingDimKind::FastenerCallout: {
    // Try eight leader directions at growing lengths, preferring the base's
    // away-from-centre direction.
    const glm::dvec2 first =
        glm::length(base) > 1e-9 ? glm::normalize(base) : glm::dvec2(1.0, 0.0);
    // Far enough to leave the view. Four rings is 33 mm, which does not get
    // a leader off a 108 mm plate when the hole it points at is in the
    // middle of it — the search then fell back onto the part.
    const double reach =
        glm::length(view.bboxMax - view.bboxMin) + 3 * kDimOffsetGapMM;
    for (int ring = 0; kDimOffsetGapMM + ring * kLane <= reach; ring++) {
      const double len = kDimOffsetGapMM + ring * kLane;
      for (int k = 0; k < 8; k++) {
        const double a = k * 0.7853981633974483; // pi/4
        const double ca = std::cos(a), sa = std::sin(a);
        const glm::dvec2 dir{first.x * ca - first.y * sa,
                             first.x * sa + first.y * ca};
        const glm::dvec2 off = dir * len;
        if (clearAt(off))
          return off;
      }
    }
    return base;
  }
  default: {
    // Linear kinds: aim the LABEL at a lane line outside the view — the
    // preferred side first (below / left), then the opposite side. One side
    // alone is not enough: a label column standing over the title block
    // exhausts every downward lane and fell back onto a collision (caught
    // by the overlap gate on the W 170 S baffle's two stacked '144' dims).
    const bool vertical =
        dim.kind == DrawingDimKind::DistanceY ||
        (dim.kind == DrawingDimKind::Distance &&
         std::abs(rd.ext1.y - rd.ext2.y) > std::abs(rd.ext1.x - rd.ext2.x));
    const auto offFor = [&](double target) -> glm::dvec2 {
      switch (dim.kind) {
      case DrawingDimKind::DistanceX:
        return {0.0, target - std::min(rd.ext1.y, rd.ext2.y)};
      case DrawingDimKind::DistanceY:
        return {target - std::min(rd.ext1.x, rd.ext2.x), 0.0};
      default: // Distance / Angle: the whole dim line shifts by the offset
        return vertical
                   ? glm::dvec2(target - 0.5 * (rd.ext1.x + rd.ext2.x), 0.0)
                   : glm::dvec2(0.0, target - 0.5 * (rd.ext1.y + rd.ext2.y));
      }
    };
    const double lo = vertical ? view.bboxMin.x : view.bboxMin.y;
    const double hi = vertical ? view.bboxMax.x : view.bboxMax.y;
    for (int side = 0; side < 2; side++) {
      for (int lane = 0; lane < 8; lane++) {
        const double target = side == 0 ? lo - kDimOffsetGapMM - kLane * lane
                                        : hi + kDimOffsetGapMM + kLane * lane;
        const glm::dvec2 off = offFor(target);
        if (clearAt(off))
          return off;
      }
    }
    return base;
  }
  }
}

} // namespace drawing
