#include "core/features/RefineFeature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>

RefineFeature::RefineFeature(const std::string& name, uint64_t bodyId)
    : bodyId(bodyId) {
  m_name = name;
}

bool RefineFeature::Execute(RegenContext& ctx) {
  const auto body = std::find_if(
      ctx.bodies.begin(), ctx.bodies.end(),
      [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the body to refine no longer exists");
    return false;
  }

  try {
    // Unify coplanar faces and the seam edges between them; leave B-splines
    // unconcatenated (keeps curved faces faithful).
    ShapeUpgrade_UnifySameDomain unify(body->shape.GetShape(),
                                       /*UnifyEdges=*/true,
                                       /*UnifyFaces=*/true,
                                       /*ConcatBSplines=*/false);
    unify.Build();
    CADShape result(unify.Shape());
    if (!result.IsValid() || result.Volume() < 1e-9) {
      SetError("refine produced an empty or invalid solid");
      return false;
    }
    // Topology is reorganized; give the result fresh primitive-style names so
    // it stays referenceable (like shell/draft).
    ElementMap map;
    topo::NamePrimitiveElements(result, std::to_string(GetId()), map);
    body->shape = std::move(result);
    body->elementMap = std::move(map);
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("refine failed: ") + e.GetMessageString());
    LOG_ERROR("RefineFeature '{}': {}", m_name, GetError());
    return false;
  }
}
