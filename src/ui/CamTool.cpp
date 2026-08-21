#include "ui/CamTool.h"

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "ui/AppActions.h" // AppendPrimLine/Dot, WorldToScreen
#include "ui/CamPanel.h"
#include "ui/FeatureEditor.h"
#include "ui/PreviewFeature.h"    // BeginToolWindowFrame
#include "ui/SketchInteraction.h" // WorldToScreen (the depth label)

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <cmath>

namespace ui {

namespace {

// The ghost of the material that will disappear, and the candidate under the
// cursor. Warm for "this is coming off", cool for "this is what you would
// pick" — the same language the backplot already speaks.
constexpr glm::vec4 kGhostLine(1.00f, 0.62f, 0.25f, 0.90f);
constexpr glm::vec3 kGhostFill(0.55f, 0.30f, 0.10f);
constexpr glm::vec4 kHoverLine(0.45f, 0.85f, 1.00f, 0.95f);
constexpr glm::vec3 kStockFill(0.32f, 0.36f, 0.44f);
constexpr glm::vec4 kStockLine(0.60f, 0.66f, 0.78f, 0.55f);
constexpr glm::vec4 kLeftoverDot(1.00f, 0.35f, 0.35f, 0.95f);

// A closed loop drawn at one height.
void LoopLines(const std::vector<glm::dvec2>& pts, double z, float halfPx,
               const glm::vec4& color, std::vector<PrimVertex>& out) {
  for (size_t i = 0; i < pts.size(); i++) {
    const glm::dvec2& a = pts[i];
    const glm::dvec2& b = pts[(i + 1) % pts.size()];
    AppendPrimLine(glm::vec3(a.x, a.y, z), glm::vec3(b.x, b.y, z), halfPx,
                   color, out);
  }
}

// The wall a loop sweeps between two heights, as translucent triangles. No
// cap: an arbitrary polygon needs triangulating, and the walls alone already
// read as a volume. (A cap would also hide the part behind it, which is the
// thing the user is trying to look at.)
void LoopWalls(const std::vector<glm::dvec2>& pts, double zLow, double zHigh,
               const glm::vec3& color, std::vector<LineVertex3D>& quads) {
  for (size_t i = 0; i < pts.size(); i++) {
    const glm::dvec2& a = pts[i];
    const glm::dvec2& b = pts[(i + 1) % pts.size()];
    const glm::vec3 a0(a.x, a.y, zLow), a1(a.x, a.y, zHigh);
    const glm::vec3 b0(b.x, b.y, zLow), b1(b.x, b.y, zHigh);
    for (const glm::vec3& p : {a0, b0, b1, a0, b1, a1})
      quads.push_back(LineVertex3D{p, color});
  }
}

// The volume an operation removes, from the face it was given. Pockets and
// profiles both know their own boundary; a facing pass covers the stock, so
// it is drawn from the stock box instead.
void OperationGhost(AppState& state, const cam::Setup& setup,
                    const cam::Operation& op, std::vector<PrimVertex>& prims,
                    std::vector<LineVertex3D>& quads) {
  const std::vector<Body>& bodies = state.document->GetBodies();
  std::vector<cam::Loop> loops;
  if (!op.targets.empty()) {
    const cam::FaceRegion region = cam::PickedFaceRegion(bodies, op.targets[0]);
    if (!region.ok)
      return;
    loops = region.loops;
  } else {
    glm::dvec3 lo(0.0), hi(0.0);
    if (!cam::StockBounds(setup, bodies, lo, hi))
      return;
    cam::Loop rect;
    rect.pts = {{lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
    loops.push_back(rect);
  }
  for (const cam::Loop& loop : loops) {
    // An island is material that STAYS: outline it, but do not ghost it as
    // removed — that is the difference the picture has to get right.
    LoopLines(loop.pts, op.bottomZ, loop.hole ? 1.2f : 2.0f, kGhostLine, prims);
    if (loop.hole)
      continue;
    LoopLines(loop.pts, op.topZ, 1.4f, kGhostLine, prims);
    LoopWalls(loop.pts, op.bottomZ, op.topZ, kGhostFill, quads);
  }
}

const cam::Operation* FindOp(const cam::Setup& setup, uint64_t id) {
  for (const cam::Operation& op : setup.operations)
    if (op.id == id)
      return &op;
  return nullptr;
}

// The verb buttons: each is one sentence a person would say out loud, and
// each resolves to one click on the model.
struct Verb {
  const char* label;
  const char* id;
  cam::OpType type;
  PickSlot slot;
  const char* prompt;
  const char* tip;
};

constexpr Verb kVerbs[] = {
    {"Clear a pocket", "###cam_verb_pocket", cam::OpType::Pocket,
     PickSlot::CamPocketFloor, "Click the floor of the pocket to clear",
     "Click the flat bottom of the pocket. Its walls become the boundary and "
     "its height becomes the depth."},
    {"Cut it out", "###cam_verb_profile", cam::OpType::Profile,
     PickSlot::CamProfileOutline, "Click the face whose outline to cut around",
     "Cut the part free of the stock, with tabs so it does not break loose "
     "under the cutter."},
};

} // namespace

void ArmCamPick(AppState& state, PickSlot slot, const char* prompt) {
  state.camRefusal.clear();
  state.slotPick = {kCamToolSlot, static_cast<int>(slot),
                    SelectionKindBit(SelectionItem::Kind::Face),
                    /*multi=*/false, prompt};
}

namespace {

// Creates the operation a verb implies, or refuses in words. The derivation
// itself is engine-side (cam::DeriveOperation) so a headless test can reach
// it — the panel must not be where a depth comes from.
bool CreateVerbOperation(AppState& state, cam::OpType type,
                         const ElementRef& target) {
  state.camRefusal.clear();
  if (!state.document)
    return false;
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr) {
    // A verb with no setup should make one rather than tell the user to.
    cam::Setup& fresh = state.document->AddCamSetup();
    state.camActiveSetup = fresh.id;
    fresh.safeZ = cam::DeriveSafeZ(fresh, state.document->GetBodies());
    state.camSafeZEdit = static_cast<float>(fresh.safeZ);
    setup = &fresh;
  }
  const cam::DerivedOp derived =
      cam::DeriveOperation(*setup, state.document->GetBodies(), type, target);
  if (!derived.ok) {
    state.camRefusal = derived.why;
    return false;
  }
  cam::Operation op = derived.op;
  op.id = state.document->AllocateId();
  setup->operations.push_back(op);
  state.camGhostOp = op.id;
  state.projectMetaDirty = true;
  RefreshCamToolpaths(state);
  RunCamVerification(state);
  LOG_INFO("CAM: {} from a picked face, Z {:.3f} to {:.3f}", op.name, op.topZ,
           op.bottomZ);
  return true;
}

} // namespace

void CamFacePicked(AppState& state, const SelectionItem& item) {
  const PickSlot slot = static_cast<PickSlot>(state.slotPick.slot);
  if (!item.element.IsSet() || !state.document) {
    state.slotPick.miss = "that is not a face this job can use";
    return;
  }

  if (slot == PickSlot::CamTopFace) {
    cam::Setup* setup = ActiveCamSetup(state);
    if (setup == nullptr) {
      state.slotPick.miss = "add a setup first";
      return;
    }
    const cam::FaceRegion region =
        cam::PickedFaceRegion(state.document->GetBodies(), item.element);
    if (!region.ok) {
      state.slotPick.miss = region.why;
      return;
    }
    // Z zero is this face. The stock top follows it, and every operation's
    // default top comes from there — so nobody types a depth again.
    setup->origin = glm::dvec3(0.0, 0.0, region.z);
    setup->stock.margin.z = 0.0; // the picked face IS the top of the stock
    state.camTopFace = item.element;
    // The stock just changed shape, so the rapid height that cleared the old
    // one may now be inside the new one.
    setup->safeZ = cam::DeriveSafeZ(*setup, state.document->GetBodies());
    state.camSafeZEdit = static_cast<float>(setup->safeZ);
    for (cam::Operation& op : setup->operations)
      if (op.topZ > region.z)
        op.topZ = region.z;
    state.slotPick = {};
    state.projectMetaDirty = true;
    RefreshCamToolpaths(state);
    RunCamVerification(state);
    return;
  }

  if (slot == PickSlot::CamIndexFace) {
    // C9: turn the table until this face looks at the spindle. Everything
    // downstream then works in the rotated frame, so the same verbs apply to
    // the side of a part as to its top.
    cam::Setup* setup = ActiveCamSetup(state);
    if (setup == nullptr) {
      state.slotPick.miss = "add a setup first";
      return;
    }
    glm::vec3 centre(0.0f), normal(0.0f), uAxis(0.0f);
    bool found = false;
    for (const Body& body : state.document->GetBodies()) {
      if (body.createdBy != item.element.featureId)
        continue;
      const std::optional<int> idx = body.elementMap.Resolve(item.element.name);
      if (idx && body.shape.FacePlane(*idx, centre, normal, uAxis))
        found = true;
      break;
    }
    if (!found) {
      state.slotPick.miss = "pick a flat face — that one is curved";
      return;
    }
    double degrees = 0.0;
    if (!cam::SolveIndexA(glm::dvec3(normal), degrees)) {
      state.slotPick.miss = "no rotation about X can bring that face up — it "
                            "points along the rotary axis";
      return;
    }
    setup->rotationA = degrees;
    setup->safeZ = cam::DeriveSafeZ(*setup, state.document->GetBodies());
    state.camSafeZEdit = static_cast<float>(setup->safeZ);
    state.slotPick = {};
    state.projectMetaDirty = true;
    RefreshCamToolpaths(state);
    RunCamVerification(state);
    LOG_INFO("CAM: indexed to A{:.3f}", degrees);
    return;
  }

  const cam::OpType type = slot == PickSlot::CamPocketFloor
                               ? cam::OpType::Pocket
                               : cam::OpType::Profile;
  // A refusal keeps the slot armed: the user's next click is almost always
  // the face they meant, and re-pressing the verb first is a wasted step.
  if (CreateVerbOperation(state, type, item.element))
    state.slotPick = {};
  else
    state.slotPick.miss = state.camRefusal;
}

void DrawCamToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "CAM");
  if (!state.document) {
    ImGui::TextWrapped("CAM cuts a part. Switch to a part tab.");
    if (ImGui::Button("Done###cam_tool_done"))
      state.camToolOpen = false;
    ImGui::End();
    return;
  }

