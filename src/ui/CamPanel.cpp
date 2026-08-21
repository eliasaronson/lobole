#include "ui/CamPanel.h"

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/io/Serialization.h"
#include "ui/AppActions.h"
#include "ui/FeatureEditor.h"
#include "ui/SelectionPipeline.h"

#include "imgui.h"

#include <cstdio>

namespace ui {

namespace {

// Backplot colours. Cutting moves read as "material comes off here", rapids
// as "the tool flies here" — a rapid through the part is the mistake this
// picture exists to make visible.
constexpr glm::vec4 kCutColor(0.35f, 0.80f, 1.00f, 0.95f);
constexpr glm::vec4 kPlungeColor(1.00f, 0.55f, 0.25f, 0.95f);
constexpr glm::vec4 kRapidColor(0.55f, 0.55f, 0.62f, 0.45f);

const char* kOpTypes[] = {"Profile", "Pocket", "Face", "Drill"};

cam::Setup* ActiveSetup(AppState& state) {
  if (!state.document)
    return nullptr;
  std::vector<cam::Setup>& setups = state.document->GetCamSetups();
  if (setups.empty())
    return nullptr;
  for (cam::Setup& s : setups)
    if (s.id == state.camActiveSetup)
      return &s;
  state.camActiveSetup = setups.front().id;
  return &setups.front();
}

} // namespace

void ArmCamTopFacePick(AppState& state) {
  state.slotPick = {kCamToolSlot, static_cast<int>(PickSlot::CamTopFace),
                    SelectionKindBit(SelectionItem::Kind::Face),
                    /*multi=*/false,
                    "Click the top face — that sets Z zero for the job"};
}

void CamFacePicked(AppState& state, const SelectionItem& item) {
  cam::Setup* setup = ActiveSetup(state);
  if (setup == nullptr || !item.element.IsSet() || !state.document) {
    state.slotPick.miss = "that is not a face this job can use";
    return;
  }
  // The face must be planar and horizontal: a "top face" that is tilted has
  // no single Z, and pretending otherwise would put the work origin
  // somewhere the user did not point at.
  for (const Body& body : state.document->GetBodies()) {
    if (body.createdBy != item.element.featureId)
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(item.element.name);
    glm::vec3 origin(0.0f), normal(0.0f), uAxis(0.0f);
    if (!idx || !body.shape.FacePlane(*idx, origin, normal, uAxis)) {
      state.slotPick.miss = "pick a flat face — that one is curved";
      return;
    }
    if (std::abs(normal.z) < 0.99f) {
      state.slotPick.miss = "pick a face that points up; this job cuts in Z";
      return;
    }
    // Z zero is this face. The stock top follows it, and every operation's
    // default top comes from there — so nobody types a depth again.
    setup->origin = glm::dvec3(0.0, 0.0, origin.z);
    setup->stock.margin.z = 0.0; // the picked face IS the top of the stock
    state.camTopFace = item.element;
    for (cam::Operation& op : setup->operations)
      if (op.topZ > origin.z)
        op.topZ = origin.z;
    state.slotPick = {};
    state.projectMetaDirty = true;
    RefreshCamToolpaths(state);
    return;
  }
  state.slotPick.miss = "that face is not on a body this job can machine";
}

void RefreshCamToolpaths(AppState& state) {
  state.camToolpaths.clear();
  cam::Setup* setup = ActiveSetup(state);
  if (setup == nullptr || !state.document)
    return;
  state.camToolpaths = cam::GenerateAll(*setup, state.document->GetBodies());
}

void AppendCamToolpathOverlay(AppState& state, std::vector<PrimVertex>& out) {
  if (!state.camShowToolpath || state.camToolpaths.empty())
    return;
  for (const cam::Toolpath& tp : state.camToolpaths) {
    glm::dvec3 from(0.0);
    bool have = false;
    for (const cam::Move& m : tp.moves) {
      if (have) {
        const glm::vec4 color = m.kind == cam::Move::Kind::Rapid ? kRapidColor
                                : m.kind == cam::Move::Kind::Plunge
                                    ? kPlungeColor
                                    : kCutColor;
        AppendPrimLine(glm::vec3(from), glm::vec3(m.to),
                       m.kind == cam::Move::Kind::Rapid ? 1.0f : 1.6f, color,
                       out);
      }
      from = m.to;
      have = true;
    }
  }
}

void DrawCamPanel(AppState& state) {
  if (!ImGui::Begin("CAM###CamPanel")) {
    ImGui::End();
    return;
  }
  if (!state.document) {
    ImGui::TextWrapped("CAM works on a part — a setup cuts one model. Switch "
                       "to a part tab.");
    ImGui::End();
    return;
  }

  std::vector<cam::Setup>& setups = state.document->GetCamSetups();
  if (setups.empty()) {
    ImGui::TextWrapped("A setup describes how to cut this part: stock, tools "
                       "and operations. It changes nothing about the model.");
    ImGui::Spacing();
    if (ImGui::Button("Add setup###cam_add_setup")) {
      cam::Setup& fresh = state.document->AddCamSetup();
      state.camActiveSetup = fresh.id;
      // CAM edits live in the document but bump no document version (the
      // same as study edits), so this is what keeps the save prompt honest.
      state.projectMetaDirty = true;
    }
    ImGui::End();
    return;
  }

  cam::Setup* setup = ActiveSetup(state);
  bool dirty = false;

  // --- setup row ---------------------------------------------------------
  ImGui::SetNextItemWidth(140.0f);
  if (ImGui::BeginCombo("###cam_setup", setup->name.c_str())) {
    for (cam::Setup& s : setups) {
      if (ImGui::Selectable(s.name.c_str(), s.id == state.camActiveSetup)) {
        state.camActiveSetup = s.id;
        dirty = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("+###cam_add_setup2")) {
    cam::Setup& fresh = state.document->AddCamSetup();
    state.camActiveSetup = fresh.id;
    state.projectMetaDirty = true;
    ImGui::End();
    return; // `setups` may have reallocated
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("x###cam_del_setup")) {
    state.document->RemoveCamSetup(setup->id);
    state.camActiveSetup = 0;
    state.camToolpaths.clear();
    state.projectMetaDirty = true;
    ImGui::End();
    return;
  }

  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  if (ImGui::BeginCombo("###cam_post", setup->post.c_str())) {
    for (const std::string& id : cam::PostIds())
      if (ImGui::Selectable(id.c_str(), id == setup->post)) {
        setup->post = id;
        dirty = true;
      }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Which machine this job is posted for");

  // G1: the highest-value single click in CAM. Z zero on the table instead of
  // the stock top is the mistake that breaks tools, so it is the first
  // control and it is a pick, not a number.
  const bool arming = state.slotPick.featureId == kCamToolSlot;
  if (arming)
    ImGui::BeginDisabled();
  if (ImGui::Button(state.camTopFace.IsSet() ? "Re-pick top face###cam_topface"
                                             : "Pick top face###cam_topface"))
    ArmCamTopFacePick(state);
  if (arming)
    ImGui::EndDisabled();
  ImGui::SameLine();
  if (arming)
    ImGui::TextDisabled("click the top face…");
  else if (state.camTopFace.IsSet())
    ImGui::TextDisabled("Z zero at %.3f", setup->origin.z);
  else
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                       "Z zero is not set yet");
  if (!state.slotPick.miss.empty() && arming)
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s",
                       state.slotPick.miss.c_str());

  ImGui::SetNextItemWidth(90.0f);
  if (ImGui::DragFloat("Safe Z###cam_safez",
                       reinterpret_cast<float*>(&state.camSafeZEdit), 0.5f))
    dirty = true;
  // The struct holds a double; the widget edits a float mirror so ImGui's
  // drag behaves. Kept in sync here rather than in two places.
  if (dirty)
    setup->safeZ = state.camSafeZEdit;
  else
    state.camSafeZEdit = static_cast<float>(setup->safeZ);

  // --- tools -------------------------------------------------------------
  if (ImGui::CollapsingHeader("Tools###cam_tools")) {
    // Namespaced: a tool number and an operation id are both small ints, and
    // PushID(int) hashes them into the same id space — two rows then share an
    // id and ImGui says so in a modal.
    ImGui::PushID("tools");
    for (cam::Tool& t : setup->tools) {
      ImGui::PushID(t.number);
      ImGui::Text("T%d", t.number);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.0f);
      float dia = static_cast<float>(t.diameter);
      if (ImGui::DragFloat("mm###dia", &dia, 0.1f, 0.1f, 50.0f, "%.3f")) {
        t.diameter = dia;
        dirty = true;
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.0f);
      float feed = static_cast<float>(t.feedCut);
      if (ImGui::DragFloat("F###feed", &feed, 10.0f, 1.0f, 10000.0f, "%.0f")) {
        t.feedCut = feed;
        dirty = true;
      }
      ImGui::SetItemTooltip("Cutting feed, mm/min — what YOU run. The app "
                            "does not guess feeds and speeds.");
      ImGui::PopID();
    }
    ImGui::PopID(); // "tools"
    if (ImGui::SmallButton("Add tool###cam_add_tool")) {
      cam::Tool t;
      t.number = static_cast<int>(setup->tools.size()) + 1;
      t.name = "tool " + std::to_string(t.number);
      setup->tools.push_back(t);
      dirty = true;
    }
  }

  // --- operations --------------------------------------------------------
  ImGui::SeparatorText("Operations");
  uint64_t removeOp = 0;
  ImGui::PushID("ops");
  for (cam::Operation& op : setup->operations) {
    ImGui::PushID(static_cast<int>(op.id));
    if (ImGui::Checkbox("###enabled", &op.enabled))
      dirty = true;
    ImGui::SameLine();
    int type = static_cast<int>(op.type);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::Combo("###type", &type, kOpTypes, 4)) {
      op.type = static_cast<cam::OpType>(type);
      dirty = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    float top = static_cast<float>(op.topZ);
    if (ImGui::DragFloat("###top", &top, 0.25f, -1000.0f, 1000.0f, "%.2f")) {
      op.topZ = top;
      dirty = true;
    }
    ImGui::SetItemTooltip("Top of the cut (Z)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.0f);
    float bottom = static_cast<float>(op.bottomZ);
    if (ImGui::DragFloat("###bottom", &bottom, 0.25f, -1000.0f, 1000.0f,
                         "%.2f")) {
      op.bottomZ = bottom;
      dirty = true;
    }
    ImGui::SetItemTooltip("Bottom of the cut (Z)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(55.0f);
    float step = static_cast<float>(op.stepdown);
    if (ImGui::DragFloat("###stepdown", &step, 0.25f, 0.0f, 100.0f, "%.2f")) {
      op.stepdown = step;
      dirty = true;
    }
    ImGui::SetItemTooltip("Stepdown per pass (0 = one pass)");
    ImGui::SameLine();
    if (ImGui::SmallButton("x###rmop"))
      removeOp = op.id;

    // The op's own verdict, from the last generate. A failed op says why
    // right here rather than only in the posted file.
    for (const cam::Toolpath& tp : state.camToolpaths) {
      if (tp.operationId != op.id)
        continue;
      if (!tp.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "  %s",
                           tp.error.c_str());
      } else {
        ImGui::TextDisabled("  %zu moves, %.0f mm of cutting, down to Z%.2f",
                            tp.moves.size(), tp.CutLength(), tp.MinZ());
      }
    }
    ImGui::PopID();
  }
  ImGui::PopID(); // "ops"
  if (removeOp != 0) {
    auto& ops = setup->operations;
    for (auto it = ops.begin(); it != ops.end(); ++it)
      if (it->id == removeOp) {
        ops.erase(it);
        break;
      }
    dirty = true;
  }

  if (ImGui::SmallButton("Add operation###cam_add_op")) {
    cam::Operation op;
    op.id = state.document->AllocateId();
    op.toolNumber = setup->tools.empty() ? 1 : setup->tools.front().number;
    op.name = "Operation " + std::to_string(setup->operations.size() + 1);
    // Depths derived from the model, not zeros: a default that cuts nothing
    // (or everything) is the u10 rule this app already holds itself to.
    glm::dvec3 lo(0.0), hi(0.0);
    if (cam::StockBounds(*setup, state.document->GetBodies(), lo, hi)) {
      op.topZ = hi.z - setup->stock.margin.z;
      op.bottomZ = lo.z + setup->stock.bottomMargin;
      op.stepdown = std::max(0.5, (op.topZ - op.bottomZ) / 4.0);
    }
    setup->operations.push_back(op);
    dirty = true;
  }

  // --- generate / post ---------------------------------------------------
  ImGui::Separator();
  if (ImGui::Button("Generate###cam_generate"))
    RefreshCamToolpaths(state);
  ImGui::SameLine();
  ImGui::Checkbox("Show path###cam_show", &state.camShowToolpath);

  if (!state.camToolpaths.empty()) {
    double total = 0.0;
    int failed = 0;
    for (const cam::Toolpath& tp : state.camToolpaths) {
      total += tp.CutLength();
      if (!tp.error.empty())
        failed++;
    }
    ImGui::TextDisabled("%.0f mm of cutting", total);
    if (failed > 0) {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                         "· %d operation%s produced nothing", failed,
                         failed == 1 ? "" : "s");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save G-code###cam_post")) {
      // Beside the project, named after the setup: somewhere findable, and
      // never overwriting the model.
      std::string path = state.projectPath.empty()
                             ? std::string("job.nc")
                             : state.projectPath + "/" + setup->name + ".nc";
      std::string gcode, error;
      if (cam::PostProcess(setup->post, *setup, state.camToolpaths, {}, gcode,
                           &error) &&
          DocumentIO::WriteTextAtomic(path, gcode, &error)) {
        state.camLastPost = path;
        LOG_INFO("CAM: wrote {}", path);
      } else {
        state.camLastPost = "could not write: " + error;
      }
    }
  }
  if (!state.camLastPost.empty())
    ImGui::TextDisabled("%s", state.camLastPost.c_str());

  if (dirty) {
    state.projectMetaDirty = true;
    RefreshCamToolpaths(state); // the picture always matches the parameters
  }
  ImGui::End();
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
void DrawCamPanel(AppState&) {}
void AppendCamToolpathOverlay(AppState&, std::vector<PrimVertex>&) {}
void RefreshCamToolpaths(AppState&) {}
} // namespace ui

#endif
