#include "ui/DrawingActions.h"
#include "ui/DocumentSlots.h"
#include "ui/RendererSync.h"

#include "core/Units.h"
#include "core/sheet/Unfold.h"     // sheet::SquareTo (bend lines)
#include "ui/AppActionsInternal.h" // SharedBody / PartRenderCacheStore
#include "ui/BackgroundJobs.h"
#include "ui/CamPanel.h" // RefreshCamToolpaths (job undo)
#include "ui/CamTool.h"  // RunCamVerification (job undo)
#include "ui/DrawingJob.h"
#include "ui/JobActions.h"

#include "imgui.h" // IsAnyItemActive (the undo guard coalesces a drag)         // JobBodies / JobRenderVersion (a job tab)
#include <chrono>
#include <utility>

#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/ElementHistory.h" // topo::ResolveEdgeOnBody (blend edge picks)
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/ThreadStandards.h"
#include "core/drawing/DrawingBalloons.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/drawing/DrawingSection.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/FrameFeature.h"
#include "core/fem/FemTopo.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectStamp.h"
#include "core/io/RecentProjects.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"
#include <BRepAdaptor_Surface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Cylinder.hxx>

#include "core/features/DraftFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/AssemblyActions.h"
#include "ui/CamPlayback.h"   // SyncCamPlayback (SyncRenderersToDocument)
#include "ui/FastenerTool.h"  // SyncFastenerGhost (SyncRenderersToDocument)
#include "ui/FeatureEditor.h" // PickSlot (thread face slot)
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (Hole select-first)
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include "thirdparty/stb_image.h" // stbi_info_from_memory (impl in IBL.cpp)

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

// Projecting a drawing view: gathering the source bodies, running the
// projection, placing the view on the sheet, resolving its dimensions and
// exporting the result.
//
// DrawingPanel.cpp is the other half -- the sheet as the user manipulates
// it. This is what a view IS; that is how it is edited.
// Declared in ui/DrawingActions.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// ---- Drawing view projection (workstream 13, D2) --------------------------

namespace {

// One body of a view's source document, ready to project. `input.sourceId` is
// a slot index into the gathered list (how ProjectView tags each curve so the
// join finds this entry); `refId` is the SEMANTIC source a dimension attaches
// to — a body's createdBy for a part, or the occurrence id for an assembly.
struct SourceBody {
  drawing::InputBody input;
  CADShape shape;         // the document's own, whose copy the worker gets
  uint64_t featureId = 0; // the body's createdBy (owner of its element names)
  uint64_t refId = 0;     // semantic source id -> SheetCurve.sourceId
  const ElementMap* map = nullptr;
  const std::vector<BendRegion>* bends = nullptr;   // flat creases to draw
  const std::vector<ThreadMark>* threads = nullptr; // cosmetic threads
  TopoDS_Shape whole; // the body before any section cut (its element map's)
  // A section view's cut of this body: says which source edge or face each
  // element of the projected cut is.
  std::shared_ptr<const drawing::SectionCut> cut;
};

// Appends one part document's bodies at the given world placement, less any
// whose creating feature is in `hidden` (a part view's `hidden`; null for an
// assembly's occurrences, which leave whole parts off before this).
void AppendBodies(const Document& doc, const glm::dmat4& xf, uint64_t occId,
                  std::vector<SourceBody>& out,
                  const std::vector<uint64_t>* hidden = nullptr) {
  for (const Body& body : doc.GetBodies()) {
    if (body.shape.GetShape().IsNull())
      continue;
    // Left off this sheet: the ground under a house, a fixture beside the
    // casting (docs/drawings/README.md#leaving-bodies-off-a-view).
    if (hidden != nullptr && std::find(hidden->begin(), hidden->end(),
                                       body.createdBy) != hidden->end())
      continue;
    // A Derived ghost is not the part: a drawing of the gripper body is a
    // drawing of the body, not of the servo it was drawn on (and the HLR of
    // a 197-body camera ghost took the view past a minute).
    if (body.reference)
      continue;
    SourceBody sb;
    sb.input.shape = body.shape.GetShape();
    sb.shape = body.shape;
    sb.input.transform = xf;
    sb.input.geometryRevision = doc.Version();
    sb.input.sourceId = out.size(); // slot index: picks this entry on join
    sb.featureId = body.createdBy;
    sb.refId = occId ? occId : body.createdBy; // occurrence, else body owner
    sb.input.groupId =
        sb.refId; // what a cost is charged to, and `hidden` names
    sb.map = &body.elementMap;
    sb.bends = &body.bends;
    sb.threads = &body.threads;
    sb.whole = sb.input.shape;
    out.push_back(std::move(sb));
  }
}

// Gathers the projectable bodies of a drawing view's source document — a part
// (identity placement) or an assembly (each visible occurrence's bodies at its
// solved transform, tagged with the occurrence id). Returns false and sets
// *error when the source can't be resolved.
bool GatherSourceBodies(AppState& state, const DrawingView& view,
                        std::vector<SourceBody>& out, uint64_t* sourceVersion,
                        std::string* error) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].id != view.sourceDocId)
      continue;
    if (state.parts[i].kind == DocKind::Part) {
      if (!SlotDoc(state, i)) {
        *error = "source document is empty";
        return false;
      }
      // The configured copy lives in AppState's cache, so the element maps
      // and bends `out` points into outlive the projection.
      const Document* doc = DrawingViewDocument(state, view, error);
      if (!doc)
        return false;
      *sourceVersion = SlotDoc(state, i)->Version();
      AppendBodies(*doc, glm::dmat4(1.0), 0, out, &view.hidden);
      return true;
    }
    if (view.configuration != 0) {
      *error = "a configuration can only be chosen for a view of a part";
      return false;
    }
    if (state.parts[i].kind == DocKind::Assembly) {
      const AssemblyDocument* asmDoc = SlotAssembly(state, i);
      if (!asmDoc) {
        *error = "source assembly is empty";
        return false;
      }
      *sourceVersion = asmDoc->Version();
      // A5: project the leaf parts of the whole tree. Every projected body is
      // stamped with its TOP-LEVEL occurrence id, not the leaf's — a leaf
      // inside a sub-assembly has an id belonging to the SUB's document,
      // which this drawing's dimension refs could never resolve. So a
      // sub-assembly dimensions as one unit, and no ref can dangle; to
      // dimension an inner part, draw that assembly.
      auto flat = assembly::Flatten(*asmDoc, PartResolverFor(state),
                                    AssemblyResolverFor(state));
      if (!flat) {
        *error = flat.error();
        return false;
      }
      for (const assembly::FlatInstance& inst : *flat) {
        if (!inst.visible)
          continue;
        // Left off this SHEET, which is not the same as hidden in the model:
        // `hidden` is per view, so one sheet can drop the springs and another
        // keep them (F4).
        if (std::find(view.hidden.begin(), view.hidden.end(),
                      inst.rootOccurrenceId) != view.hidden.end())
          continue;
        AppendBodies(*inst.part, inst.world, inst.rootOccurrenceId, out);
      }
      return true;
    }
    *error = "a drawing cannot be a source for another drawing";
    return false;
  }
  *error = "source document not found";
  return false;
}

// The current version of whatever document a view projects (a part or an
// assembly); 0 when it cannot be resolved, which forces a re-projection.
uint64_t SourceVersionOf(AppState& state, const DrawingView& view) {
  if (Document* doc = PartDocById(state, view.sourceDocId))
    return doc->Version();
  for (const PartSlot& slot : state.parts) {
    if (slot.id != view.sourceDocId || slot.kind != DocKind::Assembly)
      continue;
    if (state.assembly && state.activePart < state.parts.size() &&
        state.parts[state.activePart].id == slot.id) {
      return state.assembly->Version();
    }
    if (slot.assembly)
      return slot.assembly->Version();
  }
  return 0;
}

drawing::SheetView AssembleSheetView(AppState& state, const DrawingView& view,
                                     const std::vector<SourceBody>& bodies,
                                     const drawing::ProjectionResult& proj);
void SectionDecorations(AppState& state, const DrawingView& view,
                        const std::vector<SourceBody>& bodies,
                        drawing::SheetView& sheet);
void ThreadDecorations(AppState& state, const DrawingView& view,
                       const std::vector<SourceBody>& bodies,
                       drawing::SheetView& sheet);

void ClipToDetail(const DrawingView& view, drawing::SheetView& sheet);
void DetailCircles(AppState& state, const DrawingView& view,
                   drawing::SheetView& sheet);

// ---- Projection off the frame
// (docs/drawings/README.md#projection-off-the-frame)

// The view as it is projected: a detail draws what its parent draws, at its
// own scale and place -- the parent's source, orientation, turn, section,
// hidden occurrences and configuration, read NOW, so an edit to the parent
// carries over.
std::optional<DrawingView> EffectiveView(const DrawingDocument& dwg,
                                         const DrawingView& asked,
                                         std::string* error) {
  if (!asked.detail.on)
    return asked;
  const DrawingView* parent = dwg.FindView(asked.detail.parentViewId);
  if (parent == nullptr) {
    *error = "the view this detail is of is gone";
    return std::nullopt;
  }
  if (parent->detail.on) {
    *error = "a detail is of a view, not of another detail";
    return std::nullopt;
  }
  DrawingView view = *parent;
  view.id = asked.id;
  view.name = asked.name;
  view.scale = asked.scale;
  view.scaleExpr.clear();
  view.position = asked.position;
  view.baseViewId = 0;
  view.detail = asked.detail;
  return view;
}

// The view whose projection `view` draws: its own, or a detail's parent's.
uint64_t ProjectedViewId(const DrawingView& view) {
  return view.detail.on ? view.detail.parentViewId : view.id;
}

// The worker's own copy of a body's shape (DrawingShapeMemo), made now when
// `create`; null when there is none and none may be made. A deep copy at no
// location, put where the document's is: a worker may cut, mesh and hash it
// while the frame meshes and picks the original.
std::optional<TopoDS_Shape> WorkerCopy(AppState& state, const CADShape& shape,
                                       uint64_t revision, bool create) {
  DrawingShapeMemo& memo = state.drawingShapes;
  const std::pair<const void*, uint64_t> key{shape.GeometryId(),
                                             shape.PlacementId()};
  auto it = memo.entries.find(key);
  if (it != memo.entries.end() && it->second.revision == revision &&
      it->second.original.SameGeometryAs(shape)) {
    it->second.used = ++memo.clock;
    return it->second.copy.GetShape();
  }
  if (!create)
    return std::nullopt;
  DrawingShapeMemo::Entry e;
  e.original = shape;
  e.copy = shape.DetachedUnlocated().LocatedLike(shape);
  e.revision = revision;
  e.used = ++memo.clock;
  if (!e.copy.IsValid())
    return std::nullopt;
  memo.entries[key] = e;
  return e.copy.GetShape();
}

// Lets go of the copies whose original only the memo still holds (the
// document moved on), and of the least recently used past a bound.
void PruneShapeMemo(AppState& state) {
  DrawingShapeMemo& memo = state.drawingShapes;
  std::erase_if(memo.entries, [](const auto& kv) {
    const TopoDS_Shape& s = kv.second.original.GetShape();
    return s.IsNull() || s.TShape().IsNull() || s.TShape()->GetRefCount() <= 1;
  });
  constexpr size_t kMaxShapes = 4096;
  while (memo.entries.size() > kMaxShapes)
    memo.entries.erase(std::min_element(memo.entries.begin(),
                                        memo.entries.end(),
                                        [](const auto& a, const auto& b) {
                                          return a.second.used < b.second.used;
                                        }));
}

// One view, ready to project or to assemble: its bodies as the document has
// them (for the join, main thread only) and its projection's inputs, with
// the worker's copies when `copies` says they are all in hand.
struct PreparedView {
  DrawingView view; // effective (a detail is its parent's)
  std::vector<SourceBody> bodies;
  drawing::ProjectionInputs inputs;
  bool copies = false;
  uint64_t sourceVersion = 0;
  std::string error; // why it cannot be projected at all
};

// Fills `pv.inputs.bodies` with the worker's copies; false when one is not
// in hand and may not be made.
bool FillCopies(AppState& state, PreparedView& pv, bool create) {
  pv.inputs.bodies.clear();
  pv.copies = false;
  for (size_t i = 0; i < pv.bodies.size(); ++i) {
    const SourceBody& sb = pv.bodies[i];
    const auto copy =
        WorkerCopy(state, sb.shape, sb.input.geometryRevision, create);
    if (!copy)
      return false;
    drawing::InputBody in = sb.input;
    in.shape = *copy;
    in.sourceId = i;
    pv.inputs.bodies.push_back(std::move(in));
  }
  pv.copies = true;
  return true;
}

PreparedView PrepareView(AppState& state, const DrawingView& asked,
                         bool createCopies) {
  PreparedView pv;
  const auto eff = EffectiveView(*state.drawing, asked, &pv.error);
  if (!eff)
    return pv;
  pv.view = *eff;
  if (!GatherSourceBodies(state, pv.view, pv.bodies, &pv.sourceVersion,
                          &pv.error))
    return pv;
  if (pv.bodies.empty()) {
    pv.error = "source has no geometry to project";
    return pv;
  }
  ViewAxes(pv.view, pv.inputs.viewDir, pv.inputs.up);
  pv.inputs.mode = pv.view.quality == "fast" ? drawing::ProjectionMode::Poly
                                             : drawing::ProjectionMode::Exact;
  pv.inputs.hiddenLines = pv.view.showHidden;
  pv.inputs.section = pv.view.section.on;
  pv.inputs.sectionPoint = pv.view.section.point;
  FillCopies(state, pv, createCopies);
  return pv;
}

// The sheet view of `pv` drawn from `vp`: its curves turned to the view's
// up, placed, named through the bodies the document has now, decorated.
// Not `named` (the source's bodies are not the ones it was projected from),
// the curves are drawn as they were projected and name nothing.
drawing::SheetView AssembleFromProjection(AppState& state,
                                          const PreparedView& pv,
                                          const drawing::ViewProjection& vp,
                                          bool named = true) {
  drawing::SheetView fail;
  fail.viewId = pv.view.id;
  std::vector<SourceBody> bodies;
  if (!named) {
    // nothing to join against
  } else if (vp.sectioned) {
    for (size_t k = 0; k < vp.cuts.size(); ++k) {
      const drawing::ViewProjection::Cut& c = vp.cuts[k];
      if (c.body < 0 || static_cast<size_t>(c.body) >= pv.bodies.size()) {
        fail.error = "the projection names a body its source no longer has; "
                     "update the view";
        return fail;
      }
      SourceBody sb = pv.bodies[c.body];
      auto cut = std::make_shared<drawing::SectionCut>();
      cut->edgeOrigin = c.edgeOrigin;
      cut->edgeFromFace = c.edgeFromFace;
      cut->faceOrigin = c.faceOrigin;
      cut->cutLoops = c.cutLoops;
      sb.cut = std::move(cut);
      sb.input.sourceId = k;
      bodies.push_back(std::move(sb));
    }
  } else {
    bodies = pv.bodies;
  }
  for (const drawing::ProjectedCurve& c : vp.result.curves)
    if (named && c.source.IsResolved() && c.source.sourceId >= bodies.size()) {
      fail.error = "the projection names a body its source no longer has; "
                   "update the view";
      return fail;
    }
  glm::dvec3 dir, up;
  ViewAxes(pv.view, dir, up);
  const drawing::ProjectionResult turned = drawing::TurnedTo(vp, up);
  drawing::SheetView sheet = AssembleSheetView(state, pv.view, bodies, turned);
  sheet.sectioned = pv.view.section.on;
  SectionDecorations(state, pv.view, bodies, sheet);
  ThreadDecorations(state, pv.view, bodies, sheet);
  if (pv.view.detail.on)
    ClipToDetail(pv.view, sheet);
  else
    DetailCircles(state, pv.view, sheet);
  return sheet;
}

