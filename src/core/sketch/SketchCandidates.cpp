// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SketchCandidates.h"

#include "core/Angles.h"
#include "core/sketch/SectionTrace.h"
#include "core/sketch/SketchSolver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace sketch {

SizeSteps StepsFor(units::Length unit) {
  // The one table. A metric part is made to millimetres whatever it is
  // shown in; an inch part to 32nds below an inch and 16ths above.
  switch (unit) {
  case units::Length::Inch: {
    const double in = units::MmPerLength(units::Length::Inch);
    return {1.0 * in, in / 32.0, in / 16.0, in / 32.0};
  }
  case units::Length::Millimetre:
  case units::Length::Centimetre:
  case units::Length::Metre:
    break;
  }
  return {10.0, 0.5, 1.0, 0.5};
}

const char* ToString(CandidateKind k) {
  using K = CandidateKind;
  switch (k) {
  case K::Horizontal:
    return "horizontal";
  case K::Vertical:
    return "vertical";
  case K::Parallel:
    return "parallel";
  case K::Perpendicular:
    return "perpendicular";
  case K::Collinear:
    return "collinear";
  case K::OnAxis:
    return "on_axis";
  case K::EqualLength:
    return "equal_length";
  case K::EqualRadius:
    return "equal_radius";
  case K::Concentric:
    return "concentric";
  case K::AtOrigin:
    return "at_origin";
  case K::Tangent:
    return "tangent";
  case K::Symmetric:
    return "symmetric";
  case K::Length:
    return "length";
  case K::Distance:
    return "distance";
  case K::Radius:
    return "radius";
  case K::Diameter:
    return "diameter";
  case K::Angle:
    return "angle";
  }
  return "?";
}

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

double Cross(const glm::dvec2& a, const glm::dvec2& b) {
  return a.x * b.y - a.y * b.x;
}

double Deg(double rad) { return rad * 180.0 / angles::kPi; }

// An angle taken into (-pi/2, pi/2]: the difference of two undirected
// lines.
double FoldHalf(double a) {
  a = std::fmod(a, angles::kPi);
  if (a > angles::kPi / 2)
    a -= angles::kPi;
  if (a <= -angles::kPi / 2)
    a += angles::kPi;
  return a;
}

const Entity* ById(const SketchFeature& sk, uint32_t id) {
  const size_t i = sk.IndexOfEntity(id);
  return i == std::numeric_limits<size_t>::max() ? nullptr : &sk.entities[i];
}

bool IsRound(const Entity& e) {
  return std::holds_alternative<Arc>(e) || std::holds_alternative<Circle>(e);
}

// A joint between two traced entities: a Coincident of an end of one with
// an end of the other.
struct Joint {
  uint32_t a = 0, b = 0;
  uint8_t wa = 0, wb = 0;
};

std::vector<Joint> Joints(const SketchFeature& sk,
                          const std::set<uint32_t>& traced) {
  std::vector<Joint> out;
  for (const Constraint& c : sk.constraints)
    if (c.kind == ConstraintKind::Coincident && c.a.entity != c.b.entity &&
        traced.count(c.a.entity) && traced.count(c.b.entity))
      out.push_back({c.a.entity, c.b.entity, c.a.which, c.b.which});
  return out;
}

// The line's points ordered so they end (before = true) or start at `at`.
std::vector<glm::dvec2> Toward(std::vector<glm::dvec2> pts,
                               const glm::dvec2& at, bool before) {
  if (pts.size() < 2)
    return pts;
  const bool frontNearer =
      glm::length(pts.front() - at) < glm::length(pts.back() - at);
  if (frontNearer == before)
    std::reverse(pts.begin(), pts.end());
  return pts;
}

} // namespace

