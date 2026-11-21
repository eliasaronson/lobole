// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The simulation tab's Model dock (docs/ui/panels.md
// #simulation-tab-the-model-panel): the tree of what the simulation holds.
// A row says one thing -- its name and one short value -- with its state a
// dot on its glyph; no buttons and no fields. The tools are the toolbar's,
// and the tool window shows what a row is set to.

#include "ui/SimTree.h"

#include "core/Occurrence.h"
#include "core/fem/FemFields.h"
#include "core/fem/SimDocument.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/Icons.h"
#include "ui/ListSelection.h"
#include "ui/SimRunActions.h"
#include "ui/SimulationActions.h"
#include "ui/SimulationView.h"
#include "ui/StudyHost.h"
#include "ui/StudyPanel.h" // SimulationGravityLabel, SetSimParticipantRole
#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ui {

namespace {

using FocusKind = SimulationState::Focus::Kind;

// The colours the study list has always used: a failure red, a stale
// result amber; a current one green and a run in flight the accent.
const ImVec4 kFailed(0.95f, 0.5f, 0.4f, 1.0f);
const ImVec4 kStaleDot(0.95f, 0.75f, 0.35f, 1.0f);
const ImVec4 kCurrent(0.45f, 0.8f, 0.5f, 1.0f);
const ImVec4 kRunning(0.45f, 0.7f, 1.0f, 1.0f);

struct RowState {
  ImVec4 dot{0, 0, 0, 0};
  std::string word;
};

// Where a study stands: solving, failed, not run, stale or solved.
RowState StudyState(AppState& state, const StudyHost& host,
                    const fem::Study& s) {
  const uint64_t slot = host.simSlotId;
  if (GetStudyRunStatus(state, s.id, slot).running ||
      GetTopologyStatus(state, s.id, slot).running)
    return {kRunning, "solving"};
  if (!s.error.empty())
    return {kFailed, "failed: " + s.error};
  if (host.Results(s.id) == nullptr)
    return {{0, 0, 0, 0}, "not run"};
  if (StudyStale(state, s.id))
    return {kStaleDot, "stale: the model changed since it was solved"};
  return {kCurrent, "solved"};
}

// A study's headline number, for its row's tail: the peak stress, the
// first frequency or buckling factor, the peak temperature, the volume.
std::string Headline(const fem::Study& s, const fem::StudyRuntime* rt) {
  if (rt == nullptr)
    return "";
  char buf[64] = "";
  switch (s.type) {
  case fem::AnalysisType::Static:
    std::snprintf(buf, sizeof(buf), "%.3g MPa", rt->maxVonMises);
    break;
  case fem::AnalysisType::Modal:
  case fem::AnalysisType::Buckling:
    for (const fem::StudyBodyResult& b : rt->bodies)
      if (!b.results.modes.empty()) {
        if (s.type == fem::AnalysisType::Modal)
          std::snprintf(buf, sizeof(buf), "%.4g Hz",
                        b.results.modes[0].frequencyHz);
        else
          std::snprintf(buf, sizeof(buf), "x %.3g", b.results.modes[0].factor);
        break;
      }
    break;
  case fem::AnalysisType::Thermal: {
    double peak = -1e300;
    for (const fem::StudyBodyResult& b : rt->bodies)
      peak = std::max(peak, b.thermal.maxC);
    if (peak > -1e300)
      std::snprintf(buf, sizeof(buf),
                    "%.4g \xC2\xB0"
                    "C",
                    peak);
    break;
  }
  case fem::AnalysisType::Topology:
    std::snprintf(buf, sizeof(buf), "%.0f %% volume",
                  rt->volumeFractionAchieved * 100.0);
    break;
  }
  return buf;
}

// A face's name as a row shows it: "face3", not "12/face3" -- the
// feature's id is the window's to say, and the dock is narrow.
std::string ShortFace(const std::string& name) {
  const size_t slash = name.rfind('/');
  return slash == std::string::npos ? name : name.substr(slash + 1);
}

// What a load stands on, as its row names it.
std::string LoadPlace(const StudyHost& host, const fem::Load& l) {
  if (l.contactWith != 0)
    return "contact with " + host.OccurrenceName(l.contactWith);
  if (l.occurrenceId != 0)
    return host.OccurrenceName(l.occurrenceId) + " " + ShortFace(l.face.name);
  if (l.type == fem::LoadType::HeatGeneration)
    return l.body; // a heat source names its body, not a face
  return ShortFace(l.face.name);
}

std::string LoadTooltip(const StudyHost& host, const fem::Load& l) {
  std::string out = std::string(SimLoadName(l.type)) + ": " +
                    (l.contactWith != 0    ? LoadPlace(host, l)
                     : l.face.name.empty() ? l.body
                     : l.occurrenceId != 0 ? LoadPlace(host, l)
                                           : l.face.name);
  if (!l.loadCase.empty())
    out += ", in " + l.loadCase;
  char buf[200] = "";
  if (l.type == fem::LoadType::Force)
    // A force's direction decides the answer; showing only its magnitude
    // hid the one number most likely to be wrong.
    std::snprintf(buf, sizeof(buf), "Direction [%.2g, %.2g, %.2g]",
                  l.direction.x, l.direction.y, l.direction.z);
  else if (l.type == fem::LoadType::Torque && l.HasAxis())
    std::snprintf(buf, sizeof(buf),
                  "About (%.3g, %.3g, %.3g) through (%.3g, %.3g, %.3g)",
                  l.axisDirection.x, l.axisDirection.y, l.axisDirection.z,
                  l.axisOrigin.x, l.axisOrigin.y, l.axisOrigin.z);
  else if (l.type == fem::LoadType::Torque)
    std::snprintf(buf, sizeof(buf), "About the cylindrical face's own axis");
  else if (l.type == fem::LoadType::Remote)
    std::snprintf(buf, sizeof(buf),
                  "At (%.3g, %.3g, %.3g) mm, along [%.2g, %.2g, %.2g]",
                  l.point.x, l.point.y, l.point.z, l.direction.x, l.direction.y,
                  l.direction.z);
  return buf[0] != '\0' ? out + "\n" + buf : out;
}

// The face last picked in the viewport, so a pick on a load's face selects
// its row once, and the tree's own clicks are not undone by it.
struct PickSeen {
  uint64_t occurrence = 0, feature = 0;
  std::string name;
  bool operator==(const PickSeen&) const = default;
  bool On(const fem::Load& l) const {
    return l.contactWith == 0 && feature == l.face.featureId &&
           name == l.face.name && occurrence == l.occurrenceId;
  }
};

// The mesh row's tail and dot: the session's mesh when there is one, else
// the mesh the stored result was solved on.
struct MeshRow {
  std::string tail;
  RowState st;
  int warnings = 0;
};
MeshRow MeshRowOf(AppState& state, const StudyHost& host, const fem::Study& s) {
  MeshRow out;
  if (GetMeshRunStatus(state, s.id, host.simSlotId).running) {
    out.tail = "meshing";
    out.st = {kRunning, "meshing"};
    return out;
  }
  const MeshReport* mr = StudyMesh(state, s.id);
  if (mr != nullptr && mr->error.empty()) {
    size_t elements = 0;
    for (const MeshReport::Body& b : mr->bodies)
      elements += b.elements;
    // The report's warnings, counted once per mesh made.
    static std::map<uint64_t, int> counted;
    auto it = counted.find(mr->epoch);
    if (it == counted.end()) {
      int n = 0;
      for (const MeshFinding& f : MeshFindings(state, *mr))
        n += f.level == MeshFinding::Level::Warning;
      if (counted.size() > 64)
        counted.clear();
      it = counted.emplace(mr->epoch, n).first;
    }
    out.warnings = it->second;
    out.tail = std::to_string(elements) + " tets";
    out.st = StudyMeshStale(state, s.id)
                 ? RowState{kStaleDot, "stale: the geometry or the mesh "
                                       "settings changed since it was made"}
                 : RowState{kCurrent, "current"};
    return out;
  }
  if (mr != nullptr) {
    out.tail = "failed";
    out.st = {kFailed, "failed: " + mr->error};
    return out;
  }
  if (const fem::StudyRuntime* rt = host.Results(s.id)) {
    size_t elements = 0;
    for (const fem::StudyBodyResult& b : rt->bodies)
      elements += b.mesh.stats.tetCount;
    if (elements > 0) {
      out.tail = std::to_string(elements) + " tets";
      out.st = {{0, 0, 0, 0}, "the mesh the result was solved on"};
      return out;
    }
  }
  out.tail = "not built";
  out.st = {{0, 0, 0, 0}, "not built: Mesh on the toolbar, or Run"};
  return out;
}

// A plain click on a row the list selection does not reach (a mesh, a
// case): it alone is lit, so the studies and loads let go.
void FocusOnly(AppState& state, FocusKind kind, uint64_t studyId,
               uint64_t id = 0, int index = -1) {
  std::erase_if(state.selection.items, [](const SelectionItem& it) {
    return it.kind == SelectionItem::Kind::SimStudy ||
           it.kind == SelectionItem::Kind::SimLoad;
  });
  state.simulation.tool = SimulationState::Tool::None;
  FocusSim(state, kind, studyId, id, index);
}

bool Focused(const AppState& state, FocusKind kind, uint64_t studyId,
             uint64_t id = 0, int index = -1) {
  const SimulationState::Focus& f = state.simulation.focus;
  return f.kind == kind && f.studyId == studyId && f.id == id &&
         f.index == index;
}

// A mesh row: the simulation's (###sim_mesh, shown for `s`, a study that
// solves on it) or one study's own (###sim_mesh_<id>). A click shows that
// mesh in the tool window.
void DrawMeshRow(AppState& state, const StudyHost& host, const fem::Study& s,
                 const std::string& id, bool selected) {
  const MeshRow m = MeshRowOf(state, host, s);
  ListRowSpec row;
  row.glyph = CommandIcon("sim.mesh");
  row.label = "Mesh" + id;
  row.tail = m.tail;
  if (m.warnings > 0) {
    row.tailGlyph = ICON_FA_TRIANGLE_EXCLAMATION;
    row.tail = std::to_string(m.warnings) + "  " + m.tail;
    row.tooltip = std::to_string(m.warnings) + " warning" +
                  (m.warnings == 1 ? "" : "s") + " in the mesh report; ";
  }
  row.tooltip += m.st.word;
  row.dotColor = m.st.dot;
  row.selected = selected;
  const ListRowResult res = ListRow(row);
  if (res.clicked || res.doubleClicked)
    FocusOnly(state, FocusKind::Mesh, s.id);
}

// The simulation's mesh row: one for every study that solves on it, read
// through the active study when it is one of them, else the first that has
// a mesh to say anything about, else the first. Absent when every study
// meshes apart (or there are none).
void DrawSharedMeshRow(AppState& state, const StudyHost& host) {
  const fem::Study* shown = nullptr;
  const auto shares = [](const fem::Study& s) {
    return s.shapeFrom.study == 0 && fem::SimDocument::SharesMesh(s);
  };
  if (const fem::Study* a = host.Find(state.simulation.activeStudyId);
      a != nullptr && shares(*a))
    shown = a;
  for (const fem::Study& s : host.Studies())
    if (shown == nullptr && shares(s) &&
        (StudyMesh(state, s.id) != nullptr || host.Results(s.id) != nullptr))
      shown = &s;
  for (const fem::Study& s : host.Studies())
    if (shown == nullptr && shares(s))
      shown = &s;
  if (shown == nullptr)
    return;
  const SimulationState::Focus& f = state.simulation.focus;
  const fem::Study* focused =
      f.kind == FocusKind::Mesh ? host.Find(f.studyId) : nullptr;
  DrawMeshRow(state, host, *shown, "###sim_mesh",
              focused != nullptr && shares(*focused));
}

// The subject row (###sim_subject) and, on an assembly, a row per
// participant with its role as its menu.
void DrawSubject(AppState& state, const StudyHost& host) {
  const fem::SimSubject& subject = host.sim->Subject();
  // The subject's name as its tab has it now; the document's copy is what
  // a deleted subject is still called.
  const std::optional<size_t> at = SlotIndexById(state, subject.docId);
  const bool gone = !at;
  const bool assembly = subject.kind == fem::SubjectKind::Assembly;
  ListRowSpec row;
  row.glyph = assembly ? ICON_FA_CUBES : ICON_FA_CUBE;
  row.label = (gone ? subject.name : state.parts[*at].name) + "###sim_subject";
  row.dimmed = gone;
  row.tail = gone ? "missing" : fem::SubjectKindToString(subject.kind);
  row.selected = Focused(state, FocusKind::Subject, 0);
  if (gone)
    row.tooltip = "The " + std::string(fem::SubjectKindToString(subject.kind)) +
                  " this simulation analyses is no longer in the project";
  const ListRowResult res = ListRow(row);
  if (res.clicked || res.doubleClicked)
    FocusOnly(state, FocusKind::Subject, 0);
  if (!assembly || host.Assembly() == nullptr)
    return;
  const float indent = ImGui::GetFontSize() * 1.3f;
  ImGui::Indent(indent);
  for (const Occurrence& occ : host.Assembly()->GetOccurrences()) {
    const fem::Participant* pt = host.sim->FindRole(occ.id);
    const fem::ParticipantRole role =
        pt ? pt->role : fem::ParticipantRole::Ignored;
    ListRowSpec p;
    p.glyph = ICON_FA_CUBE;
    p.label = occ.name + "###sim_part_" + std::to_string(occ.id);
    p.tail = fem::ParticipantRoleToString(role);
    p.dimmed = role == fem::ParticipantRole::Ignored;
    p.selected = Focused(state, FocusKind::Participant, 0, occ.id);
    p.menuId = "###sim_part_ctx_" + std::to_string(occ.id);
    const uint64_t id = occ.id;
    p.menu = [&state, id, role] {
      // The one control a row keeps: a role is a single choice, read with
      // the others (the CAM part row's Configuration menu is the same).
      if (ImGui::BeginMenu("Role###sim_part_role")) {
        for (const fem::ParticipantRole r :
             {fem::ParticipantRole::Meshed, fem::ParticipantRole::Bonded,
              fem::ParticipantRole::Contact, fem::ParticipantRole::Rigid,
              fem::ParticipantRole::Ignored}) {
          const std::string word = fem::ParticipantRoleToString(r);
          std::string label = word;
          label[0] = static_cast<char>(
              std::toupper(static_cast<unsigned char>(label[0])));
          if (ImGui::MenuItem(
                  (label + "###role_set_" + std::to_string(id) + "_" + word)
                      .c_str(),
                  nullptr, r == role) &&
              r != role)
            SetSimParticipantRole(state, id, r);
        }
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Settings###sim_part_edit"))
        FocusOnly(state, FocusKind::Participant, 0, id);
    };
    const ListRowResult res2 = ListRow(p);
    if (res2.clicked || res2.doubleClicked)
      FocusOnly(state, FocusKind::Participant, 0, id);
  }
  ImGui::Unindent(indent);
}

// Everything under one study, in the order the work goes.
void DrawStudyChildren(AppState& state, const StudyHost& host,
                       const fem::Study& s,
                       const std::vector<uint64_t>& loadOrder,
                       const PickSeen* picked) {
  SimulationState& sim = state.simulation;
  const uint64_t sid = s.id;

  // The study's own mesh, where it meshes apart from the simulation's (a
  // size or a refinement of its own); a study of a topology result meshes
  // nothing of its own.
  if (s.shapeFrom.study == 0 && !fem::SimDocument::SharesMesh(s))
    DrawMeshRow(state, host, s, "###sim_mesh_" + std::to_string(sid),
                Focused(state, FocusKind::Mesh, sid));

  // Supports, then loads; the evaluated values (a load typed as a
  // variable shows what it is now).
  const fem::Study shown = host.Shown(s);
  bool removeLoads = false;
  for (const bool supports : {true, false})
    for (const fem::Load& l : shown.loads) {
      if (fem::IsSupport(l.type) != supports)
        continue;
      ListRowSpec row;
      row.glyph = CommandIcon(SimLoadCommand(l.type));
      row.label = std::string(SimLoadName(l.type)) + "  " + LoadPlace(host, l) +
                  "###load_row_" + std::to_string(l.id);
      row.tail = SimLoadTail(l);
      row.tooltip = LoadTooltip(host, l);
      row.selected = IsSelected(state, SelectionItem::Kind::SimLoad, l.id) ||
                     Focused(state, FocusKind::Load, sid, l.id) ||
                     (picked != nullptr && picked->On(l));
      row.menuId = "###load_ctx_" + std::to_string(l.id);
      const uint64_t loadId = l.id;
      row.menu = [&state, &removeLoads, sid, loadId] {
        if (!IsSelected(state, SelectionItem::Kind::SimLoad, loadId))
          ListRowContext(state, SelectionItem::Kind::SimLoad, loadId, false);
        if (ImGui::MenuItem("Settings###load_edit"))
          FocusSim(state, FocusKind::Load, sid, loadId);
        const size_t n =
            SelectedIds(state, SelectionItem::Kind::SimLoad).size();
        if (ImGui::MenuItem(
                (ICON_FA_TRASH_CAN " " + DeleteLabel(n, "load") + "###load_del")
                    .c_str()))
          removeLoads = true;
      };
      const ListRowResult res = ListRow(row);
      if (res.clicked || res.doubleClicked) {
        sim.tool = SimulationState::Tool::None;
        FocusSim(state, FocusKind::Load, sid, loadId);
        ListRowClicked(state, SelectionItem::Kind::SimLoad, loadId, loadOrder,
                       /*ownKindOnly=*/false);
      }
    }
  if (removeLoads)
    DeleteSelection(state); // the row's menu put them in the selection

  // Gravity and a thermal load are the study's own settings, shown as the
  // load they are.
  if (s.gravity.x != 0 || s.gravity.y != 0 || s.gravity.z != 0) {
    ListRowSpec row;
    row.glyph = ICON_FA_ARROW_DOWN;
    row.label = "Gravity###sim_gravity_" + std::to_string(sid);
    row.tail = SimulationGravityLabel(s.gravity);
    row.tooltip = "Self-weight: in the study's settings";
    const ListRowResult res = ListRow(row);
    if (res.clicked || res.doubleClicked)
      FocusOnly(state, FocusKind::Study, sid);
  }
  if (s.thermalLoad.On()) {
    ListRowSpec row;
    row.glyph = ICON_FA_TEMPERATURE_ARROW_UP;
    row.label = "Thermal load###sim_thermal_load_" + std::to_string(sid);
    if (s.thermalLoad.kind == fem::ThermalLoad::Kind::Uniform) {
      char buf[48];
      std::snprintf(buf, sizeof(buf),
                    "%.4g \xC2\xB0"
                    "C",
                    s.thermalLoad.uniformC);
      row.tail = buf;
    } else if (const fem::Study* from = host.Find(s.thermalLoad.fromStudy)) {
      row.tail = "from " + from->name;
    }
    row.tooltip = "The temperature the part is solved at: in the study's "
                  "settings";
    const ListRowResult res = ListRow(row);
    if (res.clicked || res.doubleClicked)
      FocusOnly(state, FocusKind::Study, sid);
  }

  // Load cases: each its own solve.
  for (size_t i = 0; i < s.loadCases.size(); ++i) {
    const fem::LoadCase& c = s.loadCases[i];
    const int n = static_cast<int>(
        std::count_if(s.loads.begin(), s.loads.end(), [&](const fem::Load& l) {
          return l.loadCase == c.name;
        }));
    ListRowSpec row;
    row.glyph = CommandIcon("sim.add_case");
    row.label =
        c.name + "###case_row_" + std::to_string(sid) + "_" + std::to_string(i);
    row.tail = std::to_string(n) + (n == 1 ? " load" : " loads");
    if (s.type == fem::AnalysisType::Topology) {
      char w[32];
      std::snprintf(w, sizeof(w), ", weight %.3g", c.weight.value);
      row.tail += w;
    }
    row.selected = Focused(state, FocusKind::Case, sid, 0, static_cast<int>(i));
    const ListRowResult res = ListRow(row);
    if (res.clicked || res.doubleClicked)
      FocusOnly(state, FocusKind::Case, sid, 0, static_cast<int>(i));
  }

  // A topology study's kept faces and keep-outs.
  for (size_t i = 0; i < s.keepFaces.size(); ++i) {
    ListRowSpec row;
    row.glyph = CommandIcon("sim.keep");
    row.label = s.keepFaces[i].name + "###keep_row_" + std::to_string(i);
    row.tail = "kept solid";
    row.selected = Focused(state, FocusKind::Keep, sid, 0, static_cast<int>(i));
    row.menuId = "###keep_ctx_" + std::to_string(i);
    const ElementRef f = s.keepFaces[i];
    row.menu = [&state, sid, f] {
      if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Remove###keep_del"))
        ModifyStudy(state, sid, "Remove keep face", [&](fem::Study& cs) {
          std::erase_if(cs.keepFaces, [&](const ElementRef& k) {
            return k.featureId == f.featureId && k.name == f.name;
          });
        });
    };
    const ListRowResult res = ListRow(row);
    if (res.clicked || res.doubleClicked)
      FocusOnly(state, FocusKind::Keep, sid, 0, static_cast<int>(i));
  }
  for (size_t i = 0; i < s.keepOuts.size(); ++i) {
    const fem::KeepOut& k = s.keepOuts[i];
    char box[160];
    std::snprintf(box, sizeof(box), "box [%.4g, %.4g, %.4g]-[%.4g, %.4g, %.4g]",
                  k.boxMin.x, k.boxMin.y, k.boxMin.z, k.boxMax.x, k.boxMax.y,
                  k.boxMax.z);
    ListRowSpec row;
    row.glyph = ICON_FA_BAN;
    row.label = (k.body.empty() ? std::string(box) : "body " + k.body) +
                "###keepout_row_" + std::to_string(i);
    row.tail = "kept empty";
    row.selected =
        Focused(state, FocusKind::KeepOut, sid, 0, static_cast<int>(i));
    row.menuId = "###keepout_ctx_" + std::to_string(i);
    const int at = static_cast<int>(i);
    row.menu = [&state, sid, at] {
      if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Remove###keepout_del"))
        ModifyStudy(state, sid, "Remove keep-out", [&](fem::Study& cs) {
          if (at < static_cast<int>(cs.keepOuts.size()))
            cs.keepOuts.erase(cs.keepOuts.begin() + at);
        });
    };
    const ListRowResult res = ListRow(row);
    if (res.clicked || res.doubleClicked)
      FocusOnly(state, FocusKind::KeepOut, sid, 0, static_cast<int>(i));
  }

  // The result, once there is one (or one is being made).
  const RowState st = StudyState(state, host, s);
  if (host.Results(sid) != nullptr || st.word == "solving") {
    ListRowSpec row;
    row.glyph = CommandIcon("sim.result");
    row.label = "Result###sim_result_" + std::to_string(sid);
    if (sim.activeStudyId == sid && sim.field != SimulationState::Field::None)
      if (const auto f = ResultFieldOf(sim.field)) {
        row.tail = fem::SpecOf(*f).title;
        if (!row.tail.empty())
          row.tail[0] = static_cast<char>(
              std::toupper(static_cast<unsigned char>(row.tail[0])));
      }
    row.dotColor = st.dot;
    row.tooltip = st.word;
    row.selected = Focused(state, FocusKind::Result, sid);
    const ListRowResult res = ListRow(row);
    if (res.clicked || res.doubleClicked)
      FocusOnly(state, FocusKind::Result, sid);
  }
}

} // namespace

const char* SimLoadName(fem::LoadType t) {
  switch (t) {
  case fem::LoadType::Fixed:
    return "Fixed";
  case fem::LoadType::Force:
    return "Force";
  case fem::LoadType::Pressure:
    return "Pressure";
  case fem::LoadType::Torque:
    return "Torque";
  case fem::LoadType::Remote:
    return "Remote";
  case fem::LoadType::Pin:
    return "Pin";
  case fem::LoadType::Contact:
    return "Contact";
  case fem::LoadType::Roller:
    return "Roller";
  case fem::LoadType::Held:
    return "Held";
  case fem::LoadType::Cylindrical:
    return "Cylindrical";
  case fem::LoadType::Temperature:
    return "Temperature";
  case fem::LoadType::HeatFlux:
    return "Heat flux";
  case fem::LoadType::Convection:
    return "Convection";
  case fem::LoadType::HeatGeneration:
    return "Heat source";
  }
  return "?";
}

const char* SimLoadCommand(fem::LoadType t) {
  switch (t) {
  case fem::LoadType::Fixed:
    return "sim.fixed";
  case fem::LoadType::Force:
    return "sim.force";
  case fem::LoadType::Pressure:
    return "sim.pressure";
  case fem::LoadType::Torque:
    return "sim.torque";
  case fem::LoadType::Remote:
    return "sim.remote";
  case fem::LoadType::Pin:
    return "sim.pin";
  case fem::LoadType::Contact:
    return "sim.contact";
  case fem::LoadType::Roller:
    return "sim.roller";
  case fem::LoadType::Held:
    return "sim.held";
  case fem::LoadType::Cylindrical:
    return "sim.cylindrical";
  case fem::LoadType::Temperature:
    return "sim.temperature";
  case fem::LoadType::HeatFlux:
    return "sim.heat_flux";
  case fem::LoadType::Convection:
    return "sim.convection";
  case fem::LoadType::HeatGeneration:
    return "sim.heat_source";
  }
  return "sim.force";
}

const char* SimStudyCommand(fem::AnalysisType t) {
  switch (t) {
  case fem::AnalysisType::Static:
    return "sim.add_static";
  case fem::AnalysisType::Modal:
    return "sim.add_modal";
  case fem::AnalysisType::Buckling:
    return "sim.add_buckling";
  case fem::AnalysisType::Thermal:
    return "sim.add_thermal";
  case fem::AnalysisType::Topology:
    return "sim.add_topology";
  }
  return "sim.add_static";
}

std::string SimLoadTail(const fem::Load& l) {
  if (fem::IsThermalLoad(l.type) || fem::IsFrameSupport(l.type))
    return SimulationLoadLabel(l);
  if (l.type == fem::LoadType::Fixed)
    return "";
  if (l.type == fem::LoadType::Pin)
    return l.fixRotation ? "held from turning" : "free to turn";
  if (l.type == fem::LoadType::Contact) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s, \xCE\xBC %.3g",
                  l.contactWith != 0 ? "on a rigid part" : "on the ground",
                  l.friction);
    return buf;
  }
  // The value as the viewport's label beside its glyph says it ("50 kPa",
  // "1.2 kN"): one formatter, so the tree and the picture agree. Its case
  // is the case row's, and its tooltip's.
  return SimulationLoadLabel(l);
}

