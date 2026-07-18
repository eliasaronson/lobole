#include "ui/SelectionPipeline.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "MeshRenderer.h"
#include "ui/AppActions.h"        // EdgeElementRef, AppendPrimLine
#include "ui/SketchInteraction.h" // FaceElementRef

#include "imgui.h"

#include <algorithm>

namespace ui {

namespace {

// The body a render mesh mirrors, or null (assembly meshes, stale index).
const Body* BodyOfMesh(const AppState& state, uint32_t meshId) {
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.meshId == meshId && state.document &&
        m.bodyIndex < state.document->GetBodies().size()) {
      return &state.document->GetBodies()[m.bodyIndex];
    }
  }
  return nullptr;
}

// Candidates under the cursor, best first. Edges outrank the face they bound
// (PickEdge's angular tolerance only fires near an edge, and it already
// rejects occluded edges); the face's body comes last so same-spot cycling
// reaches it (face → body, or edge → face → body).
std::vector<SelectionItem> Candidates(AppState& state, const Ray& ray) {
  std::vector<SelectionItem> out;
  const EdgeHit edge = state.faceSelector->PickEdge(ray);
  if (edge.hit) {
    if (auto ref = EdgeElementRef(state, edge.meshId, edge.edgeIndex))
      out.push_back({SelectionItem::Kind::Edge, 0, *ref});
  }
  const RayHit face = state.faceSelector->PickFace(ray);
  if (face.hit) {
    if (auto ref = FaceElementRef(state, face.meshId, face.faceIndex))
      out.push_back({SelectionItem::Kind::Face, 0, *ref});
    if (const Body* body = BodyOfMesh(state, face.meshId))
      out.push_back({SelectionItem::Kind::Body, body->createdBy, {}});
  }
  return out;
}

void ClearHoverPreviews(AppState& state) {
  state.pick.edgeHover = false;
  if (state.faceSelector)
    state.faceSelector->ClearHover();
  if (state.meshRenderer)
    state.meshRenderer->ClearFaceHover();
}

} // namespace

bool ResolveFaceItem(const AppState& state, const SelectionItem& item,
                     uint32_t& meshId, uint32_t& faceIndex) {
  if (item.kind != SelectionItem::Kind::Face || !state.document)
    return false;
  const auto& bodies = state.document->GetBodies();
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.bodyIndex >= bodies.size())
      continue;
    const Body& body = bodies[m.bodyIndex];
    if (body.createdBy != item.element.featureId)
      continue;
    if (const auto idx = body.elementMap.Resolve(item.element.name)) {
      meshId = m.meshId;
      faceIndex = static_cast<uint32_t>(*idx);
      return true;
    }
  }
  return false;
}

bool ResolveEdgeItem(const AppState& state, const SelectionItem& item,
                     uint32_t& meshId, uint32_t& edgeIndex) {
  if (item.kind != SelectionItem::Kind::Edge || !state.document)
    return false;
  const auto& bodies = state.document->GetBodies();
  for (const DocumentRenderMesh& m : state.documentMeshes) {
    if (m.bodyIndex >= bodies.size())
      continue;
    const Body& body = bodies[m.bodyIndex];
    if (body.createdBy != item.element.featureId)
      continue;
    if (const auto idx = body.elementMap.ResolveEdge(item.element.name)) {
      meshId = m.meshId;
      edgeIndex = static_cast<uint32_t>(*idx);
      return true;
    }
  }
  return false;
}

