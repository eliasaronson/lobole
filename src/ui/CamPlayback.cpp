#include "ui/CamPlayback.h"
#include "ui/FloatingWindow.h"

#ifdef HAVE_CAM

#include "core/Document.h"
#include "core/cam/CamGcode.h"
#include "core/cam/CamOtherSetups.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamStockMesh.h"
#include "core/cam/CamToolpath.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/StockRenderer.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/CamPanel.h"      // ActiveCamSetup, PostOpts
#include "ui/FeatureEditor.h" // kCamToolSlot
#include "ui/JobActions.h"
#include "ui/SketchOverlay.h"
#include "ui/Theme.h" // ElevatedWindowBorder

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <memory>
#include <thread>

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
// The rotary playback's grid: the one the panel VERIFIES a rotary job on
// (CamTool.cpp), so the last frame is the verdict's own stock. Coarser than
// the planar simulation's default, and deliberately -- a radius field is
// meshed as a tube with no run merging, and at this size the pin is ~200,000
// cells, which remeshes inside the playback's 12 Hz cap.
constexpr double kRotarySimRes = 0.5;

constexpr glm::vec4 kDoneColor(0.20f, 0.22f, 0.30f, 0.85f);
constexpr glm::vec4 kAheadCut(1.00f, 0.55f, 0.08f, 1.00f);
constexpr glm::vec4 kAheadRapid(0.50f, 0.52f, 0.60f, 0.55f);

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

// Where a playback's stock starts: what the job's EARLIER setups left, read
// from their posted files as the verifier reads them
// (docs/cam/simulation.md#leftover-another-setup-removes). Everything the
// read needs is COPIED -- plain values and shape handles -- so it runs the
// same inline and on a worker: reading a turning's program is the rotary
// carve of the whole file, 40 s before the chess rook and bishop's mitre, and
// that is not a thing to do on the frame.
// What the earlier setups' files are made from, cheap to take and compare:
// telling whether a start read a moment ago is still this job's start must
// not cost what reading it did -- regenerating a turning evicted from the
// pass cache is 18 s on the chess rook and bishop.
struct EarlierPrint {
  std::vector<uint64_t> ids, framing;
  std::vector<std::vector<cam::Operation>> ops;
  std::vector<std::vector<cam::Tool>> tools;
  std::vector<cam::Machine> machines;
  bool operator==(const EarlierPrint&) const = default;
};

EarlierPrint PrintOf(AppState& state, uint64_t setupId) {
  EarlierPrint p;
  for (const cam::Setup& s : state.job->GetSetups()) {
    if (s.id == setupId)
      break;
    p.ids.push_back(s.id);
    p.framing.push_back(cam::FramingKey(s));
    p.ops.push_back(s.operations);
    p.tools.push_back(s.tools);
    p.machines.push_back(s.machine);
  }
  return p;
}

struct StartWork {
  // in
  EarlierPrint print;
  cam::Setup setup;
  std::vector<Body> bodies;
  std::vector<cam::Setup> others; // earlier, in job order
  std::vector<std::string> files;
  std::vector<std::string> unposted;
  bool rotary = false;
  double res = 0.25;
  uint64_t setupId = 0;
  std::string gcode; // the file the playback is for
  // out
  cam::OtherSetups earlier;
  cam::HeightMap height;
  cam::RadiusMap radius;
  bool ok = false;
  std::string why;
};

// The earlier setups' posted files, on the frame from the caches. False,
// with the refusal, when one of them is still being cut and `settle` does
// not ask for it now: a playback started from the bar there would be a
// picture of a job nobody posts.
bool CollectEarlier(AppState& state, const cam::Setup& setup, bool settle,
                    StartWork& w, std::string& refusal) {
  for (const cam::Setup& other : state.job->GetSetups()) {
    if (other.id == setup.id)
      break; // the job's order is the order they are cut in
    if (other.operations.empty())
      continue;
    std::vector<cam::Toolpath> paths;
    if (settle) {
      paths = GenerateCamToolpathsNow(state, other);
    } else {
      bool pending = false;
      paths = CamToolpathsQueued(state, other, pending);
      if (pending) {
        refusal = "Setup '" + other.name +
                  "' is cut before this one, and its toolpaths are still "
                  "being generated -- this one starts from what it leaves. "
                  "Simulate again when they land.";
        return false;
      }
    }
    std::string file, why;
    if (!cam::PostProcess(other.machine.post, other, paths, PostOpts(other),
                          file, &why)) {
      w.unposted.push_back("setup '" + other.name +
                           "', because it does not post: " + why);
      continue;
    }
    w.others.push_back(other);
    w.files.push_back(std::move(file));
  }
  return true;
}

