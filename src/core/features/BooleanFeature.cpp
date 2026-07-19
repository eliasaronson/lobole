#include "core/features/BooleanFeature.h"

#include "Logger.h"
#include "core/ElementHistory.h"

#include <algorithm>

BooleanFeature::BooleanFeature(const std::string& name, uint64_t targetId,
                               uint64_t toolId, BooleanOp op)
    : targetId(targetId), toolId(toolId), op(op) {
  m_name = name;
}

bool BooleanFeature::Execute(RegenContext& ctx) {
  if (targetId == 0 || toolId == 0 || targetId == toolId) {
    LOG_ERROR("BooleanFeature '{}': needs two distinct operand bodies", m_name);
    return false;
  }

  auto& bodies = ctx.bodies;
  const auto byOwner = [&](uint64_t id) {
    return std::find_if(bodies.begin(), bodies.end(),
                        [&](const Body& b) { return b.createdBy == id; });
  };
  auto target = byOwner(targetId);
  auto tool = byOwner(toolId);
  if (target == bodies.end() || tool == bodies.end()) {
    LOG_ERROR("BooleanFeature '{}': an operand body was not found "
              "(consumed earlier, or its feature made none)",
              m_name);
    return false;
  }

  // Run the boolean with history kept alive so the operands' element names
  // propagate onto the result (toponaming T5).
  ElementMap map;
  CADShape result = topo::BooleanNamed(op, target->shape, target->elementMap,
                                       tool->shape, tool->elementMap, GetId(),
                                       map);
  if (!result.IsValid() || result.Volume() < 1e-9) {
    LOG_ERROR("BooleanFeature '{}': operation produced an empty or invalid "
              "solid (are the bodies disjoint?)",
              m_name);
    return false;
  }

  // Inherit the target's material unless this feature sets its own.
  const std::string material = m_material.empty() ? target->material
                                                  : m_material;
  // Erase both operands, then append the result. Erase the later iterator
  // first so the earlier one stays valid.
  if (target > tool)
    std::swap(target, tool);
  bodies.erase(tool);
  bodies.erase(target);
  bodies.push_back({m_name, std::move(result), material, GetId()});
  bodies.back().elementMap = std::move(map);
  return true;
}
