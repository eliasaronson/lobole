#include "ui/VariableActions.h"
#include "ui/CatalogActions.h"
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

// Project-wide variables: the one table every part resolves against, its
// cross-tab undo, and the global undo router that has to decide whether
// Ctrl+Z means the active tab or a project-var edit made on another one.
//
// VariablesPanel.cpp draws them; this is what editing one DOES.
// Declared in ui/VariableActions.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// ---- Project-wide variables (Phase V) -------------------------------------

void SyncProjectScope(AppState& state) {
  // Re-evaluate the project table (project vars may reference each other — the
  // shared topo-sort orders them), then regenerate every part against it. A
  // changed project value bakes into each part's feature JSON, so dependent
  // features across all tabs invalidate through the regen cache naturally.
  EvaluateParameterTable(state.projectParameters, state.projectSymbols, nullptr,
                         {});
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (Document* d = SlotDoc(state, i)) {
      d->SetProjectSymbols(&state.projectSymbols);
      d->SetProjectParameters(&state.projectParameters);
      d->Regenerate();
    }
  }
  state.syncedDocumentVersion = UINT64_MAX; // a cross-tab edit; force resync
}

const Parameter* FindProjectParameter(const AppState& state,
                                      const std::string& name) {
  for (const Parameter& p : state.projectParameters)
    if (p.name == name)
      return &p;
  return nullptr;
}

std::vector<std::string> ProjectVarReferences(AppState& state,
                                              const std::string& name) {
  const auto& engine = ExpressionEngine::Default();
  const auto references = [&](const std::string& text) {
    if (text.empty())
      return false;
    const auto compiled = engine.Compile(text);
    return compiled.has_value() && (*compiled)->Dependencies().contains(name);
  };
  std::vector<std::string> refs;
  for (const Parameter& p : state.projectParameters)
    if (p.name != name && references(p.expression))
      refs.push_back("project variable '" + p.name + "'");
  // A project variable's references span every part (walk all slots, not just
  // the active doc); a part with a same-named LOCAL variable shadows it, so its
  // references belong to the local one and don't block the project delete.
  for (size_t i = 0; i < state.parts.size(); i++) {
    Document* d = SlotDoc(state, i);
    if (d == nullptr || d->FindParameter(name) != nullptr)
      continue;
    for (const std::string& r : d->FindParameterReferences(name))
      refs.push_back(state.parts[i].name + ": " + r);
  }
  return refs;
}

namespace {
// Applies a project-var undo/redo snapshot: replay the cross-part reference
// rewrite (rename only), restore the table, regenerate all.
void ApplyProjectVarSnapshot(AppState& state, const ProjectVarSnapshot& s) {
  if (!s.docRefFrom.empty()) {
    for (size_t i = 0; i < state.parts.size(); i++)
      if (Document* d = SlotDoc(state, i))
        d->RewriteProjectVarRefs(s.docRefFrom, s.docRefTo);
  }
  state.projectParameters = s.params;
  SyncProjectScope(state);
}
// Pushes a project-var undo entry capturing the PRE-edit table (restored on
// undo) plus the reference rewrite undo must replay across parts (empty for
// set/delete; the reverse rename for a rename). Clears the redo stack.
void PushProjectVarUndo(AppState& state, std::vector<Parameter> preEdit,
                        std::string undoRefFrom, std::string undoRefTo) {
  ProjectVarSnapshot s;
  s.params = std::move(preEdit);
  s.stamp = NextActionStamp();
  s.docRefFrom = std::move(undoRefFrom);
  s.docRefTo = std::move(undoRefTo);
  state.projectVarHistory.undo.push_back(std::move(s));
  state.projectVarHistory.redo.clear();
}
} // namespace

