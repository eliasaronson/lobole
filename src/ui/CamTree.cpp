// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The CAM panel: the job as a Model tree
// (docs/ui/panels.md#cam-job-tab-the-cam-panel).
//
// Parts, Setups and Tools are SectionHeaders with counts, the way the part
// and assembly panels list what their document is made of. A setup is a row
// that discloses its operations and fixtures; the expanded, selected one is
// the ACTIVE setup (it was a combo). Settings -- a setup's machine, stock and
// rotary, a part's placement -- open in the floating tool window, and every
// verb that is not a create verb on the toolbar is in a row's menu.
//
// Split from CamPanel.cpp, which keeps the backplot, the toolpath cache, the
// operation row and the settings forms the tool window draws.

#include "ui/CamPanel.h"
#include "ui/CamTool.h"

#include "ui/DockLayout.h" // PanelHeading
#include "ui/PanelTitles.h"
#include "ui/SettingsWindow.h" // OpenPreferences
#include "ui/Widgets.h"

#ifdef HAVE_CAM

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/cam/CamMachine.h"
#include "core/cam/CamMount.h"
#include "core/cam/CamNames.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamGenerate.h"
#include "ui/CamJobs.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h" // ConfiguredPartDocument
#include "ui/ExprInput.h"
#include "ui/FeatureEditor.h" // kCamToolSlot
#include "ui/Icons.h"
#include "ui/JobActions.h"
#include "ui/ListSelection.h"
#include "ui/MachineActions.h"
#include "ui/PreviewFeature.h" // BeginToolWindowFrame
#include "ui/TextFit.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <optional>

namespace ui {

namespace {

const ImVec4 kRed(0.95f, 0.45f, 0.40f, 1.0f);
const ImVec4 kAmber(0.95f, 0.80f, 0.40f, 1.0f);
const ImVec4 kGreen(0.55f, 0.90f, 0.60f, 1.0f);

// A glyph per end shape, so a list of tools reads at a glance.
const char* CamToolGlyph(cam::ToolType type) {
  switch (type) {
  case cam::ToolType::Flat:
    return ICON_FA_SQUARE_FULL;
  case cam::ToolType::Ball:
    return ICON_FA_CIRCLE_DOT;
  case cam::ToolType::VBit:
    return ICON_FA_CARET_DOWN;
  case cam::ToolType::Drill:
    return ICON_FA_ARROW_DOWN_LONG;
  case cam::ToolType::ThreadMill:
    return ICON_FA_SCREWDRIVER;
  }
  return ICON_FA_SQUARE_FULL;
}

const char* CamToolShapeName(cam::ToolType type) {
  switch (type) {
  case cam::ToolType::Flat:
    return "flat";
  case cam::ToolType::Ball:
    return "ball";
  case cam::ToolType::VBit:
    return "V-bit";
  case cam::ToolType::Drill:
    return "drill";
  case cam::ToolType::ThreadMill:
    return "thread mill";
  }
  return "flat";
}

// "6 mm ball", the row's tail.
std::string CamToolTail(const cam::Tool& t) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.3g mm %s", t.diameter,
                CamToolShapeName(t.type));
  return buf;
}

// Everything else about it, for the row's tooltip.
std::string CamToolSummary(const cam::Tool& t) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s, %.1f mm of flute, F%.0f mm/min",
                CamToolTail(t).c_str(), t.fluteLength, t.feedCut);
  std::string out = buf;
  if (t.overallLength > 0.0 && t.collarDiameter > 0.0) {
    std::snprintf(buf, sizeof(buf), "; %.1f mm out of a %.1f mm collar",
                  t.overallLength, t.collarDiameter);
    out += buf;
  }
  return out;
}

std::string JoinNames(const std::vector<std::string>& names) {
  std::string out;
  for (size_t i = 0; i < names.size(); i++)
    out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
  return out;
}

// A row's disclosure id: a setup over its operations and fixtures. The row
// is drawn under PushID("setup<id>"), so opening it from outside the walk
// pushes the same scope (OpenSetupRow).
std::string SetupOpenId(uint64_t id) {
  return "###cam_setup_open_" + std::to_string(id);
}
std::string SetupScope(uint64_t id) { return "setup" + std::to_string(id); }
void OpenSetupRow(uint64_t id) {
  ImGui::PushID(SetupScope(id).c_str());
  SetListRowOpen(SetupOpenId(id), true);
  ImGui::PopID();
}

// The setup being renamed in place (the row becomes a text field, as a
// feature's does), and its buffer.
struct SetupRename {
  uint64_t setup = 0;
  char buf[128] = {};
  bool focus = false;
};
SetupRename g_rename;

// How the active setup's run stands, as the glyph at the end of its row --
// what the one status line under the operations used to say. A click opens
// the Generate window as the line did.
struct RunStatus {
  const char* glyph = nullptr;
  ImVec4 color{0, 0, 0, 0}; // alpha 0 = dim
  std::string text;
};

RunStatus RunStatusOf(AppState& state) {
  int warns = 0, bads = 0;
  std::string minutes;
  for (const cam::VerdictLine& line : state.camVerdict) {
    if (line.level == cam::VerdictLine::Level::Bad)
      bads++;
    else if (line.level == cam::VerdictLine::Level::Warn)
      warns++;
    else if (line.text.find("min of cutting") != std::string::npos)
      minutes = line.text.substr(0, line.text.find(" of cutting"));
  }
  bool cutNothing = false;
  for (const cam::Toolpath& tp : state.camToolpaths)
    cutNothing = cutNothing || !tp.error.empty();
  const bool busy = CamPassProgressOf(state).running ||
                    BackgroundJobRunning(state, "cam-verify");
  RunStatus out;
  if (busy) {
    out.glyph = ICON_FA_HOURGLASS_HALF;
    out.text = "Generating...";
    return out;
  }
  if (state.camToolpaths.empty()) {
    out.glyph = CommandIcon("job.generate");
    out.text = "Not generated";
    return out;
  }
  if (state.camVerdict.empty()) {
    out.glyph = cutNothing ? ICON_FA_CIRCLE_XMARK : ICON_FA_CIRCLE_QUESTION;
    out.color = cutNothing ? kRed : ImVec4(0, 0, 0, 0);
    out.text = cutNothing ? "Generated; an operation cut nothing"
                          : "Generated, not verified";
    return out;
  }
  out.text = "Generated";
  if (!minutes.empty())
    out.text += " · " + minutes;
  if (bads > 0)
    out.text +=
        " · " + std::to_string(bads) + " problem" + (bads == 1 ? "" : "s");
  if (warns > 0)
    out.text +=
        " · " + std::to_string(warns) + " warning" + (warns == 1 ? "" : "s");
  if (bads > 0 || cutNothing) {
    out.glyph = ICON_FA_CIRCLE_XMARK;
    out.color = kRed;
  } else if (warns > 0) {
    out.glyph = ICON_FA_TRIANGLE_EXCLAMATION;
    out.color = kAmber;
  } else {
    out.glyph = ICON_FA_CIRCLE_CHECK;
    out.color = kGreen;
  }
  return out;
}

// The part slots of the project a job could cut.
std::vector<const PartSlot*> ProjectParts(const AppState& state) {
  std::vector<const PartSlot*> out;
  for (const PartSlot& slot : state.parts)
    if (slot.kind == DocKind::Part)
      out.push_back(&slot);
  return out;
}

bool JobHolds(const cam::JobDocument& job, uint64_t sourceId,
              uint64_t configuration) {
  for (const cam::JobPart& part : job.GetParts())
    if (part.sourceDocId == sourceId && part.configuration == configuration)
      return true;
  return false;
}

bool JobCuts(const cam::JobDocument& job, uint64_t sourceId) {
  for (const cam::JobPart& part : job.GetParts())
    if (part.sourceDocId == sourceId)
      return true;
  return false;
}

// The name a job part's configuration goes by, "" for the source's active one.
std::string ConfigurationName(AppState& state, const cam::JobPart& part) {
  if (part.configuration == 0)
    return "";
  if (const Document* src = JobSourceDocument(state, part.sourceDocId))
    if (const Configuration* c = src->FindConfiguration(part.configuration))
      return c->name;
  return "configuration " + std::to_string(part.configuration);
}

// Adds a part in a configuration: one job part, which the caller's undo
// guard records.
void AddJobPart(AppState& state, const PartSlot& slot, uint64_t configuration) {
  const uint64_t id = state.job->AddPart(slot.id, slot.name);
  if (configuration != 0)
    state.job->FindPart(id)->configuration = configuration;
  state.job->BumpVersion();
}

