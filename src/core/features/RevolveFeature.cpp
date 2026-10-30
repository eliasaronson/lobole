// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/RevolveFeature.h"

#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

RevolveFeature::RevolveFeature(const std::string& name, uint64_t sketchId,
                               double angleDeg, ExtrudeMode mode, bool reversed)
    : sketchId(sketchId), angleDeg(angleDeg), reversed(reversed), mode(mode) {
  m_name = name;
}

bool RevolveFeature::Execute(RegenContext& ctx) {
  m_tool = CADShape(); // a failed regen leaves no stale tool to pattern
  m_toolMap = ElementMap();
  const auto* sketch =
      dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (!sketch) {
    SetError("profile sketch (id " + std::to_string(sketchId) +
             ") not found earlier in history");
    return false;
  }
  // As Extrude: a failed sketch's stale plane is not a place to build.
  if (!sketch->GetError().empty()) {
    SetError("profile sketch '" + sketch->GetName() +
             "' failed: " + sketch->GetError());
    return false;
  }

  std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch->entities);
  // Single-stroke text makes no profile: refused by name when it is what was
  // picked, or all there is (docs/sketching/README.md#text).
  if (const std::string why = sketch::StrokeTextProfileRefusal(
          sketch->entities, regions, loops.empty());
      !why.empty()) {
    SetError(why);
    return false;
  }
  if (loops.empty()) {
    const std::string where =
        sketch::OpenEndsWords(sketch::OpenEnds(sketch->entities));
    SetError("sketch has no closed profile" +
             (where.empty() ? std::string() : ": " + where));
    return false;
  }
  // Region selection (U2): keep only the chosen regions (+ their holes).
  if (!regions.empty() && !sketch::FilterLoopsToRegions(loops, regions)) {
    SetError("a selected region no longer exists in the sketch - re-pick "
             "the profiles");
    return false;
  }
  if (loops.empty()) {
    SetError("the selected regions leave no profile");
    return false;
  }

  glm::dvec3 axisOrigin{0.0};
  glm::dvec3 axisDir{1.0, 0.0, 0.0};
  if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
    SetError("revolve axis not found (deleted datum axis or sketch line?)");
    return false;
  }

  // The axis must lie in the sketch plane: its direction perpendicular to the
  // plane normal, and a point of it on the plane. Otherwise the revolve is a
  // skew sweep rather than a solid of revolution.
  const glm::dvec3 n = sketch->plane.GetNormal();
  const glm::dvec3 planeO = sketch->plane.GetOrigin();
  if (std::abs(glm::dot(axisDir, n)) > 1e-4 ||
      std::abs(glm::dot(axisOrigin - planeO, n)) > 1e-4) {
    SetError("revolve axis is not in the sketch plane");
    return false;
  }

  ElementMap map;
  std::string buildErr;
  CADShape solid = sketch::BuildRevolveNamed(
      sketch->plane, loops, axisOrigin, axisDir, glm::radians(angleDeg),
      reversed, symmetric, GetId(), map, &buildErr);
  if (!solid.IsValid()) {
    SetError(buildErr.empty() ? "revolve construction failed" : buildErr);
    return false;
  }

  if (mode == ExtrudeMode::Cut || mode == ExtrudeMode::Add) {
    m_tool = solid; // for a Pattern of this cut or add (ToolShape)
    m_toolMap = map;
  }
  // A full turn's seam is its profile, which lies in the sketch plane -- and
  // a sketch drawn on a face of the body puts it IN that face: a d2.5 sphere
  // cut centred on a half revolve's cap beside a fillet left the cap inside
  // the sphere, exact and fuzzy alike, and was refused. A solid of a full
  // revolution is the same point set turned about its axis, so a refused cut
  // or add is tried once more with the solid turned a quarter, which takes
  // the seam out of the plane. The names are the solid's (the turn shares its
  // topology); a refused retry reports the first refusal.
  const bool fullTurn = std::abs(std::abs(angleDeg) - 360.0) < 1e-9;
  if (fullTurn && (mode == ExtrudeMode::Cut || mode == ExtrudeMode::Add)) {
    // A refused cut or add leaves the bodies as they were (ApplyBodyMode),
    // so the retry starts from the same ones.
    std::string warning, error;
    if (ApplyBodyMode(ctx.bodies, solid, map, mode, GetName(), GetMaterial(),
                      GetId(), &warning, &error)) {
      if (!warning.empty())
        SetWarning(warning);
      return true;
    }
    const glm::dmat4 turn =
        glm::translate(glm::dmat4(1.0), axisOrigin) *
        glm::rotate(glm::dmat4(1.0), glm::half_pi<double>(), axisDir) *
        glm::translate(glm::dmat4(1.0), -axisOrigin);
    std::string turnedWarning, turnedError;
    if (ApplyBodyMode(ctx.bodies, solid.Transformed(turn), map, mode, GetName(),
                      GetMaterial(), GetId(), &turnedWarning, &turnedError)) {
      LOG_INFO("revolve '{}': kept with its seam turned a quarter about the "
               "axis; as drawn: {}",
               GetName(), error);
      if (!turnedWarning.empty())
        SetWarning(turnedWarning);
      return true;
    }
    SetError(error.empty() ? "body operation failed" : error);
    return false;
  }
  return ApplyBodyModeReporting(ctx.bodies, std::move(solid), std::move(map),
                                mode, *this);
}