// A view that could not be (re)drawn keeps its last geometry with `error`
// (design §1): a failed view shows its previous curves rather than blanking
// the sheet.
void SetViewError(std::map<uint64_t, drawing::SheetView>& renders,
                  uint64_t viewId, const std::string& error,
                  uint64_t sourceVersion) {
  auto it = renders.find(viewId);
  if (it != renders.end() && it->second.hasGeometry) {
    it->second.error = error;
    it->second.solvedSourceVersion = sourceVersion;
    return;
  }
  drawing::SheetView sv;
  sv.viewId = viewId;
  sv.error = error;
  sv.solvedSourceVersion = sourceVersion;
  renders[viewId] = std::move(sv);
}

// Whether two views project the same thing from the same source: what
// decides whether a kept projection may still be drawn (stale) after the
// source moved. Scale, place, turn and name are the sheet's, not the
// projection's.
bool SameProjectedDefinition(const DrawingView& a, const DrawingView& b) {
  return a.sourceDocId == b.sourceDocId && a.orientation == b.orientation &&
         a.section.on == b.section.on &&
         (!a.section.on || a.section.point == b.section.point) &&
         a.hidden == b.hidden && a.configuration == b.configuration &&
         a.quality == b.quality && a.showHidden == b.showHidden;
}

// Assembles the active drawing's `viewId` from what it keeps (a detail from
// its parent's). Fresh when the view would project the very same inputs now;
// otherwise, with `allowStale`, drawn as it was projected and marked stale --
// provided it still looks the same way at the same bodies, so its curves'
// names still join. False when there is nothing to draw it from. `landed`:
// its projection just ran, so the view carries what it cost; otherwise it
// reports what assembling it spent on projecting, which is nothing (a hit).
bool AssembleKeptView(AppState& state, uint64_t viewId, bool allowStale,
                      bool landed = false) {
  const DrawingView* asked = state.drawing->FindView(viewId);
  if (asked == nullptr)
    return false;
  const auto kept = state.drawingProjections.kept.find(ProjectedViewId(*asked));
  if (kept == state.drawingProjections.kept.end())
    return false;
  PreparedView pv = PrepareView(state, *asked, /*createCopies=*/false);
  if (!pv.error.empty())
    return false;
  const DrawingProjectionState::Kept& k = kept->second;
  const bool fresh = pv.copies && k.inputs.SameAs(pv.inputs);
  if (!fresh) {
    if (!allowStale || !SameProjectedDefinition(pv.view, k.view) ||
        pv.inputs.viewDir != k.inputs.viewDir)
      return false;
  }
  // Stale, and the source no longer has the bodies it was projected from:
  // drawn as it was, naming nothing, until it is brought up to date.
  const bool named = fresh || pv.bodies.size() == k.inputs.bodies.size();
  drawing::SheetView sv =
      AssembleFromProjection(state, pv, *k.projection, named);
  sv.solvedSourceVersion = fresh ? pv.sourceVersion : k.sourceVersion;
  if (!landed) {
    drawing::ProjectionResult::Stats& st = sv.projectionStats;
    const int edges = st.dsEdges, unresolved = st.unresolvedSources;
    st = {};
    st.dsEdges = edges;
    st.unresolvedSources = unresolved;
    st.projectionHit = true;
    sv.updateMs = sv.hideMs = 0.0;
    sv.costBySource.clear();
  }
  if (!sv.error.empty()) {
    SetViewError(state.drawingRenders, viewId, sv.error,
                 sv.solvedSourceVersion);
    return true;
  }
  state.drawingRenders[viewId] = std::move(sv);
  return true;
}

// The active drawing's views sorted into the ones assembled now from what
// they keep and the work still to project. A view whose source cannot even
// be gathered lands its error now; a detail is assembled from its parent's
// projection, or waits for it. `only` (when set) limits the projecting to
// those views: the others are assembled as they are, stale or not. Views a
// run is projecting are left to it.
std::vector<DrawingViewWork>
PlanDrawingUpdate(AppState& state, const std::set<uint64_t>* only, bool* owed) {
  DrawingDocument& dwg = *state.drawing;
  DrawingProjectionState& ps = state.drawingProjections;
  std::vector<DrawingViewWork> work;
  std::set<uint64_t> projecting;
  for (const DrawingView& view : dwg.GetViews()) {
    if (view.detail.on)
      continue;
    if (ps.projecting.contains(view.id)) {
      if (owed)
        *owed = true;
      continue;
    }
    const bool wanted = only == nullptr || only->contains(view.id);
    if (AssembleKeptView(state, view.id, /*allowStale=*/!wanted))
      continue;
    if (!wanted) {
      if (!state.drawingRenders.contains(view.id))
        SetViewError(state.drawingRenders, view.id,
                     "not projected yet: Update views", 0);
      continue;
    }
    if (owed)
      *owed = true;
    PreparedView pv = PrepareView(state, view, /*createCopies=*/true);
    if (!pv.error.empty()) {
      SetViewError(state.drawingRenders, view.id, pv.error, pv.sourceVersion);
      ps.placeholder.erase(view.id);
      continue;
    }
    if (!pv.copies) {
      SetViewError(state.drawingRenders, view.id,
                   "a body of the source could not be copied to project",
                   pv.sourceVersion);
      continue;
    }
    DrawingViewWork w;
    w.viewId = view.id;
    w.name = view.name;
    w.inputs = std::move(pv.inputs);
    w.view = pv.view;
    w.candidate = dwg.SavedProjection(view.id);
    w.sourceVersion = pv.sourceVersion;
    work.push_back(std::move(w));
    projecting.insert(view.id);
  }
  for (const DrawingView& view : dwg.GetViews()) {
    if (!view.detail.on)
      continue;
    const uint64_t parent = view.detail.parentViewId;
    if (projecting.contains(parent) || ps.projecting.contains(parent))
      continue; // lands with its parent
    if (AssembleKeptView(state, view.id, /*allowStale=*/true))
      continue;
    std::string error;
    if (!EffectiveView(dwg, view, &error))
      SetViewError(state.drawingRenders, view.id, error, 0);
    else
      SetViewError(state.drawingRenders, view.id,
                   "the view this detail is of has no projection yet", 0);
  }
  // What the sheet no longer has.
  const auto gone = [&](uint64_t id) { return dwg.FindView(id) == nullptr; };
  std::erase_if(state.drawingRenders,
                [&](const auto& kv) { return gone(kv.first); });
  std::erase_if(ps.kept, [&](const auto& kv) { return gone(kv.first); });
  std::erase_if(ps.placeholder, [&](const auto& kv) { return gone(kv.first); });
  return work;
}

