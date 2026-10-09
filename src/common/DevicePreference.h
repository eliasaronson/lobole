// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <span>
#include <string>

// Which GPU to use, as a pure decision so both device-picking sites agree and
// the rule is testable without a Vulkan instance. There are two sites: the
// renderer builds its device once at startup (main.cpp) and the FEM compute
// context builds one per run (femgpu/ComputeContext.cpp) — which is why the
// same preference is "restart" for the viewport and immediate for simulation.
//
// Header-only on purpose: the FEM compute library does not link
// designer_core, and one shared rule beats two that drift.
namespace devicepref {

namespace detail {
inline std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}
} // namespace detail

// Index into `names` of the device to use, or -1 when nothing is eligible.
//
// A non-empty `preferred` wins whenever it matches — case-insensitive, and a
// SUBSTRING, so a user can type "NVIDIA" instead of the full marketing name.
// Devices scored below zero are ineligible (the compute path uses this for
// "no float32 atomics"), and a preference that names one of them is ignored:
// a setting must not be able to make the app pick a device that cannot work.
// With no preference (or no match) the highest score wins, first on a tie.
inline int Choose(std::span<const std::string> names,
                  std::span<const int> scores, const std::string& preferred) {
  const size_t n = std::min(names.size(), scores.size());
  if (!preferred.empty()) {
    const std::string want = detail::Lower(preferred);
    for (size_t i = 0; i < n; i++)
      if (scores[i] >= 0 &&
          detail::Lower(names[i]).find(want) != std::string::npos)
        return static_cast<int>(i);
    // Fall through: the named device is gone (a laptop undocked from its
    // eGPU, a driver removed) or cannot do the job. Running on the automatic
    // choice beats refusing to start over a preference.
  }
  int best = -1;
  for (size_t i = 0; i < n; i++)
    if (scores[i] >= 0 && (best < 0 || scores[i] > scores[best]))
      best = static_cast<int>(i);
  return best;
}

} // namespace devicepref
