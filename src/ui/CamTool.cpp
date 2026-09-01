#include "ui/CamTool.h"

#ifdef HAVE_CAM

#include "common/Logger.h"
#include "core/Document.h"
#include "core/cam/CamPost.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"
#include "ui/AppActions.h" // AppendPrimLine/Dot, WorldToScreen
#include "ui/CamPanel.h"
#include "ui/JobActions.h"

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
     PickSlot::CamProfileOutline,
     "Click the wall to follow — the part's own side, or the inside of an "
     "opening",
     "Follow a wall. The part's own side cuts it free of the stock, with tabs "
     "so it does not break loose under the cutter; the wall of an opening "
     "cuts that opening out. An opening that goes all the way through has no "
     "floor to click, so its wall is the only thing to point at."},
    {"Drill a hole", "###cam_verb_drill", cam::OpType::Drill,
     PickSlot::CamDrillHole, "Click the inside of the hole to drill",
     "Drill a hole that is already in the model: click its cylindrical wall "
     "and the depth comes from the hole's own extent. ONE plunge on the "
     "hole's own axis at the tool's plunge feed — no pecking and no helix, so "
     "the tool has to be able to plunge the full depth and clear its own "
     "chips. The verb stays armed, so a bolt circle is one click per hole."},
    {"Break an edge", "###cam_verb_edge", cam::OpType::EdgeRound,
     PickSlot::CamEdgeWall, "Click the wall whose top edge to round or chamfer",
     "Follow a wall's top edge with a shaped cutter: a ball nose rounds it "
     "over, a V-bit chamfers it. The TOOL decides which, so a flat end mill "
     "is refused rather than ploughing a square step. The size of the break "
     "comes from the tool and the wall unless the operation says otherwise."},
};

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

// Creates the operation a verb implies, or refuses in words. The derivation
// itself is engine-side (cam::DeriveOperation) so a headless test can reach
// it — the panel must not be where a depth comes from.
bool CreateVerbOperation(AppState& state, cam::OpType type,
                         const ElementRef& target) {
  state.camRefusal.clear();
  if (!state.job)
    return false;
  cam::Setup* setup = EnsureCamSetup(state);
  if (setup == nullptr)
    return false;
  // Which job part the picked face is on, read off the body it resolves to:
  // two parts in one job can own the same element name (v73).
  const cam::DerivedOp derived =
      cam::DeriveOperation(*setup, ui::JobBodies(state), type,
                           ui::TargetFor(state, target), state.camVerbTool);
  if (!derived.ok) {
    state.camRefusal = derived.why;
    return false;
  }
  cam::Operation op = derived.op;
  op.id = state.job->AllocateId();
  setup->operations.push_back(op);
  state.camGhostOp = op.id;
  state.projectMetaDirty = true;
  // Just the new one: the operations already in the job did not change, and
  // one of them may be an adaptive pocket that takes seconds to re-cut.
  RefreshCamToolpath(state, op.id);
  RunCamVerification(state);
  LOG_INFO("CAM: {} from a picked face, Z {:.3f} to {:.3f}", op.name, op.topZ,
           op.bottomZ);
  return true;
}

// The picked face's plane, in MODEL space. Both orientation picks need it,
// and neither wants the setup frame applied — the whole point is to work out
// what the frame should become.
bool PickedFaceNormal(const std::vector<Body>& bodies, const ElementRef& ref,
                      glm::vec3& normal) {
  glm::vec3 centre(0.0f), uAxis(0.0f);
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId)
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    return idx && body.shape.FacePlane(*idx, centre, normal, uAxis);
  }
  return false;
}

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

