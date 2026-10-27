// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/DirectEdit.h"
#include "core/ElementMap.h"
#include "core/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

// Moves faces of one body along their own normals by `distance` mm, positive
// outward (adding material); their neighbours follow along their own
// surfaces, so a drafted wall stays drafted and a cylinder changes radius
// (docs/modeling/README.md#offset-face). BRepOffset_MakeOffset in skin mode,
// intersection joins, zero offset on the shape and the distance on each
// picked face; named from its history.
class OffsetFaceFeature : public Feature {
public:
  OffsetFaceFeature(const std::string& name, uint64_t bodyId,
                    std::vector<ElementRef> faces, double distance);

  std::string TypeName() const override { return "OffsetFace"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<OffsetFaceFeature>(*this);
  }

  // The fingerprints of the imported faces it picked
  // (directedit::ResolvePicked).
  bool RecordsWhatItIsKeyedOn() const override { return true; }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<OffsetFaceFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
    VisitRefs(faces, visit);
  }

  void VisitExprs(const ExprVisitor& visit) override {
    visit(distanceExpr, distance);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return faces.empty() ? "pick the faces to offset" : "";
  }

  uint64_t bodyId = 0; // producing-feature id of the body edited
  std::vector<ElementRef> faces;
  double distance = 1.0; // mm, + outward
  std::string distanceExpr;
  directedit::Fingerprints fingerprints;
};

namespace directedit {

// The offset, built: `out` and `map` set on success, else why not. What
// ReplaceFace runs for a parallel planar target too. `op` names the edit in
// a refusal.
std::string OffsetFaces(const Picked& picked, double distance, uint64_t tag,
                        CADShape& out, ElementMap& map,
                        const std::string& op = "Offset Face");

// How thick the body is under the face at `faceIndex`: from a point well
// inside the face, along its inward normal, to where the line leaves the
// solid. nullopt when it cannot be measured.
std::optional<double> ThicknessUnder(const Body& body, int faceIndex);

// ThicknessUnder for each of `faceIndices` at once (the solid loaded into
// one intersector), with the face where each line leaves the solid.
struct WallUnder {
  double thickness = 0.0;
  int exitFace = -1; // 0-based explorer index
};
std::vector<std::optional<WallUnder>>
WallsUnder(const Body& body, const std::vector<int>& faceIndices);

} // namespace directedit
