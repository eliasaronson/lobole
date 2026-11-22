// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ScanCompareTool.h"

#include "common/Logger.h"
#include "core/Document.h"
#include "core/ScanCompare.h"
#include "core/mesh/Segment.h" // ScanTolerance
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "ui/BackgroundJobs.h"
#include "ui/ExprInput.h"
#include "ui/LengthText.h"
#include "ui/PanelLayout.h" // LeftLabel
#include "ui/PreviewFeature.h"
#include "ui/SimulationView.h" // ShownField
#include "ui/ViewActions.h"    // FitCameraToWorldBox
#include "ui/Widgets.h"        // DrawJobProgress

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>

namespace ui {

// A comparison running on a `deviation` job: the worker writes the outcome,
// the job's finish lands it on the frame.
struct ScanCompareRun {
  std::shared_ptr<const scancompare::Input> input;
  std::shared_ptr<const scancompare::Outcome> outcome;
  std::string error;
};

namespace {

// The features that made a body, newest first: scans (mesh bodies), and
// what a scan is compared with (B-Reps, then other scans).
struct Candidates {
  std::vector<uint64_t> scans, bodies;
};

Candidates CandidatesOf(const Document& doc) {
  std::vector<uint64_t> meshes, breps;
  for (const Body& b : doc.GetBodies()) {
    if (b.createdBy == 0)
      continue;
    std::vector<uint64_t>& into = b.IsMesh() ? meshes : breps;
    if (std::find(into.begin(), into.end(), b.createdBy) == into.end())
      into.push_back(b.createdBy);
  }
  const auto newestFirst = [&](std::vector<uint64_t>& ids) {
    std::vector<std::pair<size_t, uint64_t>> at;
    for (uint64_t id : ids) {
      size_t index = 0;
      if (doc.FindFeatureById(id, &index) != nullptr)
        at.emplace_back(index, id);
    }
    std::sort(at.rbegin(), at.rend());
    ids.clear();
    for (const auto& [index, id] : at)
      ids.push_back(id);
  };
  newestFirst(meshes);
  newestFirst(breps);
  Candidates c;
  c.scans = meshes;
  c.bodies = breps;
  c.bodies.insert(c.bodies.end(), meshes.begin(), meshes.end());
  return c;
}

std::string NameOf(const Document& doc, uint64_t id) {
  const Feature* f = doc.FindFeatureById(id);
  return f != nullptr ? f->GetName() : std::string("(gone)");
}

void StopRun(AppState& state) {
  auto& t = state.scanCompareTool;
  if (t.jobId != 0)
    (void)CancelBackgroundJobById(state, t.jobId,
                                  "the Compare to scan tool moved on");
  t.jobId = 0;
  t.run.reset();
}

void Land(AppState& state, const std::shared_ptr<ScanCompareRun>& run) {
  auto& t = state.scanCompareTool;
  if (!t.open || t.run != run)
    return;
  t.jobId = 0;
  t.run.reset();
  if (!run->error.empty()) {
    t.error = run->error;
    return;
  }
  if (run->outcome == nullptr || run->outcome->deviation.cancelled) {
    t.error = "Cancelled: nothing was compared";
    return;
  }
  if (state.document.get() != t.document ||
      state.document->Version() != t.documentVersion) {
    t.error = "The part changed while it compared: compare again";
    return;
  }
  ShowScanDeviation(state, run->input, run->outcome);
  t.selected = -1;
  LOG_INFO("tool: Compare to scan: {:.1f}% within {} mm, {} hotspots",
           100.0 * run->outcome->deviation.within,
           run->outcome->deviation.toleranceMm,
           run->outcome->deviation.hotspots.size());
}

// The comparison of the tool's pair, started on a `deviation` job.
void Start(AppState& state) {
  auto& t = state.scanCompareTool;
  StopRun(state);
  t.error.clear();
  ClearScanDeviation(state);
  if (state.document == nullptr || t.scanId == 0 || t.bodyId == 0)
    return;
  scancompare::Request rq;
  rq.scanId = t.scanId;
  rq.bodyId = t.bodyId;
  if (t.ownTolerance)
    rq.toleranceMm = t.toleranceMm;
  auto input = std::make_shared<scancompare::Input>();
  if (const std::string why = scancompare::Gather(*state.document, rq, *input);
      !why.empty()) {
    t.error = why;
    return;
  }
  t.documentVersion = state.document->Version();
  auto run = std::make_shared<ScanCompareRun>();
  run->input = input;
  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "deviation";
  bg->label =
      "Comparing '" + input->bodyName + "' to '" + input->scanName + "'";
  bg->cancellable = true;
  bg->fraction.store(0.0);
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([run, raw]() {
    try {
      run->outcome = std::make_shared<const scancompare::Outcome>(
          scancompare::Run(*run->input, &raw->cancel, &raw->fraction));
    } catch (const std::exception& e) {
      run->error = e.what();
    }
    raw->done.store(true); // last
  });
  bg->finish = [run](AppState& s) {
    Land(s, run);
    return true;
  };
  t.run = run;
  t.jobId = StartBackgroundJob(state, std::move(bg)).id;
  LOG_INFO("tool: Compare to scan '{}' against '{}' started", input->scanName,
           input->bodyName);
}

// 1, 2, 2.5 or 5 times a power of ten, at least `v`.
double NiceAbove(double v) {
  if (!(v > 0.0))
    return 1.0;
  const double p = std::pow(10.0, std::floor(std::log10(v)));
  for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
    if (m * p >= v * (1.0 - 1e-9))
      return m * p;
  return 10.0 * p;
}

} // namespace

unsigned int DivergingColour(float t) {
  // Moreland's cool-warm: the shader's twin is field.frag's diverging().
  static const float kStops[5][3] = {{0.230f, 0.299f, 0.754f},
                                     {0.552f, 0.690f, 0.996f},
                                     {0.865f, 0.865f, 0.865f},
                                     {0.958f, 0.604f, 0.482f},
                                     {0.706f, 0.016f, 0.150f}};
  t = std::clamp(t, 0.0f, 1.0f) * 4.0f;
  const int i = std::min(3, int(t));
  const float f = t - float(i);
  const auto ch = [&](int k) {
    return int(255.0f * (kStops[i][k] + f * (kStops[i + 1][k] - kStops[i][k])));
  };
  return IM_COL32(ch(0), ch(1), ch(2), 255);
}

std::string ScanCompareRefusal(const AppState& state) {
  if (state.document == nullptr)
    return "Open a part";
  const Candidates c = CandidatesOf(*state.document);
  if (c.scans.empty())
    return "No scan: import one as a mesh body (Import, representation "
           "Mesh)";
  if (c.bodies.size() < 2)
    return "Nothing to compare the scan with: build or import the model "
           "beside it";
  return "";
}

bool HasScanToCompare(const AppState& state) {
  return ScanCompareRefusal(state).empty();
}

void OpenScanCompareTool(AppState& state) {
  if (!HasScanToCompare(state))
    return;
  CancelPendingFeature(state); // one session at a time
  CancelScanCompareTool(state);
  state.editFeatureId = 0;
  const Candidates c = CandidatesOf(*state.document);
  auto& t = state.scanCompareTool;
  t = AppState::ScanCompareTool{};
  t.open = true;
  t.document = state.document.get();
  // A scan and a body picked first are taken: a selected mesh body is the
  // scan, another selected body the body.
  for (const SelectionItem& item : state.selection.items) {
    const uint64_t id = item.kind == SelectionItem::Kind::Face ||
                                item.kind == SelectionItem::Kind::Edge
                            ? item.element.featureId
                            : item.id;
    if (t.scanId == 0 &&
        std::find(c.scans.begin(), c.scans.end(), id) != c.scans.end())
      t.scanId = id;
    else if (t.bodyId == 0 && id != t.scanId &&
             std::find(c.bodies.begin(), c.bodies.end(), id) != c.bodies.end())
      t.bodyId = id;
  }
  if (t.scanId == 0)
    t.scanId = c.scans.front();
  for (uint64_t id : c.bodies)
    if (t.bodyId == 0 && id != t.scanId)
      t.bodyId = id;
  LOG_INFO("tool: Compare to scan opened on '{}' against '{}'",
           NameOf(*state.document, t.scanId),
           NameOf(*state.document, t.bodyId));
  Start(state);
}

void CloseScanCompareTool(AppState& state) {
  if (!state.scanCompareTool.open)
    return;
  StopRun(state);
  state.scanCompareTool = AppState::ScanCompareTool{};
}

void CancelScanCompareTool(AppState& state) {
  if (!state.scanCompareTool.open)
    return;
  CloseScanCompareTool(state);
  ClearScanDeviation(state);
}

void ShowScanDeviation(AppState& state,
                       std::shared_ptr<const scancompare::Input> input,
                       std::shared_ptr<const scancompare::Outcome> outcome) {
  auto& v = state.scanDeviation;
  v.input = std::move(input);
  v.outcome = std::move(outcome);
  v.document = state.document.get();
  v.documentVersion = state.document != nullptr ? state.document->Version() : 0;
  v.rangeMm = 0.0;
  v.serial++;
}

void ClearScanDeviation(AppState& state) {
  auto& v = state.scanDeviation;
  if (v.outcome == nullptr && v.input == nullptr)
    return;
  const uint64_t serial = v.serial + 1;
  v = AppState::ScanDeviation{};
  v.serial = serial;
}

double ScanDeviationRangeMm(const AppState& state) {
  const auto& v = state.scanDeviation;
  if (v.rangeMm > 0.0)
    return v.rangeMm;
  if (v.outcome == nullptr)
    return 1.0;
  const mesh::Deviation& d = v.outcome->deviation;
  std::vector<float> mag;
  mag.reserve(d.distanceMm.size());
  for (float x : d.distanceMm)
    if (!std::isnan(x))
      mag.push_back(std::abs(x));
  double p99 = 0.0;
  if (!mag.empty()) {
    const size_t k =
        std::min(mag.size() - 1, size_t(0.99 * double(mag.size())));
    std::nth_element(mag.begin(), mag.begin() + long(k), mag.end());
    p99 = mag[k];
  }
  return NiceAbove(std::max(2.0 * d.toleranceMm, p99));
}

bool BuildScanDeviationOverlay(AppState& state,
                               std::vector<FieldVertex>& vertices,
                               std::vector<uint32_t>& indices,
                               FieldStyle& style, ShownField& shown,
                               std::vector<int>& hide) {
  auto& v = state.scanDeviation;
  if (v.outcome == nullptr || v.input == nullptr)
    return false;
  if (state.document == nullptr || state.document.get() != v.document ||
      state.document->Version() != v.documentVersion) {
    ClearScanDeviation(state); // an edit, or another tab: it is not this
    return false;
  }
  const scancompare::Input& in = *v.input;
  const mesh::Deviation& d = v.outcome->deviation;
  const mesh::TriMesh& scan = *in.scan;
  const double range = ScanDeviationRangeMm(state);
  vertices.resize(scan.positions.size());
  for (size_t i = 0; i < scan.positions.size(); i++) {
    FieldVertex& fv = vertices[i];
    fv.position = glm::vec3(scan.positions[i]);
    fv.displacement = glm::vec3(0.0f);
    const float x = i < d.distanceMm.size()
                        ? d.distanceMm[i]
                        : std::numeric_limits<float>::quiet_NaN();
    // No body within reach: under 0, the plain grey (FieldStyle::diverging).
    fv.scalar = std::isnan(x) ? -1.0f
                              : std::clamp(float((x + range) / (2.0 * range)),
                                           0.0f, 1.0f);
    fv.edge = glm::vec3(1.0f);
  }
  indices.clear();
  indices.reserve(scan.triangles.size() * 3);
  for (const auto& t : scan.triangles)
    indices.insert(indices.end(), {t[0], t[1], t[2]});
  style = FieldStyle{};
  style.lit = true;
  style.diverging = true;
  const units::System sys = state.ActiveUnits();
  const double per = units::MmPerLength(sys.length);
  shown = ShownField{};
  shown.title = "Deviation";
  shown.unit = units::ToString(sys.length);
  shown.barLo = -range / per;
  shown.barHi = range / per;
  shown.lo = d.minMm / per;
  shown.hi = d.maxMm / per;
  shown.diverging = true;
  shown.bandLo = -d.toleranceMm / per;
  shown.bandHi = d.toleranceMm / per;
  shown.subject = in.scanName + " against " + in.bodyName;
  char buf[160];
  std::snprintf(buf, sizeof buf, "%.1f%% within \xc2\xb1%s", 100.0 * d.within,
                ReadableLength(d.toleranceMm, sys).c_str());
  shown.notes.push_back(buf);
  if (d.noScan > 0.02) {
    std::snprintf(buf, sizeof buf, "%.0f%% of the body has no scan",
                  100.0 * d.noScan);
    shown.notes.push_back(buf);
  }
  shown.key = v.serial * 1000003ull + uint64_t(std::llround(range * 1e6));
  // The scan's own render, which the map stands in for, and the body's,
  // which lies on it within the tolerance and would show through it
  // wherever the two meet.
  hide = in.scanBodies;
  hide.insert(hide.end(), in.bodyBodies.begin(), in.bodyBodies.end());
  return true;
}

void FrameScanHotspot(AppState& state, size_t index) {
  const auto& v = state.scanDeviation;
  if (v.outcome == nullptr || state.camera == nullptr ||
      index >= v.outcome->deviation.hotspots.size())
    return;
  const mesh::Deviation& d = v.outcome->deviation;
  const mesh::DeviationHotspot& h = d.hotspots[index];
  // The scan's normal at the peak: its triangles' there, summed.
  const mesh::TriMesh& scan = *v.input->scan;
  glm::dvec3 n(0.0);
  for (uint32_t t = 0; t < scan.triangles.size(); t++)
    for (int c = 0; c < 3; c++)
      if (scan.positions[scan.triangles[t][c]] == h.atMm)
        n += scan.AreaNormal(t);
  n = glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0, 0, 1);
  // The patch and a margin round it, so its place on the part shows.
  const glm::dvec3 centre = 0.5 * (h.lo + h.hi);
  const double size = std::max(glm::length(h.hi - h.lo), 20.0 * d.spacingMm);
  const glm::dvec3 grow(0.6 * size);
  Camera& cam = *state.camera;
  cam.StopAnimation();
  glm::dvec3 up(0.0, 0.0, 1.0);
  if (std::abs(glm::dot(up, n)) > 0.9)
    up = glm::dvec3(0.0, 1.0, 0.0);
  // A little off the normal, so a flat patch reads as lying on the part.
  const glm::dvec3 side = glm::normalize(glm::cross(n, up));
  const glm::dvec3 look = glm::normalize(n + 0.35 * side + 0.25 * up);
  cam.SetTarget(glm::vec3(centre));
  cam.SetUp(glm::vec3(up));
  cam.SetPosition(glm::vec3(centre + 3.0 * size * look));
  FitCameraToWorldBox(state, glm::vec3(centre - grow),
                      glm::vec3(centre + grow));
}

