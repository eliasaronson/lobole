#include "render/Camera.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/Command.h"
#include "core/io/Serialization.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (BC glyphs)

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ui {

namespace {

const ImVec4 kError(0.95f, 0.5f, 0.4f, 1.0f);
const ImVec4 kStale(0.95f, 0.75f, 0.35f, 1.0f);
const ImVec4 kMuted(0.6f, 0.6f, 0.6f, 1.0f);

// Runs one study edit as an undoable ModifyStudyCommand: snapshot before,
// apply `mutate` to a copy, snapshot after, execute. Mirrors the RPC path.
void ModifyStudy(AppState& state, uint64_t id, const std::string& desc,
                 const std::function<void(fem::Study&)>& mutate) {
  Document* doc = state.document.get();
  const fem::Study* cur = doc->FindStudy(id);
  if (!cur || !state.commands) return;
  nlohmann::json before = DocumentIO::StudyToJson(*cur);
  fem::Study copy = *cur;
  mutate(copy);
  nlohmann::json after = DocumentIO::StudyToJson(copy);
  state.commands->Execute(
      std::make_unique<ModifyStudyCommand>(desc, before, after), *doc);
}

const char* LoadLabel(fem::LoadType t) {
  switch (t) {
    case fem::LoadType::Fixed: return "Fixed";
    case fem::LoadType::Force: return "Force";
    case fem::LoadType::Pressure: return "Pressure";
  }
  return "?";
}

void DrawStudyList(AppState& state) {
  Document* doc = state.document.get();
  SimulationState& sim = state.simulation;

  ImGui::SeparatorText("Studies");
  if (ImGui::Button(ICON_FA_PLUS " Add study")) {
    auto cmd = std::make_unique<AddStudyCommand>("");
    AddStudyCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), *doc);
    sim.activeStudyId = raw->StudyId();
  }

  for (const fem::Study& s : doc->GetStudies()) {
    ImGui::PushID(static_cast<int>(s.id));
    if (ImGui::Selectable(s.name.c_str(), sim.activeStudyId == s.id))
      sim.activeStudyId = s.id;
    ImGui::PopID();
  }
}

void DrawLoads(AppState& state, const fem::Study& s) {
  SimulationState& sim = state.simulation;

  ImGui::SeparatorText("Boundary conditions");
  ImGui::TextDisabled("Arm a tool, then click a face in the viewport:");
  auto arm = [&](const char* icon, const char* label, SimulationState::Tool t) {
    const bool armed = sim.tool == t;
    if (armed)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(label))
      sim.tool = armed ? SimulationState::Tool::None : t;
    if (armed) ImGui::PopStyleColor();
  };
  arm(ICON_FA_LOCK, ICON_FA_LOCK " Fixed", SimulationState::Tool::Fixed);
  ImGui::SameLine();
  arm(ICON_FA_BOLT, ICON_FA_BOLT " Force", SimulationState::Tool::Force);
  ImGui::SameLine();
  arm(ICON_FA_DOWN_LONG, ICON_FA_DOWN_LONG " Pressure",
      SimulationState::Tool::Pressure);

  // Pending magnitude/direction applied to the next placed force/pressure.
  if (sim.tool == SimulationState::Tool::Force ||
      sim.tool == SimulationState::Tool::Pressure) {
    ImGui::InputDouble(sim.tool == SimulationState::Tool::Force ? "Magnitude (N)"
                                                                : "Pressure (MPa)",
                       &sim.pendingMagnitude, 0.0, 0.0, "%.1f");
    if (sim.tool == SimulationState::Tool::Force) {
      float dir[3] = {(float)sim.pendingDirection.x, (float)sim.pendingDirection.y,
                      (float)sim.pendingDirection.z};
      if (ImGui::InputFloat3("Direction", dir))
        sim.pendingDirection = glm::dvec3(dir[0], dir[1], dir[2]);
    }
  }

  if (s.loads.empty()) {
    ImGui::TextColored(kMuted, "No loads yet.");
    return;
  }
  for (const fem::Load& l : s.loads) {
    ImGui::PushID(static_cast<int>(l.id));
    std::string line = std::string(LoadLabel(l.type)) + "  " + l.face.name;
    if (l.type != fem::LoadType::Fixed) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "  (%.1f%s)", l.magnitude,
                    l.type == fem::LoadType::Force ? " N" : " MPa");
      line += buf;
    }
    ImGui::TextUnformatted(line.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_TRASH)) {
      const uint64_t loadId = l.id;
      ModifyStudy(state, s.id, "Remove load", [&](fem::Study& cs) {
        cs.loads.erase(std::remove_if(cs.loads.begin(), cs.loads.end(),
                                      [&](const fem::Load& x) { return x.id == loadId; }),
                       cs.loads.end());
      });
      ImGui::PopID();
      break; // list mutated; rebuild next frame
    }
    ImGui::PopID();
  }
}

