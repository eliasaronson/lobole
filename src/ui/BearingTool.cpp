// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/BearingTool.h"

#include "cad/CADMeshConverter.h"
#include "core/Document.h"
#include "core/features/BearingFeature.h"
#include "render/MeshRenderer.h"
#include "ui/BearingActions.h"
#include "ui/FeatureEditor.h"  // PickSlot, kBearingToolSlot
#include "ui/PreviewFeature.h" // BeginToolWindowFrame

#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

namespace ui {

void ArmBearingSeat(AppState& state) {
  state.slotPick = {kBearingToolSlot, static_cast<int>(PickSlot::BearingSeat),
                    SelectionKindBit(SelectionItem::Kind::Face) |
                        SelectionKindBit(SelectionItem::Kind::Edge),
                    /*multi=*/false,
                    "Pick the seat — the shaft or bore the bearing goes on, "
                    "or the rim where it meets its shoulder"};
}

void OpenBearingTool(AppState& state) {
  state.bearingTool = AppState::BearingTool{};
  state.bearingTool.open = true;
  ArmBearingSeat(state); // the pick is the verb; ask for it at once
}

void CloseBearingTool(AppState& state) {
  state.bearingTool.open = false;
  state.bearingTool.ok = false; // SyncBearingGhost drops the instance
  if (state.slotPick.featureId == kBearingToolSlot)
    state.slotPick = {};
}

void RefreshBearingFit(AppState& state) {
  AppState::BearingTool& tool = state.bearingTool;
  tool.ok = false;
  tool.summary.clear();
  tool.why.clear();
  tool.candidates.clear();
  if (!tool.seatElement.IsSet())
    return;
  BearingRequest request;
  request.designation = tool.designation;
  request.dryRun = true;
  const BearingFit fit = FitBearing(
      state, RimPick{tool.seatOccurrence, tool.seatElement}, request);
  tool.ok = fit.ok;
  tool.summary = fit.summary;
  tool.why = fit.why;
  tool.candidates = fit.candidates;
  if (fit.ok) {
    tool.designation = fit.designation;
    tool.position = fit.position;
    tool.orientation = fit.orientation;
  }
}

void BearingSeatPicked(AppState& state, const SelectionItem& item) {
  if ((item.kind != SelectionItem::Kind::Face &&
       item.kind != SelectionItem::Kind::Edge) ||
      !item.element.IsSet()) {
    state.slotPick.miss = "that is not a face or an edge — pick the shaft or "
                          "bore the bearing goes on";
    return;
  }
  state.bearingTool.seatOccurrence = item.id;
  state.bearingTool.seatElement = item.element;
  state.bearingTool.designation.clear(); // a new seat, a new diameter
  state.slotPick = {};
  RefreshBearingFit(state);
}

namespace {
void ClearBearingGhost(AppState& state) {
  if (state.meshRenderer && state.bearingGhostInstance != 0)
    state.meshRenderer->RemoveInstance(state.bearingGhostInstance);
  state.bearingGhostInstance = 0;
  state.bearingGhostKey.clear();
}
} // namespace

void SyncBearingGhost(AppState& state) {
  const AppState::BearingTool& tool = state.bearingTool;
  if (!(tool.open && tool.ok && state.meshRenderer)) {
    if (state.bearingGhostInstance != 0)
      ClearBearingGhost(state);
    return;
  }
  const std::string key =
      tool.designation + "@" + std::to_string(tool.position.x) + "," +
      std::to_string(tool.position.y) + "," + std::to_string(tool.position.z) +
      ";" + std::to_string(tool.orientation.w) + "," +
      std::to_string(tool.orientation.x) + "," +
      std::to_string(tool.orientation.y) + "," +
      std::to_string(tool.orientation.z);
  if (key == state.bearingGhostKey)
    return;
  ClearBearingGhost(state);
  Document scratch;
  scratch.AddFeature(std::make_unique<BearingFeature>("", tool.designation));
  if (scratch.GetBodies().empty())
    return;
  const CADShape::Mesh mesh = scratch.GetBodies().front().shape.GenerateMesh();
  if (mesh.vertices.empty())
    return;
  const uint32_t asset = state.meshRenderer->CreateAsset(
      CADMeshConverter::ConvertCADMeshToVertices(mesh), mesh.indices);
  if (asset == 0)
    return;
  const glm::mat4 xf(glm::translate(glm::dmat4(1.0), tool.position) *
                     glm::mat4_cast(tool.orientation));
  const uint32_t id = state.meshRenderer->AddInstance(asset, xf);
  // The fastener ghost's blue: a preview must not read as geometry.
  state.meshRenderer->SetInstanceColor(id, glm::vec3(0.35f, 0.62f, 1.0f));
  state.meshRenderer->SetInstanceGhost(id, 0.55f);
  state.bearingGhostInstance = id;
  state.bearingGhostKey = key;
}

void DrawBearingToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Bearing");
  AppState::BearingTool& tool = state.bearingTool;

