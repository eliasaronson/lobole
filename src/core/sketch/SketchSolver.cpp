// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/sketch/SketchSolver.h"

#include "common/Logger.h"
#include "core/Angles.h"
#include "core/sketch/SketchOrientation.h"
#include "core/sketch/SketchText.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <set>

#ifdef HAVE_PLANEGCS

#include "GCS.h"

namespace sketch {

namespace {
// The kind as the file and add_constraint spell it, for refusal messages.
const char* KindWord(ConstraintKind k) {
  using K = ConstraintKind;
  switch (k) {
  case K::Coincident:
    return "coincident";
  case K::Horizontal:
    return "horizontal";
  case K::Vertical:
    return "vertical";
  case K::Parallel:
    return "parallel";
  case K::Perpendicular:
    return "perpendicular";
  case K::Equal:
    return "equal";
  case K::Distance:
    return "distance";
  case K::Radius:
    return "radius";
  case K::Angle:
    return "angle";
  case K::Tangent:
    return "tangent";
  case K::PointOnLine:
    return "point_on_line";
  case K::Symmetric:
    return "symmetric";
  case K::Midpoint:
    return "midpoint";
  case K::Fixed:
    return "fixed";
  case K::Concentric:
    return "concentric";
  case K::Collinear:
    return "collinear";
  case K::DistanceX:
    return "distance_x";
  case K::DistanceY:
    return "distance_y";
  case K::Diameter:
    return "diameter";
  case K::DistancePL:
    return "distance_pl";
  }
  return "?";
}

// What each kind accepts, in the words a refusal uses.
const char* Accepts(ConstraintKind k) {
  using K = ConstraintKind;
  switch (k) {
  case K::Coincident:
  case K::Distance:
  case K::DistanceX:
  case K::DistanceY:
    return "two points";
  case K::Horizontal:
  case K::Vertical:
    return "a line, an ellipse or elliptical arc (its major axis), or two "
           "points";
  case K::Parallel:
  case K::Perpendicular:
  case K::Angle:
  case K::Collinear:
    return "two lines (a datum axis counts as a line)";
  case K::Equal:
    return "two lines, two circles/arcs, or two ellipses/elliptical arcs";
  case K::Radius:
  case K::Diameter:
    return "a circle or an arc";
  case K::Tangent:
    return "a line and a circle, arc, ellipse or elliptical arc; two "
           "circles/arcs; or two of line, arc, elliptical arc and spline "
           "joined end to end by a coincident (tangent at that end)";
  case K::PointOnLine:
    return "a point and a line, circle, arc, ellipse, elliptical arc or "
           "spline";
  case K::Symmetric:
    return "two points and, as the third ref, a line or a point";
  case K::Midpoint:
  case K::DistancePL:
    return "a point and a line";
  case K::Fixed:
    return "a point";
  case K::Concentric:
    return "two of circle, arc, ellipse and elliptical arc";
  }
  return "";
}

const char* EntityWord(const Entity& e) {
  return std::visit(
      [](const auto& g) -> const char* {
        using T = std::decay_t<decltype(g)>;
        if constexpr (std::is_same_v<T, Point>)
          return "a point";
        else if constexpr (std::is_same_v<T, Line>)
          return "a line";
        else if constexpr (std::is_same_v<T, Circle>)
          return "a circle";
        else if constexpr (std::is_same_v<T, Arc>)
          return "an arc";
        else if constexpr (std::is_same_v<T, Ellipse>)
          return "an ellipse";
        else if constexpr (std::is_same_v<T, EllipseArc>)
          return "an elliptical arc";
        else if constexpr (std::is_same_v<T, BSpline>)
          return "a spline";
        else
          return "a text label";
      },
      e);
}

// The axis-end alignment of `which` (3..6) to PlaneGCS's internal-alignment
// pair (x, y).
std::pair<GCS::InternalAlignmentType, GCS::InternalAlignmentType>
AxisAlignment(uint8_t which) {
  switch (which) {
  case kEllipseMajorPos:
    return {GCS::EllipsePositiveMajorX, GCS::EllipsePositiveMajorY};
  case kEllipseMajorNeg:
    return {GCS::EllipseNegativeMajorX, GCS::EllipseNegativeMajorY};
  case kEllipseMinorPos:
    return {GCS::EllipsePositiveMinorX, GCS::EllipsePositiveMinorY};
  default:
    return {GCS::EllipseNegativeMinorX, GCS::EllipseNegativeMinorY};
  }
}

// The spline parameter of the curve point nearest `p`: a dense sample, then
// a bracketing refinement. Only a SEED for the solver's own parameter
// unknown, so it needs to be on the right span, not exact.
double NearestSplineParam(const BSpline& s, glm::dvec2 p) {
  const int n = static_cast<int>(s.poles.size());
  const int deg = BSplineDegree(s);
  const double umax = static_cast<double>(n - deg);
  const int samples = std::max(16, 64 * (n - deg));
  double best = 0.0, bestD = INFINITY;
  for (int i = 0; i <= samples; i++) {
    const double u = umax * i / samples;
    const double d = glm::length(BSplineEvalUV(s, u) - p);
    if (d < bestD)
      bestD = d, best = u;
  }
  if (deg < 2)
    return best;
  // Then Gauss-Newton on (C(u) − p)·C'(u) = 0, to machine precision: a point
  // already on the curve must start the solve AT its parameter, or every
  // re-solve would nudge it (and the poles) by the seed's error. C' is the
  // clamped-uniform spline of degree p−1 over p·ΔP/Δknot -- the same family,
  // so BSplineEvalUV evaluates it.
  const std::vector<double> U = BSplineKnots(n, deg);
  BSpline d;
  d.degree = deg - 1;
  for (int i = 0; i + 1 < n; i++)
    d.poles.push_back(static_cast<double>(deg) * (s.poles[i + 1] - s.poles[i]) /
                      (U[i + deg + 1] - U[i + 1]));
  double u = best;
  for (int it = 0; it < 30; it++) {
    const glm::dvec2 c1 = BSplineEvalUV(d, u);
    const double den = glm::dot(c1, c1);
    if (den < 1e-300)
      break;
    const double step = glm::dot(BSplineEvalUV(s, u) - p, c1) / den;
    u = std::clamp(u - step, 0.0, umax);
    if (std::abs(step) < 1e-16 * std::max(1.0, umax))
      break;
  }
  return u;
}

// What an ellipse or elliptical arc was loaded as, for the write-back.
struct EllipseLoad {
  GCS::Ellipse* curve = nullptr; // null = rigid (a circle-shaped ellipse)
  glm::dvec2 offset0{0.0};       // focus − centre as loaded
  double b0 = 0.0;               // minor radius as loaded
};
} // namespace

struct Solver::Impl {
  GCS::System sys;
  // deque: element addresses stay stable across push_back (GCS holds raw
  // pointers into these), so no reservation bookkeeping is needed.
  std::deque<double> params;          // point coords + dimension values
  std::deque<double> radii;           // circle + arc radii, ellipse radmin
  std::deque<double> angles;          // (elliptical) arc start/end params
  std::deque<double> foci;            // ellipse focus1 coordinates
  std::deque<double> curveParams;     // point-on-spline curve parameters
  std::deque<double> constants;       // spline knots and weights (fixed)
  std::deque<double> holds;           // soft shape holds' targets (fixed)
  std::vector<GCS::Point> gcsPoints;  // one per (entity, which) we expose
  std::map<uint64_t, int> pointIndex; // (entity<<8 | which) -> gcsPoints index
  std::vector<GCS::Line> lines;       // index-aligned with line entities
  std::map<uint32_t, int> lineOf;     // entity index -> lines index
  std::vector<GCS::Circle> circles;
  std::map<uint32_t, int> circleOf; // entity id -> circles index
  std::vector<GCS::Arc> arcs;
  std::map<uint32_t, int> arcOf; // entity id -> arcs index
  // Ellipses, elliptical arcs and splines are held by address: a
  // PointOnBSpline constraint keeps a REFERENCE to its spline.
  std::deque<GCS::Ellipse> ellipses;
  std::deque<GCS::ArcOfEllipse> ellArcs;
  std::deque<GCS::BSpline> splines;
  std::map<uint32_t, GCS::Ellipse*> ellipseOf; // ellipse OR elliptical arc
  std::map<uint32_t, GCS::ArcOfEllipse*> ellArcOf;
  std::map<uint32_t, GCS::BSpline*> splineOf;
  std::map<uint32_t, EllipseLoad> ellipseLoad;
  std::deque<GCS::Line> legs; // spline end legs, as tangent stand-ins
  // An entity's solver unknowns that are not point coordinates (radius,
  // angles, focus): what "fix this entity" must also hold, and what
  // "is it under-defined" must also read.
  std::map<uint32_t, std::vector<double*>> shapeParams;

  // Loaded sketch shape for write-back
  std::vector<Entity> entityShapes;
  std::map<uint32_t, int> shapeIndexOf; // entity id -> index in entityShapes

  int dragTag = 0;
  std::deque<double> dragTarget;       // 2 slots per dragged point (x, y)
  std::vector<int> dragTargetBase;     // dragTarget index base per input point
  std::deque<double> dragRadiusTarget; // 1 slot for a radius drag
  std::set<double*> fixedParams; // params held out of the unknowns (Fixed)
  std::vector<int> constraintFirstTag; // per-constraint first GCS tag (+ end)
  std::vector<Unsupported> unsupported;
  // Shape holds as real equations (tag 0) instead of second-priority ones:
  // Solver::Load's first pass, which moves ellipses only whole.
  bool hardHolds = false;
  // Each ellipse's soft holds carry tags of their own (any negative tag is
  // second priority), so a drag of one handle can release just what that
  // handle changes (BeginDrag).
  struct HoldTags {
    int shape = 0, start = 0, end = 0;
  };
  std::map<uint32_t, HoldTags> holdTags;
  int nextHoldTag = -2; // -1 is PlaneGCS's own temporary tag
  int NewHoldTag() { return hardHolds ? 0 : nextHoldTag--; }

