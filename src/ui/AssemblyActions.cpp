#include "ui/AssemblyActions.h"
#include "ui/BodyAppearance.h"
#include "ui/MotionActions.h"

#include "core/cam/CamToolpath.h" // StockBounds (the placement snap)
#include "imgui.h" // KeyCtrl: the placement drag snaps unless Ctrl says free
#include "ui/CamPanel.h" // ActiveCamSetup, RefreshCamToolpaths (the placement gizmo)
#include "ui/CamTool.h"  // RunCamVerification
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h" // ConfiguredPartDocument
#include "ui/JobActions.h" // JobPlacedBodies
#include "ui/ProjectActions.h"
#include "ui/SelectionPipeline.h"
#include "ui/ViewActions.h"

#include "ui/FeatureEditor.h" // PickSlot, kMateSessionSlot (U7c mate slots)

#include "cad/CADKernel.h" // CADKernel::ImportedModel (import)
#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingCallout.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FastenerFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ImportAssembly.h"
#include "core/io/Serialization.h"
#include "core/parts/AxisStack.h"
#include "core/parts/FastenerProfile.h"
#include "core/parts/FastenerTable.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"
#include "render/VulkanCommon.h"
#include "ui/DragMath.h"

#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <OSD.hxx>              // per-thread signal handler (prewarm workers)
#include <Standard_Failure.hxx> // ...and what it converts faults into

#include <atomic>
#include <thread>

