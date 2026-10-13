// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"

#include <cstdint>
#include <string>

// Folds back up the bends an Unfold laid flat
// (docs/sheetmetal/README.md#how-the-unfolder-works), carrying every
// through-thickness cut made across a crease onto the bend. See UnfoldFeature.h
// for the pair.
class RefoldFeature : public Feature {
public:
  RefoldFeature(const std::string& name, uint64_t unfoldId);

  std::string TypeName() const override { return "Refold"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<RefoldFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<RefoldFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(unfoldId);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return unfoldId == 0 ? "pick the Unfold to fold back up" : std::string{};
  }

  // The Unfold whose bends this folds back. Nothing else: the creases carry
  // everything a fold needs, so there is no second copy of it to disagree.
  uint64_t unfoldId = 0;
};
