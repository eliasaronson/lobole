#include <filesystem>

#include "ui/CamPanel.h"
#include "ui/DockLayout.h" // PanelHeading
#include "ui/PanelTitles.h"

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/io/Serialization.h"
#include "ui/AppActions.h"
#include "ui/CamPlayback.h"
#include "ui/CamTool.h"
#include "ui/ExprInput.h"
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
// In OpType order — the combo writes the index straight back as the enum.
const char* kOpTypes[] = {"Profile", "Pocket",   "Face",
                          "Drill",   "Adaptive", "Edge break"};

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

void RefreshCamToolpath(AppState& state, uint64_t opId) {
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr || !state.document || opId == 0)
    return;
  const cam::Operation* op = nullptr;
  for (const cam::Operation& candidate : setup->operations)
    if (candidate.id == opId)
      op = &candidate;
  if (op == nullptr) {
    DropCamToolpath(state, opId);
    return;
  }
  if (!op->enabled) {
    // GenerateAll skips disabled operations, so the incremental path has to
    // as well or the two disagree about what the job contains.
    DropCamToolpath(state, opId);
    return;
  }
  cam::Toolpath fresh = cam::Generate(*setup, *op, state.document->GetBodies());
  for (cam::Toolpath& existing : state.camToolpaths)
    if (existing.operationId == opId) {
      existing = std::move(fresh);
      return;
    }
  // New: insert where a full pass would have put it — operations order, so
  // the backplot and the posted file follow the list the user sees.
  size_t at = state.camToolpaths.size();
  bool seen = false;
  for (size_t i = 0, slot = 0; i < setup->operations.size(); i++) {
    const cam::Operation& candidate = setup->operations[i];
    if (candidate.id == opId) {
      at = slot;
      seen = true;
      break;
    }
    if (candidate.enabled)
      slot++;
  }
  (void)seen;
  state.camToolpaths.insert(
      state.camToolpaths.begin() +
          static_cast<ptrdiff_t>(std::min(at, state.camToolpaths.size())),
      std::move(fresh));
}

