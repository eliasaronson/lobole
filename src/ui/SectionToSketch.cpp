// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SectionToSketch.h"

#include "common/Logger.h"
#include "core/Command.h"
#include "core/io/Serialization.h"
#include "core/sketch/SectionTrace.h"
#include "core/sketch/SketchSection.h"
#include "core/sketch/SketchSolver.h"
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
  sk->external.back().detail = std::clamp(state.sectionSketchDetail, 0.0, 1.0);
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

uint64_t TraceSectionToSketch(AppState& state, uint64_t sectionId) {
  if (state.document == nullptr || !state.commands) {
    state.sectionSketchMsg =
        "A sketch belongs to a part: open the part to trace its section";
    return 0;
  }
  const size_t depth = state.commands->UndoDepth();
  const uint64_t id = SectionToSketch(state, sectionId);
  if (id == 0)
    return 0;
  size_t index = 0;
  auto* sk =
      dynamic_cast<SketchFeature*>(state.document->FindFeatureById(id, &index));
  if (sk == nullptr || sk->external.empty())
    return id;
  const nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  const sketch::TraceResult tr =
      sketch::TraceSection(*sk, sk->external.size() - 1,
                           std::clamp(state.sectionSketchDetail, 0.0, 1.0));
  if (!tr.error.empty()) {
    state.sectionSketchMsg = "Traced nothing: " + tr.error;
    return id;
  }
  sketch::Solver solver;
  solver.Load(*sk);
  if (solver.Solve().ok)
    solver.WriteBack(sk->entities);
  sk->RegenerateDerived();
  state.document->Regenerate();
  const nlohmann::json after = DocumentIO::FeatureToJson(*sk);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  // The sketch and its tracing are one step to undo.
  state.commands->CoalesceSince(depth, "Trace section");
  LOG_INFO("Section {} traced into sketch {}: {} entities", sectionId, id,
           tr.ids.size());
  return id;
}

namespace {

// The pieces fit's detail: fewer, longer pieces to the left.
void DrawDetailSlider(AppState& state) {
  float detail = static_cast<float>(state.sectionSketchDetail);
  ImGui::SetNextItemWidth(130.0f);
  if (ImGui::SliderFloat("###sketchdetail", &detail, 0.0f, 1.0f, "detail %.2f"))
    state.sectionSketchDetail =
        std::clamp(static_cast<double>(detail), 0.0, 1.0);
  ImGui::SetItemTooltip("The pieces fit's detail: fewer, longer pieces to "
                        "the left, more, shorter ones to the right (Trace, "
                        "and To sketch in Pieces)");
}

} // namespace

void DrawSectionToSketchRow(AppState& state) {
  if (state.document == nullptr)
    return;
  // In SectionFit's order.
  static const char* kFits[] = {"Lines", "Lines and arcs", "Splines", "Pieces"};
  int fit = static_cast<int>(state.sectionSketchFit);
  ImGui::SetNextItemWidth(130.0f);
  if (ImGui::Combo("###sketchfit", &fit, kFits, IM_ARRAYSIZE(kFits)))
    state.sectionSketchFit = static_cast<sketch::SectionFit>(fit);
  ImGui::SetItemTooltip(
      "What the cut is drawn with: lines, arcs or splines to within 1/1000 "
      "of the bodies' size, or the pieces -- the lines and arcs it is made "
      "of, found from its own scatter");
  ImGui::SameLine();
  const bool none = state.sectionActiveId == 0;
  ImGui::BeginDisabled(none);
  if (ImGui::Button("To sketch###tosketch"))
    SectionToSketch(state, state.sectionActiveId);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("A new sketch on the active cut, with the cut drawn in "
                        "it as construction geometry that follows the "
                        "bodies. Offset it for a profile");
  // Trace: the pieces, as real geometry over the cut, at this detail.
  DrawDetailSlider(state);
  ImGui::SameLine();
  ImGui::BeginDisabled(none);
  if (ImGui::Button("Trace###tracesection"))
    TraceSectionToSketch(state, state.sectionActiveId);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip(
      "A new sketch on the active cut with the cut drawn in it, and real "
      "lines and arcs traced over it: the pieces it is made of, joined end "
      "to end, to dimension and build on. They stay where they are when the "
      "cut moves");
  if (!state.sectionSketchMsg.empty())
    ImGui::TextWrapped("%s", state.sectionSketchMsg.c_str());
}

} // namespace ui
