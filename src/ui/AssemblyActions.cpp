#include "ui/AppActions.h"

#include "ui/FeatureEditor.h" // PickSlot, kMateSessionSlot (U7c mate slots)

#include "cad/CADKernel.h" // ImportCADFile structure probe
#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ImportAssembly.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"
#include "ui/DragMath.h"

#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

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
  const assembly::SolveReport rep = assembly::Solve(*state.assembly, resolve);
  state.assembly->BumpVersion(); // solved transforms changed → renderer resync
  // Stamp the referenced parts' versions (A4): AssemblyPartsStale compares
  // against these, so a later part edit re-solves on tab activation. Stamped
  // at solve (not render sync) so propagation also works headless.
  state.solvedPartVersions.clear();
  for (const Occurrence& occ : state.assembly->GetOccurrences())
    if (const Document* part = PartDocById(state, occ.partId))
      state.solvedPartVersions[occ.partId] = part->Version();
  return rep.erroredMates;
}

bool AssemblyPartsStale(AppState& state) {
  if (!state.assembly)
    return false;
  for (const Occurrence& occ : state.assembly->GetOccurrences()) {
    const Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue; // dangling reference — nothing to compare (solve won't fix it)
    const auto it = state.solvedPartVersions.find(occ.partId);
    if (it == state.solvedPartVersions.end() || it->second != part->Version())
      return true; // never stamped, or the part regenerated since the stamp
  }
  return false;
}