std::vector<TracedRefit> RefitTraced(const SketchFeature& sk) {
  std::vector<TracedRefit> out;
  std::map<uint32_t, size_t> at;
  for (const TracedSpan& t : TracedSpans(sk)) {
    const Entity* e = ById(sk, t.from.entity);
    if (e == nullptr)
      continue;
    TracedRefit r;
    r.entity = t.from.entity;
    r.pts = t.pts;
    r.noise = t.noise;
    r.floorMm = t.floorMm;
    const bool round = IsRound(*e);
    if (t.cut && (std::holds_alternative<Line>(*e) || round)) {
      r.fit = FitSpan(t.pts, round, t.floorMm);
      r.ok = r.fit.ok && r.fit.arc == round;
    }
    at[r.entity] = out.size();
    out.push_back(std::move(r));
  }
  // An arc beside traced lines, held tangent to each where that holds.
  std::set<uint32_t> traced;
  for (const auto& [id, i] : at)
    traced.insert(id);
  const std::vector<Joint> joints = Joints(sk, traced);
  for (TracedRefit& r : out) {
    const Entity* e = ById(sk, r.entity);
    if (!r.ok || e == nullptr || !std::holds_alternative<Arc>(*e) ||
        r.pts.size() < 2)
      continue;
    RoundSide side[2];
    for (const Joint& j : joints) {
      const bool mine = j.a == r.entity || j.b == r.entity;
      if (!mine)
        continue;
      const uint32_t other = j.a == r.entity ? j.b : j.a;
      const TracedRefit& o = out[at[other]];
      const Entity* oe = ById(sk, other);
      if (!o.ok || oe == nullptr || !std::holds_alternative<Line>(*oe))
        continue;
      // Which end of the arc's points the line meets.
      const glm::dvec2 meet = EntityPointUV(*e, j.a == r.entity ? j.wa : j.wb);
      const int s =
          glm::length(meet - r.pts.front()) < glm::length(meet - r.pts.back())
              ? 0
              : 1;
      side[s].pts = Toward(o.pts, meet, s == 0);
      side[s].p0 = o.fit.p0;
      side[s].dir = o.fit.dir;
    }
    bool keep[2] = {false, false};
    for (int s = 0; s < 2; s++) {
      if (side[s].pts.size() < 2)
        continue;
      RoundSide one[2];
      one[s] = side[s];
      one[s].tangent = true;
      const RoundFit f = FitTangentRound(r.pts, r.fit.centre, r.fit.radius,
                                         one[0], one[1], r.floorMm);
      keep[s] = f.ok && f.tangentSigmas < PieceThresholds::kTangentSigmas;
    }
    if (!keep[0] && !keep[1])
      continue;
    side[0].tangent = keep[0];
    side[1].tangent = keep[1];
    r.held = FitTangentRound(r.pts, r.fit.centre, r.fit.radius, side[0],
                             side[1], r.floorMm);
  }
  return out;
}

namespace {

// One traced entity as the candidates read it: its refit, oriented as it
// is drawn.
struct Piece {
  uint32_t id = 0;
  bool line = false, circle = false;
  const TracedRefit* refit = nullptr;
  // A line: its direction (as drawn), angle sigma, a point on it and the
  // offset sigma there; its drawn ends and length, and that length's sigma.
  glm::dvec2 dir{1.0, 0.0}, p0{0.0}, a{0.0}, b{0.0};
  double angleSigma = kInf, offsetSigma = kInf, length = 0.0,
         lengthSigma = kInf;
  // A round: centre, radius and their sigmas (the held fit's where held).
  glm::dvec2 centre{0.0}, centreSigma{kInf};
  double radius = 0.0, radiusSigma = kInf;
  // Drawn: its middle and its normal there.
  glm::dvec2 mid{0.0}, normal{0.0, 1.0};
  // A traced arc's drawn sweep, for the facing test.
  double start = 0.0, sweep = 0.0;
};

double Norm(const glm::dvec2& s) { return std::sqrt(s.x * s.x + s.y * s.y); }

// The sigma of where a line's end lies along it, from the piece it meets
// there.
double EndSigma(const Piece& line, const glm::dvec2& end, const Piece* nb) {
  if (nb == nullptr)
    return kInf;
  if (nb->line) {
    const double s = std::abs(Cross(line.dir, nb->dir));
    if (s < 0.1)
      return kInf;
    const double lever = glm::length(end - nb->p0);
    const double perp = std::sqrt(nb->offsetSigma * nb->offsetSigma +
                                  std::pow(nb->angleSigma * lever, 2));
    return perp / s;
  }
  return std::sqrt(std::pow(nb->centreSigma.x * line.dir.x, 2) +
                   std::pow(nb->centreSigma.y * line.dir.y, 2));
}

// The first traced piece a ray from `from` along `dir` reaches, and how far.
double RayHit(const Piece& p, const Entity& e, const glm::dvec2& from,
              const glm::dvec2& dir) {
  double best = kInf;
  if (const auto* l = std::get_if<Line>(&e)) {
    const glm::dvec2 ab = l->b - l->a;
    const double den = Cross(dir, ab);
    if (std::abs(den) < 1e-12)
      return kInf;
    const glm::dvec2 w = l->a - from;
    const double t = Cross(w, ab) / den, u = Cross(w, dir) / den;
    return t > 0.0 && u >= 0.0 && u <= 1.0 ? t : kInf;
  }
  const glm::dvec2 c = p.centre;
  const double r = p.radius;
  const glm::dvec2 f = from - c;
  const double bq = glm::dot(f, dir), cq = glm::dot(f, f) - r * r;
  const double disc = bq * bq - cq;
  if (disc < 0.0)
    return kInf;
  for (const double t : {-bq - std::sqrt(disc), -bq + std::sqrt(disc)}) {
    if (!(t > 0.0) || t >= best)
      continue;
    if (!p.circle) {
      const glm::dvec2 q = from + dir * t - c;
      double ang = std::fmod(std::atan2(q.y, q.x) - p.start, angles::kTwoPi);
      if (ang < 0.0)
        ang += angles::kTwoPi;
      if (ang > p.sweep)
        continue;
    }
    best = t;
  }
  return best;
}

class Finder {
public:
  Finder(const SketchFeature& sk, const CandidateOptions& o)
      : sk_(sk), o_(o), steps_(StepsFor(o.unit)) {}

