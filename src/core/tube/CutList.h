#pragma once

#include "cad/CADKernel.h"
#include "core/Feature.h" // Body

#include <optional>
#include <utility>

// How much of a member's path still has metal on it (docs/tube/plan.md, T3).
//
// A tube is cut to the DEVELOPED length of its centreline — but a member that
// has been coped is shorter than the path it was swept along, by the depth of
// the cope at each end. Reporting the node-to-node centreline instead is 8-9
// mm long per coped end on a Ø19 brace into a Ø25.4 run, which is a number
// that arrives as forty tubes that do not fit.
//
// Measured rather than derived: every vertex of the body is projected onto the
// path and its ARC LENGTH from the start taken, so the answer is right for a
// bent member as well as a straight one, and right for whatever shape the cope
// happens to be.
namespace tube {

// The arc-length span [from, to] of `pathEdge` that `body` still occupies, in
// millimetres from the path's start. nullopt when the path is not a usable
// edge or the body has no vertices to project.
[[nodiscard]] std::optional<std::pair<double, double>>
MetalAlongPath(const Body& body, const CADShape& pathEdge);

} // namespace tube