// Topology results: an async Run (worker thread) with a live progress bar +
// Cancel, then the optimization summary and the Shape/Density display controls.
void DrawTopoResults(AppState& state, const fem::Study& s) {
  Document* doc = state.document.get();
  SimulationState& sim = state.simulation;
  ImGui::SeparatorText("Optimization");

  // Store a finished run (the GUI's per-frame join point).
  PollTopologyRun(state, *doc);
  const TopoRunStatus st = GetTopologyStatus(state, s.id);
  if (st.running) {
    const float frac = st.maxIterations > 0
                           ? std::clamp(static_cast<float>(st.iteration) /
                                            st.maxIterations,
                                        0.0f, 1.0f)
                           : 0.0f;
    char ov[48];
    std::snprintf(ov, sizeof(ov), "iter %d / %d", st.iteration, st.maxIterations);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), ov);
    ImGui::Text("Compliance: %.4g   Vf: %.3f", st.compliance, st.volumeFraction);
    ImGui::TextColored(kMuted, "%s   %.0fs", st.solver.c_str(),
                       st.elapsedSeconds);
    if (ImGui::Button(ICON_FA_STOP " Cancel")) CancelTopologyRun(state, s.id);
    return;
  }

  if (ImGui::Button(ICON_FA_PLAY " Run")) {
    std::string err;
    if (!StartTopologyRun(state, *doc, s.id, &err))
      LOG_WARN("topology run failed: {}", err);
  }
  if (!s.error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kError, ICON_FA_TRIANGLE_EXCLAMATION);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.error.c_str());
  }

  const fem::StudyRuntime* rt = doc->StudyResults(s.id);
  if (!rt) {
    ImGui::TextColored(kMuted, "Not run yet.");
    return;
  }
  if (doc->StudyResultsStale(s.id))
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION
                       " Stale (model changed since last run)");

  size_t nodes = 0, elements = 0;
  for (const auto& b : rt->bodies) {
    nodes += b.mesh.stats.nodeCount;
    elements += b.mesh.stats.tetCount;
  }
  ImGui::Text("Mesh: %zu nodes / %zu elements", nodes, elements);
  ImGui::Text("Iterations: %d%s", rt->iterations,
              rt->converged ? " (converged)"
                            : rt->cancelled ? " (cancelled)" : "");
  ImGui::Text("Volume fraction: %.3f", rt->volumeFractionAchieved);
  if (!rt->solver.empty())  // transient provenance; empty on a reloaded result
    ImGui::TextColored(kMuted, "Solver: %s (%.0f s)", rt->solver.c_str(),
                       rt->solveSeconds);
  if (!rt->complianceHistory.empty()) {
    ImGui::Text("Compliance: %.4g -> %.4g", rt->complianceHistory.front(),
                rt->complianceHistory.back());
    std::vector<float> hv(rt->complianceHistory.begin(),
                          rt->complianceHistory.end());
    ImGui::PlotLines("##compliance", hv.data(), static_cast<int>(hv.size()), 0,
                     nullptr, FLT_MAX, FLT_MAX, ImVec2(-FLT_MIN, 40));
  }

  // Display: the grey iso-surface (Shape), the density colormap (Density), or
  // the normal model (Off), plus the iso-surface threshold.
  ImGui::SeparatorText("Display");
  int field = sim.field == SimulationState::Field::Shape     ? 1
              : sim.field == SimulationState::Field::Density ? 2
                                                             : 0;
  bool changed = ImGui::RadioButton("Off", &field, 0);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("Shape", &field, 1);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("Density", &field, 2);
  if (changed)
    sim.field = field == 1   ? SimulationState::Field::Shape
                : field == 2 ? SimulationState::Field::Density
                             : SimulationState::Field::None;
  float thr = sim.topoThreshold;
  if (ImGui::SliderFloat("Threshold (0=auto)", &thr, 0.0f, 1.0f, "%.2f"))
    sim.topoThreshold = thr;

  // Shape guide: show the optimized shape translucent in the modeling
  // workspace (visible after exiting simulation mode) to re-model against.
  bool ghost = state.topoGhostStudyId == s.id;
  if (ImGui::Checkbox("Shape guide (modeling overlay)", &ghost)) {
    state.topoGhostStudyId = ghost ? s.id : 0;
    state.syncedDocumentVersion = UINT64_MAX;  // rebuild renderers
  }
}

