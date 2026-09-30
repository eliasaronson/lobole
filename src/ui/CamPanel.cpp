#include <algorithm>
#include <cctype>
#include <filesystem>

#include "ui/BackgroundJobs.h"
#include "ui/CamPanel.h"

#include "core/cam/CamEngrave.h"
#include "ui/Icons.h"
#include "ui/Widgets.h"

#include "ui/DockLayout.h" // PanelHeading
#include "ui/PanelTitles.h"
#include <cstdlib>
#include <cstring>

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamMachine.h"
#include "core/cam/CamMount.h"
#include "core/cam/CamNames.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/io/Serialization.h"
#include "ui/CamGenerate.h"
#include "ui/CamJobs.h" // adaptive passes off the frame
#include "ui/CamPlayback.h"
#include "ui/CamTool.h"
#include "ui/DocumentSlots.h"
#include "ui/ExprInput.h"
#include "ui/FeatureEditor.h"
#include "ui/JobActions.h"
#include "ui/ListSelection.h"
#include "ui/MachineActions.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchOverlay.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "imgui_internal.h" // the window's indent, for the op row

#include <cfloat>
#include <cstdio>

namespace ui {

namespace {

// Backplot colours. Cutting moves read as "material comes off here", rapids
// as "the tool flies here" — a rapid through the part is the mistake this
// picture exists to make visible.
// Against a light grey workpiece AND a dark ground. Pale blue managed only
// the second — on the part it sat a hair off the surface it was drawn on.
// Amber is far from grey in hue and from the ground in value, and clear of
// the blues that selection and sketches use; the plunge moves to red so the
// two stay apart.
constexpr glm::vec4 kCutColor(1.00f, 0.55f, 0.08f, 1.00f);
constexpr glm::vec4 kPlungeColor(0.95f, 0.22f, 0.22f, 1.00f);
constexpr glm::vec4 kRapidColor(0.50f, 0.52f, 0.60f, 0.55f);

// Order matches cam::OpType. "Adaptive" is a pocket cut a different way,
// so it reads as a strategy rather than as a separate kind of job.
// In OpType order — the combo writes the index straight back as the enum,
// and indexes it by that enum at three more places, so a row out of step is
// an out-of-bounds read and a row missing is the wrong label on every type
// after it. Asserted rather than trusted.
const char* kOpTypes[] = {
    "Profile",    "Pocket",         "Face",         "Drill",         "Adaptive",
    "Edge break", "Finish surface", "Finish walls", "Finish rotary", "Engrave"};
static_assert(IM_ARRAYSIZE(kOpTypes) == static_cast<int>(cam::OpType::Count),
              "kOpTypes must have one label per OpType, in enum order");

} // namespace

namespace {
// A toolpath that came back pending has no moves of its own yet. Blanking it
// would read as "this operation produces nothing", which is a real state
// the panel already has to express — so it keeps what the previous generate
// drew, and `pending` is what tells post, verify and simulate not to use it
// (docs/ui/long-operations.md#cam-toolpath-passes).
void KeepPreviousMoves(const cam::Toolpath& previous, cam::Toolpath& fresh) {
  if (!fresh.pending || !fresh.moves.empty())
    return;
  fresh.moves = previous.moves;
  fresh.reliefs = previous.reliefs;
  fresh.tabs = previous.tabs;
  fresh.grooves = previous.grooves;
  fresh.strategy = previous.strategy;
}

cam::GenerateContext ContextOf(AppState& state, const cam::Setup& setup) {
  cam::GenerateContext ctx;
  // An engraving's curves are a sketch's, which makes no body: resolved here,
  // where the part documents are (docs/cam/operations.md#engrave).
  ctx.sketchCurves = JobSketchCurves(state, setup);
  ctx.cache = &state.camAdaptiveCache;
  ctx.surfaceCache = &state.camSurfaceCache;
  // The fingerprint the surfacing key needs of the geometry. `JobRenderVersion`
  // is already the mix of the job, its sources, the active setup, the
  // fixturing and every placement -- which is exactly "which bodies, where".
  ctx.bodiesKey = JobRenderVersion(state);
  ctx.outlineCache = &state.camOutlineCache;
  ctx.defer = state.camDeferPasses;
  return ctx;
}
} // namespace

void RefreshCamToolpaths(AppState& state) {
  std::vector<cam::Toolpath> previous = std::move(state.camToolpaths);
  state.camToolpaths.clear();
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr || !state.job)
    return;
  cam::GenerateContext ctx = ContextOf(state, *setup);
  state.camToolpaths = cam::GenerateAll(*setup, ui::JobBodies(state), &ctx);
  for (cam::Toolpath& fresh : state.camToolpaths)
    for (const cam::Toolpath& old : previous)
      if (old.operationId == fresh.operationId && old.finish == fresh.finish)
        KeepPreviousMoves(old, fresh);
  QueueOutlinePasses(state, std::move(ctx.pendingOutline));
  QueueAdaptivePasses(state, std::move(ctx.pending));
  QueueSurfacePasses(state, std::move(ctx.pendingSurface));
}

