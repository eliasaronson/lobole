#include "core/features/CopeFeature.h"

#include "core/ElementHistory.h"
#include "core/features/BooleanFeature.h"
#include "core/features/PipeFeature.h"

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
    if (next.Volume() > was - 1e-9) {
      SetError("cope to '" + f->GetName() +
               "' removed nothing — the two do not meet, so there is no "
               "joint here to cut");
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
