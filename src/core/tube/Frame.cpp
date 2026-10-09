// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/tube/Frame.h"

#include "core/Angles.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace tube {
namespace {

std::string Mm(double v) {
  std::ostringstream s;
  s.precision(1);
  s << std::fixed << v;
  return s.str();
}

// The closest approach of `p` to a member's centreline, and how far along it
// that lands. The polyline IS the metal — sampled from the curve the feature
// built, arcs and all — so this needs no knowledge of bends.
struct Hit {
  double distance = 0.0;
  double at = 0.0; // 0..1 along the polyline, by segment count
  bool valid = false;
};

Hit Closest(const std::vector<glm::dvec3>& line, const glm::dvec3& p) {
  Hit best;
  if (line.size() < 2)
    return best;
  double run = 0.0, total = 0.0;
  for (size_t i = 0; i + 1 < line.size(); i++)
    total += glm::length(line[i + 1] - line[i]);
  if (total <= 0.0)
    return best;
  for (size_t i = 0; i + 1 < line.size(); i++) {
    const glm::dvec3 ab = line[i + 1] - line[i];
    const double len = glm::length(ab);
    const double t =
        len < 1e-12
            ? 0.0
            : std::clamp(glm::dot(p - line[i], ab) / (len * len), 0.0, 1.0);
    const double d = glm::length(p - (line[i] + t * ab));
    if (!best.valid || d < best.distance) {
      best = {d, (run + t * len) / total, true};
    }
    run += len;
  }
  return best;
}

} // namespace

