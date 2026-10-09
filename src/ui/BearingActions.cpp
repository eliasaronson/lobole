// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/BearingActions.h"

#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/PrintAllowance.h"
#include "core/catalog/Catalog.h"
#include "core/features/BearingFeature.h"
#include "core/features/RetainingRingFeature.h"
#include "core/parts/BearingSeat.h"
#include "core/parts/RetainingRingTable.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <sstream>

// Bearings as generated standard parts (docs/parts/README.md#bearings): the
// same shape as FastenerActions — one part per spec, placed as an occurrence
// — with a seat instead of a hole.

namespace ui {

namespace {

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

std::string Dims(const parts::BearingRow& r) {
  return std::string(r.designation) + " (" + Mm(r.d) + "x" + Mm(r.D) + "x" +
         Mm(r.B) + ")";
}

// The diameter a seat comes out at. A seat on a PRINTED part is modelled at
// its size plus the process allowance (a PETG housing for a Ø32 outside is
// modelled Ø32.3, docs/tolerances/README.md#printed-parts), so the bearing it
// takes and the size it is checked against are what it PRINTS at; anything
// else is read as modelled. `note` (optional) says so when it applies.
double SeatDiameter(const Document* part, const parts::Seat& seat,
                    std::string* note = nullptr) {
  if (part == nullptr || !part->PrintProcess())
    return seat.diameter;
  const printfit::Process& proc = *part->PrintProcess();
  std::string why;
  const auto a = printfit::ForModelled(
      proc, seat.shaft ? printfit::Printed::Shaft : printfit::Printed::Hole,
      seat.diameter, &why);
  if (!a)
    return seat.diameter;
  if (note != nullptr)
    *note = "modelled Ø" + Mm(seat.diameter) + ", prints Ø" + Mm(a->basicMm) +
            " (" + proc.process + " " + proc.material + ")";
  return a->basicMm;
}

// +Z (a bearing's own axis, side A to side B) rotated onto `to`.
glm::dquat AlignZTo(const glm::dvec3& to) {
  const glm::dvec3 from(0.0, 0.0, 1.0);
  const double dot = glm::dot(from, to);
  if (dot < -0.999999)
    return glm::dquat(0.0, 1.0, 0.0, 0.0); // 180 degrees about X
  if (dot > 0.999999)
    return glm::dquat(1.0, 0.0, 0.0, 0.0);
  const glm::dvec3 c = glm::cross(from, to);
  return glm::normalize(glm::dquat(1.0 + dot, c.x, c.y, c.z));
}

// A fastened mate between two faces in the pose they are ALREADY in: the
// degrees of freedom measured through the solver's own connector frames, so
// the solve that follows moves nothing. The fastener's JoinAcrossInterface
// does the same for a bolted interface. 0 when the frames are not square to
// one axis — no mate is better than one that moves the part it records.
uint64_t AddMateAsPlaced(AppState& state, Mate mate) {
  const assembly::PartResolver resolve = PartResolverFor(state);
  const assembly::AssemblyResolver asmResolve = AssemblyResolverFor(state);
  const auto fa = assembly::ConnectorLocalFrame(mate.a, *state.assembly,
                                                resolve, nullptr, asmResolve);
  const auto fb = assembly::ConnectorLocalFrame(mate.b, *state.assembly,
                                                resolve, nullptr, asmResolve);
  const Occurrence* occA = state.assembly->FindById(mate.a.occurrenceId);
  const Occurrence* occB = state.assembly->FindById(mate.b.occurrenceId);
  if (!fa || !fb || occA == nullptr || occB == nullptr)
    return 0;
  const glm::dmat4 wa = OccurrenceMatrix(*occA) * *fa;
  const glm::dmat4 wb = OccurrenceMatrix(*occB) * *fb;
  const glm::dmat4 rel = glm::inverse(wa) * wb;
  mate.flipped = rel[2][2] < 0.0;
  mate.rotation = std::atan2(rel[0][1], rel[0][0]);
  mate.dx = rel[3][0];
  mate.dy = rel[3][1];
  mate.offset = rel[3][2];
  glm::dmat4 adjust(1.0);
  adjust = glm::translate(adjust, glm::dvec3(mate.dx, mate.dy, mate.offset));
  adjust = glm::rotate(adjust, mate.rotation, glm::dvec3(0.0, 0.0, 1.0));
  if (mate.flipped)
    adjust = glm::rotate(adjust, glm::pi<double>(), glm::dvec3(1.0, 0.0, 0.0));
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      if (std::abs(adjust[c][r] - rel[c][r]) > 1e-3)
        return 0;
  return state.assembly->AddMate(mate);
}

// The BearingFeature an occurrence places, or null.
const BearingFeature* BearingOn(AppState& state, const Occurrence& occ) {
  return SoleBearingOf(OccurrencePartDoc(state, occ));
}

} // namespace

uint64_t AddBearing(AppState& state, const std::string& designation,
                    const glm::dvec3& position, const glm::dquat& orientation,
                    std::string* error) {
  const auto fail = [&](const std::string& why) -> uint64_t {
    if (error)
      *error = why;
    return 0;
  };
  if (state.assembly == nullptr)
    return fail("a bearing is a bought part placed in an ASSEMBLY, not "
                "geometry in a part — on a part tab it would be machined by "
                "CAM and counted as the part's own mass. Switch to an "
                "assembly tab (set_active_part) and place it there.");
  // Refused BEFORE a tab exists for it: a refusal that arrives after the
  // part was created leaves the tab behind.
  std::string why;
  const parts::BearingRow* row = parts::FindBearing(designation, &why);
  if (row == nullptr)
    return fail(why);

  const std::string name = std::string("ISO 15 ") + row->designation;
  uint64_t partId = PartIdByName(state, name);
  if (partId == 0) {
    const int assemblyTab = state.activePart;
    AddPart(state, name);
    PartSlot& slot = state.parts[static_cast<size_t>(state.activePart)];
    slot.folder = kStandardPartsFolder;
    CreateFolder(state, kStandardPartsFolder);
    partId = slot.id;
    state.document->AddFeature(
        std::make_unique<BearingFeature>(name, row->designation));
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

BearingFit FitBearing(AppState& state, const RimPick& pick,
                      const BearingRequest& request) {
  BearingFit fit;
  const auto fail = [&](const std::string& why) -> BearingFit {
    fit.ok = false;
    fit.why = why;
    return fit;
  };
  if (state.assembly == nullptr)
    return fail("a bearing goes in an ASSEMBLY — switch to the assembly tab "
                "and pick the seat it goes on");

  // The seat, on every placement that answers to the pick — an element name
  // is shared by every placement of a part, so more than one is refused
  // naming them rather than resolved to the first (as fit_fastener does).
  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);
  struct On {
    const assembly::FlatInstance* inst;
    parts::Seat seat;
  };
  std::vector<On> found;
  std::string seatWhy;
  for (const assembly::FlatInstance& inst : flat) {
    if (inst.part == nullptr || !inst.visible)
      continue;
    const auto& bodies = inst.part->GetBodies();
    if (std::none_of(bodies.begin(), bodies.end(), [&](const Body& b) {
          return b.createdBy == pick.element.featureId;
        }))
      continue;
    if (pick.occurrence != 0 &&
        (inst.rootOccurrenceId != pick.occurrence ||
         (!pick.innerPath.empty() && inst.innerIds != pick.innerPath)))
      continue;
    if (auto seat = parts::SeatOf(bodies, pick.element, &seatWhy))
      found.push_back({&inst, std::move(*seat)});
  }
  if (found.empty())
    return fail(!seatWhy.empty()
                    ? seatWhy
                    : "that pick is on no placed part" +
                          std::string(pick.occurrence != 0
                                          ? " in occurrence " +
                                                std::to_string(pick.occurrence)
                                          : "") +
                          " — pick the shaft or bore the bearing goes on");
  if (found.size() > 1) {
    std::string where;
    for (const On& o : found)
      where += (where.empty() ? "" : ", ") + o.inst->path + " (occurrence " +
               std::to_string(o.inst->rootOccurrenceId) + ")";
    return fail("that element is on " + std::to_string(found.size()) +
                " placements — " + where +
                ". Say which occurrence you mean; a pick in the viewport "
                "carries it.");
  }
  const assembly::FlatInstance& inst = *found.front().inst;
  const parts::Seat& seat = found.front().seat;
  std::string printedNote;
  const double seatDia = SeatDiameter(inst.part, seat, &printedNote);
  fit.shaft = seat.shaft;
  fit.seatDiameter = seatDia;

  // Which end the bearing sits at. A rim names it; a face has two, and the
  // one with a shoulder is the one a bearing is pushed up against.
  const parts::SeatEnd* end = nullptr;
  if (seat.ends.size() == 1) {
    end = &seat.ends.front();
  } else {
    std::vector<const parts::SeatEnd*> shouldered;
    for (const parts::SeatEnd& e : seat.ends)
      if (e.shoulder)
        shouldered.push_back(&e);
    if (shouldered.size() == 1)
      end = shouldered.front();
    else
      return fail(std::string(shouldered.empty()
                                  ? "nothing on this seat stops a bearing — "
                                    "neither end has a shoulder"
                                  : "this seat has a shoulder at BOTH ends") +
                  ", so the face alone does not say where the bearing sits. "
                  "Pick the rim at the end it goes against.");
  }
  fit.shoulder = end->shoulder;

  // Which bearing. The diameter narrows it to a handful; the caller says
  // which of those, or the handful is the answer.
  const std::vector<const parts::BearingRow*> rows =
      seat.shaft ? parts::BearingsForBore(seatDia)
                 : parts::BearingsForOutside(seatDia);
  for (const parts::BearingRow* r : rows)
    fit.candidates.push_back(Dims(*r));
  const std::string what =
      (seat.shaft ? "a Ø" + Mm(seatDia) + " shaft"
                  : "a Ø" + Mm(seatDia) + " housing bore") +
      (printedNote.empty() ? "" : " (" + printedNote + ")");
  if (rows.empty())
    return fail(what + " takes no bearing this build carries — ISO 15 " +
                std::string(seat.shaft ? "bores" : "outside diameters") +
                " in the table are whole sizes (series " +
                parts::KnownBearingSeries() +
                "). Model the seat at the bearing's size; a fit is a "
                "tolerance class on it (check_fits), not a different "
                "diameter.");
  const parts::BearingRow* row = nullptr;
  if (!request.designation.empty()) {
    std::string why;
    const parts::BearingRow* named =
        parts::FindBearing(request.designation, &why);
    if (named == nullptr)
      return fail(why);
    if (std::find(rows.begin(), rows.end(), named) == rows.end()) {
      std::string list;
      for (const std::string& c : fit.candidates)
        list += (list.empty() ? "" : ", ") + c;
      return fail("a " + Dims(*named) + " has a " +
                  (seat.shaft ? "Ø" + Mm(named->d) + " bore"
                              : "Ø" + Mm(named->D) + " outside") +
                  ", and this is " + what + ". It takes " + list + ".");
    }
    row = named;
  } else {
    std::vector<const parts::BearingRow*> inSeries;
    for (const parts::BearingRow* r : rows)
      if (request.series.empty() || request.series == r->series)
        inSeries.push_back(r);
    if (inSeries.size() == 1) {
      row = inSeries.front();
    } else {
      std::string list;
      for (const std::string& c : fit.candidates)
        list += (list.empty() ? "" : ", ") + c;
      if (inSeries.empty())
        return fail("series \"" + request.series + "\" has no bearing for " +
                    what + " — it takes " + list);
      return fail(what + " takes " + list +
                  ". Say which (designation, or series).");
    }
  }
  fit.designation = row->designation;

  // Where: side A on the shoulder plane, the axis along the seat.
  const glm::dvec3 origin =
      glm::dvec3(inst.world * glm::dvec4(glm::dvec3(end->origin), 1.0));
  const glm::dvec3 along = glm::normalize(
      glm::dvec3(inst.world * glm::dvec4(glm::dvec3(end->along), 0.0)));
  fit.position = origin;
  fit.orientation = AlignZTo(along);
  // Under a micron is the seat's own rounding (a sketch plane is single
  // precision), not an overhang: a 9 mm counterbore for a 9 mm 6002 read
  // "overhangs the seat by 4.578e-06 mm".
  fit.overhang = row->B - end->length;
  if (fit.overhang < 1e-3)
    fit.overhang = 0.0;

  std::ostringstream s;
  s << Dims(*row) << (seat.shaft ? " on " : " in ") << inst.path << "'s Ø"
    << Mm(seatDia) << (seat.shaft ? " journal" : " bore");
  if (!printedNote.empty())
    s << " (" << printedNote << ")";
  if (end->shoulder) {
    s << ", against its Ø" << Mm(end->shoulderDiameter) << " shoulder";
    if (end->filletRadius > 0.0)
      s << " (fillet R" << Mm(end->filletRadius) << ")";
  } else {
    s << ", flush with the end of the seat — nothing locates it axially";
  }
  if (fit.overhang > 1e-6)
    s << "; overhangs the seat by " << Mm(fit.overhang) << " mm";
  fit.summary = s.str();

  if (request.dryRun) {
    fit.ok = true;
    return fit;
  }

  std::string error;
  fit.occurrence = AddBearing(state, row->designation, fit.position,
                              fit.orientation, &error);
  if (fit.occurrence == 0)
    return fail(error);
  const Occurrence* placed = state.assembly->FindById(fit.occurrence);
  const BearingFeature* bearing =
      placed != nullptr ? BearingOn(state, *placed) : nullptr;
  if (bearing != nullptr) {
    Mate mate;
    mate.type = MateType::Fastened;
    mate.name = "Bearing seat";
    mate.a.occurrenceId = inst.rootOccurrenceId;
    mate.a.innerPath = inst.innerIds;
    mate.a.anchor.face = seat.face;
    mate.b.occurrenceId = fit.occurrence;
    mate.b.anchor.face = {bearing->GetId(),
                          bearing->FaceName(seat.shaft
                                                ? BearingFeature::kBore
                                                : BearingFeature::kOutside)};
    fit.mate = AddMateAsPlaced(state, mate);
  }
  if (fit.mate == 0)
    fit.summary += "; NOT mated — the seat's frame could not be matched";

  // The other ring, read the way check_seat reads it: through the seat it
  // sits on, on the bearing's axis, though no mate names it.
  const std::string otherRing = seat.shaft ? "outer" : "inner";
  const std::string otherKey =
      seat.shaft ? "housing_diameter" : "shaft_diameter";
  try {
    const nlohmann::json report = CheckSeat(state, fit.occurrence, nullptr, "");
    nlohmann::json other = {{"ring", otherRing}};
    for (const nlohmann::json& s : report.at("seats"))
      if (s.at("ring") == otherRing && s.contains("face"))
        other["seat"] = s;
    for (const nlohmann::json& c : report.at("checks"))
      if (c.at("characteristic") == otherKey)
        other["diameter_check"] = c;
    fit.otherRing = std::move(other);
  } catch (const std::exception& e) {
    fit.otherRing = {{"ring", otherRing}, {"error", e.what()}};
  }
  const bool otherFound = fit.otherRing.contains("seat");
  // Asked to mate the other ring and nothing sits under it: said in THIS
  // answer (other_mate 0, the summary), not left for a check_seat to find.
  // The placed bearing and its seat mate stand; they are what was asked
  // first, and undo takes them back.
  if (request.mateOther && bearing != nullptr && !otherFound)
    fit.summary += "; the " + otherRing +
                   " ring is NOT mated - nothing on the bearing's axis is a "
                   "seat for it";
  if (request.mateOther && bearing != nullptr && otherFound) {
    const nlohmann::json& os = fit.otherRing.at("seat");
    Mate mate;
    mate.type = MateType::Revolute;
    mate.name = "Bearing " + otherRing + " ring";
    mate.a.occurrenceId = os.at("occurrence").get<uint64_t>();
    mate.a.innerPath = os.at("inner_path").get<std::vector<uint64_t>>();
    mate.a.anchor.face = {os.at("feature_id").get<uint64_t>(),
                          os.at("face").get<std::string>()};
    mate.b.occurrenceId = fit.occurrence;
    mate.b.anchor.face = {bearing->GetId(),
                          bearing->FaceName(seat.shaft
                                                ? BearingFeature::kOutside
                                                : BearingFeature::kBore)};
    fit.otherMate = AddMateAsPlaced(state, mate);
    if (fit.otherMate == 0)
      fit.summary += "; the " + otherRing +
                     " ring is NOT mated — its seat's frame could not be "
                     "matched";
  }
  fit.ok = true;
  return fit;
}

std::optional<BearingRing> BearingRingOf(const Document* part,
                                         const ElementRef& face) {
  const BearingFeature* bearing = SoleBearingOf(part);
  if (bearing == nullptr || face.featureId != bearing->GetId())
    return std::nullopt;
  const std::string role = BearingFeature::RoleOf(face.name);
  const bool bore = role == BearingFeature::kBore;
  if (!bore && role != BearingFeature::kOutside)
    return std::nullopt;
  const parts::BearingRow* row = parts::FindBearing(bearing->designation);
  if (row == nullptr)
    return std::nullopt;
  const auto band =
      bore ? parts::BoreTolerance(row->d) : parts::OutsideTolerance(row->D);
  if (!band)
    return std::nullopt;
  return BearingRing{row->designation, bore, bore ? row->d : row->D, *band};
}

nlohmann::json CheckSeat(AppState& state, uint64_t occurrence,
                         const nlohmann::json* catalogEntry,
                         const std::string& catalogDesignation) {
  using json = nlohmann::json;
  if (state.assembly == nullptr)
    throw std::runtime_error("check_seat reads a bearing placed in an "
                             "ASSEMBLY — switch to the assembly tab");
  const Occurrence* occ = state.assembly->FindById(occurrence);
  if (occ == nullptr)
    throw std::runtime_error("no occurrence " + std::to_string(occurrence) +
                             " in this assembly (list_occurrences)");
  const BearingFeature* bearing = BearingOn(state, *occ);
  if (bearing == nullptr)
    throw std::runtime_error(
        "occurrence " + std::to_string(occurrence) + " (" + occ->name +
        ") is not a placed bearing — place one with fit_bearing or "
        "add_bearing; for a bearing modelled by hand, pass the observations "
        "to catalog_requirements instead");
  const parts::BearingRow* row = parts::FindBearing(bearing->designation);
  if (row == nullptr)
    throw std::runtime_error(bearing->GetError());

  // The supplier's abutment limits (docs/parts/README.md#abutment-limits);
  // null for a designation the catalogue does not list.
  const parts::BearingAbutment* abut = parts::AbutmentOf(row->designation);
  const std::string source = parts::AbutmentSource();

  json out = {{"occurrence", occurrence},
              {"designation", row->designation},
              {"d", row->d},
              {"D", row->D},
              {"B", row->B},
              {"r_min", row->rMin}};
  if (abut != nullptr) {
    json a = {{"source", source},
              {"d_a_min", abut->daMin},
              {"D_a_max", abut->DaMax},
              {"r_a_max", abut->raMax}};
    a["d_a_max"] = parts::BearingAbutment::Has(abut->daMax) ? json(abut->daMax)
                                                            : json(nullptr);
    a["variant"] = abut->variant != nullptr ? json(abut->variant) : json();
    out["abutment"] = std::move(a);
  } else {
    out["abutment"] = nullptr;
  }

  // The bearing's two side planes, in the world.
  const glm::dmat4 bw = OccurrenceMatrix(*occ);
  const glm::dvec3 sideA = glm::dvec3(bw * glm::dvec4(0, 0, 0, 1));
  const glm::dvec3 sideB = glm::dvec3(bw * glm::dvec4(0, 0, row->B, 1));

  const std::vector<assembly::FlatInstance> flat = FlattenActiveAssembly(state);
  json seats = json::array();
  json checks = json::array();
  bool fail = false, missing = false;
  const auto check = [&](json c) {
    const std::string status = c.at("status");
    fail |= status == "fail";
    missing |= status == "unverified";
    checks.push_back(std::move(c));
  };
  json measurements = json::object();
  bool haveBore = false, haveOutside = false;

  // A retaining ring that locates a bearing ring where no shoulder does
  // (docs/parts/README.md#a-ring-in-its-groove): one SEATED in a groove of
  // the seat's own placement (attached, its seat resolving), of the standard
  // the ring's side takes (471 on a shaft for the inner ring, 472 in a bore
  // for the outer), on the bearing's axis and beside it within the groove's
  // float (m - s). Its reach as installed -- its inside on the groove floor,
  // d2 + 2b (471), its outside there, d2 - 2b (472) -- is the abutment, so
  // it is judged by the same limits as a shoulder.
  const auto retainer = [&](bool bore, const assembly::FlatInstance& inst,
                            json& s) {
    const glm::dvec3 dir = glm::normalize(sideB - sideA);
    for (const Occurrence& o : state.assembly->GetOccurrences()) {
      if (o.attachedTo != inst.rootOccurrenceId || !o.attachSeat.IsSet() ||
          o.attachSeat.innerPath != inst.innerIds || !o.attachError.empty())
        continue;
      const RetainingRingFeature* ring =
          SoleRetainingRingOf(OccurrencePartDoc(state, o));
      parts::RetainingRingStandard which{};
      if (ring == nullptr ||
          !parts::RetainingRingStandardFromString(ring->standard, which) ||
          (which == parts::RetainingRingStandard::Din471) != bore)
        continue;
      const parts::RetainingRingRow* rr =
          parts::FindRetainingRing(which, ring->size);
      if (rr == nullptr)
        continue;
      const glm::dmat4 rw = OccurrenceMatrix(o);
      const glm::dvec3 ro(rw[3]);
      const glm::dvec3 rz = glm::normalize(glm::dvec3(rw[2]));
      if (std::abs(std::abs(glm::dot(rz, dir)) - 1.0) > 1e-6)
        continue;
      const glm::dvec3 off = ro - sideA;
      if (glm::length(off - glm::dot(off, dir) * dir) > 1e-3)
        continue;
      const double t0 = glm::dot(off, dir);
      const double t1 = t0 + rr->s * glm::dot(rz, dir);
      const double lo = std::min(t0, t1), hi = std::max(t0, t1);
      // Beside the bearing, on whichever side its middle is: its gap to that
      // side face, which may be a hair negative where the two only touch
      // (the 0.01 mm a ring is "seated" within on a shoulder).
      const double raw = 0.5 * (lo + hi) < 0.5 * row->B ? -hi : lo - row->B;
      const double play = std::max(0.0, rr->m - rr->s) + 0.01;
      if (raw < -0.01 || raw > play)
        continue;
      const double gap = std::max(0.0, raw);
      const double reach = bore ? rr->d2 + 2.0 * rr->b : rr->d2 - 2.0 * rr->b;
      const std::string ringName = bore ? "inner" : "outer";
      const std::string designation =
          parts::RetainingRingDesignation(which, *rr);
      s["retainer"] = {{"occurrence", o.id},
                       {"designation", designation},
                       {"gap_mm", gap},
                       {"reach_mm", reach}};
      check(
          {{"characteristic", ringName + "_ring_located"},
           {"status", "pass"},
           {"observed", gap},
           {"maximum", play},
           {"units", "mm"},
           {"reason", "held by " + designation + " (occurrence " +
                          std::to_string(o.id) + ") in its groove, " + Mm(gap) +
                          " mm off the ring's side (the groove lets it "
                          "float " +
                          Mm(play - 0.01) + ")"}});
      json r = {{"characteristic", ringName + "_retainer_reach"},
                {"observed", reach},
                {"units", "mm"}};
      if (abut != nullptr) {
        r["source"] = source;
        if (bore) {
          const bool haveMax = parts::BearingAbutment::Has(abut->daMax);
          r["minimum"] = abut->daMin;
          if (haveMax)
            r["maximum"] = abut->daMax;
          const bool low = reach < abut->daMin - 1e-6;
          const bool high = haveMax && reach > abut->daMax + 1e-6;
          r["status"] = low || high ? "fail" : haveMax ? "pass" : "unverified";
          r["reason"] =
              low ? "the ring reaches Ø" + Mm(reach) +
                        " in its groove, below d_a min " + Mm(abut->daMin) +
                        ": the inner ring's side is unsupported"
              : high ? "the ring reaches Ø" + Mm(reach) + ", above d_a max " +
                           Mm(abut->daMax) + ": it reaches the shield"
              : haveMax
                  ? "within d_a " + Mm(abut->daMin) + " to " + Mm(abut->daMax)
                  : std::string("it meets d_a min; d_a max is not printed");
        } else {
          r["maximum"] = abut->DaMax;
          const bool wide = reach > abut->DaMax + 1e-6;
          r["status"] = wide ? "fail" : "pass";
          r["reason"] = wide ? "the ring's inside, Ø" + Mm(reach) +
                                   " in its groove, is above D_a max " +
                                   Mm(abut->DaMax) +
                                   ": the outer ring's side is unsupported"
                             : "within D_a max " + Mm(abut->DaMax);
        }
      } else {
        const double edge =
            bore ? row->d + 2.0 * row->rMin : row->D - 2.0 * row->rMin;
        const bool reaches = bore ? reach > edge : reach < edge;
        r["status"] = reaches ? "unverified" : "fail";
        r["reason"] = reaches ? "it reaches past the ring's chamfer; " +
                                    std::string(source) + " lists no " +
                                    row->designation
                              : std::string("it does not reach past the "
                                            "ring's chamfer");
      }
      check(std::move(r));
      return true;
    }
    return false;
  };

  // One ring on one seat, however the seat was found (a mate, or the
  // coaxial search below): the end it sits at, the diameter, the shoulder.
  const auto judge = [&](bool bore, const assembly::FlatInstance& inst,
                         const parts::Seat& seat, json s) {
    // The end the ring sits at: the one nearer either side face.
    const glm::dmat4 toLocal = glm::inverse(inst.world);
    const glm::dvec3 a(toLocal * glm::dvec4(sideA, 1));
    const glm::dvec3 b(toLocal * glm::dvec4(sideB, 1));
    const parts::SeatEnd* end = nullptr;
    double gap = 0.0;
    for (const parts::SeatEnd& e : seat.ends) {
      const double g = std::min(std::abs(glm::dot(a - e.origin, e.along)),
                                std::abs(glm::dot(b - e.origin, e.along)));
      if (end == nullptr || g < gap ||
          (e.shoulder && !end->shoulder && g < gap + 1e-6)) {
        end = &e;
        gap = g;
      }
    }
    std::string printedNote;
    const double seatDia = SeatDiameter(inst.part, seat, &printedNote);
    s["diameter"] = seatDia;
    if (!printedNote.empty())
      s["modelled_diameter"] = seat.diameter;
    const double nominal = bore ? row->d : row->D;
    const std::string key = bore ? "shaft_diameter" : "housing_diameter";
    measurements[key] = seatDia;
    (bore ? haveBore : haveOutside) = true;
    const bool same = std::abs(seatDia - nominal) < 1e-3;
    json dia = {{"characteristic", key},
                {"observed", seatDia},
                {"required", nominal},
                {"status", same ? "pass" : "fail"}};
    if (!printedNote.empty())
      dia["printed"] = printedNote;
    if (!same)
      dia["reason"] =
          "the seat is not the ring's basic size — a fit is a tolerance "
          "class on the seat (check_fits), not a different diameter";
    check(std::move(dia));
    if (end == nullptr || !end->shoulder) {
      s["shoulder"] = false;
      if (retainer(bore, inst, s)) {
        seats.push_back(std::move(s));
        return;
      }
      check({{"characteristic",
              std::string(bore ? "inner" : "outer") + "_ring_located"},
             {"status", "unverified"},
             {"reason", "nothing on this seat stops the ring axially — no "
                        "shoulder at the end it sits at, and no retaining ring "
                        "seated in a groove beside it"}});
      seats.push_back(std::move(s));
      return;
    }
    s["shoulder"] = true;
    s["shoulder_diameter"] = end->shoulderDiameter;
    s["fillet_radius"] = end->filletRadius;
    s["gap_mm"] = gap;
    if (!measurements.contains("shoulder_diameter") || bore) {
      // The catalog schema describes ONE shoulder; the shaft's is the one
      // supplier tables give as d_a, so it wins when both exist.
      measurements["shoulder_diameter"] = end->shoulderDiameter;
      measurements["fillet_radius"] = end->filletRadius;
    }
    const std::string ring = bore ? "inner" : "outer";
    check({{"characteristic", ring + "_ring_seated"},
           {"observed", gap},
           {"maximum", 0.01},
           {"units", "mm"},
           {"status", gap <= 0.01 ? "pass" : "fail"},
           {"reason", gap <= 0.01 ? "the ring's side face is on the shoulder"
                                  : "the ring sits " + Mm(gap) +
                                        " mm off its shoulder"}});
    // The fillet: the supplier's r_a max where the abutment table has one
    // (docs/parts/README.md#abutment-limits), else the ring's own chamfer.
    const bool haveRa =
        abut != nullptr && parts::BearingAbutment::Has(abut->raMax);
    const double filletMax = haveRa ? abut->raMax : row->rMin;
    const std::string filletLimit =
        haveRa ? "r_a max " + Mm(filletMax) + " (" + source + ")"
               : "the ring's chamfer, r_s min " + Mm(filletMax);
    const bool filletOk = end->filletRadius <= filletMax + 1e-9;
    json fillet = {{"characteristic", ring + "_fillet_radius"},
                   {"observed", end->filletRadius},
                   {"maximum", filletMax},
                   {"limit", haveRa ? "r_a max" : "r_s min"},
                   {"units", "mm"},
                   {"status", filletOk ? "pass" : "fail"},
                   {"reason", filletOk
                                  ? "the fillet is within " + filletLimit
                                  : "the fillet is larger than " + filletLimit +
                                        ", so the ring rides up it "
                                        "instead of seating"}};
    if (haveRa)
      fillet["source"] = source;
    check(std::move(fillet));
    json sh = {{"characteristic", ring + "_shoulder_diameter"},
               {"observed", end->shoulderDiameter},
               {"units", "mm"}};
    const double obs = end->shoulderDiameter;
    if (abut != nullptr) {
      // The supplier's abutment limits. A shaft shoulder below d_a min
      // leaves the ring's side face unsupported; above d_a max it reaches
      // the shield (or seal) and leaves no ring face for a puller. A housing
      // shoulder whose bore is above D_a max leaves the outer ring
      // unsupported. A limit the catalogue does not print stays unverified.
      sh["source"] = source;
      if (abut->variant != nullptr)
        sh["variant"] = std::string(row->designation) + "-" + abut->variant;
      if (bore) {
        sh["minimum"] = abut->daMin;
        const bool haveMax = parts::BearingAbutment::Has(abut->daMax);
        if (haveMax)
          sh["maximum"] = abut->daMax;
        if (obs < abut->daMin - 1e-6) {
          sh["status"] = "fail";
          sh["limit"] = "d_a min";
          sh["reason"] = "the shaft shoulder (Ø" + Mm(obs) +
                         ") is below d_a min " + Mm(abut->daMin) +
                         ": too low, the ring's side face is unsupported";
        } else if (haveMax && obs > abut->daMax + 1e-6) {
          sh["status"] = "fail";
          sh["limit"] = "d_a max";
          sh["reason"] = "the shaft shoulder (Ø" + Mm(obs) +
                         ") is above d_a max " + Mm(abut->daMax) + " of the " +
                         sh.at("variant").get<std::string>() +
                         ": too high, it reaches the shield and leaves no "
                         "ring face for a puller";
        } else if (!haveMax) {
          sh["status"] = "unverified";
          sh["limit"] = "d_a max";
          sh["reason"] = "it meets d_a min " + Mm(abut->daMin) +
                         "; the catalogue prints d_a max only for a capped "
                         "bearing and lists no capped " +
                         std::string(row->designation) +
                         ", so how high it may reach is not judged";
        } else {
          sh["status"] = "pass";
          sh["reason"] =
              "within d_a " + Mm(abut->daMin) + " to " + Mm(abut->daMax);
        }
      } else {
        sh["maximum"] = abut->DaMax;
        if (obs > abut->DaMax + 1e-6) {
          sh["status"] = "fail";
          sh["limit"] = "D_a max";
          sh["reason"] = "the housing shoulder's bore (Ø" + Mm(obs) +
                         ") is above D_a max " + Mm(abut->DaMax) +
                         ": too large, the outer ring's side face is "
                         "unsupported";
        } else {
          sh["status"] = "pass";
          sh["reason"] = "within D_a max " + Mm(abut->DaMax) +
                         " (the catalogue gives no lower limit for a "
                         "housing shoulder)";
        }
      }
    } else {
      // No abutment row for this designation: the shoulder has to reach
      // past the ring's chamfer to give it anything to bear on, and beyond
      // that only an installed catalog can judge it.
      const double chamferEdge =
          bore ? row->d + 2.0 * row->rMin : row->D - 2.0 * row->rMin;
      const bool reaches = bore ? obs > chamferEdge : obs < chamferEdge;
      if (!reaches) {
        sh[bore ? "minimum" : "maximum"] = chamferEdge;
        sh["status"] = "fail";
        sh["reason"] = "the shoulder does not reach past the ring's chamfer, "
                       "so the ring's side face bears on nothing";
      } else if (catalogEntry != nullptr) {
        sh["status"] = "pass";
        sh["reason"] = "it reaches past the ring's chamfer; the supplier's "
                       "abutment limits are judged under `catalog`";
      } else {
        sh["status"] = "unverified";
        sh["reason"] = std::string(source) + " lists no " + row->designation +
                       ", so how far its shoulder may reach is not judged — "
                       "pass an installed catalog to judge it";
      }
    }
    check(std::move(sh));
    seats.push_back(std::move(s));
  };

  for (const Mate& mate : state.assembly->GetMates()) {
    const Connector* mine = nullptr;
    const Connector* other = nullptr;
    if (mate.a.occurrenceId == occurrence && mate.a.innerPath.empty()) {
      mine = &mate.a;
      other = &mate.b;
    } else if (mate.b.occurrenceId == occurrence && mate.b.innerPath.empty()) {
      mine = &mate.b;
      other = &mate.a;
    }
    if (mine == nullptr || mine->anchor.face.featureId != bearing->GetId())
      continue;
    const std::string role = BearingFeature::RoleOf(mine->anchor.face.name);
    const bool bore = role == BearingFeature::kBore;
    if (!bore && role != BearingFeature::kOutside)
      continue;
    const assembly::FlatInstance* inst = nullptr;
    for (const assembly::FlatInstance& f : flat)
      if (f.rootOccurrenceId == other->occurrenceId &&
          f.innerIds == other->innerPath && f.part != nullptr)
        inst = &f;
    json s = {{"ring", bore ? "inner" : "outer"}, {"mate", mate.id}};
    if (inst == nullptr || !other->anchor.face.IsSet()) {
      s["error"] = "the other side of this mate is not a face of a placed "
                   "part";
      seats.push_back(std::move(s));
      continue;
    }
    s["part"] = inst->path;
    s["occurrence"] = inst->rootOccurrenceId;
    s["inner_path"] = inst->innerIds;
    s["feature_id"] = other->anchor.face.featureId;
    s["face"] = other->anchor.face.name;
    std::string why;
    const auto seat =
        parts::SeatOf(inst->part->GetBodies(), other->anchor.face, &why);
    if (!seat) {
      s["error"] = why;
      seats.push_back(std::move(s));
      continue;
    }
    if (seat->shaft != bore) {
      s["error"] = std::string("the ") + (bore ? "bore" : "outside") +
                   " is mated to a " + (seat->shaft ? "shaft" : "bore") +
                   ", not a seat for it";
      seats.push_back(std::move(s));
      continue;
    }
    judge(bore, *inst, *seat, std::move(s));
  }

  // A ring no mate reaches is read through the seat it SITS on. A one-solid
  // bearing in a mechanism is mated to one seat only: fastened to the
  // housing AND the shaft it would lock the shaft to the housing, so the
  // inner ring's journal is named by no mate. It is still the seat that
  // ring is on — a cylinder of the right sense (a shaft for the bore, a bore
  // for the outside) on the bearing's axis, running under the ring's width,
  // within 5 % (or 0.5 mm) of its size so an unrelated coaxial face (a
  // clearance bore, a hub) is not judged as a seat. The nearest diameter
  // wins; the diameter check then says whether it is the ring's size.
  const auto searchCoaxial = [&](bool bore) {
    const double nominal = bore ? row->d : row->D;
    const double window = std::max(0.5, 0.05 * nominal);
    const assembly::FlatInstance* best = nullptr;
    std::optional<parts::Seat> bestSeat;
    double bestOff = 0.0;
    for (const assembly::FlatInstance& f : flat) {
      if (f.part == nullptr || !f.visible ||
          (f.rootOccurrenceId == occurrence && f.innerIds.empty()))
        continue;
      const glm::dmat4 toLocal = glm::inverse(f.world);
      const glm::dvec3 a(toLocal * glm::dvec4(sideA, 1));
      const glm::dvec3 b(toLocal * glm::dvec4(sideB, 1));
      const glm::dvec3 dir = glm::normalize(b - a);
      for (const ElementRef& face :
           parts::CoaxialCylinders(f.part->GetBodies(), a, dir)) {
        std::string why;
        auto seat = parts::SeatOf(f.part->GetBodies(), face, &why);
        if (!seat || seat->shaft != bore || seat->ends.empty())
          continue;
        // The seat's span along the bearing's axis, against the ring's.
        double lo = std::numeric_limits<double>::max(), hi = -lo;
        for (const parts::SeatEnd& e : seat->ends) {
          const double t0 = glm::dot(e.origin - a, dir);
          const double t1 = t0 + e.length * glm::dot(e.along, dir);
          lo = std::min({lo, t0, t1});
          hi = std::max({hi, t0, t1});
        }
        if (std::min(hi, row->B) - std::max(lo, 0.0) <= 1e-6)
          continue;
        const double off = std::abs(SeatDiameter(f.part, *seat) - nominal);
        if (off > window || (best != nullptr && off >= bestOff))
          continue;
        best = &f;
        bestSeat = std::move(seat);
        bestOff = off;
      }
    }
    if (best == nullptr)
      return;
    json s = {{"ring", bore ? "inner" : "outer"},
              {"mate", nullptr},
              {"found", "coaxial"},
              {"part", best->path},
              {"occurrence", best->rootOccurrenceId},
              {"inner_path", best->innerIds},
              {"feature_id", bestSeat->face.featureId},
              {"face", bestSeat->face.name}};
    judge(bore, *best, *bestSeat, std::move(s));
  };
  if (!haveBore)
    searchCoaxial(true);
  if (!haveOutside)
    searchCoaxial(false);

  if (!haveBore)
    check({{"characteristic", "shaft_diameter"},
           {"status", "unverified"},
           {"reason", "no seat for the bore: it is mated to none, and no "
                      "shaft of its size runs through it on its axis "
                      "(fit_bearing places it; or add_mate the bore to the "
                      "journal)"}});
  if (!haveOutside)
    check({{"characteristic", "housing_diameter"},
           {"status", "unverified"},
           {"reason", "no seat for the outside: it is mated to none, and no "
                      "housing bore of its size surrounds it on its axis "
                      "(add_mate it, revolute if the housing turns about the "
                      "shaft)"}});
  out["seats"] = seats;
  out["checks"] = checks;
  out["measurements"] = measurements;

  if (catalogEntry != nullptr) {
    const std::string designation =
        catalogDesignation.empty() ? row->designation : catalogDesignation;
    json catalogReport =
        catalog::SeatRequirements(*catalogEntry, designation, measurements);
    const std::string status = catalogReport.at("status");
    fail |= status == "fail";
    missing |= status == "unverified";
    out["catalog"] = std::move(catalogReport);
  }
  out["status"] = fail ? "fail" : missing ? "unverified" : "pass";
  out["evidence"] = "measured from the model: the placed bearing's ISO 15 "
                    "dimensions, the seats it sits on and the supplier's "
                    "abutment limits (`abutment`); load, speed, "
                    "temperature and fit selection are not evaluated";
  return out;
}

} // namespace ui
