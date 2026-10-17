// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/drawing/DrawingCallout.h"

#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/FitStandards.h"
#include "core/Measure.h"
#include "core/features/HoleFeature.h"
#include "core/io/Format.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace drawing {

std::vector<uint64_t> MappedNameFeatureIds(const std::string& mappedName) {
  return topo::FeatureIdsMentioned(mappedName); // the one grammar reader
}

const HoleFeature* FindHoleForRef(const Document& doc,
                                  const std::string& mappedName,
                                  double modelRadius) {
  const auto radiusMatches = [&](const HoleFeature& h) {
    const auto match = [&](double dia) {
      const double r = dia / 2.0;
      return r > 0.0 && std::abs(modelRadius - r) <= 1e-3 * std::max(r, 1.0);
    };
    if (match(h.diameter))
      return true;
    if (h.kind == HoleKind::Counterbore && match(h.cbDiameter))
      return true;
    if (h.kind == HoleKind::Countersink && match(h.csDiameter))
      return true;
    return false;
  };
  for (const uint64_t id : MappedNameFeatureIds(mappedName)) {
    // FindFeatureById is non-const only; the lookup itself doesn't mutate.
    const Feature* f = const_cast<Document&>(doc).FindFeatureById(id);
    if (const auto* hole = dynamic_cast<const HoleFeature*>(f))
      if (radiusMatches(*hole))
        return hole;
  }
  return nullptr;
}

const Tolerance* FindToleranceForRef(const Document& doc,
                                     const std::string& mappedName,
                                     double modelRadius) {
  // Provenance is compared NAME to NAME, not against ElementRef::featureId:
  // that field is the id of the feature whose BODY owns the name (the box a
  // hole was cut into), while the name itself encodes the features that made
  // the face (the hole). Matching on the former finds nothing on any bore
  // that was cut rather than created — which is every interesting one.
  const std::vector<uint64_t> ids = MappedNameFeatureIds(mappedName);
  const auto sharesProvenance = [&](const std::string& other) {
    for (const uint64_t id : MappedNameFeatureIds(other))
      if (std::find(ids.begin(), ids.end(), id) != ids.end())
        return true;
    return false;
  };
  const Tolerance* found = nullptr;
  for (const Tolerance& t : doc.GetTolerances()) {
    if (t.IsDistance() || !sharesProvenance(t.ref.name))
      continue;
    const auto el = Measure::Describe(doc.GetBodies(), t.ref);
    if (!el || !el->hasRadius)
      continue;
    if (std::abs(el->radius - modelRadius) > 1e-3 * std::max(el->radius, 1.0))
      continue;
    if (found != nullptr)
      return nullptr; // ambiguous: two bands, and no way to say which
    found = &t;
  }
  return found;
}

std::string FormatToleranceSuffix(const Tolerance& tol,
                                  const units::System& units, double nominal) {
  if (tol.IsDistance())
    return {};
  const auto signed_ = [&](double v) {
    // A leading + is not decoration: "0.05/-0.02" reads as a range, while
    // "+0.05/-0.02" reads as the two deviations it is.
    const std::string n = core::FormatDeviation(v, units);
    return v >= 0.0 ? "+" + n : n;
  };
  if (tol.printedPress)
    return " printed press"; // its band is the print profile's
  if (!tol.isoClass.empty()) {
    std::string out = " " + tol.isoClass;
    if (nominal > 0.0)
      if (const auto band = fits::Limits(nominal, tol.isoClass, nullptr)) {
        // A zero deviation is written 0, as ISO 286 prints it: "+0.021/0".
        const auto dev = [&](double v) {
          return std::abs(v) < 1e-9 ? std::string("0") : signed_(v);
        };
        out += " (" + dev(band->upper) + "/" + dev(band->lower) + ")";
      }
    return out;
  }
  return " " + signed_(tol.upper) + "/" + signed_(tol.lower);
}

const ThreadMark* FindThreadForHole(const Document& doc,
                                    const HoleFeature& hole) {
  const ThreadMark* found = nullptr;
  for (const Body& body : doc.GetBodies())
    for (const ThreadMark& m : body.threads) {
      const std::vector<uint64_t> ids = MappedNameFeatureIds(m.face.name);
      if (m.featureId == hole.GetId() ||
          std::find(ids.begin(), ids.end(), hole.GetId()) != ids.end())
        found = &m;
    }
  return found;
}