void DrawResults(AppState& state, const fem::Study& s) {
  Document* doc = state.document.get();
  SimulationState& sim = state.simulation;

  if (s.type == fem::AnalysisType::Topology) {
    DrawTopoResults(state, s);
    return;
  }

  ImGui::SeparatorText("Results");
  if (ImGui::Button(ICON_FA_PLAY " Run")) {
    std::string err;
    if (!RunStudyAndCache(*doc, s.id, &err))
      LOG_WARN("run study failed: {}", err);
  }
  if (!s.error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kError, ICON_FA_TRIANGLE_EXCLAMATION);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.error.c_str());
  }

  const fem::StudyRuntime* rt = doc->StudyResults(s.id);
  if (!rt) {
    ImGui::TextColored(kMuted, "Not run yet.");
    return;
  }
  if (doc->StudyResultsStale(s.id))
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION
                       " Stale (model changed since last run)");

  size_t nodes = 0, elements = 0;
  for (const auto& b : rt->bodies) {
    nodes += b.mesh.stats.nodeCount;
    elements += b.mesh.stats.tetCount;
  }
  ImGui::Text("Mesh: %zu nodes / %zu elements", nodes, elements);

  if (s.type == fem::AnalysisType::Modal) {
    // Natural frequencies: a selectable list; picking one shows that mode
    // shape in the viewport.
    ImGui::SeparatorText("Natural frequencies");
    const fem::StudyBodyResult* b = nullptr;
    for (const auto& br : rt->bodies)
      if (!br.results.modes.empty()) { b = &br; break; }
    if (b) {
      for (int i = 0; i < (int)b->results.modes.size(); ++i) {
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "Mode %d:  %.4g Hz", i + 1,
                      b->results.modes[i].frequencyHz);
        if (ImGui::Selectable(lbl, sim.activeMode == i)) {
          sim.activeMode = i;
          sim.field = SimulationState::Field::Displacement;  // show the shape
        }
      }
    }
    ImGui::SeparatorText("Display");
    bool showing = sim.field != SimulationState::Field::None;
    if (ImGui::Checkbox("Show mode shape", &showing))
      sim.field = showing ? SimulationState::Field::Displacement
                          : SimulationState::Field::None;
    ImGui::SliderFloat("Deform", &sim.deformScale, 0.0f, 100.0f, "%.0fx");
    return;
  }

  ImGui::Text("Max von Mises: %.3g MPa", rt->maxVonMises);
  ImGui::Text("Max displacement: %.4g mm", rt->maxDisplacement);

  // Field overlay controls (display only; no command). Each RadioButton must
  // draw every frame, so evaluate them independently (no short-circuit).
  ImGui::SeparatorText("Display");
  int field = static_cast<int>(sim.field);
  bool changed = ImGui::RadioButton("Off", &field, 0);
  ImGui::SameLine();
  changed |= ImGui::RadioButton("von Mises", &field,
                                static_cast<int>(SimulationState::Field::VonMises));
  ImGui::SameLine();
  changed |= ImGui::RadioButton(
      "Displacement", &field, static_cast<int>(SimulationState::Field::Displacement));
  if (changed) sim.field = static_cast<SimulationState::Field>(field);
  ImGui::SliderFloat("Deform", &sim.deformScale, 0.0f, 100.0f, "%.0fx");
}

}  // namespace