// The source's bounds seen along `view`, at 1:1 in the view plane: what the
// sheet draws while a view with no geometry projects, and what placing a new
// view starts from. The conservative box of each body, so a little large.
bool EstimateViewBox(AppState& state, const DrawingView& asked, glm::dvec2& lo,
                     glm::dvec2& hi) {
  std::string error;
  const auto eff = EffectiveView(*state.drawing, asked, &error);
  if (!eff)
    return false;
  std::vector<SourceBody> bodies;
  uint64_t version = 0;
  if (!GatherSourceBodies(state, *eff, bodies, &version, &error) ||
      bodies.empty())
    return false;
  glm::dvec3 dir, up;
  ViewAxes(*eff, dir, up);
  lo = glm::dvec2(1e300);
  hi = glm::dvec2(-1e300);
  for (const SourceBody& sb : bodies) {
    glm::dvec3 mn, mx;
    sb.shape.GetBoundingBox(mn, mx);
    for (int c = 0; c < 8; ++c) {
      const glm::dvec3 corner((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y,
                              (c & 4) ? mx.z : mn.z);
      const glm::dvec3 w(sb.input.transform * glm::dvec4(corner, 1.0));
      const glm::dvec2 p = drawing::ProjectPoint(w, dir, up);
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
  }
  return hi.x >= lo.x && hi.y >= lo.y;
}

// The cheap half of a view: place the projected curves on the sheet at the
// view's scale and folded position, and join the projection's index-space
// SourceRefs to stable ElementRefs.
//
// Split from the HLR so the expensive half can run OFF the main thread: this
// part needs the live documents (their ElementMaps) and the sheet layout, and
// the part it is split from needs neither
// (docs/drawings/README.md#projection-hlr).
drawing::SheetView
AssembleSheetView(AppState& state, const DrawingView& view,
                  const std::vector<SourceBody>& bodies,
                  const drawing::ProjectionResult& projection) {
  drawing::SheetView result;
  result.viewId = view.id;
  result.solvedSourceVersion = SourceVersionOf(state, view);
  result.updateMs = projection.stats.updateMs;
  result.hideMs = projection.stats.hideMs;
  result.dsEdges = projection.stats.dsEdges;
  result.projectionStats = projection.stats;
  result.bodies = static_cast<int>(bodies.size());
  // Already grouped by refId (InputBody::groupId), dearest first.
  result.costBySource = projection.stats.costBySource;
  std::sort(result.costBySource.begin(), result.costBySource.end(),
            [](const auto& a, const auto& b) { return a.ms > b.ms; });
  const drawing::ProjectionResult* projected = &projection;

  // Map an index-space SourceRef back to a stable ElementRef via the owning
  // body's ElementMap (sourceId is the body index in `bodies`).
  const auto bodyOf = [&](uint64_t sourceId) -> const SourceBody* {
    return sourceId < bodies.size() ? &bodies[sourceId] : nullptr;
  };

  const double s = view.scale != 0.0 ? view.scale : 1.0;

  // Folding alignment (the one computation lives in DrawingFoldedPosition).
  const glm::dvec2 effectivePos = DrawingFoldedPosition(state, view).pos;

  glm::dvec2 lo(1e30), hi(-1e30);
  for (const drawing::ProjectedCurve& pc : projected->curves) {
    drawing::SheetCurve sc;
    sc.kind = pc.kind;
    sc.visible = pc.visible;
    sc.curveType = pc.curveType;
    sc.closed = pc.closed;
    if (pc.conicRadius > 0.0) { // place the conic on the sheet too
      sc.conicCenter = effectivePos + s * pc.conicCenter;
      sc.conicRadius = s * pc.conicRadius;
    }
    sc.points.reserve(pc.points.size());
    for (const glm::dvec2& p : pc.points) {
      const glm::dvec2 sheet = effectivePos + s * p;
      sc.points.push_back(sheet);
      lo = glm::min(lo, sheet);
      hi = glm::max(hi, sheet);
    }
    if (const SourceBody* sb = bodyOf(pc.source.sourceId)) {
      sc.sourceId = sb->refId; // occurrence id (assembly) or body owner (part)
      int edgeIndex = pc.source.edgeIndex, faceIndex = pc.source.faceIndex;
      if (sb->cut) {
        // A section's elements are the cut's: name them by the source
        // element each one is, and an edge the plane made by the face it
        // was cut out of -- the wall of a bore in section, named like its
        // silhouette would be.
        const drawing::SectionCut& cut = *sb->cut;
        const int e = edgeIndex, f = faceIndex;
        edgeIndex = faceIndex = -1;
        if (e >= 0 && e < static_cast<int>(cut.edgeOrigin.size())) {
          edgeIndex = cut.edgeOrigin[e];
          if (edgeIndex < 0)
            faceIndex = cut.edgeFromFace[e];
        }
        if (f >= 0 && f < static_cast<int>(cut.faceOrigin.size()))
          faceIndex = cut.faceOrigin[f];
      }
      if (edgeIndex >= 0 && sb->map) {
        if (const std::string* n = sb->map->EdgeNameOf(edgeIndex))
          sc.edge = ElementRef{sb->featureId, *n};
      }
      if (faceIndex >= 0 && sb->map) {
        if (const std::string* n = sb->map->NameOf(faceIndex))
          sc.silFace = ElementRef{sb->featureId, *n};
      }
    }
    result.curves.push_back(std::move(sc));
  }

  // Bend lines. A flat blank has no edge where a crease is, so nothing above
  // drew one; they come from the bends the features recorded, and only in a
  // view that looks square onto the blank — seen edge-on, a crease is a
  // point or lies along the sheet's own outline.
  glm::dvec3 viewDir, up;
  ViewAxes(view, viewDir, up);
  const glm::dvec3 d = glm::normalize(viewDir);
  for (const SourceBody& sb : bodies) {
    if (!sb.bends)
      continue;
    const glm::dmat4& xf = sb.input.transform;
    for (const BendRegion& bend : *sb.bends) {
      if (!bend.flat)
        continue;
      if (!sheet::SquareTo(bend, d, xf))
        continue;
      drawing::SheetCurve sc;
      sc.kind = drawing::CurveKind::Bend;
      sc.visible = true;
      sc.curveType = 0; // a line
      sc.sourceId = sb.refId;
      for (const glm::dvec3& p : {bend.flatA, bend.flatB}) {
        const glm::dvec3 w(xf * glm::dvec4(p, 1.0));
        const glm::dvec2 sheet =
            effectivePos + s * drawing::ProjectPoint(w, viewDir, up);
        sc.points.push_back(sheet);
        lo = glm::min(lo, sheet);
        hi = glm::max(hi, sheet);
      }
      result.curves.push_back(std::move(sc));
    }
  }

  if (!result.curves.empty()) {
    result.hasGeometry = true;
    result.bboxMin = lo;
    result.bboxMax = hi;
  }
  return result;
}

// What a section adds to the sheet (docs/drawings/README.md#section-views):
// on the section view itself, hatching over the faces the cut exposed and
// its caption; on any other view of the same source that sees the plane
// edge-on, the cutting-plane line, thick at its ends, with arrows in the
// direction the section looks and its letter at both.
void SectionDecorations(AppState& state, const DrawingView& view,
                        const std::vector<SourceBody>& bodies,
                        drawing::SheetView& sheet) {
  if (!sheet.hasGeometry || !state.drawing)
    return;
  glm::dvec3 viewDir, up;
  ViewAxes(view, viewDir, up);
  const double s = view.scale != 0.0 ? view.scale : 1.0;
  const glm::dvec2 pos = DrawingFoldedPosition(state, view).pos;
  const auto toSheet = [&](const glm::dvec3& world) {
    return pos + s * drawing::ProjectPoint(world, viewDir, up);
  };
  drawing::SheetGraphics& g = sheet.extras;

  if (view.section.on) {
    // One hatch per part, alternating direction, so two parts that touch
    // in the section read as two.
    std::vector<uint64_t> parts;
    for (const SourceBody& sb : bodies) {
      if (!sb.cut || sb.cut->cutLoops.empty())
        continue;
      size_t which =
          std::find(parts.begin(), parts.end(), sb.refId) - parts.begin();
      if (which == parts.size())
        parts.push_back(sb.refId);
      std::vector<std::vector<glm::dvec2>> loops;
      for (const auto& loop : sb.cut->cutLoops) {
        std::vector<glm::dvec2> pts;
        for (const glm::dvec3& p : loop)
          pts.push_back(
              toSheet(glm::dvec3(sb.input.transform * glm::dvec4(p, 1.0))));
        loops.push_back(std::move(pts));
      }
      constexpr double kHatchMM = 2.5, kPi = 3.14159265358979323846;
      const double angle = (which % 2 == 0) ? kPi / 4.0 : 3.0 * kPi / 4.0;
      for (const auto& [a, b] : drawing::HatchRegion(loops, kHatchMM, angle))
        g.lines.push_back({a, b, 0.18});
    }
    const std::string label =
        view.section.label.empty()
            ? std::string("SECTION")
            : "SECTION " + view.section.label + "-" + view.section.label;
    // A detail of a section is captioned as the detail it is.
    if (!view.detail.on)
      g.texts.push_back(
          {{0.5 * (sheet.bboxMin.x + sheet.bboxMax.x), sheet.bboxMin.y - 9.0},
           label,
           true,
           5.0});
  }

  for (const DrawingView& other : state.drawing->GetViews()) {
    if (other.id == view.id || !other.section.on ||
        other.sourceDocId != view.sourceDocId)
      continue;
    glm::dvec3 sDir, sUp;
    ViewAxes(other, sDir, sUp);
    sDir = glm::normalize(sDir);
    if (std::abs(glm::dot(sDir, glm::normalize(viewDir))) > 1e-6)
      continue; // this view does not see that plane edge-on
    // Once, on the view the section is projected from, when it folds from
    // one that sees the plane: the line on every view that could show it
    // drew it twice on a plate's top and underside.
    if (other.baseViewId != 0 && other.baseViewId != view.id)
      if (const DrawingView* base = state.drawing->FindView(other.baseViewId)) {
        glm::dvec3 bDir, bUp;
        ViewAxes(*base, bDir, bUp);
        if (std::abs(glm::dot(sDir, glm::normalize(bDir))) < 1e-6)
          continue;
      }
    const glm::dvec3 P = other.section.point;
    const glm::dvec2 p0 = toSheet(P);
    const glm::dvec3 along = glm::normalize(glm::cross(sDir, viewDir));
    const glm::dvec2 d = glm::normalize(toSheet(P + along) - p0);
    const glm::dvec2 sight = glm::normalize(toSheet(P + sDir) - p0);
    double t0 = 1e300, t1 = -1e300;
    for (const glm::dvec2& c : {sheet.bboxMin, sheet.bboxMax,
                                glm::dvec2(sheet.bboxMin.x, sheet.bboxMax.y),
                                glm::dvec2(sheet.bboxMax.x, sheet.bboxMin.y)}) {
      t0 = std::min(t0, glm::dot(c - p0, d));
      t1 = std::max(t1, glm::dot(c - p0, d));
    }
    constexpr double kOverrun = 6.0, kThick = 6.0, kShaft = 9.0;
    const glm::dvec2 e0 = p0 + d * (t0 - kOverrun);
    const glm::dvec2 e1 = p0 + d * (t1 + kOverrun);
    g.lines.push_back({e0, e1, 0.25, true});
    g.lines.push_back({e0, e0 + d * kThick, 0.7});
    g.lines.push_back({e1 - d * kThick, e1, 0.7});
    for (const glm::dvec2& e : {e0 + d * 2.0, e1 - d * 2.0}) {
      // The arrow stands on the viewer's side and points the way the section
      // looks, its tip on the line.
      const glm::dvec2 tail = e - sight * kShaft;
      g.lines.push_back({tail, e - sight * 2.6, 0.35});
      const glm::dvec2 across(-sight.y, sight.x);
      const glm::dvec2 base = e - sight * 3.0;
      g.fills.push_back({e, base + across * 1.0, base - across * 1.0});
      if (!other.section.label.empty())
        g.texts.push_back({tail - sight * 4.0 - glm::dvec2(0.0, 1.75),
                           other.section.label, true, 5.0});
    }
  }
}

// The ISO 6410 convention for a thread nothing modelled (a Thread feature's
// cosmetic thread, a Hole's tapped fit), drawn in thin line
// (docs/drawings/README.md#threads): seen down its axis, three quarters of a
// circle at the major diameter of a tapped hole (the minor of a screw) where
// the drilled rim shows; seen from the side, two lines along the threaded
// length -- solid where a section cuts the thread open, dashed where the
// hole is hidden, solid on a screw.
void ThreadDecorations(AppState& state, const DrawingView& view,
                       const std::vector<SourceBody>& bodies,
                       drawing::SheetView& sheet) {
  if (!sheet.hasGeometry)
    return;
  glm::dvec3 viewDir, up;
  ViewAxes(view, viewDir, up);
  const glm::dvec3 d = glm::normalize(viewDir);
  const double s = view.scale != 0.0 ? view.scale : 1.0;
  const glm::dvec2 pos = DrawingFoldedPosition(state, view).pos;
  const auto toSheet = [&](const glm::dvec3& world) {
    return pos + s * drawing::ProjectPoint(world, viewDir, up);
  };
  constexpr double kThin = 0.18, kPi = 3.14159265358979323846;
  drawing::SheetGraphics& g = sheet.extras;
  for (const SourceBody& sb : bodies) {
    if (!sb.threads || sb.threads->empty() || !sb.map || sb.whole.IsNull())
      continue;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(sb.whole, TopAbs_FACE, faces);
    // One per face: the last mark written says the most (a Thread on a
    // tapped Hole's bore comes after the Hole).
    std::map<int, const ThreadMark*> byFace;
    for (const ThreadMark& m : *sb.threads)
      if (m.face.featureId == sb.featureId && !m.modeled)
        if (const auto i = sb.map->Resolve(m.face.name))
          byFace[*i] = &m;
    for (const auto& [index, mark] : byFace) {
      if (index + 1 > faces.Extent())
        continue;
      try {
        const BRepAdaptor_Surface surf(TopoDS::Face(faces(index + 1)));
        if (surf.GetType() != GeomAbs_Cylinder)
          continue;
        const gp_Cylinder cyl = surf.Cylinder();
        const gp_Pnt l = cyl.Location();
        const gp_Dir a = cyl.Axis().Direction();
        const double faceR = cyl.Radius();
        double v0 = surf.FirstVParameter(), v1 = surf.LastVParameter();
        if (v1 < v0)
          std::swap(v0, v1);
        if (mark->length > 1e-9)
          v1 = std::min(v1, v0 + mark->length);
        const glm::dmat4& xf = sb.input.transform;
        const auto world = [&](double v) {
          return glm::dvec3(xf * glm::dvec4(l.X() + v * a.X(),
                                            l.Y() + v * a.Y(),
                                            l.Z() + v * a.Z(), 1.0));
        };
        const glm::dvec3 p0 = world(v0), p1 = world(v1);
        const glm::dvec3 axis = glm::normalize(p1 - p0);
        // The thin line's radius: a tapped hole's major, a screw's root.
        const double P = mark->pitch;
        const double major = mark->majorDia > 0.0 ? 0.5 * mark->majorDia
                             : mark->external     ? faceR
                                                  : faceR + 0.54 * P;
        const double thinR = mark->external ? major - 0.6134 * P : major;
        if (std::abs(glm::dot(axis, d)) > 0.999) {
          // Down the axis: only where the drilled rim (or the screw's end)
          // shows -- a thread behind the part is left off, as drafting does.
          const glm::dvec2 c = toSheet(p0);
          bool shows = false;
          for (const drawing::SheetCurve& sc : sheet.curves)
            shows = shows || (sc.visible && sc.conicRadius > 0.0 &&
                              glm::length(sc.conicCenter - c) < 1e-3 &&
                              std::abs(sc.conicRadius - s * faceR) <
                                  1e-3 * std::max(1.0, s * faceR));
          if (!shows)
            continue;
          // Three quarters, the gap in the upper right quadrant.
          constexpr int kSteps = 36;
          glm::dvec2 prev = c + s * thinR * glm::dvec2(-1.0, 0.0);
          for (int i = 1; i <= kSteps; ++i) {
            const double t = kPi + 1.5 * kPi * i / kSteps; // 180 -> 450 deg
            const glm::dvec2 q =
                c + s * thinR * glm::dvec2(std::cos(t), std::sin(t));
            g.lines.push_back({prev, q, kThin});
            prev = q;
          }
          continue;
        }
        if (std::abs(glm::dot(axis, d)) > 1e-3)
          continue; // oblique: no convention to draw
        // From the side: two lines `thinR` either side of the axis.
        const glm::dvec3 across = glm::normalize(glm::cross(axis, d));
        bool cutOpen = false;
        if (view.section.on)
          cutOpen = std::abs(glm::dot(p0 - view.section.point, d)) < faceR;
        const bool dashed = !mark->external && !cutOpen;
        if (dashed && !view.showHidden)
          continue;
        for (double side : {-1.0, 1.0}) {
          drawing::SheetGraphics::Line line{
              toSheet(p0 + across * (side * thinR)),
              toSheet(p1 + across * (side * thinR)), kThin};
          line.dashed = dashed;
          g.lines.push_back(line);
        }
      } catch (const Standard_Failure&) {
        continue; // an unreadable face draws no convention, not a wrong one
      }
    }
  }
}

// The parts of a polyline inside a circle, each as its own run of points.
std::vector<std::vector<glm::dvec2>>
ClipPolyline(const std::vector<glm::dvec2>& pts, const glm::dvec2& c,
             double R) {
  std::vector<std::vector<glm::dvec2>> out;
  std::vector<glm::dvec2> run;
  const auto inside = [&](const glm::dvec2& p) {
    return glm::length(p - c) <= R;
  };
  const auto flush = [&] {
    if (run.size() >= 2)
      out.push_back(run);
    run.clear();
  };
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const glm::dvec2 a = pts[i], b = pts[i + 1];
    // Where the segment meets the circle: |a + t(b - a) - c| = R.
    const glm::dvec2 d = b - a, f = a - c;
    const double A = glm::dot(d, d), B = 2.0 * glm::dot(f, d),
                 C = glm::dot(f, f) - R * R;
    double t0 = 0.0, t1 = 1.0;
    if (A < 1e-18) {
      if (!inside(a))
        flush();
      continue;
    }
    const double disc = B * B - 4.0 * A * C;
    if (disc <= 0.0) { // misses the circle (or grazes it)
      flush();
      continue;
    }
    const double sq = std::sqrt(disc);
    t0 = std::max(0.0, (-B - sq) / (2.0 * A));
    t1 = std::min(1.0, (-B + sq) / (2.0 * A));
    if (t0 >= t1) {
      flush();
      continue;
    }
    const glm::dvec2 p0 = a + d * t0, p1 = a + d * t1;
    if (run.empty() || glm::length(run.back() - p0) > 1e-9) {
      flush();
      run.push_back(p0);
    }
    run.push_back(p1);
    if (t1 < 1.0)
      flush(); // it left the circle
  }
  flush();
  return out;
}

// A detail keeps what lies inside its circle, draws the circle, and is
// captioned DETAIL B (5:1) under it (docs/drawings/README.md#detail-views).
void ClipToDetail(const DrawingView& view, drawing::SheetView& sheet) {
  const double s = view.scale != 0.0 ? view.scale : 1.0;
  const glm::dvec2 c = view.position; // where the circle's centre lands
  const double R = s * view.detail.radius;
  std::vector<drawing::SheetCurve> kept;
  for (drawing::SheetCurve& curve : sheet.curves) {
    bool whole = true;
    for (const glm::dvec2& p : curve.points)
      whole = whole && glm::length(p - c) <= R;
    if (whole) {
      kept.push_back(std::move(curve));
      continue;
    }
    for (auto& piece : ClipPolyline(curve.points, c, R)) {
      drawing::SheetCurve part = curve;
      part.points = std::move(piece);
      part.closed = false; // an arc of what the parent drew whole
      kept.push_back(std::move(part));
    }
  }
  sheet.curves = std::move(kept);
  drawing::SheetGraphics g;
  for (const auto& l : sheet.extras.lines)
    for (const auto& piece : ClipPolyline({l.a, l.b}, c, R)) {
      auto line = l;
      line.a = piece.front();
      line.b = piece.back();
      g.lines.push_back(line);
    }
  for (const auto& f : sheet.extras.fills) {
    bool in = true;
    for (const glm::dvec2& p : f)
      in = in && glm::length(p - c) <= R;
    if (in)
      g.fills.push_back(f);
  }
  for (const auto& t : sheet.extras.texts)
    if (glm::length(t.pos - c) <= R)
      g.texts.push_back(t);
  // The boundary, thin, and the caption under it.
  constexpr int kSteps = 96;
  constexpr double kPi = 3.14159265358979323846;
  for (int i = 0; i < kSteps; ++i) {
    const double a0 = 2.0 * kPi * i / kSteps, a1 = 2.0 * kPi * (i + 1) / kSteps;
    g.lines.push_back({c + R * glm::dvec2(std::cos(a0), std::sin(a0)),
                       c + R * glm::dvec2(std::cos(a1), std::sin(a1)), 0.25});
  }
  char ratio[32];
  if (s >= 1.0)
    std::snprintf(ratio, sizeof ratio, "%.3g:1", s);
  else
    std::snprintf(ratio, sizeof ratio, "1:%.3g", 1.0 / s);
  g.texts.push_back(
      {{c.x, c.y - R - 9.0},
       "DETAIL " + view.detail.label + " (" + std::string(ratio) + ")",
       true,
       5.0});
  sheet.extras = std::move(g);
  sheet.hasGeometry = !sheet.curves.empty();
  sheet.bboxMin = c - glm::dvec2(R);
  sheet.bboxMax = c + glm::dvec2(R);
}

// On a view that has details: each detail's circle, thin, lettered outside
// its upper right.
void DetailCircles(AppState& state, const DrawingView& view,
                   drawing::SheetView& sheet) {
  if (!state.drawing || !sheet.hasGeometry)
    return;
  glm::dvec3 viewDir, up;
  ViewAxes(view, viewDir, up);
  const double s = view.scale != 0.0 ? view.scale : 1.0;
  const glm::dvec2 pos = DrawingFoldedPosition(state, view).pos;
  constexpr int kSteps = 72;
  constexpr double kPi = 3.14159265358979323846;
  for (const DrawingView& d : state.drawing->GetViews()) {
    if (!d.detail.on || d.detail.parentViewId != view.id)
      continue;
    const glm::dvec2 c =
        pos + s * drawing::ProjectPoint(d.detail.center, viewDir, up);
    const double R = s * d.detail.radius;
    for (int i = 0; i < kSteps; ++i) {
      const double a0 = 2.0 * kPi * i / kSteps,
                   a1 = 2.0 * kPi * (i + 1) / kSteps;
      sheet.extras.lines.push_back(
          {c + R * glm::dvec2(std::cos(a0), std::sin(a0)),
           c + R * glm::dvec2(std::cos(a1), std::sin(a1)), 0.35});
    }
    sheet.extras.texts.push_back({c + (R + 3.5) * glm::dvec2(0.7071, 0.7071),
                                  d.detail.label, true, 5.0});
  }
}

} // namespace