  std::vector<Candidate> Run() {
    refits_ = RefitTraced(sk_);
    Pieces();
    Neighbours();
    Relations();
    Sizes();
    std::vector<Candidate> kept;
    for (const Candidate& c : out_)
      if (c.sigmas <= o_.maxSigmas && !Holds(c))
        kept.push_back(c);
    // Implied rows are judged against every row, held ones too: a line
    // the sketch already holds level still makes parallel redundant.
    Prune(kept, out_);
    std::stable_sort(kept.begin(), kept.end(),
                     [](const Candidate& x, const Candidate& y) {
                       return x.sigmas < y.sigmas;
                     });
    return kept;
  }

private:
  const SketchFeature& sk_;
  CandidateOptions o_;
  SizeSteps steps_;
  std::vector<TracedRefit> refits_;
  std::vector<Piece> pieces_;
  std::map<uint32_t, size_t> at_;
  std::vector<Joint> joints_;
  std::set<std::pair<uint32_t, uint32_t>> near_;
  std::vector<Candidate> out_;

  bool Near(uint32_t a, uint32_t b) const {
    return near_.count({std::min(a, b), std::max(a, b)}) > 0;
  }
  const Joint* JointOf(uint32_t a, uint32_t b) const {
    for (const Joint& j : joints_)
      if ((j.a == a && j.b == b) || (j.a == b && j.b == a))
        return &j;
    return nullptr;
  }

  void Pieces() {
    for (const TracedRefit& r : refits_) {
      const Entity* e = ById(sk_, r.entity);
      if (!r.ok || e == nullptr)
        continue;
      Piece p;
      p.id = r.entity;
      p.refit = &r;
      if (const auto* l = std::get_if<Line>(e)) {
        p.line = true;
        p.a = l->a;
        p.b = l->b;
        p.length = glm::length(l->b - l->a);
        p.dir = r.fit.dir;
        if (glm::dot(p.dir, l->b - l->a) < 0.0)
          p.dir = -p.dir;
        p.p0 = r.fit.p0;
        p.angleSigma = r.fit.fit.angleSigma;
        p.offsetSigma = r.fit.fit.offsetSigmaMm;
        p.mid = 0.5 * (l->a + l->b);
        p.normal = {-p.dir.y, p.dir.x};
      } else {
        const bool held = r.held.ok;
        p.centre = held ? r.held.centre : r.fit.centre;
        p.radius = held ? r.held.radius : r.fit.radius;
        p.centreSigma = held ? r.held.centreSigmaMm : r.fit.fit.centreSigmaMm;
        p.radiusSigma = held ? r.held.radiusSigmaMm : r.fit.fit.radiusSigmaMm;
        double m = 0.0;
        if (const auto* a = std::get_if<Arc>(e)) {
          p.start = a->startAngle;
          p.sweep = a->endAngle - a->startAngle;
          m = 0.5 * (a->startAngle + a->endAngle);
        } else {
          p.circle = true;
          p.sweep = angles::kTwoPi;
        }
        p.normal = {std::cos(m), std::sin(m)};
        p.mid = p.centre + p.radius * p.normal;
      }
      at_[p.id] = pieces_.size();
      pieces_.push_back(p);
    }
    std::set<uint32_t> ids;
    for (const Piece& p : pieces_)
      ids.insert(p.id);
    joints_ = Joints(sk_, ids);
    // A line's length, from where the pieces it meets place its ends.
    for (Piece& p : pieces_) {
      if (!p.line)
        continue;
      double s2 = 0.0;
      for (uint8_t w : {uint8_t(0), uint8_t(1)}) {
        const Piece* nb = nullptr;
        for (const Joint& j : joints_) {
          if (j.a == p.id && j.wa == w && at_.count(j.b))
            nb = &pieces_[at_[j.b]];
          else if (j.b == p.id && j.wb == w && at_.count(j.a))
            nb = &pieces_[at_[j.a]];
        }
        const double s = EndSigma(p, w == 0 ? p.a : p.b, nb);
        s2 += s * s;
      }
      p.lengthSigma = std::sqrt(s2);
    }
  }

