#include "ui/CamTool.h"
#include "ui/Overlays.h" // ViewportDrawList

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamEngrave.h"
#include "core/cam/CamFeeds.h"
#include "core/cam/CamMount.h" // HeldInRotary, NormalizeIndexDeg
#include "core/cam/CamNames.h"
#include "core/cam/CamOtherSetups.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "core/io/Serialization.h" // CamSetupToJson: the stock view's key
#include "ui/AppActionsInternal.h" // kSketchColor
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/CamPanel.h"
#include "ui/DocumentSlots.h"
#include "ui/ExprInput.h"
#include "ui/JobActions.h"
#include "ui/ListSelection.h"
#include "ui/SketchOverlay.h"
#include "ui/ViewActions.h" // AppendPrimLine/Dot, WorldToScreen

#include "render/Camera.h"
#include "ui/FeatureEditor.h"
#include "ui/PreviewFeature.h"    // BeginToolWindowFrame
#include "ui/SketchInteraction.h" // WorldToScreen (the depth label)

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <limits>
#include <optional>

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

// The stock and the machine's box, CACHED. The job tab draws both every
// frame, and each asks OCC for the tight bounding box of every body framed for
// the setup -- three times over (StockBounds, MachineBounds, MachineWarning).
// On a turned blank's toroidal faces that is a particle-swarm optimisation per
// face: a second a frame on the chess set's undersides job, which ran at one
// frame a second while doing nothing. The key is the job's geometry and the
// setup as it is saved, less its operations, which none of the three reads --
// so any edit that could move a box moves the key.
struct StockView {
  bool stock = false;
  glm::dvec3 lo{0.0}, hi{0.0};
  bool bed = false;
  glm::dvec3 bedLo{0.0}, bedHi{0.0};
  bool fits = true;
};

const StockView& CachedStockView(AppState& state, const cam::Setup& setup) {
  // One memo, not one per AppState: it is keyed on everything it answers
  // from, so a second state (a test) can only miss, never read a stale box.
  static StockView view;
  static std::string key;
  // Asked first: it is what refreshes `jobBodiesKey`.
  const std::vector<Body>& bodies = ui::JobBodies(state);
  cam::Setup shape = setup;
  shape.operations.clear();
  std::string now = std::to_string(state.jobBodiesKey) + "|" +
                    DocumentIO::CamSetupToJson(shape).dump();
  if (now == key)
    return view;
  key = std::move(now);
  view = StockView{};
  view.stock = cam::StockBounds(setup, bodies, view.lo, view.hi);
  view.bed = cam::MachineBounds(setup, bodies, view.bedLo, view.bedHi);
  view.fits = !view.bed || cam::MachineWarning(setup, bodies).empty();
  return view;
}

// The sketch curves a job tab draws and an engraving can be pointed at
// (docs/cam/gui.md#engrave-from-the-viewport): each job part's sketches that
// hold a label or an open curve (cam::EngravableEntities), laid out through
// the part's placement in the active setup -- BodyToSetup, the transform its
// bodies take -- so a curve is drawn ON the part it marks. CACHED: resolving
// a label lays its glyphs out, which is not per-frame work. The key is what
// the job tab draws (every part's version, the active setup, every frame and
// placement), so a sketch edited in its part, or a re-clamp, rebuilds it.
const cam::Operation* FindOp(const cam::Setup& setup, uint64_t id);

struct EngraveCandidate {
  uint64_t jobPartId = 0;
  ElementRef ref;
  std::vector<std::vector<glm::vec3>> strokes; // job-tab (setup) space
};

const std::vector<EngraveCandidate>& EngraveCandidates(AppState& state) {
  static const std::vector<EngraveCandidate> kNone;
  static std::vector<EngraveCandidate> cache;
  static uint64_t cacheKey = 0;
  static const cam::JobDocument* cacheJob = nullptr;
  const cam::JobDocument* job = ActiveJob(state);
  if (job == nullptr)
    return kNone;
  const uint64_t key = JobRenderVersion(state);
  if (key == cacheKey && job == cacheJob)
    return cache;
  cacheKey = key;
  cacheJob = job;
  cache.clear();
  const cam::Setup* setup = ActiveJobSetup(state);
  for (const cam::JobPart& part : job->GetParts()) {
    const Document* doc = JobPartDocument(state, part);
    if (doc == nullptr)
      continue;
    const glm::dmat4 m =
        setup != nullptr ? cam::BodyToSetup(*setup, part.id) : glm::dmat4(1.0);
    for (const ElementRef& ref : cam::EngravableEntities(*doc)) {
      const cam::SketchCurves curves = cam::SketchEntityCurves(*doc, ref);
      if (!curves.why.empty())
        continue;
      EngraveCandidate c;
      c.jobPartId = part.id;
      c.ref = ref;
      // Drawn a hair off the face it lies on, towards the viewer's side of
      // it: exactly ON the face, the depth test broke the letters up.
      const glm::dvec3 lift =
          0.02 * glm::normalize(glm::dmat3(m) * curves.planeNormal);
      for (const cam::SketchStroke& stroke : curves.strokes) {
        std::vector<glm::vec3> pts;
        pts.reserve(stroke.pts.size() + 1);
        for (const glm::dvec3& p : stroke.pts)
          pts.emplace_back(glm::dvec3(m * glm::dvec4(p, 1.0)) + lift);
        if (stroke.closed && pts.size() > 2 && pts.front() != pts.back())
          pts.push_back(pts.front());
        c.strokes.push_back(std::move(pts));
      }
      cache.push_back(std::move(c));
    }
  }
  return cache;
}

// The drawn curves: thin, in the sketch colour; the one a click would take,
// in the hover colour; an engraving's targets while it is shown, in the
// ghost's -- "this is what comes off".
void AppendEngraveCurves(AppState& state, const cam::Setup& setup,
                         std::vector<PrimVertex>& prims) {
  const std::vector<EngraveCandidate>& all = EngraveCandidates(state);
  if (all.empty())
    return;
  const cam::Operation* ghost = state.camToolOpen && state.camGhostOp != 0
                                    ? FindOp(setup, state.camGhostOp)
                                    : nullptr;
  if (ghost != nullptr && ghost->type != cam::OpType::Engrave)
    ghost = nullptr;
  const bool armed =
      state.slotPick.featureId == kCamToolSlot &&
      state.slotPick.slot == static_cast<int>(PickSlot::CamEngraveCurve);
  const glm::vec4 plain(kSketchColor, 0.85f);
  for (const EngraveCandidate& c : all) {
    const auto same = [&c](const ElementRef& r, uint64_t part) {
      return r.featureId == c.ref.featureId && r.name == c.ref.name &&
             (part == 0 || part == c.jobPartId);
    };
    bool target = false;
    if (ghost != nullptr)
      for (const cam::Target& t : ghost->targets)
        target = target || same(t.ref, t.jobPartId);
    const bool hovered =
        armed && same(state.pick.hoverCurve, state.pick.hoverCurvePart) &&
        state.pick.hoverCurve.IsSet();
    const glm::vec4 color = hovered ? kHoverLine : target ? kGhostLine : plain;
    const float halfPx = hovered ? 2.2f : target ? 1.6f : 0.9f;
    for (const std::vector<glm::vec3>& stroke : c.strokes)
      for (size_t i = 0; i + 1 < stroke.size(); i++)
        AppendPrimLine(stroke[i], stroke[i + 1], halfPx, color, prims);
  }
}