FoldedPosition DrawingFoldedPosition(AppState& state, const DrawingView& view) {
  // A projected child is kept aligned to its base along the shared sheet
  // axis (design §2). Two orthographic views share sheet-X when they map the
  // same world direction to it (equal `right` vectors), and sheet-Y when
  // their `up` vectors match; the shared coordinate locks to the base's,
  // leaving the fold axis free. Iso shares neither -> placed freely.
  FoldedPosition out{view.position, false, false};
  if (view.detail.on) {
    // A detail is placed by where its circle's centre lands: the model
    // maps so that centre is at `position`. Its axes are its parent's.
    const DrawingView* parent =
        state.drawing ? state.drawing->FindView(view.detail.parentViewId)
                      : nullptr;
    glm::dvec3 vd, up;
    ViewAxes(parent ? *parent : view, vd, up);
    const double s = view.scale != 0.0 ? view.scale : 1.0;
    out.pos =
        view.position - s * drawing::ProjectPoint(view.detail.center, vd, up);
    return out;
  }
  if (!view.baseViewId || !state.drawing)
    return out;
  const DrawingView* base = state.drawing->FindView(view.baseViewId);
  if (!base)
    return out;
  glm::dvec3 vd, up, bvd, bup;
  ViewAxes(view, vd, up);
  ViewAxes(*base, bvd, bup);
  const glm::dvec3 right = glm::normalize(glm::cross(vd, up));
  const glm::dvec3 bright = glm::normalize(glm::cross(bvd, bup));
  constexpr double eps = 1e-6;
  if (glm::length(right - bright) < eps) {
    out.pos.x = base->position.x;
    out.xLocked = true;
  }
  if (glm::length(up - bup) < eps) {
    out.pos.y = base->position.y;
    out.yLocked = true;
  }
  return out;
}

drawing::ResolvedDim ResolveDrawingDim(AppState& state, const DrawingDim& dim) {
  const auto it = state.drawingRenders.find(dim.viewId);
  if (it == state.drawingRenders.end()) {
    drawing::ResolvedDim rd; // the view isn't projected: unresolved
    rd.id = dim.id;
    rd.kind = dim.kind;
    return rd;
  }
  // Callout kinds need the source part document (hole provenance) and the
  // view scale (sheet mm -> model mm); legacy kinds ignore the context.
  drawing::DimContext ctx;
  if (state.drawing) {
    if (const DrawingView* view = state.drawing->FindView(dim.viewId)) {
      ctx.sourceDoc = DrawingViewDocument(state, *view);
      ctx.viewScale = view->scale;
      // A fastener callout names the part an OCCURRENCE places, which is a
      // different document from the assembly the view is of. Resolved through
      // the VIEW's source assembly rather than through state.assembly: a
      // drawing is its own tab, so the assembly it draws is never the active
      // one while the drawing is on screen.
      const uint64_t sourceId = view->sourceDocId;
      // The units a sheet is written in are the SOURCE document's, not the
      // reader's — a drawing of an inch part is an inch drawing wherever it
      // is opened, exactly as the part is. Dimension text ignored this
      // entirely and always printed millimetres.
      if (ctx.sourceDoc != nullptr) {
        ctx.units = ctx.sourceDoc->Units();
      } else if (const AssemblyDocument* asmDoc =
                     AssemblyDocById(state, sourceId)) {
        ctx.units = asmDoc->Units();
      }
      ctx.occurrenceDoc = [&state,
                           sourceId](uint64_t occId) -> const Document* {
        const AssemblyDocument* asmDoc = AssemblyDocById(state, sourceId);
        if (asmDoc == nullptr)
          return nullptr;
        const Occurrence* occ = asmDoc->FindById(occId);
        return occ ? OccurrencePartDoc(state, *occ) : nullptr;
      };
    }
  }
  return drawing::ResolveDim(dim, it->second, ctx);
}

namespace {
// The words a view draws besides its lines -- a section's or a detail's
// caption, a cutting plane's letters, a detail circle's -- as boxes a
// label or a balloon must stay off. A view's box is its lines', so
// without these a dimension landed on SECTION A-A.
void AvoidViewTexts(const AppState& state,
                    std::vector<std::pair<glm::dvec2, glm::dvec2>>& avoid) {
  for (const auto& [vid, sv] : state.drawingRenders)
    for (const drawing::SheetText& t : sv.extras.texts) {
      const double w = drawing::TextWidthMM(t.text, t.heightMM);
      const double x0 = t.center ? t.pos.x - 0.5 * w : t.pos.x;
      avoid.push_back({{x0, t.pos.y - 0.3 * t.heightMM},
                       {x0 + w, t.pos.y + 0.8 * t.heightMM}});
    }
}
// Everything on the active sheet a new label must stay off: the title
// block, every view's box and its words, every dimension's value and every
// note. With the border, what auto-placement works inside.
drawing::DimPlaceContext SheetPlaceContext(AppState& state) {
  drawing::DimPlaceContext ctx;
  const SheetChrome chrome = ChromeOf(state.drawing->GetSheet());
  ctx.borderMin = chrome.borderMin;
  ctx.borderMax = chrome.borderMax;
  ctx.avoid.push_back({chrome.titleBlockMin, chrome.titleBlockMax});
  for (const auto& [vid, sv] : state.drawingRenders)
    if (sv.hasGeometry)
      ctx.avoid.push_back({sv.bboxMin, sv.bboxMax});
  AvoidViewTexts(state, ctx.avoid);
  for (const DrawingDim& dm : state.drawing->GetDims()) {
    const drawing::ResolvedDim rd = ResolveDrawingDim(state, dm);
    if (!rd.resolved)
      continue;
    glm::dvec2 mn(0.0), mx(0.0);
    drawing::DimLabelBox(rd, mn, mx);
    ctx.avoid.push_back({mn, mx});
  }
  for (const auto& note : state.drawing->GetNotes()) {
    const auto resolved = ResolveDrawingNote(state, note);
    glm::dvec2 mn, mx;
    drawing::NoteBox(resolved, mn, mx);
    ctx.avoid.push_back({mn, mx});
  }
  return ctx;
}

// Is the box [mn, mx] on the sheet, inside the border, and off everything
// `ctx` avoids?
bool BoxIsClear(const drawing::DimPlaceContext& ctx, const glm::dvec2& mn,
                const glm::dvec2& mx) {
  if (mn.x < ctx.borderMin.x || mn.y < ctx.borderMin.y ||
      mx.x > ctx.borderMax.x || mx.y > ctx.borderMax.y)
    return false;
  for (const auto& [amn, amx] : ctx.avoid)
    if (mn.x < amx.x && mx.x > amn.x && mn.y < amx.y && mx.y > amn.y)
      return false;
  return true;
}
} // namespace

glm::dvec2 AutoDimOffset(AppState& state, const DrawingDim& dim) {
  const auto it = state.drawingRenders.find(dim.viewId);
  if (it == state.drawingRenders.end() || !state.drawing)
    return {0.0, -12.0};
  const drawing::DimPlaceContext ctx = SheetPlaceContext(state);
  // Resolved HERE, where the source document is reachable: a hole callout
  // that cannot be resolved cannot be placed, and it would fall back onto
  // the hole it names.
  DrawingDim probe = dim;
  probe.labelOffset = {0.0, 0.0};
  const drawing::ResolvedDim rd = ResolveDrawingDim(state, probe);
  return drawing::PlaceDimOffset(dim, it->second, ctx,
                                 rd.resolved ? &rd : nullptr);
}

namespace {

// The parts list of an assembly view: its source's leaves less the
// occurrences the view hides
// (docs/drawings/README.md#parts-lists-and-balloons).
std::optional<BomTable> ViewBom(AppState& state, const DrawingView& view,
                                std::string* why) {
  const AssemblyDocument* asmDoc = AssemblyDocById(state, view.sourceDocId);
  if (asmDoc == nullptr) {
    *why = "a parts list is of an assembly; this view draws a part";
    return std::nullopt;
  }
  auto flat = assembly::Flatten(*asmDoc, PartResolverFor(state),
                                AssemblyResolverFor(state));
  if (!flat) {
    *why = flat.error();
    return std::nullopt;
  }
  std::vector<assembly::FlatInstance> shown;
  // What the table depends on: every shown leaf's part, the version of the
  // document it is, its configuration, material and root occurrence. A
  // balloon resolves through this on every placement probe, every check
  // and every export, and the masses under it are a mass-properties pass
  // per part: seventeen balloons on a 60-part head took an export past a
  // minute until the table was kept while this key held.
  std::string key = std::to_string(view.id);
  for (assembly::FlatInstance& inst : *flat)
    if (std::find(view.hidden.begin(), view.hidden.end(),
                  inst.rootOccurrenceId) == view.hidden.end()) {
      key += "|" + std::to_string(inst.partId) + ":" +
             std::to_string(inst.part ? inst.part->Version() : 0) + ":" +
             std::to_string(inst.configurationId) + ":" + inst.material + ":" +
             std::to_string(inst.rootOccurrenceId) + ":" +
             (inst.visible ? "v" : "h");
      shown.push_back(std::move(inst));
    }
  static std::map<std::string, BomTable> cache;
  if (const auto it = cache.find(key); it != cache.end())
    return it->second;
  if (cache.size() > 32)
    cache.clear();
  return cache[key] = ComputeBomOf(state, shown);
}

// The parts list as a table note's text (drawing::IsTableText): a caption
// row if there is one, the header, then one row per BOM line, numbered as
// the balloons number them.
std::string BomTableText(AppState& state, const BomTable& bom,
                         const std::string& caption) {
  std::string out;
  if (!caption.empty())
    out += caption + "\n";
  out += "ITEM\tQTY\tPART\tKIND\tMATERIAL\tMASS g";
  for (size_t i = 0; i < bom.rows.size(); ++i) {
    const BomRow& r = bom.rows[i];
    std::string kind;
    if (r.bought()) {
      kind = "BOUGHT";
    } else {
      // A part declared printed (set_print_process) says so: its sizes are
      // modelled with the print allowance, and a shop must not machine it.
      // The process is the part's, whatever configuration it is placed in,
      // so the live part answers it.
      const Document* part = PartDocById(state, r.partId);
      kind = part && part->PrintProcess() ? "PRINTED" : "MADE";
      if (!r.configuration.empty())
        kind += " (" + r.configuration + ")";
    }
    char mass[32] = "";
    if (r.massKnown)
      std::snprintf(mass, sizeof mass, "%.1f", r.unitMassG);
    out += "\n" + std::to_string(i + 1) + "\t" + std::to_string(r.quantity) +
           "\t" + r.partName + "\t" + kind + "\t" +
           CADMaterials::MaterialLibrary::GradeOf(r.material) + "\t" + mass;
  }
  return out;
}

} // namespace

