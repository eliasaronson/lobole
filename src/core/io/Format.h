#pragma once

#include <string>

// Shared numeric formatting for measured values (drawings D3, and the sketch/
// measure/study readouts that migrate onto it). Centralizes the `%.3g`-style
// formatting that was scattered ad hoc across dimension text and editors.

namespace core {

// A length in millimetres as compact display text: up to 3 significant figures,
// trailing zeros trimmed (e.g. 40.0 -> "40", 8.5 -> "8.5", 12.345 -> "12.3").
std::string FormatLength(double mm);

// An angle in radians as degrees with one decimal and a degree sign
// (e.g. "90°", "45.5°"). UTF-8 output.
std::string FormatAngle(double radians);

} // namespace core
