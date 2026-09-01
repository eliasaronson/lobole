#include "ui/AssemblyActions.h"
#include "ui/AppActions.h"

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

bool UndoAssembly(AppState& state) {
  if (!state.assembly || state.assemblyHistory.undo.empty())
    return false;
  state.assemblyHistory.redo.push_back(*state.assembly);
  state.assemblyHistory.redoStamps.push_back(
      state.assemblyHistory.undoStamps.back());
  state.assemblyHistory.undoStamps.pop_back();
  *state.assembly = std::move(state.assemblyHistory.undo.back());
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
  *state.assembly = std::move(state.assemblyHistory.redo.back());
  state.assemblyHistory.redo.pop_back();
  state.syncedDocumentVersion = UINT64_MAX;
  return true;
}

bool IsOccurrenceSelected(const AppState& state, uint64_t occId) {
  return state.selection.Has({SelectionItem::Kind::Occurrence, occId, {}});
}

void ToggleOccurrence(AppState& state, uint64_t occId) {
  if (occId != 0)
    state.selection.Toggle({SelectionItem::Kind::Occurrence, occId, {}});
}

int DeleteSelectedOccurrences(AppState& state) {
  // One snapshot and one solve for the whole batch: undoing a five-part
  // deletion should be one Ctrl+Z, not five, and re-solving between removals
  // would run the mate solver against half-deleted state.
  if (state.assembly == nullptr)
    return 0;
  const std::vector<uint64_t> doomed = SelectedOccurrenceIds(state);
  if (doomed.empty())
    return 0;
  SnapshotAssembly(state);
  int removed = 0;
  for (const uint64_t id : doomed) {
    // Whatever rides on it goes with it. A screw whose plate is gone is not a
    // screw, and leaving it behind reintroduces exactly the floating hardware
    // attaching it was for (docs/parts/f1-loose-ends.md).
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

int SolveActiveAssembly(AppState& state) {
  if (!state.assembly)
    return 0;
  const assembly::PartResolver resolve =
      [&state](uint64_t partId) -> const Document* {
    return PartDocById(state, partId);
  };
  const assembly::SolveReport rep =
      assembly::Solve(*state.assembly, resolve, AssemblyResolverFor(state));
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
  return rep.erroredMates;
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
// How far along the axis the LAST material is — the maximum exit, not the
// last segment's.
//
// `TraverseAxis` orders by where each solid is entered, which is not the same
// order as where they are left once anything overlaps: a plate moved down past
// a nut is entered after the washer and left after the nut. Reading the end of
// the ordered list said a stale screw still reached, which is precisely the
// case the check exists for.
double LastExit(const std::vector<parts::StackSegment>& stack) {
  double out = 0.0;
  for (const parts::StackSegment& seg : stack)
    out = std::max(out, seg.exit);
  return out;
}

// Millimetres as a person writes them: "12 mm", "3.5 mm" — not the six
// decimal places std::to_string gives, which makes a refusal read like a
// stack trace.
std::string Mm(double v) {
  std::ostringstream out;
  out.precision(4);
  // -0.0 prints as "-0", and a refusal reading "the axis meets its face -0 mm
  // out" reads as a broken message rather than a measurement. (-0.0 == 0.0 is
  // true, so this assignment is what normalises the sign.)
  if (v == 0.0)
    v = 0.0;
  out << v << " mm";
  return out.str();
}

} // namespace

BomTable ComputeBom(AppState& state) {
  BomTable table;
  if (!state.assembly)
    return table;
  // A5: aggregate over the LEAF parts of the whole tree, so a sub-assembly
  // placed twice doubles its parts' quantities. The per-unit BOM needs no
  // code of its own — it is this same function on that assembly's tab.
  for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
    if (!inst.visible)
      continue; // hidden or suppressed, here or above: out of the BOM
    BomRow* row = nullptr;
    for (BomRow& r : table.rows)
      if (r.partId == inst.partId) {
        row = &r;
        break;
      }
    if (row) {
      row->quantity++;
      continue;
    }
    BomRow fresh;
    fresh.partId = inst.partId;
    fresh.quantity = 1;
    for (const PartSlot& slot : state.parts)
      if (slot.kind == DocKind::Part && slot.id == inst.partId)
        fresh.partName = slot.name;
    const Document* part = PartDocById(state, inst.partId);
    if (const FastenerFeature* screw = SoleFastenerOf(part)) {
      fresh.standard = screw->standard;
      fresh.designation = screw->Designation();
    }
    if (!part) {
      fresh.partName = "(missing part)"; // dangling reference — mass unknown
      table.warnings.push_back("'" + inst.path +
                               "' references a missing part — mass unknown");
    } else if (const auto mass = Measure::MassOfBodies(part->GetBodies())) {
      fresh.unitVolume = mass->volume;
      fresh.massKnown = mass->massKnown;
      fresh.unitMassG = mass->massG;
      for (size_t i = 0; i < mass->bodies.size(); i++) {
        if (i == 0)
          fresh.material = mass->bodies[i].material;
        else if (mass->bodies[i].material != fresh.material) {
          fresh.material = "mixed";
          break;
        }
      }
      for (const Measure::BodyMass& bm : mass->bodies)
        if (!bm.massKnown)
          table.warnings.push_back(
              "part '" + fresh.partName + "': body '" + bm.name + "' has " +
              (bm.material.empty() ? "no material" : "no density") +
              " — mass unknown");
    }
    table.rows.push_back(std::move(fresh));
  }
  for (const BomRow& r : table.rows) {
    if (r.massKnown)
      table.totalMassG += r.unitMassG * r.quantity;
    else
      table.massKnown = false;
  }
  if (!table.massKnown)
    table.totalMassG = 0.0;
  return table;
}

std::vector<FastenerCheck> CheckFasteners(AppState& state) {
  std::vector<FastenerCheck> out;
  if (!state.assembly)
    return out;
  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);

  // Every body once, tagged with the instance it came from, so a fastener can
  // be left out of its OWN traversal without leaving out its twin.
  std::vector<parts::PlacedBody> all;
  std::vector<size_t> owner;
  for (size_t i = 0; i < flat.size(); i++) {
    if (flat[i].part == nullptr || !flat[i].visible)
      continue;
    for (const Body& body : flat[i].part->GetBodies()) {
      parts::PlacedBody pb;
      pb.body = &body;
      pb.world = flat[i].world;
      pb.part = flat[i].path.empty() ? body.name : flat[i].path;
      pb.partId = flat[i].partId;
      pb.occurrence = flat[i].rootOccurrenceId;
      all.push_back(pb);
      owner.push_back(i);
    }
  }

  for (size_t i = 0; i < flat.size(); i++) {
    const FastenerFeature* screw = SoleFastenerOf(flat[i].part);
    if (screw == nullptr || !flat[i].visible)
      continue;
    const parts::Profile* profile = parts::ProfileFor(screw->standard);
    if (profile == nullptr || profile->kind != parts::Kind::Screw)
      continue; // a nut or a washer spans nothing; the screw is the claim
    const std::optional<double> nominal = parts::NominalDiameter(screw->size);
    if (!nominal)
      continue;
    // Probed past the LOOSEST hole this size is drilled to, not past the
    // thread: a placed fastener knows its size and its axis but not the bore
    // it went through, and a ray offset by the thread radius runs down the
    // inside of the clearance and finds nothing. A hole wider than "Loose"
    // would make this cry wolf, which is the right way round for a check.
    const std::optional<double> bore =
        parts::WidestClearance(screw->size, nullptr);
    const double probe = bore ? *bore * 0.5 : *nominal * 0.5;

    // Its own placement IS the derivation: the bearing face is the frame's
    // z = 0 and the shank runs +Z, so the rim and the axis come back out of
    // the matrix with nothing stored.
    const glm::vec3 origin(flat[i].world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    const glm::vec3 axis = glm::normalize(
        glm::vec3(flat[i].world * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));

    std::vector<parts::PlacedBody> others;
    for (size_t b = 0; b < all.size(); b++)
      if (owner[b] != i)
        others.push_back(all[b]);

    FastenerCheck finding;
    finding.occurrence = flat[i].rootOccurrenceId;
    finding.part = screw->Designation();

    const double reach = 10000.0;
    const std::vector<parts::StackSegment> stack = parts::TraverseAxis(
        others, glm::dvec3(origin), glm::dvec3(axis), probe, reach);
    if (stack.empty()) {
      finding.why = "spans nothing — there is no material on its axis, so it "
                    "is holding the two halves of the air together";
      out.push_back(finding);
      continue;
    }
    // Everything the shank has to reach through, WASHERS AND NUT INCLUDED:
    // they are occurrences on the same axis, so re-traversing finds them the
    // same way it found them when the length was derived.
    const double grip = LastExit(stack);
    if (screw->length < grip - 1e-3) {
      finding.why = "is " + Mm(grip - screw->length) +
                    " too short: its stack is now " + Mm(grip) + " and it is " +
                    Mm(screw->length) +
                    " under the head. Re-fit it, or re-derive from the rim.";
      out.push_back(finding);
      continue;
    }
    // And bottoming out, which the length alone cannot show: a blind hole is
    // whatever stops the ON-axis ray, and a screw longer than that is jacking
    // the joint apart rather than clamping it.
    const std::vector<parts::StackSegment> onAxis = parts::TraverseAxis(
        others, glm::dvec3(origin), glm::dvec3(axis), 0.0, reach);
    if (!onAxis.empty() && screw->length > onAxis.front().enter + 1e-3) {
      finding.why = "bottoms out: the hole is " + Mm(onAxis.front().enter) +
                    " deep and the screw is " + Mm(screw->length) +
                    " long, so it lands on the floor and clamps nothing.";
      out.push_back(finding);
    }
  }
  return out;
}

std::string BomToCsv(const BomTable& bom) {
  auto field = [](const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos)
      return s;
    std::string quoted = "\"";
    for (char c : s) {
      if (c == '"')
        quoted += '"';
      quoted += c;
    }
    return quoted + "\"";
  };
  // `standard` and `designation` are the two columns a purchase order needs
  // and the panel has no room for: blank on a part somebody made, filled on
  // a bought one, so a filter on either splits the list in two.
  std::string csv = "part,quantity,standard,designation,material,"
                    "unit_volume_mm3,unit_mass_g,total_mass_g\n";
  char buf[64];
  auto num = [&buf](double v) {
    snprintf(buf, sizeof buf, "%.6g", v);
    return std::string(buf);
  };
  for (const BomRow& r : bom.rows) {
    csv += field(r.partName) + "," + std::to_string(r.quantity) + "," +
           field(r.standard) + "," + field(r.designation) + "," +
           field(r.material) + "," + num(r.unitVolume) + ",";
    if (r.massKnown)
      csv += num(r.unitMassG) + "," + num(r.unitMassG * r.quantity);
    else
      csv += ",";
    csv += "\n";
  }
  csv += "total,,,,,,,";
  if (bom.massKnown)
    csv += num(bom.totalMassG);
  csv += "\n";
  return csv;
}

