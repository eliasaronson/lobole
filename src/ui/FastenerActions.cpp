#include "ui/AssemblyActions.h"

#include "core/cam/CamToolpath.h" // StockBounds (the placement snap)
#include "imgui.h" // KeyCtrl: the placement drag snaps unless Ctrl says free
#include "ui/CamPanel.h" // ActiveCamSetup, RefreshCamToolpaths (the placement gizmo)
#include "ui/CamTool.h" // RunCamVerification
#include "ui/DocumentSlots.h"
#include "ui/JobActions.h" // JobPlacedBodies
#include "ui/ProjectActions.h"
#include "ui/SelectionPipeline.h"
#include "ui/ViewActions.h"

#include "ui/FeatureEditor.h" // PickSlot, kMateSessionSlot (U7c mate slots)

#include "cad/CADKernel.h" // CADKernel::ImportedModel (import)
#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingCallout.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FastenerFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ImportAssembly.h"
#include "core/io/Serialization.h"
#include "core/parts/AxisStack.h"
#include "core/parts/FastenerProfile.h"
#include "core/parts/FastenerTable.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"
#include "render/VulkanCommon.h"
#include "ui/DragMath.h"

#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <OSD.hxx>              // per-thread signal handler (prewarm workers)
#include <Standard_Failure.hxx> // ...and what it converts faults into

#include <atomic>
#include <thread>

// Putting a standard fastener in a hole: deriving which one from the hole it
// is dropped on, sizing it to the stack it has to span, and mating it to
// every interface along the way.
// Split out of AssemblyActions.cpp (docs/architecture-review-2026-09-06.md,
// stage 6). AssemblyActions.h still declares everything it declared.

