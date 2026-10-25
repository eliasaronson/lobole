// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/ShellFeature.h"

#include "cad/OccPatches.h"
#include "cad/OccSignals.h"
#include "core/DirectEdit.h" // MakeDeadline, BuilderBudgetS
#include "core/Measure.h"    // InvalidReasons
#include "core/ModelSpace.h"
#include "core/features/OffsetFaceFeature.h" // WallsUnder

#include "common/Logger.h"
#include "core/ElementHistory.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepTools_History.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Type.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

namespace {

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

// The offset builder's tolerance, under the wall it builds: the 1e-3 it
// always had for an ordinary wall, a hundredth of one under 0.1 mm.
double OffsetTolerance(double thickness) {
  return std::min(1.0e-3, 1.0e-2 * thickness);
}

// One build of the shell of `body` from `input`, a copy of the body's shape.
// Empty on success with `out` and `map` set, else why it failed.
std::string ShellOnce(const Body& body, const TopoDS_Shape& input,
                      const TopTools_ListOfShape& remove, double thickness,
                      const Message_ProgressRange& range, uint64_t featureTag,
                      std::optional<CADShape>& out, ElementMap& map) {
  try {
    // Negative offset hollows inward, leaving a wall of `thickness`; the
    // selected faces are removed (open shell).
    BRepOffsetAPI_MakeThickSolid maker;
    // In an OCC signal scope (cad/OccSignals.h): the offset builder faults
    // rather than failing on walls it cannot make.
    if (const auto why = occsig::RunCatchingOccSignals([&] {
          maker.MakeThickSolidByJoin(
              input, remove, -thickness, OffsetTolerance(thickness),
              BRepOffset_Skin, Standard_False, Standard_False, GeomAbs_Arc,
              Standard_False, range);
        }))
      return "shell failed inside OpenCASCADE (" + *why +
             "); a thinner wall usually builds";
    if (!maker.IsDone())
      return "shell failed (wall too thick for the geometry?)";
    CADShape result(maker.Shape());
    if (!result.IsValid() || result.Volume() < 1e-9)
      return "shell produced no solid (an empty result); a thinner wall "
             "usually builds";
    if (const std::string bad = Measure::InvalidReasons(result); !bad.empty())
      return "shell produced an invalid solid (" + bad +
             "); a thinner wall usually builds";
    // OCC can report success and hand back the INPUT solid (a wall thicker
    // than the geometry allows, or an offset that self-intersects around a
    // hole). Opening a face always removes material, so a result that did
    // not shrink is a silent failure — and it used to commit as a no-op
    // feature that looked fine in the tree.
    // Two different failures, told apart: the input handed back unchanged,
    // and a result that GREW (an offset that went outward somewhere).
    const double before = body.shape.Volume(), after = result.Volume();
    if (std::abs(after - before) <= 1e-9 * std::max(1.0, before))
      return "shell had no effect: OpenCASCADE handed the body back "
             "unchanged (" +
             Mm(before) + " mm3) for a " + Mm(thickness) +
             " mm wall; a thinner wall, or another open face, usually builds";
    if (after > before)
      return "shell grew the body from " + Mm(before) + " to " + Mm(after) +
             " mm3 for a " + Mm(thickness) +
             " mm wall: an offset went outward. A thinner wall, or another "
             "open face, usually builds";
    // A shell hollows INWARD, so it can never reach past the body's own
    // outside. The offset builder sometimes moved a face outward instead (a
    // revolve with a bore in its floor, shelled 4.6 mm: its floor went 4.6
    // mm down) and the result was a valid, wrong solid (tools/fuzz.py).
    {
      glm::dvec3 lo0, hi0, lo1, hi1;
      body.shape.GetBoundingBox(lo0, hi0, /*tight=*/true);
      result.GetBoundingBox(lo1, hi1, /*tight=*/true);
      const double slack = 1e-6 * glm::length(hi0 - lo0) + 1e-6;
      for (int a = 0; a < 3; a++) {
        const double past = std::max(lo0[a] - lo1[a], hi1[a] - hi0[a]);
        if (past > slack)
          return "shell grew the body " + std::to_string(past) +
                 " mm past its own outside along " + std::string(1, "xyz"[a]) +
                 ": OpenCASCADE offset a face the wrong way. A thinner "
                 "wall, or another open face, usually builds";
      }
    }
    // Names through the builder's own history: a face a shell leaves alone
    // keeps its name, one it moved keeps it with ";M<id>", one it made from
    // an edge or a face is named by that ";G<id>". Fresh "<id>/faceN" names
    // numbered the faces in the order the builder happened to hand them
    // back, which changed from one run to the next: a draft on a shelled
    // body's inner wall picked a different face after a rebuild from nothing
    // (tools/fuzz.py). Built on a copy, the names come through the copy:
    // BRepBuilderAPI_Copy rebuilds the shape sub-shape for sub-shape, so the
    // copy's explorer order -- and with it every index-keyed name of the
    // body's map -- is the body's. (Through a history from the body to the
    // copy, every face the shell left alone read as modified, ";M<id>".)
    // Fresh names only if no history is to be had.
    {
      const Handle(BRepTools_History) history =
          new BRepTools_History(topo::HistoryArguments(input), maker);
      if (!history.IsNull())
        map = topo::NamesThroughHistory(CADShape(input), body.elementMap,
                                        *history, result, featureTag);
      else
        topo::NamePrimitiveElements(result, std::to_string(featureTag), map);
    }
    out = std::move(result);
    return {};
  } catch (const Standard_Failure& e) {
    // An OCC message can be empty ("shell failed:" said nothing); its type
    // still says what threw.
    const char* said = e.GetMessageString();
    return std::string("shell failed: OpenCASCADE threw ") +
           e.DynamicType()->Name() +
           (said != nullptr && *said ? std::string(" (") + said + ")" : "") +
           "; a thinner wall usually builds";
  }
}

} // namespace

