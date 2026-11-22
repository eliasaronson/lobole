// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ScanRelations.h"

#include "common/Logger.h"
#include "core/Command.h"
#include "core/io/Serialization.h"
#include "core/sketch/SectionTrace.h"
#include "core/sketch/SketchCandidates.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

namespace {

// What the candidates were found for: the sketch, the limit, and a print of
// its traced geometry, constraints and cut -- they are found again only
// when one of those moves, not every frame.
struct Cache {
  uint64_t sketchId = 0;
  double maxSigmas = 0.0;
  size_t print = 0;
  units::Length unit = units::Length::Millimetre;
  std::vector<sketch::Candidate> rows;
  // What the last Apply settled -- added, or found already fixed -- kept
  // out of the list while the sketch's constraints are as it left them.
  std::vector<std::string> settled;
  uint64_t settledSketch = 0;
  size_t settledConstraints = 0;
};
Cache& TheCache() {
  static Cache c;
  return c;
}

size_t Print(const SketchFeature& sk) {
  size_t h = sk.constraints.size() * 1000003u + sk.traced.size();
  const auto mix = [&](double v) {
    h ^= std::hash<double>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  };
  for (const sketch::TracedFrom& t : sk.traced) {
    const size_t i = sk.IndexOfEntity(t.entity);
    if (i >= sk.entities.size())
      continue;
    for (uint8_t w = 0; w < 3; w++) {
      const glm::dvec2 p = sketch::EntityPointUV(sk.entities[i], w);
      mix(p.x);
      mix(p.y);
    }
  }
  for (const sketch::ExternalGeom& e : sk.external)
    for (const sketch::CutChain& c : e.cut) {
      mix(double(c.points.size()));
      if (!c.points.empty()) {
        mix(c.points.front().x);
        mix(c.points.back().y);
      }
    }
  return h;
}

std::vector<sketch::Candidate> Candidates(AppState& state,
                                          const SketchFeature& sk) {
  Cache& c = TheCache();
  if (c.settledSketch != sk.GetId() ||
      c.settledConstraints != sk.constraints.size())
    c.settled.clear();
  const units::Length unit = state.ActiveUnits().length;
  const double maxSigmas = state.sketchEdit.relationsMaxSigmas;
  const size_t print = Print(sk);
  if (c.sketchId != sk.GetId() || c.maxSigmas != maxSigmas ||
      c.print != print || c.unit != unit) {
    c.sketchId = sk.GetId();
    c.maxSigmas = maxSigmas;
    c.print = print;
    c.unit = unit;
    c.rows = sketch::FindCandidates(sk, {maxSigmas, unit});
  }
  std::vector<sketch::Candidate> out;
  for (const sketch::Candidate& r : c.rows)
    if (std::find(c.settled.begin(), c.settled.end(), r.key) == c.settled.end())
      out.push_back(r);
  return out;
}

// "line 12", "arc 14", "the X axis".
std::string EntityName(const SketchFeature& sk, uint32_t id) {
  if (id == sketch::kAxisX)
    return "the X axis";
  if (id == sketch::kAxisY)
    return "the Y axis";
  if (id == sketch::kOrigin)
    return "the origin";
  const size_t i = sk.IndexOfEntity(id);
  const char* kind = "entity";
  if (i < sk.entities.size()) {
    const sketch::Entity& e = sk.entities[i];
    kind = std::holds_alternative<sketch::Line>(e)     ? "line"
           : std::holds_alternative<sketch::Circle>(e) ? "circle"
           : std::holds_alternative<sketch::Arc>(e)    ? "arc"
                                                       : "entity";
  }
  // Named by its place along the trace ("line 3"), not by its id: an id
  // is a ten-digit number no one reads.
  for (size_t k = 0; k < sk.traced.size(); k++)
    if (sk.traced[k].entity == id)
      return std::string(kind) + " " + std::to_string(k + 1);
  return std::string(kind) + " " + std::to_string(id);
}

// A row's words: what it is, and for a size, the round figure.
std::string RowLabel(AppState& state, const SketchFeature& sk,
                     const sketch::Candidate& c) {
  using K = sketch::CandidateKind;
  static const char* kWords[] = {"Horizontal",    "Vertical",     "Parallel",
                                 "Perpendicular", "Collinear",    "On the axis",
                                 "Equal",         "Equal radius", "Concentric",
                                 "At the origin", "Tangent",      "Symmetric",
                                 "Length",        "Distance",     "Radius",
                                 "Diameter",      "Angle"};
  std::string s = kWords[static_cast<int>(c.kind)];
  const units::System sys = state.ActiveUnits();
  if (c.kind == K::Angle)
    s += " " + units::FormatAngle(c.nominal, sys);
  else if (sketch::IsSize(c.kind))
    s += " " + units::FormatLength(c.nominal, sys);
  return s;
}

std::string RowEntities(const SketchFeature& sk, const sketch::Candidate& c) {
  std::string s;
  for (size_t i = 0; i < c.entities.size(); i++) {
    if (i > 0)
      s += c.kind == sketch::CandidateKind::Symmetric &&
                   i + 1 == c.entities.size()
               ? " about "
               : ", ";
    s += EntityName(sk, c.entities[i]);
  }
  return s;
}

// Three figures: the off is read against its sigma, not to the
// nanometre.
std::string RowOff(AppState& state, const sketch::Candidate& c) {
  const units::System sys = state.ActiveUnits();
  const double v = std::abs(c.off) < 1e-9 ? 0.0 : c.off;
  return c.degrees
             ? units::FormatNumber(v, 3) + "\xc2\xb0"
             : units::FormatNumber(v / units::MmPerLength(sys.length), 3) +
                   " " + units::ToString(sys.length);
}

bool Unticked(const AppState& state, const std::string& key) {
  const auto& u = state.sketchEdit.relationsUnticked;
  return std::find(u.begin(), u.end(), key) != u.end();
}

} // namespace

