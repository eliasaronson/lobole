// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ToleranceStack.h"

#include "core/FitStandards.h"
#include "core/Measure.h"

#include <cstdio>

namespace tolstack {
namespace {

void Fail(std::string* error, std::string why) {
  if (error)
    *error = std::move(why);
}

// A tolerance's nominal is MEASURED, never stored — the same rule the rest of
// this carries: what the kernel built is what gets made.
std::optional<double> NominalOf(const Document& doc, const Tolerance& t) {
  const std::vector<Body>& bodies = doc.GetBodies();
  if (t.IsDistance()) {
    if (const auto d = Measure::DistanceDetailed(bodies, t.ref, t.ref2))
      return d->distance;
    return std::nullopt;
  }
  if (const auto el = Measure::Describe(bodies, t.ref))
    if (el->hasRadius)
      return el->radius * 2.0;
  return std::nullopt;
}

} // namespace

std::optional<Result> Sum(const Document& doc, const std::vector<Term>& terms,
                          std::string* error) {
  if (error)
    error->clear();
  if (terms.size() < 2) {
    Fail(error, "a stack needs at least two terms — one tolerance is a "
                "tolerance, and summing it would dress a lookup up as an "
                "analysis");
    return std::nullopt;
  }
  Result out;
  for (const Term& term : terms) {
    const Tolerance* t = doc.FindTolerance(term.toleranceId);
    if (!t) {
      Fail(error, "no tolerance with id " + std::to_string(term.toleranceId) +
                      " (list_tolerances shows them)");
      return std::nullopt;
    }
    const auto nominal = NominalOf(doc, *t);
    if (!nominal) {
      Fail(error, "tolerance " + std::to_string(term.toleranceId) +
                      " no longer resolves to geometry, so it has no nominal "
                      "to add");
      return std::nullopt;
    }
    TermResult r;
    r.toleranceId = t->id;
    r.subtract = term.subtract;
    r.nominal = *nominal;
    r.what = t->ref.name;
    if (t->IsDistance())
      r.what += " -> " + t->ref2.name;
    if (t->isoClass.empty()) {
      r.lower = t->lower;
      r.upper = t->upper;
    } else {
      std::string why;
      const auto band = fits::Limits(*nominal, t->isoClass, &why);
      if (!band) {
        Fail(error, "tolerance " + std::to_string(term.toleranceId) + " (" +
                        t->isoClass + "): " + why);
        return std::nullopt;
      }
      r.lower = band->lower;
      r.upper = band->upper;
    }

    // Worst case, and the signs are the whole of it: a term that SUBTRACTS
    // contributes its UPPER deviation to the minimum and its lower to the
    // maximum. Getting this backwards produces a stack that is wrong by twice
    // the band and looks entirely reasonable.
    if (term.subtract) {
      out.nominal -= r.nominal;
      out.min -= r.nominal + r.upper;
      out.max -= r.nominal + r.lower;
    } else {
      out.nominal += r.nominal;
      out.min += r.nominal + r.lower;
      out.max += r.nominal + r.upper;
    }
    out.terms.push_back(std::move(r));
  }
  return out;
}

} // namespace tolstack
