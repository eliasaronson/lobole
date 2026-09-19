#include "core/features/CopeFeature.h"

#include "core/Angles.h"
#include "core/ElementHistory.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/PipeFeature.h"

#include <glm/glm.hpp>

#include <algorithm>

namespace {

// The centreline a feature was swept along, when it is a bent path.
const CurveFeature* BentPathOf(const RegenContext& ctx, const Feature* f) {
  const auto* pipe = dynamic_cast<const PipeFeature*>(f);
  if (pipe == nullptr)
    return nullptr;
  const auto* curve =
      dynamic_cast<const CurveFeature*>(ctx.FindById(pipe->pathId));
  if (curve == nullptr || curve->kind != Curve3DKind::BentPath)
    return nullptr;
  return curve;
}

// Whether this cope lands on the neighbour's BEND, and the sentence that
// says so.
//
// A joint does not belong on a bend: the tube there is a torus, it has been
// thinned by the bending, and there is no straight to fit a cope against —
// so a cut that finds nothing at a bend is a DESIGN answer, not a kernel
// quirk, and blaming tangency sends the reader to open a gap that will not
// help. A bend occupies the path from one tangent point to the other, which
// is `radius * tan(angle/2)` either side of the corner it was drawn from.
std::string OnABend(const RegenContext& ctx, const CopeFeature& cope,
                    const Feature* neighbour) {
  const CurveFeature* theirs = BentPathOf(ctx, neighbour);
  const CurveFeature* ours = BentPathOf(ctx, ctx.FindById(cope.memberId));
  if (theirs == nullptr || ours == nullptr || ours->points.empty())
    return {};
  const auto* pipe =
      dynamic_cast<const PipeFeature*>(ctx.FindById(cope.memberId));
  const double reach = pipe != nullptr ? pipe->outerDiameter * 0.5 : 0.0;

  for (const glm::vec3& end : {ours->points.front(), ours->points.back()}) {
    for (const CurveFeature::Bend& b : theirs->Bends()) {
      const double tangent =
          b.radius * std::tan(angles::ToRadians(b.angleDeg) * 0.5);
      const double d = glm::length(glm::dvec3(end) - glm::dvec3(b.corner));
      if (d < tangent + reach) {
        return "the joint lands on a BEND of '" + neighbour->GetName() +
               "'. A bend is a torus, thinned by the bending, with no "
               "straight to fit a cope against, so a joint does not belong "
               "there — move it clear of the bend, which reaches " +
               std::to_string(static_cast<int>(std::lround(tangent))) +
               " mm either side of that corner";
      }
    }
  }
  return {};
}

} // namespace

CopeFeature::CopeFeature(const std::string& name, uint64_t memberId,
                         std::vector<uint64_t> against)
    : memberId(memberId), against(std::move(against)) {
  m_name = name;
}

bool CopeFeature::Execute(RegenContext& ctx) {
  auto member =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == memberId; });
  if (member == ctx.bodies.end()) {
    SetError("the member (feature " + std::to_string(memberId) +
             ") has no body — did it fail, or has something already consumed "
             "it?");
    return false;
  }
  if (against.empty()) {
    SetError("a cope needs something to cope TO");
    return false;
  }

  CADShape shape = member->shape;
  ElementMap map = member->elementMap;
  for (const uint64_t id : against) {
    if (id == memberId) {
      SetError("a member cannot be coped to itself");
      return false;
    }
    const Feature* f = ctx.FindById(id);
    if (f == nullptr) {
      SetError("feature " + std::to_string(id) +
               " is not earlier in this part's history");
      return false;
    }
    // A tube neighbour is rebuilt from its PATH as a solid, so the cut is the
    // outside of the tube and not its wall. Anything else cuts with the body
    // it made — non-destructively, which is the whole point.
    CADShape cutter;
    ElementMap cutterMap;
    if (const auto* pipe = dynamic_cast<const PipeFeature*>(f)) {
      std::string why;
      cutter =
          PipeOuterSolid(ctx, pipe->pathId, pipe->outerDiameter, gap, &why);
      if (!cutter.IsValid()) {
        SetError("cope to '" + f->GetName() + "': " + why);
        return false;
      }
    } else {
      if (gap > 1e-9) {
        SetError("a gap is a tube's, measured on its radius — '" +
                 f->GetName() + "' is a " + f->TypeName() +
                 ", so there is no radius to open");
        return false;
      }
      const auto neighbour =
          std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                       [&](const Body& b) { return b.createdBy == id; });
      if (neighbour == ctx.bodies.end()) {
        SetError("'" + f->GetName() + "' has no body to cope to");
        return false;
      }
      cutter = neighbour->shape;
      cutterMap = neighbour->elementMap;
    }

    ElementMap out;
    std::string why;
    const double was = shape.Volume();
    CADShape next = topo::BooleanNamed(BooleanOp::Subtract, shape, map, cutter,
                                       cutterMap, GetId(), out, &why);
    if (!next.IsValid() || next.Volume() < 1e-9) {
      SetError("cope to '" + f->GetName() +
               "' removed the whole member (do they actually meet?)" +
               (why.empty() ? "" : ": " + why));
      return false;
    }
    // A cope that removed NOTHING is a joint that does not exist. Reporting
    // it as done puts an uncoped tube in the order and a gap in the frame,
    // and both are found at the welding table.
    //
    // There are two ways to remove nothing and they want different answers,
    // so the boxes are asked which one this is: members that do not reach
    // each other, and members that overlap while the KERNEL declines. The
    // second is the tangency case — two tubes of the same diameter whose
    // axes meet exactly have surfaces that touch along a curve rather than
    // crossing it, and a boolean asked to cut one with the other hands back
    // the argument. It is not rare in a frame: it is what every equal
    // diameter joint drawn to its node looks like.
    if (next.Volume() > was - 1e-9) {
      glm::vec3 lo{0.0f}, hi{0.0f}, clo{0.0f}, chi{0.0f};
      shape.GetBoundingBox(lo, hi);
      cutter.GetBoundingBox(clo, chi);
      const bool apart = hi.x < clo.x || chi.x < lo.x || hi.y < clo.y ||
                         chi.y < lo.y || hi.z < clo.z || chi.z < lo.z;
      if (apart) {
        SetError("cope to '" + f->GetName() +
                 "' removed nothing — the two do not reach each other, so "
                 "there is no joint here to cut");
      } else if (const std::string bend = OnABend(ctx, *this, f);
                 !bend.empty()) {
        SetError("cope to '" + f->GetName() + "' removed nothing: " + bend);
      } else {
        SetError(
            "cope to '" + f->GetName() +
            "' removed nothing, although they overlap: the cut is TANGENT, "
            "which a boolean answers by handing back what it was given. Two "
            "tubes of one diameter whose axes meet exactly do this. Give the "
            "cope a `gap` — 0.05 mm is enough to make the surfaces cross, "
            "and a real cope is cut a little open for the weld root anyway");
      }
      return false;
    }
    shape = std::move(next);
    map = std::move(out);
  }

  // The member is REPLACED, not consumed into something new: it is the same
  // part of the frame, one cut shorter, and the cut list has to go on seeing
  // one member where there is one member.
  member->shape = std::move(shape);
  member->elementMap = std::move(map);
  return true;
}