drawing::ResolvedNote ResolveDrawingNote(AppState& state,
                                         const DrawingNote& note) {
  SymbolTable symbols = state.projectSymbols;
  const DrawingView* view = state.drawing->FindView(note.viewId);
  if (view) {
    if (const Document* doc = DrawingViewDocument(state, *view)) {
      for (const auto& [name, value] : doc->Symbols())
        symbols[name] = value;
      for (size_t i = 0; i < doc->GetFeatures().size(); ++i) {
        if (doc->EffectiveRollback() >= 0 &&
            i >= static_cast<size_t>(doc->EffectiveRollback()))
          break;
        const auto& f = doc->GetFeatures()[i];
        if (doc->EffectiveSuppressed(i) || !f->GetError().empty())
          continue;
        const std::string prefix =
            "feature_" + std::to_string(f->GetId()) + "_";
        if (const auto* hole = dynamic_cast<const HoleFeature*>(f.get())) {
          symbols[prefix + "diameter"] = hole->diameter;
          symbols[prefix + "depth"] = hole->depth;
        }
        if (const auto* thread = dynamic_cast<const ThreadFeature*>(f.get())) {
          if (const auto spec = threads::Lookup(thread->designation)) {
            symbols[prefix + "pitch"] = spec->pitch;
            symbols[prefix + "major"] = spec->majorDia;
            symbols[prefix + "tap_drill"] = threads::TapDrill(*spec);
          }
        }
      }
    }
  }
  const auto it = state.drawingRenders.find(note.viewId);
  const drawing::SheetView* sheetView =
      it == state.drawingRenders.end() ? nullptr : &it->second;
  const auto unresolved = [&](const std::string& shown,
                              const std::string& why) {
    drawing::ResolvedNote rn;
    rn.id = note.id;
    rn.pos = note.pos;
    rn.style = note.style;
    rn.text = shown;
    rn.resolved = false;
    rn.error = why;
    return rn;
  };
  if (note.table == NoteTable::Bom) {
    // The rows are what the view draws NOW: an occurrence the view hides
    // is not on its parts list.
    std::string why;
    const auto bom = view ? ViewBom(state, *view, &why) : std::nullopt;
    if (!bom)
      return unresolved(note.text.empty() ? "PARTS LIST" : note.text,
                        view ? why : "a parts list needs `view`");
    DrawingNote table = note;
    table.text = BomTableText(state, *bom, note.text);
    return drawing::ResolveNote(table, sheetView, symbols);
  }
  if (note.style == NoteStyle::Balloon && note.text.empty()) {
    // The item number of the part the leader points at, in the same list
    // the view's parts list draws -- numbered the same way, by the same
    // code, so a balloon and its row cannot disagree.
    std::string why = "a balloon with no text needs an assembly view";
    // Numbered by the sheet's parts list when it has one of this assembly,
    // whatever view the balloon is on: a plan with the yoke left off lists
    // fewer parts, and its own numbering would disagree with the table.
    const DrawingView* listed = view;
    if (view)
      for (const DrawingNote& other : state.drawing->GetNotes())
        if (other.table == NoteTable::Bom)
          if (const DrawingView* v = state.drawing->FindView(other.viewId);
              v && v->sourceDocId == view->sourceDocId) {
            listed = v;
            break;
          }
    const auto bom = listed ? ViewBom(state, *listed, &why) : std::nullopt;
    if (!bom)
      return unresolved("?", why);
    if (note.leader.occurrenceId == 0)
      return unresolved("?", "a balloon numbers the occurrence its leader "
                             "points at; give the ref an `occurrence`");
    DrawingNote balloon = note;
    for (size_t r = 0; r < bom->rows.size() && balloon.text.empty(); ++r)
      for (uint64_t occ : bom->rows[r].rootOccurrences)
        if (occ == note.leader.occurrenceId) {
          balloon.text = std::to_string(r + 1);
          break;
        }
    if (balloon.text.empty())
      return unresolved("?", "the occurrence this balloon points at is not "
                             "on the view's parts list (hidden in the view?)");
    return drawing::ResolveNote(balloon, sheetView, symbols);
  }
  if (note.table == NoteTable::Bends) {
    // The rows are the part's bends NOW, in the configuration the view draws,
    // so the table cannot disagree with the bend lines beside it.
    std::string why = "a bend table needs a view of a part";
    const Document* doc =
        view ? DrawingViewDocument(state, *view, &why) : nullptr;
    DrawingNote table = note;
    if (doc) {
      glm::dvec3 viewDir, up;
      ViewAxes(*view, viewDir, up);
      table.text = drawing::BendTableText(*doc, note.text, viewDir);
    }
    if (!doc || table.text.empty()) {
      drawing::ResolvedNote rn;
      rn.id = note.id;
      rn.pos = note.pos;
      rn.text = note.text.empty() ? "BEND TABLE" : note.text;
      rn.resolved = false;
      rn.error = doc ? "the part this view draws has no bends" : why;
      return rn;
    }
    return drawing::ResolveNote(table, sheetView, symbols);
  }
  if (note.style == NoteStyle::Fcf) {
    // Every datum the frame names is a datum feature on this sheet: a
    // letter nothing carries would be a reference to nothing.
    std::vector<std::string> have;
    for (const DrawingNote& other : state.drawing->GetNotes())
      if (other.style == NoteStyle::Datum)
        have.push_back(other.text);
    drawing::ResolvedNote rn = drawing::ResolveNote(note, sheetView, symbols,
                                                    ActiveDrawingUnits(state));
    for (const std::string& d : note.fcf.datums)
      if (std::find(have.begin(), have.end(), d) == have.end()) {
        std::string list;
        for (const std::string& h : have)
          list += (list.empty() ? "" : ", ") + h;
        rn.resolved = false;
        rn.error = "datum " + d + " is not on this sheet (its datums: " +
                   (list.empty() ? std::string("none") : list) +
                   ") -- add it as a `datum` note on its feature";
        return rn;
      }
    if (note.attachDim != 0) {
      // Stacked with the dimension's value, no leader: the frame then
      // applies to the feature that dimension sizes. Under a leader's
      // value; over a value written along its line, whose other side is
      // the line itself.
      const DrawingDim* dim = state.drawing->FindDim(note.attachDim);
      const drawing::ResolvedDim rd =
          dim ? ResolveDrawingDim(state, *dim) : drawing::ResolvedDim{};
      if (!dim || !rd.resolved) {
        rn.resolved = false;
        rn.error = dim ? "the dimension this frame stands under no longer "
                         "resolves"
                       : "the dimension this frame stands under is gone";
        return rn;
      }
      glm::dvec2 mn, mx;
      drawing::DimLabelBox(rd, mn, mx);
      const glm::dvec2 size = drawing::FcfSize(rn.fcf, rn.fcfTolerance);
      rn.pos = {0.5 * (mn.x + mx.x), rd.aligned ? mx.y + 0.8 + 0.5 * size.y
                                                : mn.y - 0.8 - 0.5 * size.y};
      rn.hasLeader = false;
    }
    return rn;
  }
  return drawing::ResolveNote(note, sheetView, symbols);
}

units::System ActiveDrawingUnits(AppState& state) {
  if (!state.drawing)
    return {};
  // The FIRST view's source, which is what ResolveDrawingDim used to format
  // each dimension. A sheet mixing an inch part with a millimetre one would
  // need a per-view answer; nothing produces that yet, and picking one system
  // for the sheet is what the single UNITS row can honestly claim.
  for (const DrawingView& v : state.drawing->GetViews()) {
    if (const Document* src = DrawingSourceDocument(state, v.sourceDocId))
      return src->Units();
    if (const AssemblyDocument* a = AssemblyDocById(state, v.sourceDocId))
      return a->Units();
  }
  return {};
}

namespace {

// "1:2", "2:1", "1:1": reductions read 1:n and enlargements n:1.
std::string ScaleRatio(double s) {
  if (s <= 0.0)
    return {};
  char buf[32];
  if (s >= 1.0)
    std::snprintf(buf, sizeof buf, "%.3g:1", s);
  else
    std::snprintf(buf, sizeof buf, "1:%.3g", 1.0 / s);
  return buf;
}

} // namespace

Sheet DrawingSheetForOutput(AppState& state) {
  if (!state.drawing)
    return {};
  Sheet sheet = state.drawing->GetSheet();
  const std::vector<DrawingView>& views = state.drawing->GetViews();
  if (views.empty())
    return sheet;
  const DrawingView& first = views.front();
  if (sheet.titleBlock.scale.empty()) {
    // The sheet's scale is its first view's; a view drawn at another says
    // so beside it, and the block says that it may.
    sheet.titleBlock.scale = ScaleRatio(first.scale);
    for (const DrawingView& v : views)
      if (std::abs(v.scale - first.scale) > 1e-9) {
        sheet.titleBlock.scale += " UNLESS SHOWN";
        break;
      }
  }
  if (sheet.titleBlock.material.empty())
    if (const Document* doc = DrawingViewDocument(state, first)) {
      std::string slug;
      for (const Body& body : doc->GetBodies()) {
        if (body.reference || body.material.empty())
          continue;
        // A body left off the sheet is not what the sheet is made of.
        if (std::find(first.hidden.begin(), first.hidden.end(),
                      body.createdBy) != first.hidden.end())
          continue;
        if (!slug.empty() && slug != body.material) {
          slug = "mixed";
          break;
        }
        slug = body.material;
      }
      // The grade the library carries for it (6082-T6), or the generic
      // material's name, which says no grade was chosen.
      sheet.titleBlock.material =
          slug == "mixed" ? std::string("MIXED")
                          : CADMaterials::MaterialLibrary::GradeOf(slug);
    }
  return sheet;
}

bool ExportActiveDrawingWhenProjected(AppState& state, const std::string& path,
                                      std::string* error) {
  if (!state.drawing) {
    if (error)
      *error = "no active drawing";
    return false;
  }
  if (!state.drawingDeferProjection)
    return ExportActiveDrawing(state, path, error, /*update=*/true);
  RequestDrawingUpdate(state);
  if (state.drawingRun || state.drawingProjections.updateOwed) {
    // Written by FlushPendingExport when the last view lands.
    state.drawingProjections.exportPath = path;
    state.drawingMessage =
        "Exporting " + path + " once the views are projected";
    state.drawingMessageIsError = false;
    return true;
  }
  return ExportActiveDrawing(state, path, error, /*update=*/false);
}

bool ExportActiveDrawing(AppState& state, const std::string& path,
                         std::string* error, bool update) {
  if (!state.drawing) {
    if (error)
      *error = "no active drawing";
    return false;
  }
  if (update)
    UpdateDrawingRendersNow(state); // export reflects current geometry
  // The sheet as it goes out, its title block's blanks filled.
  DrawingDocument out = *state.drawing;
  out.GetSheet() = DrawingSheetForOutput(state);
  std::vector<drawing::SheetView> views;
  for (const DrawingView& v : state.drawing->GetViews()) {
    const auto it = state.drawingRenders.find(v.id);
    if (it != state.drawingRenders.end())
      views.push_back(it->second);
  }
  std::vector<drawing::ResolvedDim> dims;
  for (const DrawingDim& dm : state.drawing->GetDims())
    dims.push_back(ResolveDrawingDim(state, dm));
  std::vector<drawing::ResolvedNote> notes;
  for (const DrawingNote& n : state.drawing->GetNotes()) {
    const auto resolved = ResolveDrawingNote(state, n);
    if (!resolved.resolved) {
      if (error)
        *error = "note " + std::to_string(n.id) + ": " + resolved.error;
      return false;
    }
    notes.push_back(resolved);
  }
  const units::System sheetUnits = ActiveDrawingUnits(state);
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".svg")
    return DrawingExport::SaveSVG(out, views, dims, path, error, notes,
                                  sheetUnits);
  if (ext == ".pdf")
    return DrawingExport::SavePDF(out, views, dims, path, error, notes,
                                  sheetUnits);
  if (error)
    *error = "unsupported drawing format '" + ext + "' (use .svg or .pdf)";
  return false;
}

uint64_t DrawingSourceVersion(AppState& state, uint64_t sourceDocId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].id != sourceDocId)
      continue;
    if (state.parts[i].kind == DocKind::Part) {
      const Document* doc = SlotDoc(state, i);
      return doc ? doc->Version() : 0;
    }
    if (state.parts[i].kind == DocKind::Assembly) {
      const AssemblyDocument* asmDoc = SlotAssembly(state, i);
      return asmDoc ? asmDoc->Version() : 0;
    }
  }
  return 0;
}

Document* DrawingSourceDocument(AppState& state, uint64_t sourceDocId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].id == sourceDocId &&
        state.parts[i].kind == DocKind::Part)
      return SlotDoc(state, i);
  }
  return nullptr;
}

const Document* ConfiguredPartDocument(AppState& state, uint64_t partId,
                                       uint64_t configuration,
                                       std::string* error) {
  const auto fail = [&](std::string why) -> const Document* {
    if (error)
      *error = std::move(why);
    return nullptr;
  };
  const Document* live = DrawingSourceDocument(state, partId);
  if (!live)
    return fail("source document is not a part");
  if (configuration == 0 || live->ActiveConfiguration() == configuration)
    return live;
  const Configuration* cfg = live->FindConfiguration(configuration);
  if (!cfg) {
    std::string known;
    for (const Configuration& c : live->GetConfigurations())
      known += (known.empty() ? "" : ", ") + c.name + " (" +
               std::to_string(c.id) + ")";
    return fail("the source part has no configuration " +
                std::to_string(configuration) +
                (known.empty() ? std::string(" (it has none)")
                               : " (it has: " + known + ")"));
  }
  const std::pair<uint64_t, uint64_t> key{partId, configuration};
  const auto hit = state.configuredSources.find(key);
  const bool fresh =
      hit != state.configuredSources.end() && hit->second.doc &&
      hit->second.sourceInstance == live->InstanceId() &&
      hit->second.sourceVersion == live->Version() &&
      hit->second.configurationsRevision == live->ConfigurationsRevision();
  if (!fresh) {
    auto copy = std::make_unique<Document>(*live);
    copy->SetActiveConfiguration(configuration);
    // What the copy's own regeneration said. Nobody can LOOK at this
    // document — the model tab is on another configuration — so a feature
    // that failed here would otherwise show up only as the folded part
    // drawn in the blank's place, with nothing anywhere saying why.
    for (size_t i = 0; i < copy->GetFeatures().size(); ++i) {
      const Feature& f = *copy->GetFeatures()[i];
      if (copy->EffectiveSuppressed(i) || f.GetError().empty())
        continue;
      return fail("in configuration '" + cfg->name + "', " + f.GetName() +
                  " failed: " + f.GetError());
    }
    // Inserted only once it is a document worth keeping: a refusal above
    // must not leave an empty entry behind.
    AppState::ConfiguredSource& cached = state.configuredSources[key];
    cached.doc = std::move(copy);
    cached.sourceInstance = live->InstanceId();
    cached.sourceVersion = live->Version();
    cached.configurationsRevision = live->ConfigurationsRevision();
    return cached.doc.get();
  }
  return hit->second.doc.get();
}

const Document* DrawingViewDocument(AppState& state, const DrawingView& view,
                                    std::string* error) {
  const Document* live = DrawingSourceDocument(state, view.sourceDocId);
  if (live && view.configuration != 0 &&
      !live->FindConfiguration(view.configuration)) {
    if (error)
      *error = "the source part has no configuration " +
               std::to_string(view.configuration) +
               " any more; pick another for this view";
    return nullptr;
  }
  return ConfiguredPartDocument(state, view.sourceDocId, view.configuration,
                                error);
}

namespace {

// One view's footprint at 1:1, anchored at the origin (the sheet anchor maps
// model -> position + scale * model).
struct ViewFootprint {
  glm::dvec2 modelMin, modelMax;
};

// Lays every view out from the sheet's top-left at one scale: a row to the
// right, wrapping below when the row is full. Returns false when a wrapped
// row would land on the title block or under the border — the sheet is full
// at this scale. `positions` receives one anchor per footprint.
bool FlowLayout(const SheetChrome& chrome, double scale,
                const std::vector<ViewFootprint>& items,
                std::vector<glm::dvec2>& positions) {
  positions.clear();
  double gapMax = 8.0;
  for (const ViewFootprint& f : items) {
    const glm::dvec2 size = (f.modelMax - f.modelMin) * scale;
    gapMax = std::max(gapMax, std::max(size.x, size.y) * 0.15);
  }
  const double gap = gapMax;
  double cursorX = chrome.borderMin.x + gap; // next left edge
  double rowTop = chrome.borderMax.y - gap;  // this row's top edge
  double rowBottom = rowTop;                 // lowest edge in this row
  for (const ViewFootprint& f : items) {
    const glm::dvec2 size = (f.modelMax - f.modelMin) * scale;
    if (cursorX + size.x > chrome.borderMax.x &&
        cursorX > chrome.borderMin.x + gap + 1e-9) {
      cursorX = chrome.borderMin.x + gap; // wrap
      rowTop = rowBottom - gap;
    }
    const double left = cursorX, top = rowTop;
    const double bottom = top - size.y;
    const Span rowX{left, left + size.x};
    const Span blockX{chrome.titleBlockMin.x, chrome.titleBlockMax.x};
    const double floorY =
        Overlaps(rowX, blockX) ? chrome.titleBlockMax.y : chrome.borderMin.y;
    if (bottom < floorY + gap || left + size.x > chrome.borderMax.x)
      return false; // does not fit at this scale
    positions.push_back(
        glm::dvec2(left - scale * f.modelMin.x, top - scale * f.modelMax.y));
    cursorX = left + size.x + gap;
    rowBottom = std::min(rowBottom, bottom);
  }
  return true;
}

bool BoxesOverlap(glm::dvec2 aMin, glm::dvec2 aMax, glm::dvec2 bMin,
                  glm::dvec2 bMax) {
  return Overlaps({aMin.x, aMax.x}, {bMin.x, bMax.x}) &&
         Overlaps({aMin.y, aMax.y}, {bMin.y, bMax.y});
}

} // namespace