bool PickAssemblyFace(AppState& state, const Ray& ray, AssemblyFacePick& out) {
  if (!state.assembly || !state.faceSelector)
    return false;
  const RayHit hit = state.faceSelector->PickFace(ray);
  if (!hit.hit)
    return false;

  // meshId → the occurrence + which of its part's bodies was hit (both carried
  // on the render mesh precisely so an assembly pick can be named).
  const DocumentRenderMesh* rm = nullptr;
  for (const DocumentRenderMesh& m : state.documentMeshes)
    if (m.meshId == hit.meshId) {
      rm = &m;
      break;
    }
  if (!rm || rm->occurrenceId == 0)
    return false;

  const Occurrence* occ = state.assembly->FindById(rm->occurrenceId);
  if (!occ)
    return false;
  // A5: the face belongs to the LEAF part. For a sub-assembly occurrence
  // occ->partId is an assembly — no bodies, no faces — so resolving through
  // it returned false and a click inside a placed unit picked nothing.
  Document* part =
      PartDocById(state, rm->leafPartId ? rm->leafPartId : occ->partId);
  if (!part || rm->bodyIndex >= part->GetBodies().size())
    return false;

  // The face's persistent toponaming name → an ElementRef the mate stores.
  const Body& body = part->GetBodies()[rm->bodyIndex];
  const std::string* name =
      body.elementMap.NameOf(static_cast<int>(hit.faceIndex));
  if (!name)
    return false; // an unnamed face (e.g. a pattern output) can't be mated

  out.occurrenceId = rm->occurrenceId;
  out.innerPath = rm->innerPath;
  out.face = ElementRef{body.createdBy, *name};
  out.snapEdge = ElementRef{};
  out.snapEdge2 = ElementRef{};
  out.snapType = AssemblyFacePick::SnapType::FaceCenter;

  // Every named boundary edge contributes its characteristic point: a circle
  // center or another edge's midpoint. The face center competes as a candidate
  // too, so a click in the middle of a face does not always snap to an edge.
  //
  // ONE call for all of them. This was three — FaceEdgeIndices,
  // FaceCircleCenters, and EdgeSnapPoint once per edge — each of which mapped
  // every edge of the whole body, so a hover on a drilled face cost
  // O(face edges x body edges) every frame: 10 ms on a 144-hole plate,
  // 88 ms on a 400-hole one (docs/known-bugs.md).
  std::vector<CADShape::FaceSnap> snaps;
  body.shape.FaceSnapPoints(static_cast<int>(hit.faceIndex), snaps);
  // ...and the face's CORNERS (phase 4c), which join the same ranking. A
  // corner is named by the two edges meeting there, both already named;
  // pairs that meet twice name no single point and are not offered.
  std::vector<CADShape::FaceCorner> corners;
  body.shape.FaceCorners(static_cast<int>(hit.faceIndex), corners);
  const glm::mat4 xf = OccurrenceMatrix(*occ);
  glm::vec3 faceOrigin, faceNormal, faceXAxis;
  const bool hasFaceFrame =
      body.shape.FacePlane(static_cast<int>(hit.faceIndex), faceOrigin,
                           faceNormal, faceXAxis) ||
      body.shape.FaceAxis(static_cast<int>(hit.faceIndex), faceOrigin,
                          faceNormal, faceXAxis);
  const glm::vec3 worldFaceOrigin = glm::vec3(xf * glm::vec4(faceOrigin, 1.0f));
  float best =
      hasFaceFrame ? glm::length(worldFaceOrigin - hit.point) : FLT_MAX;
  for (const CADShape::FaceSnap& snap : snaps) {
    const std::string* edgeName = body.elementMap.EdgeNameOf(snap.edgeIndex);
    if (!edgeName) // an unnamed edge cannot be stored on the anchor
      continue;
    const glm::vec3 worldPoint = glm::vec3(xf * glm::vec4(snap.point, 1.0f));
    const float distance = glm::length(worldPoint - hit.point);
    if (distance >= best)
      continue;
    best = distance;
    out.snapEdge = ElementRef{body.createdBy, *edgeName};
    out.snapEdge2 = ElementRef{};
    out.snapType = snap.circleCenter ? AssemblyFacePick::SnapType::HoleCenter
                                     : AssemblyFacePick::SnapType::EdgeMidpoint;
  }
  for (const CADShape::FaceCorner& corner : corners) {
    const std::string* nameA = body.elementMap.EdgeNameOf(corner.edgeA);
    const std::string* nameB = body.elementMap.EdgeNameOf(corner.edgeB);
    if (!nameA || !nameB) // both halves must be nameable, or it is not stable
      continue;
    const glm::vec3 worldPoint = glm::vec3(xf * glm::vec4(corner.point, 1.0f));
    const float distance = glm::length(worldPoint - hit.point);
    if (distance >= best)
      continue;
    best = distance;
    out.snapEdge = ElementRef{body.createdBy, *nameA};
    out.snapEdge2 = ElementRef{body.createdBy, *nameB};
    out.snapType = AssemblyFacePick::SnapType::Corner;
  }
  return true;
}

std::optional<glm::mat4> ConnectorWorldFrame(AppState& state,
                                             const Connector& c) {
  if (!state.assembly)
    return std::nullopt;
  const assembly::PartResolver resolve =
      [&state](uint64_t partId) -> const Document* {
    return PartDocById(state, partId);
  };
  const auto local = assembly::ConnectorLocalFrame(c, *state.assembly, resolve);
  if (!local)
    return std::nullopt;
  const Occurrence* occ = state.assembly->FindById(c.occurrenceId);
  if (!occ)
    return std::nullopt;
  return OccurrenceMatrix(*occ) * *local;
}

// Arms one of the mate session's connector slots on the shared armed-slot
// state (kMateSessionSlot marks it as non-feature; SlotPickClicked routes
// back to MateConnectorPicked).
void ArmMateConnector(AppState& state, bool second) {
  state.slotPick = {kMateSessionSlot,
                    static_cast<int>(second ? PickSlot::MateConnectorB
                                            : PickSlot::MateConnectorA),
                    SelectionKindBit(SelectionItem::Kind::Connector),
                    /*multi=*/false,
                    second ? "Pick the second face (connector B)"
                           : "Pick the first face (connector A)"};
}

namespace {
// A pipeline Connector item as the mate-side Connector it names.
Connector ConnectorFromItem(const SelectionItem& item) {
  Connector c;
  c.occurrenceId = item.id;
  c.anchor.origin = false;
  c.anchor.face = item.element;
  c.anchor.snapEdge = item.snapElement;
  c.anchor.snapEdge2 = item.snapElement2;
  // A5/v57: a face inside a placed unit. The mate still positions the
  // top-level occurrence; the path just says which face it anchors on.
  c.innerPath = item.innerPath;
  return c;
}
} // namespace

void StartMateTool(AppState& state, MateType type) {
  if (!state.assembly)
    return;
  CancelMateTool(state); // discard any in-progress preview first
  state.mateTool = AppState::MateToolState{};
  state.mateTool.active = true;
  state.mateTool.type = type;
  SelectOccurrence(state, 0); // the tool owns clicks now
  ArmMateConnector(state, /*second=*/false);
}

void StartMateEdit(AppState& state, uint64_t mateId) {
  if (!state.assembly || state.assembly->FindMateById(mateId) == nullptr)
    return;
  CancelMateTool(state);
  state.mateTool = AppState::MateToolState{};
  state.mateTool.active = true;
  state.mateTool.editMateId = mateId;
  state.mateTool.message =
      "Click a connector row, then pick its new face in the viewport";
}

void MateConnectorPicked(AppState& state, const SelectionItem& item) {
  auto& tool = state.mateTool;
  if (!state.assembly || !tool.active ||
      item.kind != SelectionItem::Kind::Connector) {
    return;
  }
  const bool slotB =
      state.slotPick.slot == static_cast<int>(PickSlot::MateConnectorB);
  const Connector conn = ConnectorFromItem(item);

  // Re-pick session on an existing mate: rewrite the armed side and
  // re-solve; each pick is one undo step. The slot disarms after one pick
  // (click the other row to re-pick that side).
  if (tool.editMateId != 0) {
    Mate* m = state.assembly->FindMateById(tool.editMateId);
    if (m == nullptr) {
      CancelMateTool(state);
      return;
    }
    Connector& other = slotB ? m->a : m->b;
    if (conn.occurrenceId == other.occurrenceId) {
      tool.message = "Pick a face on a different occurrence";
      return;
    }
    SnapshotAssembly(state);
    (slotB ? m->b : m->a) = conn;
    state.assembly->BumpVersion();
    SolveActiveAssembly(state);
    tool.message.clear();
    state.slotPick = {};
    return;
  }

  if (!slotB) {
    tool.first = conn;
    tool.haveFirst = true;
    tool.message = conn.anchor.snapEdge.IsSet()
                       ? "Snapped to an edge — pick connector B"
                       : "";
    ArmMateConnector(state, /*second=*/true);
    return;
  }
  if (conn.occurrenceId == tool.first.occurrenceId) {
    tool.message = "Pick a face on a different occurrence";
    return; // stay armed
  }
  // Second connector: add a live preview mate (snapshot first, so accept is a
  // single undoable action and cancel just pops it) and solve.
  Mate mate;
  mate.type = tool.type;
  mate.name = "Mate";
  mate.a = tool.first;
  mate.b = conn;
  SnapshotAssembly(state);
  tool.previewMateId = state.assembly->AddMate(mate);
  const int errored = SolveActiveAssembly(state);
  tool.message = errored ? "Over-constrained — try Flip, or Cancel"
                         : "Accept, Flip, or Cancel";
  state.slotPick = {}; // both picked; the session shows Accept/Cancel
}

void AcceptMateTool(AppState& state) {
  if (!state.mateTool.active)
    return;
  const uint64_t id = state.mateTool.previewMateId;
  state.mateTool = AppState::MateToolState{}; // keep the mate; clear the tool
  if (state.slotPick.featureId == kMateSessionSlot)
    state.slotPick = {};
  if (id)
    SelectMate(state, id);
}

void CancelMateTool(AppState& state) {
  auto& tool = state.mateTool;
  if (tool.previewMateId && state.assembly) {
    state.assembly->RemoveMate(tool.previewMateId);
    if (!state.assemblyHistory.undo.empty())
      state.assemblyHistory.undo.pop_back(); // drop the snapshot we pushed
    SolveActiveAssembly(state);
  }
  tool = AppState::MateToolState{};
  if (state.slotPick.featureId == kMateSessionSlot)
    state.slotPick = {};
}

namespace {
void AppendSnapMarker(std::vector<LineVertex3D>& lines, const glm::mat4& f,
                      float scale, bool edgeSnap, bool midpoint, bool corner) {
  const glm::vec3 origin(f[3]);
  const glm::vec3 color = corner     ? glm::vec3(0.55f, 1.0f, 0.45f)
                          : midpoint ? glm::vec3(0.30f, 0.85f, 1.0f)
                          : edgeSnap ? glm::vec3(1.0f, 0.78f, 0.22f)
                                     : glm::vec3(0.82f, 0.82f, 0.60f);
  const glm::vec3 u = glm::normalize(glm::vec3(f[0])) * scale;
  const glm::vec3 v = glm::normalize(glm::vec3(f[1])) * scale;
  if (corner) {
    // An L, not a cross: a corner marker that reads as a corner, and does
    // not look identical to the three snaps that are not one.
    lines.push_back(LineVertex(origin, color));
    lines.push_back(LineVertex(origin + u, color));
    lines.push_back(LineVertex(origin, color));
    lines.push_back(LineVertex(origin + v, color));
    return;
  }
  lines.push_back(LineVertex(origin - u, color));
  lines.push_back(LineVertex(origin + u, color));
  lines.push_back(LineVertex(origin - v, color));
  lines.push_back(LineVertex(origin + v, color));
}

void AppendFrameTriad(std::vector<LineVertex3D>& lines, const glm::mat4& f,
                      float scale) {
  const glm::vec3 o(f[3]);
  const glm::vec3 cols[3] = {
      {0.90f, 0.32f, 0.32f}, {0.35f, 0.82f, 0.38f}, {0.38f, 0.55f, 0.95f}};
  for (int a = 0; a < 3; a++) {
    lines.push_back(LineVertex(o, cols[a]));
    lines.push_back(LineVertex(o + glm::vec3(f[a]) * scale, cols[a]));
  }
}
} // namespace

