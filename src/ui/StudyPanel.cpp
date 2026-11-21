// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/StudyPanel.h"
#include "cad/CADMaterials.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/Command.h"
#include "core/Conventions.h"
#include "core/DatumFrame.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/Requirements.h"
#include "core/features/DatumAxisFeature.h"
#include "core/fem/Calculix.h"
#include "core/fem/FemStudy.h"
#include "core/fem/SimDocument.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/MeshRenderer.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h" // FindBackgroundJob // OccurrenceSubtreeBodies, PartDocById
#include "ui/DockLayout.h"     // PanelHeading
#include "ui/DocumentSlots.h"
#include "ui/ExprInput.h" // expression-capable numeric fields
#include "ui/FileDialogs.h"
#include "ui/FloatingWindow.h" // RecordViewportObstacle
#include "ui/Icons.h"
#include "ui/ListSelection.h" // IsSelected (a picked load lights its face)
#include "ui/Overlays.h"      // ViewportDrawList
#include "ui/PanelLayout.h"   // LeftLabel / WrapRow (readable at dock width)
#include "ui/PanelTitles.h"
#include "ui/PreviewFeature.h"    // BeginToolWindowFrame
#include "ui/SelectionPipeline.h" // ResolveFaceItem (BC glyphs)
#include "ui/SimGeometryPanel.h"
#include "ui/SimRunActions.h"
#include "ui/SimTree.h"
#include "ui/SimulationActions.h"
#include "ui/SimulationView.h"
#include "ui/SketchInteraction.h" // WorldToScreen (labels)
#include "ui/SketchOverlay.h"
#include "ui/StudyHost.h"
#include "ui/ViewportHandle.h"
#include "ui/Widgets.h"
#include <utility>

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ui {

namespace {

// The bodies of one top-level occurrence of the studies' assembly, placed:
// the simulation tab's own (SimSubjectBodies, by SimBodyOccurrence), or the
// active assembly's.
std::vector<Body> SubjectOccurrenceBodies(AppState& state,
                                          uint64_t occurrenceId) {
  if (!state.sim)
    return OccurrenceSubtreeBodies(state, occurrenceId);
  std::vector<Body> out;
  const std::vector<Body>& flat = SimSubjectBodies(state);
  for (size_t i = 0; i < flat.size(); i++)
    if (SimBodyOccurrence(state, i) == occurrenceId)
      out.push_back(flat[i]);
  return out;
}

// The occurrence a render mesh draws: a simulation tab's meshes are its
// subject's flattened bodies, each of one top-level occurrence.
uint64_t MeshOccurrence(AppState& state, const DocumentRenderMesh& m) {
  return state.sim ? SimBodyOccurrence(state, m.bodyIndex) : m.occurrenceId;
}

// The body a render mesh draws, by its index within its occurrence's part:
// a simulation tab's flattened bodies are numbered across the occurrences.
int MeshLocalBody(AppState& state, const DocumentRenderMesh& m) {
  if (!state.sim)
    return static_cast<int>(m.bodyIndex);
  const uint64_t occ = SimBodyOccurrence(state, m.bodyIndex);
  int k = 0;
  for (size_t i = 0; i < m.bodyIndex; i++)
    if (SimBodyOccurrence(state, i) == occ)
      k++;
  return k;
}

const ImVec4 kError(0.95f, 0.5f, 0.4f, 1.0f);
const ImVec4 kStale(0.95f, 0.75f, 0.35f, 1.0f);
const ImVec4 kMuted(0.6f, 0.6f, 0.6f, 1.0f);

// A message line, wrapped at the width the participants legend uses. The
// Simulation window sizes itself to its content, so ONE unwrapped sentence
// (a contact planner's refusal, a missing material) set its width: it grew
// to the whole app and covered the viewport it asks you to click in. And its
// rows' right-aligned tails then hold the width they were given, so the next
// tool session opened just as wide (docs/ui/panels.md#floating-windows).
void MessageText(const ImVec4& color, const std::string& text) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
}

const char* LoadLabel(fem::LoadType t) { return SimLoadName(t); }

// What the armed tool does with the face clicked: a line wrapped at the
// window's width, as MessageText is, so a long one cannot widen it.
void Hint(const char* fmt, ...) IM_FMTARGS(1);
void Hint(const char* fmt, ...) {
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22.0f);
  va_list args;
  va_start(args, fmt);
  ImGui::TextDisabledV(fmt, args);
  va_end(args);
  ImGui::PopTextWrapPos();
}

// The roller, held and cylindrical tools (docs/simulation/README.md
// #rollers-held-directions-and-cylindrical-supports): offered on a part's
// static, modal or buckling study, each with what it holds chosen before the
// click. Nothing here has a default with a unit: a held support's
// displacements are zero until typed, and one with no direction ticked is
// refused where it is placed.
void DrawFrameSupportTools(AppState& state, const fem::Study& s) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (sim.tool == SimulationState::Tool::Roller)
    Hint("Click a face: held along its normal, free across "
         "it -- a symmetry plane on a flat face, a "
         "frictionless seat on a curved one");
  if (sim.tool == SimulationState::Tool::Held) {
    // The frame: the world's, or a Frame or datum of the part.
    std::string current = "World";
    std::vector<std::pair<uint64_t, std::string>> frames;
    if (const Document* part = host.GeometryPart())
      for (const auto& f : part->GetFeatures())
        if (datumframe::IsAnchor(*f))
          frames.push_back({f->GetId(), f->GetName()});
    for (const auto& [id, name] : frames)
      if (id == sim.pendingHeldFrame)
        current = name;
    LeftLabel("Frame");
    if (ImGui::BeginCombo("###sim_held_frame", current.c_str())) {
      if (ImGui::Selectable("World###held_frame_world",
                            sim.pendingHeldFrame == 0))
        sim.pendingHeldFrame = 0;
      for (const auto& [id, name] : frames)
        if (ImGui::Selectable(
                (name + "###held_frame_" + std::to_string(id)).c_str(),
                sim.pendingHeldFrame == id))
          sim.pendingHeldFrame = id;
      ImGui::EndCombo();
    }
    for (int k = 0; k < 3; ++k) {
      const std::string id = std::string("Hold ") + fem::kHeldAxisNames[k] +
                             "###sim_held_" + fem::kHeldAxisNames[k];
      ImGui::Checkbox(id.c_str(), &sim.pendingHold[k]);
      if (sim.pendingHold[k] && s.type == fem::AnalysisType::Static) {
        ImGui::SameLine();
        std::string expr;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ExprInputDouble(
                state,
                (std::string("###sim_held_d") + fem::kHeldAxisNames[k]).c_str(),
                &sim.pendingHeldMm[k], "%.4g mm", &expr))
          sim.pendingHeldMmExpr[k] = expr;
      }
    }
    Hint("Click a face: each ticked direction held, at the "
         "displacement typed (0: where it is)");
  }
  if (sim.tool == SimulationState::Tool::Cylindrical) {
    for (int k = 0; k < 3; ++k) {
      const std::string id = std::string("Hold ") +
                             fem::kCylindricalAxisNames[k] + "###sim_cyl_" +
                             fem::kCylindricalAxisNames[k];
      ImGui::Checkbox(id.c_str(), &sim.pendingCylHold[k]);
    }
    Hint("Click a bore or a shaft: held both ways in the "
         "directions ticked, about its axis");
  }
}

// A torque's axis, chosen before the click (DrawAxisRefCombo's choices on a
// part: X/Y/Z, datum axes, construction lines, custom; an assembly's world
// X/Y/Z or custom). Resolved to a point and direction when the load is
// placed, so the load stores numbers, as add_load does.
struct AxisChoice {
  std::string label;
  AxisRef ref;
};
std::vector<AxisChoice> TorqueAxisChoices(AppState& state) {
  std::vector<AxisChoice> out = {{"X", {AxisRef::Kind::X}},
                                 {"Y", {AxisRef::Kind::Y}},
                                 {"Z", {AxisRef::Kind::Z}}};
  if (const Document* doc = StudyHostOf(state).GeometryPart()) {
    for (const auto& f : doc->GetFeatures()) {
      if (const auto* da = dynamic_cast<const DatumAxisFeature*>(f.get())) {
        AxisRef r;
        r.kind = AxisRef::Kind::DatumAxis;
        r.datumAxisId = da->GetId();
        out.push_back({da->GetName(), r});
      } else if (const auto* sk = dynamic_cast<const SketchFeature*>(f.get())) {
        for (const auto& e : sk->entities) {
          const auto* line = std::get_if<sketch::Line>(&e);
          if (line == nullptr || !line->construction)
            continue;
          AxisRef r;
          r.kind = AxisRef::Kind::SketchLine;
          r.sketchId = sk->GetId();
          r.entityId = line->id;
          out.push_back(
              {sk->GetName() + ": line " + std::to_string(line->id), r});
        }
      }
    }
  }
  AxisRef custom;
  custom.kind = AxisRef::Kind::Custom;
  custom.direction = glm::dvec3(0, 0, 1);
  out.push_back({"Custom", custom});
  return out;
}

bool SameAxisChoice(const AxisRef& a, const AxisRef& b) {
  return a.kind == b.kind &&
         (a.kind != AxisRef::Kind::DatumAxis ||
          a.datumAxisId == b.datumAxisId) &&
         (a.kind != AxisRef::Kind::SketchLine ||
          (a.sketchId == b.sketchId && a.entityId == b.entityId));
}

// The chosen axis as numbers; false when it no longer resolves.
bool ResolveTorqueAxis(AppState& state, const AxisRef& ref, glm::dvec3& origin,
                       glm::dvec3& dir) {
  if (Document* doc =
          const_cast<Document*>(StudyHostOf(state).GeometryPart())) {
    RegenContext ctx{const_cast<std::vector<Body>&>(doc->GetBodies()),
                     doc->GetFeatures(), doc->GetFeatures().size()};
    return ResolveAxis(ctx, ref, origin, dir);
  }
  switch (ref.kind) {
  case AxisRef::Kind::X:
    origin = glm::dvec3(0.0);
    dir = glm::dvec3(1, 0, 0);
    return true;
  case AxisRef::Kind::Y:
    origin = glm::dvec3(0.0);
    dir = glm::dvec3(0, 1, 0);
    return true;
  case AxisRef::Kind::Z:
    origin = glm::dvec3(0.0);
    dir = glm::dvec3(0, 0, 1);
    return true;
  case AxisRef::Kind::Custom:
    if (glm::dot(ref.direction, ref.direction) < 1e-18)
      return false;
    origin = ref.origin;
    dir = glm::normalize(ref.direction);
    return true;
  default:
    return false;
  }
}

// "390 N·mm", "1.2 N·m": a torque the way a person reads one.
std::string TorqueText(double nmm) {
  char buf[64];
  if (std::abs(nmm) >= 10000.0)
    std::snprintf(buf, sizeof(buf), "%.3g N\xC2\xB7m", nmm / 1000.0);
  else
    std::snprintf(buf, sizeof(buf), "%.3g N\xC2\xB7mm", nmm);
  return buf;
}

// A face's stress in a solved static part study, in the case the viewport
// shows (docs/simulation/README.md#probing-a-face): what probe_study
// answers. nullopt when the study has no current result, or the face is not
// on a body it solved.
struct Probed {
  fem::FaceStress stress;
  std::string body;
};
std::optional<Probed> ProbeFace(AppState& state, const fem::Study& s,
                                const ElementRef& face) {
  const StudyHost host = StudyHostOf(state);
  const Document* part = host.GeometryPart();
  if (!part || s.type != fem::AnalysisType::Static)
    return std::nullopt;
  const fem::StudyRuntime* rt = host.Results(s.id);
  if (!rt || StudyStale(state, s.id))
    return std::nullopt;
  fem::ResolvedFace rf;
  if (!fem::ResolveFace(*part, face, "probe", rf, nullptr))
    return std::nullopt;
  for (const fem::StudyBodyResult& b : rt->bodies) {
    if (b.bodyIndex != rf.bodyIndex)
      continue;
    const auto st = fem::FaceStressOf(
        b.mesh, b.Case(state.simulation.activeCase).vonMises, rf.faceIndex);
    if (!st)
      return std::nullopt;
    return Probed{*st, b.bodyName};
  }
  return std::nullopt;
}

std::string ProbeText(const fem::FaceStress& st) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "max %.3g  mean %.3g MPa", st.maxMPa,
                st.meanMPa);
  return buf;
}

// The pinned probe as a requirement: the face's peak von Mises held at most
// the yield strength of its body's material -- the limit the part's own
// material sets -- or, without one, at most what it reads now.
void AddProbeRequirement(AppState& state, const fem::Study& s,
                         const ElementRef& face, double now) {
  const StudyHost host = StudyHostOf(state);
  const Document* part = host.GeometryPart();
  if (!part || state.activePart < 0 ||
      state.activePart >= static_cast<int>(state.parts.size()))
    return;
  double limit = now;
  fem::ResolvedFace rf;
  if (fem::ResolveFace(*part, face, "probe", rf, nullptr)) {
    const Body& body = part->GetBodies()[rf.bodyIndex];
    const auto mp = fem::StudyMaterialRow(s, body.material);
    if (mp.yieldMPa > 0.0)
      limit = mp.yieldMPa;
  }
  req::Requirement r;
  r.id = state.nextRequirementId++;
  r.revision = 1;
  r.name = s.name + ": " + face.name + " stress";
  r.source = "Pinned with the Probe tool: the stress on this face, not the "
             "global peak.";
  r.measure = {
      {"kind", "study_probe"},
      // The part the study analyses; the study is found in its simulation
      // (fem::SimulationHoldingStudy).
      {"document",
       host.sim ? host.sim->Subject().docId : state.parts[state.activePart].id},
      {"study", s.id},
      {"face", {{"feature_id", face.featureId}, {"element", face.name}}},
      {"quantity", "von_mises_max"}};
  r.bound.kind = "at_most";
  r.bound.value = limit;
  try {
    req::Validate(r.measure, r.bound);
  } catch (const std::exception& e) {
    LOG_WARN("probe requirement refused: {}", e.what());
    return;
  }
  state.requirements.push_back(std::move(r));
  state.requirementsEdits++;
  state.projectMetaDirty = true;
}

// Every new body starts with the DEFAULT material, which deliberately carries
// no stiffness data (CADMaterials.h: fail rather than guess). So the FIRST Run
// on any new part fails — and it used to fail only after the click, as a
// tooltip on a small triangle. Say it up front, and offer the fix here rather
// than sending the user out of simulation mode to find the feature's Material
// combo.
void DrawMaterialCheck(AppState& state) {
  using CADMaterials::MaterialLibrary;
  // The part the studies stand on, and the command stack its edits go
  // through: its own tab's, in front or behind the simulation tab.
  Document* doc = nullptr;
  CommandStack* commands = nullptr;
  if (state.document) {
    doc = state.document.get();
    commands = state.commands.get();
  } else if (state.sim && state.sim->Subject().kind == fem::SubjectKind::Part) {
    if (const auto at = SlotIndexById(state, state.sim->Subject().docId)) {
      doc = state.parts[*at].doc.get();
      commands = state.parts[*at].commands.get();
    }
  }
  if (!doc || !commands)
    return;
  const auto& types = MaterialLibrary::AllTypes();
  for (const Body& body : doc->GetBodies()) {
    const auto& mp = MaterialLibrary::GetMaterialOrDefault(body.material);
    if (mp.youngGPa > 0.0f)
      continue;
    size_t index = 0;
    Feature* owner = doc->FindFeatureById(body.createdBy, &index);
    if (!owner)
      continue;
    // A VISIBLE label, and no PushID: an ID scope or a hidden label leaves the
    // widget addressable only by the numeric id ui_items prints, and this is
    // the control that unblocks a user's first study. One combo per body, so
    // the id carries the feature it edits.
    const std::string comboId = "###material_" + std::to_string(body.createdBy);
    MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " + body.name +
                            " has no material");
    Hint("A study needs stiffness data to solve.");
    LeftLabel("Material");
    std::vector<const char*> labels;
    labels.reserve(types.size() + 1);
    labels.push_back("Assign a material...");
    for (auto t : types)
      labels.push_back(MaterialLibrary::GetMaterial(t).name.c_str());
    int sel = 0;
    if (ImGui::Combo(comboId.c_str(), &sel, labels.data(),
                     static_cast<int>(labels.size())) &&
        sel > 0) {
      nlohmann::json before = DocumentIO::FeatureToJson(*owner);
      owner->SetMaterial(MaterialLibrary::Slug(types[sel - 1]));
      nlohmann::json after = DocumentIO::FeatureToJson(*owner);
      commands->Execute(
          std::make_unique<ModifyFeatureCommand>(index, before, after), *doc);
    }
  }
}

// The case the next force, pressure, torque or remote load goes in, on a
// study with load cases (docs/topopt/README.md#load-cases).
void DrawPendingCase(AppState& state, const fem::Study& s) {
  SimulationState& sim = state.simulation;
  if (s.loadCases.empty() || (sim.tool != SimulationState::Tool::Force &&
                              sim.tool != SimulationState::Tool::Pressure &&
                              sim.tool != SimulationState::Tool::Torque &&
                              sim.tool != SimulationState::Tool::Remote &&
                              sim.tool != SimulationState::Tool::Bearing))
    return;
  int k = std::max(0, s.FindLoadCase(sim.pendingCase));
  LeftLabel("In case");
  if (ImGui::BeginCombo("###sim_pending_case", s.loadCases[k].name.c_str())) {
    for (size_t i = 0; i < s.loadCases.size(); ++i)
      if (ImGui::Selectable(s.loadCases[i].name.c_str(),
                            static_cast<int>(i) == k))
        sim.pendingCase = s.loadCases[i].name;
    ImGui::EndCombo();
  }
}

