#include <algorithm>
#include <filesystem>

#include "ui/BackgroundJobs.h"
#include "ui/CamPanel.h"
#include "ui/Widgets.h"

#include "ui/DockLayout.h" // PanelHeading
#include "ui/PanelTitles.h"
#include <cstring>

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamMachine.h"
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
const char* kOpTypes[] = {"Profile",        "Pocket",       "Face",
                          "Drill",          "Adaptive",     "Edge break",
                          "Finish surface", "Finish walls", "Finish rotary"};
static_assert(IM_ARRAYSIZE(kOpTypes) == static_cast<int>(cam::OpType::Count),
              "kOpTypes must have one label per OpType, in enum order");

} // namespace

namespace {
// A toolpath that came back pending has no moves of its own yet. Blanking it
// would read as "this operation produces nothing", which is a real state
// the panel already has to express — so it keeps what the previous generate
// drew, and `pending` is what tells post, verify and simulate not to use it
// (docs/ui/long-operations.md, "R2 and R6").
void KeepPreviousMoves(const cam::Toolpath& previous, cam::Toolpath& fresh) {
  if (!fresh.pending || !fresh.moves.empty())
    return;
  fresh.moves = previous.moves;
  fresh.reliefs = previous.reliefs;
  fresh.tabs = previous.tabs;
  fresh.strategy = previous.strategy;
}

cam::GenerateContext ContextOf(AppState& state) {
  cam::GenerateContext ctx;
  ctx.cache = &state.camAdaptiveCache;
  ctx.surfaceCache = &state.camSurfaceCache;
  // The fingerprint the surfacing key needs of the geometry. `JobRenderVersion`
  // is already the mix of the job, its sources, the active setup, the
  // fixturing and every placement -- which is exactly "which bodies, where".
  ctx.bodiesKey = JobRenderVersion(state);
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
  cam::GenerateContext ctx = ContextOf(state);
  state.camToolpaths = cam::GenerateAll(*setup, ui::JobBodies(state), &ctx);
  for (cam::Toolpath& fresh : state.camToolpaths)
    for (const cam::Toolpath& old : previous)
      if (old.operationId == fresh.operationId && old.finish == fresh.finish)
        KeepPreviousMoves(old, fresh);
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
  cam::GenerateContext ctx = ContextOf(state);
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
std::vector<PrimVertex> g_backplot;
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

uint64_t BackplotKey(const std::vector<cam::Toolpath>& paths) {
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
  // Moves are in SETUP space (C9), and so is the viewport on a job tab
  // (v73/J6) — the parts are drawn laid out. Nothing to convert.
  if (const uint64_t key = BackplotKey(state.camToolpaths);
      key != g_backplotKey) {
    g_backplot.clear();
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
                       g_backplot);
        i = j;
      }
    }
    g_backplotKey = key;
  }
  // Reserved, not grown: this is the largest single thing the overlay
  // appends -- a rotary finishing pass is hundreds of thousands of segments
  // at six 56-byte vertices each -- and letting the vector find that out by
  // reallocating copies it more than once.
  out.reserve(out.size() + g_backplot.size());
  out.insert(out.end(), g_backplot.begin(), g_backplot.end());

  // The tool glyph stays OUT of the cache: it is a handful of vertices and it
  // follows the setup, which can move without the toolpath changing.
  if (const cam::Setup* setup = ActiveCamSetup(state))
    for (const cam::Toolpath& tp : state.camToolpaths)
      AppendToolGlyph(state, *setup, tp, out);
}

namespace {

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

} // namespace