namespace ui {

void SnapshotAssembly(AppState& state) {
  if (!state.assembly)
    return;
  state.assemblyHistory.undo.push_back(*state.assembly); // full-state copy
  state.assemblyHistory.undoStamps.push_back(NextActionStamp());
  state.assemblyHistory.redo.clear();
  state.assemblyHistory.redoStamps.clear();
}

namespace {
// A snapshot keeps the version and save stamp it was taken at, which later
// edits may have passed and a save may have recorded: restored as it is, an
// undo can land on the saved stamp and read clean. So a restore moves both
// past the current ones (RestoreJob's rule).
void RestoreAssembly(AppState& state, AssemblyDocument restored) {
  const uint64_t current = state.assembly->Version();
  const uint64_t currentStamp = state.assembly->SaveStamp();
  while (restored.Version() <= current || restored.SaveStamp() <= currentStamp)
    restored.BumpVersion();
  *state.assembly = std::move(restored);
}
} // namespace

bool UndoAssembly(AppState& state) {
  if (!state.assembly || state.assemblyHistory.undo.empty())
    return false;
  state.assemblyHistory.redo.push_back(*state.assembly);
  state.assemblyHistory.redoStamps.push_back(
      state.assemblyHistory.undoStamps.back());
  state.assemblyHistory.undoStamps.pop_back();
  RestoreAssembly(state, std::move(state.assemblyHistory.undo.back()));
  state.assemblyHistory.undo.pop_back();
  state.syncedDocumentVersion =
      UINT64_MAX; // restored state may share a version
  return true;
}

bool RedoAssembly(AppState& state) {
  if (!state.assembly || state.assemblyHistory.redo.empty())
    return false;
  state.assemblyHistory.undo.push_back(*state.assembly);
  state.assemblyHistory.undoStamps.push_back(
      state.assemblyHistory.redoStamps.back());
  state.assemblyHistory.redoStamps.pop_back();
  RestoreAssembly(state, std::move(state.assemblyHistory.redo.back()));
  state.assemblyHistory.redo.pop_back();
  state.syncedDocumentVersion = UINT64_MAX;
  return true;
}

bool IsOccurrenceSelected(const AppState& state, uint64_t occId) {
  return state.selection.Has({SelectionItem::Kind::Occurrence, occId, {}});
}

int DeleteSelectedOccurrences(AppState& state, bool takeSnapshot) {
  // One snapshot and one solve for the whole batch: undoing a five-part
  // deletion should be one Ctrl+Z, not five, and re-solving between removals
  // would run the mate solver against half-deleted state.
  if (state.assembly == nullptr)
    return 0;
  const std::vector<uint64_t> doomed = SelectedOccurrenceIds(state);
  if (doomed.empty())
    return 0;
  if (takeSnapshot)
    SnapshotAssembly(state);
  int removed = 0;
  for (const uint64_t id : doomed) {
    // Whatever rides on it goes with it. A screw whose plate is gone is not a
    // screw, and leaving it behind reintroduces exactly the floating hardware
    // attaching it was for (docs/parts/README.md).
    std::vector<uint64_t> riders;
    for (const Occurrence& o : state.assembly->GetOccurrences())
      if (o.attachedTo == id)
        riders.push_back(o.id);
    for (const uint64_t rider : riders)
      if (state.assembly->RemoveOccurrence(rider))
        removed++;
    if (state.assembly->RemoveOccurrence(id))
      removed++;
  }
  SelectOccurrence(state, 0);
  if (removed > 0)
    SolveActiveAssembly(state); // a mate referencing one now dangles/frees
  return removed;
}

int SetOccurrencesVisible(AppState& state, const std::vector<uint64_t>& ids,
                          bool visible) {
  if (state.assembly == nullptr || ids.empty())
    return 0;
  // Snapshot ONCE, and only if something will actually change: a Show over an
  // already-visible selection should not leave an undo step that does nothing
  // when you press Ctrl+Z.
  int changed = 0;
  for (const uint64_t id : ids) {
    const Occurrence* o = state.assembly->FindById(id);
    if (o != nullptr && o->visible != visible)
      changed++;
  }
  if (changed == 0)
    return 0;
  SnapshotAssembly(state);
  for (const uint64_t id : ids) {
    Occurrence* o = state.assembly->FindById(id);
    if (o != nullptr)
      o->visible = visible;
  }
  // The version is what the renderer resyncs against; no solve, because
  // visibility is not an input to one.
  state.assembly->BumpVersion();
  return changed;
}

bool SelectionWouldHide(const AppState& state) {
  if (state.assembly == nullptr)
    return false;
  for (const uint64_t id : SelectedOccurrenceIds(state)) {
    const Occurrence* o = state.assembly->FindById(id);
    if (o != nullptr && o->visible)
      return true;
  }
  return false;
}

int ToggleSelectedOccurrencesVisible(AppState& state) {
  const std::vector<uint64_t> ids = SelectedOccurrenceIds(state);
  return SetOccurrencesVisible(state, ids, !SelectionWouldHide(state));
}

int ShowAllOccurrences(AppState& state) {
  if (state.assembly == nullptr)
    return 0;
  std::vector<uint64_t> hidden;
  for (const Occurrence& o : state.assembly->GetOccurrences())
    if (!o.visible)
      hidden.push_back(o.id);
  return SetOccurrencesVisible(state, hidden, true);
}

void SelectOccurrence(AppState& state, uint64_t occId) {
  // Occurrence items replace only their own kind (a selected mate stays, as
  // with the old separate fields). Highlights are applied by the per-frame
  // Selection projection (SyncSelectionHighlights), not here.
  auto& items = state.selection.items;
  items.erase(std::remove_if(items.begin(), items.end(),
                             [](const SelectionItem& it) {
                               return it.kind ==
                                      SelectionItem::Kind::Occurrence;
                             }),
              items.end());
  if (occId != 0)
    items.push_back({SelectionItem::Kind::Occurrence, occId, {}});
}

bool EnterInContextEdit(AppState& state, uint64_t occurrenceId) {
  if (!state.assembly)
    return false;
  const Occurrence* occ = state.assembly->FindById(occurrenceId);
  if (!occ)
    return false;
  std::optional<size_t> partIndex;
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part &&
        state.parts[i].id == occ->partId)
      partIndex = i;
  if (!partIndex)
    return false; // dangling part reference
  const uint64_t asmSlotId = state.parts[state.activePart].id;
  SetActivePart(state, *partIndex); // clears inContext; re-arm after
  state.inContext = {asmSlotId, occurrenceId};
  return true;
}

assembly::SolveReport SolveActiveAssemblyReport(AppState& state,
                                                const assembly::Driven& driven,
                                                bool diagnose) {
  if (!state.assembly)
    return {};
  const assembly::PartResolver resolve = PartResolverFor(state);
  // A rider from before v157 rides its part's frame: seat it in the hole
  // under it once, so it follows from here on
  // (docs/parts/README.md#attached-and-mated). A no-op once looked at.
  assembly::BindRidersToHoles(*state.assembly, resolve,
                              AssemblyResolverFor(state));
  const assembly::SolveReport rep = assembly::Solve(
      *state.assembly, resolve, AssemblyResolverFor(state), driven, diagnose);
  state.assembly->BumpVersion(); // solved transforms changed → renderer resync
  // Stamp the referenced parts' versions (A4): AssemblyPartsStale compares
  // against these, so a later part edit re-solves on tab activation. Stamped
  // at solve (not render sync) so propagation also works headless.
  //
  // A5: TRANSITIVELY. Resolving through PartDocById alone returns null for an
  // assembly reference, so a sub-assembly never read as stale and the top tab
  // kept rendering yesterday's geometry after an edit inside it — a
  // correct-looking wrong answer. SubtreeVersions stamps every part below,
  // plus each nested assembly's own version (moving an occurrence inside a
  // sub changes no part at all, and must still restale the top).
  state.solvedPartVersions.clear();
  for (const auto& [slotId, version] : assembly::SubtreeVersions(
           *state.assembly, PartResolverFor(state), AssemblyResolverFor(state)))
    state.solvedPartVersions[slotId] = version;
  return rep;
}