namespace ui {

uint64_t AddFastener(AppState& state, const std::string& standard,
                     const std::string& size, double length,
                     const std::string& drive, const glm::vec3& position,
                     const glm::quat& orientation, std::string* error,
                     uint64_t ridesOn) {
  const auto fail = [&](const std::string& why) -> uint64_t {
    if (error)
      *error = why;
    return 0;
  };
  if (state.assembly == nullptr)
    return fail("a fastener is a bought part placed in an ASSEMBLY, not "
                "geometry in a part — on a part tab it would be machined by "
                "CAM, counted as the part's own mass, and reported by "
                "check_geometry as a disconnected body. Switch to an assembly "
                "tab (set_active_part) and place it there.");

  // Built once, off to the side, purely to name the spec and to let the table
  // refuse an unstocked one BEFORE a document exists for it. A refusal that
  // arrives after a tab has been created leaves the tab behind.
  FastenerFeature probe("", standard, size, length);
  probe.drive = drive;
  {
    Document scratch;
    auto copy = std::make_unique<FastenerFeature>(probe);
    scratch.AddFeature(std::move(copy));
    if (scratch.GetBodies().empty())
      return fail(scratch.GetFeature(0)->GetError());
  }
  const std::string designation = probe.Designation();

  // One part per spec: an existing one with this designation is THE part, and
  // every occurrence of it aggregates in the BOM for free.
  uint64_t partId = PartIdByName(state, designation);
  if (partId == 0) {
    const int assemblyTab = state.activePart;
    AddPart(state, designation); // switches tabs, like the importer's
    PartSlot& slot = state.parts[static_cast<size_t>(state.activePart)];
    slot.folder = kStandardPartsFolder;
    CreateFolder(state, kStandardPartsFolder);
    partId = slot.id;
    auto feature = std::make_unique<FastenerFeature>(probe);
    feature->SetName(designation);
    state.document->AddFeature(std::move(feature));
    // Back to the assembly the caller was on: creating the part is a side
    // effect of placing one, and a tab switch the caller did not ask for is
    // the kind of thing that reads as a bug.
    if (assemblyTab >= 0 && assemblyTab < static_cast<int>(state.parts.size()))
      SetActivePart(state, static_cast<size_t>(assemblyTab));
  }
  if (state.assembly == nullptr)
    return fail("could not return to the assembly after creating " +
                designation);

  Occurrence occ;
  occ.partId = partId;
  occ.name = designation;
  occ.position = position;
  occ.orientation = orientation;
  // Attached to the part it bears on, so it travels with that part and cannot
  // be dragged out of its hole on its own (docs/parts/f1-loose-ends.md). The
  // offset is whatever the two poses already imply, expressed in the parent's
  // frame — quaternion arithmetic rather than a matrix round trip, which is
  // exact for a rigid placement and needs no decomposition.
  if (const Occurrence* parent =
          ridesOn != 0 ? state.assembly->FindById(ridesOn) : nullptr) {
    const glm::quat inv = glm::inverse(parent->orientation);
    occ.attachedTo = ridesOn;
    occ.attachOffset = inv * (position - parent->position);
    occ.attachRotation = inv * orientation;
  }
  return state.assembly->AddOccurrence(occ);
}

namespace {
// +Z (a fastener's own reach direction) rotated onto `to`.
//
// Written out rather than pulled from glm's experimental quaternion header,
// which this file does not otherwise need and which errors unless a global
// GLM_ENABLE_EXPERIMENTAL is defined.
glm::quat AlignZTo(const glm::vec3& to) {
  const glm::vec3 from(0.0f, 0.0f, 1.0f);
  const float dot = glm::dot(from, to);
  if (dot < -0.999999f)
    return glm::quat(0.0f, 1.0f, 0.0f, 0.0f); // 180 degrees about X
  if (dot > 0.999999f)
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  const glm::vec3 c = glm::cross(from, to);
  return glm::normalize(glm::quat(1.0f + dot, c.x, c.y, c.z));
}

// One dimension of a companion part (a washer's thickness), refusing by name
// when the standard is not the kind the caller asked it to be. A "washer"
// that turns out to be a nut would place silently and be found by eye.
std::optional<double> CompanionDim(const std::string& standard,
                                   parts::Kind wanted, const std::string& size,
                                   std::initializer_list<const char*> columns,
                                   std::string* why) {
  const parts::Profile* profile = parts::ProfileFor(standard);
  if (profile == nullptr) {
    *why = "\"" + standard +
           "\" is not a standard this build makes — it makes " +
           parts::KnownStandards();
    return std::nullopt;
  }
  if (profile->kind != wanted) {
    *why = profile->designation + " is a " + profile->what + ", not a " +
           (wanted == parts::Kind::Nut ? "nut" : "washer");
    return std::nullopt;
  }
  const auto dims = parts::Dimensions(standard, size, why);
  if (!dims)
    return std::nullopt;
  for (const char* column : columns) {
    const auto it = dims->find(column);
    if (it != dims->end())
      return it->second;
  }
  *why = profile->designation + " " + size + " has no thickness to stack";
  return std::nullopt;
}

// The planar face an axis crosses at `t`, named the way a mate connector
// wants it: whose body owns it, and the face's mapped name.
//
// A bolt joins two parts, and a mate between parts anchors on FACES — so the
// joint a fastener makes has to be found in the same geometry its traversal
// already walked. Perpendicular to the axis, at the right distance along it,
// facing `outward`.
//
// Coplanar candidates (a stepped part has them) are broken by centroid
// distance to the axis, and it is worth saying why that crude tiebreak is
// enough: every face in the running is in the SAME PLANE, so picking the
// other one moves nothing — the mate's in-plane dx/dy absorb the difference
// and the joint holds identically. The tiebreak decides which face the mate
// is LABELLED with, not where anything ends up.
std::optional<ElementRef> FaceAcrossAxis(const assembly::FlatInstance& inst,
                                         const glm::vec3& origin,
                                         const glm::vec3& along, double t,
                                         const glm::vec3& outward) {
  if (inst.part == nullptr)
    return std::nullopt;
  constexpr float kSquare = 0.999f; // perpendicular enough to the axis
  constexpr float kOnPlane = 0.05f; // mm off the expected station
  const glm::vec3 at = origin + along * static_cast<float>(t);
  std::optional<ElementRef> best;
  float nearest = 0.0f;
  for (const Body& body : inst.part->GetBodies()) {
    for (const auto& [faceIndex, name] : body.elementMap.names) {
      glm::vec3 o, n, xHint;
      if (!body.shape.FacePlane(faceIndex, o, n, xHint))
        continue; // not planar: FaceAxis would answer, a bolt face is a plane
      const glm::vec3 nw =
          glm::normalize(glm::vec3(inst.world * glm::vec4(n, 0.0f)));
      if (glm::dot(nw, outward) < kSquare)
        continue;
      const glm::vec3 ow = glm::vec3(inst.world * glm::vec4(o, 1.0f));
      if (std::abs(glm::dot(ow - at, along)) > kOnPlane)
        continue;
      const auto info = Measure::DescribeFaceAt(body, faceIndex);
      if (!info)
        continue;
      const glm::vec3 c =
          glm::vec3(inst.world * glm::vec4(info->centroid, 1.0f)) - at;
      const float off = glm::length(c - glm::dot(c, along) * along);
      if (best && off >= nearest)
        continue;
      best = ElementRef{body.createdBy, name};
      nearest = off;
    }
  }
  return best;
}

// Hold two parts a fastener clamps to each other: a fastened mate across the
// interface between them, in the pose they are ALREADY in
// (docs/parts/f1-loose-ends.md).
//
// Attaching the hardware to the parts makes the screw follow its plate. It
// does not make the plates follow each other, so before this a bolted joint
// tore open the moment either part was dragged — and the Mates list, where a
// person looks to see what holds an assembly together, stayed empty.
//
// `tA` is where the axis LEAVES a and `tB` where it ENTERS b: the two faces
// that touch, which is the interface a person would have picked by hand. A
// gap between them is not an error here — it is an unmodelled gasket or a
// defect, the traversal already says which in the summary, and the mate holds
// whatever is actually there.
//
// The DOFs are MEASURED, never assumed: both connector frames are resolved
// through the same ConnectorLocalFrame the solver uses, and flip/rotation/
// offset/dx/dy come out of the transform between them. So the solve that
// follows moves nothing — which is the whole requirement, since the parts
// are already where the person put them and a mate that "corrects" them is
// a mate that breaks the model it was meant to record.
//
// Returns the mate's id, or 0 if the geometry could not name both faces or
// the two frames are not square to one axis — no mate is better than a wrong
// one, and the fastener itself is placed either way.
uint64_t JoinAcrossInterface(AppState& state, const assembly::FlatInstance& a,
                             double tA, const assembly::FlatInstance& b,
                             double tB, const glm::vec3& centre,
                             const glm::vec3& axis) {
  const glm::vec3 into = -axis;
  const std::optional<ElementRef> faceA =
      FaceAcrossAxis(a, centre, into, tA, into);
  const std::optional<ElementRef> faceB =
      FaceAcrossAxis(b, centre, into, tB, axis);
  if (!faceA || !faceB)
    return 0;

  Mate mate;
  mate.type = MateType::Fastened;
  mate.name = "Bolted";
  mate.a.occurrenceId = a.rootOccurrenceId;
  mate.a.innerPath = a.innerIds;
  mate.a.anchor.face = *faceA;
  mate.b.occurrenceId = b.rootOccurrenceId;
  mate.b.innerPath = b.innerIds;
  mate.b.anchor.face = *faceB;

  const assembly::PartResolver resolve =
      [&state](uint64_t partId) -> const Document* {
    return PartDocById(state, partId);
  };
  const assembly::AssemblyResolver asmResolve = AssemblyResolverFor(state);
  const auto fa = assembly::ConnectorLocalFrame(mate.a, *state.assembly,
                                                resolve, nullptr, asmResolve);
  const auto fb = assembly::ConnectorLocalFrame(mate.b, *state.assembly,
                                                resolve, nullptr, asmResolve);
  const Occurrence* occA = state.assembly->FindById(mate.a.occurrenceId);
  const Occurrence* occB = state.assembly->FindById(mate.b.occurrenceId);
  if (!fa || !fb || occA == nullptr || occB == nullptr)
    return 0;

  // What the solver will compose: world(b's frame) = world(a's frame) · adjust.
  const glm::mat4 wa = OccurrenceMatrix(*occA) * *fa;
  const glm::mat4 wb = OccurrenceMatrix(*occB) * *fb;
  const glm::mat4 rel = glm::inverse(wa) * wb;

  // Read the mate's DOFs off `rel`. A fastened adjust is T(dx,dy,offset) ·
  // Rz(rotation) · [Rx(180)], and both frames' Z lie along the same hole
  // axis, so it can express this transform exactly — but only if it really
  // is of that form, which the check below insists on rather than assumes.
  mate.flipped = rel[2][2] < 0.0f;
  mate.rotation = std::atan2(rel[0][1], rel[0][0]);
  mate.dx = rel[3][0];
  mate.dy = rel[3][1];
  mate.offset = rel[3][2];
  glm::mat4 adjust(1.0f);
  adjust = glm::translate(adjust, glm::vec3(mate.dx, mate.dy, mate.offset));
  adjust = glm::rotate(adjust, mate.rotation, glm::vec3(0.0f, 0.0f, 1.0f));
  if (mate.flipped)
    adjust = glm::rotate(adjust, glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      if (std::abs(adjust[c][r] - rel[c][r]) > 1e-3f)
        return 0; // the two faces are not square to one axis: say nothing

  return state.assembly->AddMate(mate);
}
} // namespace

FastenerFit FitFastenerToHole(AppState& state, const RimPick& rim,
                              const FastenerRequest& request) {
  FastenerFit fit;
  const auto fail = [&](const std::string& why) -> FastenerFit {
    fit.ok = false;
    fit.why = why;
    return fit;
  };
  if (state.assembly == nullptr)
    return fail("a fastener goes in an ASSEMBLY — switch to the assembly tab "
                "and pick the rim of the hole it passes through");

  const parts::Profile* screwProfile = parts::ProfileFor(request.standard);
  if (screwProfile == nullptr)
    return fail("\"" + request.standard +
                "\" is not a standard this build makes — it makes " +
                parts::KnownStandards());
  if (screwProfile->kind != parts::Kind::Screw)
    return fail(screwProfile->designation + " is a " + screwProfile->what +
                ", which does not pass through a hole — ask for a screw and "
                "put this on it with `nut` or `washer`");

  // Every placed leaf, once: the rim is on one of them and the stack is made
  // of the rest.
  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);
  std::vector<parts::PlacedBody> placed;
  // EVERY instance the element resolves on, not the first. An ElementRef is
  // unique within a PART; in an assembly it names one rim per placement of
  // that part, so collecting the candidates is what makes the ambiguity
  // visible instead of silently resolved.
  struct RimOn {
    const assembly::FlatInstance* inst;
    glm::vec3 centre;
    glm::vec3 axis;
    double radius;
  };
  std::vector<RimOn> candidates;
  for (const assembly::FlatInstance& inst : flat) {
    if (inst.part == nullptr || !inst.visible)
      continue;
    for (const Body& body : inst.part->GetBodies()) {
      if (body.reference)
        continue; // a derived ghost is not the occurrence's matter
      parts::PlacedBody pb;
      pb.body = &body;
      pb.world = inst.world;
      pb.part = inst.path.empty() ? body.name : inst.path;
      pb.partId = inst.partId;
      pb.occurrence = inst.rootOccurrenceId;
      placed.push_back(pb);
    }
    // The rim, in the frame the assembly puts it in. Described in the part's
    // own space and then placed, because that is where the ElementRef means
    // something.
    const std::optional<Measure::Element> element =
        Measure::Describe(inst.part->GetBodies(), rim.element);
    if (!element || element->kind != "edge" || element->shape != "circle" ||
        !element->hasRadius || !element->hasAxis)
      continue;
    const std::optional<glm::vec3> local =
        Measure::CentreOf(inst.part->GetBodies(), rim.element);
    if (!local)
      continue;
    candidates.push_back(
        {&inst, glm::vec3(inst.world * glm::vec4(*local, 1.0f)),
         glm::normalize(glm::vec3(inst.world * glm::vec4(element->axis, 0.0f))),
         element->radius});
  }
  if (candidates.empty())
    return fail("pick the RIM of the hole — a circular edge, the one the head "
                "sits against. A cylindrical face has an axis but no "
                "direction, so it cannot say which end the fastener goes in "
                "from; the rim says both.");

