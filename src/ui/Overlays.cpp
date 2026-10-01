#include "ui/Overlays.h"

#include "cad/FaceSelector.h"
#include "core/Feature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/FrameFeature.h"
#include "ui/AppActionsInternal.h" // DatumPlanesVisible
#include "ui/SceneOverlays.h"
#include "ui/SketchInteraction.h" // WorldToScreen / SketchUVToScreen
#include "ui/SketchOverlay.h"     // EdgeElementRef

#include "imgui_internal.h" // FindWindowByName (the dock host's draw list)

#include "imgui.h"

#include <algorithm>

namespace ui {

ImDrawList* ViewportDrawList() {
  // "##DockHost" is begun by DrawDockLayout and hosts the DockSpace, so it
  // spans the whole main viewport and is the bottom-most window.
  if (ImGuiWindow* host = ImGui::FindWindowByName("##DockHost"))
    if (host->DrawList != nullptr)
      return host->DrawList;
  return ImGui::GetForegroundDrawList();
}

void DrawDatumLabels(AppState& state) {
  if (!state.document || !DatumPlanesVisible(state))
    return;
  ImDrawList* dl = ViewportDrawList();
  const ImU32 col = IM_COL32(230, 180, 90, 230);
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    Feature* f = state.document->GetFeature(i);
    if (auto* dp = dynamic_cast<DatumPlaneFeature*>(f)) {
      // Label near a corner of the 3-unit quad.
      glm::vec2 s =
          SketchUVToScreen(state, dp->OutputPlane(), glm::dvec2(2.6, 2.6));
      dl->AddText(ImVec2(s.x, s.y), col, dp->GetName().c_str());
    } else if (auto* da = dynamic_cast<DatumAxisFeature*>(f)) {
      const glm::vec3 end = da->Origin() + da->Direction() * 3.0;
      glm::vec2 s = WorldToScreen(state, end);
      dl->AddText(ImVec2(s.x, s.y), col, da->GetName().c_str());
    } else if (auto* fr = dynamic_cast<FrameFeature*>(f)) {
      if (!fr->GetError().empty())
        continue;
      const glm::vec3 end =
          fr->Origin() + fr->ZAxis() * FrameTriadLength(state);
      glm::vec2 s = WorldToScreen(state, end);
      dl->AddText(ImVec2(s.x, s.y), col, fr->GetName().c_str());
    }
  }
}

glm::vec4 PromptPillRect(const AppState& state, float y, glm::vec2 textSize) {
  const float padX = 10.0f, padY = 5.0f;
  // Centred on the viewport — but on the part of it the floating tool window
  // is not sitting in. The window anchors top-right of the same viewport and
  // the pill is drawn at the same height, so "Measure: pick a face or edge (a
  // second for distance) - Esc exits" was printed straight across the tool
  // window's first row, in every screenshot anyone ever took of the tool.
  // The rect is a frame old (PreviewFeature records it after drawing), which
  // is fine: it moves when the viewport does, and so does the pill.
  float right = state.viewportPos.x + state.viewportSize.x;
  const glm::vec4& tw = state.toolWindowRect;
  const bool toolWindowOpen = tw.z > tw.x && tw.w > tw.y;
  if (toolWindowOpen && y < tw.w && y + textSize.y + 2.0f * padY > tw.y)
    right = std::min(right, tw.x - 8.0f);
  // Never push it off the left edge: a cramped pill that overlaps slightly
  // beats one whose text starts outside the viewport.
  const float left = state.viewportPos.x;
  const float cx =
      std::max(left + textSize.x * 0.5f + padX, (left + right) * 0.5f);
  return glm::vec4(cx - textSize.x * 0.5f - padX, y,
                   cx + textSize.x * 0.5f + padX, y + textSize.y + 2.0f * padY);
}

