#include "core/cam/CamToolpath.h"

#include "core/cam/CamToolpathInternal.h"

#include "core/Measure.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

#include <glm/trigonometric.hpp>

// The setup's frames and bounds: model and stock extents, the work origin,
// the fixture frame, and turning a picked face up to the spindle.

// One of six files split from CamToolpath.cpp
// (docs/architecture-review-2026-09-06.md, stage 2).

namespace cam {

using namespace detail;

namespace {

// Where this body's part sits in the stock, or null when nobody has said.
const Placement* PlacementForBody(const Setup& setup, const Body& b) {
  if (b.jobPartId == 0)
    return nullptr;
  for (const Placement& p : setup.placements)
    if (p.jobPartId == b.jobPartId)
      return &p;
  return nullptr;
}

} // namespace

namespace detail {

// Nothing to do: the part is clamped as modelled and the table is square. The
// common case, and worth not building a matrix for — this is on the per-point
// path that draws every backplot.
bool SetupIsSquare(const Setup& setup) {
  return std::abs(setup.rotationA) < 1e-9 && FrameIsSquare(setup);
}

} // namespace detail

// The MODEL's own box, in whatever space `bodies` are in. The stock is built
// around it in both modes, and "does this wall go all the way through" is a
// question about the part rather than the material.
bool ModelBoundsFramed(const std::vector<Body>& bodies, glm::dvec3& min,
                       glm::dvec3& max) {
  bool any = false;
  glm::vec3 lo(0.0f), hi(0.0f);
  for (const Body& b : bodies) {
    if (!b.shape.IsValid())
      continue;
    glm::vec3 bmin(0.0f), bmax(0.0f);
    b.shape.GetBoundingBox(bmin, bmax);
    lo = any ? glm::min(lo, bmin) : bmin;
    hi = any ? glm::max(hi, bmax) : bmax;
    any = true;
  }
  if (!any)
    return false;
  min = glm::dvec3(lo);
  max = glm::dvec3(hi);
  return true;
}

bool StockBoundsFramed(const Setup& setup, const std::vector<Body>& bodies,
                       glm::dvec3& min, glm::dvec3& max) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!ModelBoundsFramed(bodies, lo, hi))
    return false;
  const Stock& s = setup.stock;
  if (s.mode == StockMode::Fixed) {
    // The part sits inside the sheet at `place`: 0 puts its low face on the
    // stock's low face, 1 its high face on the stock's high face. A sheet
    // SMALLER than the part is refused rather than quietly grown, because a
    // grown one is a job that looks fine and cannot be cut from the material
    // in the room (see StockRefusal).
    //
    // ...unless the parts have been LAID OUT. `place` and a per-part
    // placement are two ways to say where a part sits in the sheet, and they
    // fight: with the sheet derived from the parts, moving a part drags the
    // sheet along, so nothing could ever be off the edge and "the sheet you
    // actually own" (W3) was still a shrink-wrap in disguise. Once anything
    // is placed, the sheet is a real object at a known place -- anchored in
    // X and Y at the setup origin, which is the corner you touch off on --
    // and the parts are arranged ON it. Z keeps the `place` derivation:
    // thickness is still a fact about the board, not about the layout.
    const bool laidOut = !setup.placements.empty();
    for (int a = 0; a < 3; a++) {
      if (laidOut && a < 2) {
        min[a] = 0.0;
        max[a] = s.size[a];
        continue;
      }
      const double extent = hi[a] - lo[a];
      if (s.size[a] < extent - kStockFit)
        return false;
      // Never negative: an 18 mm board holding an 18 mm part is a fit, and
      // OCC's gap must not push the stock inside the model.
      const double slack = std::max(0.0, s.size[a] - extent);
      min[a] = lo[a] - slack * std::clamp(s.place[a], 0.0, 1.0);
      max[a] = min[a] + std::max(s.size[a], extent);
    }
    return true;
  }
  min = lo - glm::dvec3(s.margin.x, s.margin.y, s.bottomMargin);
  max = hi + glm::dvec3(s.margin.x, s.margin.y, s.margin.z);
  return true;
}

bool StockBounds(const Setup& setup, const std::vector<Body>& bodies,
                 glm::dvec3& min, glm::dvec3& max) {
  return StockBoundsFramed(setup, BodiesInSetupFrame(setup, bodies), min, max);
}

std::string StockRefusal(const Setup& setup, const std::vector<Body>& bodies) {
  const std::vector<Body> framed = BodiesInSetupFrame(setup, bodies);
  glm::dvec3 lo(0.0), hi(0.0);
  if (!ModelBoundsFramed(framed, lo, hi))
    return "there is no solid in this part yet, so there is no stock";
  if (setup.stock.mode != StockMode::Fixed)
    return "";
  static const char* kAxis[3] = {"X", "Y", "Z"};
  for (int a = 0; a < 3; a++) {
    const double extent = hi[a] - lo[a];
    if (setup.stock.size[a] < extent - kStockFit) {
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "the stock is %.4g mm in %s and the part needs %.4g mm — "
                    "give it a bigger workpiece, or lay the part down another "
                    "way",
                    setup.stock.size[a], kAxis[a], extent);
      return buf;
    }
  }
  return "";
}

