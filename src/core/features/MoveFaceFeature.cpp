// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/MoveFaceFeature.h"

#include "common/Logger.h"
#include "core/Angles.h"
#include "core/ElementHistory.h"
#include "core/ModelSpace.h"
#include "core/features/DeleteFaceFeature.h"

#include <BRepTools_History.hxx>
#include <Message_ProgressScope.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

MoveFaceFeature::MoveFaceFeature(const std::string& name, uint64_t bodyId,
                                 std::vector<ElementRef> faces,
                                 const glm::dvec3& translate)
    : bodyId(bodyId), faces(std::move(faces)), translate(translate) {
  m_name = name;
}

namespace {

std::string Mm(double v) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

// "25 mm along X", "12 mm along (0.6, 0.8, 0)", "30 deg about the axis".
std::string MoveWords(const glm::dvec3& t, double rotateDeg) {
  std::string out;
  const double len = glm::length(t);
  if (len > 0.0) {
    const glm::dvec3 d = t / len;
    std::string along;
    for (int a = 0; a < 3; a++)
      if (std::abs(std::abs(d[a]) - 1.0) < 1e-9)
        along = std::string(d[a] < 0 ? "-" : "") + "XYZ"[a];
    if (along.empty())
      along = "(" + Mm(d.x) + ", " + Mm(d.y) + ", " + Mm(d.z) + ")";
    out = Mm(len) + " mm along " + along;
  }
  if (rotateDeg != 0.0)
    out += (out.empty() ? "" : " and ") + Mm(rotateDeg) + " deg about its axis";
  return out;
}

// The name a name had before this edit's own ";M<tag>" steps.
std::string Stem(const std::string& name, uint64_t tag) {
  const std::string step = ";M" + std::to_string(tag);
  std::string out = directedit::CollapseSteps(name, tag);
  if (out.size() >= step.size() &&
      out.compare(out.size() - step.size(), step.size(), step) == 0)
    out.erase(out.size() - step.size());
  return out;
}

double ExtentAlong(const CADShape& s, const glm::dvec3& dir) {
  glm::dvec3 lo, hi;
  s.GetBoundingBox(lo, hi, /*tight=*/true);
  if (glm::length(dir) < 1e-12)
    return glm::length(hi - lo);
  const glm::dvec3 d = glm::normalize(dir);
  double mn = 1e300, mx = -1e300;
  for (int c = 0; c < 8; c++) {
    const glm::dvec3 p((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
                       (c & 4) ? hi.z : lo.z);
    mn = std::min(mn, glm::dot(p, d));
    mx = std::max(mx, glm::dot(p, d));
  }
  return mx - mn;
}

// Names of `crossed`'s faces that came from `host` (not from the feature's
// own material, `own`): the faces of the body a moved feature runs into.
std::vector<std::string> HostFaces(const ElementMap& crossed,
                                   const ElementMap& host,
                                   const ElementMap& own, uint64_t tag) {
  std::set<std::string> hostNames, ownNames;
  for (const auto& [i, n] : host.names)
    hostNames.insert(Stem(n, tag));
  for (const auto& [i, n] : own.names)
    ownNames.insert(Stem(n, tag));
  std::vector<std::string> out;
  for (const auto& [i, n] : crossed.names) {
    const std::string s = Stem(n, tag);
    if (hostNames.count(s) && !ownNames.count(s) &&
        std::find(out.begin(), out.end(), s) == out.end())
      out.push_back(s);
  }
  return out;
}

} // namespace

bool MoveFaceFeature::Execute(RegenContext& ctx) {
  if (faces.empty())
    return true; // previewing, nothing picked (WhyIncomplete)
  directedit::Picked picked;
  if (std::string why = directedit::ResolvePicked(ctx, bodyId, faces,
                                                  fingerprints, "move", picked);
      !why.empty()) {
    SetError(why);
    return false;
  }
  const uint64_t tag = GetId();
  const Body& body = *picked.body;
  const std::string what = directedit::NameList(picked.names);
  if (!InModelSpace(translate.x) || !InModelSpace(translate.y) ||
      !InModelSpace(translate.z) || !std::isfinite(rotateDeg)) {
    SetError("Move Face of " + what +
             ": the move is not a finite distance in model space");
    return false;
  }
  if (glm::length(translate) < 1e-9 && std::abs(rotateDeg) < 1e-9) {
    SetError("Move Face of " + what +
             " moves nothing: give a translation "
             "or a rotation");
    return false;
  }
  gp_Trsf trsf;
  if (std::abs(rotateDeg) > 0.0) {
    glm::dvec3 o, d;
    if (!ResolveAxis(ctx, axis, o, d)) {
      SetError("Move Face of " + what + ": the rotation axis does not resolve");
      return false;
    }
    trsf.SetRotation(gp_Ax1(gp_Pnt(o.x, o.y, o.z), gp_Dir(d.x, d.y, d.z)),
                     angles::ToRadians(rotateDeg));
  }
  {
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(translate.x, translate.y, translate.z));
    trsf = shift * trsf; // rotate first, then translate
  }
  const TopLoc_Location moved(trsf);