void DrawStudyPanel(AppState& state) {
  ImGui::Begin("Model");
  Document* doc = state.document.get();
  if (!doc) {
    ImGui::End();
    return;
  }

  DrawStudyList(state);

  fem::Study* s = doc->FindStudy(state.simulation.activeStudyId);
  if (!s) {
    ImGui::TextColored(kMuted, "Select or add a study.");
    ImGui::End();
    return;
  }

  ImGui::Spacing();
  char name[128];
  std::snprintf(name, sizeof(name), "%s", s->name.c_str());
  if (ImGui::InputText("Name", name, sizeof(name),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    const std::string newName = name;
    ModifyStudy(state, s->id, "Rename study",
                [&](fem::Study& cs) { cs.name = newName; });
  }
  // Analysis type: Static (stress), Modal (frequencies), or Topology
  // (generative shape).
  const char* kTypes[] = {"Static", "Modal", "Topology"};
  int typeIdx = s->type == fem::AnalysisType::Modal      ? 1
                : s->type == fem::AnalysisType::Topology ? 2
                                                         : 0;
  if (ImGui::Combo("Analysis", &typeIdx, kTypes, 3)) {
    const auto nt = typeIdx == 1   ? fem::AnalysisType::Modal
                    : typeIdx == 2 ? fem::AnalysisType::Topology
                                   : fem::AnalysisType::Static;
    ModifyStudy(state, s->id, "Set analysis type",
                [&](fem::Study& cs) { cs.type = nt; });
  }
  // Numeric fields commit on deactivation (Enter OR clicking away), not only on
  // Enter — so typing a value then clicking Run applies it instead of reverting.
  double meshSize = s->meshSize;
  ImGui::InputDouble("Mesh size (mm, 0=auto)", &meshSize, 0.0, 0.0, "%.3g");
  if (ImGui::IsItemDeactivatedAfterEdit() && meshSize != s->meshSize)
    ModifyStudy(state, s->id, "Set mesh size", [&](fem::Study& cs) {
      cs.meshSize = meshSize;
      cs.meshSizeExpr.clear();
    });

  if (s->type == fem::AnalysisType::Modal) {
    int nm = s->numModes;
    const bool nmChanged = ImGui::InputInt("Modes", &nm);
    if ((nmChanged || ImGui::IsItemDeactivatedAfterEdit()) && nm >= 1 &&
        nm != s->numModes)
      ModifyStudy(state, s->id, "Set mode count",
                  [&](fem::Study& cs) { cs.numModes = nm; });
  } else if (s->type == fem::AnalysisType::Topology) {
    // Topology: target volume, minimum-member (filter) size, iteration cap.
    float vf = static_cast<float>(s->volumeFraction);
    if (ImGui::SliderFloat("Volume fraction", &vf, 0.05f, 0.9f, "%.2f"))
      ModifyStudy(state, s->id, "Set volume fraction", [&](fem::Study& cs) {
        cs.volumeFraction = vf;
        cs.volumeFractionExpr.clear();
      });
    double fr = s->filterRadius;
    ImGui::InputDouble("Filter radius (mm, 0=auto)", &fr, 0.0, 0.0, "%.3g");
    if (ImGui::IsItemDeactivatedAfterEdit() && fr != s->filterRadius)
      ModifyStudy(state, s->id, "Set filter radius", [&](fem::Study& cs) {
        cs.filterRadius = fr;
        cs.filterRadiusExpr.clear();
      });
    int mi = s->maxIterations;
    const bool miChanged = ImGui::InputInt("Max iterations", &mi);
    if ((miChanged || ImGui::IsItemDeactivatedAfterEdit()) && mi >= 1 &&
        mi != s->maxIterations)
      ModifyStudy(state, s->id, "Set max iterations",
                  [&](fem::Study& cs) { cs.maxIterations = mi; });
    // Keep faces: frozen solid, added by picking a face with the Keep tool.
    SimulationState& sim = state.simulation;
    const bool arming = sim.tool == SimulationState::Tool::Keep;
    if (arming)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(ICON_FA_LOCK " Keep face"))
      sim.tool = arming ? SimulationState::Tool::None
                        : SimulationState::Tool::Keep;
    if (arming) ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "%zu kept", s->keepFaces.size());
    for (size_t i = 0; i < s->keepFaces.size(); ++i) {
      ImGui::PushID(static_cast<int>(i));
      ImGui::TextColored(kMuted, "%s", s->keepFaces[i].name.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton(ICON_FA_TRASH)) {
        const ElementRef f = s->keepFaces[i];
        ModifyStudy(state, s->id, "Remove keep face", [&](fem::Study& cs) {
          cs.keepFaces.erase(
              std::remove_if(cs.keepFaces.begin(), cs.keepFaces.end(),
                             [&](const ElementRef& k) {
                               return k.featureId == f.featureId &&
                                      k.name == f.name;
                             }),
              cs.keepFaces.end());
        });
        ImGui::PopID();
        break;
      }
      ImGui::PopID();
    }
  } else {
    // Static: an optional gravity / self-weight load.
    bool on = s->gravity.x != 0 || s->gravity.y != 0 || s->gravity.z != 0;
    if (ImGui::Checkbox("Gravity (self-weight)", &on)) {
      const glm::dvec3 g = on ? glm::dvec3(0, 0, -9.81) : glm::dvec3(0, 0, 0);
      ModifyStudy(state, s->id, "Toggle gravity",
                  [&](fem::Study& cs) { cs.gravity = g; });
    }
    if (on) {
      float g[3] = {(float)s->gravity.x, (float)s->gravity.y, (float)s->gravity.z};
      if (ImGui::InputFloat3("g (m/s^2)", g, "%.2f",
                             ImGuiInputTextFlags_EnterReturnsTrue))
        ModifyStudy(state, s->id, "Set gravity", [&](fem::Study& cs) {
          cs.gravity = glm::dvec3(g[0], g[1], g[2]);
        });
    }
  }

  DrawLoads(state, *s);
  DrawResults(state, *s);

  if (ImGui::Button(ICON_FA_TRASH " Delete study")) {
    const uint64_t id = s->id;
    state.commands->Execute(std::make_unique<RemoveStudyCommand>(id), *doc);
    state.simulation.activeStudyId =
        doc->GetStudies().empty() ? 0 : doc->GetStudies().front().id;
  }

  ImGui::End();
}

