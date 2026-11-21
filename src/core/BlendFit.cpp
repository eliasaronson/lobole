// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/BlendFit.h"

#include "core/Measure.h"
#include "core/MeasureBlend.h"
#include "core/features/FilletFeature.h" // EdgeBlendOp

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <numbers>

namespace fit {

namespace {

std::atomic<std::uint64_t> g_built{0};

std::string NameOrIndex(const std::string* n, const char* kind, int index) {
  return n != nullptr ? *n : std::string(kind) + std::to_string(index);
}

} // namespace

std::string Mm(double v) {
  std::string s = std::format("{:.6g}", v);
  return s;
}

double RoundDown3(double v) {
  if (!(v > 0.0) || !std::isfinite(v))
    return 0.0;
  const int e = static_cast<int>(std::floor(std::log10(v)));
  const int k = 2 - e; // decimals kept
  // Scaled by an exact power of ten, so the division gives the double
  // nearest the decimal: 749 / 100 prints 7.49.
  if (k >= 0) {
    const double p = std::pow(10.0, k);
    return std::floor(v * p * (1.0 + 1e-12)) / p;
  }
  const double p = std::pow(10.0, -k);
  return std::floor(v / p * (1.0 + 1e-12)) * p;
}

std::optional<Bound> BlendBound(const Body& body, const std::vector<int>& edges,
                                EdgeBlendOp op) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  Measure::BlendProbe probe(body);
  const char* blends = chamfer ? "chamfers" : "rounds";
  const char* aBlend = chamfer ? "a chamfer" : "a round";
  // How much of each face a blend of size 1 takes: its distance for a
  // chamfer, r / tan(theta/2) of a round where the faces meet at theta.
  const auto setbackPerMm = [&](int edge) -> std::optional<double> {
    if (chamfer)
      return 1.0;
    const auto theta = probe.Dihedral(edge);
    if (!theta)
      return std::nullopt;
    const double turn = std::abs(std::numbers::pi - *theta);
    if (turn < 1e-4)
      return std::nullopt; // tangent: nothing to round
    return std::tan(0.5 * turn);
  };
  const auto faceName = [&](int f) {
    return NameOrIndex(body.elementMap.NameOf(f), "face", f);
  };
  const auto edgeName = [&](int e) {
    return NameOrIndex(body.elementMap.EdgeNameOf(e), "edge", e);
  };
  std::optional<Bound> least;
  const auto offer = [&](double size, std::string why, std::string element) {
    if (!(size > 0.0) || !std::isfinite(size))
      return;
    if (!least || size < least->size)
      least = Bound{size, std::move(why), std::move(element)};
  };
  for (const int e : edges) {
    const auto faces = probe.Faces(e);
    const auto k = setbackPerMm(e);
    if (!faces || !k)
      continue;
    const bool convex =
        chamfer ? true : probe.Dihedral(e).value_or(0.0) < std::numbers::pi;
    for (const int f : *faces) {
      if (const auto w = probe.Width(e, f)) {
        const bool farPicked =
            std::find(edges.begin(), edges.end(), w->endsAt) != edges.end();
        const auto info = w->endsAt == e && convex
                              ? Measure::DescribeEdgeAt(body, e)
                              : std::nullopt;
        if (info && info->hasRadius) {
          // Across a disc to the same rim: the circle bounds it.
          offer(w->width / (2.0 * *k),
                "edge '" + edgeName(e) + "' is a circle of radius " +
                    Mm(info->radius) + " mm",
                edgeName(e));
        } else if (farPicked) {
          const double kFar =
              w->endsAt == e ? *k : setbackPerMm(w->endsAt).value_or(0.0);
          const double size = w->width / (*k + kFar);
          offer(size,
                "face '" + faceName(f) + "' is " + Mm(w->width) +
                    " mm wide between the two picked edges, so the " + blends +
                    " meet at " + Mm(size) + " mm",
                faceName(f));
        } else {
          const double size = w->width / *k;
          offer(size,
                "face '" + faceName(f) + "' runs " + Mm(w->width) +
                    " mm from edge '" + edgeName(e) + "', so " + aBlend +
                    " of " + Mm(size) + " mm takes all of it",
                faceName(f));
        }
      }
      if (!chamfer)
        if (const auto rc = probe.ConcaveRadius(e, f))
          offer(*rc,
                "face '" + faceName(f) + "' curves with a radius of " +
                    Mm(*rc) + " mm toward the round",
                faceName(f));
    }
  }
  return least;
}

