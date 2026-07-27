#pragma once

#include "core/features/SketchFeature.h"

#include <functional>
#include <glm/glm.hpp>
#include <vector>

namespace sketch {

// Ids of the entities that fall inside the screen-space rectangle
// [rectMin, rectMax]. `crossing` false = window select (an entity is kept only
// if it lies *entirely* inside); true = crossing select (kept if it merely
// touches the box). `project` maps an entity UV point to screen pixels —
// injected so this is camera-free and unit-testable. Construction geometry is
// eligible.
std::vector<uint32_t>
BoxSelect(const std::vector<Entity>& entities, glm::vec2 rectMin,
          glm::vec2 rectMax, bool crossing,
          const std::function<glm::vec2(glm::vec2)>& project);

} // namespace sketch