// One load case (the tree's row in focus): its loads, its weight in a
// topology study's objective, and Remove once nothing is in it.
void DrawCaseWindow(AppState& state, const fem::Study& s, int index) {
  if (index < 0 || index >= static_cast<int>(s.loadCases.size())) {
    ImGui::TextColored(kMuted, "That load case is gone.");
    return;
  }
  const fem::LoadCase& c = s.loadCases[static_cast<size_t>(index)];
  std::vector<const fem::Load*> in;
  for (const fem::Load& l : s.loads)
    if (l.loadCase == c.name)
      in.push_back(&l);
  ImGui::TextColored(kMuted, "Solved on its own, with every support.");
  if (s.type == fem::AnalysisType::Topology) {
    double w = c.weight;
    std::string wExpr;
    LeftLabel("Weight");
    const bool wChanged =
        ExprInputDouble(state, "###case_weight", &w, "%.3g", &wExpr);
    if ((wChanged || ImGui::IsItemDeactivatedAfterEdit()) && w > 0.0 &&
        (w != c.weight || wExpr != c.weight.expr)) {
      const std::string name = c.name;
      ModifyStudy(state, s.id, "Set load case weight", [&](fem::Study& cs) {
        for (fem::LoadCase& lc : cs.loadCases)
          if (lc.name == name) {
            lc.weight = w;
            lc.weight.expr = wExpr;
          }
      });
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Weight in the objective: the run minimises the "
                        "weighted sum of the cases' compliances.");
  }
  ImGui::SeparatorText("Loads in it");
  if (in.empty())
    ImGui::TextColored(kMuted, "None: arm a load with this case chosen.");
  for (const fem::Load* l : in)
    ImGui::BulletText("%s  %s", LoadLabel(l->type), l->face.name.c_str());
  ImGui::BeginDisabled(!in.empty());
  if (ImGui::Button(ICON_FA_TRASH_CAN " Remove case###case_del")) {
    const std::string name = c.name;
    ModifyStudy(state, s.id, "Remove load case", [&](fem::Study& cs) {
      std::erase_if(cs.loadCases,
                    [&](const fem::LoadCase& x) { return x.name == name; });
    });
    state.simulation.focus = {};
  }
  ImGui::EndDisabled();
  if (!in.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Loads are in this case; remove them first.");
}

// A value with no default (docs/ui/tool-sessions.md#defaults-and-refusals):
// empty, showing what it wants, until a number is typed; NaN while empty.
void OptionalNumber(const char* label, const char* id, const char* hint,
                    double& v) {
  static std::map<std::string, std::string> text;
  std::string& t = text[id];
  if (std::isnan(v))
    t.clear();
  else if (t.empty())
    t = [&] {
      char b[48];
      std::snprintf(b, sizeof(b), "%.6g", v);
      return std::string(b);
    }();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s", t.c_str());
  LeftLabel(label);
  if (ImGui::InputTextWithHint(id, hint, buf, sizeof(buf))) {
    t = buf;
    char* end = nullptr;
    const double d = std::strtod(buf, &end);
    v = (end != buf && *end == '\0' && std::isfinite(d))
            ? d
            : std::numeric_limits<double>::quiet_NaN();
  }
}

// A thermal study's tools (docs/simulation/README.md#thermal): each with its
// values, empty until typed.
void DrawThermalTools(AppState& state) {
  SimulationState& sim = state.simulation;
  using T = SimulationState::Tool;
  if (sim.tool == T::Temperature) {
    OptionalNumber("Temperature (\xC2\xB0"
                   "C)",
                   "###sim_temperature_c", "type it", sim.pendingTemperatureC);
    Hint("Click a face: it is held at this temperature");
  }
  if (sim.tool == T::HeatFlux) {
    ImGui::Checkbox("Per area (W/m\xC2\xB2)###sim_flux_per_area",
                    &sim.pendingFluxPerArea);
    if (sim.pendingFluxPerArea)
      OptionalNumber("Flux (W/m\xC2\xB2)", "###sim_flux_w_m2", "type it",
                     sim.pendingFluxWm2);
    else
      OptionalNumber("Power (W)", "###sim_flux_power_w", "type it",
                     sim.pendingHeatW);
    Hint("Click a face: heat goes into it (negative: out)");
  }
  if (sim.tool == T::Convection) {
    OptionalNumber("Film (W/m\xC2\xB2K)", "###sim_film_w_m2k",
                   "air 2-25 free, 25-250 forced", sim.pendingFilmWm2K);
    OptionalNumber("Ambient (\xC2\xB0"
                   "C)",
                   "###sim_ambient_c", "type it", sim.pendingAmbientC);
    Hint("Click a face: it is cooled by a fluid at the "
         "ambient");
  }
  if (sim.tool == T::HeatSource) {
    OptionalNumber("Power (W)", "###sim_source_power_w", "type it",
                   sim.pendingHeatW);
    Hint("Click a face of the body that makes the heat");
  }
}

// The armed support's or load's settings: what the next face clicked
// takes. Every value with a unit is derived from the part or typed; none
// is a literal (docs/ui/tool-sessions.md#defaults-and-refusals).
void DrawArmedTool(AppState& state, const fem::Study& s) {
  SimulationState& sim = state.simulation;
  DrawFrameSupportTools(state, s);
  DrawThermalTools(state);
  if (sim.tool == SimulationState::Tool::Contact) {
    LeftLabel("Friction (\xCE\xBC)");
    ExprInputDouble(state, "###sim_contact_friction",
                    &sim.pendingContactFriction, "%.3g");
    if (sim.pendingContactFriction < 0.0)
      sim.pendingContactFriction = 0.0;
    Hint("Click a flat face: it rests on the ground, which "
         "pushes it and never pulls; solved on CalculiX");
  }
  if (sim.tool == SimulationState::Tool::Pin) {
    ImGui::Checkbox("Hold it from turning###sim_pin_fix_rotation",
                    &sim.pendingPinFixRotation);
    Hint("Click a bore or a shaft: the pin holds it where the "
         "part presses on it%s",
         sim.pendingPinFixRotation ? "" : ", free to turn");
  }

  if (sim.tool == SimulationState::Tool::Torque) {
    LeftLabel("Torque (N\xC2\xB7mm)");
    std::string expr;
    if (ExprInputDouble(state, "###sim_torque", &sim.pendingTorque, "%.1f",
                        &expr))
      sim.pendingTorqueExpr = expr;
    if (sim.pendingTorque == 0.0)
      Hint("Default: the part's weight times the face's "
           "radius");
    const std::vector<AxisChoice> choices = TorqueAxisChoices(state);
    std::string current = "From the face (cylinder)";
    for (const AxisChoice& c : choices)
      if (sim.torqueAxisChosen && SameAxisChoice(c.ref, sim.torqueAxis))
        current = c.label;
    LeftLabel("Axis");
    if (ImGui::BeginCombo("###sim_torque_axis", current.c_str())) {
      if (ImGui::Selectable("From the face (cylinder)###axis_face",
                            !sim.torqueAxisChosen))
        sim.torqueAxisChosen = false;
      for (size_t i = 0; i < choices.size(); ++i) {
        const std::string id =
            choices[i].label + "###axis_choice_" + std::to_string(i);
        const bool sel = sim.torqueAxisChosen &&
                         SameAxisChoice(choices[i].ref, sim.torqueAxis);
        if (ImGui::Selectable(id.c_str(), sel)) {
          const AxisRef keep = sim.torqueAxis;
          sim.torqueAxis = choices[i].ref;
          if (choices[i].ref.kind == AxisRef::Kind::Custom &&
              keep.kind == AxisRef::Kind::Custom) {
            sim.torqueAxis = keep; // what was typed survives re-picking
          }
          sim.torqueAxisChosen = true;
        }
      }
      ImGui::EndCombo();
    }
    if (sim.torqueAxisChosen && sim.torqueAxis.kind == AxisRef::Kind::Custom) {
      LeftLabel("Through (mm)");
      ExprInputDouble3(state, "###sim_torque_origin", &sim.torqueAxis.origin.x,
                       "%.2f");
      LeftLabel("Along");
      ExprInputDouble3(state, "###sim_torque_dir", &sim.torqueAxis.direction.x,
                       "%.3f");
    }
    if (!sim.torqueAxisChosen)
      ImGui::TextColored(kMuted, "A cylindrical face lends its own axis; any "
                                 "other face needs one chosen here.");
  }
  if (sim.tool == SimulationState::Tool::Remote) {
    LeftLabel("Force (N)");
    std::string expr;
    if (ExprInputDouble(state, "###sim_remote_force", &sim.pendingMagnitude,
                        "%.1f", &expr))
      sim.pendingMagnitudeExpr = expr;
    ImGui::Checkbox("Custom direction###sim_remote_custom_dir",
                    &sim.directionOverridden);
    if (sim.directionOverridden) {
      LeftLabel("Direction");
      ExprInputDouble3(state, "###sim_remote_direction",
                       &sim.pendingDirection.x, "%.3f");
    } else {
      Hint("Direction: into the face you click");
    }
    LeftLabel("At (mm)");
    if (ExprInputDouble3(state, "###sim_remote_point", &sim.pendingPoint.x,
                         "%.2f"))
      sim.pointOverridden = true;
    if (sim.pointOverridden) {
      ImGui::SameLine();
      if (ImGui::SmallButton("From face###sim_remote_point_reset"))
        sim.pointOverridden = false;
    } else {
      Hint("At: off the face you click, along its normal");
    }
  }

  // A bearing load (docs/simulation/README.md#bearing-loads): its total
  // force, and the way the shaft in the bore pushes it.
  if (sim.tool == SimulationState::Tool::Bearing) {
    LeftLabel("Force (N)");
    std::string expr;
    if (ExprInputDouble(state, "###sim_bearing_force", &sim.pendingMagnitude,
                        "%.1f", &expr))
      sim.pendingMagnitudeExpr = expr;
    ImGui::Checkbox("Custom direction###sim_bearing_custom_dir",
                    &sim.directionOverridden);
    if (sim.directionOverridden) {
      LeftLabel("Direction");
      ExprInputDouble3(state, "###sim_bearing_direction",
                       &sim.pendingDirection.x, "%.3f");
    } else {
      Hint("Direction: down, across the bore's axis");
    }
    Hint("Click a bore or a shaft: the force presses the half it pushes "
         "on, most where it pushes straight");
  }

  // Pending magnitude/direction applied to the next placed force/pressure.
  if (sim.tool == SimulationState::Tool::Force ||
      sim.tool == SimulationState::Tool::Pressure) {
    LeftLabel(sim.tool == SimulationState::Tool::Force ? "Magnitude (N)"
                                                       : "Pressure (MPa)");
    std::string magExpr;
    if (ExprInputDouble(state, "###sim_magnitude", &sim.pendingMagnitude,
                        "%.1f", &magExpr))
      sim.pendingMagnitudeExpr = magExpr; // empty for a plain number
    if (sim.tool == SimulationState::Tool::Force) {
      // Off by default: the force pushes INTO whichever face is clicked, which
      // is what placing a force on a face means. A world-space vector typed
      // before the face is even picked can only be right by luck.
      ImGui::Checkbox("Custom direction###sim_custom_dir",
                      &sim.directionOverridden);
      if (sim.directionOverridden) {
        LeftLabel("Direction");
        ExprInputDouble3(state, "###sim_direction", &sim.pendingDirection.x,
                         "%.3f");
      } else {
        Hint("Direction: into the face you click");
      }
    }
  }

  using T = SimulationState::Tool;
  if (sim.tool == T::Fixed)
    Hint("Click a face: it is held where it is");
  if (sim.tool == T::Force || sim.tool == T::Pressure)
    Hint("Click a face to place it");
  if (sim.tool == T::Torque || sim.tool == T::Remote)
    Hint("Click the face that carries it");
  if (sim.tool == T::Keep)
    Hint("Click a face: the optimizer keeps it solid");
  DrawPendingCase(state, s);
  if (!sim.placeRefusal.empty())
    MessageText(kError, sim.placeRefusal);
}

// Reused: shown under the result it is about.
void DrawReuseNote(AppState& state, uint64_t studyId) {
  if (state.simulation.reusedStudy == studyId)
    ImGui::TextColored(kMuted, "Reused: these inputs were already solved.");
}

// Topology results: an async Run (worker thread) with a live progress bar +
// Cancel, then the optimization summary and the Shape/Density display controls.
void DrawTopoResults(AppState& state, const fem::Study& s) {
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return;
  SimulationState& sim = state.simulation;
  ImGui::SeparatorText("Optimization");

  // No poll here any more: this panel is drawn only in Simulation mode, so
  // leaving that mode stranded a finished solve until you came back. Every
  // loop polls instead (ui/BackgroundJobs.h); this panel only reads.
  const TopoRunStatus st =
      GetTopologyStatus(state, s.id, ActiveSimSlotId(state));
  if (st.running) {
    const float frac =
        st.maxIterations > 0
            ? std::clamp(static_cast<float>(st.iteration) / st.maxIterations,
                         0.0f, 1.0f)
            : 0.0f;
    char ov[48];
    std::snprintf(ov, sizeof(ov), "iter %d / %d", st.iteration,
                  st.maxIterations);
    JobProgress bar; // the same widget the import modal draws (ui/Widgets.h)
    bar.fraction = st.maxIterations > 0 ? frac : -1.0;
    bar.overlay = ov;
    // The widget's own Cancel, as every job's: this one drew a second button
    // of its own under the bar.
    const BackgroundJob* bg = FindBackgroundJob(state, "topo");
    bar.cancelAsked = bg != nullptr && bg->cancel.load();
    bar.cancelId = "###topo_cancel";
    bar.cancelTip = "Stop after this iteration, keeping the best so far.";
    if (DrawJobProgress(bar))
      CancelTopologyRun(state, s.id);
    ImGui::Text("Compliance: %.4g   Vf: %.3f", st.compliance,
                st.volumeFraction);
    ImGui::TextColored(kMuted, "%s   %.0fs", st.solver.c_str(),
                       st.elapsedSeconds);
    return;
  }

  if (!s.error.empty())
    MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " + s.error);
  DrawReuseNote(state, s.id);
  const fem::StudyRuntime* rt = host.Results(s.id);
  if (!rt) {
    ImGui::TextColored(kMuted, "Not run yet.");
    return;
  }
  if (StudyStale(state, s.id))
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION
                       " Stale (model changed since last run)");

  size_t nodes = 0, elements = 0;
  for (const auto& b : rt->bodies) {
    nodes += b.mesh.stats.nodeCount;
    elements += b.mesh.stats.tetCount;
  }
  ImGui::Text("Mesh: %zu nodes / %zu elements", nodes, elements);
  // In a static study the mesh resolves the stress field; in a topology study
  // it resolves the STRUCTURE — members thinner than an element cannot be
  // found, so a coarse mesh returns a coarse shape and looks like a bad
  // optimizer. The auto size is chosen for stress, and on a small part that
  // is a few hundred elements, which is one to two orders below what a 3D
  // SIMP design needs. Say it rather than let the shape speak for itself.
  if (elements < 2000)
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION
                       " Coarse for a shape: set a smaller mesh size");
  ImGui::Text("Iterations: %d%s", rt->iterations,
              rt->converged   ? " (converged)"
              : rt->cancelled ? " (cancelled)"
                              : "");
  ImGui::Text("Volume fraction: %.3f", rt->volumeFractionAchieved);
  if (!rt->solver.empty()) { // transient provenance; empty on a reload
    ImGui::TextColored(kMuted, "Solver: %s (%.0f s)", rt->solver.c_str(),
                       rt->solveSeconds);
    // The string carries WHY a run fell back off the GPU, and that is the
    // half a 210 px panel clips.
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", rt->solver.c_str());
  }
  if (!rt->complianceHistory.empty()) {
    ImGui::Text("Compliance: %.4g -> %.4g", rt->complianceHistory.front(),
                rt->complianceHistory.back());
    std::vector<float> hv(rt->complianceHistory.begin(),
                          rt->complianceHistory.end());
    ImGui::PlotLines("##compliance", hv.data(), static_cast<int>(hv.size()), 0,
                     nullptr, FLT_MAX, FLT_MAX, ImVec2(-FLT_MIN, 40));
  }

  for (size_t k = 0; k < rt->caseNames.size(); ++k)
    ImGui::TextColored(kMuted, "  %s: %.4g (x %.3g)", rt->caseNames[k].c_str(),
                       k < rt->caseCompliance.size() ? rt->caseCompliance[k]
                                                     : 0.0,
                       k < rt->caseWeights.size() ? rt->caseWeights[k] : 1.0);
  for (const fem::KeepOutReport& k : rt->keepOuts)
    ImGui::TextColored(kMuted, ICON_FA_BAN " %s: %d tets out", k.what.c_str(),
                       k.tets);
  if (rt->islands.tets > 0)
    ImGui::TextColored(
        kStale, "Removed %d loose piece%s (%d tets, %.0f mm3)%s",
        rt->islands.components, rt->islands.components == 1 ? "" : "s",
        rt->islands.tets, rt->islands.volume,
        rt->islands.keepFaces.empty() ? "" : ", holding keep faces");
  if (rt->islands.loadPathBroken)
    ImGui::TextColored(kError, ICON_FA_TRIANGLE_EXCLAMATION
                       " A load reaches no support: the shape is broken");
  // A static study of the shape and of the solid domain, before anything is
  // modelled from it: both are made (or refreshed) and the shape is run.
  if (!StudyStale(state, s.id) &&
      ImGui::Button(ICON_FA_SCALE_BALANCED " Check shape###topo_check")) {
    uint64_t shapeId = 0, solidId = 0;
    std::string err;
    const bool made = EnsureSimTopologyCheckStudies(
        state, host.simSlotId, s.id, 0.0, &shapeId, &solidId, &err);
    if (!made) {
      LOG_WARN("check shape: {}", err);
    } else {
      JobStart start;
      if (!StartSimStudyRun(state, host.simSlotId, shapeId, &err, &start))
        LOG_WARN("check shape: {}", err);
      sim.activeStudyId = shapeId;
      sim.field = SimulationState::Field::None;
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A static study of the optimised shape and one of the "
                      "solid design domain, with these loads and cases. The "
                      "shape's runs now; run the solid's to compare.");

  // The shape or the density, and the iso-surface's threshold, are the
  // legend's (DrawFieldLegend: the picker, and the threshold behind its
  // gear).
  // Shape guide: show the optimized shape translucent in the modeling
  // workspace (visible after exiting simulation mode) to re-model against.
  bool ghost = state.topoGhostStudyId == s.id;
  if (ImGui::Checkbox("Shape guide (modeling overlay)###topo_ghost", &ghost)) {
    state.topoGhostStudyId = ghost ? s.id : 0;
    state.gpu.syncedDocumentVersion = UINT64_MAX; // rebuild renderers
  }
}

// A buckling study's answer (docs/simulation/README.md#buckling): each
// body's factors as a selectable list (picking one shows its mode), the
// governing factor with what made it, and the flags as text.
void DrawBucklingResults(AppState& state, const fem::Study& s,
                         const fem::StudyRuntime& rt) {
  (void)s;
  SimulationState& sim = state.simulation;
  ImGui::SeparatorText("Buckling factors");
  const fem::StudyBodyResult* b = nullptr;
  for (const auto& br : rt.bodies)
    if (br.buckling.solved) {
      b = &br;
      break;
    }
  if (!b) {
    ImGui::TextColored(kMuted, "No buckling result.");
    return;
  }
  // The mode shown is the legend's; the one on show is marked here.
  for (int i = 0; i < (int)b->results.modes.size(); ++i)
    ImGui::TextColored(sim.activeMode == i &&
                               sim.field != SimulationState::Field::None
                           ? ImVec4(1, 1, 1, 1)
                           : kMuted,
                       "Mode %d:  x %.4g", i + 1, b->results.modes[i].factor);
  const fem::StudyBodyResult::Buckling& k = b->buckling;
  const auto fmt = [](const char* f, double v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), f, v);
    return std::string(buf);
  };
  if (!k.buckles)
    MessageText(kMuted, k.none);
  ImGui::SeparatorText("Governing");
  if (k.governingFactor > 0)
    ImGui::Text("Governing factor:  x %.3g", k.governingFactor);
  else
    MessageText(kMuted, "No governing factor: no yield strength.");
  if (k.buckles)
    ImGui::Text("Knockdown:  %.3g (%s)", k.knockdown, k.shapeClass.c_str());
  if (k.yieldFactor > 0)
    ImGui::Text("Yield factor:  x %.3g", k.yieldFactor);
  if (k.interlayerFactor > 0)
    ImGui::Text("Interlayer factor:  x %.3g", k.interlayerFactor);
  if (k.atLoadEdge)
    MessageText(kStale, "The first mode is local where a load enters (face " +
                            std::to_string(k.edgeFace) +
                            "): crippling at its edge, not the member "
                            "buckling.");
  if (k.wallLayers == 1)
    MessageText(kStale,
                fmt("The first mode bends a %.2g mm wall with one element "
                    "through it.",
                    k.wallMm));
}

// A thermal study's answer (docs/simulation/README.md#thermal): each
// body's peak and lowest temperature, its margin to its material's service
// temperature, the heat through each load and the balance.
void DrawThermalResults(const fem::StudyRuntime& rt) {
  for (const fem::StudyBodyResult& b : rt.bodies) {
    const fem::StudyBodyResult::Thermal& t = b.thermal;
    ImGui::SeparatorText(b.bodyName.c_str());
    ImGui::Text("Peak:  %.4g \xC2\xB0"
                "C   lowest:  %.4g \xC2\xB0"
                "C",
                t.maxC, t.minC);
    ImGui::Text("Peak heat flux:  %.4g W/m\xC2\xB2", t.maxFluxWm2);
    if (t.hasService) {
      ImGui::TextColored(t.marginK >= 0 ? kMuted : kError,
                         "Margin:  %.3g K below %.4g \xC2\xB0"
                         "C",
                         t.marginK, t.serviceMaxC);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", t.serviceBasis.c_str());
    }
    if (t.bulkConductivity)
      MessageText(kStale, "A printed part conducts less than its resin: its "
                          "hot spots run hotter than predicted.");
    for (const fem::HeatFlow& f : t.flows)
      ImGui::Text("%s %llu:  %.4g W %s", LoadLabel(f.type),
                  static_cast<unsigned long long>(f.id), std::abs(f.watts),
                  f.watts >= 0 ? "out" : "in");
    ImGui::TextColored(kMuted, "Balance:  %.4g W in, %.4g W out", t.inW,
                       t.outW);
  }
}

// One row of a report: a glyph, the text wrapped at the window's width (a
// finding is a sentence, and an elided sentence is not a finding), a hover
// highlight across the whole of it, and a tooltip. Returns true on the click
// that frames its place. The window sizes itself to its content, so the
// width is the fixed one MessageText wraps at, never the space available.
bool FindingRow(const std::string& id, const char* glyph,
                const ImVec4& glyphColor, const std::string& text,
                const std::string& tip, bool clickable) {
  const float width = ImGui::GetFontSize() * 22.0f;
  const float indent = ImGui::GetFontSize() * 1.5f;
  const float wrap = width - indent;
  const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.0f));
  const bool clicked = ImGui::Selectable(
      id.c_str(), false, ImGuiSelectableFlags_None, ImVec2(width, size.y));
  ImGui::PopStyleVar();
  if (ImGui::IsItemHovered() && !tip.empty()) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
    ImGui::TextUnformatted(tip.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(pos, ImGui::GetColorU32(glyphColor), glyph);
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
              ImVec2(pos.x + indent, pos.y), ImGui::GetColorU32(ImGuiCol_Text),
              text.c_str(), nullptr, wrap);
  return clicked && clickable;
}

// The mesh step (docs/simulation/README.md#the-mesh-you-look-at): Mesh as an
// action whose result is looked at. One row to make it and say what came
// out, one to choose how it is drawn, and its report folded under them.
// A study's own mesh size: 0 solves it on the simulation's mesh, anything
// else gives it a mesh of its own (docs/simulation/README.md#one-mesh-shared).
// A typed EXPRESSION keeps its text (the study carries a slot for it); a
// plain number clears the slot back to a literal. Commits on deactivation
// too, so a value typed before Mesh is the one it meshes at.
void DrawStudyOwnMeshSize(AppState& state, const fem::Study& s,
                          const char* label) {
  double meshSize = s.meshSize;
  LeftLabel(label);
  std::string meshExpr;
  const bool meshChanged = ExprInputDouble(state, "###study_mesh_size",
                                           &meshSize, "%.3g", &meshExpr);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("0: this study solves on the simulation's mesh. A "
                      "size of its own (or a refinement)\ngives it a mesh "
                      "of its own, with its own row in the tree.");
  if ((meshChanged || ImGui::IsItemDeactivatedAfterEdit()) &&
      (meshSize != s.meshSize || meshExpr != s.meshSize.expr))
    ModifyStudy(state, s.id, "Set mesh size", [&](fem::Study& cs) {
      cs.meshSize = meshSize;
      cs.meshSize.expr = meshExpr;
    });
}

void DrawMeshWindow(AppState& state, const fem::Study& s) {
  if (s.shapeFrom.study != 0) {
    ImGui::TextColored(kMuted, "A study of a topology result meshes its "
                               "shape when it runs.");
    return;
  }
  SimulationState& sim = state.simulation;
  if (fem::SimDocument::SharesMesh(s)) {
    // The simulation's mesh: every study that sets no size or refinement
    // of its own solves on it. Same expression rules as a study's size.
    const StudyHost host = StudyHostOf(state);
    const ExprValue& have = host.sim->Mesh().size;
    double meshSize = have;
    LeftLabel("Size (mm, 0=auto)");
    std::string meshExpr;
    const bool meshChanged = ExprInputDouble(state, "###sim_mesh_size",
                                             &meshSize, "%.3g", &meshExpr);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The simulation's mesh: every study that sets no "
                        "size of its own solves on it.\n0 = auto, each "
                        "body's bounding diagonal / 15.");
    if ((meshChanged || ImGui::IsItemDeactivatedAfterEdit()) &&
        (meshSize != have.value || meshExpr != have.expr)) {
      SimUndoGuard guard(state);
      fem::SimMesh mesh = host.sim->Mesh();
      mesh.size = meshSize;
      mesh.size.expr = meshExpr;
      host.sim->SetMesh(std::move(mesh));
    }
  } else {
    DrawStudyOwnMeshSize(state, s, "Size (mm, 0=shared)");
  }
  const MeshRunStatus run =
      GetMeshRunStatus(state, s.id, ActiveSimSlotId(state));
  if (run.running) {
    JobProgress bar;
    bar.status = "Meshing";
    bar.overlay = std::to_string(static_cast<int>(run.elapsedSeconds)) + " s";
    BackgroundJob* bg = FindBackgroundJob(state, "mesh");
    bar.cancellable = bg != nullptr;
    bar.cancelAsked = bg != nullptr && bg->cancel.load();
    bar.cancelId = "###mesh_cancel";
    bar.cancelTip = "The mesher has no place to stop: the mesh is dropped "
                    "when it lands.";
    if (DrawJobProgress(bar) && bg != nullptr)
      CancelBackgroundJobById(state, bg->id, "the Cancel button");
    return;
  }
  const MeshReport* mr = StudyMesh(state, s.id);
  if (mr == nullptr) {
    ImGui::TextColored(kMuted, "Not meshed: Mesh on the toolbar, or Run.");
    return;
  }
  if (!mr->error.empty()) {
    MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " + mr->error);
    return;
  }
  size_t elements = 0;
  double worst = 1.0;
  for (const MeshReport::Body& b : mr->bodies) {
    elements += b.elements;
    worst = std::min(worst, b.report.worstQuality);
  }
  // The report: everything the mesh says about itself. Rebuilt when the
  // mesh is.
  static uint64_t rowsEpoch = 0;
  static std::vector<MeshFinding> rows;
  if (rowsEpoch != mr->epoch) {
    rows = MeshFindings(state, *mr);
    rowsEpoch = mr->epoch;
  }
  int warnings = 0;
  for (const MeshFinding& f : rows)
    warnings += f.level == MeshFinding::Level::Warning;
  const bool stale = StudyMeshStale(state, s.id);
  ImGui::TextColored(stale ? kStale : kMuted, "%zu elements, worst %.2f%s",
                     elements, worst, stale ? "  (stale)" : "");
  if (stale && ImGui::IsItemHovered())
    ImGui::SetTooltip("The geometry or the mesh settings changed since this "
                      "mesh was made.");
  if (warnings > 0) {
    ImGui::SameLine();
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION " %d", warnings);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%d warning%s in the mesh report.", warnings,
                        warnings == 1 ? "" : "s");
  }

  static const char* kViews[] = {"Hidden", "Edges", "Quality", "Size"};
  int view = static_cast<int>(sim.meshView);
  LeftLabel("Show mesh");
  if (ImGui::Combo("###mesh_view", &view, kViews, 4)) {
    sim.meshView = static_cast<SimulationState::MeshView>(view);
    if (sim.meshView != SimulationState::MeshView::Off)
      sim.field = SimulationState::Field::None; // one picture at a time
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Element edges over the body, alone or coloured by "
                      "each element's quality\n(the worse of its shape and "
                      "its scaled Jacobian) or its size.\nA section cuts it "
                      "along whole elements.");

  // Each row frames its place.
  if (SectionHeader("Mesh report###mesh_report", static_cast<int>(rows.size()),
                    true)) {
    // The five worst elements are enough to find a bad region by; the
    // RPC's report has all of them.
    int worstShown = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      const MeshFinding& f = rows[i];
      if (f.code == "worst_element" && ++worstShown > 5)
        continue;
      const bool warn = f.level == MeshFinding::Level::Warning;
      if (FindingRow("###mesh_row_" + std::to_string(i),
                     warn ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_INFO,
                     warn ? kStale : kMuted, f.text,
                     f.hasPlace ? "Click to frame it." : "", f.hasPlace)) {
        if (sim.meshView == SimulationState::MeshView::Off) {
          sim.meshView = SimulationState::MeshView::Edges;
          sim.field = SimulationState::Field::None;
        }
        FrameSimulationPlace(state, f.at, f.radiusMm);
      }
    }
  }
}

