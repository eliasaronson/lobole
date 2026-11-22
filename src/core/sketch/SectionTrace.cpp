// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SectionTrace.h"

#include "core/Angles.h"
#include "core/sketch/SketchSection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace sketch {

TraceResult TraceSection(SketchFeature& sk, size_t extIndex, double detail) {
  TraceResult out;
  if (extIndex >= sk.external.size() ||
      sk.external[extIndex].kind != ExternalKind::Section) {
    out.error = "external " + std::to_string(extIndex) +
                " of the sketch is not a section";
    return out;
  }
  const ExternalGeom& ext = sk.external[extIndex];
  if (ext.cut.empty()) {
    out.error = "the section has no cut to trace (the sketch's plane cuts "
                "none of its bodies, or the sketch has not regenerated)";
    return out;
  }
  // A copy: adding entities may not move `ext`, but the cut is read whole
  // first anyway.
  const std::vector<CutChain> cut = ext.cut;
  const double tol = ext.usedToleranceMm;
  for (size_t c = 0; c < cut.size(); c++) {
    if (cut[c].points.size() < 2)
      continue;
    PiecesFit f = FitPieces(cut[c], static_cast<int>(c), tol, detail);
    out.deviation = std::max(out.deviation, f.deviation);
    std::vector<uint32_t> ids;
    for (size_t k = 0; k < f.entities.size(); k++) {
      SetConstruction(f.entities[k], false);
      const uint32_t id = sk.AddEntity(std::move(f.entities[k]));
      ids.push_back(id);
      sk.traced.push_back({id, static_cast<uint32_t>(extIndex),
                           static_cast<uint32_t>(c), f.pieces[k].from,
                           f.pieces[k].to});
      out.ids.push_back(id);
      out.pieces.push_back(f.pieces[k]);
    }
    // Coincident where consecutive pieces meet, and round a closed chain.
    const size_t n = ids.size();
    const size_t joints = cut[c].closed ? (n >= 2 ? n : 0) : (n ? n - 1 : 0);
    for (size_t k = 0; k < joints; k++) {
      const size_t next = (k + 1) % n;
      Constraint con{ConstraintKind::Coincident,
                     PointRef{ids[k], f.ends[k].second},
                     PointRef{ids[next], f.ends[next].first}, 0.0};
      sk.constraints.push_back(con);
      out.coincident++;
    }
  }
  if (out.ids.empty())
    out.error = "the section's cut has no chain long enough to trace";
  return out;
}

namespace {

// The distance from p to the entity, and whether its foot lies within the
// entity's own extent (a line's segment, an arc's sweep).
bool Foot(const Entity& e, const glm::dvec2& p, double& dist) {
  if (const auto* l = std::get_if<Line>(&e)) {
    const glm::dvec2 ab = l->b - l->a;
    const double len2 = glm::dot(ab, ab);
    if (!(len2 > 0.0))
      return false;
    const double t = glm::dot(p - l->a, ab) / len2;
    dist = std::abs((ab.x * (p.y - l->a.y) - ab.y * (p.x - l->a.x)) /
                    std::sqrt(len2));
    return t >= 0.0 && t <= 1.0;
  }
  if (const auto* c = std::get_if<Circle>(&e)) {
    dist = std::abs(glm::length(p - c->center) - c->radius);
    return true;
  }
  if (const auto* a = std::get_if<Arc>(&e)) {
    dist = std::abs(glm::length(p - a->center) - a->radius);
    double t = std::atan2(p.y - a->center.y, p.x - a->center.x) - a->startAngle;
    t = std::fmod(t, angles::kTwoPi);
    if (t < 0.0)
      t += angles::kTwoPi;
    return t <= a->endAngle - a->startAngle;
  }
  return false;
}

glm::dvec2 Middle(const Entity& e) {
  if (const auto* l = std::get_if<Line>(&e))
    return 0.5 * (l->a + l->b);
  if (const auto* c = std::get_if<Circle>(&e))
    return c->center + glm::dvec2(c->radius, 0.0);
  if (const auto* a = std::get_if<Arc>(&e)) {
    const double m = 0.5 * (a->startAngle + a->endAngle);
    return a->center + a->radius * glm::dvec2(std::cos(m), std::sin(m));
  }
  return glm::dvec2(0.0);
}

} // namespace

std::vector<TracedMiss> SectionFitReport(const SketchFeature& sk) {
  std::vector<TracedMiss> out;
  // Per (section, chain): the records measured against it.
  std::map<std::pair<uint32_t, size_t>, std::vector<size_t>> onChain;
  for (const TracedFrom& t : sk.traced) {
    const size_t idx = sk.IndexOfEntity(t.entity);
    if (idx == std::numeric_limits<size_t>::max() ||
        t.section >= sk.external.size() ||
        sk.external[t.section].kind != ExternalKind::Section)
      continue;
    TracedMiss m;
    m.from = t;
    const ExternalGeom& ext = sk.external[t.section];
    const glm::dvec2 mid = Middle(sk.entities[idx]);
    double best = std::numeric_limits<double>::infinity();
    size_t chain = SIZE_MAX;
    for (size_t c = 0; c < ext.cut.size(); c++)
      for (const glm::dvec2& p : ext.cut[c].points)
        if (const double d = glm::length(p - mid); d < best) {
          best = d;
          chain = c;
        }
    out.push_back(m);
    if (chain != SIZE_MAX)
      onChain[{t.section, chain}].push_back(out.size() - 1);
  }
  for (const auto& [key, rows] : onChain) {
    const ExternalGeom& ext = sk.external[key.first];
    // As the fit sees it: an exact cut's straight edge filled in, so its
    // middle has points to stand on.
    const CutChain chain = EvenedChain(ext.cut[key.second]);
    const ChainNoise noise = MeasureNoise(
        chain, ext.usedToleranceMm * PieceThresholds::kNoiseFloorShare);
    std::vector<std::vector<size_t>> at(rows.size());
    for (size_t k = 0; k < chain.points.size(); k++) {
      const glm::dvec2& p = chain.points[k];
      double best = std::numeric_limits<double>::infinity();
      size_t which = SIZE_MAX;
      for (size_t r = 0; r < rows.size(); r++) {
        const Entity& e =
            sk.entities[sk.IndexOfEntity(out[rows[r]].from.entity)];
        double d = 0.0;
        if (Foot(e, p, d) && d < best) {
          best = d;
          which = r;
        }
      }
      if (which != SIZE_MAX)
        at[which].push_back(k);
    }
    const size_t n = chain.points.size();
    for (size_t r = 0; r < rows.size(); r++) {
      TracedMiss& m = out[rows[r]];
      m.noiseMm = noise.sigma;
      if (at[r].size() < 2)
        continue;
      // In the chain's order from after its widest gap, so a span over a
      // closed chain's first point is read in one piece.
      size_t start = 0, widest = 0;
      for (size_t q = 0; q < at[r].size(); q++) {
        const size_t next = at[r][(q + 1) % at[r].size()];
        const size_t gap = (next + n - at[r][q]) % n;
        if (chain.closed && gap > widest) {
          widest = gap;
          start = (q + 1) % at[r].size();
        }
      }
      std::vector<glm::dvec2> pts;
      for (size_t q = 0; q < at[r].size(); q++)
        pts.push_back(chain.points[at[r][(start + q) % at[r].size()]]);
      const Entity& e = sk.entities[sk.IndexOfEntity(m.from.entity)];
      m.cut = true;
      m.miss = MeasureMiss(e, pts, noise, ext.detail);
    }
  }
  return out;
}

} // namespace sketch
