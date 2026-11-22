// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/FoldFeature.h"

#include "core/Angles.h"
#include "core/Document.h"
#include "core/features/SketchFeature.h"

#include <BRepClass3d_SolidClassifier.hxx>

#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

constexpr std::array<std::pair<sheet::FoldPosition, const char*>, 3> kPositions{
    {{sheet::FoldPosition::Centerline, "centerline"},
     {sheet::FoldPosition::BendStart, "bend_start"},
     {sheet::FoldPosition::BendEnd, "bend_end"}}};
constexpr std::array<std::pair<FoldSide, const char*>, 2> kSides{
    {{FoldSide::Left, "left"}, {FoldSide::Right, "right"}}};

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3g", v);
  return buf;
}

} // namespace

const char* FoldPositionName(sheet::FoldPosition p) {
  for (const auto& [v, n] : kPositions)
    if (v == p)
      return n;
  return "centerline";
}

bool FoldPositionFromString(const std::string& name, sheet::FoldPosition& out) {
  for (const auto& [v, n] : kPositions)
    if (name == n) {
      out = v;
      return true;
    }
  return false;
}

const std::string& FoldPositionList() {
  static const std::string list = "centerline, bend_start, bend_end";
  return list;
}

const char* FoldSideName(FoldSide s) {
  for (const auto& [v, n] : kSides)
    if (v == s)
      return n;
  return "";
}

bool FoldSideFromString(const std::string& name, FoldSide& out) {
  for (const auto& [v, n] : kSides)
    if (name == n) {
      out = v;
      return true;
    }
  return false;
}

const std::string& FoldSideList() {
  static const std::string list = "left, right";
  return list;
}

FoldFeature::FoldFeature(const std::string& name, uint64_t sketchId)
    : sketchId(sketchId) {
  m_name = name;
}

std::string FoldFeature::FindLine(const std::vector<Body>& bodies,
                                  const Feature* sketchFeature, double t,
                                  Line& out) const {
  const auto* sketch = dynamic_cast<const SketchFeature*>(sketchFeature);
  if (sketch == nullptr)
    return "the fold's sketch (id " + std::to_string(sketchId) +
           ") is not earlier in the history";
  // A sketch that failed this regeneration still holds its last plane and
  // profile; building from those puts the fold where the line used to be.
  if (!sketch->GetError().empty())
    return "the fold's sketch '" + sketch->GetName() +
           "' failed: " + sketch->GetError();
  const sketch::Line* line = nullptr;
  int lines = 0, others = 0;
  for (const sketch::Entity& ent : sketch->entities) {
    if (const auto* l = std::get_if<sketch::Line>(&ent)) {
      if (l->construction)
        continue;
      line = l;
      ++lines;
    } else if (!std::holds_alternative<sketch::Point>(ent)) {
      ++others;
    }
  }
  if (lines != 1 || others != 0)
    return "a fold's sketch holds exactly one line - '" + sketch->GetName() +
           "' has " + std::to_string(lines) + " line" +
           (lines == 1 ? "" : "s") +
           (others != 0 ? " and " + std::to_string(others) + " other curve" +
                              (others == 1 ? "" : "s")
                        : std::string{});
  out.a2 = line->a;
  out.b2 = line->b;
  out.sketchNormal = sketch->plane.GetNormal();
  const glm::dvec3 a = sketch->plane.PlaneToWorld(line->a);
  const glm::dvec3 b = sketch->plane.PlaneToWorld(line->b);
  if (glm::length(b - a) < 1e-9)
    return "the fold's line has no length";
  const auto panel = sheet::PanelUnder(bodies, a, b, t, true);
  if (!panel)
    return "the fold's line is not drawn on a flat panel of a sheet - sketch "
           "it on the panel's face";
  out.panel = *panel;
  return {};
}

FoldSide FoldFeature::SideOf(const Line& line, const glm::dvec2& p) {
  const glm::dvec2 d = line.b2 - line.a2;
  const glm::dvec2 q = p - line.a2;
  const double len = glm::length(d);
  if (len < 1e-12)
    return FoldSide::Unset;
  // Signed distance from the line in the sketch's own frame; left of the line
  // seen from the sketch normal. The face may look the other way.
  const double s = (d.x * q.y - d.y * q.x) / len;
  if (std::abs(s) < 1e-6)
    return FoldSide::Unset;
  const bool leftInSketch = s > 0.0;
  const bool sameWay = glm::dot(line.sketchNormal, line.panel.normal) > 0.0;
  return leftInSketch == sameWay ? FoldSide::Left : FoldSide::Right;
}

