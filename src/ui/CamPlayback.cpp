#include "ui/CamPlayback.h"

#ifdef HAVE_CAM

#include "core/Document.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamStockMesh.h"
#include "core/cam/CamToolpath.h"
#include "render/MeshRenderer.h"
#include "render/StockRenderer.h"
#include "ui/AppActions.h"
#include "ui/CamPanel.h" // ActiveCamSetup, PostOpts
#include "ui/JobActions.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>

namespace ui {

namespace {

// Animation only, never the clock. A rapid's real speed is a fact about the
// machine that the setup usually does not state (`Setup::rapidFeed` is 0 by
// default and `CuttingMinutes` leaves it out for exactly that reason), so
// this number moves the picture and is kept away from every number the panel
// reports.
constexpr double kDisplayRapidFeed = 3000.0; // mm/min

// A stall — a slow re-mesh, a dragged window, a breakpoint — must not teleport
// the tool through the part.
constexpr double kMaxStepSeconds = 0.25;

// The backplot has to read against BOTH the light grey of a workpiece and the
// dark ground behind it, and pale blue only manages the second — on the part
// it is a hair off the surface it is drawn on. Amber sits far from grey in
// hue and from the dark ground in value, and away from the blues selection
// and sketches use.
// Done is the one that has to sit ON the workpiece, and the workpiece got
// brighter when the stock started reading the scene's light rig — a 0.40-value
// line at 0.40 alpha composited to 0.63 against a 0.78 surface, 0.15 apart and
// invisible. Dark and nearly opaque instead: ~0.30 against the stock, half a
// unit of contrast. It gives up some of its reading against the dark ground,
// which is the right way round — a done move above the part is the one thing
// on screen that is meant to recede.
constexpr glm::vec4 kDoneColor(0.20f, 0.22f, 0.30f, 0.85f);
constexpr glm::vec4 kAheadCut(1.00f, 0.55f, 0.08f, 1.00f);
constexpr glm::vec4 kAheadRapid(0.50f, 0.52f, 0.60f, 0.55f);

// Setup space -> model space, as a matrix: the renderers want it per draw
// rather than per point. The rotation itself is `cam::SetupToModel` — this
// used to build its own from `rotationA`, which is one of the four places
// that quietly assumed a setup could only be turned about X.
glm::mat4 SetupModelMatrix(const cam::Setup& setup) {
  const glm::dmat3 r = cam::SetupToModel(setup);
  glm::mat4 m(1.0f);
  for (int col = 0; col < 3; col++)
    for (int row = 0; row < 3; row++)
      m[col][row] = static_cast<float>(r[col][row]);
  return m;
}

std::vector<Vertex3D> ToMeshVertices(const cam::StockMesh& m) {
  std::vector<Vertex3D> out;
  out.reserve(m.vertices.size());
  for (const cam::StockVertex& v : m.vertices)
    out.push_back({v.position, v.normal});
  return out;
}

// One move forward / back. Landing on the END of a segment is what "step"
// means here: the move has happened, and its cut is on the block.
cam::SimCursor StepForward(cam::SimCursor at, const cam::Timeline& tl) {
  if (at.t < 1.0)
    return {at.seg, 1.0};
  return at.seg + 1 < tl.Segments() ? cam::SimCursor{at.seg + 1, 1.0}
                                    : tl.End();
}

cam::SimCursor StepBack(cam::SimCursor at) {
  if (at.t > 0.0)
    return {at.seg, 0.0};
  return at.seg > 0 ? cam::SimCursor{at.seg - 1, 0.0} : cam::SimCursor{0, 0.0};
}

void Tip(const char* text) {
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", text);
}

void DropToolInstance(AppState& state) {
  if (state.meshRenderer && state.camToolInstance != 0)
    state.meshRenderer->RemoveMesh(state.camToolInstance);
  state.camToolInstance = 0;
  state.camToolInstanceFor = -1;
}

} // namespace

bool BeginCamPlayback(AppState& state) {
  state.camPlaybackRefusal.clear();
  if (!state.job) {
    state.camPlaybackRefusal = "A simulation needs a part. Switch to a part "
                               "tab.";
    return false;
  }
  cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr) {
    state.camPlaybackRefusal = "There is no setup to simulate yet.";
    return false;
  }
  if (state.camToolpaths.empty()) {
    state.camPlaybackRefusal =
        "Nothing has been generated to simulate. Generate the job first.";
    return false;
  }

