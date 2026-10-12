// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// check_print and the Print process section's Check print
// (docs/tolerances/README.md#checking-a-print): one helper that derives the
// limits from the part's print process, so the RPC and the button cannot
// disagree about what a part is checked against.

#include "core/PrintCheck.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

class Document;

namespace ui {

// What a caller states; anything left unset comes from the part's print.
struct PrintCheckAsk {
  std::optional<glm::dvec3> up;
  std::optional<double> maxOverhangDeg;
  std::optional<double> overhangAllowanceMm2;
  std::optional<double> maxBridgeMm;
  std::optional<double> minGapMm; // the gap a side
  std::optional<double> stlDeflectionMm;
};

struct PrintCheckRun {
  printcheck::Report report;
  printcheck::Options options;
  std::vector<int> bodyIndex; // report body i is part.GetBodies()[bodyIndex[i]]
  std::string process;        // printfit::Label, or "" for an unprinted part
  // Where each limit came from: "asked", "process" (the process's own
  // number), "profile" / "gap" (the print's chord deviation), or "" when
  // the item was skipped for want of one.
  std::string overhangFrom, bridgeFrom, gapFrom, deflectionFrom;
  std::string overhangSource, bridgeSource; // the process's, when used
};

// The check over the part's non-reference bodies (only those created by a
// feature in `only`, when given). An unprinted part with no stated limit
// skips that item and says what would run it. Throws std::runtime_error
// when there is nothing to check.
[[nodiscard]] PrintCheckRun RunPrintCheck(const Document& part,
                                          const std::set<uint64_t>* only,
                                          const PrintCheckAsk& ask);

} // namespace ui
