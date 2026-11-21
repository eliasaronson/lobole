// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/DocumentSlots.h"
#include "ui/DerivedActions.h" // RefreshLinks (the tab gate)
#include "ui/DrawingActions.h"
#include "ui/MotionActions.h" // AdoptSavedMotionResults
#include "ui/RendererSync.h"
#include "ui/ViewActions.h"

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
#include "core/io/SerializeSimulation.h" // SimulationFingerprint (undo)
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

// The tabs: which document is live, what gets stashed when you leave one, and
// adding, duplicating, deleting and foldering them. Per-tab undo for the
// kinds that keep their own history -- drawings and jobs -- lives here too,
// beside the slots it acts on.
//
// The four Slot* accessors at the end are the seam every other actions file
// reaches a stashed document through; they are declared in
// AppActionsInternal.h.
// Declared in ui/DocumentSlots.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc) {
  // A wholesale replacement is followed by state.parts.clear() and a reset of
  // nextPartId, so slot ids get REUSED. A cache entry keyed on an old id
  // would then look like a hit for an unrelated new part — and its stored
  // docVersion could match a fresh document's, which draws the previous
  // project's geometry. Drop it all here rather than at each of the four
  // parts.clear() sites, where a fifth would eventually be missed.
  ReleasePartRenderCache(state);
  // A drawing run in flight is cancelled and joined, and the projections,
  // copies and caches let go (they name shapes of the project going away).
  ForgetDrawingProjections(state);
  // Whole Documents, one per configuration a drawing view named. The
  // instance-id guard keeps them from ever being READ as the new project's,
  // but nothing would otherwise free them for the rest of the session.
  state.configuredSources.clear();
  state.document = std::move(doc);
  state.commands = std::make_unique<CommandStack>();
  // The active tab may have been an assembly or drawing: this replacement
  // makes the live tab a part, so drop the other-kind state — leaving
  // state.assembly set alongside state.document half-switches the UI (the
  // assembly toolbar keeps drawing and the part never syncs; File > New on
  // an assembly tab hit exactly that).
  state.assembly = nullptr;
  state.drawing = nullptr;
  state.job = nullptr;
  state.sim = nullptr;
  state.assemblyHistory = {};
  state.drawingHistory = {};
  state.jobHistory = {};
  state.simHistory = {};
  state.drawingRenders.clear();
  state.drawingRendersVersion = 0;
  state.drawingProjections = {};
  // The assembly's motion results went with it (stashed per tab: StashActive).
  state.motionResults.clear();
  state.motionResultRevisions.clear();
  // The job's assembled bodies are keyed on document ids that this
  // replacement is about to reuse, so a stale cache reads as a hit for an
  // unrelated part -- the same trap ReleasePartRenderCache exists for.
  state.jobBodies.clear();
  state.jobBodiesKey = 0;
  state.jobPlacedBodies.clear();
  state.jobPlacedKey = 0;
  state.simBodies.clear(); // the same, for a simulation's subject
  state.simBodiesKey = 0;
  state.solvedPartVersions.clear();
  // Phase V: a wholesale replacement (File > New, Open flat, checkpoint/history
  // restore) collapses the project to this single part, so ITS parameters
  // become the project scope (one part = the whole project). The project-var
  // undo history belonged to the old model — discard it, then regenerate the
  // document against the (now project) scope.
  state.catalogs = state.document->TakeCatalogs();
  state.catalogHistory = {};
  state.projectParameters = state.document->TakeParameters();
  state.projectTables = state.document->TakeTables();
  state.projectTemplates = state.document->TakeTemplates();
  state.projectVarHistory = {};
  // The requirements a lone part carries are the project's
  // (docs/requirements/README.md#saved-form); validated when it was read.
  state.requirements = req::ListFromJson(state.document->TakeRequirementsRaw());
  state.nextRequirementId = 1;
  for (const req::Requirement& r : state.requirements)
    state.nextRequirementId = std::max(state.nextRequirementId, r.id + 1);
  state.requirementsEdits++;
  state.document->SetProjectSymbols(&state.projectSymbols);
  state.document->SetProjectParameters(&state.projectParameters);
  {
    SymbolTable cells;
    lookuptable::AddCellSymbols(state.projectTables, cells);
    EvaluateParameterTable(state.projectParameters, state.projectSymbols,
                           &cells, {});
  }
  state.document->Regenerate();
  state.selection.Clear();
  state.pendingFeatureId = 0;
  state.pendingCompanionId = 0;
  state.primitive = AppState::PrimitiveSession{};
  state.sketchEdit = SketchEditState{};
  // Simulation is a MODE, and the studies it edits belonged to the document
  // that just went away. File > New in simulation mode used to leave the
  // simulation toolbar up over a brand-new part, editing the new document's
  // (empty) study list — the same half-switched UI the assembly/drawing
  // clearing above exists to prevent.
  if (state.simulation.active) {
    ResetSimulationDisplay(state);
    state.simulationToolOpen = false;
  }
  state.measure = MeasureState{};
  // The CAM job belonged to the document that just went away. Leaving this
  // behind made the panel claim "Z zero at 17.000" and offer to Save G-code
  // for a part that no longer existed — and an armed face pick survived into
  // the new document, where the button that would re-arm it is disabled
  // while a pick is live, so the panel was simply stuck.
  state.camToolpaths.clear();
  state.camVerdict.clear();
  state.camLeftover.clear();
  state.camFixtureHit.reset();
  state.camLastPost.clear();
  state.camVerdictSettled = false;
  state.camSetupResults.clear();
  state.camResultOwner = {};
  state.camToolpathsFor = {};
  state.camActiveSetup = 0;
  state.camGhostOp = 0;
  if (state.slotPick.featureId == kCamToolSlot)
    state.slotPick = {};
  // Every other TOOL SESSION goes with the document too, for the same reason
  // the assembly/drawing state above does: a session left open is editing
  // something that no longer exists. `editFeatureId` is the sharp one — it
  // names a feature in the document that was just discarded — and camToolOpen
  // is the one that bit: it is a toggle, so a stale one turned OFF at the next
  // click on the toolbar button meant to turn it on.
  state.editFeatureId = 0;
  state.dimTool = AppState::DimTool{};
  state.drawingNoteTool = AppState::DrawingNoteTool{};
  state.detailTool = AppState::DetailTool{};
  state.mateTool = AppState::MateToolState{};
  state.camToolOpen = false;
  state.camVerb = AppState::CamVerb::None;
  state.camVerbOp = 0;
  state.camNote.clear();
  // The assembly's two viewport modes, for the same reason and with the same
  // sharp edge: both are toggles, so a stale one turns OFF at the next click
  // on the toolbar button meant to turn it on — and until then its window
  // floats over the viewport of a document it knows nothing about.
  state.explodeToolOpen = false;
  state.motionToolOpen = false;
  // Force a resync even if the version counters happen to collide
  state.gpu.syncedDocumentVersion = UINT64_MAX;
}