  // The POSTED FILE, like the verification: a bug in the post, in modal
  // state or in linking is invisible to anything that plays a
  // std::vector<Move>, and one of those had already shipped (a NaN retract).
  std::string gcode, error;
  if (!cam::PostProcess(setup->post, *setup, state.camToolpaths,
                        PostOpts(*setup), gcode, &error)) {
    state.camPlaybackRefusal = "Could not post this job: " + error;
    return false;
  }

  glm::dvec3 lo(0.0), hi(0.0);
  if (!cam::StockBounds(*setup, ui::JobBodies(state), lo, hi)) {
    state.camPlaybackRefusal =
        "This setup has no stock to cut — the model has no size.";
    return false;
  }

  state.camPlaybackGcode = gcode;
  state.camTimeline = cam::BuildTimeline(
      cam::ProgramInSetupSpace(gcode, *setup), setup->rapidFeed);
  if (state.camTimeline.Empty()) {
    state.camPlaybackRefusal = "The posted file contains no moves to simulate.";
    state.camPlaybackGcode.clear();
    return false;
  }
  state.camStockSim.Begin(state.camTimeline, *setup,
                          cam::MakeStock(lo, hi, state.camSimRes));
  state.camCursor = {0, 0.0};
  state.camPlaybackOpen = true;
  state.camPlaying = false;
  state.camStockDrawnRevision = 0;
  return true;
}

void EndCamPlayback(AppState& state) {
  state.camPlaybackOpen = false;
  state.camPlaying = false;
  state.camStockSim.Clear();
  state.camTimeline = cam::Timeline{};
  state.camPlaybackGcode.clear();
  state.camCursor = {0, 0.0};
  state.camStockDrawnRevision = 0;
  if (state.stockRenderer)
    state.stockRenderer->Clear();
  DropToolInstance(state);
  // The bodies were ghosted in place; a resync puts their real materials
  // back. Same move ExitSimulation makes, for the same reason.
  state.syncedDocumentVersion = UINT64_MAX;
}

void SeekCamPlayback(AppState& state, cam::SimCursor to) {
  if (!state.camPlaybackOpen)
    return;
  state.camStockSim.SeekTo(to);
  state.camCursor = state.camStockSim.At();
}

void TickCamPlayback(AppState& state, double now) {
  if (!state.camPlaybackOpen)
    return;
  // A document edit invalidates the playback rather than re-cutting under the
  // cursor: the toolpaths it was posted from no longer describe this model,
  // and regenerating here would put a multi-second freeze inside the frame
  // loop (docs/known-bugs.md).
  if (state.camToolpaths.empty()) {
    EndCamPlayback(state);
    return;
  }
  if (!state.camPlaying) {
    state.camPlaybackLastTick = now;
    return;
  }
  const double dt =
      std::clamp(now - state.camPlaybackLastTick, 0.0, kMaxStepSeconds);
  state.camPlaybackLastTick = now;
  if (dt <= 0.0)
    return;

  const cam::GMove* move = state.camTimeline.MoveAt(state.camCursor);
  const double feed = move == nullptr     ? kDisplayRapidFeed
                      : move->rapid       ? kDisplayRapidFeed
                      : move->feed > 1e-9 ? move->feed
                                          : kDisplayRapidFeed;
  const double mm = feed / 60.0 * state.camPlaybackSpeed * dt;
  const double at = state.camTimeline.DistanceAt(state.camCursor) + mm;
  if (at >= state.camTimeline.totalDistance) {
    SeekCamPlayback(state, state.camTimeline.End());
    state.camPlaying = false; // the end is a stop, not a loop
    return;
  }
  SeekCamPlayback(state, state.camTimeline.CursorAtDistance(at));
}

