#include "ui/CamTool.h"

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamFeeds.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "ui/AppActions.h" // AppendPrimLine/Dot, WorldToScreen
#include "ui/CamJobs.h"
#include "ui/CamPanel.h"
#include "ui/JobActions.h"
#include "ui/ListSelection.h"

#include "render/Camera.h"
#include "ui/FeatureEditor.h"
#include "ui/PreviewFeature.h"    // BeginToolWindowFrame
#include "ui/SketchInteraction.h" // WorldToScreen (the depth label)

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <limits>

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
// The machine's work area: dimmer than the stock, so the stock reads as the
// thing in front; red when the job does not fit it.
constexpr glm::vec4 kBedLine(0.45f, 0.55f, 0.70f, 0.35f);
constexpr glm::vec4 kBedRefusedLine(1.00f, 0.35f, 0.35f, 0.80f);

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
// The region a picked face bounds, CACHED. Both callers below run once per
// frame — the ghost while it is shown, the hover preview while a pick is
// armed — and each call framed every body (a copy, element maps and all) and
// sectioned the face through OCC. That is model work inside the frame loop.
// The key is everything the answer depends on, so a stale hit cannot be
// expressed: a model edit moves the document version, an indexed setup moves
// rotationA, a re-clamped one moves the frame, and a different pick moves the
// ref.
const cam::FaceRegion& CachedRegion(AppState& state,
                                    AppState::CamRegionCache& cache,
                                    const cam::Setup& setup,
                                    const ElementRef& ref) {
  const std::vector<Body>& bodies = ui::JobBodies(state);
  // The cache key is what the bodies were assembled from, not one document's
  // version: a job's geometry moves when any of its parts does.
  const uint64_t version = state.jobBodiesKey;
  const bool sameRef =
      cache.ref.featureId == ref.featureId && cache.ref.name == ref.name;
  if (cache.documentVersion != version || cache.setupId != setup.id ||
      cache.rotationA != setup.rotationA || cache.frame != setup.frame ||
      !sameRef) {
    cache.documentVersion = version;
    cache.setupId = setup.id;
    cache.rotationA = setup.rotationA;
    cache.frame = setup.frame;
    cache.ref = ref;
    cache.region = bodies.empty()
                       ? cam::FaceRegion{}
                       : cam::PickedFaceRegion(
                             cam::BodiesInSetupFrame(setup, bodies), ref);
  }
  return cache.region;
}