// What a run would refuse, beside Run (docs/simulation/README.md
// #checking-a-study): every problem at once, each with the call that fixes
// it in its tooltip, and a click framing the face or the body it is about.
void DrawStudyCheck(AppState& state, const fem::Study& s) {
  const StudyCheck& check = CachedStudyCheck(state, s.id);
  if (check.problems.empty()) {
    ImGui::TextColored(kMuted, ICON_FA_CHECK " Nothing a run would refuse.");
    return;
  }
  const StudyHost host = StudyHostOf(state);
  for (size_t i = 0; i < check.problems.size(); ++i) {
    const fem::StudyProblem& p = check.problems[i];
    // Said once: a body's material is flagged above with the combo that
    // sets it (DrawMaterialCheck), and a refusal Run already printed in
    // full is not repeated under it.
    bool flaggedAbove = false;
    if (p.code == "material" && host.GeometryPart() && !s.material)
      for (const Body& b : host.GeometryPart()->GetBodies())
        flaggedAbove =
            flaggedAbove ||
            (b.name == p.body &&
             CADMaterials::MaterialLibrary::GetMaterialOrDefault(b.material)
                     .youngGPa <= 0.0f);
    if (flaggedAbove || (!s.error.empty() && p.message == s.error))
      continue;
    std::string tip;
    if (!p.fixMethod.empty())
      tip = "Fix: " + p.fixMethod + " " + p.fixParams.dump();
    if (!p.fixNote.empty())
      tip += (tip.empty() ? "" : "\n") + p.fixNote;
    // Where it is: its face, else its body.
    std::optional<std::pair<glm::dvec3, double>> place;
    if (const Document* part = host.GeometryPart()) {
      const auto& bodies = part->GetBodies();
      fem::ResolvedFace rf;
      std::string err;
      if (p.hasFace && fem::ResolveFace(*part, p.face, "", rf, &err)) {
        if (const auto info =
                Measure::DescribeFaceAt(bodies[rf.bodyIndex], rf.faceIndex))
          place = {info->centroid, std::sqrt(std::max(info->area, 1e-6))};
      } else if (!p.body.empty()) {
        for (const Body& b : bodies)
          if (b.name == p.body) {
            glm::dvec3 lo(0.0), hi(0.0);
            b.shape.GetBoundingBox(lo, hi);
            place = {0.5 * (lo + hi), 0.5 * glm::length(hi - lo)};
          }
      }
    }
    if (place)
      tip += (tip.empty() ? "" : "\n") + std::string("Click to frame it.");
    if (FindingRow("###check_row_" + std::to_string(i),
                   ICON_FA_TRIANGLE_EXCLAMATION, kStale, p.message, tip,
                   place.has_value()))
      FrameSimulationPlace(state, place->first, place->second);
  }
}

// A solved result out to a file for another post-processor
// (docs/simulation/README.md#vtu-export). Disabled, saying why, when the
// session holds only the result's surface.
void DrawResultExport(AppState& state, const fem::Study&) {
  // The export is the toolbar's; what it last did is said here.
  SimulationState& sim = state.simulation;
  if (!sim.exportNote.empty())
    MessageText(kMuted, sim.exportNote);
}

void DrawProbes(AppState& state, const fem::Study& s);

void DrawResults(AppState& state, const fem::Study& s) {
  const StudyHost host = StudyHostOf(state);
  SimulationState& sim = state.simulation;

  if (s.type == fem::AnalysisType::Topology) {
    DrawTopoResults(state, s);
    return;
  }

  // The solve runs on a worker thread (StartSimStudyRun), the same as
  // topology and the same as the run_study RPC has always done. Clicking Run
  // used to call a blocking solve straight from the draw, so the whole
  // app — not just this panel — stopped answering for the length of a solve,
  // with no progress and nothing to cancel. On a real part that is a hang.
  // (The join happens in ui::PollBackgroundJobs, once per loop iteration —
  // not here, where it only ran while this panel was on screen.)
  const StudyRunStatus run =
      GetStudyRunStatus(state, s.id, ActiveSimSlotId(state));
  if (run.running) {
    // The shared bar (ui/Widgets.h), indeterminate: the linear solver has no
    // progress to report. Its Cancel lands at the next boundary -- after the
    // meshing, or when the factorization ends -- and drops the answer.
    JobProgress bar;
    bar.status = "Solving";
    bar.overlay = std::to_string(static_cast<int>(run.elapsedSeconds)) + " s";
    BackgroundJob* bg = FindBackgroundJob(state, "study");
    bar.cancellable = bg != nullptr;
    bar.cancelAsked = bg != nullptr && bg->cancel.load();
    bar.cancelId = "###study_cancel";
    bar.cancelTip = "Stop after the meshing, or when the solve ends; the "
                    "answer is dropped.";
    if (DrawJobProgress(bar) && bg != nullptr)
      CancelBackgroundJobById(state, bg->id, "the Cancel button");
  }
  if (!s.error.empty()) {
    // A refusal is read, not hovered for: it says what to fix, or how to
    // install the solver, or what the solver said.
    ImGui::PushStyleColor(ImGuiCol_Text, kError);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                           ImGui::GetFontSize() * 22.0f);
    ImGui::TextUnformatted(
        (ICON_FA_TRIANGLE_EXCLAMATION " " + s.error).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  }
  DrawReuseNote(state, s.id);
  const fem::StudyRuntime* rt = host.Results(s.id);
  if (!rt) {
    ImGui::TextColored(kMuted, "Not run yet.");
    return;
  }
  if (StudyStale(state, s.id))
    ImGui::TextColored(kStale, ICON_FA_TRIANGLE_EXCLAMATION
                       " Stale (model changed since last run)");

  size_t nodes = 0, elements = 0;
  for (const auto& b : rt->bodies) {
    nodes += b.mesh.stats.nodeCount;
    elements += b.mesh.stats.tetCount;
  }
  ImGui::Text("Mesh: %zu nodes / %zu elements", nodes, elements);

  if (s.type == fem::AnalysisType::Buckling) {
    DrawBucklingResults(state, s, *rt);
    return;
  }
  if (s.type == fem::AnalysisType::Thermal) {
    DrawThermalResults(*rt);
    DrawResultExport(state, s);
    return;
  }
  if (s.type == fem::AnalysisType::Modal) {
    // Natural frequencies: the numbers; which mode shape is drawn is the
    // legend's.
    ImGui::SeparatorText("Natural frequencies");
    const fem::StudyBodyResult* b = nullptr;
    for (const auto& br : rt->bodies)
      if (!br.results.modes.empty()) {
        b = &br;
        break;
      }
    // The mode shown is the legend's; the one on show is marked here. Each
    // mode's effective mass along x, y, z as a share of the body's
    // (docs/simulation/README.md#free-free-modes), and a free study's rigid
    // modes named as such: their frequency is zero, not a resonance.
    if (b) {
      const double total = b->results.modalMassKg;
      const bool masses = total > 0.0;
      if (ImGui::BeginTable("###modal_modes", masses ? 5 : 2,
                            ImGuiTableFlags_SizingFixedFit |
                                ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Mode");
        ImGui::TableSetupColumn("Frequency");
        if (masses) {
          ImGui::TableSetupColumn("x");
          ImGui::TableSetupColumn("y");
          ImGui::TableSetupColumn("z");
        }
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)b->results.modes.size(); ++i) {
          const fem::FemResults::ModeShape& md = b->results.modes[i];
          const ImVec4 col =
              sim.activeMode == i && sim.field != SimulationState::Field::None
                  ? ImVec4(1, 1, 1, 1)
                  : kMuted;
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextColored(col, "%d", i + 1);
          ImGui::TableNextColumn();
          if (md.rigid)
            ImGui::TextColored(col, "rigid");
          else
            ImGui::TextColored(col, "%.4g Hz", md.frequencyHz);
          if (masses)
            for (int k = 0; k < 3; ++k) {
              ImGui::TableNextColumn();
              const double pct = 100.0 * md.effectiveMassKg[k] / total;
              if (pct < 0.05)
                ImGui::TextColored(kMuted, "-");
              else
                ImGui::TextColored(col, "%.1f%%", pct);
            }
        }
        ImGui::EndTable();
      }
      if (masses) {
        glm::dvec3 sum(0.0);
        for (const auto& md : b->results.modes)
          sum += md.effectiveMassKg;
        ImGui::TextColored(kMuted,
                           "Effective mass, share of %.4g kg: these modes "
                           "hold %.0f / %.0f / %.0f %%",
                           total, 100.0 * sum.x / total, 100.0 * sum.y / total,
                           100.0 * sum.z / total);
      }
    }
    return;
  }

  // A nonlinear run (docs/simulation/README.md#nonlinear-static): its peak,
  // or the load it reached, and what it says about itself.
  for (const auto& b : rt->bodies) {
    const fem::StudyBodyResult::Nonlinear& n = b.nonlinear;
    if (!n.solved)
      continue;
    if (n.snapThrough) {
      if (n.peakAt == "none")
        MessageText(kMuted, "No peak: " + n.none);
      else
        ImGui::Text("Peak load factor:  x %.4g (%s)", n.peakFactor,
                    n.peakAt.c_str());
      ImGui::TextColored(kMuted,
                         "Linear buckling x %.4g; seeded %.3g mm; %d "
                         "increments",
                         n.linearFactor, n.imperfectionMm, n.increments);
    } else {
      ImGui::TextColored(kMuted, "%s: %d increments",
                         n.plasticity         ? (n.largeDeformation
                                                     ? "Plastic, large deformation"
                                                     : "Plastic, small deformation")
                         : n.largeDeformation ? "Large deformation"
                                              : "Contact, small deformation",
                         n.increments);
    }
    // A plastic run (docs/simulation/README.md#plasticity): how much it
    // yielded, what is left once the load is off, or where it collapsed.
    if (n.plasticity) {
      if (n.collapsed)
        MessageText(kStale, "Collapses at x " + std::to_string(n.factor) +
                                " of the load (between x " +
                                std::to_string(n.factor) + " and x " +
                                std::to_string(n.collapseHigh) + ").");
      ImGui::Text("Plastic strain up to %.3g", b.results.maxPlasticStrain);
      if (n.unloaded)
        ImGui::Text("Left deformed by %.4g mm once unloaded",
                    b.results.maxResidualDisplacement);
      if (n.strainLimit > 0 && b.results.maxPlasticStrain > n.strainLimit)
        MessageText(kStale, "Past the end of the hardening data (" +
                                std::to_string(n.strainLimit) +
                                "): extrapolated.");
    }
    // Each contact (docs/simulation/README.md#contact): how much of its
    // faces closed, and how hard it pressed there.
    for (const fem::StudyBodyResult::ContactResult& c : b.contacts) {
      ImGui::Text("%s %s: %.4g of %.4g mm2 closed",
                  c.participant ? "Contact with" : "Rests on", c.name.c_str(),
                  c.closedMm2, c.areaMm2);
      ImGui::TextColored(kMuted,
                         "  %.4g N normal, %.4g N shear; pressure up to %.4g "
                         "MPa",
                         c.normalN, c.shearN, c.maxPressureMPa);
    }
    if (n.stabilization > 0)
      ImGui::TextColored(kMuted, "Stabilization carried %.2g of the load",
                         n.stabilization);
    if (n.snapped)
      MessageText(kStale, "Snapped through between factor " +
                              std::to_string(n.snappedFrom) + " and " +
                              std::to_string(n.snappedTo) + ".");
    if (n.firstYieldFactor > 0)
      ImGui::TextColored(kStale, "Yields at x %.3g of the load",
                         n.firstYieldFactor);
    break;
  }
  if (s.shapeFrom.study != 0)
    ImGui::TextColored(kMuted, "Shape of study %llu at %.2f: %.0f mm3",
                       static_cast<unsigned long long>(s.shapeFrom.study),
                       rt->shapeThreshold, rt->shapeVolume);
  // Whether the peak has settled: the line a reader checks before quoting it.
  {
    const fem::Convergence& c = rt->convergence;
    if (c.checked && c.converged)
      ImGui::TextColored(kMuted, "Peak converged: %.1f %% on refinement %d",
                         c.lastChangePct, static_cast<int>(c.steps.size()) - 1);
    else if (c.checked && c.stopped == "time")
      ImGui::TextColored(kStale,
                         "Peak not converged: a step took %.0f s (limit in "
                         "Preferences)",
                         c.stoppedSeconds);
    else if (c.checked && c.stopped == "failed")
      ImGui::TextColored(kStale, "Peak not converged: %s", c.why.c_str());
    else if (c.checked)
      ImGui::TextColored(kStale,
                         "Peak not converged: still moving %.1f %% after %d "
                         "refinements",
                         c.lastChangePct, static_cast<int>(c.steps.size()) - 1);
    else
      ImGui::TextColored(kMuted, "Peak not checked%s%s",
                         c.why.empty() ? "" : ": ", c.why.c_str());
  }
  if (rt->caseNames.empty()) {
    ImGui::Text("Max von Mises: %.3g MPa", rt->maxVonMises);
    ImGui::Text("Max displacement: %.4g mm", rt->maxDisplacement);
  } else {
    // Each case's maxima; the selected one is what the viewport shows.
    sim.activeCase = std::clamp(sim.activeCase, 0,
                                static_cast<int>(rt->caseNames.size()) - 1);
    for (size_t k = 0; k < rt->caseNames.size(); ++k) {
      double vm = 0.0, disp = 0.0;
      for (const fem::StudyBodyResult& b : rt->bodies) {
        vm = std::max(vm, b.Case(static_cast<int>(k)).maxVonMises);
        disp = std::max(disp, b.Case(static_cast<int>(k)).maxDisplacement);
      }
      // The case shown is the legend's; the one on show is marked here.
      ImGui::TextColored(
          sim.activeCase == static_cast<int>(k) ? ImVec4(1, 1, 1, 1) : kMuted,
          "%s:  %.4g mm, %.3g MPa", rt->caseNames[k].c_str(), disp, vm);
    }
  }

  DrawResultExport(state, s);

  // Probing (docs/simulation/README.md#probing-a-face): the stress ON a face
  // -- a tooth root, a fillet -- where the global peak above may sit in a
  // sharp corner whose value is the mesh's, not the part's. A part study:
  // an assembly's mesh is the imprinted fuse, whose faces are not the
  // parts'.
  if (!host.GeometryPart())
    return;
  DrawProbes(state, s);
}

// The faces pinned with the Probe tool (docs/simulation/README.md
// #probing-a-face), under the result and while the tool is armed.
void DrawProbes(AppState& state, const fem::Study& s) {
  SimulationState& sim = state.simulation;
  bool any = false;
  for (const SimulationState::ProbePin& pin : sim.probes)
    any = any || pin.studyId == s.id;
  if (!any && sim.tool != SimulationState::Tool::Probe)
    return;
  ImGui::SeparatorText("Probes");
  if (!any)
    ImGui::TextColored(kMuted, "None pinned yet.");
  size_t removeAt = SIZE_MAX;
  for (size_t i = 0; i < sim.probes.size(); ++i) {
    const SimulationState::ProbePin& pin = sim.probes[i];
    if (pin.studyId != s.id)
      continue;
    const auto probed = ProbeFace(state, s, pin.face);
    ListRowSpec row;
    row.glyph = ICON_FA_CROSSHAIRS;
    row.label = pin.face.name + "###probe_row_" + std::to_string(i);
    row.tail = probed ? ProbeText(probed->stress) : std::string("-");
    if (probed) {
      char at[160];
      std::snprintf(at, sizeof(at), "Peak at (%.3g, %.3g, %.3g) mm on %s",
                    probed->stress.maxAt.x, probed->stress.maxAt.y,
                    probed->stress.maxAt.z, probed->body.c_str());
      row.tooltip = at;
    }
    row.menuId = "###probe_ctx_" + std::to_string(i);
    const double now = probed ? probed->stress.maxMPa : 0.0;
    const bool has = probed.has_value();
    const ElementRef face = pin.face;
    row.menu = [&state, &s, &removeAt, i, face, now, has] {
      ImGui::BeginDisabled(!has);
      if (ImGui::MenuItem(ICON_FA_LIST_CHECK " Add requirement###probe_req"))
        AddProbeRequirement(state, s, face, now);
      ImGui::EndDisabled();
      if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Remove###probe_del"))
        removeAt = i;
    };
    ListRow(row);
  }
  if (removeAt != SIZE_MAX)
    sim.probes.erase(sim.probes.begin() + static_cast<long>(removeAt));
}

} // namespace

namespace {

// One participant of an ASSEMBLY subject (S1), the tree's row in focus: its
// role, a mesh size on the meshed one, a friction on a contact, and on a
// rigid support the clamp at its contact -- "fix this panel where that part
// touches it", the cheap half of an assembly study. The clamp is a load of
// the study the tools act on.
void DrawParticipantWindow(AppState& state, uint64_t occurrenceId) {
  const StudyHost host = StudyHostOf(state);
  const Occurrence* occ =
      host.Assembly() ? host.Assembly()->FindById(occurrenceId) : nullptr;
  if (occ == nullptr) {
    ImGui::TextColored(kMuted, "That occurrence is no longer in the assembly.");
    return;
  }
  const fem::Study* s = host.Find(state.simulation.activeStudyId);
  const fem::Participant* pt = host.sim->FindRole(occ->id);
  // All five roles, as the row's menu has them.
  const char* kRoles[] = {"Ignored", "Rigid", "Meshed", "Bonded", "Contact"};
  int role = !pt                                         ? 0
             : pt->role == fem::ParticipantRole::Meshed  ? 2
             : pt->role == fem::ParticipantRole::Rigid   ? 1
             : pt->role == fem::ParticipantRole::Bonded  ? 3
             : pt->role == fem::ParticipantRole::Contact ? 4
                                                         : 0;
  LeftLabel("Role");
  const std::string comboId = "###role_" + std::to_string(occ->id);
  if (ImGui::Combo(comboId.c_str(), &role, kRoles, 5))
    SetSimParticipantRole(state, occ->id,
                          role == 4   ? fem::ParticipantRole::Contact
                          : role == 3 ? fem::ParticipantRole::Bonded
                          : role == 2 ? fem::ParticipantRole::Meshed
                          : role == 1 ? fem::ParticipantRole::Rigid
                                      : fem::ParticipantRole::Ignored);
  Hint("Meshed: solved. Bonded: glued to it, solved with it. Contact: "
       "pressed against it. Rigid: a support, clamped where it touches. "
       "Ignored: absent. Every study of the simulation shares the roles.");
  pt = host.sim->FindRole(occ->id);
  if (pt && s && pt->role == fem::ParticipantRole::Rigid) {
    bool clamped = false, rests = false;
    for (const fem::Load& l : s->loads) {
      clamped = clamped || l.contactWith == occ->id;
      rests = rests ||
              (l.contactWith == occ->id && l.type == fem::LoadType::Contact);
    }
    const uint64_t id = occ->id;
    const std::string clampId =
        std::string(clamped ? ICON_FA_LOCK " Clamped###clamp_"
                            : ICON_FA_LOCK_OPEN " Clamp here###clamp_") +
        std::to_string(id);
    if (ImGui::Button(clampId.c_str()))
      ModifyStudy(state, s->id, clamped ? "Unclamp contact" : "Clamp contact",
                  [&](fem::Study& cs) {
                    if (clamped) {
                      std::erase_if(cs.loads, [&](const fem::Load& l) {
                        return l.contactWith == id;
                      });
                      return;
                    }
                    fem::Load clamp;
                    clamp.id = host.AllocateId();
                    clamp.type = fem::LoadType::Fixed;
                    clamp.contactWith = id;
                    cs.loads.push_back(clamp);
                  });
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Fix the meshed part where %s touches it, in %s. The "
                        "patch is found by imprinting when the study meshes; "
                        "it exists in neither part.",
                        occ->name.c_str(), s->name.c_str());
    // Or rest on it: a contact support, pressed and never pulled
    // (docs/simulation/README.md#contact).
    ImGui::SameLine();
    const std::string restId =
        std::string(rests ? ICON_FA_ARROWS_DOWN_TO_LINE " Rests on it###rest_"
                          : ICON_FA_ARROWS_DOWN_TO_LINE " Rest on it###rest_") +
        std::to_string(id);
    if (ImGui::Button(restId.c_str()))
      ModifyStudy(state, s->id, rests ? "Lift off contact" : "Rest on contact",
                  [&](fem::Study& cs) {
                    if (rests) {
                      std::erase_if(cs.loads, [&](const fem::Load& l) {
                        return l.contactWith == id &&
                               l.type == fem::LoadType::Contact;
                      });
                      return;
                    }
                    fem::Load rest;
                    rest.id = host.AllocateId();
                    rest.type = fem::LoadType::Contact;
                    rest.contactWith = id;
                    cs.loads.push_back(rest);
                  });
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The meshed part rests on %s where they face each "
                        "other: pressed, never pulled, solved on CalculiX.",
                        occ->name.c_str());
  }
  if (pt && s && pt->role == fem::ParticipantRole::Contact) {
    double mu = pt->friction;
    LeftLabel("Friction (\xCE\xBC)");
    const std::string muId = "###part_friction_" + std::to_string(occ->id);
    ExprInputDouble(state, muId.c_str(), &mu, "%.3g");
    if (ImGui::IsItemDeactivatedAfterEdit() && mu != pt->friction &&
        mu >= 0.0) {
      const uint64_t id = occ->id;
      ModifyStudy(state, s->id, "Set contact friction", [&](fem::Study& cs) {
        for (fem::Participant& x : cs.participants)
          if (x.occurrenceId == id)
            x.friction = mu;
      });
    }
  }
  if (pt && s && pt->role == fem::ParticipantRole::Meshed) {
    double size = pt->meshSize;
    std::string expr;
    LeftLabel("Its mesh size (mm, 0=study's)");
    const std::string sizeId = "###part_mesh_" + std::to_string(occ->id);
    const bool changed =
        ExprInputDouble(state, sizeId.c_str(), &size, "%.3g", &expr);
    if ((changed || ImGui::IsItemDeactivatedAfterEdit()) &&
        (size != pt->meshSize || expr != pt->meshSize.expr)) {
      const uint64_t id = occ->id;
      ModifyStudy(state, s->id, "Set participant mesh size",
                  [&](fem::Study& cs) {
                    for (fem::Participant& x : cs.participants)
                      if (x.occurrenceId == id) {
                        x.meshSize = size;
                        x.meshSize.expr = expr;
                      }
                  });
    }
    // Materials come from the parts: say so when the meshed one has none.
    for (const Body& b : SubjectOccurrenceBodies(state, occ->id)) {
      const auto& mp =
          CADMaterials::MaterialLibrary::GetMaterialOrDefault(b.material);
      if (mp.youngGPa <= 0.0f)
        MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " + b.name +
                                " has no material with stiffness data; "
                                "assign one on its part tab");
    }
  }
}

// The plan's own word on an assembly's setup, before anyone presses Run:
// the contact cache asks the planner, and a refusal (a support in mid-air,
// an interpenetration) is shown where the study is set up.
void DrawAssemblySetupCheck(AppState& state) {
  if (!StudyHostOf(state).OnAssembly())
    return;
  SimulationContacts(state);
  if (!state.simulation.contactError.empty())
    MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " +
                            state.simulation.contactError);
}