void SyncCamPlayback(AppState& state) {
  if (!state.camPlaybackOpen)
    return;
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr || !state.stockRenderer) {
    return;
  }
  const glm::mat4 model = SetupModelMatrix(*setup);

  // The stock, re-meshed only when the simulation actually moved.
  if (state.camStockSim.Revision() != state.camStockDrawnRevision) {
    state.camStockDrawnRevision = state.camStockSim.Revision();
    glm::dvec3 lo(0.0), hi(0.0);
    const double floorZ =
        cam::StockBounds(*setup, ui::JobBodies(state), lo, hi) ? lo.z : 0.0;
    const cam::StockMesh mesh =
        cam::BuildStockMesh(state.camStockSim.Stock(), floorZ);
    std::vector<StockVertex> verts;
    verts.reserve(mesh.vertices.size());
    for (const cam::StockVertex& v : mesh.vertices)
      verts.push_back({v.position, v.normal});
    state.stockRenderer->SetMesh(std::move(verts), mesh.indices);
  }
  state.stockRenderer->SetTransform(model);

  // The bodies underneath, ghosted: the part is what the block is being taken
  // down to, and seeing it inside the stock is most of why this picture is
  // worth having. Re-applied every frame — instance ids change on every
  // regeneration, so an "already ghosted" early-out can hold only stale ids
  // (the FEM overlay learned this the hard way).
  if (state.meshRenderer)
    for (const DocumentRenderMesh& m : state.documentMeshes)
      state.meshRenderer->SetInstanceGhost(m.meshId, 0.22f);

  // The tool, where it is.
  const cam::GMove* move = state.camTimeline.MoveAt(state.camCursor);
  const cam::Tool* tool =
      move != nullptr ? setup->FindTool(move->tool) : nullptr;
  if (tool == nullptr || !state.meshRenderer) {
    DropToolInstance(state);
    return;
  }
  if (state.camToolInstanceFor != tool->number) {
    DropToolInstance(state);
    const cam::StockMesh mesh = cam::BuildToolMesh(*tool);
    state.camToolInstance =
        state.meshRenderer->AddMesh(ToMeshVertices(mesh), mesh.indices);
    state.camToolInstanceFor = tool->number;
    // MATTE, deliberately. This was metallic 0.9 / roughness 0.25 — a
    // near-mirror, so what you saw on the holder was mostly the environment
    // map reflected in it. A reflection slides across a curved surface as the
    // camera orbits, at its own rate rather than the geometry's, which reads
    // exactly as "the upper part almost follows the camera". The flute is
    // thin and mostly buried in material, so it did not show the effect and
    // "behaved correctly". A tool glyph is an instrument for reading where
    // the cutter is; a mirror finish costs that reading to gain nothing.
    state.meshRenderer->SetMeshMaterial(state.camToolInstance,
                                        glm::vec3(0.78f, 0.79f, 0.82f),
                                        glm::vec4(0.0f, 0.65f, 0.3f, 0.04f));
  }
  const glm::dvec3 at = state.camTimeline.PositionAt(state.camCursor);
  state.meshRenderer->SetMeshTransform(
      state.camToolInstance,
      model * glm::translate(glm::mat4(1.0f), glm::vec3(at)));
}

bool CamPlaybackDrawsPath(const AppState& state) {
  return state.camPlaybackOpen && !state.camTimeline.Empty();
}

void AppendCamPlaybackPath(AppState& state, std::vector<PrimVertex>& out) {
  if (!CamPlaybackDrawsPath(state) || !state.camShowToolpath)
    return;
  // Setup space is what the viewport draws on a job tab (v73/J6).
  const auto shown = [&](const glm::dvec3& p) { return glm::vec3(p); };
  const cam::Timeline& tl = state.camTimeline;
  const size_t at = state.camCursor.seg;
  for (size_t k = 0; k < tl.Segments(); k++) {
    const cam::GMove& m = tl.program.moves[k + 1];
    const bool done = k < at;
    const glm::vec4 color = done      ? kDoneColor
                            : m.rapid ? kAheadRapid
                                      : kAheadCut;
    // The cut ahead is the line the eye is following; give it the width as
    // well as the colour.
    AppendPrimLine(shown(tl.program.moves[k].to), shown(m.to),
                   done ? 1.4f : (m.rapid ? 1.0f : 2.4f), color, out);
  }
}