  const cam::Setup* setup = ActiveCamSetup(state);
  const bool armed = state.slotPick.featureId == kCamToolSlot;

  // Z zero first, and loudly, because it is the number that breaks tools.
  ImGui::TextDisabled("Zero");
  const bool armedTop =
      armed && state.slotPick.slot == static_cast<int>(PickSlot::CamTopFace);
  ImGui::BeginDisabled(armed && !armedTop);
  if (ImGui::Button(state.camTopFace.IsSet()
                        ? "Re-pick top face###cam_tool_topface"
                        : "Pick top face###cam_tool_topface"))
    ArmCamPick(state, PickSlot::CamTopFace,
               "Click the top face — that sets Z zero for the job");
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (armedTop)
    ImGui::TextDisabled("click the top face…");
  else if (state.camTopFace.IsSet() && setup != nullptr)
    ImGui::TextDisabled("Z zero at %.3f", setup->origin.z);
  else
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                       "Z zero is not set yet");

  const bool armedIndex =
      armed && state.slotPick.slot == static_cast<int>(PickSlot::CamIndexFace);
  ImGui::BeginDisabled(armed && !armedIndex);
  if (ImGui::Button("Turn a face up###cam_index_face"))
    ArmCamPick(state, PickSlot::CamIndexFace,
               "Click the face to turn up under the spindle");
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("Rotary (4th axis) jobs: index the table so this face "
                        "points at the spindle, then use the verbs as usual");
  ImGui::SameLine();
  if (armedIndex)
    ImGui::TextDisabled("pick a face…");
  else if (setup != nullptr && std::abs(setup->rotationA) > 1e-9)
    ImGui::TextDisabled("A %.2f°", setup->rotationA);
  else
    ImGui::TextDisabled("A 0° (3-axis)");

  ImGui::Separator();
  ImGui::TextDisabled("Machine it");
  for (const Verb& verb : kVerbs) {
    const bool armedHere =
        armed && state.slotPick.slot == static_cast<int>(verb.slot);
    ImGui::BeginDisabled(armed && !armedHere);
    if (ImGui::Button((std::string(verb.label) + verb.id).c_str(),
                      ImVec2(190.0f, 0.0f)))
      ArmCamPick(state, verb.slot, verb.prompt);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", verb.tip);
    if (armedHere) {
      ImGui::SameLine();
      ImGui::TextDisabled("pick a face…");
    }
  }
  // The two verbs that need no pick at all.
  ImGui::BeginDisabled(armed);
  if (ImGui::Button("Cut out the whole part###cam_verb_whole",
                    ImVec2(190.0f, 0.0f)))
    CreateVerbOperation(state, cam::OpType::Profile, {});
  ImGui::SetItemTooltip("Follow the part's own outline — no pick needed");
  if (ImGui::Button("Face the top###cam_verb_face", ImVec2(190.0f, 0.0f)))
    CreateVerbOperation(state, cam::OpType::Face, {});
  ImGui::SetItemTooltip("Skim the material sitting above the part");
  ImGui::EndDisabled();

  if (!state.camRefusal.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f), "%s",
                       state.camRefusal.c_str());

  // What the job is, in one sentence per operation — the numbers are derived,
  // so they are shown rather than asked for. The drag fields still exist, in
  // the CAM panel, for anyone who wants them.
  if (setup != nullptr && !setup->operations.empty()) {
    ImGui::Separator();
    for (const cam::Operation& op : setup->operations) {
      const cam::Tool* tool = setup->FindTool(op.toolNumber);
      const double depth = op.topZ - op.bottomZ;
      const int passes = static_cast<int>(cam::DepthPasses(op).size());
      ImGui::PushID(static_cast<int>(op.id));
      const bool ghosted = state.camGhostOp == op.id;
      if (ImGui::RadioButton("###ghost", ghosted))
        state.camGhostOp = ghosted ? 0 : op.id;
      ImGui::SetItemTooltip("Show what this one removes");
      ImGui::SameLine();
      ImGui::Text("%s: %.3g mm tool, %.3g mm deep in %d pass%s, %s",
                  op.name.c_str(), tool != nullptr ? tool->diameter : 0.0,
                  depth, passes, passes == 1 ? "" : "es",
                  op.climb ? "climb" : "conventional");
      ImGui::PopID();
    }
  }

  ImGui::Separator();
  if (ImGui::Button("Generate###cam_tool_generate")) {
    RefreshCamToolpaths(state);
    RunCamVerification(state);
  }
  ImGui::SameLine();
  ImGui::Checkbox("Show path###cam_tool_show", &state.camShowToolpath);
  ImGui::SameLine();
  ImGui::Checkbox("Stock###cam_tool_stock", &state.camShowStock);

  for (const cam::VerdictLine& line : state.camVerdict) {
    const ImVec4 color = line.level == cam::VerdictLine::Level::Bad
                             ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                         : line.level == cam::VerdictLine::Level::Warn
                             ? ImVec4(0.95f, 0.80f, 0.40f, 1.0f)
                             : ImVec4(0.55f, 0.90f, 0.60f, 1.0f);
    ImGui::PushTextWrapPos(320.0f);
    // The icon font is merged into the UI atlas, so these are real glyphs;
    // a literal U+2713 would not be (the text font stops at Latin-1).
    ImGui::TextColored(color, "%s %s",
                       line.level == cam::VerdictLine::Level::Good
                           ? ICON_FA_CIRCLE_CHECK
                           : ICON_FA_TRIANGLE_EXCLAMATION,
                       line.text.c_str());
    ImGui::PopTextWrapPos();
  }

  ImGui::Separator();
  if (ImGui::Button("Done###cam_tool_done")) {
    // The session closes; the job stays, and so does the backplot. Only the
    // decision-time drawings go away.
    state.camToolOpen = false;
    state.camGhostOp = 0;
    state.camRefusal.clear();
    if (state.slotPick.featureId == kCamToolSlot)
      state.slotPick = {};
  }
  ImGui::End();
}