namespace {

// Defined further down, beside the other tab-slot plumbing: a slot's document
// whether it is the active tab (AppState holds it) or a stashed one.

// A tab name nothing else has taken: "<source> copy", then "<source> copy 2".
// A copy cannot inherit its original's name — names ADDRESS tabs (the RPC
// refuses a duplicate outright) and each one becomes that document's directory
// when the project is saved, so two of them is two documents writing to one
// place.
std::string FreshCopyName(const AppState& state, const std::string& source) {
  const auto taken = [&state](const std::string& name) {
    for (const PartSlot& slot : state.parts)
      if (slot.name == name)
        return true;
    return false;
  };
  std::string candidate = source + " copy";
  for (int n = 2; taken(candidate); n++)
    candidate = source + " copy " + std::to_string(n);
  return candidate;
}
} // namespace

// Hoisted out of the anonymous namespace by the stage-3 split: the tab-slot
// accessors are the seam ProjectActions.cpp reaches the slots through, so
// they are declared in AppActionsInternal.h and defined here.

// Move the active tab's live model (part doc+commands OR assembly) back into
// its slot. Moving a null pointer is a no-op, so this handles both kinds.
void StashActive(AppState& state) {
  PartSlot& slot = state.parts[state.activePart];
  slot.doc = std::move(state.document);
  slot.assembly = std::move(state.assembly);
  slot.drawing = std::move(state.drawing);
  slot.job = std::move(state.job);
  slot.sim = std::move(state.sim);
  slot.commands = std::move(state.commands);
  slot.history = std::move(state.assemblyHistory);
  slot.drawingHistory = std::move(state.drawingHistory);
  slot.jobHistory = std::move(state.jobHistory);
  slot.simHistory = std::move(state.simHistory);
  state.simHistory = {}; // moved-from: guarantee empty
  slot.drawingRenders = std::move(state.drawingRenders);
  state.drawingRenders.clear(); // moved-from: guarantee empty
  slot.drawingRendersVersion = state.drawingRendersVersion;
  state.drawingRendersVersion = 0;
  slot.drawingProjections = std::move(state.drawingProjections);
  state.drawingProjections = {}; // moved-from: guarantee empty
  slot.lastSavedVersion = state.lastSavedVersion;
  slot.solvedPartVersions = std::move(state.solvedPartVersions);
  state.solvedPartVersions.clear(); // moved-from: guarantee empty
  // Motion results are by study id, which is unique within one assembly
  // only: left in AppState, the next assembly's study of the same id read
  // this one's verdict until a run of its own replaced it.
  slot.motionResults = std::move(state.motionResults);
  state.motionResults.clear();
  slot.motionResultRevisions = std::move(state.motionResultRevisions);
  state.motionResultRevisions.clear();
  // A job's setups' last runs go with it, as the drawing renders do. The one
  // on screen joins them when SyncCamResultOwner sees another come on.
  slot.camResults = std::move(state.camSetupResults);
  state.camSetupResults.clear();
  slot.camActiveSetup = state.camActiveSetup;
  state.camActiveSetup = 0;
}

// Move a slot's model into AppState as the active tab. Assumes AppState holds
// nothing live (call StashActive first, or after a clear).
void ActivateSlot(AppState& state, size_t index) {
  PartSlot& slot = state.parts[index];
  state.document = std::move(slot.doc);
  state.assembly = std::move(slot.assembly);
  state.drawing = std::move(slot.drawing);
  state.job = std::move(slot.job);
  state.sim = std::move(slot.sim);
  state.commands = std::move(slot.commands);
  state.assemblyHistory = std::move(slot.history);
  state.drawingHistory = std::move(slot.drawingHistory);
  state.jobHistory = std::move(slot.jobHistory);
  state.simHistory = std::move(slot.simHistory);
  slot.simHistory = {};
  state.drawingRenders = std::move(slot.drawingRenders);
  slot.drawingRenders.clear();
  state.drawingRendersVersion = slot.drawingRendersVersion;
  state.drawingProjections = std::move(slot.drawingProjections);
  slot.drawingProjections = {};
  state.lastSavedVersion = slot.lastSavedVersion;
  state.solvedPartVersions = std::move(slot.solvedPartVersions);
  slot.solvedPartVersions.clear();
  state.motionResults = std::move(slot.motionResults);
  slot.motionResults.clear();
  state.motionResultRevisions = std::move(slot.motionResultRevisions);
  slot.motionResultRevisions.clear();
  state.camSetupResults = std::move(slot.camResults);
  slot.camResults.clear();
  state.camActiveSetup = slot.camActiveSetup;
  state.activePart = static_cast<int>(index);
}

// Shared reset after a tab switch/create.
void ResetActiveTabUiState(AppState& state) {
  state.selection.Clear(); // features, occurrences and mates alike
  state.slotPick = {};
  state.hiddenBodies.clear();
  state.pendingFeatureId = 0;
  state.pendingCompanionId = 0;
  state.primitive = AppState::PrimitiveSession{};
  state.editFeatureId = 0;
  state.sketchEdit = SketchEditState{};
  // Any tab change exits in-context editing (EnterInContextEdit re-arms it
  // after its SetActivePart returns).
  state.inContext = AppState::InContextEdit{};
  // Explode display state is per-tab and transient: reset on switch.
  state.activeExplodedView = 0;
  state.explodeFactor = 1.0f;
  // The 2D sheet pan/zoom is per-tab and transient: a fresh scale=0 makes the
  // next drawing paint fit-to-window (harmless for non-drawing tabs).
  state.sheetView = AppState::Sheet2DView{};
  // NOTE: drawingRenders is NOT cleared here — it is stashed/restored per slot
  // (StashActive/ActivateSlot) so a drawing keeps its projected geometry across
  // tab switches and only an explicit update re-projects.
  state.dimTool = AppState::DimTool{}; // transient dimension-pick state
  state.drawingNoteTool = AppState::DrawingNoteTool{}; // ...and the rest
  state.detailTool = AppState::DetailTool{};
  state.drawingMessage.clear();
  state.dimDrag = 0;
  // Any tab change exits simulation mode (results themselves live on the
  // Document and survive; only the mode/display state resets). Through
  // ResetSimulationDisplay, not by resetting the state: the topology Shape
  // view is a mesh instance of its own, and a bare reset forgot its id and
  // left it drawn over every tab -- and project -- that came after.
  ResetSimulationDisplay(state);
  // ...and drops the measure session. Its picks are ElementRefs into the
  // document that was on screen; carried onto another tab they resolve
  // against nothing while the readout keeps displaying the old distance over
  // geometry that was never measured. Simulation was reset here from the
  // start and measure was simply missed.
  state.measure = MeasureState{};
  // A job tab's panel is the CAM panel, and it shares the left node with
  // Variables, History and the rest: without this the tab that was in front
  // on the part stays in front here, and the job's list is behind it.
  // Repeated frames, as simulation mode does (DockLayout.cpp).
  if (state.job) {
    state.focusPanel = "CAM";
    state.focusPanelFrames = 60;
  }
  // A simulation tab IS simulation mode: the mode is on exactly while one is
  // in front (docs/simulation/README.md#the-simulation-tab). Its study list
  // is the Model panel's, for the same reason as the job's.
  if (state.sim) {
    state.simulation.active = true;
    if (!state.sim->GetStudies().empty()) {
      state.simulation.activeStudyId = state.sim->GetStudies().front().id;
      // Its settings in the tool window, as the tree's row would show them.
      state.simulation.focus = {SimulationState::Focus::Kind::Study,
                                state.simulation.activeStudyId, 0, -1};
    }
    state.gpu.syncedDocumentVersion = UINT64_MAX;
    state.focusPanel = "Model";
    state.focusPanelFrames = 60;
  }
  state.gpu.syncedDocumentVersion = UINT64_MAX; // force a renderer resync
}