// The expensive half: touches nothing but its own struct.
void ReadStart(StartWork& w) {
  std::vector<cam::SetupProgram> programs;
  for (size_t k = 0; k < w.others.size(); k++)
    programs.push_back({&w.others[k], w.files[k], /*earlier=*/true});
  w.earlier = cam::OtherSetupsOf(w.setup, programs, w.bodies, w.res);
  w.earlier.notChecked.insert(w.earlier.notChecked.end(), w.unposted.begin(),
                              w.unposted.end());
  if (w.rotary) {
    // The part TURNS: a radius field about the rotary axis, from the same
    // start the verifier derives (the blank, or what earlier setups left of
    // it), at the grid the panel verifies on -- so the last frame of this
    // playback is the stock the verdict was read from, cell for cell (cam
    // rotary: playing to the end leaves the verifier's own stock).
    w.radius =
        cam::StartingRotaryStock(w.setup, w.bodies, w.res, &w.earlier, &w.why);
    w.ok = !w.radius.r.empty();
    if (!w.ok)
      w.why = "This job cannot be simulated: " + w.why;
    return;
  }
  w.ok = cam::StartingStock(w.setup, w.bodies, w.res, &w.earlier, w.height);
  if (!w.ok)
    w.why = "This setup has no stock to cut — the model has no size.";
}

// Arms the playback from a start `ReadStart` made. False + the refusal when
// the start could not be made.
bool ArmPlayback(AppState& state, const cam::Setup& setup, StartWork& w) {
  if (!w.ok) {
    state.camPlaybackRefusal = w.why;
    state.camPlaybackGcode.clear();
    state.camTimeline = cam::Timeline{};
    return false;
  }
  state.camPlaybackSeededFrom.clear();
  for (const cam::SetupStock& s : w.earlier.stocks)
    if (s.earlier)
      state.camPlaybackSeededFrom.push_back(s.name);
  state.camPlaybackNotSeeded = w.earlier.notChecked;
  if (w.rotary)
    state.camRotarySim.Begin(state.camTimeline, setup, std::move(w.radius));
  else
    state.camStockSim.Begin(state.camTimeline, setup, std::move(w.height));
  state.camCursor = {0, 0.0};
  state.camPlaybackOpen = true;
  // What it plays: a switch to another job or setup ends it
  // (SyncCamResultOwner).
  state.camPlaybackOwner = CurrentCamResultOwner(state);
  // The simulation is the one thing on screen now: a verb window still
  // asking for a face, or a measurement, over a block being cut is two
  // sessions claiming the same clicks. Close them -- the job stays.
  state.camToolOpen = false;
  state.camVerb = AppState::CamVerb::None;
  state.camVerbOp = 0;
  state.camGhostOp = 0;
  state.camRefusal.clear();
  state.camNote.clear();
  if (state.slotPick.featureId == kCamToolSlot)
    state.slotPick = {};
  state.measure = MeasureState{};
  state.sectionToolOpen = false;
  state.camPlaying = false;
  state.camStockDrawnRevision = 0;
  return true;
}

// A start read on a worker, kept for the playback that asks for it: the
// panel's Simulate, whose transport opens when it lands, or a parked
// `cam_simulate` (docs/cam/README.md#requests-that-wait), which is answered
// once it has. One at a time.
struct StartSlot {
  std::shared_ptr<StartWork> work;
  bool landed = false;
  bool armWhenLanded = false; // the panel's: open the transport on landing
};
StartSlot g_start;

