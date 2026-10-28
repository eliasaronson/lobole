// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
// A disc cam (mechanism) cut from a follower motion
// (docs/modeling/cam-profiles.md): the profile is the exact envelope of the
// stated follower, fitted as one periodic B-spline (or one per span between
// drops) and extruded across the picked hub, or a Frame. The math is
// CamProfileMath; this is the placement, the defaults and the solid.

#include "core/CamProfileMath.h"
#include "core/Feature.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class CamProfileFeature : public Feature {
public:
  CamProfileFeature(const std::string& name, ElementRef face);

  std::string TypeName() const override { return "CamProfile"; }
  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CamProfileFeature>(*this);
  }
  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<CamProfileFeature>(executed);
  }
  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    VisitRefs(face, visit);
    visit(frameId);
  }
  void VisitExprs(const ExprVisitor& visit) override {
    visit(baseRadiusExpr, baseRadius);
    visit(widthExpr, width);
  }
  [[nodiscard]] std::string WhyIncomplete() const override {
    return face.name.empty() && frameId == 0
               ? "pick the shaft or hub the cam goes on"
               : std::string{};
  }

  // Where: the OUTSIDE of a cylindrical shaft or hub (its axis, radius and
  // length), or a Frame feature (its Z the axis, X the cam's zero).
  ElementRef face;
  uint64_t frameId = 0;
  // mm; 0 = derived: the picked cylinder's radius, and its length along the
  // axis. A Frame gives neither, so both are then required.
  double baseRadius = 0.0;
  std::string baseRadiusExpr;
  double width = 0.0;
  std::string widthExpr;
  // The motion; empty = derived: rise half the base radius, a cycloidal
  // rise-dwell-fall-dwell over 120/60/120/60 degrees (a sliding follower).
  cammech::MotionSpec motion;
  // The follower it is cut for; unset = a radial roller a third of the base
  // radius.
  bool followerSet = false;
  cammech::FollowerSpec follower;
  bool ccw = true;     // the sense the cam turns in about its axis
  bool newBody = true; // false: fused into the picked hub's body

  // Session-only, rebuilt by Execute: what it built and with what.
  struct Report {
    bool valid = false;
    double baseRadius = 0.0, width = 0.0;
    cammech::MotionSpec motion; // as built: the derived default included
    cammech::FollowerSpec follower;
    cammech::Profile profile;  // its numbers (the points are dropped)
    double fitDeviation = 0.0; // mm: the fitted curves against the envelope
    std::vector<std::string> derived; // which inputs came from the pick
  };
  Report report;
  [[nodiscard]] nlohmann::json ReportJson() const;
};
