#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>

// Retaining rings as generated standard parts
// (docs/parts/README.md#retaining-rings). Placing one in its groove waits for
// the fastener placement rework (fasteners follow their holes' named faces);
// this places one where told, as AddBearing does.
namespace ui {

struct AppState;

// Place a DIN 471 / DIN 472 ring in the ACTIVE ASSEMBLY where told: one
// generated part per spec, named "DIN 471 25x1.2" and filed with the
// standard parts, so `bom` counts ten of one as one row. Refused on a part
// tab; an unknown standard or size is refused naming what is carried, before
// any tab is made. Returns the occurrence id, or 0 with `error`.
uint64_t AddRetainingRing(AppState& state, const std::string& standard,
                          double size, const glm::dvec3& position,
                          const glm::dquat& orientation, std::string* error);

} // namespace ui