Answer LargestThatBuilds(double asked, const std::optional<Bound>& bound,
                         const std::function<bool(double)>& builds,
                         const std::function<bool()>& stop) {
  Answer a;
  a.asked = asked;
  a.fails = asked;
  double lo = 0.0; // the largest that built; 0 = none yet
  const auto tryOne = [&](double c) {
    g_built.fetch_add(1, std::memory_order_relaxed);
    if (builds(c)) {
      lo = c;
      a.fits = c;
      return true;
    }
    a.fails = c;
    return false;
  };
  // Just under the bound: rounded down from a thousandth below it, so two
  // rounds that would meet exactly leave a sliver between them.
  if (bound && bound->size < asked) {
    const double c = RoundDown3(bound->size * (1.0 - 1e-3));
    if (c > 0.0 && c < a.fails) {
      if (stop()) {
        a.stopped = true;
        return a;
      }
      if (tryOne(c)) {
        a.bound = bound; // nothing above the bound fits
        return a;
      }
    }
  }
  for (int step = 0; step < kSearchSteps; step++) {
    // Down by a factor while nothing has built, so eight steps reach a
    // thousandth of the size; then halving the bracket by ratio, so the
    // answer is as close to the limit in proportion whatever its size.
    const double c =
        RoundDown3(lo > 0.0 ? std::sqrt(lo * a.fails) : 0.4 * a.fails);
    if (c <= lo || c >= a.fails)
      break; // converged at three digits
    if (stop()) {
      a.stopped = true;
      return a;
    }
    tryOne(c);
  }
  return a;
}

std::uint64_t CandidatesBuilt() { return g_built.load(); }

std::optional<Answer> Memo::For(const CADShape& input,
                                const std::vector<int>& edgeIndices,
                                EdgeBlendOp blendOp, double asked) const {
  if (op != static_cast<int>(blendOp) || edges != edgeIndices ||
      answer.stopped || body.GetShape().IsNull() ||
      !body.GetShape().IsEqual(input.GetShape()) || asked < answer.fails)
    return std::nullopt;
  Answer a = answer;
  a.asked = asked;
  return a;
}

std::string Sentence(EdgeBlendOp op, const Answer& a, const std::string& places,
                     const std::string& cause) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  const std::string what = chamfer ? "chamfer" : "fillet";
  const std::string param = chamfer ? "distance" : "radius";
  std::string s = what + " " + param + " " + Mm(a.asked) + " mm does not fit: ";
  if (a.stopped) {
    if (a.fits)
      s += "a " + param + " of " + Mm(*a.fits) + " mm builds and " +
           Mm(a.fails) + " mm does not (the search was stopped)";
    else
      s += Mm(a.fails) + " mm does not build either (the search was stopped)";
    return s + places;
  }
  if (a.fits) {
    s += "the largest " + param + " that fits is " + Mm(*a.fits) + " mm";
    if (a.bound)
      s += " (" + a.bound->why + ")";
    return s + places;
  }
  s += "no " + param + " builds on these edges, down to " + Mm(a.fails) + " mm";
  if (!cause.empty())
    s += " (" + cause + ")";
  return s + "; fewer edges in one " + what + " usually builds";
}

FitHint HintOf(const Answer& a) {
  FitHint h;
  h.param = "radius";
  h.askedMm = a.asked;
  h.fitsMm = a.fits;
  h.failsMm = a.fails;
  h.basis = "built";
  if (a.bound)
    h.element = a.bound->element;
  h.stopped = a.stopped;
  return h;
}

} // namespace fit