// Everything below the study list, for either host.
// Keep-outs (docs/topopt/README.md#keep-outs): the list, and another body of
// the part to keep out of (a neighbour arrives as a Derived reference body).
// A box is set over RPC (add_keep_out {box_min, box_max}).
void DrawKeepOuts(AppState& state, const fem::Study& s) {
  const Document* doc = StudyHostOf(state).GeometryPart();
  if (doc == nullptr)
    return;
  if (ImGui::BeginCombo("###topo_keepout_body", ICON_FA_BAN " Keep out of...",
                        ImGuiComboFlags_NoArrowButton)) {
    for (const Body& b : doc->GetBodies()) {
      const bool have =
          std::any_of(s.keepOuts.begin(), s.keepOuts.end(),
                      [&](const fem::KeepOut& k) { return k.body == b.name; });
      if (ImGui::Selectable(b.name.c_str(), false,
                            have ? ImGuiSelectableFlags_Disabled : 0)) {
        const std::string name = b.name;
        ModifyStudy(state, s.id, "Add keep-out", [&](fem::Study& cs) {
          cs.keepOuts.push_back({name, {}, {}});
        });
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A body the result must leave empty: its tets are "
                      "removed before the run.");
}

// The ccx this machine has, asked at most every two seconds: the combo is
// drawn every frame, and finding it walks PATH.
const std::optional<fem::CalculixInstall>& CalculixHere(const AppState& state,
                                                        std::string* why) {
  static std::optional<fem::CalculixInstall> found;
  static std::string whyNot, forSetting;
  static std::chrono::steady_clock::time_point asked{};
  const auto now = std::chrono::steady_clock::now();
  if (asked == std::chrono::steady_clock::time_point{} ||
      now - asked > std::chrono::seconds(2) ||
      forSetting != state.settings.calculixPath) {
    asked = now;
    forSetting = state.settings.calculixPath;
    whyNot.clear();
    found = fem::FindCalculix(forSetting, &whyNot);
  }
  if (why)
    *why = whyNot;
  return found;
}

// A static study's thermal load (docs/simulation/README.md
// #the-thermal-load-on-a-static-study): none, one temperature everywhere,
// or a thermal study's, with the stress-free temperature seeded where it is
// derived (the thermal study's one ambient). A choice the study cannot take
// is refused where it is made, saying why. On an assembly only None and
// Uniform: it has no thermal studies to take a temperature from
// (docs/simulation/README.md#a-thermal-load-on-an-assembly).
void DrawThermalLoad(AppState& state, const fem::Study& s) {
  const StudyHost host = StudyHostOf(state);
  std::vector<const fem::Study*> thermals;
  for (const fem::Study& o : host.Studies())
    if (o.type == fem::AnalysisType::Thermal)
      thermals.push_back(&o);
  const fem::ThermalLoad& tl = s.thermalLoad;
  std::string current = "None";
  if (tl.kind == fem::ThermalLoad::Kind::Uniform)
    current = "Uniform";
  for (const fem::Study* t : thermals)
    if (tl.kind == fem::ThermalLoad::Kind::FromStudy && tl.fromStudy == t->id)
      current = t->name;
  static std::string refusal;
  const auto apply = [&](fem::ThermalLoad next) {
    fem::Study with = s;
    with.thermalLoad = next;
    refusal = fem::ThermalRefusal(with);
    if (refusal.empty())
      ModifyStudy(state, s.id, "Set thermal load",
                  [&](fem::Study& cs) { cs.thermalLoad = next; });
  };
  LeftLabel("Thermal load");
  if (ImGui::BeginCombo("###study_thermal_load", current.c_str())) {
    if (ImGui::Selectable("None###thermal_none", !tl.On()))
      apply({});
    if (ImGui::Selectable("Uniform###thermal_uniform",
                          tl.kind == fem::ThermalLoad::Kind::Uniform)) {
      fem::ThermalLoad next;
      next.kind = fem::ThermalLoad::Kind::Uniform;
      next.uniformC = tl.uniformC;
      next.stressFreeC = tl.stressFreeC;
      next.hasStressFree = tl.hasStressFree;
      apply(next);
    }
    for (const fem::Study* t : thermals) {
      const std::string id =
          t->name + "###thermal_from_" + std::to_string(t->id);
      if (ImGui::Selectable(id.c_str(), current == t->name)) {
        fem::ThermalLoad next;
        next.kind = fem::ThermalLoad::Kind::FromStudy;
        next.fromStudy = t->id;
        if (const auto ambient = fem::DerivedStressFree(*t)) {
          next.stressFreeC = *ambient;
          next.hasStressFree = true;
        }
        apply(next);
      }
    }
    ImGui::EndCombo();
  }
  if (tl.On()) {
    if (tl.kind == fem::ThermalLoad::Kind::Uniform) {
      double t = tl.uniformC;
      LeftLabel("Temperature (\xC2\xB0"
                "C)");
      if (ExprInputDouble(state, "###thermal_uniform_c", &t, "%.4g") ||
          ImGui::IsItemDeactivatedAfterEdit())
        if (t != tl.uniformC) {
          fem::ThermalLoad next = tl;
          next.uniformC = t;
          apply(next);
        }
    }
    double t0 = tl.hasStressFree ? tl.stressFreeC
                                 : std::numeric_limits<double>::quiet_NaN();
    const double was = t0;
    OptionalNumber("Stress-free (\xC2\xB0"
                   "C)",
                   "###thermal_stress_free_c", "type it", t0);
    if (!(t0 == was) && !(std::isnan(t0) && std::isnan(was))) {
      fem::ThermalLoad next = tl;
      next.hasStressFree = !std::isnan(t0);
      next.stressFreeC = std::isnan(t0) ? 0.0 : t0;
      apply(next);
    }
  }
  if (!refusal.empty())
    MessageText(kError, refusal);
}

// Which program solves the study (docs/simulation/README.md#calculix), shown
// only where there is a choice: a static study can be solved on CalculiX
// too; modal and topology are in-house only. CalculiX stays choosable when
// it is not installed -- Run then refuses with how to install it, rather
// than the choice vanishing.
void DrawSolverCombo(AppState& state, const fem::Study& s) {
  if (s.type != fem::AnalysisType::Static &&
      s.type != fem::AnalysisType::Buckling &&
      s.type != fem::AnalysisType::Thermal && s.solver != fem::Solver::Calculix)
    return;
  std::string why;
  const auto& ccx = CalculixHere(state, &why);
  const std::string calculix =
      ccx ? "CalculiX " + ccx->Version() : std::string("CalculiX (not found)");
  const char* labels[] = {fem::IsNonlinear(s) ? "Auto (CalculiX: nonlinear)"
                                              : "Auto (in-house)",
                          "In-house", calculix.c_str()};
  int idx = s.solver == fem::Solver::InHouse    ? 1
            : s.solver == fem::Solver::Calculix ? 2
                                                : 0;
  LeftLabel("Solver");
  if (ImGui::Combo("###study_solver", &idx, labels, 3)) {
    const fem::Solver ns = idx == 1   ? fem::Solver::InHouse
                           : idx == 2 ? fem::Solver::Calculix
                                      : fem::Solver::Auto;
    // A nonlinear study is not solved in-house: that pick changes nothing.
    if (ns != s.solver &&
        fem::NonlinearRefusal(ns, s.type, s.nonlinear, s.loadCases.size(),
                              s.converge, fem::HasContact(s))
            .empty())
      ModifyStudy(state, s.id, "Set solver",
                  [&](fem::Study& cs) { cs.solver = ns; });
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s",
                      ccx ? "CalculiX solves the same mesh and loads as a "
                            "second, independent answer; its displacement "
                            "is checked against this study's own stiffness."
                          : why.c_str());
}

// The study's material (docs/simulation/README.md
// #a-study-states-its-own-material): each body's own from the part, or the
// constants the study states. Custom starts from the first subject body's
// library material that has stiffness data, so it starts as the part
// already solves; each field is one undoable edit, and a value the study
// cannot take is refused under the fields, the study left as it was.
void DrawStudyMaterial(AppState& state, const fem::Study& s) {
  using CADMaterials::MaterialLibrary;
  static std::string refusal;
  static uint64_t refusalStudy = 0;
  if (refusalStudy != s.id)
    refusal.clear();
  const char* kModes[] = {"From the part", "Custom"};
  int mode = s.material ? 1 : 0;
  LeftLabel("Material");
  if (ImGui::Combo("###study_material", &mode, kModes, 2) &&
      mode != (s.material ? 1 : 0)) {
    refusal.clear();
    if (mode == 0) {
      ModifyStudy(state, s.id, "Material from the part",
                  [&](fem::Study& cs) { cs.material.reset(); });
    } else {
      // Derived from what the study acts on: the first body that has
      // stiffness data, else the library's first material that does.
      const CADMaterials::MaterialProperties* from = nullptr;
      for (const Body& b : SimSubjectBodies(state)) {
        const auto& mp = MaterialLibrary::GetMaterialOrDefault(b.material);
        if (CADMaterials::ElasticDataRefusal(mp).empty()) {
          from = &mp;
          break;
        }
      }
      if (from == nullptr)
        for (auto t : MaterialLibrary::AllTypes())
          if (CADMaterials::ElasticDataRefusal(MaterialLibrary::GetMaterial(t))
                  .empty()) {
            from = &MaterialLibrary::GetMaterial(t);
            break;
          }
      if (from != nullptr) {
        fem::StudyMaterial m;
        m.youngGPa = from->youngGPa;
        m.poisson = from->poisson;
        m.densityKgM3 = from->density;
        m.yieldMPa = from->yieldMPa;
        m.conductivityWmK = from->conductivityWmK;
        m.expansionPerK = from->expansionPerK;
        ModifyStudy(state, s.id, "Custom material",
                    [&](fem::Study& cs) { cs.material = m; });
      }
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("From the part: each body is solved with its own "
                      "library material.\nCustom: every body is solved with "
                      "the constants below\n(a benchmark that prescribes its "
                      "own material).");
  if (!s.material)
    return;
  const fem::StudyMaterial& now = *s.material;
  const auto field = [&](const char* label, const char* id, double now_v,
                         double fem::StudyMaterial::* key, const char* fmt) {
    double v = now_v;
    LeftLabel(label);
    if ((ExprInputDouble(state, id, &v, fmt) ||
         ImGui::IsItemDeactivatedAfterEdit()) &&
        v != now_v) {
      fem::StudyMaterial next = now;
      next.*key = v;
      refusal = fem::StudyMaterialRefusal(next);
      refusalStudy = s.id;
      if (refusal.empty())
        ModifyStudy(state, s.id, "Set study material",
                    [&](fem::Study& cs) { cs.material = next; });
    }
  };
  field("Young's modulus (GPa)", "###study_material_young_gpa", now.youngGPa,
        &fem::StudyMaterial::youngGPa, "%.6g");
  field("Poisson's ratio", "###study_material_poisson", now.poisson,
        &fem::StudyMaterial::poisson, "%.4g");
  field("Density (kg/m\xC2\xB3, 0=none)", "###study_material_density",
        now.densityKgM3, &fem::StudyMaterial::densityKgM3, "%.6g");
  field("Yield (MPa, 0=none)", "###study_material_yield", now.yieldMPa,
        &fem::StudyMaterial::yieldMPa, "%.6g");
  if (s.type == fem::AnalysisType::Thermal)
    field("Conductivity (W/(m K), 0=none)", "###study_material_conductivity",
          now.conductivityWmK, &fem::StudyMaterial::conductivityWmK, "%.6g");
  if (s.thermalLoad.On())
    field("Expansion (1/K, 0=none)", "###study_material_expansion",
          now.expansionPerK, &fem::StudyMaterial::expansionPerK, "%.6g");
  if (!refusal.empty())
    MessageText(kError, refusal);
}

void DrawStudySettings(AppState& state, const fem::Study* s) {
  // What the last Run refused, where the study is set up: it says what to
  // fix, or how to install the solver.
  if (!s->error.empty())
    MessageText(kError, ICON_FA_TRIANGLE_EXCLAMATION " " + s->error);
  char name[128];
  std::snprintf(name, sizeof(name), "%s", s->name.c_str());
  LeftLabel("Name");
  if (ImGui::InputText("###study_name", name, sizeof(name),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    const std::string newName = name;
    ModifyStudy(state, s->id, "Rename study",
                [&](fem::Study& cs) { cs.name = newName; });
  }
  // Analysis type: Static (stress), Modal (frequencies), or Topology
  // (generative shape).
  const char* kTypes[] = {"Static", "Modal", "Topology", "Buckling", "Thermal"};
  int typeIdx = s->type == fem::AnalysisType::Modal      ? 1
                : s->type == fem::AnalysisType::Topology ? 2
                : s->type == fem::AnalysisType::Buckling ? 3
                : s->type == fem::AnalysisType::Thermal  ? 4
                                                         : 0;
  LeftLabel("Analysis");
  if (ImGui::Combo("###study_analysis", &typeIdx, kTypes, 5)) {
    const auto nt = typeIdx == 1   ? fem::AnalysisType::Modal
                    : typeIdx == 2 ? fem::AnalysisType::Topology
                    : typeIdx == 3 ? fem::AnalysisType::Buckling
                    : typeIdx == 4 ? fem::AnalysisType::Thermal
                                   : fem::AnalysisType::Static;
    // CalculiX solves static studies only: a study switched to another
    // type goes back to the in-house solver with it, in the same step,
    // rather than becoming a study no file could hold.
    ModifyStudy(state, s->id, "Set analysis type", [&](fem::Study& cs) {
      if (nt == fem::AnalysisType::Buckling && cs.type != nt)
        cs.numModes = fem::kBucklingFactorsDefault;
      cs.type = nt;
      // Free-free is a modal study's, and a topology result keeps its
      // whole mesh already: neither setting is carried to a study that
      // cannot read it.
      if (nt != fem::AnalysisType::Modal)
        cs.freeFree = false;
      if (nt == fem::AnalysisType::Topology)
        cs.keep = fem::KeepMode::Surface;
      if (!fem::SolverRefusal(cs.solver, cs.type).empty())
        cs.solver = fem::Solver::Auto;
    });
    // The displayed field belongs to the analysis that produced it. Leaving
    // "von Mises" selected across a switch to Topology left a colorbar on
    // screen titled von Mises with a 0 MPa maximum — a legend for a field
    // this study does not have, which reads as an answer.
    state.simulation.field = SimulationState::Field::None;
    state.simulation.activeMode = 0;
  }
  DrawSolverCombo(state, *s);
  DrawStudyMaterial(state, *s);
  if (s->shapeFrom.study == 0)
    DrawStudyOwnMeshSize(state, *s, "Own mesh (mm, 0=shared)");
  // What the stored result keeps (docs/simulation/README.md#a-full-result):
  // the surface, or the whole field so a reopened result can be cut and
  // probed inside. A topology result keeps its whole mesh already.
  if (s->type != fem::AnalysisType::Topology) {
    int keep = s->keep == fem::KeepMode::Full ? 1 : 0;
    const char* kKeep[] = {"Surface", "Whole field"};
    LeftLabel("Keep");
    if (ImGui::Combo("###study_keep", &keep, kKeep, 2)) {
      const fem::KeepMode k =
          keep == 1 ? fem::KeepMode::Full : fem::KeepMode::Surface;
      if (k != s->keep)
        ModifyStudy(state, s->id, "Set what the result keeps",
                    [&](fem::Study& cs) { cs.keep = k; });
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Surface: what the panels read, small on disk.\n"
                        "Whole field: every node and element, so a reopened "
                        "result can be sectioned, probed inside and "
                        "exported.");
  }

  if (s->type == fem::AnalysisType::Modal ||
      s->type == fem::AnalysisType::Buckling) {
    int nm = s->numModes;
    LeftLabel(s->type == fem::AnalysisType::Buckling ? "Factors" : "Modes");
    const bool nmChanged = ExprInputInt(state, "###study_modes", &nm);
    if ((nmChanged || ImGui::IsItemDeactivatedAfterEdit()) && nm >= 1 &&
        nm != s->numModes)
      ModifyStudy(state, s->id, "Set mode count",
                  [&](fem::Study& cs) { cs.numModes = nm; });
    // Free-free (docs/simulation/README.md#free-free-modes): the part with
    // no support. Offered only on a study that holds nothing: a support is
    // never dropped to make it so.
    if (s->type == fem::AnalysisType::Modal) {
      bool free = s->freeFree;
      const bool held = !s->loads.empty() && !s->freeFree;
      ImGui::BeginDisabled(held);
      if (ImGui::Checkbox("Free-free: no support###study_free", &free))
        ModifyStudy(state, s->id,
                    free ? "Make study free-free" : "Make study supported",
                    [&](fem::Study& cs) { cs.freeFree = free; });
      ImGui::EndDisabled();
      if (held && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Remove its supports first: a free-free study "
                          "holds nothing");
      if (s->freeFree)
        Hint("Six rigid modes come first, then %d elastic ones", s->numModes);
    }
  } else if (s->type == fem::AnalysisType::Topology) {
    // Topology: target volume, minimum-member (filter) size, iteration cap.
    float vf = static_cast<float>(s->volumeFraction);
    LeftLabel("Volume fraction");
    if (ImGui::SliderFloat("###topo_volume_fraction", &vf, 0.05f, 0.9f, "%.2f"))
      ModifyStudy(state, s->id, "Set volume fraction", [&](fem::Study& cs) {
        cs.volumeFraction = vf;
        cs.volumeFraction.expr.clear();
      });
    double fr = s->filterRadius;
    LeftLabel("Filter radius (mm, 0=auto)");
    std::string frExpr;
    const bool frChanged =
        ExprInputDouble(state, "###topo_filter_radius", &fr, "%.3g", &frExpr);
    if ((frChanged || ImGui::IsItemDeactivatedAfterEdit()) &&
        (fr != s->filterRadius || frExpr != s->filterRadius.expr))
      ModifyStudy(state, s->id, "Set filter radius", [&](fem::Study& cs) {
        cs.filterRadius = fr;
        cs.filterRadius.expr = frExpr;
      });
    int mi = s->maxIterations;
    LeftLabel("Max iterations");
    const bool miChanged = ExprInputInt(state, "###topo_max_iterations", &mi);
    if ((miChanged || ImGui::IsItemDeactivatedAfterEdit()) && mi >= 1 &&
        mi != s->maxIterations)
      ModifyStudy(state, s->id, "Set max iterations",
                  [&](fem::Study& cs) { cs.maxIterations = mi; });
    // Kept faces are the tree's rows, added with the toolbar's Keep.
    DrawKeepOuts(state, *s);
  } else if (s->type == fem::AnalysisType::Thermal) {
    // Whether a run refines round its peak temperature rise until it
    // settles (docs/simulation/README.md#thermal).
    const char* kConverge[] = {"Auto", "Always", "Never"};
    int conv = static_cast<int>(s->converge);
    LeftLabel("Converge peak");
    if (ImGui::Combo("###thermal_converge", &conv, kConverge, 3) &&
        conv != static_cast<int>(s->converge))
      ModifyStudy(state, s->id, "Set converge", [&](fem::Study& cs) {
        cs.converge = static_cast<fem::ConvergeMode>(conv);
      });
  } else {
    DrawThermalLoad(state, *s);
    // Static: whether a run refines round its peak until it settles
    // (docs/simulation/README.md#a-peak-that-has-settled).
    const char* kConverge[] = {"Auto", "Always", "Never"};
    int conv = static_cast<int>(s->converge);
    LeftLabel("Converge peak");
    if (ImGui::Combo("###study_converge", &conv, kConverge, 3) &&
        conv != static_cast<int>(s->converge))
      ModifyStudy(state, s->id, "Set converge", [&](fem::Study& cs) {
        cs.converge = static_cast<fem::ConvergeMode>(conv);
      });
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "Auto refines round the peak stress until it moves under 2 %%,\n"
          "while a solve takes under the Preferences limit (Simulation).\n"
          "Always refines whatever it costs; Never solves once.");
    // Past linear, on CalculiX (docs/simulation/README.md#nonlinear-static).
    if (s->type == fem::AnalysisType::Static) {
      const auto refused = [&](const fem::Nonlinear& nl) {
        return fem::NonlinearRefusal(s->solver, s->type, nl,
                                     s->loadCases.size(), s->converge,
                                     fem::HasContact(*s));
      };
      // Each switch keeps the other; off together, the study is linear.
      const auto toggled = [&](bool fem::Nonlinear::* key) {
        fem::Nonlinear nl = s->nonlinear;
        nl.*key = !(nl.*key);
        if (!nl.largeDeformation) {
          nl.snapThrough = false;
          nl.hasImperfection = nl.hasDeflectionLimit = false;
          nl.imperfectionMm = nl.deflectionLimitMm = 0.0;
        }
        if (!nl.On() && !fem::HasContact(*s))
          nl = fem::Nonlinear{};
        return nl;
      };
      const fem::Nonlinear large = toggled(&fem::Nonlinear::largeDeformation);
      const std::string whyNot = refused(large);
      bool on = s->nonlinear.largeDeformation;
      ImGui::BeginDisabled(!on && !whyNot.empty());
      if (ImGui::Checkbox("Large deformation###study_nonlinear", &on))
        ModifyStudy(state, s->id, "Set large deformation",
                    [&](fem::Study& cs) { cs.nonlinear = large; });
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            "%s", !whyNot.empty() && !on
                      ? whyNot.c_str()
                      : "Solved on CalculiX with the load stepped up on the "
                        "deformed part:\na pressure follows its face, a "
                        "force keeps its direction.");
      const fem::Nonlinear plastic = toggled(&fem::Nonlinear::plasticity);
      const std::string whyPlastic = refused(plastic);
      bool yields = s->nonlinear.plasticity;
      ImGui::BeginDisabled(!yields && !whyPlastic.empty());
      if (ImGui::Checkbox("Plasticity###study_plasticity", &yields))
        ModifyStudy(state, s->id, "Set plasticity",
                    [&](fem::Study& cs) { cs.nonlinear = plastic; });
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(
            "%s", !whyPlastic.empty() && !yields
                      ? whyPlastic.c_str()
                      : "Solved on CalculiX past yield, along the material's "
                        "hardening curve;\nthe load is then taken off for "
                        "the deformation it leaves.\nA material with no "
                        "curve is refused when the study runs.");
      if (s->nonlinear.largeDeformation) {
        fem::Nonlinear snapped = s->nonlinear;
        snapped.snapThrough = !snapped.snapThrough;
        if (!snapped.snapThrough) {
          snapped.hasImperfection = snapped.hasDeflectionLimit = false;
          snapped.imperfectionMm = snapped.deflectionLimitMm = 0.0;
        }
        const std::string whySnap = refused(snapped);
        bool snap = s->nonlinear.snapThrough;
        ImGui::BeginDisabled(!whySnap.empty());
        if (ImGui::Checkbox("Snap-through (peak load)###study_snap", &snap))
          ModifyStudy(state, s->id, "Set snap-through",
                      [&](fem::Study& cs) { cs.nonlinear = snapped; });
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip(
              "%s", !whySnap.empty()
                        ? whySnap.c_str()
                        : "Seeds the shape with the first buckling mode and "
                          "steps the load up\nuntil the part stops carrying "
                          "more: the answer for an arch or a\ncurved wall "
                          "loaded across its curve.");
      }
    }
    // Static: an optional gravity / self-weight load.
    bool on = s->gravity.x != 0 || s->gravity.y != 0 || s->gravity.z != 0;
    if (ImGui::Checkbox("Gravity (self-weight)###study_gravity", &on)) {
      // Straight down in the WORLD frame — a hand-written axis here once
      // sagged the part sideways (docs/conventions.md#world-axes).
      const glm::dvec3 g = on ? conv::kGravity : glm::dvec3(0, 0, 0);
      ModifyStudy(state, s->id, "Toggle gravity",
                  [&](fem::Study& cs) { cs.gravity = g; });
    }
    if (on) {
      double g[3] = {s->gravity.x, s->gravity.y, s->gravity.z};
      LeftLabel("g (m/s^2)");
      // Commit on deactivation (Enter OR clicking away), like the mesh-size
      // field above. NOT EnterReturnsTrue: that flag is InputText-only —
      // InputScalar ASSERTS on it, so this line aborted every Debug build
      // (a fresh clone's default) the moment gravity was switched on.
      const bool gChanged =
          ExprInputDouble3(state, "###study_gravity_vec", g, "%.2f");
      if (gChanged || ImGui::IsItemDeactivatedAfterEdit())
        ModifyStudy(state, s->id, "Set gravity", [&](fem::Study& cs) {
          cs.gravity = glm::dvec3(g[0], g[1], g[2]);
        });
    }
  }

  // A study that states its own material reads no body's.
  if (!s->material)
    DrawMaterialCheck(state);
  DrawAssemblySetupCheck(state);
}

// What a support or a load is (the tree's row in focus): its kind and face,
// its value as placed, and the case it is in. A placed load is not edited
// in place but removed and placed again -- except a remote load's point,
// which IS the load, and the case a force goes in.
void DrawLoadWindow(AppState& state, const fem::Study& s, uint64_t loadId) {
  const StudyHost host = StudyHostOf(state);
  const fem::Study shown = host.Shown(s);
  const fem::Load* l = nullptr;
  for (const fem::Load& x : shown.loads)
    if (x.id == loadId)
      l = &x;
  if (l == nullptr) {
    ImGui::TextColored(kMuted, "That load is gone.");
    return;
  }
  const std::string where =
      l->contactWith != 0
          ? "contact with " + host.OccurrenceName(l->contactWith)
      : l->occurrenceId != 0
          ? host.OccurrenceName(l->occurrenceId) + " " + l->face.name
      : l->type == fem::LoadType::HeatGeneration ? l->body
                                                 : l->face.name;
  LeftLabel("On");
  ImGui::TextUnformatted(where.c_str());
  const std::string value = SimLoadTail(*l);
  if (!value.empty()) {
    LeftLabel(fem::IsSupport(l->type) ? "Holds" : "Value");
    ImGui::TextUnformatted(value.c_str());
  }
  if (l->type == fem::LoadType::Force || l->type == fem::LoadType::Bearing) {
    LeftLabel("Direction");
    ImGui::Text("[%.3g, %.3g, %.3g]", l->direction.x, l->direction.y,
                l->direction.z);
  }
  if (!l->magnitude.expr.empty()) {
    LeftLabel("From");
    ImGui::TextUnformatted(l->magnitude.expr.c_str());
  }
  if (l->type == fem::LoadType::Remote) {
    // The point after placing: where the force acts is the load.
    glm::dvec3 p = l->point;
    LeftLabel("At (mm)");
    if (ExprInputDouble3(state, "###load_point", &p.x, "%.2f") && p != l->point)
      ModifyStudy(state, s.id, "Move remote load", [&](fem::Study& cs) {
        for (fem::Load& x : cs.loads)
          if (x.id == loadId)
            x.point = p;
      });
  }
  if (!s.loadCases.empty() && !fem::IsSupport(l->type)) {
    const int k = std::max(0, s.FindLoadCase(l->loadCase));
    LeftLabel("In case");
    if (ImGui::BeginCombo("###load_case", s.loadCases[k].name.c_str())) {
      for (size_t i = 0; i < s.loadCases.size(); ++i)
        if (ImGui::Selectable(s.loadCases[i].name.c_str(),
                              static_cast<int>(i) == k)) {
          const std::string to = s.loadCases[i].name;
          ModifyStudy(state, s.id, "Move load to case", [&](fem::Study& cs) {
            for (fem::Load& x : cs.loads)
              if (x.id == loadId)
                x.loadCase = to;
          });
        }
      ImGui::EndCombo();
    }
  }
  Hint("Delete removes it; place it again from the toolbar to change it.");
}

// The simulation's subject (the tree's first row): what it analyses, and a
// body with no material, which every study needs.
void DrawSubjectWindow(AppState& state) {
  const StudyHost host = StudyHostOf(state);
  const fem::SimSubject& subject = host.sim->Subject();
  LeftLabel("Analyses");
  ImGui::Text("%s '%s'", fem::SubjectKindToString(subject.kind),
              subject.name.c_str());
  if (host.OnAssembly())
    Hint("Each occurrence's role is its row's menu, under this one.");
  DrawMaterialCheck(state);
  DrawAssemblySetupCheck(state);
}
} // namespace

void DrawStudyPanel(AppState& state) {
  ImGui::Begin(kPanelModel);
  PanelHeading(kPanelModel);
  // What the simulation holds, and nothing that acts on it: the tools are
  // the toolbar's (docs/ui/panels.md#simulation-tab-the-model-panel).
  DrawSimTree(state);
  ImGui::End();
}

namespace {

// The armed tool's name, for the window's title.
const char* ToolTitle(SimulationState::Tool t) {
  using T = SimulationState::Tool;
  switch (t) {
  case T::Fixed:
    return "Fixed support";
  case T::Force:
    return "Force";
  case T::Pressure:
    return "Pressure";
  case T::Keep:
    return "Keep face";
  case T::Torque:
    return "Torque";
  case T::Remote:
    return "Remote load";
  case T::Probe:
    return "Probe";
  case T::Pin:
    return "Pin";
  case T::Contact:
    return "Contact";
  case T::Temperature:
    return "Temperature";
  case T::HeatFlux:
    return "Heat flux";
  case T::Convection:
    return "Convection";
  case T::HeatSource:
    return "Heat source";
  case T::Roller:
    return "Roller";
  case T::Held:
    return "Held directions";
  case T::Cylindrical:
    return "Cylindrical support";
  case T::Suppress:
    return "Suppress for the analysis";
  case T::Symmetry:
    return "Symmetry plane";
  case T::Refine:
    return "Refine";
  case T::None:
    break;
  }
  return "Simulation";
}

} // namespace

void DrawSimulationToolWindow(AppState& state) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return;
  using K = SimulationState::Focus::Kind;
  const fem::Study* active = host.Find(sim.activeStudyId);
  // A focus whose thing went (a study deleted, an undo) shows nothing.
  const SimulationState::Focus f = sim.focus;
  const fem::Study* fs = f.studyId != 0 ? host.Find(f.studyId) : nullptr;
  const bool needsStudy = f.kind != K::None && f.kind != K::Subject &&
                          f.kind != K::Participant && f.kind != K::Geometry;
  if (needsStudy && fs == nullptr)
    sim.focus = {};
  const bool armed = sim.tool != SimulationState::Tool::None && active;
  if (!armed && sim.focus.kind == K::None)
    return;

  // The title says what the window is about, so the rows need no heading.
  std::string title;
  if (armed)
    title = ToolTitle(sim.tool);
  else
    switch (sim.focus.kind) {
    case K::Subject:
      title = "Subject";
      break;
    case K::Participant:
      title = host.OccurrenceName(sim.focus.id);
      break;
    case K::Study:
      title = "Study: " + fs->name;
      break;
    case K::Mesh:
      // The simulation's mesh, or one study's own.
      title = fem::SimDocument::SharesMesh(*fs) && fs->shapeFrom.study == 0
                  ? std::string("Mesh")
                  : "Mesh: " + fs->name;
      break;
    case K::Load: {
      title = "Load";
      for (const fem::Load& l : fs->loads)
        if (l.id == sim.focus.id)
          title = SimLoadName(l.type);
      break;
    }
    case K::Case:
      title = sim.focus.index >= 0 &&
                      sim.focus.index < static_cast<int>(fs->loadCases.size())
                  ? "Load case: " +
                        fs->loadCases[static_cast<size_t>(sim.focus.index)].name
                  : "Load case";
      break;
    case K::Keep:
      title = "Kept face";
      break;
    case K::KeepOut:
      title = "Keep-out";
      break;
    case K::Result:
      title = "Result: " + fs->name;
      break;
    case K::Check:
      title = "Check: " + fs->name;
      break;
    case K::Geometry:
      title = "Geometry";
      break;
    case K::None:
      break;
    }
  BeginToolWindowFrame(state, title);
  if (armed) {
    if (sim.tool == SimulationState::Tool::Probe) {
      Hint("Hover a face for its von Mises stress; click to pin it.");
      if (!sim.placeRefusal.empty())
        MessageText(kError, sim.placeRefusal);
      DrawProbes(state, *active);
    } else if (sim.tool == SimulationState::Tool::Suppress ||
               sim.tool == SimulationState::Tool::Symmetry ||
               sim.tool == SimulationState::Tool::Refine) {
      DrawSimGeometryTool(state);
    } else {
      DrawArmedTool(state, *active);
    }
  } else {
    switch (sim.focus.kind) {
    case K::Subject:
      DrawSubjectWindow(state);
      break;
    case K::Participant:
      DrawParticipantWindow(state, sim.focus.id);
      break;
    case K::Study:
      DrawStudySettings(state, fs);
      break;
    case K::Mesh:
      DrawMeshWindow(state, *fs);
      break;
    case K::Load:
      DrawLoadWindow(state, *fs, sim.focus.id);
      break;
    case K::Case:
      DrawCaseWindow(state, *fs, sim.focus.index);
      break;
    case K::Keep:
      Hint("Kept solid: the optimizer leaves its material. Its row's menu "
           "removes it.");
      break;
    case K::KeepOut:
      Hint("Kept empty: its tets are removed before the run. Its row's menu "
           "removes it.");
      break;
    case K::Result:
      DrawResults(state, *fs);
      break;
    case K::Check:
      DrawStudyCheck(state, *fs);
      break;
    case K::Geometry:
      DrawSimGeometryWindow(state);
      break;
    case K::None:
      break;
    }
  }
  ImGui::Separator();
  // Close, not Esc (which leaves the simulation): the window applies as you
  // go, and a field it shows outlives it.
  if (ImGui::Button(armed ? "Done###simulation_close"
                          : "Close###simulation_close")) {
    if (armed)
      sim.tool = SimulationState::Tool::None;
    else
      sim.focus = {};
  }
  ImGui::End();
}

void SetSimParticipantRole(AppState& state, uint64_t occurrenceId,
                           fem::ParticipantRole role) {
  const StudyHost host = StudyHostOf(state);
  if (!host || !host.OnAssembly())
    return;
  SimUndoGuard guard(state);
  std::vector<fem::Participant> roles = host.sim->Roles();
  auto it =
      std::find_if(roles.begin(), roles.end(), [&](const fem::Participant& x) {
        return x.occurrenceId == occurrenceId;
      });
  if (role == fem::ParticipantRole::Ignored) {
    if (it != roles.end())
      roles.erase(it);
    // Its contact supports go with it, in every study: a clamp on a
    // support that is absent clamps nothing.
    for (const fem::Study& st : std::vector<fem::Study>(host.Studies().begin(),
                                                        host.Studies().end())) {
      fem::Study edited = st;
      if (std::erase_if(edited.loads, [&](const fem::Load& l) {
            return l.contactWith == occurrenceId;
          }) > 0)
        host.sim->ReplaceStudy(std::move(edited));
    }
  } else {
    if (it == roles.end()) {
      fem::Participant np;
      np.occurrenceId = occurrenceId;
      np.role = role;
      roles.push_back(np);
    } else {
      it->role = role;
    }
    if (role == fem::ParticipantRole::Meshed) // one meshed at a time (S1)
      for (fem::Participant& other : roles)
        if (other.occurrenceId != occurrenceId &&
            other.role == fem::ParticipantRole::Meshed)
          other.role = fem::ParticipantRole::Rigid;
  }
  host.sim->SetRoles(std::move(roles));
}

namespace {
// Analytic Turbo colormap (matches field.frag) for the legend colorbar.
ImU32 Turbo(float x) {
  x = x < 0 ? 0 : (x > 1 ? 1 : x);
  const float r = 0.13572138f +
                  x * (4.61539260f + x * (-42.66032258f + x * 132.13108234f)) +
                  x * x * x * x * (-152.94239396f + x * 59.28637943f);
  const float g = 0.09140261f +
                  x * (2.19418839f + x * (4.84296658f + x * -14.18503333f)) +
                  x * x * x * x * (4.27729857f + x * 2.82956604f);
  const float b = 0.10667330f +
                  x * (12.64194608f + x * (-60.58204836f + x * 110.36276771f)) +
                  x * x * x * x * (-89.90310912f + x * 27.34824973f);
  auto ch = [](float v) {
    return (int)(255.0f * (v < 0 ? 0 : (v > 1 ? 1 : v)));
  };
  return IM_COL32(ch(r), ch(g), ch(b), 255);
}
} // namespace

namespace {

// The legend's result controls (docs/simulation/README.md#results-display).
// The bar leads and the controls fold: the legend's face is what is read --
// its title, which is the field picker, the mode or the case where there is
// more than one, the study, the bar and where its ends are -- and the gear on
// the title row opens the rest beside it. What is set away from its default
// in there is said on the face, in one dim line, so a clamped range or a
// mirrored half is never a picture nobody can tell from the whole one.

using LegendField = SimulationState::Field;

// What the study's kind makes of its result's display.
struct LegendKind {
  bool thermal = false, modal = false, buckling = false, topo = false;
};

LegendKind LegendKindOf(const fem::Study& s) {
  LegendKind k;
  k.thermal = s.type == fem::AnalysisType::Thermal;
  k.buckling = s.type == fem::AnalysisType::Buckling;
  k.modal = s.type == fem::AnalysisType::Modal || k.buckling;
  k.topo = s.type == fem::AnalysisType::Topology;
  return k;
}

std::string Capitalised(std::string t) {
  if (!t.empty() && t.rfind("von ", 0) != 0)
    t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
  return t;
}

// A field's name as the picker lists it.
std::string LegendFieldName(LegendField f) {
  if (f == LegendField::None)
    return "Off";
  if (f == LegendField::Shape)
    return "Shape";
  if (f == LegendField::Density)
    return "Density";
  return Capitalised(fem::SpecOf(*ResultFieldOf(f)).title);
}

// Which of the folded controls the shown result has: the popover draws these
// rows, the gear is there when there is one, and the face's summary reads
// the same flags.
struct LegendMore {
  bool deform = false, layers = false, threshold = false, range = false,
       yield = false, markers = false, mirror = false;
  bool Any() const {
    return deform || layers || threshold || range || markers || mirror;
  }
};

LegendMore LegendMoreOf(AppState& state, const fem::Study& s,
                        const ShownField* shown) {
  const SimulationState& sim = state.simulation;
  const LegendKind k = LegendKindOf(s);
  LegendMore m;
  if (sim.field == LegendField::None)
    return m;
  if (k.topo) {
    m.threshold = sim.field == LegendField::Shape;
    return m;
  }
  m.deform = !k.thermal;
  m.layers = sim.field == LegendField::AcrossLayers ||
             sim.field == LegendField::InterlayerFactor;
  m.range = !k.modal && shown != nullptr && !shown->mesh && !shown->normalized;
  m.markers = m.range;
  m.yield = m.range && (sim.field == LegendField::VonMises ||
                        sim.field == LegendField::PrincipalMax ||
                        sim.field == LegendField::PrincipalMin);
  const StudyHost host = StudyHostOf(state);
  m.mirror = host && host.sim->Geometry().symmetry.On();
  return m;
}

// The picker that is the legend's title: the field, read as a title until
// it is hovered.
void DrawLegendFieldCombo(AppState& state, const fem::Study& s,
                          const char* preview) {
  SimulationState& sim = state.simulation;
  const LegendKind k = LegendKindOf(s);
  const auto choose = [&](LegendField f) {
    if (sim.field != f) {
      sim.field = f;
      sim.clampLo = sim.clampHi = false; // a range is one field's
    }
    if (f != LegendField::None)
      sim.meshView = SimulationState::MeshView::Off;
  };
  if (!ImGui::BeginCombo("###result_field", preview,
                         ImGuiComboFlags_WidthFitPreview))
    return;
  if (ImGui::Selectable("Off###field_off", sim.field == LegendField::None))
    choose(LegendField::None);
  if (k.modal) {
    if (ImGui::Selectable("Mode shape###field_mode_shape",
                          sim.field != LegendField::None))
      choose(LegendField::Displacement);
  } else if (k.topo) {
    if (ImGui::Selectable("Shape###field_shape",
                          sim.field == LegendField::Shape))
      choose(LegendField::Shape);
    if (ImGui::Selectable("Density###field_density",
                          sim.field == LegendField::Density))
      choose(LegendField::Density);
  } else {
    using F = LegendField;
    static constexpr F kStatic[] = {
        F::VonMises,        F::Displacement,     F::PrincipalMax,
        F::PrincipalMid,    F::PrincipalMin,     F::FactorOfSafety,
        F::AcrossLayers,    F::InterlayerFactor, F::PlasticStrain,
        F::ContactPressure, F::Temperature};
    static constexpr F kThermal[] = {F::Temperature, F::HeatFlux};
    // What this study's kind has at all: a plastic strain only with
    // plasticity, a contact pressure only with a contact, a temperature
    // only under a thermal load. One its result does not hold is listed
    // greyed with why.
    const auto offered = [&](F f) {
      if (f == F::PlasticStrain)
        return s.nonlinear.plasticity;
      if (f == F::ContactPressure)
        return fem::HasContact(s);
      if (f == F::Temperature && !k.thermal)
        return s.thermalLoad.On();
      return true;
    };
    const auto item = [&](F f) {
      if (!offered(f))
        return;
      const std::string why = FieldRefusal(state, s.id, f);
      const std::string id =
          LegendFieldName(f) + "###field_" + fem::SpecOf(*ResultFieldOf(f)).key;
      ImGui::BeginDisabled(!why.empty());
      if (ImGui::Selectable(id.c_str(), sim.field == f))
        choose(f);
      ImGui::EndDisabled();
      if (!why.empty() &&
          ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", why.c_str());
    };
    if (k.thermal)
      for (F f : kThermal)
        item(f);
    else
      for (F f : kStatic)
        item(f);
  }
  ImGui::EndCombo();
}

// The thing changed most, on the face: which mode, or which load case, when
// the result has more than one. One of each is not a choice and is not
// drawn.
struct LegendPick {
  const char* id = nullptr; // "###result_mode" / "###result_case"
  const char* itemPrefix = nullptr;
  std::vector<std::string> items;
  int* current = nullptr;
};

LegendPick LegendPickOf(AppState& state, const fem::Study& s,
                        const fem::StudyRuntime& rt) {
  SimulationState& sim = state.simulation;
  const LegendKind k = LegendKindOf(s);
  LegendPick p;
  if (sim.field == LegendField::None || k.topo)
    return p;
  if (k.modal) {
    const fem::StudyBodyResult* b = nullptr;
    for (const fem::StudyBodyResult& br : rt.bodies)
      if (!br.results.modes.empty()) {
        b = &br;
        break;
      }
    if (b == nullptr)
      return p;
    p.id = "###result_mode";
    p.itemPrefix = "###result_mode_";
    char buf[64];
    for (size_t i = 0; i < b->results.modes.size(); ++i) {
      const auto& m = b->results.modes[i];
      if (k.buckling)
        std::snprintf(buf, sizeof(buf), "%zu:  x %.4g", i + 1, m.factor);
      else if (m.rigid)
        std::snprintf(buf, sizeof(buf), "%zu:  rigid", i + 1);
      else
        std::snprintf(buf, sizeof(buf), "%zu:  %.4g Hz", i + 1, m.frequencyHz);
      p.items.emplace_back(buf);
    }
    p.current = &sim.activeMode;
  } else if (!rt.caseNames.empty()) {
    p.id = "###result_case";
    p.itemPrefix = "###result_case_";
    p.items = rt.caseNames;
    p.current = &sim.activeCase;
  }
  if (p.current != nullptr)
    *p.current =
        std::clamp(*p.current, 0, static_cast<int>(p.items.size()) - 1);
  return p;
}

void DrawLegendPick(const LegendPick& p) {
  if (!ImGui::BeginCombo(p.id, p.items[*p.current].c_str(),
                         ImGuiComboFlags_WidthFitPreview))
    return;
  for (size_t i = 0; i < p.items.size(); ++i)
    if (ImGui::Selectable(
            (p.items[i] + p.itemPrefix + std::to_string(i)).c_str(),
            *p.current == static_cast<int>(i)))
      *p.current = static_cast<int>(i);
  ImGui::EndCombo();
}

// The folded controls that are not at their default, said in one line:
// "deformed ×200 · range to 40 · mirrored". Empty when everything is.
std::string LegendSummary(const SimulationState& sim, const LegendMore& m) {
  std::vector<std::string> parts;
  char buf[96];
  if (m.deform && sim.deformScale > 0.0f) {
    std::snprintf(buf, sizeof(buf), "deformed \xc3\x97%.3g", sim.deformScale);
    parts.emplace_back(buf);
  }
  if (m.layers) {
    const int axis = std::abs(sim.layerNormal.x) > 0.5   ? 0
                     : std::abs(sim.layerNormal.y) > 0.5 ? 1
                                                         : 2;
    parts.emplace_back(std::string("layers along ") + "XYZ"[axis]);
  }
  if (m.threshold && sim.topoThreshold > 0.0f) {
    std::snprintf(buf, sizeof(buf), "threshold %.2f", sim.topoThreshold);
    parts.emplace_back(buf);
  }
  // The range in the title's unit, which the line does not repeat.
  if (m.range && (sim.clampLo || sim.clampHi)) {
    if (sim.clampLo && sim.clampHi)
      std::snprintf(buf, sizeof(buf), "range %.3g to %.3g", sim.rangeLo,
                    sim.rangeHi);
    else if (sim.clampHi)
      std::snprintf(buf, sizeof(buf), "range to %.3g", sim.rangeHi);
    else
      std::snprintf(buf, sizeof(buf), "range from %.3g", sim.rangeLo);
    parts.emplace_back(buf);
  }
  if (m.markers && !sim.showExtremes)
    parts.emplace_back("markers off");
  if (m.mirror && sim.mirror)
    parts.emplace_back("mirrored");
  std::string out;
  for (const std::string& p : parts)
    out += (out.empty() ? "" : " \xc2\xb7 ") + p;
  return out;
}

// The popover's rows: a label column and one control column, every control
// the same width, the toggles together at the bottom.
// The gear's popover, by the one name it is opened, asked after and drawn
// under (GUI tests reach its window with LegendMore).
constexpr const char* kLegendMorePopup = "###legend_more";
constexpr const char* kMarkersLabel = "Max and min markers###result_extremes";
constexpr const char* kMirrorLabel = "Mirrored half###result_mirror";
void DrawLegendMoreRows(AppState& state, const fem::Study& s,
                        const LegendMore& m, const ShownField* shown) {
  SimulationState& sim = state.simulation;
  const ImGuiStyle& style = ImGui::GetStyle();
  const std::string rangeLabel = shown != nullptr && !shown->unit.empty()
                                     ? "Range (" + shown->unit + ")"
                                     : std::string("Range");
  float labelW = 0.0f;
  for (const char* t : {"Deform", "Layers", "Threshold", "Show"})
    labelW = std::max(labelW, ImGui::CalcTextSize(t).x);
  if (m.range)
    labelW = std::max(labelW, ImGui::CalcTextSize(rangeLabel.c_str()).x);
  labelW += style.ItemSpacing.x * 2.0f;
  // One width for every control, wide enough for the toggles' labels.
  float ctlW = ImGui::GetFontSize() * 9.0f;
  const float box = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x;
  if (m.markers)
    ctlW = std::max(ctlW,
                    box + ImGui::CalcTextSize(kMarkersLabel, nullptr, true).x);
  if (m.mirror)
    ctlW = std::max(ctlW,
                    box + ImGui::CalcTextSize(kMirrorLabel, nullptr, true).x);
  const float x0 = ImGui::GetCursorPosX();
  const auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "%s", text);
    ImGui::SameLine(x0 + labelW);
    ImGui::SetNextItemWidth(ctlW);
  };
  const auto indent = [&] { ImGui::SetCursorPosX(x0 + labelW); };

  if (m.deform) {
    label("Deform");
    ImGui::SliderFloat("###deform", &sim.deformScale, 0.0f, 100.0f,
                       "\xc3\x97%.0f");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How many times the displacement the shape is drawn "
                        "deformed by. Ctrl+click to type a larger one.");
  }
  // The build direction of the layered fields: a question asked of the
  // result, so it is here with the field.
  if (m.layers) {
    int axis = std::abs(sim.layerNormal.x) > 0.5   ? 0
               : std::abs(sim.layerNormal.y) > 0.5 ? 1
                                                   : 2;
    label("Layers");
    static const char* kAxes[] = {"along X", "along Y", "along Z"};
    if (ImGui::Combo("###result_layer_axis", &axis, kAxes, 3)) {
      sim.layerNormal = glm::dvec3(0.0);
      sim.layerNormal[axis] = 1.0;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The direction the part was built up in, in its "
                        "own axes.");
  }
  if (m.threshold) {
    float thr = sim.topoThreshold;
    label("Threshold");
    if (ImGui::SliderFloat("###result_threshold", &thr, 0.0f, 1.0f,
                           thr == 0.0f ? "auto" : "%.2f"))
      sim.topoThreshold = thr;
  }
  // The range: an end left empty is the field's own; past an end the
  // picture takes the over or the under colour.
  if (m.range) {
    const float dashW = ImGui::CalcTextSize("to").x;
    const float inner = style.ItemInnerSpacing.x;
    const float boxW = (ctlW - dashW - 2.0f * inner) * 0.5f;
    const auto bound = [&](const char* id, const char* hint, bool& on,
                           double& v) {
      char buf[32] = "";
      if (on)
        std::snprintf(buf, sizeof(buf), "%.4g", v);
      ImGui::SetNextItemWidth(boxW);
      if (ImGui::InputTextWithHint(id, hint, buf, sizeof(buf))) {
        char* end = nullptr;
        const double d = std::strtod(buf, &end);
        on = end != buf && *end == '\0' && std::isfinite(d);
        if (on)
          v = d;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Empty: the field's own %s.", hint);
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "%s", rangeLabel.c_str());
    ImGui::SameLine(x0 + labelW);
    bound("###result_range_lo", "min", sim.clampLo, sim.rangeLo);
    ImGui::SameLine(0.0f, inner);
    ImGui::TextColored(kMuted, "to");
    ImGui::SameLine(0.0f, inner);
    bound("###result_range_hi", "max", sim.clampHi, sim.rangeHi);
    // The yield as the top of a stress, so what is over it reads as yielded.
    if (m.yield) {
      indent();
      const std::optional<double> yield = YieldClampMPa(state, s.id);
      ImGui::BeginDisabled(!yield.has_value());
      if (ImGui::Button("Above yield###result_range_yield", ImVec2(ctlW, 0))) {
        sim.clampHi = true;
        sim.rangeHi = *yield;
      }
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", yield ? "Tops the range at the material's "
                                        "yield strength: what is over it "
                                        "takes the over colour."
                                      : "The material has no yield strength.");
    }
  }
  // The toggles, together: the markers at the field's ends, and a half
  // model drawn with its mirror image.
  if (m.markers || m.mirror) {
    if (m.deform || m.layers || m.threshold || m.range)
      ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kMuted, "Show");
    ImGui::SameLine(x0 + labelW);
    if (m.markers) {
      ImGui::Checkbox(kMarkersLabel, &sim.showExtremes);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Mark the field's maximum and minimum, with their "
                          "values.");
      if (m.mirror)
        indent();
    }
    if (m.mirror) {
      ImGui::Checkbox(kMirrorLabel, &sim.mirror);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The study solved half the part, cut on its "
                          "symmetry plane: draw the other half as its mirror "
                          "image.");
    }
  }
}

