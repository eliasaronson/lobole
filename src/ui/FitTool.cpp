// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/FitTool.h"

#include "common/Logger.h"
#include "core/Angles.h"
#include "core/Command.h"
#include "core/MeshFit.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/io/ReadContext.h" // CheckFeatureRoom
#include "ui/ExprInput.h"
#include "ui/FeatureEditor.h" // kFitToolSlot
#include "ui/PanelLayout.h"   // LeftLabel
#include "ui/PreviewFeature.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <string>

namespace ui {

namespace {

constexpr const char* kKindLabels[] = {"Auto", "Plane", "Cylinder", "Cone",
                                       "Sphere"};
// The combo's order, after Auto.
constexpr mesh::Kind kKinds[] = {mesh::Kind::Plane, mesh::Kind::Cylinder,
                                 mesh::Kind::Cone, mesh::Kind::Sphere};

bool OnItsDocument(const AppState& state) {
  const auto& t = state.fitTool;
  return t.open && state.document != nullptr &&
         state.document.get() == t.document;
}

void Reset(AppState& state) {
  if (state.slotPick.featureId == kFitToolSlot)
    state.slotPick = {};
  state.fitTool = AppState::FitTool{};
}

void Arm(AppState& state) {
  state.slotPick = {};
  state.slotPick.featureId = kFitToolSlot;
  state.slotPick.slot = 0;
  state.slotPick.kinds = SelectionKindBit(SelectionItem::Kind::Face);
  state.slotPick.prompt = "Fit: click a point on the surface to fit (a "
                          "scan's flat, bore, boss, cone or ball)";
}

std::optional<mesh::Kind> AskedKind(const AppState& state) {
  const int k = state.fitTool.kind;
  if (k <= 0 || k > 4)
    return std::nullopt;
  return kKinds[k - 1];
}

// The hints the window holds, as the fit and the datum take them.
mesh::FitHints HintsOf(const AppState& state) {
  const auto& t = state.fitTool;
  mesh::FitHints h;
  const std::optional<mesh::Kind> kind = AskedKind(state);
  if (t.side != 0 && kind != mesh::Kind::Plane)
    h.outside = t.side == 2;
  if (t.radiusOn && kind != mesh::Kind::Plane && kind != mesh::Kind::Cone) {
    h.radiusMin = t.radiusMin;
    h.radiusMax = t.radiusMax;
  }
  return h;
}

std::vector<const Body*> PickedBodies(const AppState& state) {
  return meshfit::BodiesOf(state.document->GetBodies(),
                           state.fitTool.bodyFeatureId);
}

std::string Summary(const meshfit::Result& r) {
  const mesh::FitResult& f = r.region.fit;
  char head[160] = {};
  switch (f.kind) {
  case mesh::Kind::Plane:
    std::snprintf(head, sizeof head, "Plane, normal %.3f, %.3f, %.3f", f.axis.x,
                  f.axis.y, f.axis.z);
    break;
  case mesh::Kind::Cylinder:
    std::snprintf(head, sizeof head, "Cylinder, radius %.4g mm", f.radius);
    break;
  case mesh::Kind::Cone:
    std::snprintf(head, sizeof head, "Cone, half angle %.4g deg",
                  angles::ToDegrees(f.halfAngle));
    break;
  case mesh::Kind::Sphere:
    std::snprintf(head, sizeof head, "Sphere, radius %.4g mm", f.radius);
    break;
  }
  const nlohmann::json j = meshfit::ResultJson(r);
  char tail[200] = {};
  std::snprintf(tail, sizeof tail,
                "\n%zu triangles, %.4g mm2: %.3g mm RMS (%.3g max) within "
                "%.3g mm",
                r.region.triangles.size(), r.region.area, f.rms, f.max,
                r.toleranceMm);
  char used[200] = {};
  std::snprintf(used, sizeof used,
                "\nUsed %zu points (%s): sigma %.3g mm, form %.3g mm",
                r.region.used,
                r.clipSigma == 0   ? "all"
                : r.clipSigma == 1 ? "1 sigma"
                : r.clipSigma == 2 ? "2 sigma"
                                   : "3 sigma",
                j.value("sigma_mm", 0.0), j.value("form_mm", 0.0));
  char size[200] = {};
  if (j.contains("diameter_min_mm"))
    std::snprintf(size, sizeof size, "\nDiameter %.4g to %.4g mm",
                  j.value("diameter_min_mm", 0.0),
                  j.value("diameter_max_mm", 0.0));
  char extent[200] = {};
  if (f.kind == mesh::Kind::Cylinder)
    std::snprintf(extent, sizeof extent, "\n%.4g mm long, %.4g deg of arc",
                  j.value("length_mm", 0.0), j.value("arc_deg", 0.0));
  else if (f.kind == mesh::Kind::Sphere)
    std::snprintf(extent, sizeof extent, "\nA cap of %.4g deg",
                  j.value("arc_deg", 0.0));
  else if (f.kind == mesh::Kind::Plane && j.contains("extent_mm"))
    std::snprintf(extent, sizeof extent, "\n%.4g x %.4g mm",
                  j["extent_mm"][0].get<double>(),
                  j["extent_mm"][1].get<double>());
  return std::string(head) + tail + used + size + extent +
         (r.chosen ? "\n(auto chose it)" : "");
}

// The fit at the picked point with the window's kind and tolerance.
void Recompute(AppState& state) {
  auto& t = state.fitTool;
  t.error.clear();
  t.summary.clear();
  t.warning.clear();
  t.fittedKind = -1;
  t.fittedRadius = 0.0;
  if (!OnItsDocument(state) || !t.point)
    return;
  meshfit::Request rq;
  rq.pointMm = *t.point;
  rq.kind = AskedKind(state);
  if (t.ownTolerance)
    rq.toleranceMm = t.toleranceMm;
  rq.clipSigma = t.clipSigma;
  rq.hints = HintsOf(state);
  meshfit::Result res;
  std::string why;
  try {
    if (!meshfit::Fit(PickedBodies(state), rq, res, why)) {
      t.error = why;
      return;
    }
  } catch (const std::exception& e) {
    t.error = e.what();
    return;
  }
  t.summary = Summary(res);
  t.fittedKind = static_cast<int>(res.region.fit.kind);
  if (res.region.fit.kind == mesh::Kind::Cylinder ||
      res.region.fit.kind == mesh::Kind::Sphere)
    t.fittedRadius = res.region.fit.radius;
  if (const nlohmann::json j = meshfit::ResultJson(res); j.contains("warnings"))
    for (const auto& w : j.at("warnings"))
      t.warning += (t.warning.empty() ? "" : "\n") + w.get<std::string>();
  if (!t.ownTolerance)
    t.toleranceMm = res.toleranceMm; // what it used, shown and editable
  if (res.region.fit.kind == mesh::Kind::Sphere)
    t.error = "A sphere has no plane or axis for a datum to keep: its "
              "centre and radius are above (fit_primitive reports them). "
              "Pick a plane, cylinder or cone to make a datum.";
}

} // namespace

void OpenFitTool(AppState& state) {
  if (state.document == nullptr)
    return;
  bool any = false;
  for (const Body& b : state.document->GetBodies())
    any = any || !b.reference;
  if (!any)
    return;
  // One session at a time.
  CancelPendingFeature(state);
  CancelFitTool(state);
  state.editFeatureId = 0;
  auto& t = state.fitTool;
  t.open = true;
  t.document = state.document.get();
  LOG_INFO("tool: Fit started");
  Arm(state);
}

void CancelFitTool(AppState& state) {
  if (!state.fitTool.open)
    return;
  LOG_INFO("tool: Fit cancelled");
  Reset(state);
}

bool AcceptFitTool(AppState& state) {
  auto& t = state.fitTool;
  if (!OnItsDocument(state) || !t.point || t.fittedKind < 0 ||
      t.fittedKind == static_cast<int>(mesh::Kind::Sphere))
    return false;
  // A datum is one more feature: refused visibly at the part's limit.
  if (const std::string why = readctx::CheckFeatureRoom(*state.document, 1);
      !why.empty()) {
    t.error = why;
    return false;
  }
  meshfit::FitRef ref;
  ref.bodyFeatureId = t.bodyFeatureId;
  ref.pointMm = *t.point;
  ref.kind = static_cast<mesh::Kind>(t.fittedKind); // never auto
  if (t.ownTolerance)
    ref.toleranceMm = t.toleranceMm;
  // Written only when given, so a datum refits as it was fitted here.
  if (t.clipSigma != meshfit::kDefaultClipSigma)
    ref.clipSigma = t.clipSigma;
  ref.hints = HintsOf(state);
  if (ref.hints.NotFor(ref.kind) != nullptr)
    ref.hints = {}; // auto picked a kind the hints were not for
  std::unique_ptr<Feature> datum;
  if (ref.kind == mesh::Kind::Plane) {
    auto dp = std::make_unique<DatumPlaneFeature>("Fitted Plane",
                                                  DatumPlaneKind::FromFit);
    dp->fit = ref;
    datum = std::move(dp);
  } else {
    auto da = std::make_unique<DatumAxisFeature>("Fitted Axis",
                                                 DatumAxisKind::FromFit);
    da->fit = ref;
    datum = std::move(da);
  }
  LOG_INFO("tool: Fit accepted ({} on feature {})", mesh::KindName(ref.kind),
           ref.bodyFeatureId);
  Reset(state);
  state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(datum)),
                          *state.document);
  return true;
}