namespace {

// Chooses a new view's scale (when `chooseScale`) and its place, from its
// size at 1:1 (`lo`..`hi` in the view plane, model mm; `measured` false when
// there is none to go by), clear of the views already placed (all but
// `self`). A placement that overlaps a view or the title block re-lays the
// whole sheet out, so other views may move and rescale too.
void PlaceView(AppState& state, DrawingView& v, glm::dvec2 lo, glm::dvec2 hi,
               bool measured, bool chooseScale, uint64_t self) {
  DrawingDocument& dwg = *state.drawing;
  const glm::dvec2 ext = ChromeOf(dwg.GetSheet()).extent;
  const glm::dvec2 size = measured ? hi - lo : glm::dvec2(0.0);
  const auto others = [&](const DrawingView& o) { return o.id != self; };

  // Scale (only when the caller did not choose one). Views of the SAME part
  // share a scale: a right view at 2:1 beside a front view at 1:1 is a
  // drafting error, and it is what the fit-each-view-alone rule below
  // produced for a plate (the right view is the smallest face, so it got
  // blown up until it collided with the top view). Matching first, fitting
  // only when this is the part's first view.
  if (chooseScale) {
    v.scale = 0.0;
    for (const DrawingView& other : dwg.GetViews()) {
      if (others(other) && other.sourceDocId == v.sourceDocId &&
          other.scale > 0.0) {
        v.scale = other.scale;
        break;
      }
    }
  }
  if (v.scale <= 0.0) {
    v.scale = 1.0;
    if (measured && size.x > 1e-9 && size.y > 1e-9) {
      static const double kStandard[] = {100.0, 50.0, 20.0, 10.0, 5.0,
                                         2.0,   1.0,  0.5,  0.2,  0.1,
                                         0.05,  0.02, 0.01};
      const glm::dvec2 budget = ext * 0.45;
      v.scale = kStandard[std::size(kStandard) - 1];
      for (double s : kStandard) {
        if (size.x * s <= budget.x && size.y * s <= budget.y) {
          v.scale = s;
          break;
        }
      }
    }
  }

  // Position: sheet centre for the first view, else clear to the right of
  // everything already placed by the shared rule (ui/Placement.h), wrapping
  // below when the row is full. The sheet anchor maps model -> pos + scale *
  // model, so a left edge at `x` means pos.x = x - scale * lo.x.
  glm::dvec2 pos = ext * 0.5;
  if (measured) {
    const SheetChrome chrome = ChromeOf(dwg.GetSheet());
    std::vector<Span> takenX;
    double lowest = 1e30, firstMidY = ext.y * 0.5;
    bool anyPlaced = false;
    for (const DrawingView& other : dwg.GetViews()) {
      if (!others(other))
        continue;
      const auto it = state.drawingRenders.find(other.id);
      if (it == state.drawingRenders.end() || !it->second.hasGeometry)
        continue;
      if (!anyPlaced)
        firstMidY = (it->second.bboxMin.y + it->second.bboxMax.y) * 0.5;
      anyPlaced = true;
      takenX.push_back({it->second.bboxMin.x, it->second.bboxMax.x});
      lowest = std::min(lowest, it->second.bboxMin.y);
    }
    const glm::dvec2 placed = size * v.scale;
    const Span incoming{v.scale * lo.x, v.scale * hi.x};
    const double gap = std::max(8.0, std::max(placed.x, placed.y) * 0.15);
    if (!anyPlaced) {
      pos = ext * 0.5 - v.scale * (lo + hi) * 0.5;
    } else if (const double dx = ClearOffset(takenX, incoming, 8.0);
               dx + v.scale * hi.x <= chrome.borderMax.x) {
      pos.x = dx; // same row, to the right of everything placed
      pos.y = firstMidY - v.scale * (lo.y + hi.y) * 0.5;
    } else {
      // A new row BELOW the others (sheet Y grows upward), starting at the
      // left margin — and stopping above the title block, which a wrapped
      // row used to land on top of.
      pos.x = chrome.borderMin.x + gap - v.scale * lo.x;
      pos.y = lowest - gap - v.scale * hi.y;
      const Span rowX{pos.x + v.scale * lo.x, pos.x + v.scale * hi.x};
      const Span blockX{chrome.titleBlockMin.x, chrome.titleBlockMax.x};
      const double floorY =
          Overlaps(rowX, blockX) ? chrome.titleBlockMax.y : chrome.borderMin.y;
      if (pos.y + v.scale * lo.y < floorY + gap)
        pos.y = floorY + gap - v.scale * lo.y;
    }
  }
  v.position = pos;

  // The row-and-wrap rule above clamps a wrapped row up to the border, and
  // when the views already there reach that low the new one lands ON them —
  // four auto-placed views of a 90 mm gripper body at 2:1 came out as one
  // pile (the D4 dogfood). A placement that overlaps a view or the title
  // block is not a placement: re-lay the whole sheet out from the top-left,
  // at this scale first and then each standard scale down, until it fits.
  // Every view of the sheet is moved (and, if needed, rescaled together —
  // one scale per sheet is the drafting rule the block above already keeps).
  if (!measured)
    return;
  const SheetChrome chrome = ChromeOf(dwg.GetSheet());
  const glm::dvec2 newMin = pos + v.scale * lo;
  const glm::dvec2 newMax = pos + v.scale * hi;
  bool collides =
      BoxesOverlap(newMin, newMax, chrome.titleBlockMin,
                   chrome.titleBlockMax) ||
      newMin.x < chrome.borderMin.x || newMin.y < chrome.borderMin.y ||
      newMax.x > chrome.borderMax.x || newMax.y > chrome.borderMax.y;
  for (const DrawingView& other : dwg.GetViews()) {
    if (!others(other))
      continue;
    const auto it = state.drawingRenders.find(other.id);
    if (it != state.drawingRenders.end() && it->second.hasGeometry &&
        BoxesOverlap(newMin, newMax, it->second.bboxMin, it->second.bboxMax))
      collides = true;
  }
  if (!collides)
    return;
  std::vector<DrawingView*> laid;
  std::vector<ViewFootprint> items;
  for (DrawingView& other : dwg.GetViews()) {
    if (!others(other))
      continue;
    const auto it = state.drawingRenders.find(other.id);
    if (it == state.drawingRenders.end() || !it->second.hasGeometry ||
        other.scale <= 0.0)
      continue;
    laid.push_back(&other);
    items.push_back({(it->second.bboxMin - other.position) / other.scale,
                     (it->second.bboxMax - other.position) / other.scale});
  }
  items.push_back({lo, hi});
  static const double kScales[] = {100.0, 50.0, 20.0, 10.0, 5.0,  2.0, 1.0,
                                   0.5,   0.2,  0.1,  0.05, 0.02, 0.01};
  std::vector<glm::dvec2> positions;
  for (double scale : kScales) {
    if (scale > v.scale + 1e-12)
      continue;
    if (!FlowLayout(chrome, scale, items, positions))
      continue;
    for (size_t i = 0; i < laid.size(); i++) {
      laid[i]->scale = scale;
      laid[i]->position = positions[i];
    }
    v.scale = scale;
    v.position = positions.back();
    break;
  }
}

// The active drawing's view `viewId` at 1:1 in the view plane, by its kept
// projection (turned to the view's up): its size, which placing it needs.
bool ProjectedBox(AppState& state, uint64_t viewId, glm::dvec2& lo,
                  glm::dvec2& hi) {
  const DrawingView* v = state.drawing->FindView(viewId);
  if (v == nullptr)
    return false;
  const auto kept = state.drawingProjections.kept.find(ProjectedViewId(*v));
  if (kept == state.drawingProjections.kept.end())
    return false;
  glm::dvec3 dir, up;
  ViewAxes(*v, dir, up);
  const drawing::ProjectionResult r =
      drawing::TurnedTo(*kept->second.projection, up);
  lo = glm::dvec2(1e300);
  hi = glm::dvec2(-1e300);
  for (const drawing::ProjectedCurve& c : r.curves)
    for (const glm::dvec2& p : c.points) {
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
  return hi.x >= lo.x && hi.y >= lo.y;
}

// Assembles every view of the active drawing again from what it keeps
// (stale ones as stale): what a change of place or scale needs. Returns the
// views that have nothing to be drawn from.
std::set<uint64_t> AssembleAllKept(AppState& state) {
  std::set<uint64_t> missing;
  for (const DrawingView& view : state.drawing->GetViews()) {
    if (state.drawingProjections.projecting.contains(ProjectedViewId(view)))
      continue;
    if (!AssembleKeptView(state, view.id, /*allowStale=*/true))
      missing.insert(ProjectedViewId(view));
  }
  return missing;
}

// A view added with its place still to choose has its projection: place it
// by its real bounds, and draw the sheet again (others may have moved).
void PlaceLandedView(AppState& state, uint64_t viewId, bool chooseScale) {
  DrawingView* v = state.drawing->FindView(viewId);
  glm::dvec2 lo, hi;
  if (v == nullptr || !ProjectedBox(state, viewId, lo, hi))
    return;
  PlaceView(state, *v, lo, hi, true, chooseScale, viewId);
  state.drawing->BumpVersion();
  // Moving it is not projecting it: the view keeps the bill of the
  // projection that just landed, which is what placing it asked for.
  std::optional<drawing::SheetView> landed;
  if (const auto it = state.drawingRenders.find(viewId);
      it != state.drawingRenders.end())
    landed = it->second;
  AssembleAllKept(state);
  if (const auto it = state.drawingRenders.find(viewId);
      landed && it != state.drawingRenders.end()) {
    it->second.projectionStats = landed->projectionStats;
    it->second.updateMs = landed->updateMs;
    it->second.hideMs = landed->hideMs;
    it->second.dsEdges = landed->dsEdges;
    it->second.costBySource = landed->costBySource;
  }
}

// Where an export waits on the projection, it goes once nothing projects.
void FlushPendingExport(AppState& state) {
  DrawingProjectionState& ps = state.drawingProjections;
  if (ps.exportPath.empty() || !state.drawing || state.drawingRun)
    return;
  const std::string path = std::move(ps.exportPath);
  ps.exportPath.clear();
  std::string error;
  if (ExportActiveDrawing(state, path, &error, /*update=*/false)) {
    LOG_INFO("Exported drawing to {}", path);
    state.drawingMessage = "Exported " + path;
    state.drawingMessageIsError = false;
  } else {
    LOG_ERROR("Drawing export failed: {}", error);
    state.drawingMessage = "Export failed: " + error;
    state.drawingMessageIsError = true;
  }
}

void UpdateNowImpl(AppState& state, double budgetSeconds,
                   const std::set<uint64_t>* only) {
  if (!state.drawing)
    return;
  // A run in flight finishes first, and gives the caches back.
  JoinBackgroundJobs(state, "drawing");
  if (!state.drawing)
    return;
  const auto started = std::chrono::steady_clock::now();
  const auto outOfBudget = [&] {
    if (budgetSeconds <= 0.0)
      return false;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         started)
               .count() > budgetSeconds;
  };
  std::vector<DrawingViewWork> work =
      PlanDrawingUpdate(state, only, /*owed=*/nullptr);
  const uint64_t slotId = ActiveSlotId(state);
  for (DrawingViewWork& w : work) {
    // Before starting, as well as during. A single body's Hide() is ONE
    // uninterruptible OCC call — the pairwise drive only has somewhere to
    // stop when there are several shapes to classify against each other — so
    // without this a sheet whose budget went on view one would still pay for
    // views two and three in full.
    std::string why;
    if (outOfBudget()) {
      why = "budget";
    } else {
      RunDrawingViewWork(w, &state.drawingProjectionCache,
                         &state.drawingCutCache, outOfBudget, nullptr);
      if (!w.out && w.error == "cancelled")
        why = "budget";
    }
    LandDrawingViewWork(state, slotId, w, why);
  }
  PruneShapeMemo(state);
  state.drawingRendersVersion = state.drawing->Version();
}

std::string StoppedError(const std::string& why) {
  if (why == "budget")
    // Out of budget rather than broken. Say which, and say the way through:
    // a `fast` view of the same geometry is a couple of hundred milliseconds
    // against a minute and a half.
    return "ran out of the update's time budget — exact HLR on this source "
           "is slower than that. Raise the budget, or set this view's "
           "quality to \"fast\" (polygonal HLR: orders of magnitude cheaper, "
           "and a look rather than a dimensionable deliverable).";
  return "projection cancelled — Update views to project";
}

} // namespace

uint64_t AddDrawingViewPlaced(AppState& state, DrawingView v,
                              std::optional<bool> defer) {
  if (!state.drawing)
    return 0;
  const bool deferred = defer.value_or(state.drawingDeferProjection);
  const bool chooseScale = v.scale <= 0.0;
  // One undo step: the snapshot goes first, because placing may move the
  // views already on the sheet, and an undo of this add puts them back too.
  SnapshotDrawing(state);
  // Placed at once by its source's bounds, which need no projection...
  glm::dvec2 lo, hi;
  v.id = 0;
  const bool estimated = EstimateViewBox(state, v, lo, hi);
  PlaceView(state, v, lo, hi, estimated, chooseScale, 0);
  const uint64_t id = state.drawing->AddView(v);
  // ...and again by its real ones, once it is projected.
  if (deferred) {
    state.drawingProjections.placePending[id] = chooseScale;
    RequestDrawingUpdate(state);
    return id;
  }
  UpdateNowImpl(state, 0.0, nullptr);
  PlaceLandedView(state, id, chooseScale);
  return id;
}

void RequestDrawingUpdate(AppState& state, double budgetSeconds,
                          const std::set<uint64_t>* only) {
  if (!state.drawing)
    return;
  if (!state.drawingDeferProjection) {
    UpdateNowImpl(state, budgetSeconds, only);
    return;
  }
  DrawingProjectionState& ps = state.drawingProjections;
  if (state.drawingRun) {
    // One run at a time. What can be assembled is; what would project waits
    // for the run in flight, and is asked for when it ends.
    // (A view the run is projecting may have moved on since it started, so
    // asking about one is an update owed too: it costs only assembling,
    // when it has not.)
    bool owed = false;
    for (const DrawingView& view : state.drawing->GetViews()) {
      const uint64_t projected = ProjectedViewId(view);
      if (ps.projecting.contains(projected)) {
        owed = owed || only == nullptr || only->contains(projected);
        continue;
      }
      if (AssembleKeptView(state, view.id, /*allowStale=*/false))
        continue;
      if (!view.detail.on && (only == nullptr || only->contains(view.id)))
        owed = true;
      AssembleKeptView(state, view.id, /*allowStale=*/true);
    }
    if (owed)
      ps.updateOwed = true;
    return;
  }
  const auto prepStart = std::chrono::steady_clock::now();
  std::vector<DrawingViewWork> work = PlanDrawingUpdate(state, only, nullptr);
  PruneShapeMemo(state);
  state.drawingRendersVersion = state.drawing->Version();
  if (work.empty()) {
    FlushPendingExport(state);
    return;
  }
  for (const DrawingViewWork& w : work) {
    ps.projecting.insert(w.viewId);
    for (const DrawingView& d : state.drawing->GetViews())
      if (d.detail.on && d.detail.parentViewId == w.viewId)
        ps.projecting.insert(d.id);
  }
  for (uint64_t id : ps.projecting) {
    const auto r = state.drawingRenders.find(id);
    if (r != state.drawingRenders.end() && r->second.hasGeometry)
      continue;
    const DrawingView* v = state.drawing->FindView(id);
    glm::dvec2 lo, hi;
    if (v && EstimateViewBox(state, *v, lo, hi))
      ps.placeholder[id] = {lo, hi};
  }
  LOG_INFO("Drawing: {} view(s) to project off the frame, {:.0f} ms to "
           "prepare on it",
           work.size(),
           std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - prepStart)
               .count());
  StartDrawingJob(state, ActiveSlotId(state), std::move(work), budgetSeconds);
}