  void Neighbours() {
    std::map<uint32_t, std::set<uint32_t>> adj;
    for (const Joint& j : joints_) {
      adj[j.a].insert(j.b);
      adj[j.b].insert(j.a);
    }
    const auto add = [&](uint32_t a, uint32_t b) {
      if (a != b)
        near_.insert({std::min(a, b), std::max(a, b)});
    };
    for (const auto& [a, nbs] : adj)
      for (uint32_t b : nbs) {
        add(a, b);
        for (uint32_t c : adj[b])
          add(a, c);
      }
    // Facing: the first piece a normal from one's middle reaches.
    for (const Piece& p : pieces_) {
      for (const double sense : {1.0, -1.0}) {
        const glm::dvec2 dir = p.normal * sense;
        double best = kInf;
        uint32_t hit = 0;
        for (const Piece& q : pieces_) {
          if (q.id == p.id)
            continue;
          const double t = RayHit(q, *ById(sk_, q.id), p.mid, dir);
          if (t < best) {
            best = t;
            hit = q.id;
          }
        }
        if (hit != 0)
          add(p.id, hit);
      }
    }
  }

  std::string Key(CandidateKind k, const std::vector<uint32_t>& ids) const {
    std::string s = ToString(k);
    s += ':';
    for (size_t i = 0; i < ids.size(); i++)
      s += (i ? "," : "") + std::to_string(ids[i]);
    return s;
  }

  void Add(CandidateKind k, std::vector<uint32_t> ids, double off, double sigma,
           bool degrees, std::vector<Constraint> adds, double measured = 0.0,
           double nominal = 0.0) {
    // No uncertainty at all: offered only where it holds exactly.
    if (!std::isfinite(sigma) || !std::isfinite(off) || sigma < 0.0 ||
        (sigma == 0.0 && off != 0.0))
      return;
    Candidate c;
    c.kind = k;
    c.entities = std::move(ids);
    c.off = off;
    c.sigma = sigma;
    c.sigmas = sigma > 0.0 ? std::abs(off) / sigma : 0.0;
    c.degrees = degrees;
    c.measured = measured;
    c.nominal = nominal;
    c.adds = std::move(adds);
    c.key = Key(k, c.entities);
    out_.push_back(std::move(c));
  }

  static Constraint Con(ConstraintKind k, PointRef a, PointRef b = {},
                        double value = 0.0, PointRef c = {}) {
    Constraint con{k, a, b, value};
    con.c = c;
    return con;
  }

  double Round(double v, double step) const {
    return std::round(v / step) * step;
  }