void CamFacePicked(AppState& state, const SelectionItem& item) {
  const PickSlot slot = static_cast<PickSlot>(state.slotPick.slot);
  if (!item.element.IsSet() || !state.job) {
    state.slotPick.miss = "that is not a face this job can use";
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
    glm::vec3 normal(0.0f);
    if (PickedFaceNormal(ui::JobBodies(state), item.element, normal)) {
      const glm::dquat want =
          cam::SolveFaceUp(glm::dvec3(normal), setup->frame);
      if (std::abs(glm::dot(want, setup->frame)) < 1.0 - 1e-9) {
        setup->frame = want;
        reclamped = true;
      }
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
    setup->topFace = ui::TargetFor(state, item.element);
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
    if (!PickedFaceNormal(ui::JobBodies(state), item.element, normal)) {
      state.slotPick.miss = "pick a flat face — that one is curved";
      return;
    }
    setup->frame = cam::SolveFaceUp(glm::dvec3(normal), setup->frame);
    CamOrientationChanged(state, *setup);
    LOG_INFO("CAM: laid a face up, frame ({:.4f} {:.4f} {:.4f} {:.4f})",
             setup->frame.w, setup->frame.x, setup->frame.y, setup->frame.z);
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
    if (!PickedFaceNormal(ui::JobBodies(state), item.element, normal)) {
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
  if (CreateVerbOperation(state, type, item.element)) {
    state.slotPick.miss.clear();
    state.camNote = "added — keep clicking faces, or press Escape when done";
  } else {
    state.slotPick.miss = state.camRefusal;
  }
  state.slotPick.prompt = prompt;
}

void DrawCamToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "CAM");
  if (!state.job) {
    ImGui::TextWrapped("CAM cuts a part. Switch to a part tab.");
    if (ImGui::Button("Done###cam_tool_done"))
      state.camToolOpen = false;
    ImGui::End();
    return;
  }

  cam::Setup* setup = ActiveCamSetup(state);
  const bool armed = state.slotPick.featureId == kCamToolSlot;

  // Z zero first, and loudly, because it is the number that breaks tools.
  ImGui::TextDisabled("Zero");
  const bool armedTop =
      armed && state.slotPick.slot == static_cast<int>(PickSlot::CamTopFace);
  if (ImGui::Button(setup != nullptr && setup->topFace.IsSet()
                        ? "Re-pick top face###cam_tool_topface"
                        : "Pick top face###cam_tool_topface"))
    ArmCamPick(state, PickSlot::CamTopFace,
               "Click the top face — that sets Z zero for the job");
  ImGui::SameLine();
  if (armedTop)
    ImGui::TextDisabled("click the top face…");
  else if (setup != nullptr && setup->topFace.IsSet())
    ImGui::TextDisabled("Z zero at %.3f", setup->origin.z);
  else
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
                       "Z zero is not set yet");

  // How the part is clamped. Offered to every job, because it is a statement
  // about the fixture and not about the machine — the reason it exists is
  // that a part modelled upright (a speaker baffle, an imported panel) has no
  // level face worth zeroing on until it is laid down.
  const bool armedLay =
      armed && state.slotPick.slot == static_cast<int>(PickSlot::CamLayFaceUp);
  if (ImGui::Button("Lay a face up###cam_lay_face"))
    ArmCamPick(state, PickSlot::CamLayFaceUp,
               "Click the face to clamp facing up — that is how the part sits "
               "on the bed");
  ImGui::SetItemTooltip("Clamp the part with this face up. Changes nothing in "
                        "the model and posts no rotary move — it is how the "
                        "part sits on the bed for this setup.");
  ImGui::SameLine();
  if (armedLay)
    ImGui::TextDisabled("pick a face…");
  else if (setup != nullptr && !cam::FrameIsSquare(*setup))
    ImGui::TextDisabled("laid flat");
  else
    ImGui::TextDisabled("as modelled");

  // The ROTARY index, which is a different claim and only true of a machine
  // that has the axis. Offering it unconditionally is what let a 3-axis job
  // be handed `G0 A90` for wanting a panel flat (docs/cam/setup-frame.md).
  if (setup != nullptr && setup->hasRotaryAxis) {
    const bool armedIndex =
        armed &&
        state.slotPick.slot == static_cast<int>(PickSlot::CamIndexFace);
    if (ImGui::Button("Turn a face up###cam_index_face"))
      ArmCamPick(state, PickSlot::CamIndexFace,
                 "Click the face to turn up under the spindle");
    ImGui::SetItemTooltip("Rotary (4th axis) jobs: index the table so this "
                          "face points at the spindle, then use the verbs as "
                          "usual");
    ImGui::SameLine();
    if (armedIndex)
      ImGui::TextDisabled("pick a face…");
    else if (std::abs(setup->rotationA) > 1e-9)
      ImGui::TextDisabled("A %.2f°", setup->rotationA);
    else
      ImGui::TextDisabled("A 0° (3-axis)");
  }

  // Why the last pick did not take. This window never drew it, so a refused
  // click looked exactly like a click that did nothing — and one of them
  // ("add a setup first") was reachable on any fresh part
  // (docs/cam/dogfood-round.md). The docked panel printed it all along.
  if (armed && !state.slotPick.miss.empty()) {
    ImGui::PushTextWrapPos(320.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", state.slotPick.miss.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
  }
  // …and what the last action DID, when it did more than was asked. Laying
  // the part flat to take a Z zero, or re-deriving depths after a re-clamp,
  // are consequences the user has to be told about — the alternative is a job
  // that changed under them between one click and the next.
  if (!state.camNote.empty()) {
    ImGui::PushTextWrapPos(320.0f);
    ImGui::TextDisabled("%s", state.camNote.c_str());
    ImGui::PopTextWrapPos();
  }

  // The plan view of THIS setup. For a plain 3-axis job (rotationA = 0) the
  // spindle is world up and this equals the Top view — the world is Z-up
  // since docs/up-axis.md — but an indexed setup looks down ITS spindle,
  // not the world's, and the stock-framed zoom is useful either way.
  if (ImGui::Button("Look down the spindle###cam_view_spindle") &&
      state.camera != nullptr && setup != nullptr) {
    // The camera goes where the spindle is and looks the way it looks, with
    // the setup's own +Y up the screen. Snap rather than animate: this is a
    // "show me the job" button, not a flourish.
    const glm::vec3 dir = glm::normalize(
        glm::vec3(cam::FromSetupSpace(*setup, glm::dvec3(0, 0, 1))));
    const glm::vec3 up = glm::normalize(
        glm::vec3(cam::FromSetupSpace(*setup, glm::dvec3(0, 1, 0))));
    state.camera->SnapToView(dir, up);
    // …and framed on the STOCK, which is the thing being cut. Keeping the
    // previous zoom gave a plan view of four square millimetres if that is
    // where you happened to be.
    glm::dvec3 lo(0.0), hi(0.0);
    if (cam::StockBounds(*setup, ui::JobBodies(state), lo, hi)) {
      // StockBounds reports the box in SETUP space; the camera lives in model
      // space, so every corner comes back through the frame.
      glm::vec3 mn(std::numeric_limits<float>::max());
      glm::vec3 mx(std::numeric_limits<float>::lowest());
      for (int c = 0; c < 8; c++) {
        const glm::dvec3 corner((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y,
                                (c & 4) ? hi.z : lo.z);
        const glm::vec3 w(cam::FromSetupSpace(*setup, corner));
        mn = glm::min(mn, w);
        mx = glm::max(mx, w);
      }
      FitCameraToWorldBox(state, mn, mx);
      // Fitting moves the TARGET, and Camera::SetTarget keeps the camera
      // where it is and re-aims — so it swings the view off the axis that
      // was just set. Aim first (so the fit's pan away from this window is
      // computed in the right frame), fit, then aim again.
      state.camera->SnapToView(dir, up);
    }
  }
  ImGui::SetItemTooltip("Plan view in the frame this setup cuts in, framed "
                        "on the stock — for an indexed setup this is the "
                        "rotated spindle, not the world's Top");

  ImGui::Separator();
  ImGui::TextDisabled("Machine it");
  // WHICH CUTTER the next verb derives for. Every number a verb produces is a
  // fraction of the cutter, so choosing the tool after the fact means
  // re-deriving; and the verbs are the one-click way in, which is worth
  // nothing if the one click has to be undone to change tools.
  if (setup != nullptr && setup->tools.size() > 1) {
    ImGui::TextDisabled("Tool");
    for (const cam::Tool& tool : setup->tools) {
      ImGui::SameLine();
      const bool active = state.camVerbTool == 0
                              ? tool.number == setup->tools.front().number
                              : tool.number == state.camVerbTool;
      if (active)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      char id[40];
      std::snprintf(id, sizeof(id), "T%d###cam_verb_t%d", tool.number,
                    tool.number);
      if (ImGui::SmallButton(id))
        state.camVerbTool = tool.number;
      if (active)
        ImGui::PopStyleColor();
      ImGui::SetItemTooltip("%.3g mm %s — what the next verb derives for",
                            tool.diameter, tool.name.c_str());
    }
  }
  for (const Verb& verb : kVerbs) {
    const bool armedHere =
        armed && state.slotPick.slot == static_cast<int>(verb.slot);
    // NOT disabled while another pick is armed. They were, back when a verb
    // disarmed itself on success; now that one stays armed, greying its
    // neighbours out means Escape between every change of verb. Any pick
    // button switches to itself.
    if (ImGui::Button((std::string(verb.label) + verb.id).c_str(),
                      ImVec2(190.0f, 0.0f))) {
      // Pressing the armed verb again puts it away. A verb that STAYS armed
      // after a pick needs a way out that is not only the Escape key.
      if (armedHere) {
        state.slotPick = {};
        state.camNote.clear();
      } else {
        ArmCamPick(state, verb.slot, verb.prompt);
      }
    }
    ImGui::SetItemTooltip("%s", verb.tip);
    if (armedHere) {
      ImGui::SameLine();
      ImGui::TextDisabled("click faces…");
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

  if (!state.camRefusal.empty()) {
    // Wrapped at the same width the verdict uses. Unwrapped, one long refusal
    // stretched this floating window to the width of the sentence — across
    // the whole app, over the panel and the model both.
    ImGui::PushTextWrapPos(320.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", state.camRefusal.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
  }

  // What the job is, in one sentence per operation — the numbers are derived,
  // so they are shown rather than asked for. The drag fields still exist, in
  // the CAM panel, for anyone who wants them.
  if (setup != nullptr && !setup->operations.empty()) {
    ImGui::Separator();
    // Deferred, so the list is not mutated while it is being walked — the
    // same shape the docked panel uses.
    uint64_t removeOp = 0;
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
      // Removing an operation was only possible in the DOCKED panel, which is
      // a tab behind Model — while every operation is made here. An operation
      // added by mistake (or three, when the first two silently cut nothing)
      // could not be taken back without going to find another window
      // (docs/cam/dogfood-round.md).
      if (ImGui::SmallButton("x###rmop"))
        removeOp = op.id;
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
        if (tp.operationId == op.id)
          path = &tp;
      ImGui::Text("%s: %.3g mm tool, %.3g mm deep in %d pass%s, %s%s",
                  op.name.c_str(), tool != nullptr ? tool->diameter : 0.0,
                  depth, passes, passes == 1 ? "" : "es",
                  op.climb ? "climb" : "conventional",
                  op.type == cam::OpType::Adaptive ? ", adaptive" : "");
      if (path != nullptr && !path->error.empty()) {
        ImGui::PushTextWrapPos(320.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s cut nothing — %s", ICON_FA_TRIANGLE_EXCLAMATION,
                           path->error.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
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
    if (removeOp != 0) {
      auto& ops = setup->operations;
      for (auto it = ops.begin(); it != ops.end(); ++it)
        if (it->id == removeOp) {
          ops.erase(it);
          break;
        }
      if (state.camGhostOp == removeOp)
        state.camGhostOp = 0;
      state.projectMetaDirty = true;
      DropCamToolpath(state, removeOp);
      RunCamVerification(state);
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
  if (!state.job)
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
      cam::StockBounds(*setup, ui::JobBodies(state), lo, hi)) {
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
      state,
      glm::vec3(cam::FromSetupSpace(
          *setup, {centre.x, centre.y, (op->topZ + op->bottomZ) * 0.5})));
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
  const cam::Setup* setup = ActiveCamSetup(state);
  if (setup == nullptr)
    return;

  std::string gcode, error;
  if (!cam::PostProcess(setup->post, *setup, state.camToolpaths,
                        PostOpts(*setup), gcode, &error)) {
    state.camVerdict.push_back(
        {cam::VerdictLine::Level::Bad, "Could not post this job: " + error});
    return;
  }
  // 0.4 mm cells: fine enough to catch a gouge that matters on a mill, coarse
  // enough that the check runs while the user is still looking at the button.
  constexpr double kRes = 0.4;
  const cam::Inspection insp =
      cam::Inspect(gcode, *setup, ui::JobBodies(state), kRes);
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