void BuildMateToolOverlay(AppState& state, std::vector<LineVertex3D>& lines) {
  if (!state.assembly || !state.mateTool.active)
    return;
  const float scale = 1.2f;
  if (state.mateTool.hasHover && state.mateTool.previewMateId == 0)
    if (const auto f = ConnectorWorldFrame(state, state.mateTool.hover))
      AppendSnapMarker(lines, *f, GizmoScale(state) * 0.12f,
                       state.mateTool.hover.anchor.snapEdge.IsSet(),
                       state.mateTool.hoverSnapIsMidpoint,
                       state.mateTool.hoverSnapIsCorner);
  if (state.mateTool.haveFirst)
    if (const auto f = ConnectorWorldFrame(state, state.mateTool.first))
      AppendFrameTriad(lines, *f, scale);
  if (const Mate* m =
          state.mateTool.previewMateId
              ? state.assembly->FindMateById(state.mateTool.previewMateId)
              : nullptr) {
    if (const auto fb = ConnectorWorldFrame(state, m->b))
      AppendFrameTriad(lines, *fb, scale);
  }
}

// ---- Occurrence drag gizmo (A2.5) -----------------------------------------

namespace {
const glm::vec3 kGizmoAxis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const glm::vec3 kGizmoColor[3] = {
    {0.90f, 0.32f, 0.32f}, {0.35f, 0.82f, 0.38f}, {0.38f, 0.55f, 0.95f}};

// Ray/axis/plane/arc math shared with the preview handles: ui/DragMath.h
// (GizmoScale, PerpTo, RayAxisParam, RayPlane, PointRayDist, RotateAngle).

// The gizmo-anchored occurrence: selected, non-fixed, mate tool idle. null
// else.
Occurrence* GizmoOccurrence(AppState& state) {
  const uint64_t occId = SelectedOccurrenceId(state);
  if (!state.assembly || occId == 0 || state.mateTool.active)
    return nullptr;
  Occurrence* o = state.assembly->FindById(occId);
  if (!o)
    return nullptr;
  // Explode mode drags a view offset, not the placement — even a fixed base
  // part explodes.
  if (state.activeExplodedView)
    return o;
  // Something that RIDES on another part has a DERIVED pose: dragging it
  // would be overwritten by the next ApplyAttachments, which is the shape of
  // a control that appears to work and does not. This is why a placed
  // fastener could be pulled out of its hole (docs/parts/f1-loose-ends.md).
  if (o->attachedTo != 0)
    return nullptr;
  return !o->fixed ? o : nullptr;
}

// The gizmo's frame + which handles show. A free occurrence gets world axes at
// its origin with all 6 handles; a mated occurrence gets its mate's connector-A
// world frame with only the mate's free DOF (drag-within-mate, phase 3).
struct GizmoSpec {
  glm::vec3 origin{0};
  glm::vec3 axis[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  bool trans[3] = {false, false, false}; // translate arrow per axis
  bool rot[3] = {false, false, false};   // rotate arc per axis
};

// The mate whose placed (`b`) side is this occurrence, if any (its DOF the
// gizmo drives). Skips errored mates.
Mate* MateDrivingOccurrence(AssemblyDocument& asmDoc, uint64_t occId) {
  for (Mate& m : asmDoc.GetMates())
    if (m.b.occurrenceId == occId && m.error.empty())
      return &m;
  return nullptr;
}

// Fills `spec` for the selected occurrence and reports the driving mate (null =
// free 6-DOF). Returns false if no gizmo should show (fastened mate / dangling
// connector).
bool BuildGizmoSpec(AppState& state, Occurrence* occ, GizmoSpec& spec,
                    Mate*& mate) {
  mate = nullptr;
  if (state.activeExplodedView) {
    // Explode authoring (A4): world-aligned translate arrows at the DISPLAYED
    // position; the drag writes the view's step offset. No rotation — an
    // exploded view is pure displacement.
    if (!state.assembly->FindExplodedViewById(state.activeExplodedView))
      return false;
    spec.origin = occ->position + ExplodeOffset(state, occ->id);
    for (int i = 0; i < 3; i++) {
      spec.axis[i] = kGizmoAxis[i];
      spec.trans[i] = true;
    }
    return true;
  }
  mate = MateDrivingOccurrence(*state.assembly, occ->id);
  if (!mate) {
    spec.origin = occ->position; // free: world axes, all handles
    for (int i = 0; i < 3; i++) {
      spec.axis[i] = kGizmoAxis[i];
      spec.trans[i] = spec.rot[i] = true;
    }
    return true;
  }
  const auto Wa = ConnectorWorldFrame(state, mate->a);
  if (!Wa)
    return false; // dangling connector → no meaningful DOF gizmo
  spec.origin = glm::vec3((*Wa)[3]);
  for (int i = 0; i < 3; i++)
    spec.axis[i] = glm::normalize(glm::vec3((*Wa)[i]));
  switch (mate->type) {
  case MateType::Revolute:
    spec.rot[2] = true;
    break;
  case MateType::Slider:
    spec.trans[2] = true;
    break;
  case MateType::Cylindrical:
    spec.rot[2] = spec.trans[2] = true;
    break;
  case MateType::Planar:
    spec.trans[0] = spec.trans[1] = spec.rot[2] = true;
    break;
  case MateType::Ball:
    spec.rot[0] = spec.rot[1] = spec.rot[2] = true;
    break;
  case MateType::Fastened:
    return false; // no DOF
  }
  return true;
}

// Ray vs the spec's enabled handles → (axis, rotate). Arrows win ties.
bool GizmoHitTest(const AppState& state, const GizmoSpec& spec, const Ray& ray,
                  int& axis, bool& rotate) {
  const float scale = GizmoScale(state);
  const float grab = 0.14f * scale, L = scale, R = 0.78f * scale;
  float best = grab;
  bool found = false;
  for (int i = 0; i < 3; i++) {
    const glm::vec3 A = spec.axis[i];
    if (spec.trans[i]) {
      float t =
          std::clamp(RayAxisParam(ray.origin, ray.direction, spec.origin, A),
                     0.15f * L, L);
      const float da =
          PointRayDist(spec.origin + A * t, ray.origin, ray.direction);
      if (da < best) {
        best = da;
        axis = i;
        rotate = false;
        found = true;
      }
    }
    if (spec.rot[i]) {
      glm::vec3 hit;
      if (RayPlane(ray.origin, ray.direction, spec.origin, A, hit)) {
        const float dr = std::abs(glm::length(hit - spec.origin) - R);
        if (dr < best) {
          best = dr;
          axis = i;
          rotate = true;
          found = true;
        }
      }
    }
  }
  return found;
}

} // namespace

// Push a dragged occurrence's live transform straight to the renderers (no
// version bump / resync) for every render mesh of the occurrence.
void PushOccurrenceLive(AppState& state, uint64_t occId) {
  const Occurrence* occ = state.assembly->FindById(occId);
  if (!occ)
    return;
  const glm::mat4 base = DisplayMatrix(state, *occ);
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.occurrenceId != occId)
      continue;
    // A5: compose the leaf's place INSIDE the unit. Pushing the occurrence
    // matrix alone piled every part of a dragged sub-assembly onto its
    // origin mid-drag — they snapped back only because the resync on release
    // recomputed the composition.
    const glm::mat4 xf = base * m.localXf;
    if (state.meshRenderer)
      state.meshRenderer->SetInstanceTransform(m.meshId, xf);
    if (state.outlineRenderer)
      state.outlineRenderer->SetMeshTransform(m.outlineId, xf);
  }
}

namespace {

// Appends one axis's handles (arrow and/or arc) per the spec, and publishes
// each as a ViewportHandle for the shared hover + click hit test
// (docs/ui/viewport-handles.md). Per-axis ids and per-axis colour, so
// hovering X highlights X rather than the whole gizmo.
void AppendGizmoHandles(AppState& state, const GizmoSpec& spec, float L,
                        std::vector<LineVertex3D>& lines) {
  const float R = 0.78f * L, head = 0.18f * L;
  const uint64_t activeId =
      state.gizmoDrag.active
          ? HandleId(HandleOwner::Gizmo,
                     GizmoSub(state.gizmoDrag.axis, state.gizmoDrag.rotate))
          : 0;
  const auto publish = [&](int axis, bool rotate, float len) {
    ViewportHandle vh;
    vh.id = HandleId(HandleOwner::Gizmo, GizmoSub(axis, rotate));
    vh.kind = rotate ? HandleKind::Rotate : HandleKind::Linear;
    vh.origin = spec.origin;
    vh.dir = spec.axis[axis];
    vh.length = len;
    vh.color = kGizmoColor[axis];
    state.viewportHandles.push_back(vh);
    return HandleTint(vh, state.hoveredHandle, activeId);
  };
  for (int i = 0; i < 3; i++) {
    const glm::vec3 A = spec.axis[i], C = spec.origin;
    glm::vec3 col = kGizmoColor[i];
    if (spec.trans[i]) {
      col = publish(i, false, L);
      const glm::vec3 tip = C + A * L, side = PerpTo(A);
      lines.push_back(LineVertex(C, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip - A * head + side * head, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip - A * head - side * head, col));
    }
    if (spec.rot[i]) {
      col = publish(i, true, R);
      const glm::vec3 b1 = PerpTo(A), b2 = glm::cross(A, b1);
      const int seg = 40;
      glm::vec3 prev = C + b1 * R;
      for (int s = 1; s <= seg; s++) {
        const float a = (float)s / seg * glm::two_pi<float>();
        const glm::vec3 p = C + (std::cos(a) * b1 + std::sin(a) * b2) * R;
        lines.push_back(LineVertex(prev, col));
        lines.push_back(LineVertex(p, col));
        prev = p;
      }
    }
  }
}
} // namespace

void BuildGizmoOverlay(AppState& state, std::vector<LineVertex3D>& lines) {
  Occurrence* occ = GizmoOccurrence(state);
  if (!occ)
    return;
  GizmoSpec spec;
  Mate* mate = nullptr;
  if (!BuildGizmoSpec(state, occ, spec, mate))
    return;
  AppendGizmoHandles(state, spec, GizmoScale(state), lines);
}

