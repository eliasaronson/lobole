// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SketchOrientation.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace sketch {

namespace {

constexpr size_t kMaxTriangles = 256;
constexpr size_t kMaxTriangleWork = 20000; // adjacency checks
constexpr size_t kMaxHeldCorners = 32;     // implicit held-held edges
// Nearer flat (or nearer a half turn) than this, solver noise decides the
// sign, so the orientation counts as not clear.
constexpr double kFlatRatio = 1e-3;
constexpr double kHalfTurnTol = 1e-3;

uint64_t Key(const PointRef& r) { return (uint64_t(r.entity) << 8) | r.which; }

struct Geometry {
  const std::vector<Entity>& entities;
  std::unordered_map<uint32_t, size_t> indexOf;
  explicit Geometry(const std::vector<Entity>& e) : entities(e) {
    indexOf.reserve(e.size());
    for (size_t i = 0; i < e.size(); i++)
      indexOf[EntityId(e[i])] = i;
  }
  const Entity* Find(uint32_t id) const {
    const auto it = indexOf.find(id);
    return it == indexOf.end() ? nullptr : &entities[it->second];
  }
  std::optional<glm::dvec2> Point(const PointRef& r) const {
    if (IsDatum(r.entity))
      return DatumPointUV(r.entity, r.which);
    const Entity* e = Find(r.entity);
    if (e == nullptr)
      return std::nullopt;
    return EntityPointUV(*e, r.which);
  }
  // A line's two ends (a datum axis counts).
  std::optional<std::pair<glm::dvec2, glm::dvec2>> LineEnds(uint32_t id) const {
    if (id == kAxisX || id == kAxisY)
      return DatumAxisEndpoints(id);
    const Entity* e = Find(id);
    if (e == nullptr || !std::holds_alternative<Line>(*e))
      return std::nullopt;
    const auto& l = std::get<Line>(*e);
    return std::pair{l.a, l.b};
  }
};

struct UnionFind {
  std::unordered_map<uint64_t, uint64_t> parent;
  uint64_t Find(uint64_t k) {
    auto it = parent.find(k);
    if (it == parent.end()) {
      parent[k] = k;
      return k;
    }
    uint64_t root = k;
    while (parent[root] != root)
      root = parent[root];
    while (parent[k] != root) { // path compression
      const uint64_t next = parent[k];
      parent[k] = root;
      k = next;
    }
    return root;
  }
  void Join(uint64_t a, uint64_t b) {
    a = Find(a), b = Find(b);
    if (a != b)
      parent[a] = b;
  }
};

} // namespace