int SolveActiveAssembly(AppState& state) {
  return SolveActiveAssemblyReport(state).erroredMates;
}

int SolveActiveAssemblyDriven(AppState& state, uint64_t mateId, MateDof dof,
                              bool diagnose,
                              const std::vector<uint64_t>& path) {
  return SolveActiveAssemblyReport(state, {mateId, dof, path}, diagnose)
      .erroredMates;
}

bool AssemblyPartsStale(AppState& state) {
  if (!state.assembly)
    return false;
  for (const auto& [slotId, version] :
       assembly::SubtreeVersions(*state.assembly, PartResolverFor(state),
                                 AssemblyResolverFor(state))) {
    const auto it = state.solvedPartVersions.find(slotId);
    if (it == state.solvedPartVersions.end() || it->second != version)
      return true; // never stamped, or something below regenerated since
  }
  return false;
}

namespace {
// A body's export appearance: the colour the viewport resolves
// (ui/BodyAppearance.h, so an appearance override and an imported colour
// survive), with the material's PBR when one applies and a neutral plastic
// when not.
void FillItemAppearance(const Body& body, size_t& palette,
                        MeshExport::Item& item) {
  const BodyLook look = ResolveBodyLook(body, palette);
  item.baseColor = look.Color();
  item.metallic = look.hasMaterial ? look.mat.metallic : 0.0f;
  item.roughness = look.hasMaterial ? look.mat.roughness : 0.5f;
}
} // namespace

std::vector<MeshExport::Item> BuildVisualExportItems(AppState& state) {
  std::vector<MeshExport::Item> items;
  if (state.assembly) {
    // One item per (occurrence × body) at the occurrence's SOLVED placement
    // (never exploded — an export is the real assembly). Occurrences of one
    // part reuse the part's shapes, so the shared TShapes let the writers
    // instance the geometry rather than copy it.
    for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
      if (!inst.visible)
        continue;
      // Seeded from the PART, as the assembly renderer seeds it: every
      // occurrence of a part is the colour it is on screen.
      size_t palette = static_cast<size_t>(inst.partId);
      const auto& bodies = inst.part->GetBodies();
      for (size_t bi = 0; bi < bodies.size(); bi++) {
        if (!bodies[bi].shape.IsValid() || bodies[bi].reference)
          continue; // a derived ghost is another part's geometry
        MeshExport::Item item;
        item.shape = bodies[bi].shape;
        // The tree path names the item ("Gangway/Board 03"), which is what a
        // glTF consumer needs to tell two identical boards apart.
        const std::string label =
            inst.path.empty() ? ("occ" + std::to_string(inst.rootOccurrenceId))
                              : inst.path;
        item.name = bodies.size() > 1 ? label + "-" + bodies[bi].name : label;
        item.transform = inst.world; // solved, not exploded
        FillItemAppearance(bodies[bi], palette, item);
        items.push_back(std::move(item));
      }
    }
  } else if (state.document) {
    size_t palette = 0; // the part tab's count, over every body it lists
    for (const Body& body : state.document->GetBodies()) {
      MeshExport::Item item;
      FillItemAppearance(body, palette, item);
      if (!body.shape.IsValid() || body.reference)
        continue; // a derived ghost is another part's geometry
      item.shape = body.shape;
      item.name = body.name;
      items.push_back(std::move(item));
    }
  }
  return items;
}

bool ImportStepAsAssembly(AppState& state, const std::string& stepPath,
                          std::string* error) {
  CADKernel::ImportedModel model;
  if (!CADKernel::Instance().LoadSTEPStructured(stepPath, model)) {
    if (error)
      *error = "could not read '" + stepPath + "' as STEP";
    LOG_WARN("Assembly import: could not read '{}' as STEP", stepPath);
    return false;
  }
  return ImportStepAsAssembly(state, model, stepPath, error);
}