bool HandleGizmoInput(AppState& state, const Ray& ray, bool clicked, bool down,
                      bool released) {
  auto& g = state.gizmoDrag;

  if (g.active) {
    Occurrence* occ =
        state.assembly ? state.assembly->FindById(SelectedOccurrenceId(state))
                       : nullptr;
    Mate* mate = g.mateId ? state.assembly->FindMateById(g.mateId) : nullptr;
    if (released || !down || !occ || (g.mateId && !mate)) {
      g.active = false;
      if (state.assembly) {
        if (g.explodeView)
          state.assembly->BumpVersion(); // view edit: resync, nothing to solve
        else
          SolveActiveAssembly(state); // one undo step + resync
      }
      return true;
    }
    const glm::vec3 A = g.frameAxis[g.axis];
    if (g.explodeView) {
      // Explode authoring: the drag writes the step's offset so that the
      // DISPLAYED position follows the mouse (offset scales by 1/factor).
      ExplodedView* view = state.assembly->FindExplodedViewById(g.explodeView);
      ExplodeStep* step = nullptr;
      if (view)
        for (ExplodeStep& s : view->steps)
          if (s.occurrenceId == occ->id) {
            step = &s;
            break;
          }
      if (!step) { // view or step vanished mid-drag (undo?) — abort cleanly
        g.active = false;
        return true;
      }
      const float t = RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
      const float f =
          std::max(state.explodeFactor, 0.05f); // never divide by ~0
      step->offset = g.startOffset + A * ((t - g.startParam) / f);
      PushOccurrenceLive(state, occ->id);
      return true;
    }
    if (!g.mateId) {
      // Free occurrence: drag its transform directly.
      if (!g.rotate) {
        const float t =
            RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
        occ->position = g.startPos + A * (t - g.startParam);
      } else {
        float ang;
        if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang))
          occ->orientation = glm::angleAxis(ang - g.startParam, A) * g.startRot;
      }
    } else {
      // Drag-within-mate: write the mate's DOF, then re-solve so the
      // occurrence follows.
      if (!g.rotate) {
        const float t =
            RayAxisParam(ray.origin, ray.direction, g.frameOrigin, A);
        const float v = g.startDof + (t - g.startParam);
        if (g.axis == 0)
          mate->dx = v;
        else if (g.axis == 1)
          mate->dy = v;
        else
          mate->offset = v;
      } else {
        float ang;
        if (RotateAngle(ray.origin, ray.direction, g.frameOrigin, A, ang)) {
          const float d = ang - g.startParam;
          if (mate->type == MateType::Ball)
            mate->ballRotation = glm::angleAxis(d, A) * g.startBall;
          else
            mate->rotation = g.startDof + d;
        }
      }
      SolveActiveAssembly(state);
      return true; // solve already repositioned + resynced
    }
    PushOccurrenceLive(state, occ->id);
    return true;
  }

  Occurrence* occ = GizmoOccurrence(state);
  if (!occ || !clicked)
    return false;
  GizmoSpec spec;
  Mate* mate = nullptr;
  if (!BuildGizmoSpec(state, occ, spec, mate))
    return false;
  int axis = 0;
  bool rotate = false;
  if (!GizmoHitTest(state, spec, ray, axis, rotate))
    return false;

  SnapshotAssembly(state); // one snapshot per drag
  g.active = true;
  g.axis = axis;
  g.rotate = rotate;
  g.frameOrigin = spec.origin;
  for (int i = 0; i < 3; i++)
    g.frameAxis[i] = spec.axis[i];
  g.mateId = mate ? mate->id : 0;
  g.explodeView = state.activeExplodedView;
  if (g.explodeView) {
    // Find-or-create this occurrence's step in the active view (after the
    // snapshot, so the whole drag is one undo step).
    ExplodedView* view = state.assembly->FindExplodedViewById(g.explodeView);
    ExplodeStep* step = nullptr;
    for (ExplodeStep& s : view->steps)
      if (s.occurrenceId == occ->id) {
        step = &s;
        break;
      }
    if (!step) {
      view->steps.push_back({occ->id, glm::vec3(0.0f)});
      step = &view->steps.back();
    }
    g.startOffset = step->offset;
  }
  g.startPos = occ->position;
  g.startRot = occ->orientation;
  const glm::vec3 A = spec.axis[axis];
  if (!rotate)
    g.startParam = RayAxisParam(ray.origin, ray.direction, spec.origin, A);
  else {
    float ang = 0.0f;
    RotateAngle(ray.origin, ray.direction, spec.origin, A, ang);
    g.startParam = ang;
  }
  if (mate) { // capture the DOF value at grab
    g.startDof = rotate ? mate->rotation
                        : (axis == 0   ? mate->dx
                           : axis == 1 ? mate->dy
                                       : mate->offset);
    g.startBall = mate->ballRotation;
  }
  return true; // consumed the click (don't also re-select)
}

bool AutoExplode(AppState& state, uint64_t viewId) {
  if (!state.assembly)
    return false;
  ExplodedView* view = state.assembly->FindExplodedViewById(viewId);
  if (!view)
    return false;

  // World-space center of every placed occurrence + the whole-assembly box.
  struct OccCenter {
    uint64_t id;
    glm::vec3 center;
  };
  std::vector<OccCenter> centers;
  glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
  // A5: bound each TOP-LEVEL occurrence's whole subtree. A sub-assembly
  // explodes as one unit — spacing it by the bounds of its first part would
  // send a gangway the distance a single board deserves.
  std::map<uint64_t, std::pair<glm::vec3, glm::vec3>> boxes;
  for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
    if (!inst.visible)
      continue;
    for (const auto& body : inst.part->GetBodies()) {
      glm::vec3 mn, mx;
      body.shape.GetBoundingBox(mn, mx);
      for (int c = 0; c < 8; c++) {
        const glm::vec3 corner((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y,
                               (c & 4) ? mx.z : mn.z);
        const glm::vec3 w = glm::vec3(inst.world * glm::vec4(corner, 1.0f));
        auto [it, fresh] =
            boxes.try_emplace(inst.rootOccurrenceId, std::make_pair(w, w));
        if (!fresh) {
          it->second.first = glm::min(it->second.first, w);
          it->second.second = glm::max(it->second.second, w);
        }
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
      }
    }
  }
  for (const auto& [occId, box] : boxes)
    centers.push_back({occId, (box.first + box.second) * 0.5f});
  if (centers.size() < 2)
    return false; // nothing to spread apart

  glm::vec3 centroid(0.0f);
  for (const OccCenter& c : centers)
    centroid += c.center;
  centroid /= static_cast<float>(centers.size());
  const float spread = 0.6f * glm::length(hi - lo);

  SnapshotAssembly(state);
  view->steps.clear();
  for (size_t i = 0; i < centers.size(); i++) {
    glm::vec3 dir = centers[i].center - centroid;
    if (glm::length(dir) < 1e-6f)
      dir = kGizmoAxis[i % 3]; // concentric parts: cycle world axes
    else
      dir = glm::normalize(dir);
    view->steps.push_back({centers[i].id, dir * spread});
  }
  state.assembly->BumpVersion();
  return true;
}

void ApplyExplodeTransforms(AppState& state) {
  if (!state.assembly)
    return;
  for (const Occurrence& occ : state.assembly->GetOccurrences())
    PushOccurrenceLive(state, occ.id);
}

// `SharedBody` — one part body tessellated once and shared by every
// occurrence of the part — now lives in AppActionsInternal.h, because it
// outlives this function: AppState::partRenderCache keeps it across resyncs.

namespace {

// One body's tessellation, computed on a worker thread. Pure CPU: no OCC
// document mutation beyond the shape's own triangulation, no AppState, no
// Vulkan.
struct CpuBody {
  std::vector<Vertex3D> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> triangleFaces;
  std::vector<std::vector<glm::vec3>> edges;
};

using PrewarmedParts = std::unordered_map<uint64_t, std::vector<CpuBody>>;

// Tessellates the given parts across threads, ahead of the sync loop that
// consumes them.
//
// This is the single biggest cost of first seeing a large assembly: OCC meshes
// 299 parts one after another for ~3.7 s. `BRepMesh_IncrementalMesh` already
// parallelises WITHIN one shape; nothing parallelised across shapes, and the
// parts are independent.
//
// Safety, established by measurement rather than by reading OCC's docs
// (harness in the session notes):
//   - Meshing 299 distinct products serially and across 8 threads produced
//     IDENTICAL output, vertex counts and summed positions alike.
//   - The only shared mutable state a mesher can touch is the triangulation it
//     writes into each TFace, so the hazard is two threads meshing shapes that
//     SHARE a TShape. Part documents load their geometry independently (an
//     .brep sidecar each, or features that build fresh shapes), and a scan of
//     the real 299-part import found zero TFaces shared between parts.
//   - That last point is a property of today's features, not an invariant
//     anything enforces — a future "duplicate part" would copy a CADShape,
//     which SHARES its TShape. So rather than trust it, work is GROUPED BY
//     TSHAPE below: shapes that share one are always meshed by the same
//     thread, which makes the race impossible instead of merely absent.
void PrewarmTessellations(AppState& state, const std::vector<uint64_t>& parts,
                          PrewarmedParts& out) {
  // One work item per body, keyed by the TShape whose triangulation it writes.
  struct Item {
    uint64_t partId;
    size_t bodyIndex;
    const CADShape* shape;
  };
  std::unordered_map<const void*, std::vector<Item>> byTShape;
  size_t bodies = 0;
  for (uint64_t partId : parts) {
    const Document* part = PartDocById(state, partId);
    if (part == nullptr)
      continue;
    const auto& partBodies = part->GetBodies();
    out[partId].resize(partBodies.size());
    for (size_t bi = 0; bi < partBodies.size(); bi++) {
      if (!partBodies[bi].shape.IsValid())
        continue;
      byTShape[partBodies[bi].shape.GetShape().TShape().get()].push_back(
          {partId, bi, &partBodies[bi].shape});
      bodies++;
    }
  }
  if (bodies == 0)
    return;

  std::vector<std::vector<Item>*> groups;
  groups.reserve(byTShape.size());
  for (auto& [tshape, items] : byTShape)
    groups.push_back(&items);

  const auto meshOne = [&out](const Item& item) {
    CADShape::Mesh m = item.shape->GenerateMesh(0.1);
    if (m.vertices.empty())
      return;
    CpuBody& dst = out[item.partId][item.bodyIndex];
    dst.vertices = CADMeshConverter::ConvertCADMeshToVertices(m);
    dst.indices = std::move(m.indices);
    dst.triangleFaces = std::move(m.triangleFaces);
    dst.edges = item.shape->GenerateEdgePolylines(0.1);
  };

  // `out` is pre-sized above and every item writes its own slot, so the map
  // is never rehashed and no two threads touch the same element.
  //
  // Half the cores, capped: the mesher is internally parallel, so
  // oversubscribing loses — measured 3.2x at 8 threads and only 2.3x at 16 on
  // a 22-thread machine.
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  const size_t workers =
      std::min<size_t>({groups.size(), std::max(1u, hw / 2), 8});
  if (workers <= 1) {
    for (const std::vector<Item>* g : groups)
      for (const Item& item : *g)
        meshOne(item);
    return;
  }

  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (size_t t = 0; t < workers; t++) {
    pool.emplace_back([&] {
      // OCC turns hardware signals into Standard_Failure through a handler
      // installed PER THREAD (OCC_CONVERT_SIGNALS). Without this a fault in
      // the mesher here is a raw SIGSEGV rather than a catchable failure.
      OSD::SetSignal(Standard_False);
      for (;;) {
        const size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= groups.size())
          break;
        // Every item in a group shares a TShape, so they run in sequence on
        // this one thread.
        for (const Item& item : *groups[i]) {
          try {
            meshOne(item);
          } catch (const Standard_Failure& e) {
            LOG_WARN("Tessellation failed for part {}: {}", item.partId,
                     e.GetMessageString());
          }
        }
      }
    });
  }
  for (std::thread& th : pool)
    th.join();
}

// Releases the renderer assets an entry holds. The cache is the only thing
// referencing them once the instances are gone, so dropping an entry without
// this leaks a GPU buffer for the life of the process.
void ReleaseEntryAssets(AppState& state, const PartRenderCacheEntry& entry) {
  for (const SharedBody& sb : entry.bodies) {
    if (sb.assetId != 0 && state.meshRenderer)
      state.meshRenderer->ReleaseAsset(sb.assetId);
    if (sb.outlineAssetId != 0 && state.outlineRenderer)
      state.outlineRenderer->ReleaseAsset(sb.outlineAssetId);
  }
}
} // namespace