void AppendCamVisualisation(AppState& state, std::vector<PrimVertex>& prims,
                            std::vector<LineVertex3D>& quads) {
  if (!state.document)
    return;
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;
  const bool session = state.camToolOpen;

  // The stock, so "where is Z zero and how much material is above the part"
  // stops being invisible. Only while the session is open: outside it, the
  // box is clutter over the model you are drawing.
  glm::dvec3 lo(0.0), hi(0.0);
  if (session && state.camShowStock &&
      cam::StockBounds(*setup, state.document->GetBodies(), lo, hi)) {
    const std::vector<glm::dvec2> rect = {
        {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
    LoopLines(rect, lo.z, 1.0f, kStockLine, prims);
    LoopLines(rect, hi.z, 1.2f, kStockLine, prims);
    for (const glm::dvec2& c : rect)
      AppendPrimLine(glm::vec3(c.x, c.y, lo.z), glm::vec3(c.x, c.y, hi.z), 1.0f,
                     kStockLine, prims);
    LoopWalls(rect, lo.z, hi.z, kStockFill, quads);
  }

  // What the selected operation removes.
  if (session && state.camGhostOp != 0)
    if (const cam::Operation* op = FindOp(*setup, state.camGhostOp))
      OperationGhost(state, *setup, *op, prims, quads);

  // The candidate under the cursor, BEFORE the click: the region that would
  // be cleared, at the height it would be cleared to.
  if (session && state.slotPick.featureId == kCamToolSlot &&
      state.pick.hoverFace.IsSet()) {
    const cam::FaceRegion region = cam::PickedFaceRegion(
        state.document->GetBodies(), state.pick.hoverFace);
    if (region.ok)
      for (const cam::Loop& loop : region.loops)
        LoopLines(loop.pts, region.z, 2.4f, kHoverLine, prims);
  }

  // Where the tool could not reach (G4). Outlives the session, because it is
  // a fact about the generated job rather than about a decision in progress.
  for (const glm::dvec3& p : state.camLeftover)
    AppendPrimDot(glm::vec3(p), 2.0f, kLeftoverDot, prims);
}

void DrawCamLabels(AppState& state) {
  if (!state.camToolOpen || !state.document || state.camGhostOp == 0)
    return;
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;
  const cam::Operation* op = FindOp(*setup, state.camGhostOp);
  if (op == nullptr || op->targets.empty())
    return;
  const cam::FaceRegion region =
      cam::PickedFaceRegion(state.document->GetBodies(), op->targets[0]);
  if (!region.ok || region.loops.empty() || region.loops[0].pts.empty())
    return;

  // At the centroid of the boundary, halfway up the cut: the number sits ON
  // the material it describes.
  glm::dvec2 centre(0.0);
  for (const glm::dvec2& p : region.loops[0].pts)
    centre += p;
  centre /= static_cast<double>(region.loops[0].pts.size());
  const glm::vec2 at = WorldToScreen(
      state, glm::vec3(centre.x, centre.y, (op->topZ + op->bottomZ) * 0.5));
  char text[64];
  std::snprintf(text, sizeof(text), "%.3g mm deep", op->topZ - op->bottomZ);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 size = ImGui::CalcTextSize(text);
  dl->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 3.0f),
                    ImVec2(at.x + size.x + 4.0f, at.y + size.y + 3.0f),
                    IM_COL32(30, 24, 16, 220), 3.0f);
  dl->AddText(ImVec2(at.x, at.y), IM_COL32(255, 190, 120, 255), text);
}

