// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/ExprValue.h"

#include <cstdint>

#include <glm/glm.hpp>

struct RegenContext;

// A toponaming-free reference to an axis in model space: one of the global
// axes, or a sketch line addressed by stable feature id + stable entity id
// (the same discipline sketch constraints use). Shared by DatumPlane (angled
// variant), Revolve and Pattern. Never references B-Rep edges — that
// waits for topological naming (M6).
//
// Custom (v128) is an axis written out as a point and a direction, in mm:
// the way a script already knows where a turned bore off the origin is, which
// otherwise needed a datum axis or a sketch line whose entity id is random.
struct AxisRef {
  enum class Kind { X, Y, Z, SketchLine, DatumAxis, Custom };

  Kind kind = Kind::X;
  uint64_t sketchId = 0;    // SketchLine: owning sketch's feature id
  uint32_t entityId = 0;    // SketchLine: stable sketch-entity id
  uint64_t datumAxisId = 0; // DatumAxis: referenced DatumAxisFeature id
  glm::dvec3 origin{0.0};   // Custom: a point on the axis, mm
  Vec3Exprs originExpr;     // Custom: its texts ("" for a number)
  glm::dvec3 direction{0.0, 0.0, 1.0}; // Custom: need not be unit
};

// A custom axis's point, each coordinate a slot at `<path>/origin/0..2`;
// nothing for any other kind, whose axis is a reference.
inline void VisitAxisExprs(AxisRef& ref, const ExprVisitor& visit,
                           const std::string& path) {
  if (ref.kind == AxisRef::Kind::Custom)
    VisitVec3Exprs(visit, ref.origin, ref.originExpr, ExprQuantity::Length,
                   path + "/origin");
}

// Resolves the axis to a world-space origin + unit direction against the
// features visible in ctx (strictly earlier in the history). Returns false
// if a SketchLine reference doesn't resolve or is degenerate.
bool ResolveAxis(const RegenContext& ctx, const AxisRef& ref,
                 glm::dvec3& outOrigin, glm::dvec3& outDir);
