#include "ui/ToleranceActions.h"

#include "core/Document.h"
#include "core/FitStandards.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"

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
};

} // namespace

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
    Document* pd = nullptr;
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
      pd = PartDocById(state, inner->partId);
    }
    if (c.anchor.frameId != 0 || c.anchor.origin || !c.anchor.face.IsSet()) {
      side.reason = std::string(which) + " (" + side.occurrence +
                    ") is mated on a " +
                    (c.anchor.frameId != 0 ? "datum frame" : "part origin") +
                    ", not a face — there is no surface to give a size";
      return side;
    }
    if (pd == nullptr)
      pd = PartDocById(state, occ->partId);
    if (!pd) {
      side.reason =
          std::string(which) + " (" + side.occurrence + ")" +
          (occ->refKind == OccurrenceRef::Assembly && c.innerPath.empty()
               ? " is a sub-assembly, which has no faces of its own"
               : " has no part document loaded");
      return side;
    }
    side.part = pd;
    side.tol = pd->ToleranceFor(c.anchor.face);
    if (!side.tol) {
      side.reason = std::string(which) + " (" + side.occurrence + ") face " +
                    c.anchor.face.name +
                    " carries no tolerance (set_tolerance on that part)";
      return side;
    }
    const auto el = Measure::Describe(pd->GetBodies(), c.anchor.face);
    if (!el || !el->hasRadius) {
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
    if (!a.reason.empty() || !b.reason.empty()) {
      stop(!a.reason.empty() ? a.reason : b.reason);
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
    constexpr double kAgree = 1e-5;
    if (std::abs(*hole.diameter - *shaft.diameter) > kAgree) {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "the two faces are not the same basic size — the bore "
                    "measures %.4f mm and the shaft %.4f mm",
                    *hole.diameter, *shaft.diameter);
      stop(buf);
      continue;
    }
    std::string why;
    const auto fit = fits::Fit(*hole.diameter, hole.tol->isoClass,
                               shaft.tol->isoClass, &why);
    if (!fit) {
      stop(why);
      continue;
    }
    e.computed = true;
    e.size = *hole.diameter;
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