  const RimOn* chosen = nullptr;
  if (rim.occurrence != 0) {
    for (const RimOn& c : candidates)
      if (c.inst->rootOccurrenceId == rim.occurrence)
        chosen = &c;
    if (chosen == nullptr) {
      std::string where;
      for (const RimOn& c : candidates)
        where += (where.empty() ? "" : ", ") + c.inst->path + " (" +
                 std::to_string(c.inst->rootOccurrenceId) + ")";
      return fail("occurrence " + std::to_string(rim.occurrence) +
                  " does not carry that rim — it is on " + where);
    }
  } else if (candidates.size() == 1) {
    chosen = &candidates.front();
  } else {
    // Refused, not guessed — and the ambiguity is wider than it first looks.
    // An element name is unique within a DOCUMENT, so it is shared not only
    // by every placement of one part but by every part built the same way:
    // two plates drawn with the same three features carry the same feature
    // ids and the same mapped names. Taking the first put the fastener at the
    // near end of a two-plate joint, which reads as correct in the viewport.
    std::string where;
    for (const RimOn& c : candidates)
      where += (where.empty() ? "" : ", ") + c.inst->path + " (occurrence " +
               std::to_string(c.inst->rootOccurrenceId) + ")";
    return fail("that element is on " + std::to_string(candidates.size()) +
                " placements — " + where +
                ". An element name is unique within a PART, so every "
                "placement of it and every part built the same way carry the "
                "same one; say which occurrence you mean. A pick in the "
                "viewport carries it, a name on its own does not.");
  }
  glm::vec3 centre = chosen->centre;
  glm::vec3 axis = chosen->axis;
  const double radius = chosen->radius;

