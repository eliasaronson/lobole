#include "FaceSelector.h"
#include "Logger.h"
#include "core/Command.h"
#include "core/Serialization.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>

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

void DrawResults(AppState& state, const fem::Study& s) {
  Document* doc = state.document.get();
  SimulationState& sim = state.simulation;

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
  double meshSize = s->meshSize;
  if (ImGui::InputDouble("Mesh size (mm, 0=auto)", &meshSize, 0.0, 0.0, "%.3g",
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
    ModifyStudy(state, s->id, "Set mesh size", [&](fem::Study& cs) {
      cs.meshSize = meshSize;
      cs.meshSizeExpr.clear();
    });
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
  if (!sim.IsActive() || sim.field == SimulationState::Field::None ||
      !state.document)
    return;
  const fem::StudyRuntime* rt = state.document->StudyResults(sim.activeStudyId);
  if (!rt) return;
  const bool vm = sim.field == SimulationState::Field::VonMises;
  const double maxVal = vm ? rt->maxVonMises : rt->maxDisplacement;
  const char* unit = vm ? "MPa" : "mm";
  const char* title = vm ? "von Mises" : "displacement";

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

void SimulationPick(AppState& state, const Ray& ray) {
  SimulationState& sim = state.simulation;
  Document* doc = state.document.get();
  if (!doc || sim.tool == SimulationState::Tool::None || !state.faceSelector)
    return;
  fem::Study* s = doc->FindStudy(sim.activeStudyId);
  if (!s) return;

  const RayHit hit = state.faceSelector->PickFace(ray);
  if (!hit.hit) return;
  // Resolve the picked mesh back to its Body, then that face's stable name.
  const auto& meshes = state.documentMeshes;
  const auto it = std::find_if(meshes.begin(), meshes.end(),
                               [&](const DocumentRenderMesh& m) {
                                 return m.meshId == hit.meshId;
                               });
  if (it == meshes.end() || it->bodyIndex >= doc->GetBodies().size()) return;
  const Body& body = doc->GetBodies()[it->bodyIndex];
  const std::string* name = body.elementMap.NameOf(static_cast<int>(hit.faceIndex));
  if (!name) return; // unnamed face: can't anchor a stable ref

  fem::Load load;
  load.id = doc->AllocateId();
  load.type = sim.tool == SimulationState::Tool::Fixed    ? fem::LoadType::Fixed
              : sim.tool == SimulationState::Tool::Force   ? fem::LoadType::Force
                                                           : fem::LoadType::Pressure;
  load.face = ElementRef{body.createdBy, *name};
  if (load.type != fem::LoadType::Fixed) load.magnitude = sim.pendingMagnitude;
  if (load.type == fem::LoadType::Force) load.direction = sim.pendingDirection;

  ModifyStudy(state, s->id, std::string("Add ") + LoadLabel(load.type) + " load",
              [&](fem::Study& cs) { cs.loads.push_back(load); });
  sim.tool = SimulationState::Tool::None; // one placement per arm
}

}  // namespace ui
