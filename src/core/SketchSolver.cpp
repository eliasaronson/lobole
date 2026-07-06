#include "core/SketchSolver.h"

#include "Logger.h"

#include <deque>
#include <map>

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
  std::deque<double> radii;          // circle radii
  std::vector<GCS::Point> gcsPoints; // one per (entity, which) we expose
  std::map<uint64_t, int> pointIndex; // (entity<<8 | which) -> gcsPoints index
  std::vector<GCS::Line> lines;      // index-aligned with line entities
  std::map<uint32_t, int> lineOf;    // entity index -> lines index
  std::vector<GCS::Circle> circles;
  std::map<uint32_t, int> circleOf;  // entity id -> circles index

  // Loaded sketch shape for write-back
  std::vector<Entity> entityShapes;
  std::map<uint32_t, int> shapeIndexOf; // entity id -> index in entityShapes

  int dragTag = 0;
  std::deque<double> dragTarget; // 2 slots when dragging

  static uint64_t Key(uint32_t entity, uint8_t which) {
    return (uint64_t(entity) << 8) | which;
  }

  GCS::Point* Resolve(const PointRef& r) {
    auto it = pointIndex.find(Key(r.entity, r.which));
    if (it == pointIndex.end())
      return nullptr;
    return &gcsPoints[it->second];
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
    }
    // Rectangle: rigid in v1, not added to the solver.
  }

  // Constraints
  int tag = 1;
  for (const auto& con : sketch.constraints) {
    GCS::Point* a = im.Resolve(con.a);
    GCS::Point* b = im.Resolve(con.b);
    switch (con.kind) {
    case ConstraintKind::Coincident:
      if (a && b) im.sys.addConstraintP2PCoincident(*a, *b, tag++);
      break;
    case ConstraintKind::Horizontal:
      if (im.lineOf.count(con.a.entity))
        im.sys.addConstraintHorizontal(im.lines[im.lineOf[con.a.entity]], tag++);
      break;
    case ConstraintKind::Vertical:
      if (im.lineOf.count(con.a.entity))
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
    case ConstraintKind::Equal:
      if (im.lineOf.count(con.a.entity) && im.lineOf.count(con.b.entity))
        im.sys.addConstraintEqualLength(im.lines[im.lineOf[con.a.entity]],
                                        im.lines[im.lineOf[con.b.entity]], tag++);
      else if (im.circleOf.count(con.a.entity) && im.circleOf.count(con.b.entity))
        im.sys.addConstraintEqualRadius(im.circles[im.circleOf[con.a.entity]],
                                        im.circles[im.circleOf[con.b.entity]],
                                        tag++);
      break;
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
      }
      break;
    case ConstraintKind::PointOnLine:
      if (a && im.lineOf.count(con.b.entity))
        im.sys.addConstraintPointOnLine(*a, im.lines[im.lineOf[con.b.entity]],
                                        tag++);
      break;
    }
  }
  im.dragTag = tag;
}

void Solver::SetDragPoint(const PointRef& point, glm::vec2 targetUV) {
  Impl& im = *m_impl;
  GCS::Point* p = im.Resolve(point);
  if (!p)
    return;
  // Pin the dragged point to a fixed target with a temporary coincidence so it
  // leads and the rest follows the constraints.
  im.dragTarget = {targetUV.x, targetUV.y};
  GCS::Point target(&im.dragTarget[0], &im.dragTarget[1]);
  im.sys.addConstraintP2PCoincident(*p, target, im.dragTag++);
}

SolveResult Solver::Solve() {
  Impl& im = *m_impl;
  SolveResult result;

  // Every point coordinate + radius is an unknown.
  GCS::VEC_pD unknowns;
  for (auto& gp : im.gcsPoints) {
    unknowns.push_back(gp.x);
    unknowns.push_back(gp.y);
  }
  for (auto& r : im.radii)
    unknowns.push_back(&r);

  if (unknowns.empty()) {
    result.ok = true;
    result.dof = 0;
    return result;
  }

  im.sys.declareUnknowns(unknowns);
  im.sys.initSolution();
  int status = im.sys.solve(true, GCS::DogLeg);
  if (status == GCS::Success || status == GCS::Converged) {
    im.sys.applySolution();
    result.ok = true;
  }
  result.dof = im.sys.dofsNumber();
  im.sys.getConflicting(result.conflicting);
  im.sys.getRedundant(result.redundant);
  return result;
}

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
void Solver::SetDragPoint(const PointRef&, glm::vec2) {}
SolveResult Solver::Solve() {
  LOG_WARN("SketchSolver: built without PlaneGCS; constraints ignored");
  return {true, -1, {}, {}};
}
void Solver::WriteBack(std::vector<Entity>& e) const { e = m_impl->shapes; }
} // namespace sketch

#endif