  // Which size the hole is for. ASK IT FIRST: a hole that records its fit
  // states the answer outright, and reading a diameter backwards out of the
  // clearance table only guesses at it — a guess the dock round showed is
  // weak, since 48% of whole millimetres from 1 to 160 are some size's
  // clearance and a Ø32 ladder rung reads as an M30
  // (docs/modeling/hole-standards.md).
  std::string why;
  const HoleFeature* drilled =
      drawing::FindHoleForRef(*chosen->inst->part, rim.element.name, radius);
  bool tapped = false;
  if (drilled != nullptr && !drilled->fitThread.empty()) {
    fit.size = drilled->fitThread;
    fit.clearance = drilled->fitKind;
    tapped = drilled->fitKind == "tapped";
  } else if (!request.size.empty()) {
    // The caller broke the tie. Believe the SIZE, but not that this hole is
    // one: a size whose clearances do not include this diameter would put an
    // M12 in an M6 hole and report it as derived.
    const auto fits =
        parts::Dimensions("clearance_hole_sizes", request.size, &why);
    if (!fits)
      return fail(why);
    std::string matched;
    for (const auto& [column, value] : *fits)
      if (std::abs(value - radius * 2.0) < 0.005)
        matched = column;
    if (matched.empty()) {
      std::ostringstream no;
      no.precision(4);
      no << "this hole is " << radius * 2.0 << " mm across, which is not a "
         << request.size << " clearance —";
      for (const auto& [column, value] : *fits)
        no << " " << column << " " << value << ",";
      no.seekp(-1, std::ios_base::end);
      no << " are. Pass the size the hole IS for, or pick another rim.";
      return fail(no.str());
    }
    fit.size = request.size;
    fit.clearance = matched;
  } else {
    const auto thread = parts::ThreadForClearance(radius * 2.0, &why);
    if (!thread)
      return fail(why);
    fit.size = thread->first;
    fit.clearance = thread->second;
  }

