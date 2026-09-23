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
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

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
// (docs/architecture.md#source-layout).

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
    // TIGHT. The default bound is computed from a B-spline's CONTROL POINTS,
    // which lie outside the surface -- fine for a broad phase, wrong for
    // stock, because stock is the size of material a person buys and clamps.
    // A 200 mm revolved waveguide reported +-108.24 mm, so the blank came out
    // 16 mm oversize across and the facing and profile passes cut air to
    // reach it (docs/cam/README.md#invariants). CADKernel.h says which answer
    // is for which purpose; this is the "what a PERSON or a script wants"
    // case, and it is also what sets the work origin.
    b.shape.GetBoundingBox(bmin, bmax, /*tight=*/true);
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
    // part (docs/cam/setup.md#parts-and-placements). A second copy of this
    // transform is a bug (docs/cam/README.md#invariants).
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

// --- seats (docs/cam/setup.md#seats) --------------------------------

bool ParseStockEdge(const std::string& text, StockEdge& out) {
  static const std::pair<const char*, StockEdge> kEdges[] = {
      {"-x", StockEdge::MinusX},
      {"+x", StockEdge::PlusX},
      {"-y", StockEdge::MinusY},
      {"+y", StockEdge::PlusY}};
  for (const auto& [name, edge] : kEdges)
    if (text == name) {
      out = edge;
      return true;
    }
  return false;
}

std::string StockEdgeName(StockEdge edge) {
  switch (edge) {
  case StockEdge::MinusX:
    return "-x";
  case StockEdge::PlusX:
    return "+x";
  case StockEdge::MinusY:
    return "-y";
  case StockEdge::PlusY:
    return "+y";
  }
  return "-x";
}

namespace {

struct SeatFace {
  TopoDS_Face face;
  glm::dvec3 centroid{0.0};
};

// The face a seat names, in the fixture's own model, and whether it can be
// one. Refused rather than bent to fit: a fixture only turns about Z, so a
// face that does not face down would have to be TILTED onto the stock, and
// a clamp drawn tilted is a clamp nobody placed.
bool FindSeatFace(const std::vector<Body>& model, const ElementRef& ref,
                  SeatFace& out, std::string& why) {
  for (const Body& b : model) {
    if (b.createdBy != ref.featureId || !b.shape.IsValid())
      continue;
    const std::optional<int> index = b.elementMap.Resolve(ref.name);
    if (!index)
      continue;
    glm::vec3 c(0.0f), n(0.0f), u(0.0f);
    if (!b.shape.FacePlane(*index, c, n, u)) {
      why = "that face is not flat -- a fixture rests on a flat face";
      return false;
    }
    if (n.z > -0.999f) {
      why = "that face does not face down -- a fixture only turns about Z, "
            "so the face it rests on has to be one that faces down";
      return false;
    }
    int i = 0;
    for (TopExp_Explorer e(b.shape.GetShape(), TopAbs_FACE); e.More();
         e.Next(), i++)
      if (i == *index) {
        out.face = TopoDS::Face(e.Current());
        break;
      }
    out.centroid = glm::dvec3(c);
    if (out.face.IsNull()) {
      why = "the face it rests on could not be found on the fixture's part";
      return false;
    }
    return true;
  }
  why = "the face it rests on (" + ref.name +
        ") is no longer on the fixture's part";
  return false;
}

// Into the stock, square to the edge.
glm::dvec2 InwardOf(StockEdge edge) {
  switch (edge) {
  case StockEdge::MinusX:
    return {1.0, 0.0};
  case StockEdge::PlusX:
    return {-1.0, 0.0};
  case StockEdge::MinusY:
    return {0.0, 1.0};
  case StockEdge::PlusY:
    return {0.0, -1.0};
  }
  return {1.0, 0.0};
}

bool AlongX(StockEdge edge) {
  return edge == StockEdge::MinusY || edge == StockEdge::PlusY;
}

} // namespace

std::string SeatFaceRefusal(const std::vector<Body>& fixtureModel,
                            const ElementRef& face) {
  SeatFace sf;
  std::string why;
  return FindSeatFace(fixtureModel, face, sf, why) ? std::string() : why;
}