bool RecomputeOrigin(Setup& setup, const std::vector<Body>& bodies) {
  const std::vector<Body> framed = BodiesInSetupFrame(setup, bodies);
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, framed, lo, hi))
    return false;
  glm::dvec3 o(0.0, 0.0, hi.z); // touch off on the top of the material
  switch (setup.originAt) {
  case OriginAt::StockMin:
    o.x = lo.x;
    o.y = lo.y;
    break;
  case OriginAt::StockCentre:
    o.x = (lo.x + hi.x) * 0.5;
    o.y = (lo.y + hi.y) * 0.5;
    break;
  case OriginAt::StockMax:
    // The FAR corner. On a sheet slid through the bed the near corner is the
    // end hanging in the air, which is a datum nobody can touch off on; the
    // far one is over the table where the cutter can reach it.
    o.x = hi.x;
    o.y = hi.y;
    break;
  case OriginAt::ModelMin: {
    glm::dvec3 mlo(0.0), mhi(0.0);
    if (!ModelBoundsFramed(framed, mlo, mhi))
      return false;
    o.x = mlo.x;
    o.y = mlo.y;
    break;
  }
  }
  // The picked face wins on Z, because "Pick top face — that sets Z zero for
  // the job" is a promise the file now has to keep.
  if (setup.topFace.IsSet()) {
    const FaceRegion region = PickedFaceRegion(framed, setup.topFace.ref);
    if (region.ok)
      o.z = region.z;
  }
  setup.origin = o;
  return true;
}

bool EmitsRotary(const Setup& setup) {
  return setup.machine.hasRotaryAxis && std::abs(setup.rotationA) > 1e-9;
}

glm::dquat FlipFrame(const glm::dquat& frame) {
  // Half a turn about X, composed on the right so it acts in SETUP space —
  // about the axis the part is lying on, not about the model's X.
  return frame * glm::dquat(0.0, 1.0, 0.0, 0.0);
}

bool FrameIsSquare(const Setup& setup) {
  // A unit quaternion is a pure rotation, so |w| = 1 exactly characterises
  // "no rotation" — cheaper and steadier than comparing four components or
  // reconstructing an angle.
  return std::abs(std::abs(setup.frame.w) - 1.0) < 1e-9;
}

glm::dquat FixtureFrame(const Setup& setup, uint64_t jobPartId) {
  if (jobPartId != 0)
    for (const Placement& p : setup.placements)
      if (p.jobPartId == jobPartId && p.frame)
        return *p.frame;
  return setup.frame;
}

bool FrameIsSquare(const Setup& setup, uint64_t jobPartId) {
  return std::abs(std::abs(FixtureFrame(setup, jobPartId).w) - 1.0) < 1e-9;
}

glm::dmat3 SetupToModel(const Setup& setup) { return SetupToModel(setup, 0); }

glm::dmat3 SetupToModel(const Setup& setup, uint64_t jobPartId) {
  // Fixture first, then the index: the part is clamped to the table, and the
  // table then turns the whole fixture with it. Composing them the other way
  // would index the part about an axis it is not sitting on.
  glm::dmat3 m = glm::mat3_cast(FixtureFrame(setup, jobPartId));
  if (std::abs(setup.rotationA) > 1e-9) {
    const double a = glm::radians(setup.rotationA);
    const double c = std::cos(a), s = std::sin(a);
    // Columns are the images of X, Y, Z: R_x(A).
    m *= glm::dmat3(glm::dvec3(1.0, 0.0, 0.0), glm::dvec3(0.0, c, s),
                    glm::dvec3(0.0, -s, c));
  }
  return m;
}

bool FaceNormalOf(const std::vector<Body>& bodies, const ElementRef& ref,
                  glm::vec3& normal, uint64_t jobPartId) {
  glm::vec3 centre(0.0f), uAxis(0.0f);
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId)
      continue;
    // Another part of the job, sharing the feature id: not this pick. A body
    // with no job part (a plain part document) has nothing to disambiguate.
    if (jobPartId != 0 && body.jobPartId != 0 && body.jobPartId != jobPartId)
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    // Keep looking rather than answering for the first body that shares the
    // id: a part is often several bodies, and only one of them owns the name.
    if (idx && body.shape.FacePlane(*idx, centre, normal, uAxis))
      return true;
  }
  return false;
}

