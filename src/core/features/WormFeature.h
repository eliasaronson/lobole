// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"
#include "core/WormGear.h"

#include <nlohmann/json.hpp>

// A worm and its wheel (docs/modeling/worm-gears.md), each cut from a
// cylindrical BLANK the way Thread cuts a boss: pick the blank's outside face
// and the feature cuts the teeth into its body. The blank's size is the
// gear's size, so nothing needs to be typed that the model already says: a
// worm's tip is the face, a wheel's rim is the face and its face width the
// face's length.
//
// Two features rather than one with a kind: they take different numbers (the
// wheel needs its tooth count and the mating worm's diameter; the worm needs
// a backlash and a length), a worm is useful alone (a lead screw, a feed
// worm), and each lives in its own part of an assembly.

// The worm: a thread with a gear's axial profile (ZA, straight-sided in the
// axial section), cut into a cylindrical boss by HelicalSweep.
class WormFeature : public Feature {
public:
  WormFeature(const std::string& name, ElementRef face);

  std::string TypeName() const override { return "Worm"; }
  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<WormFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<WormFeature>(executed);
  }
  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    VisitRefs(face, visit);
  }
  void VisitExprs(const ExprVisitor& visit) override {
    visit(moduleExpr, module);
  }
  [[nodiscard]] std::string WhyIncomplete() const override {
    return face.name.empty() ? "pick the worm blank's cylindrical face"
                             : std::string{};
  }

  ElementRef face; // the blank's outside: the worm's tip cylinder
  // Axial module, mm. 0 = derived from the blank at the usual diameter
  // quotient: tip diameter / (10 + 2).
  double module = 0.0;
  std::string moduleExpr;
  int starts = 1;
  double pressureDeg = 20.0;
  bool rightHanded = true; // JSON "hand": "right" | "left"
  // Axial backlash, mm: the tooth is thinned by half of it per flank, the
  // clearance a printed pair runs at (0.2-0.3 mm).
  double backlash = 0.0;
  double length = 0.0; // the threaded length from the face's start; 0 = full

  // Session-only, rebuilt by Execute (never serialized): what it built.
  struct Report {
    bool valid = false;
    worm::PairSpec spec;
    worm::Sizes sizes;
    double length = 0.0;
  };
  Report report;
  [[nodiscard]] nlohmann::json ReportJson() const;
};

// The wheel: hobbed. The mating worm (module, starts, pitch diameter,
// pressure angle, hand) rolled through the mesh around the blank, and the
// envelope it cuts taken as the tooth spaces, with the throat and the tip
// (worm::WheelCutTools).
class WormWheelFeature : public Feature {
public:
  WormWheelFeature(const std::string& name, ElementRef face);

  std::string TypeName() const override { return "WormWheel"; }
  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<WormWheelFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<WormWheelFeature>(executed);
  }
  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    VisitRefs(face, visit);
  }
  void VisitExprs(const ExprVisitor& visit) override {
    visit(moduleExpr, module);
  }
  [[nodiscard]] std::string WhyIncomplete() const override {
    return face.name.empty() ? "pick the wheel blank's cylindrical face"
                             : std::string{};
  }

  ElementRef face; // the blank's rim; its length is the face width
  // mm. 0 = derived from the blank: rim diameter / (teeth + 3).
  double module = 0.0;
  std::string moduleExpr;
  int teeth = 40;
  // The mating worm's.
  int starts = 1;
  double wormDiameter = 0.0; // pitch diameter d1, mm; 0 = 10 modules
  double pressureDeg = 20.0;
  bool rightHanded = true; // JSON "hand": "right" | "left"

  struct Report {
    bool valid = false;
    worm::PairSpec spec;
    worm::Sizes sizes;
    worm::HobStats hob;
    double faceWidth = 0.0;
    // How the teeth were cut: by replicating one sector, or all at once
    // (docs/modeling/worm-gears.md#how-the-teeth-are-cut); and whether this
    // regeneration reused a wheel already built from the same inputs.
    bool replicated = false;
    bool reused = false;
  };
  Report report;
  [[nodiscard]] nlohmann::json ReportJson() const;
};

namespace worm {
// The self-locking report both features give: the lead angle against the
// friction angle at the dry floor for printed polymer on itself (mu 0.2) and
// greased (mu 0.1) (docs/modeling/worm-gears.md#self-locking).
[[nodiscard]] nlohmann::json LockingJson(double leadAngleRad);
} // namespace worm