void EvictPartRenderCache(AppState& state, uint64_t partSlotId) {
  if (!state.partRenderCache)
    return;
  auto it = state.partRenderCache->parts.find(partSlotId);
  if (it == state.partRenderCache->parts.end())
    return;
  ReleaseEntryAssets(state, it->second);
  state.partRenderCache->parts.erase(it);
}

void ReleasePartRenderCache(AppState& state) {
  if (!state.partRenderCache)
    return;
  for (const auto& [partId, entry] : state.partRenderCache->parts)
    ReleaseEntryAssets(state, entry);
  state.partRenderCache->parts.clear();
}

// Renders an assembly: one asset per referenced part body, one instance per
// occurrence. Assets are shared across occurrences of the same part.
namespace {
// The section plane in ONE occurrence's part space. A world plane
// {n, d} and a rigid placement M = [R|t] give n' = Rᵀn and d' = d - n·t:
// caps are computed on the part's own bodies (which is where its geometry
// lives) and placed by M afterwards.
bool SectionPlaneInPartSpace(const glm::vec4& world, const glm::mat4& xf,
                             glm::vec3& localOrigin, glm::vec3& localNormal) {
  const glm::vec3 n(world);
  if (glm::length(n) < 1e-6f)
    return false;
  const glm::mat3 basis(xf);
  const glm::vec3 t(xf[3]);
  localNormal = glm::normalize(glm::transpose(basis) * n);
  localOrigin = localNormal * (world.w - glm::dot(n, t));
  return true;
}
} // namespace

void SyncAssemblyRenderers(AppState& state) {
  if (!state.partRenderCache)
    state.partRenderCache = std::make_shared<PartRenderCacheStore>();
  PartRenderCacheStore& cache = *state.partRenderCache;
  cache.syncSerial++;
  // Which parts this sync actually drew. They are never evicted below — you
  // cannot drop what is on screen — and everything else ages against the
  // budget.
  std::unordered_set<uint64_t> touched;

  // One submit+wait for every asset this sync uploads, instead of one per
  // buffer. A cold 299-part build creates 1196 buffers; unbatched that is
  // 1196 queue drains. Closed at the end of the function (and defensively by
  // the renderers' Render()).
  if (state.meshRenderer)
    state.meshRenderer->BeginAssetBatch();
  if (state.outlineRenderer)
    state.outlineRenderer->BeginAssetBatch();

  // This function has been the app's worst stall twice (per-occurrence
  // outline uploads, then the un-cached rebuild), and both times the guess
  // about which half was slow was wrong. The breakdown is logged, so the next
  // regression is read rather than re-profiled.
  using SyncClock = std::chrono::steady_clock;
  const auto syncStart = SyncClock::now();
  // Device ALLOCATIONS this sync makes, not bytes — a work-volume signal the
  // phase timings above cannot give. Each buffer and image takes its own
  // vkAllocateMemory (no sub-allocator), and a cold 299-part build makes 1196
  // of them; a sync that suddenly makes ten times what it used to has stopped
  // reusing something, which shows here before it shows in the clock.
  // (No device we target imposes a limit — see VulkanCommon.h.)
  const uint64_t allocsAtStart = vkutil::TotalDeviceAllocations();
  double buildMs = 0, instanceMs = 0, pickMs = 0, prewarmMs = 0;
  size_t builtParts = 0, instanceCount = 0;
  const auto elapsedMsSince = [](SyncClock::time_point t) {
    return std::chrono::duration<double, std::milli>(SyncClock::now() - t)
        .count();
  };

  // Which parts this sync will have to tessellate, gathered BEFORE the loop
  // so they can be meshed in parallel (PrewarmTessellations). On a warm cache
  // this list is empty and the whole phase costs one walk of the tree.
  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);
  PrewarmedParts prewarmed;
  {
    std::vector<uint64_t> missing;
    std::unordered_set<uint64_t> seen;
    for (const assembly::FlatInstance& inst : flat) {
      if (!inst.visible || !seen.insert(inst.partId).second)
        continue;
      const Document* part = PartDocById(state, inst.partId);
      if (part == nullptr)
        continue;
      const auto entry = cache.parts.find(inst.partId);
      if (entry == cache.parts.end() ||
          entry->second.docVersion != part->Version())
        missing.push_back(inst.partId);
    }
    if (!missing.empty()) {
      const auto warmStart = SyncClock::now();
      PrewarmTessellations(state, missing, prewarmed);
      prewarmMs = elapsedMsSince(warmStart);
    }
  }

  // A5: the LEAF parts of the tree, placed. A top-level part occurrence is a
  // one-element subtree, so this loop is unchanged for a flat assembly — and
  // instancing survives nesting, because a FlatInstance still carries its
  // partId and the mesh-asset cache is keyed on that (six barrels in two
  // nested assemblies remain one asset).
  for (const assembly::FlatInstance& inst : flat) {
    if (!inst.visible)
      continue; // hidden here, or inside a hidden sub-assembly
    Document* part = PartDocById(state, inst.partId);
    if (!part)
      continue; // dangling reference (part deleted) — skip
    // The occurrence this leaf belongs to at the TOP level: what selection,
    // explode and picking address. A leaf inside a sub-assembly reports its
    // sub-assembly's occurrence, which is what makes it one unit.
    const Occurrence* rootOcc = state.assembly->FindById(inst.rootOccurrenceId);
    if (rootOcc == nullptr)
      continue;
    const Occurrence& occ = *rootOcc;

    touched.insert(inst.partId);
    auto it = cache.parts.find(inst.partId);
    // A hit is only a hit at the SAME document version: a regenerated part
    // must re-tessellate. Materials and appearances bump the version too, and
    // they are baked into the entry, so one key covers both.
    if (it != cache.parts.end() && it->second.docVersion != part->Version()) {
      ReleaseEntryAssets(state, it->second);
      cache.parts.erase(it);
      it = cache.parts.end();
    }
    if (it == cache.parts.end()) {
      const auto buildStart = SyncClock::now();
      builtParts++;
      // First time we see this part at this version: tessellate its bodies +
      // create assets. Everything below is what the cache exists to skip.
      PartRenderCacheEntry entry;
      entry.docVersion = part->Version();
      std::vector<SharedBody>& shared = entry.bodies;
      const auto& bodies = part->GetBodies();
      // The fallback palette is seeded from the PART, not from the order
      // parts happen to be encountered in. It used to be a counter running
      // across the whole assembly, so hiding one occurrence could renumber
      // every part after it and silently recolour unrelated geometry — and a
      // cached colour would then disagree with a freshly built one.
      size_t palette = static_cast<size_t>(inst.partId);
      const auto warm = prewarmed.find(inst.partId);
      for (size_t bodyIdx = 0; bodyIdx < bodies.size(); bodyIdx++) {
        const auto& body = bodies[bodyIdx];
        SharedBody sb;
        // Normally the tessellation is already done (PrewarmTessellations,
        // above); meshing here is the fallback for a part that was not in the
        // missing list — an in-context ghost path, say — and keeps this loop
        // correct on its own.
        CpuBody* pre = nullptr;
        if (warm != prewarmed.end() && bodyIdx < warm->second.size() &&
            !warm->second[bodyIdx].vertices.empty())
          pre = &warm->second[bodyIdx];
        if (pre != nullptr) {
          sb.vertices = std::move(pre->vertices);
          sb.indices = std::move(pre->indices);
          sb.triangleFaces = std::move(pre->triangleFaces);
          sb.edges = std::move(pre->edges);
        } else {
          CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
          if (cadMesh.vertices.empty()) {
            shared.push_back(std::move(sb)); // keep index alignment; assetId 0
            continue;
          }
          sb.vertices = CADMeshConverter::ConvertCADMeshToVertices(cadMesh);
          sb.indices = std::move(cadMesh.indices);
          sb.triangleFaces = std::move(cadMesh.triangleFaces);
          sb.edges = body.shape.GenerateEdgePolylines(0.1);
        }
        sb.palette = body.hasDisplayColor
                         ? body.displayColor
                         : kBodyPalette[palette++ % std::size(kBodyPalette)];
        sb.assetId = state.meshRenderer->CreateAsset(sb.vertices, sb.indices);
        if (state.outlineRenderer)
          sb.outlineAssetId =
              state.outlineRenderer->CreateAsset(sb.vertices, sb.indices);
        // Hold both assets for the CACHE, on top of whatever instances take.
        // The teardown at the head of the next resync removes every instance;
        // without this retain that drops the refcount to zero and destroys
        // the upload we are trying to keep.
        if (sb.assetId != 0)
          state.meshRenderer->RetainAsset(sb.assetId);
        if (sb.outlineAssetId != 0 && state.outlineRenderer)
          state.outlineRenderer->RetainAsset(sb.outlineAssetId);
        // Resolve order: appearance > material > displayColor > palette
        // (same merge as the part-tab draw loop in AppActions).
        if (!body.material.empty() || body.appearance.Any()) {
          CADMaterials::MaterialProperties mat =
              CADMaterials::MaterialLibrary::GetMaterial(
                  CADMaterials::MaterialLibrary::FromSlug(body.material));
          if (body.material.empty()) {
            mat.baseColor = sb.palette;
            mat.metallic = 0.0f;
            mat.roughness = 0.6f;
            mat.specular = 0.5f;
            mat.reflectance = 0.04f;
            mat.procPattern = 0;
          }
          if (body.appearance.color)
            mat.baseColor = *body.appearance.color;
          if (body.appearance.roughness)
            mat.roughness = *body.appearance.roughness;
          if (body.appearance.metallic)
            mat.metallic = *body.appearance.metallic;
          sb.hasMaterial = true;
          sb.albedo = mat.baseColor;
          sb.pbr = glm::vec4(mat.metallic, mat.roughness, mat.specular,
                             mat.reflectance);
          sb.procPattern = mat.procPattern;
          sb.grainScale = mat.grainScale;
          sb.grainContrast = mat.grainContrast;
        }
        shared.push_back(std::move(sb));
      }
      for (const SharedBody& sb : entry.bodies) {
        if (sb.assetId == 0)
          continue;
        // Both renderers upload the same vertices + indices, hence x2.
        entry.gpuBytes += 2 * (sb.vertices.size() * sizeof(Vertex3D) +
                               sb.indices.size() * sizeof(uint32_t));
      }
      it = cache.parts.emplace(inst.partId, std::move(entry)).first;
      buildMs += elapsedMsSince(buildStart);
    }
    it->second.lastUsed = cache.syncSerial;

    // The flattened world transform, offset by the TOP-LEVEL occurrence's
    // explode vector — exploding a sub-assembly moves the whole unit, not
    // each of its parts to its own corner.
    const glm::mat4 xf =
        glm::translate(glm::mat4(1.0f), ExplodeOffset(state, occ.id)) *
        inst.world;
    for (size_t bi = 0; bi < it->second.bodies.size(); bi++) {
      const SharedBody& sb = it->second.bodies[bi];
      if (sb.assetId == 0)
        continue; // empty body
      const auto instanceStart = SyncClock::now();
      uint32_t meshId = state.meshRenderer->AddInstance(sb.assetId, xf);
      if (sb.hasMaterial) {
        state.meshRenderer->SetInstanceMaterial(meshId, sb.albedo, sb.pbr);
        state.meshRenderer->SetInstanceProcedural(
            meshId, sb.procPattern, sb.grainScale, sb.grainContrast);
      } else {
        state.meshRenderer->SetInstanceColor(meshId, sb.palette);
      }
      uint32_t outlineId = 0;
      if (state.outlineRenderer && sb.outlineAssetId != 0) {
        // Untransformed vertices + a per-instance model matrix (A2.5): the
        // outline draws at the occurrence transform via a push constant, so a
        // live drag is a SetMeshTransform matrix update, not a re-upload — and
        // since R? the geometry itself is shared, so placing the sixth copy of
        // a bracket uploads nothing at all.
        outlineId = state.outlineRenderer->AddInstance(sb.outlineAssetId, xf);
      }
      instanceMs += elapsedMsSince(instanceStart);
      instanceCount++;
      const auto pickStart = SyncClock::now();
      if (state.faceSelector)
        state.faceSelector->AddMesh(meshId, sb.vertices, sb.indices,
                                    sb.triangleFaces, xf, sb.edges);
      pickMs += elapsedMsSince(pickStart);
      // localXf: the leaf's place inside the unit. xf = DisplayMatrix(occ) *
      // localXf by construction, which is what lets the drag path recompose
      // it per frame without a resync.
      state.documentMeshes.push_back(
          {meshId, outlineId, bi, occ.id,
           glm::inverse(OccurrenceMatrix(occ)) * inst.world, inst.partId,
           inst.innerIds, sb.edges});
    }

    // Section caps (I3). Without these an assembly section shows the inside
    // of hollow shells — the geometry is clipped by the shader but the cut
    // has no surface, which reads as "section view is broken" rather than as
    // a section. One call per LEAF instance: the plane lands somewhere
    // different in each part's own space, and a part nested two levels down
    // gets there through its composed world transform.
    const ClipPlaneArray planes = SectionPlanes(state);
    std::vector<glm::vec3> capColors;
    for (size_t i = 0; i < planes.size(); i++) {
      glm::vec3 localOrigin, localNormal;
      if (!SectionPlaneInPartSpace(planes[i], xf, localOrigin, localNormal))
        continue;
      if (capColors.empty()) {
        capColors.reserve(it->second.bodies.size());
        for (const SharedBody& sb : it->second.bodies)
          capColors.push_back(sb.hasMaterial ? sb.albedo : sb.palette);
      }
      // The other sections, also in this part's space, trim this cap.
      std::vector<glm::vec4> trim;
      for (size_t j = 0; j < planes.size(); j++) {
        glm::vec3 to, tn;
        if (j != i && SectionPlaneInPartSpace(planes[j], xf, to, tn))
          trim.push_back(glm::vec4(tn, glm::dot(to, tn)));
      }
      AppendCapMeshes(state,
                      Measure::SectionCaps(part->GetBodies(), localOrigin,
                                           localNormal, trim),
                      capColors, xf, glm::normalize(glm::vec3(planes[i])));
    }
  }
  // Land every upload this sync recorded before anything reads the assets.
  if (state.meshRenderer)
    state.meshRenderer->EndAssetBatch();
  if (state.outlineRenderer)
    state.outlineRenderer->EndAssetBatch();

  // Age out what is not on screen, down to kPartRenderCacheBudgetBytes.
  //
  // This was once a strict "release anything not drawn this sync", which
  // looked tidy and made two assembly tabs THRASH: alternating between two
  // 299-part assemblies re-tessellated all 299 every switch (4.2 s each), so
  // the cache bought exactly nothing in a multi-assembly project. Keeping
  // everything instead grows without bound across imports. Hence a budget,
  // evicting least-recently-drawn first — and never a part drawn this sync,
  // whose assets are in use.
  size_t idleBytes = 0;
  std::vector<std::pair<uint64_t, uint64_t>> idle; // (lastUsed, partId)
  for (const auto& [partId, entry] : cache.parts) {
    if (touched.count(partId) != 0)
      continue;
    idleBytes += entry.gpuBytes;
    idle.emplace_back(entry.lastUsed, partId);
  }
  if (idleBytes > kPartRenderCacheBudgetBytes) {
    std::sort(idle.begin(), idle.end()); // oldest first
    size_t dropped = 0;
    for (const auto& [lastUsed, partId] : idle) {
      if (idleBytes <= kPartRenderCacheBudgetBytes)
        break;
      auto it = cache.parts.find(partId);
      if (it == cache.parts.end())
        continue;
      idleBytes -= it->second.gpuBytes;
      dropped += it->second.gpuBytes;
      ReleaseEntryAssets(state, it->second);
      cache.parts.erase(it);
    }
    LOG_INFO("Part render cache over budget: dropped {} MB of idle geometry",
             dropped / (1024 * 1024));
  }

  // (Highlights re-apply themselves: they are a per-frame projection of the
  // Selection now — see SyncSelectionHighlights.)
  LOG_INFO("Assembly synced: {} occurrences, {} instances, {} parts cached "
           "({} tessellated) in {:.0f} ms [build {:.0f}, instances {:.0f}, "
           "picking {:.0f}, of which parallel tessellation {:.0f}], "
           "{} device allocations ({} total)",
           state.assembly->OccurrenceCount(), instanceCount, cache.parts.size(),
           builtParts, elapsedMsSince(syncStart), buildMs, instanceMs, pickMs,
           prewarmMs, vkutil::TotalDeviceAllocations() - allocsAtStart,
           vkutil::TotalDeviceAllocations());
}