// Whether the start in the slot was read for the playback of `setupId` at
// grid `res` on the stock model `rotary`, of the file `gcode`, from earlier
// setups that still stand as `print` says. Compared without posting them
// again.
bool SlotFits(AppState& state, uint64_t setupId, double res, bool rotary,
              const std::string* gcode) {
  const StartWork* w = g_start.work.get();
  return w != nullptr && w->setupId == setupId && w->rotary == rotary &&
         w->res == (rotary ? kRotarySimRes : res) &&
         (gcode == nullptr || w->gcode == *gcode) &&
         w->print == PrintOf(state, setupId);
}

void ReadStartOffFrame(AppState& state, std::shared_ptr<StartWork> work,
                       bool armWhenLanded) {
  g_start = {work, false, armWhenLanded};
  auto job = std::make_unique<BackgroundJob>();
  job->kind = "cam-simulate";
  job->label = "Reading what the earlier setups left";
  job->cancellable = false; // the carve has no interruption points
  job->fraction.store(-1.0);
  BackgroundJob* raw = job.get();
  job->thread = std::thread([work, raw] {
    ReadStart(*work);
    raw->done.store(true); // last
  });
  job->finish = [work](AppState& s) -> bool {
    if (g_start.work != work)
      return true; // superseded
    g_start.landed = true;
    // Opened only if the same file is still the one to play: Begin reads
    // the job again and takes this start only when it still fits.
    if (g_start.armWhenLanded && !s.camPlaybackOpen && !BeginCamPlayback(s))
      s.camRefusalPopup = true; // said where the button was pressed
    return true;
  };
  StartBackgroundJob(state, std::move(job));
}

// The start for `setup`, collected on the frame; false + the refusal when an
// earlier setup's passes are still being cut and `settle` does not ask for
// them now.
bool CollectStart(AppState& state, const cam::Setup& setup,
                  const std::string& gcode, bool rotary, double simRes,
                  bool settle, StartWork& w, std::string& refusal) {
  w.print = PrintOf(state, setup.id);
  w.setup = setup;
  w.bodies = ui::JobBodies(state);
  w.rotary = rotary;
  w.res = rotary ? kRotarySimRes : simRes;
  w.setupId = setup.id;
  w.gcode = gcode;
  return CollectEarlier(state, setup, settle, w, refusal);
}

} // namespace

bool CamPlaybackStartReading(AppState& state, uint64_t setupId, double simRes) {
  if (!state.job)
    return false;
  const cam::Setup* setup = state.job->FindSetup(setupId);
  if (setup == nullptr || setup->operations.empty())
    return false;
  // Asked every tick while the read runs: answered from the slot, without
  // posting anything again.
  if (g_start.work && g_start.work->setupId == setupId &&
      SlotFits(state, setupId, simRes, g_start.work->rotary, nullptr))
    return !g_start.landed;
  // Every pass is cached by now (the caller waited for them), so this posts
  // from the cache.
  const std::vector<cam::Toolpath> paths =
      GenerateCamToolpathsNow(state, *setup);
  std::string gcode, error;
  if (!cam::PostProcess(setup->machine.post, *setup, paths, PostOpts(*setup),
                        gcode, &error))
    return false; // the handler refuses it, in its own words
  auto work = std::make_shared<StartWork>();
  std::string refusal;
  if (!CollectStart(
          state, *setup, gcode,
          cam::RotatesWhileCutting(cam::ProgramInSetupSpace(gcode, *setup)),
          simRes, /*settle=*/true, *work, refusal) ||
      work->others.empty())
    return false; // nothing earlier to read: the handler is quick
  ReadStartOffFrame(state, std::move(work), /*armWhenLanded=*/false);
  return true;
}

