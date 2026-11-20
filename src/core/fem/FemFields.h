// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The fields a solved result is read by
// (docs/simulation/README.md#results-display): one value per node, derived
// from what the result keeps -- the displacement, the stress tensor, the
// plastic strain, the temperature -- and never stored themselves. The
// viewport's contour, its legend and the VTU export read them here, so a
// number in one is the number in the others.

#include "core/fem/FemSolver.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace fem {

enum class ResultField {
  VonMises,
  Displacement,
  PrincipalMax, // the largest principal stress (tension positive)
  PrincipalMid,
  PrincipalMin,     // the smallest: the hardest compression
  AcrossLayers,     // n . sigma . n along the build direction
  InterlayerFactor, // interlayer strength / the tension across the layers
  FactorOfSafety,   // yield / von Mises
  PlasticStrain,    // equivalent plastic strain
  ContactPressure,
  Temperature,
  HeatFlux
};

struct ResultFieldSpec {
  ResultField field;
  const char* key;   // the RPC's name for it
  const char* title; // the legend's
  const char* unit;  // UTF-8; "" for a ratio
  // A factor reads the other way round: its LOW end is the one that
  // matters, it has no upper bound where the stress is zero, and its
  // legend runs from the least value up to a few times it.
  bool factor;
  // A signed field (a principal stress, the stress across layers) spans
  // from its least value, not from zero.
  bool isSigned;
};
const std::vector<ResultFieldSpec>& ResultFieldSpecs();
const ResultFieldSpec& SpecOf(ResultField f);
// nullopt for a name that is not a field's key: the caller refuses, naming
// ResultFieldKeys().
[[nodiscard]] std::optional<ResultField>
ResultFieldFromKey(const std::string& key);
std::string ResultFieldKeys(); // "von_mises | displacement | ..."

// The three principal stresses of `s`, largest first (MPa, tension
// positive), in closed form.
std::array<double, 3> Principals(const Stress& s);

// What a field needs beside the result: the build direction (AcrossLayers,
// InterlayerFactor) and the body's allowables (MPa; 0 = the material has
// none).
struct FieldInputs {
  glm::dvec3 layerNormal{0.0};
  double yieldMPa = 0.0;
  double interlayerMPa = 0.0;
  // The result is of a bonded group, several materials in one mesh: its
  // factor of safety is only the solve's own (FemResults::
  // nodeFactorOfSafety), never one yield over every node.
  bool severalMaterials = false;
};
// fem::kUnboundedFactor (FemSolver.h) is what an unbounded factor reads as.

// One value per node of `r`. Empty, with *why saying what is missing, when
// the result does not hold the field (a modal result has no stress, a
// result with no layer direction no stress across layers, a material with
// no yield no factor of safety).
std::vector<double> NodalField(const FemResults& r, ResultField f,
                               const FieldInputs& in, std::string* why);

// The least and largest finite value of a field (unbounded factors left
// out) and the nodes they are at; nodes -1 on an empty field.
struct FieldRange {
  double lo = 0.0, hi = 0.0;
  int loNode = -1, hiNode = -1;
};
FieldRange RangeOf(const std::vector<double>& field);

} // namespace fem