std::vector<Orientation>
FindOrientations(const std::vector<Entity>& entities,
                 const std::vector<Constraint>& constraints,
                 const PointHeld& held) {
  using K = ConstraintKind;
  std::vector<Orientation> out;
  const auto isHeld = [&](const PointRef& r) {
    return IsDatum(r.entity) || (held && held(r));
  };

  UnionFind uf;
  for (const Constraint& c : constraints)
    if (c.kind == K::Coincident)
      uf.Join(Key(c.a), Key(c.b));

  // --- Triangles --------------------------------------------------------
  // Corners: the groups a Distance touches, numbered in first-seen order so
  // the search is deterministic.
  std::map<uint64_t, int> cornerOf; // group root -> corner number
  std::vector<PointRef> cornerRef;  // a point of each corner
  std::vector<bool> cornerHeld;
  const auto corner = [&](const PointRef& r) {
    const uint64_t g = uf.Find(Key(r));
    auto [it, fresh] = cornerOf.emplace(g, static_cast<int>(cornerRef.size()));
    if (fresh) {
      cornerRef.push_back(r);
      cornerHeld.push_back(false);
    }
    if (isHeld(r))
      cornerHeld[it->second] = true;
    return it->second;
  };
  std::map<std::pair<int, int>, std::vector<size_t>> edges; // (lo, hi)
  for (size_t k = 0; k < constraints.size(); k++) {
    const Constraint& c = constraints[k];
    if (c.kind != K::Distance || c.driven) // a driven one sets nothing
      continue;
    const int a = corner(c.a), b = corner(c.b);
    if (a != b)
      edges[{std::min(a, b), std::max(a, b)}].push_back(k);
  }
  // A held point is a group member too: a corner coincident with a held
  // point is held. (Corners were only marked by their own refs above.)
  for (const auto& [key, root] : uf.parent) {
    const PointRef r{static_cast<uint32_t>(key >> 8),
                     static_cast<uint8_t>(key & 0xFF)};
    if (!isHeld(r))
      continue;
    const auto it = cornerOf.find(uf.Find(key));
    if (it != cornerOf.end())
      cornerHeld[it->second] = true;
  }
  {
    std::vector<int> heldCorners;
    for (size_t i = 0; i < cornerHeld.size(); i++)
      if (cornerHeld[i] && heldCorners.size() < kMaxHeldCorners)
        heldCorners.push_back(static_cast<int>(i));
    for (size_t i = 0; i < heldCorners.size(); i++)
      for (size_t j = i + 1; j < heldCorners.size(); j++)
        edges.try_emplace({heldCorners[i], heldCorners[j]});
  }
  std::vector<std::vector<int>> adj(cornerRef.size());
  for (const auto& [e, ks] : edges) {
    adj[e.first].push_back(e.second);
    adj[e.second].push_back(e.first);
  }
  for (auto& a : adj)
    std::sort(a.begin(), a.end());
  size_t work = 0;
  bool capped = false;
  for (int u = 0; u < static_cast<int>(adj.size()) && !capped; u++) {
    for (size_t i = 0; i < adj[u].size() && !capped; i++) {
      const int v = adj[u][i];
      if (v <= u)
        continue;
      for (size_t j = i + 1; j < adj[u].size(); j++) {
        const int w = adj[u][j];
        if (++work > kMaxTriangleWork || out.size() >= kMaxTriangles) {
          capped = true;
          break;
        }
        if (!edges.count({v, w}))
          continue;
        if (cornerHeld[u] && cornerHeld[v] && cornerHeld[w])
          continue;
        Orientation o;
        o.kind = Orientation::Kind::Triangle;
        o.corners = {cornerRef[u], cornerRef[v], cornerRef[w]};
        for (const auto& e :
             {std::pair{u, v}, std::pair{u, w}, std::pair{v, w}})
          for (size_t k : edges.at(e))
            o.constraints.push_back(k);
        std::sort(o.constraints.begin(), o.constraints.end());
        out.push_back(std::move(o));
      }
    }
  }

  // --- Arc sweeps -------------------------------------------------------
  for (const Entity& e : entities) {
    const auto* arc = std::get_if<Arc>(&e);
    if (arc == nullptr)
      continue;
    const uint32_t id = arc->id;
    if (isHeld({id, 0}) && isHeld({id, 1}) && isHeld({id, 2}))
      continue;
    std::set<uint64_t> groups;
    for (uint8_t w = 0; w <= 2; w++)
      groups.insert(uf.Find(Key({id, w})));
    const auto touches = [&](const PointRef& r) {
      return r.entity == id || groups.count(uf.Find(Key(r))) != 0;
    };
    Orientation o;
    o.kind = Orientation::Kind::ArcSweep;
    o.arc = id;
    for (size_t k = 0; k < constraints.size(); k++) {
      const Constraint& c = constraints[k];
      if (IsDimension(c.kind) && !c.driven && (touches(c.a) || touches(c.b)))
        o.constraints.push_back(k);
    }
    out.push_back(std::move(o));
  }
  return out;
}