void DrawCamPanel(AppState& state) {
  // NoFocusOnAppearing, like every other panel tabbed into the left node: a
  // window that appears in a shared dock node becomes the SELECTED tab, and
  // this one shipped without the flag — so the first launch after CAM landed
  // put the job list in front of the model tree. Three GUI tests caught it as
  // "unable to hover Model"; a user would have called it "my tree is gone".
  // A JOB tab's panel, and no other's. It used to exist on every kind of
  // tab, saying "open a part and press the CAM button" on the ones it could
  // do nothing for -- a tab in the left node of every part, assembly and
  // drawing, for a document kind that has its own tab (v73). The way into
  // CAM from a part is the part bar's own button.
  if (!state.job)
    return;
  // Every edit this panel makes lands on the undo stack through this one
  // guard (R7): a drag is one step, a click is one step.
  JobUndoGuard undoGuard(state, /*coalesce=*/true);
  if (!ImGui::Begin(kPanelCam, nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
    ImGui::End();
    return;
  }
  PanelHeading(kPanelCam);

  std::vector<cam::Setup>& setups = state.job->GetSetups();
  if (setups.empty()) {
    // The setup STEP (round two, M3): a job is made the way an assembly is
    // -- from the tab bar, empty -- and this is where it becomes a job: the
    // machine, the stock, then parts. A slicer asks printer, plate, objects
    // in that order; FreeCAD's job dialog asks template and models and puts
    // the stock last. The machine goes first because everything after it
    // is measured against it.
    ImGui::TextWrapped("A job cuts parts out of a sheet on a machine.");
    ImGui::Spacing();
    AppState::CamNewSetup& ns = state.camNewSetup;
    // Default: the only machine, or the first -- a choice nobody has to
    // make when the shop has one.
    if (cam::FindMachine(state.machines, ns.machine) == nullptr)
      ns.machine = state.machines.empty() ? "" : state.machines.front().name;
    ImGui::TextDisabled("Machine");
    if (state.machines.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.40f, 1.0f));
      ImGui::TextWrapped("No machine yet. Define one first — post, work "
                         "area, rotary — and every job you set up on it "
                         "follows.");
      ImGui::PopStyleColor();
      if (ImGui::SmallButton("Preferences › Machines###cam_new_machine_prefs"))
        state.settingsWindowOpen = true;
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
    const std::vector<Body>& bodies = ui::JobBodies(state);
    if (ns.fixedSheet) {
      // Derived, not literal (docs/ui/u10-defaults-and-refusals.md): the
      // sheet defaults to the machine's bed in X and Y, and to the parts'
      // height in Z -- a sheet is as thick as what it is cut into. With no
      // parts there is no thickness to derive, so the fields wait.
      const cam::Machine* mc = cam::FindMachine(state.machines, ns.machine);
      if (ns.sheet.x <= 0.0f && mc != nullptr && mc->HasWorkArea()) {
        ns.sheet.x = static_cast<float>(mc->travelMax.x - mc->travelMin.x);
        ns.sheet.y = static_cast<float>(mc->travelMax.y - mc->travelMin.y);
      }
      if (ns.sheet.z <= 0.0f && !bodies.empty()) {
        glm::vec3 lo(FLT_MAX), hi(-FLT_MAX);
        for (const Body& body : bodies) {
          glm::vec3 bmin, bmax;
          body.shape.GetBoundingBox(bmin, bmax);
          lo = glm::min(lo, bmin);
          hi = glm::max(hi, bmax);
        }
        ns.sheet.z = hi.z - lo.z;
      }
      ImGui::SetNextItemWidth(200.0f);
      float sheet[3] = {ns.sheet.x, ns.sheet.y, ns.sheet.z};
      if (ExprDragFloat3(state, "mm###cam_new_sheet", sheet, 1.0f))
        ns.sheet = {sheet[0], sheet[1], sheet[2]};
      if (bodies.empty())
        ImGui::TextDisabled("(thickness follows the parts — add one below)");
    }
    ImGui::Spacing();
    // The step CHANGES the document or REFUSES visibly (u10): no machine is
    // a refusal here, said above, and the button does nothing else.
    const bool canCreate =
        !state.machines.empty() && (!ns.fixedSheet || ns.sheet.x > 0.0f);
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
      // Safe Z clear of the stock, not the struct's bare 5 mm — which sits
      // inside anything taller than that (docs/cam/gui.md).
      fresh.safeZ = cam::DeriveSafeZ(fresh, bodies);
      state.camSafeZEdit = static_cast<float>(fresh.safeZ);
      // CAM edits live in the document but bump no document version (the
      // same as study edits), so this is what keeps the save prompt honest.
      state.projectMetaDirty = true;
      state.camNewSetup = {};
    }
    ImGui::EndDisabled();
    if (!canCreate && !state.machines.empty())
      ImGui::TextDisabled("a sheet needs a size");
    ImGui::Spacing();

    // Parts can come in before the setup: the sheet's thickness is derived
    // from them, and a job with nothing in it has nothing to cut.
    ImGui::TextDisabled("Parts");
    for (const cam::JobPart& part : state.job->GetParts())
      ImGui::BulletText("%s", part.name.c_str());
    for (const ui::PartSlot& slot : state.parts) {
      if (slot.kind != ui::DocKind::Part)
        continue;
      bool held = false;
      for (const cam::JobPart& part : state.job->GetParts())
        held = held || part.sourceDocId == slot.id;
      if (held)
        continue;
      ImGui::PushID(static_cast<int>(slot.id));
      if (ImGui::SmallButton(
              ("Add " + slot.name + "###cam_new_part_add").c_str())) {
        state.job->AddPart(slot.id, slot.name);
        state.job->BumpVersion();
        state.projectMetaDirty = true;
      }
      ImGui::PopID();
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
    cam::Setup& fresh = state.job->AddSetup();
    state.camActiveSetup = fresh.id;
    (void)cam::RecomputeOrigin(fresh, ui::JobBodies(state));
    fresh.safeZ = ui::DeriveJobSafeZ(state, fresh);
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
    cam::Setup& other = state.job->AddSetup(setup->name + " (back)");
    // Carry the MACHINE and the material across — they did not change when
    // the part was turned over. Operations do not come: they are what the
    // other side is for.
    other.frame = cam::FlipFrame(setup->frame);
    other.stock = setup->stock;
    other.tools = setup->tools;
    other.machine.post = setup->machine.post;
    other.machine.fitArcs = setup->machine.fitArcs;
    other.rapidFeed = setup->rapidFeed;
    other.machine.hasRotaryAxis = setup->machine.hasRotaryAxis;
    other.originAt = setup->originAt;
    other.safeZ = ui::DeriveJobSafeZ(state, other);
    (void)cam::RecomputeOrigin(other, ui::JobBodies(state));
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
    state.job->RemoveSetup(setup->id);
    state.camActiveSetup = 0;
    state.camToolpaths.clear();
    state.projectMetaDirty = true;
    ImGui::End();
    return;
  }

  ImGui::SameLine();
  // WHICH MACHINE (round two). The combo lists the store; picking one copies
  // it onto the setup (post, arcs, rotary, work area, and the rapid it
  // seeds). It used to be a post combo defaulting to mach3, which is how
  // every new job re-answered a question the shop answers once.
  ImGui::SetNextItemWidth(120.0f);
  const char* machineLabel =
      setup->machine.name.empty() ? "(job's own)" : setup->machine.name.c_str();
  if (ImGui::BeginCombo("###cam_machine", machineLabel)) {
    for (const cam::Machine& mc : state.machines)
      if (ImGui::Selectable(mc.name.c_str(), mc.name == setup->machine.name)) {
        ApplyMachine(*setup, mc);
        dirty = true;
        changed = kAllOps;
      }
    if (state.machines.empty())
      ImGui::TextDisabled("no machines yet — Preferences › Machines");
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("The machine this setup is for. Picking one copies "
                        "its post, arc fitting, rotary axis and work area "
                        "onto the setup; the job keeps that copy, so it "
                        "opens anywhere.");

  // How the setup's copy stands against the store, SAID. Every reference
  // that embeds a machine profile reconciles it quietly on load; here the
  // differing field is the one that matters -- a bed 100 mm shorter, a post
  // that writes a different arc word.
  switch (cam::MatchMachine(state.machines, setup->machine)) {
  case cam::MachineMatch::Global:
    break;
  case cam::MachineMatch::Unnamed:
    if (state.machines.empty()) {
      ImGui::TextDisabled("No machine: define one in");
      ImGui::SameLine();
      if (ImGui::SmallButton("Preferences › Machines###cam_machine_prefs"))
        state.settingsWindowOpen = true;
    } else {
      ImGui::TextDisabled("No machine chosen — the job's own settings");
    }
    break;
  case cam::MachineMatch::Differs:
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.40f, 1.0f));
    ImGui::TextWrapped("Set up on a different '%s' than yours.",
                       setup->machine.name.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Use mine###cam_machine_use_mine")) {
      ApplyMachine(*setup,
                   *cam::FindMachine(state.machines, setup->machine.name));
      dirty = true;
      changed = kAllOps;
    }
    ImGui::SetItemTooltip("Replace the job's copy with your '%s'",
                          setup->machine.name.c_str());
    ImGui::SameLine();
    break;
  case cam::MachineMatch::Missing:
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.40f, 1.0f));
    ImGui::TextWrapped("'%s' is not one of your machines.",
                       setup->machine.name.c_str());
    ImGui::PopStyleColor();
    break;
  }
  {
    const cam::MachineMatch match =
        cam::MatchMachine(state.machines, setup->machine);
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
  }

  // Z zero and the verbs were here too, once: a "Pick top face" row and a
  // button opening a window that listed the verbs. Both are the job TOOLBAR
  // now (2026-09-02) -- the way in belongs where the part bar's tools are,
  // not third on a row that overflowed a 217 px panel. What stays here is
  // the LIST: setups, parts, stock, tools, operations, the numbers.

  // What this job cuts, and where each part sits in the stock. A job holds
  // more than one part (v74) — four brackets out of one sheet — and until
  // this section existed the only door was the RPC, which makes a feature
  // half-built by this project's own rule.
  if (SectionHeader("Parts###cam_parts")) {
    const std::vector<cam::PlacementFit> fits =
        setup != nullptr ? cam::CheckPlacements(*setup, ui::JobBodies(state))
                         : std::vector<cam::PlacementFit>{};
    uint64_t removePart = 0;
    for (const cam::JobPart& part : state.job->GetParts()) {
      ImGui::PushID(static_cast<int>(part.id));
      const bool missing =
          ui::JobSourceDocument(state, part.sourceDocId) == nullptr;
      if (missing) {
        // Said here rather than left to a toolpath that comes back empty:
        // a job cannot cut what it cannot find.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s — its part is gone", part.name.c_str());
        ImGui::PopStyleColor();
      } else {
        ImGui::TextUnformatted(part.name.c_str());
      }
      if (setup != nullptr) {
        cam::Placement* place = nullptr;
        for (cam::Placement& pl : setup->placements)
          if (pl.jobPartId == part.id)
            place = &pl;
        if (place == nullptr) {
          ImGui::SameLine();
          if (ImGui::SmallButton("Place###cam_place")) {
            cam::Placement fresh;
            fresh.id = state.job->AllocateId();
            fresh.jobPartId = part.id;
            setup->placements.push_back(fresh);
            state.job->BumpVersion();
            dirty = true;
          }
          ImGui::SetItemTooltip("Say where this part sits in the stock");
        } else {
          float xy[2] = {static_cast<float>(place->pos.x),
                         static_cast<float>(place->pos.y)};
          ImGui::SetNextItemWidth(140.0f);
          // Through the expression wrapper, like every other numeric field
          // here: a raw drag reads "10/2" as 10.
          if (ui::ExprDragFloat2(state, "###cam_place_xy", xy, 0.5f)) {
            place->pos = {xy[0], xy[1]};
            state.job->BumpVersion();
            dirty = true;
          }
          ImGui::SetItemTooltip("Where on the stock (X, Y in setup space)");
          ImGui::SameLine();
          float rot = static_cast<float>(place->rotZdeg);
          ImGui::SetNextItemWidth(70.0f);
          if (ui::ExprDragFloat(state, "###cam_place_rot", &rot, 1.0f, -360.0f,
                                360.0f, "%.1f°")) {
            place->rotZdeg = rot;
            state.job->BumpVersion();
            dirty = true;
          }
          ImGui::SetItemTooltip("Spin under the spindle");
          ImGui::SameLine();
          if (ImGui::Checkbox("Cut###cam_place_on", &place->enabled)) {
            state.job->BumpVersion();
            dirty = true;
          }
          ImGui::SetItemTooltip("Off leaves it on the sheet and does not cut "
                                "it");
          for (const cam::PlacementFit& f : fits) {
            if (f.placementId != place->id)
              continue;
            const char* say =
                f.fit == cam::Fit::FullyOutside    ? "not on the stock"
                : f.fit == cam::Fit::PartlyOutside ? "hangs over the edge"
                : !f.overlaps.empty()              ? "on top of another part"
                                                   : nullptr;
            if (say != nullptr) {
              ImGui::PushStyleColor(ImGuiCol_Text,
                                    ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
              ImGui::TextUnformatted(say);
              ImGui::PopStyleColor();
            }
          }
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove###cam_part_remove"))
        removePart = part.id;
      ImGui::SetItemTooltip("Take this part out of the job (its operations go "
                            "with it)");
      ImGui::PopID();
    }
    if (removePart != 0) {
      state.job->RemovePart(removePart);
      state.job->BumpVersion();
      dirty = true;
    }
    // Every part of the project this job does not already hold. Buttons, not
    // a combo: an ImGui combo gathers with an empty label and cannot be
    // gated by a GUI test (docs/cam/stock-and-origin.md W6).
    for (const ui::PartSlot& slot : state.parts) {
      if (slot.kind != ui::DocKind::Part)
        continue;
      bool held = false;
      for (const cam::JobPart& part : state.job->GetParts())
        held = held || part.sourceDocId == slot.id;
      if (held)
        continue;
      ImGui::PushID(static_cast<int>(slot.id));
      if (ImGui::SmallButton(
              ("Add " + slot.name + "###cam_part_add").c_str())) {
        state.job->AddPart(slot.id, slot.name);
        state.job->BumpVersion();
        dirty = true;
      }
      ImGui::PopID();
    }
  }

  // What HOLDS the stock in this setup: clamps, a vise, a fixture plate --
  // a part, or a keep-out box with no part at all
  // (docs/cam/workholding-plan.md). Verify reports every move into one. Per
  // setup, because re-clamping is what a second setup is. Rows in the list
  // style the tools use (docs/ui/consistency-round-plan.md): the numbers are
  // in the tool window, the verbs in the row's menu.
  if (setup != nullptr && SectionHeader("Fixtures###cam_fixtures")) {
    ImGui::PushID("fixtures"); // PushID(int) below shares an id space
    bool changedFixtures = false;
    uint64_t removeFixture = 0;
    static const std::pair<const char*, cam::FixtureSide> kPlaces[] = {
        {"-X side###cam_fixture_to_mx", cam::FixtureSide::MinusX},
        {"+X side###cam_fixture_to_px", cam::FixtureSide::PlusX},
        {"-Y side###cam_fixture_to_my", cam::FixtureSide::MinusY},
        {"+Y side###cam_fixture_to_py", cam::FixtureSide::PlusY},
        {"Centre###cam_fixture_to_centre", cam::FixtureSide::Centre},
        {"Top###cam_fixture_to_top", cam::FixtureSide::Top}};
    for (cam::Fixture& f : setup->fixtures) {
      const std::vector<Body> model = ui::FixtureModelBodies(state, f);
      const bool missing = model.empty();
      ListRowSpec row;
      row.glyph = f.IsBox() ? ICON_FA_BOX : ICON_FA_LOCK;
      row.label = f.name + "###cam_fixture_" + std::to_string(f.id);
      row.selected = state.camEditFixture == f.id;
      row.dimmed = !f.enabled || missing;
      const std::string place =
          missing
              ? std::string("its part is gone")
              : cam::FixturePlaceName(*setup, ui::JobBodies(state), model, f);
      row.tail = !place.empty() ? place : std::string("placed");
      if (!f.enabled && !missing)
        row.tail += ", not checked";
      row.tooltip =
          (f.IsBox() ? std::string("A keep-out box. ") : std::string()) +
          (missing ? "Its part is gone, so nothing is checked against it."
           : f.enabled
               ? "Verify reports every move of the cutter or holder into it."
               : "Drawn, not checked.");
      row.menuId = "###cam_fixture_ctx_" + std::to_string(f.id);
      row.menu = [&] {
        if (ImGui::MenuItem("Edit###cam_fixture_edit"))
          EditCamFixture(state, f.id);
        if (ImGui::MenuItem("Check###cam_fixture_check", nullptr, f.enabled)) {
          f.enabled = !f.enabled;
          changedFixtures = true;
        }
        if (ImGui::BeginMenu("Move to###cam_fixture_move")) {
          for (const auto& [label, side] : kPlaces)
            if (ImGui::MenuItem(label) &&
                cam::PlaceFixture(*setup, ui::JobBodies(state), model,
                                  f.rotZdeg, side, f.pos))
              changedFixtures = true;
          ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Remove###cam_fixture_remove"))
          removeFixture = f.id;
      };
      const ListRowResult res = ListRow(row);
      if (res.doubleClicked)
        EditCamFixture(state, f.id);
    }
    if (removeFixture != 0) {
      std::erase_if(setup->fixtures, [removeFixture](const cam::Fixture& f) {
        return f.id == removeFixture;
      });
      changedFixtures = true;
    }
    // Adding one: a keep-out box, sized from the stock, or any part the job
    // does not CUT. Placed from the stock and its own size -- centred when it
    // is wider than the stock (a vise), else against the next free side.
    const auto add = [&](cam::Fixture f) {
      const std::vector<Body> model = ui::FixtureModelBodies(state, f);
      if (model.empty())
        return;
      const cam::FixtureSide side = cam::DefaultFixtureSide(
          *setup, ui::JobBodies(state), model, f.rotZdeg);
      if (!cam::PlaceFixture(*setup, ui::JobBodies(state), model, f.rotZdeg,
                             side, f.pos))
        return;
      f.id = state.job->AllocateId();
      setup->fixtures.push_back(f);
      changedFixtures = true;
    };
    if (ImGui::SmallButton(ICON_FA_BOX " Keep-out box###cam_fixture_box")) {
      cam::Fixture f;
      f.box = cam::DefaultKeepOutBox(*setup, ui::JobBodies(state));
      f.name = "Clamp " + std::to_string(setup->fixtures.size() + 1);
      add(f);
    }
    ImGui::SetItemTooltip("A box the tool must keep out of -- a clamp, this "
                          "big, here. No part needed.");
    for (const ui::PartSlot& slot : state.parts) {
      if (slot.kind != ui::DocKind::Part)
        continue;
      bool cut = false;
      for (const cam::JobPart& part : state.job->GetParts())
        cut = cut || part.sourceDocId == slot.id;
      if (cut)
        continue;
      ImGui::PushID(static_cast<int>(slot.id));
      ImGui::SameLine();
      if (ImGui::SmallButton(
              (ICON_FA_LOCK " " + slot.name + "###cam_fixture_add").c_str())) {
        cam::Fixture f;
        f.sourceDocId = slot.id;
        f.name = slot.name;
        add(f);
      }
      ImGui::SetItemTooltip("Hold the stock with this part: placed against "
                            "the stock, never cut, checked against the tool");
      ImGui::PopID();
    }
    ImGui::PopID(); // "fixtures"
    if (changedFixtures)
      CamFixturesChanged(state, *setup);
  }

  // The stock, and the rapid height above it. Both are properties of the JOB
  // rather than of an operation, and the facing verb refuses without the
  // first — so it lives where that refusal can send you.
  if (SectionHeader("Stock and safety###cam_stock")) {
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
        if (cam::StockBounds(*setup, ui::JobBodies(state), lo, hi))
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
          cam::StockRefusal(*setup, ui::JobBodies(state));
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
    // The machine's own fields, edited on the JOB'S COPY: changing one here
    // is what makes the setup differ from the named machine, and the row
    // under the setup combo says so.
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
    // Does this machine HAVE a 4th axis? A property of the machine, like the
    // post and whether it fits arcs. Until it is ticked no file can carry an
    // A word — which is the whole point: laying a part flat on the bed used
    // to post `G0 A90` at routers that have no A (docs/cam/setup-frame.md).
    if (ImGui::Checkbox("Has a 4th axis (A)###cam_has_rotary",
                        &setup->machine.hasRotaryAxis)) {
      dirty = true;
      changed = kAllOps;
    }
    ImGui::SetItemTooltip("Tick only for a machine with a rotary table. How "
                          "the part is CLAMPED is \"Lay a face up\" and needs "
                          "no rotary axis.");
    if (setup->machine.hasRotaryAxis) {
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
    (void)cam::RecomputeOrigin(*setup, ui::JobBodies(state));
  } else
    state.camSafeZEdit = static_cast<float>(setup->safeZ);

  // --- tools -------------------------------------------------------------
  if (SectionHeader("Tools###cam_tools")) {
    // Namespaced: a tool number and an operation id are both small ints, and
    // PushID(int) hashes them into the same id space — two rows then share an
    // id and ImGui says so in a modal.
    // One row each, in the feature list's style (ui::ListRow): the fields
    // are edited in the tool window (double-click, or Edit), the way an
    // operation's are, and Delete is in the row's menu -- refused while an
    // operation cuts with the tool (docs/ui/consistency-round-plan.md).
    ImGui::PushID("tools");
    int deleteTool = 0;
    for (const cam::Tool& t : setup->tools) {
      const std::vector<std::string> users = CamOpsUsingTool(*setup, t.number);
      ListRowSpec row;
      row.glyph = CamToolGlyph(t.type);
      row.label = "T" + std::to_string(t.number) + "  " + t.name +
                  "###cam_tool_" + std::to_string(t.number);
      row.selected = state.camEditTool == t.number;
      row.tail = CamToolTail(t);
      row.tooltip = CamToolSummary(t) +
                    (users.empty() ? std::string("\nNo operation uses it.")
                                   : "\nUsed by " + JoinNames(users) + ".");
      row.menuId = "###cam_tool_ctx_" + std::to_string(t.number);
      row.menu = [&state, &t, &users, &deleteTool] {
        if (ImGui::MenuItem("Edit###cam_tool_edit"))
          EditCamTool(state, t.number);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Delete###cam_tool_delete",
                            nullptr, false, users.empty()))
          deleteTool = t.number;
        if (!users.empty() &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Used by %s. Give those operations another tool "
                            "first.",
                            JoinNames(users).c_str());
      };
      const ListRowResult res = ListRow(row);
      if (res.doubleClicked)
        EditCamTool(state, t.number);
    }
    ImGui::PopID(); // "tools"
    if (deleteTool != 0) {
      std::string why;
      if (!RemoveCamTool(state, deleteTool, &why))
        state.camRefusal = why; // cannot happen from the menu; the RPC's rule
    }
    // The machine's library first (M4): the tools the shop owns, minus the
    // ones this setup already has; then a blank one. Copied in, never
    // linked, so a library edit later changes nothing already cut with.
    const auto nextNumber = [&]() {
      int n = 0;
      for (const cam::Tool& t : setup->tools)
        n = std::max(n, t.number);
      return n + 1;
    };
    for (const cam::Tool& lib : setup->machine.library) {
      bool held = false;
      for (const cam::Tool& t : setup->tools)
        held = held || (t.number == lib.number && t.diameter == lib.diameter &&
                        t.type == lib.type);
      if (held)
        continue;
      ImGui::PushID(lib.number);
      char label[96];
      std::snprintf(label, sizeof(label),
                    "Add T%d %.3g mm %s###cam_add_lib_tool", lib.number,
                    lib.diameter, lib.name.c_str());
      if (ImGui::SmallButton(label)) {
        cam::Tool t = lib;
        // Its own number stays unless the setup already uses it.
        for (const cam::Tool& have : setup->tools)
          if (have.number == t.number)
            t.number = nextNumber();
        setup->tools.push_back(t);
        dirty = true;
      }
      ImGui::SetItemTooltip("From the machine's library");
      ImGui::PopID();
    }
    if (ImGui::SmallButton("Add tool###cam_add_tool")) {
      cam::Tool t;
      t.number = nextNumber();
      t.name = "tool " + std::to_string(t.number);
      setup->tools.push_back(t);
      dirty = true;
    }
  }

  // --- the run, in one line ----------------------------------------------
  // Generate, the verdict, the output options and Save G-code are the
  // Generate window's now (CamGenerate.cpp), opened by the toolbar's gears
  // like Simulate: a verb with a result, not a block of this panel that the
  // operations list pushed off the bottom (docs/ui/cam-panel-tidy-plan.md).
  // What stays is one line saying how the run stands, which opens it.
  ImGui::Separator();
  {
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
    const bool busy = CamPassProgressOf(state).running ||
                      BackgroundJobRunning(state, "cam-verify");
    std::string status;
    if (busy)
      status = "Generating...";
    else if (state.camToolpaths.empty())
      status = "Not generated";
    else if (state.camVerdict.empty())
      status = "Generated, not verified";
    else {
      status = "Generated";
      if (!minutes.empty())
        status += " · " + minutes;
      if (bads > 0)
        status +=
            " · " + std::to_string(bads) + " problem" + (bads == 1 ? "" : "s");
      if (warns > 0)
        status += " · " + std::to_string(warns) + " warning" +
                  (warns == 1 ? "" : "s");
    }
    const ImVec4 tint = bads > 0 ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                        : warns > 0
                            ? ImVec4(0.95f, 0.80f, 0.40f, 1.0f)
                            : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    if (ImGui::Selectable(
            (std::string(ICON_FA_GEARS " ") + status + "###cam_run_status")
                .c_str()))
      OpenCamGenerate(state, /*regenerate=*/false);
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Open the Generate window: the verdict, the output "
                          "options and Save G-code");
  }
  // Why Simulate (on the job toolbar) would not start. The transport itself
  // is a floating window over the viewport now (CamPlayback.cpp); it used to
  // be a section here, and this panel was the busiest thing on screen.
  if (!state.camPlaybackOpen && !state.camPlaybackRefusal.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", state.camPlaybackRefusal.c_str());
    ImGui::PopStyleColor();
  }

  // --- operations --------------------------------------------------------
  ImGui::SeparatorText("Operations");
  // Both deferred past the loop, which walks the vector they would change.
  bool deleteSelected = false; // the row menu's Delete (the selection)
  uint64_t moveOp = 0;         // dragged operation …
  size_t moveTo = 0;           // … and where it was dropped
  std::vector<uint64_t> opOrder;
  for (const cam::Operation& op : setup->operations)
    opOrder.push_back(op.id);
  // Rows in the feature list's style (CamOperationRow): the gestures are the
  // part tab's -- click, double-click to edit, drag to reorder, right-click
  // for the rest -- where each row used to carry a drag handle, a checkbox,
  // "show", "x", a sentence and an "Advanced" tree.
  ImGui::PushID("ops");
  for (size_t opIndex = 0; opIndex < setup->operations.size(); opIndex++) {
    cam::Operation& op = setup->operations[opIndex];
    ImGui::PushID(static_cast<int>(op.id));
    const CamOpRowResult row = CamOperationRow(state, *setup, op, opOrder);
    if (row.deleteSelection)
      deleteSelected = true;
    if (row.toggleEnabled) {
      op.enabled = !op.enabled;
      touched(op.id);
    }
    if (row.edit)
      EditCamOperation(state, op.id);
    if (row.droppedHere != 0) {
      moveOp = row.droppedHere;
      moveTo = opIndex;
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
  if (deleteSelected)
    DeleteSelection(state);

  if (ImGui::SmallButton("Add operation###cam_add_op")) {
    // ONE derivation, two entry points: the same engine call the verbs use
    // (docs/cam/gui.md). This path used to start the cut at the MODEL top,
    // which leaves the stock above it uncut and was the last residue of the
    // `2 -> -0` default — and it derived a stepdown from the depth rather
    // than from the cutter. A refusal is shown rather than an operation that
    // cannot cut (the u10 rule).
    const cam::DerivedOp derived = cam::DeriveOperation(
        *setup, ui::JobBodies(state), cam::OpType::Profile, {});
    if (derived.ok) {
      cam::Operation op = derived.op;
      op.id = state.job->AllocateId();
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

bool DrawCamOpSettings(AppState& state, cam::Setup& setupRef,
                       cam::Operation& op) {
  // What the docked panel's "Advanced" tree held, moved rather than
  // rewritten: the CAM tool window draws it for the operation being edited
  // (docs/ui/cam-panel-tidy-plan.md).
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
                          "bevel (V-bit), mm. 0 = the biggest this tool "
                          "can make within the wall.");
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

// The glyph for an operation type: the job toolbar's own where it has one,
// so the row and the button that made it look alike.
const char* OpGlyph(cam::OpType t) {
  switch (t) {
  case cam::OpType::Profile:
    return ICON_FA_VECTOR_SQUARE;
  case cam::OpType::Pocket:
    return ICON_FA_SQUARE_MINUS;
  case cam::OpType::Face:
    return ICON_FA_GRIP_LINES;
  case cam::OpType::Drill:
    return ICON_FA_CIRCLE_DOT;
  case cam::OpType::Adaptive:
    return ICON_FA_WAVE_SQUARE;
  case cam::OpType::EdgeRound:
    return ICON_FA_BORDER_TOP_LEFT;
  case cam::OpType::SurfaceParallel:
    return ICON_FA_BARS;
  case cam::OpType::SurfaceWaterline:
    return ICON_FA_LAYER_GROUP;
  case cam::OpType::SurfaceRotary:
    return ICON_FA_ROTATE;
  case cam::OpType::Count:
    break;
  }
  return ICON_FA_SHAPES;
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
  const int passes = static_cast<int>(cam::DepthPasses(op).size());
  std::snprintf(buf, sizeof(buf), "%.3g mm tool, %.3g mm deep in %d pass%s%s",
                dia, op.topZ - op.bottomZ, passes, passes == 1 ? "" : "es",
                cam::OpTypeHas(op.type, cam::OpField::Climb)
                    ? (op.climb ? ", climb" : ", conventional")
                    : "");
  return buf;
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
  ImGui::SameLine(ImGui::GetFontSize() * 1.8f);

  const std::string name =
      op.name.empty() ? std::string(cam::OpTypeNameOrThrow(op.type)) : op.name;
  if (!op.enabled)
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
  if (ImGui::Selectable((name + "###op_row").c_str(),
                        IsSelected(state, kKind, op.id),
                        ImGuiSelectableFlags_AllowDoubleClick |
                            ImGuiSelectableFlags_AllowOverlap)) {
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
void DrawCamPanel(AppState&) {}
void AppendCamToolpathOverlay(AppState&, std::vector<PrimVertex>&) {}
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
