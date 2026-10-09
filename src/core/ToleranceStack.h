// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Document.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Worst case along a STATED chain of tolerances
// (docs/tolerances/README.md#checks).
//
// The chain is stated, not inferred, and deliberately so. A mate names the
// two faces that TOUCH, but a stack alternates a contact and a length inside
// a part, and nothing says which of a part's distances lies along the axis.
// Deriving it would work only when someone happened to tolerance exactly the
// right pairs.
//
// So: the person chooses the terms, the tool does the addition, and the
// result says outright what it does not account for. Which is a great deal —
// most of all, correlation. Two lengths driven by one variable CANCEL rather
// than add, and this cannot tell: a tolerance's nominal is measured, and
// there is no path from a measured distance back to the variable that drove
// it. Anyone reading a stack from this must know its terms are independent.
namespace tolstack {

struct Term {
  uint64_t toleranceId = 0;
  bool subtract = false; // a gap is A - B - C, not a sum of everything
};

struct TermResult {
  uint64_t toleranceId = 0;
  std::string what; // the element name(s) the tolerance is on
  bool subtract = false;
  double nominal = 0.0; // measured, mm
  double lower = 0.0;   // signed deviation, mm
  double upper = 0.0;
};

struct Result {
  std::vector<TermResult> terms;
  double nominal = 0.0; // the signed sum of the nominals
  double min = 0.0;     // worst case low
  double max = 0.0;     // worst case high
};

// nullopt with a refusal in `error` when a term does not resolve, an id is
// unknown, or fewer than two terms are given — one term is a tolerance, not
// a stack, and answering would dress a lookup up as an analysis.
[[nodiscard]] std::optional<Result>
Sum(const Document& doc, const std::vector<Term>& terms, std::string* error);

} // namespace tolstack