  // 1. H: the body with the faces deleted.
  directedit::Healed healed;
  if (std::string why = directedit::DeleteAndHeal(picked, healed, "Move Face");
      !why.empty()) {
    SetError(why + ". A face that does not lift out of the body is moved "
                   "with Offset Face");
    return false;
  }
  const ElementMap mapH = directedit::NameThrough(
      healed.input, body.elementMap, *healed.history, healed.result, tag);
  const CADShape H(healed.result);
  const CADShape B(directedit::CopyFor(body, {}).shape);
  const double vol = std::max(1.0, std::abs(body.shape.Volume()));
  const double tol = 1e-9 * vol;

  // Every boolean below runs under one deadline: separating the feature
  // from a body is a boolean of two nearly identical solids, every face
  // coincident with one of the other's, and on chamfered, drilled bodies the
  // kernel spends minutes intersecting them (a hole's bore on random design
  // 33 took 68 s just to separate).
  const double budget = directedit::BuilderBudgetS(picked.names.size());
  const Handle(Message_ProgressIndicator) deadline =
      directedit::MakeDeadline(budget);
  Message_ProgressScope steps(deadline->Start(), "Move Face", 7);
  auto gaveUp = [&] {
    return "Move Face of " + what + " gave up after " +
           std::to_string(int(budget)) +
           " s: the body's faces are too costly to intersect with the "
           "feature's";
  };

  // 2. The feature's material.
  ElementMap mapCut, mapAdd;
  std::string err;
  const CADShape cut =
      topo::BooleanNamed(BooleanOp::Subtract, H, mapH, B, body.elementMap, tag,
                         mapCut, &err, 0.0, {}, steps.Next());
  const CADShape add =
      directedit::Expired(deadline)
          ? CADShape()
          : topo::BooleanNamed(BooleanOp::Subtract, B, body.elementMap, H, mapH,
                               tag, mapAdd, &err, 0.0, {}, steps.Next());
  if (directedit::Expired(deadline)) {
    SetError(gaveUp());
    return false;
  }
  if (!cut.IsValid() && !add.IsValid()) {
    SetError("Move Face could not separate " + what +
             " from the body: " + (err.empty() ? "the boolean failed" : err));
    return false;
  }
  const double cutV = cut.IsValid() ? cut.Volume() : 0.0;
  const double addV = add.IsValid() ? add.Volume() : 0.0;
  const bool hasCut = cutV > tol, hasAdd = addV > tol;
  if (!hasCut && !hasAdd) {
    SetError("Move Face: deleting " + what +
             " changes nothing, so there is no feature to move; Offset Face "
             "moves a face of the body itself");
    return false;
  }
  CADShape tCut, tAdd;
  if (hasCut)
    tCut = CADShape(cut.GetShape().Moved(moved));
  if (hasAdd)
    tAdd = CADShape(add.GetShape().Moved(moved));

  // A slide within the feature's own surfaces (a bore along its axis)
  // changes nothing.
  {
    bool same = true;
    if (hasCut) {
      const CADShape both = cut.Intersection(tCut);
      same = same && both.IsValid() && std::abs(both.Volume() - cutV) <= tol;
    }
    if (hasAdd) {
      const CADShape both = add.Intersection(tAdd);
      same = same && both.IsValid() && std::abs(both.Volume() - addV) <= tol;
    }
    if (same) {
      SetError("Move Face: moving " + what + " " +
               MoveWords(translate, rotateDeg) +
               " slides it within its own surfaces, which changes nothing");
      return false;
    }
  }

