#pragma once

#include <cstdint>

#include <glm/glm.hpp>

struct RegenContext;

// A toponaming-free reference to an axis in model space: one of the global
// axes, or a sketch line addressed by stable feature id + stable entity id
// (the same discipline sketch constraints use). Shared by DatumPlane (angled
// variant) and, later, Revolve (M2). Never references B-Rep edges — that
// waits for topological naming (M6).
struct AxisRef {
  enum class Kind { X, Y, Z, SketchLine, DatumAxis };

  Kind kind = Kind::X;
  uint64_t sketchId = 0;    // SketchLine: owning sketch's feature id
  uint32_t entityId = 0;    // SketchLine: stable sketch-entity id
  uint64_t datumAxisId = 0; // DatumAxis: referenced DatumAxisFeature id
};

// Resolves the axis to a world-space origin + unit direction against the
// features visible in ctx (strictly earlier in the history). Returns false
// if a SketchLine reference doesn't resolve or is degenerate.
bool ResolveAxis(const RegenContext& ctx, const AxisRef& ref,
                 glm::vec3& outOrigin, glm::vec3& outDir);