void UpdateDrawingRendersNow(AppState& state, double budgetSeconds) {
  UpdateNowImpl(state, budgetSeconds, nullptr);
}

bool PrefetchDrawingView(AppState& state, const DrawingView& v) {
  if (!state.drawing || state.drawingRun || !state.drawingDeferProjection)
    return false;
  PreparedView pv = PrepareView(state, v, /*createCopies=*/true);
  if (!pv.error.empty() || !pv.copies)
    return false;
  for (const auto& [id, kept] : state.drawingProjections.kept)
    if (kept.inputs.SameAs(pv.inputs))
      return false; // projected already: assembling is all it costs
  DrawingViewWork w;
  w.viewId = 0; // lands on no view: what it leaves is the cache's
  w.name = v.name;
  w.inputs = std::move(pv.inputs);
  w.view = pv.view;
  w.sourceVersion = pv.sourceVersion;
  std::vector<DrawingViewWork> work;
  work.push_back(std::move(w));
  StartDrawingJob(state, ActiveSlotId(state), std::move(work), 0.0);
  return true;
}

void ReassembleDrawingViews(AppState& state) {
  if (!state.drawing)
    return;
  const std::set<uint64_t> missing = AssembleAllKept(state);
  std::erase_if(state.drawingRenders, [&](const auto& kv) {
    return state.drawing->FindView(kv.first) == nullptr;
  });
  if (!missing.empty())
    RequestDrawingUpdate(state, 0.0, &missing);
}

bool DrawingViewProjecting(const AppState& state, uint64_t viewId) {
  if (!state.drawing)
    return false;
  const DrawingProjectionState& ps = state.drawingProjections;
  if (ps.projecting.contains(viewId))
    return true;
  const DrawingView* v = state.drawing->FindView(viewId);
  return v != nullptr && v->detail.on &&
         ps.projecting.contains(v->detail.parentViewId);
}

bool DrawingPlaceholderBox(const AppState& state, const DrawingView& view,
                           glm::dvec2& min, glm::dvec2& max) {
  const auto it = state.drawingProjections.placeholder.find(view.id);
  if (it == state.drawingProjections.placeholder.end())
    return false;
  const double s = view.scale != 0.0 ? view.scale : 1.0;
  const glm::dvec2 pos =
      view.detail.on
          ? view.position
          : DrawingFoldedPosition(const_cast<AppState&>(state), view).pos;
  min = pos + s * it->second.first;
  max = pos + s * it->second.second;
  return true;
}

void LandDrawingViewWork(AppState& state, uint64_t slotId, DrawingViewWork& w,
                         const std::string& why) {
  // The tab the work was for: active, stashed, or closed meanwhile.
  const bool active = state.drawing != nullptr && ActiveSlotId(state) == slotId;
  DrawingDocument* dwg = nullptr;
  DrawingProjectionState* ps = nullptr;
  std::map<uint64_t, drawing::SheetView>* renders = nullptr;
  if (active) {
    dwg = state.drawing.get();
    ps = &state.drawingProjections;
    renders = &state.drawingRenders;
  } else {
    for (PartSlot& slot : state.parts)
      if (slot.id == slotId && slot.kind == DocKind::Drawing && slot.drawing) {
        dwg = slot.drawing.get();
        ps = &slot.drawingProjections;
        renders = &slot.drawingRenders;
      }
  }
  if (dwg == nullptr)
    return; // the tab was closed: nothing to land on
  std::vector<uint64_t> views{w.viewId};
  for (const DrawingView& d : dwg->GetViews())
    if (d.detail.on && d.detail.parentViewId == w.viewId)
      views.push_back(d.id);
  for (uint64_t id : views)
    ps->projecting.erase(id);
  if (dwg->FindView(w.viewId) == nullptr)
    return; // the view was deleted meanwhile
  if (w.out) {
    ps->kept[w.viewId] = {w.out, w.inputs, w.sourceVersion, w.view};
    dwg->SetSavedProjection(w.viewId, w.out);
    for (uint64_t id : views)
      ps->placeholder.erase(id);
    if (!active) {
      ps->assemble.insert(views.begin(), views.end());
      return;
    }
    for (uint64_t id : views)
      if (!AssembleKeptView(state, id, /*allowStale=*/true, /*landed=*/true))
        SetViewError(*renders, id,
                     "its source changed while it projected: Update views",
                     w.sourceVersion);
    if (const auto place = ps->placePending.find(w.viewId);
        place != ps->placePending.end()) {
      const bool chooseScale = place->second;
      ps->placePending.erase(place);
      PlaceLandedView(state, w.viewId, chooseScale);
    }
    return;
  }
  const std::string error = !why.empty()       ? StoppedError(why)
                            : !w.error.empty() ? w.error
                                               : w.gatherError;
  for (uint64_t id : views) {
    SetViewError(*renders, id,
                 id == w.viewId
                     ? error
                     : "the view this detail is of did not project: " + error,
                 w.sourceVersion);
    // Cancelled, it is still coming (at the next update) and keeps its
    // placeholder; failed, it is not.
    if (why.empty())
      ps->placeholder.erase(id);
  }
  ps->placePending.erase(w.viewId);
}

void DrawingRunEnded(AppState& state, uint64_t slotId, bool cancelled) {
  for (PartSlot& slot : state.parts)
    if (slot.id == slotId)
      slot.drawingProjections.projecting.clear();
  const bool active = state.drawing != nullptr && ActiveSlotId(state) == slotId;
  if (active)
    state.drawingProjections.projecting.clear();
  if (cancelled) {
    // Stopping is the user's word for everything that waited on the run.
    for (PartSlot& slot : state.parts)
      if (slot.id == slotId)
        slot.drawingProjections.exportPath.clear();
    if (active && !state.drawingProjections.exportPath.empty()) {
      state.drawingProjections.exportPath.clear();
      state.drawingMessage =
          "Export cancelled: the views were not all projected";
      state.drawingMessageIsError = true;
    }
    if (active)
      state.drawingProjections.updateOwed = false;
    return;
  }
  if (!state.drawing)
    return;
  if (state.drawingProjections.updateOwed) {
    state.drawingProjections.updateOwed = false;
    RequestDrawingUpdate(state);
  }
  FlushPendingExport(state);
}

void DrawingTabShown(AppState& state) {
  if (!state.drawing)
    return;
  DrawingProjectionState& ps = state.drawingProjections;
  if (!ps.assemble.empty()) {
    const std::set<uint64_t> landed = std::move(ps.assemble);
    ps.assemble.clear();
    for (uint64_t id : landed)
      if (state.drawing->FindView(id) &&
          !AssembleKeptView(state, id, /*allowStale=*/true))
        SetViewError(state.drawingRenders, id,
                     "its source changed while it projected: Update views", 0);
    // A view added while it projected is placed now that it can be.
    for (auto it = ps.placePending.begin(); it != ps.placePending.end();) {
      if (landed.contains(it->first) && !ps.projecting.contains(it->first)) {
        const uint64_t id = it->first;
        const bool chooseScale = it->second;
        it = ps.placePending.erase(it);
        PlaceLandedView(state, id, chooseScale);
      } else {
        ++it;
      }
    }
  }
  // A drawing projects on the FIRST view of the tab (design §2): if the
  // restored renders are empty (never projected this session) it asks;
  // otherwise it keeps the stashed geometry, so staleness is only cleared by
  // an explicit update. An update asked for while another tab's run went is
  // asked for now.
  if (ps.updateOwed && !state.drawingRun) {
    ps.updateOwed = false;
    RequestDrawingUpdate(state);
  } else if (state.drawingRenders.empty() && state.drawing->ViewCount() > 0 &&
             ps.projecting.empty()) {
    RequestDrawingUpdate(state);
  }
  FlushPendingExport(state);
}

void ForgetDrawingProjections(AppState& state) {
  if (state.drawingRun) {
    CancelDrawingJob(state);
    JoinBackgroundJobs(state, "drawing");
  }
  state.drawingProjectionCache.Clear();
  state.drawingCutCache.Clear();
  state.drawingShapes = {};
  state.drawingProjections = {};
}

std::string NextViewLetter(AppState& state) {
  std::set<std::string> used;
  if (state.drawing)
    for (const DrawingView& v : state.drawing->GetViews()) {
      if (v.section.on)
        used.insert(v.section.label);
      if (v.detail.on)
        used.insert(v.detail.label);
    }
  for (char c = 'A'; c <= 'Z'; ++c) {
    if (c == 'I' || c == 'O' || c == 'Q')
      continue;
    if (!used.contains(std::string(1, c)))
      return std::string(1, c);
  }
  return "Z";
}

std::string NextDatumLetter(AppState& state) {
  std::set<std::string> used;
  if (state.drawing)
    for (const DrawingNote& n : state.drawing->GetNotes())
      if (n.style == NoteStyle::Datum)
        used.insert(n.text);
  for (char c = 'A'; c <= 'Z'; ++c) {
    if (c == 'I' || c == 'O' || c == 'Q')
      continue;
    if (!used.contains(std::string(1, c)))
      return std::string(1, c);
  }
  return "";
}

std::string WhyDrawingNoteRefused(AppState& state, const DrawingNote& note) {
  if (!state.drawing)
    return "the active document is not a drawing";
  if (note.style == NoteStyle::Datum) {
    // A datum feature is named once per sheet, by a letter that cannot be
    // read as a digit: two datum A's would make every frame naming A
    // ambiguous.
    if (const std::string why = ValidateDatumLetter(note.text); !why.empty())
      return why;
    for (const DrawingNote& other : state.drawing->GetNotes())
      if (other.id != note.id && other.style == NoteStyle::Datum &&
          other.text == note.text)
        return "datum " + note.text + " is already on this sheet (note " +
               std::to_string(other.id) + ")";
  }
  if (note.style == NoteStyle::Fcf) {
    if (const std::string why = ValidateFcf(note.fcf); !why.empty())
      return why;
    if (note.attachDim != 0 && !state.drawing->FindDim(note.attachDim))
      return "no dimension " + std::to_string(note.attachDim) +
             " on this sheet for the frame to stand under";
    if (note.attachDim == 0 && !note.leader.edge.IsSet() &&
        !note.leader.silFace.IsSet())
      return "a feature control frame points at its feature: give it a "
             "leader to the feature, or stand it under a dimension";
  }
  if ((note.style == NoteStyle::Datum || note.style == NoteStyle::Balloon) &&
      !note.leader.edge.IsSet() && !note.leader.silFace.IsSet())
    return std::string("a ") + NoteStyleToString(note.style) +
           " points at geometry: give it a leader to a feature";
  const drawing::ResolvedNote resolved = ResolveDrawingNote(state, note);
  if (!resolved.resolved)
    return resolved.error;
  return "";
}

glm::dvec2 AutoNotePosition(AppState& state, const DrawingNote& note) {
  if (!state.drawing)
    return note.pos;
  const drawing::DimPlaceContext ctx = SheetPlaceContext(state);
  // Where the leader lands, and which way is OUT of its view.
  glm::dvec2 anchor = note.pos;
  glm::dvec2 out(0.0, 1.0);
  const auto it = state.drawingRenders.find(note.viewId);
  if (it != state.drawingRenders.end()) {
    const drawing::SheetView& sv = it->second;
    glm::dvec2 at;
    if ((note.leader.edge.IsSet() || note.leader.silFace.IsSet()) &&
        drawing::RefPointOf(sv, note.leader, at))
      anchor = at;
    const glm::dvec2 centre = 0.5 * (sv.bboxMin + sv.bboxMax);
    if (glm::length(anchor - centre) > 1e-6)
      out = glm::normalize(anchor - centre);
  }
  // Rings outward from the anchor, the directions nearest "out" first; the
  // first spot whose box is clear wins.
  DrawingNote probe = note;
  constexpr double kPi = 3.14159265358979323846;
  const double base = std::atan2(out.y, out.x);
  for (double r = 10.0; r <= 120.0; r += 5.0)
    for (int k = 0; k < 24; ++k) {
      // 0, +15, -15, +30, -30 ... degrees from straight out.
      const double step = (k + 1) / 2 * (kPi / 12.0);
      const double a = base + (k % 2 == 0 ? step : -step);
      probe.pos = anchor + r * glm::dvec2(std::cos(a), std::sin(a));
      const drawing::ResolvedNote rn = ResolveDrawingNote(state, probe);
      glm::dvec2 mn, mx;
      drawing::NoteBox(rn, mn, mx);
      if (BoxIsClear(ctx, mn, mx))
        return probe.pos;
    }
  return anchor + 12.0 * out;
}

double DefaultGeoTolerance(GeoSymbol symbol, double featureMM) {
  // ISO 2768-2, class K (medium), by nominal length.
  const double len = featureMM > 0.0 ? featureMM : 10.0;
  const auto band = [len](std::initializer_list<double> limits,
                          std::initializer_list<double> values) {
    auto v = values.begin();
    for (const double limit : limits) {
      if (len <= limit)
        return *v;
      ++v;
    }
    return *v;
  };
  switch (symbol) {
  case GeoSymbol::Parallelism:
  case GeoSymbol::Perpendicularity:
  case GeoSymbol::Angularity: // Table 2, perpendicularity
    return band({100, 300, 1000}, {0.4, 0.6, 0.8, 1.0});
  case GeoSymbol::Symmetry: // Table 3
    return 0.6;
  case GeoSymbol::CircularRunout:
  case GeoSymbol::TotalRunout:
  case GeoSymbol::Coaxiality: // Table 4, circular run-out
    return 0.2;
  default: // Table 1, straightness and flatness
    return band({10, 30, 100, 300, 1000}, {0.05, 0.1, 0.2, 0.4, 0.6, 0.8});
  }
}

std::string WhyDetailRefused(AppState& state, const DetailSpec& d) {
  if (!state.drawing)
    return "the active document is not a drawing";
  const DrawingView* parent = state.drawing->FindView(d.parentViewId);
  if (parent == nullptr)
    return "detail.view: no view " + std::to_string(d.parentViewId) +
           " on this sheet";
  if (parent->detail.on)
    return "detail.view names a detail; a detail is of a view (detail its "
           "parent at a larger scale instead)";
  if (!(d.radius > 0.0))
    return "detail.radius_mm is the circle's radius in model mm, above 0";
  if (const std::string why = ValidateDatumLetter(d.label); !why.empty())
    return "detail.label: " + why;
  return "";
}

