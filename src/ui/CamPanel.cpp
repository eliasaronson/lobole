#include "ui/CamPanel.h"

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/io/Serialization.h"
#include "ui/AppActions.h"
#include "ui/CamTool.h"
#include "ui/FeatureEditor.h"
#include "ui/SelectionPipeline.h"

#include "IconsFontAwesome6.h"
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

// Order matches cam::OpType. "Adaptive" is a pocket cut a different way,
// so it reads as a strategy rather than as a separate kind of job.
const char* kOpTypes[] = {"Profile", "Pocket", "Face", "Drill", "Adaptive"};

} // namespace

cam::Setup* ActiveCamSetup(AppState& state) {
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

void RefreshCamToolpaths(AppState& state) {
  state.camToolpaths.clear();
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr || !state.document)
    return;
  state.camToolpaths = cam::GenerateAll(*setup, state.document->GetBodies());
}

namespace {

// The tool, to scale, where it first enters the material (docs/cam/gui.md).
// This is the picture that answers the question a first-time CAM user does
// not know to ask: will my cutter fit, and what radius will it leave in the
// corners? A circle of the real diameter plus a short shank says both.
void AppendToolGlyph(const AppState& state, const cam::Setup& setup,
                     const cam::Toolpath& tp, std::vector<PrimVertex>& out) {
  const cam::Tool* tool = setup.FindTool(tp.toolNumber);
  if (tool == nullptr)
    return;
  glm::dvec3 at(0.0);
  bool found = false;
  for (const cam::Move& m : tp.moves)
    if (m.kind == cam::Move::Kind::Cut || m.kind == cam::Move::Kind::Plunge) {
      at = m.to;
      found = true;
      break;
    }
  if (!found)
    return;

  const double r = tool->diameter * 0.5;
  constexpr int kSegments = 32;
  const glm::vec4 color(1.0f, 0.85f, 0.35f, 0.85f);
  // Built in setup space (where the tool is) and shown in model space (where
  // the part is): for an indexed setup the cutter leans with the table.
  const auto shown = [&](const glm::dvec3& p) {
    return glm::vec3(cam::FromSetupSpace(setup, p));
  };
  glm::vec3 prev(0.0f);
  for (int i = 0; i <= kSegments; i++) {
    const double a = 2.0 * 3.14159265358979 * i / kSegments;
    const glm::vec3 p =
        shown({at.x + r * std::cos(a), at.y + r * std::sin(a), at.z});
    if (i > 0)
      AppendPrimLine(prev, p, 1.4f, color, out);
    prev = p;
  }
  // A stub of shank, so the circle reads as a cutter rather than a hole.
  const double shank = std::max(2.0, tool->diameter);
  for (int i = 0; i < 4; i++) {
    const double a = 3.14159265358979 * 0.5 * i;
    const glm::dvec3 base(at.x + r * std::cos(a), at.y + r * std::sin(a), at.z);
    AppendPrimLine(shown(base), shown(base + glm::dvec3(0.0, 0.0, shank)), 1.0f,
                   color, out);
  }
}

} // namespace

void AppendCamToolpathOverlay(AppState& state, std::vector<PrimVertex>& out) {
  if (!state.camShowToolpath || state.camToolpaths.empty())
    return;
  const cam::Setup* active = ActiveCamSetup(state);
  // Moves are in SETUP space (C9). Drawn straight, an indexed job's backplot
  // floats beside the part instead of on it.
  const auto shown = [&](const glm::dvec3& p) {
    return active != nullptr ? cam::FromSetupSpace(*active, p) : p;
  };
  for (const cam::Toolpath& tp : state.camToolpaths) {
    glm::dvec3 from(0.0);
    bool have = false;
    for (const cam::Move& m : tp.moves) {
      if (have) {
        const glm::vec4 color = m.kind == cam::Move::Kind::Rapid ? kRapidColor
                                : m.kind == cam::Move::Kind::Plunge
                                    ? kPlungeColor
                                    : kCutColor;
        AppendPrimLine(glm::vec3(shown(from)), glm::vec3(shown(m.to)),
                       m.kind == cam::Move::Kind::Rapid ? 1.0f : 1.6f, color,
                       out);
      }
      from = m.to;
      have = true;
    }
    if (const cam::Setup* setup = ActiveCamSetup(state))
      AppendToolGlyph(state, *setup, tp, out);
  }
}