BomTable ComputeBom(AppState& state) {
  BomTable table;
  if (!state.assembly)
    return table;
  for (const Occurrence& occ : state.assembly->GetOccurrences()) {
    if (state.assembly->OccurrenceSuppressed(occ.id))
      continue; // suppressed by the active config: out of the BOM
    BomRow* row = nullptr;
    for (BomRow& r : table.rows)
      if (r.partId == occ.partId) {
        row = &r;
        break;
      }
    if (row) {
      row->quantity++;
      continue;
    }
    BomRow fresh;
    fresh.partId = occ.partId;
    fresh.quantity = 1;
    for (const PartSlot& slot : state.parts)
      if (slot.kind == DocKind::Part && slot.id == occ.partId)
        fresh.partName = slot.name;
    const Document* part = PartDocById(state, occ.partId);
    if (!part) {
      fresh.partName = "(missing part)"; // dangling reference — mass unknown
      table.warnings.push_back("occurrence '" + occ.name +
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
  std::string csv =
      "part,quantity,material,unit_volume_mm3,unit_mass_g,total_mass_g\n";
  char buf[64];
  auto num = [&buf](double v) {
    snprintf(buf, sizeof buf, "%.6g", v);
    return std::string(buf);
  };
  for (const BomRow& r : bom.rows) {
    csv += field(r.partName) + "," + std::to_string(r.quantity) + "," +
           field(r.material) + "," + num(r.unitVolume) + ",";
    if (r.massKnown)
      csv += num(r.unitMassG) + "," + num(r.unitMassG * r.quantity);
    else
      csv += ",";
    csv += "\n";
  }
  csv += "total,,,,,";
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
  Document* part = PartDocById(state, occ->partId);
  if (!part || rm->bodyIndex >= part->GetBodies().size())
    return false;

  // The face's persistent toponaming name → an ElementRef the mate stores.
  const Body& body = part->GetBodies()[rm->bodyIndex];
  const std::string* name =
      body.elementMap.NameOf(static_cast<int>(hit.faceIndex));
  if (!name)
    return false; // an unnamed face (e.g. a pattern output) can't be mated

  out.occurrenceId = rm->occurrenceId;
  out.face = ElementRef{body.createdBy, *name};
  out.snapEdge = ElementRef{};
  out.snapType = AssemblyFacePick::SnapType::FaceCenter;

  // Every named boundary edge contributes its characteristic point: a circle
  // center or another edge's midpoint. The face center competes as a candidate
  // too, so a click in the middle of a face does not always snap to an edge.
  std::vector<int> edgeIndices;
  body.shape.FaceEdgeIndices(static_cast<int>(hit.faceIndex), edgeIndices);
  std::vector<glm::vec3> circleCenters;
  std::vector<int> circleEdges;
  body.shape.FaceCircleCenters(static_cast<int>(hit.faceIndex), circleCenters,
                               circleEdges);
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
  for (const int edgeIndex : edgeIndices) {
    const std::string* edgeName = body.elementMap.EdgeNameOf(edgeIndex);
    glm::vec3 localPoint;
    if (!edgeName || !body.shape.EdgeSnapPoint(edgeIndex, localPoint))
      continue;
    const glm::vec3 worldPoint = glm::vec3(xf * glm::vec4(localPoint, 1.0f));
    const float distance = glm::length(worldPoint - hit.point);
    if (distance >= best)
      continue;
    bool isCircular = false;
    for (const int circleEdge : circleEdges)
      if (circleEdge == edgeIndex) {
        isCircular = true;
        break;
      }
    best = distance;
    out.snapEdge = ElementRef{body.createdBy, *edgeName};
    out.snapType = isCircular ? AssemblyFacePick::SnapType::HoleCenter
                              : AssemblyFacePick::SnapType::EdgeMidpoint;
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
                      float scale, bool edgeSnap, bool midpoint) {
  const glm::vec3 origin(f[3]);
  const glm::vec3 color = midpoint   ? glm::vec3(0.30f, 0.85f, 1.0f)
                          : edgeSnap ? glm::vec3(1.0f, 0.78f, 0.22f)
                                     : glm::vec3(0.82f, 0.82f, 0.60f);
  const glm::vec3 u = glm::normalize(glm::vec3(f[0])) * scale;
  const glm::vec3 v = glm::normalize(glm::vec3(f[1])) * scale;
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
                       state.mateTool.hoverSnapIsMidpoint);
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

// Push a dragged occurrence's live transform straight to the renderers (no
// version bump / resync) for every render mesh of the occurrence.
void PushOccurrenceLive(AppState& state, uint64_t occId) {
  const Occurrence* occ = state.assembly->FindById(occId);
  if (!occ)
    return;
  const glm::mat4 xf = DisplayMatrix(state, *occ);
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.occurrenceId != occId)
      continue;
    if (state.meshRenderer)
      state.meshRenderer->SetInstanceTransform(m.meshId, xf);
    if (state.outlineRenderer)
      state.outlineRenderer->SetMeshTransform(m.outlineId, xf);
  }
}

// Appends one axis's handles (arrow and/or arc) per the spec.
void AppendGizmoHandles(const GizmoSpec& spec, float L,
                        std::vector<LineVertex3D>& lines) {
  const float R = 0.78f * L, head = 0.18f * L;
  for (int i = 0; i < 3; i++) {
    const glm::vec3 A = spec.axis[i], col = kGizmoColor[i], C = spec.origin;
    if (spec.trans[i]) {
      const glm::vec3 tip = C + A * L, side = PerpTo(A);
      lines.push_back(LineVertex(C, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip - A * head + side * head, col));
      lines.push_back(LineVertex(tip, col));
      lines.push_back(LineVertex(tip - A * head - side * head, col));
    }
    if (spec.rot[i]) {
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
  AppendGizmoHandles(spec, GizmoScale(state), lines);
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
  for (const Occurrence& occ : state.assembly->GetOccurrences()) {
    Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue;
    const glm::mat4 xf = OccurrenceMatrix(occ);
    glm::vec3 bmin(FLT_MAX), bmax(-FLT_MAX);
    bool any = false;
    for (const auto& body : part->GetBodies()) {
      glm::vec3 mn, mx;
      body.shape.GetBoundingBox(mn, mx);
      for (int c = 0; c < 8; c++) {
        const glm::vec3 corner((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y,
                               (c & 4) ? mx.z : mn.z);
        const glm::vec3 w = glm::vec3(xf * glm::vec4(corner, 1.0f));
        bmin = glm::min(bmin, w);
        bmax = glm::max(bmax, w);
        any = true;
      }
    }
    if (!any)
      continue;
    centers.push_back({occ.id, (bmin + bmax) * 0.5f});
    lo = glm::min(lo, bmin);
    hi = glm::max(hi, bmax);
  }
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

namespace {
// One part body, tessellated once and cached so N occurrences of the same part
// share a single GPU asset (the instancing lever). CPU copies are kept for the
// outline + face-selector, whose geometry sharing is a later optimization.
struct SharedBody {
  uint32_t assetId = 0;
  std::vector<Vertex3D> vertices;
  std::vector<uint32_t> indices;
  std::vector<uint32_t> triangleFaces;
  std::vector<std::vector<glm::vec3>> edges;
  glm::vec3 palette;
  bool hasMaterial = false;
  glm::vec3 albedo;
  glm::vec4 pbr;
  // Procedural wood/plywood grain: pattern, grainScale, grainContrast.
  int procPattern = 0;
  float grainScale = 0.0f;
  float grainContrast = 0.0f;
};
} // namespace

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
  std::unordered_map<uint64_t, std::vector<SharedBody>>
      byPart; // partId -> bodies
  size_t palette = 0;

  for (const Occurrence& occ : state.assembly->GetOccurrences()) {
    if (!occ.visible || state.assembly->OccurrenceSuppressed(occ.id))
      continue; // hidden (or suppressed by the active config): not rendered
    Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue; // dangling reference (part deleted) — skip

    auto it = byPart.find(occ.partId);
    if (it == byPart.end()) {
      // First time we see this part: tessellate its bodies + create assets.
      std::vector<SharedBody> shared;
      const auto& bodies = part->GetBodies();
      for (const auto& body : bodies) {
        SharedBody sb;
        CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
        if (cadMesh.vertices.empty()) {
          shared.push_back(std::move(sb)); // keep index alignment; assetId 0
          continue;
        }
        sb.vertices = CADMeshConverter::ConvertCADMeshToVertices(cadMesh);
        sb.indices = cadMesh.indices;
        sb.triangleFaces = cadMesh.triangleFaces;
        sb.edges = body.shape.GenerateEdgePolylines(0.1);
        sb.palette = body.hasDisplayColor
                         ? body.displayColor
                         : kBodyPalette[palette++ % std::size(kBodyPalette)];
        sb.assetId = state.meshRenderer->CreateAsset(sb.vertices, sb.indices);
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
      it = byPart.emplace(occ.partId, std::move(shared)).first;
    }

    const glm::mat4 xf = DisplayMatrix(state, occ); // solved ∘ explode offset
    for (size_t bi = 0; bi < it->second.size(); bi++) {
      const SharedBody& sb = it->second[bi];
      if (sb.assetId == 0)
        continue; // empty body
      uint32_t meshId = state.meshRenderer->AddInstance(sb.assetId, xf);
      if (sb.hasMaterial) {
        state.meshRenderer->SetInstanceMaterial(meshId, sb.albedo, sb.pbr);
        state.meshRenderer->SetInstanceProcedural(
            meshId, sb.procPattern, sb.grainScale, sb.grainContrast);
      } else {
        state.meshRenderer->SetInstanceColor(meshId, sb.palette);
      }
      uint32_t outlineId = 0;
      if (state.outlineRenderer) {
        // Untransformed vertices + a per-mesh model matrix (A2.5): the outline
        // draws at the occurrence transform via a push constant, so a live drag
        // is a SetMeshTransform matrix update, not a vertex re-upload/resync.
        outlineId = state.outlineRenderer->AddMesh(sb.vertices, sb.indices);
        state.outlineRenderer->SetMeshTransform(outlineId, xf);
      }
      if (state.faceSelector)
        state.faceSelector->AddMesh(meshId, sb.vertices, sb.indices,
                                    sb.triangleFaces, xf, sb.edges);
      state.documentMeshes.push_back({meshId, outlineId, bi, occ.id, sb.edges});
    }

    // Section caps (I3). Without these an assembly section shows the inside
    // of hollow shells — the geometry is clipped by the shader but the cut
    // has no surface, which reads as "section view is broken" rather than as
    // a section. One call per occurrence: the plane lands somewhere different
    // in each part's own space.
    const ClipPlaneArray planes = SectionPlanes(state);
    std::vector<glm::vec3> capColors;
    for (size_t i = 0; i < planes.size(); i++) {
      glm::vec3 localOrigin, localNormal;
      if (!SectionPlaneInPartSpace(planes[i], xf, localOrigin, localNormal))
        continue;
      if (capColors.empty()) {
        capColors.reserve(it->second.size());
        for (const SharedBody& sb : it->second)
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
  // (Highlights re-apply themselves: they are a per-frame projection of the
  // Selection now — see SyncSelectionHighlights.)
  LOG_INFO("Assembly synced: {} occurrences",
           state.assembly->OccurrenceCount());
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
    assembly::Solve(asmDoc, resolve);
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
  for (const Occurrence& occ : asmDoc.GetOccurrences()) {
    if (!occ.visible || occ.id == edited->id)
      continue;
    Document* part = PartDocById(state, occ.partId);
    if (!part)
      continue;
    auto it = byPart.find(occ.partId);
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
      it = byPart.emplace(occ.partId, std::move(assets)).first;
    }
    const glm::mat4 xf = inv * OccurrenceMatrix(occ);
    for (size_t bi = 0; bi < it->second.size(); bi++) {
      if (it->second[bi] == 0)
        continue; // empty body
      const uint32_t meshId =
          state.meshRenderer->AddInstance(it->second[bi], xf);
      state.meshRenderer->SetInstanceColor(meshId, kGhostTint);
      state.meshRenderer->SetInstanceGhost(meshId); // after color: sets alpha
      state.documentMeshes.push_back({meshId, /*outlineId=*/0, bi, occ.id, {}});
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
    for (const Occurrence& occ : state.assembly->GetOccurrences()) {
      if (!occ.visible || state.assembly->OccurrenceSuppressed(occ.id))
        continue;
      Document* part = PartDocById(state, occ.partId);
      if (!part)
        continue;
      const glm::mat4 xf = OccurrenceMatrix(occ); // solved, not exploded
      const auto& bodies = part->GetBodies();
      for (size_t bi = 0; bi < bodies.size(); bi++) {
        if (!bodies[bi].shape.IsValid())
          continue;
        MeshExport::Item item;
        item.shape = bodies[bi].shape;
        const std::string label =
            occ.name.empty() ? ("occ" + std::to_string(occ.id)) : occ.name;
        item.name = bodies.size() > 1 ? label + "-" + bodies[bi].name : label;
        item.transform = xf;
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
      interchange::BuildAssemblyImportPlan(stepPath, destDir.string());
  if (!plan.error.empty())
    return fail(plan.error);

  // Materialize: one part tab per product (an ImportFeature on the extracted
  // sidecar), then the assembly with every occurrence placed. Deliberately
  // not undoable — it creates documents, like File > Open. The parts land in
  // a tab-bar folder named after the assembly so a 100-part import is one
  // flyout, not 100 tabs.
  std::vector<uint64_t> partIds;
  for (const interchange::PlannedPart& p : plan.parts) {
    AddPart(state, p.name);
    PartSlot& slot = state.parts[static_cast<size_t>(state.activePart)];
    slot.folder = asmName;
    CreateFolder(state, asmName); // register (idempotent) so saves know it
    auto feature = std::make_unique<ImportFeature>(p.name, p.brepPath);
    feature->hasColor = p.hasColor;
    feature->color = p.color;
    state.document->AddFeature(std::move(feature));
    partIds.push_back(slot.id);
  }

  AddAssembly(state, asmName);
  bool first = true;
  for (const interchange::PlannedOccurrence& o : plan.occurrences) {
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
  LOG_INFO("Imported '{}' as assembly: {} parts, {} occurrences", stepPath,
           plan.parts.size(), plan.occurrences.size());
  return true;
}

bool ImportCADFile(AppState& state, const std::string& path,
                   std::string* error) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".step" || ext == ".stp") {
    // Probe the product structure: several placed instances is a real
    // assembly → parts + occurrences; a lone product reads better as a
    // feature in the active part. (The probe re-parses the file; STEP reads
    // are cheap next to the extraction that follows.)
    CADKernel::ImportedModel model;
    if (CADKernel::Instance().LoadSTEPStructured(path, model) &&
        model.instances.size() > 1)
      return ImportStepAsAssembly(state, path, error);
  }
  return ImportAsFeature(state, path, error);
}

} // namespace ui
