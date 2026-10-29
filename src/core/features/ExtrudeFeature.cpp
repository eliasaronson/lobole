// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/ExtrudeFeature.h"
#include "core/ElementHistory.h"
#include "core/Measure.h" // InvalidReasons

#include "common/Logger.h"
#include "core/Angles.h"
#include "core/BodyMode.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"

#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRep_Builder.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>

ExtrudeFeature::ExtrudeFeature(const std::string& name, uint64_t sketchId,
                               double distance, ExtrudeMode mode, bool reversed)
    : sketchId(sketchId), distance(distance), reversed(reversed), mode(mode) {
  m_name = name;
}

namespace {

double SegmentDistance(const glm::dvec2& p, const glm::dvec2& a,
                       const glm::dvec2& b) {
  const glm::dvec2 ab = b - a;
  const double len2 = glm::dot(ab, ab);
  const double t =
      len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
  return glm::length(p - (a + t * ab));
}

// The radius of the largest circle inside the region `inside` says is
// material, bounded by `polys`: how far its walls can move in before it
// closes up. A grid finds the best start and a pattern search climbs to it.
template <typename Inside>
double Inradius(const std::vector<std::vector<glm::dvec2>>& polys,
                Inside&& inside) {
  glm::dvec2 lo(1e300), hi(-1e300);
  for (const auto& poly : polys)
    for (const glm::dvec2& p : poly) {
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
  if (!(hi.x > lo.x) || !(hi.y > lo.y))
    return 0.0;
  const auto clearance = [&](const glm::dvec2& p) {
    if (!inside(p))
      return -1.0;
    double d = 1e300;
    for (const auto& poly : polys)
      for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        d = std::min(d, SegmentDistance(p, poly[j], poly[i]));
    return d;
  };
  constexpr int kGrid = 64;
  const glm::dvec2 step = (hi - lo) / double(kGrid);
  glm::dvec2 best = lo;
  double bestD = -1.0;
  for (int i = 0; i <= kGrid; ++i)
    for (int k = 0; k <= kGrid; ++k) {
      const glm::dvec2 p = lo + glm::dvec2(i * step.x, k * step.y);
      const double d = clearance(p);
      if (d > bestD) {
        bestD = d;
        best = p;
      }
    }
  if (bestD <= 0.0)
    return 0.0;
  double h = std::max(step.x, step.y);
  const double floor = 1e-7 * std::max(hi.x - lo.x, hi.y - lo.y);
  while (h > floor) {
    bool moved = false;
    for (const glm::dvec2 dir :
         {glm::dvec2(1, 0), glm::dvec2(-1, 0), glm::dvec2(0, 1),
          glm::dvec2(0, -1), glm::dvec2(0.7071, 0.7071),
          glm::dvec2(-0.7071, 0.7071), glm::dvec2(0.7071, -0.7071),
          glm::dvec2(-0.7071, -0.7071)}) {
      const glm::dvec2 q = best + dir * h;
      const double d = clearance(q);
      if (d > bestD) {
        bestD = d;
        best = q;
        moved = true;
      }
    }
    if (!moved)
      h *= 0.5;
  }
  return bestD;
}

} // namespace

double ExtrudeFeature::TaperCloseDistance(
    const std::vector<sketch::Loop>& loops) const {
  std::vector<std::vector<glm::dvec2>> polys;
  for (const sketch::Loop& l : loops)
    polys.push_back(sketch::LoopBoundaryPolygon(l));
  // How many loops hold a point: odd is material (a hole's hole is an
  // island).
  const auto depth = [&](const glm::dvec2& p) {
    int n = 0;
    for (const sketch::Loop& l : loops)
      n += sketch::PointInLoop(l, p) ? 1 : 0;
    return n;
  };
  if (taperDeg > 0.0) // the material closes up
    return Inradius(polys, [&](const glm::dvec2& p) { return depth(p) % 2; });
  // Leaning out, a hole closes up: the smallest of them.
  double least = 1e300;
  for (size_t i = 0; i < loops.size(); ++i) {
    const glm::dvec2 at = polys[i].empty() ? glm::dvec2(0.0) : polys[i][0];
    // A loop inside an odd number of others is a hole.
    int around = 0;
    for (size_t k = 0; k < loops.size(); ++k)
      if (k != i && sketch::PointInLoop(loops[k], at))
        ++around;
    if (around % 2 == 0)
      continue;
    // A round hole's is its radius, exactly; any other's, its boundary's.
    least = std::min(
        least, loops[i].circle ? loops[i].circle->radius
                               : Inradius({polys[i]}, [&](const glm::dvec2& p) {
                                   return sketch::PointInLoop(loops[i], p);
                                 }));
  }
  return least;
}

std::string ExtrudeFeature::WhyIncomplete() const {
  if (taperDeg != 0.0 || !taperExpr.empty()) {
    if (endMode != ExtrudeEnd::Blind)
      return "a taper needs end_mode blind: it leans the walls from the "
             "sketch plane, which is the section there (set taper_deg to 0 "
             "for another end condition)";
    if (thin)
      return "a tapered extrude cannot be thin (set taper_deg to 0, or "
             "turn thin off)";
    if (twistDeg != 0.0 || !twistExpr.empty())
      return "an extrude is twisted or tapered, not both (set one of "
             "twist_deg and taper_deg to 0)";
    if (!std::isfinite(taperDeg) || std::abs(taperDeg) >= 90.0)
      return "taper_deg is how far the walls lean from the extrude "
             "direction, between -90 and 90 degrees";
  }
  if (twistDeg == 0.0 && twistExpr.empty())
    return {};
  if (endMode != ExtrudeEnd::Blind && endMode != ExtrudeEnd::Symmetric) {
    const char* end = endMode == ExtrudeEnd::TwoSided   ? "two_sided"
                      : endMode == ExtrudeEnd::UpToFace ? "up_to_face"
                                                        : "through_all";
    return std::string("a twist needs end_mode blind or symmetric, not ") +
           end + " (set twist_deg to 0 for that end condition)";
  }
  if (thin)
    return "a twisted extrude cannot be thin: a twist sweeps a filled "
           "profile (set twist_deg to 0, or turn thin off)";
  if (!std::isfinite(twistDeg))
    return "twist_deg must be a finite number of degrees";
  return {};
}

bool ExtrudeFeature::Execute(RegenContext& ctx) {
  // The tool is THIS evaluation's or none: a failed or new-body extrude that
  // kept the last one would hand a Pattern of it a cutter it no longer makes.
  m_tool = CADShape();
  m_toolMap = ElementMap();
  if (const std::string why = WhyIncomplete(); !why.empty()) {
    SetError(why);
    return false;
  }
  const Feature* ref = ctx.FindById(sketchId);
  const auto* sketch = dynamic_cast<const SketchFeature*>(ref);
  if (!sketch) {
    SetError("profile sketch (id " + std::to_string(sketchId) +
             ") not found earlier in history");
    return false;
  }
  // A sketch that failed this regeneration (its face or plane is gone) still
  // holds its last plane and profile; extruding those would build the solid
  // where the reference USED to be, which is a fall-back nobody asked for.
  if (!sketch->GetError().empty()) {
    SetError("profile sketch '" + sketch->GetName() +
             "' failed: " + sketch->GetError());
    return false;
  }

  std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch->entities);
  // Single-stroke text makes no profile: refused by name when it is what was
  // picked, or all there is (docs/sketching/README.md#text).
  if (const std::string why = sketch::StrokeTextProfileRefusal(
          sketch->entities, regions, loops.empty());
      !why.empty()) {
    SetError(why);
    return false;
  }
  if (loops.empty()) {
    const std::string where =
        sketch::OpenEndsWords(sketch::OpenEnds(sketch->entities));
    SetError("sketch has no closed profile" +
             (where.empty() ? std::string() : ": " + where));
    return false;
  }
  // Region selection (U2): keep only the chosen regions (+ their holes).
  if (!regions.empty() && !sketch::FilterLoopsToRegions(loops, regions)) {
    SetError("a selected region no longer exists in the sketch - re-pick "
             "the profiles");
    return false;
  }
  if (loops.empty()) {
    SetError("the selected regions leave no profile");
    return false;
  }