  void Relations() {
    using K = CandidateKind;
    using CK = ConstraintKind;
    const size_t n = pieces_.size();
    for (size_t i = 0; i < n; i++) {
      const Piece& p = pieces_[i];
      if (p.line) {
        const double th = std::atan2(p.dir.y, p.dir.x);
        Add(K::Horizontal, {p.id}, Deg(FoldHalf(th)), Deg(p.angleSigma), true,
            {Con(CK::Horizontal, {p.id, 0})});
        Add(K::Vertical, {p.id}, Deg(FoldHalf(th - angles::kPi / 2)),
            Deg(p.angleSigma), true, {Con(CK::Vertical, {p.id, 0})});
        // On an axis: square to it and through the origin; the worse of
        // the two decides.
        for (const uint32_t axis : {kAxisX, kAxisY}) {
          const bool x = axis == kAxisX;
          const double ang =
              std::abs(FoldHalf(th - (x ? 0.0 : angles::kPi / 2)));
          const double offAxis = x ? p.p0.y : p.p0.x;
          const double angSig = ang / p.angleSigma;
          const double offSig = std::abs(offAxis) / p.offsetSigma;
          if (angSig > o_.maxSigmas)
            continue;
          // Reported as the offset, at the worse of the two's sigmas.
          const double sig = std::max(angSig, offSig);
          Add(K::OnAxis, {p.id, axis}, offAxis,
              sig > 0.0 ? std::abs(offAxis) / sig : p.offsetSigma, false,
              {Con(CK::Collinear, {p.id, 0}, {axis, 0})});
        }
      } else {
        Add(K::AtOrigin, {p.id, kOrigin}, glm::length(p.centre),
            Norm(p.centreSigma), false,
            {Con(CK::Coincident, {p.id, 0}, {kOrigin, 0})});
      }
      for (size_t j = i + 1; j < n; j++) {
        const Piece& q = pieces_[j];
        const bool neighbours = Near(p.id, q.id);
        if (p.line && q.line) {
          const double d =
              FoldHalf(std::atan2(Cross(p.dir, q.dir), glm::dot(p.dir, q.dir)));
          const double s = std::hypot(p.angleSigma, q.angleSigma);
          Add(K::Parallel, {p.id, q.id}, Deg(d), Deg(s), true,
              {Con(CK::Parallel, {p.id, 0}, {q.id, 0})});
          Add(K::Perpendicular, {p.id, q.id},
              Deg(FoldHalf(d - angles::kPi / 2)), Deg(s), true,
              {Con(CK::Perpendicular, {p.id, 0}, {q.id, 0})});
          if (!JointOf(p.id, q.id) && std::abs(d) <= o_.maxSigmas * s) {
            // Collinear: q's middle on p's line.
            const glm::dvec2 nrm(-p.dir.y, p.dir.x);
            const double off = glm::dot(q.p0 - p.p0, nrm);
            const double lever = glm::length(q.p0 - p.p0);
            const double so = std::sqrt(p.offsetSigma * p.offsetSigma +
                                        q.offsetSigma * q.offsetSigma +
                                        std::pow(p.angleSigma * lever, 2));
            Add(K::Collinear, {p.id, q.id}, off, so, false,
                {Con(CK::Collinear, {p.id, 0}, {q.id, 0})});
          }
          if (neighbours)
            Add(K::EqualLength, {p.id, q.id}, p.length - q.length,
                std::hypot(p.lengthSigma, q.lengthSigma), false,
                {Con(CK::Equal, {p.id, 0}, {q.id, 0})});
        } else if (!p.line && !q.line) {
          if (neighbours)
            Add(K::EqualRadius, {p.id, q.id}, p.radius - q.radius,
                std::hypot(p.radiusSigma, q.radiusSigma), false,
                {Con(CK::Equal, {p.id, 0}, {q.id, 0})});
          Add(K::Concentric, {p.id, q.id}, glm::length(p.centre - q.centre),
              std::hypot(Norm(p.centreSigma), Norm(q.centreSigma)), false,
              {Con(CK::Concentric, {p.id, 0}, {q.id, 0})});
          // Mirrored about an axis: rounds of a size.
          if (std::abs(p.radius - q.radius) <=
              o_.maxSigmas * std::hypot(p.radiusSigma, q.radiusSigma))
            for (const uint32_t axis : {kAxisX, kAxisY}) {
              const glm::dvec2 m = axis == kAxisX
                                       ? glm::dvec2(p.centre.x, -p.centre.y)
                                       : glm::dvec2(-p.centre.x, p.centre.y);
              // Not the same point twice: a round on the axis mirrors to
              // itself.
              if (glm::length(p.centre - q.centre) <
                  3.0 * std::hypot(Norm(p.centreSigma), Norm(q.centreSigma)))
                continue;
              Add(K::Symmetric, {p.id, q.id, axis}, glm::length(m - q.centre),
                  std::hypot(Norm(p.centreSigma), Norm(q.centreSigma)), false,
                  {Con(CK::Symmetric, {p.id, 0}, {q.id, 0}, 0.0, {axis, 0})});
            }
        }
        // Tangent where they meet.
        if (const Joint* jt = JointOf(p.id, q.id); jt != nullptr)
          Tangent(p, q, *jt);
      }
    }
  }