// In-context editing (A4): appends the context assembly's other occurrences
// as GHOST instances around the active part — alpha-blended, unpickable, no
// outlines. The part stays at identity; each ghost renders at
// OccMatrix(edited)⁻¹ · OccMatrix(other), so every spatial relationship reads
// the same as in the assembly with zero changes to part tooling. Before
// placing ghosts the context re-solves if any referenced part regenerated
// (this runs inside the version-gated resync, so a part edit updates the
// ghosts in the same frame — mates track live).
void SyncInContextGhosts(AppState& state) {
  if (!state.inContext.assemblySlotId)
    return;
  PartSlot* asmSlot = nullptr;
  for (PartSlot& s : state.parts)
    if (s.kind == DocKind::Assembly && s.id == state.inContext.assemblySlotId)
      asmSlot = &s;
  if (!asmSlot || !asmSlot->assembly)
    return; // the context assembly is gone; the mode quietly shows nothing
  AssemblyDocument& asmDoc = *asmSlot->assembly;
  const assembly::PartResolver resolve =
      [&state](uint64_t partId) -> const Document* {
    return PartDocById(state, partId);
  };

  // Stale check against the stashed slot's stamps (the active-tab machinery
  // in SolveActiveAssembly/AssemblyPartsStale can't see a stashed assembly).
  bool stale = false;
  for (const Occurrence& occ : asmDoc.GetOccurrences()) {
    const Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue;
    const auto it = asmSlot->solvedPartVersions.find(occ.partId);
    if (it == asmSlot->solvedPartVersions.end() ||
        it->second != part->Version()) {
      stale = true;
      break;
    }
  }
  if (stale) {
    assembly::Solve(asmDoc, resolve, AssemblyResolverFor(state));
    asmDoc.BumpVersion();
    asmSlot->solvedPartVersions.clear();
    for (const Occurrence& occ : asmDoc.GetOccurrences())
      if (const Document* part = PartDocById(state, occ.partId))
        asmSlot->solvedPartVersions[occ.partId] = part->Version();
  }

  const Occurrence* edited = asmDoc.FindById(state.inContext.occurrenceId);
  if (!edited)
    return;
  const glm::mat4 inv = glm::inverse(OccurrenceMatrix(*edited));

  // One shared asset per context part body (as in SyncAssemblyRenderers),
  // minus everything a ghost doesn't need: outlines, picking, materials.
  const glm::vec3 kGhostTint(0.55f, 0.60f, 0.68f);
  std::unordered_map<uint64_t, std::vector<uint32_t>> byPart; // partId->assets
  // A5: ghost the whole tree. Editing INTO a sub-assembly is still a non-goal
  // (open its own tab), but the CONTEXT around the part you are editing has
  // to look like the assembly does — a gangway that vanished while you worked
  // would be worse than no context at all.
  auto flat = assembly::Flatten(asmDoc, PartResolverFor(state),
                                AssemblyResolverFor(state));
  if (!flat)
    return; // a cycle: the resync that draws the real thing already said so
  for (const assembly::FlatInstance& inst : *flat) {
    if (!inst.visible || inst.rootOccurrenceId == edited->id)
      continue;
    Document* part = PartDocById(state, inst.partId);
    if (!part)
      continue;
    auto it = byPart.find(inst.partId);
    if (it == byPart.end()) {
      std::vector<uint32_t> assets;
      for (const auto& body : part->GetBodies()) {
        CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
        if (cadMesh.vertices.empty()) {
          assets.push_back(0); // keep index alignment
          continue;
        }
        assets.push_back(state.meshRenderer->CreateAsset(
            CADMeshConverter::ConvertCADMeshToVertices(cadMesh),
            cadMesh.indices));
      }
      it = byPart.emplace(inst.partId, std::move(assets)).first;
    }
    const glm::mat4 xf = inv * inst.world;
    for (size_t bi = 0; bi < it->second.size(); bi++) {
      if (it->second[bi] == 0)
        continue; // empty body
      const uint32_t meshId =
          state.meshRenderer->AddInstance(it->second[bi], xf);
      state.meshRenderer->SetInstanceColor(meshId, kGhostTint);
      state.meshRenderer->SetInstanceGhost(meshId); // after color: sets alpha
      // Ghosts are never dragged, so their localXf is unused; keep it
      // honest anyway rather than leaving a lie in the record.
      state.documentMeshes.push_back(
          {meshId, /*outlineId=*/0, bi, inst.rootOccurrenceId, inst.world, {}});
    }
  }
}

namespace {
// A body's export appearance: its material's PBR, or a palette color (cycled
// by `paletteIndex`) with a neutral-plastic default when unassigned — the
// same rule the renderer uses, so an export looks like the viewport.
void FillItemAppearance(const Body& body, size_t paletteIndex,
                        MeshExport::Item& item) {
  if (!body.material.empty()) {
    const auto& mat = CADMaterials::MaterialLibrary::GetMaterial(
        CADMaterials::MaterialLibrary::FromSlug(body.material));
    item.baseColor = mat.baseColor;
    item.metallic = mat.metallic;
    item.roughness = mat.roughness;
  } else {
    item.baseColor = kBodyPalette[paletteIndex % std::size(kBodyPalette)];
    item.metallic = 0.0f;
    item.roughness = 0.5f;
  }
}
} // namespace