// The gear's popover, beside the legend and level with its foot. It closes
// on a click away and on Esc, as the app's other popovers do.
void DrawLegendMorePopover(AppState& state, const fem::Study& s,
                           const LegendMore& m, const ShownField* shown) {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  ImGui::SetNextWindowPos(ImVec2(pos.x + size.x + 6.0f, pos.y + size.y),
                          ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 20, 26, 240));
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
  const bool open = ImGui::BeginPopup(kLegendMorePopup);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  if (!open)
    return;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    ImGui::CloseCurrentPopup();
  DrawLegendMoreRows(state, s, m, shown);
  ImGui::EndPopup();
}

} // namespace

std::string FieldLegendSummary(AppState& state) {
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return {};
  const fem::Study* study = host.Find(state.simulation.activeStudyId);
  if (study == nullptr || host.Results(study->id) == nullptr)
    return {};
  const ShownField* shown = state.simulation.shown.get();
  return LegendSummary(state.simulation, LegendMoreOf(state, *study, shown));
}

void DrawFieldLegend(AppState& state) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host)
    return;
  // What the overlay last built (ui::ShownField): the bar describes the
  // picture that is drawn, so a field the result does not hold, the grey
  // Shape iso-surface and the plain mesh have no colour bar.
  const ShownField* shown = sim.shown.get();
  const fem::StudyRuntime* rt = host.Results(sim.activeStudyId);
  const fem::Study* study = host.Find(sim.activeStudyId);
  const bool meshBar = shown != nullptr && !shown->plain && shown->mesh &&
                       sim.meshView != SimulationState::MeshView::Off;
  bool bar = shown != nullptr && !shown->plain &&
             (shown->mesh
                  ? meshBar
                  : sim.field != SimulationState::Field::None && rt != nullptr);
  // The controls are the result's, while there is one to show and the mesh
  // is not: a mesh view's legend is its bar alone (the mesh's Show combo
  // puts it away).
  const bool controls = study != nullptr && rt != nullptr && !meshBar;
  if (!controls && !bar)
    return;
  const LegendKind kind =
      study != nullptr ? LegendKindOf(*study) : LegendKind{};

  const float lineH = ImGui::GetTextLineHeight();
  const float barW = 14.0f, barH = 160.0f, capH = 8.0f;
  const float margin = 12.0f, tickGap = 8.0f;
  const bool extremes =
      bar && !shown->mesh && !shown->normalized && shown->hasExtremes;

  // The title, which is the field picker when there are controls: the
  // quantity and its unit as the bar draws it.
  std::string title;
  if (bar)
    title = Capitalised(shown->unit.empty()
                            ? shown->title
                            : shown->title + " (" + shown->unit + ")");
  else if (sim.field == SimulationState::Field::None)
    title = "No field";
  else
    title = kind.modal ? "Mode shape" : LegendFieldName(sim.field);

  // What the picture is a picture OF, under the title: the study and its
  // analysis. A colour bar with a number and no subject was a picture a
  // reader had to trust, and two studies of one part draw alike.
  std::string studyLine;
  if (study)
    studyLine = study->name + " \xc2\xb7 " +
                (meshBar         ? "mesh"
                 : kind.buckling ? "buckling"
                 : kind.modal    ? "modal"
                 : kind.topo     ? "topology"
                 : kind.thermal  ? "thermal"
                                 : "static");

  // The ticks: the ends and the quarters, so a colour halfway up the bar is
  // a number too.
  char buf[160];
  std::string ticks[5];
  if (bar) {
    for (int k = 0; k <= 4; ++k) {
      std::snprintf(buf, sizeof(buf), "%.3g",
                    shown->barLo + (shown->barHi - shown->barLo) * (k / 4.0));
      ticks[k] = buf;
    }
    if (shown->topOpen)
      ticks[4] += "+";
  }

  // Under the bar: where the hot end is, in world millimetres and on which
  // body (the magenta cross marks it), and with the markers on, the other
  // end (the pale cross); a mesh's worst element. A factor's hot end is its
  // least value and is the only one marked.
  std::vector<std::string> captions;
  const auto where = [&](const char* what, const glm::dvec3& at,
                         const std::string& body) {
    std::snprintf(buf, sizeof(buf), "%s at (%.1f, %.1f, %.1f)%s%s", what, at.x,
                  at.y, at.z, body.empty() ? "" : " on ", body.c_str());
    captions.push_back(buf);
  };
  if (bar && shown->mesh) {
    if (shown->reversed) {
      std::snprintf(buf, sizeof(buf), "worst %.2f", shown->lo);
      captions.push_back(buf);
    }
  } else if (extremes) {
    if (shown->reversed) {
      where("min", shown->loAt, shown->loBody);
    } else {
      where("max", shown->hiAt, shown->hiBody);
      if (sim.showExtremes)
        where("min", shown->loAt, shown->loBody);
    }
  }

  // The folded controls: which the result has, and what of them is set.
  LegendMore more;
  LegendPick pick;
  std::string summary;
  if (controls) {
    more = LegendMoreOf(state, *study, shown);
    pick = LegendPickOf(state, *study, *rt);
    summary = LegendSummary(sim, more);
  }
  const bool picks = pick.current != nullptr && pick.items.size() > 1;

  // The face's own width: the widest of what it reads, so the gear sits at
  // its right edge without the window growing to hold it.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float comboExtra =
      style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
  const float gearW =
      ImGui::CalcTextSize(ICON_FA_GEAR).x + style.FramePadding.x * 2.0f;
  float tickW = 0.0f;
  for (const std::string& t : ticks)
    tickW = std::max(tickW, ImGui::CalcTextSize(t.c_str()).x);
  const float titleW = controls
                           ? ImGui::CalcTextSize(title.c_str()).x + comboExtra
                           : ImGui::CalcTextSize(title.c_str()).x;
  // (A picker's text starts at the face's edge, its frame padding left of it.)
  float faceW = titleW - (controls ? style.FramePadding.x : 0.0f) +
                (more.Any() ? style.ItemSpacing.x + gearW : 0.0f);
  if (picks)
    faceW = std::max(faceW,
                     ImGui::CalcTextSize(pick.items[*pick.current].c_str()).x +
                         comboExtra - style.FramePadding.x);
  faceW = std::max(faceW, ImGui::CalcTextSize(studyLine.c_str()).x);
  if (bar)
    faceW = std::max(faceW, barW + tickGap + tickW);
  for (const std::string& c : captions)
    faceW = std::max(faceW, ImGui::CalcTextSize(c.c_str()).x);
  faceW = std::max(faceW, ImGui::CalcTextSize(summary.c_str()).x);

  // One window in the viewport's lower-left corner. Its own window so its
  // widgets take the clicks, and a backdrop so it reads over any colour.
  ImGui::SetNextWindowPos(
      ImVec2(state.viewportPos.x + margin,
             state.viewportPos.y + state.viewportSize.y - margin),
      ImGuiCond_Always, ImVec2(0.0f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 20, 26, 232));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing;
  const bool open = ImGui::Begin("##FieldLegend", nullptr, flags);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  if (!open) {
    ImGui::End();
    return;
  }
  RecordViewportObstacle(state); // Fit keeps the model out from under it
  const ImU32 dim = IM_COL32(255, 255, 255, 190);
  const ImVec4 dimText(1, 1, 1, 0.6f);
  const float x0 = ImGui::GetCursorPosX();

  // The title row: the picker, read as a title (no frame until it is
  // hovered), and the gear at the face's right edge.
  if (controls) {
    // Read as a title: no frame or arrow box until hovered, its text on the
    // face's left edge with the lines under it.
    const auto titleStyle = [&](bool on) {
      if (on) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::SetCursorPosX(x0 - style.FramePadding.x);
      } else {
        ImGui::PopStyleColor(2);
      }
    };
    titleStyle(true);
    DrawLegendFieldCombo(state, *study, title.c_str());
    titleStyle(false);
    if (more.Any()) {
      const float after = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x +
                          style.ItemSpacing.x;
      ImGui::SameLine(std::max(after, x0 + faceW - gearW));
      // A click on it while the popover is open is a click away: it folds
      // the popover, the legend under it not taking the press.
      const bool isOpen = ImGui::IsPopupOpen(kLegendMorePopup);
      ImGui::PushStyleColor(ImGuiCol_Button, isOpen
                                                 ? IM_COL32(255, 255, 255, 40)
                                                 : IM_COL32(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Text, isOpen ? IM_COL32_WHITE : dim);
      const bool pressed = ImGui::Button(ICON_FA_GEAR "###result_more");
      ImGui::PopStyleColor(2);
      if (pressed)
        ImGui::OpenPopup(kLegendMorePopup);
      if (ImGui::IsItemHovered() && !isOpen)
        ImGui::SetTooltip("Display settings");
      DrawLegendMorePopover(state, *study, more, shown);
    }
    if (picks) {
      titleStyle(true);
      DrawLegendPick(pick);
      titleStyle(false);
    }
    // What the controls changed is drawn next frame: read it again.
    shown = sim.shown.get();
    bar = bar && shown != nullptr &&
          (shown->mesh ? sim.meshView != SimulationState::MeshView::Off
                       : sim.field != SimulationState::Field::None);
  } else {
    ImGui::TextUnformatted(title.c_str());
  }
  if (!studyLine.empty())
    ImGui::TextColored(dimText, "%s", studyLine.c_str());

  if (bar) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float topCap = shown->over ? capH : 0.0f;
    const float bottomCap = shown->under ? capH : 0.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(barW + tickGap + tickW,
                        topCap + barH + bottomCap + lineH * 0.5f + 4.0f));
    const float x = at.x;
    const float y0 = at.y + lineH * 0.5f + topCap;
    const float y1 = y0 + barH;
    // A vertical Turbo colorbar, the high value at the top; a factor's runs
    // the other way, its low end the hot one.
    const auto colour = [&](float t) {
      return Turbo(shown->reversed ? 1.0f - t : t);
    };
    const int segs = 48;
    for (int i = 0; i < segs; ++i) {
      const float t0 = i / (float)segs, t1 = (i + 1) / (float)segs;
      dl->AddRectFilledMultiColor(ImVec2(x, y1 - t1 * barH),
                                  ImVec2(x + barW, y1 - t0 * barH), colour(t1),
                                  colour(t1), colour(t0), colour(t0));
    }
    const ImU32 frame = IM_COL32(255, 255, 255, 140);
    dl->AddRect(ImVec2(x, y0), ImVec2(x + barW, y1), frame);
    // Past the ends: what lies over the range and under it, in the colours
    // the picture draws them in (FieldStyle::kOver / kUnder).
    const auto cap = [&](float top, const glm::vec3& c) {
      dl->AddRectFilled(
          ImVec2(x, top), ImVec2(x + barW, top + capH),
          IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), 255));
      dl->AddRect(ImVec2(x, top), ImVec2(x + barW, top + capH), frame);
    };
    // Pink past the hot end and grey past the other; a factor's hot end is
    // its low one.
    if (shown->over)
      cap(y0 - capH, shown->reversed ? FieldStyle::kUnder : FieldStyle::kOver);
    if (shown->under)
      cap(y1, shown->reversed ? FieldStyle::kOver : FieldStyle::kUnder);
    for (int k = 0; k <= 4; ++k) {
      const float yk = y1 - barH * (k / 4.0f);
      if (k > 0 && k < 4)
        dl->AddLine(ImVec2(x + barW, yk), ImVec2(x + barW + 4, yk), frame);
      dl->AddText(ImVec2(x + barW + tickGap, yk - lineH * 0.5f),
                  k == 0 || k == 4 ? IM_COL32_WHITE : dim, ticks[k].c_str());
    }
    for (const std::string& c : captions)
      ImGui::TextColored(ImVec4(1, 1, 1, 0.75f), "%s", c.c_str());
  }
  // What is set in the folded controls, so it is never invisible.
  if (!summary.empty())
    ImGui::TextColored(dimText, "%s", summary.c_str());
  const ImVec2 boxLo = ImGui::GetWindowPos();
  const ImVec2 boxHi(boxLo.x + ImGui::GetWindowSize().x,
                     boxLo.y + ImGui::GetWindowSize().y);
  ImGui::End();
  if (!extremes || !sim.showExtremes || state.camera == nullptr)
    return;
  ImDrawList* dl = ViewportDrawList();

  // The values at the crosses, so a marker reads without the legend: the
  // hot end's tag above its cross and the other's below, pushed clear of it
  // where the two ends are close on screen.
  const glm::mat4 viewProj =
      state.camera->GetProjectionMatrix() * state.camera->GetViewMatrix();
  const std::string unit = shown->unit.empty() ? "" : " " + shown->unit;
  struct Tag {
    ImVec2 lo, hi;
    std::string text;
    float px = 0.0f; // the cross it labels, on screen
  };
  const auto place = [&](const glm::dvec3& at, const glm::dvec3& disp,
                         const char* what, double value,
                         bool above) -> std::optional<Tag> {
    const glm::vec3 w = glm::vec3(at) + sim.deformScale * glm::vec3(disp);
    if ((viewProj * glm::vec4(w, 1.0f)).w <= 1e-6f)
      return std::nullopt;
    const glm::vec2 p = WorldToScreen(state, w);
    if (p.x < state.viewportPos.x || p.y < state.viewportPos.y ||
        p.x > state.viewportPos.x + state.viewportSize.x ||
        p.y > state.viewportPos.y + state.viewportSize.y)
      return std::nullopt;
    Tag t;
    std::snprintf(buf, sizeof(buf), "%s %.3g%s", what, value, unit.c_str());
    t.text = buf;
    const ImVec2 size = ImGui::CalcTextSize(buf);
    const float top = above ? p.y - size.y - 14.0f : p.y + 8.0f;
    t.px = p.x;
    t.lo = ImVec2(p.x + 10.0f, top);
    t.hi = ImVec2(p.x + 20.0f + size.x, top + size.y + 6.0f);
    return t;
  };
  const auto overlaps = [](ImVec2 alo, ImVec2 ahi, ImVec2 blo, ImVec2 bhi) {
    return alo.x < bhi.x && blo.x < ahi.x && alo.y < bhi.y && blo.y < ahi.y;
  };
  const auto shift = [](Tag& t, float dx, float dy) {
    t.lo = ImVec2(t.lo.x + dx, t.lo.y + dy);
    t.hi = ImVec2(t.hi.x + dx, t.hi.y + dy);
  };
  std::vector<Tag> tags;
  const auto add = [&](std::optional<Tag> t) {
    if (!t)
      return;
    // Inside what is seen: a tag that would run under the tool window or
    // off the viewport's right edge goes to the left of its cross.
    const glm::vec4& tw = state.toolWindowRect;
    const bool windowOpen =
        state.toolWindowLastFrame >= ImGui::GetFrameCount() - 1 &&
        tw.z > tw.x && tw.w > tw.y;
    if ((windowOpen && overlaps(t->lo, t->hi, ImVec2(tw.x - 4.0f, tw.y),
                                ImVec2(tw.z, tw.w))) ||
        t->hi.x > state.viewportPos.x + state.viewportSize.x)
      shift(*t, t->px - 10.0f - (t->hi.x - t->lo.x) - t->lo.x, 0.0f);
    // Clear of the legend: a tag over it is moved out past its right edge.
    if (overlaps(t->lo, t->hi, boxLo, boxHi))
      shift(*t, boxHi.x + 6.0f - t->lo.x, 0.0f);
    // And clear of the tag before it, below it.
    if (!tags.empty() &&
        overlaps(t->lo, ImVec2(t->hi.x, t->hi.y + 4.0f), tags[0].lo,
                 ImVec2(tags[0].hi.x, tags[0].hi.y + 4.0f)))
      shift(*t, 0.0f, tags[0].hi.y + 4.0f - t->lo.y);
    tags.push_back(*t);
  };
  if (shown->reversed) {
    add(place(shown->loAt, shown->loDisp, "min", shown->lo, true));
  } else {
    add(place(shown->hiAt, shown->hiDisp, "max", shown->hi, true));
    if (shown->hi > shown->lo)
      add(place(shown->loAt, shown->loDisp, "min", shown->lo, false));
  }
  for (const Tag& t : tags) {
    dl->AddRectFilled(t.lo, t.hi, IM_COL32(18, 20, 26, 210), 4.0f);
    dl->AddText(ImVec2(t.lo.x + 5.0f, t.lo.y + 3.0f),
                IM_COL32(255, 255, 255, 235), t.text.c_str());
  }
}