  void Tangent(const Piece& p, const Piece& q, const Joint& jt) {
    using CK = ConstraintKind;
    if (p.line && q.line)
      return;
    const Piece& round = p.line ? q : p;
    const Piece& other = p.line ? p : q;
    const glm::dvec2 meet =
        EntityPointUV(*ById(sk_, jt.a), jt.wa); // the drawn joint
    const std::vector<Constraint> adds = {
        Con(CK::Tangent, {p.id, 0}, {q.id, 0})};
    if (other.line) {
      // What holding the round tangent to the line costs, in the free
      // fits' sigmas (FitTangentRound, that side alone).
      const TracedRefit& rr = *round.refit;
      const TracedRefit& lr = *other.refit;
      const int s =
          glm::length(meet - rr.pts.front()) < glm::length(meet - rr.pts.back())
              ? 0
              : 1;
      RoundSide side[2];
      side[s].tangent = true;
      side[s].pts = Toward(lr.pts, meet, s == 0);
      side[s].p0 = lr.fit.p0;
      side[s].dir = lr.fit.dir;
      const RoundFit f = FitTangentRound(rr.pts, rr.fit.centre, rr.fit.radius,
                                         side[0], side[1], rr.floorMm);
      if (!f.ok)
        return;
      // The free fits' angle at the joint, for the reader.
      const glm::dvec2 radial = glm::normalize(meet - rr.fit.centre);
      const double off = Deg(std::asin(
          std::clamp(std::abs(glm::dot(radial, other.dir)), 0.0, 1.0)));
      // Its evidence is what holding it costs, not the angle over a sigma:
      // the angle at a small round's end is what its own fit knows least.
      Candidate c;
      c.kind = CandidateKind::Tangent;
      c.entities = {p.id, q.id};
      c.off = off;
      c.degrees = true;
      c.sigmas = f.tangentSigmas;
      c.sigma = f.tangentSigmas > 0.0 ? off / f.tangentSigmas : kInf;
      c.adds = adds;
      c.key = Key(c.kind, c.entities);
      out_.push_back(std::move(c));
      return;
    }
    // Two rounds: their radial directions at the joint.
    const glm::dvec2 ra = glm::normalize(meet - p.centre);
    const glm::dvec2 rb = glm::normalize(meet - q.centre);
    const double d = FoldHalf(std::atan2(Cross(ra, rb), glm::dot(ra, rb)));
    const double s = std::hypot(Norm(p.centreSigma) / p.radius,
                                Norm(q.centreSigma) / q.radius);
    Add(CandidateKind::Tangent, {p.id, q.id}, Deg(d), Deg(s), true, adds);
  }