namespace {
// Analytic Turbo colormap (matches field.frag) for the legend colorbar.
ImU32 Turbo(float x) {
  x = x < 0 ? 0 : (x > 1 ? 1 : x);
  const float r = 0.13572138f + x * (4.61539260f + x * (-42.66032258f + x * 132.13108234f)) +
                  x * x * x * x * (-152.94239396f + x * 59.28637943f);
  const float g = 0.09140261f + x * (2.19418839f + x * (4.84296658f + x * -14.18503333f)) +
                  x * x * x * x * (4.27729857f + x * 2.82956604f);
  const float b = 0.10667330f + x * (12.64194608f + x * (-60.58204836f + x * 110.36276771f)) +
                  x * x * x * x * (-89.90310912f + x * 27.34824973f);
  auto ch = [](float v) { return (int)(255.0f * (v < 0 ? 0 : (v > 1 ? 1 : v))); };
  return IM_COL32(ch(r), ch(g), ch(b), 255);
}
}  // namespace

void DrawFieldLegend(AppState& state) {
  const SimulationState& sim = state.simulation;
  // No colorbar for "off" or the grey Shape iso-surface (it has no scalar).
  if (!sim.IsActive() || sim.field == SimulationState::Field::None ||
      sim.field == SimulationState::Field::Shape || !state.document)
    return;
  const fem::StudyRuntime* rt = state.document->StudyResults(sim.activeStudyId);
  if (!rt) return;
  const fem::Study* study = state.document->FindStudy(sim.activeStudyId);
  const bool modal = study && study->type == fem::AnalysisType::Modal;
  const bool density = sim.field == SimulationState::Field::Density;
  const bool vm = sim.field == SimulationState::Field::VonMises && !modal;
  // Modal shows a normalized mode shape (0..1); density is 0..1; static shows
  // the real field.
  const double maxVal =
      (modal || density) ? 1.0 : (vm ? rt->maxVonMises : rt->maxDisplacement);
  const char* unit = (modal || density) ? "" : (vm ? "MPa" : "mm");
  const char* title = density  ? "density"
                      : modal  ? "mode shape"
                      : vm     ? "von Mises"
                               : "displacement";

  // A vertical Turbo colorbar anchored to the viewport's lower-left corner.
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float x = state.viewportPos.x + 16.0f;
  const float barW = 18.0f, barH = 180.0f;
  const float y1 = state.viewportPos.y + state.viewportSize.y - 28.0f;
  const float y0 = y1 - barH;
  const int segs = 48;
  for (int i = 0; i < segs; ++i) {
    const float t0 = i / (float)segs, t1 = (i + 1) / (float)segs;
    // High value at the top.
    dl->AddRectFilledMultiColor(ImVec2(x, y1 - t1 * barH), ImVec2(x + barW, y1 - t0 * barH),
                                Turbo(t1), Turbo(t1), Turbo(t0), Turbo(t0));
  }
  dl->AddRect(ImVec2(x, y0), ImVec2(x + barW, y1), IM_COL32(255, 255, 255, 160));
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3g %s", maxVal, unit);
  dl->AddText(ImVec2(x + barW + 6, y0 - 6), IM_COL32_WHITE, buf);
  dl->AddText(ImVec2(x + barW + 6, y1 - 6), IM_COL32_WHITE, "0");
  dl->AddText(ImVec2(x, y0 - 22), IM_COL32_WHITE, title);
}

