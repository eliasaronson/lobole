// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Feature.h"

#include <glm/glm.hpp>
#include <gp_Cylinder.hxx>

// Thread (docs/modeling/holes-and-threads.md#thread) on a cylindrical face —
// external boss or hole wall. Cosmetic by default: the geometry is untouched
// and the UI draws the drafting convention (dashed thread-depth circles +
// crest lines) from the session `overlay` this feature computes each
// regeneration. `modeled` cuts the real 60-degree helical groove instead
// (docs/modeling/holes-and-threads.md#modelled-threads); a cut that fails is
// a feature error that keeps the body and the overlay. The designation drives
// the pitch via the ISO metric and unified tables (core/ThreadStandards); a
// custom designation keeps the manual pitch, and one that claims a series
// the tables do not carry is refused.
class ThreadFeature : public Feature {
public:
  ThreadFeature(const std::string& name, ElementRef face,
                std::string designation, double pitch);

  std::string TypeName() const override { return "Thread"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<ThreadFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;
  void VisitFeatureRefs(const RefVisitor& visit) const override {
    VisitRefs(face, visit); // the cylindrical face being threaded
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(lengthExpr, length);
  }

  ElementRef face;         // a cylindrical face, by stable toponame
  std::string designation; // "M8", "M8x1", or free text (custom)
  double pitch = 1.25;
  // "6g"/"6H", "2A"/"2B": checked against the series and the side, drives
  // no geometry (a modelled thread is the basic profile).
  std::string threadClass;
  double length = 0.0; // threaded length from the face's start; 0 = full
  std::string lengthExpr;
  bool allowEngagement = false;
  double minimumEngagement = 0.0;
  bool modeled = false;    // cut the real helical groove
  bool rightHanded = true; // JSON "hand": "right" | "left"

  // Session-only, rebuilt by Execute (never serialized): what the UI draws.
  struct Overlay {
    bool valid = false;
    glm::vec3 origin{0.0f};          // a point on the cylinder axis
    glm::vec3 dir{0.0f, 0.0f, 1.0f}; // unit axis
    float radius = 0.0f;     // dashed-circle radius (thread depth convention)
    float faceRadius = 0.0f; // the face's own radius (crest lines)
    float v0 = 0.0f, v1 = 0.0f; // threaded span along dir, relative to origin
    bool external = true;       // boss (true) vs hole wall (false)
    bool cut = false; // the modelled groove was cut: the real geometry shows
                      // the thread, so the UI draws no convention over it
  };
  Overlay overlay;

private:
  // Cuts the modelled groove from `body` over [v0, v1] of the face's axis.
  // False with the feature's error set, and `body` untouched, on a failure.
  bool CutModeled(Body& body, const gp_Cylinder& cyl, bool external, double v0,
                  double v1, double faceV1, double tableMajor);
};