  // The solver parameter that holds each dimension's value (null for a
  // constraint that is not one, or was refused): what a value edit steps.
  struct ValueSlot {
    double* p = nullptr;
    bool absolute = false; // the solver reads |value|
    double scale = 1.0;    // a diameter's slot holds the radius
  };
  std::vector<ValueSlot> valueSlots;
  std::vector<Constraint> constraints; // as loaded, for orientations
  std::optional<std::vector<Orientation>> orientations;
  int solveCount = 0;

  // The drag guard: the last accepted frame's orientations, targets and
  // unknowns (SolveDrag).
  std::vector<int> dragSigns;
  std::vector<double> dragPrevTarget;
  std::vector<double> dragPrevRadius;

  using Snapshot = std::vector<std::pair<double*, double>>;
  Snapshot dragState;
  Snapshot Take() {
    Snapshot s;
    for (double* p : AllUnknowns())
      s.emplace_back(p, *p);
    return s;
  }
  static void Put(const Snapshot& s) {
    for (const auto& [p, v] : s)
      *p = v;
  }

  void SetSlot(const ValueChange& c, double value) {
    const ValueSlot& slot = valueSlots[c.constraint];
    *slot.p = (slot.absolute ? std::abs(value) : value) * slot.scale;
  }

  bool PointHeld(const PointRef& r) const {
    const auto it = pointIndex.find(Key(r.entity, r.which));
    if (it == pointIndex.end())
      return true; // nothing the solver moves
    const GCS::Point& gp = gcsPoints[static_cast<size_t>(it->second)];
    return fixedParams.count(gp.x) && fixedParams.count(gp.y);
  }
  const std::vector<Orientation>& Orientations() {
    if (!orientations)
      orientations =
          FindOrientations(entityShapes, constraints,
                           [this](const PointRef& r) { return PointHeld(r); });
    return *orientations;
  }
  // The orientations' signs in the solver's current values.
  std::vector<int> Signs() {
    if (Orientations().empty())
      return {};
    std::vector<Entity> now;
    WriteBackTo(now);
    return OrientationSigns(Orientations(), now);
  }
  // The orientations clear in `before` that are not the same in `after`.
  static std::vector<size_t> Turned(const std::vector<int>& before,
                                    const std::vector<int>& after) {
    std::vector<size_t> out;
    for (size_t i = 0; i < before.size() && i < after.size(); i++)
      if (before[i] != 0 && after[i] != 0 && after[i] != before[i])
        out.push_back(i);
    return out;
  }
  void ResetDragGuard() {
    dragSigns = Signs();
    dragPrevTarget.assign(dragTarget.begin(), dragTarget.end());
    dragPrevRadius.assign(dragRadiusTarget.begin(), dragRadiusTarget.end());
    dragState = Take();
  }

  void LoadFrom(const SketchFeature& sketch,
                const std::vector<Entity>& entities);
  void WriteBackTo(std::vector<Entity>& entities) const;

  // Maps solver tags to the SketchFeature::constraints indices that own them.
  std::vector<size_t> ConstraintsForTags(const std::vector<int>& tags) const {
    std::vector<size_t> out;
    for (int t : tags) {
      for (size_t k = 0; k + 1 < constraintFirstTag.size(); k++)
        if (constraintFirstTag[k] <= t && t < constraintFirstTag[k + 1]) {
          out.push_back(k);
          break;
        }
    }
    return out;
  }

  static uint64_t Key(uint32_t entity, uint8_t which) {
    return (uint64_t(entity) << 8) | which;
  }

  GCS::Point* Resolve(const PointRef& r) {
    auto it = pointIndex.find(Key(r.entity, r.which));
    if (it == pointIndex.end())
      return nullptr;
    return &gcsPoints[it->second];
  }

  const Entity* EntityOf(uint32_t id) const {
    const auto it = shapeIndexOf.find(id);
    return it == shapeIndexOf.end() ? nullptr : &entityShapes[it->second];
  }

  // The ends a curve can be joined at: a line's 0/1, an (elliptical) arc's
  // 1/2, a spline's first and last pole.
  std::vector<uint8_t> EndsOf(uint32_t id) const {
    const Entity* e = EntityOf(id);
    if (e == nullptr)
      return {};
    if (std::holds_alternative<Line>(*e))
      return {0, 1};
    if (std::holds_alternative<Arc>(*e) ||
        std::holds_alternative<EllipseArc>(*e))
      return {1, 2};
    if (const auto* bs = std::get_if<BSpline>(e)) {
      const size_t n = std::min<size_t>(bs->poles.size(), 256);
      if (n >= 2)
        return {0, static_cast<uint8_t>(n - 1)};
    }
    return {};
  }

  // The ends (of `a`, of `b`) a Coincident joins, if any.
  std::optional<std::pair<PointRef, PointRef>>
  SharedEnd(const std::vector<Constraint>& constraints, uint32_t a,
            uint32_t b) const {
    const auto endsA = EndsOf(a), endsB = EndsOf(b);
    const auto isEnd = [](const std::vector<uint8_t>& ends, uint8_t w) {
      return std::find(ends.begin(), ends.end(), w) != ends.end();
    };
    for (const Constraint& c : constraints) {
      if (c.kind != ConstraintKind::Coincident)
        continue;
      for (const auto& [p, q] : {std::pair{c.a, c.b}, std::pair{c.b, c.a}})
        if (p.entity == a && isEnd(endsA, p.which) && q.entity == b &&
            isEnd(endsB, q.which))
          return std::pair{p, q};
    }
    return std::nullopt;
  }

  // The curve a tangent at `end` reads: the entity itself, or for a spline
  // the leg from its end pole to the next one (a clamped spline's tangent
  // there).
  GCS::Curve* TangentCurveAt(const PointRef& end) {
    if (lineOf.count(end.entity))
      return &lines[lineOf[end.entity]];
    if (arcOf.count(end.entity))
      return &arcs[arcOf[end.entity]];
    if (ellArcOf.count(end.entity))
      return ellArcOf[end.entity];
    if (splineOf.count(end.entity)) {
      GCS::BSpline& bs = *splineOf[end.entity];
      GCS::Line leg;
      if (end.which == 0) {
        leg.p1 = bs.poles[0];
        leg.p2 = bs.poles[1];
      } else {
        leg.p1 = bs.poles[bs.poles.size() - 2];
        leg.p2 = bs.poles[bs.poles.size() - 1];
      }
      legs.push_back(leg);
      return &legs.back();
    }
    return nullptr;
  }

  // Every solver unknown: point coordinates, radii, angles, foci, curve
  // parameters -- less whatever is held fixed.
  std::vector<double*> AllUnknowns() {
    GCS::VEC_pD unknowns;
    for (auto& gp : gcsPoints) {
      if (!fixedParams.count(gp.x)) // Fixed points stay put
        unknowns.push_back(gp.x);
      if (!fixedParams.count(gp.y))
        unknowns.push_back(gp.y);
    }
    for (auto* dq : {&radii, &angles, &foci, &curveParams})
      for (auto& v : *dq)
        if (!fixedParams.count(&v))
          unknowns.push_back(&v);
    return unknowns;
  }

  // Declares every free parameter as an unknown and initialises the solution
  // structure. Call after all constraints (incl. drag) are added.
  void DeclareUnknowns() {
    GCS::VEC_pD unknowns = AllUnknowns();
    if (unknowns.empty())
      return;
    sys.declareUnknowns(unknowns);
    sys.initSolution();
  }

