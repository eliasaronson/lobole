#include "core/SketchSolver.h"

#include "Logger.h"

#include <cmath>
#include <deque>
#include <map>
#include <set>

#ifdef HAVE_PLANEGCS

#include "GCS.h"

namespace sketch {

namespace {
// A point's two parameter slots within the flat param array.
struct PointParams {
  int x, y;
};
} // namespace

struct Solver::Impl {
  GCS::System sys;
  // deque: element addresses stay stable across push_back (GCS holds raw
  // pointers into these), so no reservation bookkeeping is needed.
  std::deque<double> params;         // point coords + dimension values
  std::deque<double> radii;          // circle + arc radii
  std::deque<double> angles;         // arc start/end angles
  std::vector<GCS::Point> gcsPoints; // one per (entity, which) we expose
  std::map<uint64_t, int> pointIndex; // (entity<<8 | which) -> gcsPoints index
  std::vector<GCS::Line> lines;      // index-aligned with line entities
  std::map<uint32_t, int> lineOf;    // entity index -> lines index
  std::vector<GCS::Circle> circles;
  std::map<uint32_t, int> circleOf;  // entity id -> circles index
  std::vector<GCS::Arc> arcs;
  std::map<uint32_t, int> arcOf;     // entity id -> arcs index

  // Loaded sketch shape for write-back
  std::vector<Entity> entityShapes;
  std::map<uint32_t, int> shapeIndexOf; // entity id -> index in entityShapes

  int dragTag = 0;
  std::deque<double> dragTarget;       // 2 slots per dragged point (x, y)
  std::vector<int> dragTargetBase;     // dragTarget index base per input point
  std::deque<double> dragRadiusTarget; // 1 slot for a radius drag
  std::set<double*> fixedParams;       // params held out of the unknowns (Fixed)
  std::vector<int> constraintFirstTag; // per-constraint first GCS tag (+ end)

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

  // Declares every point coord + radius as an unknown and initialises the
  // solution structure. Call after all constraints (incl. drag) are added.
  void DeclareUnknowns() {
    GCS::VEC_pD unknowns;
    for (auto& gp : gcsPoints) {
      if (!fixedParams.count(gp.x)) // Fixed points stay put
        unknowns.push_back(gp.x);
      if (!fixedParams.count(gp.y))
        unknowns.push_back(gp.y);
    }
    for (auto& r : radii)
      unknowns.push_back(&r);
    for (auto& ang : angles) // arc start/end angles (tied to points by ArcRules)
      unknowns.push_back(&ang);
    if (unknowns.empty())
      return;
    sys.declareUnknowns(unknowns);
    sys.initSolution();
  }