  // End condition: every variant is ONE prism of the total length, shifted
  // back along the normal — geometry-identical to sweeping both ways, and
  // the element map stays exactly a blind extrude's.
  double total = distance;
  double backOffset = 0.0; // along the sweep direction, before `reversed`
  bool sweepReversed = reversed;
  if (endMode == ExtrudeEnd::Symmetric) {
    backOffset = distance / 2.0;
  } else if (endMode == ExtrudeEnd::TwoSided) {
    total = distance + distance2;
    backOffset = distance2;
  } else if (endMode == ExtrudeEnd::UpToFace) {
    if (!endTarget.IsSet()) {
      SetError("no target face set for up-to-face");
      return false;
    }
    const auto target = ResolveFacePlane(ctx, endTarget);
    if (!target) {
      SetError(topo::UnresolvedReason(ctx, endTarget, topo::ElementKind::Face,
                                      "target face") +
               " (or is not planar)");
      return false;
    }
    // Signed sweep along the sketch normal to the target's plane — the sign
    // picks the side, so `reversed` does not apply.
    const glm::dvec3 n = sketch->plane.GetNormal();
    total = glm::dot(target->GetOrigin() - sketch->plane.GetOrigin(), n);
    sweepReversed = false;
  } else if (endMode == ExtrudeEnd::ThroughAll) {
    if (mode != ExtrudeMode::Cut) {
      SetError("through_all requires mode 'cut'");
      return false;
    }
    if (ctx.bodies.empty()) {
      SetError("through_all cut needs an existing body");
      return false;
    }
    // Sweep from the sketch plane to just past the farthest body extent
    // along the cut direction (`reversed` picks the side, like a blind cut).
    const glm::dvec3 n = sketch->plane.GetNormal();
    const glm::dvec3 dir = reversed ? -n : n;
    const glm::dvec3 origin = sketch->plane.GetOrigin();
    double reach = 0.0;
    for (const auto& b : ctx.bodies) {
      glm::dvec3 lo, hi;
      BodyBounds(b, lo, hi);
      for (int c = 0; c < 8; ++c) {
        const glm::dvec3 corner{c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y,
                                c & 4 ? hi.z : lo.z};
        reach = std::max(reach, glm::dot(corner - origin, dir));
      }
    }
    total = reach + 1.0;
  }
  if (std::abs(total) < 1e-9) {
    SetError(endMode == ExtrudeEnd::UpToFace
                 ? "target face lies on the sketch plane"
                 : "extrude distance is zero");
    return false;
  }