void EnsureParts(AppState& state, const std::string& firstName) {
  if (state.parts.empty()) {
    PartSlot slot;
    slot.id = state.nextPartId++;
    slot.name = firstName;
    slot.kind = DocKind::Part;
    slot.lastSavedVersion = state.lastSavedVersion;
    state.parts.push_back(
        std::move(slot)); // active: doc/commands stay in AppState
    state.activePart = 0;
  }
}

size_t AddPart(AppState& state, const std::string& name) {
  EnsureParts(state);
  CancelPendingFeature(state); // before StashActive — see SetActivePart
  StashActive(state);
  // Create and activate the new part.
  state.document = std::make_unique<Document>();
  // Phase V: the new part shares the project scope (its features can reference
  // project variables immediately).
  state.document->SetProjectSymbols(&state.projectSymbols);
  state.document->SetProjectParameters(&state.projectParameters);
  state.assembly = nullptr;
  state.drawing = nullptr;
  state.job = nullptr;
  state.sim = nullptr;
  state.commands = std::make_unique<CommandStack>();
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Part;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // Which documents the project HAS lives in project.json, not in any
  // document version — so without this the project reads clean with a brand
  // new (still empty) tab in it, and closing loses the tab with no prompt.
  state.projectMetaDirty = true;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

size_t AddAssembly(AppState& state, const std::string& name) {
  EnsureParts(state);
  CancelPendingFeature(state); // before StashActive — see SetActivePart
  StashActive(state);
  // Create and activate the new assembly. Occurrence undo is Phase 3, so an
  // assembly tab currently carries no CommandStack.
  state.document = nullptr;
  state.assembly = std::make_unique<AssemblyDocument>();
  state.drawing = nullptr;
  state.job = nullptr;
  state.sim = nullptr;
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Assembly;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // Which documents the project HAS lives in project.json, not in any
  // document version — so without this the project reads clean with a brand
  // new (still empty) tab in it, and closing loses the tab with no prompt.
  state.projectMetaDirty = true;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

size_t AddDrawing(AppState& state, const std::string& name) {
  EnsureParts(state);
  CancelPendingFeature(state); // before StashActive — see SetActivePart
  StashActive(state);
  // Create and activate the new drawing. Undo is a snapshot stack (the
  // assembly pattern), so a drawing tab carries no CommandStack.
  state.document = nullptr;
  state.assembly = nullptr;
  state.drawing = std::make_unique<DrawingDocument>();
  state.job = nullptr;
  state.sim = nullptr;
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Drawing;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // Which documents the project HAS lives in project.json, not in any
  // document version — so without this the project reads clean with a brand
  // new (still empty) tab in it, and closing loses the tab with no prompt.
  state.projectMetaDirty = true;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

const std::vector<Body>& ViewportBodies(AppState& state) {
  static const std::vector<Body> kNone;
  if (state.job)
    return JobPlacedBodies(state);
  if (state.sim)
    return SimSubjectBodies(state);
  return state.document ? state.document->GetBodies() : kNone;
}

size_t AddJob(AppState& state, const std::string& name) {
  EnsureParts(state);
  CancelPendingFeature(state); // before StashActive — see SetActivePart
  StashActive(state);
  // Create and activate the new job. Undo is a snapshot stack (the drawing
  // pattern): a job is a parts list and a few setups, small enough that a
  // full copy per edit beats generalizing the Document-only CommandStack.
  state.document = nullptr;
  state.assembly = nullptr;
  state.drawing = nullptr;
  state.job = std::make_unique<cam::JobDocument>();
  state.sim = nullptr;
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Job;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // Which documents the project HAS lives in project.json, not in any
  // document version — so without this the project reads clean with a brand
  // new (still empty) tab in it, and closing loses the tab with no prompt.
  state.projectMetaDirty = true;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

bool RenameDocument(AppState& state, size_t index, const std::string& name,
                    std::string* why) {
  const auto refuse = [&](std::string reason) {
    if (why)
      *why = std::move(reason);
    return false;
  };
  if (index >= state.parts.size())
    return refuse("no tab at index " + std::to_string(index));
  if (name.empty())
    return refuse("a tab's name must be non-empty");
  if (state.parts[index].name == name)
    return true;
  for (const PartSlot& ps : state.parts)
    if (ps.name == name)
      return refuse("a tab named '" + name +
                    "' already exists — names address tabs (add_occurrence, "
                    "set_active_part), so two answering to one would be "
                    "ambiguous");
  state.parts[index].name = name;
  state.projectMetaDirty = true; // tab names live in project.json only
  return true;
}

size_t AddSimulation(AppState& state, const std::string& name,
                     fem::SimDocument sim, std::string* why) {
  EnsureParts(state);
  const auto refuse = [&](std::string reason) {
    if (why)
      *why = std::move(reason);
    return SIZE_MAX;
  };
  if (name.empty())
    return refuse("a simulation's name must be non-empty");
  for (const PartSlot& ps : state.parts)
    if (ps.name == name)
      return refuse("a tab named '" + name +
                    "' already exists — names address tabs, so two "
                    "answering to one would be ambiguous");
  CancelPendingFeature(state); // before StashActive — see SetActivePart
  StashActive(state);
  // Undo is a snapshot stack (the job's pattern): a copy of a simulation
  // shares its results and meshes, so a snapshot is its settings and studies.
  state.document = nullptr;
  state.assembly = nullptr;
  state.drawing = nullptr;
  state.job = nullptr;
  state.sim = std::make_unique<fem::SimDocument>(std::move(sim));
  state.commands = nullptr;
  state.lastSavedVersion = 0;
  state.solvedPartVersions.clear();
  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = name;
  slot.kind = DocKind::Simulation;
  state.parts.push_back(std::move(slot));
  state.activePart = static_cast<int>(state.parts.size()) - 1;
  // The document list lives in project.json (AddPart says why).
  state.projectMetaDirty = true;
  ResetActiveTabUiState(state);
  return state.parts.size() - 1;
}

size_t FindSimulationFor(const AppState& state, fem::SubjectKind kind,
                         uint64_t docId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].kind != DocKind::Simulation)
      continue;
    const fem::SimDocument* sim = static_cast<int>(i) == state.activePart
                                      ? state.sim.get()
                                      : state.parts[i].sim.get();
    if (sim != nullptr && sim->Subject().kind == kind &&
        sim->Subject().docId == docId)
      return i;
  }
  return SIZE_MAX;
}

size_t OpenSimulationFor(AppState& state, size_t subjectSlot,
                         std::string* why) {
  EnsureParts(state);
  if (subjectSlot >= state.parts.size()) {
    if (why)
      *why = "no tab at index " + std::to_string(subjectSlot);
    return SIZE_MAX;
  }
  const PartSlot& subject = state.parts[subjectSlot];
  if (subject.kind != DocKind::Part && subject.kind != DocKind::Assembly) {
    if (why)
      *why = "'" + subject.name +
             "' is not a part or an assembly: a simulation analyses one";
    return SIZE_MAX;
  }
  const fem::SubjectKind kind = subject.kind == DocKind::Assembly
                                    ? fem::SubjectKind::Assembly
                                    : fem::SubjectKind::Part;
  if (const size_t found = FindSimulationFor(state, kind, subject.id);
      found != SIZE_MAX) {
    SetActivePart(state, found);
    return found;
  }
  const std::string stem = subject.name + " simulation";
  const auto taken = [&state](const std::string& name) {
    for (const PartSlot& ps : state.parts)
      if (ps.name == name)
        return true;
    return false;
  };
  std::string name = stem;
  for (int n = 2; taken(name); n++)
    name = stem + " " + std::to_string(n);
  fem::SimDocument sim;
  sim.SetSubject({kind, subject.id, 0, subject.name});
  const size_t added = AddSimulation(state, name, std::move(sim), why);
  if (added != SIZE_MAX && !state.parts[added].everFramed) {
    state.parts[added].everFramed = true;
    FitCameraToDocument(state);
  }
  return added;
}

namespace {
constexpr uint64_t kFnvPrime = 1099511628211ull;
}

uint64_t SimRenderVersion(AppState& state) {
  if (!state.sim)
    return 0;
  const fem::SimSubject& subject = state.sim->Subject();
  uint64_t key = 1469598103934665603ull ^
                 (subject.kind == fem::SubjectKind::Assembly ? 2u : 1u);
  key = (key ^ subject.docId) * kFnvPrime;
  key = (key ^ subject.configuration) * kFnvPrime;
  const std::optional<size_t> at = SlotIndexById(state, subject.docId);
  if (!at)
    return key; // the subject is gone: nothing to draw, still a key
  if (subject.kind == fem::SubjectKind::Part) {
    if (const Document* d =
            PartResolverFor(state)(subject.docId, subject.configuration))
      key = (key ^ PartStamp(*d)) * kFnvPrime;
    return key;
  }
  // An assembly places other tabs' parts and sub-assemblies, any of which
  // may have moved: every one is in the key. Cheaper than walking the tree
  // to find which, and it is read every frame.
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].kind == DocKind::Part) {
      if (const Document* d = SlotDoc(state, i))
        key =
            ((key ^ state.parts[i].id) * kFnvPrime ^ PartStamp(*d)) * kFnvPrime;
    } else if (state.parts[i].kind == DocKind::Assembly) {
      if (const AssemblyDocument* a = SlotAssembly(state, i))
        key =
            ((key ^ state.parts[i].id) * kFnvPrime ^ a->Version()) * kFnvPrime;
    }
  }
  return key;
}