bool BeginCamPlayback(AppState& state, bool settle) {
  // The toolpaths it posts must be the setup on screen's.
  SyncCamResultOwner(state);
  state.camPlaybackRefusal.clear();
  state.camPlaybackSeededFrom.clear();
  state.camPlaybackNotSeeded.clear();
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
  if (const std::string why = CamToolpathsPendingWhy(state); !why.empty()) {
    // The picture would be of a file the job no longer posts.
    state.camPlaybackRefusal = why;
    return false;
  }
  if (g_start.work && !g_start.landed && g_start.armWhenLanded && !settle)
    return true; // already on its way: it opens when the read lands

  // The POSTED FILE, like the verification: a bug in the post, in modal
  // state or in linking is invisible to anything that plays a
  // std::vector<Move>, and one of those had already shipped (a NaN retract).
  std::string gcode, error;
  if (!cam::PostProcess(setup->machine.post, *setup, state.camToolpaths,
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
      cam::ProgramInSetupSpace(gcode, *setup), setup->rapidFeed,
      cam::FeedModelOf(setup->machine.post));
  if (!state.camTimeline.unsupported.empty()) {
    // Refused VISIBLY, where the buttons are, rather than played wrong. The
    // transport's own rule (docs/cam/simulation.md#transport) and the one the
    // rotary survey exists to keep us on the right side of.
    state.camPlaybackRefusal =
        "This job cannot be simulated: " + state.camTimeline.unsupported;
    state.camPlaybackGcode.clear();
    return false;
  }
  if (state.camTimeline.Empty()) {
    state.camPlaybackRefusal = "The posted file contains no moves to simulate.";
    state.camPlaybackGcode.clear();
    return false;
  }

  // Read off the frame already, for this very playback: take it.
  if (g_start.landed && SlotFits(state, setup->id, state.camSimRes,
                                 state.camTimeline.rotary, &gcode)) {
    std::shared_ptr<StartWork> landed = std::move(g_start.work);
    g_start = {};
    return ArmPlayback(state, *setup, *landed);
  }

  // Where the stock starts: what the job's earlier setups left, as this
  // setup's verification starts from it.
  auto work = std::make_shared<StartWork>();
  {
    std::string refusal;
    if (!CollectStart(state, *setup, gcode, state.camTimeline.rotary,
                      state.camSimRes, settle, *work, refusal)) {
      state.camPlaybackRefusal = refusal;
      state.camPlaybackGcode.clear();
      state.camTimeline = cam::Timeline{};
      return false;
    }
  }

  // Inline when there is nothing earlier to read, when the caller settles
  // (the RPC reader, which parks for the read before it gets here -- this is
  // its fallback), or when the panel is not deferring -- the GUI suite's
  // setting, as for the verification.
  if (work->others.empty() || settle || !state.camDeferPasses) {
    ReadStart(*work);
    return ArmPlayback(state, *setup, *work);
  }

  // Otherwise on a worker, and the transport opens when it lands.
  ReadStartOffFrame(state, std::move(work), /*armWhenLanded=*/true);
  return true;
}

void EndCamPlayback(AppState& state) {
  state.camPlaybackOpen = false;
  state.camPlaying = false;
  state.camStockSim.Clear();
  state.camRotarySim.Clear();
  state.camTimeline = cam::Timeline{};
  // The part turned while it played. The bodies come back at identity by
  // themselves -- the resync below recreates their instances -- but the
  // stock renderer is not resynced, and a model matrix left standing would
  // turn the NEXT playback's block by this one's last angle.
  if (state.stockRenderer)
    state.stockRenderer->SetModel(glm::mat4(1.0f));
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
  if (state.camTimeline.rotary) {
    state.camRotarySim.SeekTo(to);
    state.camCursor = state.camRotarySim.At();
  } else {
    state.camStockSim.SeekTo(to);
    state.camCursor = state.camStockSim.At();
  }
}

void TickCamPlayback(AppState& state, double now) {
  if (!state.camPlaybackOpen)
    return;
  // A document edit invalidates the playback rather than re-cutting under the
  // cursor: the toolpaths it was posted from no longer describe this model,
  // and regenerating here would put a multi-second freeze inside the frame
  // loop (docs/cam/simulation.md#transport).
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
  // No transform anywhere in here, on purpose. A job tab DRAWS setup space
  // (v73/J6): the parts are laid out in it, and so is everything drawn over
  // them. This used to multiply the stock and the cutter by setup-to-model,
  // which undid exactly the transform the viewport had just applied -- and
  // was invisible while a setup had one frame (the two spaces coincide), then
  // permanent once fixturing became per part (v76): the block stood upright
  // through a part lying flat. The backplot had the same bug the same week.
  // A function whose whole job is to undo what the viewport does is an
  // invitation to put it back, so there is none to call.

  // The stock, re-meshed when the simulation moved -- but no faster than
  // this while it is PLAYING. See camStockDrawnAt: the gate below is correct
  // and never fires during playback, because the stock really does change
  // every frame, so the cap is what keeps a 700,000-cell remesh off the
  // frame. Paused or stepped, it rebuilds at once and the picture is exact.
  constexpr double kStockRedrawSeconds = 1.0 / 12.0;
  // A steady clock, NOT ImGui::GetTime(). This function also runs on the
  // OFFSCREEN path (SyncOffscreenScene), where there is no ImGui context at
  // all, and asking one for the time there is a null dereference:
  // `cam_simulate` followed by a `screenshot` on a headless instance crashed
  // with SIGSEGV from the day this cap was added until the rotary picture made
  // somebody take that screenshot.
  const double now = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const bool rotary = state.camTimeline.rotary;
  const uint64_t revision =
      rotary ? state.camRotarySim.Revision() : state.camStockSim.Revision();
  const bool stale = revision != state.camStockDrawnRevision;
  const bool allowed =
      !state.camPlaying || now - state.camStockDrawnAt >= kStockRedrawSeconds;
  if (stale && allowed) {
    state.camStockDrawnAt = now;
    state.camStockDrawnRevision = revision;
    cam::StockMesh mesh;
    if (rotary) {
      mesh = cam::BuildRotaryStockMesh(state.camRotarySim.Stock());
    } else {
      glm::dvec3 lo(0.0), hi(0.0);
      const double floorZ =
          cam::StockBounds(*setup, ui::JobBodies(state), lo, hi) ? lo.z : 0.0;
      mesh = cam::BuildStockMesh(state.camStockSim.Stock(), floorZ);
    }
    std::vector<StockVertex> verts;
    verts.reserve(mesh.vertices.size());
    for (const cam::StockVertex& v : mesh.vertices)
      verts.push_back({v.position, v.normal});
    state.stockRenderer->SetMesh(std::move(verts), mesh.indices);
  }

  // The bodies underneath, ghosted: the part is what the block is being taken
  // down to, and seeing it inside the stock is most of why this picture is
  // worth having. Re-applied every frame — instance ids change on every
  // regeneration, so an "already ghosted" early-out can hold only stale ids
  // (the FEM overlay learned this the hard way).
  //
  // And for a rotary program, TURNED: one matrix, R_x(A) at the cursor, on
  // everything that belongs to the part -- the ghosted bodies, their
  // outlines and the stock -- because the part is what the machine turns.
  // The tool below is in machine space already and does not move with it.
  // Every frame, not on change: the instances are recreated on any resync,
  // and the angle moves continuously while it plays. Identity for a planar
  // program, which is what these instances always had.
  const glm::mat4 partTurn =
      rotary ? glm::rotate(glm::mat4(1.0f),
                           glm::radians(static_cast<float>(
                               state.camTimeline.AngleAt(state.camCursor))),
                           glm::vec3(1.0f, 0.0f, 0.0f))
             : glm::mat4(1.0f);
  state.stockRenderer->SetModel(partTurn);
  if (state.meshRenderer)
    for (const DocumentRenderMesh& m : state.documentMeshes) {
      state.meshRenderer->SetInstanceGhost(m.meshId, 0.22f);
      if (rotary) {
        state.meshRenderer->SetMeshTransform(m.meshId, partTurn);
        if (state.outlineRenderer && m.outlineId != 0)
          state.outlineRenderer->SetMeshTransform(m.outlineId, partTurn);
      }
    }

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
      state.camToolInstance, glm::translate(glm::mat4(1.0f), glm::vec3(at)));
}