  // Name the prism's faces from the sketch entities that generate them
  // (topological naming, T2), for every mode: NewBody attaches the map as-is,
  // and Add/Cut propagate it through their boolean (T5) — a face the boolean
  // leaves alone keeps its name, a pierced one gains ";M<id>".
  ElementMap map;
  std::string buildErr;
  const bool twisted = twistDeg != 0.0;
  CADShape prism =
      twisted ? sketch::BuildTwistedPrismNamed(
                    // A symmetric one is the same either way round, so
                    // `reversed` must not flip its hand either.
                    sketch->plane, loops, total,
                    endMode != ExtrudeEnd::Symmetric && sweepReversed,
                    angles::ToRadians(twistDeg), herringbone,
                    endMode == ExtrudeEnd::Symmetric, GetId(), map, &buildErr)
      : thin ? sketch::BuildThinPrismNamed(sketch->plane, loops, total,
                                           sweepReversed, thinThickness,
                                           thinSide, GetId(), map, &buildErr)
             : sketch::BuildPrismNamed(sketch->plane, loops, total,
                                       sweepReversed, GetId(), map, &buildErr);
  if (!prism.IsValid()) {
    SetError(buildErr.empty() ? "profile could not be extruded" : buildErr);
    return false;
  }
  if (taperDeg != 0.0) {
    // Refused past the angle at which the profile closes up
    // (docs/modeling/README.md#tapered-extrudes): its walls move in by
    // L tan(taper) at the far end, and the profile is gone when that
    // reaches its inradius (a hole's, leaning out).
    const double length = std::abs(total);
    const double close = TaperCloseDistance(loops);
    if (close < 1e299) {
      const double limitDeg = glm::degrees(std::atan(close / length));
      if (std::abs(taperDeg) >= limitDeg - 1e-9) {
        char buf[200];
        std::snprintf(buf, sizeof buf,
                      "a taper of %.4g degrees closes the profile up before "
                      "the end: over %.4g mm its %s %.4g mm apart at most, "
                      "so the taper must be under %.4g degrees",
                      taperDeg, length,
                      taperDeg > 0.0 ? "walls come together from"
                                     : "holes' walls come together from",
                      2.0 * close, limitDeg);
        SetError(buf);
        return false;
      }
    }
    std::string why;
    if (!TaperPrism(*sketch, sweepReversed, prism, map, &why)) {
      SetError(why);
      return false;
    }
  }
  if (backOffset != 0.0 && !twisted) { // a twisted one centres itself
    const double sign = reversed ? 1.0 : -1.0;
    prism.Translate(sketch->plane.GetNormal() * (sign * backOffset));
  }