  // A countersunk screw sinks its head INTO the part, so it needs a
  // countersunk hole and the rim picked has to be the COUNTERSINK's, not the
  // bore's. This was a blanket refusal until holes could describe themselves;
  // the three questions below are the ones that refusal was standing in for
  // (docs/parts/f1-loose-ends.md).
  if (screwProfile->lengthOverHead) {
    if (drilled == nullptr || drilled->kind != HoleKind::Countersink)
      return fail(screwProfile->designation +
                  " sinks its head into the part, so it needs a COUNTERSUNK "
                  "hole — this one is not, and the head would end up inside "
                  "solid material. Countersink the hole, or use a screw whose "
                  "head stands proud (iso4762, iso14579, iso7380, iso4017).");
    if (std::abs(radius * 2.0 - drilled->csDiameter) > 1e-3)
      return fail("pick the rim of the COUNTERSINK, not the bore — the head "
                  "sits in the cone, so " +
                  Mm(drilled->csDiameter) +
                  " across is the edge it bears on and " +
                  Mm(drilled->diameter) + " is the hole under it.");
    // Deep enough for the head? A countersink shallower than the head leaves
    // it standing proud of a surface that was machined to swallow it, which
    // is not something you can see at render distance.
    const auto dims = parts::Dimensions(screwProfile->stem, fit.size, &why);
    if (!dims)
      return fail(why);
    const auto csh = dims->find("csh_height");
    if (csh != dims->end() && drilled->csDepth < csh->second - 1e-6)
      return fail("the countersink is " + Mm(drilled->csDepth) +
                  " deep and an " + fit.size + " " + screwProfile->designation +
                  " head is " + Mm(csh->second) +
                  " — the head would stand proud of a surface cut to take it. "
                  "Deepen the countersink, or use a smaller screw.");
    if (request.washerUnderHead)
      return fail("a washer does not go under a countersunk head — the head "
                  "bears on the cone, not on the surface.");
  }

  // Down the axis, from the rim. The direction is chosen by which way there is
  // material to pass through: an axis has no sign, and picking the wrong one
  // would send the screw out into space.
  const double reach = 10000.0;
  std::vector<parts::StackSegment> stack = parts::TraverseAxis(
      placed, glm::dvec3(centre), glm::dvec3(-axis), radius, reach);
  if (stack.empty()) {
    axis = -axis; // an axis has no sign; take the way with material in it
    stack = parts::TraverseAxis(placed, glm::dvec3(centre), glm::dvec3(-axis),
                                radius, reach);
  }
  if (stack.empty())
    return fail("nothing lies along that hole's axis — the rim was picked on "
                "a surface feature rather than on a joint, so there is no "
                "stack for a fastener to span");

  // The decomposition. Gaps are SPANNED and named rather than silently added:
  // a gap is either an unmodelled gasket or two parts that were meant to touch
  // and do not, and the geometry cannot tell those apart. Saying so is what
  // makes the second kind visible; contact_report reports it independently.
  // The optional SECOND pick: where the joint ends, when the axis carries on
  // past it. Resolved the same way the rim was, then projected onto the axis
  // — a point off the line is a mis-pick and says so rather than silently
  // sizing the screw to its shadow.
  double truncate = 0.0; // 0 = span everything on the axis
  if (request.farEnd.element.IsSet()) {
    const assembly::FlatInstance* on = nullptr;
    std::optional<glm::vec3> local;
    for (const assembly::FlatInstance& inst : flat) {
      if (inst.part == nullptr || !inst.visible)
        continue;
      if (request.farEnd.occurrence != 0 &&
          inst.rootOccurrenceId != request.farEnd.occurrence)
        continue;
      if (const std::optional<glm::vec3> c = Measure::CentreOf(
              inst.part->GetBodies(), request.farEnd.element)) {
        on = &inst;
        local = c;
        break;
      }
    }
    if (!local)
      return fail("the far end you picked is not on any visible part — pick "
                  "the rim the fastener should stop at");
    const glm::vec3 end = glm::vec3(on->world * glm::vec4(*local, 1.0f));
    const glm::vec3 alongAxis = end - centre;
    truncate = glm::dot(alongAxis, -axis);
    const double offLine =
        glm::length(alongAxis - (-axis) * static_cast<float>(truncate));
    if (offLine > 1.0)
      return fail("that far end is " + Mm(offLine) +
                  " off this hole's axis — the second pick says where the "
                  "joint ENDS, so it has to be on the same line as the rim "
                  "you started from.");
    if (truncate <= 1e-3)
      return fail("that far end is on the head's side of the rim — it names "
                  "where the fastener stops, which has to be past where it "
                  "starts.");
  }