void DropCamToolpath(AppState& state, uint64_t opId) {
  for (auto it = state.camToolpaths.begin(); it != state.camToolpaths.end();
       ++it)
    if (it->operationId == opId) {
      state.camToolpaths.erase(it);
      return;
    }
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
  // While a simulation is open the line drawn is the file being PLAYED, dimmed
  // behind the cursor — one path on screen, and it is the one being cut.
  if (CamPlaybackDrawsPath(state)) {
    AppendCamPlaybackPath(state, out);
    return;
  }
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
  if (!ImGui::Begin(kPanelCam, nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return;
  }
  PanelHeading(kPanelCam);
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
      (void)cam::RecomputeOrigin(fresh, state.document->GetBodies());
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
  // WHICH operation changed, so only that one is regenerated. One adaptive
  // pocket takes seconds to generate; regenerating the whole setup on every
  // widget touch made the app freeze for as long as its slowest operation,
  // once per edit (docs/known-bugs.md). 0 = nothing yet; kAllOps = something
  // setup-wide (the stock, a tool, the frame), which every operation reads.
  constexpr uint64_t kAllOps = ~uint64_t{0};
  uint64_t changed = 0;
  const auto touched = [&](uint64_t opId) {
    dirty = true;
    // Two different operations in one frame is not a thing a person can do,
    // but a script or a re-entrant widget could: fall back to everything
    // rather than leave one of them stale.
    changed = (changed == 0 || changed == opId) ? opId : kAllOps;
  };

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
    (void)cam::RecomputeOrigin(fresh, state.document->GetBodies());
    fresh.safeZ = cam::DeriveSafeZ(fresh, state.document->GetBodies());
    state.camSafeZEdit = static_cast<float>(fresh.safeZ);
    state.projectMetaDirty = true;
    ImGui::End();
    return; // `setups` may have reallocated
  }
  ImGui::SameLine();
  // The other side of the same part. A flip job is two setups on ONE part —
  // the alternative people reach for is a rotated COPY of the part, which
  // then has to be kept in step with the original by hand
  // (docs/cam/setup-frame.md S4).
  if (ImGui::SmallButton("Flip###cam_flip_setup")) {
    cam::Setup& other = state.document->AddCamSetup(setup->name + " (back)");
    // Carry the MACHINE and the material across — they did not change when
    // the part was turned over. Operations do not come: they are what the
    // other side is for.
    other.frame = cam::FlipFrame(setup->frame);
    other.stock = setup->stock;
    other.tools = setup->tools;
    other.post = setup->post;
    other.fitArcs = setup->fitArcs;
    other.rapidFeed = setup->rapidFeed;
    other.hasRotaryAxis = setup->hasRotaryAxis;
    other.originAt = setup->originAt;
    other.safeZ = cam::DeriveSafeZ(other, state.document->GetBodies());
    (void)cam::RecomputeOrigin(other, state.document->GetBodies());
    state.camActiveSetup = other.id;
    state.camSafeZEdit = static_cast<float>(other.safeZ);
    state.projectMetaDirty = true;
    ImGui::End();
    return; // `setups` may have reallocated
  }
  ImGui::SetItemTooltip("Add a second setup for the other side: the same "
                        "part, turned over. Same stock and tools, no "
                        "operations yet.");
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
  if (ImGui::Button(setup->topFace.IsSet() ? "Re-pick top face###cam_topface"
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
  else if (setup->topFace.IsSet())
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
    if (ExprDragFloat(state, "Safe Z###cam_safez",
                      reinterpret_cast<float*>(&state.camSafeZEdit), 0.5f,
                      -FLT_MAX, FLT_MAX, "%.3f"))
      dirty = true;
    ImGui::SetItemTooltip("Rapid height above the stock top");
    float above = static_cast<float>(setup->stock.margin.z);
    ImGui::SetNextItemWidth(90.0f);
    if (ExprDragFloat(state, "Above the part###cam_stock_above", &above, 0.1f,
                      0.0f, 100.0f, "%.2f")) {
      setup->stock.margin.z = above;
      dirty = true;
    }
    ImGui::SetItemTooltip("Material sitting on top of the part — what a "
                          "facing pass removes. Picking the top face sets "
                          "this to zero.");
    float around = static_cast<float>(setup->stock.margin.x);
    ImGui::SetNextItemWidth(90.0f);
    if (ExprDragFloat(state, "Around it###cam_stock_around", &around, 0.1f,
                      0.0f, 100.0f, "%.2f")) {
      setup->stock.margin.x = around;
      setup->stock.margin.y = around;
      dirty = true;
    }
    // WHAT the workpiece is. Relative — the model's box plus margins — is what
    // every job had before v68 and stays the default; Fixed is the sheet you
    // actually own, which a shrink-wrap around one part cannot express.
    int mode = setup->stock.mode == cam::StockMode::Fixed ? 1 : 0;
    static const char* kStockModes[] = {"Around the part", "A fixed sheet"};
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("###cam_stock_mode", &mode, kStockModes,
                     IM_ARRAYSIZE(kStockModes))) {
      setup->stock.mode =
          mode == 1 ? cam::StockMode::Fixed : cam::StockMode::Relative;
      // Seed the sheet from the box it already had, so switching modes never
      // lands on a stock of zero (the u10 rule: a default carrying a unit is
      // derived from what it acts on).
      if (setup->stock.mode == cam::StockMode::Fixed &&
          setup->stock.size == glm::dvec3(0.0)) {
        glm::dvec3 lo(0.0), hi(0.0);
        if (cam::StockBounds(*setup, state.document->GetBodies(), lo, hi))
          setup->stock.size = hi - lo;
      }
      dirty = true;
      changed = kAllOps;
    }
    if (setup->stock.mode == cam::StockMode::Fixed) {
      for (int a = 0; a < 3; a++) {
        float v = static_cast<float>(setup->stock.size[a]);
        ImGui::SetNextItemWidth(58.0f);
        const char* ids[3] = {"###stock_sx", "###stock_sy", "###stock_sz"};
        if (a > 0)
          ImGui::SameLine();
        if (ExprDragFloat(state, ids[a], &v, 0.5f, 0.0f, 100000.0f, "%.1f")) {
          setup->stock.size[a] = v;
          dirty = true;
          changed = kAllOps;
        }
      }
      ImGui::SameLine();
      ImGui::TextDisabled("mm");
      const std::string refusal =
          cam::StockRefusal(*setup, state.document->GetBodies());
      if (!refusal.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s",
                           refusal.c_str());
        ImGui::PopTextWrapPos();
      }
    }

    // WHERE ZERO IS — the corner the operator touches off on, and the point
    // every coordinate in the posted file is measured from.
    int at = static_cast<int>(setup->originAt);
    static const char* kOriginAt[] = {"Stock corner", "Stock centre",
                                      "Part corner"};
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("###cam_origin_at", &at, kOriginAt,
                     IM_ARRAYSIZE(kOriginAt))) {
      setup->originAt = static_cast<cam::OriginAt>(at);
      dirty = true;
      changed = kAllOps;
    }
    ImGui::SetItemTooltip("Where X0 Y0 is in the posted file — the corner you "
                          "touch off on. Z comes from the picked top face.");
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f %.2f %.2f", setup->origin.x, setup->origin.y,
                        setup->origin.z);

    float below = static_cast<float>(setup->stock.bottomMargin);
    ImGui::SetNextItemWidth(90.0f);
    if (ExprDragFloat(state, "Below it###cam_stock_below", &below, 0.1f, 0.0f,
                      100.0f, "%.2f")) {
      setup->stock.bottomMargin = below;
      dirty = true;
    }
    ImGui::SetItemTooltip("How far a through-cut goes into the spoilboard");
    if (ImGui::Checkbox("Arcs (G2/G3)###cam_arcs", &setup->fitArcs))
      dirty = true;
    ImGui::SetItemTooltip("Post curves as arcs instead of many short lines. "
                          "Turn off for a controller that mishandles them.");
    // Does this machine HAVE a 4th axis? A property of the machine, like the
    // post and whether it fits arcs. Until it is ticked no file can carry an
    // A word — which is the whole point: laying a part flat on the bed used
    // to post `G0 A90` at routers that have no A (docs/cam/setup-frame.md).
    if (ImGui::Checkbox("Has a 4th axis (A)###cam_has_rotary",
                        &setup->hasRotaryAxis)) {
      dirty = true;
      changed = kAllOps;
    }
    ImGui::SetItemTooltip("Tick only for a machine with a rotary table. How "
                          "the part is CLAMPED is \"Lay a face up\" and needs "
                          "no rotary axis.");
    if (setup->hasRotaryAxis) {
      float indexA = static_cast<float>(setup->rotationA);
      ImGui::SetNextItemWidth(90.0f);
      if (ExprDragFloat(state, "A index###cam_index_a", &indexA, 0.5f, -360.0f,
                        360.0f, "%.2f°")) {
        setup->rotationA = indexA;
        dirty = true;
        changed = kAllOps;
      }
      ImGui::SetItemTooltip("Rotary 4th axis: how far the table is turned for "
                            "this setup. 0 for a 3-axis job.");
    }
  }
  // The struct holds a double; the widget edits a float mirror so ImGui's
  // drag behaves. Kept in sync here rather than in two places.
  if (dirty) {
    setup->safeZ = state.camSafeZEdit;
    // The stock box may have moved, and the work origin is derived from it —
    // so is every coordinate in the posted file.
    (void)cam::RecomputeOrigin(*setup, state.document->GetBodies());
  } else
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
      if (ExprDragFloat(state, "mm###dia", &dia, 0.1f, 0.1f, 50.0f, "%.3f")) {
        t.diameter = dia;
        dirty = true;
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70.0f);
      float feed = static_cast<float>(t.feedCut);
      if (ExprDragFloat(state, "F###feed", &feed, 10.0f, 1.0f, 10000.0f,
                        "%.0f")) {
        t.feedCut = feed;
        dirty = true;
      }
      ImGui::SetItemTooltip("Cutting feed, mm/min — what YOU run. The app "
                            "does not guess feeds and speeds.");
      // The shape of the end. It is not decoration: the verifier simulates
      // it, and "Break an edge" reads it to know whether it is cutting a
      // bevel or an arc.
      ImGui::SetNextItemWidth(90.0f);
      int shape = static_cast<int>(t.type);
      if (ImGui::Combo("###tooltype", &shape, "flat\0ball\0V-bit\0drill\0")) {
        t.type = static_cast<cam::ToolType>(shape);
        dirty = true;
      }
      ImGui::SetItemTooltip("End shape. A ball rounds an edge, a V-bit "
                            "chamfers it; a flat mill does neither.");
      if (t.type == cam::ToolType::VBit || t.type == cam::ToolType::Drill) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        float angle = static_cast<float>(t.tipAngleDeg);
        if (ExprDragFloat(state, "deg###tipangle", &angle, 1.0f, 5.0f, 179.0f,
                          "%.0f")) {
          t.tipAngleDeg = angle;
          dirty = true;
        }
        ImGui::SetItemTooltip("INCLUDED angle at the point: 90 for the "
                              "common chamfer bit, 118 for a twist drill.");
      }
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
    // A playback is a picture of a FILE, and this button replaces the file.
    // Leaving it running would scrub through G-code the job no longer posts.
    EndCamPlayback(state);
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
    // NOT "###cam_post": that is the post-DIALECT combo on the setup row, and
    // two widgets in one window answering to one id is a real ImGui error —
    // it raised "2 visible items with conflicting ID" over the user's job.
    // The collision was always there; it started firing when this button
    // moved above the operations list and the two became visible at once
    // (docs/cam/dogfood-round.md).
    if (ImGui::Button("Save G-code###cam_save_gcode")) {
      // BESIDE the project, which is what this always meant to say and did
      // not: `projectPath` is the .dsgn bundle itself, so appending to it
      // wrote the G-code INSIDE the project. `StampProject` walks every
      // regular file in there, so the job's own output changed the project's
      // disk stamp and the next save refused with "the project changed on
      // disk since you opened it" — the app fighting itself over a file it
      // had just written.
      //
      // With no project there is no "beside", and the fallback was a bare
      // "job.nc" written into whatever directory the app was launched from.
      // Refuse and say where it would go instead of leaving G-code somewhere
      // nobody will look for it (docs/cam/dogfood-round.md).
      if (state.projectPath.empty()) {
        state.camLastPost =
            "save the project first — the G-code goes beside it, named after "
            "the setup";
        LOG_INFO("CAM: refused to post an unsaved project");
      } else {
        const std::filesystem::path project(state.projectPath);
        std::filesystem::path dir = project.parent_path();
        if (dir.empty())
          dir = ".";
        // Named for the project AND the setup: several parts' jobs land in
        // one directory, and "Setup 1.nc" from two of them is one file.
        const std::string path =
            (dir / (project.stem().string() + " - " + setup->name + ".nc"))
                .string();
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
  }
  if (!state.camLastPost.empty())
    ImGui::TextDisabled("%s", state.camLastPost.c_str());

  // The transport sits with Generate and the verdict, because it answers the
  // same question they do — is this job right — and it is the one that shows
  // rather than tells.
  DrawCamTransport(state);

  // --- operations --------------------------------------------------------
  ImGui::SeparatorText("Operations");
  uint64_t removeOp = 0;
  uint64_t moveOp = 0; // dragged operation …
  size_t moveTo = 0;   // … and where it was dropped
  ImGui::PushID("ops");
  for (size_t opIndex = 0; opIndex < setup->operations.size(); opIndex++) {
    cam::Operation& op = setup->operations[opIndex];
    ImGui::PushID(static_cast<int>(op.id));
    // The order is the cutting sequence — GenerateAll and the post both walk
    // this vector — so it has to be editable. Rough before finish, and the
    // tool changes fall where the order puts them.
    ImGui::Selectable(ICON_FA_GRIP_VERTICAL "###drag", false,
                      ImGuiSelectableFlags_None, ImVec2(12.0f, 0.0f));
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
      ImGui::SetDragDropPayload("cam_op", &opIndex, sizeof(size_t));
      ImGui::Text("%s", op.name.empty() ? kOpTypes[static_cast<int>(op.type)]
                                        : op.name.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("cam_op")) {
        size_t from = 0;
        std::memcpy(&from, pl->Data, sizeof(size_t));
        if (from < setup->operations.size() && from != opIndex) {
          moveOp = setup->operations[from].id;
          moveTo = opIndex;
        }
      }
      ImGui::EndDragDropTarget();
    }
    ImGui::SetItemTooltip("Drag to reorder — this is the order it cuts in");
    ImGui::SameLine();
    if (ImGui::Checkbox("###enabled", &op.enabled))
      touched(op.id);
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
      // WHICH CUTTER. There was no way to say it by clicking — `toolNumber`
      // was read to print a diameter and never written — so "use a cutter of
      // 5.46 mm or less" was a refusal naming a remedy the panel could not
      // perform, even with the smaller tool already in the setup.
      //
      // A row of buttons rather than a combo: a setup has a handful of tools,
      // so this shows every option at a glance and costs ONE click instead of
      // two. It is also the only form the test engine can address — a combo
      // gathers with an empty label whatever you name it (the trap the
      // drawings round hit), so a combo here could not be gated.
      ImGui::TextDisabled("Tool");
      for (const cam::Tool& t : setup->tools) {
        ImGui::SameLine();
        const bool active = t.number == op.toolNumber;
        if (active)
          ImGui::PushStyleColor(
              ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        char id[32];
        std::snprintf(id, sizeof(id), "T%d###optool_t%d", t.number, t.number);
        if (ImGui::SmallButton(id)) {
          std::string why;
          if (cam::Retool(*setup, op, t.number, why))
            touched(op.id);
          else
            state.camRefusal = why;
        }
        if (active)
          ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%.3g mm %s — the stepdown follows the cutter "
                              "unless you have typed your own",
                              t.diameter, t.name.c_str());
      }
      if (setup->FindTool(op.toolNumber) == nullptr) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "T%d missing",
                           op.toolNumber);
      }
      int type = static_cast<int>(op.type);
      ImGui::SetNextItemWidth(90.0f);
      if (ImGui::Combo("###type", &type, kOpTypes, IM_ARRAYSIZE(kOpTypes))) {
        op.type = static_cast<cam::OpType>(type);
        touched(op.id);
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(60.0f);
      float top = static_cast<float>(op.topZ);
      if (ExprDragFloat(state, "###top", &top, 0.25f, -1000.0f, 1000.0f,
                        "%.2f")) {
        op.topZ = top;
        touched(op.id);
      }
      ImGui::SetItemTooltip("Top of the cut (Z)");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(60.0f);
      float bottom = static_cast<float>(op.bottomZ);
      if (ExprDragFloat(state, "###bottom", &bottom, 0.25f, -1000.0f, 1000.0f,
                        "%.2f")) {
        op.bottomZ = bottom;
        touched(op.id);
      }
      ImGui::SetItemTooltip("Bottom of the cut (Z)");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(55.0f);
      float step = static_cast<float>(op.stepdown);
      if (ExprDragFloat(state, "###stepdown", &step, 0.25f, 0.0f, 100.0f,
                        "%.2f")) {
        op.stepdown = step;
        touched(op.id);
      }
      ImGui::SetItemTooltip("Stepdown per pass (0 = one pass)");
      if (op.type == cam::OpType::EdgeRound) {
        ImGui::SetNextItemWidth(70.0f);
        float size = static_cast<float>(op.edgeSize);
        if (ExprDragFloat(state, "Break###edgesize", &size, 0.1f, 0.0f, 100.0f,
                          "%.2f")) {
          op.edgeSize = size;
          touched(op.id);
        }
        ImGui::SetItemTooltip("Radius of the round (ball) or width of the "
                              "bevel (V-bit), mm. 0 = the biggest this tool "
                              "can make within the wall.");
      }
      if (op.type == cam::OpType::Pocket || op.type == cam::OpType::Face ||
          op.type == cam::OpType::Adaptive ||
          op.type == cam::OpType::EdgeRound) {
        ImGui::SetNextItemWidth(70.0f);
        float over = static_cast<float>(op.stepover);
        if (ExprDragFloat(state, "Stepover###stepover", &over, 0.01f, 0.05f,
                          0.9f, "%.2f")) {
          op.stepover = over;
          touched(op.id);
        }
        ImGui::SetItemTooltip("Fraction of the cutter taken per pass "
                              "sideways — the number adaptive clearing holds "
                              "constant");
      }
      // What this pass DOES NOT take: the roughing/finishing split. A pocket
      // roughed with 0.3 mm left and then re-cut at 0 comes out with a wall
      // the cutter has touched once, at full depth, in one pass — which is
      // the difference between a flange a driver seats flat on and a stack of
      // stepdown terraces. The engine has had it since C1; only a script
      // could reach it.
      float leave = static_cast<float>(op.stockToLeave);
      ImGui::SetNextItemWidth(58.0f);
      if (ExprDragFloat(state, "Leave###leave", &leave, 0.05f, 0.0f, 10.0f,
                        "%.2f")) {
        op.stockToLeave = leave;
        touched(op.id);
      }
      ImGui::SetItemTooltip("Material this pass leaves behind, in mm. Rough "
                            "with 0.2-0.4, then a second operation at 0 "
                            "finishes the wall in one full-depth pass.");
      ImGui::SameLine();
      if (ImGui::Checkbox("Climb###climb", &op.climb))
        touched(op.id);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(55.0f);
      int tabs = op.tabCount;
      if (ExprDragInt(state, "Tabs###tabs", &tabs, 0.2f, 0, 32)) {
        op.tabCount = tabs;
        touched(op.id);
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
  if (moveOp != 0 && cam::MoveOperation(*setup, moveOp, moveTo)) {
    // Reordering changes no toolpath — only the sequence they are cut in and
    // where the tool changes fall — so nothing is regenerated. The posted
    // file does change, which is what the dirty flag is for.
    dirty = true;
    state.projectMetaDirty = true;
    state.camVerdict.clear();
    state.camLeftover.clear();
  }
  if (removeOp != 0) {
    auto& ops = setup->operations;
    for (auto it = ops.begin(); it != ops.end(); ++it)
      if (it->id == removeOp) {
        ops.erase(it);
        break;
      }
    // Deleting one operation cannot change another's toolpath, so drop its
    // entry and regenerate NOTHING. This used to re-cut the whole job.
    DropCamToolpath(state, removeOp);
    dirty = true;
    state.projectMetaDirty = true;
    state.camVerdict.clear();
    state.camLeftover.clear();
    if (state.camGhostOp == removeOp)
      state.camGhostOp = 0;
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
      touched(op.id);
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
    // The picture always matches the parameters — but only the operation
    // that changed is re-cut. `test_cam` pins that one-at-a-time and
    // all-at-once agree, which is what lets this be a cache at all.
    if (changed == 0 || changed == kAllOps)
      RefreshCamToolpaths(state);
    else
      RefreshCamToolpath(state, changed);
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
void RefreshCamToolpath(AppState&, uint64_t) {}
void DropCamToolpath(AppState&, uint64_t) {}
} // namespace ui

#endif