bool ImportStepAsAssembly(AppState& state,
                          const CADKernel::ImportedModel& model,
                          const std::string& stepPath, std::string* error) {
  const auto fail = [&](const std::string& msg) {
    if (error)
      *error = msg;
    LOG_WARN("Assembly import: {}", msg);
    return false;
  };
  // Part geometry extracts as .brep sidecars. A saved project keeps them in
  // its own folder (self-contained, versionable); before the first save they
  // stage under the app state dir and the next SaveProject adopts them into
  // the project (AdoptStagedImports).
  const std::string asmName = std::filesystem::path(stepPath).stem().string();
  const std::filesystem::path importRoot =
      state.projectPath.empty()
          ? std::filesystem::path(Autosaver::StateDir()) / "import-staging"
          : std::filesystem::path(state.projectPath) / "imported";
  // A unique subfolder per import: different files can share product names,
  // and re-importing a changed file must not overwrite .breps that earlier
  // features still read.
  std::filesystem::path destDir = importRoot / asmName;
  for (int n = 2; std::filesystem::exists(destDir); n++)
    destDir = importRoot / (asmName + "-" + std::to_string(n));
  const interchange::AssemblyImportPlan plan =
      interchange::BuildAssemblyImportPlan(model, stepPath, destDir.string());
  if (!plan.error.empty())
    return fail(plan.error);

  // Materialize: one part tab per product, then the assembly. Split into the
  // two steps below so the BACKGROUND import (ui/ImportJob.cpp) can run them a
  // slice per frame and keep its progress modal alive — same code, same order,
  // just spread over frames.
  std::vector<uint64_t> partIds;
  for (const interchange::PlannedPart& p : plan.parts)
    partIds.push_back(MaterializeImportedPart(state, p, asmName));
  MaterializeImportedAssembly(state, plan, asmName, partIds);

  LOG_INFO("Imported '{}' as assembly: {} parts, {} occurrences", stepPath,
           plan.parts.size(), plan.occurrences.size());
  return true;
}

uint64_t MaterializeImportedPart(AppState& state,
                                 const interchange::PlannedPart& p,
                                 const std::string& asmName) {
  // An ImportFeature on the extracted sidecar. Deliberately not undoable — it
  // creates a document, like File > Open. The parts land in a tab-bar folder
  // named after the assembly so a 100-part import is one flyout, not 100 tabs.
  AddPart(state, p.name);
  PartSlot& slot = state.parts[static_cast<size_t>(state.activePart)];
  slot.folder = asmName;
  CreateFolder(state, asmName); // register (idempotent) so saves know it
  auto feature = std::make_unique<ImportFeature>(p.name, p.brepPath);
  feature->hasColor = p.hasColor;
  feature->color = p.color;
  state.document->AddFeature(std::move(feature));
  return slot.id;
}

void MaterializeImportedAssembly(AppState& state,
                                 const interchange::AssemblyImportPlan& plan,
                                 const std::string& asmName,
                                 const std::vector<uint64_t>& partIds) {
  AddAssembly(state, asmName);
  bool first = true;
  for (const interchange::PlannedOccurrence& o : plan.occurrences) {
    if (o.part >= partIds.size())
      continue; // a part that failed to materialize takes its occurrences
    Occurrence occ;
    occ.partId = partIds[o.part];
    occ.name = o.name;
    occ.position = o.position;
    occ.orientation = o.orientation;
    occ.fixed = first; // the first occurrence anchors, as insert-part does
    first = false;
    state.assembly->AddOccurrence(occ);
  }
  state.assembly->BumpVersion();
  FitCameraToDocument(state);
}

// ---------------------------------------------------------------------------
// Occurrence placement and part lookup, from AppActions.cpp
// (docs/architecture.md#the-ui-actions-layer). They were in
// the monolith because the lifecycle and drawing code needed them too;
// AppActionsInternal.h keeps declaring the two that still cross a file, and
// they are defined here now, with the assembly they describe.

// An occurrence's placement as a model matrix (translate ∘ rotate).
glm::dmat4 OccurrenceMatrix(const Occurrence& o) {
  return glm::translate(glm::dmat4(1.0), o.position) *
         glm::mat4_cast(o.orientation);
}

// Exploded-view display (A4): the occurrence's factor-scaled world offset in
// the active exploded view (zero when none / no step). PURE VIEW STATE —
// composed into rendered/picked transforms only, never into the document.
glm::dvec3 ExplodeOffset(const AppState& state, uint64_t occId) {
  if (!state.activeExplodedView || !state.assembly)
    return glm::dvec3(0.0);
  const ExplodedView* v =
      state.assembly->FindExplodedViewById(state.activeExplodedView);
  if (!v)
    return glm::dvec3(0.0);
  for (const ExplodeStep& s : v->steps)
    if (s.path.empty() && s.occurrenceId == occId)
      return s.offset * double(state.explodeFactor);
  return glm::dvec3(0.0);
}