bool SketchHasTraced(AppState& state) {
  const SketchFeature* sk = ResolveActiveSketch(state);
  return sk != nullptr && !sk->traced.empty();
}

bool ApplyScanRelations(AppState& state) {
  size_t index = 0;
  SketchFeature* sk = ResolveActiveSketch(state, &index);
  if (sk == nullptr || !state.commands || state.document == nullptr)
    return false;
  SketchEditState& edit = state.sketchEdit;
  sketch::AcceptOptions o;
  o.maxSigmas = edit.relationsMaxSigmas;
  o.unit = state.ActiveUnits().length;
  for (const sketch::Candidate& c : Candidates(state, *sk))
    if (!Unticked(state, c.key))
      o.only.push_back(c.key);
  if (o.only.empty())
    return false;
  const nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  SketchFeature work = *sk;
  const sketch::AcceptResult res = sketch::AcceptCandidates(work, o);
  // What the solve took back, and why; the rows that only repeat what was
  // added are counted, not listed.
  edit.relationsLeft.clear();
  size_t implied = 0;
  for (const sketch::CandidateOutcome& l : res.left) {
    if (l.reason.find("already fixes") != std::string::npos) {
      implied++;
      continue;
    }
    edit.relationsLeft.push_back(RowLabel(state, *sk, l.candidate) + " (" +
                                 RowEntities(*sk, l.candidate) +
                                 "): " + l.reason);
  }
  if (implied > 0)
    edit.relationsLeft.push_back(std::to_string(implied) +
                                 (implied == 1 ? " row was" : " rows were") +
                                 " already implied by what was added");
  edit.relationsApplied = res.accepted.size();
  Cache& cache = TheCache();
  cache.settled.clear();
  for (const sketch::CandidateOutcome& a : res.accepted)
    cache.settled.push_back(a.candidate.key);
  for (const sketch::CandidateOutcome& l : res.left)
    if (l.reason.find("already fixes") != std::string::npos)
      cache.settled.push_back(l.candidate.key);
  cache.settledSketch = sk->GetId();
  cache.settledConstraints = work.constraints.size();
  if (res.accepted.empty())
    return true;
  sk->entities = work.entities;
  sk->constraints = work.constraints;
  sk->RegenerateDerived();
  const nlohmann::json after = DocumentIO::FeatureToJson(*sk);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  LOG_INFO("Scan relations: {} accepted, {} left", res.accepted.size(),
           res.left.size());
  return true;
}