  // The bundle above measured PLATE THICKNESS. What stops the screw is a
  // different question, and needs the on-axis ray the bundle deliberately does
  // not cast: anything solid ON the axis is either a blind hole's floor or a
  // plate whose hole does not line up, and telling those apart is the
  // difference between an M6x8 and a bolt through solid metal.
  const std::vector<parts::StackSegment> onAxis = parts::TraverseAxis(
      placed, glm::dvec3(centre), glm::dvec3(-axis), 0.0, reach);
  // Where the joint ends, from the same helper `check_fasteners` uses, so the
  // two cannot drift apart (docs/agent-scale-followup-plan.md, W1).
  const parts::JointExtent extent = parts::JointEnd(stack, onAxis, truncate);
  fit.blind = extent.blind;
  // The summary is bounded by the JOINT, not by the far-end pick alone: in a
  // blind hole the screw reaches the floor, so listing the block's full
  // thickness beside a grip that stops at the floor made the decomposition
  // disagree with the number next to it ("grip 45: plate 20, base 60").
  const double counted = extent.end;

  std::ostringstream summary;
  summary.precision(4);
  double previousExit = 0.0;
  bool first = true;
  std::string beyond; // what the axis carries on into, past the second pick
  for (const parts::StackSegment& seg : stack) {
    // Past the second pick: named, but not counted. Listing it as part of the
    // joint made the summary's own numbers add up to something other than the
    // grip beside them — found the first time a real casing was bolted, where
    // "grip 65: ... DE rib 30 ..." totalled 95.
    if (seg.enter >= counted - 1e-6) {
      beyond += (beyond.empty() ? "" : ", ") + seg.part;
      continue;
    }
    if (!first && seg.enter - previousExit > 1e-3)
      summary << ", gap " << (seg.enter - previousExit);
    const double within = std::min(seg.exit, counted) - seg.enter;
    summary << (first ? "" : ", ") << seg.part << " " << within;
    if (seg.exit > counted + 1e-6)
      summary << " (of " << seg.thickness() << ")";
    previousExit = std::min(seg.exit, counted);
    first = false;
  }
  // What else is on the axis, and how thick. A washer under the head pushes
  // the head out by its own thickness, so it lengthens the screw — deriving
  // the length and THEN adding hardware is how a joint ends up one washer
  // short, which looks perfectly right in a viewport.
  double underHead = 0.0, underNut = 0.0, nutHeight = 0.0;
  if (request.washerUnderHead) {
    const auto h = CompanionDim(request.washer, parts::Kind::Washer, fit.size,
                                {"h", "h_max"}, &why);
    if (!h)
      return fail(why);
    underHead = *h;
  }
  if (!request.nut.empty()) {
    if (fit.blind)
      return fail("there is no far side to put a nut on — the axis stops "
                  "inside " +
                  stack.back().part +
                  ", which is a blind hole. Tap it and leave the nut off.");
    const auto m =
        CompanionDim(request.nut, parts::Kind::Nut, fit.size, {"m"}, &why);
    if (!m)
      return fail(why);
    nutHeight = *m;
    if (request.washerUnderNut) {
      const auto h = CompanionDim(request.washer, parts::Kind::Washer, fit.size,
                                  {"h", "h_max"}, &why);
      if (!h)
        return fail(why);
      underNut = *h;
    }
  } else if (request.washerUnderNut) {
    return fail("a washer under the nut needs a nut — ask for one (nut: "
                "\"iso4032\"), or drop the washer.");
  }

