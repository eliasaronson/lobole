// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/FilletFeature.h"
#include "core/Measure.h" // InvalidReasons

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <algorithm>

FilletFeature::FilletFeature(const std::string& name,
                             std::vector<ElementRef> edges, double radius,
                             EdgeBlendOp op)
    : edges(std::move(edges)), op(op), radius(radius) {
  m_name = name;
}

bool FilletFeature::Execute(RegenContext& ctx) {
  if (edges.empty()) {
    return true; // previewing: no edges picked yet, nothing to do
  }
  const char* what = op == EdgeBlendOp::Chamfer ? "chamfer" : "fillet";

  const uint64_t bodyFeature = edges[0].featureId;
  for (const ElementRef& ref : edges) {
    if (ref.featureId != bodyFeature) {
      SetError("all edges of one blend must belong to the same body");
      return false;
    }
  }
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == bodyFeature; });
  if (body == ctx.bodies.end()) {
    SetError("the body the edges belong to no longer exists");
    return false;
  }

  // Resolve every ref against the body's current map — a name that is gone
  // (deleted entity, consumed edge) is a first-class failure with reattach,
  // never a silent renumber.
  std::vector<int> indices;
  for (const ElementRef& ref : edges) {
    const auto idx = body->elementMap.ResolveEdge(ref.name);
    if (!idx) {
      SetError(
          topo::UnresolvedReason(ctx, ref, topo::ElementKind::Edge, "edge"));
      return false;
    }
    if (std::find(indices.begin(), indices.end(), *idx) == indices.end()) {
      indices.push_back(*idx); // two refs can land on one edge; blend it once
    }
  }

  if (radius <= 1e-9) {
    SetError(std::string(what) + " radius must be positive");
    return false;
  }

  ElementMap map;
  std::string error;
  CADShape result = topo::EdgeBlendNamed(body->shape, body->elementMap, indices,
                                         op, radius, GetId(), map, &error);
  if (!result.IsValid()) {
    SetError(error.empty() ? std::string(what) +
                                 " failed (radius too large for the geometry?)"
                           : error);
    LOG_ERROR("FilletFeature '{}': {}", m_name, GetError());
    return false;
  }

  // Kept only when it is a valid solid: the blend builder reported success
  // on blends that crossed themselves, and the part went on with a body
  // check_geometry called invalid (tools/fuzz.py).
  if (const std::string bad = Measure::InvalidReasons(result); !bad.empty()) {
    SetError(std::string(what) + " produced an invalid solid (" + bad +
             "); a smaller radius, or fewer edges in one " + what +
             ", usually builds");
    return false;
  }
  body->shape = std::move(result);
  body->elementMap = std::move(map);
  return true;
}