namespace {

// Which way the NOSE points, in the fixture's own XY: from the middle of the
// whole fixture to the face it rests on. A step clamp's pad is at one end and
// its studs at the other, so this is the direction it reaches; a face in the
// middle has no direction of its own and takes +X.
glm::dvec2 NoseOf(const std::vector<Body>& model, const SeatFace& sf) {
  glm::dvec3 lo(1e30), hi(-1e30);
  for (const Body& b : model) {
    if (!b.shape.IsValid())
      continue;
    glm::vec3 blo(0.0f), bhi(0.0f);
    b.shape.GetBoundingBox(blo, bhi, /*tight=*/true);
    lo = glm::min(lo, glm::dvec3(blo));
    hi = glm::max(hi, glm::dvec3(bhi));
  }
  const glm::dvec2 nose(sf.centroid.x - (lo.x + hi.x) * 0.5,
                        sf.centroid.y - (lo.y + hi.y) * 0.5);
  return glm::length(nose) < 1e-6 ? glm::dvec2(1.0, 0.0) : glm::normalize(nose);
}

// The face turned by `turn` (radians about Z, before any move): how far it
// reaches along `in`, and where its centroid goes. Shared by the solve and its
// inverse, so the two cannot disagree about what "over" measures.
bool TurnedFace(const SeatFace& sf, double turn, const glm::dvec2& in,
                double& reach, glm::dvec2& centroid) {
  gp_Trsf spin;
  spin.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), turn);
  Bnd_Box box;
  BRepBndLib::AddOptimal(sf.face.Moved(TopLoc_Location(spin)), box, false,
                         false);
  if (box.IsVoid())
    return false;
  double xmin = 0, ymin = 0, zmin = 0, xmax = 0, ymax = 0, zmax = 0;
  box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
  reach = in.x > 0.5 ? xmax : in.x < -0.5 ? -xmin : in.y > 0.5 ? ymax : -ymin;
  const double c = std::cos(turn), s = std::sin(turn);
  centroid = {c * sf.centroid.x - s * sf.centroid.y,
              s * sf.centroid.x + c * sf.centroid.y};
  return true;
}

// The edge's own coordinate along its inward direction (so larger is further
// in), and the low end of the coordinate running along it.
double EdgeIn(StockEdge edge, const glm::dvec3& lo, const glm::dvec3& hi) {
  switch (edge) {
  case StockEdge::MinusX:
    return lo.x;
  case StockEdge::PlusX:
    return -hi.x;
  case StockEdge::MinusY:
    return lo.y;
  case StockEdge::PlusY:
    return -hi.y;
  }
  return lo.x;
}

} // namespace

bool SolveFixtureSeat(const glm::dvec3& stockLo, const glm::dvec3& stockHi,
                      const std::vector<Body>& fixtureModel,
                      const FixtureSeat& seat, glm::dvec3& pos, double& rotZdeg,
                      std::string* why) {
  std::string reason;
  const auto refuse = [&](std::string w) {
    if (why != nullptr)
      *why = std::move(w);
    return false;
  };
  if (!(stockHi.x > stockLo.x && stockHi.y > stockLo.y))
    return refuse("there is no stock to rest it on");
  SeatFace sf;
  if (!FindSeatFace(fixtureModel, seat.face, sf, reason))
    return refuse(reason);
  const glm::dvec2 nose = NoseOf(fixtureModel, sf);
  const glm::dvec2 in = InwardOf(seat.edge);
  const double turn = std::atan2(in.y, in.x) - std::atan2(nose.y, nose.x) +
                      glm::radians(seat.spinDeg);
  double reach = 0.0;
  glm::dvec2 turned(0.0);
  if (!TurnedFace(sf, turn, in, reach, turned))
    return refuse("the face it rests on has no extent");
  const double posIn =
      EdgeIn(seat.edge, stockLo, stockHi) + seat.overMm - reach;
  const bool alongX = AlongX(seat.edge);
  const double low = alongX ? stockLo.x : stockLo.y;
  const double posAlong = low + seat.alongMm - (alongX ? turned.x : turned.y);
  const glm::dvec2 t = alongX ? glm::dvec2(1.0, 0.0) : glm::dvec2(0.0, 1.0);
  const glm::dvec2 xy = in * posIn + t * posAlong;
  pos = glm::dvec3(xy.x, xy.y, stockHi.z - sf.centroid.z);
  const double deg = std::remainder(glm::degrees(turn), 360.0);
  rotZdeg = std::abs(deg) < 1e-9 ? 0.0 : deg;
  return true;
}

