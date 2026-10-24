// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/DesignSweepPanel.h"

#include "ui/AppActionsInternal.h" // SlotDoc
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"
#include "ui/DocumentSlots.h"
#include "ui/ExprInput.h"
#include "ui/RequirementsPanel.h" // the engine: plan, start, result
#include "ui/VariableActions.h"
#include "ui/Widgets.h" // DrawJobProgress

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

// The Design sweep window (docs/requirements/README.md#design-sweep). It
// asks the requirements engine -- design_sweep's own planner and job, in
// src/remote/RemoteRequirements.cpp -- so the window and an agent plan,
// refuse and run the same sweep.

namespace ui {

namespace {

using json = nlohmann::json;

struct Row {
  std::string name;
  uint64_t part = 0; // 0: the project's variable
  char from[64] = "";
  char to[64] = "";
  std::string note; // why it has no default range
};

struct Window {
  bool open = false;
  bool fromRequirements = false;
  std::vector<Row> rows;
  int samples = 5;
  bool corners = false;
  int refine = 0;
  uint64_t job = 0;
  json result;
  std::string error;
  // The plan line, recomputed when the request changes or a second passes.
  std::string planKey;
  std::chrono::steady_clock::time_point plannedAt;
  json plan;
  std::string planError;
};

Window& W() {
  static Window w;
  return w;
}

DesignSweepDrawn& Drawn() {
  static DesignSweepDrawn d;
  return d;
}

ImVec4 FailColour() { return ImVec4(0.90f, 0.35f, 0.30f, 1.0f); }
ImVec4 PassColour() { return ImVec4(0.35f, 0.75f, 0.40f, 1.0f); }

std::string Num(double v) { return std::format("{:.6g}", v); }

// The request the window stands for, in design_sweep's own words.
json Request(const Window& w) {
  json p{{"samples", w.samples}};
  if (w.corners)
    p["corners"] = true;
  if (w.refine > 0)
    p["refine"] = w.refine;
  if (w.fromRequirements) {
    p["from_requirements"] = true;
    return p;
  }
  json vars = json::array();
  for (const Row& r : w.rows) {
    json v{{"name", r.name},
           {"scope", r.part == 0 ? json("project") : json(r.part)}};
    // Each end as typed: a number or an expression; empty is left out, and
    // the plan says what is missing.
    const auto end = [&](const char* key, const char* text) {
      const std::string t = text;
      if (t.empty())
        return;
      char* stop = nullptr;
      const double d = std::strtod(t.c_str(), &stop);
      if (stop != nullptr && *stop == '\0')
        v[key] = d;
      else
        v[key] = t;
    };
    end("min", r.from);
    end("max", r.to);
    vars.push_back(std::move(v));
  }
  p["variables"] = vars;
  return p;
}

void Reset(Window& w) {
  w.open = true;
  w.rows.clear();
  w.fromRequirements = false;
  w.result = json();
  w.error.clear();
  w.planKey.clear();
}

void DrawResult(AppState&, Window& w, DesignSweepDrawn& drawn) {
  const json& r = w.result;
  if (r.contains("error")) {
    ImGui::TextColored(FailColour(), "The sweep failed: %s",
                       r.at("error").get<std::string>().c_str());
    return;
  }
  drawn.cancelled = r.value("cancelled", false);
  if (drawn.cancelled)
    ImGui::TextColored(FailColour(), "Cancelled: the points measured before "
                                     "are kept below.");
  // Each variable's first failure, above the table: where it breaks, the
  // last good value before it, and why.
  const json firstFailure = r.value("first_failure", json::object());
  for (const auto& [name, sides] : firstFailure.items()) {
    for (const char* side : {"below", "above"}) {
      if (!sides.contains(side))
        continue;
      const json& s = sides.at(side);
      std::string line =
          name + " " + side + ": fails at " + Num(s.at("failed").get<double>());
      if (!s.at("last_ok").is_null())
        line += " (good at " + Num(s.at("last_ok").get<double>()) + ")";
      if (!s.value("why", std::string()).empty())
        line += ": " + s.at("why").get<std::string>();
      drawn.firstFailures.push_back(line);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(FailColour(), "%s", line.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  if (firstFailure.empty())
    ImGui::TextColored(PassColour(), "No variable breaks the model in its "
                                     "range.");
  ImGui::TextDisabled("%d points, %.1f s",
                      static_cast<int>(r.value("points", json::array()).size()),
                      r.value("seconds", 0.0));
  for (const json& n : r.value("not_swept", json::array()))
    ImGui::TextDisabled("not swept: '%s' (%s)",
                        n.value("name", std::string()).c_str(),
                        n.value("why", std::string()).c_str());

  const json vars = r.value("variables", json::array());
  const json points = r.value("points", json::array());
  const json reqs = points.empty()
                        ? json::array()
                        : points[0].value("requirements", json::array());
  const int columns = static_cast<int>(vars.size() + 1 + reqs.size());
  if (columns <= 1 ||
      !ImGui::BeginTable("###sweep_table", columns,
                         ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_SizingFixedFit,
                         ImVec2(0.0f, ImGui::GetFontSize() * 16.0f)))
    return;
  for (const json& v : vars)
    ImGui::TableSetupColumn(v.value("name", std::string()).c_str());
  ImGui::TableSetupColumn("Model");
  for (const json& q : reqs)
    ImGui::TableSetupColumn(q.value("name", std::string()).c_str());
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableHeadersRow();
  const auto why = [](const json& p) {
    std::string out;
    for (const json& line : p.value("why", json::array()))
      out += (out.empty() ? "" : "\n") + line.get<std::string>();
    return out;
  };
  for (const json& p : points) {
    const bool ok = p.value("ok", false);
    drawn.points++;
    ImGui::TableNextRow();
    for (const json& v : vars) {
      ImGui::TableNextColumn();
      const std::string name = v.value("name", std::string());
      ImGui::Text("%s", Num(p.at("values").value(name, 0.0)).c_str());
    }
    ImGui::TableNextColumn();
    // The model: whether every part rebuilt and every sketch solved.
    bool built = true;
    for (const json& part : p.value("parts", json::array())) {
      built = built && part.value("regen", std::string()) == "ok";
      for (const json& sk : part.value("sketches", json::array()))
        built = built && sk.value("ok", true);
    }
    ImGui::TextColored(built ? PassColour() : FailColour(), "%s",
                       built ? "builds" : "broken");
    if (!built && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", why(p).c_str());
    for (const json& q : p.value("requirements", json::array())) {
      ImGui::TableNextColumn();
      const std::string st = q.value("state", std::string());
      const std::string text =
          q.contains("value")
              ? Num(q.at("value").get<double>()) +
                    (q.contains("unit") ? " " + q.at("unit").get<std::string>()
                                        : std::string())
              : st;
      ImGui::TextColored(st == "pass" ? PassColour() : FailColour(), "%s",
                         text.c_str());
      if (st != "pass" && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", q.value("note", st).c_str());
    }
    if (!ok) {
      drawn.failing++;
      if (drawn.firstFailingWhy.empty())
        drawn.firstFailingWhy = why(p);
    }
  }
  ImGui::EndTable();
}

} // namespace

void OpenDesignSweepForVariable(AppState& state, const std::string& name,
                                uint64_t part) {
  Window& w = W();
  Reset(w);
  Row row;
  row.name = name;
  row.part = part;
  double value = 0.0;
  bool found = false;
  if (part == 0) {
    if (const Parameter* p = FindProjectParameter(state, name)) {
      value = p->value;
      found = true;
    }
  } else if (const auto slot = SlotIndexById(state, part)) {
    if (const Document* d = SlotDoc(state, *slot))
      if (const Parameter* p = d->FindParameter(name)) {
        value = p->value;
        found = true;
      }
  }
  // Half to twice what it is now: derived from the variable, never a
  // literal range that is wrong for every real part.
  if (found && value != 0.0) {
    const double a = value / 2.0, b = value * 2.0;
    std::snprintf(row.from, sizeof row.from, "%.6g", std::min(a, b));
    std::snprintf(row.to, sizeof row.to, "%.6g", std::max(a, b));
  } else {
    row.note =
        "'" + name + "' is 0, so there is no range to derive from it: type one";
  }
  w.rows.push_back(std::move(row));
}

void OpenDesignSweepFromRequirements(AppState&) {
  Window& w = W();
  Reset(w);
  w.fromRequirements = true;
}

const DesignSweepDrawn& DesignSweepDrawnForTest() { return Drawn(); }

void DrawDesignSweepWindow(AppState& state) {
  Window& w = W();
  DesignSweepDrawn& drawn = Drawn();
  drawn = {};
  if (!w.open)
    return;
  const RequirementsEngine& engine = GetRequirementsEngine();
  if (engine.sweepPlan == nullptr) {
    w.open = false;
    return;
  }
  drawn.open = true;
  drawn.job = w.job;
  ImGui::SetNextWindowSize(
      ImVec2(ImGui::GetFontSize() * 40.0f, ImGui::GetFontSize() * 30.0f),
      ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Design sweep###DesignSweep", &w.open)) {
    ImGui::End();
    return;
  }
  BackgroundJob* job =
      w.job != 0 ? FindBackgroundJobById(state, w.job) : nullptr;
  if (w.job != 0 && job == nullptr && w.result.is_null()) {
    w.result = engine.sweepResult(state, w.job);
    if (w.result.is_null())
      w.error = "the sweep (job " + std::to_string(w.job) +
                ") ended without an answer";
  }

  ImGui::TextWrapped("Regenerates copies of the model over each range and "
                     "finds where it stops building or meeting its "
                     "requirements. Nothing in the document changes.");
  ImGui::Spacing();
  if (w.fromRequirements) {
    ImGui::TextUnformatted("The ranges the requirements state:");
    for (const json& v : w.plan.value("variables", json::array()))
      ImGui::BulletText("%s  %s .. %s", v.value("name", std::string()).c_str(),
                        Num(v.value("min", 0.0)).c_str(),
                        Num(v.value("max", 0.0)).c_str());
  } else if (ImGui::BeginTable("###sweep_rows", 3,
                               ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Variable");
    ImGui::TableSetupColumn("From");
    ImGui::TableSetupColumn("To");
    ImGui::TableHeadersRow();
    for (Row& r : w.rows) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.name.c_str());
      if (r.part != 0 && ImGui::IsItemHovered())
        ImGui::SetTooltip("Local to part %llu",
                          static_cast<unsigned long long>(r.part));
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputText(("###sweep_from_" + r.name).c_str(), r.from,
                       sizeof r.from);
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::InputText(("###sweep_to_" + r.name).c_str(), r.to, sizeof r.to);
    }
    ImGui::EndTable();
    for (const Row& r : w.rows)
      if (!r.note.empty())
        ImGui::TextDisabled("%s", r.note.c_str());
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
  ExprInputInt(state, "Samples###sweep_samples", &w.samples);
  w.samples = std::clamp(w.samples, 2, 33);
  ImGui::SameLine();
  ImGui::Checkbox("Corners###sweep_corners", &w.corners);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
  ExprInputInt(state, "Refine###sweep_refine", &w.refine);
  w.refine = std::clamp(w.refine, 0, 8);

  // The planned count against the cap, before Run: what design_sweep would
  // answer, or why it would refuse.
  const json request = Request(w);
  const auto now = std::chrono::steady_clock::now();
  if (job == nullptr && (request.dump() != w.planKey ||
                         now - w.plannedAt > std::chrono::seconds(1))) {
    w.planKey = request.dump();
    w.plannedAt = now;
    try {
      w.plan = engine.sweepPlan(state, request);
      w.planError.clear();
    } catch (const std::exception& e) {
      w.plan = json();
      w.planError = e.what();
    }
  }
  if (!w.planError.empty()) {
    drawn.planned = w.planError;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(FailColour(), "%s", w.planError.c_str());
    ImGui::PopTextWrapPos();
  } else if (w.plan.is_object()) {
    drawn.planned = std::to_string(w.plan.value("planned", 0)) +
                    " points planned, cap " +
                    std::to_string(w.plan.value("cap", 0));
    ImGui::TextDisabled("%s", drawn.planned.c_str());
  }

  if (job != nullptr) {
    JobProgress bar;
    bar.status = job->label;
    bar.fraction = job->fraction.load();
    bar.cancellable = job->cancellable;
    bar.cancelAsked = job->cancel.load();
    bar.cancelId = "###sweep_cancel";
    bar.cancelTip = "Stop the sweep: the points measured so far are kept";
    if (DrawJobProgress(bar))
      CancelBackgroundJobById(state, w.job,
                              "cancelled in the Design sweep "
                              "window");
  } else {
    ImGui::BeginDisabled(!w.planError.empty());
    if (ImGui::Button("Run###sweep_run")) {
      try {
        w.job = engine.sweepStart(state, request);
        w.result = json();
        w.error.clear();
      } catch (const std::exception& e) {
        w.error = e.what();
      }
    }
    ImGui::EndDisabled();
  }
  if (!w.error.empty())
    ImGui::TextColored(FailColour(), "%s", w.error.c_str());
  ImGui::Separator();
  if (w.result.is_object())
    DrawResult(state, w, drawn);
  ImGui::End();
}

} // namespace ui