void DrawSimTree(AppState& state) {
  const StudyHost host = StudyHostOf(state);
  if (!host)
    return;
  SimulationState& sim = state.simulation;

  DrawSubject(state, host);
  DrawSharedMeshRow(state, host);

  // A face picked in the viewport on a load selects that load's row, once
  // per pick: the tree and the picture agree on what is selected.
  static PickSeen lastPick;
  std::optional<PickSeen> pick;
  for (const SelectionItem& it : state.selection.items)
    if (it.kind == SelectionItem::Kind::Face) {
      pick = PickSeen{it.id, it.element.featureId, it.element.name};
      break;
    }
  if (pick && !(*pick == lastPick) && sim.tool == SimulationState::Tool::None)
    if (const fem::Study* s = host.Find(sim.activeStudyId))
      for (const fem::Load& l : s->loads)
        if (pick->On(l)) {
          FocusSim(state, FocusKind::Load, s->id, l.id);
          break;
        }
  lastPick = pick.value_or(PickSeen{});

  const std::vector<fem::Study>& studies = host.Studies();
  ImGui::Spacing();
  if (!SectionHeader("Studies###sim_studies", static_cast<int>(studies.size()),
                     true))
    return;
  if (studies.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("No studies yet: add one from the toolbar.");
    ImGui::PopTextWrapPos();
    return;
  }
  // The lists' orders, for a Shift-click's range.
  std::vector<uint64_t> studyOrder, loadOrder;
  for (const fem::Study& s : studies) {
    studyOrder.push_back(s.id);
    for (const bool supports : {true, false})
      for (const fem::Load& l : s.loads)
        if (fem::IsSupport(l.type) == supports)
          loadOrder.push_back(l.id);
  }
  bool deleteStudies = false;
  const float childIndent = ImGui::GetFontSize() * 1.3f;
  for (const fem::Study& s : studies) {
    const RowState st = StudyState(state, host, s);
    ListRowSpec row;
    row.glyph = CommandIcon(SimStudyCommand(s.type));
    row.label = s.name + "###study_sel_" + std::to_string(s.id);
    row.tail = Headline(s, host.Results(s.id));
    row.dotColor = st.dot;
    row.tooltip = st.word;
    if (!s.error.empty())
      row.glyphColor = kFailed;
    row.selected = IsSelected(state, SelectionItem::Kind::SimStudy, s.id) ||
                   Focused(state, FocusKind::Study, s.id);
    row.disclosureId = "###sim_study_open_" + std::to_string(s.id);
    row.disclosureDefaultOpen = true;
    row.menuId = "###study_ctx_" + std::to_string(s.id);
    const uint64_t id = s.id;
    row.menu = [&state, &deleteStudies, id] {
      if (!IsSelected(state, SelectionItem::Kind::SimStudy, id))
        ListRowContext(state, SelectionItem::Kind::SimStudy, id, false);
      if (ImGui::MenuItem("Settings###study_edit"))
        FocusSim(state, FocusKind::Study, id);
      const size_t n = SelectedIds(state, SelectionItem::Kind::SimStudy).size();
      const std::string what =
          n <= 1 ? std::string("Delete study")
                 : "Delete " + std::to_string(n) + " studies"; // not "studys"
      if (ImGui::MenuItem(
              (ICON_FA_TRASH_CAN " " + what + "###study_delete").c_str()))
        deleteStudies = true;
    };
    const ListRowResult res = ListRow(row);
    if (res.clicked || res.doubleClicked) {
      sim.tool = SimulationState::Tool::None;
      FocusSim(state, FocusKind::Study, s.id);
      ListRowClicked(state, SelectionItem::Kind::SimStudy, s.id, studyOrder,
                     /*ownKindOnly=*/false);
    }
    if (!res.open)
      continue;
    ImGui::Indent(childIndent);
    DrawStudyChildren(state, host, s, loadOrder,
                      sim.activeStudyId == s.id && pick ? &*pick : nullptr);
    ImGui::Unindent(childIndent);
  }
  if (deleteStudies)
    DeleteSelection(state);
}

} // namespace ui
