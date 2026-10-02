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
#include "core/fem/FemSolver.h"
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
  // The tables' cells are the outermost scope: a variable may read one
  // (bore = lookup("bearings", 6002, "d")), and a part reads them through
  // the project symbols.
  SymbolTable cells;
  lookuptable::AddCellSymbols(state.projectTables, cells);
  EvaluateParameterTable(state.projectParameters, state.projectSymbols, &cells,
                         {});
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (Document* d = SlotDoc(state, i)) {
      d->SetProjectSymbols(&state.projectSymbols);
      d->SetProjectParameters(&state.projectParameters);
      d->Regenerate();
      // A result whose inputs the new values did not touch is still the
      // answer (an open regenerates every part once more, here).
      fem::RestampByInputs(*d);
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
// The features an entry names, as they stand now: what the opposite entry
// (redo for an undo, undo for a redo) puts back.
std::vector<ProjectVarSnapshot::FeatureState>
CurrentFeatureStates(AppState& state,
                     const std::vector<ProjectVarSnapshot::FeatureState>& of) {
  std::vector<ProjectVarSnapshot::FeatureState> out;
  for (const ProjectVarSnapshot::FeatureState& f : of)
    // the live part: its authored features, not a placement of it
    if (Document* d = PartDocById(state, f.partId))
      if (const Feature* feature = d->FindFeatureById(f.featureId))
        out.push_back(
            {f.partId, f.featureId, DocumentIO::FeatureToJson(*feature)});
  return out;
}

// Applies a project-var undo/redo snapshot: put back the features it names,
// replay the cross-part reference rewrite (rename only), restore the table,
// regenerate all.
void ApplyProjectVarSnapshot(AppState& state, const ProjectVarSnapshot& s) {
  // A part whose features come back regenerates ONCE, after the project
  // table is evaluated: the guards defer it past SyncProjectScope, so a
  // feature reading a variable this entry restores never sees the old table.
  std::vector<std::unique_ptr<Document::BulkEdit>> guards;
  std::set<Document*> guarded;
  for (const ProjectVarSnapshot::FeatureState& f : s.features) {
    // the live part: its authored features, not a placement of it
    Document* d = PartDocById(state, f.partId);
    size_t index = 0;
    if (d == nullptr || d->FindFeatureById(f.featureId, &index) == nullptr)
      continue;
    if (guarded.insert(d).second)
      guards.push_back(std::make_unique<Document::BulkEdit>(*d));
    d->ReplaceFeature(index, DocumentIO::FeatureFromJson(f.json));
  }
  if (!s.docRefFrom.empty()) {
    for (size_t i = 0; i < state.parts.size(); i++)
      if (Document* d = SlotDoc(state, i))
        d->RewriteProjectVarRefs(s.docRefFrom, s.docRefTo);
  }
  state.projectParameters = s.params;
  state.projectTables = s.tables;
  state.projectTemplates = s.templates;
  SyncProjectScope(state);
}
// Pushes a project-var undo entry capturing the PRE-edit table (restored on
// undo) plus the reference rewrite undo must replay across parts (empty for
// set/delete; the reverse rename for a rename). Clears the redo stack.
void PushProjectVarUndo(AppState& state, std::vector<Parameter> preEdit,
                        std::string undoRefFrom, std::string undoRefTo) {
  ProjectVarSnapshot s;
  s.params = std::move(preEdit);
  s.tables = state.projectTables;
  s.templates = state.projectTemplates;
  s.stamp = NextActionStamp();
  s.docRefFrom = std::move(undoRefFrom);
  s.docRefTo = std::move(undoRefTo);
  state.projectVarHistory.undo.push_back(std::move(s));
  state.projectVarHistory.redo.clear();
}
} // namespace

void PushProjectVarUndoWithFeatures(
    AppState& state, std::vector<ProjectVarSnapshot::FeatureState> features) {
  PushProjectVarUndo(state, state.projectParameters, {}, {});
  state.projectVarHistory.undo.back().features = std::move(features);
}

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