  void Sizes() {
    using K = CandidateKind;
    using CK = ConstraintKind;
    const auto lengthStep = [&](double v) {
      return v < steps_.smallBelow ? steps_.smallStep : steps_.largeStep;
    };
    const size_t n = pieces_.size();
    for (size_t i = 0; i < n; i++) {
      const Piece& p = pieces_[i];
      if (p.line) {
        const double nom = Round(p.length, lengthStep(p.length));
        if (nom > 0.0)
          Add(K::Length, {p.id}, p.length - nom, p.lengthSigma, false,
              {Con(CK::Distance, {p.id, 0}, {p.id, 1}, nom)}, p.length, nom);
      } else if (p.circle) {
        const double d = 2.0 * p.radius;
        const double nom = Round(d, lengthStep(d));
        if (nom > 0.0)
          Add(K::Diameter, {p.id}, d - nom, 2.0 * p.radiusSigma, false,
              {Con(CK::Diameter, {p.id, 0}, {}, nom)}, d, nom);
      } else {
        const double nom = Round(p.radius, steps_.radiusStep);
        if (nom > 0.0)
          Add(K::Radius, {p.id}, p.radius - nom, p.radiusSigma, false,
              {Con(CK::Radius, {p.id, 0}, {}, nom)}, p.radius, nom);
      }
      for (size_t j = i + 1; j < n; j++) {
        const Piece& q = pieces_[j];
        if (!Near(p.id, q.id))
          continue;
        if (p.line && q.line) {
          const double ang =
              std::atan2(Cross(p.dir, q.dir), glm::dot(p.dir, q.dir));
          const double s = std::hypot(p.angleSigma, q.angleSigma);
          const double fold = FoldHalf(ang);
          if (std::abs(fold) <= o_.maxSigmas * s) {
            // Parallel: the distance between them.
            const glm::dvec2 nrm(-p.dir.y, p.dir.x);
            const double dist = std::abs(glm::dot(q.p0 - p.p0, nrm));
            const double nom = Round(dist, lengthStep(dist));
            if (nom > 0.0)
              Add(K::Distance, {p.id, q.id}, dist - nom,
                  std::hypot(p.offsetSigma, q.offsetSigma), false,
                  {Con(CK::DistancePL, {q.id, 0}, {p.id, 0}, nom)}, dist, nom);
            continue;
          }
          if (std::abs(FoldHalf(fold - angles::kPi / 2)) <= o_.maxSigmas * s)
            continue; // square: perpendicular says it
          const double deg = Deg(ang);
          const double nom = Round(deg, kAngleStepDeg);
          if (std::abs(nom) < kAngleStepDeg / 2 ||
              std::abs(std::abs(nom) - 90.0) < 1e-9 ||
              std::abs(std::abs(nom) - 180.0) < 1e-9)
            continue;
          Add(K::Angle, {p.id, q.id}, deg - nom, Deg(s), true,
              {Con(CK::Angle, {p.id, 0}, {q.id, 0}, angles::ToRadians(nom))},
              deg, nom);
        } else if (!p.line && !q.line) {
          const double dist = glm::length(p.centre - q.centre);
          const double s = std::hypot(Norm(p.centreSigma), Norm(q.centreSigma));
          if (dist <= o_.maxSigmas * s)
            continue; // concentric says it
          const double nom = Round(dist, lengthStep(dist));
          if (nom > 0.0)
            Add(K::Distance, {p.id, q.id}, dist - nom, s, false,
                {Con(CK::Distance, {p.id, 0}, {q.id, 0}, nom)}, dist, nom);
        }
      }
    }
  }

  // The sketch already says it: a constraint of the kind on those entities.
  bool Holds(const Candidate& c) const {
    for (const Constraint& add : c.adds)
      for (const Constraint& h : sk_.constraints) {
        const bool sameKind = h.kind == add.kind ||
                              (IsDimension(h.kind) && IsDimension(add.kind) &&
                               (c.kind == CandidateKind::Radius ||
                                c.kind == CandidateKind::Diameter));
        if (!sameKind)
          continue;
        const auto set = [](const Constraint& k) {
          std::set<uint32_t> s{k.a.entity};
          if (k.b.entity != 0)
            s.insert(k.b.entity);
          if (k.c.entity != 0)
            s.insert(k.c.entity);
          return s;
        };
        if (set(h) == set(add))
          return true;
      }
    return false;
  }

  // A relation two stronger rows already imply is not listed: parallel
  // between two lines both offered level, square between a level and an
  // upright one.
  void Prune(std::vector<Candidate>& kept,
             const std::vector<Candidate>& all) const {
    // Only rows a default acceptance would take imply anything: with the
    // limit raised past them every line is offered level and upright.
    const double strong = std::min(o_.maxSigmas, 3.0);
    std::set<uint32_t> h, v;
    for (const Candidate& c : all) {
      if (c.sigmas > strong)
        continue;
      if (c.kind == CandidateKind::Horizontal)
        h.insert(c.entities[0]);
      if (c.kind == CandidateKind::Vertical)
        v.insert(c.entities[0]);
    }
    std::erase_if(kept, [&](const Candidate& c) {
      if (c.kind != CandidateKind::Parallel &&
          c.kind != CandidateKind::Perpendicular)
        return false;
      const uint32_t a = c.entities[0], b = c.entities[1];
      if (c.kind == CandidateKind::Parallel)
        return (h.count(a) && h.count(b)) || (v.count(a) && v.count(b));
      return (h.count(a) && v.count(b)) || (v.count(a) && h.count(b));
    });
  }
};

// A point's signed distance to an entity's own curve (a line's infinite
// line, an arc's circle).
double Signed(const Entity& e, const glm::dvec2& p) {
  if (const auto* l = std::get_if<Line>(&e)) {
    const glm::dvec2 d = l->b - l->a;
    const double len = glm::length(d);
    return len > 0.0 ? Cross(d, p - l->a) / len : 0.0;
  }
  if (const auto* a = std::get_if<Arc>(&e))
    return glm::length(p - a->center) - a->radius;
  if (const auto* c = std::get_if<Circle>(&e))
    return glm::length(p - c->center) - c->radius;
  return 0.0;
}

} // namespace

