// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/cam/CamJob.h"

#include <glm/glm.hpp>

#include <string>

// Cutter radius compensation in the controller, wear mode
// (docs/cam/post.md#cutter-compensation).
//
// The file stays the tool centre -- the cut the verifier, the playback and
// every check read -- and the wall passes go under G41/G42 so the machinist
// corrects a measured wall by typing its error into the tool table's
// diameter, without re-posting. Here the declared wall passes
// (`Toolpath::walls`) get the lead-in and lead-out a controller needs to turn
// compensation on and off away from the wall, and are checked for what a
// controller cannot offset; the post writes the words.
namespace cam {

// The wear a compensated run is checked to take either way, on the DIAMETER
// (what the tool table's entry holds): a tenth of the cutter. 0.6 mm for a
// 6 mm cutter -- far more than a router's runout and deflection, and small
// enough that the cutter's own rounded corners (a radius of the cutter) still
// offset cleanly.
[[nodiscard]] inline double CompCheckedWear(const Tool& tool) {
  return tool.diameter * 0.1;
}

// Turn each declared wall pass of `tp` into a compensated run (Toolpath::comps)
// with its lead-in and lead-out on the waste side, rerouting the approach to
// the lead-in's start and the retract over the lead-out's end. `origin` is the
// work origin, so a refusal names a place in the file's coordinates. Returns
// false with `why` -- the operation's error -- when a wall cannot be
// compensated: no room for the leads, or a turn the controller cannot offset
// by the checked wear.
bool CompensateWalls(Toolpath& tp, const Tool& tool, const glm::dvec3& origin,
                     std::string& why);

} // namespace cam