  // A profile whose edges overlap (a notch as deep as the rectangle it is
  // cut from) swept to a solid BRepCheck rejects, and a new body kept it
  // with a clean regen (tools/fuzz.py). Refused, naming why.
  if (const std::string bad = Measure::InvalidReasons(prism); !bad.empty()) {
    SetError("the profile sweeps to an invalid solid (" + bad +
             "): its loops overlap or touch themselves; check the sketch");
    return false;
  }
  if (mode == ExtrudeMode::Cut || mode == ExtrudeMode::Add) {
    m_tool = prism; // for a Pattern of this cut or add (ToolShape)
    m_toolMap = map;
  }
  return ApplyBodyModeReporting(ctx.bodies, std::move(prism), std::move(map),
                                mode, *this);
}

// Each side face drafted about the sketch plane, its name carried to the
// face it became (topological naming keeps a wall's identity under a
// taper as under a length edit); the caps keep theirs, and the edges are
// named again from their faces.
bool ExtrudeFeature::TaperPrism(const SketchFeature& sketch, bool reversed,
                                CADShape& prism, ElementMap& map,
                                std::string* why) const {
  const glm::dvec3 n = sketch.plane.GetNormal() * (reversed ? -1.0 : 1.0);
  const glm::dvec3 o = sketch.plane.GetOrigin();
  const gp_Dir pull(n.x, n.y, n.z);
  const gp_Pln neutral(gp_Pnt(o.x, o.y, o.z), pull);
  const TopoDS_Shape before = prism.GetShape();
  TopTools_IndexedMapOfShape oldFaces;
  TopExp::MapShapes(before, TopAbs_FACE, oldFaces);
  try {
    // One draft per solid: a profile of several regions is a compound.
    std::vector<TopoDS_Shape> solids;
    for (TopExp_Explorer ex(before, TopAbs_SOLID); ex.More(); ex.Next())
      solids.push_back(ex.Current());
    std::vector<std::unique_ptr<BRepOffsetAPI_DraftAngle>> drafts;
    std::vector<TopoDS_Shape> results;
    for (const TopoDS_Shape& solid : solids) {
      auto draft = std::make_unique<BRepOffsetAPI_DraftAngle>(solid);
      for (TopExp_Explorer ex(solid, TopAbs_FACE); ex.More(); ex.Next()) {
        const TopoDS_Face& f = TopoDS::Face(ex.Current());
        const int idx = oldFaces.FindIndex(f) - 1;
        const std::string* name = map.NameOf(idx);
        // The caps stay where they are: the base IS the sketch, and the
        // far one is the taper's narrowed end.
        if (name != nullptr && (name->find("/cap0") != std::string::npos ||
                                name->find("/cap1") != std::string::npos))
          continue;
        draft->Add(f, pull, glm::radians(taperDeg), neutral);
        if (!draft->AddDone()) {
          if (why)
            *why = "the taper could not lean the wall '" +
                   (name ? *name : std::to_string(idx)) +
                   "' (a profile edge the draft cannot follow)";
          return false;
        }
      }
      draft->Build();
      if (!draft->IsDone()) {
        if (why)
          *why = "the taper could not be built on this profile";
        return false;
      }
      results.push_back(draft->Shape());
      drafts.push_back(std::move(draft));
    }
    TopoDS_Shape after;
    if (results.size() == 1) {
      after = results.front();
    } else {
      BRep_Builder b;
      TopoDS_Compound c;
      b.MakeCompound(c);
      for (const TopoDS_Shape& r : results)
        b.Add(c, r);
      after = c;
    }
    TopTools_IndexedMapOfShape newFaces;
    TopExp::MapShapes(after, TopAbs_FACE, newFaces);
    ElementMap out;
    for (const auto& [idx, name] : map.names) {
      if (idx < 0 || idx >= oldFaces.Extent())
        continue;
      const TopoDS_Shape& f = oldFaces(idx + 1);
      for (size_t k = 0; k < solids.size(); ++k) {
        bool in = false;
        for (TopExp_Explorer ex(solids[k], TopAbs_FACE); ex.More() && !in;
             ex.Next())
          in = ex.Current().IsSame(f);
        if (!in)
          continue;
        const TopoDS_Shape img = drafts[k]->ModifiedShape(f);
        const int at = newFaces.FindIndex(img.IsNull() ? f : img);
        if (at > 0)
          out.names[at - 1] = name;
      }
    }
    prism = CADShape(after);
    topo::ApplyFallbackPasses(prism, out);
    map = std::move(out);
    return true;
  } catch (const Standard_Failure& e) {
    if (why)
      *why = std::string("the taper failed in the kernel: ") +
             e.GetMessageString();
    return false;
  }
}