namespace {
// A world-space arrow from a to b with a two-line head.
void ArrowGlyph(const glm::vec3& a, const glm::vec3& b, const glm::vec4& color,
                std::vector<PrimVertex>& out) {
  ui::AppendPrimLine(a, b, 2.2f, color, out);
  const glm::vec3 dir = glm::normalize(b - a);
  glm::vec3 side =
      glm::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  side = glm::normalize(glm::cross(dir, side));
  const float headLen = glm::length(b - a) * 0.25f;
  ui::AppendPrimLine(b, b - dir * headLen + side * headLen * 0.5f, 2.2f, color,
                     out);
  ui::AppendPrimLine(b, b - dir * headLen - side * headLen * 0.5f, 2.2f, color,
                     out);
}
} // namespace

// Dragging a force arrow changes its MAGNITUDE. The mapping is relative, not
// absolute: pulling one arrow-length doubles the force, pushing back toward
// the face shrinks it to zero. An absolute mapping would need the arrow's
// length to mean newtons, and it cannot — the glyph is view-scaled so it
// stays visible on a 90 mm part and a 3 m dock alike.
//
// Snapshot at grab, mutate live, one SimUndoGuard step on release: the
// gizmo's contract, so a drag is one undo step and the solved results go
// stale exactly once.
bool HandleLoadDragInput(AppState& state, const Ray& ray, bool clicked,
                         bool down, bool released) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host)
    return false;
  // Mutable for the drag's live preview; the release records the edit.
  fem::Study* study = host.FindForRun(sim.activeStudyId);
  if (study == nullptr)
    return false;

  const auto findLoad = [&](uint64_t id) -> fem::Load* {
    for (fem::Load& l : study->loads)
      if (l.id == id)
        return &l;
    return nullptr;
  };

  if (sim.loadDrag.active) {
    fem::Load* load = findLoad(sim.loadDrag.loadId);
    if (released || !down || load == nullptr) {
      sim.loadDrag.active = false;
      if (load != nullptr && !sim.loadDrag.beforeJson.empty()) {
        // One undo entry for the whole drag: replay the before/after pair
        // the command stack expects -- or, on an assembly, the snapshot
        // taken at the grab plus one version bump now.
        const nlohmann::json after = DocumentIO::StudyToJson(*study);
        const nlohmann::json before =
            nlohmann::json::parse(sim.loadDrag.beforeJson, nullptr, false);
        // A snapshot step (SimUndoGuard): the study is put back as it was
        // at the grab, and the dragged one replaces it under the guard.
        if (host && !before.is_discarded() && before != after) {
          fem::Study dragged = *study;
          *study = DocumentIO::StudyFromJson(before); // in place, unseen
          SimUndoGuard guard(state);
          host.sim->ReplaceStudy(std::move(dragged));
        }
      }
      return true;
    }
    const glm::vec3 dir = glm::normalize(
        glm::vec3(load->direction.x, load->direction.y, load->direction.z));
    const float len = state.camera
                          ? std::max(0.055f * state.camera->GetDistance(), 1.2f)
                          : 1.2f;
    const float t =
        RayAxisParam(ray.origin, ray.direction, glm::vec3(0.0f), dir);
    const double scale =
        1.0 + static_cast<double>(t - sim.loadDrag.startParam) / len;
    load->magnitude = std::max(0.0, sim.loadDrag.startMagnitude * scale);
    load->magnitude.expr.clear(); // a dragged value is a number, not a formula
    // No staleness bump mid-drag: the replace on release is what moves the
    // study's revision, so results go stale exactly once — the
    // gizmo's solve-on-release contract.
    return true;
  }

  if (!clicked || OwnerOf(state.hoveredHandle) != HandleOwner::Load)
    return false;
  fem::Load* load = findLoad(SubOf(state.hoveredHandle));
  if (load == nullptr)
    return false;
  const glm::vec3 dir = glm::normalize(
      glm::vec3(load->direction.x, load->direction.y, load->direction.z));
  sim.loadDrag.active = true;
  sim.loadDrag.loadId = load->id;
  // From the value the arrow shows, which a variable-driven load's stored
  // magnitude need not be until the study runs.
  sim.loadDrag.startMagnitude = host.Shown(*load).magnitude;
  sim.loadDrag.startParam =
      RayAxisParam(ray.origin, ray.direction, glm::vec3(0.0f), dir);
  sim.loadDrag.beforeJson = DocumentIO::StudyToJson(*study).dump();
  return true; // consumed: do not also re-pick the face behind the arrow
}

namespace {
// The view-scaled glyph length every simulation glyph is built from (the
// peak marker uses the same rule): a fixed 1.2 model-mm arrow vanished
// entirely on a 90 mm part.
float GlyphLength(const AppState& state) {
  return state.camera ? std::max(0.055f * state.camera->GetDistance(), 1.2f)
                      : 1.2f;
}

// Radical-inverse (van der Corput) sequence in base b: the low-discrepancy
// numbers behind SampleFace, so the arrows over a face are evenly spread
// and identical from one frame to the next.
float Halton(int i, int base) {
  float f = 1.0f, r = 0.0f;
  while (i > 0) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(i % base);
    i /= base;
  }
  return r;
}

const glm::vec4 kFixed{0.20f, 0.80f, 0.35f, 1.0f};    // green, like its tint
const glm::vec4 kForce{1.00f, 0.65f, 0.20f, 1.0f};    // the handle's orange
const glm::vec4 kPressure{0.85f, 0.30f, 0.75f, 1.0f}; // magenta, like its tint
const glm::vec4 kKeep{0.35f, 0.85f, 0.95f, 1.0f};
const glm::vec4 kGravity{0.55f, 0.55f, 1.00f, 1.0f};
const glm::vec4 kTorque{1.00f, 0.42f, 0.30f,
                        1.0f}; // red-orange, a turning load
const glm::vec4 kRemote{0.95f, 0.85f, 0.25f, 1.0f}; // yellow, a load at a point
const glm::vec4 kProbe{0.95f, 0.95f, 0.95f, 1.0f};  // white: a reading, no load

