// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cmath>
#include <random>

// Gaussian noise that is the same sample on every standard library.
// std::mt19937 is specified to the bit; std::normal_distribution is not, so
// a fixture built on it is one scan under libstdc++ and another under
// libc++, and a check tuned on the first met the second on macOS (a 0.5 mm
// round read 3.2 sigma off instead of 2.6). This is libstdc++'s algorithm
// (Marsaglia's polar method on two 32-bit draws a variate), so the fixtures
// written against std::normal_distribution there are unchanged.
namespace testnoise {

class Gauss {
public:
  explicit Gauss(double sigma) : m_sigma(sigma) {}
  double operator()(std::mt19937& rng) {
    if (m_have) {
      m_have = false;
      return m_saved * m_sigma;
    }
    double x, y, r2;
    do {
      x = 2.0 * Canonical(rng) - 1.0;
      y = 2.0 * Canonical(rng) - 1.0;
      r2 = x * x + y * y;
    } while (r2 > 1.0 || r2 == 0.0);
    const double mult = std::sqrt(-2.0 * std::log(r2) / r2);
    m_saved = x * mult;
    m_have = true;
    return y * mult * m_sigma;
  }

private:
  static double Canonical(std::mt19937& rng) {
    const double lo = rng(), hi = rng();
    const double r = (lo + hi * 4294967296.0) / 18446744073709551616.0;
    return r >= 1.0 ? std::nextafter(1.0, 0.0) : r;
  }
  double m_sigma;
  double m_saved = 0.0;
  bool m_have = false;
};

} // namespace testnoise