namespace {
// A world-space arrow from a to b with a two-line head.
void ArrowGlyph(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color,
                std::vector<PrimVertex>& out) {
  ui::AppendPrimLine(a, b, 2.2f, color, out);
  const glm::vec3 dir = glm::normalize(b - a);
  glm::vec3 side = glm::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0)
                                          : glm::vec3(1, 0, 0);
  side = glm::normalize(glm::cross(dir, side));
  const float headLen = glm::length(b - a) * 0.25f;
  ui::AppendPrimLine(b, b - dir * headLen + side * headLen * 0.5f, 2.2f,
                     color, out);
  ui::AppendPrimLine(b, b - dir * headLen - side * headLen * 0.5f, 2.2f,
                     color, out);
}
} // namespace

void AppendSimulationBCGlyphs(AppState& state, std::vector<PrimVertex>& out) {
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();
  if (!sim.IsActive() || doc == nullptr || !state.faceSelector)
    return;
  const fem::Study* s = doc->FindStudy(sim.activeStudyId);
  if (s == nullptr)
    return;
  const glm::vec4 kFixed{0.95f, 0.35f, 0.35f, 1.0f};
  const glm::vec4 kForce{1.00f, 0.65f, 0.20f, 1.0f};
  const glm::vec4 kPressure{0.95f, 0.85f, 0.30f, 1.0f};
  const glm::vec4 kKeep{0.35f, 0.85f, 0.95f, 1.0f};

  const auto faceFrame = [&](const ElementRef& ref, glm::vec3& center,
                             glm::vec3& normal) {
    uint32_t mesh = 0, index = 0;
    if (!ResolveFaceItem(state, {SelectionItem::Kind::Face, 0, ref}, mesh,
                         index))
      return false;
    center = state.faceSelector->GetFaceCenter(mesh, index);
    normal = state.faceSelector->GetFaceNormal(mesh, index);
    return true;
  };
  // Glyph scale tracks the view (like the peak marker): a fixed 1.2 model-mm
  // arrow vanished entirely on a 90 mm part.
  const float len =
      state.camera ? std::max(0.055f * state.camera->GetDistance(), 1.2f)
                   : 1.2f;

  for (const fem::Load& load : s->loads) {
    glm::vec3 c(0.0f), n(0.0f);
    if (!faceFrame(load.face, c, n))
      continue;
    switch (load.type) {
    case fem::LoadType::Fixed: {
      // Anchor hatch: short strokes fanning "into the ground" off the face.
      glm::vec3 side = glm::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0)
                                            : glm::vec3(1, 0, 0);
      side = glm::normalize(glm::cross(n, side));
      const glm::vec3 other = glm::normalize(glm::cross(n, side));
      ui::AppendPrimLine(c - side * len * 0.4f, c + side * len * 0.4f, 2.4f,
                         kFixed, out);
      for (int i = -1; i <= 1; i++) {
        const glm::vec3 base = c + side * (len * 0.3f * i);
        ui::AppendPrimLine(base, base - n * len * 0.3f - side * len * 0.15f,
                           1.8f, kFixed, out);
      }
      (void)other;
      break;
    }
    case fem::LoadType::Force: {
      const glm::vec3 dir =
          glm::normalize(glm::vec3(load.direction.x, load.direction.y,
                                   load.direction.z));
      ArrowGlyph(c - dir * len, c, kForce, out); // arrow INTO the face point
      break;
    }
    case fem::LoadType::Pressure: {
      // Pressure presses along -normal: a shorter arrow at the face.
      ArrowGlyph(c + n * len * 0.8f, c, kPressure, out);
      break;
    }
    }
  }
  for (const ElementRef& keep : s->keepFaces) {
    glm::vec3 c(0.0f), n(0.0f);
    if (!faceFrame(keep, c, n))
      continue;
    glm::vec3 side = glm::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0)
                                          : glm::vec3(1, 0, 0);
    side = glm::normalize(glm::cross(n, side));
    const glm::vec3 other = glm::normalize(glm::cross(n, side));
    const float r = len * 0.35f;
    const glm::vec3 pts[4] = {c + side * r, c + other * r, c - side * r,
                              c - other * r};
    for (int i = 0; i < 4; i++)
      ui::AppendPrimLine(pts[i], pts[(i + 1) % 4], 2.0f, kKeep, out);
  }
}