void DrawScanDeviationLegendRows(AppState& state) {
  auto& v = state.scanDeviation;
  if (v.outcome == nullptr)
    return;
  const units::System sys = state.ActiveUnits();
  const double per = units::MmPerLength(sys.length);
  const std::string label =
      std::string("Range \xc2\xb1 (") + units::ToString(sys.length) + ")";
  ImGui::TextUnformatted(label.c_str());
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  double r = ScanDeviationRangeMm(state) / per;
  if (ExprInputDouble(state, "##scan_dev_range", &r, "%.4g") && r > 0.0 &&
      std::isfinite(r))
    v.rangeMm = r * per;
  ImGui::SameLine();
  ImGui::BeginDisabled(!(v.rangeMm > 0.0));
  if (ImGui::Button("Auto###scan_dev_auto"))
    v.rangeMm = 0.0;
  ImGui::EndDisabled();
  if (ImGui::Button("Clear the map###scan_dev_clear")) {
    if (state.scanCompareTool.open)
      CancelScanCompareTool(state);
    else
      ClearScanDeviation(state);
    ImGui::CloseCurrentPopup();
  }
}

void DrawScanCompareToolWindow(AppState& state) {
  auto& t = state.scanCompareTool;
  if (state.document == nullptr || state.document.get() != t.document) {
    CancelScanCompareTool(state); // the tab went away
    return;
  }
  Document& doc = *state.document;
  const units::System sys = state.ActiveUnits();
  BeginToolWindowFrame(state, "Compare to scan");
  const Candidates c = CandidatesOf(doc);
  const auto picker = [&](const char* label, const char* id, uint64_t& value,
                          const std::vector<uint64_t>& ids, uint64_t other) {
    LeftLabel(label);
    ImGui::SetNextItemWidth(-1);
    const std::string now = value != 0 ? NameOf(doc, value) : "(none)";
    bool changed = false;
    if (ImGui::BeginCombo(id, now.c_str())) {
      for (uint64_t fid : ids) {
        if (fid == other)
          continue;
        ImGui::PushID(static_cast<int>(fid));
        if (ImGui::Selectable(NameOf(doc, fid).c_str(), fid == value) &&
            fid != value) {
          value = fid;
          changed = true;
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    return changed;
  };
  bool again = picker("Scan", "##scan_cmp_scan", t.scanId, c.scans, 0);
  if (t.bodyId == t.scanId)
    t.bodyId = 0;
  again |= picker("Body", "##scan_cmp_body", t.bodyId, c.bodies, t.scanId);
  bool own = t.ownTolerance;
  if (ImGui::Checkbox("Own tolerance###scan_cmp_own_tol", &own)) {
    t.ownTolerance = own;
    // Seeded from what the scan's noise gave, never a literal.
    if (own && !(t.toleranceMm > 0.0) && state.scanDeviation.outcome)
      t.toleranceMm = state.scanDeviation.outcome->deviation.toleranceMm;
    if (own && !(t.toleranceMm > 0.0)) {
      scancompare::Input probe;
      scancompare::Request rq{t.scanId, t.bodyId, std::nullopt};
      if (scancompare::Gather(doc, rq, probe).empty())
        t.toleranceMm = mesh::ScanTolerance(*probe.scan);
    }
    again = true;
  }
  if (t.ownTolerance) {
    LeftLabel(("Tolerance (" + std::string(units::ToString(sys.length)) + ")")
                  .c_str());
    ImGui::SetNextItemWidth(-1);
    const double per = units::MmPerLength(sys.length);
    double tol = t.toleranceMm / per;
    if (ExprInputDouble(state, "##scan_cmp_tol", &tol, "%.4g") && tol > 0.0 &&
        std::isfinite(tol) && tol * per != t.toleranceMm) {
      t.toleranceMm = tol * per;
      again = true;
    }
  }
  if (again)
    Start(state);

  if (BackgroundJob* job =
          t.jobId != 0 ? FindBackgroundJobById(state, t.jobId) : nullptr) {
    JobProgress bar;
    bar.status = job->label;
    bar.fraction = job->fraction.load();
    bar.cancellable = job->cancellable;
    bar.cancelAsked = job->cancel.load();
    bar.cancelId = "###scan_cmp_cancel";
    bar.cancelTip = "Stop comparing";
    if (DrawJobProgress(bar))
      (void)CancelBackgroundJobById(state, t.jobId, "cancelled in the tool");
  }
  const float wrap = 340.0f;
  if (!t.error.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "%s", t.error.c_str());
    ImGui::PopTextWrapPos();
  }
  const auto& v = state.scanDeviation;
  if (v.outcome != nullptr && t.jobId == 0) {
    const mesh::Deviation& d = v.outcome->deviation;
    ImGui::Separator();
    // The headline: how much of the scan is inside, then by how much the
    // rest is out.
    ImGui::Text("%.1f%% within \xc2\xb1%s", 100.0 * d.within,
                ReadableLength(d.toleranceMm, sys).c_str());
    ImGui::SameLine();
    const char* from = d.toleranceFrom == "asked"   ? "as set"
                       : d.toleranceFrom == "chord" ? "the body's facets"
                                                    : "the scan's noise";
    ImGui::TextDisabled("(%s)", from);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The scan's noise is %s; the body's facets stand off "
                        "it by up to %s, the floor under the map",
                        ReadableLength(d.noiseMm, sys).c_str(),
                        ReadableLength(d.chordMm, sys).c_str());
    ImGui::TextDisabled("Mean %s, from %s to %s",
                        ReadableLength(d.meanMm, sys, true).c_str(),
                        ReadableLength(d.minMm, sys, true).c_str(),
                        ReadableLength(d.maxMm, sys, true).c_str());
    if (d.noScan > 0.02)
      ImGui::TextDisabled("%.0f%% of the body has no scan under it",
                          100.0 * d.noScan);

    if (d.hotspots.empty()) {
      ImGui::TextDisabled("Nothing past the tolerance");
    } else {
      size_t features = 0;
      for (const auto& h : d.hotspots)
        features += h.feature ? 1 : 0;
      ImGui::Spacing();
      ImGui::TextUnformatted("Past the tolerance");
      ImGui::SameLine();
      if (features == d.hotspots.size())
        ImGui::TextDisabled("%zu", d.hotspots.size());
      else
        ImGui::TextDisabled("%zu, %zu small", d.hotspots.size(),
                            d.hotspots.size() - features);
      const ImGuiTableFlags flags =
          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerH;
      const float rowH = ImGui::GetTextLineHeightWithSpacing();
      const float height =
          rowH * float(std::min<size_t>(d.hotspots.size(), 8) + 1) + 6.0f;
      if (ImGui::BeginTable("##scan_cmp_hotspots", 4, flags,
                            ImVec2(wrap, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::GetTextLineHeight());
        ImGui::TableSetupColumn("Depth");
        ImGui::TableSetupColumn("Area");
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        const double range = ScanDeviationRangeMm(state);
        for (size_t i = 0; i < d.hotspots.size(); i++) {
          const mesh::DeviationHotspot& h = d.hotspots[i];
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          // The patch's colour on the map, as a swatch.
          const ImVec2 at = ImGui::GetCursorScreenPos();
          const float s = ImGui::GetTextLineHeight() - 4.0f;
          ImGui::GetWindowDrawList()->AddRectFilled(
              ImVec2(at.x + 1, at.y + 2), ImVec2(at.x + 1 + s, at.y + 2 + s),
              DivergingColour(float((h.depthMm + range) / (2.0 * range))),
              2.0f);
          ImGui::TableNextColumn();
          const std::string label = ReadableLength(h.depthMm, sys, true) +
                                    "###scan_cmp_hot" + std::to_string(i);
          if (ImGui::Selectable(label.c_str(), t.selected == int(i),
                                ImGuiSelectableFlags_SpanAllColumns)) {
            t.selected = int(i);
            FrameScanHotspot(state, i);
          }
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Show it: the view turns to look at it");
          ImGui::TableNextColumn();
          const double per = units::MmPerLength(sys.length);
          const std::string area =
              units::FormatNumber(h.areaMm2 / (per * per), 3) + " " +
              units::AreaSuffix(sys);
          ImGui::TextDisabled("%s", area.c_str());
          ImGui::TableNextColumn();
          std::string on = "the scan";
          if (h.face != mesh::kNone && h.face < v.input->faces.size()) {
            const scancompare::FaceRef& f = v.input->faces[h.face];
            on = f.face >= 0 ? "face " + std::to_string(f.face + 1)
                             : v.input->bodyName;
          }
          if (!h.feature)
            on += ", small";
          ImGui::TextDisabled("%s", on.c_str());
        }
        ImGui::EndTable();
      }
    }
  }

  ImGui::Separator();
  // Done keeps the map; Clear takes it away with the tool.
  ImGui::PushStyleColor(ImGuiCol_Button,
                        ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  const bool done = ImGui::Button("Done###tw_accept");
  ImGui::PopStyleColor();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Close, keeping the map");
  ImGui::SameLine();
  const bool clear = ImGui::Button("Clear###tw_cancel");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Close and take the map away");
  ImGui::End();
  if (done) {
    LOG_INFO("tool: Compare to scan closed, the map kept");
    CloseScanCompareTool(state);
  } else if (clear) {
    LOG_INFO("tool: Compare to scan cleared");
    CancelScanCompareTool(state);
  }
}

} // namespace ui