// The Parts header's "+": each project part; one with configurations is a
// submenu of them, since a job may cut one part in several (the chess
// undersides: a blank per rank, one part). A combination the job already
// holds is not offered twice. True when something was added.
bool DrawPartAddMenu(AppState& state) {
  bool added = false;
  bool any = false;
  for (const PartSlot* slot : ProjectParts(state)) {
    const Document* src = JobSourceDocument(state, slot->id);
    const std::vector<Configuration> none;
    const std::vector<Configuration>& cfgs =
        src != nullptr ? src->GetConfigurations() : none;
    ImGui::PushID(static_cast<int>(slot->id));
    if (cfgs.empty()) {
      if (!JobCuts(*state.job, slot->id)) {
        any = true;
        if (ImGui::MenuItem((slot->name + "###cam_part_add").c_str())) {
          AddJobPart(state, *slot, 0);
          added = true;
        }
        ImGui::SetItemTooltip("Cut it: place it in the stock and give it "
                              "operations");
      }
    } else {
      bool free = !JobHolds(*state.job, slot->id, 0);
      for (const Configuration& c : cfgs)
        free = free || !JobHolds(*state.job, slot->id, c.id);
      if (free) {
        any = true;
        if (ImGui::BeginMenu((slot->name + "###cam_part_add_cfgs").c_str())) {
          const std::string active =
              src->ActiveConfiguration() != 0 &&
                      src->FindConfiguration(src->ActiveConfiguration())
                  ? src->FindConfiguration(src->ActiveConfiguration())->name
                  : std::string("as modelled");
          if (ImGui::MenuItem(
                  ("Its active one (" + active + ")###cam_part_add_cfg_0")
                      .c_str(),
                  nullptr, false, !JobHolds(*state.job, slot->id, 0))) {
            AddJobPart(state, *slot, 0);
            added = true;
          }
          ImGui::SetItemTooltip("Follows whichever configuration the part "
                                "has active");
          for (const Configuration& c : cfgs) {
            // One that does not build is offered, disabled, with the reason:
            // leaving it out would hide a configuration the part does have.
            std::string bad;
            const bool builds =
                ConfiguredPartDocument(state, slot->id, c.id, &bad) != nullptr;
            const std::string id =
                "###cam_part_add_cfg_" + std::to_string(c.id);
            if (ImGui::MenuItem((c.name + id).c_str(), nullptr, false,
                                builds &&
                                    !JobHolds(*state.job, slot->id, c.id))) {
              AddJobPart(state, *slot, c.id);
              added = true;
            }
            if (!builds &&
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip("%s", bad.c_str());
          }
          ImGui::EndMenu();
        }
      }
    }
    ImGui::PopID();
  }
  if (!any)
    ImGui::TextDisabled("Every part in this project is already in the job");
  return added;
}

// A setup's "Add fixture" submenu: a keep-out box first, then the project
// parts the job does not CUT, each saying what the choice does so cutting a
// part and clamping with it are not confused.
void DrawFixtureAddMenu(AppState& state, cam::Setup& setup,
                        std::optional<cam::Fixture>& add) {
  if (ImGui::MenuItem(ICON_FA_BOX " Keep-out box###cam_fixture_box")) {
    cam::Fixture f;
    f.box = cam::DefaultKeepOutBox(setup, JobBodies(state));
    f.name = "Clamp " + std::to_string(setup.fixtures.size() + 1);
    add = f;
  }
  ImGui::SetItemTooltip("A box the tool must keep out of -- a clamp, this "
                        "big, here. No part needed.");
  bool heading = false;
  for (const PartSlot* slot : ProjectParts(state)) {
    if (JobCuts(*state.job, slot->id))
      continue;
    if (!heading) {
      ImGui::Separator();
      ImGui::TextDisabled("Hold it with a part:");
      heading = true;
    }
    ImGui::PushID(static_cast<int>(slot->id));
    if (ImGui::MenuItem((slot->name + "###cam_fixture_add").c_str())) {
      cam::Fixture f;
      f.sourceDocId = slot->id;
      f.name = slot->name;
      add = f;
    }
    ImGui::SetItemTooltip("Hold the stock with this part: placed against the "
                          "stock, never cut, checked against the tool");
    ImGui::PopID();
  }
}

// Places a new fixture from the stock and its own size -- centred when it is
// wider than the stock (a vise), else against the next free side.
void AddFixture(AppState& state, cam::Setup& setup, cam::Fixture f) {
  const std::vector<Body> model = FixtureModelBodies(state, f);
  if (model.empty())
    return;
  const cam::FixtureSide side =
      cam::DefaultFixtureSide(setup, JobBodies(state), model, f.rotZdeg);
  if (!cam::PlaceFixture(setup, JobBodies(state), model, f.rotZdeg, side,
                         f.pos))
    return;
  f.id = state.job->AllocateId();
  setup.fixtures.push_back(f);
  CamFixturesChanged(state, setup);
}

// A new, empty setup, made the active one.
void NewSetup(AppState& state) {
  cam::Setup& fresh = state.job->AddSetup();
  state.camActiveSetup = fresh.id;
  (void)cam::RecomputeOrigin(fresh, JobBodies(state));
  fresh.safeZ = DeriveJobSafeZ(state, fresh);
  state.camSafeZEdit = fresh.safeZ;
  MarkJobEdited(state);
}

// The other side of the same part. A flip job is two setups on ONE part --
// the alternative people reach for is a rotated COPY of the part, which then
// has to be kept in step with the original by hand
// (docs/cam/setup.md#setups).
void FlipSetup(AppState& state, uint64_t setupId) {
  const cam::Setup* from = state.job->FindSetup(setupId);
  if (from == nullptr)
    return;
  const cam::Setup setup = *from; // AddSetup may reallocate
  cam::Setup& other = state.job->AddSetup(setup.name + " (back)");
  // Carry the MACHINE and the material across -- they did not change when
  // the part was turned over. Operations do not come: they are what the
  // other side is for.
  other.frame = cam::FlipFrame(setup.frame);
  other.stock = setup.stock;
  other.tools = setup.tools;
  other.machine.post = setup.machine.post;
  other.machine.fitArcs = setup.machine.fitArcs;
  other.rapidFeed = setup.rapidFeed;
  other.machine.hasRotaryAxis = setup.machine.hasRotaryAxis;
  other.originAt = setup.originAt;
  other.safeZ = DeriveJobSafeZ(state, other);
  (void)cam::RecomputeOrigin(other, JobBodies(state));
  state.camActiveSetup = other.id;
  state.camSafeZEdit = other.safeZ;
  MarkJobEdited(state);
}

// A copy of a setup, operations, fixtures and placements included, each with
// an id of its own: the same clamping for a second board, or a variation to
// try without losing the first.
void DuplicateSetup(AppState& state, uint64_t setupId) {
  const cam::Setup* from = state.job->FindSetup(setupId);
  if (from == nullptr)
    return;
  cam::Setup copy = *from; // AddSetup may reallocate
  cam::Setup& made = state.job->AddSetup(copy.name + " copy");
  const uint64_t id = made.id;
  const std::string name = made.name;
  made = std::move(copy);
  made.id = id;
  made.name = name;
  for (cam::Operation& op : made.operations)
    op.id = state.job->AllocateId();
  for (cam::Fixture& f : made.fixtures)
    f.id = state.job->AllocateId();
  for (cam::Placement& pl : made.placements)
    pl.id = state.job->AllocateId();
  state.camActiveSetup = made.id;
  state.camSafeZEdit = made.safeZ;
  state.job->BumpVersion();
  MarkJobEdited(state);
}

// ONE derivation, two entry points: the same engine call the verbs use
// (docs/cam/operations.md#derivation--what-a-click-means). What follows the
// setup: a job cut on the trunnion gets a rotary pass, not a 3-axis contour
// round the dowel. A refusal is shown rather than an operation that cannot
// cut (the u10 rule). Returns the new operation's id, or 0.
uint64_t AddOperation(AppState& state, cam::Setup& setup) {
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, JobBodies(state), cam::AddOperationType(setup), {}, 0,
      cam::EarlierSetups(state.job->GetSetups(), setup.id));
  if (!derived.ok) {
    state.camRefusal = derived.why;
    return 0;
  }
  cam::Operation op = derived.op;
  op.id = state.job->AllocateId();
  op.name = "Operation " + std::to_string(setup.operations.size() + 1);
  setup.operations.push_back(op);
  state.camRefusal.clear();
  return op.id;
}

// A thread mill for one thread of the job's parts, through the same
// derivation `add_cam_op` uses: its tool is the setup's first thread mill,
// and a refusal -- no thread mill, a cutter too big for the bore -- is shown.
uint64_t AddThreadMill(AppState& state, cam::Setup& setup,
                       const cam::Target& target) {
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, JobBodies(state), cam::OpType::ThreadMill, target, 0,
      cam::EarlierSetups(state.job->GetSetups(), setup.id));
  if (!derived.ok) {
    state.camRefusal = derived.why;
    return 0;
  }
  cam::Operation op = derived.op;
  op.id = state.job->AllocateId();
  setup.operations.push_back(op);
  state.camRefusal.clear();
  return op.id;
}