bool CamPlaybackDrawsPath(const AppState& state) {
  return state.camPlaybackOpen && !state.camTimeline.Empty();
}

namespace {

// The played path's vertices, cached — and RECOLOURED rather than rebuilt as
// the cursor moves.
//
// This ran in full every frame while a simulation was open, and a 3-axis job
// is a big program: six PrimVertex per segment over tens of thousands of
// segments, rebuilt for a picture whose GEOMETRY never changes. Only the
// colour does, and only at one place — `done` is the prefix k < at — so a
// cursor step needs the vertices between the old split and the new one
// touched and nothing else. Playing forward that is a handful per frame.
std::vector<PrimVertex> g_played;
size_t g_playedAt = 0;    // the split the cache currently holds
uint64_t g_playedKey = 0; // which program it was built from

constexpr size_t kVertsPerSeg = 6; // AppendPrimLine's two triangles

uint64_t PlayedKey(const cam::Timeline& tl) {
  uint64_t k = 1469598103934665603ull;
  const auto mix = [&k](uint64_t v) { k = (k ^ v) * 1099511628211ull; };
  mix(tl.program.moves.size());
  mix(static_cast<uint64_t>(tl.totalDistance * 1000.0));
  mix(tl.opStarts.size());
  mix(tl.toolChanges.size());
  return k;
}

// Colour and width for one segment, at a given split.
void SegmentStyle(const cam::GMove& m, bool done, glm::vec4& color,
                  float& halfPx) {
  color = done ? kDoneColor : m.rapid ? kAheadRapid : kAheadCut;
  // The cut ahead is the line the eye is following; give it the width as
  // well as the colour.
  halfPx = done ? 1.4f : (m.rapid ? 1.0f : 2.4f);
}

} // namespace