void FitPointPicked(AppState& state, const SelectionItem& item) {
  auto& t = state.fitTool;
  if (!OnItsDocument(state)) {
    state.slotPick = {};
    return;
  }
  if (item.kind != SelectionItem::Kind::Face || item.element.featureId == 0) {
    state.slotPick.miss = "that is not on a body: click a point on one";
    return;
  }
  t.bodyFeatureId = item.element.featureId;
  t.point = state.slotPick.hitPoint;
  state.slotPick = {};
  Recompute(state);
}

void DrawFitToolWindow(AppState& state) {
  auto& t = state.fitTool;
  if (!OnItsDocument(state)) {
    Reset(state); // the document went away
    return;
  }
  BeginToolWindowFrame(state, "Fit");
  int kind = t.kind;
  LeftLabel("Kind");
  ImGui::SetNextItemWidth(-1);
  if (ImGui::Combo("##fit_kind", &kind, kKindLabels, 5) && kind != t.kind) {
    t.kind = kind;
    Recompute(state);
  }
  {
    char text[160];
    const bool armed = state.slotPick.featureId == kFitToolSlot;
    if (t.point)
      std::snprintf(text, sizeof text, "Point: %.4g, %.4g, %.4g%s", t.point->x,
                    t.point->y, t.point->z, armed ? " (click again)" : "");
    else
      std::snprintf(text, sizeof text, "Point: %s",
                    armed ? "click it in the view" : "not picked");
    if (ImGui::Button((text + std::string("###fit_pt")).c_str()))
      Arm(state);
  }
  bool own = t.ownTolerance;
  if (ImGui::Checkbox("Own tolerance###fit_own_tol", &own)) {
    t.ownTolerance = own;
    Recompute(state);
  }
  if (t.ownTolerance) {
    LeftLabel("Tolerance (mm)");
    ImGui::SetNextItemWidth(-1);
    double tol = t.toleranceMm;
    if (ExprInputDouble(state, "##fit_tol", &tol, "%.4g") && tol > 0.0 &&
        std::isfinite(tol) && tol != t.toleranceMm) {
      t.toleranceMm = tol;
      Recompute(state);
    }
  } else {
    ImGui::TextDisabled("Tolerance: set by the surface's own noise");
  }
  // The clip and the hints, folded away: most fits need neither.
  if (ImGui::CollapsingHeader("Hints and clip###fit_hints")) {
    static constexpr const char* kClips[] = {"All points", "1 sigma", "2 sigma",
                                             "3 sigma"};
    int clip = t.clipSigma;
    LeftLabel("Clip");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##fit_clip", &clip, kClips, 4) && clip != t.clipSigma) {
      t.clipSigma = clip;
      Recompute(state);
    }
    const bool flat = AskedKind(state) == mesh::Kind::Plane;
    ImGui::BeginDisabled(flat);
    static constexpr const char* kSides[] = {"Either", "Inside (a bore)",
                                             "Outside (a boss)"};
    int side = t.side;
    LeftLabel("Side");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##fit_side", &side, kSides, 3) && side != t.side) {
      t.side = side;
      Recompute(state);
    }
    ImGui::EndDisabled();
    const bool round = AskedKind(state) != mesh::Kind::Plane &&
                       AskedKind(state) != mesh::Kind::Cone;
    ImGui::BeginDisabled(!round);
    bool on = t.radiusOn;
    if (ImGui::Checkbox("Radius range###fit_radius_on", &on)) {
      t.radiusOn = on;
      // Seeded from the fit it steers, a tenth either side.
      if (on && t.fittedRadius > 0.0 && !(t.radiusMax > 0.0)) {
        t.radiusMin = 0.9 * t.fittedRadius;
        t.radiusMax = 1.1 * t.fittedRadius;
      }
      Recompute(state);
    }
    if (t.radiusOn) {
      double lo = t.radiusMin, hi = t.radiusMax;
      LeftLabel("Radius from (mm)");
      ImGui::SetNextItemWidth(-1);
      if (ExprInputDouble(state, "##fit_rmin", &lo, "%.4g") && lo >= 0.0 &&
          std::isfinite(lo) && lo != t.radiusMin) {
        t.radiusMin = lo;
        Recompute(state);
      }
      LeftLabel("Radius to (mm)");
      ImGui::SetNextItemWidth(-1);
      if (ExprInputDouble(state, "##fit_rmax", &hi, "%.4g") && hi > 0.0 &&
          std::isfinite(hi) && hi != t.radiusMax) {
        t.radiusMax = hi;
        Recompute(state);
      }
    }
    ImGui::EndDisabled();
  }
  if (state.slotPick.featureId == kFitToolSlot && !state.slotPick.miss.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "%s",
                       state.slotPick.miss.c_str());
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
  if (!t.summary.empty())
    ImGui::TextDisabled("%s", t.summary.c_str());
  if (!t.warning.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "%s",
                       t.warning.c_str());
  if (!t.error.empty())
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", t.error.c_str());
  ImGui::PopTextWrapPos();
  ImGui::Separator();
  const bool ready = t.point.has_value() && t.fittedKind >= 0 &&
                     t.fittedKind != static_cast<int>(mesh::Kind::Sphere);
  if (!t.point)
    ImGui::TextDisabled("Pick a point to continue");
  else if (ready)
    ImGui::TextDisabled(t.fittedKind == static_cast<int>(mesh::Kind::Plane)
                            ? "Accept adds a datum plane fitted here"
                            : "Accept adds a datum axis fitted here");
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
    AcceptFitTool(state);
  else if (cancel)
    CancelFitTool(state);
}

} // namespace ui