void HandleSelectionInput(AppState& state, bool overViewport) {
  if (!state.faceSelector || !state.meshRenderer || !state.document ||
      !state.camera) {
    return;
  }
  ImGuiIO& io = ImGui::GetIO();

  // Escape clears (the armed tools each consumed Escape before reaching us).
  if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape) &&
      !state.selection.Empty()) {
    state.selection.Clear();
  }

  if (!overViewport || state.viewportDrag != ViewportDrag::None) {
    ClearHoverPreviews(state);
    return;
  }

  const Ray ray = state.faceSelector->ScreenToWorldRay(
      io.MousePos.x - state.viewportPos.x, io.MousePos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
  const std::vector<SelectionItem> candidates = Candidates(state, ray);

  // Hover preview: what a plain click would take (the top candidate).
  ClearHoverPreviews(state);
  if (!candidates.empty()) {
    uint32_t mesh = 0, index = 0;
    const SelectionItem& top = candidates.front();
    if (top.kind == SelectionItem::Kind::Edge &&
        ResolveEdgeItem(state, top, mesh, index)) {
      state.pick.edgeHover = true;
      state.pick.edgeHoverMesh = mesh;
      state.pick.edgeHoverIndex = index;
    } else if (ResolveFaceItem(state, top, mesh, index)) {
      uint32_t first = 0, count = 0;
      state.faceSelector->SetHoveredFace(mesh, index);
      if (state.faceSelector->GetFaceVertexRange(mesh, index, first, count))
        state.meshRenderer->SetFaceHover(mesh, first, count);
    }
  }

  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    return;

  const glm::vec2 pos(io.MousePos.x, io.MousePos.y);
  const bool samePos = glm::length(pos - state.pick.lastClickPos) < 6.0f;
  state.pick.lastClickPos = pos;

  if (candidates.empty()) {
    if (!io.KeyCtrl)
      state.selection.Clear(); // click empty space = deselect
    state.pick.cycle = 0;
    return;
  }

  if (io.KeyCtrl) {
    state.selection.Toggle(candidates.front()); // multi-select
    state.pick.cycle = 0;
  } else {
    // Same-spot repeat cycles the stacked candidates (edge → face → body,
    // face behind face …); a fresh spot takes the best one.
    state.pick.cycle = samePos
                           ? (state.pick.cycle + 1) %
                                 static_cast<int>(candidates.size())
                           : 0;
    state.selection.Replace(candidates[static_cast<size_t>(state.pick.cycle)]);
  }

  // Shift+click: also align the camera to the picked face (the pre-U1 Ctrl
  // gesture; Ctrl now means multi-select).
  if (io.KeyShift) {
    for (const SelectionItem& it : candidates) {
      uint32_t mesh = 0, index = 0;
      if (ResolveFaceItem(state, it, mesh, index)) {
        state.camera->AlignToFace(
            state.faceSelector->GetFaceCenter(mesh, index),
            state.faceSelector->GetFaceNormal(mesh, index));
        break;
      }
    }
  }
}

void SyncSelectionHighlights(AppState& state) {
  if (!state.faceSelector || !state.meshRenderer)
    return;
  // Single face-highlight slot: the most recent Face item shows.
  const SelectionItem* lastFace = nullptr;
  for (const SelectionItem& it : state.selection.items)
    if (it.kind == SelectionItem::Kind::Face)
      lastFace = &it;
  uint32_t mesh = 0, index = 0, first = 0, count = 0;
  if (lastFace != nullptr && ResolveFaceItem(state, *lastFace, mesh, index) &&
      state.faceSelector->GetFaceVertexRange(mesh, index, first, count)) {
    state.faceSelector->SetSelectedFace(mesh, index);
    state.meshRenderer->SetFaceHighlight(mesh, first, count);
  } else {
    state.faceSelector->ClearSelection();
    state.meshRenderer->ClearFaceHighlight();
  }
}

void AppendSelectionPrimitives(AppState& state, std::vector<PrimVertex>& out) {
  if (!state.document || state.edgePick.Active() ||
      state.sketchEdit.IsEditing()) {
    return; // blend edge-pick / sketch mode own the overlay channel
  }
  const glm::vec4 kHover{1.00f, 0.85f, 0.30f, 1.0f};
  const glm::vec4 kSelected{0.35f, 0.80f, 1.00f, 1.0f};

  const auto drawEdge = [&](uint32_t meshId, uint32_t edgeIndex,
                            const glm::vec4& color) {
    for (const DocumentRenderMesh& m : state.documentMeshes) {
      if (m.meshId != meshId)
        continue;
      if (edgeIndex >= m.edgePolylines.size())
        return;
      const auto& poly = m.edgePolylines[edgeIndex];
      for (size_t i = 0; i + 1 < poly.size(); i++)
        AppendPrimLine(poly[i], poly[i + 1], 2.6f, color, out);
      return;
    }
  };

  for (const SelectionItem& it : state.selection.items) {
    uint32_t mesh = 0, index = 0;
    if (it.kind == SelectionItem::Kind::Edge &&
        ResolveEdgeItem(state, it, mesh, index)) {
      drawEdge(mesh, index, kSelected);
    }
  }
  if (state.pick.edgeHover)
    drawEdge(state.pick.edgeHoverMesh, state.pick.edgeHoverIndex, kHover);
}

} // namespace ui