std::vector<int> OrientationSigns(const std::vector<Orientation>& orientations,
                                  const std::vector<Entity>& entities) {
  const Geometry geo(entities);
  std::vector<int> signs;
  signs.reserve(orientations.size());
  for (const Orientation& o : orientations) {
    int s = 0;
    if (o.kind == Orientation::Kind::Triangle) {
      const auto a = geo.Point(o.corners[0]);
      const auto b = geo.Point(o.corners[1]);
      const auto c = geo.Point(o.corners[2]);
      if (a && b && c) {
        const glm::dvec2 ab = *b - *a, ac = *c - *a;
        const double cross = ab.x * ac.y - ab.y * ac.x;
        const double longest =
            std::max({glm::length(ab), glm::length(ac), glm::length(*c - *b)});
        // height over the longest side = |cross| / longest
        if (longest > 0.0 && std::abs(cross) > kFlatRatio * longest * longest)
          s = cross > 0.0 ? 1 : -1;
      }
    } else if (const Entity* e = geo.Find(o.arc)) {
      if (const auto* arc = std::get_if<Arc>(e)) {
        double sweep = std::fmod(arc->endAngle - arc->startAngle, 2.0 * M_PI);
        if (sweep <= 0.0)
          sweep += 2.0 * M_PI;
        if (sweep < M_PI - kHalfTurnTol)
          s = 1;
        else if (sweep > M_PI + kHalfTurnTol)
          s = -1;
      }
    }
    signs.push_back(s);
  }
  return signs;
}

std::optional<double> MeasureDimension(const std::vector<Entity>& entities,
                                       const Constraint& c) {
  using K = ConstraintKind;
  const Geometry geo(entities);
  switch (c.kind) {
  case K::Distance:
  case K::DistanceX:
  case K::DistanceY: {
    const auto a = geo.Point(c.a), b = geo.Point(c.b);
    if (!a || !b)
      return std::nullopt;
    if (c.kind == K::Distance)
      return glm::length(*b - *a);
    return c.kind == K::DistanceX ? b->x - a->x : b->y - a->y;
  }
  case K::DistancePL: {
    const auto p = geo.Point(c.a);
    const auto l = geo.LineEnds(c.b.entity);
    if (!p || !l)
      return std::nullopt;
    const glm::dvec2 d = l->second - l->first;
    const double len = glm::length(d);
    if (len <= 0.0)
      return std::nullopt;
    const glm::dvec2 r = *p - l->first;
    return std::abs(d.x * r.y - d.y * r.x) / len;
  }
  case K::Radius:
  case K::Diameter: {
    const Entity* e = geo.Find(c.a.entity);
    if (e == nullptr)
      return std::nullopt;
    double r = 0.0;
    if (const auto* ci = std::get_if<Circle>(e))
      r = ci->radius;
    else if (const auto* ar = std::get_if<Arc>(e))
      r = ar->radius;
    else
      return std::nullopt;
    return c.kind == K::Diameter ? 2.0 * r : r;
  }
  case K::ArcLength: {
    const Entity* e = geo.Find(c.a.entity);
    const auto* ar = e ? std::get_if<Arc>(e) : nullptr;
    if (ar == nullptr)
      return std::nullopt;
    double sweep = std::fmod(ar->endAngle - ar->startAngle, 2.0 * M_PI);
    if (sweep <= 0.0)
      sweep += 2.0 * M_PI;
    return ar->radius * sweep;
  }
  case K::Angle: {
    // PlaneGCS's L2LAngle is met when line b is turned `value` from line a.
    const auto la = geo.LineEnds(c.a.entity);
    const auto lb = geo.LineEnds(c.b.entity);
    if (!la || !lb)
      return std::nullopt;
    const glm::dvec2 da = la->second - la->first;
    const glm::dvec2 db = lb->second - lb->first;
    return std::atan2(da.x * db.y - da.y * db.x, glm::dot(da, db));
  }
  default:
    return std::nullopt;
  }
}

std::optional<double> MeasureDimension(const SketchFeature& sketch,
                                       size_t constraint) {
  if (constraint >= sketch.constraints.size())
    return std::nullopt;
  return MeasureDimension(sketch.entities, sketch.constraints[constraint]);
}

} // namespace sketch
