// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/AlignTool.h"

#include "common/Logger.h"
#include "core/Command.h"
#include "core/ImportAlign.h"
#include "core/io/Serialization.h"
#include "ui/ExprInput.h"
#include "ui/FeatureEditor.h" // kAlignToolSlot
#include "ui/PanelLayout.h"   // LeftLabel
#include "ui/PreviewFeature.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <string>

namespace ui {

namespace {

using importalign::Mode;

constexpr const char* kModeLabels[] = {"Floor", "Three points",
                                       "Principal axes", "Scale"};

ImportFeature* AlignedImport(AppState& state, size_t* index = nullptr) {
  const auto& t = state.alignTool;
  if (!t.open || state.document == nullptr ||
      state.document.get() != t.document)
    return nullptr;
  return dynamic_cast<ImportFeature*>(
      state.document->FindFeatureById(t.featureId, index));
}

void Reset(AppState& state) {
  if (state.slotPick.featureId == kAlignToolSlot)
    state.slotPick = {};
  state.alignTool = AppState::AlignTool{};
}

// Writes `p` onto the import and regenerates: the preview, off the undo
// stack, as a pending feature's parameters are.
void Show(AppState& state, ImportFeature& imp,
          const importplace::Placement& p) {
  if (imp.placement == p)
    return;
  imp.placement = p;
  state.document->Regenerate();
}

std::string Prompt(Mode mode, size_t i) {
  switch (mode) {
  case Mode::Floor:
    return "Align: click the floor of the import (on an open scan, the rim "
           "of the hole where it stood)";
  case Mode::ThreePoints:
    return "Align: click point " + std::to_string(i + 1) +
           " of 3 on the floor plane";
  case Mode::Scale:
    return "Align: click " + std::string(i == 0 ? "one end" : "the other end") +
           " of a distance you know";
  case Mode::Principal:
    break;
  }
  return "";
}

void Arm(AppState& state, size_t i) {
  state.slotPick = {};
  state.slotPick.featureId = kAlignToolSlot;
  state.slotPick.slot = static_cast<int>(i);
  state.slotPick.kinds = SelectionKindBit(SelectionItem::Kind::Face);
  state.slotPick.prompt = Prompt(Mode(state.alignTool.mode), i);
}

void ArmNext(AppState& state) {
  const auto& t = state.alignTool;
  if (t.points.size() < importalign::PointsFor(Mode(t.mode)))
    Arm(state, t.points.size());
  else if (state.slotPick.featureId == kAlignToolSlot)
    state.slotPick = {};
}

std::string Summary(Mode mode, const nlohmann::json& r) {
  char buf[256] = {};
  switch (mode) {
  case Mode::Floor:
    if (r.value("fitted", std::string()) == "boundary_loop")
      std::snprintf(buf, sizeof buf,
                    "Fitted the rim: %zu vertices, %.3g mm RMS (%.3g max)",
                    r.value("loop_vertices", size_t{0}), r.value("rms_mm", 0.0),
                    r.value("max_mm", 0.0));
    else
      std::snprintf(buf, sizeof buf,
                    "Fitted %zu triangles, %.4g mm2: %.3g mm RMS (%.3g max) "
                    "within %.3g mm",
                    r.value("triangles", size_t{0}), r.value("area_mm2", 0.0),
                    r.value("rms_mm", 0.0), r.value("max_mm", 0.0),
                    r.value("tolerance_mm", 0.0));
    break;
  case Mode::ThreePoints:
    std::snprintf(buf, sizeof buf, "The plane through the three points");
    break;
  case Mode::Principal: {
    const auto s = r.value("spread_mm", std::vector<double>{0, 0, 0});
    std::snprintf(buf, sizeof buf, "Spread %.3g / %.3g / %.3g mm along X/Y/Z",
                  s.at(0), s.at(1), s.at(2));
    break;
  }
  case Mode::Scale:
    std::snprintf(buf, sizeof buf, "Measured %.4g mm: scaled by %.6g",
                  r.value("measured_mm", 0.0), r.value("factor", 1.0));
    break;
  }
  std::string out = buf;
  if (r.contains("note"))
    out += "\n" + r.at("note").get<std::string>();
  return out;
}

// The alignment of the points as picked, previewed; or, while the mode
// lacks an input, the import where it was.
void Recompute(AppState& state) {
  auto& t = state.alignTool;
  ImportFeature* imp = AlignedImport(state);
  if (imp == nullptr)
    return;
  t.error.clear();
  t.summary.clear();
  const Mode mode = Mode(t.mode);
  // Always from where it stood: the points were picked there, and an
  // alignment of the preview would compound.
  Show(state, *imp, t.original);
  t.preview.reset();
  if (t.points.size() < importalign::PointsFor(mode))
    return;
  importalign::Request rq;
  rq.mode = mode;
  rq.pointsMm = t.points;
  rq.distanceMm = t.distanceMm;
  if (mode == Mode::Floor && t.ownTolerance)
    rq.toleranceMm = t.toleranceMm;
  importalign::Result res;
  std::string why;
  try {
    if (!importalign::Align(importalign::ImportBodies(*state.document, *imp),
                            t.original, rq, res, why)) {
      t.error = why;
      return;
    }
  } catch (const std::exception& e) {
    t.error = e.what();
    return;
  }
  t.summary = Summary(mode, res.report);
  // Re-resolved: the regeneration above may have moved the feature list's
  // storage, never the feature itself, but ask again rather than assume.
  imp = AlignedImport(state);
  if (imp == nullptr)
    return;
  Show(state, *imp, res.placement);
  t.preview = res.placement;
}

// A default derived from the import: a five-hundredth of its diagonal, as
// align_import's.
double DerivedTolerance(AppState& state, const ImportFeature& imp) {
  double diag = 0.0;
  glm::dvec3 lo(0.0), hi(0.0), blo, bhi;
  bool any = false;
  for (const Body* b : importalign::ImportBodies(*state.document, imp))
    if (BodyBounds(*b, blo, bhi)) {
      lo = any ? glm::min(lo, blo) : blo;
      hi = any ? glm::max(hi, bhi) : bhi;
      any = true;
    }
  if (any)
    diag = glm::length(hi - lo);
  return diag / 500.0;
}

} // namespace

bool HasImportToAlign(const AppState& state) {
  if (state.document == nullptr)
    return false;
  for (size_t i = 0; i < state.document->FeatureCount(); i++)
    if (dynamic_cast<const ImportFeature*>(state.document->GetFeature(i)))
      return true;
  return false;
}

void OpenAlignTool(AppState& state, uint64_t importId) {
  if (state.document == nullptr)
    return;
  Document& doc = *state.document;
  const auto isImport = [&](uint64_t id) {
    return id != 0 && dynamic_cast<const ImportFeature*>(
                          doc.FindFeatureById(id)) != nullptr;
  };
  // What was picked first: a body, a face or a tree row of an Import.
  if (!isImport(importId))
    importId = 0;
  for (const SelectionItem& item : state.selection.items) {
    if (importId != 0)
      break;
    const uint64_t id = item.kind == SelectionItem::Kind::Face ||
                                item.kind == SelectionItem::Kind::Edge
                            ? item.element.featureId
                            : item.id;
    if (isImport(id))
      importId = id;
  }
  for (size_t i = doc.FeatureCount(); importId == 0 && i > 0; i--)
    if (dynamic_cast<const ImportFeature*>(doc.GetFeature(i - 1)))
      importId = doc.GetFeature(i - 1)->GetId();
  if (importId == 0)
    return;
  // One session at a time: a preview, an editor or another alignment ends.
  CancelPendingFeature(state);
  CancelAlignTool(state);
  state.editFeatureId = 0;
  auto* imp = dynamic_cast<ImportFeature*>(doc.FindFeatureById(importId));
  auto& t = state.alignTool;
  t.open = true;
  t.document = &doc;
  t.featureId = importId;
  t.mode = static_cast<int>(Mode::Floor);
  t.original = imp->placement;
  LOG_INFO("tool: Align started on '{}'", imp->GetName());
  ArmNext(state);
}

void CancelAlignTool(AppState& state) {
  if (!state.alignTool.open)
    return;
  // Put back only what the preview wrote: an RPC or an undo that changed
  // the placement since is not the tool's to overwrite.
  if (ImportFeature* imp = AlignedImport(state);
      imp != nullptr && state.alignTool.preview &&
      imp->placement == *state.alignTool.preview) {
    imp->placement = state.alignTool.original;
    state.document->Regenerate();
  }
  LOG_INFO("tool: Align cancelled");
  Reset(state);
}

bool AcceptAlignTool(AppState& state) {
  auto& t = state.alignTool;
  size_t index = 0;
  ImportFeature* imp = AlignedImport(state, &index);
  if (imp == nullptr || !t.preview || *t.preview == t.original ||
      !(imp->placement == *t.preview))
    return false;
  // One undo step from where it stood to where the preview put it.
  ImportFeature was(*imp);
  was.placement = t.original;
  const nlohmann::json before = DocumentIO::FeatureToJson(was);
  const nlohmann::json after = DocumentIO::FeatureToJson(*imp);
  LOG_INFO("tool: Align accepted ({})", importalign::ToString(Mode(t.mode)));
  Reset(state);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  return true;
}

void AlignPointPicked(AppState& state, const SelectionItem& item) {
  auto& t = state.alignTool;
  ImportFeature* imp = AlignedImport(state);
  if (imp == nullptr) {
    state.slotPick = {};
    return;
  }
  if (item.kind != SelectionItem::Kind::Face ||
      item.element.featureId != t.featureId) {
    state.slotPick.miss =
        "that is not on '" + imp->GetName() + "': click a point on the import";
    return;
  }
  // Clicked where the bodies are shown; kept where they stood before the
  // preview moved them, which is where every point is computed from.
  glm::dvec3 p = state.slotPick.hitPoint;
  if (t.preview)
    p = glm::dvec3(t.original.Matrix() * glm::inverse(t.preview->Matrix()) *
                   glm::dvec4(p, 1.0));
  const size_t slot = static_cast<size_t>(std::max(0, state.slotPick.slot));
  if (slot < t.points.size())
    t.points[slot] = p;
  else
    t.points.push_back(p);
  // The true distance starts as the one measured, so nothing scales until
  // it is typed: a default derived from the picks, never a literal.
  if (Mode(t.mode) == Mode::Scale && t.points.size() == 2 &&
      !(t.distanceMm > 0.0))
    t.distanceMm = glm::length(t.points[1] - t.points[0]);
  state.slotPick = {};
  ArmNext(state);
  Recompute(state);
}

void DrawAlignToolWindow(AppState& state) {
  auto& t = state.alignTool;
  ImportFeature* imp = AlignedImport(state);
  if (imp == nullptr) {
    Reset(state); // the document or the import went away
    return;
  }
  BeginToolWindowFrame(state, "Align");
  ImGui::TextDisabled("Import: %s", imp->GetName().c_str());
  int mode = t.mode;
  LeftLabel("Mode");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##align_mode", &mode, kModeLabels, 4) && mode != t.mode) {
    t.mode = mode;
    t.points.clear();
    t.distanceMm = 0.0;
    ArmNext(state);
    Recompute(state); // principal needs no point: it previews at once
  }
  const Mode m = Mode(t.mode);
  const size_t need = importalign::PointsFor(m);
  for (size_t i = 0; i < need; i++) {
    char text[128];
    const bool armed = state.slotPick.featureId == kAlignToolSlot &&
                       state.slotPick.slot == static_cast<int>(i);
    if (i < t.points.size())
      std::snprintf(text, sizeof text, "Point %zu: %.4g, %.4g, %.4g%s", i + 1,
                    t.points[i].x, t.points[i].y, t.points[i].z,
                    armed ? " (click again)" : "");
    else
      std::snprintf(text, sizeof text, "Point %zu: %s", i + 1,
                    armed ? "click it in the view" : "not picked");
    // One id a point, whatever the row says: tests click ###align_pt<i>.
    const std::string label =
        text + std::string("###align_pt") + std::to_string(i);
    if (ImGui::Button(label.c_str()))
      Arm(state, i);
  }
  if (m == Mode::Floor) {
    bool own = t.ownTolerance;
    if (ImGui::Checkbox("Own tolerance###align_own_tol", &own)) {
      t.ownTolerance = own;
      if (own && !(t.toleranceMm > 0.0))
        t.toleranceMm = DerivedTolerance(state, *imp);
      Recompute(state);
    }
    if (t.ownTolerance) {
      LeftLabel("Tolerance (mm)");
      ImGui::SetNextItemWidth(-1);
      double tol = t.toleranceMm;
      if (ExprInputDouble(state, "##align_tol", &tol, "%.4g") && tol > 0.0 &&
          std::isfinite(tol) && tol != t.toleranceMm) {
        t.toleranceMm = tol;
        Recompute(state);
      }
    } else {
      ImGui::TextDisabled("Tolerance: a 500th of the import's size");
    }
  }
  if (m == Mode::Scale && t.points.size() == 2) {
    LeftLabel("True distance (mm)");
    ImGui::SetNextItemWidth(-1);
    double d = t.distanceMm;
    if (ExprInputDouble(state, "##align_distance", &d, "%.6g") && d > 0.0 &&
        std::isfinite(d) && d != t.distanceMm) {
      t.distanceMm = d;
      Recompute(state);
    }
  }
  if (state.slotPick.featureId == kAlignToolSlot &&
      !state.slotPick.miss.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "%s",
                       state.slotPick.miss.c_str());
  if (!t.error.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", t.error.c_str());
    ImGui::PopTextWrapPos();
  } else if (!t.summary.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
    ImGui::TextDisabled("%s", t.summary.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Separator();
  const bool ready = t.preview.has_value() && !(*t.preview == t.original);
  if (!ready && t.error.empty()) {
    if (t.points.size() < need)
      ImGui::TextDisabled("Pick point %zu to continue", t.points.size() + 1);
    else if (m == Mode::Scale)
      ImGui::TextDisabled("Type the true distance to continue");
    else
      ImGui::TextDisabled("It stands this way already");
  }
  ImGui::BeginDisabled(!ready);
  ImGui::PushStyleColor(ImGuiCol_Button,
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  const bool accept = ImGui::Button("Accept###tw_accept");
  ImGui::PopStyleColor();
  ImGui::EndDisabled();
  ImGui::SameLine();
  const bool cancel = ImGui::Button("Cancel###tw_cancel");
  ImGui::End();
  if (accept)
    AcceptAlignTool(state);
  else if (cancel)
    CancelAlignTool(state);
}

} // namespace ui
