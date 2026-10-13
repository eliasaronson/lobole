// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/ElementMap.h"
#include "core/Feature.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

// How a Frame gets its placement.
//
//   Explicit    origin (+expressions) and two directions typed in.
//   FromElement the frame of ONE face or edge (a planar face: centroid +
//               normal; a cylindrical/conical/spherical face or a circular
//               edge: centre + axis), or of SEVERAL circular elements — a
//               bolt circle: origin = the mean of their centres, Z = their
//               common axis, X = toward the first of them.
//   Offset      another Frame (or the part origin when parentId is 0) moved
//               by a translation and an XYZ rotation, both expression-driven.
enum class FrameKind { Explicit, FromElement, Offset };

// A named datum frame as a history feature
// (docs/assemblies/solver.md#connector-frames). It is the thing a mate
// connector anchors to by NAME — the mate as designed, instead of the centroid
// of whatever face happened to be nearby — and the thing a script positions
// against without ever transcribing a coordinate. Contributes no body. A
// definition that does not resolve (an element gone, a parent missing, a bolt
// circle whose axes disagree) fails Execute and stops regeneration there, the
// datum contract, so nothing downstream silently mates to a stale frame.
class FrameFeature : public Feature {
public:
  FrameFeature(const std::string& name, FrameKind kind = FrameKind::Explicit);

  [[nodiscard]] std::string TypeName() const override { return "Frame"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<FrameFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<FrameFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(parentId); // Offset kind: the frame this one hangs off
    VisitRefs(elements, visit);
  }

  [[nodiscard]] bool IsDatum() const override { return true; }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(originExpr[0], origin[0]);
    visit(originExpr[1], origin[1]);
    visit(originExpr[2], origin[2]);
    visit(offsetExpr[0], offset[0]);
    visit(offsetExpr[1], offset[1]);
    visit(offsetExpr[2], offset[2]);
    visit(rotationExpr[0], rotationDeg[0]);
    visit(rotationExpr[1], rotationDeg[1]);
    visit(rotationExpr[2], rotationDeg[2]);
  }

  // The resolved frame, part space: columns X, Y, Z, origin (frame::Build).
  [[nodiscard]] const glm::dmat4& Matrix() const { return m_frame; }
  [[nodiscard]] glm::dvec3 Origin() const { return glm::dvec3(m_frame[3]); }
  [[nodiscard]] glm::dvec3 XAxis() const { return glm::dvec3(m_frame[0]); }
  [[nodiscard]] glm::dvec3 YAxis() const { return glm::dvec3(m_frame[1]); }
  [[nodiscard]] glm::dvec3 ZAxis() const { return glm::dvec3(m_frame[2]); }

  FrameKind kind = FrameKind::Explicit;

  // Explicit: the origin (expression-driven, mirrors ExtrudeFeature's
  // distance/distanceExpr twin) and the axes as plain directions.
  double origin[3] = {0.0, 0.0, 0.0};
  std::string originExpr[3];
  glm::dvec3 zAxis{0.0, 0.0, 1.0};
  glm::dvec3 xAxis{1.0, 0.0, 0.0};

  // FromElement: one face/edge, or several circular ones (a bolt circle).
  std::vector<ElementRef> elements;
  bool flip = false; // negate Z (a face's outward normal points AWAY from
                     // the part; a connector usually wants INTO it)

  // Offset: parent Frame feature id (0 = the part origin), then translate by
  // `offset` in the parent's axes and rotate by XYZ Euler degrees, applied
  // as parent · T(offset) · Rx · Ry · Rz.
  uint64_t parentId = 0;
  double offset[3] = {0.0, 0.0, 0.0};
  std::string offsetExpr[3];
  double rotationDeg[3] = {0.0, 0.0, 0.0};
  std::string rotationExpr[3];

private:
  glm::dmat4 m_frame{1.0};
};

namespace frame {
// The frame of one face or edge among `bodies`: a planar face gives its
// centroid + outward normal + in-plane U; a cylindrical/conical/spherical
// face or a circular edge gives its centre + axis (`circular` = true). On
// failure `why` names what exists (the bodies' producers, or the similar
// names when a later feature extended this one). Shared by the Frame
// feature and the `frame` query so both answer identically.
bool ElementFrame(const std::vector<Body>& bodies, const ElementRef& ref,
                  glm::dvec3& origin, glm::dvec3& axis, glm::dvec3& xHint,
                  bool& circular, std::string& why);
} // namespace frame
