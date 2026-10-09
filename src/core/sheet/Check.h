// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"
#include "core/SheetMetal.h"

#include <string>
#include <vector>

// Manufacturability as a query (docs/sheetmetal/README.md#checks-and-data).
//
// Every one of these is valid B-Rep and comes back from the shop as a
// question, which is the whole argument for checking it where the geometry
// is: a hole 1.2 mm from a bend, a flange that will not reach the die, a bend
// line that stops in the middle of a panel with nothing for the metal to move
// into, a radius the material stopped being able to take when somebody
// changed the gauge.
//
// Structured like check_geometry and check_fasteners: every finding names the
// element and the required-versus-actual number, so an agent can act on it
// without parsing prose.
namespace sheet {

struct Violation {
  std::string kind; // "bend_below_minimum_radius", "hole_too_close_to_bend", …
  std::string message;
  uint64_t bend = 0;     // index into the body's bends, 1-based; 0 = none
  ElementRef element;    // the hole, face or edge at fault, when there is one
  double actual = 0.0;   // mm
  double required = 0.0; // mm
};

struct CheckResult {
  std::vector<Violation> violations;
  // What was actually looked at, so "no violations" cannot be read as "all
  // clear" on something nobody checked.
  int bendsChecked = 0;
  int holesChecked = 0;
  // Checks this build cannot run and why — a brake's length and its die set
  // are properties of a SHOP, and nothing here knows one yet.
  std::vector<std::string> notChecked;
};

// Checks `body` against `rules`. A body with no bends is checked for nothing
// but says so.
[[nodiscard]] CheckResult Check(const Body& body, const SheetMetalRules& rules);

} // namespace sheet
