// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/SketchPlane.h" // PlaneType
#include "core/DirectEdit.h"
#include "core/ElementMap.h"
#include "core/Feature.h"

#include <TopoDS_Face.hxx>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// What a replaced face is put onto: a plane (a base plane, or a datum plane
// or a sketch's plane by feature id) or any body's face, its whole surface.
enum class ReplaceTargetKind { Plane, Sketch, Face };

// Puts one face of a body onto another surface, its neighbours extended or
// trimmed along their own surfaces to meet it
// (docs/modeling/README.md#replace-face). A parallel planar target is an
// Offset Face by the signed distance; anything else runs BOPAlgo_MakerVolume
// over the body's faces less the replaced one, its neighbours extended past
// the target, and the target bounded to the body's size, and keeps the cell
// on the inner side of the untouched faces. The new face is
// "<replaced>;M<id>".
class ReplaceFaceFeature : public Feature {
public:
  ReplaceFaceFeature(const std::string& name, uint64_t bodyId,
                     const ElementRef& face);

  std::string TypeName() const override { return "ReplaceFace"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<ReplaceFaceFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<ReplaceFaceFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
    VisitRefs(face, visit);
    visit(targetId);
    VisitRefs(targetFace, visit);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    if (face.name.empty())
      return "pick the face to replace";
    if (targetKind == ReplaceTargetKind::Face && targetFace.name.empty())
      return "pick the face or plane to put it onto";
    if (targetKind == ReplaceTargetKind::Sketch && targetId == 0)
      return "pick the face or plane to put it onto";
    return "";
  }

  uint64_t bodyId = 0; // producing-feature id of the body edited
  ElementRef face;     // the face replaced
  // The tool starts on Face with nothing picked, so a target is a choice.
  ReplaceTargetKind targetKind = ReplaceTargetKind::Face;
  // Plane: 0 = basePlane, else a DatumPlane's id. Sketch: the sketch's id.
  uint64_t targetId = 0;
  PlaneType basePlane = PlaneType::XY;
  ElementRef targetFace; // Face: on any body
  directedit::Fingerprints fingerprints;
};

namespace directedit {

// Replace Face's general case, for one face of `body`: the body's faces
// less that one, its neighbours extended past the target, and `makeOnto`'s
// face (the target's surface, bounded to `half` around `centre`) handed to
// BOPAlgo_MakerVolume, keeping the cell on the inner side of the faces left
// alone. `out` and `map` on success, else why not, naming the faces.
std::string ReplaceOnto(
    const Body& body, int index, const std::string& name,
    const std::function<TopoDS_Face(const glm::dvec3&, double)>& makeOnto,
    const std::string& label, uint64_t tag, CADShape& out, ElementMap& map);

// The same onto the plane through `origin` square to `normal`: what Offset
// Face falls back to for a planar face the offset builder will not move.
std::string ReplaceOntoPlane(const Body& body, int index,
                             const std::string& name, const glm::dvec3& origin,
                             const glm::dvec3& normal, const std::string& label,
                             uint64_t tag, CADShape& out, ElementMap& map);

} // namespace directedit
