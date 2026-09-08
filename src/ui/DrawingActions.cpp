#include "ui/DrawingActions.h"
#include "ui/DocumentSlots.h"
#include "ui/RendererSync.h"

#include "core/Units.h"
#include "ui/AppActionsInternal.h" // SharedBody / PartRenderCacheStore
#include "ui/CamPanel.h"           // RefreshCamToolpaths (job undo)
#include "ui/CamTool.h"            // RunCamVerification (job undo)
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
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
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
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>

// Projecting a drawing view: gathering the source bodies, running the
// projection, placing the view on the sheet, resolving its dimensions and
// exporting the result.
//
// DrawingPanel.cpp is the other half -- the sheet as the user manipulates
// it. This is what a view IS; that is how it is edited.
// Split out of AppActions.cpp (docs/architecture-review-2026-09-06.md,
// stage 3, second wave). AppActions.h still declares everything, so no
// caller changed.

namespace ui {

// ---- Drawing view projection (workstream 13, D2) --------------------------

namespace {

// One body of a view's source document, ready to project. `input.sourceId` is
// a slot index into the gathered list (how ProjectView tags each curve so the
// join finds this entry); `refId` is the SEMANTIC source a dimension attaches
// to — a body's createdBy for a part, or the occurrence id for an assembly.
struct SourceBody {
  drawing::InputBody input;
  uint64_t featureId = 0; // the body's createdBy (owner of its element names)
  uint64_t refId = 0;     // semantic source id -> SheetCurve.sourceId
  const ElementMap* map = nullptr;
};

// Appends one part document's bodies at the given world placement.
void AppendBodies(const Document& doc, const glm::dmat4& xf, uint64_t occId,
                  std::vector<SourceBody>& out) {
  for (const Body& body : doc.GetBodies()) {
    if (body.shape.GetShape().IsNull())
      continue;
    // A Derived ghost is not the part: a drawing of the gripper body is a
    // drawing of the body, not of the servo it was drawn on (and the HLR of
    // a 197-body camera ghost took the view past a minute).
    if (body.reference)
      continue;
    SourceBody sb;
    sb.input.shape = body.shape.GetShape();
    sb.input.transform = xf;
    sb.input.geometryRevision = doc.Version();
    sb.input.sourceId = out.size(); // slot index: picks this entry on join
    sb.featureId = body.createdBy;
    sb.refId = occId ? occId : body.createdBy; // occurrence, else body owner
    sb.map = &body.elementMap;
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
      const Document* doc = SlotDoc(state, i);
      if (!doc) {
        *error = "source document is empty";
        return false;
      }
      *sourceVersion = doc->Version();
      AppendBodies(*doc, glm::dmat4(1.0), 0, out);
      return true;
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
        AppendBodies(*inst.part, glm::dmat4(inst.world), inst.rootOccurrenceId,
                     out);
      }
      return true;
    }
    *error = "a drawing cannot be a source for another drawing";
    return false;
  }
  *error = "source document not found";
  return false;
}

// Projects one drawing view: resolves its source, runs HLR, places the curves
// on the sheet at the view's scale + position, and joins the index-space
// source refs to stable ElementRefs. A failure leaves `error` set and no
// geometry (callers keep the previous SheetView so a stale view isn't blanked).
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

drawing::SheetView ProjectDrawingView(AppState& state, const DrawingView& view,
                                      std::function<bool()> cancelled) {
  drawing::SheetView result;
  result.viewId = view.id;

  std::vector<SourceBody> bodies;
  std::string error;
  if (!GatherSourceBodies(state, view, bodies, &result.solvedSourceVersion,
                          &error)) {
    result.error = error;
    return result;
  }
  if (bodies.empty()) {
    result.error = "source has no geometry to project";
    return result;
  }

  std::vector<drawing::InputBody> inputs;
  inputs.reserve(bodies.size());
  for (const SourceBody& sb : bodies)
    inputs.push_back(sb.input);

  glm::dvec3 viewDir, up;
  OrientationAxes(view.orientation, viewDir, up);
  drawing::ProjectionOptions opts;
  opts.hiddenLines = view.showHidden;
  opts.cache = &state.drawingProjectionCache;
  opts.mode = view.quality == "fast" ? drawing::ProjectionMode::Poly
                                     : drawing::ProjectionMode::Exact;
  opts.cancelled = cancelled;
  auto projected = drawing::ProjectView(inputs, viewDir, up, opts, &error);
  if (!projected) {
    result.error = error;
    return result;
  }

  return AssembleSheetView(state, view, bodies, *projected);
}