CheckResult CheckFrame(const std::vector<FrameMember>& members) {
  CheckResult out;
  out.membersChecked = static_cast<int>(members.size());
  for (const FrameMember& m : members)
    out.bendsChecked += static_cast<int>(m.bends.size());

  out.notChecked.emplace_back(
      "how STIFF the frame is: that is a beam model's question and this "
      "build meshes solids, so an empty violations list is not a claim about "
      "torsional stiffness");
  out.notChecked.emplace_back(
      "whether a PANEL braces a bay: a floor or a firewall triangulates the "
      "bay it spans as surely as a diagonal does, and nothing here can see "
      "one");

  // --- the joints ------------------------------------------------------
  // A node is where member ends meet. Ends within a tube's width of each
  // other are the same joint.
  struct Node {
    glm::dvec3 p{0.0};
    int count = 0;
  };
  std::vector<Node> nodes;
  double widest = 0.0;
  for (const FrameMember& m : members)
    widest = std::max(widest, m.outerDiameter);
  const double weld = std::max(widest, 1.0);

  const auto nodeFor = [&](const glm::dvec3& p) {
    for (size_t i = 0; i < nodes.size(); i++) {
      if (glm::length(nodes[i].p - p) < weld)
        return i;
    }
    nodes.push_back({p, 0});
    return nodes.size() - 1;
  };

  for (const FrameMember& m : members) {
    if (m.centreline.size() < 2)
      continue;
    for (const glm::dvec3& end : {m.centreline.front(), m.centreline.back()})
      nodes[nodeFor(end)].count++;
  }

  // --- 1. a member welded to nothing at one end ------------------------
  for (const FrameMember& m : members) {
    if (m.centreline.size() < 2)
      continue;
    const double rm = m.outerDiameter * 0.5;
    for (const glm::dvec3& end : {m.centreline.front(), m.centreline.back()}) {
      bool held = false;
      for (const FrameMember& o : members) {
        if (&o == &m)
          continue;
        const Hit h = Closest(o.centreline, end);
        if (h.valid && h.distance < rm + o.outerDiameter * 0.5) {
          held = true;
          break;
        }
      }
      if (!held) {
        out.violations.push_back(
            {"member_end_unattached",
             m.name + " is welded to nothing at one end — a member carried at "
                      "one end is a lever, and a space frame has no levers",
             m.name, 0, 0.0, 0.0});
      }
    }
  }

  // --- 2. a joint landing on a neighbour's bend ------------------------
  // The tube there is a torus, thinned by the bending, with no straight to
  // fit against. A bend reaches radius*tan(angle/2) either side of the
  // corner it was drawn from.
  for (const FrameMember& m : members) {
    if (m.centreline.size() < 2)
      continue;
    const double rm = m.outerDiameter * 0.5;
    for (const glm::dvec3& end : {m.centreline.front(), m.centreline.back()}) {
      for (const FrameMember& o : members) {
        if (&o == &m)
          continue;
        for (size_t b = 0; b < o.bends.size() && b < o.corners.size(); b++) {
          const double tangent =
              o.bends[b].radius *
              std::tan(angles::ToRadians(o.bends[b].angleDeg) * 0.5);
          const double d = glm::length(end - o.corners[b]);
          if (d < tangent + rm) {
            out.violations.push_back(
                {"joint_on_a_bend",
                 m.name + " joins " + o.name + " on its bend " +
                     std::to_string(b + 1) + ", which reaches " + Mm(tangent) +
                     " mm either side of that corner — move the joint clear "
                     "of it",
                 m.name, static_cast<int>(b) + 1, d, tangent + rm});
          }
        }
      }
    }
  }

  // --- 3. a bay with nothing across it ---------------------------------
  // Edges: a member is cut at every node that lies on it, so a T-joint makes
  // two edges out of one tube and the graph is the frame a pin-jointed
  // analysis would see.
  std::set<std::pair<size_t, size_t>> edges;
  for (const FrameMember& m : members) {
    if (m.centreline.size() < 2)
      continue;
    std::vector<std::pair<double, size_t>> on;
    for (size_t i = 0; i < nodes.size(); i++) {
      const Hit h = Closest(m.centreline, nodes[i].p);
      if (h.valid && h.distance < weld)
        on.push_back({h.at, i});
    }
    std::sort(on.begin(), on.end());
    for (size_t i = 0; i + 1 < on.size(); i++) {
      if (on[i].second == on[i + 1].second)
        continue;
      edges.insert({std::min(on[i].second, on[i + 1].second),
                    std::max(on[i].second, on[i + 1].second)});
    }
  }
  std::map<size_t, std::set<size_t>> adj;
  for (const auto& [a, b] : edges) {
    adj[a].insert(b);
    adj[b].insert(a);
  }
  const auto joined = [&](size_t a, size_t b) {
    return edges.count({std::min(a, b), std::max(a, b)}) > 0;
  };

  // A four-bar loop u-v-w-x with neither diagonal present. In a pin-jointed
  // frame that is a mechanism whatever its shape — four joints and four bars
  // in space leave two degrees of freedom — so no coplanarity test is
  // needed, and a welded joint only turns the mechanism into bending.
  // A bay has two diagonals, and the loop below reaches it from either of
  // them, so it is remembered by its four corners rather than counted twice.
  std::set<std::vector<size_t>> seen;
  int bays = 0;
  for (const auto& [u, nu] : adj) {
    for (const auto& [w, nw] : adj) {
      if (w <= u || joined(u, w))
        continue;
      std::vector<size_t> shared;
      std::set_intersection(nu.begin(), nu.end(), nw.begin(), nw.end(),
                            std::back_inserter(shared));
      for (size_t i = 0; i < shared.size(); i++) {
        for (size_t j = i + 1; j < shared.size(); j++) {
          if (joined(shared[i], shared[j]))
            continue; // the other diagonal is there: the bay is braced
          // A BAY is a face — four members round a roughly flat loop, which
          // is what a shear panel or a diagonal would close. A skew
          // four-cycle is a mechanism too in pin-jointed theory, but as a
          // FINDING it is noise: a dense frame has combinatorially many, and
          // a check that reports forty of them on a frame somebody just
          // triangulated teaches nothing. Flat to 5% of its own diagonal.
          const glm::dvec3& a = nodes[u].p;
          const glm::dvec3& b = nodes[shared[i]].p;
          const glm::dvec3& c = nodes[w].p;
          const glm::dvec3& e = nodes[shared[j]].p;
          const glm::dvec3 n = glm::cross(c - a, b - a);
          const double area = glm::length(n);
          const double span = std::max(glm::length(c - a), glm::length(e - b));
          if (area < 1e-9 || span < 1e-9)
            continue;
          const double out_of_plane =
              std::abs(glm::dot(glm::normalize(n), e - a));
          if (out_of_plane > 0.05 * span)
            continue;
          std::vector<size_t> corners = {u, w, shared[i], shared[j]};
          std::sort(corners.begin(), corners.end());
          if (!seen.insert(corners).second)
            continue;
          bays++;
          if (bays <= 12) {
            out.violations.push_back(
                {"bay_not_triangulated",
                 "a four-member bay has no diagonal across it: it carries its "
                 "load by BENDING its welds, which is what a space frame "
                 "exists not to do. Its corners are near (" +
                     Mm(nodes[u].p.x) + ", " + Mm(nodes[u].p.y) + ", " +
                     Mm(nodes[u].p.z) + ") and (" + Mm(nodes[w].p.x) + ", " +
                     Mm(nodes[w].p.y) + ", " + Mm(nodes[w].p.z) + ")",
                 {},
                 0,
                 0.0,
                 0.0});
          }
        }
      }
    }
  }
  if (bays > 12) {
    out.notChecked.emplace_back(
        "another " + std::to_string(bays - 12) +
        " untriangulated bays beyond the first twelve, not listed one by one");
  }
  return out;
}

} // namespace tube