std::vector<MeshExport::Item> BuildVisualExportItems(AppState& state) {
  std::vector<MeshExport::Item> items;
  size_t palette = 0;
  if (state.assembly) {
    // One item per (occurrence × body) at the occurrence's SOLVED placement
    // (never exploded — an export is the real assembly). Occurrences of one
    // part reuse the part's shapes, so the shared TShapes let the writers
    // instance the geometry rather than copy it.
    for (const assembly::FlatInstance& inst : FlattenActiveAssembly(state)) {
      if (!inst.visible)
        continue;
      const auto& bodies = inst.part->GetBodies();
      for (size_t bi = 0; bi < bodies.size(); bi++) {
        if (!bodies[bi].shape.IsValid())
          continue;
        MeshExport::Item item;
        item.shape = bodies[bi].shape;
        // The tree path names the item ("Gangway/Board 03"), which is what a
        // glTF/STEP consumer needs to tell two identical boards apart.
        const std::string label =
            inst.path.empty() ? ("occ" + std::to_string(inst.rootOccurrenceId))
                              : inst.path;
        item.name = bodies.size() > 1 ? label + "-" + bodies[bi].name : label;
        item.transform = inst.world; // solved, not exploded
        FillItemAppearance(bodies[bi], palette++, item);
        items.push_back(std::move(item));
      }
    }
  } else if (state.document) {
    const auto& bodies = state.document->GetBodies();
    for (size_t bi = 0; bi < bodies.size(); bi++) {
      if (!bodies[bi].shape.IsValid())
        continue;
      MeshExport::Item item;
      item.shape = bodies[bi].shape;
      item.name = bodies[bi].name;
      FillItemAppearance(bodies[bi], palette++, item);
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

uint64_t AddFastener(AppState& state, const std::string& standard,
                     const std::string& size, double length,
                     const std::string& drive, const glm::vec3& position,
                     const glm::quat& orientation, std::string* error,
                     uint64_t ridesOn) {
  const auto fail = [&](const std::string& why) -> uint64_t {
    if (error)
      *error = why;
    return 0;
  };
  if (state.assembly == nullptr)
    return fail("a fastener is a bought part placed in an ASSEMBLY, not "
                "geometry in a part — on a part tab it would be machined by "
                "CAM, counted as the part's own mass, and reported by "
                "check_geometry as a disconnected body. Switch to an assembly "
                "tab (set_active_part) and place it there.");

  // Built once, off to the side, purely to name the spec and to let the table
  // refuse an unstocked one BEFORE a document exists for it. A refusal that
  // arrives after a tab has been created leaves the tab behind.
  FastenerFeature probe("", standard, size, length);
  probe.drive = drive;
  {
    Document scratch;
    auto copy = std::make_unique<FastenerFeature>(probe);
    scratch.AddFeature(std::move(copy));
    if (scratch.GetBodies().empty())
      return fail(scratch.GetFeature(0)->GetError());
  }
  const std::string designation = probe.Designation();

  // One part per spec: an existing one with this designation is THE part, and
  // every occurrence of it aggregates in the BOM for free.
  uint64_t partId = PartIdByName(state, designation);
  if (partId == 0) {
    const int assemblyTab = state.activePart;
    AddPart(state, designation); // switches tabs, like the importer's
    PartSlot& slot = state.parts[static_cast<size_t>(state.activePart)];
    slot.folder = kStandardPartsFolder;
    CreateFolder(state, kStandardPartsFolder);
    partId = slot.id;
    auto feature = std::make_unique<FastenerFeature>(probe);
    feature->SetName(designation);
    state.document->AddFeature(std::move(feature));
    // Back to the assembly the caller was on: creating the part is a side
    // effect of placing one, and a tab switch the caller did not ask for is
    // the kind of thing that reads as a bug.
    if (assemblyTab >= 0 && assemblyTab < static_cast<int>(state.parts.size()))
      SetActivePart(state, static_cast<size_t>(assemblyTab));
  }
  if (state.assembly == nullptr)
    return fail("could not return to the assembly after creating " +
                designation);

  Occurrence occ;
  occ.partId = partId;
  occ.name = designation;
  occ.position = position;
  occ.orientation = orientation;
  // Attached to the part it bears on, so it travels with that part and cannot
  // be dragged out of its hole on its own (docs/parts/f1-loose-ends.md). The
  // offset is whatever the two poses already imply, expressed in the parent's
  // frame — quaternion arithmetic rather than a matrix round trip, which is
  // exact for a rigid placement and needs no decomposition.
  if (const Occurrence* parent =
          ridesOn != 0 ? state.assembly->FindById(ridesOn) : nullptr) {
    const glm::quat inv = glm::inverse(parent->orientation);
    occ.attachedTo = ridesOn;
    occ.attachOffset = inv * (position - parent->position);
    occ.attachRotation = inv * orientation;
  }
  return state.assembly->AddOccurrence(occ);
}

namespace {
// +Z (a fastener's own reach direction) rotated onto `to`.
//
// Written out rather than pulled from glm's experimental quaternion header,
// which this file does not otherwise need and which errors unless a global
// GLM_ENABLE_EXPERIMENTAL is defined.
glm::quat AlignZTo(const glm::vec3& to) {
  const glm::vec3 from(0.0f, 0.0f, 1.0f);
  const float dot = glm::dot(from, to);
  if (dot < -0.999999f)
    return glm::quat(0.0f, 1.0f, 0.0f, 0.0f); // 180 degrees about X
  if (dot > 0.999999f)
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  const glm::vec3 c = glm::cross(from, to);
  return glm::normalize(glm::quat(1.0f + dot, c.x, c.y, c.z));
}

// One dimension of a companion part (a washer's thickness), refusing by name
// when the standard is not the kind the caller asked it to be. A "washer"
// that turns out to be a nut would place silently and be found by eye.
std::optional<double> CompanionDim(const std::string& standard,
                                   parts::Kind wanted, const std::string& size,
                                   std::initializer_list<const char*> columns,
                                   std::string* why) {
  const parts::Profile* profile = parts::ProfileFor(standard);
  if (profile == nullptr) {
    *why = "\"" + standard +
           "\" is not a standard this build makes — it makes " +
           parts::KnownStandards();
    return std::nullopt;
  }
  if (profile->kind != wanted) {
    *why = profile->designation + " is a " + profile->what + ", not a " +
           (wanted == parts::Kind::Nut ? "nut" : "washer");
    return std::nullopt;
  }
  const auto dims = parts::Dimensions(standard, size, why);
  if (!dims)
    return std::nullopt;
  for (const char* column : columns) {
    const auto it = dims->find(column);
    if (it != dims->end())
      return it->second;
  }
  *why = profile->designation + " " + size + " has no thickness to stack";
  return std::nullopt;
}
} // namespace

FastenerFit FitFastenerToHole(AppState& state, const RimPick& rim,
                              const FastenerRequest& request) {
  FastenerFit fit;
  const auto fail = [&](const std::string& why) -> FastenerFit {
    fit.ok = false;
    fit.why = why;
    return fit;
  };
  if (state.assembly == nullptr)
    return fail("a fastener goes in an ASSEMBLY — switch to the assembly tab "
                "and pick the rim of the hole it passes through");

  const parts::Profile* screwProfile = parts::ProfileFor(request.standard);
  if (screwProfile == nullptr)
    return fail("\"" + request.standard +
                "\" is not a standard this build makes — it makes " +
                parts::KnownStandards());
  if (screwProfile->kind != parts::Kind::Screw)
    return fail(screwProfile->designation + " is a " + screwProfile->what +
                ", which does not pass through a hole — ask for a screw and "
                "put this on it with `nut` or `washer`");

  // Every placed leaf, once: the rim is on one of them and the stack is made
  // of the rest.
  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);
  std::vector<parts::PlacedBody> placed;
  // EVERY instance the element resolves on, not the first. An ElementRef is
  // unique within a PART; in an assembly it names one rim per placement of
  // that part, so collecting the candidates is what makes the ambiguity
  // visible instead of silently resolved.
  struct RimOn {
    const assembly::FlatInstance* inst;
    glm::vec3 centre;
    glm::vec3 axis;
    double radius;
  };
  std::vector<RimOn> candidates;
  for (const assembly::FlatInstance& inst : flat) {
    if (inst.part == nullptr || !inst.visible)
      continue;
    for (const Body& body : inst.part->GetBodies()) {
      parts::PlacedBody pb;
      pb.body = &body;
      pb.world = inst.world;
      pb.part = inst.path.empty() ? body.name : inst.path;
      pb.partId = inst.partId;
      pb.occurrence = inst.rootOccurrenceId;
      placed.push_back(pb);
    }
    // The rim, in the frame the assembly puts it in. Described in the part's
    // own space and then placed, because that is where the ElementRef means
    // something.
    const std::optional<Measure::Element> element =
        Measure::Describe(inst.part->GetBodies(), rim.element);
    if (!element || element->kind != "edge" || element->shape != "circle" ||
        !element->hasRadius || !element->hasAxis)
      continue;
    const std::optional<glm::vec3> local =
        Measure::CentreOf(inst.part->GetBodies(), rim.element);
    if (!local)
      continue;
    candidates.push_back(
        {&inst, glm::vec3(inst.world * glm::vec4(*local, 1.0f)),
         glm::normalize(glm::vec3(inst.world * glm::vec4(element->axis, 0.0f))),
         element->radius});
  }
  if (candidates.empty())
    return fail("pick the RIM of the hole — a circular edge, the one the head "
                "sits against. A cylindrical face has an axis but no "
                "direction, so it cannot say which end the fastener goes in "
                "from; the rim says both.");

  const RimOn* chosen = nullptr;
  if (rim.occurrence != 0) {
    for (const RimOn& c : candidates)
      if (c.inst->rootOccurrenceId == rim.occurrence)
        chosen = &c;
    if (chosen == nullptr) {
      std::string where;
      for (const RimOn& c : candidates)
        where += (where.empty() ? "" : ", ") + c.inst->path + " (" +
                 std::to_string(c.inst->rootOccurrenceId) + ")";
      return fail("occurrence " + std::to_string(rim.occurrence) +
                  " does not carry that rim — it is on " + where);
    }
  } else if (candidates.size() == 1) {
    chosen = &candidates.front();
  } else {
    // Refused, not guessed — and the ambiguity is wider than it first looks.
    // An element name is unique within a DOCUMENT, so it is shared not only
    // by every placement of one part but by every part built the same way:
    // two plates drawn with the same three features carry the same feature
    // ids and the same mapped names. Taking the first put the fastener at the
    // near end of a two-plate joint, which reads as correct in the viewport.
    std::string where;
    for (const RimOn& c : candidates)
      where += (where.empty() ? "" : ", ") + c.inst->path + " (occurrence " +
               std::to_string(c.inst->rootOccurrenceId) + ")";
    return fail("that element is on " + std::to_string(candidates.size()) +
                " placements — " + where +
                ". An element name is unique within a PART, so every "
                "placement of it and every part built the same way carry the "
                "same one; say which occurrence you mean. A pick in the "
                "viewport carries it, a name on its own does not.");
  }
  glm::vec3 centre = chosen->centre;
  glm::vec3 axis = chosen->axis;
  const double radius = chosen->radius;

  // Which size the hole is for. ASK IT FIRST: a hole that records its fit
  // states the answer outright, and reading a diameter backwards out of the
  // clearance table only guesses at it — a guess the dock round showed is
  // weak, since 48% of whole millimetres from 1 to 160 are some size's
  // clearance and a Ø32 ladder rung reads as an M30
  // (docs/modeling/hole-standards.md).
  std::string why;
  const HoleFeature* drilled =
      drawing::FindHoleForRef(*chosen->inst->part, rim.element.name, radius);
  bool tapped = false;
  if (drilled != nullptr && !drilled->fitThread.empty()) {
    fit.size = drilled->fitThread;
    fit.clearance = drilled->fitKind;
    tapped = drilled->fitKind == "tapped";
  } else {
    const auto thread = parts::ThreadForClearance(radius * 2.0, &why);
    if (!thread)
      return fail(why);
    fit.size = thread->first;
    fit.clearance = thread->second;
  }

  // A countersunk screw sinks its head INTO the part, so it needs a
  // countersunk hole and the rim picked has to be the COUNTERSINK's, not the
  // bore's. This was a blanket refusal until holes could describe themselves;
  // the three questions below are the ones that refusal was standing in for
  // (docs/parts/f1-loose-ends.md).
  if (screwProfile->lengthOverHead) {
    if (drilled == nullptr || drilled->kind != HoleKind::Countersink)
      return fail(screwProfile->designation +
                  " sinks its head into the part, so it needs a COUNTERSUNK "
                  "hole — this one is not, and the head would end up inside "
                  "solid material. Countersink the hole, or use a screw whose "
                  "head stands proud (iso4762, iso14579, iso7380, iso4017).");
    if (std::abs(radius * 2.0 - drilled->csDiameter) > 1e-3)
      return fail("pick the rim of the COUNTERSINK, not the bore — the head "
                  "sits in the cone, so " +
                  Mm(drilled->csDiameter) +
                  " across is the edge it bears on and " +
                  Mm(drilled->diameter) + " is the hole under it.");
    // Deep enough for the head? A countersink shallower than the head leaves
    // it standing proud of a surface that was machined to swallow it, which
    // is not something you can see at render distance.
    const auto dims = parts::Dimensions(screwProfile->stem, fit.size, &why);
    if (!dims)
      return fail(why);
    const auto csh = dims->find("csh_height");
    if (csh != dims->end() && drilled->csDepth < csh->second - 1e-6)
      return fail("the countersink is " + Mm(drilled->csDepth) +
                  " deep and an " + fit.size + " " + screwProfile->designation +
                  " head is " + Mm(csh->second) +
                  " — the head would stand proud of a surface cut to take it. "
                  "Deepen the countersink, or use a smaller screw.");
    if (request.washerUnderHead)
      return fail("a washer does not go under a countersunk head — the head "
                  "bears on the cone, not on the surface.");
  }

  // Down the axis, from the rim. The direction is chosen by which way there is
  // material to pass through: an axis has no sign, and picking the wrong one
  // would send the screw out into space.
  const double reach = 10000.0;
  std::vector<parts::StackSegment> stack = parts::TraverseAxis(
      placed, glm::dvec3(centre), glm::dvec3(-axis), radius, reach);
  if (stack.empty()) {
    axis = -axis; // an axis has no sign; take the way with material in it
    stack = parts::TraverseAxis(placed, glm::dvec3(centre), glm::dvec3(-axis),
                                radius, reach);
  }
  if (stack.empty())
    return fail("nothing lies along that hole's axis — the rim was picked on "
                "a surface feature rather than on a joint, so there is no "
                "stack for a fastener to span");

  // The decomposition. Gaps are SPANNED and named rather than silently added:
  // a gap is either an unmodelled gasket or two parts that were meant to touch
  // and do not, and the geometry cannot tell those apart. Saying so is what
  // makes the second kind visible; contact_report reports it independently.
  std::ostringstream summary;
  summary.precision(4);
  double previousExit = 0.0;
  bool first = true;
  for (const parts::StackSegment& seg : stack) {
    if (!first && seg.enter - previousExit > 1e-3)
      summary << ", gap " << (seg.enter - previousExit);
    summary << (first ? "" : ", ") << seg.part << " " << seg.thickness();
    previousExit = seg.exit;
    first = false;
  }
  // The bundle above measured PLATE THICKNESS. What stops the screw is a
  // different question, and needs the on-axis ray the bundle deliberately does
  // not cast: anything solid ON the axis is either a blind hole's floor or a
  // plate whose hole does not line up, and telling those apart is the
  // difference between an M6x8 and a bolt through solid metal.
  const std::vector<parts::StackSegment> onAxis = parts::TraverseAxis(
      placed, glm::dvec3(centre), glm::dvec3(-axis), 0.0, reach);
  fit.blind = !onAxis.empty();

  // What else is on the axis, and how thick. A washer under the head pushes
  // the head out by its own thickness, so it lengthens the screw — deriving
  // the length and THEN adding hardware is how a joint ends up one washer
  // short, which looks perfectly right in a viewport.
  double underHead = 0.0, underNut = 0.0, nutHeight = 0.0;
  if (request.washerUnderHead) {
    const auto h = CompanionDim(request.washer, parts::Kind::Washer, fit.size,
                                {"h", "h_max"}, &why);
    if (!h)
      return fail(why);
    underHead = *h;
  }
  if (!request.nut.empty()) {
    if (fit.blind)
      return fail("there is no far side to put a nut on — the axis stops "
                  "inside " +
                  stack.back().part +
                  ", which is a blind hole. Tap it and leave the nut off.");
    const auto m =
        CompanionDim(request.nut, parts::Kind::Nut, fit.size, {"m"}, &why);
    if (!m)
      return fail(why);
    nutHeight = *m;
    if (request.washerUnderNut) {
      const auto h = CompanionDim(request.washer, parts::Kind::Washer, fit.size,
                                  {"h", "h_max"}, &why);
      if (!h)
        return fail(why);
      underNut = *h;
    }
  } else if (request.washerUnderNut) {
    return fail("a washer under the nut needs a nut — ask for one (nut: "
                "\"iso4032\"), or drop the washer.");
  }

  std::optional<double> length;
  if (!fit.blind) {
    // Clear all the way: a through joint, and the screw spans the lot — the
    // plates, the washers at both ends, and enough thread to fill the nut.
    // "Enough" is the nut's own height, which is the shortest length at which
    // every one of its threads is engaged; a nut hanging off the last two is
    // the failure this rule exists to prevent.
    fit.grip = underHead + LastExit(stack) + underNut + nutHeight;
    length = parts::SnapLength(request.standard, fit.size, fit.grip, &why);
  } else {
    const double blocked = onAxis.front().enter;
    // Material ON the axis, right at the rim: this is not a hole at all. It
    // is the end of a SOLID — a boss, a pin, a shaft — whose end cap has a
    // circular edge like any bore does.
    //
    // Found by dogfooding the dock (docs/dogfooding.md): every circular edge
    // in a 49-part assembly was one of these, and the refusal a person got
    // was about plates and reaching past things, which never once said "that
    // is a solid". A circular edge is not a hole, and on a real model it
    // usually is not.
    if (blocked < 1e-6)
      return fail("that circular edge is the end of a SOLID, not a hole — the "
                  "axis runs straight into " +
                  onAxis.front().part +
                  " from the rim. A boss, a pin and a bore all have a circle "
                  "at the end; pick the rim of one a fastener can pass "
                  "through.");
    // Which plate the obstruction is in. The LAST one means a blind hole —
    // the screw threads into it. An EARLIER one means the axis runs into
    // material it should have passed through, which is a misalignment and a
    // refusal, because the alternative is a bolt drawn through solid metal.
    const parts::StackSegment* blocking = nullptr;
    for (const parts::StackSegment& seg : stack)
      if (blocked >= seg.enter - 1e-6 && blocked <= seg.exit + 1e-6)
        blocking = &seg;
    if (blocking == nullptr)
      return fail("something solid sits on this hole's axis " + Mm(blocked) +
                  " out, clear of every plate the hole passes through — a "
                  "fastener cannot reach past it");

    // Blocked at a part's ENTRY FACE means that part has no hole on this axis
    // at all, whether or not it is the last in the stack. That is a
    // misalignment, and it is worth separating from a blind hole: told "the
    // hole is 0 mm deep" a person goes looking for a drilling depth, when what
    // is wrong is that two holes do not line up.
    const double into = blocked - blocking->enter;
    if (into < 1e-3)
      return fail("there is no hole on this axis in " + blocking->part +
                  " — the axis meets its face " + Mm(blocked) +
                  " out and stops. Its hole is not lined up with this one, so "
                  "a fastener here would pass through solid material.");
    if (blocking->partId != stack.back().partId)
      return fail("the axis runs into " + blocking->part + " " + Mm(into) +
                  " in — its hole does not go all the way through, so a "
                  "fastener cannot reach the parts beyond it.");

    // A blind hole bounds the length from ABOVE: the screw must not bottom
    // out. Take the longest stocked length that fits, which is also the most
    // thread engaged.
    // A blind hole a screw can bite in is a TAPPED one. A clearance hole of
    // the same depth looks identical and holds nothing — the plan promised
    // this refusal and could not make it until a hole could say which it is.
    if (drilled != nullptr && !drilled->fitThread.empty() && !tapped)
      return fail("the blind hole in " + blocking->part + " is a " +
                  fit.clearance + " CLEARANCE hole for " + fit.size +
                  ", not a tapped one — a machine screw has nothing to bite "
                  "in it. Tap it, or put a nut on the far side.");
    const double engagement = *parts::NominalDiameter(fit.size); // ~1x d
    fit.grip = underHead + blocked;
    if (into < engagement)
      return fail("the hole is only " + Mm(into) + " deep in " +
                  blocking->part + ", which is less than the " +
                  Mm(engagement) + " of thread an " + fit.size +
                  " needs to hold. Drill deeper, or use a smaller size.");
    length = parts::SnapLengthDown(request.standard, fit.size, fit.grip, &why);
  }
  if (!length)
    return fail(why + " (the stack is " + summary.str() + ")");
  fit.length = *length;

  std::ostringstream head;
  head.precision(4);
  head << fit.size << "x" << fit.length << " — grip " << fit.grip << ": ";
  if (underHead > 0.0)
    head << "washer " << underHead << ", ";
  head << summary.str();
  if (underNut > 0.0)
    head << ", washer " << underNut;
  if (nutHeight > 0.0)
    head << ", nut " << nutHeight;
  fit.summary = head.str();

  // WHERE each piece goes, worked out whether or not anything is placed.
  // Every fastener is built with its bearing face on z = 0 and its body
  // below, so all four of these are the same two questions: where is the
  // face, and which way is "away from the material".
  const glm::vec3 into = -axis;          // down the hole
  const glm::quat down = AlignZTo(into); // head end: body points back out
  const glm::quat up = AlignZTo(axis); // far end: body points out the far side
  const glm::vec3 exit = centre + into * static_cast<float>(LastExit(stack));

  // Which occurrence each piece rides on. The head end bears on the part the
  // rim was picked in; the far end on whatever the axis leaves last.
  const uint64_t headOn = chosen->inst->rootOccurrenceId;
  uint64_t farOn = headOn;
  double farthest = 0.0;
  for (const parts::StackSegment& seg : stack)
    if (seg.exit > farthest) {
      farthest = seg.exit;
      farOn = seg.occurrence;
    }

  fit.plan.push_back({request.standard, fit.size, fit.length,
                      centre - into * static_cast<float>(underHead), down,
                      headOn});
  if (underHead > 0.0)
    fit.plan.push_back({request.washer, fit.size, 0.0, centre, down, headOn});
  if (underNut > 0.0)
    fit.plan.push_back({request.washer, fit.size, 0.0, exit, up, farOn});
  if (!request.nut.empty())
    fit.plan.push_back({request.nut, fit.size, 0.0,
                        exit + axis * static_cast<float>(-underNut), up,
                        farOn});

  if (request.dryRun) {
    fit.ok = true;
    return fit;
  }

  // The commit just walks the plan. It used to recompute the same arithmetic
  // a second time, which is how a preview and a placement come to disagree.
  for (size_t i = 0; i < fit.plan.size(); i++) {
    const PlannedPart& part = fit.plan[i];
    // The drive is the SCREW's — it is first — because a washer has no recess
    // and handing it one is refused by the standard that does not have it.
    const uint64_t id =
        AddFastener(state, part.standard, part.size, part.length,
                    i == 0 ? request.drive : std::string{}, part.position,
                    part.orientation, &fit.why, part.ridesOn);
    if (id == 0)
      return fail(fit.why);
    fit.placed.push_back(id);
    for (const Occurrence& o : state.assembly->GetOccurrences())
      if (o.id == id)
        fit.parts.push_back(o.name);
  }
  fit.occurrence = fit.placed.front();

  fit.ok = true;
  return fit;
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

} // namespace ui