void OperationGhost(AppState& state, const cam::Setup& setup,
                    const cam::Operation& op, std::vector<PrimVertex>& prims,
                    std::vector<LineVertex3D>& quads) {
  // Setup space: op.topZ/bottomZ are, so the boundary must be too (C9).
  std::vector<cam::Loop> loops;
  if (!op.targets.empty()) {
    const cam::FaceRegion& region =
        CachedRegion(state, state.camGhostRegion, setup, op.targets[0].ref);
    if (!region.ok)
      return;
    loops = region.loops;
  } else {
    const std::vector<Body> bodies =
        cam::BodiesInSetupFrame(setup, ui::JobBodies(state));
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

// The verbs: each is one sentence a person would say out loud, and each
// resolves to one click on the model. Since 2026-09-02 they are the JOB
// TOOLBAR (one button each, like the part tools), and the window that opens
// is the verb's own -- its prompt, its tool, what it made. The three
// orientation picks are verbs too: same button, same window, no cutter.
struct Verb {
  AppState::CamVerb verb;
  const char* label;
  cam::OpType type; // what the verb makes; unused for the orientation picks
  PickSlot slot;
  bool picks;  // false: the verb needs no click on the model
  bool cutter; // true: the window offers the tool the verb derives for
  const char* prompt;
  const char* tip;
};

using CamVerb = AppState::CamVerb;

constexpr Verb kVerbs[] = {
    {CamVerb::TopFace, "Pick the top face", cam::OpType::Face,
     PickSlot::CamTopFace, true, false,
     "Click the top face — that sets Z zero for the job",
     "Z zero is this face: where the operator touches off. Calling a face "
     "the top also clamps the part with it up, so an upright part lies down "
     "with the same click."},
    {CamVerb::LayFaceUp, "Lay a face up", cam::OpType::Face,
     PickSlot::CamLayFaceUp, true, false,
     "Click the face to clamp facing up — that is how the part sits on the "
     "bed",
     "Clamp the part with this face up. Changes nothing in the model and "
     "posts no rotary move — it is how the part sits on the bed for this "
     "setup."},
    {CamVerb::IndexFace, "Turn a face up", cam::OpType::Face,
     PickSlot::CamIndexFace, true, false,
     "Click the face to turn up under the spindle",
     "Rotary (4th axis) jobs: index the table so this face points at the "
     "spindle, then use the verbs as usual."},
    {CamVerb::Pocket, "Clear a pocket", cam::OpType::Pocket,
     PickSlot::CamPocketFloor, true, true,
     "Click the floor of the pocket to clear",
     "Click the flat bottom of the pocket. Its walls become the boundary and "
     "its height becomes the depth."},
    {CamVerb::Profile, "Cut it out", cam::OpType::Profile,
     PickSlot::CamProfileOutline, true, true,
     "Click the wall to follow — the part's own side, or the inside of an "
     "opening",
     "Follow a wall. The part's own side cuts it free of the stock, with tabs "
     "so it does not break loose under the cutter; the wall of an opening "
     "cuts that opening out. An opening that goes all the way through has no "
     "floor to click, so its wall is the only thing to point at."},
    {CamVerb::Drill, "Drill a hole", cam::OpType::Drill, PickSlot::CamDrillHole,
     true, true, "Click the inside of the hole to drill",
     "Drill a hole that is already in the model: click its cylindrical wall "
     "and the depth comes from the hole's own extent. ONE plunge on the "
     "hole's own axis at the tool's plunge feed — no pecking and no helix, so "
     "the tool has to be able to plunge the full depth and clear its own "
     "chips. The verb stays armed, so a bolt circle is one click per hole."},
    {CamVerb::Edge, "Break an edge", cam::OpType::EdgeRound,
     PickSlot::CamEdgeWall, true, true,
     "Click the wall whose top edge to round or chamfer",
     "Follow a wall's top edge with a shaped cutter: a ball nose rounds it "
     "over, a V-bit chamfers it. The TOOL decides which, so a flat end mill "
     "is refused rather than ploughing a square step. The size of the break "
     "comes from the tool and the wall unless the operation says otherwise."},
    {CamVerb::Whole, "Cut out the whole part", cam::OpType::Profile,
     PickSlot::CamProfileOutline, false, true, "",
     "Follow the part's own outline — no pick needed."},
    {CamVerb::Face, "Face the top", cam::OpType::Face, PickSlot::CamTopFace,
     false, true, "", "Skim the material sitting above the part."},
};

const Verb* FindVerb(CamVerb verb) {
  for (const Verb& v : kVerbs)
    if (v.verb == verb)
      return &v;
  return nullptr;
}

} // namespace

void ArmCamPick(AppState& state, PickSlot slot, const char* prompt) {
  state.camRefusal.clear();
  state.slotPick = {kCamToolSlot, static_cast<int>(slot),
                    SelectionKindBit(SelectionItem::Kind::Face),
                    /*multi=*/false, prompt};
}

namespace {

// The setup this window works on, made on demand.
//
// Nothing in here should ever answer "add a setup first". The toolbar opens
// this window on a part with no CAM job at all — which is exactly when
// someone reaches for it — and the machining verbs already knew to make one
// rather than send the user off to find another panel. The two picks in the
// Zero section did not, so pressing "Pick top face" on a fresh part and
// clicking a face did nothing at all: the refusal went into slotPick.miss,
// which this window did not draw.
//
// Made on the first ACTION, not when the window opens: looking at the CAM
// window and closing it again should not leave an empty setup in a document
// and mark it dirty.
cam::Setup* EnsureCamSetup(AppState& state) {
  if (!state.job)
    return nullptr;
  if (cam::Setup* setup = ActiveCamSetup(state))
    return setup;
  cam::Setup& fresh = state.job->AddSetup();
  state.camActiveSetup = fresh.id;
  (void)cam::RecomputeOrigin(fresh, ui::JobBodies(state));
  fresh.safeZ = cam::DeriveSafeZ(fresh, ui::JobBodies(state));
  state.camSafeZEdit = static_cast<float>(fresh.safeZ);
  state.projectMetaDirty = true;
  return &fresh;
}

// What the generator said about one operation's toolpath; empty when it cut.
std::string ToolpathError(const AppState& state, uint64_t opId) {
  // Any of the operation's toolpaths: a rough-then-finish has two (R6).
  for (const cam::Toolpath& tp : state.camToolpaths)
    if (tp.operationId == opId && !tp.error.empty())
      return tp.error;
  return {};
}

// Creates the operation a verb implies, or refuses in words. The derivation
// itself is engine-side (cam::DeriveOperation) so a headless test can reach
// it — the panel must not be where a depth comes from.
// `hint` is the job part the click landed on; 0 means "work it out", which is
// only an answer while the element name is unique across the job.
// Returns the new operation's id, 0 when refused.
uint64_t CreateVerbOperation(AppState& state, cam::OpType type,
                             const ElementRef& target, uint64_t hint = 0) {
  state.camRefusal.clear();
  if (!state.job)
    return 0;
  JobUndoGuard undoGuard(state); // one step per verb (R7)
  cam::Setup* setup = EnsureCamSetup(state);
  if (setup == nullptr)
    return 0;
  // Which job part the picked face is on. A target that cannot be pinned to
  // one is refused in words rather than machined on whichever part answered
  // to the name first (v76).
  std::string why;
  const cam::Target picked = ui::TargetFor(state, target, hint, &why);
  if (target.IsSet() && picked.jobPartId == 0) {
    state.camRefusal = why;
    return 0;
  }
  const cam::DerivedOp derived = cam::DeriveOperation(
      *setup, ui::JobBodies(state), type, picked, VerbTool(state, *setup));
  if (!derived.ok) {
    state.camRefusal = derived.why;
    return 0;
  }
  cam::Operation op = derived.op;
  op.id = state.job->AllocateId();
  // "Then finish with" (R6): the window's second tool row.
  if (state.camVerbFinishTool != 0 &&
      state.camVerbFinishTool != op.toolNumber &&
      setup->FindTool(state.camVerbFinishTool) != nullptr) {
    op.finishTool = state.camVerbFinishTool;
    if (const cam::Tool* rough = setup->FindTool(op.toolNumber))
      op.finishStock = cam::DefaultFinishStock(*rough);
  }
  setup->operations.push_back(op);
  state.camGhostOp = op.id;
  // The tool this cut with is the one in the spindle: the next verb derives
  // for it unless told otherwise.
  state.camVerbTool = op.toolNumber;
  state.projectMetaDirty = true;
  // Just the new one: the operations already in the job did not change, and
  // one of them may be an adaptive pocket that takes seconds to re-cut.
  RefreshCamToolpath(state, op.id);
  // An operation that cut NOTHING is a refusal, not an operation. The
  // derivation can only check what it can see (a floor, a wall, a depth);
  // the generator finds the rest -- a 6 mm drill asked for an 8 mm hole --
  // and until now its answer arrived as a red line under a dead row in the
  // list, which the user then had to notice and remove. Twice, in the
  // screenshot that prompted this. The verb says why and adds nothing; the
  // fix (another tool) is one click away in the same window.
  if (const std::string why = ToolpathError(state, op.id); !why.empty()) {
    setup->operations.pop_back();
    DropCamToolpath(state, op.id);
    state.camGhostOp = 0;
    state.camRefusal = why;
    RunCamVerification(state);
    return 0;
  }
  RunCamVerification(state);
  LOG_INFO("CAM: {} from a picked face, Z {:.3f} to {:.3f}", op.name, op.topZ,
           op.bottomZ);
  return op.id;
}

// The verb's newest operation, derived again for a different cutter. Every
// number in it is a fraction of the tool, so a tool change after the fact is
// a re-derivation and not a field edit -- and doing it here is what lets the
// no-pick verbs stay ONE click: the cut is made with the tool in the spindle,
// and the window offers the others without a remove-and-redo.
void RederiveVerbOp(AppState& state, cam::Setup& setup, const Verb& verb,
                    int tool) {
  cam::Operation* op = nullptr;
  for (cam::Operation& o : setup.operations)
    if (o.id == state.camVerbOp)
      op = &o;
  if (op == nullptr)
    return;
  JobUndoGuard undoGuard(state);
  const cam::Target target =
      op->targets.empty() ? cam::Target{} : op->targets.front();
  const cam::DerivedOp derived = cam::DeriveOperation(
      setup, ui::JobBodies(state), verb.type, target, tool);
  if (!derived.ok) {
    state.camRefusal = derived.why;
    return;
  }
  const uint64_t id = op->id;
  const cam::Operation before = *op;
  *op = derived.op;
  op->id = id;
  state.camRefusal.clear();
  state.camGhostOp = id;
  state.projectMetaDirty = true;
  RefreshCamToolpath(state, id);
  // Same rule as making one: a tool the cut cannot be made with is refused,
  // and the cut stays as it was rather than becoming a dead row.
  if (const std::string why = ToolpathError(state, id); !why.empty()) {
    *op = before;
    state.camVerbTool = before.toolNumber;
    RefreshCamToolpath(state, id);
    state.camRefusal = why;
  }
  RunCamVerification(state);
}

// The picked face's plane, in MODEL space. Both orientation picks need it,
// and neither wants the setup frame applied — the whole point is to work out
// what the frame should become.
// Turning the part re-answers where zero is: the origin is a point in SETUP
// space and the setup just moved. Leaving it stale made the panel report a Z
// zero from the previous orientation — the one number that breaks tools,
// stated confidently and wrong — and since v68 it also offsets every
// coordinate in the posted file.
void ReZeroFromTopFace(AppState& state, cam::Setup& setup) {
  if (setup.topFace.IsSet()) {
    const cam::FaceRegion region = cam::PickedFaceRegion(
        cam::BodiesInSetupFrame(setup, ui::JobBodies(state)),
        setup.topFace.ref);
    // The face that was level is not level any more: this orientation cannot
    // keep that Z zero, and pretending otherwise is worse than asking again.
    if (!region.ok)
      setup.topFace = {};
  }
  (void)cam::RecomputeOrigin(setup, ui::JobBodies(state));
}

// Everything that has to follow the setup's orientation changing — and it
// says what it did. Re-clamping moves the box every derived number is measured
// from, so a job that already had operations quietly became a different job;
// the depths follow the frame, and the count is reported rather than left for
// the user to notice in the backplot.
void CamOrientationChanged(AppState& state, cam::Setup& setup) {
  const bool hadZero = setup.topFace.IsSet();
  ReZeroFromTopFace(state, setup);
  const int moved = cam::RederiveDepths(setup, ui::JobBodies(state));
  setup.safeZ = cam::DeriveSafeZ(setup, ui::JobBodies(state));
  state.camSafeZEdit = static_cast<float>(setup.safeZ);

  std::string note;
  if (hadZero && !setup.topFace.IsSet())
    note = "the face Z zero came from is not level any more — pick a top face";
  if (moved > 0)
    note += (note.empty() ? "" : "; ") + std::to_string(moved) +
            (moved == 1 ? " operation's depths" : " operations' depths") +
            " re-derived in the new clamping";
  state.camNote = note;

  state.slotPick = {};
  state.projectMetaDirty = true;
  RefreshCamToolpaths(state);
  RunCamVerification(state);
}

} // namespace

// Clamp ONE part this way -- or the whole setup, when there is only one part
// to clamp.
//
// Fixturing is a claim about a part in a fixture (v76), so with several parts
// in one stock it belongs to each of them. With one part it belongs to the
// setup, which is where it has always lived and where every single-part job
// still finds it: writing a per-part frame there instead would be a change of
// meaning nobody asked for.
//
// Returns true when something actually moved.
bool SetFixtureFrame(AppState& state, cam::Setup& setup, uint64_t jobPartId,
                     const glm::dquat& want) {
  // A part that HAS a placement is clamped THERE. That is also what
  // `set_job_placement lay_face_up` does over the RPC, and the two doors
  // agreeing is the point (clicking_and_scripting_agree): keying on the
  // part COUNT instead made an agent and a user produce different documents
  // from the same intent.
  if (jobPartId != 0)
    for (cam::Placement& p : setup.placements)
      if (p.jobPartId == jobPartId) {
        const glm::dquat had = p.frame ? *p.frame : setup.frame;
        if (std::abs(glm::dot(want, had)) >= 1.0 - 1e-9)
          return false;
        p.frame = want;
        return true;
      }
  // No placement. With one part there is nothing to distinguish it from, so
  // the setup's frame IS the answer and stays where it has always been. With
  // several, laying a face up is a statement about THIS part, so it gets a
  // placement rather than quietly reorienting every other one.
  if (state.job == nullptr || state.job->PartCount() <= 1 || jobPartId == 0) {
    if (std::abs(glm::dot(want, setup.frame)) >= 1.0 - 1e-9)
      return false;
    setup.frame = want;
    return true;
  }
  cam::Placement fresh;
  fresh.id = state.job->AllocateId();
  fresh.jobPartId = jobPartId;
  fresh.frame = want;
  setup.placements.push_back(fresh);
  return std::abs(glm::dot(want, setup.frame)) < 1.0 - 1e-9;
}

void CamFacePicked(AppState& state, const SelectionItem& item) {
  const PickSlot slot = static_cast<PickSlot>(state.slotPick.slot);
  if (!item.element.IsSet() || !state.job) {
    state.slotPick.miss = "that is not a face this job can use";
    return;
  }
  JobUndoGuard undoGuard(state); // one step per pick (R7)

  if (slot == PickSlot::CamTopFace) {
    cam::Setup* setup = EnsureCamSetup(state);
    if (setup == nullptr)
      return;
    // Calling a face "the top" IS the statement that the part is clamped with
    // it up, so this lays it up rather than refusing. For a face already level
    // in this setup `SolveFaceUp` returns the frame it was given, so nothing
    // moves — the two were only ever separate steps for the faces that used to
    // be refused, and making the user translate a refusal into two more clicks
    // is not a design.
    //
    // "Lay a face up" stays, for re-clamping WITHOUT re-answering Z zero — the
    // flip that machines the back off a reference you have already set.
    bool reclamped = false;
    glm::vec3 normal(0.0f);
    const cam::Target picked =
        ui::TargetFor(state, item.element, item.jobPartId);
    if (cam::FaceNormalOf(ui::JobBodies(state), item.element, normal)) {
      const glm::dquat had = cam::FixtureFrame(*setup, picked.jobPartId);
      reclamped = SetFixtureFrame(state, *setup, picked.jobPartId,
                                  cam::SolveFaceUp(glm::dvec3(normal), had));
    }
    const cam::FaceRegion region = cam::PickedFaceRegion(
        cam::BodiesInSetupFrame(*setup, ui::JobBodies(state)), item.element);
    if (!region.ok) {
      state.slotPick.miss = region.why;
      return;
    }
    // Z zero is this face: where the operator touches off. How much material
    // sits ABOVE the part is a different statement, and this used to make it
    // for them — zeroing the stock allowance on the reading that "the picked
    // face IS the top of the stock". That threw away an allowance already
    // typed, and it made "Face the top" refuse immediately after the pick
    // that is meant to set the job up: the first operation on a rough board
    // needed a detour into Advanced to undo what this had just done
    // (docs/cam/dogfood-round.md, round 2). The default 1 mm is the rough
    // board every facing pass exists for; a board already flat is one number.
    setup->topFace = ui::TargetFor(state, item.element, item.jobPartId);
    // Through the ONE derivation, so a Z zero picked here and a work origin
    // set from the panel cannot disagree about where the file's zero is.
    (void)cam::RecomputeOrigin(*setup, ui::JobBodies(state));
    state.camNote.clear();
    if (reclamped) {
      const int moved = cam::RederiveDepths(*setup, ui::JobBodies(state));
      state.camNote = "laid the part flat on that face";
      if (moved > 0)
        state.camNote +=
            ", and re-derived " + std::to_string(moved) +
            (moved == 1 ? " operation's depths" : " operations' depths");
    }
    // The stock just moved, so the rapid height that cleared the old one may
    // now be inside the new one.
    setup->safeZ = cam::DeriveSafeZ(*setup, ui::JobBodies(state));
    state.camSafeZEdit = static_cast<float>(setup->safeZ);
    // Operations that started above the stock come down to it — never up: an
    // operation asked for a shallower top than the stock is a choice.
    glm::dvec3 lo(0.0), hi(0.0);
    if (cam::StockBounds(*setup, ui::JobBodies(state), lo, hi))
      for (cam::Operation& op : setup->operations)
        if (op.topZ > hi.z)
          op.topZ = hi.z;
    state.slotPick = {};
    state.projectMetaDirty = true;
    RefreshCamToolpaths(state);
    RunCamVerification(state);
    return;
  }

  if (slot == PickSlot::CamLayFaceUp) {
    // S2: clamp the part so this face looks at the spindle. A part is
    // modelled where it belongs in the assembly — a speaker baffle stands
    // upright — and this is the statement that it lies flat on the bed for
    // the job. It says nothing about the machine: no A word is posted, and
    // any planar face works, because a clamp is not a trunnion.
    cam::Setup* setup = EnsureCamSetup(state);
    if (setup == nullptr)
      return;
    glm::vec3 normal(0.0f);
    if (!cam::FaceNormalOf(ui::JobBodies(state), item.element, normal)) {
      state.slotPick.miss = "pick a flat face — that one is curved";
      return;
    }
    const uint64_t part =
        ui::TargetFor(state, item.element, item.jobPartId).jobPartId;
    const glm::dquat had = cam::FixtureFrame(*setup, part);
    (void)SetFixtureFrame(state, *setup, part,
                          cam::SolveFaceUp(glm::dvec3(normal), had));
    CamOrientationChanged(state, *setup);
    const glm::dquat now = cam::FixtureFrame(*setup, part);
    LOG_INFO("CAM: laid a face up, frame ({:.4f} {:.4f} {:.4f} {:.4f})", now.w,
             now.x, now.y, now.z);
    return;
  }

  if (slot == PickSlot::CamIndexFace) {
    // C9: turn the TABLE until this face looks at the spindle — the rotary
    // path, which posts an A word. How the part is clamped is `frame` and the
    // branch above; this one is a statement about the machine.
    cam::Setup* setup = EnsureCamSetup(state);
    if (setup == nullptr)
      return;
    glm::vec3 normal(0.0f);
    if (!cam::FaceNormalOf(ui::JobBodies(state), item.element, normal)) {
      state.slotPick.miss = "pick a flat face — that one is curved";
      return;
    }
    double degrees = 0.0;
    if (!cam::SolveIndexA(glm::dvec3(normal), degrees)) {
      state.slotPick.miss = "no rotation about X can bring that face up — it "
                            "points along the rotary axis. Lay it up instead: "
                            "a clamp can hold the part any way round.";
      return;
    }
    setup->rotationA = degrees;
    CamOrientationChanged(state, *setup);
    LOG_INFO("CAM: indexed to A{:.3f}", degrees);
    return;
  }

  const cam::OpType type =
      slot == PickSlot::CamPocketFloor ? cam::OpType::Pocket
      : slot == PickSlot::CamEdgeWall  ? cam::OpType::EdgeRound
      : slot == PickSlot::CamDrillHole ? cam::OpType::Drill
                                       : cam::OpType::Profile;
  // The slot stays armed either way. A refusal keeps it because the user's
  // next click is almost always the face they meant; a SUCCESS keeps it
  // because one verb is almost never one face — a part has six holes to
  // clear, not one, and re-pressing "Clear a pocket" between each was a click
  // per hole spent saying something already said. Escape disarms, and so does
  // pressing the verb again.
  const std::string prompt = state.slotPick.prompt;
  if (const uint64_t made =
          CreateVerbOperation(state, type, item.element, item.jobPartId)) {
    state.camVerbOp = made; // the one a tool change in the window re-derives
    state.slotPick.miss.clear();
    state.camNote = "added — keep clicking faces, or press Escape when done";
  } else {
    // The pick's own line is where the refusal shows; the window used to
    // print it a second time from camRefusal directly under it.
    state.slotPick.miss = state.camRefusal;
    state.camRefusal.clear();
  }
  state.slotPick.prompt = prompt;
}

int VerbTool(const AppState& state, const cam::Setup& setup) {
  if (setup.tools.empty())
    return 0;
  if (state.camVerbTool != 0 && setup.FindTool(state.camVerbTool) != nullptr)
    return state.camVerbTool;
  // The last tool USED: the newest operation's. Operations are kept in the
  // order they were made, so the back is the most recent -- and this holds
  // across sessions, because the operations are in the document and the
  // session's choice is not.
  for (auto it = setup.operations.rbegin(); it != setup.operations.rend(); ++it)
    if (setup.FindTool(it->toolNumber) != nullptr)
      return it->toolNumber;
  return setup.tools.front().number;
}

void LookDownTheSpindle(AppState& state) {
  const cam::Setup* setup = ActiveCamSetup(state);
  if (state.camera == nullptr || setup == nullptr)
    return;
  // The camera goes where the spindle is and looks the way it looks, with
  // the setup's own +Y up the screen. Snap rather than animate: this is a
  // "show me the job" button, not a flourish.
  // The spindle points down setup +Z, and a job tab DRAWS setup space, so
  // that is the world direction too — no frame to come back through.
  const glm::vec3 dir(0.0f, 0.0f, 1.0f);
  const glm::vec3 up(0.0f, 1.0f, 0.0f);
  state.camera->SnapToView(dir, up);
  // …and framed on the STOCK, which is the thing being cut. Keeping the
  // previous zoom gave a plan view of four square millimetres if that is
  // where you happened to be.
  glm::dvec3 lo(0.0), hi(0.0);
  if (cam::StockBounds(*setup, ui::JobBodies(state), lo, hi)) {
    // StockBounds reports the box in SETUP space, which is what the job tab
    // draws — the box IS the world box.
    FitCameraToWorldBox(state, glm::vec3(lo), glm::vec3(hi));
    // Fitting moves the TARGET, and Camera::SetTarget keeps the camera
    // where it is and re-aims — so it swings the view off the axis that
    // was just set. Aim first (so the fit's pan away from this window is
    // computed in the right frame), fit, then aim again.
    state.camera->SnapToView(dir, up);
  }
}

void StartCamVerb(AppState& state, AppState::CamVerb verb) {
  if (!state.job)
    return;
  const Verb* v = FindVerb(verb);
  if (v == nullptr) { // None: the plain session window
    state.camToolOpen = true;
    state.camVerb = verb;
    return;
  }
  // Pressing the armed verb again puts it away. A verb that STAYS armed
  // after a pick needs a way out that is not only the Escape key.
  const bool armedHere = v->picks && state.camVerb == verb &&
                         state.slotPick.featureId == kCamToolSlot &&
                         state.slotPick.slot == static_cast<int>(v->slot);
  if (armedHere) {
    state.slotPick = {};
    state.camNote.clear();
    state.camToolOpen = false;
    state.camVerb = CamVerb::None;
    state.camGhostOp = 0;
    return;
  }
  state.camToolOpen = true;
  state.camVerb = verb;
  state.camVerbOp = 0;
  state.camRefusal.clear();
  state.camNote.clear();
  if (v->picks) {
    // Any pick button switches to itself: greying the others out while one
    // was armed meant Escape between every change of verb.
    ArmCamPick(state, v->slot, v->prompt);
    return;
  }
  // No pick: the cut is made NOW, with the tool in the spindle. The window
  // that opens shows it, and offers the other tools (RederiveVerbOp).
  if (state.slotPick.featureId == kCamToolSlot)
    state.slotPick = {};
  state.camVerbOp = CreateVerbOperation(state, v->type, {});
}

namespace {

// The cutter row: which tool the verb derives for, the one in use marked.
// Shown for every job with a setup, one tool or six -- a verb that derives
// every number from the cutter should say which cutter, and "the last one
// used" is a default worth being able to see.
void VerbToolRow(AppState& state, cam::Setup& setup, const Verb& verb) {
  if (setup.tools.empty())
    return;
  const int active = VerbTool(state, setup);
  ImGui::TextDisabled("Tool");
  for (const cam::Tool& tool : setup.tools) {
    ImGui::SameLine();
    const bool on = tool.number == active;
    if (on)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    char id[40];
    std::snprintf(id, sizeof(id), "T%d###cam_verb_t%d", tool.number,
                  tool.number);
    if (ImGui::SmallButton(id) && !on) {
      state.camVerbTool = tool.number;
      if (state.camVerbOp != 0)
        RederiveVerbOp(state, setup, verb, tool.number);
    }
    if (on)
      ImGui::PopStyleColor();
    ImGui::SetItemTooltip("%.3g mm %s — what the verb derives for%s",
                          tool.diameter, tool.name.c_str(),
                          state.camVerbOp != 0 ? "; changes the cut just made"
                                               : "");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(more: CAM panel › Tools)");
  // Then finish with (R6): only offered when there is a second tool to
  // finish with. "none" is a button too, so the choice can be undone.
  if (setup.tools.size() < 2)
    return;
  ImGui::TextDisabled("Then finish with");
  ImGui::SameLine();
  const bool noneOn = state.camVerbFinishTool == 0;
  if (noneOn)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  if (ImGui::SmallButton("none###cam_verb_finish_none"))
    state.camVerbFinishTool = 0;
  if (noneOn)
    ImGui::PopStyleColor();
  for (const cam::Tool& tool : setup.tools) {
    if (tool.number == active)
      continue;
    ImGui::SameLine();
    const bool on = tool.number == state.camVerbFinishTool;
    if (on)
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    char id[48];
    std::snprintf(id, sizeof(id), "T%d###cam_verb_finish_t%d", tool.number,
                  tool.number);
    if (ImGui::SmallButton(id))
      state.camVerbFinishTool = on ? 0 : tool.number;
    if (on)
      ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Rough with T%d leaving a little, then take the "
                          "wall in one pass with this %.3g mm %s",
                          active, tool.diameter, tool.name.c_str());
  }
}

// Red, wrapped at the verdict's width. Unwrapped, one long refusal stretched
// the floating window to the width of the sentence — across the whole app,
// over the panel and the model both.
void RefusalText(const std::string& text) {
  ImGui::PushTextWrapPos(320.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
  ImGui::TextWrapped("%s", text.c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
}

// What the job is, in one sentence per operation — the numbers are derived,
// so they are shown rather than asked for. The drag fields still exist, in
// the CAM panel, for anyone who wants them.
void OperationList(AppState& state, cam::Setup& setup) {
  if (setup.operations.empty())
    return;
  ImGui::Separator();
  // Deferred, so the list is not mutated while it is being walked — the
  // same shape the docked panel uses.
  std::vector<uint64_t> removeOps; // the row's own x button
  bool deleteSelected = false;     // the row menu's Delete (the selection)
  std::vector<uint64_t> order;
  for (const cam::Operation& op : setup.operations)
    order.push_back(op.id);
  for (const cam::Operation& op : setup.operations) {
    const cam::Tool* tool = setup.FindTool(op.toolNumber);
    const double depth = op.topZ - op.bottomZ;
    const int passes = static_cast<int>(cam::DepthPasses(op).size());
    ImGui::PushID(static_cast<int>(op.id));
    const bool ghosted = state.camGhostOp == op.id;
    if (ImGui::RadioButton("###ghost", ghosted))
      state.camGhostOp = ghosted ? 0 : op.id;
    ImGui::SetItemTooltip("Show what this one removes");
    ImGui::SameLine();
    // Removing an operation was only possible in the DOCKED panel, which is
    // a tab behind Model — while every operation is made here. An operation
    // added by mistake (or three, when the first two silently cut nothing)
    // could not be taken back without going to find another window
    // (docs/cam/dogfood-round.md).
    if (ImGui::SmallButton("x###rmop"))
      removeOps = {op.id};
    ImGui::SetItemTooltip("Remove this operation");
    ImGui::SameLine();
    // What the operation SAYS about itself, from its own fields — and then
    // what actually came of it. This line used to be the description alone,
    // so an operation that generated nothing rendered identically to one
    // that cut the part: the user clicked "Clear a pocket" three times at a
    // face that never changed, because nothing in this window ever said the
    // op was dead (docs/cam/dogfood-round.md). The docked panel printed it;
    // the floating window is where the work happens.
    const cam::Toolpath* path = nullptr;
    for (const cam::Toolpath& tp : state.camToolpaths)
      if (tp.operationId == op.id && (path == nullptr || !tp.error.empty()))
        path = &tp; // the one with something to say, else the last
    char sentence[256];
    std::snprintf(sentence, sizeof(sentence),
                  "%s: %.3g mm tool, %.3g mm deep in %d pass%s, %s%s",
                  op.name.c_str(), tool != nullptr ? tool->diameter : 0.0,
                  depth, passes, passes == 1 ? "" : "es",
                  op.climb ? "climb" : "conventional",
                  op.type == cam::OpType::Adaptive ? ", adaptive" : "");
    // The sentence is the ROW — select, Ctrl/Shift-extend, right-click
    // Delete — the same row the docked panel draws (CamOperationRow).
    if (CamOperationRow(state, op.id, sentence, order))
      deleteSelected = true;
    if (path != nullptr && !path->error.empty()) {
      RefusalText(std::string(ICON_FA_TRIANGLE_EXCLAMATION) +
                  " cut nothing — " + path->error);
    } else if (path != nullptr && !path->strategy.empty()) {
      // It cut, but not by the strategy its type names. Feeds were chosen
      // for one of them.
      ImGui::PushTextWrapPos(320.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.40f, 1.0f));
      ImGui::TextWrapped("%s cut %s", ICON_FA_TRIANGLE_EXCLAMATION,
                         path->strategy.c_str());
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
    }
    ImGui::PopID();
  }
  if (deleteSelected)
    DeleteSelection(state);
  else if (!removeOps.empty())
    RemoveCamOperations(state, removeOps);
}

} // namespace

void DrawCamToolWindow(AppState& state) {
  // The session belongs to the JOB (v73), so without one there is nothing to
  // draw and nothing to keep open. It used to say "switch to a part tab" and
  // stay up, which is now both wrong and expensive: a floating tool window
  // over the viewport eats the clicks of whatever the user does next -- it
  // was a sketch trim, two tests later in a shuffled GUI run.
  if (!state.job) {
    state.camToolOpen = false;
    state.camVerb = CamVerb::None;
    state.camVerbOp = 0;
    state.camGhostOp = 0;
    if (state.slotPick.featureId == kCamToolSlot)
      state.slotPick = {};
    return;
  }
  const Verb* verb = FindVerb(state.camVerb);
  BeginToolWindowFrame(state, verb != nullptr ? verb->label : "CAM");

  cam::Setup* setup = ActiveCamSetup(state);
  const bool armed = state.slotPick.featureId == kCamToolSlot;
  const bool armedHere = verb != nullptr && armed && verb->picks &&
                         state.slotPick.slot == static_cast<int>(verb->slot);

  // What the verb is for, in a sentence, and where its pick stands.
  if (verb != nullptr) {
    ImGui::PushTextWrapPos(320.0f);
    ImGui::TextDisabled("%s", verb->tip);
    ImGui::PopTextWrapPos();
    switch (state.camVerb) {
    case CamVerb::TopFace:
      if (armedHere)
        ImGui::TextDisabled("click the top face…");
      else if (setup != nullptr && setup->topFace.IsSet())
        ImGui::TextDisabled("Z zero at %.3f", setup->origin.z);
      else
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                           "Z zero is not set yet");
      break;
    case CamVerb::LayFaceUp:
      if (armedHere)
        ImGui::TextDisabled("pick a face…");
      else if (setup != nullptr && !cam::FrameIsSquare(*setup))
        ImGui::TextDisabled("laid flat");
      else
        ImGui::TextDisabled("as modelled");
      break;
    case CamVerb::IndexFace:
      if (armedHere)
        ImGui::TextDisabled("pick a face…");
      else if (setup != nullptr && std::abs(setup->rotationA) > 1e-9)
        ImGui::TextDisabled("A %.2f°", setup->rotationA);
      else
        ImGui::TextDisabled("A 0° (3-axis)");
      break;
    default:
      if (armedHere)
        // The slot stays armed after a pick: one verb is almost never one
        // face. Say how it ends, since the pill only says how it starts.
        ImGui::TextDisabled("click faces… Escape, or the button again, when "
                            "done");
      break;
    }
    if (verb->cutter && setup != nullptr)
      VerbToolRow(state, *setup, *verb);
  }

  // Why the last pick did not take. This window never drew it, so a refused
  // click looked exactly like a click that did nothing — and one of them
  // ("add a setup first") was reachable on any fresh part
  // (docs/cam/dogfood-round.md). The docked panel printed it all along.
  if (armed && !state.slotPick.miss.empty())
    RefusalText(state.slotPick.miss);
  // …and what the last action DID, when it did more than was asked. Laying
  // the part flat to take a Z zero, or re-deriving depths after a re-clamp,
  // are consequences the user has to be told about — the alternative is a job
  // that changed under them between one click and the next.
  if (!state.camNote.empty()) {
    ImGui::PushTextWrapPos(320.0f);
    ImGui::TextDisabled("%s", state.camNote.c_str());
    ImGui::PopTextWrapPos();
  }
  if (!state.camRefusal.empty())
    RefusalText(state.camRefusal);

  // The job so far: for the machining verbs and the plain window. The
  // orientation picks are one question each, and the list under them was
  // noise.
  if (setup != nullptr && (verb == nullptr || verb->cutter))
    OperationList(state, *setup);

  ImGui::Separator();
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
    state.camVerb = CamVerb::None;
    state.camVerbOp = 0;
    state.camGhostOp = 0;
    state.camRefusal.clear();
    if (state.slotPick.featureId == kCamToolSlot)
      state.slotPick = {};
  }
  ImGui::End();
}

