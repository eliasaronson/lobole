// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SectionToSketch.h"

#include "common/Logger.h"
#include "core/sketch/SketchSection.h"
#include "ui/SketchInteraction.h"
#include "ui/ViewActions.h"

#include <imgui.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace ui {

SketchPlane SectionSketchPlane(AppState& state,
                               const AppState::SectionView& section) {
  const glm::vec4 cut = SectionPlaneOf(state, section);
  const glm::dvec3 n = glm::normalize(glm::dvec3(glm::vec3(cut)));
  const double d =
      static_cast<double>(cut.w) / glm::length(glm::dvec3(glm::vec3(cut)));
  glm::vec3 lo(0.0f), hi(0.0f);
  const glm::dvec3 centre = VisibleSceneBounds(state, lo, hi)
                                ? 0.5 * (glm::dvec3(lo) + glm::dvec3(hi))
                                : glm::dvec3(0.0);
  const glm::dvec3 origin = centre - n * (glm::dot(centre, n) - d);
  // The base plane's U, put onto the cut; its V where U is seen end on.
  const SketchPlane base = DefaultPlane(std::clamp(section.basePlane, 0, 2));
  glm::dvec3 u = base.GetUAxis() - n * glm::dot(base.GetUAxis(), n);
  if (glm::length(u) < 1e-6)
    u = base.GetVAxis() - n * glm::dot(base.GetVAxis(), n);
  return SketchPlane(origin, n, glm::normalize(u));
}

uint64_t SectionToSketch(AppState& state, uint64_t sectionId) {
  state.sectionSketchMsg.clear();
  if (state.document == nullptr) {
    state.sectionSketchMsg =
        "A sketch belongs to a part: open the part to draw its section";
    return 0;
  }
  const AppState::SectionView* section = FindSection(state, sectionId);
  if (section == nullptr) {
    state.sectionSketchMsg = "No cut is active: pick one with its round button";
    return 0;
  }
  if (!SectionSourceResolved(state, *section)) {
    state.sectionSketchMsg =
        "This cut's datum plane or face is gone: pick a source again";
    return 0;
  }
  const SketchPlane plane = SectionSketchPlane(state, *section);
  // Refused before anything is made: a sketch whose section cuts nothing
  // would only fail.
  std::vector<const Body*> bodies;
  double diagonal = 0.0;
  for (const Body& b : state.document->GetBodies()) {
    glm::dvec3 lo, hi;
    if (!b.reference && BodyBounds(b, lo, hi)) {
      bodies.push_back(&b);
      diagonal = std::max(diagonal, glm::length(hi - lo));
    }
  }
  // Coarse: only whether it cuts anything is asked here.
  const sketch::SectionCut probe =
      sketch::CutBodies(bodies, plane, std::max(diagonal, 1e-6) * 0.01);
  if (bodies.empty() || (probe.chains.empty() && probe.error.empty())) {
    state.sectionSketchMsg = "This cut crosses no body of the part: move it "
                             "into the model first";
    return 0;
  }
  auto sk = std::make_unique<SketchFeature>("Section sketch", plane);
  sk->AddSectionGeometry({}, 0.0, state.sectionSketchFit);
  const uint64_t id = BeginPreparedSketch(state, std::move(sk));
  if (id == 0) {
    state.sectionSketchMsg = state.sketchEdit.actionMsg;
    return 0;
  }
  if (const Feature* f = state.document->FindFeatureById(id);
      f != nullptr && !f->GetError().empty())
    state.sectionSketchMsg = f->GetError();
  LOG_INFO("Section {} drawn into sketch {}", sectionId, id);
  return id;
}

void DrawSectionToSketchRow(AppState& state) {
  if (state.document == nullptr)
    return;
  static const char* kFits[] = {"Lines", "Lines and arcs", "Splines"};
  int fit = static_cast<int>(state.sectionSketchFit);
  ImGui::SetNextItemWidth(130.0f);
  if (ImGui::Combo("###sketchfit", &fit, kFits, 3))
    state.sectionSketchFit = static_cast<sketch::SectionFit>(fit);
  ImGui::SetItemTooltip("What the cut is drawn with, to within 1/1000 of the "
                        "bodies' size");
  ImGui::SameLine();
  const bool none = state.sectionActiveId == 0;
  ImGui::BeginDisabled(none);
  if (ImGui::Button("To sketch###tosketch"))
    SectionToSketch(state, state.sectionActiveId);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("A new sketch on the active cut, with the cut drawn in "
                        "it as construction geometry that follows the "
                        "bodies. Offset it for a profile");
  if (!state.sectionSketchMsg.empty())
    ImGui::TextWrapped("%s", state.sectionSketchMsg.c_str());
}

} // namespace ui