glm::dvec3 SheetPointToWorld(AppState& state, const DrawingView& view,
                             const glm::dvec2& sheet) {
  // The inverse of what DetailCircles draws: sheet = pos + s * (p.x, p.y)
  // in the viewing system ProjectPoint uses.
  glm::dvec3 viewDir, up;
  ViewAxes(view, viewDir, up);
  const glm::dvec3 d = glm::normalize(viewDir);
  const glm::dvec3 x = glm::normalize(glm::cross(d, up));
  const glm::dvec3 y = glm::cross(-d, x);
  const double s = view.scale != 0.0 ? view.scale : 1.0;
  const glm::dvec2 uv = (sheet - DrawingFoldedPosition(state, view).pos) / s;
  return uv.x * x + uv.y * y;
}

double DefaultDetailScale(AppState& state, const DrawingView& parent,
                          double radiusMM) {
  const double parentScale = parent.scale > 0.0 ? parent.scale : 1.0;
  const SheetChrome chrome = ChromeOf(state.drawing->GetSheet());
  const glm::dvec2 room = chrome.borderMax - chrome.borderMin;
  const double fit = std::min(room.x, room.y) / 3.0;
  static const double kScales[] = {50.0, 20.0, 10.0, 5.0,  2.0,  1.0,
                                   0.5,  0.2,  0.1,  0.05, 0.02, 0.01};
  for (const double s : kScales)
    if (s >= 2.0 * parentScale - 1e-12 && 2.0 * radiusMM * s <= fit)
      return s;
  // A circle too big to enlarge within that: twice the parent, the least a
  // detail is for.
  return 2.0 * parentScale;
}

glm::dvec2 AutoDetailPosition(AppState& state, const DrawingView& detail) {
  const DrawingView* parent =
      state.drawing->FindView(detail.detail.parentViewId);
  const drawing::DimPlaceContext ctx = SheetPlaceContext(state);
  const double R = detail.scale * detail.detail.radius;
  // Near where the circle is drawn on its parent.
  glm::dvec2 aim = 0.5 * (ctx.borderMin + ctx.borderMax);
  if (parent != nullptr) {
    glm::dvec3 vd, up;
    ViewAxes(*parent, vd, up);
    aim = DrawingFoldedPosition(state, *parent).pos +
          parent->scale * drawing::ProjectPoint(detail.detail.center, vd, up);
  }
  // The detail's box: its circle, and the caption 9 mm under it.
  const auto boxOf = [&](const glm::dvec2& c, glm::dvec2& mn, glm::dvec2& mx) {
    mn = c - glm::dvec2(R + 2.0, R + 14.0);
    mx = c + glm::dvec2(R + 2.0, R + 2.0);
  };
  glm::dvec2 best = aim;
  double bestDist = std::numeric_limits<double>::max();
  for (double x = ctx.borderMin.x; x <= ctx.borderMax.x; x += 5.0)
    for (double y = ctx.borderMin.y; y <= ctx.borderMax.y; y += 5.0) {
      glm::dvec2 mn, mx;
      boxOf({x, y}, mn, mx);
      const double dist = glm::length(glm::dvec2(x, y) - aim);
      if (dist < bestDist && BoxIsClear(ctx, mn, mx)) {
        best = {x, y};
        bestDist = dist;
      }
    }
  return best;
}

namespace {

double PolylineLength(const std::vector<glm::dvec2>& pts) {
  double L = 0.0;
  for (size_t i = 1; i < pts.size(); ++i)
    L += glm::length(pts[i] - pts[i - 1]);
  return L;
}

double PointToPolyline(const glm::dvec2& p,
                       const std::vector<glm::dvec2>& pts) {
  double best = 1e300;
  for (size_t i = 1; i < pts.size(); ++i) {
    const glm::dvec2 a = pts[i - 1], ab = pts[i] - a;
    const double L2 = glm::dot(ab, ab);
    const double t =
        L2 < 1e-18 ? 0.0 : std::clamp(glm::dot(p - a, ab) / L2, 0.0, 1.0);
    best = std::min(best, glm::length(p - (a + ab * t)));
  }
  if (pts.size() == 1)
    best = glm::length(p - pts[0]);
  return best;
}

// Where a balloon's leader may end on one occurrence of a view: points its
// refs resolve to (a line's middle, a curve's ends, its extremes) that lie
// on a VISIBLE piece of that edge and `clearance` mm clear of every other
// part's lines -- a dot where two parts meet says neither.
struct AnchorRef {
  ProjRef ref;
  glm::dvec2 at{0.0};
  double cost = 0.0;
};

std::vector<AnchorRef> BalloonAnchors(const drawing::SheetView& sv,
                                      uint64_t viewId, uint64_t occurrence,
                                      double clearance) {
  std::vector<AnchorRef> out;
  std::vector<const drawing::SheetCurve*> others;
  for (const drawing::SheetCurve& c : sv.curves)
    if (c.visible && c.sourceId != occurrence)
      others.push_back(&c);
  std::set<std::string> seen;
  for (const drawing::SheetCurve& c : sv.curves) {
    if (c.sourceId != occurrence || !c.visible || c.points.size() < 2)
      continue;
    const bool isEdge = c.edge.IsSet();
    const ElementRef& er = isEdge ? c.edge : c.silFace;
    if (!er.IsSet())
      continue;
    const std::string key =
        (isEdge ? "e|" : "s|") + std::to_string(er.featureId) + "|" + er.name;
    if (!seen.insert(key).second)
      continue;
    // The visible pieces of this ref, which a point must lie on.
    std::vector<const drawing::SheetCurve*> pieces;
    for (const drawing::SheetCurve& o : sv.curves)
      if (o.sourceId == occurrence && o.visible &&
          (isEdge ? o.edge : o.silFace).featureId == er.featureId &&
          (isEdge ? o.edge : o.silFace).name == er.name)
        pieces.push_back(&o);
    double shown = 0.0;
    for (const auto* q : pieces)
      shown += PolylineLength(q->points);
    if (shown < 1.0)
      continue; // a speck: a dot on it would be a dot on its neighbour
    ProjRef base;
    base.viewId = viewId;
    base.occurrenceId = occurrence;
    (isEdge ? base.edge : base.silFace) = er;
    struct Variant {
      int end;
      const char* extreme;
      double cost;
    };
    static const Variant kVariants[] = {
        {0, "", 0.0},      {1, "", 3.0},    {2, "", 3.0},      {0, "left", 1.5},
        {0, "right", 1.5}, {0, "top", 1.5}, {0, "bottom", 1.5}};
    for (const Variant& v : kVariants) {
      if (v.end == 0 && v.extreme[0] == '\0' && c.conicRadius > 0.0)
        continue; // a circle's "whole" is its centre: not on the part
      ProjRef r = base;
      r.end = v.end;
      r.extreme = v.extreme;
      glm::dvec2 at;
      if (!drawing::RefPointOf(sv, r, at))
        continue;
      bool onVisible = false;
      for (const auto* q : pieces)
        onVisible = onVisible || PointToPolyline(at, q->points) < 0.05;
      if (!onVisible)
        continue;
      bool clear = true;
      for (const auto* o : others)
        if (PointToPolyline(at, o->points) < clearance) {
          clear = false;
          break;
        }
      if (!clear)
        continue;
      bool dup = false;
      for (const AnchorRef& a : out)
        dup = dup || glm::length(a.at - at) < 0.5;
      if (!dup)
        out.push_back({r, at, v.cost});
    }
  }
  return out;
}

double ShownLength(const drawing::SheetView& sv, uint64_t occurrence) {
  double L = 0.0;
  for (const drawing::SheetCurve& c : sv.curves)
    if (c.sourceId == occurrence && c.visible)
      L += PolylineLength(c.points);
  return L;
}

} // namespace

bool LayoutDrawingBalloons(AppState& state,
                           const std::vector<uint64_t>& viewIds,
                           BalloonLayoutReport& report, std::string* error,
                           bool update) {
  const auto fail = [&](std::string why) {
    if (error)
      *error = std::move(why);
    return false;
  };
  if (!state.drawing)
    return fail("the active document is not a drawing");
  if (viewIds.empty())
    return fail("name the views the balloons go round (`views`)");
  uint64_t source = 0;
  for (uint64_t id : viewIds) {
    const DrawingView* v = state.drawing->FindView(id);
    if (!v)
      return fail("no view " + std::to_string(id) + " on this sheet");
    if (!AssemblyDocById(state, v->sourceDocId))
      return fail("view '" + v->name +
                  "' draws a part; balloons number an assembly's parts");
    if (source != 0 && v->sourceDocId != source)
      return fail("the views are of different assemblies; balloons number "
                  "one parts list");
    source = v->sourceDocId;
  }
  if (update)
    UpdateDrawingRendersNow(state);
  for (uint64_t id : viewIds)
    if (DrawingViewProjecting(state, id))
      return fail("view '" + state.drawing->FindView(id)->name +
                  "' is still projecting; lay the balloons out once it has "
                  "landed");
  for (uint64_t id : viewIds)
    if (!state.drawingRenders.contains(id) ||
        !state.drawingRenders.at(id).hasGeometry)
      return fail("view " + std::to_string(id) + " has no projection: " +
                  (state.drawingRenders.contains(id)
                       ? state.drawingRenders.at(id).error
                       : std::string("update the drawing")));
  // Numbered by the sheet's parts list of this assembly, as a balloon
  // resolves its number (else by the first view's own).
  const DrawingView* listed = state.drawing->FindView(viewIds.front());
  for (const DrawingNote& n : state.drawing->GetNotes())
    if (n.table == NoteTable::Bom)
      if (const DrawingView* v = state.drawing->FindView(n.viewId);
          v && v->sourceDocId == source) {
        listed = v;
        break;
      }
  std::string why;
  const auto bom = ViewBom(state, *listed, &why);
  if (!bom)
    return fail(why);

  SnapshotDrawing(state);
  // The balloons already round these views go: this lays them out again.
  {
    auto& notes = state.drawing->GetNotes();
    notes.erase(std::remove_if(notes.begin(), notes.end(),
                               [&](const DrawingNote& n) {
                                 return n.style == NoteStyle::Balloon &&
                                        n.text.empty() &&
                                        std::find(viewIds.begin(),
                                                  viewIds.end(),
                                                  n.viewId) != viewIds.end();
                               }),
                notes.end());
    state.drawing->BumpVersion();
  }

  // Each row on the view that draws the most of it.
  struct Item {
    int row = 0;
    uint64_t occurrence = 0;
    std::vector<AnchorRef> anchors;
  };
  std::map<uint64_t, std::vector<Item>> byView;
  for (size_t r = 0; r < bom->rows.size(); ++r) {
    double best = 0.0;
    uint64_t bestView = 0, bestOcc = 0;
    std::vector<AnchorRef> bestAnchors;
    for (uint64_t v : viewIds) {
      const drawing::SheetView& sv = state.drawingRenders.at(v);
      for (uint64_t occ : bom->rows[r].rootOccurrences) {
        const double L = ShownLength(sv, occ);
        if (L <= best)
          continue;
        // Clear of its neighbours by 0.8 mm where it can be; a screw in
        // its washer on a plate has no edge that far from another part's,
        // and then the best it has (0.3 mm, then any visible point).
        std::vector<AnchorRef> anchors;
        for (double clearance : {0.8, 0.3, 0.0}) {
          anchors = BalloonAnchors(sv, v, occ, clearance);
          if (!anchors.empty())
            break;
        }
        if (anchors.empty())
          continue;
        best = L;
        bestView = v;
        bestOcc = occ;
        bestAnchors = std::move(anchors);
      }
    }
    if (bestView == 0) {
      report.unplaced.push_back(
          "item " + std::to_string(r + 1) + " (" + bom->rows[r].partName +
          "): none of these views shows a visible edge of it");
      continue;
    }
    byView[bestView].push_back(
        {static_cast<int>(r + 1), bestOcc, std::move(bestAnchors)});
  }

  // What the balloons must stay clear of, gathered once and grown as each
  // view's balloons land.
  const SheetChrome chrome = ChromeOf(state.drawing->GetSheet());
  std::vector<std::pair<glm::dvec2, glm::dvec2>> taken;
  taken.push_back({chrome.titleBlockMin, chrome.titleBlockMax});
  AvoidViewTexts(state, taken);
  for (const DrawingDim& dm : state.drawing->GetDims()) {
    const drawing::ResolvedDim rd = ResolveDrawingDim(state, dm);
    if (!rd.resolved)
      continue;
    glm::dvec2 mn, mx;
    drawing::DimLabelBox(rd, mn, mx);
    taken.push_back({mn, mx});
  }
  for (const DrawingNote& n : state.drawing->GetNotes()) {
    const auto rn = ResolveDrawingNote(state, n);
    glm::dvec2 mn, mx;
    drawing::NoteBox(rn, mn, mx);
    taken.push_back({mn, mx});
  }
  for (uint64_t v : viewIds) {
    auto it = byView.find(v);
    if (it == byView.end())
      continue;
    const drawing::SheetView& sv = state.drawingRenders.at(v);
    drawing::BalloonLayoutInput in;
    in.viewMin = sv.bboxMin;
    in.viewMax = sv.bboxMax;
    in.borderMin = chrome.borderMin;
    in.borderMax = chrome.borderMax;
    in.avoid = taken;
    for (const auto& [id, other] : state.drawingRenders)
      if (id != v && other.hasGeometry)
        in.avoid.push_back({other.bboxMin, other.bboxMax});
    for (const Item& item : it->second) {
      drawing::BalloonItem bi;
      drawing::ResolvedNote probe;
      probe.style = NoteStyle::Balloon;
      probe.text = std::to_string(item.row);
      glm::dvec2 mn, mx;
      drawing::NoteBox(probe, mn, mx);
      bi.radius = 0.5 * (mx.x - mn.x);
      for (const AnchorRef& a : item.anchors)
        bi.anchors.push_back({a.at, a.cost});
      in.items.push_back(std::move(bi));
    }
    const drawing::BalloonLayout layout = drawing::LayoutBalloons(in);
    if (!layout.error.empty()) {
      const DrawingView* dv = state.drawing->FindView(v);
      return fail("view '" + (dv ? dv->name : std::to_string(v)) +
                  "': " + layout.error);
    }
    report.crossings += layout.crossings;
    for (size_t i = 0; i < it->second.size(); ++i) {
      const Item& item = it->second[i];
      const drawing::BalloonPlacement& pl = layout.placements[i];
      DrawingNote n;
      n.style = NoteStyle::Balloon;
      n.viewId = v;
      n.leader = item.anchors[pl.anchor].ref;
      n.pos = pl.centre;
      const uint64_t id = state.drawing->AddNote(n);
      const double r = in.items[i].radius;
      taken.push_back({pl.centre - glm::dvec2(r), pl.centre + glm::dvec2(r)});
      report.placed.push_back({id, v, item.occurrence, item.row,
                               bom->rows[item.row - 1].partName, pl.centre});
    }
  }
  return true;
}

} // namespace ui