void RunCamVerification(AppState& state) {
  state.camVerdict.clear();
  state.camLeftover.clear();
  if (!state.document || state.camToolpaths.empty())
    return;
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;

  std::string gcode, error;
  if (!cam::PostProcess(setup->post, *setup, state.camToolpaths, {}, gcode,
                        &error)) {
    state.camVerdict.push_back(
        {cam::VerdictLine::Level::Bad, "Could not post this job: " + error});
    return;
  }
  // 0.4 mm cells: fine enough to catch a gouge that matters on a mill, coarse
  // enough that the check runs while the user is still looking at the button.
  constexpr double kRes = 0.4;
  const cam::Inspection insp =
      cam::Inspect(gcode, *setup, state.document->GetBodies(), kRes);
  if (!insp.ok)
    return;
  // The tolerance IS the grid: a "gouge" of half a cell is quantisation, and
  // reporting it would teach the user to ignore the verdict.
  const double tolerance = kRes * 1.5;
  state.camVerdict = cam::Explain(
      insp.verdict, tolerance, cam::CuttingMinutes(*setup, state.camToolpaths));
  state.camLeftover = cam::LeftoverPoints(insp, tolerance, 3000);
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
void DrawCamToolWindow(AppState&) {}
void ArmCamPick(AppState&, PickSlot, const char*) {}
void CamFacePicked(AppState&, const SelectionItem&) {}
void AppendCamVisualisation(AppState&, std::vector<PrimVertex>&,
                            std::vector<LineVertex3D>&) {}
void DrawCamLabels(AppState&) {}
void RunCamVerification(AppState&) {}
} // namespace ui

#endif
