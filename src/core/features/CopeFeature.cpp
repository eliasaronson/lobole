#include "core/features/CopeFeature.h"

#include "core/ElementHistory.h"
#include "core/features/BooleanFeature.h"
#include "core/features/PipeFeature.h"

#include <glm/glm.hpp>

#include <algorithm>

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