ShellFeature::ShellFeature(const std::string& name, uint64_t bodyId,
                           std::vector<ElementRef> openFaces, double thickness)
    : bodyId(bodyId), openFaces(std::move(openFaces)), thickness(thickness) {
  m_name = name;
}

namespace {

// 1, 2 or 5 x 10^n at or under `raw`: the number a person would type.
double Round125Down(double raw) {
  const double decade = std::pow(10.0, std::floor(std::log10(raw)));
  const double mant = raw / decade;
  return decade * (mant >= 5.0 ? 5.0 : mant >= 2.0 ? 2.0 : 1.0);
}

// Why a wall of `thickness` cannot be shelled out of `body` with the faces
// `open` removed, naming the wall that would fit; empty when nothing here
// says it cannot (the kernel may still refuse). Under an open face the body
// is as thick as a line from it inward runs before it leaves the solid
// (directedit::WallsUnder), and the face the line leaves through moves
// toward the opening by the wall: a wall at or past that thickness breaks
// through. The offset builder took 5-42 s on a gear's flanks to fail, or
// "had no effect" on a body already thin-walled, whose bounding box is not
// its wall. Only under the open faces: where two kept faces face each other
// across less than two walls, the builder can still make a valid shell (a
// bore beside the outside merges into the cavity, tests/fuzz/cases/
// rebuild-139.jsonl), so that is the kernel's to answer.
std::string WallObjection(const Body& body, const std::vector<int>& open,
                          double thickness) {
  if (thickness <= 1e-9)
    return "shell thickness must be positive";
  // A wall at least half the body's thinnest extent leaves no cavity, and
  // BRepOffset given one far past it (1e308) crashed inside the offset
  // builder rather than failing (hostile_params_smoke).
  glm::dvec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi);
  const glm::dvec3 ext = hi - lo;
  const double thinnest = std::min({ext.x, ext.y, ext.z});
  if (!InModelSpace(thickness) || thickness >= 0.5 * thinnest)
    return "shell thickness " + Mm(thickness) +
           " mm is at least half the body's thinnest extent (" + Mm(thinnest) +
           " mm): a wall that thick leaves no cavity";
  const auto walls = directedit::WallsUnder(body, open);
  std::optional<double> limit;
  int at = -1;
  for (size_t i = 0; i < open.size(); i++) {
    if (!walls[i] ||
        std::find(open.begin(), open.end(), walls[i]->exitFace) != open.end())
      continue; // nothing kept across from it
    if (!limit || walls[i]->thickness < *limit) {
      limit = walls[i]->thickness;
      at = open[i];
    }
  }
  if (!limit || thickness < *limit - 1e-9)
    return {};
  const std::string* name = body.elementMap.NameOf(at);
  return "a shell wall of " + Mm(thickness) + " mm breaks through the open " +
         (name != nullptr ? "face '" + *name + "'" : "face") +
         ": the body is " + Mm(*limit) + " mm thick under it. A wall under " +
         Mm(*limit) + " mm (" + Mm(Round125Down(0.5 * *limit)) +
         " mm, say) fits";
}

} // namespace

