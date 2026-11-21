// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The blend measures of Measure.h (DihedralAngleAt, WidthAcrossEdgeAt,
// ConcaveRadiusAt) asked of many edges of one body: the topology maps and
// each face's boundary are built once, not once a question. The size
// search's bounds (core/BlendFit.cpp) ask a hundred rims of one plate, whose
// top face has every rim as its boundary.

#include "core/Measure.h"

#include <memory>

namespace Measure {

class BlendProbe {
public:
  explicit BlendProbe(const Body& body);
  ~BlendProbe();
  BlendProbe(const BlendProbe&) = delete;
  BlendProbe& operator=(const BlendProbe&) = delete;

  [[nodiscard]] std::optional<std::array<int, 2>> Faces(int edge);
  [[nodiscard]] std::optional<double> Dihedral(int edge);
  [[nodiscard]] std::optional<WidthAcross> Width(int edge, int face);
  [[nodiscard]] std::optional<double> ConcaveRadius(int edge, int face);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace Measure
