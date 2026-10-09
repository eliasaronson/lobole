// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/RefineFeature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BRepTools_History.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>

RefineFeature::RefineFeature(const std::string& name, uint64_t bodyId)
    : bodyId(bodyId) {
  m_name = name;
}

bool RefineFeature::Execute(RegenContext& ctx) {
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
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
    // Names through the unifier's history: a face it leaves alone keeps its
    // name, merged pieces take one of theirs with ";M<id>". Fresh names
    // ("<id>/faceN") broke every reference to the body -- a study's loads,
    // a bearing's seat, a mate -- for the sake of the seams it cleaned, so
    // the advice to Refine a body that will not mesh cost the study its
    // faces (docs/modeling/toponaming.md).
    ElementMap map;
    if (const Handle(BRepTools_History) & history = unify.History();
        !history.IsNull())
      map = topo::NamesThroughHistory(body->shape, body->elementMap, *history,
                                      result, GetId());
    else
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