void DrawPromptPill(AppState& state, float& y, const std::string& text,
                    PromptPill style) {
  if (text.empty())
    return;
  ImU32 bg = 0, fg = 0;
  switch (style) {
  case PromptPill::StatusGood:
    bg = IM_COL32(24, 46, 32, 230);
    fg = IM_COL32(140, 235, 165, 255);
    break;
  case PromptPill::Status:
    bg = IM_COL32(26, 36, 52, 230);
    fg = IM_COL32(150, 200, 255, 255);
    break;
  case PromptPill::Prompt:
    bg = IM_COL32(28, 32, 40, 225);
    fg = IM_COL32(226, 232, 240, 255);
    break;
  case PromptPill::Warning:
    bg = IM_COL32(60, 44, 24, 235);
    fg = IM_COL32(255, 205, 120, 255);
    break;
  }
  ImDrawList* dl = ViewportDrawList();
  const ImVec2 sz = ImGui::CalcTextSize(text.c_str());
  const glm::vec4 r = PromptPillRect(state, y, glm::vec2(sz.x, sz.y));
  const ImVec2 a(r.x, r.y), b(r.z, r.w);
  const float padX = 10.0f, padY = 5.0f;
  const float cx = (r.x + r.z) * 0.5f;
  dl->AddRectFilled(a, b, bg, 5.0f);
  dl->AddText(ImVec2(cx - sz.x * 0.5f, y + padY), fg, text.c_str());
  y = b.y + 4.0f;
}

void DrawElementNameOverlay(AppState& state) {
  if (!state.showElementNames || !state.document || !state.faceSelector)
    return;
  // While an edge is hovered, label it instead of a face.
  if (state.pick.edgeHover) {
    const auto ref = EdgeElementRef(state, state.pick.edgeHoverMesh,
                                    state.pick.edgeHoverIndex);
    const char* text = ref ? ref->name.c_str() : "(unnamed)";
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* dl = ViewportDrawList();
    const ImVec2 sz = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(mouse.x + 12 - 3, mouse.y + 12 - 2),
                      ImVec2(mouse.x + 12 + sz.x + 3, mouse.y + 12 + sz.y + 2),
                      IM_COL32(20, 24, 30, 220), 3.0f);
    dl->AddText(ImVec2(mouse.x + 12, mouse.y + 12),
                IM_COL32(120, 230, 170, 255), text);
    return;
  }
  // Label the hovered face (live feedback), falling back to the selected one.
  const Face* face = state.faceSelector->GetHoveredFace();
  if (face == nullptr)
    face = state.faceSelector->GetSelectedFace();
  if (face == nullptr)
    return;

  // Resolve the mesh back to its Body, then that face's element name.
  const auto& meshes = state.documentMeshes;
  const auto it = std::find_if(
      meshes.begin(), meshes.end(),
      [&](const DocumentRenderMesh& m) { return m.meshId == face->meshId; });
  if (it == meshes.end() || it->bodyIndex >= state.document->GetBodies().size())
    return;
  const Body& body = state.document->GetBodies()[it->bodyIndex];
  const std::string* name =
      body.elementMap.NameOf(static_cast<int>(face->faceIndex));

  ImDrawList* dl = ViewportDrawList();
  const glm::vec3 center =
      state.faceSelector->GetFaceCenter(face->meshId, face->faceIndex);
  const glm::vec2 s = WorldToScreen(state, center);
  const char* text = name != nullptr ? name->c_str() : "(unnamed)";
  const ImVec2 sz = ImGui::CalcTextSize(text);
  dl->AddRectFilled(ImVec2(s.x - sz.x * 0.5f - 3, s.y - 2),
                    ImVec2(s.x + sz.x * 0.5f + 3, s.y + sz.y + 2),
                    IM_COL32(20, 24, 30, 220), 3.0f);
  dl->AddText(ImVec2(s.x - sz.x * 0.5f, s.y), IM_COL32(120, 230, 170, 255),
              text);
}

} // namespace ui