void AppendCamVisualisation(AppState& state, std::vector<PrimVertex>& prims,
                            std::vector<LineVertex3D>& quads) {
  if (!state.job)
    return;
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;
  const bool session = state.camToolOpen;

  // The stock, ALWAYS on a job tab (round two, M2): it is the thing the
  // parts are placed in, and drawing it only while a verb window was open
  // left it invisible most of the time. "Only in the session" was written
  // when CAM lived on the part tab, where the box was clutter over the model
  // being drawn; a job tab draws nothing else.
  glm::dvec3 lo(0.0), hi(0.0);
  if (state.camShowStock &&
      cam::StockBounds(*setup, ui::JobBodies(state), lo, hi)) {
    const std::vector<glm::dvec2> rect = {
        {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
    LoopLines(rect, lo.z, 1.0f, kStockLine, prims);
    LoopLines(rect, hi.z, 1.2f, kStockLine, prims);
    for (const glm::dvec2& c : rect)
      AppendPrimLine(glm::vec3(c.x, c.y, lo.z), glm::vec3(c.x, c.y, hi.z), 1.0f,
                     kStockLine, prims);
    LoopWalls(rect, lo.z, hi.z, kStockFill, quads);

    // ...and the MACHINE's work area around it, when the machine states one:
    // a box the size of the travel, its near-left corner at the stock's and
    // its floor at the stock's bottom, which is how a sheet is loaded. Where
    // the operator touches off inside the travel is not the job's to know,
    // so this is the machine's SIZE against the job's -- the check
    // cam::MachineWarning makes in words. Never resized to fit.
    glm::dvec3 bedLo(0.0), bedHi(0.0);
    if (cam::MachineBounds(*setup, ui::JobBodies(state), bedLo, bedHi)) {
      // The SAME box the generator clips to (cam::MachineBounds): what is
      // drawn as the machine is what the file stays inside.
      const std::vector<glm::dvec2> bed = {{bedLo.x, bedLo.y},
                                           {bedHi.x, bedLo.y},
                                           {bedHi.x, bedHi.y},
                                           {bedLo.x, bedHi.y}};
      const bool fits =
          cam::MachineWarning(*setup, ui::JobBodies(state)).empty();
      const glm::vec4 line = fits ? kBedLine : kBedRefusedLine;
      LoopLines(bed, bedLo.z, 1.0f, line, prims);
      LoopLines(bed, bedHi.z, 0.8f, line, prims);
      for (const glm::dvec2& c : bed)
        AppendPrimLine(glm::vec3(c.x, c.y, bedLo.z),
                       glm::vec3(c.x, c.y, bedHi.z), 0.8f, line, prims);
    }
  }

  // What the selected operation removes.
  if (session && state.camGhostOp != 0)
    if (const cam::Operation* op = FindOp(*setup, state.camGhostOp))
      OperationGhost(state, *setup, *op, prims, quads);

  // The candidate under the cursor, BEFORE the click: the region that would
  // be cleared, at the height it would be cleared to.
  if (session && state.slotPick.featureId == kCamToolSlot &&
      state.pick.hoverFace.IsSet()) {
    const cam::FaceRegion& region =
        CachedRegion(state, state.camHoverRegion, *setup, state.pick.hoverFace);
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
  if (!state.camToolOpen || !state.job || state.camGhostOp == 0)
    return;
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;
  const cam::Operation* op = FindOp(*setup, state.camGhostOp);
  if (op == nullptr || op->targets.empty())
    return;
  const cam::FaceRegion region = cam::PickedFaceRegion(
      cam::BodiesInSetupFrame(*setup, ui::JobBodies(state)),
      op->targets[0].ref);
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
  if (!state.job || state.camToolpaths.empty())
    return;
  if (const std::string why = CamToolpathsPendingWhy(state); !why.empty()) {
    // A verdict about the previous toolpath is worse than none. Say what is
    // happening and verify when the last pass lands (CamJobs.cpp).
    state.camVerifyWhenSettled = true;
    state.camVerdict.push_back(
        {cam::VerdictLine::Level::Warn, "Not verified yet: " + why +
                                            ". The verdict "
                                            "follows when the passes land."});
    return;
  }
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;

  std::string gcode, error;
  if (!cam::PostProcess(setup->machine.post, *setup, state.camToolpaths,
                        PostOpts(*setup), gcode, &error)) {
    state.camVerdict.push_back(
        {cam::VerdictLine::Level::Bad, "Could not post this job: " + error});
    return;
  }
  // 0.4 mm cells: fine enough to catch a gouge that matters on a mill, coarse
  // enough that the check runs while the user is still looking at the button.
  constexpr double kRes = 0.4;
  std::vector<cam::Toolpath::Relief> reliefs;
  for (const cam::Toolpath& tp : state.camToolpaths)
    reliefs.insert(reliefs.end(), tp.reliefs.begin(), tp.reliefs.end());
  const cam::Inspection insp =
      cam::Inspect(gcode, *setup, ui::JobBodies(state), kRes, reliefs);
  if (!insp.ok)
    return;
  // The tolerance IS the grid: a "gouge" of half a cell is quantisation, and
  // reporting it would teach the user to ignore the verdict.
  const double tolerance = kRes * 1.5;
  bool breaksAnEdge = false;
  for (const cam::Operation& op : setup->operations)
    breaksAnEdge |= op.enabled && op.type == cam::OpType::EdgeRound;
  state.camVerdict = cam::Explain(
      insp.verdict, tolerance, cam::CuttingMinutes(*setup, state.camToolpaths),
      breaksAnEdge);
  // A target one operation could not follow while its others cut. It is not
  // an `error` -- that would drop the good targets with the bad one -- so
  // nothing else in the panel would say the file is short of a hole.
  for (const cam::Toolpath& tp : state.camToolpaths)
    for (const std::string& why : tp.skipped) {
      std::string name;
      for (const cam::Operation& op : setup->operations)
        if (op.id == tp.operationId)
          name = op.name;
      state.camVerdict.push_back(
          {cam::VerdictLine::Level::Warn, name + ": " + why});
    }
  // Feeds and speeds against the material being cut: the same lines the RPC
  // returns, so the panel and a script cannot disagree about them.
  const cam::FeedReport feeds = cam::FeedsFor(*setup, ui::JobBodies(state));
  for (const cam::Finding& f : feeds.findings)
    state.camVerdict.push_back({cam::VerdictLine::Level::Warn, f.text});
  // What could NOT be checked, said as such: silence reads as approval.
  for (const std::string& u : feeds.unchecked)
    state.camVerdict.push_back(
        {cam::VerdictLine::Level::Warn, "Not checked: " + u});
  // The machine's lines: the parts' reach beyond the travel, or the sheet's
  // overhang -- warnings, never refusals. Cut as far as it reaches.
  for (const cam::VerdictLine& line :
       cam::MachineLines(*setup, ui::JobBodies(state), &state.camToolpaths))
    state.camVerdict.push_back(line);
  state.camLeftover = cam::LeftoverPoints(insp, tolerance, 3000);
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
void DrawCamToolWindow(AppState&) {}
void StartCamVerb(AppState&, AppState::CamVerb) {}
int VerbTool(const AppState&, const cam::Setup&) { return 0; }
void LookDownTheSpindle(AppState&) {}
void ArmCamPick(AppState&, PickSlot, const char*) {}
void CamFacePicked(AppState&, const SelectionItem&) {}
void AppendCamVisualisation(AppState&, std::vector<PrimVertex>&,
                            std::vector<LineVertex3D>&) {}
void DrawCamLabels(AppState&) {}
void RunCamVerification(AppState&) {}
} // namespace ui

#endif