const std::vector<Body>& SimSubjectBodies(AppState& state) {
  static const std::vector<Body> kNone;
  if (!state.sim) {
    state.simBodies.clear();
    state.simBodyOccurrences.clear();
    state.simBodiesKey = 0;
    return kNone;
  }
  const uint64_t key = SimRenderVersion(state);
  if (key == state.simBodiesKey)
    return state.simBodies;
  state.simBodies.clear();
  state.simBodyOccurrences.clear();
  state.simBodiesKey = key;
  const fem::SimSubject& subject = state.sim->Subject();
  const std::optional<size_t> at = SlotIndexById(state, subject.docId);
  if (!at)
    return state.simBodies;
  if (subject.kind == fem::SubjectKind::Part) {
    if (state.parts[*at].kind != DocKind::Part)
      return state.simBodies;
    if (const Document* d =
            PartResolverFor(state)(subject.docId, subject.configuration))
      state.simBodies = d->GetBodies();
    state.simBodyOccurrences.assign(state.simBodies.size(), 0);
    return state.simBodies;
  }
  const AssemblyDocument* asmDoc = SlotAssembly(state, *at);
  if (asmDoc == nullptr)
    return state.simBodies;
  // Placed as the assembly tab draws them (FlattenActiveAssembly's walk),
  // without the derived ghosts, which are no occurrence's matter.
  auto flat = assembly::Flatten(*asmDoc, PartResolverFor(state),
                                AssemblyResolverFor(state));
  if (!flat) {
    LOG_ERROR("Simulation subject '{}' cannot be walked: {}", subject.name,
              flat.error());
    return state.simBodies;
  }
  for (const assembly::FlatInstance& inst : *flat) {
    if (!inst.visible || inst.part == nullptr)
      continue;
    for (const Body& b : inst.part->GetBodies())
      if (!b.reference) {
        state.simBodies.push_back(PlacedBody(b, inst.world));
        state.simBodyOccurrences.push_back(inst.rootOccurrenceId);
      }
  }
  return state.simBodies;
}