// A torque's axis in the frame the viewport draws in: the load's own, or its
// cylindrical face's, placed by the occurrence on an assembly.
std::optional<fem::LoadAxis> TorqueAxisInView(AppState& state,
                                              const fem::Load& load) {
  if (load.HasAxis())
    return fem::LoadAxis{load.axisOrigin, glm::normalize(load.axisDirection)};
  const StudyHost host = StudyHostOf(state);
  if (const Document* part = host.GeometryPart())
    return fem::CylindricalFaceAxis(*part, load.face);
  const Occurrence* occ =
      host.Assembly() ? host.Assembly()->FindById(load.occurrenceId) : nullptr;
  const Document* part = occ ? OccurrencePartDoc(state, *occ) : nullptr;
  if (!part)
    return std::nullopt;
  auto axis = fem::CylindricalFaceAxis(*part, load.face);
  if (axis) {
    const glm::dmat3 r = glm::mat3_cast(occ->orientation);
    axis->origin = r * axis->origin + occ->position;
    axis->direction = r * axis->direction;
  }
  return axis;
}

// A circular arrow about `axis`, centred where the axis passes the face
// (nearest `c`): three quarters of a turn in the sense a positive torque
// turns, right-handed about the direction, with a head at its end.
void TorqueGlyph(const glm::vec3& centre, const glm::vec3& axis, float radius,
                 float sign, std::vector<PrimVertex>& out) {
  glm::vec3 u =
      glm::abs(axis.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
  u = glm::normalize(glm::cross(axis, u));
  const glm::vec3 v = glm::cross(axis, u) * sign;
  constexpr int kSegments = 36;
  constexpr float kSweep = 1.5f * 3.14159265f;
  glm::vec3 prev = centre + u * radius;
  for (int i = 1; i <= kSegments; ++i) {
    const float t = kSweep * static_cast<float>(i) / kSegments;
    const glm::vec3 p = centre + (u * std::cos(t) + v * std::sin(t)) * radius;
    ui::AppendPrimLine(prev, p, 2.4f, kTorque, out);
    prev = p;
  }
  // The head: along the tangent at the end, two strokes back from it.
  const glm::vec3 tangent =
      glm::normalize(-u * std::sin(kSweep) + v * std::cos(kSweep));
  const glm::vec3 inward = glm::normalize(centre - prev);
  const float head = radius * 0.35f;
  ui::AppendPrimLine(prev, prev - tangent * head + inward * head * 0.5f, 2.4f,
                     kTorque, out);
  ui::AppendPrimLine(prev, prev - tangent * head - inward * head * 0.5f, 2.4f,
                     kTorque, out);
  // The axis itself, short, through the centre.
  ui::AppendPrimLine(centre - axis * radius * 0.6f,
                     centre + axis * radius * 1.2f, 1.6f, kTorque, out);
}

// A roller: a ground line off the face along `n`, on two wheels that touch
// the face, in both in-plane axes as the anchor's hatch is.
void RollerGlyph(const glm::vec3& c, const glm::vec3& n, float len,
                 std::vector<PrimVertex>& out) {
  glm::vec3 side =
      glm::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  side = glm::normalize(glm::cross(n, side));
  const glm::vec3 other = glm::normalize(glm::cross(n, side));
  const float r = len * 0.1f;
  for (const glm::vec3& axis : {side, other}) {
    const glm::vec3 ground = c + n * (2.0f * r);
    ui::AppendPrimLine(ground - axis * len * 0.4f, ground + axis * len * 0.4f,
                       2.4f, kFixed, out);
    for (float at : {-0.22f, 0.22f}) {
      const glm::vec3 wc = c + axis * (len * at) + n * r;
      constexpr int kSegments = 12;
      for (int i = 0; i < kSegments; ++i) {
        const float a0 = 6.2831853f * i / kSegments;
        const float a1 = 6.2831853f * (i + 1) / kSegments;
        ui::AppendPrimLine(wc + (axis * std::cos(a0) + n * std::sin(a0)) * r,
                           wc + (axis * std::cos(a1) + n * std::sin(a1)) * r,
                           1.6f, kFixed, out);
      }
    }
  }
}

// A held direction `d`: an arrow along it from the face's centre with a
// bar across its foot; `moved` (a prescribed displacement) gives the arrow a
// second head, the way it is pushed.
void HeldGlyph(const glm::vec3& c, const glm::vec3& d, float len, bool moved,
               std::vector<PrimVertex>& out) {
  const glm::vec3 dir = glm::normalize(d);
  ArrowGlyph(c, c + dir * len, kFixed, out);
  if (moved)
    ArrowGlyph(c, c + dir * len * 0.7f, kFixed, out);
  glm::vec3 side =
      glm::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  side = glm::normalize(glm::cross(dir, side));
  ui::AppendPrimLine(c - side * len * 0.2f, c + side * len * 0.2f, 2.4f, kFixed,
                     out);
}

// An anchor hatch: a ground line with strokes fanning off it, in BOTH
// in-plane axes. It used to draw along one axis only, so from most angles
// a fixed support was a single stick lying on the face. The strokes lean
// OUT of the face (+n): leaning in, they were inside the solid and the
// depth test left only the ground lines, a cross that read as a marker.
void AnchorGlyph(const glm::vec3& c, const glm::vec3& n, float len,
                 std::vector<PrimVertex>& out) {
  glm::vec3 side =
      glm::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  side = glm::normalize(glm::cross(n, side));
  const glm::vec3 other = glm::normalize(glm::cross(n, side));
  for (const glm::vec3& axis : {side, other}) {
    ui::AppendPrimLine(c - axis * len * 0.4f, c + axis * len * 0.4f, 2.4f,
                       kFixed, out);
    for (int i = -1; i <= 1; i++) {
      const glm::vec3 base = c + axis * (len * 0.3f * i);
      ui::AppendPrimLine(base, base + n * len * 0.3f - axis * len * 0.15f, 1.8f,
                         kFixed, out);
    }
  }
}
} // namespace

std::vector<FaceSample> SampleFace(AppState& state, uint64_t occurrenceId,
                                   const ElementRef& face, float spacing,
                                   int maxCount) {
  std::vector<FaceSample> out;
  uint32_t mesh = 0, index = 0;
  if (!state.faceSelector ||
      !ResolveFaceItem(state, {SelectionItem::Kind::Face, occurrenceId, face},
                       mesh, index))
    return out;
  struct Tri {
    glm::vec3 a, b, c, n;
    float cumArea;
  };
  std::vector<Tri> tris;
  float area = 0.0f;
  state.faceSelector->ForEachFaceTriangle(
      mesh, index,
      [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
        const glm::vec3 cr = glm::cross(b - a, c - a);
        const float l = glm::length(cr);
        if (l < 1e-12f)
          return;
        area += 0.5f * l;
        tris.push_back({a, b, c, cr / l, area});
      });
  // Always the centre with the face's average normal: the one glyph a face
  // had before, and the one a face too small for the grid still gets.
  out.push_back({state.faceSelector->GetFaceCenter(mesh, index),
                 state.faceSelector->GetFaceNormal(mesh, index)});
  if (tris.empty() || spacing <= 0.0f)
    return out;
  // One sample per spacing^2 of area, capped; below two the centre alone
  // is the honest picture.
  const int count = std::min(
      maxCount, static_cast<int>(std::floor(area / (spacing * spacing))));
  if (count < 2)
    return out;
  out.clear();
  for (int i = 0; i < count; ++i) {
    // Pick the triangle by area (Halton base 2 against the cumulative
    // areas), then a point in it (bases 3 and 5, folded into the triangle).
    const float pick = Halton(i + 1, 2) * area;
    const auto it =
        std::lower_bound(tris.begin(), tris.end(), pick,
                         [](const Tri& t, float v) { return t.cumArea < v; });
    const Tri& t = it == tris.end() ? tris.back() : *it;
    float u = Halton(i + 1, 3), v = Halton(i + 1, 5);
    if (u + v > 1.0f) {
      u = 1.0f - u;
      v = 1.0f - v;
    }
    out.push_back({t.a + (t.b - t.a) * u + (t.c - t.a) * v, t.n});
  }
  return out;
}

void AppendSimulationBCGlyphs(AppState& state, std::vector<PrimVertex>& out) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host || !state.faceSelector)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (s == nullptr)
    return;

  const auto faceFrame = [&](uint64_t occurrenceId, const ElementRef& ref,
                             glm::vec3& center, glm::vec3& normal) {
    uint32_t mesh = 0, index = 0;
    if (!ResolveFaceItem(state, {SelectionItem::Kind::Face, occurrenceId, ref},
                         mesh, index))
      return false;
    center = state.faceSelector->GetFaceCenter(mesh, index);
    normal = state.faceSelector->GetFaceNormal(mesh, index);
    return true;
  };
  const float len = GlyphLength(state);
  // A distributed condition covers its face at one glyph per cell of this
  // spacing -- derived from the glyph length, so zooming in fills the face
  // in and zooming out thins it, and never fewer than the centre. The cap
  // keeps a sheet from costing thousands of lines a frame.
  const float spacing = len * 1.6f;
  constexpr int kMaxGlyphsPerFace = 160;

  const fem::Study shown = host.Shown(*s); // a torque's sign, evaluated
  for (const fem::Load& load : shown.loads) {
    if (load.contactWith != 0)
      continue; // a contact anchor is drawn as its tinted patch, below
    glm::vec3 c(0.0f), n(0.0f);
    if (!faceFrame(load.occurrenceId, load.face, c, n))
      continue;
    switch (load.type) {
    case fem::LoadType::Roller: {
      // A roller: the hatch on wheels, standing on each sample's normal --
      // free across the face, held along it.
      const auto samples = SampleFace(state, load.occurrenceId, load.face,
                                      spacing * 1.5f, kMaxGlyphsPerFace / 2);
      const float size = samples.size() > 1 ? len * 0.55f : len;
      for (const FaceSample& fs : samples)
        RollerGlyph(fs.point, fs.normal, size, out);
      break;
    }
    case fem::LoadType::Held: {
      // An arrow along each held direction of its frame, with a bar across
      // it: held there, free in the others.
      std::array<glm::dvec3, 3> axes{glm::dvec3(1, 0, 0), glm::dvec3(0, 1, 0),
                                     glm::dvec3(0, 0, 1)};
      if (load.frame != 0 && host.GeometryPart())
        if (const Feature* f = host.GeometryPart()->FindFeatureById(load.frame);
            f && datumframe::IsAnchor(*f)) {
          const glm::dmat4 m = datumframe::Matrix(*f);
          for (int k = 0; k < 3; ++k)
            axes[k] = glm::normalize(glm::dvec3(m[k]));
        }
      for (int k = 0; k < 3; ++k)
        if (load.hold[k])
          HeldGlyph(c, glm::vec3(axes[k]), len, load.prescribedMm[k] != 0.0,
                    out);
      break;
    }
    case fem::LoadType::Fixed:
    case fem::LoadType::Pin:
    case fem::LoadType::Cylindrical:
    case fem::LoadType::Contact: {
      // The hatch repeated over the face, so "what is fixed" reads as an
      // AREA -- and each hatch stands on its own triangle's normal, which on
      // a curved face is not the average one.
      const auto samples = SampleFace(state, load.occurrenceId, load.face,
                                      spacing * 1.5f, kMaxGlyphsPerFace / 2);
      const float hatch = samples.size() > 1 ? len * 0.55f : len;
      for (const FaceSample& fs : samples)
        AnchorGlyph(fs.point, fs.normal, hatch, out);
      break;
    }
    case fem::LoadType::Force: {
      const glm::vec3 dir = glm::normalize(
          glm::vec3(load.direction.x, load.direction.y, load.direction.z));
      // One resultant arrow: a force is "this much in total", and the
      // tinted face says where it is spread. The arrow flies ALONG the
      // force; which end sits on the face depends on where the shaft is
      // visible. A push approaches from outside and lands head-on the face.
      // A pull drawn the same way would put the whole shaft INSIDE the
      // solid -- depth-tested away, an invisible load -- so a pull takes
      // off from the face instead: tail on the face, head out.
      const glm::vec3 tail = glm::dot(dir, n) > 0.0f ? c : c - dir * len;
      // Draggable (docs/ui/viewport-handles.md): the shaft is the handle,
      // so what highlights is exactly what a click grabs. Published, not
      // drawn: DrawViewportHandles draws it with every other handle, on
      // top -- so the pull case above is belt and braces.
      ViewportHandle vh;
      vh.id = HandleId(HandleOwner::Load, load.id);
      vh.kind = HandleKind::Linear;
      vh.origin = tail;
      vh.dir = dir;
      vh.length = len;
      vh.color = glm::vec3(kForce);
      state.viewportHandles.push_back(vh);
      break;
    }
    case fem::LoadType::Pressure: {
      // A pressure is a FIELD: one arrow per cell, each pressing along its
      // own triangle's inward normal. One arrow at the centre read as a
      // point load on a face that carries none.
      const auto samples = SampleFace(state, load.occurrenceId, load.face,
                                      spacing, kMaxGlyphsPerFace);
      const float arrow = samples.size() > 1 ? len * 0.6f : len * 0.8f;
      for (const FaceSample& fs : samples)
        ArrowGlyph(fs.point + fs.normal * arrow, fs.point, kPressure, out);
      break;
    }
    case fem::LoadType::Torque: {
      // A circular arrow about the axis, where the axis passes the face,
      // just outside it along the axis so the solid does not hide it.
      const auto axis = TorqueAxisInView(state, load);
      if (!axis)
        break;
      const glm::vec3 a = glm::normalize(glm::vec3(axis->direction));
      const glm::vec3 o(axis->origin);
      glm::vec3 centre = o + a * glm::dot(c - o, a);
      const float out_ = glm::dot(n, a) >= 0.0f ? 1.0f : -1.0f;
      centre += a * out_ * len * 0.25f;
      const float r = std::max(glm::length(c - centre), len * 0.6f);
      TorqueGlyph(centre, a, std::min(r, len * 1.4f),
                  load.magnitude >= 0.0 ? 1.0f : -1.0f, out);
      break;
    }
    case fem::LoadType::Bearing: {
      // The pressure over the half it pushes on: an arrow at each sample
      // there, into the material, as long as the cosine it carries.
      const glm::dvec3 dd = load.direction;
      if (glm::length(dd) < 1e-12)
        break;
      const glm::vec3 d = glm::normalize(glm::vec3(dd));
      const auto samples = SampleFace(state, load.occurrenceId, load.face,
                                      spacing * 0.8f, kMaxGlyphsPerFace);
      for (const FaceSample& fs : samples) {
        const float w = glm::dot(-fs.normal, d);
        if (w < 0.15f)
          continue;
        ArrowGlyph(fs.point + fs.normal * (len * 0.7f * w), fs.point, kForce,
                   out);
      }
      break;
    }
    case fem::LoadType::Remote: {
      // The force where it acts, and thin lines tying that point to the
      // face that carries it.
      const glm::vec3 p(load.point);
      glm::vec3 dir(load.direction);
      dir =
          glm::length(dir) > 1e-9f ? glm::normalize(dir) : glm::vec3(0, 0, -1);
      ArrowGlyph(p - dir * len, p, kRemote, out);
      const auto samples =
          SampleFace(state, load.occurrenceId, load.face, spacing * 2.0f, 6);
      ui::AppendPrimLine(p, c, 1.2f, kRemote, out);
      for (const FaceSample& fs : samples)
        ui::AppendPrimLine(p, fs.point, 1.0f, kRemote, out);
      break;
    }
    default: // a thermal load: its face is tinted and labelled
      break;
    }
  }
  for (const ElementRef& keep : s->keepFaces) {
    glm::vec3 c(0.0f), n(0.0f);
    if (!faceFrame(0, keep, c, n))
      continue;
    glm::vec3 side =
        glm::abs(n.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    side = glm::normalize(glm::cross(n, side));
    const glm::vec3 other = glm::normalize(glm::cross(n, side));
    const float r = len * 0.35f;
    const glm::vec3 pts[4] = {c + side * r, c + other * r, c - side * r,
                              c - other * r};
    for (int i = 0; i < 4; i++)
      ui::AppendPrimLine(pts[i], pts[(i + 1) % 4], 2.0f, kKeep, out);
  }
}

void AppendSimulationOverlayGlyphs(AppState& state,
                                   std::vector<PrimVertex>& out) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (s == nullptr)
    return;
  // The pinned probes' peaks: a small cross where each face's stress is
  // largest. See-through, because a peak on a hidden face still counts.
  {
    const float m = GlyphLength(state) * 0.15f;
    const glm::vec3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const SimulationState::ProbePin& pin : sim.probes)
      if (pin.studyId == s->id)
        if (const auto probed = ProbeFace(state, *s, pin.face)) {
          const glm::vec3 at(probed->stress.maxAt);
          for (const glm::vec3& a : ax)
            ui::AppendPrimLine(at - a * m, at + a * m, 2.0f, kProbe, out);
        }
  }
  // Gravity: one arrow leaving the centre of mass, the way the weight acts.
  // A self-weight study whose picture showed no weight was a setup nobody
  // could check against the panel. The centre of mass is inside the solid,
  // so this goes in the see-through channel.
  if (s->type != fem::AnalysisType::Static || glm::length(s->gravity) < 1e-9)
    return;
  if (sim.comVersion != host.Version() || !sim.comKnown) {
    sim.comVersion = host.Version();
    sim.comKnown = false;
    // The weight of what is SOLVED: the part, or the meshed occurrence.
    std::vector<Body> placed;
    const std::vector<Body>* bodies =
        host.GeometryPart() ? &host.GeometryPart()->GetBodies() : &placed;
    if (host.OnAssembly())
      for (const fem::Participant& pt : host.Roles(*s))
        if (pt.role == fem::ParticipantRole::Meshed)
          placed = SubjectOccurrenceBodies(state, pt.occurrenceId);
    if (const auto mass = Measure::MassOfBodies(*bodies)) {
      sim.com = glm::vec3(mass->centerOfMass);
      sim.comKnown = mass->volume > 0.0;
    }
  }
  if (!sim.comKnown)
    return;
  const float len = GlyphLength(state);
  const glm::vec3 g = glm::normalize(glm::vec3(s->gravity));
  // A small cross marks the point itself; the arrow leaves it.
  const glm::vec3 ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const glm::vec3& a : ax)
    ui::AppendPrimLine(sim.com - a * len * 0.12f, sim.com + a * len * 0.12f,
                       2.0f, kGravity, out);
  ArrowGlyph(sim.com, sim.com + g * len * 1.3f, kGravity, out);
}

void AppendSimulationFaceTints(AppState& state) {
  const SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!sim.IsActive() || !host || !state.faceSelector || !state.meshRenderer)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (s == nullptr)
    return;
  const auto tintFace = [&](uint64_t occurrenceId, const ElementRef& ref,
                            uint32_t tint) {
    uint32_t mesh = 0, index = 0, first = 0, count = 0;
    if (ResolveFaceItem(state, {SelectionItem::Kind::Face, occurrenceId, ref},
                        mesh, index) &&
        state.faceSelector->GetFaceVertexRange(mesh, index, first, count))
      state.meshRenderer->SetFaceHighlight(mesh, first, count, tint);
  };
  // A load picked in the tree is lit as a selection is, so the row and
  // the face it stands on read as one thing.
  for (const fem::Load& load : s->loads)
    if (load.contactWith == 0)
      tintFace(load.occurrenceId, load.face,
               sim.selectedLoadId == load.id ||
                       IsSelected(state, SelectionItem::Kind::SimLoad, load.id)
                   ? MeshRenderer::kTintSelection
               : fem::IsSupport(load.type) ? MeshRenderer::kTintFixture
                                           : MeshRenderer::kTintLoad);
  for (const ElementRef& keep : s->keepFaces)
    tintFace(0, keep, MeshRenderer::kTintKeep);

  if (!host.OnAssembly())
    return;
  // Roles, on the picture (S1): a rigid support is a GHOST with its contact
  // faces tinted as the fixture they are -- the faces the imprint found,
  // not the whole part -- and an ignored occurrence stays as it is. The
  // renderer sync rebuilds every instance on the next version change or on
  // ExitSimulation's forced resync, so the ghosting needs no undo.
  for (const fem::Participant& pt : host.Roles(*s))
    if (pt.role == fem::ParticipantRole::Rigid)
      for (const DocumentRenderMesh& m : state.gpu.documentMeshes)
        if (MeshOccurrence(state, m) == pt.occurrenceId &&
            !state.meshRenderer->IsInstanceGhost(m.meshId))
          state.meshRenderer->SetInstanceGhost(m.meshId, 0.35f);
  // The glued seams (S2): both sides' faces in the bond tint.
  for (const fem::BondPatch& bond : SimulationBonds(state)) {
    const auto tintSide = [&](uint64_t occ, int body,
                              const std::vector<int>& faces) {
      for (const DocumentRenderMesh& m : state.gpu.documentMeshes) {
        if (MeshOccurrence(state, m) != occ || MeshLocalBody(state, m) != body)
          continue;
        for (int face : faces) {
          uint32_t first = 0, count = 0;
          if (state.faceSelector->GetFaceVertexRange(
                  m.meshId, static_cast<uint32_t>(face), first, count))
            state.meshRenderer->SetFaceHighlight(m.meshId, first, count,
                                                 MeshRenderer::kTintBond);
        }
      }
    };
    tintSide(bond.occurrenceA, bond.bodyA, bond.facesA);
    tintSide(bond.occurrenceB, bond.bodyB, bond.facesB);
  }
  bool clamped = false;
  for (const fem::Load& load : s->loads)
    clamped = clamped || load.contactWith != 0;
  if (!clamped)
    return;
  for (const fem::ContactPatch& patch : SimulationContacts(state)) {
    bool wanted = false;
    for (const fem::Load& load : s->loads)
      wanted = wanted || load.contactWith == patch.rigidOccurrenceId;
    if (!wanted)
      continue;
    for (const DocumentRenderMesh& m : state.gpu.documentMeshes) {
      if (MeshOccurrence(state, m) != patch.rigidOccurrenceId ||
          MeshLocalBody(state, m) != patch.rigidBody)
        continue;
      for (int face : patch.rigidFaces) {
        uint32_t first = 0, count = 0;
        if (state.faceSelector->GetFaceVertexRange(
                m.meshId, static_cast<uint32_t>(face), first, count))
          state.meshRenderer->SetFaceHighlight(m.meshId, first, count,
                                               MeshRenderer::kTintFixture);
      }
    }
  }
}

std::string SimulationLoadLabel(const fem::Load& load) {
  char buf[64];
  switch (load.type) {
  case fem::LoadType::Fixed:
    return "fixed";
  case fem::LoadType::Pin:
    return load.fixRotation ? "pin, held" : "pin";
  case fem::LoadType::Contact:
    return "contact";
  case fem::LoadType::Roller:
    return "roller";
  case fem::LoadType::Held:
  case fem::LoadType::Cylindrical: {
    const bool held = load.type == fem::LoadType::Held;
    std::string text = held ? "held" : "cylindrical";
    const char* sep = " ";
    for (int k = 0; k < 3; ++k)
      if (load.hold[k]) {
        text += sep;
        text += held ? fem::kHeldAxisNames[k] : fem::kCylindricalAxisNames[k];
        if (held && load.prescribedMm[k] != 0.0) {
          std::snprintf(buf, sizeof(buf), " %+.3g mm",
                        load.prescribedMm[k].value);
          text += buf;
        }
        sep = ", ";
      }
    return text;
  }
  case fem::LoadType::Force:
    if (std::abs(load.magnitude) >= 1000.0)
      std::snprintf(buf, sizeof(buf), "%.3g kN", load.magnitude.value / 1000.0);
    else
      std::snprintf(buf, sizeof(buf), "%.3g N", load.magnitude.value);
    return buf;
  case fem::LoadType::Pressure:
    if (std::abs(load.magnitude) < 0.1 && load.magnitude != 0.0)
      std::snprintf(buf, sizeof(buf), "%.3g kPa",
                    load.magnitude.value * 1000.0);
    else
      std::snprintf(buf, sizeof(buf), "%.3g MPa", load.magnitude.value);
    return buf;
  case fem::LoadType::Torque:
    return TorqueText(load.magnitude);
  case fem::LoadType::Bearing: // the row and the tooltip say it is one
    if (std::abs(load.magnitude) >= 1000.0)
      std::snprintf(buf, sizeof(buf), "%.3g kN", load.magnitude / 1000.0);
    else
      std::snprintf(buf, sizeof(buf), "%.3g N", load.magnitude.value);
    return buf;
  case fem::LoadType::Remote:
    if (std::abs(load.magnitude) >= 1000.0)
      std::snprintf(buf, sizeof(buf), "%.3g kN at a point",
                    load.magnitude / 1000.0);
    else
      std::snprintf(buf, sizeof(buf), "%.3g N at a point",
                    load.magnitude.value);
    return buf;
  case fem::LoadType::Temperature:
    std::snprintf(buf, sizeof(buf),
                  "%.4g \xC2\xB0"
                  "C",
                  load.temperatureC);
    return buf;
  case fem::LoadType::HeatFlux:
    if (std::isnan(load.powerW))
      std::snprintf(buf, sizeof(buf), "%.4g W/m\xC2\xB2", load.fluxWm2);
    else
      std::snprintf(buf, sizeof(buf), "%.4g W in", load.powerW);
    return buf;
  case fem::LoadType::Convection:
    std::snprintf(buf, sizeof(buf),
                  "%.4g W/m\xC2\xB2K to %.4g \xC2\xB0"
                  "C",
                  load.filmWm2K, load.ambientC);
    return buf;
  case fem::LoadType::HeatGeneration:
    std::snprintf(buf, sizeof(buf), "%.4g W", load.powerW);
    return buf;
  }
  return "";
}