void AppendCamPlaybackPath(AppState& state, std::vector<PrimVertex>& out) {
  if (!CamPlaybackDrawsPath(state) || !state.camShowToolpath)
    return;
  // Setup space is what the viewport draws on a job tab (v73/J6).
  const auto shown = [&](const glm::dvec3& p) { return glm::vec3(p); };
  const cam::Timeline& tl = state.camTimeline;
  const size_t at = state.camCursor.seg;
  const size_t segs = tl.Segments();

  // A ROTARY program draws a WINDOW of it, in machine space, around the
  // cursor -- not all of it.
  //
  // In machine space every pass of a rotary job lies along the top of the
  // axis, which is exactly where the tool is while the part turns under it:
  // the window shows where the cutter is travelling, and is true. The passes
  // already cut are grooves in the turning stock, which is the better record
  // of them. Drawing the whole program here would put the pin's 644,160
  // moves in one line along the top -- the flat fan the backplot used to be
  // -- and rebuild it for nothing.
  if (tl.rotary) {
    constexpr size_t kBehind = 600, kAhead = 1800;
    const size_t from = at > kBehind ? at - kBehind : 0;
    const size_t to = std::min(segs, at + kAhead);
    out.reserve(out.size() + (to - from) * kVertsPerSeg);
    for (size_t k = from; k < to; k++) {
      const cam::GMove& m = tl.program.moves[k + 1];
      glm::vec4 color;
      float halfPx = 1.0f;
      SegmentStyle(m, k < at, color, halfPx);
      AppendPrimLine(shown(tl.program.moves[k].to), shown(m.to), halfPx, color,
                     out);
    }
    return;
  }

  if (const uint64_t key = PlayedKey(tl);
      key != g_playedKey || g_played.size() != segs * kVertsPerSeg) {
    g_played.clear();
    g_played.reserve(segs * kVertsPerSeg);
    for (size_t k = 0; k < segs; k++) {
      const cam::GMove& m = tl.program.moves[k + 1];
      glm::vec4 color;
      float halfPx = 1.0f;
      SegmentStyle(m, k < at, color, halfPx);
      AppendPrimLine(shown(tl.program.moves[k].to), shown(m.to), halfPx, color,
                     g_played);
    }
    g_playedKey = key;
    g_playedAt = at;
  } else if (at != g_playedAt) {
    // Only the segments the cursor crossed change side.
    const size_t lo = std::min(at, g_playedAt);
    const size_t hi = std::min(std::max(at, g_playedAt), segs);
    for (size_t k = lo; k < hi; k++) {
      glm::vec4 color;
      float halfPx = 1.0f;
      SegmentStyle(tl.program.moves[k + 1], k < at, color, halfPx);
      for (size_t v = 0; v < kVertsPerSeg; v++) {
        PrimVertex& pv = g_played[k * kVertsPerSeg + v];
        pv.color = color;
        pv.params.x = halfPx;
      }
    }
    g_playedAt = at;
  }
  out.insert(out.end(), g_played.begin(), g_played.end());
}