void OperationGhost(AppState& state, const cam::Setup& setup,
                    const cam::Operation& op, std::vector<PrimVertex>& prims,
                    std::vector<LineVertex3D>& quads) {
  // An engraving's target is a sketch curve, drawn by the curves' own
  // overlay (AppendEngraveCurves), not a face with a region.
  if (op.type == cam::OpType::Engrave)
    return;
  // Setup space: op.topZ/bottomZ are, so the boundary must be too (C9).
  std::vector<cam::Loop> loops;
  if (!op.targets.empty()) {
    const cam::FaceRegion& region =
        CachedRegion(state, state.camGhostRegion, setup, op.targets[0].ref);
    if (!region.ok)
      return;
    loops = region.loops;
  } else {
    // Model-space bodies: StockBounds frames them itself, and handed framed
    // ones it framed them TWICE -- the ghost of a turned setup drawn in the
    // wrong place.
    const StockView& view = CachedStockView(state, setup);
    if (!view.stock)
      return;
    const glm::dvec3 lo = view.lo, hi = view.hi;
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
  // How the verb's operation is derived: DeriveOperation for the type, or
  // one of the derivations that makes a choice the plain one does not --
  // layers and a skin (the two roughs), or a larger cutter to follow (Rest).
  enum class Derive { Standard, RotaryRough, SurfaceRough, Rest };
  Derive derive = Derive::Standard;
};

cam::DerivedOp
DeriveForVerb(Verb::Derive derive, const cam::Setup& setup,
              const std::vector<Body>& bodies, cam::OpType type,
              const cam::Target& target, int tool,
              const std::vector<const cam::Setup*>& earlier,
              const std::vector<cam::SketchCurves>& curves = {}) {
  switch (derive) {
  case Verb::Derive::RotaryRough:
    return cam::DeriveRotaryRough(setup, bodies, tool);
  case Verb::Derive::SurfaceRough:
    return cam::DeriveSurfaceRough(setup, bodies, tool);
  case Verb::Derive::Rest:
    return cam::DeriveRest(setup, bodies, tool);
  case Verb::Derive::Standard:
    break;
  }
  return cam::DeriveOperation(setup, bodies, type, target, tool, earlier,
                              curves);
}

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
    // The one verb whose pick is not a face: a label or a curve of a part's
    // sketch, drawn on the job tab (docs/cam/gui.md#engrave-from-the-viewport).
    {CamVerb::Engrave, "Engrave", cam::OpType::Engrave,
     PickSlot::CamEngraveCurve, true, true,
     "Click a label or a curve drawn on the part",
     "Cut a groove along a sketch's lines with a V-bit: a text label, or a "
     "line or curve drawn on the part. The groove's width comes from the "
     "label's height, its depth from the V-bit. Each click adds a label to "
     "the same engraving."},
    {CamVerb::Whole, "Cut out the whole part", cam::OpType::Profile,
     PickSlot::CamProfileOutline, false, true, "",
     "Follow the part's own outline — no pick needed."},
    {CamVerb::Face, "Face the top", cam::OpType::Face, PickSlot::CamTopFace,
     false, true, "", "Skim the material sitting above the part."},
    // The two verbs that take a curved or tilted face -- the pick every other
    // verb refuses. The face gives its OUTLINE and not its depths: those come
    // from the whole model's surface, because a pass taking Z from the picked
    // face alone would gouge whatever stands beside it. So the wording is
    // "within", not "of".
    {CamVerb::Surface, "Finish a surface", cam::OpType::SurfaceParallel,
     PickSlot::CamSurfaceRegion, true, true,
     "Click the surface to finish — curved or sloped is fine",
     "Rake a ball nose back and forth over the surface, following it in Z. "
     "This is the verb for anything the level-by-level ops refuse: a curved "
     "face, or a flat one that is tilted and so has no single depth. It "
     "FINISHES -- rough the bulk out first with Clear a pocket, then come "
     "back to this. Best on shallow surfaces; on a steep wall the passes "
     "crowd together and Finish walls suits it better."},
    {CamVerb::Walls, "Finish walls", cam::OpType::SurfaceWaterline,
     PickSlot::CamSurfaceRegion, true, true,
     "Click the surface whose walls to finish",
     "Lap the surface at constant depths, like contour lines. The opposite "
     "of Finish a surface and for the opposite shape: good where the wall is "
     "steep, poor where it is shallow, because the laps spread out as the "
     "surface flattens."},
    // The rotary verbs (docs/cam/gui.md#the-job-toolbar) take no pick: the part
    // turns under the cutter, so a face would name a patch the axis rotates
    // away. Only offered on a machine with a 4th axis.
    {CamVerb::RotaryFinish, "Finish on the rotary", cam::OpType::SurfaceRotary,
     PickSlot::CamSurfaceRegion, false, true, "",
     "Turn the whole part under a ball nose and finish it all the way round, "
     "along the axis. The pattern -- parallel passes, one spiral, or a helix "
     "that follows a twist -- is in the operation's settings. It FINISHES: "
     "rough the blank down first with Rough on the rotary."},
    {CamVerb::RotaryRough, "Rough on the rotary", cam::OpType::SurfaceRotary,
     PickSlot::CamSurfaceRegion, false, true, "",
     "Take the blank down to the part in layers of half the cutter, turning "
     "it all the way round, and leave a skin of a tenth of the cutter for "
     "Finish on the rotary to take off evenly.",
     Verb::Derive::RotaryRough},
    // Rough, finish, clean up
    // (docs/cam/operations.md#rough-a-surface-and-rest-machining). Neither
    // takes a pick: the rough is the whole part, the clean-up follows the last
    // finish.
    {CamVerb::SurfaceRough, "Rough a surface", cam::OpType::SurfaceParallel,
     PickSlot::CamSurfaceRegion, false, true, "",
     "Take the stock down to a curved part in layers of half the cutter, "
     "following it, and leave a skin of a tenth of the cutter for Finish a "
     "surface. It clears a ring one cutter wide round the part, so the "
     "finishing ball can reach its edges, and leaves the rest of the block to "
     "hold it.",
     Verb::Derive::SurfaceRough},
    {CamVerb::Rest, "Clean up what's left", cam::OpType::SurfaceParallel,
     PickSlot::CamSurfaceRegion, false, true, "",
     "Follow the last surface finish with a SMALLER ball, cutting only where "
     "the larger one could not reach: inside corners and narrow valleys. "
     "Choose the smaller tool below.",
     Verb::Derive::Rest},
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
  // An engraving points at a sketch curve drawn on the job tab, which is
  // not a face; every other CAM pick is one.
  const SelectionItem::Kind kind = slot == PickSlot::CamEngraveCurve
                                       ? SelectionItem::Kind::SketchCurve
                                       : SelectionItem::Kind::Face;
  state.slotPick = {kCamToolSlot, static_cast<int>(slot),
                    SelectionKindBit(kind),
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
  fresh.safeZ = ui::DeriveJobSafeZ(state, fresh);
  state.camSafeZEdit = fresh.safeZ;
  MarkJobEdited(state);
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
// The V-bit an engraving is cut with: the verb's tool when that is one, else
// the setup's first. 0, and `why` naming the tools the setup does have, when
// it has none -- an engraving's groove is the cone's
// (docs/cam/operations.md#engrave), and the verb refuses rather than ploughing
// a square slot with an end mill.
int EngraveTool(const AppState& state, const cam::Setup& setup,
                std::string* why = nullptr) {
  const int verbTool = VerbTool(state, setup);
  if (const cam::Tool* t = setup.FindTool(verbTool);
      t != nullptr && t->type == cam::ToolType::VBit)
    return verbTool;
  for (const cam::Tool& t : setup.tools)
    if (t.type == cam::ToolType::VBit)
      return t.number;
  if (why != nullptr) {
    std::string have;
    for (const cam::Tool& t : setup.tools) {
      char one[160];
      std::snprintf(one, sizeof(one), "%sT%d, a %.3g mm %s",
                    have.empty() ? "" : "; ", t.number, t.diameter,
                    cam::ToolTypeName(t.type));
      have += one;
    }
    *why = "Engraving cuts with a V-bit, and this setup has none" +
           (have.empty() ? std::string(": it has no tools at all")
                         : " -- it has " + have) +
           ". Add a V-bit in the CAM panel's Tools.";
  }
  return 0;
}

// Where a picked target faces the spindle on a setup held in the rotary
// (docs/cam/rotary.md#one-operation-at-several-indices): the setup's own
// index when it already faces up there, else the index `SolveIndexA` turns
// it to -- read in the setup's frame at A = 0, as "Turn a face up" reads it.
// Only for a target that is a FLOOR (a clearing's) or a sketch plane (an
// engraving's): a wall or a bore is followed at any index. Nothing when the
// setup is not in the rotary, or nothing turns the target up -- the
// derivation then refuses it in its own words.
std::optional<double> PresentingIndex(AppState& state, const cam::Setup& setup,
                                      cam::OpType type,
                                      const cam::Target& target,
                                      const cam::SketchCurves* curves) {
  if (!setup.machine.hasRotaryAxis || !cam::HeldInRotary(setup) ||
      !target.IsSet() || !cam::OpTypeHas(type, cam::OpField::Indexed))
    return std::nullopt;
  const double own = cam::NormalizeIndexDeg(setup.rotationA);
  cam::Setup unturned = setup;
  unturned.rotationA = 0.0;
  glm::dvec3 normal(0.0);
  if (type == cam::OpType::Engrave) {
    if (curves == nullptr || !curves->why.empty())
      return std::nullopt;
    if (cam::EngraveFrameOf(setup, *curves).ok)
      return own;
    normal = glm::normalize(
        glm::dmat3(cam::BodyToSetup(unturned, target.jobPartId)) *
        curves->planeNormal);
  } else if (cam::OpTypeHas(type, cam::OpField::NeedsFlatFloor)) {
    const std::vector<Body>& bodies = ui::JobBodies(state);
    glm::dvec3 n(0.0);
    if (cam::FaceNormalOf(cam::BodiesInSetupFrame(setup, bodies), target.ref, n,
                          target.jobPartId) &&
        n.z > 0.99f)
      return own;
    if (!cam::FaceNormalOf(cam::BodiesInSetupFrame(unturned, bodies),
                           target.ref, n, target.jobPartId))
      return std::nullopt; // curved: the derivation says why
    normal = glm::dvec3(n);
  } else {
    return std::nullopt;
  }
  double degrees = 0.0;
  if (!cam::SolveIndexA(normal, degrees))
    return std::nullopt;
  return cam::NormalizeIndexDeg(degrees);
}

// A derived top over several targets or indices: the top of what the
// earlier setups left over ALL of them, as add_cam_op takes it -- the
// derivation read only the first.
void RetopForTargets(AppState& state, const cam::Setup& setup,
                     cam::Operation& op) {
  if ((op.targets.size() < 2 && !cam::CutAtIndices(op)) ||
      !cam::TopFromStock(op.type))
    return;
  glm::dvec3 lo(0.0), hi(0.0);
  if (!cam::StockBounds(setup, ui::JobBodies(state), lo, hi))
    return;
  const double top = cam::StockTopOver(
      setup, cam::EarlierSetups(state.job->GetSetups(), setup.id),
      ui::JobBodies(state), op, hi.z);
  if (top > op.bottomZ)
    op.topZ = top;
}

uint64_t CreateVerbOperation(AppState& state, cam::OpType type,
                             const ElementRef& target, uint64_t hint = 0,
                             Verb::Derive derive = Verb::Derive::Standard,
                             std::optional<double> atIndex = std::nullopt) {
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
  // An engraving's pick is a sketch's curves, resolved from the part
  // documents, and it is cut with a V-bit or not at all.
  std::vector<cam::SketchCurves> curves;
  int tool = VerbTool(state, *setup);
  if (type == cam::OpType::Engrave) {
    tool = EngraveTool(state, *setup, &why);
    if (tool == 0) {
      state.camRefusal = why;
      return 0;
    }
    curves = ui::JobSketchCurves(state, *setup, {picked});
  }
  // Derived where the target faces the spindle: at an index of its own, the
  // setup turned to it (docs/cam/rotary.md#one-operation-at-several-indices).
  cam::Setup frame = *setup;
  const bool ownIndex =
      !atIndex ||
      std::abs(*atIndex - cam::NormalizeIndexDeg(setup->rotationA)) < 1e-6;
  if (!ownIndex)
    frame.rotationA = *atIndex;
  const cam::DerivedOp derived = DeriveForVerb(
      derive, frame, ui::JobBodies(state), type, picked, tool,
      cam::EarlierSetups(state.job->GetSetups(), setup->id), curves);
  if (!derived.ok) {
    state.camRefusal = derived.why;
    return 0;
  }
  cam::Operation op = derived.op;
  op.id = state.job->AllocateId();
  if (!ownIndex)
    op.indexA = {*atIndex};
  // "Then finish with" (R6): the window's second tool row.
  if (state.camVerbFinishTool != 0 && op.type != cam::OpType::Engrave &&
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
  MarkJobEdited(state);
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
  // An engraving's curves, and an op cut at its own indices derived where
  // its first target faces the spindle -- as it was made.
  const std::vector<cam::SketchCurves> curves =
      verb.type == cam::OpType::Engrave
          ? ui::JobSketchCurves(state, setup, op->targets)
          : std::vector<cam::SketchCurves>{};
  const cam::Setup frame = cam::IndexFrameFor(
      setup, ui::JobBodies(state), verb.type, target, op->indexA, curves);
  cam::DerivedOp derived = DeriveForVerb(
      verb.derive, frame, ui::JobBodies(state), verb.type, target, tool,
      cam::EarlierSetups(state.job->GetSetups(), setup.id), curves);
  if (!derived.ok) {
    state.camRefusal = derived.why;
    return;
  }
  const uint64_t id = op->id;
  const cam::Operation before = *op;
  *op = derived.op;
  op->id = id;
  // Every target the op had, and its indices: a tool change re-derives the
  // numbers, not what is cut. (A rotary pass limited to a face keeps the
  // face this way too; the rough's derivation takes no target of its own.)
  if (!before.targets.empty())
    op->targets = before.targets;
  op->indexA = before.indexA;
  RetopForTargets(state, setup, *op);
  state.camRefusal.clear();
  state.camGhostOp = id;
  MarkJobEdited(state);
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

// A further pick of an armed verb, taken into the operation it already made
// rather than making another: a second label into the same engraving
// (docs/cam/operations.md#engrave), and on a setup held in the rotary a floor
// at another index -- that index and that floor added to the same op, so the
// rook's four crenels are one pocket at 0/90/180/270
// (docs/cam/rotary.md#one-operation-at-several-indices). A floor is taken in
// only when it is the same cut: its depth, derived where it faces the
// spindle, the op's own, since every index is cut to the op's one depth.
enum class Merge { NotHere, Merged, Refused };
Merge MergeIntoVerbOp(AppState& state, cam::Setup& setup, cam::OpType type,
                      const cam::Target& picked,
                      const std::vector<cam::SketchCurves>& curves,
                      std::optional<double> atIndex) {
  cam::Operation* op = nullptr;
  // The verb's op, by id: what it derived may be another type of the same
  // cut (a pocket is derived as an adaptive clearing), so only the kind of
  // target is checked.
  for (cam::Operation& o : setup.operations)
    if (o.id == state.camVerbOp &&
        (type == cam::OpType::Engrave
             ? o.type == cam::OpType::Engrave
             : cam::OpTypeHas(o.type, cam::OpField::NeedsFlatFloor)))
      op = &o;
  if (op == nullptr)
    return Merge::NotHere;
  const bool engrave = type == cam::OpType::Engrave;
  if (!engrave && !atIndex)
    return Merge::NotHere; // a floor on the bed: an op of its own, as ever
  if (!engrave) {
    cam::Setup frame = setup;
    frame.rotationA = *atIndex;
    const cam::DerivedOp derived = cam::DeriveOperation(
        frame, ui::JobBodies(state), type, picked, op->toolNumber,
        cam::EarlierSetups(state.job->GetSetups(), setup.id));
    if (!derived.ok) {
      state.camRefusal = derived.why;
      return Merge::Refused;
    }
    if (std::abs(derived.op.bottomZ - op->bottomZ) > 1e-3)
      return Merge::NotHere;
  } else {
    const cam::SketchCurves* c = cam::FindSketchCurves(curves, picked);
    if (c == nullptr || !c->why.empty()) {
      state.camRefusal =
          c != nullptr ? c->why : "that curve cannot be read from its part";
      return Merge::Refused;
    }
    // A label no index of the op would turn up is refused here, not left for
    // the generator to skip.
    cam::Setup frame = setup;
    if (atIndex)
      frame.rotationA = *atIndex;
    if (const cam::EngraveFrame f = cam::EngraveFrameOf(frame, *c); !f.ok) {
      state.camRefusal = f.why;
      return Merge::Refused;
    }
  }
  JobUndoGuard undoGuard(state);
  const cam::Operation before = *op;
  const bool changed =
      atIndex ? cam::AddIndexedTarget(*op, picked, *atIndex, setup.rotationA)
              : cam::AddIndexedTarget(*op, picked, setup.rotationA,
                                      setup.rotationA);
  if (!changed) {
    state.camRefusal = "that is already in this operation";
    return Merge::Refused;
  }
  RetopForTargets(state, setup, *op);
  MarkJobEdited(state);
  RefreshCamToolpath(state, op->id);
  if (const std::string why = ToolpathError(state, op->id); !why.empty()) {
    *op = before;
    RefreshCamToolpath(state, op->id);
    state.camRefusal = why;
    RunCamVerification(state);
    return Merge::Refused;
  }
  state.camGhostOp = op->id;
  RunCamVerification(state);
  return Merge::Merged;
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
  const int moved =
      cam::RederiveDepths(setup, ui::JobBodies(state),
                          cam::EarlierSetups(state.job->GetSetups(), setup.id));
  setup.safeZ = ui::DeriveJobSafeZ(state, setup);
  state.camSafeZEdit = setup.safeZ;

  std::string note;
  if (hadZero && !setup.topFace.IsSet())
    note = "the face Z zero came from is not level any more — pick a top face";
  if (moved > 0)
    note += (note.empty() ? "" : "; ") + std::to_string(moved) +
            (moved == 1 ? " operation's depths" : " operations' depths") +
            " re-derived in the new clamping";
  state.camNote = note;

  state.slotPick = {};
  MarkJobEdited(state);
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

  // A rotary verb's "Limit to a face": the pick bounds the operation the
  // verb already made ALONG the axis
  // (docs/cam/rotary.md#continuous-surface_rotary), rather than making another
  // one.
  if (slot == PickSlot::CamSurfaceRegion &&
      (state.camVerb == CamVerb::RotaryFinish ||
       state.camVerb == CamVerb::RotaryRough) &&
      state.camVerbOp != 0) {
    cam::Setup* setup = ActiveCamSetup(state);
    cam::Operation* op = nullptr;
    if (setup != nullptr)
      for (cam::Operation& o : setup->operations)
        if (o.id == state.camVerbOp)
          op = &o;
    const Verb* verb = FindVerb(state.camVerb);
    if (op == nullptr || verb == nullptr) {
      state.slotPick.miss = "the operation to limit is gone";
      return;
    }
    op->targets = {ui::TargetFor(state, item.element, item.jobPartId)};
    RederiveVerbOp(state, *setup, *verb, op->toolNumber);
    state.slotPick = {};
    state.camNote = "limited to that face's length along the axis";
    return;
  }

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
    glm::dvec3 normal(0.0);
    const cam::Target picked =
        ui::TargetFor(state, item.element, item.jobPartId);
    if (cam::FaceNormalOf(ui::JobBodies(state), item.element, normal,
                          picked.jobPartId)) {
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
    // (docs/cam/gui.md#verb-windows). The default 1 mm is the rough
    // board every facing pass exists for; a board already flat is one number.
    setup->topFace = ui::TargetFor(state, item.element, item.jobPartId);
    // Through the ONE derivation, so a Z zero picked here and a work origin
    // set from the panel cannot disagree about where the file's zero is.
    (void)cam::RecomputeOrigin(*setup, ui::JobBodies(state));
    state.camNote.clear();
    if (reclamped) {
      const int moved = cam::RederiveDepths(
          *setup, ui::JobBodies(state),
          cam::EarlierSetups(state.job->GetSetups(), setup->id));
      state.camNote = "laid the part flat on that face";
      if (moved > 0)
        state.camNote +=
            ", and re-derived " + std::to_string(moved) +
            (moved == 1 ? " operation's depths" : " operations' depths");
    }
    // The stock just moved, so the rapid height that cleared the old one may
    // now be inside the new one.
    setup->safeZ = ui::DeriveJobSafeZ(state, *setup);
    state.camSafeZEdit = setup->safeZ;
    // Operations that started above the stock come down to it — never up: an
    // operation asked for a shallower top than the stock is a choice.
    glm::dvec3 lo(0.0), hi(0.0);
    if (cam::StockBounds(*setup, ui::JobBodies(state), lo, hi))
      for (cam::Operation& op : setup->operations)
        if (op.topZ > hi.z)
          op.topZ = hi.z;
    state.slotPick = {};
    MarkJobEdited(state);
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
    const uint64_t part =
        ui::TargetFor(state, item.element, item.jobPartId).jobPartId;
    glm::dvec3 normal(0.0);
    if (!cam::FaceNormalOf(ui::JobBodies(state), item.element, normal, part)) {
      state.slotPick.miss = "pick a flat face — that one is curved";
      return;
    }
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
    // The normal as the rotary SEES it: the setup's frame with no index --
    // the fixture frame and the part's placement applied, which is what the
    // index then turns. Read in model space it ignored both, and a part laid
    // on its side was indexed for the way it was modelled.
    cam::Setup unturned = *setup;
    unturned.rotationA = 0.0;
    glm::dvec3 normal(0.0);
    if (!cam::FaceNormalOf(
            cam::BodiesInSetupFrame(unturned, ui::JobBodies(state)),
            item.element, normal,
            ui::TargetFor(state, item.element, item.jobPartId).jobPartId)) {
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

  // Which op a picked slot makes. The last arm used to be a bare fallback to
  // Profile, so a slot added later and not listed here would have quietly
  // produced a PROFILE -- the same silent substitution the op-type table
  // exists to abolish, one layer up. Named arms, and the default is the one
  // slot that really does mean a profile.
  cam::OpType type = cam::OpType::Profile;
  switch (slot) {
  case PickSlot::CamPocketFloor:
    type = cam::OpType::Pocket;
    break;
  case PickSlot::CamEdgeWall:
    type = cam::OpType::EdgeRound;
    break;
  case PickSlot::CamDrillHole:
    type = cam::OpType::Drill;
    break;
  case PickSlot::CamProfileOutline:
    type = cam::OpType::Profile;
    break;
  case PickSlot::CamSurfaceRegion:
    // Which of the two finishing verbs armed it. They share a slot because
    // they take the same pick and differ only in the pattern.
    type = state.camVerb == ui::AppState::CamVerb::Walls
               ? cam::OpType::SurfaceWaterline
               : cam::OpType::SurfaceParallel;
    break;
  case PickSlot::CamEngraveCurve:
    type = cam::OpType::Engrave;
    break;
  default:
    LOG_WARN("CAM: pick slot {} has no operation type; treating it as a "
             "profile",
             static_cast<int>(slot));
    break;
  }
  // The slot stays armed either way. A refusal keeps it because the user's
  // next click is almost always the face they meant; a SUCCESS keeps it
  // because one verb is almost never one face — a part has six holes to
  // clear, not one, and re-pressing "Clear a pocket" between each was a click
  // per hole spent saying something already said. Escape disarms, and so does
  // pressing the verb again.
  const std::string prompt = state.slotPick.prompt;
  // Where the pick faces the spindle, on a setup in the rotary, and whether
  // it belongs in the op this verb already made.
  std::optional<double> atIndex;
  if (cam::Setup* setup = ActiveCamSetup(state);
      setup != nullptr &&
      (type == cam::OpType::Engrave ||
       cam::OpTypeHas(type, cam::OpField::NeedsFlatFloor))) {
    std::string why;
    const cam::Target picked =
        ui::TargetFor(state, item.element, item.jobPartId, &why);
    const std::vector<cam::SketchCurves> curves =
        type == cam::OpType::Engrave && picked.jobPartId != 0
            ? ui::JobSketchCurves(state, *setup, {picked})
            : std::vector<cam::SketchCurves>{};
    if (picked.jobPartId != 0) {
      atIndex = PresentingIndex(state, *setup, type, picked,
                                cam::FindSketchCurves(curves, picked));
      state.camRefusal.clear();
      switch (MergeIntoVerbOp(state, *setup, type, picked, curves, atIndex)) {
      case Merge::Merged:
        state.slotPick.miss.clear();
        state.camNote = type == cam::OpType::Engrave
                            ? "added to the same engraving -- keep clicking, "
                              "or press Escape when done"
                            : "added to the same operation, at the index "
                              "that turns it up -- keep clicking, or press "
                              "Escape when done";
        state.slotPick.prompt = prompt;
        return;
      case Merge::Refused:
        state.slotPick.miss = state.camRefusal;
        state.camRefusal.clear();
        state.slotPick.prompt = prompt;
        return;
      case Merge::NotHere:
        break;
      }
    }
  }
  if (const uint64_t made =
          CreateVerbOperation(state, type, item.element, item.jobPartId,
                              Verb::Derive::Standard, atIndex)) {
    state.camVerbOp = made; // the one a tool change in the window re-derives
    state.slotPick.miss.clear();
    state.camNote = type == cam::OpType::Engrave
                        ? "added — keep clicking labels to add them to it, "
                          "or press Escape when done"
                        : "added — keep clicking faces, or press Escape when "
                          "done";
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
  // A verb takes the window from an op or tool being edited: the editors are
  // drawn first, so leaving either set would keep the window on it.
  state.camEditOp = 0;
  state.camEditTool = 0;
  state.camEditFixture = 0;
  // An engraving is a V-bit's cut. Without one the verb says so where it
  // was pressed, naming the tools the setup does have, and arms nothing: a
  // pick that could only be refused is a click wasted
  // (docs/ui/tool-sessions.md#defaults-and-refusals).
  if (verb == CamVerb::Engrave)
    if (const cam::Setup* setup = ActiveCamSetup(state)) {
      std::string why;
      if (EngraveTool(state, *setup, &why) == 0) {
        if (state.slotPick.featureId == kCamToolSlot)
          state.slotPick = {};
        state.camRefusal = why;
        return;
      }
    }
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
  state.camVerbOp = CreateVerbOperation(state, v->type, {}, 0, v->derive);
}

namespace {

// The cutter row: which tool the verb derives for, the one in use marked.
// Shown for every job with a setup, one tool or six -- a verb that derives
// every number from the cutter should say which cutter, and "the last one
// used" is a default worth being able to see.
void VerbToolRow(AppState& state, cam::Setup& setup, const Verb& verb) {
  if (setup.tools.empty())
    return;
  // An engraving is offered the V-bits only (EngraveTool).
  const bool engrave = verb.type == cam::OpType::Engrave;
  const int active =
      engrave ? EngraveTool(state, setup) : VerbTool(state, setup);
  ImGui::TextDisabled("Tool");
  for (const cam::Tool& tool : setup.tools) {
    if (engrave && tool.type != cam::ToolType::VBit)
      continue;
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
  // finish with. "none" is a button too, so the choice can be undone. An
  // engraving is one pass of the cone: nothing to finish.
  if (setup.tools.size() < 2 || engrave)
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
  // same shape the docked panel uses, and the same row (CamOperationRow):
  // glyph, name, tool, a red mark and the reason when it cut nothing.
  bool deleteSelected = false; // the row menu's Delete (the selection)
  uint64_t moveOp = 0;
  size_t moveTo = 0;
  std::vector<uint64_t> order;
  for (const cam::Operation& op : setup.operations)
    order.push_back(op.id);
  for (size_t i = 0; i < setup.operations.size(); i++) {
    cam::Operation& op = setup.operations[i];
    ImGui::PushID(static_cast<int>(op.id));
    const CamOpRowResult row = CamOperationRow(state, setup, op, order);
    if (row.deleteSelection)
      deleteSelected = true;
    if (row.toggleEnabled) {
      op.enabled = !op.enabled;
      ApplyCamOpEdit(state, op.id);
    }
    if (row.edit)
      EditCamOperation(state, op.id);
    if (row.droppedHere != 0) {
      moveOp = row.droppedHere;
      moveTo = i;
    }
    ImGui::PopID();
  }
  if (moveOp != 0 && cam::MoveOperation(setup, moveOp, moveTo)) {
    MarkJobEdited(state);
    state.camVerdict.clear();
    state.camLeftover.clear();
  }
  if (deleteSelected)
    DeleteSelection(state);
}

// One operation's settings, when it is being edited (a double-click on its
// row). False when there is nothing to edit, so the caller draws the plain
// window instead.
bool DrawOperationEditor(AppState& state) {
  cam::Setup* setup = ActiveCamSetup(state);
  cam::Operation* op = nullptr;
  if (setup != nullptr)
    for (cam::Operation& o : setup->operations)
      if (o.id == state.camEditOp)
        op = &o;
  if (op == nullptr) {
    state.camEditOp = 0; // deleted, or its setup is no longer the active one
    return false;
  }
  // Every committed field is one undo step, a drag one from where it
  // started (docs/cam/gui.md#undo) -- as the setup editor's are.
  JobUndoGuard undoGuard(state, /*coalesce=*/true);
  BeginToolWindowFrame(state, "Edit operation");
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);

  char name[128];
  std::snprintf(name, sizeof(name), "%s", op->name.c_str());
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
  if (ImGui::InputText("Name###op_name", name, sizeof(name))) {
    op->name = name; // a name changes no toolpath
    MarkJobEdited(state);
  }
  ImGui::TextDisabled("%s", CamOpSummary(*setup, *op).c_str());
  ImGui::Separator();

  // An edit that would drive a shank into a wall is taken back, and says
  // why -- as add_cam_op/set_cam_op refuse it (docs/cam/setup.md#reach).
  const cam::Operation before = *op;
  if (DrawCamOpSettings(state, *setup, *op)) {
    bool refused = false;
    for (const cam::Reach& r : cam::ReachChecks(*setup, *op))
      if (r.level == cam::Reach::Level::Collides) {
        *op = before;
        state.camRefusal = r.text;
        refused = true;
        break;
      }
    if (!refused) {
      state.camRefusal.clear();
      ApplyCamOpEdit(state, op->id);
    }
  }
  // What it can cut, but only just or only by rubbing: said here, where the
  // depth is typed, not first in the verdict after a post.
  for (const cam::Reach& r : cam::ReachChecks(*setup, *op)) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
    ImGui::TextWrapped("%s", r.text.c_str());
    ImGui::PopStyleColor();
  }

  for (const cam::Toolpath& tp : state.camToolpaths)
    if (tp.operationId == op->id) {
      const bool failed = !tp.error.empty();
      ImGui::PushStyleColor(
          ImGuiCol_Text, failed
                             ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                             : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::TextWrapped("%s", CamOpPathLine(*op, tp).c_str());
      ImGui::PopStyleColor();
    }
  if (!state.camRefusal.empty())
    RefusalText(state.camRefusal);

  ImGui::PopTextWrapPos();
  ImGui::Separator();
  // Close: the edits are already applied (docs/ui/panels.md).
  if (ImGui::Button("Close###cam_edit_done")) {
    state.camEditOp = 0;
    state.camToolOpen = false;
    state.camRefusal.clear();
  }
  ImGui::End();
  return true;
}

// One setup tool's fields, when it is being edited (a double-click on its
// row). False when there is nothing to edit.
bool DrawToolEditor(AppState& state) {
  cam::Setup* setup = ActiveCamSetup(state);
  cam::Tool* tool = nullptr;
  if (setup != nullptr)
    for (cam::Tool& t : setup->tools)
      if (t.number == state.camEditTool)
        tool = &t;
  if (tool == nullptr) {
    state.camEditTool = 0; // deleted, or its setup is no longer active
    return false;
  }
  JobUndoGuard undoGuard(state, /*coalesce=*/true); // one step per edit
  BeginToolWindowFrame(state, "Edit tool T" + std::to_string(tool->number));
  ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
  // The tool as the ops that use it need it: an edit that leaves one of
  // them with a shank driven into a wall is taken back, naming the op.
  const cam::Tool before = *tool;
  if (DrawCamToolFields(state, *tool)) {
    bool refused = false;
    for (const cam::Operation& op : setup->operations) {
      if (op.toolNumber != tool->number && op.finishTool != tool->number)
        continue;
      for (const cam::Reach& r : cam::ReachChecks(*setup, op))
        if (r.level == cam::Reach::Level::Collides) {
          *tool = before;
          state.camRefusal = r.text;
          refused = true;
          break;
        }
      if (refused)
        break;
    }
    if (!refused) {
      state.camRefusal.clear();
      ApplyCamToolEdit(state);
    }
  }
  const std::vector<std::string> users = CamOpsUsingTool(*setup, tool->number);
  ImGui::Spacing();
  if (users.empty()) {
    ImGui::TextDisabled("No operation uses it.");
  } else {
    std::string list;
    for (size_t i = 0; i < users.size(); i++)
      list += (i == 0 ? "" : ", ") + users[i];
    ImGui::TextDisabled("Used by %s.", list.c_str());
  }
  if (!state.camRefusal.empty())
    RefusalText(state.camRefusal);
  ImGui::PopTextWrapPos();
  ImGui::Separator();
  ImGui::BeginDisabled(!users.empty());
  if (ImGui::Button(ICON_FA_TRASH_CAN " Delete###cam_tool_editor_delete")) {
    std::string why;
    if (!RemoveCamTool(state, tool->number, &why))
      state.camRefusal = why;
    else
      state.camToolOpen = false;
  }
  ImGui::EndDisabled();
  if (!users.empty())
    ImGui::SetItemTooltip("Used by an operation. Give it another tool first.");
  ImGui::SameLine();
  if (ImGui::Button("Close###cam_tool_editor_close")) {
    state.camEditTool = 0;
    state.camToolOpen = false;
    state.camRefusal.clear();
  }
  ImGui::End();
  return true;
}

// One fixture's numbers (docs/cam/setup.md#workholding-fixtures): where it
// sits, its spin, a box's size, and the derived placements as buttons. Applies
// as you go, like the tool editor, so it says Close.
bool DrawFixtureEditor(AppState& state) {
  cam::Setup* setup = ActiveCamSetup(state);
  cam::Fixture* f =
      setup != nullptr ? setup->FindFixture(state.camEditFixture) : nullptr;
  if (f == nullptr) {
    state.camEditFixture = 0; // removed, or its setup is no longer active
    return false;
  }
  // One undo step per edit, a drag one: the guard records, so no button
  // snapshots on its own (a snapshot under it would be a second step).
  JobUndoGuard undoGuard(state, /*coalesce=*/true);
  BeginToolWindowFrame(state, "Fixture: " + f->name);
  bool changed = false;
  // How it moves: the arrows in the viewport (docs/cam/gui.md#gizmos). Said
  // here because this window is where a person looks for it,
  // and while a drag snaps, WHAT it snapped to replaces the sentence.
  const bool dragging =
      state.gizmoDrag.active && state.gizmoDrag.fixtureId == f->id;
  ImGui::PushTextWrapPos(0.0f);
  if (dragging && !state.gizmoDrag.snapLabel.empty())
    ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.55f, 1.0f), "Snapped: %s",
                       state.gizmoDrag.snapLabel.c_str());
  else
    ImGui::TextDisabled("Drag the arrows in the viewport: its faces snap to "
                        "the stock's (hold Ctrl to move freely). Put it down "
                        "on the stock's top and it rests there, following "
                        "the stock.");
  ImGui::PopTextWrapPos();
  if (f->seat) {
    // RESTING ON the stock (docs/cam/setup.md#seats): every number is
    // measured from the stock, and the pose follows it. The bed position and
    // the side buttons are for a fixture that stands on its own.
    ImGui::SeparatorText("Resting on the stock");
    if (const auto it = state.camSeatRefusals.find(f->id);
        it != state.camSeatRefusals.end()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
      ImGui::TextWrapped("It cannot rest there any more: %s. It is drawn "
                         "where it last sat.",
                         it->second.c_str());
      ImGui::PopStyleColor();
    }
    static const std::pair<const char*, cam::StockEdge> kEdges[] = {
        {"-X###cam_seat_mx", cam::StockEdge::MinusX},
        {"+X###cam_seat_px", cam::StockEdge::PlusX},
        {"-Y###cam_seat_my", cam::StockEdge::MinusY},
        {"+Y###cam_seat_py", cam::StockEdge::PlusY}};
    ImGui::TextUnformatted("Over the edge");
    for (const auto& [label, edge] : kEdges) {
      ImGui::SameLine();
      const bool on = f->seat->edge == edge;
      if (on)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::SmallButton(label) && !on) {
        f->seat->edge = edge;
        changed = true;
      }
      if (on)
        ImGui::PopStyleColor();
    }
    const bool alongX = f->seat->edge == cam::StockEdge::MinusY ||
                        f->seat->edge == cam::StockEdge::PlusY;
    double along = f->seat->alongMm;
    if (ui::ExprDragDouble(state, "Along###cam_seat_along", &along, 0.5f, 0.0,
                           0.0, "%.2f mm")) {
      f->seat->alongMm = along;
      changed = true;
    }
    ImGui::SetItemTooltip(alongX ? "From the stock's -X end to the middle of "
                                   "the face it rests on"
                                 : "From the stock's -Y end to the middle of "
                                   "the face it rests on");
    double over = f->seat->overMm;
    if (ui::ExprDragDouble(state, "Over the stock###cam_seat_over", &over, 0.5f,
                           0.0, 0.0, "%.2f mm")) {
      f->seat->overMm = over;
      changed = true;
    }
    ImGui::SetItemTooltip("How far it reaches in past the edge -- where its "
                          "nose presses");
    double spin = f->seat->spinDeg;
    if (ui::ExprDragDouble(state, "Spin###cam_seat_spin", &spin, 1.0f, -180.0,
                           180.0, "%.1f°")) {
      f->seat->spinDeg = spin;
      changed = true;
    }
    ImGui::SetItemTooltip("Turned from pointing straight in");
    if (ImGui::Button("Unseat###cam_seat_unseat")) {
      // Keeps the pose it has: the bed position it stood at a moment ago.
      f->seat.reset();
      changed = true;
    }
    ImGui::SetItemTooltip("Stand it on its own at this bed position; it will "
                          "no longer follow the stock");
  } else {
    if (ui::ExprDragFloat3(state, "Position###cam_fixture_xyz", &f->pos.x,
                           0.5f))
      changed = true;
    ImGui::SetItemTooltip("Where its origin sits on the bed (setup space, mm)");
    double rot = f->rotZdeg;
    if (ui::ExprDragDouble(state, "Spin###cam_fixture_rot", &rot, 1.0f, -360.0,
                           360.0, "%.1f°")) {
      f->rotZdeg = rot;
      changed = true;
    }
    if (f->IsBox()) {
      double size[3] = {f->box.x, f->box.y, f->box.z};
      if (ui::ExprDragFloat3(state, "Size###cam_fixture_size", size, 0.5f) &&
          size[0] > 0.0 && size[1] > 0.0 && size[2] > 0.0) {
        f->box = {size[0], size[1], size[2]};
        changed = true;
      }
      ImGui::SetItemTooltip("The keep-out box, X by Y by Z, mm");
    }
    ImGui::SeparatorText("Put it");
    const std::vector<Body> model = ui::FixtureModelBodies(state, *f);
    static const std::pair<const char*, cam::FixtureSide> kPlaces[] = {
        {"-X###cam_fixture_mx", cam::FixtureSide::MinusX},
        {"+X###cam_fixture_px", cam::FixtureSide::PlusX},
        {"-Y###cam_fixture_my", cam::FixtureSide::MinusY},
        {"+Y###cam_fixture_py", cam::FixtureSide::PlusY},
        {"Centre###cam_fixture_centre", cam::FixtureSide::Centre},
        {"Top###cam_fixture_top", cam::FixtureSide::Top}};
    for (size_t k = 0; k < std::size(kPlaces); k++) {
      if (k > 0)
        ImGui::SameLine();
      if (ImGui::Button(kPlaces[k].first) &&
          cam::PlaceFixture(*setup, ui::JobBodies(state), model, f->rotZdeg,
                            kPlaces[k].second, f->pos))
        changed = true;
    }
    ImGui::SetItemTooltip("Against that side of the stock, centred on it (a "
                          "vise), or on its top straddling the -X edge (a toe "
                          "clamp)");
  } // unseated
  if (ImGui::Checkbox("Check the tool against it###cam_fixture_on",
                      &f->enabled))
    changed = true;
  if (changed)
    CamFixturesChanged(state, *setup);
  ImGui::Separator();
  if (ImGui::Button(ICON_FA_TRASH_CAN " Remove###cam_fixture_editor_remove")) {
    const uint64_t id = f->id;
    std::erase_if(setup->fixtures,
                  [id](const cam::Fixture& x) { return x.id == id; });
    state.camEditFixture = 0;
    state.camToolOpen = false;
    CamFixturesChanged(state, *setup);
    ImGui::End();
    return true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Close###cam_fixture_editor_close")) {
    state.camEditFixture = 0;
    state.camToolOpen = false;
  }
  ImGui::End();
  return true;
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
  // A setup's settings and a part's placement (CamTree.cpp) hold the window
  // only while no verb and no other editor asked for it.
  if (state.camVerb != CamVerb::None || state.camEditOp != 0 ||
      state.camEditTool != 0 || state.camEditFixture != 0) {
    state.camEditSetup = 0;
    state.camEditPart = 0;
  } else if (state.camEditSetup != 0 && DrawCamSetupEditor(state)) {
    return;
  } else if (state.camEditPart != 0 && DrawCamPartEditor(state)) {
    return;
  }
  if (state.camEditOp != 0 && DrawOperationEditor(state))
    return;
  if (state.camEditTool != 0 && DrawToolEditor(state))
    return;
  if (state.camEditFixture != 0 && DrawFixtureEditor(state))
    return;
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
    case CamVerb::Engrave:
      if (armedHere)
        ImGui::TextDisabled("click labels or curves… Escape, or the button "
                            "again, when done");
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
    // The rotary verbs cut the whole length unless a face says otherwise.
    const cam::Operation* made = nullptr;
    if (setup != nullptr && (state.camVerb == CamVerb::RotaryFinish ||
                             state.camVerb == CamVerb::RotaryRough))
      for (const cam::Operation& o : setup->operations)
        if (o.id == state.camVerbOp)
          made = &o;
    if (made != nullptr) {
      ImGui::TextDisabled(made->targets.empty()
                              ? "The whole length of the part."
                              : "One face's length along the axis.");
      if (ImGui::Button("Limit to a face###cam_verb_limit"))
        ArmCamPick(state, PickSlot::CamSurfaceRegion,
                   "Click the face whose length to cut");
      if (!made->targets.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Whole length###cam_verb_whole")) {
          for (cam::Operation& o : setup->operations)
            if (o.id == state.camVerbOp)
              o.targets.clear();
          RederiveVerbOp(state, *setup, *verb, made->toolNumber);
        }
      }
    }
  }

  // Why the last pick did not take. This window never drew it, so a refused
  // click looked exactly like a click that did nothing — and one of them
  // ("add a setup first") was reachable on any fresh part
  // (docs/cam/gui.md#verb-windows). The docked panel printed it all along.
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

  // What grows -- the operations and the verdict -- scrolls in a region of
  // bounded height (docs/cam/gui.md#verb-windows): the window grew with them,
  // and Engrave's, an operation per label and a line per finding, grew down
  // over the very curves it was asking to be clicked. At most two fifths of
  // the viewport, from the viewport: the rest is where the part is. The
  // prompt above and Close below stay put.
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f),
      ImVec2(FLT_MAX, std::max(ImGui::GetFrameHeightWithSpacing() * 6.0f,
                               state.viewportSize.y * 0.4f)));
  ImGui::BeginChild("cam_tool_list", ImVec2(0.0f, 0.0f),
                    ImGuiChildFlags_AutoResizeY);
  // The job so far: for the machining verbs and the plain window. The
  // orientation picks are one question each, and the list under them was
  // noise.
  if (setup != nullptr && (verb == nullptr || verb->cutter))
    OperationList(state, *setup);

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
  ImGui::EndChild();

  ImGui::Separator();
  ImGui::Checkbox("Show path###cam_tool_show", &state.camShowToolpath);
  ImGui::SameLine();
  ImGui::Checkbox("Stock###cam_tool_stock", &state.camShowStock);

  ImGui::Separator();
  if (ImGui::Button("Close###cam_tool_done")) {
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

void EditCamOperation(AppState& state, uint64_t opId) {
  if (!state.job)
    return;
  // The editor takes the tool window over from whatever verb held it: one
  // tool window at a time, the rule every tool here follows.
  if (state.slotPick.featureId == kCamToolSlot)
    state.slotPick = {};
  state.camVerb = CamVerb::None;
  state.camVerbOp = 0;
  state.camRefusal.clear();
  state.camEditTool = 0;
  state.camEditFixture = 0;
  state.camEditOp = opId;
  state.camToolOpen = true;
}

void EditCamTool(AppState& state, int number) {
  if (!state.job)
    return;
  if (state.slotPick.featureId == kCamToolSlot)
    state.slotPick = {};
  state.camVerb = CamVerb::None;
  state.camVerbOp = 0;
  state.camRefusal.clear();
  state.camEditOp = 0;
  state.camEditFixture = 0;
  state.camEditTool = number;
  state.camToolOpen = true;
}

void EditCamFixture(AppState& state, uint64_t fixtureId) {
  if (!state.job)
    return;
  if (state.slotPick.featureId == kCamToolSlot)
    state.slotPick = {};
  state.camVerb = CamVerb::None;
  state.camVerbOp = 0;
  state.camRefusal.clear();
  state.camEditOp = 0;
  state.camEditTool = 0;
  state.camEditFixture = fixtureId;
  state.camToolOpen = true;
}

bool CamStockBox(AppState& state, glm::dvec3& lo, glm::dvec3& hi) {
  const cam::Setup* setup = ActiveJobSetup(state);
  if (setup == nullptr)
    return false;
  const StockView& view = CachedStockView(state, *setup);
  if (!view.stock)
    return false;
  lo = view.lo;
  hi = view.hi;
  return true;
}

void CamFixturesChanged(AppState& state, cam::Setup& setup) {
  if (!state.job)
    return;
  state.job->BumpVersion();
  // A seat is re-solved first: safe Z below reads where the fixtures ARE.
  RefreshFixtureSeats(state, setup);
  // A clamp taller than the stock puts every rapid through it unless safe Z
  // clears it too.
  setup.safeZ = ui::DeriveJobSafeZ(state, setup);
  state.camSafeZEdit = setup.safeZ;
  MarkJobEdited(state);
  RefreshCamToolpaths(state);
  // A verdict about the fixtures as they WERE is worse than none.
  state.camVerdict.clear();
  state.camLeftover.clear();
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
  const StockView& view = CachedStockView(state, *setup);
  if (state.camShowStock && view.stock) {
    const glm::dvec3 lo = view.lo, hi = view.hi;
    const std::vector<glm::dvec2> rect = {
        {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
    LoopLines(rect, lo.z, 1.0f, kStockLine, prims);
    LoopLines(rect, hi.z, 1.2f, kStockLine, prims);
    for (const glm::dvec2& c : rect)
      AppendPrimLine(glm::vec3(c.x, c.y, lo.z), glm::vec3(c.x, c.y, hi.z), 1.0f,
                     kStockLine, prims);
    LoopWalls(rect, lo.z, hi.z, kStockFill, quads);

    // ...and the MACHINE's work area around it, when the machine states one:
    // a box the size of the travel (cam::MachineBounds: centred on a stock
    // that fits, at the near corner of a longer one), its floor at the
    // stock's bottom, which is how a sheet is loaded. Where
    // the operator touches off inside the travel is not the job's to know,
    // so this is the machine's SIZE against the job's -- the check
    // cam::MachineWarning makes in words. Never resized to fit.
    if (view.bed) {
      const glm::dvec3 bedLo = view.bedLo, bedHi = view.bedHi;
      // The SAME box the generator clips to (cam::MachineBounds): what is
      // drawn as the machine is what the file stays inside.
      const std::vector<glm::dvec2> bed = {{bedLo.x, bedLo.y},
                                           {bedHi.x, bedLo.y},
                                           {bedHi.x, bedHi.y},
                                           {bedLo.x, bedHi.y}};
      const bool fits = view.fits;
      const glm::vec4 line = fits ? kBedLine : kBedRefusedLine;
      LoopLines(bed, bedLo.z, 1.0f, line, prims);
      LoopLines(bed, bedHi.z, 0.8f, line, prims);
      for (const glm::dvec2& c : bed)
        AppendPrimLine(glm::vec3(c.x, c.y, bedLo.z),
                       glm::vec3(c.x, c.y, bedHi.z), 0.8f, line, prims);
    }
  }

  // The parts' labels and open curves, to engrave.
  AppendEngraveCurves(state, *setup, prims);

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

  // Where the worst fixture hit was, on the fixture's top: the one finding an
  // operator has to find on the bed. Only while there is a verdict, which is
  // what every place that drops the verdict relies on.
  if (state.camFixtureHit && !state.camVerdict.empty())
    AppendPrimDot(glm::vec3(*state.camFixtureHit), 9.0f,
                  glm::vec4(0.95f, 0.25f, 0.20f, 1.0f), prims);
}

bool CamEngraveCurveAt(AppState& state, const glm::vec2& mouse,
                       const glm::vec3& rayOrigin, const glm::vec3& rayDir,
                       float occluder, SelectionItem& out) {
  if (!state.job || state.camera == nullptr)
    return false;
  // A few pixels either side of a hairline: close enough to mean it, not so
  // wide that the letters of a label crowd each other out.
  constexpr float kPickPx = 7.0f;
  const glm::mat4 viewProj =
      state.camera->GetProjectionMatrix() * state.camera->GetViewMatrix();
  // WorldToScreen's mapping, with the point behind the eye refused.
  const auto toScreen = [&](const glm::vec3& p, glm::vec2& at) {
    const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
    if (clip.w <= 1e-6f)
      return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    at = state.viewportPos +
         glm::vec2((ndc.x + 1.0f) * 0.5f * state.viewportSize.x,
                   (ndc.y + 1.0f) * 0.5f * state.viewportSize.y);
    return true;
  };
  const glm::vec3 dir = glm::normalize(rayDir);
  // A curve lies ON its face, so it is behind the part only when it is
  // clearly further than the face the ray meets.
  const float slack = std::max(0.5f, occluder * 0.02f);
  float best = kPickPx;
  bool found = false;
  for (const EngraveCandidate& c : EngraveCandidates(state))
    for (const std::vector<glm::vec3>& stroke : c.strokes)
      for (size_t i = 0; i + 1 < stroke.size(); i++) {
        glm::vec2 a(0.0f), b(0.0f);
        if (!toScreen(stroke[i], a) || !toScreen(stroke[i + 1], b))
          continue;
        const glm::vec2 ab = b - a;
        const float len2 = glm::dot(ab, ab);
        const float t =
            len2 > 1e-9f
                ? std::clamp(glm::dot(mouse - a, ab) / len2, 0.0f, 1.0f)
                : 0.0f;
        const float d = glm::length(mouse - (a + ab * t));
        if (d >= best)
          continue;
        const glm::vec3 p = glm::mix(stroke[i], stroke[i + 1], t);
        if (occluder >= 0.0f && glm::dot(p - rayOrigin, dir) > occluder + slack)
          continue;
        best = d;
        found = true;
        out = SelectionItem{};
        out.kind = SelectionItem::Kind::SketchCurve;
        out.element = c.ref;
        out.jobPartId = c.jobPartId;
      }
  return found;
}

void DrawCamLabels(AppState& state) {
  if (!state.camToolOpen || !state.job || state.camGhostOp == 0)
    return;
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;
  const cam::Operation* op = FindOp(*setup, state.camGhostOp);
  if (op == nullptr || op->targets.empty() || op->type == cam::OpType::Engrave)
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
  ImDrawList* dl = ViewportDrawList();
  const ImVec2 size = ImGui::CalcTextSize(text);
  dl->AddRectFilled(ImVec2(at.x - 4.0f, at.y - 3.0f),
                    ImVec2(at.x + size.x + 4.0f, at.y + size.y + 3.0f),
                    IM_COL32(30, 24, 16, 220), 3.0f);
  dl->AddText(ImVec2(at.x, at.y), IM_COL32(255, 190, 120, 255), text);
}

namespace {

// What the expensive half of a verification needs, COPIED -- plain values and
// shape handles, nothing another thread can be mutating -- and what it makes.
// The same split phase 1 made for adaptive clearing and phase 5 for
// surfacing: prepare on the frame, inspect on a worker, land on the frame.
struct VerifyWork {
  // in
  std::string gcode;
  cam::Setup setup;
  std::vector<Body> bodies;
  std::vector<cam::Toolpath::Relief> reliefs;
  std::vector<cam::Toolpath::Tab> tabs;
  std::vector<cam::Toolpath::Groove> grooves; // engraving: not gouge
  std::vector<cam::Toolpath::Thread> threads; // judged on their own terms
  std::vector<cam::FixtureSolid> fixtures; // bed space
  std::vector<std::string> missingFixtures;
  // The job's other setups and their posted files, and the ones that could
  // not be read yet (docs/cam/simulation.md#leftover-another-setup-removes).
  std::vector<cam::Setup> others;
  std::vector<std::string> otherFiles;
  std::vector<bool> othersEarlier; // cut before this setup, in job order
  std::vector<std::string> othersNotChecked;
  bool rotary = false;
  uint64_t generation = 0;
  // out
  cam::Verdict verdict;
  std::vector<glm::dvec3> leftover;
  double tolerance = 0.0;
  bool ok = false;
  std::string why;
};

// 0.4 mm cells: fine enough to catch a gouge that matters on a mill, coarse
// enough that the check keeps up with the button.
constexpr double kVerifyRes = 0.4;
// Half a millimetre: the rotary map's own default.
constexpr double kRotaryVerifyRes = 0.5;

// The expensive half. Touches nothing but its own struct, so it is the same
// function inline and on a worker.
//
// The STOCK MODEL follows the file, exactly as `VerifyGcode` routes it: a
// program whose A moves is cut from a cylinder that turns, and measuring it
// against a height map produces a verdict about a program that does not
// exist. This panel once did precisely that and showed the rolling pin "Cuts
// 3.98 mm INTO the part, that is scrap" beside one quiet line admitting the
// check could not read the file.
void InspectWork(VerifyWork& w) {
  std::vector<cam::SetupProgram> programs;
  for (size_t k = 0; k < w.others.size(); k++)
    programs.push_back({&w.others[k], w.otherFiles[k], w.othersEarlier[k]});
  cam::OtherSetups others = cam::OtherSetupsOf(
      w.setup, programs, w.bodies, w.rotary ? kRotaryVerifyRes : kVerifyRes);
  others.notChecked.insert(others.notChecked.end(), w.othersNotChecked.begin(),
                           w.othersNotChecked.end());
  if (w.rotary) {
    const cam::RotaryInspection ri = cam::InspectRotary(
        w.gcode, w.setup, w.bodies, kRotaryVerifyRes, &others);
    w.ok = ri.ok;
    w.why = ri.why;
    w.verdict = ri.verdict;
    w.verdict.fixturesSkippedRotary = !w.fixtures.empty();
    w.verdict.otherSetupsNotChecked = others.notChecked;
    w.tolerance = kRotaryVerifyRes * 1.5;
    // No overlay: the leftover is painted from a HEIGHT MAP, and a radius
    // field has no renderer yet (docs/cam/rotary.md#the-rotary-picture).
    return;
  }
  const cam::Inspection insp =
      cam::Inspect(w.gcode, w.setup, w.bodies, kVerifyRes, w.reliefs, w.tabs,
                   w.fixtures, &others, w.grooves, w.threads);
  w.ok = insp.ok;
  w.verdict = insp.verdict;
  w.verdict.otherSetupsNotChecked = others.notChecked;
  // The tolerance IS the grid: a "gouge" of half a cell is quantisation, and
  // reporting it would teach the user to ignore the verdict.
  w.tolerance = kVerifyRes * 1.5;
  if (insp.ok)
    w.leftover = cam::LeftoverPoints(insp, w.tolerance, 3000);
}

// The cheap half, on the frame: the words, and everything that reads the
// live setup. Drops a result a later request has overtaken -- a verdict about
// the previous toolpath is worse than none.
void LandVerification(AppState& state, const VerifyWork& w) {
  if (w.generation != state.camVerifyGeneration)
    return;
  state.camVerdict.clear();
  state.camLeftover.clear();
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;
  if (!w.ok) {
    if (!w.why.empty())
      state.camVerdict.push_back({cam::VerdictLine::Level::Bad, w.why});
    return;
  }
  state.camLeftover = w.leftover;
  bool breaksAnEdge = false;
  for (const cam::Operation& op : setup->operations)
    breaksAnEdge |=
        op.enabled && cam::OpTypeHas(op.type, cam::OpField::EdgeSize);
  state.camVerdict = cam::Explain(
      w.verdict, w.tolerance, cam::CuttingMinutes(*setup, state.camToolpaths),
      breaksAnEdge);
  if (w.verdict.fixtureCollisions > 0)
    state.camFixtureHit =
        glm::dvec3(w.verdict.worstFixtureAt, w.verdict.worstFixtureZ);
  if (const auto line = ui::FixtureTravelLine(state, *setup, w.verdict))
    state.camVerdict.push_back(*line);
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
  for (const std::string& name : w.missingFixtures)
    state.camVerdict.push_back(
        {cam::VerdictLine::Level::Warn,
         "Not checked: fixture \"" + name + "\" -- its part is gone"});
  for (const std::string& u : feeds.unchecked)
    state.camVerdict.push_back(
        {cam::VerdictLine::Level::Warn, "Not checked: " + u});
  // The machine's lines: the parts' reach beyond the travel, or the sheet's
  // overhang -- warnings, never refusals. Cut as far as it reaches.
  for (const cam::VerdictLine& line :
       cam::MachineLines(*setup, ui::JobBodies(state), &state.camToolpaths))
    state.camVerdict.push_back(line);
}

} // namespace

void RunCamVerification(AppState& state) {
  state.camVerdict.clear();
  state.camLeftover.clear();
  state.camFixtureHit.reset();
  // Every request overtakes the one before it, whether that one is still on
  // a worker or not: a verdict landing about a toolpath that has since moved
  // is the thing this whole check exists to avoid.
  state.camVerifyGeneration++;
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

  auto work = std::make_shared<VerifyWork>();
  std::string error;
  if (!cam::PostProcess(setup->machine.post, *setup, state.camToolpaths,
                        PostOpts(*setup), work->gcode, &error)) {
    state.camVerdict.push_back(
        {cam::VerdictLine::Level::Bad, "Could not post this job: " + error});
    return;
  }
  for (const cam::Toolpath& tp : state.camToolpaths) {
    work->reliefs.insert(work->reliefs.end(), tp.reliefs.begin(),
                         tp.reliefs.end());
    work->tabs.insert(work->tabs.end(), tp.tabs.begin(), tp.tabs.end());
    work->grooves.insert(work->grooves.end(), tp.grooves.begin(),
                         tp.grooves.end());
    work->threads.insert(work->threads.end(), tp.threads.begin(),
                         tp.threads.end());
  }
  work->setup = *setup;
  work->bodies = ui::JobBodies(state);
  work->fixtures =
      ui::JobFixtureSolids(state, *setup, true, &work->missingFixtures);
  // Read with the machine's own rotary letter: a B-driven file parsed as A
  // reads as standing still and is checked on the wrong stock model.
  work->rotary = cam::RotatesWhileCutting(
      cam::ProgramInSetupSpace(work->gcode, work->setup));
  work->generation = state.camVerifyGeneration;

  // The job's other setups, so what one of them removes is said as its cut
  // (docs/cam/simulation.md#leftover-another-setup-removes). From the same
  // caches, never computed on the frame: a pass not cut yet is queued, this
  // verdict names that setup as not yet compared, and the whole verification
  // runs again when the passes land. Once per state of the job, so passes
  // evicted from the cache cannot keep it cycling.
  static uint64_t requeuedFor = 0;
  const uint64_t jobState = JobRenderVersion(state) * 1000003u + setup->id;
  bool othersPending = false;
  bool before = true; // the job's order is the order they are cut in
  for (const cam::Setup& other : state.job->GetSetups()) {
    if (other.id == setup->id)
      before = false;
    if (other.id == setup->id || other.operations.empty())
      continue;
    bool pending = false;
    const std::vector<cam::Toolpath> paths =
        CamToolpathsQueued(state, other, pending);
    if (pending) {
      othersPending = true;
      work->othersNotChecked.push_back(
          "setup '" + other.name +
          "', because its toolpaths are still being "
          "cut");
      continue;
    }
    std::string file, why;
    if (!cam::PostProcess(other.machine.post, other, paths, PostOpts(other),
                          file, &why)) {
      work->othersNotChecked.push_back("setup '" + other.name +
                                       "', because it does not post: " + why);
      continue;
    }
    work->others.push_back(other);
    work->otherFiles.push_back(std::move(file));
    work->othersEarlier.push_back(before);
  }
  if (othersPending && requeuedFor != jobState) {
    requeuedFor = jobState;
    state.camVerifyWhenSettled = true;
  }

  // Inline when the panel is not deferring -- the GUI suite's setting, so
  // every test that reads the verdict right after Generate stays
  // deterministic, which is phase 1's own precedent for `camDeferPasses`.
  if (!state.camDeferPasses) {
    InspectWork(*work);
    LandVerification(state, *work);
    return;
  }

  // Otherwise on a worker. This ran ON THE FRAME, and on the rolling pin --
  // 644,160 rotary moves carved into a radius field -- it was minutes with
  // the app stopped, in the continuation that fires when the last surfacing
  // pass lands. Taking the passes off the frame had just moved the freeze to
  // the moment they finished.
  state.camVerdict.push_back(
      {cam::VerdictLine::Level::Warn,
       "Verifying the posted file against the model..."});
  auto job = std::make_unique<BackgroundJob>();
  job->kind = "cam-verify";
  job->label = work->rotary ? "Verifying against the rotary stock"
                            : "Verifying against the stock";
  job->cancellable = false; // the carve has no interruption points
  job->fraction.store(-1.0);
  BackgroundJob* raw = job.get();
  job->thread = std::thread([work, raw] {
    InspectWork(*work);
    raw->done.store(true); // last
  });
  job->finish = [work](AppState& s) -> bool {
    LandVerification(s, *work);
    return true;
  };
  StartBackgroundJob(state, std::move(job));
}

} // namespace ui

#else // !HAVE_CAM

namespace ui {
void DrawCamToolWindow(AppState&) {}
void EditCamOperation(AppState&, uint64_t) {}
void EditCamTool(AppState&, int) {}
void EditCamFixture(AppState&, uint64_t) {}
void CamFixturesChanged(AppState&, cam::Setup&) {}
bool CamStockBox(AppState&, glm::dvec3&, glm::dvec3&) { return false; }
void StartCamVerb(AppState&, AppState::CamVerb) {}
int VerbTool(const AppState&, const cam::Setup&) { return 0; }
void LookDownTheSpindle(AppState&) {}
void ArmCamPick(AppState&, PickSlot, const char*) {}
void CamFacePicked(AppState&, const SelectionItem&) {}
void AppendCamVisualisation(AppState&, std::vector<PrimVertex>&,
                            std::vector<LineVertex3D>&) {}
void DrawCamLabels(AppState&) {}
bool CamEngraveCurveAt(AppState&, const glm::vec2&, const glm::vec3&,
                       const glm::vec3&, float, SelectionItem&) {
  return false;
}
void RunCamVerification(AppState&) {}
} // namespace ui

#endif
