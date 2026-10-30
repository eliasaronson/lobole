// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/BooleanFeature.h"

#include "core/BodyMode.h" // BooleanRefusal
#include "core/Measure.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"
#include "core/MeshOperand.h"
#include "core/mesh/MeshBoolean.h"

#include <algorithm>
#include <cmath>

BooleanFeature::BooleanFeature(const std::string& name, uint64_t targetId,
                               uint64_t toolId, BooleanOp op)
    : targetId(targetId), toolId(toolId), op(op) {
  m_name = name;
}

bool BooleanFeature::Execute(RegenContext& ctx) {
  if (targetId == 0 || toolId == 0 || targetId == toolId) {
    SetError("needs two distinct operand bodies");
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
    SetError("an operand body was not found (consumed earlier, or its "
             "feature made none)");
    return false;
  }
  if (target->reference || tool->reference) {
    SetError("'" + (target->reference ? target->name : tool->name) +
             "' is a derived reference body, not this part's matter — set "
             "reference: false on its Derived feature to use it in a boolean");
    return false;
  }

  m_meshReport = MeshReport{};
  // A mesh operand runs on triangles, through manifold.
  if (target->IsMesh() || tool->IsMesh())
    return ExecuteMesh(ctx, target, tool);

  // Run the boolean with history kept alive so the operands' element names
  // propagate onto the result (toponaming T5).
  const double targetVol = target->shape.Volume();
  // `CADShape::IsValid` is a null check, not a validity check. A boolean can
  // return a shape that is not null, has a plausible volume, and is not a
  // well-formed B-Rep — and everything asked of it afterwards is then
  // confidently wrong: one axle bore drilled through both dropouts of a
  // bicycle frame in a single pass left a body OpenCASCADE calls "bad wire
  // nesting", and the assembly's interference sweep went on to report
  // 17,297 mm3 of the rear cog buried in a frame whose bounding box does not
  // reach it. The check costs 0.2 s on the worst body in that bicycle (a rim
  // laced to a hub with 36 spokes) and is worth it HERE, at the operation
  // that made the mess, where the feature can simply fail.
  const auto wellFormed = [](const CADShape& result) -> std::string {
    const Measure::GeometryReport report = Measure::CheckGeometry(result);
    if (report.valid)
      return {};
    std::string why;
    for (const std::string& r : report.reasons)
      why += (why.empty() ? "" : ", ") + r;
    return "operation produced an INVALID solid" +
           (why.empty() ? "" : " (" + why + ")") +
           " — it has volume and is not a well-formed B-Rep, so anything "
           "measured on it afterwards would be wrong";
  };
  // Well-formed is not the same as right: what a boolean must keep — volume
  // the right way, no shell inside out, no face left inside the tool — is
  // checked before "is it empty", so an inside-out result is called that
  // and not "disjoint"
  // (docs/modeling/README.md#a-boolean-is-checked-before-it-is-kept). A
  // result refused by either check gets one fuzzy retry, which has to pass
  // both (docs/modeling/README.md#a-refused-boolean-gets-one-fuzzy-retry).
  CheckedBoolean checked = CheckedBooleanNamed(
      m_name, op, target->name, target->shape, target->elementMap, tool->shape,
      tool->elementMap, GetId(), wellFormed);
  if (!checked.refusal.empty()) {
    SetError(checked.refusal);
    return false;
  }
  if (!checked.shape.IsValid()) {
    SetError("operation produced an empty or invalid solid (are the bodies "
             "disjoint?)" +
             (checked.error.empty() ? "" : " — " + checked.error));
    return false;
  }
  CADShape result = std::move(checked.shape);
  ElementMap map = std::move(checked.map);
  const std::string& boolErr = checked.error;
  if (checked.fuzzy > 0.0)
    SetWarning(checked.warning);
  if (result.Volume() < 1e-9) {
    SetError("operation produced an empty or invalid solid (are the bodies "
             "disjoint?)" +
             (boolErr.empty() ? "" : " — " + boolErr));
    return false;
  }
  // A subtract/union whose result equals the target did nothing — the operands
  // are disjoint (or the tool is engulfed). Valid, but almost certainly a
  // mistake, so advise without failing.
  if ((op == BooleanOp::Subtract || op == BooleanOp::Union) &&
      std::abs(result.Volume() - targetVol) < 1e-6 * std::max(targetVol, 1.0)) {
    SetWarning(op == BooleanOp::Subtract
                   ? "subtract removed no material — the tool body does not "
                     "intersect the target"
                   : "union added no material — the bodies are disjoint or the "
                     "tool is inside the target");
  }

  // Inherit the target's material unless this feature sets its own.
  const std::string material =
      m_material.empty() ? target->material : m_material;
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

bool BooleanFeature::ExecuteMesh(RegenContext& ctx,
                                 std::vector<Body>::iterator target,
                                 std::vector<Body>::iterator tool) {
  auto& bodies = ctx.bodies;
  double dTarget = 0.0, dTool = 0.0;
  const auto a = meshop::Triangles(*target, 0.0, dTarget);
  const auto b = meshop::Triangles(*tool, 0.0, dTool);
  for (const auto& [m, body] :
       {std::pair{a.get(), &*target}, {b.get(), &*tool}})
    if (const std::string why =
            meshop::OpenOperandRefusal(*m, body->name, "A mesh Boolean");
        !why.empty()) {
      SetError(why);
      return false;
    }
  const mesh::BooleanOp kind = op == BooleanOp::Union ? mesh::BooleanOp::Union
                               : op == BooleanOp::Subtract
                                   ? mesh::BooleanOp::Subtract
                                   : mesh::BooleanOp::Intersect;
  mesh::TriMesh result;
  std::string error;
  if (!mesh::Boolean(*a, *b, kind, result, error)) {
    SetError("the mesh boolean failed: " + error);
    return false;
  }
  if (result.triangles.empty()) {
    SetError("operation produced an empty solid (are the bodies disjoint?)");
    return false;
  }
  const double before = a->Volume(), after = result.Volume();
  if ((op == BooleanOp::Subtract || op == BooleanOp::Union) &&
      std::abs(after - before) < 1e-6 * std::max(before, 1.0))
    SetWarning(op == BooleanOp::Subtract
                   ? "subtract removed no material — the tool body does not "
                     "intersect the target"
                   : "union added no material — the bodies are disjoint or "
                     "the tool is inside the target");
  m_meshReport.mesh = true;
  m_meshReport.targetDeflectionMm = dTarget;
  m_meshReport.toolDeflectionMm = dTool;
  m_meshReport.triangles = result.TriangleCount();
  // The target's meshSince carries on; a B-Rep target is made a mesh here.
  const std::string since = target->IsMesh()
                                ? target->meshSince
                                : "Boolean '" + m_name + "' (a mesh operand)";
  const std::string material =
      m_material.empty() ? target->material : m_material;
  if (target > tool)
    std::swap(target, tool);
  bodies.erase(tool);
  bodies.erase(target);
  Body out;
  out.name = m_name;
  out.material = material;
  out.createdBy = GetId();
  out.mesh = std::make_shared<const mesh::TriMesh>(std::move(result));
  out.elementMap.names[0] = std::to_string(GetId()) + "/mesh";
  out.meshSince = since;
  bodies.push_back(std::move(out));
  return true;
}

nlohmann::json BooleanFeature::ReportJson() const {
  if (!m_meshReport.mesh)
    return nullptr;
  nlohmann::json j = {{"mesh", true}, {"triangles", m_meshReport.triangles}};
  if (m_meshReport.targetDeflectionMm > 0.0)
    j["target_deflection_mm"] = m_meshReport.targetDeflectionMm;
  if (m_meshReport.toolDeflectionMm > 0.0)
    j["tool_deflection_mm"] = m_meshReport.toolDeflectionMm;
  return j;
}