std::string FoldFeature::WhySideAt(const std::vector<Body>& bodies,
                                   const Line& line, const Feature* sketch,
                                   double t) const {
  if (!sideAt)
    return {};
  const std::string at =
      "side_at_mm (" + Mm(sideAt->x) + ", " + Mm(sideAt->y) + ")";
  if (SideOf(line, *sideAt) == FoldSide::Unset)
    return at + " lies on the fold's line - give a point on the side that "
                "turns";
  const auto* sk = dynamic_cast<const SketchFeature*>(sketch);
  if (sk == nullptr)
    return {};
  glm::dvec3 p = sk->plane.PlaneToWorld(*sideAt);
  // Onto the face's plane, then half a thickness into the sheet.
  const glm::dvec3 n = line.panel.normal;
  p -= n * glm::dot(p - line.panel.a, n);
  p -= n * (t / 2.0);
  BRepClass3d_SolidClassifier in(bodies[line.panel.body].shape.GetShape(),
                                 gp_Pnt(p.x, p.y, p.z), 1e-6);
  if (in.State() != TopAbs_IN)
    return at + " is off the sheet - give a point on the panel, on the "
                "side that turns";
  return {};
}

std::string FoldFeature::WhyRefusedOn(const std::vector<Body>& input,
                                      const Document& doc) const {
  if (!sideAt)
    return {};
  const auto& rules = doc.SheetMetal();
  if (!rules)
    return {}; // Execute says so
  Line line;
  const Feature* sketch = doc.FindFeatureById(sketchId);
  if (!FindLine(input, sketch, rules->thickness, line).empty())
    return {}; // Execute says so, in the same words
  return WhySideAt(input, line, sketch, rules->thickness);
}

bool FoldFeature::Execute(RegenContext& ctx) {
  zones.clear();
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules - set the material and gauge "
             "first (set_sheet_metal_rules)");
    return false;
  }
  const sheet::SheetMetalRules& rules = *ctx.sheetMetal;
  const double t = rules.thickness;
  Line line;
  if (std::string why = FindLine(ctx.bodies, ctx.FindById(sketchId), t, line);
      !why.empty()) {
    SetError(why);
    return false;
  }
  lineA = line.panel.a;
  lineB = line.panel.b;
  faceNormal = line.panel.normal;
  const double theta = angles::ToRadians(angleDeg);
  if (theta <= 1e-6 || theta > angles::kPi + 1e-9) {
    SetError("a fold turns by more than 0 and at most 180 degrees (got " +
             Mm(angleDeg) + ")");
    return false;
  }
  const double minR = sheet::MinBendRadius(rules.material, t);
  effectiveRadius = radius > 0.0 ? radius.value : rules.EffectiveBendRadius();
  radiusSource = radius > 0.0                   ? "set"
                 : rules.bendRadius.value > 0.0 ? "rules"
                                                : "material minimum";
  if (effectiveRadius < minR - 1e-9) {
    SetError("bend radius " + Mm(effectiveRadius) + " mm is tighter than " +
             rules.material + " takes at " + rules.gauge + " (minimum " +
             Mm(minR) + " mm)");
    return false;
  }
  Body& body = ctx.bodies[line.panel.body];

  sheet::FoldSpec spec;
  spec.a = line.panel.a;
  spec.b = line.panel.b;
  spec.normal = line.panel.normal;
  spec.angle = theta;
  spec.radius = effectiveRadius;
  spec.position = position;
  spec.reverse = reverse;
  if (sideAt) {
    if (std::string why =
            WhySideAt(ctx.bodies, line, ctx.FindById(sketchId), t);
        !why.empty()) {
      SetError(why);
      return false;
    }
    side = SideOf(line, *sideAt);
    sideAt.reset();
  }
  if (side == FoldSide::Unset) {
    // Once, and stored: the side that frees less of the sheet.
    const auto smaller = sheet::FoldSmallerSideIsLeft(body, rules, spec);
    side = smaller.value_or(true) ? FoldSide::Left : FoldSide::Right;
  }
  spec.left = side == FoldSide::Left;
  const double K = rules.EffectiveKFactor(effectiveRadius);
  allowance = sheet::BendAllowance(theta, effectiveRadius, K, t);

  std::string why;
  auto folded = sheet::Fold(body, rules, spec, GetId(), &why);
  if (!folded) {
    SetError(why.empty() ? "the fold failed" : why);
    return false;
  }
  zones = folded->zones;
  for (const BendRegion& b : folded->folded.bends)
    if (b.featureId == GetId()) {
      axisOrigin = b.axisOrigin;
      axisDir = b.axisDir;
      parentDir = b.parentDir;
      upDir = b.upDir;
      break;
    }
  body.shape = std::move(folded->folded.shape);
  body.elementMap = std::move(folded->folded.map);
  body.bends = std::move(folded->folded.bends);
  return true;
}