void RefreshCamToolpath(AppState& state, uint64_t opId) {
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr || !state.job || opId == 0)
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
  if (op->finishTool != 0) {
    // Two toolpaths for one operation, and tool order sorts across them: a
    // full pass is the one place that gets both right.
    RefreshCamToolpaths(state);
    return;
  }
  // An arrangement that cannot be cut -- a sheet longer than the machine, a
  // part off the stock -- is what a FULL pass stamps on every operation
  // (GenerateAll); this path has to say the same, or a verb-made operation
  // on a bad layout generated fine here and only a Generate press refused.
  cam::Toolpath fresh;
  cam::GenerateContext ctx = ContextOf(state, *setup);
  if (const std::string why =
          cam::PlacementRefusal(*setup, ui::JobBodies(state));
      !why.empty()) {
    fresh.operationId = opId;
    fresh.toolNumber = op->toolNumber;
    fresh.error = why;
  } else {
    fresh = cam::Generate(*setup, *op, ui::JobBodies(state), &ctx);
  }
  for (cam::Toolpath& existing : state.camToolpaths)
    if (existing.operationId == opId) {
      KeepPreviousMoves(existing, fresh);
      existing = std::move(fresh);
      QueueOutlinePasses(state, std::move(ctx.pendingOutline));
      QueueAdaptivePasses(state, std::move(ctx.pending));
      QueueSurfacePasses(state, std::move(ctx.pendingSurface));
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
  QueueOutlinePasses(state, std::move(ctx.pendingOutline));
  QueueAdaptivePasses(state, std::move(ctx.pending));
  QueueSurfacePasses(state, std::move(ctx.pendingSurface));
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

// The tool, to scale, where it first enters the material
// (docs/cam/gui.md#what-the-viewport-draws).
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
  // Built in setup space and DRAWN there: a job tab shows its parts laid out
  // (v73/J6), so the viewport is already the space the tool works in. It used
  // to map back to model space, because the viewport used to draw the model
  // — which since J6 undoes exactly the transform the viewport just applied,
  // and floats the picture beside the part it belongs to.
  const auto shown = [&](const glm::dvec3& p) { return glm::vec3(p); };
  (void)setup;
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

namespace {

// The backplot's vertices, cached.
//
// `AppendPrimLine` pushes SIX PrimVertex per move, and this ran in full every
// frame. A 2.5D job is a few thousand moves and nobody noticed; a 3-axis
// finishing pass is not. The waveguide dogfood generates ~260,000 moves, so
// the overlay rebuilt 1.56 MILLION vertices -- about 87 MB -- per frame, and
// measured 133 ms of a 158 ms frame: 6 fps, with the GPU idling at 29 ms and
// 84% of the time in `ui`.
//
// The vertices are a pure function of the toolpaths, so build them once.
// Shared with the renderer, which draws them from its own cached channel
// (docs/cam/gui.md#what-the-viewport-draws): a rebuild makes a NEW list, and
// the one the renderer may still hold is never touched.
std::shared_ptr<const std::vector<PrimVertex>> g_backplot;
uint64_t g_backplotKey = 0;

// A fingerprint, not a revision counter: `camToolpaths` is also mutated IN
// PLACE when a deferred adaptive pass lands or is cancelled, so anything that
// only counts assignments would show a stale path. O(toolpaths), not O(moves)
// -- walking the moves to decide whether to walk the moves would defeat it.
// Is `mid` on the segment a->b, to within a fraction of a millimetre? The
// tolerance is a picture's, not a toolpath's: 10 um is far below one pixel at
// any zoom a person works at, and the G-code is untouched either way.
bool Collinear(const glm::vec3& a, const glm::vec3& mid, const glm::vec3& b) {
  const glm::dvec3 ab = glm::dvec3(b) - glm::dvec3(a);
  const double len = glm::length(ab);
  if (len < 1e-9)
    return true;
  return glm::length(glm::cross(ab, glm::dvec3(mid) - glm::dvec3(a))) / len <
         0.01;
}

// Where a move is, ON THE PART.
//
// Setup space is the MACHINE's frame, and in it a rotary job's tool never
// leaves the plane y = 0: it stands over the axis while the part turns
// underneath. Drawn there, the pin's three hundred and fifty passes lie on
// top of each other in one flat fan along the top of the blank -- which is
// what the machine does and is not what anyone is asking to see.
//
// A move carrying an A is drawn where it lands on the WORK: rotate it back
// by that angle and the passes wrap the part as the flutes they cut. It is
// the same relationship the verifier uses (a part angle appears in setup
// space at theta + A), read the other way.
glm::vec3 OnPart(const cam::Move& m) {
  if (!m.hasA)
    return glm::vec3(m.to);
  const double a = glm::radians(-m.a);
  const double c = std::cos(a), s = std::sin(a);
  return glm::vec3(m.to.x, m.to.y * c - m.to.z * s, m.to.y * s + m.to.z * c);
}

} // namespace

uint64_t CamToolpathsKey(const std::vector<cam::Toolpath>& paths) {
  uint64_t k = 1469598103934665603ull;
  const auto mix = [&k](uint64_t v) { k = (k ^ v) * 1099511628211ull; };
  mix(paths.size());
  for (const cam::Toolpath& tp : paths) {
    mix(tp.operationId);
    mix(tp.moves.size());
    mix(tp.pending ? 1u : 0u);
    mix(tp.error.size());
    // The two ends as well, so a pass REPLACED by one of the same length --
    // which is exactly what re-generating an operation produces -- is not
    // mistaken for the pass it replaced.
    if (!tp.moves.empty()) {
      const auto bits = [](double d) {
        uint64_t u = 0;
        std::memcpy(&u, &d, sizeof u);
        return u;
      };
      mix(bits(tp.moves.front().to.x));
      mix(bits(tp.moves.front().to.z));
      mix(bits(tp.moves.back().to.x));
      mix(bits(tp.moves.back().to.z));
    }
  }
  return k;
}

CamBackplotRef CamBackplot(AppState& state) {
  // While a simulation is open the line drawn is the file being PLAYED
  // (AppendCamToolpathExtras), not this one.
  if (CamPlaybackDrawsPath(state) || !state.camShowToolpath ||
      state.camToolpaths.empty())
    return {};
  // Moves are in SETUP space (C9), and so is the viewport on a job tab
  // (v73/J6) — the parts are drawn laid out. Nothing to convert.
  if (const uint64_t key = CamToolpathsKey(state.camToolpaths);
      key != g_backplotKey || g_backplot == nullptr) {
    auto built = std::make_shared<std::vector<PrimVertex>>();
    for (const cam::Toolpath& tp : state.camToolpaths) {
      for (size_t i = 1; i < tp.moves.size(); i++) {
        const cam::Move& m = tp.moves[i];
        // Collapse a straight RUN into one line, the way the post collapses
        // it into one G1: a finishing pass samples the surface every 0.15 mm,
        // so a gently curving pass arrives as hundreds of segments that are
        // collinear to well under a pixel. The post turns 380,000 moves into
        // 34,000 lines this way; drawing all 380,000 conveys nothing and
        // costs everything.
        size_t j = i;
        while (j + 1 < tp.moves.size() && tp.moves[j + 1].kind == m.kind &&
               Collinear(OnPart(tp.moves[i - 1]), OnPart(tp.moves[j]),
                         OnPart(tp.moves[j + 1])))
          j++;
        const glm::vec4 color = m.kind == cam::Move::Kind::Rapid ? kRapidColor
                                : m.kind == cam::Move::Kind::Plunge
                                    ? kPlungeColor
                                    : kCutColor;
        AppendPrimLine(OnPart(tp.moves[i - 1]), OnPart(tp.moves[j]),
                       m.kind == cam::Move::Kind::Rapid ? 1.0f : 2.4f, color,
                       *built);
        i = j;
      }
    }
    g_backplot = std::move(built);
    g_backplotKey = key;
  }
  return {g_backplot, g_backplotKey};
}

void AppendCamToolpathExtras(AppState& state, std::vector<PrimVertex>& out) {
  // While a simulation is open the line drawn is the file being PLAYED, dimmed
  // behind the cursor — one path on screen, and it is the one being cut.
  if (CamPlaybackDrawsPath(state)) {
    AppendCamPlaybackPath(state, out);
    return;
  }
  if (!state.camShowToolpath || state.camToolpaths.empty())
    return;
  // The tool glyph stays OUT of the cache: it is a handful of vertices and it
  // follows the setup, which can move without the toolpath changing.
  if (const cam::Setup* setup = ActiveCamSetup(state))
    for (const cam::Toolpath& tp : state.camToolpaths)
      AppendToolGlyph(state, *setup, tp, out);
}

void AppendCamToolpathOverlay(AppState& state, std::vector<PrimVertex>& out) {
  // The whole picture in one list -- for a test. The frame hands the backplot
  // to the renderer's cached channel instead of copying it here
  // (docs/cam/gui.md#what-the-viewport-draws).
  if (const CamBackplotRef bp = CamBackplot(state); bp.verts != nullptr)
    out.insert(out.end(), bp.verts->begin(), bp.verts->end());
  AppendCamToolpathExtras(state, out);
}

namespace {

std::string JoinNames(const std::vector<std::string>& names) {
  std::string out;
  for (size_t i = 0; i < names.size(); i++)
    out += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + names[i];
  return out;
}

} // namespace

// DrawCamPanel, the job's tree, is CamTree.cpp.

bool DrawCamOpSettings(AppState& state, cam::Setup& setupRef,
                       cam::Operation& op) {
  // What the docked panel's "Advanced" tree held, moved rather than
  // rewritten: the CAM tool window draws it for the operation being edited
  // (docs/ui/panels.md#cam-job-tab-the-cam-panel).
  cam::Setup* setup = &setupRef;
  bool changed = false;
  const auto touched = [&](uint64_t) { changed = true; };
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
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
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
  if (ExprDragFloat(state, "###top", &top, 0.25f, -1000.0f, 1000.0f, "%.2f")) {
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
  // Per-type from the ONE table (CamNames.h), not a predicate written
  // here. These guards disagreed with the serializer's and the RPC's, and
  // the unconditional ones offered a stepdown on an edge round and tabs
  // on a drill -- controls that moved a number nothing would read.
  if (cam::OpTypeHas(op.type, cam::OpField::Stepdown)) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(55.0f);
    float step = static_cast<float>(op.stepdown);
    if (ExprDragFloat(state, "###stepdown", &step, 0.25f, 0.0f, 100.0f,
                      "%.2f")) {
      op.stepdown = step;
      touched(op.id);
    }
    ImGui::SetItemTooltip("Stepdown per pass (0 = one pass)");
  }
  if (cam::OpTypeHas(op.type, cam::OpField::CornerRelief)) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    const char* reliefLabel =
        op.cornerRelief == cam::CornerRelief::Dogbone ? "dogbone"
        : op.cornerRelief == cam::CornerRelief::TBone ? "T-bone"
                                                      : "corners";
    if (ImGui::BeginCombo("###cam_relief", reliefLabel)) {
      const std::pair<const char*, cam::CornerRelief> choices[] = {
          {"leave the radius", cam::CornerRelief::None},
          {"dogbone", cam::CornerRelief::Dogbone},
          {"T-bone", cam::CornerRelief::TBone}};
      for (const auto& [label, value] : choices)
        if (ImGui::Selectable(label, op.cornerRelief == value)) {
          op.cornerRelief = value;
          touched(op.id);
        }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Inside corners: a round cutter leaves a radius. "
                          "A dogbone clears it along the bisector, a "
                          "T-bone along the longer wall, each by the "
                          "tool's radius -- for a joint that takes a "
                          "square tenon.");
  }
  if (cam::OpTypeHas(op.type, cam::OpField::Finish)) {
    // Rough then finish (R6): a second tool takes the wall in one pass.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    const cam::Tool* finishTool = setup->FindTool(op.finishTool);
    char finishLabel[32];
    std::snprintf(
        finishLabel, sizeof(finishLabel), "%s",
        finishTool != nullptr
            ? ("finish T" + std::to_string(finishTool->number)).c_str()
            : "no finish");
    if (ImGui::BeginCombo("###cam_finish", finishLabel)) {
      if (ImGui::Selectable("no finishing pass", op.finishTool == 0)) {
        op.finishTool = 0;
        touched(op.id);
      }
      for (const cam::Tool& t : setup->tools) {
        if (t.number == op.toolNumber)
          continue;
        char item[64];
        std::snprintf(item, sizeof(item), "finish with T%d %.3g mm", t.number,
                      t.diameter);
        if (ImGui::Selectable(item, op.finishTool == t.number)) {
          op.finishTool = t.number;
          if (const cam::Tool* rough = setup->FindTool(op.toolNumber);
              rough != nullptr && op.finishStock <= 0.0)
            op.finishStock = cam::DefaultFinishStock(*rough);
          touched(op.id);
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Rough with this tool leaving a little on the "
                          "wall, then take it off in one full-depth pass "
                          "with the finishing tool.");
    if (op.finishTool != 0) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(55.0f);
      float leave = static_cast<float>(op.finishStock);
      if (ExprDragFloat(state, "left###cam_finish_stock", &leave, 0.05f, 0.0f,
                        10.0f, "%.2f")) {
        op.finishStock = leave;
        touched(op.id);
      }
      ImGui::SetItemTooltip("What the rough leaves for the finish (mm)");
    }
  }
  if (cam::OpTypeHas(op.type, cam::OpField::LeadArcs)) {
    ImGui::SameLine();
    if (ImGui::Checkbox("Lead arcs###cam_leads", &op.leadArcs))
      touched(op.id);
    ImGui::SetItemTooltip("Leave the wall on a tangent arc of the tool's "
                          "radius instead of stopping against it and "
                          "lifting -- the stop is the dwell mark.");
  }
  if (cam::OpTypeHas(op.type, cam::OpField::Peck)) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(55.0f);
    float peck = static_cast<float>(op.peck);
    if (ExprDragFloat(state, "Peck###peck", &peck, 0.25f, 0.0f, 100.0f,
                      "%.2f")) {
      op.peck = peck;
      touched(op.id);
    }
    ImGui::SetItemTooltip("Retract fully between pecks of this depth so "
                          "the chips clear (0 = one plunge)");
  }
  if (cam::OpTypeHas(op.type, cam::OpField::EdgeSize)) {
    ImGui::SetNextItemWidth(70.0f);
    float size = static_cast<float>(op.edgeSize);
    if (ExprDragFloat(state, "Break###edgesize", &size, 0.1f, 0.0f, 100.0f,
                      "%.2f")) {
      op.edgeSize = size;
      touched(op.id);
    }
    ImGui::SetItemTooltip("Radius of the round (ball) or width of the "
                          "bevel (V-bit), mm. Derived, it is the break the "
                          "model has on that edge; 0 = the biggest this tool "
                          "can make within the wall.");
  }
  // An engraving's groove: its width at the surface OR its depth, one
  // statement (docs/cam/operations.md#engrave). Typing one clears the other,
  // and the bottom follows -- for a V-bit the two are the same number.
  if (cam::OpTypeHas(op.type, cam::OpField::EngraveWidth) ||
      cam::OpTypeHas(op.type, cam::OpField::EngraveDepth)) {
    const cam::Tool* tool = setup->FindTool(op.toolNumber);
    bool edited = false;
    if (tool != nullptr && tool->type == cam::ToolType::VBit) {
      ImGui::SetNextItemWidth(70.0f);
      float width = static_cast<float>(op.engraveWidth);
      if (ExprDragFloat(state, "Width###engravewidth", &width, 0.05f, 0.0f,
                        static_cast<float>(tool->diameter), "%.2f")) {
        op.engraveWidth = width;
        op.engraveDepth = 0.0;
        edited = true;
      }
      ImGui::SetItemTooltip("The groove's width at the surface, mm. A "
                            "V-bit's depth IS its width: the cone reaches "
                            "that width at that depth.");
      ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(70.0f);
    cam::EngraveGroove shown = tool != nullptr
                                   ? cam::EngraveGrooveFor(op, *tool)
                                   : cam::EngraveGroove{};
    float depth = static_cast<float>(shown.ok ? shown.depth : op.engraveDepth);
    if (ExprDragFloat(state, "Depth###engravedepth", &depth, 0.05f, 0.0f,
                      100.0f, "%.2f")) {
      op.engraveDepth = depth;
      op.engraveWidth = 0.0;
      edited = true;
    }
    ImGui::SetItemTooltip("How deep the tip goes below the surface the "
                          "sketch lies on, mm.");
    if (edited && tool != nullptr) {
      const cam::EngraveGroove g = cam::EngraveGrooveFor(op, *tool);
      if (g.ok)
        op.bottomZ = op.topZ - g.depth;
      else
        state.camRefusal = g.why;
      touched(op.id);
    }
  }
  // Surfacing: across, along and down. Three fields because they are
  // three quantities, and the table decides which of them this type has.
  if (cam::OpTypeHas(op.type, cam::OpField::RotaryPattern)) {
    // Radio buttons, for the reason the tool row above is buttons: two
    // options are one click each this way, and a combo gathers with an
    // empty label, so it could not be gated.
    for (const cam::RotaryPatternRow& row : cam::RotaryPatterns()) {
      const std::string label =
          std::string(row.name) + "###pattern_" + row.name;
      if (ImGui::RadioButton(label.c_str(), op.rotaryPattern == row.pattern) &&
          op.rotaryPattern != row.pattern) {
        op.rotaryPattern = row.pattern;
        touched(op.id);
      }
      ImGui::SetItemTooltip(
          "%s", row.pattern == cam::RotaryPattern::Spiral
                    ? "One helix end to end, the part turning WHILE it cuts "
                      "(continuous 4-axis): no seams, and the stepover is the "
                      "spacing between turns, closer on steep walls."
                    : "Passes along the axis, the part turning between them.");
      ImGui::SameLine();
    }
    ImGui::NewLine();
  }
  if (cam::OpTypeHas(op.type, cam::OpField::Lead) &&
      op.rotaryPattern == cam::RotaryPattern::Helical) {
    ImGui::SetNextItemWidth(80.0f);
    float lead = static_cast<float>(op.leadMm);
    if (ExprDragFloat(state, "lead mm###lead", &lead, 1.0f, -10000.0f, 10000.0f,
                      "%.1f")) {
      op.leadMm = lead;
      touched(op.id);
    }
    ImGui::SetItemTooltip(
        "How far along the axis a pass advances per turn, signed for the "
        "hand. 0 follows the part's own twist, measured from its surface. "
        "A part with no twist is cut as parallel passes, and says so.");
    ImGui::SameLine();
  }
  // Cut at several indices
  // (docs/cam/rotary.md#one-operation-at-several-indices): each index a
  // chip, "A90 x", whose x stops cutting there. The indices come from picks
  // -- a floor at another index adds its own (docs/cam/gui.md) -- so the
  // typed list is behind a small "edit", for an angle nobody can pick. A
  // typed list that does not parse is left as it was rather than read as
  // far as it went. Offered only where the machine can turn the part.
  if (cam::OpTypeHas(op.type, cam::OpField::Indexed) &&
      setup->machine.hasRotaryAxis) {
    // Which op's typed list is open: one editor at a time, like the window.
    static uint64_t typingIndicesOf = 0;
    // A row of its own: the field before it may have left the line open.
    if (ImGui::GetCursorPosX() > ImGui::GetCursorStartPos().x + 1.0f)
      ImGui::NewLine();
    ImGui::TextDisabled("at");
    if (op.indexA.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("the setup's own A%g", setup->rotationA);
    }
    size_t removeAt = op.indexA.size();
    for (size_t i = 0; i < op.indexA.size(); i++) {
      ImGui::SameLine();
      char chip[64];
      // Keyed by the angle, not the position: removing one leaves the
      // others' ids where they were.
      std::snprintf(chip, sizeof(chip), "A%g " ICON_FA_XMARK "###index_chip_%g",
                    op.indexA[i], op.indexA[i]);
      if (ImGui::SmallButton(chip))
        removeAt = i;
      ImGui::SetItemTooltip("Stop cutting this operation at A%g", op.indexA[i]);
    }
    if (removeAt < op.indexA.size()) {
      op.indexA.erase(op.indexA.begin() + static_cast<long>(removeAt));
      touched(op.id);
    }
    ImGui::SameLine();
    const bool typing = typingIndicesOf == op.id;
    if (ImGui::SmallButton(typing ? "done###index_edit" : "edit###index_edit"))
      typingIndicesOf = typing ? 0 : op.id;
    ImGui::SetItemTooltip("Type the indices, for an angle no face can be "
                          "picked at");
    if (typingIndicesOf == op.id) {
      std::string text;
      for (const double a : op.indexA) {
        char one[32];
        std::snprintf(one, sizeof(one), "%s%g", text.empty() ? "" : " ", a);
        text += one;
      }
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", text.c_str());
      ImGui::SetNextItemWidth(140.0f);
      if (ImGui::InputText("at A###index_a", buf, sizeof(buf),
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::vector<double> list;
        bool ok = true;
        std::string word;
        const std::string all = std::string(buf) + " ";
        for (const char c : all) {
          if (c == ' ' || c == ',') {
            if (word.empty())
              continue;
            char* end = nullptr;
            const double v = std::strtod(word.c_str(), &end);
            ok = ok && end != nullptr && *end == '\0';
            list.push_back(v);
            word.clear();
          } else {
            word += c;
          }
        }
        if (ok && list != op.indexA) {
          op.indexA = list;
          touched(op.id);
        }
      }
      ImGui::SetItemTooltip(
          "Cut this operation at each of these indices, in degrees -- the "
          "four crenels of a rook are one operation at 0 90 180 270. Empty "
          "cuts at the setup's own index.");
    }
  }
  if (cam::OpTypeHas(op.type, cam::OpField::CutAngle)) {
    ImGui::SetNextItemWidth(70.0f);
    float angle = static_cast<float>(op.cutAngleDeg);
    if (ExprDragFloat(state, "deg###cutangle", &angle, 1.0f, -180.0f, 180.0f,
                      "%.0f")) {
      op.cutAngleDeg = angle;
      touched(op.id);
    }
    ImGui::SetItemTooltip("Raster direction, degrees anticlockwise from "
                          "+X. Run the passes ACROSS the steepest slope, "
                          "not along it.");
    ImGui::SameLine();
  }
  if (cam::OpTypeHas(op.type, cam::OpField::ZStep)) {
    ImGui::SetNextItemWidth(70.0f);
    float zs = static_cast<float>(op.zStep);
    if (ExprDragFloat(state, "Z step###zstep", &zs, 0.05f, 0.0f, 20.0f,
                      "%.2f")) {
      op.zStep = zs;
      touched(op.id);
    }
    ImGui::SetItemTooltip("Millimetres of Z between laps. NOT a stepover: "
                          "a waterline's spacing is a depth. 0 derives it "
                          "from the cutter.");
    ImGui::SameLine();
  }
  if (cam::OpTypeHas(op.type, cam::OpField::SampleStep)) {
    ImGui::SetNextItemWidth(70.0f);
    float ss = static_cast<float>(op.sampleStep);
    if (ExprDragFloat(state, "sample###samplestep", &ss, 0.02f, 0.0f, 5.0f,
                      "%.2f")) {
      op.sampleStep = ss;
      touched(op.id);
    }
    ImGui::SetItemTooltip("Millimetres ALONG a pass, where the surface is "
                          "read. 0 derives it from the cutter and the "
                          "scallop.");
    ImGui::SameLine();
  }
  if (cam::OpTypeHas(op.type, cam::OpField::Stepover)) {
    ImGui::SetNextItemWidth(70.0f);
    float over = static_cast<float>(op.stepover);
    if (ExprDragFloat(state, "Stepover###stepover", &over, 0.01f, 0.05f, 0.9f,
                      "%.2f")) {
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
  if (cam::OpTypeHas(op.type, cam::OpField::StockToLeave)) {
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
  }
  // A hole in the floor is not an island: nothing stands in it, so the
  // cutter should pass over rather than detour around — and a detour
  // around a hole that nearly touches the pocket wall leaves an uncut
  // sliver too narrow for the tool to come back to.
  if (cam::OpTypeHas(op.type, cam::OpField::UseOutline)) {
    if (ImGui::Checkbox("Over openings###useoutline", &op.useOutline))
      touched(op.id);
    ImGui::SetItemTooltip(
        "Clear the face's outline and pass straight OVER the holes in it, "
        "instead of going around each one. Turn it off when something "
        "really does stand in the pocket.");
  }
  if (cam::OpTypeHas(op.type, cam::OpField::Climb)) {
    ImGui::SameLine();
    if (ImGui::Checkbox("Climb###climb", &op.climb))
      touched(op.id);
  }
  if (cam::OpTypeHas(op.type, cam::OpField::Tabs)) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(55.0f);
    int tabs = op.tabCount;
    if (ExprDragInt(state, "Tabs###tabs", &tabs, 0.2f, 0, 32)) {
      op.tabCount = tabs;
      touched(op.id);
    }
    // No longer "(profile only)" in words on an op that is not one: the
    // control is simply not there.
    ImGui::SetItemTooltip("Uncut bridges that hold the part on the last "
                          "pass");
  }
  if (!op.targets.empty())
    ImGui::TextDisabled("boundary: %s", op.targets[0].ref.name.c_str());
  return changed;
}

bool DrawCamToolFields(AppState& state, cam::Tool& t) {
  bool changed = false;
  char name[96];
  std::snprintf(name, sizeof(name), "%s", t.name.c_str());
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  if (ImGui::InputText("Name###tool_name", name, sizeof(name))) {
    t.name = name;
    changed = true;
  }
  ImGui::SetNextItemWidth(70.0f);
  float dia = static_cast<float>(t.diameter);
  if (ExprDragFloat(state, "mm###dia", &dia, 0.1f, 0.1f, 50.0f, "%.3f")) {
    t.diameter = dia;
    changed = true;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0f);
  // How deep this cutter can actually cut. It was drawn on the tool in
  // the simulation and settable nowhere, so every tool in every job
  // carried the 20 mm default whatever was in the spindle -- and the
  // reach finding compares against it.
  float flute = static_cast<float>(t.fluteLength);
  if (ExprDragFloat(state, "flute###flutelen", &flute, 0.5f, 0.5f, 300.0f,
                    "%.1f")) {
    t.fluteLength = flute;
    changed = true;
  }
  ImGui::SetItemTooltip("Usable depth, mm: the fluted part, not the whole "
                        "tool. Cut deeper than this and the shank rubs "
                        "and the holder goes into the work.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0f);
  float feed = static_cast<float>(t.feedCut);
  if (ExprDragFloat(state, "F###feed", &feed, 10.0f, 1.0f, 10000.0f, "%.0f")) {
    t.feedCut = feed;
    changed = true;
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
    changed = true;
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
      changed = true;
    }
    ImGui::SetItemTooltip("INCLUDED angle at the point: 90 for the "
                          "common chamfer bit, 118 for a twist drill.");
  }
  // The holder, on its own row: the two numbers that turn the collar
  // check on. Both zero means nobody said, and the check stays quiet --
  // so this row is empty on every tool until somebody measures theirs,
  // which is exactly the right default for a limit nobody typed.
  ImGui::SetNextItemWidth(70.0f);
  float stickout = static_cast<float>(t.overallLength);
  if (ExprDragFloat(state, "out###overall", &stickout, 0.5f, 0.0f, 400.0f,
                    "%.1f")) {
    t.overallLength = stickout;
    changed = true;
  }
  ImGui::SetItemTooltip("Stickout, mm: tip to the underside of the collet "
                        "nut. With the collar width, cam_verify checks "
                        "the nut against material the cutter never "
                        "touches. 0 = not stated, nothing checked.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0f);
  float collar = static_cast<float>(t.collarDiameter);
  if (ExprDragFloat(state, "collar###collardia", &collar, 0.5f, 0.0f, 120.0f,
                    "%.1f")) {
    t.collarDiameter = collar;
    changed = true;
  }
  ImGui::SetItemTooltip("The collet nut at its widest, mm. Needs the "
                        "stickout beside it to mean anything.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70.0f);
  float shank = static_cast<float>(t.shankDiameter);
  if (ExprDragFloat(state, "shank###shankdia", &shank, 0.1f, 0.0f, 50.0f,
                    "%.2f")) {
    t.shankDiameter = shank;
    changed = true;
  }
  ImGui::SetItemTooltip("How wide the tool is ABOVE its flutes, mm. Wider "
                        "than the cutter (a 3 mm bit on a 6 mm shank, a "
                        "V-bit's body) and no operation may cut deeper than "
                        "the flutes: the shank would be driven into the "
                        "wall. Narrower is a necked tool. 0 = the cutter's "
                        "width all the way up.");
  return changed;
}

void ApplyCamToolEdit(AppState& state) {
  // A tool is shared by every operation that names it, so all of them are
  // re-cut, as the panel's inline fields always did.
  state.projectMetaDirty = true;
  RefreshCamToolpaths(state);
  state.camVerdict.clear();
  state.camLeftover.clear();
}

std::vector<std::string> CamOpsUsingTool(const cam::Setup& setup, int number) {
  std::vector<std::string> names;
  for (const cam::Operation& op : setup.operations)
    if (op.toolNumber == number)
      names.push_back(op.name.empty() ? "an unnamed operation" : op.name);
  return names;
}

bool RemoveCamTool(AppState& state, int number, std::string* why) {
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr) {
    if (why)
      *why = "no setup is active";
    return false;
  }
  const std::vector<std::string> users = CamOpsUsingTool(*setup, number);
  if (!users.empty()) {
    // Refused, not cascaded: deleting the operations with it would lose
    // work, and leaving them pointing at a missing number is the silent
    // substitution the app refuses everywhere else.
    if (why)
      *why = "T" + std::to_string(number) + " is used by " + JoinNames(users) +
             ". Give those operations another tool first.";
    return false;
  }
  auto& tools = setup->tools;
  const auto it =
      std::find_if(tools.begin(), tools.end(),
                   [&](const cam::Tool& t) { return t.number == number; });
  if (it == tools.end()) {
    if (why)
      *why = "no tool T" + std::to_string(number) + " in this setup";
    return false;
  }
  SnapshotJob(state);
  tools.erase(it);
  if (state.camEditTool == number)
    state.camEditTool = 0;
  state.projectMetaDirty = true;
  return true;
}

void ApplyCamOpEdit(AppState& state, uint64_t opId) {
  // One operation changed: re-cut it alone, as the panel's edits always did
  // (`test_cam` pins one-at-a-time against all-at-once), and drop a verdict
  // about the toolpath it replaces.
  state.projectMetaDirty = true;
  RefreshCamToolpath(state, opId);
  state.camVerdict.clear();
  state.camLeftover.clear();
}

int RemoveCamOperations(AppState& state, const std::vector<uint64_t>& ids) {
  if (!ids.empty())
    SnapshotJob(state); // one undo step per removal, however many rows (R7)
  if (!state.job || ids.empty())
    return 0;
  int removed = 0;
  for (const uint64_t id : ids) {
    bool found = false;
    for (cam::Setup& setup : state.job->GetSetups()) {
      auto& ops = setup.operations;
      for (auto it = ops.begin(); it != ops.end(); ++it)
        if (it->id == id) {
          ops.erase(it);
          found = true;
          break;
        }
      if (found)
        break;
    }
    if (!found)
      continue;
    removed++;
    // Deleting one operation cannot change another's toolpath, so drop its
    // entry and regenerate NOTHING. This used to re-cut the whole job.
    DropCamToolpath(state, id);
    if (state.camGhostOp == id)
      state.camGhostOp = 0;
    if (state.camVerbOp == id)
      state.camVerbOp = 0;
    auto& items = state.selection.items;
    items.erase(std::remove_if(items.begin(), items.end(),
                               [id](const SelectionItem& it) {
                                 return it.kind ==
                                            SelectionItem::Kind::Operation &&
                                        it.id == id;
                               }),
                items.end());
  }
  if (removed == 0)
    return 0;
  state.projectMetaDirty = true;
  // A verdict about the PREVIOUS job is worse than no verdict: re-run it
  // against what is left, as the tool window always did.
  RunCamVerification(state);
  return removed;
}

namespace {

// The glyph for an operation type: the glyph of the job toolbar verb that
// makes it (ui/Icons.h), so the row and the button that made it look alike.
// Several verbs make one type (Whole and Profile are both a Profile); the
// row takes the verb that names the type.
const char* OpGlyph(cam::OpType t) {
  switch (t) {
  case cam::OpType::Profile:
    return CommandIcon("job.profile");
  case cam::OpType::Pocket:
  case cam::OpType::Adaptive: // adaptive clearing of a pocket
    return CommandIcon("job.pocket");
  case cam::OpType::Face:
    return CommandIcon("job.face");
  case cam::OpType::Drill:
    return CommandIcon("job.drill");
  case cam::OpType::EdgeRound:
    return CommandIcon("job.edge");
  case cam::OpType::SurfaceParallel:
    return CommandIcon("job.surface");
  case cam::OpType::SurfaceWaterline:
    return CommandIcon("job.walls");
  case cam::OpType::SurfaceRotary:
    return CommandIcon("job.rotary_finish");
  case cam::OpType::Engrave: // the verb's own glyph, as every other type
    return CommandIcon("job.engrave");
  case cam::OpType::Count:
    break;
  }
  return kUnknownIcon;
}

} // namespace

std::string CamOpSummary(const cam::Setup& setup, const cam::Operation& op) {
  const cam::Tool* tool = setup.FindTool(op.toolNumber);
  const double dia = tool != nullptr ? tool->diameter : 0.0;
  char buf[256];
  if (op.type == cam::OpType::SurfaceRotary) {
    // A rotary op has no depth and no climb: it has a pattern, a spacing and
    // -- for the helical pattern -- a lead.
    const double step = std::clamp(op.stepover, 0.02, 0.95) * dia;
    std::snprintf(buf, sizeof(buf), "%s 4-axis, %.3g mm tool, %.2f mm apart",
                  cam::RotaryPatternName(op.rotaryPattern), dia, step);
    std::string out = buf;
    if (op.rotaryPattern == cam::RotaryPattern::Helical) {
      if (op.leadMm != 0.0) {
        std::snprintf(buf, sizeof(buf), ", lead %.1f mm", op.leadMm);
        out += buf;
      } else {
        out += ", lead from the part";
      }
    }
    if (op.stepdown > 0.0) {
      std::snprintf(buf, sizeof(buf), ", layers every %.3g mm", op.stepdown);
      out += buf;
    }
    if (op.stockToLeave > 0.0) {
      std::snprintf(buf, sizeof(buf), ", leaves %.3g mm", op.stockToLeave);
      out += buf;
    }
    return out;
  }
  if (op.type == cam::OpType::SurfaceParallel ||
      op.type == cam::OpType::SurfaceWaterline) {
    std::snprintf(buf, sizeof(buf), "%s finish, %.3g mm tool, %.3g mm deep",
                  op.type == cam::OpType::SurfaceParallel ? "raster"
                                                          : "waterline",
                  dia, op.topZ - op.bottomZ);
    return buf;
  }
  std::string out;
  if (op.type == cam::OpType::Engrave) {
    // A groove: its width and depth are one statement (EngraveGrooveFor),
    // and the labels are what it follows.
    const cam::EngraveGroove g = tool != nullptr
                                     ? cam::EngraveGrooveFor(op, *tool)
                                     : cam::EngraveGroove{};
    std::snprintf(buf, sizeof(buf), "%.3g mm tool, %zu label%s", dia,
                  op.targets.size(), op.targets.size() == 1 ? "" : "s");
    out = buf;
    if (g.ok) {
      std::snprintf(buf, sizeof(buf), ", %.3g mm wide, %.3g mm deep", g.width,
                    g.depth);
      out += buf;
    }
  } else {
    const int passes = static_cast<int>(cam::DepthPasses(op).size());
    std::snprintf(buf, sizeof(buf), "%.3g mm tool, %.3g mm deep in %d pass%s%s",
                  dia, op.topZ - op.bottomZ, passes, passes == 1 ? "" : "es",
                  cam::OpTypeHas(op.type, cam::OpField::Climb)
                      ? (op.climb ? ", climb" : ", conventional")
                      : "");
    out = buf;
  }
  if (!op.indexA.empty()) {
    out += ", at A";
    for (size_t i = 0; i < op.indexA.size(); i++) {
      std::snprintf(buf, sizeof(buf), "%s%g", i == 0 ? "" : "/", op.indexA[i]);
      out += buf;
    }
  }
  return out;
}

std::string CamOpPathLine(const cam::Operation& op, const cam::Toolpath& tp) {
  char buf[160];
  if (tp.pending)
    return tp.moves.empty() ? "cutting..."
                            : "cutting... (showing the previous toolpath)";
  if (!tp.error.empty())
    return "cut nothing: " + tp.error;
  // A rotary op's Z is its distance from the axis, so "down to" is a RADIUS.
  std::snprintf(
      buf, sizeof(buf), "%zu moves, %.0f mm of cutting, down to %s%.2f",
      tp.moves.size(), tp.CutLength(),
      op.type == cam::OpType::SurfaceRotary ? "radius " : "Z", tp.MinZ());
  std::string out = buf;
  if (!tp.strategy.empty())
    out += "\ncut as " + tp.strategy;
  return out;
}

CamOpRowResult CamOperationRow(AppState& state, const cam::Setup& setup,
                               const cam::Operation& op,
                               const std::vector<uint64_t>& order) {
  constexpr SelectionItem::Kind kKind = SelectionItem::Kind::Operation;
  CamOpRowResult out;

  // What its toolpath came to: the one with something to say, else the last.
  const cam::Toolpath* path = nullptr;
  for (const cam::Toolpath& tp : state.camToolpaths)
    if (tp.operationId == op.id && (path == nullptr || !tp.error.empty()))
      path = &tp;
  const bool failed = path != nullptr && !path->error.empty();

  // The glyph, apart from the Selectable so the row's id is the op's --
  // the feature list's layout. Red when it cut nothing: an op that produced
  // no toolpath must not look like one that cut the part.
  const ImVec4 dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
  const ImVec4 red(0.95f, 0.45f, 0.40f, 1.0f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(failed ? red : dim, "%s",
                     failed ? ICON_FA_TRIANGLE_EXCLAMATION : OpGlyph(op.type));
  // SameLine's offset is from the window's edge and ignores ImGui::Indent:
  // under a setup in the panel's tree the name would land on its own glyph.
  const ImGuiWindow* win = ImGui::GetCurrentWindowRead();
  const float indent =
      win->DC.Indent.x - (win->DecoOuterSizeX1 + win->WindowPadding.x -
                          win->Scroll.x); // 0 unindented
  ImGui::SameLine(ImGui::GetFontSize() * 1.8f + indent);

  const std::string name =
      op.name.empty() ? std::string(cam::OpTypeNameOrThrow(op.type)) : op.name;
  if (!op.enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
  if (ImGui::Selectable((name + "###op_row").c_str(),
                        IsSelected(state, kKind, op.id),
                        ImGuiSelectableFlags_AllowDoubleClick |
                            ImGuiSelectableFlags_AllowOverlap)) {
    out.clicked = true;
    if (ImGui::IsMouseDoubleClicked(0))
      out.edit = true;
    else
      // Own kind only: a plain click keeps the faces picked on the job --
      // they are what the verb windows are aimed at.
      ListRowClicked(state, kKind, op.id, order, /*ownKindOnly=*/true);
  }
  if (!op.enabled)
    ImGui::PopStyleColor();

  // Drag to reorder: the order is the cutting sequence, which GenerateAll and
  // the post both walk. The row itself is the handle, as a feature's is.
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
    ImGui::SetDragDropPayload("cam_op", &op.id, sizeof(uint64_t));
    ImGui::Text("%s %s", OpGlyph(op.type), name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("cam_op")) {
      uint64_t from = 0;
      std::memcpy(&from, pl->Data, sizeof(uint64_t));
      if (from != op.id)
        out.droppedHere = from;
    }
    ImGui::EndDragDropTarget();
  }

  // The summary and what came of it, on hover -- the long sentence this row
  // used to BE.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) &&
      ImGui::GetDragDropPayload() == nullptr) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
    ImGui::TextUnformatted(CamOpSummary(setup, op).c_str());
    if (path != nullptr)
      ImGui::TextColored(failed ? red : dim, "%s",
                         CamOpPathLine(op, *path).c_str());
    if (!op.enabled)
      ImGui::TextDisabled("Disabled: not cut, not posted");
    ImGui::TextDisabled("Double-click to edit, drag to reorder");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }

  bool wantDelete = false;
  if (ImGui::BeginPopupContextItem("###op_ctx")) {
    out.clicked = true;
    if (ImGui::MenuItem("Edit###op_edit"))
      out.edit = true;
    if (ImGui::MenuItem(op.enabled ? "Disable###op_enable"
                                   : "Enable###op_enable"))
      out.toggleEnabled = true;
    const bool ghosted = state.camGhostOp == op.id;
    if (ImGui::MenuItem("Show what it removes###op_ghost", nullptr, ghosted))
      state.camGhostOp = ghosted ? 0 : op.id;
    ImGui::Separator();
    ListRowContext(state, kKind, op.id, /*ownKindOnly=*/true);
    const size_t picked = SelectedIds(state, kKind).size();
    if (ImGui::MenuItem(
            (DeleteLabel(picked, "operation") + "###op_delete").c_str()))
      wantDelete = true;
    ImGui::EndPopup();
  }
  out.deleteSelection = wantDelete;

  // The tool, dim, at the right: which cutter is the one thing a list of
  // operations is scanned for.
  if (const cam::Tool* tool = setup.FindTool(op.toolNumber)) {
    char tail[64];
    std::snprintf(tail, sizeof(tail), "T%d %.3g mm", tool->number,
                  tool->diameter);
    const float w = ImGui::CalcTextSize(tail).x;
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), right - w));
    ImGui::TextDisabled("%s", tail);
  }
  // A failure says why right under the row, not only in a tooltip: a job
  // with a dead operation is the thing this list has to make obvious.
  if (failed) {
    ImGui::PushStyleColor(ImGuiCol_Text, red);
    ImGui::Indent(ImGui::GetFontSize() * 1.8f);
    ImGui::TextWrapped("%s", path->error.c_str());
    ImGui::Unindent(ImGui::GetFontSize() * 1.8f);
    ImGui::PopStyleColor();
  }
  return out;
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
void AppendCamToolpathOverlay(AppState&, std::vector<PrimVertex>&) {}
CamBackplotRef CamBackplot(AppState&) { return {}; }
void AppendCamToolpathExtras(AppState&, std::vector<PrimVertex>&) {}
uint64_t CamToolpathsKey(const std::vector<cam::Toolpath>&) { return 0; }
void RefreshCamToolpaths(AppState&) {}
void RefreshCamToolpath(AppState&, uint64_t) {}
void DropCamToolpath(AppState&, uint64_t) {}
int RemoveCamOperations(AppState&, const std::vector<uint64_t>&) { return 0; }
CamOpRowResult CamOperationRow(AppState&, const cam::Setup&,
                               const cam::Operation&,
                               const std::vector<uint64_t>&) {
  return {};
}
std::string CamOpSummary(const cam::Setup&, const cam::Operation&) {
  return {};
}
std::string CamOpPathLine(const cam::Operation&, const cam::Toolpath&) {
  return {};
}
bool DrawCamOpSettings(AppState&, cam::Setup&, cam::Operation&) {
  return false;
}
void ApplyCamOpEdit(AppState&, uint64_t) {}
bool DrawCamToolFields(AppState&, cam::Tool&) { return false; }
void ApplyCamToolEdit(AppState&) {}
std::vector<std::string> CamOpsUsingTool(const cam::Setup&, int) { return {}; }
bool RemoveCamTool(AppState&, int, std::string*) { return false; }
} // namespace ui

#endif