  // Solves the already-declared system and reports the verdict.
  SolveResult RunSolve() {
    SolveResult result;
    // Trivial when nothing is free to solve. gcsPoints is never empty now (the
    // datum origin/axes are always registered) but they're all fixed, so check
    // for any free param instead.
    bool anyFree = !radii.empty() || !angles.empty();
    for (auto& gp : gcsPoints) {
      if (anyFree)
        break;
      if (!fixedParams.count(gp.x) || !fixedParams.count(gp.y))
        anyFree = true;
    }
    if (!anyFree) {
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
  Impl& im = *m_impl;
  im = Impl{}; // reset

  im.entityShapes = sketch.entities;

  // Reserve param storage up front so addresses stay stable (GCS keeps raw
  // pointers into `params`).
  size_t nParams = 0;
  for (const auto& e : sketch.entities) {
    if (std::holds_alternative<Point>(e)) nParams += 2;
    else if (std::holds_alternative<Line>(e)) nParams += 4;
    else if (std::holds_alternative<Circle>(e)) nParams += 3;
    else if (std::holds_alternative<Arc>(e)) nParams += 6; // 3 pts + rad + 2 ang
    else if (std::holds_alternative<Ellipse>(e)) nParams += 2; // centre only
  }
  (void)nParams; // deque addresses are stable; no reservation needed

  auto addPoint = [&](uint32_t entity, uint8_t which, glm::vec2 p) {
    im.params.push_back(p.x);
    im.params.push_back(p.y);
    GCS::Point gp(&im.params[im.params.size() - 2],
                  &im.params[im.params.size() - 1]);
    im.gcsPoints.push_back(gp);
    im.pointIndex[Impl::Key(entity, which)] =
        static_cast<int>(im.gcsPoints.size() - 1);
    return gp;
  };

  // Datum geometry: a fixed origin point and the two axis lines, always present
  // so constraints / mirrors / snaps can reference them. Held out of the
  // unknowns (fixedParams), so they add no DOF and never move.
  auto fixPoint = [&](GCS::Point gp) {
    im.fixedParams.insert(gp.x);
    im.fixedParams.insert(gp.y);
  };
  auto addDatumAxis = [&](uint32_t id, glm::vec2 a, glm::vec2 b) {
    GCS::Line gl;
    gl.p1 = addPoint(id, 0, a);
    gl.p2 = addPoint(id, 1, b);
    fixPoint(gl.p1);
    fixPoint(gl.p2);
    im.lines.push_back(gl);
    im.lineOf[id] = static_cast<int>(im.lines.size() - 1);
  };
  fixPoint(addPoint(kOrigin, 0, {0.0f, 0.0f}));
  {
    auto [xa, xb] = DatumAxisEndpoints(kAxisX);
    addDatumAxis(kAxisX, xa, xb);
    auto [ya, yb] = DatumAxisEndpoints(kAxisY);
    addDatumAxis(kAxisY, ya, yb);
  }

  // All solver maps are keyed by the entity's stable id, not its list index,
  // so constraint refs (which carry ids) resolve directly.
  for (uint32_t i = 0; i < sketch.entities.size(); i++) {
    const Entity& e = sketch.entities[i];
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
    } else if (auto* ar = std::get_if<Arc>(&e)) {
      // center = which 0; start/end are real solver points (which 1/2) so
      // coincident/distance/fixed on an arc endpoint need no arc-specific code.
      GCS::Arc ga;
      ga.center = addPoint(id, 0, ar->center);
      ga.start = addPoint(id, 1, ArcPointUV(*ar, 1));
      ga.end = addPoint(id, 2, ArcPointUV(*ar, 2));
      im.radii.push_back(ar->radius);
      ga.rad = &im.radii.back();
      im.angles.push_back(ar->startAngle);
      ga.startAngle = &im.angles.back();
      im.angles.push_back(ar->endAngle);
      ga.endAngle = &im.angles.back();
      im.arcs.push_back(ga);
      im.arcOf[id] = static_cast<int>(im.arcs.size() - 1);
      // Definitional: ties start/end points to center+radius+angles. Tag 0 so
      // it is never attributed to a user constraint (Phase I conflict mapping).
      im.sys.addConstraintArcRules(im.arcs.back(), 0);
    } else if (auto* el = std::get_if<Ellipse>(&e)) {
      // v1: only the centre is a solver point (so coincident/distance/fixed on
      // the centre work and the ellipse translates with it); the radii and
      // rotation are rigid — see CONSTRAINTS_PHASE_L.md for the full-rules TODO.
      (void)el;
      addPoint(id, 0, el->center);
    }
    // Rectangle: rigid in v1, not added to the solver.
  }

  // Constraints. Record the first tag each one uses so conflicting/redundant
  // tags map back to constraint indices; a constraint owns tags
  // [firstTag[k], firstTag[k+1]) (empty range = it added nothing).
  int tag = 1;
  im.constraintFirstTag.assign(sketch.constraints.size() + 1, 0);
  for (size_t k = 0; k < sketch.constraints.size(); k++) {
    im.constraintFirstTag[k] = tag;
    const Constraint& con = sketch.constraints[k];
    GCS::Point* a = im.Resolve(con.a);
    GCS::Point* b = im.Resolve(con.b);
    switch (con.kind) {
    case ConstraintKind::Coincident:
      if (a && b) im.sys.addConstraintP2PCoincident(*a, *b, tag++);
      break;
    case ConstraintKind::Horizontal:
      // Two-point form (both refs resolve to points): the pair shares a Y — used
      // for point-to-point alignment. Otherwise the whole-line form.
      if (a && b) im.sys.addConstraintHorizontal(*a, *b, tag++);
      else if (im.lineOf.count(con.a.entity))
        im.sys.addConstraintHorizontal(im.lines[im.lineOf[con.a.entity]], tag++);
      break;
    case ConstraintKind::Vertical:
      if (a && b) im.sys.addConstraintVertical(*a, *b, tag++); // pair shares an X
      else if (im.lineOf.count(con.a.entity))
        im.sys.addConstraintVertical(im.lines[im.lineOf[con.a.entity]], tag++);
      break;
    case ConstraintKind::Parallel:
      if (im.lineOf.count(con.a.entity) && im.lineOf.count(con.b.entity))
        im.sys.addConstraintParallel(im.lines[im.lineOf[con.a.entity]],
                                     im.lines[im.lineOf[con.b.entity]], tag++);
      break;
    case ConstraintKind::Perpendicular:
      if (im.lineOf.count(con.a.entity) && im.lineOf.count(con.b.entity))
        im.sys.addConstraintPerpendicular(im.lines[im.lineOf[con.a.entity]],
                                          im.lines[im.lineOf[con.b.entity]],
                                          tag++);
      break;
    case ConstraintKind::Equal: {
      const uint32_t ea = con.a.entity, eb = con.b.entity;
      const bool aC = im.circleOf.count(ea), aA = im.arcOf.count(ea);
      const bool bC = im.circleOf.count(eb), bA = im.arcOf.count(eb);
      if (im.lineOf.count(ea) && im.lineOf.count(eb))
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
      break;
    }
    case ConstraintKind::Distance:
      if (a && b) {
        im.params.push_back(con.value); // dimension value (driving, fixed)
        im.sys.addConstraintP2PDistance(*a, *b, &im.params.back(), tag++);
      }
      break;
    case ConstraintKind::Radius:
      if (im.circleOf.count(con.a.entity)) {
        im.params.push_back(con.value);
        im.sys.addConstraintCircleRadius(im.circles[im.circleOf[con.a.entity]],
                                         &im.params.back(), tag++);
      } else if (im.arcOf.count(con.a.entity)) {
        im.params.push_back(con.value);
        im.sys.addConstraintArcRadius(im.arcs[im.arcOf[con.a.entity]],
                                      &im.params.back(), tag++);
      }
      break;
    case ConstraintKind::Angle:
      if (im.lineOf.count(con.a.entity) && im.lineOf.count(con.b.entity)) {
        im.params.push_back(con.value); // angle (radians), driving
        im.sys.addConstraintL2LAngle(im.lines[im.lineOf[con.a.entity]],
                                     im.lines[im.lineOf[con.b.entity]],
                                     &im.params.back(), tag++);
      }
      break;
    case ConstraintKind::Tangent:
      if (im.lineOf.count(con.a.entity) && im.circleOf.count(con.b.entity)) {
        // The vendored PlaneGCS tangent is a *signed* point-to-line distance,
        // so pick the side the circle is already on and keep it there rather
        // than let the solve flip it across the line.
        const auto& ln =
            std::get<Line>(im.entityShapes[im.shapeIndexOf[con.a.entity]]);
        const auto& circ =
            std::get<Circle>(im.entityShapes[im.shapeIndexOf[con.b.entity]]);
        glm::vec2 d = ln.b - ln.a;
        glm::vec2 r = circ.center - ln.a;
        bool ccw = (d.x * r.y - d.y * r.x) > 0.0f;
        im.sys.addConstraintTangent(im.lines[im.lineOf[con.a.entity]],
                                    im.circles[im.circleOf[con.b.entity]], ccw,
                                    tag++);
      } else if (im.lineOf.count(con.a.entity) && im.arcOf.count(con.b.entity)) {
        const auto& ln =
            std::get<Line>(im.entityShapes[im.shapeIndexOf[con.a.entity]]);
        const auto& arc =
            std::get<Arc>(im.entityShapes[im.shapeIndexOf[con.b.entity]]);
        glm::vec2 d = ln.b - ln.a;
        glm::vec2 r = arc.center - ln.a;
        bool ccw = (d.x * r.y - d.y * r.x) > 0.0f;
        im.sys.addConstraintTangent(im.lines[im.lineOf[con.a.entity]],
                                    im.arcs[im.arcOf[con.b.entity]], ccw, tag++);
      }
      break;
    case ConstraintKind::PointOnLine:
      if (a && im.lineOf.count(con.b.entity))
        im.sys.addConstraintPointOnLine(*a, im.lines[im.lineOf[con.b.entity]],
                                        tag++);
      break;
    case ConstraintKind::Symmetric:
      // a, b are points symmetric about the line c.entity.
      if (a && b && im.lineOf.count(con.c.entity))
        im.sys.addConstraintP2PSymmetric(
            *a, *b, im.lines[im.lineOf[con.c.entity]], tag++);
      break;
    case ConstraintKind::Midpoint:
      // a is the midpoint of line b: b's endpoints are symmetric about a.
      if (a && im.lineOf.count(con.b.entity)) {
        GCS::Line& l = im.lines[im.lineOf[con.b.entity]];
        im.sys.addConstraintP2PSymmetric(l.p1, l.p2, *a, tag++);
      }
      break;
    case ConstraintKind::Fixed:
      // Pin the point a by holding its params out of the unknowns (no GCS
      // constraint; handled in DeclareUnknowns).
      if (a) {
        im.fixedParams.insert(a->x);
        im.fixedParams.insert(a->y);
      }
      break;
    case ConstraintKind::Concentric: {
      // Two circles/arcs share a center: coincident of their center points
      // (center is which 0 for both circles and arcs).
      GCS::Point* ca = im.Resolve({con.a.entity, 0});
      GCS::Point* cb = im.Resolve({con.b.entity, 0});
      const bool aCurve =
          im.circleOf.count(con.a.entity) || im.arcOf.count(con.a.entity);
      const bool bCurve =
          im.circleOf.count(con.b.entity) || im.arcOf.count(con.b.entity);
      if (ca && cb && aCurve && bCurve)
        im.sys.addConstraintP2PCoincident(*ca, *cb, tag++);
      break;
    }
    case ConstraintKind::Collinear:
      // Parallel + one endpoint on the other line = on the same infinite line.
      if (im.lineOf.count(con.a.entity) && im.lineOf.count(con.b.entity)) {
        GCS::Line& la = im.lines[im.lineOf[con.a.entity]];
        GCS::Line& lb = im.lines[im.lineOf[con.b.entity]];
        im.sys.addConstraintParallel(la, lb, tag++);
        im.sys.addConstraintPointOnLine(la.p1, lb, tag++);
      }
      break;
    case ConstraintKind::DistanceX:
      if (a && b) {
        im.params.push_back(con.value);
        im.sys.addConstraintDifference(a->x, b->x, &im.params.back(), tag++);
      }
      break;
    case ConstraintKind::DistanceY:
      if (a && b) {
        im.params.push_back(con.value);
        im.sys.addConstraintDifference(a->y, b->y, &im.params.back(), tag++);
      }
      break;
    case ConstraintKind::Diameter:
      if (im.circleOf.count(con.a.entity)) {
        im.params.push_back(con.value * 0.5); // radius = diameter / 2
        im.sys.addConstraintCircleRadius(im.circles[im.circleOf[con.a.entity]],
                                         &im.params.back(), tag++);
      } else if (im.arcOf.count(con.a.entity)) {
        im.params.push_back(con.value * 0.5);
        im.sys.addConstraintArcRadius(im.arcs[im.arcOf[con.a.entity]],
                                      &im.params.back(), tag++);
      }
      break;
    }
  }
  im.constraintFirstTag.back() = tag; // upper bound for the last constraint
  im.dragTag = tag;
}

SolveResult Solver::Solve() {
  Impl& im = *m_impl;
  im.DeclareUnknowns();
  return im.RunSolve();
}

bool Solver::BeginDrag(const std::vector<PointRef>& points) {
  Impl& im = *m_impl;
  im.dragTarget.clear();
  im.dragTargetBase.assign(points.size(), -1);
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
  return im.RunSolve().conflicting.empty();
}

void Solver::SetDragTargets(const std::vector<glm::vec2>& targets) {
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
  return im.RunSolve().conflicting.empty();
}

void Solver::SetRadiusTarget(double radius) {
  Impl& im = *m_impl;
  if (!im.dragRadiusTarget.empty())
    im.dragRadiusTarget[0] = radius;
}

SolveResult Solver::SolveDrag() { return m_impl->RunSolve(); }

void Solver::WriteBack(std::vector<Entity>& entities) const {
  const Impl& im = *m_impl;
  entities = im.entityShapes;
  for (uint32_t i = 0; i < entities.size(); i++) {
    Entity& e = entities[i];
    const uint32_t id = EntityId(e);
    auto pointAt = [&](uint8_t which) -> glm::vec2 {
      auto it = im.pointIndex.find(Impl::Key(id, which));
      const GCS::Point& gp = im.gcsPoints[it->second];
      return glm::vec2(*gp.x, *gp.y);
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
      // Keep the CCW-always invariant: sweep in (0, 2π].
      while (ar.endAngle <= ar.startAngle)
        ar.endAngle += 2.0 * M_PI;
    } else if (std::holds_alternative<Ellipse>(e)) {
      std::get<Ellipse>(e).center = pointAt(0); // radii/rotation rigid
    }
  }
}

} // namespace sketch

#else // no PlaneGCS: no-op stub

namespace sketch {
struct Solver::Impl {
  std::vector<Entity> shapes;
};
Solver::Solver() : m_impl(new Impl) {}
Solver::~Solver() { delete m_impl; }
void Solver::Load(const SketchFeature& s) { m_impl->shapes = s.entities; }
SolveResult Solver::Solve() {
  LOG_WARN("SketchSolver: built without PlaneGCS; constraints ignored");
  return {true, -1, {}, {}};
}
bool Solver::BeginDrag(const std::vector<PointRef>&) { return false; }
void Solver::SetDragTargets(const std::vector<glm::vec2>&) {}
bool Solver::BeginRadiusDrag(uint32_t) { return false; }
void Solver::SetRadiusTarget(double) {}
SolveResult Solver::SolveDrag() { return {true, -1, {}, {}}; }
void Solver::WriteBack(std::vector<Entity>& e) const { e = m_impl->shapes; }
} // namespace sketch

#endif
