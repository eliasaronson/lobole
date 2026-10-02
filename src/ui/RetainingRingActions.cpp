#include "ui/RetainingRingActions.h"

#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/features/RetainingRingFeature.h"
#include "core/features/RetainingRingGrooveFeature.h"
#include "core/parts/RetainingRingTable.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h" // kStandardPartsFolder
#include "ui/DocumentSlots.h"

#include <glm/gtc/quaternion.hpp>

#include <sstream>

namespace ui {

uint64_t AddRetainingRing(AppState& state, const std::string& standard,
                          double size, const glm::dvec3& position,
                          const glm::dquat& orientation, std::string* error) {
  const auto fail = [&](const std::string& why) -> uint64_t {
    if (error)
      *error = why;
    return 0;
  };
  if (state.assembly == nullptr)
    return fail("a retaining ring is a bought part placed in an ASSEMBLY, "
                "not geometry in a part — on a part tab it would be machined "
                "by CAM and counted as the part's own mass. Switch to an "
                "assembly tab (set_active_part) and place it there; cut its "
                "groove in the part with a RetainingRingGroove feature.");
  // Refused BEFORE a tab exists for it: a refusal that arrives after the
  // part was created leaves the tab behind.
  parts::RetainingRingStandard which{};
  if (!parts::RetainingRingStandardFromString(standard, which))
    return fail("unknown retaining ring standard \"" + standard +
                "\" — accepted: " + parts::KnownRetainingRingStandards());
  std::string why;
  const parts::RetainingRingRow* row =
      parts::FindRetainingRing(which, size, &why);
  if (row == nullptr)
    return fail(why);

  const std::string name = parts::RetainingRingDesignation(which, *row);
  uint64_t partId = PartIdByName(state, name);
  if (partId == 0) {
    const int assemblyTab = state.activePart;
    AddPart(state, name);
    PartSlot& slot = state.parts[static_cast<size_t>(state.activePart)];
    slot.folder = kStandardPartsFolder;
    CreateFolder(state, kStandardPartsFolder);
    partId = slot.id;
    state.document->AddFeature(std::make_unique<RetainingRingFeature>(
        name, parts::RetainingRingStandardName(which), row->d1));
    if (assemblyTab >= 0 && assemblyTab < static_cast<int>(state.parts.size()))
      SetActivePart(state, static_cast<size_t>(assemblyTab));
  }
  if (state.assembly == nullptr)
    return fail("could not return to the assembly after creating " + name);

  Occurrence occ;
  occ.partId = partId;
  occ.name = name;
  occ.position = position;
  occ.orientation = orientation;
  return state.assembly->AddOccurrence(occ);
}

namespace {

// The current name of the face a groove feature named `<id>/<role>`: itself,
// or the one a later feature's step extended (";M7").
std::string GrooveFace(const Body& body, uint64_t groove, const char* role) {
  const std::string seed = std::to_string(groove) + "/" + role;
  for (const auto& [index, name] : body.elementMap.names)
    if (name == seed || name.rfind(seed + ";", 0) == 0)
      return name;
  return {};
}

} // namespace

RetainingRingFit FitRetainingRing(AppState& state, const RimPick& pick,
                                  bool dryRun) {
  RetainingRingFit fit;
  const auto fail = [&](const std::string& why) {
    fit.ok = false;
    fit.why = why;
    return fit;
  };
  if (state.assembly == nullptr)
    return fail("a retaining ring goes in an ASSEMBLY — switch to the "
                "assembly tab and name a face of the groove it goes in");
  // Which groove: the feature a groove face's name starts with.
  const std::string& element = pick.element.name;
  const size_t slash = element.find('/');
  uint64_t grooveId = 0;
  if (slash != std::string::npos && slash > 0)
    try {
      grooveId = std::stoull(element.substr(0, slash));
    } catch (const std::exception&) {
      grooveId = 0;
    }

  struct Found {
    const assembly::FlatInstance* inst;
    const Body* body;
    const RingGrip* grip;
  };
  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);
  std::vector<Found> found;
  for (const assembly::FlatInstance& inst : flat) {
    if (inst.part == nullptr || !inst.visible)
      continue;
    if (pick.occurrence != 0 &&
        (inst.rootOccurrenceId != pick.occurrence ||
         (!pick.innerPath.empty() && inst.innerIds != pick.innerPath)))
      continue;
    for (const Body& body : inst.part->GetBodies()) {
      if (body.createdBy != pick.element.featureId)
        continue;
      for (const RingGrip& grip : body.ringGrips)
        if (grip.groove && grip.featureId == grooveId)
          found.push_back({&inst, &body, &grip});
    }
  }
  if (found.empty())
    return fail("\"" + element +
                "\" is not a face of a retaining ring groove on a placed part "
                "— name the groove's floor or a flank (a RetainingRingGroove "
                "feature's `groove_floor`, `groove_flank_a` or "
                "`groove_flank_b`), with the `occurrence` it is on");
  if (found.size() > 1) {
    std::string names;
    for (const Found& f : found)
      names += (names.empty() ? "" : ", ") + f.inst->path;
    return fail("the groove is on several placements (" + names +
                ") — name one with `occurrence` (and `inner_path`)");
  }
  const Found& at = found.front();
  parts::RetainingRingStandard which{};
  if (!parts::RetainingRingStandardFromString(at.grip->standard, which))
    return fail("the groove names an unknown standard \"" + at.grip->standard +
                "\"");
  std::string why;
  const parts::RetainingRingRow* row =
      parts::FindRetainingRing(which, at.grip->size, &why);
  if (row == nullptr)
    return fail(why);
  fit.standard = at.grip->standard;
  fit.size = row->d1;
  fit.designation = parts::RetainingRingDesignation(which, *row);
  fit.parent = at.inst->rootOccurrenceId;
  fit.groove = grooveId;

