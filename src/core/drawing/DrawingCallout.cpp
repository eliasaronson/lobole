#include "core/drawing/DrawingCallout.h"

#include "core/Document.h"
#include "core/features/HoleFeature.h"
#include "core/io/Format.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace drawing {
namespace {

// Parse a leading unsigned integer from `s` starting at `pos`; returns
// nullopt when there is none. Advances `pos` past the digits.
std::optional<uint64_t> LeadingId(const std::string& s, size_t& pos) {
  size_t i = pos;
  uint64_t v = 0;
  bool any = false;
  while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
    v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    any = true;
    ++i;
  }
  if (!any)
    return std::nullopt;
  pos = i;
  return v;
}

} // namespace

std::vector<uint64_t> MappedNameFeatureIds(const std::string& mappedName) {
  std::vector<uint64_t> ids;
  size_t start = 0;
  while (start <= mappedName.size()) {
    size_t semi = mappedName.find(';', start);
    if (semi == std::string::npos)
      semi = mappedName.size();
    // One ';'-separated token. Base segments may be comma-joined L-names;
    // split those too.
    size_t tokStart = start;
    while (tokStart < semi) {
      size_t comma = mappedName.find(',', tokStart);
      if (comma == std::string::npos || comma > semi)
        comma = semi;
      const std::string tok = mappedName.substr(tokStart, comma - tokStart);
      if (!tok.empty()) {
        size_t pos = 0;
        if (tok[0] == 'M' || tok[0] == 'G') {
          // Modifier suffix: "M14", "G4.0" (ordinal after '.' ignored).
          pos = 1;
          if (const auto id = LeadingId(tok, pos))
            ids.push_back(*id);
        } else if (std::isdigit(static_cast<unsigned char>(tok[0]))) {
          // Base segment: "<id>/face1", "<id>/seam0", "<id>/cap0".
          if (const auto id = LeadingId(tok, pos))
            if (pos < tok.size() && tok[pos] == '/')
              ids.push_back(*id);
        }
      }
      tokStart = comma + 1;
    }
    start = semi + 1;
  }
  return ids;
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

std::string FormatHoleCallout(const HoleFeature& hole,
                              const units::System& units) {
  const auto len = [&](double mm) { return core::FormatLength(mm, units); };
  std::string out = "\xC3\x98" + len(hole.diameter);
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