  std::optional<double> length;
  if (!fit.blind) {
    // Clear all the way: a through joint, and the screw spans the lot — the
    // plates, the washers at both ends, and enough thread to fill the nut.
    // "Enough" is the nut's own height, which is the shortest length at which
    // every one of its threads is engaged; a nut hanging off the last two is
    // the failure this rule exists to prevent.
    // Truncated at the second pick when there is one: everything between the
    // two is still in the stack, so the washers and the nut are still found —
    // only the far END moves in.
    fit.grip = underHead + extent.end + underNut + nutHeight;
    length = parts::SnapLength(request.standard, fit.size, fit.grip, &why);
  } else {
    const double blocked = extent.end;
    // Material ON the axis, right at the rim: this is not a hole at all. It
    // is the end of a SOLID — a boss, a pin, a shaft — whose end cap has a
    // circular edge like any bore does.
    //
    // Found by dogfooding the dock (docs/dogfooding.md): every circular edge
    // in a 49-part assembly was one of these, and the refusal a person got
    // was about plates and reaching past things, which never once said "that
    // is a solid". A circular edge is not a hole, and on a real model it
    // usually is not.
    if (blocked < 1e-6)
      return fail("that circular edge is the end of a SOLID, not a hole — the "
                  "axis runs straight into " +
                  onAxis.front().part +
                  " from the rim. A boss, a pin and a bore all have a circle "
                  "at the end; pick the rim of one a fastener can pass "
                  "through.");
    // Which plate the obstruction is in. The LAST one means a blind hole —
    // the screw threads into it. An EARLIER one means the axis runs into
    // material it should have passed through, which is a misalignment and a
    // refusal, because the alternative is a bolt drawn through solid metal.
    const parts::StackSegment* blocking = parts::SegmentAt(stack, blocked);
    if (blocking == nullptr)
      return fail("something solid sits on this hole's axis " + Mm(blocked) +
                  " out, clear of every plate the hole passes through — a "
                  "fastener cannot reach past it");

    // Blocked at a part's ENTRY FACE means that part has no hole on this axis
    // at all, whether or not it is the last in the stack. That is a
    // misalignment, and it is worth separating from a blind hole: told "the
    // hole is 0 mm deep" a person goes looking for a drilling depth, when what
    // is wrong is that two holes do not line up.
    const double into = blocked - blocking->enter;
    if (into < 1e-3)
      return fail("there is no hole on this axis in " + blocking->part +
                  " — the axis meets its face " + Mm(blocked) +
                  " out and stops. Its hole is not lined up with this one, so "
                  "a fastener here would pass through solid material.");
    // The last part IN THE JOINT, which is not `stack.back()` when a far end
    // was picked. The stack is deliberately untruncated — everything between
    // the two picks is still traversed, so the washers and the nut are still
    // found — but this question is "does the blind hole sit at the END of the
    // joint", and a far end is precisely the caller saying where that is.
    //
    // Reading stack.back() raw refused a perfectly ordinary joint: an
    // air-cooled barrel's head bolts stop in a blind hole 30 mm down, and
    // their axis carries on 158 mm further into a crank web that the bolt
    // does not touch and was told not to (generators/aircooled_engine.py).
    const parts::StackSegment* jointEnd = &stack.back();
    if (truncate > 0.0)
      for (const parts::StackSegment& seg : stack)
        if (seg.enter < truncate - 1e-6)
          jointEnd = &seg;
    if (blocking->partId != jointEnd->partId)
      return fail("the axis runs into " + blocking->part + " " + Mm(into) +
                  " in — its hole does not go all the way through, so a "
                  "fastener cannot reach the parts beyond it." +
                  (truncate > 0.0
                       ? ""
                       : std::string(" If the bolt is not meant to reach "
                                     "them, pick a `far_end` on the part it "
                                     "does stop in.")));

    // A blind hole bounds the length from ABOVE: the screw must not bottom
    // out. Take the longest stocked length that fits, which is also the most
    // thread engaged.
    // A blind hole a screw can bite in is a TAPPED one. A clearance hole of
    // the same depth looks identical and holds nothing — the plan promised
    // this refusal and could not make it until a hole could say which it is.
    if (drilled != nullptr && !drilled->fitThread.empty() && !tapped)
      return fail("the blind hole in " + blocking->part + " is a " +
                  fit.clearance + " CLEARANCE hole for " + fit.size +
                  ", not a tapped one — a machine screw has nothing to bite "
                  "in it. Tap it, or put a nut on the far side.");
    const double engagement = *parts::NominalDiameter(fit.size); // ~1x d
    fit.grip = underHead + blocked;
    if (into < engagement)
      return fail("the hole is only " + Mm(into) + " deep in " +
                  blocking->part + ", which is less than the " +
                  Mm(engagement) + " of thread an " + fit.size +
                  " needs to hold. Drill deeper, or use a smaller size.");
    length = parts::SnapLengthDown(request.standard, fit.size, fit.grip, &why);
  }
  if (!length)
    return fail(why + " (the stack is " + summary.str() + ")");
  fit.length = *length;

  std::ostringstream head;
  head.precision(4);
  head << fit.size << "x" << fit.length << " — grip " << fit.grip << ": ";
  if (underHead > 0.0)
    head << "washer " << underHead << ", ";
  head << summary.str();
  if (underNut > 0.0)
    head << ", washer " << underNut;
  if (nutHeight > 0.0)
    head << ", nut " << nutHeight;
  // Naming what the joint deliberately stops short of is the confirmation
  // that the second pick did what you meant by it.
  if (!beyond.empty())
    head << "; the axis carries on into " << beyond;
  fit.summary = head.str();

  // WHERE each piece goes, worked out whether or not anything is placed.
  // Every fastener is built with its bearing face on z = 0 and its body
  // below, so all four of these are the same two questions: where is the
  // face, and which way is "away from the material".
  const glm::vec3 into = -axis;          // down the hole
  const glm::quat down = AlignZTo(into); // head end: body points back out
  const glm::quat up = AlignZTo(axis); // far end: body points out the far side
  const glm::vec3 exit = centre + into * static_cast<float>(extent.end);

