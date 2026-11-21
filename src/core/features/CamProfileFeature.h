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
    visit({baseRadius, ExprQuantity::Length, "base_radius_mm"});
    visit({width, ExprQuantity::Length, "width_mm"});
    VisitMotionExprs(visit);
  }
  // The lift table's, the segments' and the follower's numbers (vNEXT):
  // a rise and a lift are in the follower's unit, mm for a slide and
  // degrees for a turn.
  void VisitMotionExprs(const ExprVisitor& visit) {
    const bool turns =
        followerSet && follower.motion == cammech::FollowerMotion::Turn;
    const ExprQuantity rise =
        turns ? ExprQuantity::Angle : ExprQuantity::Length;
    liftExpr.resize(motion.lift.size());
    for (size_t k = 0; k < motion.lift.size(); k++) {
      const std::string at = "lift/" + std::to_string(k);
      visit(ExprSlot(liftExpr[k][0], motion.lift[k].first, ExprQuantity::Angle,
                     at + "/0"));
      visit(ExprSlot(liftExpr[k][1], motion.lift[k].second, rise, at + "/1"));
    }
    segmentExpr.resize(motion.segments.size());
    for (size_t k = 0; k < motion.segments.size(); k++) {
      cammech::Segment& sg = motion.segments[k];
      const std::string at = "segments/" + std::to_string(k) + "/";
      visit(ExprSlot(segmentExpr[k][0], sg.toDeg, ExprQuantity::Angle,
                     at + "to_deg"));
      if (sg.law != cammech::Law::Dwell)
        visit(ExprSlot(segmentExpr[k][1], sg.rise, rise,
                       at + (turns ? "rise_deg" : "rise_mm")));
    }
    if (!followerSet)
      return;
    visit(ExprSlot(followerExpr.radius, follower.radiusMm, ExprQuantity::Length,
                   "follower/radius_mm"));
    if (turns) {
      visit(ExprSlot(followerExpr.pivot[0], follower.pivotXMm,
                     ExprQuantity::Length, "follower/pivot_mm/0"));
      visit(ExprSlot(followerExpr.pivot[1], follower.pivotYMm,
                     ExprQuantity::Length, "follower/pivot_mm/1"));
      visit(ExprSlot(followerExpr.arm, follower.armMm, ExprQuantity::Length,
                     "follower/arm_mm"));
    } else {
      visit(ExprSlot(followerExpr.offset, follower.offsetMm,
                     ExprQuantity::Length, "follower/offset_mm"));
    }
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
  ExprValue baseRadius{0.0};
  ExprValue width{0.0};
  // The motion; empty = derived: rise half the base radius, a cycloidal
  // rise-dwell-fall-dwell over 120/60/120/60 degrees (a sliding follower).
  cammech::MotionSpec motion;
  // The follower it is cut for; unset = a radial roller a third of the base
  // radius.
  bool followerSet = false;
  cammech::FollowerSpec follower;
  // The texts beside `motion` and `follower` (vNEXT), "" for a literal:
  // [angle, lift] per lift entry, [to_deg, rise] per segment.
  std::vector<std::array<std::string, 2>> liftExpr;
  std::vector<std::array<std::string, 2>> segmentExpr;
  struct FollowerExprs {
    std::string radius, offset, arm;
    Vec2Exprs pivot;
  } followerExpr;
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
    // The cam's frame in part space: columns X (the zero), Y, Z (the axis),
    // origin (on the axis, at the start of the width).
    glm::dmat4 frame{1.0};
    std::vector<std::string> derived; // which inputs came from the pick
  };
  Report report;
  [[nodiscard]] nlohmann::json ReportJson() const;
};
