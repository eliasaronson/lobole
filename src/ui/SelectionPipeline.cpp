#include "ui/SelectionPipeline.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "MeshRenderer.h"
#include "ui/AppActions.h"        // EdgeElementRef, AppendPrimLine
#include "ui/SketchInteraction.h" // FaceElementRef

#include "imgui.h"

#include <algorithm>
#include <optional>

namespace ui {

namespace {

constexpr uint32_t KindBit(SelectionItem::Kind k) {
  return 1u << static_cast<int>(k);
}
constexpr uint32_t kDefaultKinds = KindBit(SelectionItem::Kind::Edge) |
                                   KindBit(SelectionItem::Kind::Face) |
                                   KindBit(SelectionItem::Kind::Body);

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

// A ranked candidate: the stable item plus the render handles it resolved
// from (armed consumers like the blend remap anchor on those).
struct Candidate {
  SelectionItem item;
  uint32_t meshId = 0;
  uint32_t index = 0;
};

// Candidates under the cursor, best first, restricted to `kinds`. Edges
// outrank the face they bound (PickEdge's angular tolerance only fires near
// an edge, and it already rejects occluded ones); the face's body comes last
// so same-spot cycling reaches it (edge → face → body).
std::vector<Candidate> Candidates(AppState& state, const Ray& ray,
                                  uint32_t kinds) {
  std::vector<Candidate> out;
  if (kinds & KindBit(SelectionItem::Kind::Edge)) {
    const EdgeHit edge = state.faceSelector->PickEdge(ray);
    if (edge.hit) {
      if (auto ref = EdgeElementRef(state, edge.meshId, edge.edgeIndex))
        out.push_back({{SelectionItem::Kind::Edge, 0, *ref}, edge.meshId,
                       edge.edgeIndex});
    }
  }
  if (kinds & (KindBit(SelectionItem::Kind::Face) |
               KindBit(SelectionItem::Kind::Body))) {
    const RayHit face = state.faceSelector->PickFace(ray);
    if (face.hit) {
      if (kinds & KindBit(SelectionItem::Kind::Face)) {
        if (auto ref = FaceElementRef(state, face.meshId, face.faceIndex))
          out.push_back({{SelectionItem::Kind::Face, 0, *ref}, face.meshId,
                         face.faceIndex});
      }
      if (kinds & KindBit(SelectionItem::Kind::Body)) {
        if (const Body* body = BodyOfMesh(state, face.meshId))
          out.push_back(
              {{SelectionItem::Kind::Body, body->createdBy, {}}, 0, 0});
      }
    }
  }
  return out;
}

// The armed pick context, derived fresh each frame from the tools' own state
// — never stored, so there is no second source of truth for "what's armed".
// Priority mirrors the old ViewportInput chain: blend edges, measure, sim.
struct ArmedContext {
  uint32_t kinds = 0;
  const char* prompt = nullptr;
  void (*consume)(AppState&, const Candidate&) = nullptr;
};

void ConsumeBlendEdge(AppState& state, const Candidate& c) {
  BlendEdgeClicked(state, c.item.element, c.meshId, c.index);
}
void ConsumeMeasure(AppState& state, const Candidate& c) {
  MeasureElementClicked(state, c.item.element,
                        c.item.kind == SelectionItem::Kind::Edge);
}
void ConsumeSimFace(AppState& state, const Candidate& c) {
  SimulationApplyToFace(state, c.item.element);
}

std::optional<ArmedContext> ActiveArmedContext(AppState& state) {
  if (state.edgePick.Active()) {
    return ArmedContext{KindBit(SelectionItem::Kind::Edge),
                        "Click edges to add or remove them", ConsumeBlendEdge};
  }
  if (state.measure.active) {
    return ArmedContext{KindBit(SelectionItem::Kind::Face) |
                            KindBit(SelectionItem::Kind::Edge),
                        "Measure: pick a face or edge (a second for "
                        "distance) - Esc exits",
                        ConsumeMeasure};
  }
  if (state.simulation.IsActive() &&
      state.simulation.tool != SimulationState::Tool::None) {
    const char* prompt =
        state.simulation.tool == SimulationState::Tool::Keep
            ? "Pick a face to keep solid - Esc cancels"
            : "Pick a face for the boundary condition - Esc cancels";
    return ArmedContext{KindBit(SelectionItem::Kind::Face), prompt,
                        ConsumeSimFace};
  }
  return std::nullopt;
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

const char* ArmedPrompt(AppState& state) {
  const auto armed = ActiveArmedContext(state);
  return armed ? armed->prompt : nullptr;
}

void HandleSelectionInput(AppState& state, bool overViewport,
                          bool allowEscapeClear) {
  if (!state.faceSelector || !state.meshRenderer || !state.document ||
      !state.camera) {
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  const std::optional<ArmedContext> armed = ActiveArmedContext(state);

  // Escape clears the selection — only when nothing armed consumed it.
  if (!armed && allowEscapeClear && !io.WantTextInput &&
      ImGui::IsKeyPressed(ImGuiKey_Escape) && !state.selection.Empty()) {
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
  const std::vector<Candidate> candidates =
      Candidates(state, ray, armed ? armed->kinds : kDefaultKinds);

  // Hover preview: what a click would take (the top candidate). While a
  // blend collects edges, mirror the edge hover into its own state — the
  // blend overlay and the element-name label read it.
  ClearHoverPreviews(state);
  if (!candidates.empty()) {
    const Candidate& top = candidates.front();
    if (top.item.kind == SelectionItem::Kind::Edge) {
      state.pick.edgeHover = true;
      state.pick.edgeHoverMesh = top.meshId;
      state.pick.edgeHoverIndex = top.index;
      if (state.edgePick.Active()) {
        state.edgePick.hasHover = true;
        state.edgePick.hoverMeshId = top.meshId;
        state.edgePick.hoverEdgeIndex = top.index;
      }
    } else if (top.item.kind == SelectionItem::Kind::Face) {
      uint32_t first = 0, count = 0;
      state.faceSelector->SetHoveredFace(top.meshId, top.index);
      if (state.faceSelector->GetFaceVertexRange(top.meshId, top.index, first,
                                                 count))
        state.meshRenderer->SetFaceHover(top.meshId, first, count);
    }
  }

  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    return;

  // An armed tool consumes the pick; the Selection itself is untouched.
  if (armed) {
    if (!candidates.empty())
      armed->consume(state, candidates.front());
    return;
  }

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
    state.selection.Toggle(candidates.front().item); // multi-select
    state.pick.cycle = 0;
  } else {
    // Same-spot repeat cycles the stacked candidates (edge → face → body,
    // face behind face …); a fresh spot takes the best one.
    state.pick.cycle = samePos
                           ? (state.pick.cycle + 1) %
                                 static_cast<int>(candidates.size())
                           : 0;
    state.selection.Replace(
        candidates[static_cast<size_t>(state.pick.cycle)].item);
  }

  // Shift+click: also align the camera to the picked face (the pre-U1 Ctrl
  // gesture; Ctrl now means multi-select).
  if (io.KeyShift) {
    for (const Candidate& c : candidates) {
      if (c.item.kind == SelectionItem::Kind::Face) {
        state.camera->AlignToFace(
            state.faceSelector->GetFaceCenter(c.meshId, c.index),
            state.faceSelector->GetFaceNormal(c.meshId, c.index));
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