void DrawScanRelationsSession(AppState& state, SketchFeature& sketch) {
  SketchEditState& edit = state.sketchEdit;
  BeginToolWindowFrame(state, "Scan relations");
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
  ImGui::TextDisabled("What the traced lines and arcs nearly are, measured "
                      "on the section's cut. Fewest sigmas first.");
  ImGui::PopTextWrapPos();
  float limit = static_cast<float>(edit.relationsMaxSigmas);
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
  if (ImGui::SliderFloat("###relsigmas", &limit, 0.5f, 6.0f,
                         "within %.1f sigmas"))
    edit.relationsMaxSigmas = std::clamp(static_cast<double>(limit), 0.5, 6.0);
  ImGui::SetItemTooltip("How far from a relation the fit may be, over its "
                        "own uncertainty of it. 3 is the default: a true "
                        "relation is missed about once in 370 times");
  const std::vector<sketch::Candidate> rows = Candidates(state, sketch);
  edit.litEntities.clear();
  if (rows.empty()) {
    ImGui::TextDisabled("Nothing within %.1f sigmas.", edit.relationsMaxSigmas);
  } else {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_SizingFixedFit |
        (rows.size() > 12 ? ImGuiTableFlags_ScrollY : 0);
    const float height =
        rows.size() > 12 ? ImGui::GetTextLineHeightWithSpacing() * 12.5f : 0.0f;
    if (ImGui::BeginTable("###relations", 4, flags, ImVec2(0.0f, height))) {
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("relation", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 15.0f);
      ImGui::TableSetupColumn("off", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 5.0f);
      ImGui::TableSetupColumn("sigmas", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 4.0f);
      for (size_t i = 0; i < rows.size(); i++) {
        const sketch::Candidate& c = rows[i];
        ImGui::PushID(c.key.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool ticked = !Unticked(state, c.key);
        if (ImGui::Checkbox("###tick", &ticked)) {
          auto& u = edit.relationsUnticked;
          if (ticked)
            u.erase(std::remove(u.begin(), u.end(), c.key), u.end());
          else
            u.push_back(c.key);
        }
        bool hovered = ImGui::IsItemHovered();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(RowLabel(state, sketch, c).c_str());
        hovered |= ImGui::IsItemHovered();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", RowEntities(sketch, c).c_str());
        hovered |= ImGui::IsItemHovered();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", RowOff(state, c).c_str());
        hovered |= ImGui::IsItemHovered();
        ImGui::TableNextColumn();
        // The evidence as a bar: empty at none, full at the limit.
        const float frac = static_cast<float>(std::clamp(
            c.sigmas / std::max(edit.relationsMaxSigmas, 1e-9), 0.0, 1.0));
        // Not a progress bar: a gauge of how much of the limit it uses.
        {
          char buf[16];
          std::snprintf(buf, sizeof buf, "%.1f", c.sigmas);
          ImGui::AlignTextToFramePadding();
          ImVec2 at = ImGui::GetCursorScreenPos();
          at.y += ImGui::GetStyle().FramePadding.y;
          const ImVec2 size(ImGui::GetFontSize() * 3.5f,
                            ImGui::GetTextLineHeight());
          ImDrawList* dl = ImGui::GetWindowDrawList();
          dl->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y),
                            ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
          dl->AddRectFilled(at, ImVec2(at.x + size.x * frac, at.y + size.y),
                            ImGui::GetColorU32(ImGuiCol_PlotHistogram, 0.6f),
                            3.0f);
          ImGui::Dummy(size);
          hovered |= ImGui::IsItemHovered();
          const ImVec2 ts = ImGui::CalcTextSize(buf);
          dl->AddText(ImVec2(at.x + 0.5f * (size.x - ts.x), at.y),
                      ImGui::GetColorU32(ImGuiCol_Text), buf);
        }
        if (hovered) {
          for (uint32_t id : c.entities)
            if (!sketch::IsDatum(id))
              edit.litEntities.push_back(id);
          const units::System sys = state.ActiveUnits();
          const std::string sigma =
              c.degrees ? units::FormatNumber(c.sigma, 3) + "\xc2\xb0"
                        : units::FormatNumber(
                              c.sigma / units::MmPerLength(sys.length), 3) +
                              " " + units::ToString(sys.length);
          ImGui::SetTooltip("%s is %s from it; the fit knows that to %s (one "
                            "sigma)",
                            RowEntities(sketch, c).c_str(),
                            RowOff(state, c).c_str(), sigma.c_str());
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  size_t ticked = 0;
  for (const sketch::Candidate& c : rows)
    ticked += !Unticked(state, c.key);
  ImGui::BeginDisabled(ticked == 0);
  const std::string apply =
      "Apply " + std::to_string(ticked) + "###applyrelations";
  bool doApply = ImGui::Button(apply.c_str());
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Adds the ticked rows as constraints and dimensions, "
                        "fewest sigmas first, solving after each. One the "
                        "solve cannot meet, or that would move a piece off "
                        "its points, is left, saying why. One undo step");
  ImGui::SameLine();
  if (ImGui::SmallButton("All###relall"))
    edit.relationsUnticked.clear();
  ImGui::SameLine();
  if (ImGui::SmallButton("None###relnone")) {
    edit.relationsUnticked.clear();
    for (const sketch::Candidate& c : rows)
      edit.relationsUnticked.push_back(c.key);
  }
  ImGui::SameLine();
  bool close = ImGui::Button("Close###closerelations");
  if (edit.relationsApplied > 0 || !edit.relationsLeft.empty()) {
    ImGui::Separator();
    ImGui::Text("%zu applied", edit.relationsApplied);
    if (!edit.relationsLeft.empty()) {
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
      ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.40f, 1.0f), "Left:");
      for (const std::string& l : edit.relationsLeft)
        ImGui::BulletText("%s", l.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::End();
  // Both change the sketch or the session: after End().
  if (doApply)
    ApplyScanRelations(state);
  if (close) {
    edit.relationsOpen = false;
    edit.litEntities.clear();
  }
}

} // namespace ui
