// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>

// Retaining rings as generated standard parts
// (docs/parts/README.md#retaining-rings): placing one where told, as
// AddBearing does, and placing one IN its groove, seated on the groove's
// named faces so it follows the groove
// (docs/parts/README.md#a-ring-in-its-groove).
namespace ui {

struct AppState;
struct RimPick;

// Place a DIN 471 / DIN 472 ring in the ACTIVE ASSEMBLY where told: one
// generated part per spec, named "DIN 471 25x1.2" and filed with the
// standard parts, so `bom` counts ten of one as one row. Refused on a part
// tab; an unknown standard or size is refused naming what is carried, before
// any tab is made. Returns the occurrence id, or 0 with `error`.
uint64_t AddRetainingRing(AppState& state, const std::string& standard,
                          double size, const glm::dvec3& position,
                          const glm::dquat& orientation, std::string* error);

// What fitting a ring into a groove worked out, placed or not.
struct RetainingRingFit {
  bool ok = false;
  std::string why;     // the refusal, when !ok
  std::string summary; // one line: which ring, in which groove
  std::string standard;
  double size = 0.0;
  std::string designation; // "DIN 471 25x1.2"
  uint64_t parent = 0;     // the placement the groove is in (top level)
  uint64_t groove = 0;     // the RetainingRingGroove feature's id
  glm::dvec3 position{0.0};
  glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
  uint64_t occurrence = 0; // placed (not on a dry run)
};

// Put the ring a GROOVE takes into it: `groove` names any face of a
// RetainingRingGroove (its floor or a flank) on a placement in the active
// assembly. The ring's standard and size are the groove's; it is added
// (AddRetainingRing), attached to that placement and SEATED on two of the
// groove's faces by assembly::SeatRider — its side A on `groove_flank_a`,
// its axis the floor's — so it follows the groove when the part changes.
// Refused: not an assembly, a face that is no groove's, a name several
// placements answer to, a groove that already holds a ring.
RetainingRingFit FitRetainingRing(AppState& state, const RimPick& groove,
                                  bool dryRun);

} // namespace ui
