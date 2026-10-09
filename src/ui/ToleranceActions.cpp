// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ToleranceActions.h"

#include "core/Document.h"
#include "core/FitStandards.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/ThreadStandards.h"
#include "core/features/ThreadFeature.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BearingActions.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>

namespace ui {
namespace {

// One side of one mate, resolved as far as it goes.
struct Side {
  const Document* part = nullptr;
  const Tolerance* tol = nullptr;
  std::optional<double> diameter;
  std::string occurrence;
  std::string reason; // non-empty = this side stopped here
  // A generated bearing's bore or outside: its band is the standard's
  // (ISO 492 normal), not a stored tolerance, and it says which side of the
  // fit it is — a bore is the hole (docs/parts/README.md#bearings).
  std::optional<BearingRing> ring;
  // The part is printed: its diameter is what it is MODELLED at, and the fit
  // is at what that prints.
  std::optional<printfit::Process> print;
  bool notRound = false; // the face is flat: the joint is not a fit
};

// 8.2 -> "8.2", 8 -> "8", 7.0466 -> "7.047": millimetres as a person writes
// them, to the micrometre and no further.
std::string Fmt(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", v);
  std::string s(buf);
  while (!s.empty() && s.back() == '0')
    s.pop_back();
  if (!s.empty() && s.back() == '.')
    s.pop_back();
  return s;
}

std::string Signed(double v) { return (v >= 0.0 ? "+" : "") + Fmt(v); }

std::string ProcessName(const printfit::Process& p) {
  return p.process + " " + p.material;
}

} // namespace

std::string PrintedDiameter::Sentence(const std::string& cls) const {
  std::string out = (cls.empty() ? std::string{} : cls + " on ") +
                    "a printed Ø" + Fmt(basic) + (hole ? " bore" : " shaft") +
                    " (" + ProcessName(process) + "): model Ø" + Fmt(model);
  if (modelled && !Compensated())
    out += " - it is modelled Ø" + Fmt(*modelled) + ", which prints Ø" +
           Fmt(Prints());
  return out;
}

bool PrintedDiameter::Compensated() const {
  return !modelled || std::abs(*modelled - model) < 1e-3;
}

double PrintedDiameter::Prints() const {
  return modelled ? *modelled - allowance : basic;
}

namespace {

void AttachBandNote(PrintedDiameter& d, const std::string& cls) {
  if (cls.empty())
    return;
  std::string why;
  const auto band = fits::Limits(d.basic, cls, &why);
  const double rep = printfit::ProcessRepeatabilityMm(d.process);
  if (!band || rep <= 0.0 || band->it >= 2.0 * rep)
    return;
  d.bandNote = "the " + cls + " band (" + Fmt(band->it) +
               " mm) is finer than " + d.process.process + " holds (±" +
               Fmt(rep) +
               " mm): a print will not meet it on its own, so "
               "ream or drill it to size, or read the class as the intent (" +
               printfit::ProcessRepeatabilitySource(d.process) + ")";
}

} // namespace

std::optional<PrintedDiameter> PrintedForBasic(const printfit::Process& p,
                                               bool hole, double basic,
                                               std::optional<double> modelled,
                                               const std::string& cls,
                                               std::string* why) {
  const auto a = printfit::ForBasic(
      p, hole ? printfit::Printed::Hole : printfit::Printed::Shaft, basic, why);
  if (!a)
    return std::nullopt;
  PrintedDiameter d;
  d.process = p;
  d.hole = hole;
  d.basic = a->basicMm;
  d.allowance = a->allowanceMm;
  d.model = a->modelMm;
  d.source = a->row != nullptr ? a->row->source : std::string{};
  d.modelled = modelled;
  AttachBandNote(d, cls);
  return d;
}

std::optional<PrintedDiameter> PrintedForModelled(const printfit::Process& p,
                                                  bool hole, double modelled,
                                                  const std::string& cls,
                                                  std::string* why) {
  const auto a = printfit::ForModelled(
      p, hole ? printfit::Printed::Hole : printfit::Printed::Shaft, modelled,
      why);
  if (!a)
    return std::nullopt;
  PrintedDiameter d;
  d.process = p;
  d.hole = hole;
  d.basic = a->basicMm;
  d.allowance = a->allowanceMm;
  d.model = a->modelMm;
  d.source = a->row != nullptr ? a->row->source : std::string{};
  d.modelled = modelled;
  AttachBandNote(d, cls);
  return d;
}

std::string PrintedThread::Sentence() const {
  std::string out = "Thread " + designation + " (" +
                    (external ? "external" : "internal") + ", printed): ";
  if (known)
    out += "model its major Ø" + Fmt(modelMajor) + " (" + Signed(allowance) +
           ") to print Ø" + Fmt(wantMajor) + "; as modelled (Ø" +
           Fmt(modelledMajor) + ") it prints Ø" + Fmt(printsMajor);
  else
    out += "modelled at a Ø" + Fmt(modelledMajor) + " major, it prints Ø" +
           Fmt(printsMajor) + " (" + Signed(allowance) + " allowance)";
  return out;
}

std::optional<PrintedThread> PrintedThreadFor(const Document& doc,
                                              const ThreadFeature& th) {
  const auto& proc = doc.PrintProcess();
  if (!proc || printfit::Find(*proc) == nullptr)
    return std::nullopt;
  PrintedThread r;
  r.featureId = th.GetId();
  r.name = th.GetName();
  r.designation = th.designation;
  r.modeled = th.modeled;
  r.pitch = th.pitch;
  r.resolved = th.overlay.valid;
  r.external = th.overlay.external;
  const auto row = threads::Lookup(th.designation);
  r.known = row.has_value();
  if (row) {
    r.pitch = row->pitch;
    r.modelledMajor = row->majorDia;
  } else if (r.resolved) {
    const double face = 2.0 * th.overlay.faceRadius;
    r.modelledMajor = r.external ? face : face + 1.0825 * r.pitch;
  }
  if (!row && !r.resolved) {
    r.notes.push_back("the thread's face does not resolve, so its size is "
                      "unknown");
    return r;
  }
  const printfit::Printed kind = r.external ? printfit::Printed::ExternalThread
                                            : printfit::Printed::InternalThread;
  std::string why;
  const auto a = printfit::ForBasic(*proc, kind, r.modelledMajor, &why);
  if (!a) {
    r.notes.push_back(why);
    return r;
  }
  r.allowance = a->allowanceMm;
  r.printsMajor = r.modelledMajor - r.allowance;
  if (row) {
    r.wantMajor = row->majorDia;
    r.modelMajor = row->majorDia + r.allowance;
    r.modelFace =
        r.external ? r.modelMajor : threads::MinorDiameter(*row) + r.allowance;
    r.notes.push_back(
        "the Thread cuts the table's basic profile, so an '" +
        row->designation + "' prints tight. To print it at Ø" +
        Fmt(r.wantMajor) +
        ", model a Custom thread (a designation that names no series, pitch " +
        Fmt(r.pitch) + ") on a Ø" + Fmt(r.modelFace) +
        (r.external
             ? " boss - a custom thread takes a boss's face as its major"
             : " bore - a custom thread takes a bore's wall as its basic "
               "minor"));
  }
  if (!r.modeled)
    r.notes.push_back("cosmetic: the body has no groove, so nothing is "
                      "printed - a printed thread needs `modeled: true`");
  const double floor = printfit::MinReliableThreadMm(*proc);
  if (floor > 0.0 && r.modelledMajor < floor)
    r.notes.push_back(
        "printed threads are unreliable below Ø" + Fmt(floor) + " in " +
        proc->process + " (community practice)" +
        (r.external ? std::string{}
                    : std::string(": for a metal screw print a plain bore at "
                                  "the tap drill") +
                          (row ? " (Ø" + Fmt(threads::TapDrill(*row)) + ")"
                               : std::string{}) +
                          " and cut the thread with a tap, or use a heat-set "
                          "insert"));
  return r;
}

std::string FitCheck::Sentence() const {
  const std::string who =
      mateName.empty() ? ("mate " + std::to_string(mateId)) : mateName;
  if (!computed)
    return who + ": " + reason;
  char buf[192];
  std::snprintf(buf, sizeof(buf), "%s: %s %s / %s %s - %s %.3f..%.3f mm",
                who.c_str(), holeOccurrence.c_str(), holeClass.c_str(),
                shaftOccurrence.c_str(), shaftClass.c_str(), kind.c_str(),
                minClearance, maxClearance);
  std::string out(buf);
  if (!reason.empty())
    out += " - " + reason;
  if (holePrint)
    out += " - " + holePrint->Sentence(holeClass);
  if (shaftPrint)
    out += " - " + shaftPrint->Sentence(shaftClass);
  return out;
}

std::vector<FitCheck> CheckFits(AppState& state) {
  std::vector<FitCheck> out;
  if (!state.assembly)
    return out;

  const auto resolveSide = [&](const Connector& c, const char* which) {
    Side side;
    const Occurrence* occ = state.assembly->FindById(c.occurrenceId);
    if (!occ) {
      side.reason = std::string(which) + " occurrence is missing";
      return side;
    }
    side.occurrence = occ->name;
    // A5: walk down to the part the anchor actually means. The same descent
    // AssemblySolve's ConnectorFrame makes, minus the transform — a fit is
    // between two SIZES, and a size does not care where the part was placed.
    const Document* pd = nullptr;
    if (!c.innerPath.empty()) {
      if (occ->refKind != OccurrenceRef::Assembly) {
        side.reason = std::string(which) + " (" + occ->name +
                      ") has a path into a sub-assembly but places a part";
        return side;
      }
      const AssemblyDocument* sub = AssemblyDocById(state, occ->partId);
      const Occurrence* inner = nullptr;
      for (const uint64_t step : c.innerPath) {
        if (sub == nullptr) {
          side.reason = std::string(which) + " (" + side.occurrence +
                        ") has a path deeper than the assembly goes";
          return side;
        }
        inner = sub->FindById(step);
        if (inner == nullptr) {
          side.reason = std::string(which) + " (" + side.occurrence +
                        ") points at a part inside the sub-assembly that is "
                        "gone";
          return side;
        }
        // The path names the part, so it is also the label a person needs:
        // two occurrences of the same bearing are told apart by where they
        // sit, not by their part name.
        side.occurrence += "/" + inner->name;
        sub = inner->refKind == OccurrenceRef::Assembly
                  ? AssemblyDocById(state, inner->partId)
                  : nullptr;
      }
      pd = OccurrencePartDoc(state, *inner);
    }
    if (c.anchor.frameId != 0 || c.anchor.origin || !c.anchor.face.IsSet()) {
      side.reason = std::string(which) + " (" + side.occurrence +
                    ") is mated on a " +
                    (c.anchor.frameId != 0 ? "datum frame" : "part origin") +
                    ", not a face — there is no surface to give a size";
      return side;
    }
    if (pd == nullptr)
      pd = OccurrencePartDoc(state, *occ);
    if (!pd) {
      side.reason =
          std::string(which) + " (" + side.occurrence + ")" +
          (occ->refKind == OccurrenceRef::Assembly && c.innerPath.empty()
               ? " is a sub-assembly, which has no faces of its own"
               : " has no part document loaded");
      return side;
    }
    side.part = pd;
    if (pd->PrintProcess() && printfit::Find(*pd->PrintProcess()) != nullptr)
      side.print = pd->PrintProcess();
    if (auto ring = BearingRingOf(pd, c.anchor.face)) {
      side.ring = ring;
      side.diameter = ring->nominal;
      return side;
    }
    const auto el = Measure::Describe(pd->GetBodies(), c.anchor.face);
    if (el && !el->hasRadius) {
      // A flat face has no size: the joint is not a fit at all (the
      // interface a bolt clamps, a part sat on a face), and listing it as an
      // unverified fit buried the head's eight bearing seats under eighteen
      // such rows.
      side.notRound = true;
      return side;
    }
    side.tol = pd->ToleranceFor(c.anchor.face);
    if (!side.tol) {
      side.reason = std::string(which) + " (" + side.occurrence + ") face " +
                    c.anchor.face.name +
                    " carries no tolerance (set_tolerance on that part)";
      return side;
    }
    if (!el) {
      side.reason = std::string(which) + " (" + side.occurrence + ") face " +
                    c.anchor.face.name + " no longer resolves to a round face";
      return side;
    }
    side.diameter = el->radius * 2.0;
    return side;
  };

  for (const Mate& mate : state.assembly->GetMates()) {
    // Which mates are a fit at all, and which of those declare an intent
    // strong enough to judge against. A joint that MOVES says so outright:
    // revolute turns, slider slides, cylindrical does both. Fastened says
    // only that nothing moves — not whether the parts are pressed together
    // or bolted through a clearance hole — so its fit is reported and not
    // judged. Planar and ball are not round joints and are not fits.
    const char* mateKind = nullptr;
    bool moves = false;
    switch (mate.type) {
    case MateType::Revolute:
      mateKind = "revolute";
      moves = true;
      break;
    case MateType::Cylindrical:
      mateKind = "cylindrical";
      moves = true;
      break;
    case MateType::Slider:
      mateKind = "slider";
      moves = true;
      break;
    case MateType::Fastened:
      mateKind = "fastened";
      break;
    default:
      break;
    }
    if (mateKind == nullptr)
      continue;

    FitCheck e;
    e.mateId = mate.id;
    e.mateName = mate.name;
    e.mateType = mateKind;
    const auto stop = [&](std::string reason) {
      e.status = "unverified";
      e.reason = std::move(reason);
      out.push_back(std::move(e));
    };

    const Side a = resolveSide(mate.a, "a");
    const Side b = resolveSide(mate.b, "b");
    if (a.notRound || b.notRound)
      continue; // a flat face: not a fit
    if (!a.reason.empty() || !b.reason.empty()) {
      stop(!a.reason.empty() ? a.reason : b.reason);
      continue;
    }
    if (a.ring || b.ring) {
      // A bearing ring: the ring's band is ISO 492's and the ring says which
      // side it is, so the other side may be a class OR explicit deviations.
      if (a.ring && b.ring) {
        stop("both sides are bearing rings — a ring's fit is on the shaft or "
             "the housing it sits on, not on another bearing");
        continue;
      }
      const Side& ringSide = a.ring ? a : b;
      const Side& other = a.ring ? b : a;
      const BearingRing& ring = *ringSide.ring;
      if (!other.tol->isoClass.empty()) {
        const bool otherHole =
            std::isupper(static_cast<unsigned char>(other.tol->isoClass[0])) !=
            0;
        if (otherHole == ring.bore) {
          stop(other.occurrence + " is toleranced " + other.tol->isoClass +
               ", a " + (otherHole ? "hole" : "shaft") + " class, but it " +
               "meets the bearing's " + (ring.bore ? "bore" : "outside") +
               " — it needs a " + (ring.bore ? "shaft" : "hole") + " class");
          continue;
        }
      }
      // A printed seat is at what it PRINTS: the model carries the
      // allowance, and the ring meets the print.
      std::optional<PrintedDiameter> otherPrint;
      double seat = *other.diameter;
      if (other.print) {
        std::string why;
        otherPrint = PrintedForModelled(*other.print, !ring.bore, seat,
                                        other.tol->isoClass, &why);
        if (!otherPrint) {
          stop(why);
          continue;
        }
        seat = otherPrint->basic;
      }
      if (std::abs(ring.nominal - seat) > 1e-5) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "the seat is not the ring's basic size — the %s %s is "
                      "%.4f mm and the seat %.4f mm",
                      ring.designation.c_str(), ring.bore ? "bore" : "outside",
                      ring.nominal, *other.diameter);
        std::string reason = buf;
        if (otherPrint) {
          std::string why;
          const auto want =
              PrintedForBasic(*other.print, !ring.bore, ring.nominal,
                              *other.diameter, other.tol->isoClass, &why);
          reason += ", which prints " + Fmt(seat) + " mm (" +
                    ProcessName(*other.print) + ")" +
                    (want ? "; to print Ø" + Fmt(ring.nominal) + " model it Ø" +
                                Fmt(want->model)
                          : std::string{});
        }
        stop(reason);
        continue;
      }
      fits::Band otherBand;
      std::string otherClass;
      if (other.tol->isoClass.empty()) {
        otherBand.lower = other.tol->lower;
        otherBand.upper = other.tol->upper;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%+.3f/%+.3f", other.tol->lower,
                      other.tol->upper);
        otherClass = buf;
      } else {
        std::string why;
        const auto band = fits::Limits(ring.nominal, other.tol->isoClass, &why);
        if (!band) {
          stop(why);
          continue;
        }
        otherBand = *band;
        otherClass = other.tol->isoClass;
      }
      e.computed = true;
      e.size = ring.nominal;
      const std::string ringClass = ring.designation +
                                    (ring.bore ? " bore" : " outside") +
                                    " (ISO 492 normal)";
      const double ringMin = ring.nominal + ring.band.lower;
      const double ringMax = ring.nominal + ring.band.upper;
      if (ring.bore) {
        e.holeOccurrence = ringSide.occurrence;
        e.shaftOccurrence = other.occurrence;
        e.holeClass = ringClass;
        e.shaftClass = otherClass;
        e.holeMin = ringMin;
        e.holeMax = ringMax;
        e.shaftMin = otherBand.Min(e.size);
        e.shaftMax = otherBand.Max(e.size);
      } else {
        e.holeOccurrence = other.occurrence;
        e.shaftOccurrence = ringSide.occurrence;
        e.holeClass = otherClass;
        e.shaftClass = ringClass;
        e.holeMin = otherBand.Min(e.size);
        e.holeMax = otherBand.Max(e.size);
        e.shaftMin = ringMin;
        e.shaftMax = ringMax;
      }
      if (ring.bore)
        e.shaftPrint = otherPrint;
      else
        e.holePrint = otherPrint;
      e.minClearance = e.holeMin - e.shaftMax;
      e.maxClearance = e.holeMax - e.shaftMin;
      e.kind = fits::FitKindName(
          e.minClearance >= 0.0   ? fits::FitKind::Clearance
          : e.maxClearance <= 0.0 ? fits::FitKind::Interference
                                  : fits::FitKind::Transition);
      // Not judged, whatever the mate type. Which ring wants the tight fit
      // depends on which one turns relative to the LOAD, and the model says
      // neither; a revolute mate on a one-body bearing is the balls turning,
      // not the ring creeping in its seat.
      e.status = "reported";
      e.reason = "a bearing ring's fit depends on which ring turns relative "
                 "to the load, which the model does not say — so the fit is "
                 "reported, not judged";
      out.push_back(std::move(e));
      continue;
    }
    // Which side is the bore is said by the CLASS, not guessed from the
    // geometry: an uppercase class is a hole, lowercase a shaft. Two of the
    // same is not a fit anyone can compute, and picking one to flip would be
    // inventing the answer.
    if (a.tol->isoClass.empty() || b.tol->isoClass.empty()) {
      stop("a fit needs an ISO class on both sides; " +
           std::string(a.tol->isoClass.empty() ? a.occurrence : b.occurrence) +
           " states explicit deviations, which say nothing about which side "
           "is the bore");
      continue;
    }
    const bool aHole =
        std::isupper(static_cast<unsigned char>(a.tol->isoClass[0])) != 0;
    const bool bHole =
        std::isupper(static_cast<unsigned char>(b.tol->isoClass[0])) != 0;
    if (aHole == bHole) {
      stop("both sides are " + std::string(aHole ? "hole" : "shaft") +
           " classes (" + a.tol->isoClass + " and " + b.tol->isoClass +
           ") — one of them has to be the bore");
      continue;
    }
    const Side& hole = aHole ? a : b;
    const Side& shaft = aHole ? b : a;
    // A printed side is at what it PRINTS (the model carries the
    // allowance), so a bore modelled Ø8.2 in PETG meets a Ø8 steel pin at
    // one basic size — and one modelled Ø8 does not, and is told what to
    // model instead.
    std::optional<PrintedDiameter> holePrint, shaftPrint;
    double holeBasic = *hole.diameter, shaftBasic = *shaft.diameter;
    {
      std::string why;
      if (hole.print) {
        holePrint = PrintedForModelled(*hole.print, true, holeBasic,
                                       hole.tol->isoClass, &why);
        if (!holePrint) {
          stop(why);
          continue;
        }
        holeBasic = holePrint->basic;
      }
      if (shaft.print) {
        shaftPrint = PrintedForModelled(*shaft.print, false, shaftBasic,
                                        shaft.tol->isoClass, &why);
        if (!shaftPrint) {
          stop(why);
          continue;
        }
        shaftBasic = shaftPrint->basic;
      }
    }
    constexpr double kAgree = 1e-5;
    if (std::abs(holeBasic - shaftBasic) > kAgree) {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "the two faces are not the same basic size — the bore "
                    "measures %.4f mm and the shaft %.4f mm",
                    *hole.diameter, *shaft.diameter);
      std::string reason = buf;
      // Say what printing does to each, and what the printed side should be
      // modelled at to meet the other — the one number a person needs.
      const auto printedSay = [&](const std::optional<PrintedDiameter>& pd,
                                  const char* which) {
        if (pd)
          reason += std::string("; the ") + which + " prints " +
                    Fmt(pd->basic) + " mm (" + ProcessName(pd->process) + ")";
      };
      printedSay(holePrint, "bore");
      printedSay(shaftPrint, "shaft");
      if (holePrint && !shaftPrint) {
        std::string why;
        if (const auto want =
                PrintedForBasic(*hole.print, true, shaftBasic, *hole.diameter,
                                hole.tol->isoClass, &why))
          reason += "; to print Ø" + Fmt(shaftBasic) + " model the bore Ø" +
                    Fmt(want->model);
      } else if (shaftPrint && !holePrint) {
        std::string why;
        if (const auto want =
                PrintedForBasic(*shaft.print, false, holeBasic, *shaft.diameter,
                                shaft.tol->isoClass, &why))
          reason += "; to print Ø" + Fmt(holeBasic) + " model the shaft Ø" +
                    Fmt(want->model);
      }
      stop(reason);
      continue;
    }
    std::string why;
    const auto fit =
        fits::Fit(holeBasic, hole.tol->isoClass, shaft.tol->isoClass, &why);
    if (!fit) {
      stop(why);
      continue;
    }
    e.computed = true;
    e.size = holeBasic;
    e.holePrint = holePrint;
    e.shaftPrint = shaftPrint;
    e.holeOccurrence = hole.occurrence;
    e.shaftOccurrence = shaft.occurrence;
    e.holeClass = hole.tol->isoClass;
    e.shaftClass = shaft.tol->isoClass;
    e.holeMin = fit->hole.Min(e.size);
    e.holeMax = fit->hole.Max(e.size);
    e.shaftMin = fit->shaft.Min(e.size);
    e.shaftMax = fit->shaft.Max(e.size);
    e.minClearance = fit->minClearance;
    e.maxClearance = fit->maxClearance;
    e.kind = fits::FitKindName(fit->kind);

    // The mate type IS the intent, and this is the whole reason pairing off
    // mates is worth more than a pairing table: a joint that moves says these
    // two move against each other, and an interference fit says they do not.
    //
    // A fastened joint is deliberately NOT judged. "Nothing moves here" is
    // true of a shrunk-on gear and of a bracket bolted through a clearance
    // hole, and the two want opposite fits — so a verdict either way would be
    // invented. The fit is computed and reported, which is the useful half.
    if (!moves) {
      e.status = "reported";
      e.reason = "a fastened joint says these do not move, but not whether "
                 "they are pressed together or bolted through a clearance "
                 "hole — so the fit is reported, not judged";
    } else if (fit->kind == fits::FitKind::Interference) {
      e.status = "fail";
      e.reason = std::string("this joint ") +
                 (mate.type == MateType::Slider ? "slides" : "turns") +
                 ", but the fit is interference — the shaft is always larger "
                 "than the bore, so it is pressed in, not journalled";
    } else if (fit->kind == fits::FitKind::Transition) {
      e.status = "warn";
      e.reason = std::string("a transition fit may come out tight or loose on "
                             "any given pair, so this joint is not guaranteed "
                             "to ") +
                 (mate.type == MateType::Slider ? "slide" : "turn");
    } else {
      e.status = "pass";
    }
    out.push_back(std::move(e));
  }
  return out;
}

} // namespace ui
