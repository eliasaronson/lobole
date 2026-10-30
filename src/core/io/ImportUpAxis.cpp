// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/ImportUpAxis.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cmath>
#include <numbers>
#include <utility>

namespace interchange {

namespace {

constexpr std::array<std::pair<UpAxis, const char*>, 7> kNames = {{
    {UpAxis::Auto, "auto"},
    {UpAxis::None, "none"},
    {UpAxis::PosX, "+x"},
    {UpAxis::NegX, "-x"},
    {UpAxis::PosY, "+y"},
    {UpAxis::NegY, "-y"},
    {UpAxis::NegZ, "-z"},
}};

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A writer the table knows, by a case-blind fragment of its name. A row is
// added only with a real file's header in its test (test_cadkernel.cpp).
struct Writer {
  const char* fragment; // lower case
  const char* name;
  UpAxis up;
};
// "loböle" is matched in UTF-8 and in the ASCII spelling, since a STEP
// header is ISO 10303-21 text and a writer may fold the ö either way.
constexpr std::array<Writer, 4> kWriters = {{
    {"solidworks", "SolidWorks", UpAxis::PosY},
    {"lob\xc3\xb6le", "Lob\xc3\xb6le", UpAxis::None},
    {"lobole", "Lob\xc3\xb6le", UpAxis::None},
    {"lob\\x\\f6le", "Lob\xc3\xb6le", UpAxis::None}, // the \X\ escape
}};

} // namespace

const char* ToString(UpAxis axis) {
  for (const auto& [a, n] : kNames)
    if (a == axis)
      return n;
  return "none";
}

bool UpAxisFromString(std::string_view s, UpAxis& out) {
  for (const auto& [a, n] : kNames)
    if (s == n) {
      out = a;
      return true;
    }
  return false;
}

std::string AcceptedUpAxes() {
  std::string out;
  for (const auto& [a, n] : kNames)
    out += (out.empty() ? "" : ", ") + std::string(n);
  return out;
}

UpDecision DecideUpAxis(UpAxis requested, const CADKernel::FileHeader& header,
                        double turnDeg) {
  UpDecision d;
  d.requested = requested;
  d.turnDeg = turnDeg;
  // The writer a person would name: the first field that says anything.
  for (const auto& [label, value] : header.fields)
    if (!value.empty()) {
      d.system = value;
      break;
    }
  if (requested != UpAxis::Auto) {
    d.applied = requested;
    d.because = std::string("asked for ") + ToString(requested);
    return d;
  }
  // glTF is Y up by its specification, whoever wrote it (our own export
  // turns Z up to it), so the format decides before any writer does.
  if (header.format == "glTF") {
    d.applied = UpAxis::PosY;
    d.because = "glTF is Y up by its specification";
    return d;
  }
  d.applied = UpAxis::None;
  if (header.fields.empty()) {
    const std::string fmt = header.format.empty() ? "this file" : header.format;
    const bool an = fmt == "OBJ" || fmt == "STL" || fmt == "IGES";
    d.because = std::string(an ? "an " : "a ") + fmt +
                " has no header naming its writer, so nothing says which "
                "axis is up";
    if (fmt == "OBJ")
      d.because += " (an OBJ is usually Y up: up_axis +y stands one up)";
    return d;
  }
  for (const auto& [label, value] : header.fields) {
    const std::string lower = Lower(value);
    for (const Writer& w : kWriters)
      if (lower.find(w.fragment) != std::string::npos) {
        d.applied = w.up;
        d.system = value;
        // "writes Y up": the axis by name, its sign said by the table.
        const char* axis = w.up == UpAxis::PosX || w.up == UpAxis::NegX   ? "X"
                           : w.up == UpAxis::PosY || w.up == UpAxis::NegY ? "Y"
                                                                          : "Z";
        d.because =
            label + " '" + value + "': " + w.name + " writes " + axis + " up";
        return d;
      }
  }
  if (d.system.empty()) {
    d.because = "the " + header.format +
                " header names no writer, so nothing says which axis is up";
    return d;
  }
  for (const auto& [label, value] : header.fields)
    if (!value.empty()) {
      d.because = label + " '" + value +
                  "': not a writer known to write Y up, so the file is "
                  "left as it is";
      break;
    }
  return d;
}

glm::dmat4 UpAxisRotation(UpAxis applied, double turnDeg) {
  constexpr double kQuarter = std::numbers::pi / 2.0;
  const glm::dvec3 X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1);
  glm::dmat4 up(1.0);
  switch (applied) {
  case UpAxis::Auto:
  case UpAxis::None:
    break;
  case UpAxis::PosY:
    up = glm::rotate(glm::dmat4(1.0), kQuarter, X);
    break;
  case UpAxis::NegY:
    up = glm::rotate(glm::dmat4(1.0), -kQuarter, X);
    break;
  case UpAxis::PosX:
    up = glm::rotate(glm::dmat4(1.0), -kQuarter, Y);
    break;
  case UpAxis::NegX:
    up = glm::rotate(glm::dmat4(1.0), kQuarter, Y);
    break;
  case UpAxis::NegZ:
    up = glm::rotate(glm::dmat4(1.0), 2.0 * kQuarter, X);
    break;
  }
  // glm's rotate leaves 6e-17 where a quarter turn should put 0; snap the
  // entries so an axis lands exactly on an axis and a box stays a box.
  for (int c = 0; c < 3; c++)
    for (int r = 0; r < 3; r++)
      up[c][r] = std::round(up[c][r]);
  glm::dmat4 turn(1.0);
  if (turnDeg != 0.0)
    turn = glm::rotate(glm::dmat4(1.0), glm::radians(turnDeg), Z);
  return turn * up;
}

nlohmann::json ToJson(const UpDecision& d) {
  return {{"requested", ToString(d.requested)},
          {"applied", ToString(d.applied)},
          {"turn_deg", d.turnDeg},
          {"because", d.because},
          {"system", d.system}};
}

} // namespace interchange