size_t DuplicateDocument(AppState& state, size_t index) {
  EnsureParts(state);
  if (index >= state.parts.size())
    return SIZE_MAX;
  const PartSlot& source = state.parts[index];

  PartSlot slot;
  slot.id = state.nextPartId++;
  slot.name = FreshCopyName(state, source.name);
  slot.kind = source.kind;
  // The copy lands where the original lives: a part duplicated inside an
  // imported assembly's folder belongs in that folder, not loose on the bar.
  slot.folder = source.folder;
  // Read through the slot accessors, so duplicating the tab you are LOOKING at
  // (its document lives in AppState, not in its slot) is the same code path as
  // duplicating any other.
  switch (source.kind) {
  case DocKind::Part: {
    const Document* doc = SlotDoc(state, index);
    if (!doc)
      return SIZE_MAX;
    // Deep copy: features cloned, geometry regenerated (Document's copy
    // constructor). The copy inherits the project scope pointers from the
    // original, so its expressions resolve project variables immediately.
    slot.doc = std::make_unique<Document>(*doc);
    // A fresh, EMPTY undo stack. The original's history describes edits to a
    // document that is not this one; offering to undo them here would undo
    // them into the copy.
    slot.commands = std::make_unique<CommandStack>();
    break;
  }
  case DocKind::Assembly: {
    const AssemblyDocument* asmDoc = SlotAssembly(state, index);
    if (!asmDoc)
      return SIZE_MAX;
    // Occurrences keep referencing the same PARTS (by stable id) — duplicating
    // an assembly copies the arrangement, it does not fork the parts in it.
    // Mates and exploded views reference occurrences inside the copy, so they
    // need no repointing.
    slot.assembly = std::make_unique<AssemblyDocument>(*asmDoc);
    break;
  }
  case DocKind::Drawing: {
    const DrawingDocument* dwg = SlotDrawing(state, index);
    if (!dwg)
      return SIZE_MAX;
    // Views carry a sourceDocId, so the copy draws the same part. Its renders
    // are NOT copied: SetActivePart below asks for the projection of a
    // drawing whose renders are empty, and the copy's saved projections
    // (which the document carries) answer it with no HLR when their inputs
    // still hash the same.
    slot.drawing = std::make_unique<DrawingDocument>(*dwg);
    break;
  }
  case DocKind::Job: {
    const cam::JobDocument* job = SlotJob(state, index);
    if (!job)
      return SIZE_MAX;
    // The setups name their parts by stable id, so the copy machines the
    // same parts. Its undo history starts empty, as a part's does.
    slot.job = std::make_unique<cam::JobDocument>(*job);
    break;
  }
  case DocKind::Simulation: {
    const fem::SimDocument* sim = SlotSim(state, index);
    if (!sim)
      return SIZE_MAX;
    // A Clone, not a copy: a copy is an undo snapshot and SHARES the
    // results and meshes, so the two tabs would solve into one store. The
    // copy analyses the same subject.
    slot.sim = std::make_unique<fem::SimDocument>(sim->Clone());
    break;
  }
  }

  state.parts.push_back(std::move(slot));
  // Which documents the project HAS lives in project.json, not in any document
  // version — without this the project reads clean with a new tab in it.
  state.projectMetaDirty = true;
  const size_t added = state.parts.size() - 1;
  // Switch to it the way any other tab switch happens: solve, project, frame.
  SetActivePart(state, added);
  return added;
}

void SnapshotDrawing(AppState& state) {
  if (!state.drawing)
    return;
  // One log line per undo step: the session's activity journal
  // (docs/architecture.md#crashes-and-stalls).
  LOG_INFO("edit: drawing");
  state.drawingHistory.undo.push_back(*state.drawing); // full-state copy
  state.drawingHistory.undoStamps.push_back(NextActionStamp());
  state.drawingHistory.redo.clear();
  state.drawingHistory.redoStamps.clear();
}

namespace {
// A snapshot keeps the version it was taken at, which an edit since may have
// passed and a save may have stamped: restored as it is, an undo can land on
// the saved version and read clean (and a cache keyed on it can serve the
// wrong picture). So a restore moves the version past the current one.
void RestoreDrawing(AppState& state, DrawingDocument restored) {
  const uint64_t current = state.drawing->Version();
  while (restored.Version() <= current)
    restored.BumpVersion();
  *state.drawing = std::move(restored);
}
} // namespace

bool UndoDrawing(AppState& state) {
  if (!state.drawing || state.drawingHistory.undo.empty())
    return false;
  LOG_INFO("undo: drawing");
  state.drawingHistory.redo.push_back(*state.drawing);
  state.drawingHistory.redoStamps.push_back(
      state.drawingHistory.undoStamps.back());
  state.drawingHistory.undoStamps.pop_back();
  RestoreDrawing(state, std::move(state.drawingHistory.undo.back()));
  state.drawingHistory.undo.pop_back();
  state.gpu.syncedDocumentVersion = UINT64_MAX;
  return true;
}

bool RedoDrawing(AppState& state) {
  if (!state.drawing || state.drawingHistory.redo.empty())
    return false;
  LOG_INFO("redo: drawing");
  state.drawingHistory.undo.push_back(*state.drawing);
  state.drawingHistory.undoStamps.push_back(
      state.drawingHistory.redoStamps.back());
  state.drawingHistory.redoStamps.pop_back();
  RestoreDrawing(state, std::move(state.drawingHistory.redo.back()));
  state.drawingHistory.redo.pop_back();
  state.gpu.syncedDocumentVersion = UINT64_MAX;
  return true;
}

void PushJobUndo(AppState& state, const cam::JobDocument& before) {
  if (!state.job)
    return;
  LOG_INFO("edit: CAM job"); // the activity journal, as SnapshotDrawing
  state.jobHistory.undo.push_back(before);
  state.jobHistory.undoStamps.push_back(NextActionStamp());
  state.jobHistory.redo.clear();
  state.jobHistory.redoStamps.clear();
}

void SnapshotJob(AppState& state) {
  if (state.job)
    PushJobUndo(state, *state.job);
}

namespace {
// Put a restored copy in place: its version has to move PAST the current
// one, or a cache keyed on the version (the placed bodies, the render) can
// serve the state the copy was taken from — and so does its save stamp, or
// an undo could land on the stamp the last save took and read clean. Then
// the toolpaths, which are a picture of the job that just changed.
void RestoreJob(AppState& state, cam::JobDocument restored) {
  const uint64_t current = state.job->Version();
  const uint64_t currentStamp = state.job->SaveStamp();
  while (restored.Version() <= current || restored.SaveStamp() <= currentStamp)
    restored.BumpVersion();
  *state.job = std::move(restored);
  state.camActiveSetup = 0; // re-resolved: the setup may be gone
  state.camGhostOp = 0;
  state.camVerbOp = 0;
  state.gpu.syncedDocumentVersion = UINT64_MAX;
  RefreshCamToolpaths(state);
  RunCamVerification(state);
}
} // namespace

