// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/ShellFeature.h"

#include "cad/OccSignals.h"
#include "core/Measure.h" // InvalidReasons
#include "core/ModelSpace.h"

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepTools_History.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Type.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <string>

ShellFeature::ShellFeature(const std::string& name, uint64_t bodyId,
                           std::vector<ElementRef> openFaces, double thickness)
    : bodyId(bodyId), openFaces(std::move(openFaces)), thickness(thickness) {
  m_name = name;
}

bool ShellFeature::Execute(RegenContext& ctx) {
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the body to shell no longer exists");
    return false;
  }
  if (thickness <= 1e-9) {
    SetError("shell thickness must be positive");
    return false;
  }
  for (const ElementRef& ref : openFaces) {
    if (ref.featureId != bodyId) {
      SetError("all open faces must belong to the shelled body");
      return false;
    }
  }

  // A wall at least half the body's thinnest extent leaves no cavity, and
  // BRepOffset given one far past it (1e308) crashed inside the offset
  // builder rather than failing (hostile_params_smoke).
  {
    glm::dvec3 lo, hi;
    body->shape.GetBoundingBox(lo, hi);
    const glm::dvec3 ext = hi - lo;
    const double thinnest = std::min({ext.x, ext.y, ext.z});
    if (!InModelSpace(thickness) || thickness >= 0.5 * thinnest) {
      SetError("shell thickness " + std::to_string(thickness) +
               " mm is at least half the body's thinnest extent (" +
               std::to_string(thinnest) +
               " mm): a wall that thick leaves "
               "no cavity");
      return false;
    }
  }
  const TopoDS_Shape& shape = body->shape.GetShape();

  // Collect the faces to remove by their stable names -> current indices ->
  // TopoDS_Face (TopExp_Explorer 0-based order, the ElementMap convention).
  std::vector<int> removeIdx;
  for (const ElementRef& ref : openFaces) {
    const auto idx = body->elementMap.Resolve(ref.name);
    if (!idx) {
      SetError(topo::UnresolvedReason(ctx, ref, topo::ElementKind::Face,
                                      "open face"));
      return false;
    }
    removeIdx.push_back(*idx);
  }
  // Shell needs at least one open face to hollow through (the standard shell
  // semantics in commercial CAD). A sealed hollow shell is a v2 (a manual
  // offset+cut).
  if (removeIdx.empty()) {
    SetError("shell needs at least one open face to remove");
    return false;
  }
  TopTools_ListOfShape facesToRemove;
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++) {
    if (std::find(removeIdx.begin(), removeIdx.end(), i) != removeIdx.end())
      facesToRemove.Append(ex.Current());
  }

  try {
    // Negative offset hollows inward, leaving a wall of `thickness`; the
    // selected faces are removed (open shell).
    BRepOffsetAPI_MakeThickSolid maker;
    // In an OCC signal scope (cad/OccSignals.h): the offset builder faults
    // rather than failing on walls it cannot make.
    if (const auto why = occsig::RunCatchingOccSignals([&] {
          maker.MakeThickSolidByJoin(shape, facesToRemove, -thickness, 1.0e-3);
        })) {
      SetError("shell failed inside OpenCASCADE (" + *why +
               "); a thinner wall usually builds");
      return false;
    }
    if (!maker.IsDone()) {
      SetError("shell failed (wall too thick for the geometry?)");
      return false;
    }
    CADShape result(maker.Shape());
    if (!result.IsValid() || result.Volume() < 1e-9) {
      SetError("shell produced no solid (an empty result); a thinner wall "
               "usually builds");
      return false;
    }
    if (const std::string bad = Measure::InvalidReasons(result); !bad.empty()) {
      SetError("shell produced an invalid solid (" + bad +
               "); a thinner wall usually builds");
      return false;
    }
    // OCC can report success and hand back the INPUT solid (a wall thicker
    // than the geometry allows, or an offset that self-intersects around a
    // hole). Opening a face always removes material, so a result that did
    // not shrink is a silent failure — and it used to commit as a no-op
    // feature that looked fine in the tree.
    if (result.Volume() >= body->shape.Volume() - 1e-9) {
      SetError("shell had no effect: thickness " + std::to_string(thickness) +
               " is too large for this body (or self-intersects around a "
               "hole/fillet) - try a thinner wall");
      return false;
    }
    // A shell hollows INWARD, so it can never reach past the body's own
    // outside. The offset builder sometimes moved a face outward instead (a
    // revolve with a bore in its floor, shelled 4.6 mm: its floor went 4.6
    // mm down) and the result was a valid, wrong solid (tools/fuzz.py).
    {
      glm::dvec3 lo0, hi0, lo1, hi1;
      body->shape.GetBoundingBox(lo0, hi0, /*tight=*/true);
      result.GetBoundingBox(lo1, hi1, /*tight=*/true);
      const double slack = 1e-6 * glm::length(hi0 - lo0) + 1e-6;
      for (int a = 0; a < 3; a++) {
        const double past = std::max(lo0[a] - lo1[a], hi1[a] - hi0[a]);
        if (past > slack) {
          SetError("shell grew the body " + std::to_string(past) +
                   " mm past its own outside along " +
                   std::string(1, "xyz"[a]) +
                   ": OpenCASCADE offset a face the wrong way. A thinner "
                   "wall, or another open face, usually builds");
          return false;
        }
      }
    }
    // Topology is reorganized; give the result fresh primitive-style names so
    // it stays referenceable (a later fillet picks from these).
    // Names through the builder's own history: a face a shell leaves alone
    // keeps its name, one it moved keeps it with ";M<id>", one it made from
    // an edge or a face is named by that ";G<id>". Fresh "<id>/faceN" names
    // numbered the faces in the order the builder happened to hand them
    // back, which changed from one run to the next: a draft on a shelled
    // body's inner wall picked a different face after a rebuild from nothing
    // (tools/fuzz.py). Fresh names only if no history is to be had.
    ElementMap map;
    {
      TopTools_ListOfShape args = topo::HistoryArguments(shape);
      const Handle(BRepTools_History) history =
          new BRepTools_History(args, maker);
      if (!history.IsNull())
        map = topo::NamesThroughHistory(body->shape, body->elementMap, *history,
                                        result, GetId());
      else
        topo::NamePrimitiveElements(result, std::to_string(GetId()), map);
    }
    body->shape = std::move(result);
    body->elementMap = std::move(map);
    return true;
  } catch (const Standard_Failure& e) {
    // An OCC message can be empty ("shell failed:" said nothing); its type
    // still says what threw.
    const char* said = e.GetMessageString();
    SetError(std::string("shell failed: OpenCASCADE threw ") +
             e.DynamicType()->Name() +
             (said != nullptr && *said ? std::string(" (") + said + ")" : "") +
             "; a thinner wall usually builds");
    LOG_ERROR("ShellFeature '{}': {}", m_name, GetError());
    return false;
  }
}
