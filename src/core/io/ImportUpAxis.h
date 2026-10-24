// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

// Which axis of an imported file is up, and how the import turns it onto ours
// (docs/interchange/README.md#up-axis). Our world is Z-up
// (docs/conventions.md#world-axes); SolidWorks, among others, writes Y up, so
// its parts arrived lying on their backs.
namespace interchange {

// The axis OF THE FILE that is up. `Auto` decides from the file's header,
// `None` leaves the file as it is.
enum class UpAxis { Auto, None, PosX, NegX, PosY, NegY, NegZ };

// "auto", "none", "+x", "-x", "+y", "-y", "-z".
[[nodiscard]] const char* ToString(UpAxis axis);
// False for anything else: refused naming AcceptedUpAxes(), never read as a
// default.
[[nodiscard]] bool UpAxisFromString(std::string_view s, UpAxis& out);
// "auto, none, +x, -x, +y, -y, -z", for a refusal's text.
[[nodiscard]] std::string AcceptedUpAxes();

// What was asked, what was applied and why.
struct UpDecision {
  UpAxis requested = UpAxis::Auto;
  UpAxis applied = UpAxis::None; // never Auto
  double turnDeg = 0.0;          // about world Z, after the axis
  std::string because;           // one sentence: how the import knows
  std::string system;            // the writer the header names ("" = none)
};

// Resolves `requested` against what the file's header says. An explicit axis
// is applied as asked; `Auto` reads the table: SolidWorks named in any header
// field (case-blind) is +y, Loböle is none, and any other writer is none
// because it is not one known to write Y up. A format without a header (STL,
// BREP, OBJ) is none, and says so.
[[nodiscard]] UpDecision DecideUpAxis(UpAxis requested,
                                      const CADKernel::FileHeader& header,
                                      double turnDeg = 0.0);

// The rotation that stands `applied` up on our +Z, then turns `turnDeg` about
// world Z. Each axis has one fixed turn chosen so the writer's front lands on
// ours (-Y): +y is +90 deg about X, -y -90 about X, +x -90 about Y, -x +90
// about Y, -z 180 about X.
[[nodiscard]] glm::dmat4 UpAxisRotation(UpAxis applied, double turnDeg);

// {requested, applied, turn_deg, because, system}, what an import's reply
// carries.
[[nodiscard]] nlohmann::json ToJson(const UpDecision& d);

} // namespace interchange