std::string SetProjectTable(AppState& state, lookuptable::Table table) {
  if (std::string why = lookuptable::Validate(table); !why.empty())
    return why;
  PushProjectVarUndo(state, state.projectParameters, {}, {});
  auto it = std::find_if(
      state.projectTables.begin(), state.projectTables.end(),
      [&](const lookuptable::Table& t) { return t.name == table.name; });
  if (it != state.projectTables.end())
    *it = std::move(table);
  else
    state.projectTables.push_back(std::move(table));
  SyncProjectScope(state);
  return {};
}

std::vector<std::string> ProjectTableReferences(AppState& state,
                                                const std::string& name) {
  // A reference is a lookup whose table is this one, by its quoted name:
  // the text holds "name" or 'name' right after lookup( -- the parser's
  // own rule for a table, read without compiling every expression.
  const auto references = [&](const std::string& text) {
    for (size_t at = text.find("lookup"); at != std::string::npos;
         at = text.find("lookup", at + 1)) {
      size_t i = at + 6;
      while (i < text.size() &&
             std::isspace(static_cast<unsigned char>(text[i])))
        i++;
      if (i >= text.size() || text[i] != '(')
        continue;
      i++;
      while (i < text.size() &&
             std::isspace(static_cast<unsigned char>(text[i])))
        i++;
      if (i < text.size() && (text[i] == '"' || text[i] == '\'') &&
          text.compare(i + 1, name.size(), name) == 0 &&
          i + 1 + name.size() < text.size() &&
          text[i + 1 + name.size()] == text[i])
        return true;
    }
    return false;
  };
  std::vector<std::string> refs;
  for (const Parameter& p : state.projectParameters)
    if (references(p.expression))
      refs.push_back("project variable '" + p.name + "'");
  for (size_t i = 0; i < state.parts.size(); i++) {
    Document* d = SlotDoc(state, i);
    if (d == nullptr)
      continue;
    for (const Parameter& p : d->GetParameters())
      if (references(p.expression))
        refs.push_back(state.parts[i].name + ": variable '" + p.name + "'");
    for (const auto& f : d->GetFeatures()) {
      bool hit = false;
      f->VisitExprs(
          [&](std::string& text, double&) { hit = hit || references(text); });
      if (hit)
        refs.push_back(state.parts[i].name + ": feature '" + f->GetName() +
                       "'");
    }
  }
  return refs;
}

std::string RemoveProjectTable(AppState& state, const std::string& name) {
  const auto it =
      std::find_if(state.projectTables.begin(), state.projectTables.end(),
                   [&](const lookuptable::Table& t) { return t.name == name; });
  if (it == state.projectTables.end()) {
    std::string names;
    for (const lookuptable::Table& t : state.projectTables)
      names += (names.empty() ? "" : ", ") + t.name;
    return "no table '" + name + "' -- " +
           (names.empty() ? std::string("this project has no tables")
                          : "the tables are " + names);
  }
  if (std::vector<std::string> refs = ProjectTableReferences(state, name);
      !refs.empty()) {
    std::string list;
    for (const std::string& r : refs)
      list += (list.empty() ? "" : "; ") + r;
    return "table '" + name + "' is read by " + list + " -- change those first";
  }
  PushProjectVarUndo(state, state.projectParameters, {}, {});
  state.projectTables.erase(it);
  SyncProjectScope(state);
  return {};
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
  redoEntry.tables = state.projectTables;
  redoEntry.templates = state.projectTemplates;
  redoEntry.stamp = entry.stamp;
  redoEntry.docRefFrom = entry.docRefTo;
  redoEntry.docRefTo = entry.docRefFrom;
  redoEntry.features = CurrentFeatureStates(state, entry.features);
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
  undoEntry.tables = state.projectTables;
  undoEntry.templates = state.projectTemplates;
  undoEntry.stamp = entry.stamp;
  undoEntry.docRefFrom = entry.docRefTo;
  undoEntry.docRefTo = entry.docRefFrom;
  undoEntry.features = CurrentFeatureStates(state, entry.features);
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