std::vector<Candidate> FindCandidates(const SketchFeature& sk,
                                      const CandidateOptions& o) {
  return Finder(sk, o).Run();
}

AcceptResult AcceptCandidates(SketchFeature& sk, const AcceptOptions& o) {
  AcceptResult out;
  std::vector<Candidate> all = FindCandidates(sk, {o.maxSigmas, o.unit});
  if (!o.only.empty()) {
    std::set<std::string> want(o.only.begin(), o.only.end());
    std::set<std::string> have;
    for (const Candidate& c : all)
      have.insert(c.key);
    for (const std::string& k : o.only)
      if (!have.count(k))
        out.unknown.push_back(k);
    // A key no candidate has refuses the whole call: nothing is added.
    if (!out.unknown.empty())
      return out;
    std::erase_if(all, [&](const Candidate& c) { return !want.count(c.key); });
  }
  // Where each traced piece stood against its points before anything was
  // accepted, and how far it may move from there.
  struct Watch {
    uint32_t id = 0;
    std::vector<glm::dvec2> pts;
    std::vector<double> before;
    double limit = 0.0;
  };
  std::vector<Watch> watch;
  for (const TracedRefit& r : RefitTraced(sk)) {
    const Entity* e = ById(sk, r.entity);
    if (!r.ok || e == nullptr || r.pts.empty())
      continue;
    Watch w;
    w.id = r.entity;
    w.pts = r.pts;
    for (const glm::dvec2& p : r.pts)
      w.before.push_back(Signed(*e, p));
    w.limit = o.maxMoveMm > 0.0 ? o.maxMoveMm
                                : 3.0 * std::max(r.fit.fit.sigmaMm, r.floorMm);
    watch.push_back(std::move(w));
  }
  // Relations before sizes, each nearest first: a size taken before the
  // relations that shape the piece fixes it where the relations no longer
  // fit (a slot's width before its ends are tangent contradicts their
  // equal radius).
  std::stable_sort(all.begin(), all.end(),
                   [](const Candidate& x, const Candidate& y) {
                     return !IsSize(x.kind) && IsSize(y.kind);
                   });
  Solver solver;
  for (const Candidate& c : all) {
    SketchFeature after = sk;
    for (const Constraint& add : c.adds)
      after.constraints.push_back(add);
    CandidateOutcome oc;
    oc.candidate = c;
    const AddVerdict v = JudgeAddition(sk, after, &solver);
    if (!v.accepted) {
      oc.reason = v.reason.empty() ? "the solver cannot meet it" : v.reason;
      out.left.push_back(std::move(oc));
      continue;
    }
    solver.WriteBack(after.entities);
    // How far each traced piece moved, on the curve at its own points.
    double worst = 0.0, worstOver = 0.0;
    uint32_t worstId = 0;
    double worstLimit = 0.0;
    for (const Watch& w : watch) {
      const Entity* e = ById(after, w.id);
      if (e == nullptr)
        continue;
      double m = 0.0;
      for (size_t k = 0; k < w.pts.size(); k++)
        m = std::max(m, std::abs(Signed(*e, w.pts[k]) - w.before[k]));
      if (m / w.limit > worstOver) {
        worstOver = m / w.limit;
        worst = m;
        worstId = w.id;
        worstLimit = w.limit;
      }
    }
    oc.moveMm = worst;
    oc.movedEntity = worstId;
    if (worstOver > 1.0) {
      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "holding it moves entity %u %.3g mm off its points (the "
                    "limit is %.3g mm)",
                    worstId, worst, worstLimit);
      oc.reason = buf;
      out.left.push_back(std::move(oc));
      continue;
    }
    sk.entities = std::move(after.entities);
    sk.constraints = std::move(after.constraints);
    out.accepted.push_back(std::move(oc));
  }
  return out;
}

} // namespace sketch