bool SetProjectVariable(AppState& state, const std::string& name,
                        const std::string& expression,
                        const std::string& comment) {
  if (!ExpressionEngine::Default().IsValidParameterName(name))
    return false;
  PushProjectVarUndo(state, state.projectParameters, {}, {});
  const auto it = std::find_if(
      state.projectParameters.begin(), state.projectParameters.end(),
      [&](const Parameter& p) { return p.name == name; });
  if (it != state.projectParameters.end()) {
    it->expression = expression;
    it->comment = comment;
  } else {
    state.projectParameters.push_back({name, expression, comment, 0.0, {}});
  }
  SyncProjectScope(state);
  return true;
}

bool RemoveProjectVariable(AppState& state, const std::string& name,
                           std::vector<std::string>* outRefs) {
  const auto it = std::find_if(
      state.projectParameters.begin(), state.projectParameters.end(),
      [&](const Parameter& p) { return p.name == name; });
  if (it == state.projectParameters.end())
    return false;
  std::vector<std::string> refs = ProjectVarReferences(state, name);
  if (!refs.empty()) {
    if (outRefs != nullptr)
      *outRefs = std::move(refs);
    return false;
  }
  PushProjectVarUndo(state, state.projectParameters, {}, {});
  state.projectParameters.erase(std::find_if(
      state.projectParameters.begin(), state.projectParameters.end(),
      [&](const Parameter& p) { return p.name == name; }));
  SyncProjectScope(state);
  return true;
}

bool RenameProjectVariable(AppState& state, const std::string& oldName,
                           const std::string& newName) {
  if (FindProjectParameter(state, oldName) == nullptr ||
      FindProjectParameter(state, newName) != nullptr ||
      !ExpressionEngine::Default().IsValidParameterName(newName)) {
    return false;
  }
  // Undo restores the pre-edit table AND reverses the ref rewrite (newName ->
  // oldName) across every part.
  PushProjectVarUndo(state, state.projectParameters, newName, oldName);
  const auto& engine = ExpressionEngine::Default();
  for (Parameter& p : state.projectParameters) {
    if (p.name == oldName)
      p.name = newName;
    p.expression = engine.RenameIdentifier(p.expression, oldName, newName);
  }
  for (size_t i = 0; i < state.parts.size(); i++)
    if (Document* d = SlotDoc(state, i))
      d->RewriteProjectVarRefs(oldName, newName);
  SyncProjectScope(state);
  return true;
}

bool UndoProjectVars(AppState& state) {
  if (state.projectVarHistory.undo.empty())
    return false;
  ProjectVarSnapshot entry = std::move(state.projectVarHistory.undo.back());
  state.projectVarHistory.undo.pop_back();
  // Build the redo entry from the CURRENT (post-edit) state before restoring;
  // its ref rewrite is the inverse of the undo entry's.
  ProjectVarSnapshot redoEntry;
  redoEntry.params = state.projectParameters;
  redoEntry.stamp = entry.stamp;
  redoEntry.docRefFrom = entry.docRefTo;
  redoEntry.docRefTo = entry.docRefFrom;
  state.projectVarHistory.redo.push_back(std::move(redoEntry));
  ApplyProjectVarSnapshot(state, entry);
  return true;
}

bool RedoProjectVars(AppState& state) {
  if (state.projectVarHistory.redo.empty())
    return false;
  ProjectVarSnapshot entry = std::move(state.projectVarHistory.redo.back());
  state.projectVarHistory.redo.pop_back();
  ProjectVarSnapshot undoEntry;
  undoEntry.params = state.projectParameters;
  undoEntry.stamp = entry.stamp;
  undoEntry.docRefFrom = entry.docRefTo;
  undoEntry.docRefTo = entry.docRefFrom;
  state.projectVarHistory.undo.push_back(std::move(undoEntry));
  ApplyProjectVarSnapshot(state, entry);
  return true;
}

// ---- Global undo routing (Phase V) ----------------------------------------
// A project-variable edit spans every tab, so it can't ride any one document's
// CommandStack. Rather than merge the (separate, per-tab) stacks, every
// undoable action is stamped from one monotonic action clock (NextActionStamp);
// the global Undo/Redo route to whichever source holds the most recent stamp.
// This keeps each per-tab stack intact and correct while making Ctrl+Z undo the
// user's genuinely-last action even when it crossed into project scope.