void DrawCamTransportWindow(AppState& state) {
  if (!state.camPlaybackOpen || !state.job)
    return;
  // A media transport, where a media transport goes: along the BOTTOM of the
  // viewport, centred, and only while something is playing. Not the tool
  // window slot (BeginToolWindowFrame): that is one-at-a-time by design and
  // a verb window may be open beside this. Anchored when it appears and
  // draggable after (ui/FloatingWindow.h).
  static FloatingPlacement placement;
  if (!BeginFloatingWindow(state, "Simulate the cut###CamTransport", nullptr,
                           placement, FloatAnchor::BottomCenter,
                           ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  const cam::Timeline& tl = state.camTimeline;
  // ONE row now. This was two, sized to a 217 px panel; a floating window is
  // as wide as its scrub bar.
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
  ImGui::SameLine();
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
  ImGui::Checkbox("Path###cam_sim_show_path", &state.camShowToolpath);
  Tip("Draw the file being played, dimmed behind the cursor");
  ImGui::SameLine();
  if (ImGui::Button("Close###cam_sim_close")) {
    EndCamPlayback(state);
    ImGui::End();
    return;
  }

  // The scrub bar is DISTANCE along the path, not a clock: the file states
  // every cutting feed but no machine states its rapid rate, so a time axis
  // would be part measurement and part invention.
  // In PERCENT, because the label is the value: a 0..1 slider printed with
  // "%.0f%%" read "0%" at the halfway point and "1%" at the end, however far
  // the cut had got.
  float percent =
      tl.totalDistance > 1e-9
          ? static_cast<float>(100.0 * tl.DistanceAt(state.camCursor) /
                               tl.totalDistance)
          : 0.0f;
  ImGui::SetNextItemWidth(420.0f);
  if (ImGui::SliderFloat("###cam_sim_scrub", &percent, 0.0f, 100.0f, "%.0f%%",
                         ImGuiSliderFlags_AlwaysClamp))
    SeekCamPlayback(state,
                    tl.CursorAtDistance(percent / 100.0 * tl.totalDistance));

  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  ImGui::SliderFloat("###cam_sim_speed", &state.camPlaybackSpeed, 0.5f, 100.0f,
                     "x%.0f", ImGuiSliderFlags_Logarithmic);
  Tip("Playback speed");

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
    // Where the trunnion is. The one number a rotary program adds, and the
    // one the picture is turning by -- read off the same timeline.
    if (tl.rotary) {
      ImGui::SameLine();
      ImGui::TextDisabled("·  A %.1f°", tl.AngleAt(state.camCursor));
    }
    // Two numbers, and what each one counts. The cutting clock excludes
    // rapids unless the setup states a traverse rate — saying so here is
    // cheaper than having the number doubted later.
    ImGui::TextDisabled("%.0f of %.0f mm  ·  %.1f min cutting",
                        tl.DistanceAt(state.camCursor), tl.totalDistance,
                        tl.MinutesAt(state.camCursor));
  }
  // Where the block came from, when it is not the fresh stock: a turned
  // part under an indexed setup looks like a modelling mistake otherwise.
  if (!state.camPlaybackSeededFrom.empty()) {
    std::string names;
    for (const std::string& n : state.camPlaybackSeededFrom)
      names += (names.empty() ? "'" : ", '") + n + "'";
    ImGui::TextDisabled("Starts from what %s left", names.c_str());
  }
  if (!state.camPlaybackNotSeeded.empty())
    ImGui::TextDisabled("Not read, so started from its stock: %s",
                        state.camPlaybackNotSeeded.front().c_str());
  ImGui::End();
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
bool BeginCamPlayback(AppState&, bool) { return false; }
bool CamPlaybackStartReading(AppState&, uint64_t, double) { return false; }
void EndCamPlayback(AppState&) {}
void TickCamPlayback(AppState&, double) {}
void SeekCamPlayback(AppState&, cam::SimCursor) {}
void SyncCamPlayback(AppState&) {}
void DrawCamTransportWindow(AppState&) {}
bool CamPlaybackDrawsPath(const AppState&) { return false; }
void AppendCamPlaybackPath(AppState&, std::vector<PrimVertex>&) {}
} // namespace ui

#endif