void DrawCamTransport(AppState& state) {
  ImGui::SeparatorText("Simulate the cut");
  if (!state.camPlaybackOpen) {
    if (ImGui::Button(ICON_FA_PLAY " Simulate###cam_sim_start"))
      BeginCamPlayback(state);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Cuts the posted file into a block of stock and shows "
                        "what comes off.");
    if (!state.camPlaybackRefusal.empty())
      ImGui::TextWrapped("%s", state.camPlaybackRefusal.c_str());
    return;
  }

  const cam::Timeline& tl = state.camTimeline;
  // TWO rows. The CAM panel is 217 px wide in the default layout, and seven
  // buttons on one line would run off the end of it — the same width that
  // decided where Generate and the verdict go (docs/cam/dogfood-round.md).
  if (ImGui::Button(ICON_FA_BACKWARD_FAST "###cam_sim_rewind"))
    SeekCamPlayback(state, {0, 0.0});
  Tip("Back to the start");
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_BACKWARD_STEP "###cam_sim_stepback"))
    SeekCamPlayback(state, StepBack(state.camCursor));
  Tip("Back one move");
  ImGui::SameLine();
  if (ImGui::Button(state.camPlaying ? ICON_FA_PAUSE "###cam_sim_play"
                                     : ICON_FA_PLAY "###cam_sim_play")) {
    state.camPlaying = !state.camPlaying;
    // Playing on from the end starts over, which is what the button is for
    // at that point.
    if (state.camPlaying && state.camCursor == tl.End())
      SeekCamPlayback(state, {0, 0.0});
  }
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_FORWARD_STEP "###cam_sim_step"))
    SeekCamPlayback(state, StepForward(state.camCursor, tl));
  Tip("Forward one move — the way to look at the move a verdict names");
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_FORWARD_FAST "###cam_sim_end"))
    SeekCamPlayback(state, tl.End());
  Tip("To the end");

  if (ImGui::Button(ICON_FA_ANGLES_RIGHT " Op###cam_sim_nextop"))
    SeekCamPlayback(state, tl.NextOp(state.camCursor));
  Tip("Skip to the next operation");
  ImGui::SameLine();
  // Only when the job HAS a tool change — a button that can only ever jump to
  // the end is a button that lies about what the job does.
  if (!tl.toolChanges.empty()) {
    if (ImGui::Button(ICON_FA_ANGLES_RIGHT " Tool###cam_sim_nexttool"))
      SeekCamPlayback(state, tl.NextToolChange(state.camCursor));
    Tip("Skip to the next tool change");
    ImGui::SameLine();
  }
  if (ImGui::Button("Close###cam_sim_close")) {
    EndCamPlayback(state);
    return;
  }

  // The scrub bar is DISTANCE along the path, not a clock: the file states
  // every cutting feed but no machine states its rapid rate, so a time axis
  // would be part measurement and part invention.
  float progress = tl.totalDistance > 1e-9
                       ? static_cast<float>(tl.DistanceAt(state.camCursor) /
                                            tl.totalDistance)
                       : 0.0f;
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::SliderFloat("###cam_sim_scrub", &progress, 0.0f, 1.0f, "%.0f%%",
                         ImGuiSliderFlags_AlwaysClamp))
    SeekCamPlayback(state, tl.CursorAtDistance(progress * tl.totalDistance));

  ImGui::SetNextItemWidth(120.0f);
  ImGui::SliderFloat("Speed###cam_sim_speed", &state.camPlaybackSpeed, 0.5f,
                     100.0f, "x%.0f", ImGuiSliderFlags_Logarithmic);

  // The readout: everything the cursor is standing on, in the file's own
  // terms, so a suspicious moment can be looked up in the G-code.
  const cam::GMove* move = tl.MoveAt(state.camCursor);
  if (move != nullptr) {
    const cam::Setup* setup = ActiveCamSetup(state);
    const cam::Operation* op = nullptr;
    if (setup != nullptr)
      for (const cam::Operation& candidate : setup->operations)
        if (candidate.id == move->op)
          op = &candidate;
    const cam::Tool* tool =
        setup != nullptr ? setup->FindTool(move->tool) : nullptr;
    ImGui::Text("%s", op != nullptr ? op->name.c_str()
                      : move->op != 0
                          ? "operation"
                          : (state.camCursor == tl.End() ? "done" : "moving"));
    ImGui::SameLine();
    if (tool != nullptr)
      ImGui::TextDisabled("T%d %.3g mm", tool->number, tool->diameter);
    else
      ImGui::TextDisabled("T%d", move->tool);
    if (move->rapid)
      ImGui::TextDisabled("rapid  ·  line %d", move->line);
    else if (move->feed > 1e-9)
      ImGui::TextDisabled("%.0f mm/min  ·  line %d", move->feed, move->line);
    else
      ImGui::TextDisabled("line %d", move->line);
    // Two numbers, and what each one counts. The cutting clock excludes
    // rapids unless the setup states a traverse rate — saying so here is
    // cheaper than having the number doubted later.
    ImGui::TextDisabled("%.0f of %.0f mm  ·  %.1f min cutting",
                        tl.DistanceAt(state.camCursor), tl.totalDistance,
                        tl.MinutesAt(state.camCursor));
  }
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
bool BeginCamPlayback(AppState&) { return false; }
void EndCamPlayback(AppState&) {}
void TickCamPlayback(AppState&, double) {}
void SeekCamPlayback(AppState&, cam::SimCursor) {}
void SyncCamPlayback(AppState&) {}
void DrawCamTransport(AppState&) {}
bool CamPlaybackDrawsPath(const AppState&) { return false; }
void AppendCamPlaybackPath(AppState&, std::vector<PrimVertex>&) {}
} // namespace ui

#endif