namespace {
uint64_t ActiveTabTopUndoStamp(const AppState& state) {
  if (state.assembly)
    return state.assemblyHistory.undoStamps.empty()
               ? 0
               : state.assemblyHistory.undoStamps.back();
  if (state.drawing)
    return state.drawingHistory.undoStamps.empty()
               ? 0
               : state.drawingHistory.undoStamps.back();
  if (state.job)
    return state.jobHistory.undoStamps.empty()
               ? 0
               : state.jobHistory.undoStamps.back();
  return state.commands ? state.commands->TopUndoStamp() : 0;
}
uint64_t ActiveTabTopRedoStamp(const AppState& state) {
  if (state.assembly)
    return state.assemblyHistory.redoStamps.empty()
               ? 0
               : state.assemblyHistory.redoStamps.back();
  if (state.drawing)
    return state.drawingHistory.redoStamps.empty()
               ? 0
               : state.drawingHistory.redoStamps.back();
  if (state.job)
    return state.jobHistory.redoStamps.empty()
               ? 0
               : state.jobHistory.redoStamps.back();
  return state.commands ? state.commands->TopRedoStamp() : 0;
}
void ActiveTabUndo(AppState& state) {
  if (state.assembly)
    UndoAssembly(state);
  else if (state.drawing)
    UndoDrawing(state);
  else if (state.job)
    UndoJob(state);
  else if (state.commands && state.commands->CanUndo())
    state.commands->Undo(*state.document);
}
void ActiveTabRedo(AppState& state) {
  if (state.assembly)
    RedoAssembly(state);
  else if (state.drawing)
    RedoDrawing(state);
  else if (state.job)
    RedoJob(state);
  else if (state.commands && state.commands->CanRedo())
    state.commands->Redo(*state.document);
}
} // namespace

bool CanGlobalUndo(const AppState& state) {
  return !state.catalogHistory.undo.empty() ||
         !state.projectVarHistory.undo.empty() ||
         ActiveTabTopUndoStamp(state) != 0;
}
bool CanGlobalRedo(const AppState& state) {
  return !state.catalogHistory.redo.empty() ||
         !state.projectVarHistory.redo.empty() ||
         ActiveTabTopRedoStamp(state) != 0;
}
void GlobalUndo(AppState& state) {
  const uint64_t proj = state.projectVarHistory.undo.empty()
                            ? 0
                            : state.projectVarHistory.undo.back().stamp;
  const uint64_t catalogStamp = state.catalogHistory.undo.empty()
                                    ? 0
                                    : state.catalogHistory.undo.back().stamp;
  if (catalogStamp > proj && catalogStamp > ActiveTabTopUndoStamp(state)) {
    UndoCatalog(state);
    return;
  }
  // Undo the most recent action: the larger stamp wins (stamps are unique).
  if (proj != 0 && proj > ActiveTabTopUndoStamp(state))
    UndoProjectVars(state);
  else
    ActiveTabUndo(state);
}
void GlobalRedo(AppState& state) {
  uint64_t proj = state.projectVarHistory.redo.empty()
                      ? UINT64_MAX
                      : state.projectVarHistory.redo.back().stamp;
  uint64_t tab = ActiveTabTopRedoStamp(state);
  if (tab == 0)
    tab = UINT64_MAX;
  const uint64_t catalogStamp = state.catalogHistory.redo.empty()
                                    ? UINT64_MAX
                                    : state.catalogHistory.redo.back().stamp;
  if (catalogStamp < proj && catalogStamp < tab) {
    RedoCatalog(state);
    return;
  }
  if (proj == UINT64_MAX && tab == UINT64_MAX)
    return;
  // Redo the action undone most recently: the SMALLER redo-top stamp wins.
  if (proj < tab)
    RedoProjectVars(state);
  else
    ActiveTabRedo(state);
}

} // namespace ui