std::string SimulationGravityLabel(const glm::dvec3& gravity) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "g %.3g m/s^2", glm::length(gravity));
  return buf;
}

void DrawSimulationLabels(AppState& state) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  // The Probe tool's reading of the face under the cursor, in the Measure
  // tool's cursor-label look: what a click would pin.
  if (sim.IsActive() && sim.tool == SimulationState::Tool::Probe && host &&
      state.pick.hoverFace.IsSet())
    if (const fem::Study* ps = host.Find(sim.activeStudyId))
      if (const auto probed = ProbeFace(state, *ps, state.pick.hoverFace)) {
        const std::string text = state.pick.hoverFace.name + "\nvon Mises " +
                                 ProbeText(probed->stress);
        const ImVec2 m = ImGui::GetIO().MousePos;
        ImDrawList* dl = ViewportDrawList();
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 pos(m.x + 18.0f, m.y + 16.0f);
        dl->AddRectFilled(ImVec2(pos.x - 6.0f, pos.y - 4.0f),
                          ImVec2(pos.x + size.x + 6.0f, pos.y + size.y + 4.0f),
                          IM_COL32(30, 24, 16, 205), 4.0f);
        dl->AddText(pos, IM_COL32(255, 210, 130, 235), text.c_str());
      }
  if (!sim.IsActive() || !sim.showLabels || !host || !state.faceSelector ||
      !state.camera)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (s == nullptr)
    return;
  ImDrawList* dl = ViewportDrawList();
  const float len = GlyphLength(state);
  const glm::mat4 viewProj =
      state.camera->GetProjectionMatrix() * state.camera->GetViewMatrix();
  // A pill behind the text, so a label stays legible over a lit face or a
  // colormap. Skipped when the anchor is behind the camera.
  const auto label = [&](const glm::vec3& at, const std::string& text,
                         ImU32 colour) {
    const glm::vec4 clip = viewProj * glm::vec4(at, 1.0f);
    if (clip.w <= 1e-6f)
      return;
    const glm::vec2 p = WorldToScreen(state, at);
    if (p.x < state.viewportPos.x ||
        p.x > state.viewportPos.x + state.viewportSize.x ||
        p.y < state.viewportPos.y ||
        p.y > state.viewportPos.y + state.viewportSize.y)
      return;
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 pos(p.x + 8.0f, p.y - size.y * 0.5f);
    dl->AddRectFilled(ImVec2(pos.x - 4.0f, pos.y - 2.0f),
                      ImVec2(pos.x + size.x + 4.0f, pos.y + size.y + 2.0f),
                      IM_COL32(20, 20, 24, 190), 4.0f);
    dl->AddText(pos, colour, text.c_str());
  };
  const auto col = [](const glm::vec4& c) {
    return IM_COL32(int(c.r * 255), int(c.g * 255), int(c.b * 255), 255);
  };
  // While a mesh or a result is on show only the selected load keeps its
  // label (docs/simulation/README.md#results-display): the picture is the
  // subject then, and the setup's labels sat over it.
  const bool pictured = sim.meshView != SimulationState::MeshView::Off ||
                        (sim.field != SimulationState::Field::None &&
                         sim.shown != nullptr && !sim.shown->mesh);
  const fem::Study shown = host.Shown(*s);
  for (const fem::Load& load : shown.loads) {
    if (pictured && load.id != sim.selectedLoadId)
      continue;
    if (load.contactWith != 0) {
      // At the first contact face the imprint found on the support.
      for (const fem::ContactPatch& patch : SimulationContacts(state)) {
        if (patch.rigidOccurrenceId != load.contactWith ||
            patch.rigidFaces.empty())
          continue;
        for (const DocumentRenderMesh& m : state.gpu.documentMeshes)
          if (m.occurrenceId == patch.rigidOccurrenceId &&
              static_cast<int>(m.bodyIndex) == patch.rigidBody) {
            label(state.faceSelector->GetFaceCenter(
                      m.meshId, static_cast<uint32_t>(patch.rigidFaces[0])),
                  "clamped at " + host.OccurrenceName(load.contactWith),
                  col(kFixed));
            break;
          }
        break;
      }
      continue;
    }
    uint32_t mesh = 0, index = 0;
    if (!ResolveFaceItem(
            state, {SelectionItem::Kind::Face, load.occurrenceId, load.face},
            mesh, index))
      continue;
    const glm::vec3 c = state.faceSelector->GetFaceCenter(mesh, index);
    const glm::vec3 n = state.faceSelector->GetFaceNormal(mesh, index);
    switch (load.type) {
    case fem::LoadType::Fixed:
    case fem::LoadType::Pin:
    case fem::LoadType::Contact:
    case fem::LoadType::Roller:
    case fem::LoadType::Held:
    case fem::LoadType::Cylindrical:
      label(c, SimulationLoadLabel(load), col(kFixed));
      break;
    case fem::LoadType::Force: {
      // At the arrow's free end, whichever end that is (see the glyph).
      const glm::vec3 dir = glm::normalize(glm::vec3(load.direction));
      const glm::vec3 tail = glm::dot(dir, n) > 0.0f ? c : c - dir * len;
      const glm::vec3 free = glm::dot(dir, n) > 0.0f ? tail + dir * len : tail;
      label(free, SimulationLoadLabel(load), col(kForce));
      break;
    }
    case fem::LoadType::Pressure:
      label(c + n * len * 0.8f, SimulationLoadLabel(load), col(kPressure));
      break;
    case fem::LoadType::Torque:
      label(c + n * len * 0.6f, SimulationLoadLabel(load), col(kTorque));
      break;
    case fem::LoadType::Bearing: {
      // Behind the half it pushes on, against its direction.
      glm::vec3 d(load.direction);
      d = glm::length(d) > 1e-9f ? glm::normalize(d) : n;
      label(c - d * len * 0.9f, SimulationLoadLabel(load), col(kForce));
      break;
    }
    case fem::LoadType::Remote: {
      glm::vec3 dir(load.direction);
      dir =
          glm::length(dir) > 1e-9f ? glm::normalize(dir) : glm::vec3(0, 0, -1);
      label(glm::vec3(load.point) - dir * len, SimulationLoadLabel(load),
            col(kRemote));
      break;
    }
    case fem::LoadType::Temperature:
    case fem::LoadType::HeatFlux:
    case fem::LoadType::Convection:
    case fem::LoadType::HeatGeneration:
      label(c + n * len * 0.4f, SimulationLoadLabel(load), col(kPressure));
      break;
    }
  }
  if (pictured)
    return; // the keep faces' and gravity's labels too
  for (const ElementRef& keep : s->keepFaces) {
    uint32_t mesh = 0, index = 0;
    if (ResolveFaceItem(state, {SelectionItem::Kind::Face, 0, keep}, mesh,
                        index))
      label(state.faceSelector->GetFaceCenter(mesh, index), "keep", col(kKeep));
  }
  if (s->type == fem::AnalysisType::Static && sim.comKnown &&
      glm::length(s->gravity) > 1e-9) {
    const glm::vec3 g = glm::normalize(glm::vec3(s->gravity));
    label(sim.com + g * len * 1.3f, SimulationGravityLabel(s->gravity),
          col(kGravity));
  }
}

void SimulationApplyToFace(AppState& state, const ElementRef& face) {
  SimulationApplyToFace(state, 0, face);
}

void SimulationApplyToFace(AppState& state, uint64_t occurrenceId,
                           const ElementRef& face) {
  SimulationState& sim = state.simulation;
  const StudyHost host = StudyHostOf(state);
  if (!host || sim.tool == SimulationState::Tool::None)
    return;
  const fem::Study* s = host.Find(sim.activeStudyId);
  if (!s)
    return;
  sim.placeRefusal.clear();
  // On an assembly only the MESHED occurrence takes a load; a click on a
  // support or a bystander is refused where the person can read it, not
  // dropped on the floor.
  const Document* faceDoc = host.GeometryPart();
  if (host.OnAssembly()) {
    const fem::Participant* pt = host.sim ? host.sim->FindRole(occurrenceId)
                                          : s->FindParticipant(occurrenceId);
    if (!pt || pt->role != fem::ParticipantRole::Meshed) {
      sim.placeRefusal = host.OccurrenceName(occurrenceId) +
                         " is not the meshed occurrence; a load goes on the "
                         "part being solved";
      return;
    }
    const Occurrence* occ = host.Assembly()->FindById(occurrenceId);
    faceDoc = occ ? OccurrencePartDoc(state, *occ) : nullptr;
  }

  // The geometry step's tools: suppress, cut on, refine.
  if (SimulationGeometryPick(state, occurrenceId, face))
    return;

  // Probe: pin the face (the Probes list under the results); the tool stays
  // armed, so several faces can be pinned in a row.
  if (sim.tool == SimulationState::Tool::Probe) {
    if (!ProbeFace(state, *s, face)) {
      sim.placeRefusal = "'" + face.name +
                         "' has no stress to read: run the study (a static "
                         "part study) and pick a face of a body it solved";
      return;
    }
    const bool dup = std::any_of(sim.probes.begin(), sim.probes.end(),
                                 [&](const SimulationState::ProbePin& p) {
                                   return p.studyId == s->id &&
                                          p.face.featureId == face.featureId &&
                                          p.face.name == face.name;
                                 });
    if (!dup)
      sim.probes.push_back({s->id, face});
    return;
  }

  // A face a feature suppressed for the analysis made is not in the shape
  // the study solves: refused here, naming the feature, as its run would.
  if (faceDoc != nullptr && !host.sim->Geometry().suppressed.empty()) {
    uint64_t document = 0;
    if (host.OnAssembly())
      if (const Occurrence* occ = host.Assembly()->FindById(occurrenceId))
        document = occ->partId;
    const std::vector<uint64_t> ids =
        host.sim->Geometry().SuppressedIn(document);
    fem::Study one;
    fem::Load at;
    at.face = face;
    one.loads.push_back(at);
    std::string why;
    if (!ids.empty() && !fem::StudyForAnalysis(*faceDoc, one, ids, &why)) {
      sim.placeRefusal = why;
      return;
    }
  }

  // Keep tool (topology): freeze the picked face solid.
  if (sim.tool == SimulationState::Tool::Keep) {
    ModifyStudy(state, s->id, "Add keep face", [&](fem::Study& cs) {
      const bool dup = std::any_of(
          cs.keepFaces.begin(), cs.keepFaces.end(), [&](const ElementRef& k) {
            return k.featureId == face.featureId && k.name == face.name;
          });
      if (!dup)
        cs.keepFaces.push_back(face);
    });
    sim.tool = SimulationState::Tool::None;
    return;
  }

  // A thermal study's loads (docs/simulation/README.md#thermal): refused,
  // visibly and with nothing placed, until the tool has its values.
  {
    using T = SimulationState::Tool;
    fem::Load th;
    th.type = sim.tool == T::Temperature  ? fem::LoadType::Temperature
              : sim.tool == T::HeatFlux   ? fem::LoadType::HeatFlux
              : sim.tool == T::Convection ? fem::LoadType::Convection
              : sim.tool == T::HeatSource ? fem::LoadType::HeatGeneration
                                          : fem::LoadType::Fixed;
    if (fem::IsThermalLoad(th.type)) {
      // Only the quantities this type takes: the others' fields may hold
      // values typed for another tool.
      if (th.type == fem::LoadType::Temperature)
        th.temperatureC = sim.pendingTemperatureC;
      if (th.type == fem::LoadType::Convection) {
        th.filmWm2K = sim.pendingFilmWm2K;
        th.ambientC = sim.pendingAmbientC;
      }
      if (th.type == fem::LoadType::HeatFlux && sim.pendingFluxPerArea)
        th.fluxWm2 = sim.pendingFluxWm2;
      else if (th.type == fem::LoadType::HeatFlux ||
               th.type == fem::LoadType::HeatGeneration)
        th.powerW = sim.pendingHeatW;
      if (th.type == fem::LoadType::HeatGeneration) {
        fem::ResolvedFace rf;
        if (!faceDoc ||
            !fem::ResolveFace(*faceDoc, face, "heat source", rf, nullptr)) {
          sim.placeRefusal = "'" + face.name + "' is on no body of this part";
          return;
        }
        th.body = faceDoc->GetBodies()[rf.bodyIndex].name;
      } else {
        th.face = face;
      }
      if (const std::string why = fem::ThermalLoadRefusal(th); !why.empty()) {
        sim.placeRefusal = why;
        return;
      }
      fem::Study with = *s;
      with.loads.push_back(th);
      if (const std::string why = fem::ThermalRefusal(with); !why.empty()) {
        sim.placeRefusal = why;
        return;
      }
      th.id = host.AllocateId();
      ModifyStudy(state, s->id,
                  std::string("Add ") + LoadLabel(th.type) + " load",
                  [&](fem::Study& cs) { cs.loads.push_back(th); });
      sim.tool = SimulationState::Tool::None;
      return;
    }
  }

  fem::Load load;
  switch (sim.tool) {
  case SimulationState::Tool::Fixed:
    load.type = fem::LoadType::Fixed;
    break;
  case SimulationState::Tool::Force:
    load.type = fem::LoadType::Force;
    break;
  case SimulationState::Tool::Torque:
    load.type = fem::LoadType::Torque;
    break;
  case SimulationState::Tool::Remote:
    load.type = fem::LoadType::Remote;
    break;
  case SimulationState::Tool::Bearing:
    load.type = fem::LoadType::Bearing;
    break;
  case SimulationState::Tool::Pin:
    load.type = fem::LoadType::Pin;
    break;
  case SimulationState::Tool::Contact:
    load.type = fem::LoadType::Contact;
    break;
  case SimulationState::Tool::Roller:
    load.type = fem::LoadType::Roller;
    break;
  case SimulationState::Tool::Held:
    load.type = fem::LoadType::Held;
    break;
  case SimulationState::Tool::Cylindrical:
    load.type = fem::LoadType::Cylindrical;
    break;
  default:
    load.type = fem::LoadType::Pressure;
    break;
  }
  load.face = face;
  load.occurrenceId = occurrenceId;
  // Where the face is, in the frame the study solves in (the world, on an
  // assembly): a torque's axis and a remote load's point are stated there.
  const Occurrence* placedOcc = occurrenceId != 0 && host.Assembly()
                                    ? host.Assembly()->FindById(occurrenceId)
                                    : nullptr;
  std::optional<Measure::FaceInfo> info;
  double bodyDiagonal = 0.0;
  if (faceDoc) {
    fem::ResolvedFace rf;
    if (fem::ResolveFace(*faceDoc, face, "load", rf, nullptr)) {
      const Body& body = faceDoc->GetBodies()[rf.bodyIndex];
      info = Measure::DescribeFaceAt(body, rf.faceIndex);
      glm::dvec3 lo(0.0), hi(0.0);
      body.shape.GetBoundingBox(lo, hi);
      bodyDiagonal = glm::length(hi - lo);
    }
  }
  if (load.type == fem::LoadType::Torque) {
    if (sim.torqueAxisChosen) {
      glm::dvec3 o, d;
      if (!ResolveTorqueAxis(state, sim.torqueAxis, o, d)) {
        sim.placeRefusal = "The chosen axis no longer resolves; choose "
                           "another";
        return;
      }
      load.axisOrigin = o;
      load.axisDirection = d;
    } else if (!faceDoc || !fem::CylindricalFaceAxis(*faceDoc, face)) {
      // Refused where it can be read, and nothing placed: a flat face has
      // no axis of its own, and guessing one is a different load.
      sim.placeRefusal = "'" + face.name +
                         "' is not cylindrical, so it has no axis of its "
                         "own: choose the torque's axis above, then click "
                         "it again";
      return;
    }
    // Seeded when nothing was typed: the part's weight acting at the face's
    // radius, a torque the part itself scales.
    if (sim.pendingTorque == 0.0 && sim.pendingTorqueExpr.empty() && info) {
      const double r = info->hasRadius
                           ? info->radius
                           : std::sqrt(info->area / 3.14159265358979);
      sim.pendingTorque = SimulationWeightN(state) * r;
    }
    load.magnitude = sim.pendingTorque;
    load.magnitude.expr = sim.pendingTorqueExpr;
  }
  if (load.type == fem::LoadType::Pin) {
    // Refused where it can be read, and nothing placed: a pin holds a
    // cylinder's axis, and a flat face has none.
    if (s->type != fem::AnalysisType::Static) {
      sim.placeRefusal = "A pin is a static study's support; this "
                         "study holds its part with fixed faces";
      return;
    }
    if (!faceDoc || !fem::CylindricalFaceAxis(*faceDoc, face)) {
      sim.placeRefusal = "'" + face.name +
                         "' is not cylindrical: a pin goes on a bore or a "
                         "shaft, whose axis it holds";
      return;
    }
    load.fixRotation = sim.pendingPinFixRotation;
  }
  if (fem::IsFrameSupport(load.type)) {
    // Refused where it can be read, and nothing placed
    // (docs/simulation/README.md
    // #rollers-held-directions-and-cylindrical-supports).
    if (load.type == fem::LoadType::Held) {
      load.hold = sim.pendingHold;
      load.frame = sim.pendingHeldFrame;
      if (s->type == fem::AnalysisType::Static)
        for (int k = 0; k < 3; ++k)
          if (load.hold[k]) {
            load.prescribedMm[k] = sim.pendingHeldMm[k];
            load.prescribedMm[k].expr = sim.pendingHeldMmExpr[k];
          }
      if (!load.hold[0] && !load.hold[1] && !load.hold[2]) {
        sim.placeRefusal = "Tick the directions it holds (x, y, z), then "
                           "click the face again";
        return;
      }
    }
    if (load.type == fem::LoadType::Cylindrical) {
      if (!faceDoc || !fem::CylindricalFaceAxis(*faceDoc, face)) {
        sim.placeRefusal = "'" + face.name +
                           "' is not cylindrical: a cylindrical support "
                           "holds a bore or a shaft about its axis";
        return;
      }
      load.hold = sim.pendingCylHold;
      if (!load.hold[0] && !load.hold[1] && !load.hold[2]) {
        sim.placeRefusal = "Tick the directions it holds (radial, "
                           "tangential, axial), then click the face again";
        return;
      }
    }
    fem::Study with = *s;
    with.loads.push_back(load);
    if (const std::string why = fem::FrameSupportRefusal(with); !why.empty()) {
      sim.placeRefusal = why;
      return;
    }
  }
  if (load.type == fem::LoadType::Contact) {
    // Refused where it can be read, and nothing placed: on the ground a
    // face rests on the plane it lies on, which only a flat face has.
    if (!info || info->kind != "plane") {
      sim.placeRefusal = "'" + face.name +
                         "' is not flat: a contact support rests a flat face "
                         "on the ground (a curved one rests on a rigid "
                         "occurrence: Rest on it, on an assembly study)";
      return;
    }
    load.friction = sim.pendingContactFriction;
    fem::Study with = *s;
    with.loads.push_back(load);
    if (const std::string why = fem::ContactRefusal(with); !why.empty()) {
      sim.placeRefusal = why;
      return;
    }
  }
  if (load.type == fem::LoadType::Bearing) {
    // Refused where it can be read, and nothing placed: a bearing load
    // presses a bore or a shaft across its axis.
    const auto axis =
        faceDoc ? fem::CylindricalFaceAxis(*faceDoc, face) : std::nullopt;
    if (!axis) {
      sim.placeRefusal = "'" + face.name +
                         "' is not cylindrical: a bearing load presses a "
                         "bore or a shaft (a force on a flat face is a Force)";
      return;
    }
    glm::dvec3 a = axis->direction;
    if (placedOcc)
      a = glm::mat3_cast(placedOcc->orientation) * a;
    glm::dvec3 d = sim.pendingDirection;
    if (!sim.directionOverridden) {
      // Down, across the axis: what a part's weight or a load hung from the
      // bore does. A vertical bore has no down across it.
      d = -conv::kWorldUp - glm::dot(-conv::kWorldUp, a) * a;
      if (glm::length(d) < 1e-6) {
        sim.placeRefusal = "'" + face.name +
                           "' stands vertical, so down is along it: tick "
                           "Custom direction and give the way it is pushed";
        return;
      }
      d = glm::normalize(d);
    }
    load.direction = d;
    load.axisDirection = a; // for the check only; never saved
    if (const std::string why = fem::BearingDirectionRefusal(load);
        !why.empty()) {
      sim.placeRefusal = why;
      return;
    }
    load.axisDirection = glm::dvec3(0.0);
  }
  if (load.type == fem::LoadType::Remote) {
    if (!sim.pointOverridden) {
      if (!info) {
        sim.placeRefusal = "'" + face.name +
                           "' could not be measured for a remote load's "
                           "point; type one in At";
        return;
      }
      // Off the face along its normal, a tenth of the body's diagonal: a
      // lever arm the picture can show, derived from the part.
      glm::dvec3 p = info->centroid + info->normal * (0.1 * bodyDiagonal);
      if (placedOcc)
        p = glm::mat3_cast(placedOcc->orientation) * p + placedOcc->position;
      sim.pendingPoint = p;
    }
    load.point = sim.pendingPoint;
    load.hasPoint = true;
  }
  load.id = host.AllocateId();
  if (!fem::IsSupport(load.type)) {
    if (load.type != fem::LoadType::Torque) {
      load.magnitude = sim.pendingMagnitude;
      load.magnitude.expr = sim.pendingMagnitudeExpr;
    }
    // On a study with load cases it goes in the one chosen (the first when
    // none is, or the chosen one is gone).
    if (!s->loadCases.empty())
      load.loadCase = s->FindLoadCase(sim.pendingCase) >= 0
                          ? sim.pendingCase
                          : s->loadCases.front().name;
  }
  if ((load.type == fem::LoadType::Force ||
       load.type == fem::LoadType::Remote) &&
      faceDoc) {
    // The face decides the direction unless the user asked for a specific
    // one: a force placed on a face pushes into it.
    auto into = fem::DefaultForceDirection(*faceDoc, face);
    // A part's inward normal, turned into the world the placed part is
    // solved in.
    if (into && occurrenceId != 0 && host.Assembly())
      if (const Occurrence* occ = host.Assembly()->FindById(occurrenceId))
        into = glm::mat3_cast(occ->orientation) * *into;
    load.direction =
        sim.directionOverridden || !into ? sim.pendingDirection : *into;
  }

  ModifyStudy(state, s->id,
              std::string("Add ") + LoadLabel(load.type) + " load",
              [&](fem::Study& cs) { cs.loads.push_back(load); });
  sim.tool = SimulationState::Tool::None; // one placement per arm
}

} // namespace ui