void DrawCamPanel(AppState& state) {
  // NoFocusOnAppearing, like every other panel tabbed into the left node: a
  // window that appears in a shared dock node becomes the SELECTED tab, and
  // this one shipped without the flag — so the first launch after CAM landed
  // put the job list in front of the model tree. Three GUI tests caught it as
  // "unable to hover Model"; a user would have called it "my tree is gone".
  if (!ImGui::Begin("CAM###CamPanel", nullptr,
                    ImGuiWindowFlags_NoFocusOnAppearing)) {
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
      // Safe Z clear of the stock, not the struct's bare 5 mm — which sits
      // inside anything taller than that (docs/cam/gui.md).
      fresh.safeZ = cam::DeriveSafeZ(fresh, state.document->GetBodies());
      state.camSafeZEdit = static_cast<float>(fresh.safeZ);
      // CAM edits live in the document but bump no document version (the
      // same as study edits), so this is what keeps the save prompt honest.
      state.projectMetaDirty = true;
    }
    ImGui::End();
    return;
  }

  cam::Setup* setup = ActiveCamSetup(state);
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
    fresh.safeZ = cam::DeriveSafeZ(fresh, state.document->GetBodies());
    state.camSafeZEdit = static_cast<float>(fresh.safeZ);
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

  // The way in, on its own line. It used to sit third on the "Pick top face"
  // row, after a status message long enough ("Z zero is not set yet") that
  // the whole row overflowed the panel — and the default dock layout makes
  // this panel 217 px wide, so the one-click way into CAM was clipped off
  // the edge where nobody could click it (dogfooding round 2).
  if (ImGui::Button("Verbs — the one-click way in###cam_open_tool"))
    state.camToolOpen = true;
  ImGui::SetItemTooltip("Point at the part; every depth is derived from what "
                        "you point at");

  // G1: the highest-value single click in CAM. Z zero on the table instead of
  // the stock top is the mistake that breaks tools, so it is the first
  // control and it is a pick, not a number.
  const bool arming = state.slotPick.featureId == kCamToolSlot;
  if (arming)
    ImGui::BeginDisabled();
  if (ImGui::Button(state.camTopFace.IsSet() ? "Re-pick top face###cam_topface"
                                             : "Pick top face###cam_topface"))
    ArmCamPick(state, PickSlot::CamTopFace,
               "Click the top face — that sets Z zero for the job");
  if (arming)
    ImGui::EndDisabled();
  ImGui::SameLine();
  if (arming)
    // The prompt that is actually armed. This said "click the top face…"
    // whatever the pick was for, so arming "clear a pocket" from the verbs
    // window told you to click the wrong thing.
    ImGui::TextDisabled("%s…", state.slotPick.prompt.empty()
                                   ? "click a face"
                                   : state.slotPick.prompt.c_str());
  else if (state.camTopFace.IsSet())
    ImGui::TextDisabled("Z zero at %.3f", setup->origin.z);
  else
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                       "Z zero is not set yet");
  if (!state.slotPick.miss.empty() && arming) {
    // WRAPPED. A refusal is a sentence whose second half says what to do
    // instead, and this panel is narrow: unwrapped, the useful half was
    // clipped off the right edge where nobody could read it.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", state.slotPick.miss.c_str());
    ImGui::PopStyleColor();
  }

  // The stock, and the rapid height above it. Both are properties of the JOB
  // rather than of an operation, and the facing verb refuses without the
  // first — so it lives where that refusal can send you.
  if (ImGui::CollapsingHeader("Stock and safety###cam_stock")) {
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("Safe Z###cam_safez",
                         reinterpret_cast<float*>(&state.camSafeZEdit), 0.5f))
      dirty = true;
    ImGui::SetItemTooltip("Rapid height above the stock top");
    float above = static_cast<float>(setup->stock.margin.z);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("Above the part###cam_stock_above", &above, 0.1f, 0.0f,
                         100.0f, "%.2f")) {
      setup->stock.margin.z = above;
      dirty = true;
    }
    ImGui::SetItemTooltip("Material sitting on top of the part — what a "
                          "facing pass removes. Picking the top face sets "
                          "this to zero.");
    float around = static_cast<float>(setup->stock.margin.x);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("Around it###cam_stock_around", &around, 0.1f, 0.0f,
                         100.0f, "%.2f")) {
      setup->stock.margin.x = around;
      setup->stock.margin.y = around;
      dirty = true;
    }
    float below = static_cast<float>(setup->stock.bottomMargin);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("Below it###cam_stock_below", &below, 0.1f, 0.0f,
                         100.0f, "%.2f")) {
      setup->stock.bottomMargin = below;
      dirty = true;
    }
    ImGui::SetItemTooltip("How far a through-cut goes into the spoilboard");
    if (ImGui::Checkbox("Arcs (G2/G3)###cam_arcs", &setup->fitArcs))
      dirty = true;
    ImGui::SetItemTooltip("Post curves as arcs instead of many short lines. "
                          "Turn off for a controller that mishandles them.");
    float indexA = static_cast<float>(setup->rotationA);
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::DragFloat("A index###cam_index_a", &indexA, 0.5f, -360.0f,
                         360.0f, "%.2f°")) {
      setup->rotationA = indexA;
      dirty = true;
    }
    ImGui::SetItemTooltip("Rotary 4th axis: how far the table is turned for "
                          "this setup. 0 for a 3-axis job.");
  }
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

  // --- generate / post ---------------------------------------------------
  // ABOVE the operations list, not below it. This panel is 217 px wide and
  // the window is 720 px tall in the default layout, and the list grows with
  // the job: put the verdict and Save G-code after it and they are the first
  // things to fall off the bottom, exactly as the job gets big enough to want
  // checking (docs/cam/dogfood-round.md, round 2). The list is the part that
  // may scroll; what the job SAYS is not.
  ImGui::Separator();
  if (ImGui::Button("Generate###cam_generate")) {
    RefreshCamToolpaths(state);
    RunCamVerification(state);
  }
  ImGui::SameLine();
  ImGui::Checkbox("Show path###cam_show", &state.camShowToolpath);

  // G4: what the verifier found, in words, without being asked. The numbers
  // behind these lines are in the same Verdict the RPC returns.
  for (const cam::VerdictLine& line : state.camVerdict) {
    const ImVec4 color = line.level == cam::VerdictLine::Level::Bad
                             ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                         : line.level == cam::VerdictLine::Level::Warn
                             ? ImVec4(0.95f, 0.80f, 0.40f, 1.0f)
                             : ImVec4(0.55f, 0.90f, 0.60f, 1.0f);
    // The icon font is merged into the UI atlas, so these are real glyphs;
    // a literal U+2713 would not be (the text font stops at Latin-1).
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s %s",
                       line.level == cam::VerdictLine::Level::Good
                           ? ICON_FA_CIRCLE_CHECK
                           : ICON_FA_TRIANGLE_EXCLAMATION,
                       line.text.c_str());
    ImGui::PopStyleColor();
  }

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
      if (cam::PostProcess(setup->post, *setup, state.camToolpaths,
                           PostOpts(*setup), gcode, &error) &&
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

  // --- operations --------------------------------------------------------
  ImGui::SeparatorText("Operations");
  uint64_t removeOp = 0;
  ImGui::PushID("ops");
  for (cam::Operation& op : setup->operations) {
    ImGui::PushID(static_cast<int>(op.id));
    if (ImGui::Checkbox("###enabled", &op.enabled))
      dirty = true;
    ImGui::SameLine();
    // What this operation IS, in a sentence. The numbers behind it are
    // derived from the model and the cutter (docs/cam/gui.md); the fields
    // that set them are one disclosure away, not the way in.
    const cam::Tool* opTool = setup->FindTool(op.toolNumber);
    const int passes = static_cast<int>(cam::DepthPasses(op).size());
    ImGui::Text(
        "%s: %.3g mm tool, %.3g mm deep in %d pass%s, %s",
        op.name.empty() ? kOpTypes[static_cast<int>(op.type)] : op.name.c_str(),
        opTool != nullptr ? opTool->diameter : 0.0, op.topZ - op.bottomZ,
        passes, passes == 1 ? "" : "es", op.climb ? "climb" : "conventional");
    ImGui::SameLine();
    const bool ghosted = state.camGhostOp == op.id;
    if (ImGui::SmallButton(ghosted ? "hide###ghost" : "show###ghost"))
      state.camGhostOp = ghosted ? 0 : op.id;
    ImGui::SetItemTooltip("Ghost the material this one removes");
    ImGui::SameLine();
    if (ImGui::SmallButton("x###rmop"))
      removeOp = op.id;

    if (ImGui::TreeNode("Advanced###adv")) {
      int type = static_cast<int>(op.type);
      ImGui::SetNextItemWidth(90.0f);
      if (ImGui::Combo("###type", &type, kOpTypes, IM_ARRAYSIZE(kOpTypes))) {
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
      if (op.type == cam::OpType::Pocket || op.type == cam::OpType::Face ||
          op.type == cam::OpType::Adaptive) {
        ImGui::SetNextItemWidth(70.0f);
        float over = static_cast<float>(op.stepover);
        if (ImGui::DragFloat("Stepover###stepover", &over, 0.01f, 0.05f, 0.9f,
                             "%.2f")) {
          op.stepover = over;
          dirty = true;
        }
        ImGui::SetItemTooltip("Fraction of the cutter taken per pass "
                              "sideways — the number adaptive clearing holds "
                              "constant");
      }
      if (ImGui::Checkbox("Climb###climb", &op.climb))
        dirty = true;
      ImGui::SameLine();
      ImGui::SetNextItemWidth(55.0f);
      int tabs = op.tabCount;
      if (ImGui::DragInt("Tabs###tabs", &tabs, 0.2f, 0, 32)) {
        op.tabCount = tabs;
        dirty = true;
      }
      ImGui::SetItemTooltip("Uncut bridges that hold the part on the last "
                            "pass (profile only)");
      if (!op.targets.empty())
        ImGui::TextDisabled("boundary: %s", op.targets[0].name.c_str());
      ImGui::TreePop();
    }

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
    // ONE derivation, two entry points: the same engine call the verbs use
    // (docs/cam/gui.md). This path used to start the cut at the MODEL top,
    // which leaves the stock above it uncut and was the last residue of the
    // `2 -> -0` default — and it derived a stepdown from the depth rather
    // than from the cutter. A refusal is shown rather than an operation that
    // cannot cut (the u10 rule).
    const cam::DerivedOp derived = cam::DeriveOperation(
        *setup, state.document->GetBodies(), cam::OpType::Profile, {});
    if (derived.ok) {
      cam::Operation op = derived.op;
      op.id = state.document->AllocateId();
      op.name = "Operation " + std::to_string(setup->operations.size() + 1);
      setup->operations.push_back(op);
      state.camRefusal.clear();
      dirty = true;
    } else {
      state.camRefusal = derived.why;
    }
  }
  if (!state.camRefusal.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", state.camRefusal.c_str());
    ImGui::PopStyleColor();
  }

  if (dirty) {
    state.projectMetaDirty = true;
    RefreshCamToolpaths(state); // the picture always matches the parameters
    // A verdict about the PREVIOUS toolpath is worse than no verdict: drop
    // it, and let the next Generate produce an honest one.
    state.camVerdict.clear();
    state.camLeftover.clear();
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