// The cheap half of a view: place the projected curves on the sheet at the
// view's scale and folded position, and join the projection's index-space
// SourceRefs to stable ElementRefs.
//
// Split from the HLR so the expensive half can run OFF the main thread: this
// part needs the live documents (their ElementMaps) and the sheet layout, and
// the part it is split from needs neither (docs/agent-scale-followup-plan.md,
// W3).
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
      if (pc.source.edgeIndex >= 0 && sb->map) {
        if (const std::string* n = sb->map->EdgeNameOf(pc.source.edgeIndex))
          sc.edge = ElementRef{sb->featureId, *n};
      }
      if (pc.source.faceIndex >= 0 && sb->map) {
        if (const std::string* n = sb->map->NameOf(pc.source.faceIndex))
          sc.silFace = ElementRef{sb->featureId, *n};
      }
    }
    result.curves.push_back(std::move(sc));
  }
  if (!result.curves.empty()) {
    result.hasGeometry = true;
    result.bboxMin = lo;
    result.bboxMax = hi;
  }
  return result;
}

} // namespace

FoldedPosition DrawingFoldedPosition(AppState& state, const DrawingView& view) {
  // A projected child is kept aligned to its base along the shared sheet
  // axis (design §2). Two orthographic views share sheet-X when they map the
  // same world direction to it (equal `right` vectors), and sheet-Y when
  // their `up` vectors match; the shared coordinate locks to the base's,
  // leaving the fold axis free. Iso shares neither -> placed freely.
  FoldedPosition out{view.position, false, false};
  if (!view.baseViewId || !state.drawing)
    return out;
  const DrawingView* base = state.drawing->FindView(view.baseViewId);
  if (!base)
    return out;
  glm::dvec3 vd, up, bvd, bup;
  OrientationAxes(view.orientation, vd, up);
  OrientationAxes(base->orientation, bvd, bup);
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
      ctx.sourceDoc = DrawingSourceDocument(state, view->sourceDocId);
      ctx.viewScale = view->scale;
      // A fastener callout names the part an OCCURRENCE places, which is a
      // different document from the assembly the view is of. Resolved through
      // the VIEW's source assembly rather than through state.assembly: a
      // drawing is its own tab, so the assembly it draws is never the active
      // one while the drawing is on screen.
      const uint64_t sourceId = view->sourceDocId;
      ctx.occurrenceDoc = [&state,
                           sourceId](uint64_t occId) -> const Document* {
        const AssemblyDocument* asmDoc = AssemblyDocById(state, sourceId);
        if (asmDoc == nullptr)
          return nullptr;
        const Occurrence* occ = asmDoc->FindById(occId);
        return occ ? PartDocById(state, occ->partId) : nullptr;
      };
    }
  }
  return drawing::ResolveDim(dim, it->second, ctx);
}

glm::dvec2 AutoDimOffset(AppState& state, const DrawingDim& dim) {
  const auto it = state.drawingRenders.find(dim.viewId);
  if (it == state.drawingRenders.end() || !state.drawing)
    return {0.0, -12.0};
  drawing::DimPlaceContext ctx;
  const SheetChrome chrome = ChromeOf(state.drawing->GetSheet());
  ctx.borderMin = chrome.borderMin;
  ctx.borderMax = chrome.borderMax;
  ctx.avoid.push_back({chrome.titleBlockMin, chrome.titleBlockMax});
  for (const auto& [vid, sv] : state.drawingRenders)
    if (sv.hasGeometry)
      ctx.avoid.push_back({sv.bboxMin, sv.bboxMax});
  for (const DrawingDim& dm : state.drawing->GetDims()) {
    const drawing::ResolvedDim rd = ResolveDrawingDim(state, dm);
    if (!rd.resolved)
      continue;
    glm::dvec2 mn(0.0), mx(0.0);
    drawing::LabelBox(rd.labelPos, rd.text, mn, mx);
    ctx.avoid.push_back({mn, mx});
  }
  for (const auto& note : state.drawing->GetNotes()) {
    const auto resolved = ResolveDrawingNote(state, note);
    glm::dvec2 mn, mx;
    drawing::LabelBox(resolved.pos, resolved.text, mn, mx);
    ctx.avoid.push_back({mn, mx});
  }
  return drawing::PlaceDimOffset(dim, it->second, ctx);
}