bool SeatFromPose(const glm::dvec3& stockLo, const glm::dvec3& stockHi,
                  const std::vector<Body>& fixtureModel, const ElementRef& face,
                  const glm::dvec3& pos, double rotZdeg, FixtureSeat& seat,
                  std::string* why) {
  std::string reason;
  SeatFace sf;
  if (!FindSeatFace(fixtureModel, face, sf, reason)) {
    if (why != nullptr)
      *why = reason;
    return false;
  }
  // The edge it comes in over is the one its nose points INTO the stock
  // across; the spin is whatever is left of the pose after pointing that way.
  const glm::dvec2 nose = NoseOf(fixtureModel, sf);
  const double turn = glm::radians(rotZdeg);
  const glm::dvec2 bedNose(std::cos(turn) * nose.x - std::sin(turn) * nose.y,
                           std::sin(turn) * nose.x + std::cos(turn) * nose.y);
  StockEdge best = StockEdge::MinusX;
  double most = -2.0;
  for (const StockEdge e : {StockEdge::MinusX, StockEdge::PlusX,
                            StockEdge::MinusY, StockEdge::PlusY}) {
    const double d = glm::dot(bedNose, InwardOf(e));
    if (d > most + 1e-9) {
      most = d;
      best = e;
    }
  }
  const glm::dvec2 in = InwardOf(best);
  double reach = 0.0;
  glm::dvec2 turned(0.0);
  if (!TurnedFace(sf, turn, in, reach, turned)) {
    if (why != nullptr)
      *why = "the face it rests on has no extent";
    return false;
  }
  seat.face = face;
  seat.edge = best;
  const double base = std::atan2(in.y, in.x) - std::atan2(nose.y, nose.x);
  const double spin = std::remainder(rotZdeg - glm::degrees(base), 360.0);
  seat.spinDeg = std::abs(spin) < 1e-9 ? 0.0 : spin;
  seat.overMm =
      glm::dot(glm::dvec2(pos), in) + reach - EdgeIn(best, stockLo, stockHi);
  const bool alongX = AlongX(best);
  seat.alongMm =
      alongX ? pos.x + turned.x - stockLo.x : pos.y + turned.y - stockLo.y;
  return true;
}

std::array<std::vector<AxisFacePlane>, 3>
AxisFacePlanes(const std::vector<Body>& bodies) {
  std::array<std::vector<AxisFacePlane>, 3> out;
  for (size_t bi = 0; bi < bodies.size(); bi++) {
    if (!bodies[bi].shape.IsValid())
      continue;
    // One walk of the faces: FacePlane walks to its index every call, and an
    // imported vise has thousands.
    int index = 0;
    for (TopExp_Explorer e(bodies[bi].shape.GetShape(), TopAbs_FACE); e.More();
         e.Next(), index++) {
      const TopoDS_Face face = TopoDS::Face(e.Current());
      BRepAdaptor_Surface surf(face);
      if (surf.GetType() != GeomAbs_Plane)
        continue;
      const gp_Pln pln = surf.Plane();
      gp_Dir n = pln.Axis().Direction();
      if (face.Orientation() == TopAbs_REVERSED)
        n.Reverse();
      const double c[3] = {n.X(), n.Y(), n.Z()};
      const gp_Pnt o = pln.Location();
      const double at[3] = {o.X(), o.Y(), o.Z()};
      for (int a = 0; a < 3; a++)
        if (std::abs(c[a]) > 0.999)
          out[a].push_back({at[a], c[a] > 0.0 ? 1 : -1, bi, index});
    }
  }
  return out;
}

ElementRef FaceOnStockTop(const glm::dvec3& stockLo, const glm::dvec3& stockHi,
                          const std::vector<Body>& fixtureModel,
                          const glm::dvec3& pos, double rotZdeg) {
  Fixture placed;
  placed.pos = pos;
  placed.rotZdeg = rotZdeg;
  const glm::mat4 m(FixtureTransform(placed));
  constexpr double kOn = 1e-3; // a snapped face lands exactly; this is float
  ElementRef best;
  double most = 0.0;
  for (const Body& b : fixtureModel) {
    if (!b.shape.IsValid())
      continue;
    const CADShape at = b.shape.Transformed(m);
    int index = 0;
    for (TopExp_Explorer e(at.GetShape(), TopAbs_FACE); e.More();
         e.Next(), index++) {
      const std::string* name = b.elementMap.NameOf(index);
      if (name == nullptr)
        continue;
      const TopoDS_Face face = TopoDS::Face(e.Current());
      BRepAdaptor_Surface surf(face);
      if (surf.GetType() != GeomAbs_Plane)
        continue;
      gp_Dir n = surf.Plane().Axis().Direction();
      if (face.Orientation() == TopAbs_REVERSED)
        n.Reverse();
      if (n.Z() > -0.999 ||
          std::abs(surf.Plane().Location().Z() - stockHi.z) > kOn)
        continue;
      Bnd_Box box;
      BRepBndLib::Add(face, box);
      double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
      box.Get(x0, y0, z0, x1, y1, z1);
      const double ox = std::min(x1, stockHi.x) - std::max(x0, stockLo.x);
      const double oy = std::min(y1, stockHi.y) - std::max(y0, stockLo.y);
      if (ox <= 0.0 || oy <= 0.0 || ox * oy <= most)
        continue;
      most = ox * oy;
      best = ElementRef{b.createdBy, *name};
    }
  }
  return best;
}

} // namespace cam