  // Solves the already-declared system and reports the verdict.
  SolveResult RunSolve() {
    SolveResult result;
    // Trivial when nothing is free to solve: the datum origin/axes are always
    // registered but fixed.
    solveCount++;
    if (AllUnknowns().empty()) {
      result.ok = true;
      result.dof = 0;
      return result;
    }
    int status = sys.solve(true, GCS::DogLeg);
    if (status == GCS::Success || status == GCS::Converged) {
      sys.applySolution();
      result.ok = true;
    }
    result.dof = sys.dofsNumber();
    sys.getConflicting(result.conflicting);
    sys.getRedundant(result.redundant);
    result.conflictingConstraints = ConstraintsForTags(result.conflicting);
    result.redundantConstraints = ConstraintsForTags(result.redundant);
    return result;
  }
};

Solver::Solver() : m_impl(new Impl) {}
Solver::~Solver() { delete m_impl; }

void Solver::Load(const SketchFeature& sketch) {
  // First, with each ellipse's size, shape, turn and arc ends held HARD: when
  // the constraints can be met by moving ellipses whole (a coincident on a
  // centre, a dimension to one), that answer keeps their shape EXACTLY, where
  // the soft holds' second-priority minimisation stops at ~1e-9. The real
  // system (soft holds) then starts already solved and does not move.
  std::vector<Entity> seed = sketch.entities;
  const bool anyEllipse =
      std::any_of(seed.begin(), seed.end(), [](const Entity& e) {
        return std::holds_alternative<Ellipse>(e) ||
               std::holds_alternative<EllipseArc>(e);
      });
  if (anyEllipse && !sketch.constraints.empty()) {
    Impl pre;
    pre.hardHolds = true;
    pre.LoadFrom(sketch, sketch.entities);
    pre.DeclareUnknowns();
    const SolveResult r = pre.RunSolve();
    if (r.ok && r.conflictingConstraints.empty() && r.conflicting.empty())
      pre.WriteBackTo(seed);
  }
  *m_impl = Impl{}; // reset
  m_impl->LoadFrom(sketch, seed);
}

void Solver::Impl::LoadFrom(const SketchFeature& sketch,
                            const std::vector<Entity>& entities) {
  Impl& im = *this;
  im.entityShapes = entities;

  auto addPoint = [&](uint32_t entity, uint8_t which, glm::dvec2 p) {
    im.params.push_back(p.x);
    im.params.push_back(p.y);
    GCS::Point gp(&im.params[im.params.size() - 2],
                  &im.params[im.params.size() - 1]);
    im.gcsPoints.push_back(gp);
    im.pointIndex[Impl::Key(entity, which)] =
        static_cast<int>(im.gcsPoints.size() - 1);
    return gp;
  };
  // A point that rides `anchor` at its current offset (tag 0, definitional):
  // the solver reads it but never moves it relative to the anchor.
  auto addRider = [&](uint32_t entity, uint8_t which, glm::dvec2 p,
                      GCS::Point anchor) {
    const GCS::Point gp = addPoint(entity, which, p);
    im.params.push_back(*gp.x - *anchor.x);
    im.sys.addConstraintDifference(anchor.x, gp.x, &im.params.back(), 0);
    im.params.push_back(*gp.y - *anchor.y);
    im.sys.addConstraintDifference(anchor.y, gp.y, &im.params.back(), 0);
    return gp;
  };

  // Datum geometry: a fixed origin point and the two axis lines, always present
  // so constraints / mirrors / snaps can reference them. Held out of the
  // unknowns (fixedParams), so they add no DOF and never move.
  auto fixPoint = [&](GCS::Point gp) {
    im.fixedParams.insert(gp.x);
    im.fixedParams.insert(gp.y);
  };
  auto addDatumAxis = [&](uint32_t id, glm::dvec2 a, glm::dvec2 b) {
    GCS::Line gl;
    gl.p1 = addPoint(id, 0, a);
    gl.p2 = addPoint(id, 1, b);
    fixPoint(gl.p1);
    fixPoint(gl.p2);
    im.lines.push_back(gl);
    im.lineOf[id] = static_cast<int>(im.lines.size() - 1);
  };
  fixPoint(addPoint(kOrigin, 0, {0.0, 0.0}));
  {
    auto [xa, xb] = DatumAxisEndpoints(kAxisX);
    addDatumAxis(kAxisX, xa, xb);
    auto [ya, yb] = DatumAxisEndpoints(kAxisY);
    addDatumAxis(kAxisY, ya, yb);
  }

  // An ellipse's (or elliptical arc's) own five parameters -- centre,
  // focus1, minor radius -- plus its axis ends as points tied to it. A
  // circle-shaped one has no focus direction to solve, so it stays rigid:
  // the centre is free and every other point rides it.
  auto loadEllipse = [&](uint32_t id, glm::dvec2 center, double a, double b,
                         double rotation, GCS::Ellipse& ge) -> bool {
    const bool rigid = !(b > 0.0) || !(a > b * (1.0 + 1e-9));
    ge.center = addPoint(id, 0, center);
    if (rigid) {
      for (uint8_t w = kEllipseMajorPos; w <= kEllipseMinorNeg; w++)
        addRider(id, w, EllipseAxisEndUV(center, a, b, rotation, w), ge.center);
      im.ellipseLoad[id] = EllipseLoad{};
      return false;
    }
    const double c = std::sqrt((a - b) * (a + b));
    const glm::dvec2 u(std::cos(rotation), std::sin(rotation));
    im.foci.push_back(center.x + c * u.x);
    im.foci.push_back(center.y + c * u.y);
    ge.focus1 = GCS::Point(&im.foci[im.foci.size() - 2], &im.foci.back());
    im.radii.push_back(b);
    ge.radmin = &im.radii.back();
    EllipseLoad load;
    load.curve = &ge;
    load.offset0 = {*ge.focus1.x - center.x, *ge.focus1.y - center.y};
    load.b0 = b;
    im.ellipseLoad[id] = load;
    im.ellipseOf[id] = &ge;
    im.shapeParams[id] = {ge.focus1.x, ge.focus1.y, ge.radmin};
    // Soft holds on its size, eccentricity and turn: PlaneGCS solves
    // negatively tagged constraints as a second priority, minimised within
    // what the real ones allow and never counted in the diagnosis. Without
    // them the focus is a point like any other, so a coincident that moved
    // the centre left the focus behind and RESHAPED the ellipse (1.5 x 0.5
    // came out 0.85 x 0.68, turned 100 degrees), and levelling one shrank
    // it: the smallest step in the unknowns is not the smallest change in
    // the shape. (Solver::Load's first pass holds them hard.)
    const int tag = im.NewHoldTag();
    im.holdTags[id].shape = tag;
    im.holds.push_back(c);
    im.sys.addConstraintP2PDistance(ge.center, ge.focus1, &im.holds.back(),
                                    tag);
    im.holds.push_back(b);
    im.sys.addConstraintEqual(ge.radmin, &im.holds.back(), tag);
    im.holds.push_back(rotation);
    // (The overload without incrAngle drops its tag: this one keeps it.)
    im.sys.addConstraintP2PAngle(ge.center, ge.focus1, &im.holds.back(), 0.0,
                                 tag);
    for (uint8_t w = kEllipseMajorPos; w <= kEllipseMinorNeg; w++) {
      GCS::Point gp =
          addPoint(id, w, EllipseAxisEndUV(center, a, b, rotation, w));
      const auto [ax, ay] = AxisAlignment(w);
      im.sys.addConstraintInternalAlignmentPoint2Ellipse(ge, gp, ax, 0);
      im.sys.addConstraintInternalAlignmentPoint2Ellipse(ge, gp, ay, 0);
    }
    return true;
  };

  // A closed spline's first and last poles, tied once the fixed parameters
  // are known (below).
  std::vector<std::pair<GCS::Point, GCS::Point>> closures;

  // All solver maps are keyed by the entity's stable id, not its list index,
  // so constraint refs (which carry ids) resolve directly.
  for (uint32_t i = 0; i < entities.size(); i++) {
    const Entity& e = entities[i];
    const uint32_t id = EntityId(e);
    im.shapeIndexOf[id] = static_cast<int>(i);
    if (auto* pt = std::get_if<Point>(&e)) {
      addPoint(id, 0, pt->p);
    } else if (auto* ln = std::get_if<Line>(&e)) {
      GCS::Line gl;
      gl.p1 = addPoint(id, 0, ln->a);
      gl.p2 = addPoint(id, 1, ln->b);
      im.lines.push_back(gl);
      im.lineOf[id] = static_cast<int>(im.lines.size() - 1);
    } else if (auto* c = std::get_if<Circle>(&e)) {
      GCS::Circle gc;
      gc.center = addPoint(id, 0, c->center);
      im.radii.push_back(c->radius);
      gc.rad = &im.radii.back();
      im.circles.push_back(gc);
      im.circleOf[id] = static_cast<int>(im.circles.size() - 1);
      im.shapeParams[id] = {gc.rad};
    } else if (auto* ar = std::get_if<Arc>(&e)) {
      // center = which 0; start/end are real solver points (which 1/2) so
      // coincident/distance/fixed on an arc endpoint need no arc-specific code.
      GCS::Arc ga;
      ga.center = addPoint(id, 0, ar->center);
      // The endpoints in double, from the same centre, radius and angles the
      // arc rules read: rounded through any other path they start off
      // the rules, the solve "corrects" the ANGLES to match, and the write-
      // back keeps it -- an expression-driven sketch drifted a little on
      // every regen until its arcs no longer met their neighbours.
      auto onArc = [&](double a) {
        return glm::dvec2(ar->center) +
               ar->radius * glm::dvec2(std::cos(a), std::sin(a));
      };
      ga.start = addPoint(id, 1, onArc(ar->startAngle));
      ga.end = addPoint(id, 2, onArc(ar->endAngle));
      im.radii.push_back(ar->radius);
      ga.rad = &im.radii.back();
      im.angles.push_back(ar->startAngle);
      ga.startAngle = &im.angles.back();
      im.angles.push_back(ar->endAngle);
      ga.endAngle = &im.angles.back();
      im.arcs.push_back(ga);
      im.arcOf[id] = static_cast<int>(im.arcs.size() - 1);
      im.shapeParams[id] = {ga.rad, ga.startAngle, ga.endAngle};
      // Definitional: ties start/end points to center+radius+angles. Tag 0 so
      // it is never attributed to a user constraint (Phase I conflict mapping).
      im.sys.addConstraintArcRules(im.arcs.back(), 0);
    } else if (auto* el = std::get_if<Ellipse>(&e)) {
      im.ellipses.emplace_back();
      loadEllipse(id, el->center, el->rMajor, el->rMinor, el->rotation,
                  im.ellipses.back());
    } else if (auto* ea = std::get_if<EllipseArc>(&e)) {
      im.ellArcs.emplace_back();
      GCS::ArcOfEllipse& ga = im.ellArcs.back();
      const bool solved =
          loadEllipse(id, ea->center, ea->rMajor, ea->rMinor, ea->rotation, ga);
      // The ends in double from the stored params, as the arc's are.
      const glm::dvec2 s = EllipseArcEvalUV(*ea, ea->startParam);
      const glm::dvec2 t = EllipseArcEvalUV(*ea, ea->endParam);
      if (!solved) {
        addRider(id, 1, s, ga.center);
        addRider(id, 2, t, ga.center);
      } else {
        // PlaneGCS measures an elliptical arc's parameter from the major
        // axis, exactly as the model stores startParam/endParam.
        ga.start = addPoint(id, 1, s);
        ga.end = addPoint(id, 2, t);
        im.angles.push_back(ea->startParam);
        ga.startAngle = &im.angles.back();
        im.angles.push_back(ea->endParam);
        ga.endAngle = &im.angles.back();
        im.ellArcOf[id] = &ga;
        im.shapeParams[id].push_back(ga.startAngle);
        im.shapeParams[id].push_back(ga.endAngle);
        im.sys.addConstraintArcOfEllipseRules(ga, 0);
        // Soft holds on the ends too, so moving one end leaves the other.
        for (double* param : {ga.startAngle, ga.endAngle}) {
          const int tag = im.NewHoldTag();
          (param == ga.startAngle ? im.holdTags[id].start
                                  : im.holdTags[id].end) = tag;
          im.holds.push_back(*param);
          im.sys.addConstraintEqual(param, &im.holds.back(), tag);
        }
      }
    } else if (auto* bs = std::get_if<BSpline>(&e)) {
      // Each pole is a solver point (which = pole index); the curve itself
      // is a GCS::BSpline over those points with the clamped-uniform knots
      // and unit weights the model implies, both held fixed. which is
      // uint8_t, so at most 256 poles are addressable.
      const size_t nPoles = std::min<size_t>(bs->poles.size(), 256);
      std::vector<GCS::Point> poles;
      for (size_t k = 0; k < nPoles; k++)
        poles.push_back(addPoint(id, static_cast<uint8_t>(k), bs->poles[k]));
      BSpline clipped = *bs;
      clipped.poles.resize(nPoles);
      const int deg = BSplineDegree(clipped);
      if (nPoles >= 2 && deg >= 1 && nPoles == bs->poles.size()) {
        im.splines.emplace_back();
        GCS::BSpline& gb = im.splines.back();
        gb.poles = poles;
        for (size_t k = 0; k < nPoles; k++) {
          im.constants.push_back(1.0);
          gb.weights.push_back(&im.constants.back());
        }
        const int spans = static_cast<int>(nPoles) - deg;
        for (int k = 0; k <= spans; k++) {
          im.constants.push_back(static_cast<double>(k));
          gb.knots.push_back(&im.constants.back());
          gb.mult.push_back(k == 0 || k == spans ? deg + 1 : 1);
        }
        gb.degree = deg;
        gb.periodic = false;
        gb.start = poles.front();
        gb.end = poles.back();
        gb.setupFlattenedKnots();
        im.splineOf[id] = &gb;
        if (nPoles >= 3 && bs->poles.front() == bs->poles.back())
          closures.push_back({poles.front(), poles.back()});
      }
    } else if (auto* tx = std::get_if<Text>(&e)) {
      // The anchor (which 0) is the label's one free point; the glyphs are
      // rigid. The box corners (which 1..4) are solver points tied to it by
      // fixed offsets (tag 0, definitional), so the solver READS them — a
      // dimension or a coincidence on a corner places the label — but never
      // moves one relative to the others (docs/sketching/README.md#text).
      const GCS::Point anchor = addPoint(id, 0, tx->anchor);
      for (uint8_t k = 1; k <= 4; k++)
        addRider(id, k, TextPointUV(*tx, k), anchor);
    }
  }

  // Constraints. Record the first tag each one uses so conflicting/redundant
  // tags map back to constraint indices; a constraint owns tags
  // [firstTag[k], firstTag[k+1]) (empty range = it added nothing). One that
  // adds nothing is REFUSED with its reason (UnsupportedConstraints).
  int tag = 1;
  im.constraintFirstTag.assign(sketch.constraints.size() + 1, 0);
  im.constraints = sketch.constraints;
  im.valueSlots.assign(sketch.constraints.size(), {});
  const auto isLine = [&](uint32_t id) { return im.lineOf.count(id) != 0; };
  const auto isCircle = [&](uint32_t id) { return im.circleOf.count(id) != 0; };
  const auto isArc = [&](uint32_t id) { return im.arcOf.count(id) != 0; };
  const auto isEllipse = [&](uint32_t id) {
    return im.ellipseOf.count(id) != 0;
  };
  const auto isEllipseShaped = [&](uint32_t id) {
    const Entity* e = im.EntityOf(id);
    return e && (std::holds_alternative<Ellipse>(*e) ||
                 std::holds_alternative<EllipseArc>(*e));
  };
  const auto isSpline = [&](uint32_t id) { return im.splineOf.count(id) != 0; };
  for (size_t k = 0; k < sketch.constraints.size(); k++) {
    im.constraintFirstTag[k] = tag;
    const Constraint& con = sketch.constraints[k];
    GCS::Point* a = im.Resolve(con.a);
    GCS::Point* b = im.Resolve(con.b);
    std::string why; // set = refused, naming what was given
    const size_t paramsBefore = im.params.size();
    const auto describe = [&](const PointRef& r) -> std::string {
      if (IsDatum(r.entity))
        return r.entity == kOrigin ? "the origin" : "a datum axis";
      const Entity* e = im.EntityOf(r.entity);
      if (e == nullptr)
        return "entity " + std::to_string(r.entity) +
               ", which is not in this sketch";
      return std::string(EntityWord(*e)) + " (" + std::to_string(r.entity) +
             ")";
    };
    const auto pointProblem = [&](const PointRef& r) -> std::string {
      const Entity* e = im.EntityOf(r.entity);
      if (e == nullptr && !IsDatum(r.entity))
        return describe(r);
      return "[" + std::to_string(r.entity) + ", " + std::to_string(r.which) +
             "] is no point of " + describe(r);
    };
    switch (con.kind) {
    case ConstraintKind::Coincident:
      if (a && b)
        im.sys.addConstraintP2PCoincident(*a, *b, tag++);
      else
        why = pointProblem(a ? con.b : con.a);
      break;
    case ConstraintKind::Horizontal:
    case ConstraintKind::Vertical: {
      const bool horiz = con.kind == ConstraintKind::Horizontal;
      // Two-point form (both refs resolve to points): the pair shares a Y
      // (X) — used for point-to-point alignment. Otherwise a whole line, or
      // an ellipse's major axis (its ends 3 and 4).
      GCS::Point* p = a;
      GCS::Point* q = b;
      if (!(a && b)) {
        p = q = nullptr;
        if (isLine(con.a.entity)) {
          GCS::Line& l = im.lines[im.lineOf[con.a.entity]];
          p = &l.p1, q = &l.p2;
        } else if (isEllipseShaped(con.a.entity)) {
          p = im.Resolve({con.a.entity, kEllipseMajorPos});
          q = im.Resolve({con.a.entity, kEllipseMajorNeg});
        }
      }
      if (p && q) {
        if (horiz)
          im.sys.addConstraintHorizontal(*p, *q, tag++);
        else
          im.sys.addConstraintVertical(*p, *q, tag++);
      } else {
        why = "got " + describe(con.a);
      }
      break;
    }
    case ConstraintKind::Parallel:
    case ConstraintKind::Perpendicular:
    case ConstraintKind::Collinear:
      if (isLine(con.a.entity) && isLine(con.b.entity)) {
        GCS::Line& la = im.lines[im.lineOf[con.a.entity]];
        GCS::Line& lb = im.lines[im.lineOf[con.b.entity]];
        if (con.kind == ConstraintKind::Perpendicular) {
          im.sys.addConstraintPerpendicular(la, lb, tag++);
        } else {
          im.sys.addConstraintParallel(la, lb, tag++);
          // Collinear: parallel + one endpoint on the other's line.
          if (con.kind == ConstraintKind::Collinear)
            im.sys.addConstraintPointOnLine(la.p1, lb, tag++);
        }
      } else {
        why = "got " + describe(con.a) + " and " + describe(con.b);
      }
      break;
    case ConstraintKind::Equal: {
      const uint32_t ea = con.a.entity, eb = con.b.entity;
      const bool aC = isCircle(ea), aA = isArc(ea);
      const bool bC = isCircle(eb), bA = isArc(eb);
      if (isLine(ea) && isLine(eb))
        im.sys.addConstraintEqualLength(im.lines[im.lineOf[ea]],
                                        im.lines[im.lineOf[eb]], tag++);
      else if (aC && bC)
        im.sys.addConstraintEqualRadius(im.circles[im.circleOf[ea]],
                                        im.circles[im.circleOf[eb]], tag++);
      else if (aC && bA)
        im.sys.addConstraintEqualRadius(im.circles[im.circleOf[ea]],
                                        im.arcs[im.arcOf[eb]], tag++);
      else if (aA && bC) // overload is (Circle&, Arc&)
        im.sys.addConstraintEqualRadius(im.circles[im.circleOf[eb]],
                                        im.arcs[im.arcOf[ea]], tag++);
      else if (aA && bA)
        im.sys.addConstraintEqualRadius(im.arcs[im.arcOf[ea]],
                                        im.arcs[im.arcOf[eb]], tag++);
      else if (isEllipse(ea) && isEllipse(eb))
        im.sys.addConstraintEqualRadii(*im.ellipseOf[ea], *im.ellipseOf[eb],
                                       tag++);
      else if (isEllipseShaped(ea) && isEllipseShaped(eb))
        why = "a circle-shaped ellipse is rigid";
      else
        why = "got " + describe(con.a) + " and " + describe(con.b);
      break;
    }
    case ConstraintKind::Distance:
    case ConstraintKind::DistanceX:
    case ConstraintKind::DistanceY:
      if (a && b) {
        if (con.kind == ConstraintKind::Distance) {
          // Unsigned kinds take |value|: a negative-evaluating driving
          // expression means "flipped direction", which distances/radii
          // don't have — the magnitude still solves.
          im.params.push_back(std::abs(con.value)); // driving value, fixed
          im.sys.addConstraintP2PDistance(*a, *b, &im.params.back(), tag++);
        } else {
          im.params.push_back(con.value);
          if (con.kind == ConstraintKind::DistanceX)
            im.sys.addConstraintDifference(a->x, b->x, &im.params.back(),
                                           tag++);
          else
            im.sys.addConstraintDifference(a->y, b->y, &im.params.back(),
                                           tag++);
        }
      } else {
        why = pointProblem(a ? con.b : con.a);
      }
      break;
    case ConstraintKind::Radius:
    case ConstraintKind::Diameter: {
      const double r = std::abs(con.value) *
                       (con.kind == ConstraintKind::Diameter ? 0.5 : 1.0);
      if (isCircle(con.a.entity)) {
        im.params.push_back(r);
        im.sys.addConstraintCircleRadius(im.circles[im.circleOf[con.a.entity]],
                                         &im.params.back(), tag++);
      } else if (isArc(con.a.entity)) {
        im.params.push_back(r);
        im.sys.addConstraintArcRadius(im.arcs[im.arcOf[con.a.entity]],
                                      &im.params.back(), tag++);
      } else if (isEllipseShaped(con.a.entity)) {
        const std::string id = std::to_string(con.a.entity);
        why = "an ellipse has two radii, so this would have to pick one: "
              "dimension `distance` from its centre [" +
              id + ", 0] to a major-axis end [" + id +
              ", 3] or a minor-axis end [" + id + ", 5]";
      } else {
        why = "got " + describe(con.a);
      }
      break;
    }
    case ConstraintKind::Angle:
      if (isLine(con.a.entity) && isLine(con.b.entity)) {
        im.params.push_back(con.value); // angle (radians), driving
        im.sys.addConstraintL2LAngle(im.lines[im.lineOf[con.a.entity]],
                                     im.lines[im.lineOf[con.b.entity]],
                                     &im.params.back(), tag++);
      } else {
        why = "got " + describe(con.a) + " and " + describe(con.b);
      }
      break;
    case ConstraintKind::Tangent: {
      // Normalised so a line, when there is one, comes first.
      uint32_t ea = con.a.entity, eb = con.b.entity;
      if (!isLine(ea) && isLine(eb))
        std::swap(ea, eb);
      if (const auto joint = im.SharedEnd(sketch.constraints, ea, eb)) {
        // The two curves meet at an end (a fillet, a slot, a spline running
        // into a line): tangency AT that point, as the angle between them
        // there -- 0 or pi, whichever they already make. The distance form
        // (centre to line = radius) says the same thing as "the shared end
        // is on the arc" to first order AT exact tangency, so the pair read
        // as one redundant constraint and a free DOF -- every fillet
        // flagged, once the sketch held its geometry exactly.
        GCS::Curve* ca = im.TangentCurveAt(joint->first);
        GCS::Curve* cb = im.TangentCurveAt(joint->second);
        // The point the normals are read at: an (elliptical) arc's own end,
        // whose normal depends on where it is; a line's does not.
        const bool bCurved = isArc(eb) || im.ellArcOf.count(eb);
        GCS::Point* p = im.Resolve(bCurved ? joint->second : joint->first);
        if (isLine(ea) && isLine(eb)) {
          why = "two lines meeting at an end are not tangent; use collinear";
        } else if (ca && cb && p) {
          const double now = im.sys.calculateAngleViaPoint(*ca, *cb, *p);
          im.params.push_back(
              std::abs(std::remainder(now, 2.0 * M_PI)) < M_PI / 2.0 ? 0.0
                                                                     : M_PI);
          im.sys.addConstraintAngleViaPoint(*ca, *cb, *p, &im.params.back(),
                                            tag++);
        } else {
          why = "a circle-shaped elliptical arc is rigid";
        }
      } else if (isLine(ea) && (isCircle(eb) || isArc(eb))) {
        // The vendored PlaneGCS tangent is a *signed* point-to-line distance,
        // so pick the side the circle is already on and keep it there rather
        // than let the solve flip it across the line.
        GCS::Line& gl = im.lines[im.lineOf[ea]];
        const GCS::Point& center = isCircle(eb)
                                       ? im.circles[im.circleOf[eb]].center
                                       : im.arcs[im.arcOf[eb]].center;
        const double dx = *gl.p2.x - *gl.p1.x, dy = *gl.p2.y - *gl.p1.y;
        const double rx = *center.x - *gl.p1.x, ry = *center.y - *gl.p1.y;
        const bool ccw = (dx * ry - dy * rx) > 0.0;
        if (isCircle(eb))
          im.sys.addConstraintTangent(gl, im.circles[im.circleOf[eb]], ccw,
                                      tag++);
        else
          im.sys.addConstraintTangent(gl, im.arcs[im.arcOf[eb]], ccw, tag++);
      } else if (isLine(ea) && isEllipse(eb)) {
        // Unsigned (the focus reflected in the line), and a line cannot
        // cross an ellipse continuously without cutting it, so it keeps its
        // side.
        im.sys.addConstraintTangent(im.lines[im.lineOf[ea]], *im.ellipseOf[eb],
                                    tag++);
      } else if ((isCircle(ea) || isArc(ea)) && (isCircle(eb) || isArc(eb))) {
        // Inside or outside as they are now (PlaneGCS reads it at load).
        if (isCircle(ea) && isCircle(eb))
          im.sys.addConstraintTangent(im.circles[im.circleOf[ea]],
                                      im.circles[im.circleOf[eb]], tag++);
        else if (isArc(ea) && isArc(eb))
          im.sys.addConstraintTangent(im.arcs[im.arcOf[ea]],
                                      im.arcs[im.arcOf[eb]], tag++);
        else if (isCircle(ea))
          im.sys.addConstraintTangent(im.circles[im.circleOf[ea]],
                                      im.arcs[im.arcOf[eb]], tag++);
        else
          im.sys.addConstraintTangent(im.circles[im.circleOf[eb]],
                                      im.arcs[im.arcOf[ea]], tag++);
      } else {
        why = "got " + describe({ea, 0}) + " and " + describe({eb, 0});
        if (isSpline(ea) || isSpline(eb))
          why += "; a spline is tangent only at an end a coincident joins to "
                 "the other curve's end";
        else if (isEllipseShaped(ea) || isEllipseShaped(eb))
          why += "; an ellipse is tangent to a line, or at an end it shares "
                 "with another curve";
      }
      break;
    }
    case ConstraintKind::PointOnLine: {
      // "Point on curve": dispatches on the target — line, circle, arc,
      // ellipse (or elliptical arc: its ellipse), spline. The kind's name
      // predates the curves; the format keeps it.
      const uint32_t on = con.b.entity;
      if (!a)
        why = pointProblem(con.a);
      else if (isLine(on))
        im.sys.addConstraintPointOnLine(*a, im.lines[im.lineOf[on]], tag++);
      else if (isCircle(on))
        im.sys.addConstraintPointOnCircle(*a, im.circles[im.circleOf[on]],
                                          tag++);
      else if (isArc(on))
        im.sys.addConstraintPointOnArc(*a, im.arcs[im.arcOf[on]], tag++);
      else if (isEllipse(on))
        im.sys.addConstraintPointOnEllipse(*a, *im.ellipseOf[on], tag++);
      else if (isSpline(on)) {
        // The point's parameter along the curve is one more unknown, seeded
        // at the nearest point so the solve starts on the right span.
        const auto& bs = std::get<BSpline>(*im.EntityOf(on));
        im.curveParams.push_back(
            NearestSplineParam(bs, glm::dvec2(*a->x, *a->y)));
        im.sys.addConstraintPointOnBSpline(*a, *im.splineOf[on],
                                           &im.curveParams.back(), tag++);
      } else if (isEllipseShaped(on))
        why = "a circle-shaped ellipse is rigid";
      else
        why = "got " + describe(con.a) + " and " + describe(con.b);
      break;
    }
    case ConstraintKind::Symmetric:
      // a, b are points symmetric about the line c.entity — or about a POINT
      // when c resolves to one (the centre-rectangle anchor). The line check
      // runs first: a line ref {id, 0} would also resolve as its endpoint.
      if (a && b && isLine(con.c.entity)) {
        im.sys.addConstraintP2PSymmetric(
            *a, *b, im.lines[im.lineOf[con.c.entity]], tag++);
      } else if (GCS::Point* cp = im.Resolve(con.c); a && b && cp) {
        im.sys.addConstraintP2PSymmetric(*a, *b, *cp, tag++);
      } else {
        why = !a ? pointProblem(con.a)
                 : (!b ? pointProblem(con.b) : "got " + describe(con.c));
      }
      break;
    case ConstraintKind::Midpoint:
      // a is the midpoint of line b: b's endpoints are symmetric about a.
      if (a && isLine(con.b.entity)) {
        GCS::Line& l = im.lines[im.lineOf[con.b.entity]];
        im.sys.addConstraintP2PSymmetric(l.p1, l.p2, *a, tag++);
      } else {
        why = !a ? pointProblem(con.a) : "got " + describe(con.b);
      }
      break;
    case ConstraintKind::Fixed:
      // Pin the point a by holding its params out of the unknowns (no GCS
      // constraint; handled in DeclareUnknowns).
      if (a) {
        im.fixedParams.insert(a->x);
        im.fixedParams.insert(a->y);
      } else {
        why = pointProblem(con.a);
      }
      break;
    case ConstraintKind::Concentric: {
      // Two circles/arcs/ellipses share a centre: coincident of their centre
      // points (which 0 for all of them).
      GCS::Point* ca = im.Resolve({con.a.entity, 0});
      GCS::Point* cb = im.Resolve({con.b.entity, 0});
      const auto centred = [&](uint32_t id) {
        return isCircle(id) || isArc(id) || isEllipseShaped(id);
      };
      if (ca && cb && centred(con.a.entity) && centred(con.b.entity))
        im.sys.addConstraintP2PCoincident(*ca, *cb, tag++);
      else
        why = "got " + describe(con.a) + " and " + describe(con.b);
      break;
    }
    case ConstraintKind::DistancePL:
      // Perpendicular point-to-line distance. P2LDistance is SIGNED, so pick
      // the side the point is already on and keep it there (the Tangent
      // recipe) rather than let the solve flip it across the line.
      if (a && isLine(con.b.entity)) {
        GCS::Line& l = im.lines[im.lineOf[con.b.entity]];
        const double dx = *l.p2.x - *l.p1.x, dy = *l.p2.y - *l.p1.y;
        const double rx = *a->x - *l.p1.x, ry = *a->y - *l.p1.y;
        const bool ccw = (dx * ry - dy * rx) > 0.0;
        im.params.push_back(con.value);
        im.sys.addConstraintP2LDistance(*a, l, &im.params.back(), ccw, tag++);
      } else {
        why = !a ? pointProblem(con.a) : "got " + describe(con.b);
      }
      break;
    }
    if (why.empty() && con.driven && !IsDimension(con.kind))
      why = "only a dimension can be driven";
    if (!why.empty()) {
      im.unsupported.push_back({k, std::string(KindWord(con.kind)) + " takes " +
                                       Accepts(con.kind) + "; " + why});
    } else if (con.driven) {
      // A driven dimension adds no equation: dispatched like a driving one,
      // so what is refused cannot differ, then its equations come out again
      // and its tag range is left empty (docs/sketching/README.md#constraints).
      for (int t = im.constraintFirstTag[k]; t < tag; t++)
        im.sys.clearByTag(t);
      tag = im.constraintFirstTag[k];
    } else if (IsDimension(con.kind) && im.params.size() > paramsBefore) {
      // Each dimension case pushes its value last.
      using K = ConstraintKind;
      im.valueSlots[k].p = &im.params.back();
      im.valueSlots[k].absolute = con.kind == K::Distance ||
                                  con.kind == K::Radius ||
                                  con.kind == K::Diameter;
      im.valueSlots[k].scale = con.kind == K::Diameter ? 0.5 : 1.0;
    }
  }
  im.constraintFirstTag.back() = tag; // upper bound for the last constraint
  im.dragTag = tag;

  // Holds every unknown of one entity: its points and its shape.
  const auto fixEntity = [&](uint32_t genId) {
    for (const auto& [key, idx] : im.pointIndex) {
      if (static_cast<uint32_t>(key >> 8) != genId)
        continue;
      const GCS::Point& gp = im.gcsPoints[static_cast<size_t>(idx)];
      im.fixedParams.insert(gp.x);
      im.fixedParams.insert(gp.y);
    }
    if (const auto it = im.shapeParams.find(genId); it != im.shapeParams.end())
      for (double* p : it->second)
        im.fixedParams.insert(p);
  };

  // Linked-copy outputs are solver-FIXED anchors: their position is owned by
  // the sync (docs/sketching/README.md#linked-copies), so the solver may
  // reference but never move them.
  for (const auto& op : sketch.derived) {
    if (op.kind != DerivedKind::LinkedCopy)
      continue;
    for (const auto& inst : op.instances)
      fixEntity(inst.genId);
  }

  // External (projected) geometry is likewise solver-FIXED — its position is
  // owned by the source it projects, so the solve references but never moves
  // it.
  for (const auto& ext : sketch.external)
    for (uint32_t genId : ext.genIds)
      fixEntity(genId);

  // Expression-driven entity fields (Decision 2A) are solver-FIXED: their
  // value is owned by the document parameter table (applied in VisitExprs
  // before the solve), so the solver may reference but never move them —
  // the same mechanism as linked-copy outputs, but per scalar field.
  for (const auto& [id, fields] : sketch.entityExprs) {
    const auto fixCoord = [&](uint8_t which, bool xAxis) {
      const auto it = im.pointIndex.find(Impl::Key(id, which));
      if (it == im.pointIndex.end())
        return;
      const GCS::Point& gp = im.gcsPoints[static_cast<size_t>(it->second)];
      im.fixedParams.insert(xAxis ? gp.x : gp.y);
    };
    for (const auto& [field, expr] : fields) {
      if (expr.empty())
        continue;
      if (field == "x" || field == "ax" || field == "cx")
        fixCoord(0, /*xAxis=*/true);
      else if (field == "y" || field == "ay" || field == "cy")
        fixCoord(0, /*xAxis=*/false);
      else if (field == "bx")
        fixCoord(1, /*xAxis=*/true);
      else if (field == "by")
        fixCoord(1, /*xAxis=*/false);
      else if (const auto pf = PoleField(field); pf && pf->first < 256)
        fixCoord(static_cast<uint8_t>(pf->first), pf->second);
      else if (field == "r") {
        if (im.circleOf.count(id) != 0)
          im.fixedParams.insert(
              im.circles[static_cast<size_t>(im.circleOf[id])].rad);
        else if (im.arcOf.count(id) != 0)
          im.fixedParams.insert(im.arcs[static_cast<size_t>(im.arcOf[id])].rad);
      }
    }
  }

  // A closed spline stays closed: its first and last poles are one point.
  // Skipped when both are held (expression-driven, projected): the equation
  // would have no unknown in it.
  for (const auto& [p, q] : closures) {
    const bool pHeld = im.fixedParams.count(p.x) && im.fixedParams.count(p.y);
    const bool qHeld = im.fixedParams.count(q.x) && im.fixedParams.count(q.y);
    if (!(pHeld && qHeld)) {
      GCS::Point pp = p, qq = q;
      im.sys.addConstraintP2PCoincident(pp, qq, 0);
    }
  }
}

const std::vector<Solver::Unsupported>& Solver::UnsupportedConstraints() const {
  return m_impl->unsupported;
}

SolveResult Solver::Solve() {
  Impl& im = *m_impl;
  im.DeclareUnknowns();
  return im.RunSolve();
}

int Solver::SolveCount() const { return m_impl->solveCount; }

namespace {
// Continuation steps in sixty-fourths of the edit, this many solves at most.
// A step re-solves from the last one without reloading, so it is cheap next
// to the load's own diagnosis: forced into continuation, the largest
// sketches under scratch/ (51 constraints; 981 entities) cost what a plain
// load and solve does, and the cases in tests/test_sketch_solver.cpp [flip]
// take under 25 solves, an unreachable value included.
constexpr int kEditSteps = 64;
constexpr int kEditSolveBudget = 160;

std::string FormatValue(ConstraintKind k, double v) {
  char buf[64];
  if (k == ConstraintKind::Angle)
    std::snprintf(buf, sizeof buf, "%g deg", angles::ToDegrees(v));
  else
    std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

// The refusal text every value-edit path shows: the dimension, and what
// its new value would do.
std::string RefusalText(const std::vector<Constraint>& constraints,
                        const std::vector<ValueChange>& changes,
                        const Orientation* turned) {
  const ValueChange* named = changes.empty() ? nullptr : &changes.front();
  if (turned != nullptr)
    for (const ValueChange& c : changes)
      if (std::binary_search(turned->constraints.begin(),
                             turned->constraints.end(), c.constraint)) {
        named = &c;
        break;
      }
  std::string out = "the new values";
  if (named != nullptr) {
    const Constraint& con = constraints[named->constraint];
    out = std::string(KindWord(con.kind)) + " (constraint " +
          std::to_string(named->constraint) +
          ") = " + FormatValue(con.kind, con.value);
  }
  if (turned == nullptr)
    return out + " cannot be met";
  if (turned->kind == Orientation::Kind::ArcSweep)
    return out + " would turn arc " + std::to_string(turned->arc) +
           " inside out";
  out += " would mirror the triangle of constraints ";
  for (size_t i = 0; i < turned->constraints.size(); i++)
    out += (i ? ", " : "") + std::to_string(turned->constraints[i]);
  return out;
}
} // namespace

EditOutcome Solver::SolveValueEdit(const std::vector<ValueChange>& asked) {
  Impl& im = *m_impl;
  EditOutcome out;
  // Only the dimensions the solver holds a value for can step; an angle
  // steps from the turn nearest its new value.
  std::vector<ValueChange> changes;
  for (ValueChange c : asked) {
    if (c.constraint >= im.valueSlots.size() ||
        im.valueSlots[c.constraint].p == nullptr)
      continue;
    if (im.constraints[c.constraint].kind == ConstraintKind::Angle)
      c.from = c.to + std::remainder(c.from - c.to, 2.0 * M_PI);
    changes.push_back(c);
  }

  im.DeclareUnknowns();
  const Impl::Snapshot pre = im.Take();
  const std::vector<int> s0 = im.Signs();

  // One jump: an ordinary edit costs one solve and the check.
  out.solve = im.RunSolve();
  std::vector<size_t> turned;
  if (out.solve.ok) {
    turned = Impl::Turned(s0, im.Signs());
    if (turned.empty()) {
      out.reached = true;
      return out;
    }
  }

  // Continuation from the pre-edit geometry and the old values, in
  // sixty-fourths of the edit: side lengths stepped linearly never pass a
  // flat triangle, and each step keeps what was clear where it started.
  const auto continueAlong = [&]() {
    Impl::Put(pre);
    for (const ValueChange& c : changes)
      im.SetSlot(c, c.from);
    im.sys.initSolution(); // the reference every solve starts from
    const auto valueAt = [&](const ValueChange& c, int tn) {
      if (tn == kEditSteps)
        return c.to;
      const double f = static_cast<double>(tn) / kEditSteps;
      return c.from + (c.to - c.from) * f;
    };
    Impl::Snapshot at = pre;
    std::vector<int> signsAt = s0;
    int t = 0, step = kEditSteps / 2, budget = kEditSolveBudget;
    turned.clear();
    while (t < kEditSteps && step >= 1 && budget-- > 0) {
      const int tn = std::min(kEditSteps, t + step);
      for (const ValueChange& c : changes)
        im.SetSlot(c, valueAt(c, tn));
      const SolveResult r = im.RunSolve();
      std::vector<int> signs;
      std::vector<size_t> tr;
      if (r.ok) {
        signs = im.Signs();
        tr = Impl::Turned(signsAt, signs);
      }
      if (r.ok && tr.empty()) {
        t = tn;
        signsAt = std::move(signs);
        at = im.Take();
        im.sys.initSolution();
        step *= 2;
        out.solve = r;
      } else {
        turned = std::move(tr);
        if (!r.ok)
          out.solve = r;
        Impl::Put(at);
        im.sys.undoSolution();
        step /= 2;
      }
    }
    for (const ValueChange& c : changes)
      im.SetSlot(c, c.to);
    return t == kEditSteps;
  };
  if (!changes.empty() && continueAlong()) {
    out.reached = true;
    return out;
  }

  // Refused: the pre-edit geometry again.
  Impl::Put(pre);
  if (out.solve.ok && turned.empty())
    out.solve.ok = false; // ran out of steps short of the new values
  const std::vector<Orientation>& orients = im.Orientations();
  std::set<size_t> flipped;
  for (size_t i : turned)
    flipped.insert(orients[i].constraints.begin(),
                   orients[i].constraints.end());
  out.flipped.assign(flipped.begin(), flipped.end());
  out.refusal =
      RefusalText(im.constraints, changes,
                  turned.empty() ? nullptr : &orients[turned.front()]);
  return out;
}

std::vector<uint32_t> Solver::UnderDefinedEntities() const {
  Impl& im = *m_impl;
  GCS::VEC_pD dep;
  im.sys.getDependentParams(dep); // valid under the last solve's diagnosis
  const std::set<double*> free(dep.begin(), dep.end());
  // A constraint-free sketch has a zero-row Jacobian: diagnose() returns
  // before the dependent-parameter analysis, leaving the list empty even
  // though everything is free. Fall back to "free = not fixed" then; when
  // the analysis DID run (any constraint present) the QR list is exact.
  const bool analysisRan = !free.empty();
  const auto isFree = [&](double* p) {
    return analysisRan ? free.count(p) != 0 : im.fixedParams.count(p) == 0;
  };
  std::set<uint32_t> ids;
  const auto mark = [&](uint32_t entity) {
    if (entity != 0 && entity < 0xFFFFFF00u) // skip the datum pseudo-entities
      ids.insert(entity);
  };
  for (const auto& [key, idx] : im.pointIndex) {
    const GCS::Point& gp = im.gcsPoints[static_cast<size_t>(idx)];
    if (isFree(gp.x) || isFree(gp.y))
      mark(static_cast<uint32_t>(key >> 8));
  }
  for (const auto& [id, shape] : im.shapeParams)
    for (double* p : shape)
      if (isFree(p))
        mark(id);
  return {ids.begin(), ids.end()};
}

bool Solver::BeginDrag(const std::vector<PointRef>& points) {
  Impl& im = *m_impl;
  im.dragTarget.clear();
  im.dragTargetBase.assign(points.size(), -1);
  // A handle of an ellipse reshapes it: release the soft holds that handle
  // is FOR, and hold its centre softly instead, so the drag resizes and
  // turns the ellipse rather than sliding it whole (which every hold would
  // otherwise prefer). A major end keeps the minor radius; a minor end keeps
  // the major-axis ends (so the major radius and the turn); an arc end keeps
  // the shape and lets that end's parameter go. The centre itself is not a
  // handle: dragging it moves the ellipse.
  std::set<uint32_t> released;
  for (const PointRef& r : points) {
    const auto it = im.holdTags.find(r.entity);
    if (it == im.holdTags.end() || r.which == 0)
      continue;
    const Impl::HoldTags& tags = it->second;
    GCS::Ellipse& ge = *im.ellipseOf.at(r.entity);
    if (released.insert(r.entity).second) {
      im.holds.push_back(*ge.center.x);
      im.sys.addConstraintEqual(ge.center.x, &im.holds.back(),
                                GCS::DefaultTemporaryConstraint);
      im.holds.push_back(*ge.center.y);
      im.sys.addConstraintEqual(ge.center.y, &im.holds.back(),
                                GCS::DefaultTemporaryConstraint);
    }
    if (r.which == 1 && tags.start != 0)
      im.sys.clearByTag(tags.start);
    else if (r.which == 2 && tags.end != 0)
      im.sys.clearByTag(tags.end);
    else if (r.which >= kEllipseMajorPos && r.which <= kEllipseMinorNeg) {
      im.sys.clearByTag(tags.shape);
      if (r.which <= kEllipseMajorNeg) {
        im.holds.push_back(*ge.radmin);
        im.sys.addConstraintEqual(ge.radmin, &im.holds.back(),
                                  GCS::DefaultTemporaryConstraint);
      } else {
        for (uint8_t w : {kEllipseMajorPos, kEllipseMajorNeg}) {
          GCS::Point* p = im.Resolve({r.entity, w});
          im.holds.push_back(*p->x);
          im.sys.addConstraintEqual(p->x, &im.holds.back(),
                                    GCS::DefaultTemporaryConstraint);
          im.holds.push_back(*p->y);
          im.sys.addConstraintEqual(p->y, &im.holds.back(),
                                    GCS::DefaultTemporaryConstraint);
        }
      }
    }
  }
  // Pin each dragged point to a free target with a *weak* coincident, so the
  // real constraints dominate and the drag doesn't oscillate.
  for (size_t i = 0; i < points.size(); i++) {
    GCS::Point* gp = im.Resolve(points[i]);
    if (!gp)
      continue;
    const int base = static_cast<int>(im.dragTarget.size());
    im.dragTarget.push_back(*gp->x); // start at the point's current position
    im.dragTarget.push_back(*gp->y);
    GCS::Point target(&im.dragTarget[base], &im.dragTarget[base + 1]);
    int cid = im.sys.addConstraintP2PCoincident(*gp, target, im.dragTag++);
    im.sys.rescaleConstraint(cid, 0.01);
    im.dragTargetBase[i] = base;
  }
  im.DeclareUnknowns();
  const bool ok = im.RunSolve().conflicting.empty();
  im.ResetDragGuard();
  return ok;
}

void Solver::SetDragTargets(const std::vector<glm::dvec2>& targets) {
  Impl& im = *m_impl;
  for (size_t i = 0; i < targets.size() && i < im.dragTargetBase.size(); i++) {
    const int base = im.dragTargetBase[i];
    if (base < 0)
      continue;
    im.dragTarget[base] = targets[i].x;
    im.dragTarget[base + 1] = targets[i].y;
  }
}

bool Solver::BeginRadiusDrag(uint32_t circleId) {
  Impl& im = *m_impl;
  auto it = im.circleOf.find(circleId);
  if (it == im.circleOf.end())
    return false;
  im.dragRadiusTarget.clear();
  im.dragRadiusTarget.push_back(*im.circles[it->second].rad); // current radius
  int cid = im.sys.addConstraintCircleRadius(
      im.circles[it->second], &im.dragRadiusTarget[0], im.dragTag++);
  im.sys.rescaleConstraint(cid, 0.01);
  im.DeclareUnknowns();
  const bool ok = im.RunSolve().conflicting.empty();
  im.ResetDragGuard();
  return ok;
}

void Solver::SetRadiusTarget(double radius) {
  Impl& im = *m_impl;
  if (!im.dragRadiusTarget.empty())
    im.dragRadiusTarget[0] = radius;
}

SolveResult Solver::SolveDrag(DragGuard guard) {
  Impl& im = *m_impl;
  SolveResult r = im.RunSolve();
  if (guard == DragGuard::None || !r.ok)
    return r;
  std::vector<int> signs = im.Signs();
  const auto accept = [&](std::vector<int> s) {
    im.dragSigns = std::move(s);
    im.dragPrevTarget.assign(im.dragTarget.begin(), im.dragTarget.end());
    im.dragPrevRadius.assign(im.dragRadiusTarget.begin(),
                             im.dragRadiusTarget.end());
    im.dragState = im.Take();
  };
  if (Impl::Turned(im.dragSigns, signs).empty()) {
    accept(std::move(signs));
    return r;
  }
  // A fold: step the target from the last frame's, halving, from the last
  // frame's geometry, and keep the furthest step that holds.
  const std::vector<double> wanted(im.dragTarget.begin(), im.dragTarget.end());
  const std::vector<double> wantedR(im.dragRadiusTarget.begin(),
                                    im.dragRadiusTarget.end());
  const std::vector<double> fromT = im.dragPrevTarget;
  const std::vector<double> fromR = im.dragPrevRadius;
  const auto setTargets = [&](double f) {
    for (size_t i = 0; i < wanted.size() && i < fromT.size(); i++)
      im.dragTarget[i] = fromT[i] + (wanted[i] - fromT[i]) * f;
    for (size_t i = 0; i < wantedR.size() && i < fromR.size(); i++)
      im.dragRadiusTarget[i] = fromR[i] + (wantedR[i] - fromR[i]) * f;
  };
  Impl::Put(im.dragState);
  im.sys.initSolution();
  int t = 0, step = kEditSteps / 2, budget = kEditSolveBudget;
  while (t < kEditSteps && step >= 1 && budget-- > 0) {
    const int tn = std::min(kEditSteps, t + step);
    setTargets(static_cast<double>(tn) / kEditSteps);
    const SolveResult rr = im.RunSolve();
    std::vector<int> s;
    if (rr.ok)
      s = im.Signs();
    if (rr.ok && Impl::Turned(im.dragSigns, s).empty()) {
      t = tn;
      accept(std::move(s));
      im.sys.initSolution();
      step *= 2;
      r = rr;
    } else {
      Impl::Put(im.dragState);
      im.sys.undoSolution();
      step /= 2;
    }
  }
  setTargets(static_cast<double>(t) / kEditSteps);
  Impl::Put(im.dragState);
  r.ok = true; // the geometry holds where the fold stopped it
  return r;
}

namespace {
// The solved value, unless it moved by no more than rounding (1e-12 of the
// entity's size) from what was loaded -- then the loaded value, bit for bit.
// A solve of an already-solved curve re-derives its numbers through the
// focus and the curve rules and polishes the last ulps; written back every
// regeneration, that walks (the arc drift docs/sketching/README.md
// describes, one ulp at a time).
double Settle(double solved, double loaded, double size) {
  return std::abs(solved - loaded) <= 1e-12 * std::max(1.0, size) ? loaded
                                                                  : solved;
}
glm::dvec2 Settle(glm::dvec2 solved, glm::dvec2 loaded, double size) {
  return {Settle(solved.x, loaded.x, size), Settle(solved.y, loaded.y, size)};
}

// Keeps a swept (elliptical) arc CCW-always: the sweep in (0, 2π].
void NormalizeSweep(double start, double& end) {
  while (end <= start)
    end += 2.0 * M_PI;
  while (end - start > 2.0 * M_PI)
    end -= 2.0 * M_PI;
}
} // namespace

void Solver::WriteBack(std::vector<Entity>& entities) const {
  m_impl->WriteBackTo(entities);
}

void Solver::Impl::WriteBackTo(std::vector<Entity>& entities) const {
  const Impl& im = *this;
  entities = im.entityShapes;
  for (uint32_t i = 0; i < entities.size(); i++) {
    Entity& e = entities[i];
    const uint32_t id = EntityId(e);
    auto pointAt = [&](uint8_t which) -> glm::dvec2 {
      auto it = im.pointIndex.find(Impl::Key(id, which));
      const GCS::Point& gp = im.gcsPoints[it->second];
      return glm::dvec2(*gp.x, *gp.y);
    };
    // An ellipse's radii and rotation from its solved focus and minor
    // radius. Unmoved (to 1e-12 of its size) keeps the stored values bit for
    // bit: re-deriving them through the focus rounds by an ulp, and a sketch
    // re-solved every regeneration would walk.
    auto ellipseBack = [&](auto& g) {
      // The solver's centre, not g's (settled): the focus offset is measured
      // from where the solver put it.
      const glm::dvec2 c = pointAt(0);
      g.center = Settle(c, g.center, g.rMajor);
      const EllipseLoad& load = im.ellipseLoad.at(id);
      if (load.curve == nullptr)
        return; // rigid: it rides its centre
      const glm::dvec2 d(*load.curve->focus1.x - c.x,
                         *load.curve->focus1.y - c.y);
      const double b = *load.curve->radmin;
      const double tol = 1e-12 * std::max(1.0, g.rMajor);
      if (glm::length(d - load.offset0) <= tol && std::abs(b - load.b0) <= tol)
        return;
      g.rMinor = b;
      g.rMajor = std::sqrt(glm::dot(d, d) + b * b);
      // Nearest turn to the stored rotation, so a turn of 2π never shows.
      const double r = std::atan2(d.y, d.x);
      g.rotation += std::remainder(r - g.rotation, 2.0 * M_PI);
    };
    if (std::holds_alternative<Point>(e)) {
      std::get<Point>(e).p = pointAt(0);
    } else if (std::holds_alternative<Line>(e)) {
      std::get<Line>(e).a = pointAt(0);
      std::get<Line>(e).b = pointAt(1);
    } else if (std::holds_alternative<Circle>(e)) {
      auto& c = std::get<Circle>(e);
      c.center = pointAt(0);
      c.radius = *im.circles[im.circleOf.at(id)].rad;
    } else if (std::holds_alternative<Arc>(e)) {
      auto& ar = std::get<Arc>(e);
      const GCS::Arc& ga = im.arcs[im.arcOf.at(id)];
      ar.center = pointAt(0);
      ar.radius = *ga.rad;
      ar.startAngle = *ga.startAngle;
      ar.endAngle = *ga.endAngle;
      // Keep the CCW-always invariant: sweep in (0, 2π]. (The arc rules
      // read the angles through cos/sin, so a long solve can leave them a
      // whole turn further apart than the arc is.)
      NormalizeSweep(ar.startAngle, ar.endAngle);
    } else if (std::holds_alternative<EllipseArc>(e)) {
      auto& ea = std::get<EllipseArc>(e);
      ellipseBack(ea);
      if (const auto it = im.ellArcOf.find(id); it != im.ellArcOf.end()) {
        const double s0 = ea.startParam, e0 = ea.endParam;
        ea.startParam = Settle(*it->second->startAngle, s0, 1.0);
        ea.endParam = Settle(*it->second->endAngle, e0, 1.0);
        NormalizeSweep(ea.startParam, ea.endParam);
      }
    } else if (std::holds_alternative<Ellipse>(e)) {
      ellipseBack(std::get<Ellipse>(e));
    } else if (std::holds_alternative<Text>(e)) {
      std::get<Text>(e).anchor = pointAt(0); // the glyphs and box ride it
    } else if (std::holds_alternative<BSpline>(e)) {
      auto& bs = std::get<BSpline>(e);
      const size_t nPoles = std::min<size_t>(bs.poles.size(), 256);
      double size = 0.0;
      for (const glm::dvec2& p : bs.poles)
        size = std::max(size, std::max(std::abs(p.x), std::abs(p.y)));
      for (size_t k = 0; k < nPoles; k++)
        bs.poles[k] =
            Settle(pointAt(static_cast<uint8_t>(k)), bs.poles[k], size);
    }
  }
}

} // namespace sketch

#else // no PlaneGCS: no-op stub

namespace sketch {
struct Solver::Impl {
  std::vector<Entity> shapes;
  std::vector<Solver::Unsupported> unsupported;
};
Solver::Solver() : m_impl(new Impl) {}
Solver::~Solver() { delete m_impl; }
void Solver::Load(const SketchFeature& s) { m_impl->shapes = s.entities; }
SolveResult Solver::Solve() {
  LOG_WARN("SketchSolver: built without PlaneGCS; constraints ignored");
  return {true, -1, {}, {}};
}
bool Solver::BeginDrag(const std::vector<PointRef>&) { return false; }
void Solver::SetDragTargets(const std::vector<glm::dvec2>&) {}
bool Solver::BeginRadiusDrag(uint32_t) { return false; }
void Solver::SetRadiusTarget(double) {}
SolveResult Solver::SolveDrag(DragGuard) { return {true, -1, {}, {}}; }
EditOutcome Solver::SolveValueEdit(const std::vector<ValueChange>&) {
  EditOutcome out;
  out.solve = Solve();
  out.reached = true;
  return out;
}
int Solver::SolveCount() const { return 0; }
std::vector<uint32_t> Solver::UnderDefinedEntities() const { return {}; }
const std::vector<Solver::Unsupported>& Solver::UnsupportedConstraints() const {
  return m_impl->unsupported;
}
void Solver::WriteBack(std::vector<Entity>& e) const { e = m_impl->shapes; }
} // namespace sketch

#endif

namespace sketch {

namespace {
// Past this many constraints a refused redundancy is not traced to the
// constraints it repeats (one solve per constraint).
constexpr size_t kMaxNamedRedundancy = 200;

std::vector<bool> DrivingFlags(const SketchFeature& sketch) {
  std::vector<bool> out;
  out.reserve(sketch.constraints.size());
  for (const Constraint& c : sketch.constraints)
    out.push_back(!c.driven);
  return out;
}

std::set<size_t> Broken(const SolveResult& r) {
  std::set<size_t> out(r.conflictingConstraints.begin(),
                       r.conflictingConstraints.end());
  out.insert(r.redundantConstraints.begin(), r.redundantConstraints.end());
  return out;
}

std::string IndexList(const std::vector<size_t>& indices) {
  std::string out;
  for (size_t i : indices)
    out += (out.empty() ? "" : ", ") + std::to_string(i);
  return out;
}
} // namespace

AddVerdict JudgeAddition(const SketchFeature& before,
                         const SketchFeature& after, Solver* solved) {
  Solver s;
  s.Load(before);
  return JudgeAddition(s.Solve(), DrivingFlags(before), after, solved);
}

AddVerdict JudgeAddition(const SolveResult& before,
                         const std::vector<bool>& beforeDriving,
                         const SketchFeature& after, Solver* solved) {
  AddVerdict v;
  v.before = before;
  const size_t had = beforeDriving.size();
  Solver local;
  Solver& s = solved != nullptr ? *solved : local;
  s.Load(after);
  for (const auto& bad : s.UnsupportedConstraints()) {
    const bool added = bad.index >= had;
    const bool nowDriving = !added && beforeDriving[bad.index] == false &&
                            !after.constraints[bad.index].driven;
    if (added || nowDriving) {
      v.unsupported = bad.index;
      v.reason = bad.reason;
      return v;
    }
  }
  v.after = s.Solve();

  // Did anything start to drive? An appended driving constraint, or a
  // dimension switched from driven to driving.
  bool drives = false;
  for (size_t k = 0; k < after.constraints.size(); k++)
    if (!after.constraints[k].driven && (k >= had || !beforeDriving[k]))
      drives = true;

  const std::set<size_t> was = Broken(before), now = Broken(v.after);
  bool newlyBroken = false;
  for (size_t k : now)
    if (!was.count(k))
      newlyBroken = true;
  const bool solves = v.after.ok || !before.ok;
  const bool lowers = !drives || before.dof < 0 ||
                      (v.after.dof >= 0 && v.after.dof < before.dof);
  v.accepted = solves && !newlyBroken && lowers;
  if (v.accepted)
    return v;

  // Name what the addition runs into: the constraints already in the sketch
  // that it leaves broken.
  const bool conflicts = !v.after.conflictingConstraints.empty() &&
                         !std::all_of(v.after.conflictingConstraints.begin(),
                                      v.after.conflictingConstraints.end(),
                                      [&](size_t k) { return was.count(k); });
  if (conflicts || !solves) {
    // PlaneGCS names every member of a conflicting group.
    for (size_t k : now)
      if (k < had && beforeDriving[k] && !was.count(k))
        v.involved.push_back(k);
  } else if (newlyBroken && after.constraints.size() <= kMaxNamedRedundancy) {
    // A redundancy it names on one side only, usually the new constraint.
    // What that one repeats is every driving constraint without which it
    // would constrain: each is tried out of the solve. Only on a refusal,
    // and bounded.
    for (size_t k = 0; k < had; k++) {
      if (after.constraints[k].driven || !beforeDriving[k] || was.count(k))
        continue; // not driving, the addition itself, or broken already
      SketchFeature without = after;
      without.constraints.erase(without.constraints.begin() +
                                static_cast<std::ptrdiff_t>(k));
      Solver t;
      t.Load(without);
      const SolveResult r = t.Solve();
      bool clean = true;
      for (size_t b : Broken(r))
        if (!was.count(b < k ? b : b + 1))
          clean = false;
      if (clean)
        v.involved.push_back(k);
    }
  }
  const std::string with =
      v.involved.empty()
          ? std::string()
          : " (constraint" + std::string(v.involved.size() > 1 ? "s " : " ") +
                IndexList(v.involved) + ")";
  if (conflicts || !solves)
    v.reason = "that contradicts what the sketch already fixes" + with;
  else
    v.reason = "the sketch already fixes that" + with;
  return v;
}

} // namespace sketch