glm::dvec3 ExplodeInnerOffset(const AppState& state, uint64_t occId,
                              const std::vector<uint64_t>& innerPath) {
  if (innerPath.empty() || !state.activeExplodedView || !state.assembly)
    return glm::dvec3(0.0);
  // Only a FLEXIBLE unit's parts explode one by one; a rigid one is a block.
  const Occurrence* unit = state.assembly->FindById(occId);
  if (unit == nullptr || !unit->flexible)
    return glm::dvec3(0.0);
  const ExplodedView* v =
      state.assembly->FindExplodedViewById(state.activeExplodedView);
  if (!v)
    return glm::dvec3(0.0);
  for (const ExplodeStep& s : v->steps)
    if (s.path.size() == 1 && s.path.front() == occId &&
        s.occurrenceId == innerPath.front())
      return s.offset * double(state.explodeFactor);
  return glm::dvec3(0.0);
}

// What an occurrence renders (and picks) at: the solved placement, displaced
// by the active exploded view.
glm::dmat4 DisplayMatrix(const AppState& state, const Occurrence& o) {
  // Motion playback (docs/ui/panels.md#assembly-tab-the-model-panel): the pose
  // a run recorded stands in for the solved placement, for display only.
  glm::dmat4 played;
  if (MotionPlaybackWorld(state, o.id, played))
    return glm::translate(glm::dmat4(1.0), ExplodeOffset(state, o.id)) * played;
  return glm::translate(glm::dmat4(1.0), ExplodeOffset(state, o.id)) *
         OccurrenceMatrix(o);
}

glm::dmat4 DisplayMatrixOf(const AppState& state, const Occurrence& o) {
  return DisplayMatrix(state, o);
}

// The part document an occurrence references, by stable id (active or stashed).
Document* PartDocById(AppState& state, uint64_t partId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part && state.parts[i].id == partId)
      return SlotDoc(state, i);
  return nullptr;
}

// The ASSEMBLY document an occurrence references (A5), by the same stable slot
// id — an occurrence's `partId` resolves against parts or assemblies
// depending on its refKind. Null when the id is not an assembly slot.
AssemblyDocument* AssemblyDocById(AppState& state, uint64_t slotId) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Assembly && state.parts[i].id == slotId)
      return SlotAssembly(state, i);
  return nullptr;
}

// The two resolvers Flatten wants, bound to this AppState. Every walker that
// needs the leaf parts of the active assembly goes through here, so none of
// them has to know how slots are stored.
assembly::PartResolver PartResolverFor(AppState& state) {
  return [&state](uint64_t id, uint64_t configuration) -> const Document* {
    return configuration == 0
               ? PartDocById(state, id)
               : ConfiguredPartDocument(state, id, configuration);
  };
}
const Document* OccurrencePartDoc(AppState& state, const Occurrence& occ) {
  return PartResolverFor(state)(occ.partId, occ.configurationId);
}

const Document* MeshPartDoc(AppState& state, const DocumentRenderMesh& mesh,
                            const Occurrence& occ) {
  return mesh.leafPartId != 0
             ? PartResolverFor(state)(mesh.leafPartId, mesh.leafConfiguration)
             : OccurrencePartDoc(state, occ);
}

assembly::AssemblyResolver AssemblyResolverFor(AppState& state) {
  return [&state](uint64_t id) -> const AssemblyDocument* {
    return AssemblyDocById(state, id);
  };
}

// The leaf parts of the active assembly, placed. Empty (with the reason
// logged once) when the tree cannot be walked — a cycle, or nesting past the
// depth cap. Callers render/aggregate what they get; refusing to draw
// anything at all would hide the model AND the message.
std::vector<Body> OccurrenceSubtreeBodies(AppState& state,
                                          uint64_t rootOccurrenceId) {
  std::vector<Body> out;
  for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
    if (!inst.visible || inst.rootOccurrenceId != rootOccurrenceId)
      continue;
    for (const Body& b : inst.part->GetBodies()) {
      if (b.reference)
        continue; // a derived ghost is not the occurrence's matter
      Body nb = b;
      nb.shape = b.shape.Transformed(inst.world);
      out.push_back(std::move(nb));
    }
  }
  return out;
}

std::vector<assembly::FlatInstance> FlattenActiveAssembly(AppState& state) {
  if (!state.assembly)
    return {};
  auto flat = assembly::Flatten(*state.assembly, PartResolverFor(state),
                                AssemblyResolverFor(state));
  if (!flat) {
    LOG_ERROR("Assembly cannot be walked: {}", flat.error());
    return {};
  }
  return *flat;
}

} // namespace ui
