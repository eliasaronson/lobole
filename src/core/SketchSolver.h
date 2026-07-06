#pragma once

#include "core/SketchFeature.h"

#include <glm/glm.hpp>
#include <vector>

namespace sketch {

struct SolveResult {
  bool ok = false;
  int dof = -1; // remaining degrees of freedom (0 = fully constrained)
  std::vector<int> conflicting;
  std::vector<int> redundant;
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

  // Pin a point (entity index + which) toward a UV target for interactive
  // dragging; the rest of the sketch follows the constraints. Call after Load,
  // before Solve.
  void SetDragPoint(const PointRef& point, glm::vec2 targetUV);

  SolveResult Solve();

  // Copies solved coordinates into a copy of the sketch's entities.
  void WriteBack(std::vector<Entity>& entities) const;

private:
  struct Impl;
  Impl* m_impl;
};

} // namespace sketch
