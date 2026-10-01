#pragma once

#include "core/Units.h"

#include <string>

// Shared numeric formatting for measured values (drawings D3, and the sketch/
// measure/study readouts that migrate onto it). Centralizes the `%.3g`-style
// formatting that was scattered ad hoc across dimension text and editors.
//
// These take the DOCUMENT's unit system, and did not until 2026-09-10: a
// drawing dimensioned in millimetres whatever the document said, so an inch
// part — which set_units fully supports — produced a sheet of bare numbers
// that were secretly mm, with no UNITS row in the title block to contradict
// them. That is the exact failure core/Units.h exists to prevent ("a part
// authored in inches has to display in inches for whoever opens it"), in the
// one place a part leaves the app for a machine shop.
//
// Lengths carry NO suffix on purpose. Drafting practice states the unit once,
// in the title block, rather than on every dimension — so DrawingExport
// writes the row and these write the number. That is also why they are not
// units::FormatLength, which always appends one.

namespace core {

// A length in millimetres as display text in `sys`'s length unit: a fixed
// number of DECIMALS for the unit (mm 2, cm 3, m 5, in 3), trailing zeros
// trimmed (40.0 -> "40", 8.5 -> "8.5", 37.335 -> "37.34"), no unit suffix.
// Decimals, not significant figures: three of those printed a 37.34 hole
// spacing as 37.3 and would have printed a 1051 mm frame as "1.05e+03".
std::string FormatLength(double mm, const units::System& sys);

// A tolerance's deviation: one decimal finer than a length (mm 3, in 4), so
// a K7's +0.007 is not rounded to 0.01. Unsigned; the caller signs it.
std::string FormatDeviation(double mm, const units::System& sys);

// An angle in RADIANS as display text with the unit's symbol: degrees to one
// decimal ("90°", "45.5°"), or radians to three ("1.571 rad") when the
// document says so. UTF-8 output. Unlike a length, an angle keeps its mark —
// a bare "90" on a drawing is ambiguous with a length, and the degree sign is
// drafting practice rather than a redundant unit label.
std::string FormatAngle(double radians, const units::System& sys);

// What a sheet of bare numbers is IN, for a drawing's title block: "mm",
// "in", and " / rad" appended when the angles are not the degrees their
// degree signs already declare. One definition, because the sheet on screen
// and the sheet exported to a shop must not disagree about it.
std::string UnitsLabel(const units::System& sys);

} // namespace core