// The job's structure before it has a setup (round two, M3): a job is made
// the way an assembly is -- from the tab bar, empty -- and this is where it
// becomes a job: the machine, the stock, then parts. The machine goes first
// because everything after it is measured against it.
void DrawSetupStep(AppState& state) {
  ImGui::TextWrapped("A job cuts parts out of a sheet on a machine.");
  ImGui::Spacing();
  AppState::CamNewSetup& ns = state.camNewSetup;
  // Default: the only machine, or the first -- a choice nobody has to make
  // when the shop has one.
  if (cam::FindMachine(state.machines, ns.machine) == nullptr)
    ns.machine = state.machines.empty() ? "" : state.machines.front().name;
  ImGui::TextDisabled("Machine");
  if (state.machines.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextWrapped("No machine yet. Define one first — post, work "
                       "area, rotary — and every job you set up on it "
                       "follows.");
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Preferences › Machines###cam_new_machine_prefs"))
      OpenPreferences(state, "Machines");
  } else {
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("###cam_new_machine", ns.machine.c_str())) {
      for (const cam::Machine& mc : state.machines)
        if (ImGui::Selectable(mc.name.c_str(), mc.name == ns.machine))
          ns.machine = mc.name;
      ImGui::EndCombo();
    }
  }
  ImGui::Spacing();
  ImGui::TextDisabled("Stock");
  if (ImGui::RadioButton("Around the parts###cam_new_stock_relative",
                         !ns.fixedSheet))
    ns.fixedSheet = false;
  ImGui::SetItemTooltip("A block wrapped around whatever the job holds, "
                        "with a margin. The size follows the parts.");
  if (ImGui::RadioButton("A sheet you own###cam_new_stock_fixed",
                         ns.fixedSheet))
    ns.fixedSheet = true;
  ImGui::SetItemTooltip("The board on the bed. Its size is yours to type; "
                        "the thickness comes from the parts.");
  const std::vector<Body>& bodies = JobBodies(state);
  if (ns.fixedSheet) {
    // Derived, not literal (docs/ui/tool-sessions.md#defaults-and-refusals):
    // the sheet defaults to the machine's bed in X and Y, and to the parts'
    // height in Z -- a sheet is as thick as what it is cut into. With no
    // parts there is no thickness to derive, so the fields wait.
    const cam::Machine* mc = cam::FindMachine(state.machines, ns.machine);
    if (ns.sheet.x <= 0.0 && mc != nullptr && mc->HasWorkArea()) {
      ns.sheet.x = mc->travelMax.x - mc->travelMin.x;
      ns.sheet.y = mc->travelMax.y - mc->travelMin.y;
    }
    if (ns.sheet.z <= 0.0 && !bodies.empty()) {
      glm::dvec3 lo(DBL_MAX), hi(-DBL_MAX);
      for (const Body& body : bodies) {
        glm::dvec3 bmin, bmax;
        body.shape.GetBoundingBox(bmin, bmax);
        lo = glm::min(lo, bmin);
        hi = glm::max(hi, bmax);
      }
      ns.sheet.z = hi.z - lo.z;
    }
    ImGui::SetNextItemWidth(200.0f);
    ExprDragFloat3(state, "mm###cam_new_sheet", &ns.sheet.x, 1.0f);
    if (bodies.empty())
      ImGui::TextDisabled("(thickness follows the parts — add one below)");
  }
  ImGui::Spacing();
  // The step CHANGES the document or REFUSES visibly (u10): no machine is a
  // refusal here, said above, and the button does nothing else.
  const bool canCreate =
      !state.machines.empty() && (!ns.fixedSheet || ns.sheet.x > 0.0);
  ImGui::BeginDisabled(!canCreate);
  if (ImGui::Button("Create setup###cam_add_setup")) {
    cam::Setup& fresh = state.job->AddSetup();
    if (const cam::Machine* mc = cam::FindMachine(state.machines, ns.machine))
      ApplyMachine(fresh, *mc);
    if (ns.fixedSheet) {
      fresh.stock.mode = cam::StockMode::Fixed;
      fresh.stock.size = {ns.sheet.x, ns.sheet.y, ns.sheet.z};
    }
    state.camActiveSetup = fresh.id;
    (void)cam::RecomputeOrigin(fresh, bodies);
    // Safe Z clear of the stock, not the struct's bare 5 mm -- which sits
    // inside anything taller than that (docs/cam/setup.md#safe-z).
    fresh.safeZ = cam::DeriveSafeZ(fresh, bodies);
    state.camSafeZEdit = fresh.safeZ;
    // CAM edits live in the document but bump no document version (the same
    // as study edits), so this is what keeps the save prompt honest.
    MarkJobEdited(state);
    state.camNewSetup = {};
  }
  ImGui::EndDisabled();
  if (!canCreate && !state.machines.empty())
    ImGui::TextDisabled("a sheet needs a size");
  ImGui::Spacing();

  // Parts can come in before the setup: the sheet's thickness is derived from
  // them, and a job with nothing in it has nothing to cut.
  ImGui::TextDisabled("Parts");
  for (const cam::JobPart& part : state.job->GetParts())
    ImGui::BulletText("%s", part.name.c_str());
  for (const PartSlot* slot : ProjectParts(state)) {
    if (JobCuts(*state.job, slot->id))
      continue;
    ImGui::PushID(static_cast<int>(slot->id));
    if (ImGui::SmallButton(
            ("Add " + slot->name + "###cam_new_part_add").c_str())) {
      state.job->AddPart(slot->id, slot->name);
      state.job->BumpVersion();
      MarkJobEdited(state);
    }
    ImGui::PopID();
  }
}

// The part rows that are selected, selected the way a click in the viewport
// selects a part: its bodies, which gives it the placement gizmo
// (docs/cam/gui.md#gizmos).
void SelectJobPartBodies(AppState& state) {
  const std::vector<uint64_t> parts =
      SelectedIds(state, SelectionItem::Kind::JobPart);
  auto& items = state.selection.items;
  std::erase_if(items, [](const SelectionItem& it) {
    return it.kind == SelectionItem::Kind::Body ||
           it.kind == SelectionItem::Kind::Face ||
           it.kind == SelectionItem::Kind::Edge;
  });
  for (const uint64_t jobPartId : parts)
    for (const Body& b : JobPlacedBodies(state))
      if (b.jobPartId == jobPartId && !b.reference) {
        SelectionItem item;
        item.kind = SelectionItem::Kind::Body;
        item.id = b.createdBy;
        item.jobPartId = jobPartId;
        items.push_back(item);
        break;
      }
}

bool JobPartSelected(const AppState& state, uint64_t jobPartId) {
  if (IsSelected(state, SelectionItem::Kind::JobPart, jobPartId))
    return true;
  for (const SelectionItem& it : state.selection.items)
    if (it.jobPartId == jobPartId && it.kind == SelectionItem::Kind::Body)
      return true;
  return false;
}

// Deletes what is selected of one of the panel's row kinds -- a row menu's
// Delete, which names the kind it removes and so removes only that kind.
// The Delete KEY takes every kind at once (DeleteSelection).
void DeleteSelectedOf(AppState& state, SelectionItem::Kind kind) {
  const std::vector<uint64_t> ids = SelectedIds(state, kind);
  switch (kind) {
  case SelectionItem::Kind::CamSetup:
    RemoveCamSetups(state, ids);
    break;
  case SelectionItem::Kind::JobPart:
    RemoveJobParts(state, ids);
    break;
  case SelectionItem::Kind::Fixture:
    RemoveCamFixtures(state, ids);
    break;
  case SelectionItem::Kind::CamTool: {
    std::string why;
    RemoveCamTools(state, ids, &why);
    if (!why.empty())
      state.camRefusal = why;
    break;
  }
  default:
    break;
  }
}

// What a placement fit says about the part, or null when it sits well.
const char* FitProblem(const cam::PlacementFit& f) {
  return f.fit == cam::Fit::FullyOutside    ? "not on the stock"
         : f.fit == cam::Fit::PartlyOutside ? "hangs over the edge"
         : !f.overlaps.empty()              ? "on top of another part"
                                            : nullptr;
}

cam::Placement* PlacementOf(cam::Setup& setup, uint64_t jobPartId) {
  for (cam::Placement& pl : setup.placements)
    if (pl.jobPartId == jobPartId)
      return &pl;
  return nullptr;
}

// The configuration a job part is cut in, as a submenu or as radio buttons in
// its editor: the source's active one, then each of its own. True when it
// changed. `idPrefix` keeps the two places' ids apart.
bool ConfigurationChoices(AppState& state, const cam::JobPart& part,
                          bool asMenu, const char* idPrefix) {
  const Document* src = JobSourceDocument(state, part.sourceDocId);
  if (src == nullptr || src->GetConfigurations().empty())
    return false;
  const auto choose = [&](uint64_t cfg) {
    std::string why;
    int moved = 0;
    if (!SetJobPartConfiguration(state, part.id, cfg, &why, &moved)) {
      state.camRefusal = why;
      return false;
    }
    state.camRefusal.clear();
    state.camNote = moved > 0 ? std::to_string(moved) +
                                    (moved == 1 ? " operation's depths"
                                                : " operations' "
                                                  "depths") +
                                    " re-derived for the new configuration"
                              : std::string();
    return true;
  };
  bool changed = false;
  const auto item = [&](const std::string& label, uint64_t cfg,
                        const std::string& why) {
    const std::string id = std::string(idPrefix) + std::to_string(cfg);
    const bool on = part.configuration == cfg;
    const bool builds = why.empty();
    bool pressed = false;
    if (asMenu)
      pressed = ImGui::MenuItem((label + id).c_str(), nullptr, on, builds);
    else {
      ImGui::BeginDisabled(!builds);
      pressed = ImGui::RadioButton((label + id).c_str(), on);
      ImGui::EndDisabled();
    }
    if (!builds && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", why.c_str());
    if (pressed && !on)
      changed = choose(cfg) || changed;
  };
  const Configuration* active =
      src->ActiveConfiguration() != 0
          ? src->FindConfiguration(src->ActiveConfiguration())
          : nullptr;
  item("Its active one (" +
           (active != nullptr ? active->name : std::string("as modelled")) +
           ")",
       0, "");
  for (const Configuration& c : src->GetConfigurations()) {
    std::string bad;
    if (ConfiguredPartDocument(state, part.sourceDocId, c.id, &bad) != nullptr)
      bad.clear();
    else if (bad.empty())
      bad = "does not build in this configuration";
    item(c.name, c.id, bad);
  }
  return changed;
}

} // namespace

