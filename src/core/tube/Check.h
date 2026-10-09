// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/tube/Rules.h"

#include <string>
#include <vector>

// Tube manufacturability as a query (docs/tube/README.md#checks).
//
// Every one of these is valid geometry and comes back from the bender as a
// question: a former tighter than the wall can take, a radius nobody owns a
// die for, two bends too close together to clamp between, nothing left to
// grip past the last one, an outer wall pulled thinner than a rulebook
// allows.
//
// Structured like sheet::Check — every finding names the member, the bend and
// the required-versus-actual number, so an agent can act on it without
// parsing prose.
namespace tube {

// One bend of one member, as CurveFeature::Bend records it. Repeated here so
// this module stays what its header promises: data and arithmetic, over no
// geometry and no document.
struct Bend {
  double angleDeg = 0.0;
  double radius = 0.0;         // the centreline's, i.e. the former's
  double straightBefore = 0.0; // free length before it, mm
  double rollDeg = 0.0;
};

struct Member {
  std::string name;
  std::vector<Bend> bends;
  double length = 0.0; // the developed length of the whole centreline
  // The stock THIS member is made of. A frame is not one size: a Formula
  // Student chassis is 25.4 x 2.4 in its hoops and 19.05 in its tertiary
  // members, and what may be bent on a given former depends on which.
  double outerDiameter = 0.0;
  double wallThickness = 0.0;
};

struct Violation {
  std::string kind; // "bend_below_minimum_radius", "radius_not_in_die_set", …
  std::string message;
  std::string member;
  int bend = 0;          // 1-based index into the member's bends; 0 = none
  double actual = 0.0;   // mm, or a fraction for thinning
  double required = 0.0; // mm, or a fraction for thinning
};

struct CheckResult {
  std::vector<Violation> violations;
  // What was actually looked at, so "no violations" cannot be read as "all
  // clear" on something nobody checked.
  int membersChecked = 0;
  int bendsChecked = 0;
  // Checks this build cannot run and why.
  std::vector<std::string> notChecked;
};

// Checks `members` against `rules`. A member with no bends is a straight cut
// and is checked for nothing, but counts as looked at.
[[nodiscard]] CheckResult Check(const std::vector<Member>& members,
                                const TubeRules& rules);

} // namespace tube
