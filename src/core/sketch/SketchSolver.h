#pragma once

#include "core/features/SketchFeature.h"

#include <glm/glm.hpp>
#include <vector>

namespace sketch {

struct SolveResult {
  bool ok = false;
  int dof = -1; // remaining degrees of freedom (0 = fully constrained)
  std::vector<int> conflicting; // raw PlaneGCS tags
  std::vector<int> redundant;
  // Mapped back to indices into SketchFeature::constraints, for the UI to flag
  // the offending constraints.
  std::vector<size_t> conflictingConstraints;
  std::vector<size_t> redundantConstraints;
};

// Adapter around PlaneGCS: maps a SketchFeature's Point/Line/Circle entities
// and constraints to the solver, solves, and writes solved coordinates back.
// Rectangles are treated as rigid in v1 (not solved). Falls back to a no-op
// when built without HAVE_PLANEGCS.
class Solver {
public:
  Solver();
  ~Solver();

  // Build solver geometry + constraints from the sketch.
  void Load(const SketchFeature& sketch);

  SolveResult Solve();

  // --- Interactive drag session (FreeCAD moveGeometry recipe) --------------
  // Built once per drag: after Load(), Begin* pins the dragged element with a
  // *weak* temporary constraint (rescaled 0.01) so the rest of the sketch
  // follows without oscillating, then per frame overwrite the target(s) and
  // SolveDrag() — no rebuild. Begin* returns false (refuse the drag) if the
  // sketch is already conflicting.

  // Drag one or more points toward per-point UV targets (a line body pins both
  // endpoints; an endpoint pins one).
  bool BeginDrag(const std::vector<PointRef>& points);
  void SetDragTargets(const std::vector<glm::vec2>& targets);

  // Drag a circle's radius toward a target length (centre held by its own
  // constraints). Pair with SetRadiusTarget.
  bool BeginRadiusDrag(uint32_t circleId);
  void SetRadiusTarget(double radius);

  // Re-solve with the current drag target(s); no reload/redeclare.
  SolveResult SolveDrag();

  // Copies solved coordinates into a copy of the sketch's entities.
  void WriteBack(std::vector<Entity>& entities) const;

  // Entity ids with at least one free (under-determined) coordinate after
  // the last Solve — PlaneGCS's dependent-parameter diagnosis mapped back to
  // entities. Empty when the sketch is fully constrained (or unsolved).
  std::vector<uint32_t> UnderDefinedEntities() const;

private:
  struct Impl;
  Impl* m_impl;
};

} // namespace sketch