bool UndoJob(AppState& state) {
  if (!state.job || state.jobHistory.undo.empty())
    return false;
  LOG_INFO("undo: CAM job");
  state.jobHistory.redo.push_back(*state.job);
  state.jobHistory.redoStamps.push_back(state.jobHistory.undoStamps.back());
  state.jobHistory.undoStamps.pop_back();
  cam::JobDocument restored = std::move(state.jobHistory.undo.back());
  state.jobHistory.undo.pop_back();
  RestoreJob(state, std::move(restored));
  return true;
}

bool RedoJob(AppState& state) {
  if (!state.job || state.jobHistory.redo.empty())
    return false;
  LOG_INFO("redo: CAM job");
  state.jobHistory.undo.push_back(*state.job);
  state.jobHistory.undoStamps.push_back(state.jobHistory.redoStamps.back());
  state.jobHistory.redoStamps.pop_back();
  cam::JobDocument restored = std::move(state.jobHistory.redo.back());
  state.jobHistory.redo.pop_back();
  RestoreJob(state, std::move(restored));
  return true;
}

namespace {
int g_openJobGuards = 0; // JobUndoGuards alive, outermost first
bool g_jobEditAudit = false;
int g_jobEditAuditMisses = 0;
std::string g_jobEditAuditLastMiss;

// Where two fingerprints first differ, with some context either side: what
// an audit miss prints, so the edit that forgot MarkJobEdited can be found.
std::string FirstDifference(const std::string& a, const std::string& b) {
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i])
    i++;
  const size_t from = i > 60 ? i - 60 : 0;
  return "..." + a.substr(from, 120) + "... became ..." + b.substr(from, 120) +
         "...";
}
} // namespace

void MarkJobEdited(AppState& state) {
  state.jobEdits++;
  if (state.job)
    state.job->MarkEdited(); // unsaved now: ProjectDirty, autosave
}

void SetJobEditAudit(bool on) { g_jobEditAudit = on; }
int JobEditAuditMisses() { return g_jobEditAuditMisses; }
std::string JobEditAuditLastMiss() { return g_jobEditAuditLastMiss; }
void ResetJobEditAudit() {
  g_jobEditAuditMisses = 0;
  g_jobEditAuditLastMiss.clear();
}

JobUndoGuard::JobUndoGuard(AppState& state, bool coalesce, bool compareContent)
    : m_state(state), m_coalesce(coalesce), m_compareContent(compareContent) {
  // Only the outermost guard records: an action with a guard of its own
  // (SetJobPartConfiguration), called under the panel's or an RPC's, is
  // still one step. Counted here rather than on the history, which moves
  // with its tab.
  if (g_openJobGuards++ > 0)
    return;
  if (state.job) {
    m_job = state.job.get();
    m_edits = state.jobEdits;
    m_before = *state.job;
    if (m_compareContent || g_jobEditAudit)
      m_fingerprint = DocumentIO::JobFingerprint(*state.job);
  }
}

JobUndoGuard::~JobUndoGuard() {
  AppState& state = m_state;
  g_openJobGuards--;
  if (!state.job || !m_before)
    return;
  // Another tab's job now (an action switched tabs under the guard): its
  // history is not the one this copy belongs on.
  if (state.job.get() != m_job)
    return;
  bool moved = state.jobEdits != m_edits;
  if (m_compareContent || g_jobEditAudit) {
    const std::string now = DocumentIO::JobFingerprint(*state.job);
    const bool changed = now != m_fingerprint;
    if (m_compareContent) {
      moved = moved || changed;
    } else if (changed && !moved) {
      // An edit that did not say so: it would not undo. Recorded anyway, so
      // the audit changes nothing but the count.
      g_jobEditAuditMisses++;
      g_jobEditAuditLastMiss = FirstDifference(m_fingerprint, now);
      LOG_ERROR("job edited without MarkJobEdited: {}", g_jobEditAuditLastMiss);
      moved = true;
    }
  }
  // Whatever records an undo step is an edit to save — the RPCs' guards
  // compare content and call nothing else, so this is where they count.
  if (moved)
    state.job->MarkEdited();
  JobHistory& h = state.jobHistory;
  if (m_coalesce && ImGui::IsAnyItemActive()) {
    // Mid-drag: hold the FIRST pre-edit copy, whatever this frame did.
    if (moved && !h.pending)
      h.pending = std::move(*m_before);
    return;
  }
  if (h.pending) {
    // The drag let go. One step, from before it started -- unless it came
    // back to exactly where it began, which only the content can say (once,
    // at the release, not every frame of the drag).
    if (DocumentIO::JobFingerprint(*state.job) !=
        DocumentIO::JobFingerprint(*h.pending))
      PushJobUndo(state, *h.pending);
    h.pending.reset();
    return;
  }
  if (moved)
    PushJobUndo(state, *m_before);
}

void PushSimUndo(AppState& state, const fem::SimDocument& before) {
  if (!state.sim)
    return;
  LOG_INFO("edit: simulation"); // the activity journal, as SnapshotDrawing
  state.simHistory.undo.push_back(before);
  state.simHistory.undoStamps.push_back(NextActionStamp());
  state.simHistory.redo.clear();
  state.simHistory.redoStamps.clear();
}

void SnapshotSim(AppState& state) {
  if (state.sim)
    PushSimUndo(state, *state.sim);
}

namespace {
// RestoreJob's rule: the version (which is the save stamp) moves past the
// current one, so an undo never lands on the stamp the last save took. The
// results stay where they are -- the snapshot shares them -- and each reads
// current or stale by the study revisions the snapshot carries.
void RestoreSim(AppState& state, fem::SimDocument restored) {
  const uint64_t current = state.sim->Version();
  while (restored.Version() <= current)
    restored.BumpVersion();
  *state.sim = std::move(restored);
  state.gpu.syncedDocumentVersion = UINT64_MAX;
}
} // namespace

bool UndoSim(AppState& state) {
  if (!state.sim || state.simHistory.undo.empty())
    return false;
  LOG_INFO("undo: simulation");
  state.simHistory.redo.push_back(*state.sim);
  state.simHistory.redoStamps.push_back(state.simHistory.undoStamps.back());
  state.simHistory.undoStamps.pop_back();
  fem::SimDocument restored = std::move(state.simHistory.undo.back());
  state.simHistory.undo.pop_back();
  RestoreSim(state, std::move(restored));
  return true;
}