std::optional<double> ThinnestWall(const Body& body) {
  std::vector<int> all(static_cast<size_t>(body.shape.FaceCount()));
  for (size_t i = 0; i < all.size(); i++)
    all[i] = static_cast<int>(i);
  std::optional<double> least;
  for (const auto& w : directedit::WallsUnder(body, all))
    if (w && (!least || w->thickness < *least))
      least = w->thickness;
  return least;
}

std::string ShellFeature::WhyRefusedOn(const std::vector<Body>& input,
                                       const Document& doc) const {
  (void)doc;
  if (openFaces.empty())
    return {};
  for (const ElementRef& ref : openFaces)
    if (ref.featureId != bodyId)
      return "all open faces must belong to the shelled body";
  std::string why;
  const Body* body = topo::FindOwningBodyOfAll(
      input, openFaces, topo::ElementKind::Face, "open face", &why);
  if (body == nullptr)
    return why;
  std::vector<int> open;
  for (const ElementRef& ref : openFaces)
    open.push_back(*body->elementMap.Resolve(ref.name));
  return WallObjection(*body, open, thickness);
}

bool ShellFeature::Execute(RegenContext& ctx) {
  for (const ElementRef& ref : openFaces) {
    if (ref.featureId != bodyId) {
      SetError("all open faces must belong to the shelled body");
      return false;
    }
  }
  // Shell needs at least one open face to hollow through (the standard shell
  // semantics in commercial CAD). A sealed hollow shell is a v2 (a manual
  // offset+cut).
  if (openFaces.empty()) {
    SetError(std::none_of(ctx.bodies.begin(), ctx.bodies.end(),
                          [&](const Body& b) { return b.createdBy == bodyId; })
                 ? "the body to shell no longer exists"
                 : "shell needs at least one open face to remove");
    return false;
  }
  // The body that holds the open faces: a pattern's copies and a split's
  // pieces share the createdBy, and the first of them is not always it.
  std::string why;
  Body* const body = topo::FindOwningBodyOfAll(
      ctx, openFaces, topo::ElementKind::Face, "open face", &why);
  if (body == nullptr) {
    SetError(why);
    return false;
  }
  // Collect the faces to remove by their stable names -> current indices ->
  // TopoDS_Face (TopExp_Explorer 0-based order, the ElementMap convention).
  std::vector<int> removeIdx;
  for (const ElementRef& ref : openFaces)
    removeIdx.push_back(*body->elementMap.Resolve(ref.name));
  if (std::string no = WallObjection(*body, removeIdx, thickness);
      !no.empty()) {
    SetError(no);
    return false;
  }
  const TopoDS_Shape& shape = body->shape.GetShape();
  TopTools_ListOfShape facesToRemove;
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), i++) {
    if (std::find(removeIdx.begin(), removeIdx.end(), i) != removeIdx.end())
      facesToRemove.Append(ex.Current());
  }

  // Stock OCC's offset builder answers by where its input lives in memory,
  // not only by what it is: MakeShells quilts the faces in the order of a map
  // hashed on shape ADDRESS, so a drilled box shelled through a bore's floor
  // came back an invalid solid of five times the volume one build in seven
  // (tests/fuzz/cases/rebuild-139.jsonl, and "A shell builds the same
  // wherever its input lives in memory"). Our builds carry the fix
  // (nix/patches/occt-shell-cap-order.patch); a build against an unpatched
  // OCC -- CI's Ubuntu job, apt's -- still draws. Every bad answer is caught
  // by the checks below, so a failed attempt is retried on a copy of the
  // input -- the same solid at new addresses -- and a shell is refused only
  // when every draw failed. `attempts` bounds what an honest refusal (a wall
  // too thick for the geometry) costs.
  //
  // The first attempt is on a copy too. The offset builder WRITES to its
  // input -- apt's 7.6 raised every edge of a body to its own 1e-3
  // tolerance, and did so on a shell it then refused -- and the body's shape
  // is the previous feature's cached result. A refused shell left the part
  // unlike a rebuild from nothing (tests/fuzz_seeds_smoke.py, on CI's
  // Ubuntu job). The copy is topology only; the geometry is shared.
  //
  // With the patch the draws are fewer -- the same input mostly gives the
  // same answer, and the four more draws of every honest refusal were a
  // fifth of an action-matrix sweep -- but not none: the arc joins'
  // offset faces were still registered in address order, and the
  // toolmaker's clamp's M3 screw built on some copies and not on others.
  // occt-offset-arc-bind-order.patch (upstream's OCCT#1600) fixes that
  // order too, and with it a copy answers as the original does (one answer
  // over 100 draws of each of the screw's eight shells, where six had up to
  // six), so a second draw would only repeat the first. The offset
  // builder's Intersection mode is not a draw: on the matrix's shells it
  // built 106 the arc joins refused, but the one it built on random design
  // 33 left a body the next cut could not cut (it added material), so a
  // refusal is the better answer.
  //
  // Every draw shares one budget, the direct edits' (directedit::
  // BuilderBudgetS, by the faces the offset moves -- all of them): the
  // offset builder stops at its progress checks. A 0.02 mm wall on the
  // worm (golden/v146-worm) ran minutes past the budget inside one
  // edge/face intersection that never looked at it; our kernel asks there
  // too (occt-offset-inter3d-progress.patch).
  const int attempts = occpatch::Carries("occt-offset-arc-bind-order") ? 1
                       : occpatch::Carries("occt-shell-cap-order")     ? 2
                                                                       : 5;
  const double budget =
      directedit::BuilderBudgetS(static_cast<size_t>(body->shape.FaceCount()));
  const Handle(Message_ProgressIndicator) deadline =
      directedit::MakeDeadline(budget);
  std::string error;
  for (int attempt = 0; attempt < attempts; attempt++) {
    BRepBuilderAPI_Copy copy(shape, /*copyGeom=*/false, /*copyMesh=*/false);
    TopTools_ListOfShape remove;
    for (const TopoDS_Shape& f : facesToRemove)
      remove.Append(copy.ModifiedShape(f));
    std::optional<CADShape> result;
    ElementMap map;
    std::string why = ShellOnce(*body, copy.Shape(), remove, thickness,
                                deadline->Start(), GetId(), result, map);
    if (!why.empty() && directedit::Expired(deadline)) {
      error = "shell gave up after " + std::to_string(int(budget)) +
              " s: offsetting this body's " +
              std::to_string(body->shape.FaceCount()) + " faces by a wall of " +
              Mm(thickness) +
              " mm costs more; a thicker wall, or fewer faces, builds faster";
      break;
    }
    if (why.empty()) {
      if (attempt > 0)
        LOG_INFO("ShellFeature '{}': built on attempt {} of {}", m_name,
                 attempt + 1, attempts);
      body->shape = std::move(*result);
      body->elementMap = std::move(map);
      return true;
    }
    error = std::move(why);
  }
  SetError(error);
  LOG_ERROR("ShellFeature '{}': {} ({} attempts)", m_name, error, attempts);
  return false;
}
