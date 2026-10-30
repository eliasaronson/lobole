// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/DirectEdit.h"
#include "core/ElementMap.h"
#include "core/Feature.h"

#include <Standard_Handle.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <string>
#include <vector>

class BRepTools_History;

// Removes faces from one body and heals the gap by extending the faces around
// it (docs/modeling/README.md#delete-face): a hole's bore and floor fill, a
// boss levels, a fillet or chamfer gives back its sharp edge. The body is
// changed in place; a face it extends or trims is "<name>;M<id>", every other
// face keeps its name.
//
// BRepAlgoAPI_Defeaturing reports a feature it could not remove as a WARNING
// and returns success with the feature still there, so any warning refuses,
// naming the faces it was about.
class DeleteFaceFeature : public Feature {
public:
  DeleteFaceFeature(const std::string& name, uint64_t bodyId,
                    std::vector<ElementRef> faces);

  std::string TypeName() const override { return "DeleteFace"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<DeleteFaceFeature>(*this);
  }

  // The fingerprints of the imported faces it picked
  // (directedit::ResolvePicked).
  bool RecordsWhatItIsKeyedOn() const override { return true; }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<DeleteFaceFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  // Heavy past a handful of faces: Defeaturing heals the faces round every
  // one it deletes, on one thread
  // (docs/ui/long-operations.md#heavy-features).
  [[nodiscard]] bool MayBuildOffFrame() const override { return true; }
  [[nodiscard]] double
  BuildCostS(const std::vector<Body>& input,
             const printfit::Process* print) const override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    visit(bodyId);
    VisitRefs(faces, visit);
  }

  [[nodiscard]] std::string WhyIncomplete() const override {
    return faces.empty() ? "pick the faces to delete" : "";
  }

  uint64_t bodyId = 0; // producing-feature id of the body edited
  std::vector<ElementRef> faces;
  // The picked faces' fingerprints, for file-positional names
  // (directedit::ResolvePicked).
  directedit::Fingerprints fingerprints;
};

namespace directedit {

// The healed body: Defeaturing on a copy of `picked.body`, with its history
// from that copy. Empty on success, else why not -- a warning names the
// faces it could not heal around.
struct Healed {
  TopoDS_Shape input; // the copy the builder ran on
  TopoDS_Shape result;
  Handle(BRepTools_History) history;
};
std::string DeleteAndHeal(const Picked& picked, Healed& out,
                          const std::string& op = "Delete Face");

} // namespace directedit
