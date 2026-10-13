// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/features/SketchFeature.h"

#include <glm/glm.hpp>
#include <string>
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

// One dimension's value edit: constraint `constraint` goes from `from` (what
// the geometry the solver was loaded with measures, MeasureDimension) to `to`
// (the value the loaded sketch carries), both in the constraint's own units
// (radians for an angle, the diameter for a diameter).
struct ValueChange {
  size_t constraint = 0;
  double from = 0.0, to = 0.0;
};

// What SolveValueEdit did. Accepted (`refusal` empty) = the new values were
// solved and no clear orientation turned over; the solver then holds the new
// geometry. Refused = the solver holds the pre-edit geometry again, and
// `refusal` says why in the words every edit path shows
// (docs/sketching/README.md#solving).
struct EditOutcome {
  SolveResult solve;
  std::vector<size_t> flipped; // constraints whose orientation turned over
  bool reached = false;        // the new values were solved
  std::string refusal;
  bool Accepted() const { return refusal.empty(); }
};

// Adapter around PlaneGCS: maps a SketchFeature's entities (points, lines,
// circles, arcs, ellipses, ellipse arcs, B-splines, and a text label's anchor
// with its box corners riding it) and constraints to the solver, solves, and
// writes solved coordinates back. A rectangle is four ordinary lines, solved
// like any other. Falls back to a no-op when built
// without HAVE_PLANEGCS.
class Solver {
public:
  Solver();
  ~Solver();

  // Build solver geometry + constraints from the sketch.
  void Load(const SketchFeature& sketch);

  SolveResult Solve();

  // Solves a value edit without turning the sketch over. Call after Load on
  // the sketch carrying the new values, seeded with the pre-edit geometry.
  // One jump first; when that fails or turns a triangle of distances or an
  // arc's sweep over (SketchOrientation.h), the changed dimensions step from
  // their old values to the new ones (continuation), each step keeping every
  // orientation that was clear where it started. An empty `changes` (values
  // already in the entities, as expression-driven coordinates are) is the
  // one jump and the check.
  EditOutcome SolveValueEdit(const std::vector<ValueChange>& changes);

  // Test hook: how many solves this solver has run.
  int SolveCount() const;

  // --- Interactive drag session (FreeCAD moveGeometry recipe) --------------
  // Built once per drag: after Load(), Begin* pins the dragged element with a
  // *weak* temporary constraint (rescaled 0.01) so the rest of the sketch
  // follows without oscillating, then per frame overwrite the target(s) and
  // SolveDrag() — no rebuild. Begin* returns false (refuse the drag) if the
  // sketch is already conflicting.

  // Drag one or more points toward per-point UV targets (a line body pins both
  // endpoints; an endpoint pins one).
  bool BeginDrag(const std::vector<PointRef>& points);
  void SetDragTargets(const std::vector<glm::dvec2>& targets);

  // Drag a circle's radius toward a target length (centre held by its own
  // constraints). Pair with SetRadiusTarget.
  bool BeginRadiusDrag(uint32_t circleId);
  void SetRadiusTarget(double radius);

  // Re-solve with the current drag target(s); no reload/redeclare. Guarded,
  // a frame that would turn a clear orientation over (against the last
  // frame) retries with the target stepped from the last frame's, halving,
  // and keeps the furthest step that holds: the geometry stops at the fold.
  enum class DragGuard { Orientations, None };
  SolveResult SolveDrag(DragGuard guard = DragGuard::Orientations);

  // Copies solved coordinates into a copy of the sketch's entities.
  void WriteBack(std::vector<Entity>& entities) const;

  // Entity ids with at least one free (under-determined) coordinate after
  // the last Solve — PlaneGCS's dependent-parameter diagnosis mapped back to
  // entities. Empty when the sketch is fully constrained (or unsolved).
  std::vector<uint32_t> UnderDefinedEntities() const;

  // The constraints Load could not represent, each with why and what its
  // kind accepts. Load's dispatch IS the validator: add_constraint, the GUI
  // and the sketch's regeneration warning all read this, so what is refused
  // and what is solved cannot drift apart
  // (docs/sketching/README.md#refusals).
  struct Unsupported {
    size_t index = 0; // into SketchFeature::constraints
    std::string reason;
  };
  const std::vector<Unsupported>& UnsupportedConstraints() const;

private:
  struct Impl;
  Impl* m_impl;
};

} // namespace sketch