drawing::ResolvedNote ResolveDrawingNote(AppState& state,
                                         const DrawingNote& note) {
  SymbolTable symbols = state.projectSymbols;
  const DrawingView* view = state.drawing->FindView(note.viewId);
  if (view) {
    if (const Document* doc = DrawingSourceDocument(state, view->sourceDocId)) {
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
  return drawing::ResolveNote(
      note, it == state.drawingRenders.end() ? nullptr : &it->second, symbols);
}

bool ExportActiveDrawing(AppState& state, const std::string& path,
                         std::string* error) {
  if (!state.drawing) {
    if (error)
      *error = "no active drawing";
    return false;
  }
  UpdateDrawingRenders(state); // export reflects current geometry
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
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".svg")
    return DrawingExport::SaveSVG(*state.drawing, views, dims, path, error,
                                  notes);
  if (ext == ".pdf")
    return DrawingExport::SavePDF(*state.drawing, views, dims, path, error,
                                  notes);
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

uint64_t AddDrawingViewPlaced(AppState& state, DrawingView v) {
  if (!state.drawing)
    return 0;
  DrawingDocument& dwg = *state.drawing;
  const glm::dvec2 ext = ChromeOf(dwg.GetSheet()).extent;

  // Project a 1:1 copy anchored at the origin: its bbox IS the part's size in
  // millimetres, which is what both decisions below need.
  DrawingView probe = v;
  probe.id = 0;
  probe.scale = 1.0;
  probe.position = glm::dvec2(0.0);
  probe.baseViewId = 0; // a folded child would inherit its parent's place
  const drawing::SheetView sv = ProjectDrawingView(state, probe, nullptr);
  const bool measured = sv.hasGeometry;
  const glm::dvec2 size = measured ? sv.bboxMax - sv.bboxMin : glm::dvec2(0.0);

  // Scale (only when the caller did not choose one). Views of the SAME part
  // share a scale: a right view at 2:1 beside a front view at 1:1 is a
  // drafting error, and it is what the fit-each-view-alone rule below
  // produced for a plate (the right view is the smallest face, so it got
  // blown up until it collided with the top view). Matching first, fitting
  // only when this is the part's first view.
  if (v.scale <= 0.0) {
    for (const DrawingView& other : dwg.GetViews()) {
      if (other.sourceDocId == v.sourceDocId && other.scale > 0.0) {
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
  // model, so a left edge at `x` means pos.x = x - scale * bboxMin.x.
  glm::dvec2 pos = ext * 0.5;
  if (measured) {
    const SheetChrome chrome = ChromeOf(dwg.GetSheet());
    std::vector<Span> takenX;
    double lowest = 1e30, firstMidY = ext.y * 0.5;
    bool anyPlaced = false;
    for (const DrawingView& other : dwg.GetViews()) {
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
    const Span incoming{v.scale * sv.bboxMin.x, v.scale * sv.bboxMax.x};
    const double gap = std::max(8.0, std::max(placed.x, placed.y) * 0.15);
    if (!anyPlaced) {
      pos = ext * 0.5 - v.scale * (sv.bboxMin + sv.bboxMax) * 0.5;
    } else if (const double dx = ClearOffset(takenX, incoming, 8.0);
               dx + v.scale * sv.bboxMax.x <= chrome.borderMax.x) {
      pos.x = dx; // same row, to the right of everything placed
      pos.y = firstMidY - v.scale * (sv.bboxMin.y + sv.bboxMax.y) * 0.5;
    } else {
      // A new row BELOW the others (sheet Y grows upward), starting at the
      // left margin — and stopping above the title block, which a wrapped
      // row used to land on top of.
      pos.x = chrome.borderMin.x + gap - v.scale * sv.bboxMin.x;
      pos.y = lowest - gap - v.scale * sv.bboxMax.y;
      const Span rowX{pos.x + v.scale * sv.bboxMin.x,
                      pos.x + v.scale * sv.bboxMax.x};
      const Span blockX{chrome.titleBlockMin.x, chrome.titleBlockMax.x};
      const double floorY =
          Overlaps(rowX, blockX) ? chrome.titleBlockMax.y : chrome.borderMin.y;
      if (pos.y + v.scale * sv.bboxMin.y < floorY + gap)
        pos.y = floorY + gap - v.scale * sv.bboxMin.y;
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
  if (measured) {
    const SheetChrome chrome = ChromeOf(dwg.GetSheet());
    const glm::dvec2 newMin = pos + v.scale * sv.bboxMin;
    const glm::dvec2 newMax = pos + v.scale * sv.bboxMax;
    bool collides =
        BoxesOverlap(newMin, newMax, chrome.titleBlockMin,
                     chrome.titleBlockMax) ||
        newMin.x < chrome.borderMin.x || newMin.y < chrome.borderMin.y ||
        newMax.x > chrome.borderMax.x || newMax.y > chrome.borderMax.y;
    for (const DrawingView& other : dwg.GetViews()) {
      const auto it = state.drawingRenders.find(other.id);
      if (it != state.drawingRenders.end() && it->second.hasGeometry &&
          BoxesOverlap(newMin, newMax, it->second.bboxMin, it->second.bboxMax))
        collides = true;
    }
    if (collides) {
      std::vector<DrawingView*> laid;
      std::vector<ViewFootprint> items;
      for (DrawingView& other : dwg.GetViews()) {
        const auto it = state.drawingRenders.find(other.id);
        if (it == state.drawingRenders.end() || !it->second.hasGeometry ||
            other.scale <= 0.0)
          continue;
        laid.push_back(&other);
        items.push_back({(it->second.bboxMin - other.position) / other.scale,
                         (it->second.bboxMax - other.position) / other.scale});
      }
      items.push_back({sv.bboxMin, sv.bboxMax});
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
  }

  SnapshotDrawing(state);
  const uint64_t id = dwg.AddView(v);
  UpdateDrawingRenders(state);
  return id;
}

void UpdateDrawingRenders(AppState& state, double budgetSeconds) {
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
  // Always gather current leaf geometry and placements. The projection cache
  // checks those actual dependencies, so nested edits cannot slip through an
  // unchanged root assembly version. Sheet-only edits just reassemble curves.
  const uint64_t drawingVersion = state.drawing->Version();
  std::map<uint64_t, drawing::SheetView> fresh;
  for (const DrawingView& view : state.drawing->GetViews()) {
    // Before starting, as well as during. A single body's Hide() is ONE
    // uninterruptible OCC call — the pairwise drive only has somewhere to
    // stop when there are several shapes to classify against each other — so
    // without this a sheet whose budget went on view one would still pay for
    // views two and three in full.
    drawing::SheetView sv;
    if (outOfBudget()) {
      sv.viewId = view.id;
      sv.solvedSourceVersion = SourceVersionOf(state, view);
      sv.error = "cancelled";
    } else {
      sv = ProjectDrawingView(state, view, outOfBudget);
    }
    if (sv.error == "cancelled") {
      // Out of budget rather than broken. Say which, and say the way through:
      // a `fast` view of the same geometry is a couple of hundred
      // milliseconds against a minute and a half.
      sv.error = "ran out of the update's time budget (" +
                 std::to_string(static_cast<int>(budgetSeconds)) +
                 " s) — exact HLR on this source is slower than that. Raise "
                 "the budget, or set this view's quality to \"fast\" "
                 "(polygonal HLR: orders of magnitude cheaper, and a look "
                 "rather than a dimensionable deliverable).";
    }
    // Keep last-good geometry on failure (design §1): a failed view shows its
    // previous curves with an error, rather than blanking the sheet.
    if (!sv.error.empty() && !sv.hasGeometry) {
      const auto prev = state.drawingRenders.find(view.id);
      if (prev != state.drawingRenders.end() && prev->second.hasGeometry) {
        drawing::SheetView kept = prev->second;
        kept.error = sv.error;
        kept.solvedSourceVersion = sv.solvedSourceVersion;
        fresh[view.id] = std::move(kept);
        continue;
      }
    }
    fresh[view.id] = std::move(sv);
  }
  state.drawingRenders = std::move(fresh);
  state.drawingRendersVersion = drawingVersion;
}

} // namespace ui
