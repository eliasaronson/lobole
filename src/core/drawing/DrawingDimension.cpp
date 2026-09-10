#include "core/drawing/DrawingDimension.h"

#include "core/drawing/DrawingCallout.h"
#include "core/features/FastenerFeature.h"
#include "core/features/HoleFeature.h"
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

} // namespace

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
    rd.value = (dim.kind == DrawingDimKind::Diameter ? 2.0 * rad : rad) / vs;
    rd.text = (dim.kind == DrawingDimKind::Diameter ? "\xC3\x98" : "R") +
              core::FormatLength(rd.value);
    // A toleranced bore says so on the sheet. Only a DIAMETER: an ISO class
    // states how a diameter may vary, so "R10 H7" would be a band twice the
    // size of the one written — the sort of plausible-looking wrong number
    // this project spends its refusals on.
    if (dim.kind == DrawingDimKind::Diameter && ctx.sourceDoc) {
      if (const Tolerance* tol = FindToleranceForRef(
              *ctx.sourceDoc, dim.refs[0].edge.name, rad / vs)) {
        rd.text += FormatToleranceSuffix(*tol);
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
    rd.text = core::FormatAngle(rd.value);
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
    rd.text = FormatHoleCallout(*hole);
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
    std::string base = "\xC3\x98" + core::FormatLength(2.0 * rad / vs);
    if (ctx.sourceDoc)
      if (const HoleFeature* hole =
              FindHoleForRef(*ctx.sourceDoc, dim.refs[0].edge.name, rad / vs))
        base = FormatHoleCallout(*hole);
    rd.text = std::to_string(dim.refs.size()) + "\xC3\x97 " + base; // "N× "
    const auto pitch = UniformCollinearPitch(centers);
    if (pitch) {
      rd.value = *pitch / vs;
      rd.text += " PITCH " + core::FormatLength(rd.value);
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

ResolvedNote ResolveNote(const DrawingNote& note, const SheetView* view,
                         const SymbolTable& symbols) {
  ResolvedNote rn;
  rn.id = note.id;
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
    LabelBox(LabelPosFor(dim.kind, rd, off), text, mn, mx);
    return BoxClear(mn, mx, ctx);
  };
  constexpr double kLane = 7.0;
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