void DrawCamPanel(AppState& state) {
  // NoFocusOnAppearing, like every other panel tabbed into the left node: a
  // window that appears in a shared dock node becomes the SELECTED tab, and
  // this one shipped without the flag -- so the first launch after CAM landed
  // put the job list in front of the model tree.
  // A JOB tab's panel, and no other's (v73).
  if (!state.job)
    return;
  // The run status below is the setup on screen's.
  SyncCamResultOwner(state);
  // Every edit this panel makes lands on the undo stack through this one
  // guard (R7): a drag is one step, a click is one step.
  JobUndoGuard undoGuard(state, /*coalesce=*/true);
  if (!ImGui::Begin(kPanelCam, nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return;
  }
  PanelHeading(kPanelCam);
  // Which rows are open is ImGui's, stored in this window by setup id -- and
  // every job counts its ids from the same start, so a job's "Back" setup
  // came up open because ANOTHER job's setup of that id had been, in an
  // earlier project or on the other job tab (cam/two_setups_are_open_at_once
  // after cam/delete_reaches_every_row_kind). What is open is per job.
  {
    static uint64_t shownGeneration = ~uint64_t{0};
    static uint64_t shownJob = 0;
    const uint64_t jobSlot = state.activePart < state.parts.size()
                                 ? state.parts[state.activePart].id
                                 : 0;
    if (shownGeneration != state.projectGeneration || shownJob != jobSlot) {
      ImGui::GetStateStorage()->Clear();
      shownGeneration = state.projectGeneration;
      shownJob = jobSlot;
    }
  }

  std::vector<cam::Setup>& setups = state.job->GetSetups();
  if (setups.empty()) {
    DrawSetupStep(state);
    ImGui::End();
    return;
  }

  cam::Setup* active = ActiveCamSetup(state);
  // Something changed that the active setup's toolpaths read: they are
  // regenerated once, after the tree is drawn. `dirty` is an EDIT to the job
  // (one undo step); `switched` only another setup made the active one,
  // which reads the job and changes none of it.
  bool dirty = false;
  bool switched = false;
  // Makes `s` the active setup: the one the toolpaths, the verbs and the
  // editors act on.
  const auto activate = [&](const cam::Setup& s) {
    if (state.camActiveSetup == s.id)
      return;
    state.camActiveSetup = s.id;
    switched = true;
    // The tool rows are the active setup's: a T3 selected in the last one is
    // not this one's T3.
    std::erase_if(state.selection.items, [](const SelectionItem& it) {
      return it.kind == SelectionItem::Kind::CamTool;
    });
  };

  // --- Parts -------------------------------------------------------------
  // What this job cuts. A job holds more than one part (v74) -- four
  // brackets out of one sheet, or one piece part in a configuration per rank.
  bool addPart = false;
  const bool partsOpen = SectionHeader(
      "Parts###cam_parts", static_cast<int>(state.job->GetParts().size()), true,
      "###cam_parts_add", &addPart, "Add a part of this project to the job");
  // One name per popup, opened by its header's "+" and drawn by BeginPopup.
  constexpr const char* kPartAddPopup = "###cam_part_add_popup";
  if (addPart)
    ImGui::OpenPopup(kPartAddPopup);
  if (ImGui::BeginPopup(kPartAddPopup)) {
    if (DrawPartAddMenu(state))
      dirty = true;
    ImGui::EndPopup();
  }
  if (partsOpen) {
    if (state.job->GetParts().empty())
      TextDisabledWrapped("Nothing to cut yet -- + adds a part.");
    const std::vector<cam::PlacementFit> fits =
        active != nullptr ? cam::CheckPlacements(*active, JobBodies(state))
                          : std::vector<cam::PlacementFit>{};
    bool removeParts = false; // a row menu's Remove (the selected parts)
    std::vector<uint64_t> partOrder;
    for (const cam::JobPart& part : state.job->GetParts())
      partOrder.push_back(part.id);
    ImGui::PushID("parts"); // PushID(int) below shares an id space
    for (const cam::JobPart& part : state.job->GetParts()) {
      const std::string pid = std::to_string(part.id);
      std::string why;
      const bool missing = JobPartDocument(state, part, &why) == nullptr;
      const std::string cfgName = ConfigurationName(state, part);
      cam::Placement* place =
          active != nullptr ? PlacementOf(*active, part.id) : nullptr;
      const char* problem = nullptr;
      if (place != nullptr)
        for (const cam::PlacementFit& f : fits)
          if (f.placementId == place->id)
            problem = FitProblem(f);

      ListRowSpec row;
      row.glyph = missing || problem != nullptr ? ICON_FA_TRIANGLE_EXCLAMATION
                                                : ICON_FA_CUBE;
      if (missing || problem != nullptr)
        row.glyphColor = kRed;
      row.label = part.name + "###cam_part_" + pid;
      row.selected = JobPartSelected(state, part.id);
      // Said here rather than left to a toolpath that comes back empty: a job
      // cannot cut what it cannot find -- a part that is gone, or a
      // configuration it no longer has.
      if (missing)
        row.labelColor = kRed;
      row.dimmed = !missing && place != nullptr && !place->enabled;
      if (missing) {
        const Document* src = JobSourceDocument(state, part.sourceDocId);
        row.tail = src == nullptr ? "its part is gone"
                   : src->FindConfiguration(part.configuration) == nullptr
                       ? "its configuration is gone"
                       : cfgName + " does not build";
      } else if (problem != nullptr)
        row.tail = problem;
      else if (!cfgName.empty())
        row.tail = cfgName;
      else if (const Document* src = JobPartDocument(state, part))
        for (const Body& b : src->GetBodies())
          if (!b.reference && !b.material.empty()) {
            // Its name ("Oak"), as the Material menu shows it -- not the
            // slug the file stores ("wood_oak").
            row.tail =
                CADMaterials::MaterialLibrary::GetMaterialOrDefault(b.material)
                    .name;
            break;
          }
      row.tooltip =
          (missing ? why + "\n" : std::string()) +
          (cfgName.empty() ? "In its active configuration"
                           : "In its configuration '" + cfgName + "'") +
          (active == nullptr ? std::string()
           : place == nullptr
               ? "\nNot placed in '" + active->name + "': it sits at the origin"
           : !place->enabled
               ? "\nLeft on the sheet in '" + active->name + "', not cut"
               : std::string()) +
          "\nDouble-click to edit its placement";
      row.menuId = "###cam_part_ctx_" + pid;
      row.menu = [&, partId = part.id] {
        ListRowContext(state, SelectionItem::Kind::JobPart, partId,
                       /*ownKindOnly=*/true);
        const cam::JobPart* live = state.job->FindPart(partId);
        if (live == nullptr)
          return;
        if (ImGui::MenuItem("Edit placement###cam_part_edit"))
          EditCamJobPart(state, partId);
        const Document* src = JobSourceDocument(state, live->sourceDocId);
        if (src != nullptr && !src->GetConfigurations().empty() &&
            ImGui::BeginMenu("Configuration###cam_part_cfg")) {
          if (ConfigurationChoices(state, *live, /*asMenu=*/true,
                                   "###cam_part_cfg_"))
            dirty = true;
          ImGui::EndMenu();
        }
        if (active != nullptr) {
          cam::Placement* pl = PlacementOf(*active, partId);
          if (pl == nullptr) {
            if (ImGui::MenuItem("Place###cam_place")) {
              cam::Placement fresh;
              fresh.id = state.job->AllocateId();
              fresh.jobPartId = partId;
              active->placements.push_back(fresh);
              state.job->BumpVersion();
              dirty = true;
            }
            ImGui::SetItemTooltip("Say where this part sits in the stock");
          } else {
            if (ImGui::MenuItem("Cut###cam_place_on", nullptr, pl->enabled)) {
              pl->enabled = !pl->enabled;
              state.job->BumpVersion();
              dirty = true;
            }
            ImGui::SetItemTooltip("Off leaves it on the sheet and does not "
                                  "cut it");
          }
        }
        ImGui::Separator();
        const std::string remove =
            std::string(ICON_FA_TRASH_CAN " ") +
            RowActionLabel(
                "Remove",
                SelectedIds(state, SelectionItem::Kind::JobPart).size(),
                "part") +
            "###cam_part_remove";
        if (ImGui::MenuItem(remove.c_str()))
          removeParts = true;
        ImGui::SetItemTooltip("Take it out of the job: its placements go "
                              "with it, and no operation cuts it any more");
      };
      const ListRowResult res = ListRow(row);
      if (res.clicked) {
        CamRowClicked(state, SelectionItem::Kind::JobPart, part.id, partOrder);
        SelectJobPartBodies(state);
      }
      if (res.doubleClicked)
        EditCamJobPart(state, part.id);
    }
    ImGui::PopID(); // "parts"
    if (removeParts)
      DeleteSelectedOf(state, SelectionItem::Kind::JobPart);
  }

  // --- Setups ------------------------------------------------------------
  // Each a row over its operations and fixtures. A setup was a combo, so a
  // job's structure, setup by setup, was never visible at once.
  enum class SetupAct { None, Flip, Duplicate, AddOp, AddThread, Edit };
  // The thread "Add operation > Mill a thread" picked
  // (docs/cam/gui.md#milling-a-thread).
  cam::Target threadToAdd;
  SetupAct act = SetupAct::None;
  uint64_t actOn = 0;
  std::optional<cam::Fixture> fixtureToAdd;
  uint64_t fixtureSetup = 0;
  bool deleteSelected = false; // an op row's Delete (the selection)
  uint64_t moveOp = 0;         // an op dragged …
  size_t moveTo = 0;           // … and where it was dropped
  uint64_t moveIn = 0;         // … in which setup
  uint64_t toggleOp = 0;       // Enable / Disable
  uint64_t editOp = 0;
  bool removeFixtures = false; // a fixture row's Remove (the selection)
  bool removeSetups = false;   // a setup row's Delete (the selection)
  bool fixturesChanged = false;
  std::vector<uint64_t> setupOrder;
  for (const cam::Setup& s : setups)
    setupOrder.push_back(s.id);
  // A setup row is lit when it is SELECTED; with none selected, the active
  // one is. Lit both ways at once, a Ctrl-clicked setup and the active one
  // looked alike and only one of them was what Delete would take.
  const bool anySetupSelected =
      !SelectedIds(state, SelectionItem::Kind::CamSetup).empty();

  bool addSetup = false;
  const bool setupsOpen =
      SectionHeader("Setups###cam_setups", static_cast<int>(setups.size()),
                    true, "###cam_setups_add", &addSetup,
                    "A new setup: another clamping of the job's parts");
  if (addSetup) {
    NewSetup(state);
    ImGui::End();
    return; // `setups` may have reallocated
  }
  if (setupsOpen) {
    const RunStatus status = RunStatusOf(state);
    static const std::pair<const char*, cam::FixtureSide> kPlaces[] = {
        {"-X side###cam_fixture_to_mx", cam::FixtureSide::MinusX},
        {"+X side###cam_fixture_to_px", cam::FixtureSide::PlusX},
        {"-Y side###cam_fixture_to_my", cam::FixtureSide::MinusY},
        {"+Y side###cam_fixture_to_py", cam::FixtureSide::PlusY},
        {"Centre###cam_fixture_to_centre", cam::FixtureSide::Centre},
        {"Top###cam_fixture_to_top", cam::FixtureSide::Top}};
    for (cam::Setup& s : setups) {
      const std::string sid = std::to_string(s.id);
      const bool isActive = active != nullptr && s.id == active->id;
      ImGui::PushID(SetupScope(s.id).c_str());
      bool open = false;
      if (g_rename.setup == s.id) {
        // The row becomes a text field, as a feature's does on Rename.
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (g_rename.focus) {
          ImGui::SetKeyboardFocusHere();
          g_rename.focus = false;
        }
        const bool entered = ImGui::InputText(
            "###cam_setup_rename_field", g_rename.buf, sizeof(g_rename.buf),
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
          if (g_rename.buf[0] != '\0' && s.name != g_rename.buf) {
            s.name = g_rename.buf;
            MarkJobEdited(state);
          }
          g_rename.setup = 0;
        } else if (ImGui::IsItemDeactivated()) {
          g_rename.setup = 0; // Escape, or focus lost without an edit
        }
      } else {
        ListRowSpec row;
        row.glyph = ICON_FA_LAYER_GROUP;
        row.label = s.name + "###cam_setup_" + sid;
        row.selected =
            anySetupSelected
                ? IsSelected(state, SelectionItem::Kind::CamSetup, s.id)
                : isActive;
        row.disclosureId = SetupOpenId(s.id);
        row.disclosureDefaultOpen = isActive;
        // The run's state is the active setup's: its toolpaths are the ones
        // in hand. A click opens the Generate window, as the status line did.
        if (isActive) {
          row.tailGlyph = status.glyph;
          row.tailGlyphColor = status.color;
          row.tailGlyphId = "###cam_run_status";
          row.tailGlyphTip = status.text + "\nOpen the Generate window: the "
                                           "verdict, the output options and "
                                           "Save G-code";
        } else {
          row.tail = std::to_string(s.operations.size()) +
                     (s.operations.size() == 1 ? " op" : " ops");
        }
        row.tooltip =
            (s.machine.name.empty() ? std::string("The job's own machine")
                                    : "On '" + s.machine.name + "'") +
            (s.fixtures.empty() ? "\nNothing holds the stock -- verify checks "
                                  "no clamps."
                                : std::string()) +
            (isActive ? std::string() : "\nClick to make it the active setup") +
            "\nDouble-click for its settings";
        row.menuId = "###cam_setup_ctx_" + sid;
        row.menu = [&, setupId = s.id] {
          ListRowContext(state, SelectionItem::Kind::CamSetup, setupId,
                         /*ownKindOnly=*/true);
          cam::Setup* live = state.job->FindSetup(setupId);
          if (live == nullptr)
            return;
          if (ImGui::MenuItem("Settings###cam_setup_edit"))
            act = SetupAct::Edit, actOn = setupId;
          if (ImGui::MenuItem("Rename###cam_setup_rename")) {
            g_rename.setup = setupId;
            std::snprintf(g_rename.buf, sizeof(g_rename.buf), "%s",
                          live->name.c_str());
            g_rename.focus = true;
          }
          if (ImGui::MenuItem("Flip###cam_flip_setup"))
            act = SetupAct::Flip, actOn = setupId;
          ImGui::SetItemTooltip("Add a setup for the other side: the same "
                                "part, turned over. Same stock and tools, no "
                                "operations yet.");
          if (ImGui::MenuItem("Duplicate###cam_setup_duplicate"))
            act = SetupAct::Duplicate, actOn = setupId;
          ImGui::SetItemTooltip("A copy of this setup, its operations and "
                                "fixtures included");
          ImGui::Separator();
          if (ImGui::MenuItem("Add operation###cam_add_op"))
            act = SetupAct::AddOp, actOn = setupId;
          ImGui::SetItemTooltip("What this setup calls for, derived: another "
                                "rotary pass on a setup that already turns, "
                                "else the whole part cut out");
          // Every thread on the job's parts, by its designation: a thread
          // mill is added for the one picked, driven by it
          // (docs/cam/operations.md#thread-mill).
          if (ImGui::BeginMenu("Mill a thread###cam_add_thread")) {
            int shown = 0;
            for (const Body& b : JobBodies(state))
              for (const ThreadMark& m : b.threads) {
                // By its place in this list: a face's name has slashes in
                // it, which an item id cannot carry.
                char label[160];
                std::snprintf(
                    label, sizeof(label), "%s%s%s (%s)###cam_thread_%d",
                    m.designation.c_str(), m.threadClass.empty() ? "" : "-",
                    m.threadClass.c_str(), m.external ? "external" : "internal",
                    shown);
                if (ImGui::MenuItem(label)) {
                  act = SetupAct::AddThread;
                  actOn = setupId;
                  threadToAdd = cam::Target{b.jobPartId, m.face};
                }
                shown++;
              }
            if (shown == 0)
              ImGui::TextDisabled("No thread on this job's parts -- give a "
                                  "hole a tapped fit, or add a Thread");
            ImGui::EndMenu();
          }
          ImGui::SetItemTooltip("A thread mill for a thread on the part: its "
                                "size, pitch, hand and class come from the "
                                "Thread, the tool is the setup's thread mill");
          if (ImGui::BeginMenu("Add fixture###cam_fixture_add_menu")) {
            DrawFixtureAddMenu(state, *live, fixtureToAdd);
            if (fixtureToAdd)
              fixtureSetup = setupId;
            ImGui::EndMenu();
          }
          ImGui::SetItemTooltip("What holds the stock. Verify reports every "
                                "move of the cutter or holder into it.");
          ImGui::Separator();
          const std::string del =
              std::string(ICON_FA_TRASH_CAN " ") +
              DeleteLabel(
                  SelectedIds(state, SelectionItem::Kind::CamSetup).size(),
                  "setup") +
              "###cam_del_setup";
          if (ImGui::MenuItem(del.c_str()))
            removeSetups = true;
        };
        const ListRowResult res = ListRow(row);
        open = res.open;
        // The active setup is the one you are looking into: it opens. And
        // what is shown is kept, so a setup that was open because it was
        // active stays open when another one becomes active.
        // A plain click selects it and makes it the active one; Ctrl and
        // Shift only select, for Delete.
        if (res.clicked &&
            CamRowClicked(state, SelectionItem::Kind::CamSetup, s.id,
                          setupOrder) &&
            !isActive) {
          activate(s);
          open = true;
        }
        SetListRowOpen(SetupOpenId(s.id), open);
        if (res.doubleClicked)
          act = SetupAct::Edit, actOn = s.id;
        if (res.tailClicked)
          OpenCamGenerate(state, /*regenerate=*/false);
      }

      // What is in a setup sits under its NAME, past the chevron: indented
      // by one level only, the operations' glyphs lined up with the setup's
      // and the tree read as a flat list.
      const float childIndent = ImGui::GetFontSize() * 2.3f;
      if (open) {
        ImGui::Indent(childIndent);
        // The operations, in the feature list's style (CamOperationRow): the
        // order is the cutting sequence, dragged within the setup.
        std::vector<uint64_t> order;
        for (const cam::Operation& op : s.operations)
          order.push_back(op.id);
        ImGui::PushID("ops");
        for (size_t i = 0; i < s.operations.size(); i++) {
          cam::Operation& op = s.operations[i];
          ImGui::PushID(static_cast<int>(op.id));
          const CamOpRowResult r = CamOperationRow(state, s, op, order);
          if (r.clicked)
            activate(s); // an op is edited in its own setup
          if (r.deleteSelection)
            deleteSelected = true;
          if (r.toggleEnabled)
            toggleOp = op.id, actOn = s.id;
          if (r.edit)
            editOp = op.id, actOn = s.id;
          if (r.droppedHere != 0) {
            moveOp = r.droppedHere;
            moveTo = i;
            moveIn = s.id;
          }
          ImGui::PopID();
        }
        ImGui::PopID(); // "ops"
        if (s.operations.empty() && s.fixtures.empty())
          TextDisabledWrapped("No operations yet -- the toolbar's verbs, or "
                              "right-click the setup.");

        // What HOLDS the stock in this setup: clamps, a vise, a fixture
        // plate -- a part, or a keep-out box with no part at all
        // (docs/cam/setup.md#workholding-fixtures). The numbers are in the
        // tool window, the verbs in the row's menu.
        ImGui::PushID("fixtures");
        std::vector<uint64_t> fixtureOrder;
        for (const cam::Fixture& f : s.fixtures)
          fixtureOrder.push_back(f.id);
        for (cam::Fixture& f : s.fixtures) {
          const std::vector<Body> model = FixtureModelBodies(state, f);
          const bool missing = model.empty();
          ListRowSpec row;
          row.glyph = f.IsBox() ? ICON_FA_BOX : ICON_FA_LOCK;
          row.label = f.name + "###cam_fixture_" + std::to_string(f.id);
          row.selected = state.camEditFixture == f.id ||
                         IsSelected(state, SelectionItem::Kind::Fixture, f.id);
          row.dimmed = !f.enabled || missing;
          // A seated one is placed by the stock, and says which edge -- or
          // why it no longer can (docs/cam/setup.md#seats).
          const auto seatGone = state.camSeatRefusals.find(f.id);
          const std::string place =
              missing ? std::string("its part is gone")
              : f.seat && seatGone != state.camSeatRefusals.end()
                  ? "its seat is gone: " + seatGone->second
              : f.seat ? "over the " +
                             [&] {
                               std::string e = cam::StockEdgeName(f.seat->edge);
                               e[1] = static_cast<char>(std::toupper(e[1]));
                               return e;
                             }() +
                             " edge"
                       : cam::FixturePlaceName(s, JobBodies(state), model, f);
          row.tail = !place.empty() ? place : std::string("placed");
          if (!f.enabled && !missing)
            row.tail += ", not checked";
          row.tooltip =
              (f.IsBox() ? std::string("A keep-out box. ") : std::string()) +
              (missing ? "Its part is gone, so nothing is checked against it."
               : f.enabled
                   ? "Verify reports every move of the cutter or holder "
                     "into it."
                   : "Drawn, not checked.");
          row.menuId = "###cam_fixture_ctx_" + std::to_string(f.id);
          row.menu = [&] {
            activate(s);
            ListRowContext(state, SelectionItem::Kind::Fixture, f.id,
                           /*ownKindOnly=*/true);
            if (ImGui::MenuItem("Edit###cam_fixture_edit"))
              EditCamFixture(state, f.id);
            if (ImGui::MenuItem("Check###cam_fixture_check", nullptr,
                                f.enabled)) {
              f.enabled = !f.enabled;
              fixturesChanged = true;
            }
            if (ImGui::BeginMenu("Move to###cam_fixture_move")) {
              for (const auto& [label, side] : kPlaces)
                if (ImGui::MenuItem(label) &&
                    cam::PlaceFixture(s, JobBodies(state), model, f.rotZdeg,
                                      side, f.pos))
                  fixturesChanged = true;
              ImGui::EndMenu();
            }
            ImGui::Separator();
            const std::string remove =
                std::string(ICON_FA_TRASH_CAN " ") +
                RowActionLabel(
                    "Remove",
                    SelectedIds(state, SelectionItem::Kind::Fixture).size(),
                    "fixture") +
                "###cam_fixture_remove";
            if (ImGui::MenuItem(remove.c_str()))
              removeFixtures = true;
          };
          const ListRowResult res = ListRow(row);
          if (res.clicked && CamRowClicked(state, SelectionItem::Kind::Fixture,
                                           f.id, fixtureOrder))
            activate(s);
          if (res.doubleClicked) {
            activate(s);
            EditCamFixture(state, f.id);
          }
        }
        ImGui::PopID(); // "fixtures"
        ImGui::Unindent(childIndent);
      }
      ImGui::PopID(); // "setup<id>"
    }
  }

  // Applied after the walk, which the vector they change was.
  if (cam::Setup* s = active != nullptr
                          ? state.job->FindSetup(state.camActiveSetup)
                          : nullptr;
      s != nullptr && fixturesChanged)
    CamFixturesChanged(state, *s);
  if (removeFixtures)
    DeleteSelectedOf(state, SelectionItem::Kind::Fixture);
  if (fixtureToAdd)
    if (cam::Setup* s = state.job->FindSetup(fixtureSetup)) {
      state.camActiveSetup = s->id;
      AddFixture(state, *s, *fixtureToAdd);
    }
  if (toggleOp != 0)
    if (cam::Setup* s = state.job->FindSetup(actOn))
      for (cam::Operation& op : s->operations)
        if (op.id == toggleOp) {
          op.enabled = !op.enabled;
          state.camActiveSetup = s->id;
          dirty = true;
        }
  if (editOp != 0) {
    if (state.camActiveSetup != actOn) {
      state.camActiveSetup = actOn;
      RefreshCamToolpaths(state);
    }
    EditCamOperation(state, editOp);
  }
  if (moveOp != 0)
    if (cam::Setup* s = state.job->FindSetup(moveIn);
        s != nullptr && cam::MoveOperation(*s, moveOp, moveTo)) {
      // Reordering changes no toolpath -- only the sequence they are cut in
      // and where the tool changes fall -- so nothing is regenerated. The
      // posted file does change, which is what the dirty flag is for.
      MarkJobEdited(state);
      state.camVerdict.clear();
      state.camLeftover.clear();
    }
  if (deleteSelected)
    DeleteSelection(state);
  switch (act) {
  case SetupAct::None:
    break;
  case SetupAct::Edit:
    EditCamSetup(state, actOn);
    break;
  case SetupAct::Flip:
    FlipSetup(state, actOn);
    dirty = true;
    break;
  case SetupAct::Duplicate:
    DuplicateSetup(state, actOn);
    dirty = true;
    break;
  case SetupAct::AddThread:
  case SetupAct::AddOp:
    if (cam::Setup* s = state.job->FindSetup(actOn)) {
      if (state.camActiveSetup != s->id) {
        state.camActiveSetup = s->id;
        switched = true;
      }
      const uint64_t made = act == SetupAct::AddThread
                                ? AddThreadMill(state, *s, threadToAdd)
                                : AddOperation(state, *s);
      if (made != 0) {
        OpenSetupRow(s->id);
        MarkJobEdited(state);
        if (!dirty && !switched) {
          RefreshCamToolpath(state, made);
          state.camVerdict.clear();
          state.camLeftover.clear();
        }
      }
    }
    break;
  }
  if (removeSetups)
    DeleteSelectedOf(state, SelectionItem::Kind::CamSetup);
  // `active` may be stale past this point: a setup was added or removed.
  active = ActiveCamSetup(state);

  // --- Tools -------------------------------------------------------------
  // The ACTIVE setup's cutters, one row each (ui::ListRow): the fields are
  // edited in the tool window (double-click, or Edit), and Delete is in the
  // row's menu -- refused while an operation cuts with the tool.
  if (active != nullptr) {
    bool addTool = false;
    const bool toolsOpen = SectionHeader(
        "Tools###cam_tools", static_cast<int>(active->tools.size()), true,
        "###cam_tools_add", &addTool,
        "Add a cutter: the machine's library first, or a blank one");
    constexpr const char* kToolAddPopup = "###cam_tool_add_popup";
    if (addTool)
      ImGui::OpenPopup(kToolAddPopup);
    const auto nextNumber = [&]() {
      int n = 0;
      for (const cam::Tool& t : active->tools)
        n = std::max(n, t.number);
      return n + 1;
    };
    if (ImGui::BeginPopup(kToolAddPopup)) {
      // The machine's library first (M4): the tools the shop owns, minus the
      // ones this setup already has. Copied in, never linked, so a library
      // edit later changes nothing already cut with.
      for (const cam::Tool& lib : active->machine.library) {
        bool held = false;
        for (const cam::Tool& t : active->tools)
          held = held || (t.number == lib.number &&
                          t.diameter == lib.diameter && t.type == lib.type);
        if (held)
          continue;
        ImGui::PushID(lib.number);
        char label[96];
        std::snprintf(label, sizeof(label), "T%d %.3g mm %s###cam_add_lib_tool",
                      lib.number, lib.diameter, lib.name.c_str());
        if (ImGui::MenuItem(label)) {
          cam::Tool t = lib;
          // Its own number stays unless the setup already uses it.
          for (const cam::Tool& have : active->tools)
            if (have.number == t.number)
              t.number = nextNumber();
          active->tools.push_back(t);
          dirty = true;
        }
        ImGui::SetItemTooltip("From the machine's library");
        ImGui::PopID();
      }
      if (!active->machine.library.empty())
        ImGui::Separator();
      if (ImGui::MenuItem("Blank tool###cam_add_tool")) {
        cam::Tool t;
        t.number = nextNumber();
        t.name = "tool " + std::to_string(t.number);
        active->tools.push_back(t);
        dirty = true;
      }
      ImGui::SetItemTooltip("A tool of your own: its size, feeds and speeds "
                            "are yours to type");
      ImGui::EndPopup();
    }
    if (toolsOpen) {
      // Namespaced: a tool number and an operation id are both small ints,
      // and PushID(int) hashes them into the same id space.
      ImGui::PushID("tools");
      bool deleteTools = false;
      std::vector<uint64_t> toolOrder;
      for (const cam::Tool& t : active->tools)
        toolOrder.push_back(static_cast<uint64_t>(t.number));
      for (const cam::Tool& t : active->tools) {
        const std::vector<std::string> users =
            CamOpsUsingTool(*active, t.number);
        ListRowSpec row;
        row.glyph = CamToolGlyph(t.type);
        row.label = "T" + std::to_string(t.number) + "  " + t.name +
                    "###cam_tool_" + std::to_string(t.number);
        row.selected = state.camEditTool == t.number ||
                       IsSelected(state, SelectionItem::Kind::CamTool,
                                  static_cast<uint64_t>(t.number));
        row.tail = CamToolTail(t);
        row.tooltip = CamToolSummary(t) +
                      (users.empty() ? std::string("\nNo operation uses it.")
                                     : "\nUsed by " + JoinNames(users) + ".");
        row.menuId = "###cam_tool_ctx_" + std::to_string(t.number);
        row.menu = [&state, &t, &users, &deleteTools] {
          ListRowContext(state, SelectionItem::Kind::CamTool,
                         static_cast<uint64_t>(t.number),
                         /*ownKindOnly=*/true);
          if (ImGui::MenuItem("Edit###cam_tool_edit"))
            EditCamTool(state, t.number);
          ImGui::Separator();
          const size_t n =
              SelectedIds(state, SelectionItem::Kind::CamTool).size();
          const std::string del = std::string(ICON_FA_TRASH_CAN " ") +
                                  DeleteLabel(n, "tool") + "###cam_tool_delete";
          // One tool still cut with is disabled here, with the reason; of
          // several, the free ones go and the rest are refused in words.
          if (ImGui::MenuItem(del.c_str(), nullptr, false,
                              users.empty() || n > 1))
            deleteTools = true;
          if (!users.empty() &&
              ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Used by %s. Give those operations another "
                              "tool first.",
                              JoinNames(users).c_str());
        };
        const ListRowResult res = ListRow(row);
        if (res.clicked)
          CamRowClicked(state, SelectionItem::Kind::CamTool,
                        static_cast<uint64_t>(t.number), toolOrder);
        if (res.doubleClicked)
          EditCamTool(state, t.number);
      }
      if (active->tools.empty())
        TextDisabledWrapped("No cutters -- + adds one.");
      ImGui::PopID(); // "tools"
      if (deleteTools)
        DeleteSelectedOf(state, SelectionItem::Kind::CamTool);
    }
  }

  // Why Simulate (on the job toolbar) would not start, and why the last
  // panel action was refused: said under the tree, where the click was.
  if (!state.camPlaybackOpen && !state.camPlaybackRefusal.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextWrapped("%s", state.camPlaybackRefusal.c_str());
    ImGui::PopStyleColor();
  }
  if (!state.camRefusal.empty() && !state.camToolOpen) {
    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    ImGui::TextWrapped("%s", state.camRefusal.c_str());
    ImGui::PopStyleColor();
  }

  if ((dirty || switched) && active != nullptr) {
    // The stock box may have moved, and the work origin is derived from it --
    // so is every coordinate in the posted file. A setup made active has its
    // origin derived again too, and when that moves it, it is an edit.
    const glm::dvec3 origin = active->origin;
    (void)cam::RecomputeOrigin(*active, JobBodies(state));
    if (dirty || active->origin != origin)
      MarkJobEdited(state);
    state.camSafeZEdit = active->safeZ;
    // A switch alone keeps the last setup's verdict WITH that setup and
    // brings this one's back (RefreshCamToolpaths -> SyncCamResultOwner).
    RefreshCamToolpaths(state);
    // An edit: a verdict about the PREVIOUS toolpath is worse than none.
    if (dirty) {
      state.camVerdict.clear();
      state.camLeftover.clear();
    }
  }
  ImGui::End();
}

bool DrawCamSetupEditor(AppState& state) {
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr || setup->id != state.camEditSetup) {
    state.camEditSetup = 0; // deleted, or no longer the active setup
    return false;
  }
  // The panel's forms, moved rather than rewritten: they were undoable there
  // through its guard, and are here through this one.
  JobUndoGuard undoGuard(state, /*coalesce=*/true);
  BeginToolWindowFrame(state, "Setup: " + setup->name);
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
  bool dirty = false;

  char name[128];
  std::snprintf(name, sizeof(name), "%s", setup->name.c_str());
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  if (ImGui::InputText("Name###cam_setup_name", name, sizeof(name)) &&
      name[0] != '\0') {
    setup->name = name; // a name changes no toolpath
    MarkJobEdited(state);
  }

  // WHICH MACHINE (round two). The combo lists the store; picking one copies
  // it onto the setup (post, arcs, rotary, work area, and the rapid it seeds).
  ImGui::SeparatorText("Machine");
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
  const char* machineLabel =
      setup->machine.name.empty() ? "(job's own)" : setup->machine.name.c_str();
  if (ImGui::BeginCombo("###cam_machine", machineLabel)) {
    for (const cam::Machine& mc : state.machines)
      if (ImGui::Selectable(mc.name.c_str(), mc.name == setup->machine.name)) {
        ApplyMachine(*setup, mc);
        dirty = true;
      }
    if (state.machines.empty())
      ImGui::TextDisabled("no machines yet — Preferences › Machines");
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("The machine this setup is for. Picking one copies "
                        "its post, arc fitting, rotary axis and work area "
                        "onto the setup; the job keeps that copy, so it "
                        "opens anywhere.");

  // How the setup's copy stands against the store, SAID
  // (docs/cam/setup.md#machine-profiles).
  const cam::MachineMatch match =
      cam::MatchMachine(state.machines, setup->machine);
  switch (match) {
  case cam::MachineMatch::Global:
    break;
  case cam::MachineMatch::Unnamed:
    if (state.machines.empty()) {
      ImGui::TextDisabled("No machine: define one in");
      ImGui::SameLine();
      if (ImGui::SmallButton("Preferences › Machines###cam_machine_prefs"))
        OpenPreferences(state, "Machines");
    } else {
      ImGui::TextDisabled("No machine chosen — the job's own settings");
    }
    break;
  case cam::MachineMatch::Differs:
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextWrapped("Set up on a different '%s' than yours.",
                       setup->machine.name.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Use mine###cam_machine_use_mine")) {
      ApplyMachine(*setup,
                   *cam::FindMachine(state.machines, setup->machine.name));
      dirty = true;
    }
    ImGui::SetItemTooltip("Replace the job's copy with your '%s'",
                          setup->machine.name.c_str());
    ImGui::SameLine();
    break;
  case cam::MachineMatch::Missing:
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextWrapped("'%s' is not one of your machines.",
                       setup->machine.name.c_str());
    ImGui::PopStyleColor();
    break;
  }
  if (match == cam::MachineMatch::Differs ||
      match == cam::MachineMatch::Missing) {
    // ONE call site for both cases (widget_ids_lint): the label says which.
    if (ImGui::SmallButton(match == cam::MachineMatch::Differs
                               ? "Save as mine###cam_machine_save"
                               : "Save it###cam_machine_save")) {
      std::string error;
      if (!SaveMachine(state, setup->machine, &error))
        state.camNote = error;
    }
    ImGui::SetItemTooltip("Write the job's copy to your machines as '%s'",
                          setup->machine.name.c_str());
  }

  // The stock, and the rapid height above it. Both are properties of the
  // setup rather than of an operation, and the facing verb refuses without
  // the first -- so this is where that refusal sends you.
  ImGui::SeparatorText("Stock and safety");
  ImGui::SetNextItemWidth(90.0f);
  if (ExprDragDouble(state, "Safe Z###cam_safez", &state.camSafeZEdit, 0.5f,
                     -DBL_MAX, DBL_MAX, "%.3f"))
    dirty = true;
  ImGui::SetItemTooltip("Rapid height above the stock top");
  double above = setup->stock.margin.z;
  ImGui::SetNextItemWidth(90.0f);
  if (ExprDragDouble(state, "Above the part###cam_stock_above", &above, 0.1f,
                     0.0, 100.0, "%.2f")) {
    setup->stock.margin.z = above;
    dirty = true;
  }
  ImGui::SetItemTooltip("Material sitting on top of the part — what a "
                        "facing pass removes. Picking the top face sets "
                        "this to zero.");
  double around = setup->stock.margin.x;
  ImGui::SetNextItemWidth(90.0f);
  if (ExprDragDouble(state, "Around it###cam_stock_around", &around, 0.1f, 0.0,
                     100.0, "%.2f")) {
    setup->stock.margin.x = around;
    setup->stock.margin.y = around;
    dirty = true;
  }
  // WHAT the workpiece is. Relative -- the model's box plus margins -- is
  // the default; Fixed is the sheet you actually own.
  int mode = setup->stock.mode == cam::StockMode::Fixed ? 1 : 0;
  static const char* kStockModes[] = {"Around the part", "A fixed sheet"};
  ImGui::SetNextItemWidth(140.0f);
  if (ImGui::Combo("###cam_stock_mode", &mode, kStockModes,
                   IM_ARRAYSIZE(kStockModes))) {
    setup->stock.mode =
        mode == 1 ? cam::StockMode::Fixed : cam::StockMode::Relative;
    // Seed the sheet from the box it already had, so switching modes never
    // lands on a stock of zero (the u10 rule).
    if (setup->stock.mode == cam::StockMode::Fixed &&
        setup->stock.size == glm::dvec3(0.0)) {
      glm::dvec3 lo(0.0), hi(0.0);
      if (cam::StockBounds(*setup, JobBodies(state), lo, hi))
        setup->stock.size = hi - lo;
    }
    dirty = true;
  }
  if (setup->stock.mode == cam::StockMode::Fixed) {
    for (int a = 0; a < 3; a++) {
      double v = setup->stock.size[a];
      ImGui::SetNextItemWidth(58.0f);
      const char* ids[3] = {"###stock_sx", "###stock_sy", "###stock_sz"};
      if (a > 0)
        ImGui::SameLine();
      if (ExprDragDouble(state, ids[a], &v, 0.5f, 0.0, 100000.0, "%.1f")) {
        setup->stock.size[a] = v;
        dirty = true;
      }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("mm");
    const std::string refusal = cam::StockRefusal(*setup, JobBodies(state));
    if (!refusal.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kRed);
      ImGui::TextWrapped("%s", refusal.c_str());
      ImGui::PopStyleColor();
    }
  }

  // WHERE ZERO IS -- the corner the operator touches off on, and the point
  // every coordinate in the posted file is measured from.
  int at = static_cast<int>(setup->originAt);
  static const char* kOriginAt[] = {"Stock corner", "Stock centre",
                                    "Part corner"};
  ImGui::SetNextItemWidth(140.0f);
  if (ImGui::Combo("###cam_origin_at", &at, kOriginAt,
                   IM_ARRAYSIZE(kOriginAt))) {
    setup->originAt = static_cast<cam::OriginAt>(at);
    dirty = true;
  }
  ImGui::SetItemTooltip("Where X0 Y0 is in the posted file — the corner you "
                        "touch off on. Z comes from the picked top face.");
  ImGui::SameLine();
  ImGui::TextDisabled("%.2f %.2f %.2f", setup->origin.x, setup->origin.y,
                      setup->origin.z);

  double below = setup->stock.bottomMargin;
  ImGui::SetNextItemWidth(90.0f);
  if (ExprDragDouble(state, "Below it###cam_stock_below", &below, 0.1f, 0.0,
                     100.0, "%.2f")) {
    setup->stock.bottomMargin = below;
    dirty = true;
  }
  ImGui::SetItemTooltip("How far a through-cut goes into the spoilboard");

  // The machine's own fields, edited on the JOB'S COPY: changing one here is
  // what makes the setup differ from the named machine, and the line under
  // the machine combo says so.
  ImGui::SeparatorText("Post");
  ImGui::SetNextItemWidth(110.0f);
  if (ImGui::BeginCombo("Post###cam_post", setup->machine.post.c_str())) {
    for (const std::string& id : cam::PostIds())
      if (ImGui::Selectable(id.c_str(), id == setup->machine.post)) {
        setup->machine.post = id;
        dirty = true;
      }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Which controller dialect this job is posted for");
  if (ImGui::Checkbox("Arcs (G2/G3)###cam_arcs", &setup->machine.fitArcs))
    dirty = true;
  ImGui::SetItemTooltip("Post curves as arcs instead of many short lines. "
                        "Turn off for a controller that mishandles them.");
  // Does this machine HAVE a 4th axis? Until it is ticked no file can carry
  // an A word (docs/cam/setup.md#fixturing-frame-and-rotary-index).
  if (ImGui::Checkbox("Has a 4th axis (A)###cam_has_rotary",
                      &setup->machine.hasRotaryAxis))
    dirty = true;
  ImGui::SetItemTooltip("Tick only for a machine with a rotary table. How "
                        "the part is CLAMPED is \"Lay a face up\" and needs "
                        "no rotary axis.");
  if (setup->machine.hasRotaryAxis) {
    ImGui::SeparatorText("Rotary");
    double indexA = setup->rotationA;
    ImGui::SetNextItemWidth(90.0f);
    if (ExprDragDouble(state, "A index###cam_index_a", &indexA, 0.5f, -360.0,
                       360.0, "%.2f°")) {
      setup->rotationA = indexA;
      dirty = true;
    }
    ImGui::SetItemTooltip("Rotary 4th axis: how far the table is turned for "
                          "this setup. 0 for a 3-axis job.");
    // Where the part is held (docs/cam/rotary.md#chuck-and-tailstock).
    int held = static_cast<int>(setup->heldIn);
    const char* holds[] = {"as its operations say", "on the bed",
                           "in the rotary"};
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::Combo("Part held###cam_held_in", &held, holds, 3)) {
      setup->heldIn = static_cast<cam::Setup::HeldIn>(held);
      dirty = true;
    }
    if (cam::HeldInRotary(*setup)) {
      bool chuck = setup->chuck != 0;
      if (ImGui::Checkbox("In the chuck###cam_chuck", &chuck)) {
        setup->chuck = chuck ? 1 : 0;
        dirty = true;
      }
      ImGui::SetItemTooltip("Off only for a part held some other way the "
                            "model does not show -- nothing then keeps the "
                            "cutter off that end.");
      if (chuck) {
        ImGui::SameLine();
        double grip = setup->gripLength;
        ImGui::SetNextItemWidth(70.0f);
        if (ExprDragDouble(state, "mm in the jaws (0 = machine's)###cam_grip",
                           &grip, 0.5f, 0.0, 200.0, "%.1f")) {
          setup->gripLength = grip;
          dirty = true;
        }
      }
      bool tail =
          setup->tailstock == 1 ||
          (setup->tailstock == -1 && setup->machine.holding.hasTailstock);
      if (ImGui::Checkbox("Tailstock engaged###cam_tail", &tail)) {
        setup->tailstock = tail ? 1 : 0;
        dirty = true;
      }
    }
  }

  ImGui::SeparatorText("Fixtures");
  if (setup->fixtures.empty())
    // Said, not left blank: no fixture means verify is checking the cutter
    // against no workholding at all.
    ImGui::TextDisabled("Nothing holds the stock -- verify checks no clamps. "
                        "Right-click the setup to add a fixture.");
  else {
    std::vector<std::string> names;
    for (const cam::Fixture& f : setup->fixtures)
      names.push_back(f.name + (f.enabled ? "" : " (not checked)"));
    ImGui::TextDisabled("%s. Each is a row under the setup.",
                        JoinNames(names).c_str());
  }

  // The struct holds a double; the widget edits a float mirror so ImGui's
  // drag behaves. Kept in sync here rather than in two places.
  if (dirty) {
    setup->safeZ = state.camSafeZEdit;
    // The stock box may have moved, and the work origin is derived from it --
    // so is every coordinate in the posted file.
    (void)cam::RecomputeOrigin(*setup, JobBodies(state));
    state.job->BumpVersion();
    MarkJobEdited(state);
    RefreshCamToolpaths(state);
    state.camVerdict.clear();
    state.camLeftover.clear();
  } else {
    state.camSafeZEdit = setup->safeZ;
  }
  if (!state.camNote.empty())
    ImGui::TextDisabled("%s", state.camNote.c_str());
  ImGui::PopTextWrapPos();
  ImGui::Separator();
  if (ImGui::Button("Close###cam_setup_editor_close")) {
    state.camEditSetup = 0;
    state.camToolOpen = false;
  }
  ImGui::End();
  return true;
}

bool DrawCamPartEditor(AppState& state) {
  cam::JobPart* part =
      state.job ? state.job->FindPart(state.camEditPart) : nullptr;
  if (part == nullptr) {
    state.camEditPart = 0; // removed
    return false;
  }
  JobUndoGuard undoGuard(state, /*coalesce=*/true);
  BeginToolWindowFrame(state, "Part: " + part->name);
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
  bool dirty = false;
  const uint64_t partId = part->id;

  std::string why;
  if (JobPartDocument(state, *part, &why) == nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    ImGui::TextWrapped("%s", why.c_str());
    ImGui::PopStyleColor();
  }
  if (const Document* src = JobSourceDocument(state, part->sourceDocId);
      src != nullptr && !src->GetConfigurations().empty()) {
    ImGui::SeparatorText("Configuration");
    if (ConfigurationChoices(state, *part, /*asMenu=*/false,
                             "###cam_part_edcfg_"))
      dirty = true;
    part = state.job->FindPart(partId);
  }

  cam::Setup* setup = ActiveCamSetup(state);
  if (setup != nullptr) {
    ImGui::SeparatorText(("In '" + setup->name + "'").c_str());
    cam::Placement* place = PlacementOf(*setup, partId);
    if (place == nullptr) {
      ImGui::TextDisabled("Not placed: it sits at the origin.");
      if (ImGui::Button("Place###cam_part_editor_place")) {
        cam::Placement fresh;
        fresh.id = state.job->AllocateId();
        fresh.jobPartId = partId;
        setup->placements.push_back(fresh);
        state.job->BumpVersion();
        dirty = true;
      }
      ImGui::SetItemTooltip("Say where this part sits in the stock");
    } else {
      if (ImGui::Checkbox("Cut###cam_part_editor_cut", &place->enabled)) {
        state.job->BumpVersion();
        dirty = true;
      }
      ImGui::SetItemTooltip("Off leaves it on the sheet and does not cut it");
      // Through the expression wrapper, like every other numeric field here:
      // a raw drag reads "10/2" as 10. The axis is named INSIDE the field.
      const float w = ImGui::GetFontSize() * 5.5f;
      double x = place->pos.x;
      double y = place->pos.y;
      ImGui::SetNextItemWidth(w);
      if (ExprDragDouble(state, "###cam_place_x", &x, 0.5f, 0.0, 0.0,
                         "X %.2f")) {
        place->pos.x = x;
        state.job->BumpVersion();
        dirty = true;
      }
      ImGui::SetItemTooltip("Where on the stock, along X (setup space)");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(w);
      if (ExprDragDouble(state, "###cam_place_y", &y, 0.5f, 0.0, 0.0,
                         "Y %.2f")) {
        place->pos.y = y;
        state.job->BumpVersion();
        dirty = true;
      }
      ImGui::SetItemTooltip("Where on the stock, along Y (setup space)");
      ImGui::SameLine();
      double rot = place->rotZdeg;
      ImGui::SetNextItemWidth(w);
      if (ExprDragDouble(state, "###cam_place_rot", &rot, 1.0f, -360.0, 360.0,
                         "%.1f°")) {
        place->rotZdeg = rot;
        state.job->BumpVersion();
        dirty = true;
      }
      ImGui::SetItemTooltip("Turn: spin the part under the spindle");
      for (const cam::PlacementFit& f :
           cam::CheckPlacements(*setup, JobBodies(state)))
        if (f.placementId == place->id)
          if (const char* say = FitProblem(f)) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            ImGui::TextUnformatted(say);
            ImGui::PopStyleColor();
          }
      ImGui::TextDisabled("Or drag its arrows in the viewport.");
    }
  }
  if (!state.camRefusal.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
    ImGui::TextWrapped("%s", state.camRefusal.c_str());
    ImGui::PopStyleColor();
  } else if (!state.camNote.empty()) {
    ImGui::TextDisabled("%s", state.camNote.c_str());
  }
  ImGui::PopTextWrapPos();

  if (dirty && setup != nullptr) {
    (void)cam::RecomputeOrigin(*setup, JobBodies(state));
    MarkJobEdited(state);
    RefreshCamToolpaths(state);
    state.camVerdict.clear();
    state.camLeftover.clear();
  }
  ImGui::Separator();
  if (ImGui::Button(ICON_FA_TRASH_CAN " Remove###cam_part_editor_remove")) {
    RemoveJobParts(state, {partId});
    state.camEditPart = 0;
    state.camToolOpen = false;
    ImGui::End();
    return true;
  }
  ImGui::SetItemTooltip("Take this part out of the job (its operations go "
                        "with it)");
  ImGui::SameLine();
  if (ImGui::Button("Close###cam_part_editor_close")) {
    state.camEditPart = 0;
    state.camToolOpen = false;
  }
  ImGui::End();
  return true;
}