void SimulationApplyToFace(AppState& state, const ElementRef& face) {
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();
  if (!doc || sim.tool == SimulationState::Tool::None)
    return;
  fem::Study* s = doc->FindStudy(sim.activeStudyId);
  if (!s) return;

  // Keep tool (topology): freeze the picked face solid.
  if (sim.tool == SimulationState::Tool::Keep) {
    ModifyStudy(state, s->id, "Add keep face", [&](fem::Study& cs) {
      const bool dup = std::any_of(
          cs.keepFaces.begin(), cs.keepFaces.end(), [&](const ElementRef& k) {
            return k.featureId == face.featureId && k.name == face.name;
          });
      if (!dup) cs.keepFaces.push_back(face);
    });
    sim.tool = SimulationState::Tool::None;
    return;
  }

  fem::Load load;
  load.id = doc->AllocateId();
  load.type = sim.tool == SimulationState::Tool::Fixed    ? fem::LoadType::Fixed
              : sim.tool == SimulationState::Tool::Force   ? fem::LoadType::Force
                                                           : fem::LoadType::Pressure;
  load.face = face;
  if (load.type != fem::LoadType::Fixed) load.magnitude = sim.pendingMagnitude;
  if (load.type == fem::LoadType::Force) load.direction = sim.pendingDirection;

  ModifyStudy(state, s->id, std::string("Add ") + LoadLabel(load.type) + " load",
              [&](fem::Study& cs) { cs.loads.push_back(load); });
  sim.tool = SimulationState::Tool::None; // one placement per arm
}

}  // namespace ui