  AttachSeat seat;
  seat.face = {
      at.body->createdBy,
      GrooveFace(*at.body, grooveId, RetainingRingGrooveFeature::kFlankA)};
  seat.axis = at.grip->face;
  seat.innerPath = at.inst->innerIds;
  if (seat.face.name.empty())
    return fail("the groove's near flank (groove_flank_a) is not on the "
                "part any more — a later feature removed it");

  // One ring to a groove.
  for (const Occurrence& o : state.assembly->GetOccurrences())
    if (o.attachedTo == fit.parent &&
        o.attachSeat.innerPath == seat.innerPath &&
        o.attachSeat.face.featureId == seat.face.featureId &&
        o.attachSeat.face.name == seat.face.name)
      return fail("that groove already holds " + o.name + " (occurrence " +
                  std::to_string(o.id) + ")");

  const Occurrence* parent = state.assembly->FindById(fit.parent);
  if (parent == nullptr)
    return fail("the grooved placement is gone");
  const auto frame = assembly::SeatFrame(seat, *parent, *state.assembly,
                                         PartResolverFor(state),
                                         AssemblyResolverFor(state), &why);
  if (!frame)
    return fail("the groove's seat does not resolve: " + why);
  // The seat's Z is the flank's outward normal, into the groove; the ring's
  // own +Z runs from its side A, so it stands in the groove off the flank.
  const glm::dmat4 world = OccurrenceMatrix(*parent) * *frame;
  fit.position = glm::dvec3(world[3]);
  fit.orientation = glm::normalize(glm::quat_cast(glm::dmat3(world)));

  std::ostringstream s;
  s.precision(4);
  s << fit.designation << " in the groove on " << at.inst->path
    << ": side A on its near flank, the groove Ø" << row->d2 << " x " << row->m
    << " (the ring " << row->s << " thick, Ø" << row->d3 << " free)";
  fit.summary = s.str();
  if (dryRun) {
    fit.ok = true;
    return fit;
  }

  std::string error;
  fit.occurrence = AddRetainingRing(state, fit.standard, fit.size, fit.position,
                                    fit.orientation, &error);
  if (fit.occurrence == 0)
    return fail(error);
  Occurrence* ring = state.assembly->FindById(fit.occurrence);
  if (ring == nullptr)
    return fail("the ring was added and is not in the assembly");
  ring->attachedTo = fit.parent;
  if (!assembly::SeatRider(*state.assembly, *ring, seat, PartResolverFor(state),
                           AssemblyResolverFor(state), &why)) {
    state.assembly->RemoveOccurrence(fit.occurrence);
    fit.occurrence = 0;
    return fail("the ring could not be seated in its groove: " + why);
  }
  fit.ok = true;
  return fit;
}

} // namespace ui