  if (state.assembly == nullptr) {
    ImGui::TextWrapped("A bearing is a bought part placed in an ASSEMBLY. "
                       "Switch to an assembly tab and pick the seat it goes "
                       "on.");
    if (ImGui::Button("Close###bearing_done_notab"))
      CloseBearingTool(state);
    ImGui::End();
    return;
  }

  const bool armed = state.slotPick.featureId == kBearingToolSlot;
  const bool havePick = tool.seatElement.IsSet();
  ImGui::TextDisabled("Seat");
  if (ImGui::Button(havePick ? "Re-pick seat###bearing_pick"
                             : "Pick seat###bearing_pick"))
    ArmBearingSeat(state);
  ImGui::SameLine();
  if (armed)
    ImGui::TextDisabled("click a shaft, a bore, or its rim…");
  else if (havePick)
    ImGui::Text("occurrence %llu",
                static_cast<unsigned long long>(tool.seatOccurrence));
  else
    ImGui::TextDisabled("nothing picked");

  // The bearings this seat's diameter takes, one button each — offered only
  // once a seat is picked, because before that there is nothing to choose
  // between (U10). Buttons rather than a combo so each has a stable id.
  if (!tool.candidates.empty()) {
    ImGui::Separator();
    ImGui::TextDisabled("Bearing");
    for (const std::string& c : tool.candidates) {
      const std::string designation = c.substr(0, c.find(' '));
      const bool active = tool.ok && tool.designation == designation;
      ImGui::BeginDisabled(active);
      const std::string id = c + "###bearing_des_" + designation;
      if (ImGui::Button(id.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
        tool.designation = designation;
        RefreshBearingFit(state);
      }
      ImGui::EndDisabled();
    }
  }

  ImGui::Separator();
  if (!havePick) {
    ImGui::TextDisabled("Pick the seat; the diameter and the position follow "
                        "from it.");
  } else if (tool.ok) {
    ImGui::TextWrapped("%s", tool.summary.c_str());
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
    ImGui::TextWrapped("%s", tool.why.c_str());
    ImGui::PopStyleColor();
  }
  if (!state.slotPick.miss.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.25f, 1.0f));
    ImGui::TextWrapped("%s", state.slotPick.miss.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::Separator();
  ImGui::BeginDisabled(!tool.ok);
  if (ImGui::Button("Place###bearing_place")) {
    BearingRequest request;
    request.designation = tool.designation;
    const BearingFit fit = FitBearing(
        state, RimPick{tool.seatOccurrence, tool.seatElement}, request);
    if (fit.ok) {
      tool.placed++;
      // Back to picking: a shaft usually carries two.
      tool.seatElement = {};
      tool.seatOccurrence = 0;
      tool.designation.clear();
      tool.candidates.clear();
      tool.summary.clear();
      tool.ok = false;
      ArmBearingSeat(state);
    } else {
      tool.ok = false;
      tool.why = fit.why;
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Close###bearing_done"))
    CloseBearingTool(state);
  if (tool.placed > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("%d placed", tool.placed);
  }
  ImGui::End();
}

} // namespace ui