bool RedoSim(AppState& state) {
  if (!state.sim || state.simHistory.redo.empty())
    return false;
  LOG_INFO("redo: simulation");
  state.simHistory.undo.push_back(*state.sim);
  state.simHistory.undoStamps.push_back(state.simHistory.redoStamps.back());
  state.simHistory.redoStamps.pop_back();
  fem::SimDocument restored = std::move(state.simHistory.redo.back());
  state.simHistory.redo.pop_back();
  RestoreSim(state, std::move(restored));
  return true;
}

void MarkSimEdited(AppState& state) {
  state.simEdits++;
  if (state.sim)
    state.sim->BumpVersion(); // unsaved now: ProjectDirty, autosave
}

namespace {
int g_openSimGuards = 0; // SimUndoGuards alive, outermost first
} // namespace

SimUndoGuard::SimUndoGuard(AppState& state, bool coalesce, bool compareContent)
    : m_state(state), m_coalesce(coalesce), m_compareContent(compareContent) {
  if (g_openSimGuards++ > 0)
    return; // only the outermost records (JobUndoGuard)
  if (state.sim) {
    m_sim = state.sim.get();
    m_edits = state.simEdits;
    m_version = state.sim->Version();
    m_before = *state.sim;
    if (m_compareContent)
      m_fingerprint = DocumentIO::SimulationFingerprint(*state.sim);
  }
}

SimUndoGuard::~SimUndoGuard() {
  AppState& state = m_state;
  g_openSimGuards--;
  if (!state.sim || !m_before)
    return;
  // Another tab's simulation now: its history is not this copy's.
  if (state.sim.get() != m_sim)
    return;
  // Every setter of the document moves its version, so the version is the
  // edit; the counter covers an edit through StudyForRun (MarkSimEdited).
  bool moved = state.simEdits != m_edits || state.sim->Version() != m_version;
  if (m_compareContent && !moved)
    moved = DocumentIO::SimulationFingerprint(*state.sim) != m_fingerprint;
  if (moved && state.sim->Version() == m_version)
    state.sim->BumpVersion(); // an edit to save, as JobUndoGuard's MarkEdited
  SimHistory& h = state.simHistory;
  if (m_coalesce && ImGui::IsAnyItemActive()) {
    if (moved && !h.pending)
      h.pending = std::move(*m_before);
    return;
  }
  if (h.pending) {
    if (DocumentIO::SimulationFingerprint(*state.sim) !=
        DocumentIO::SimulationFingerprint(*h.pending))
      PushSimUndo(state, *h.pending);
    h.pending.reset();
    return;
  }
  if (moved)
    PushSimUndo(state, *m_before);
}

namespace {
int g_openSimSlotGuards = 0;

// The stashed simulation slot `slotId`, or -1 when it is gone, not a
// simulation, or the tab in front.
int StashedSimSlot(const AppState& state, uint64_t slotId) {
  const std::optional<size_t> at = SlotIndexById(state, slotId);
  if (!at || state.parts[*at].kind != DocKind::Simulation ||
      static_cast<int>(*at) == state.activePart || !state.parts[*at].sim)
    return -1;
  return static_cast<int>(*at);
}
} // namespace

SimSlotUndoGuard::SimSlotUndoGuard(AppState& state, uint64_t simSlotId,
                                   bool compareContent)
    : m_state(state), m_slotId(simSlotId), m_compareContent(compareContent) {
  if (g_openSimSlotGuards++ > 0)
    return; // only the outermost records
  m_outermost = true;
  const int i = StashedSimSlot(state, simSlotId);
  if (i < 0) {
    const std::optional<size_t> at = SlotIndexById(state, simSlotId);
    if (at && static_cast<int>(*at) == state.activePart && state.sim)
      m_active.emplace(state, false, compareContent);
    return;
  }
  const fem::SimDocument& sim = *state.parts[i].sim;
  m_version = sim.Version();
  m_before = sim;
  if (m_compareContent)
    m_fingerprint = DocumentIO::SimulationFingerprint(sim);
}

SimSlotUndoGuard::~SimSlotUndoGuard() {
  g_openSimSlotGuards--;
  if (!m_outermost || !m_before)
    return; // m_active, if any, records itself
  AppState& state = m_state;
  const int i = StashedSimSlot(state, m_slotId);
  if (i < 0)
    return; // gone, or brought forward under the guard: not this copy's
  PartSlot& slot = state.parts[i];
  bool moved = slot.sim->Version() != m_version;
  if (m_compareContent && !moved)
    moved = DocumentIO::SimulationFingerprint(*slot.sim) != m_fingerprint;
  if (!moved)
    return;
  if (slot.sim->Version() == m_version)
    slot.sim->BumpVersion(); // an edit to save
  LOG_INFO("edit: simulation '{}'", slot.name);
  slot.simHistory.undo.push_back(std::move(*m_before));
  slot.simHistory.undoStamps.push_back(NextActionStamp());
  slot.simHistory.redo.clear();
  slot.simHistory.redoStamps.clear();
}

uint64_t PartIdByName(const AppState& state, const std::string& name) {
  for (const PartSlot& slot : state.parts)
    if (slot.kind == DocKind::Part && slot.name == name)
      return slot.id;
  return 0;
}

void SetActivePart(AppState& state, size_t index) {
  EnsureParts(state);
  if (index >= state.parts.size() ||
      index == static_cast<size_t>(state.activePart)) {
    return;
  }
  // A pending preview feature was added straight to the Document (not through
  // a command), so stashing the tab would strand it in that document forever,
  // off the undo stack. Discard it while `state.document` still owns it —
  // ResetActiveTabUiState only zeroes the id, which is too late.
  CancelPendingFeature(state);
  StashActive(state);
  ActivateSlot(state, index);
  LOG_INFO("tab: {}", state.parts[index].name);
  ResetActiveTabUiState(state);
  // The gate (docs/modeling/derived.md#copies-that-keep-up): a tab shown
  // shows its copies of other parts as those parts are now, and so does
  // every tab it reads.
  RefreshLinks(state);
  // Part→assembly propagation (A4): entering an assembly whose referenced
  // parts regenerated since its last solve re-solves it, so mates track part
  // edits without an explicit solve_assembly.
  if (state.assembly && AssemblyPartsStale(state))
    SolveActiveAssembly(state);
  // An assembly's saved motion results are the ones MotionResultFor answers.
  if (state.assembly)
    AdoptSavedMotionResults(state);
  // A drawing projects on the FIRST view of the tab (design §2), off the
  // frame (docs/drawings/README.md#projection-off-the-frame); views a job
  // landed while the tab was stashed are assembled now.
  DrawingTabShown(state);
  // The CAM results on screen were the last tab's: kept with it, and a job's
  // own put back (docs/cam/gui.md#whose-results-are-on-screen). Its
  // toolpaths are regenerated too -- the ones in AppState were the last
  // job's, and its backplot drew them over this one.
  SyncCamResultOwner(state);

  // First look at this tab: frame it. The camera is global, so arriving at a
  // 5 m assembly with a 20 mm part's framing puts you inside the geometry —
  // which reads as an empty scene, not as "you are too close". Only the first
  // visit; after that the framing is the user's.
  if (index < state.parts.size() && !state.parts[index].everFramed) {
    state.parts[index].everFramed = true;
    FitCameraToDocument(state);
  }
}