glm::dquat SolveFaceUp(const glm::dvec3& normal, const glm::dquat& current) {
  const double len = glm::length(normal);
  if (len < 1e-9)
    return current;
  const glm::dvec3 n = normal / len;
  // The frame maps the spindle (+Z) to the face normal, so undoing it lays
  // the face flat under the spindle. Shortest arc from +Z to n.
  const glm::dvec3 z(0.0, 0.0, 1.0);
  const double d = glm::dot(z, n);
  glm::dquat q0;
  if (d < -1.0 + 1e-12) {
    // Antiparallel: every axis in the XY plane is a shortest arc, and X is as
    // good as any. Without this branch the cross product is zero and the
    // quaternion comes back as garbage rather than as a half turn.
    q0 = glm::dquat(0.0, 1.0, 0.0, 0.0);
  } else {
    const glm::dvec3 axis = glm::cross(z, n);
    q0 = glm::normalize(glm::dquat(1.0 + d, axis.x, axis.y, axis.z));
  }
  // q0 is one answer; q0 * R_z(t) is every other. Pick the t nearest the
  // frame in hand: dot(q0 * R_z(t), current) is maximal at this half-angle.
  const glm::dquat c = glm::inverse(q0) * current;
  const double half = std::atan2(c.z, c.w);
  return q0 * glm::dquat(std::cos(half), 0.0, 0.0, std::sin(half));
}

// Sections live here rather than with the toolpath geometry because they need
// OpenCASCADE, not Clipper2, and this file is built whether or not CAM is
// enabled: a job document's setup has a section and depths whether or not the
// build can turn them into moves (CMakeLists.txt).
std::vector<Loop> SectionLoops(const std::vector<Body>& bodies, double z,
                               double deflection) {
  std::vector<Loop> loops;
  // The section machinery the Section TOOL already uses (I3): one plane, the
  // filled cut face per body. Reusing it means a toolpath cannot disagree
  // with what the section view shows.
  const std::vector<CADShape> caps =
      Measure::SectionCaps(bodies, glm::vec3(0.0f, 0.0f, static_cast<float>(z)),
                           glm::vec3(0.0f, 0.0f, 1.0f));
  for (const CADShape& cap : caps) {
    if (!cap.IsValid())
      continue;
    for (TopExp_Explorer fx(cap.GetShape(), TopAbs_FACE); fx.More();
         fx.Next()) {
      const TopoDS_Face face = TopoDS::Face(fx.Current());
      const TopoDS_Wire outer = BRepTools::OuterWire(face);
      for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
        const TopoDS_Wire wire = TopoDS::Wire(wx.Current());
        Loop loop;
        loop.pts = WireToLoop(wire, deflection);
        if (loop.pts.size() < 3)
          continue;
        loop.hole = !wire.IsSame(outer);
        loops.push_back(std::move(loop));
      }
    }
  }
  return loops;
}

// Same reason as SectionLoops above: placing bodies in the setup's frame is
// OCC transforms, and the job tab draws them in a build with no toolpath
// generator at all.
std::vector<Body> BodiesInSetupFrame(const Setup& setup,
                                     const std::vector<Body>& in) {
  if (SetupIsSquare(setup) && setup.placements.empty())
    return in;

  std::vector<Body> out;
  out.reserve(in.size());
  for (const Body& b : in) {
    // Where the part sits in the stock composes HERE and nowhere else, after
    // the fixture frame and in setup space -- which is what makes "where on
    // the sheet" an in-plane statement rather than a fourth way to tilt a
    // part (docs/cam/job-document.md). setup-frame.md learned what a second
    // copy of this transform costs.
    const Placement* place = PlacementForBody(setup, b);
    if (place != nullptr && !place->enabled)
      continue; // on the sheet, not cut this run
    // Undo the fixture frame THIS part is clamped in -- which may not be the
    // setup's (v76): a sheet can hold a baffle laid on its face beside a rail
    // laid on its edge. The part turns under a spindle that always points
    // down, which is what both a clamp and an indexed trunnion amount to, and
    // a rotation's inverse is its transpose.
    const glm::dmat3 toSetup = glm::transpose(SetupToModel(setup, b.jobPartId));
    glm::mat4 m(1.0f);
    for (int col = 0; col < 3; col++)
      for (int row = 0; row < 3; row++)
        m[col][row] = static_cast<float>(toSetup[col][row]);
    if (place != nullptr) {
      const float c =
          static_cast<float>(std::cos(glm::radians(place->rotZdeg)));
      const float sn =
          static_cast<float>(std::sin(glm::radians(place->rotZdeg)));
      glm::mat4 t(1.0f);
      t[0][0] = c;
      t[0][1] = sn;
      t[1][0] = -sn;
      t[1][1] = c;
      t[3][0] = static_cast<float>(place->pos.x);
      t[3][1] = static_cast<float>(place->pos.y);
      m = t * m;
    }
    Body copy = b; // keeps createdBy and the element map: names must survive
    if (b.shape.IsValid())
      copy.shape = b.shape.Transformed(m);
    out.push_back(std::move(copy));
  }
  return out;
}

} // namespace cam
