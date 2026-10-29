// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// check_print over mesh bodies (PrintCheck.h, CheckMeshes): each item read
// off the triangles, which are a mesh body's geometry.

#include "core/PrintCheck.h"

#include "core/Feature.h"
#include "core/mesh/MeshBvh.h"
#include "core/mesh/TriMesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace printcheck {

namespace {

constexpr double kPi = 3.14159265358979323846;

double OverhangDegOf(const glm::dvec3& n, const glm::dvec3& up) {
  return std::asin(std::clamp(-glm::dot(n, up), -1.0, 1.0)) * 180.0 / kPi;
}

} // namespace

Report CheckMeshes(const std::vector<const Body*>& bodies, const Options& o) {
  if (bodies.empty())
    throw std::runtime_error("no bodies to check");
  if (!(glm::length(o.up) > 0.5) || !std::isfinite(o.up.x + o.up.y + o.up.z))
    throw std::runtime_error("the print direction is not a unit vector");
  if (!(o.maxOverhangDeg > 0.0 && o.maxOverhangDeg < 90.0))
    throw std::runtime_error("the overhang limit is not in (0, 90)");
  for (const Body* b : bodies)
    if (b == nullptr || !b->IsMesh() || !b->mesh->Closed())
      throw std::runtime_error("a body is not a closed mesh");
  const glm::dvec3 up = glm::normalize(o.up);
  const int n = static_cast<int>(bodies.size());
  Report r;
  r.bodies.resize(n);

  // Bed: the lowest vertex along up.
  double bed = std::numeric_limits<double>::infinity();
  int units = 0;
  for (int i = 0; i < n; ++i) {
    const mesh::TriMesh& m = *bodies[i]->mesh;
    BodyInfo& bi = r.bodies[i];
    std::vector<uint32_t> componentOf;
    bi.solids = static_cast<int>(mesh::Components(m, componentOf));
    bi.watertight = m.Closed();
    bi.freeEdges = static_cast<int>(m.boundaryEdges);
    bi.lowestMm = std::numeric_limits<double>::infinity();
    for (const glm::dvec3& p : m.positions)
      bi.lowestMm = std::min(bi.lowestMm, glm::dot(p, up));
    bed = std::min(bed, bi.lowestMm);
    units += bi.solids;
    if (bi.solids > 1)
      r.shells.split.push_back(i);
  }
  r.bed.lowestMm = bed;
  for (int i = 0; i < n; ++i)
    if (r.bodies[i].lowestMm <= bed + kBedTolMm)
      r.bed.onBed.push_back(i);
  r.bed.status = std::abs(bed) <= kBedTolMm ? Status::Pass : Status::Fail;

  // Overhang: every triangle by its own normal, off the bed.
  r.overhang.limitDeg = o.maxOverhangDeg;
  r.overhang.allowanceMm2 = o.overhangAllowanceMm2;
  for (int i = 0; i < n; ++i) {
    const mesh::TriMesh& m = *bodies[i]->mesh;
    FaceOver fo;
    fo.where.body = i;
    fo.where.face = 0; // the mesh body's one element
    for (uint32_t t = 0; t < m.triangles.size(); t++) {
      const glm::dvec3 an = m.AreaNormal(t);
      const double len = glm::length(an);
      if (len <= 0.0)
        continue;
      bool onBed = true;
      for (int c = 0; c < 3; c++)
        onBed = onBed && glm::dot(m.Corner(t, c), up) <= bed + kBedTolMm;
      if (onBed) {
        if (glm::dot(an / len, up) < -0.999)
          r.bed.contactAreaMm2 += 0.5 * len;
        continue;
      }
      const double deg = OverhangDegOf(an / len, up);
      const glm::dvec3 mid =
          (m.Corner(t, 0) + m.Corner(t, 1) + m.Corner(t, 2)) / 3.0;
      if (deg > r.overhang.worstDeg) {
        r.overhang.worstDeg = deg;
        r.overhang.worst = Where{i, 0, mid};
      }
      if (deg > o.maxOverhangDeg + kAngleEpsDeg) {
        fo.areaMm2 += 0.5 * len;
        if (deg > fo.worstDeg) {
          fo.worstDeg = deg;
          fo.where.point = mid;
        }
      }
    }
    if (fo.areaMm2 > 0.0) {
      r.overhang.areaMm2 += fo.areaMm2;
      r.overhang.faces.push_back(fo);
    }
  }
  std::sort(r.overhang.faces.begin(), r.overhang.faces.end(),
            [](const FaceOver& a, const FaceOver& b) {
              return a.worstDeg > b.worstDeg;
            });
  r.overhang.status =
      r.overhang.areaMm2 > o.overhangAllowanceMm2 ? Status::Fail : Status::Pass;
  r.overhang.note = "a mesh body's triangles, each by its own normal: a "
                    "ceiling counts as a 90° overhang";

  r.bridges.limitMm = o.maxBridgeMm;
  r.bridges.note = "not measured on a mesh body: a span is read off a "
                   "ceiling face's surface, which a mesh does not have (its "
                   "ceilings are in the overhang)";

  // Clearance and pieces: every pair of bodies by their trees, exact to the
  // triangles. A body's own components are separate pieces.
  const double required = o.gapSideMm.value_or(0.0);
  std::vector<std::shared_ptr<const mesh::Bvh>> trees;
  for (const Body* b : bodies)
    trees.push_back(mesh::Bvh::For(b->mesh));
  int joinedPairs = 0;
  std::vector<int> parent(n);
  std::iota(parent.begin(), parent.end(), 0);
  const auto find = [&](int x) {
    while (parent[x] != x)
      x = parent[x] = parent[parent[x]];
    return x;
  };
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j) {
      Pair pr;
      pr.a = i;
      pr.b = j;
      pr.distanceMm = trees[i]->Distance(*trees[j], glm::dmat4(1.0)).distance;
      pr.overlap =
          pr.distanceMm <= 0.0 &&
          (trees[j]->Winding(bodies[i]->mesh->positions.front()) > 0.5 ||
           trees[i]->Winding(bodies[j]->mesh->positions.front()) > 0.5);
      if (pr.distanceMm <= kTouchMm && find(i) != find(j)) {
        parent[find(i)] = find(j);
        joinedPairs++;
      }
      r.clearance.pairs.push_back(pr);
    }
  std::sort(
      r.clearance.pairs.begin(), r.clearance.pairs.end(),
      [](const Pair& a, const Pair& b) { return a.distanceMm < b.distanceMm; });
  if (!r.clearance.pairs.empty())
    r.clearance.minMm = r.clearance.pairs.front().distanceMm;
  std::vector<std::vector<int>> groups(n);
  for (int i = 0; i < n; ++i)
    groups[find(i)].push_back(i);
  for (const auto& g : groups)
    if (g.size() > 1)
      r.shells.joined.push_back(g);
  r.shells.watertight = true;
  for (const BodyInfo& bi : r.bodies)
    r.shells.watertight = r.shells.watertight && bi.watertight;
  r.shells.pieces = units - joinedPairs;
  r.shells.expected = units;
  r.shells.status = r.shells.pieces == r.shells.expected && r.shells.watertight
                        ? Status::Pass
                        : Status::Fail;

  if (!o.gapSideMm) {
    r.clearance.note = "the part is not printed: no gap to hold the bodies "
                       "apart (set_print_process, or state min_gap_mm)";
  } else {
    r.clearance.requiredMm = required;
    r.clearance.toleranceMm = kGapToleranceShare * required;
    const bool clear = !r.clearance.minMm ||
                       *r.clearance.minMm >= required - r.clearance.toleranceMm;
    r.clearance.status = clear ? Status::Pass : Status::Fail;
    r.clearance.note = n < 2 ? "one body: nothing to keep apart"
                             : "every pair of mesh bodies, exact to their "
                               "triangles";
  }
  r.chords.note = "a mesh body's facets are its geometry: it is written as "
                  "it is, with no chords to judge";
  r.stacking.note = "not measured on a mesh body: it reads a B-Rep's "
                    "surfaces";
  r.slender.note = "not measured on a mesh body: it reads a B-Rep's "
                   "footprint faces";

  if (o.bedMm)
    r.bedFit = FitOnBed(bodies, up, *o.bedMm, o.bedExcludeMm);
  else
    r.bedFit.note = o.bedNote.empty()
                        ? std::string("no build volume to fit the plate in")
                        : o.bedNote;
  return r;
}

} // namespace printcheck