  // Which occurrence each piece rides on. The head end bears on the part the
  // rim was picked in; the far end on the last part the JOINT reaches.
  //
  // "The joint", not "the axis": with a second pick the two differ, and the
  // difference is the whole reason the second pick exists. Measured on the
  // alternator, where the bolt circle runs on into a cast rib the bolts stop
  // short of — taking the axis's last segment attached the nut to the RIB,
  // which is a part it never touches, so the nut followed the wrong casting
  // and the joint made no mate at all (there is no face on the rib at the
  // flange's back).
  const uint64_t headOn = chosen->inst->rootOccurrenceId;
  const double joinEnds =
      truncate > 0.0 ? truncate : std::numeric_limits<double>::max();
  uint64_t farOn = headOn;
  double farthest = 0.0;
  for (const parts::StackSegment& seg : stack)
    if (seg.exit > farthest && seg.exit <= joinEnds + 1e-3) {
      farthest = seg.exit;
      farOn = seg.occurrence;
    }

  fit.plan.push_back({request.standard, fit.size, fit.length,
                      centre - into * static_cast<float>(underHead), down,
                      headOn});
  if (underHead > 0.0)
    fit.plan.push_back({request.washer, fit.size, 0.0, centre, down, headOn});
  if (underNut > 0.0)
    fit.plan.push_back({request.washer, fit.size, 0.0, exit, up, farOn});
  if (!request.nut.empty())
    fit.plan.push_back({request.nut, fit.size, 0.0,
                        exit + axis * static_cast<float>(-underNut), up,
                        farOn});

  if (request.dryRun) {
    fit.ok = true;
    return fit;
  }

  // The commit just walks the plan. It used to recompute the same arithmetic
  // a second time, which is how a preview and a placement come to disagree.
  for (size_t i = 0; i < fit.plan.size(); i++) {
    const PlannedPart& part = fit.plan[i];
    // The drive is the SCREW's — it is first — because a washer has no recess
    // and handing it one is refused by the standard that does not have it.
    const uint64_t id =
        AddFastener(state, part.standard, part.size, part.length,
                    i == 0 ? request.drive : std::string{}, part.position,
                    part.orientation, &fit.why, part.ridesOn);
    if (id == 0)
      return fail(fit.why);
    fit.placed.push_back(id);
    for (const Occurrence& o : state.assembly->GetOccurrences())
      if (o.id == id)
        fit.parts.push_back(o.name);
  }
  fit.occurrence = fit.placed.front();

  // And the joint itself. Placing the hardware is not the same as holding the
  // parts together: the screw rides its plate, the nut rides the far one, and
  // until this the parts were still free of each other — drag one and the
  // bolt tore in half.
  //
  // Every INTERFACE the joint spans, not just its two ends. Three plates
  // bolted together are two interfaces, and mating only the outer two leaves
  // the middle one floating — the alternator's stator, clamped between two
  // housings and joined to neither, which is the reported bug one layer in.
  // Consecutive pairs are a chain, so this stays a tree the solver can place.
  //
  // Once per bolt AND per interface. Every bolt across an interface gets its
  // own mate, anchored at ITS hole — so deleting one bolt leaves the parts
  // held by the others, which is what a bolt pattern means.
  //
  // This used to stop at the first bolt, because a mate between an
  // already-connected pair closes a loop and A3 reported a loop as
  // over-constrained rather than solving it. A6 closes loops, and a bolt
  // pattern is the redundant-BUT-CONSISTENT case it exists to handle: the
  // extra mates are satisfied, add no constraint, and `solve_assembly` names
  // them in `redundant_mates` instead of erroring
  // (docs/assemblies/a6-loop-solver-plan.md).
  {
    const auto instFor = [&](uint64_t occ) -> const assembly::FlatInstance* {
      for (const assembly::FlatInstance& inst : flat)
        if (inst.rootOccurrenceId == occ && inst.part != nullptr)
          return &inst;
      return nullptr;
    };
    // The parts the joint actually spans, in axis order — hardware excluded
    // (a washer from an earlier bolt down this same hole is not a part being
    // clamped) and everything past a second pick's cut left out, because
    // that is precisely what the second pick said.
    std::vector<const parts::StackSegment*> spanned;
    for (const parts::StackSegment& seg : stack) {
      if (seg.exit > joinEnds + 1e-3)
        continue;
      if (SoleFastenerOf(PartDocById(state, seg.partId)) != nullptr)
        continue;
      if (!spanned.empty() && spanned.back()->occurrence == seg.occurrence)
        continue;
      spanned.push_back(&seg);
    }
    for (size_t i = 0; i + 1 < spanned.size(); i++) {
      const parts::StackSegment& near = *spanned[i];
      const parts::StackSegment& next = *spanned[i + 1];
      if (near.occurrence == next.occurrence)
        continue; // the same part twice down the axis is not an interface
      const assembly::FlatInstance* a = instFor(near.occurrence);
      const assembly::FlatInstance* b = instFor(next.occurrence);
      if (a == nullptr || b == nullptr)
        continue;
      if (const uint64_t id = JoinAcrossInterface(state, *a, near.exit, *b,
                                                  next.enter, centre, axis))
        fit.mates.push_back(id);
    }
  }

  fit.ok = true;
  return fit;
}

} // namespace ui