std::string FormatHoleCallout(const HoleFeature& hole,
                              const units::System& units,
                              const ThreadMark* thread) {
  // A hole that recorded what it is FOR says so, instead of leaving a reader
  // to recognise 6.6 as an M6 clearance.
  // docs/modeling/holes-and-threads.md#hole promised exactly this when the fit
  // landed and it was the half that never got built, so a drawing went on
  // calling every hole by its diameter while the model knew better.
  //
  // A TAPPED hole leads with its thread, because M6 is the designation a
  // machinist reads and the tap drill is not what gets made — printing Ø5.0
  // for an M6 tapped hole is worse than useless, it is the wrong instruction.
  // A CLEARANCE hole keeps its diameter, because that IS what gets drilled,
  // and states the intent after it.
  const auto len = [&](double mm) { return core::FormatLength(mm, units); };
  const bool named = !hole.fitThread.empty() && !hole.fitKind.empty();
  const bool tapped = named && hole.fitKind == "tapped";
  std::string out;
  if (tapped && thread != nullptr && !thread->designation.empty()) {
    out = thread->designation;
    if (!thread->threadClass.empty())
      out += "-" + thread->threadClass; // "M6-6H", "3/8-16 UNC-2B"
    // How deep it is THREADED, then what is drilled under it: a blind
    // tapped hole is two depths, and the drill is what the shop starts with.
    if (thread->length > 1e-9)
      out += " DEEP " + len(thread->length);
    else
      out += hole.through ? " THRU" : " DEEP " + len(hole.depth);
    out += ", DRILL \xC3\x98" + len(hole.diameter) +
           (hole.through ? " THRU" : " DEEP " + len(hole.depth));
    return out;
  }
  if (tapped) {
    out = hole.fitThread;
    // Printed only when the document stated one: "M6" alone means the coarse
    // pitch everywhere in engineering, and spelling it out invites the reader
    // to wonder what was meant by the one that says nothing.
    if (hole.fitPitch > 0.0)
      out += "\xC3\x97" + len(hole.fitPitch);
  } else {
    out = "\xC3\x98" + len(hole.diameter);
  }
  out += hole.through ? " THRU" : " DEEP " + len(hole.depth);
  if (hole.kind == HoleKind::Counterbore) {
    out +=
        " CBORE \xC3\x98" + len(hole.cbDiameter) + " DEEP " + len(hole.cbDepth);
  } else if (hole.kind == HoleKind::Countersink) {
    const double angle =
        2.0 * std::atan2((hole.csDiameter - hole.diameter) / 2.0,
                         std::max(hole.csDepth, 1e-9));
    out += " CSK \xC3\x98" + len(hole.csDiameter) + " \xC3\x97 " +
           core::FormatAngle(angle, units); // "× 90°"
  }
  // The fit clause comes last, after any recess, so the callout still reads
  // as an instruction first and an explanation second. The fit is NAMED even
  // when it is the normal one: "M6 clearance" conventionally implies normal,
  // and an implication is exactly what this hole stopped relying on.
  if (named && !tapped) {
    std::string kind = hole.fitKind;
    for (char& c : kind)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out += ", " + hole.fitThread + " " + kind + " CLEARANCE";
  }
  return out;
}

std::optional<double> UniformCollinearPitch(std::span<const glm::dvec2> centers,
                                            double tol) {
  if (centers.size() < 2)
    return std::nullopt;
  // Line through the two extreme centres (max pairwise separation).
  glm::dvec2 a = centers[0], b = centers[1];
  double best = glm::length(b - a);
  for (size_t i = 0; i < centers.size(); ++i)
    for (size_t j = i + 1; j < centers.size(); ++j) {
      const double d = glm::length(centers[j] - centers[i]);
      if (d > best) {
        best = d;
        a = centers[i];
        b = centers[j];
      }
    }
  if (best < tol)
    return std::nullopt; // coincident points
  const glm::dvec2 dir = (b - a) / best;
  const glm::dvec2 nrm{-dir.y, dir.x};
  std::vector<double> ts;
  ts.reserve(centers.size());
  const double lateralTol = std::max(tol, 1e-3 * best);
  for (const glm::dvec2& c : centers) {
    if (std::abs(glm::dot(c - a, nrm)) > lateralTol)
      return std::nullopt; // not collinear
    ts.push_back(glm::dot(c - a, dir));
  }
  std::sort(ts.begin(), ts.end());
  const double pitch = ts[1] - ts[0];
  if (pitch < tol)
    return std::nullopt; // duplicate centres
  for (size_t i = 1; i + 1 < ts.size(); ++i)
    if (std::abs((ts[i + 1] - ts[i]) - pitch) > std::max(tol, 1e-3 * pitch))
      return std::nullopt; // uneven spacing
  return pitch;
}

} // namespace drawing