void CloseCamEditors(AppState& state) {
  state.camEditOp = 0;
  state.camEditTool = 0;
  state.camEditFixture = 0;
  state.camEditSetup = 0;
  state.camEditPart = 0;
}

namespace {
// An editor takes the tool window over from whatever verb held it: one tool
// window at a time, the rule every tool here follows.
void TakeToolWindow(AppState& state) {
  if (state.slotPick.featureId == kCamToolSlot)
    state.slotPick = {};
  state.camVerb = AppState::CamVerb::None;
  state.camVerbOp = 0;
  state.camRefusal.clear();
  state.camNote.clear();
  CloseCamEditors(state);
  state.camToolOpen = true;
}
} // namespace

void EditCamSetup(AppState& state, uint64_t setupId) {
  if (!state.job || state.job->FindSetup(setupId) == nullptr)
    return;
  TakeToolWindow(state);
  if (state.camActiveSetup != setupId) {
    state.camActiveSetup = setupId;
    RefreshCamToolpaths(state); // and this setup's own verdict, if it has one
  }
  state.camEditSetup = setupId;
}

void EditCamJobPart(AppState& state, uint64_t jobPartId) {
  if (!state.job || state.job->FindPart(jobPartId) == nullptr)
    return;
  TakeToolWindow(state);
  state.camEditPart = jobPartId;
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
void DrawCamPanel(AppState&) {}
bool DrawCamSetupEditor(AppState&) { return false; }
bool DrawCamPartEditor(AppState&) { return false; }
void EditCamSetup(AppState&, uint64_t) {}
void EditCamJobPart(AppState&, uint64_t) {}
void CloseCamEditors(AppState&) {}
} // namespace ui

#endif
