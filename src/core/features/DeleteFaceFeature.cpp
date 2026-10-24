// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/DeleteFaceFeature.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"
#include "core/Measure.h"

#include <BOPAlgo_Alerts.hxx>
#include <BRepAlgoAPI_Defeaturing.hxx>
#include <BRepBndLib.hxx>
#include <BRepTools_History.hxx>
#include <Bnd_Box.hxx>
#include <Message_Report.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_AlertWithShape.hxx>

#include <algorithm>

DeleteFaceFeature::DeleteFaceFeature(const std::string& name, uint64_t bodyId,
                                     std::vector<ElementRef> faces)
    : bodyId(bodyId), faces(std::move(faces)) {
  m_name = name;
}

namespace directedit {

std::string DeleteAndHeal(const Picked& picked, Healed& out,
                          const std::string& op) {
  const Body& body = *picked.body;
  // A set that takes every face of the body leaves nothing to heal from:
  // refused before the builder runs.
  {
    TopTools_IndexedMapOfShape all;
    TopExp::MapShapes(body.shape.GetShape(), TopAbs_FACE, all);
    if (static_cast<int>(picked.indices.size()) >= all.Extent())
      return op + " of " + NameList(picked.names) +
             ", every face of the body, leaves no closed solid";
  }
  // Healing extends the faces round the gap until they meet.
  if (std::string why = FreeformNeighbour(picked, op, "heals by extending");
      !why.empty())
    return why;
  const Copied copy = CopyFor(body, picked.indices);
  out.input = copy.shape;
  BRepAlgoAPI_Defeaturing df;
  const Handle(Message_ProgressIndicator) deadline =
      MakeDeadline(kBuilderBudgetS);
  const std::string why = Guarded(op + " of " + NameList(picked.names), [&] {
    df.SetShape(copy.shape);
    df.AddFacesToRemove(copy.faces);
    df.SetRunParallel(false);
    df.SetToFillHistory(true);
    df.Build(deadline->Start());
  });
  if (!why.empty())
    return why;
  if (Expired(deadline))
    return op + " gave up after " + std::to_string(int(kBuilderBudgetS)) +
           " s healing around " + NameList(picked.names) +
           ": the faces around them are too costly to extend (a modelled "
           "thread's flanks, freeform faces)";
  // A feature it could not remove is a WARNING with the feature's faces as
  // its shape, and the result then still has them: refuse, naming them.
  if (df.HasWarnings() || df.HasErrors()) {
    std::vector<std::string> stuck;
    bool unable = false;
    const Handle(Message_Report) report = df.GetReport();
    for (const Message_Gravity g : {Message_Warning, Message_Fail}) {
      for (const Handle(Message_Alert) & alert : report->GetAlerts(g)) {
        if (!Handle(BOPAlgo_AlertUnableToRemoveTheFeature)::DownCast(alert)
                 .IsNull())
          unable = true;
        const Handle(TopoDS_AlertWithShape) withShape =
            Handle(TopoDS_AlertWithShape)::DownCast(alert);
        if (withShape.IsNull())
          continue;
        for (const std::string& n : topo::FaceNamesIn(
                 withShape->GetShape(), copy.shape, body.elementMap))
          if (std::find(stuck.begin(), stuck.end(), n) == stuck.end())
            stuck.push_back(n);
      }
    }
    if (stuck.empty())
      stuck = picked.names;
    if (unable || df.HasWarnings())
      return op + " could not heal around " + NameList(stuck) +
             ": the faces around them do not meet when extended (parallel "
             "or tangent)";
    return op + " failed on " + NameList(stuck);
  }
  if (!df.IsDone())
    return op + " failed on " + NameList(picked.names);
  out.result = df.Shape();
  out.history = df.History();
  if (out.history.IsNull())
    return op + " kept no history of what it changed";
  return {};
}

} // namespace directedit

bool DeleteFaceFeature::Execute(RegenContext& ctx) {
  if (faces.empty()) {
    // Previewing with nothing picked yet: nothing to do (WhyIncomplete stops
    // it being committed so).
    if (std::none_of(ctx.bodies.begin(), ctx.bodies.end(),
                     [&](const Body& b) { return b.createdBy == bodyId; }) &&
        bodyId != 0) {
      SetError("the body to edit no longer exists");
      return false;
    }
    return true;
  }
  directedit::Picked picked;
  if (std::string why = directedit::ResolvePicked(
          ctx, bodyId, faces, fingerprints, "delete", picked);
      !why.empty()) {
    SetError(why);
    return false;
  }
  directedit::Healed healed;
  if (std::string why = directedit::DeleteAndHeal(picked, healed);
      !why.empty()) {
    SetError(why);
    LOG_ERROR("DeleteFaceFeature '{}': {}", m_name, why);
    return false;
  }
  const CADShape result(healed.result);
  ElementMap map =
      directedit::NameThrough(healed.input, picked.body->elementMap,
                              *healed.history, healed.result, GetId());
  directedit::RestoreUntouched(*picked.body, result, map, GetId(),
                               picked.names);
  // Healing extends the faces round the gap; it reaches no further than the
  // deleted faces themselves did.
  double reach = 0.0;
  {
    const directedit::Copied mine =
        directedit::CopyFor(*picked.body, picked.indices);
    Bnd_Box box;
    for (const TopoDS_Shape& f : mine.faces)
      BRepBndLib::Add(f, box);
    if (!box.IsVoid()) {
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      reach = std::max({x1 - x0, y1 - y0, z1 - z0});
    }
  }
  directedit::Expect expect;
  expect.volume = directedit::Expect::Volume::Changes;
  expect.faces = directedit::Expect::Faces::Gone;
  expect.bboxGrowth = reach;
  if (std::string why = directedit::CheckResult(
          *picked.body, picked, result, map, GetId(), expect, "Delete Face");
      !why.empty()) {
    SetError(why);
    return false;
  }
  picked.body->shape = result;
  picked.body->elementMap = map;
  return true;
}