  // The moved hole must stay inside the body, the moved boss outside it.
  if (hasCut) {
    ElementMap mapOut;
    const CADShape out =
        topo::BooleanNamed(BooleanOp::Subtract, tCut, mapCut, H, mapH, tag,
                           mapOut, nullptr, 0.0, {}, steps.Next());
    if (directedit::Expired(deadline)) {
      SetError(gaveUp());
      return false;
    }
    if (out.IsValid() && out.Volume() > tol) {
      const auto crossed = HostFaces(mapOut, mapH, mapCut, tag);
      SetError("moving " + what + " " + MoveWords(translate, rotateDeg) +
               " takes it " + Mm(ExtentAlong(out, translate)) + " mm past " +
               (crossed.empty() ? std::string("the body's outside")
                                : directedit::NameList(crossed)));
      return false;
    }
  }
  if (hasAdd) {
    ElementMap mapIn;
    const CADShape in =
        topo::BooleanNamed(BooleanOp::Intersect, tAdd, mapAdd, H, mapH, tag,
                           mapIn, nullptr, 0.0, {}, steps.Next());
    if (directedit::Expired(deadline)) {
      SetError(gaveUp());
      return false;
    }
    if (in.IsValid() && in.Volume() > tol) {
      const auto crossed = HostFaces(mapIn, mapH, mapAdd, tag);
      SetError("moving " + what + " " + MoveWords(translate, rotateDeg) +
               " sinks it " + Mm(ExtentAlong(in, translate)) +
               " mm into the body" +
               (crossed.empty() ? std::string()
                                : " through " + directedit::NameList(crossed)));
      return false;
    }
  }

  // 3. (H - T(cut)) u T(add).
  CADShape result = H;
  ElementMap map = mapH;
  if (hasCut) {
    ElementMap next;
    result = topo::BooleanNamed(BooleanOp::Subtract, result, map, tCut, mapCut,
                                tag, next, &err, 0.0, {}, steps.Next());
    map = std::move(next);
  }
  if (hasAdd && result.IsValid()) {
    ElementMap next;
    result = topo::BooleanNamed(BooleanOp::Union, result, map, tAdd, mapAdd,
                                tag, next, &err, 0.0, {}, steps.Next());
    map = std::move(next);
  }
  if (directedit::Expired(deadline)) {
    SetError(gaveUp());
    return false;
  }
  if (!result.IsValid()) {
    SetError("Move Face of " + what +
             " failed: " + (err.empty() ? "the boolean did not build" : err));
    return false;
  }
  // One edit, one step: the chain of booleans ran under this tag, and a wall
  // that moved is "<name>;M<tag>" whether or not a boolean trimmed it.
  {
    const std::string step = ";M" + std::to_string(tag);
    std::set<std::string> movedNames(picked.names.begin(), picked.names.end());
    std::set<std::string> seen;
    for (auto& [i, n] : map.names) {
      n = directedit::CollapseSteps(n, tag);
      if (movedNames.count(n))
        n += step;
    }
    for (auto& [i, n] : map.edgeNames)
      n = directedit::CollapseSteps(n, tag);
    // Collapsing can make two names one: keep them apart by position.
    for (auto* names : {&map.names, &map.edgeNames}) {
      std::set<std::string> used;
      for (auto& [i, n] : *names) {
        if (used.insert(n).second)
          continue;
        for (int k = 1;; k++)
          if (used.insert(n + "." + std::to_string(k)).second) {
            n += "." + std::to_string(k);
            break;
          }
      }
    }
  }
  directedit::RestoreUntouched(body, result, map, tag, picked.names);
  directedit::Expect expect;
  expect.volume = directedit::Expect::Volume::Same;
  expect.volumeTolerance = 1e-9;
  expect.faces = directedit::Expect::Faces::Modified;
  {
    glm::dvec3 lo, hi;
    body.shape.GetBoundingBox(lo, hi, /*tight=*/true);
    const double diag = glm::length(hi - lo);
    expect.bboxGrowth =
        glm::length(translate) + diag * std::abs(angles::ToRadians(rotateDeg));
  }
  if (std::string why = directedit::CheckResult(body, picked, result, map, tag,
                                                expect, "Move Face");
      !why.empty()) {
    SetError(why);
    LOG_ERROR("MoveFaceFeature '{}': {}", m_name, why);
    return false;
  }
  picked.body->shape = result;
  picked.body->elementMap = map;
  return true;
}