std::optional<size_t> SlotIndexById(const AppState& state, uint64_t slotId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].id == slotId)
      return i;
  return std::nullopt;
}

// ---- Simulation mode (FEM) ------------------------------------------------

namespace {
// Declared here, defined with its siblings further down: anonymous namespaces
// merge within a translation unit, so this is the same function.
} // namespace

std::string WhyPartUndeletable(AppState& state, size_t index) {
  EnsureParts(state);
  if (state.parts.size() <= 1)
    return "this is the last tab — a project always has one document";
  if (index >= state.parts.size())
    return "that tab is gone";
  // Refused while something references it — which `describe` has claimed
  // since A2 while nothing actually checked, so deleting a placed part left
  // every assembly holding a dangling reference. A5 widens the same question:
  // an assembly can now be referenced too.
  //
  // Split out of DeletePart so the MENU can ask before offering (F1 dogfood
  // round): the item used to be enabled, call DeletePart, and drop both the
  // false and the reason on the floor — an enabled control that silently does
  // nothing, which is the same defect that made File > Save look broken.
  const uint64_t doomedId = state.parts[index].id;
  std::string referencedBy;
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (i == index || state.parts[i].kind != DocKind::Assembly)
      continue;
    const AssemblyDocument* asmDoc = SlotAssembly(state, i);
    if (asmDoc == nullptr)
      continue;
    for (const Occurrence& occ : asmDoc->GetOccurrences()) {
      if (occ.partId != doomedId)
        continue;
      referencedBy += (referencedBy.empty() ? "" : ", ") + state.parts[i].name;
      break;
    }
  }
  if (!referencedBy.empty())
    return "'" + state.parts[index].name + "' is placed in " + referencedBy +
           " — delete those occurrences first: right-click one in the "
           "assembly's Model panel, or call remove_occurrence on that "
           "assembly's tab. Naming only the GUI leaves a script with a "
           "refusal it cannot act on.";
  return {};
}

bool DeletePart(AppState& state, size_t index, std::string* why) {
  if (const std::string blocked = WhyPartUndeletable(state, index);
      !blocked.empty()) {
    if (why)
      *why = blocked;
    return false;
  }
  const uint64_t doomedId = state.parts[index].id;
  const bool deletingActive = index == static_cast<size_t>(state.activePart);
  if (deletingActive) {
    // Activate a neighbor first so AppState always holds a live document.
    SetActivePart(state, index == 0 ? 1 : index - 1);
  }
  // The slot id is about to stop naming anything; release its cached
  // tessellation + GPU assets before it does, or they are held for the life
  // of the session with no way left to reach them.
  EvictPartRenderCache(state, doomedId);
  state.parts.erase(state.parts.begin() + static_cast<long>(index));
  // Fix up the active index after the erase.
  if (static_cast<size_t>(state.activePart) > index)
    state.activePart--;
  // As in AddPart: the deletion lives in project.json alone, and an unsaved
  // one would otherwise come back on the next open with nothing said.
  state.projectMetaDirty = true;
  return true;
}

void CreateFolder(AppState& state, const std::string& folder) {
  if (folder.empty() ||
      std::find(state.tabFolders.begin(), state.tabFolders.end(), folder) !=
          state.tabFolders.end())
    return;
  state.tabFolders.push_back(folder);
  state.projectMetaDirty = true;
}

void MoveToFolder(AppState& state, size_t index, const std::string& folder) {
  if (index >= state.parts.size() || state.parts[index].folder == folder)
    return;
  CreateFolder(state, folder); // no-op for "" / already-registered
  state.parts[index].folder = folder;
  state.projectMetaDirty = true;
}

void RenameFolder(AppState& state, const std::string& from,
                  const std::string& to) {
  if (from.empty() || to.empty() || from == to)
    return;
  for (PartSlot& slot : state.parts)
    if (slot.folder == from)
      slot.folder = to;
  // Registry: rename in place; renaming onto an existing folder merges (drop
  // the old entry instead of duplicating the surviving one).
  auto it = std::find(state.tabFolders.begin(), state.tabFolders.end(), from);
  if (it != state.tabFolders.end()) {
    if (std::find(state.tabFolders.begin(), state.tabFolders.end(), to) !=
        state.tabFolders.end())
      state.tabFolders.erase(it);
    else
      *it = to;
  }
  state.projectMetaDirty = true;
}

void DissolveFolder(AppState& state, const std::string& folder) {
  if (folder.empty())
    return;
  for (PartSlot& slot : state.parts)
    if (slot.folder == folder)
      slot.folder.clear();
  std::erase(state.tabFolders, folder);
  state.projectMetaDirty = true;
}

// Hoisted out of the anonymous namespace by the stage-3 split: the tab-slot
// accessors are the seam ProjectActions.cpp reaches the slots through, so
// they are declared in AppActionsInternal.h and defined here.

// A part's document, whether active (in AppState) or stashed (in its slot).
// Null for an assembly slot.
Document* SlotDoc(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.document.get()
                                                 : state.parts[i].doc.get();
}

// An assembly slot's document, active or stashed. Null for a part slot.
AssemblyDocument* SlotAssembly(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart
             ? state.assembly.get()
             : state.parts[i].assembly.get();
}

// A drawing slot's document, active or stashed. Null for a part/assembly slot.
DrawingDocument* SlotDrawing(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.drawing.get()
                                                 : state.parts[i].drawing.get();
}

// A job slot's document, active or stashed. Null for every other kind.
cam::JobDocument* SlotJob(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.job.get()
                                                 : state.parts[i].job.get();
}

// A simulation slot's document, active or stashed. Null for every other kind.
fem::SimDocument* SlotSim(AppState& state, size_t i) {
  return static_cast<int>(i) == state.activePart ? state.sim.get()
                                                 : state.parts[i].sim.get();
}

} // namespace ui
