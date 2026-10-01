#include "core/cam/CamMount.h"
#include "core/cam/CamToolpath.h"

#include "core/cam/CamNames.h"
#include "core/cam/CamToolpathInternal.h"

#include "core/Measure.h"
#include "core/cam/CamAdaptive.h"
#include "core/cam/CamDropCutter.h"
#include "core/cam/CamEngrave.h"
#include "core/cam/CamRotary.h"
#include "core/cam/CamSurface.h"

#include <clipper2/clipper.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
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
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

#include <glm/trigonometric.hpp>

// The strategies themselves -- profile, pocket, adaptive, face, drill and
// edge round -- and the one switch that picks between them. The finishing
// passes (planar and rotary) are in CamSurfaceStrategies.cpp.

// One of six files split from CamToolpath.cpp
// (docs/architecture.md#source-layout).

namespace cam {

using namespace detail;

namespace {

// meant twelve of those, and it reads exactly as it sounds — "does one lap,
// lifts and goes back a bit, then down and to the next circle". The link is
// one step-over through the band the next ring is about to cut anyway, so
// there is nothing to lift over. The distance check is what keeps it honest:
// two DISJOINT regions at the same inset are far apart, and linking those at
// depth would drag the cutter across whatever lies between them.
void EmitRing(Toolpath& tp, const Setup& setup, const Tool& tool,
              const std::vector<glm::dvec2>& pts, double z, double stockTop,
              double step, double entryZ);

// The XY contour(s) a profile follows, and the region a pocket clears, when
// How far above a pocket floor to look for material standing in one of its
// inner wires. Small enough that a boss of any real height is found, large
// enough to clear the tolerance a section carries — and the section at the
// floor ITSELF is ambiguous, because the floor's own face is there.
constexpr double kIslandProbe = 0.05;

// the operation was made by clicking a face: the face's own wires, the same
// at every depth. Empty when the operation has no target, which is the
// caller's signal to fall back to sectioning the model.
// --- tolerance bands (docs/cam/operations.md#tolerance-bands) --------------
//
// A toleranced round wall is cut at the middle of its band: the circle the
// material ends on is moved radially by a quarter of the band's deviations
// summed (half their mean, on the radius) before the tool is offset from it,
// so every strategy that follows that circle -- a bore's rings, a profile's
// contour, a pocket's outline or island -- aims there with no code of its
// own. The walls come from the bands the job stamped on the bodies
// (Body::bands), resolved once per generate in the setup's frame.
struct FitCircle {
  Toolpath::Fit decl;
  double radius = 0.0; // the modelled wall's radius
  double shift = 0.0;  // how far it moves outward to the band's middle
  uint64_t jobPartId = 0;
  ElementRef face;
  bool used = false;
};
thread_local std::vector<FitCircle>* tFits = nullptr;

std::vector<FitCircle> ResolveFits(const std::vector<Body>& framed) {
  std::vector<FitCircle> out;
  for (const Body& body : framed)
    for (const FaceBand& band : body.bands) {
      if (band.reference.IsSet())
        continue; // a distance: ResolvePlanes
      const std::optional<int> idx = body.elementMap.Resolve(band.face.name);
      if (!idx || !body.shape.IsValid())
        continue;
      int i = 0;
      for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
           ex.Next(), i++) {
        if (i != *idx)
          continue;
        try {
          const TopoDS_Face face = TopoDS::Face(ex.Current());
          const BRepAdaptor_Surface s(face);
          if (s.GetType() != GeomAbs_Cylinder)
            break;
          const gp_Cylinder cyl = s.Cylinder();
          const gp_Dir dir = cyl.Axis().Direction();
          if (std::abs(dir.Z()) < 0.999)
            break; // a wall this setup does not see standing up
          double v0 = s.FirstVParameter(), v1 = s.LastVParameter();
          const double u = 0.5 * (s.FirstUParameter() + s.LastUParameter());
          const gp_Pnt loc = cyl.Location();
          // Which side the metal is on: the face's outward normal points at
          // the axis in a bore, away from it round a boss.
          BRepLProp_SLProps props(s, u, 0.5 * (v0 + v1), 1, 1e-9);
          if (!props.IsNormalDefined())
            break;
          gp_Dir n = props.Normal();
          if (face.Orientation() == TopAbs_REVERSED)
            n.Reverse();
          const gp_Pnt p = props.Value();
          const gp_Vec radial(
              gp_Pnt(loc.X() + (p.Z() - loc.Z()) / dir.Z() * dir.X(),
                     loc.Y() + (p.Z() - loc.Z()) / dir.Z() * dir.Y(), p.Z()),
              p);
          FitCircle c;
          c.radius = cyl.Radius();
          c.shift = (band.lower + band.upper) * 0.25;
          c.jobPartId = body.jobPartId;
          c.face = band.face;
          Toolpath::Fit& f = c.decl;
          f.internal = radial.Dot(gp_Vec(n)) < 0.0;
          const double zA = loc.Z() + v0 * dir.Z(), zB = loc.Z() + v1 * dir.Z();
          f.zLo = std::min(zA, zB);
          f.zHi = std::max(zA, zB);
          const double zMid = 0.5 * (f.zLo + f.zHi);
          f.centre = {loc.X() + (zMid - loc.Z()) / dir.Z() * dir.X(),
                      loc.Y() + (zMid - loc.Z()) / dir.Z() * dir.Y()};
          f.nominal = 2.0 * c.radius;
          f.lower = band.lower;
          f.upper = band.upper;
          f.aim = f.nominal + 2.0 * c.shift;
          f.label = band.label;
          out.push_back(c);
        } catch (const Standard_Failure&) {
        }
        break;
      }
    }
  return out;
}

// A distance band between two faces that stand level in this setup: a
// floor or a shoulder (a face looking up) whose height from the other face
// is toleranced. Which of the two a cut ends on is the operation's: the one
// at its floor.
struct FitPlane {
  struct Side {
    double z = 0.0;
    bool up = false;             // its outward normal points up: a floor
    glm::dvec2 lo{0.0}, hi{0.0}; // its extent, setup XY
  };
  Side a, b;
  double lower = 0.0, upper = 0.0;
  std::string label;
};

std::optional<FitPlane::Side> LevelFace(const Body& body,
                                        const ElementRef& ref) {
  const std::optional<int> idx = body.elementMap.Resolve(ref.name);
  if (!idx || !body.shape.IsValid())
    return std::nullopt;
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    if (i != *idx)
      continue;
    try {
      const TopoDS_Face face = TopoDS::Face(ex.Current());
      const BRepAdaptor_Surface s(face);
      if (s.GetType() != GeomAbs_Plane)
        return std::nullopt;
      gp_Dir n = s.Plane().Axis().Direction();
      if (face.Orientation() == TopAbs_REVERSED)
        n.Reverse();
      if (std::abs(n.Z()) < 0.999)
        return std::nullopt; // not level in this setup
      Bnd_Box box;
      BRepBndLib::Add(face, box);
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      FitPlane::Side side;
      side.z = s.Plane().Location().Z();
      side.up = n.Z() > 0.0;
      side.lo = {x0, y0};
      side.hi = {x1, y1};
      return side;
    } catch (const Standard_Failure&) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::vector<FitPlane> ResolvePlanes(const std::vector<Body>& framed) {
  std::vector<FitPlane> out;
  for (const Body& body : framed)
    for (const FaceBand& band : body.bands) {
      if (!band.reference.IsSet())
        continue;
      const auto a = LevelFace(body, band.face);
      const auto b = LevelFace(body, band.reference);
      if (!a || !b || std::abs(a->z - b->z) < 1e-6)
        continue;
      FitPlane p;
      p.a = *a;
      p.b = *b;
      p.lower = band.lower;
      p.upper = band.upper;
      p.label = band.label;
      out.push_back(std::move(p));
    }
  return out;
}

// Holds the walls and the floors for one generate on this thread
// (GenerateFramed), and hands the ones the path aimed at back as
// declarations.
class FitScope {
public:
  explicit FitScope(const std::vector<Body>& framed)
      : m_fits(ResolveFits(framed)), m_planes(ResolvePlanes(framed)),
        m_outer(tFits) {
    tFits = &m_fits;
  }
  ~FitScope() { tFits = m_outer; }
  FitScope(const FitScope&) = delete;
  FitScope& operator=(const FitScope&) = delete;
  // A floor or a shoulder this operation ends on whose distance from its
  // reference is toleranced: the operation, its bottom moved to the band's
  // middle, and the floor declared. Only the cuts that end on a level face
  // -- a profile, a pocket, an adaptive clear, a facing pass -- and only
  // the first floor that matches.
  Operation AimAtFloor(const Operation& op) {
    if (op.type != OpType::Profile && op.type != OpType::Pocket &&
        op.type != OpType::Adaptive && op.type != OpType::Face)
      return op;
    for (const FitPlane& p : m_planes)
      for (const bool aCut : {true, false}) {
        const FitPlane::Side& cut = aCut ? p.a : p.b;
        const FitPlane::Side& ref = aCut ? p.b : p.a;
        // Where its depth passes end (DepthPasses): the floor it leaves. A
        // rough before a finish ends there too -- lowered by the finish
        // stock it then leaves -- and an operation told to leave stock is
        // not the one that cuts the floor to size.
        if (!cut.up || std::abs(op.bottomZ + op.stockToLeave - cut.z) > 1e-3)
          continue;
        Toolpath::Fit f;
        f.depth = true;
        f.faceZ = cut.z;
        f.refZ = ref.z;
        f.lo = cut.lo;
        f.hi = cut.hi;
        f.centre = 0.5 * (cut.lo + cut.hi);
        f.nominal = std::abs(cut.z - ref.z);
        f.lower = p.lower;
        f.upper = p.upper;
        f.aim = f.nominal + 0.5 * (p.lower + p.upper);
        // Removing metal from a floor below its reference lengthens the
        // distance, as boring a hole grows it.
        f.internal = cut.z < ref.z;
        f.zLo = f.zHi = cut.z;
        f.label = p.label;
        Operation aimed = op;
        aimed.bottomZ += f.ZAt(f.aim) - cut.z;
        m_floors.push_back(f);
        return aimed;
      }
    return op;
  }
  void Declare(uint64_t op, Toolpath& tp) const {
    for (const FitCircle& c : m_fits)
      if (c.used) {
        Toolpath::Fit f = c.decl;
        f.op = op;
        tp.fits.push_back(f);
      }
    for (Toolpath::Fit f : m_floors) {
      f.op = op;
      tp.fits.push_back(f);
    }
  }

private:
  std::vector<FitCircle> m_fits;
  std::vector<FitPlane> m_planes;
  std::vector<Toolpath::Fit> m_floors;
  std::vector<FitCircle>* m_outer;
};

// A contour that IS a toleranced wall's circle at height `z` -- every vertex
// on it, round its centre -- moved to the band's middle.
void AimAtBands(Clipper2Lib::Paths64& paths, double z) {
  if (tFits == nullptr || tFits->empty())
    return;
  for (Clipper2Lib::Path64& path : paths) {
    if (path.size() < 6)
      continue;
    for (FitCircle& c : *tFits) {
      const Toolpath::Fit& f = c.decl;
      if (z < f.zLo - 0.05 || z > f.zHi + 0.05 || std::abs(c.shift) < 1e-9)
        continue;
      bool on = true;
      for (const Clipper2Lib::Point64& p : path) {
        const glm::dvec2 q(static_cast<double>(p.x) / kScale,
                           static_cast<double>(p.y) / kScale);
        if (std::abs(glm::distance(q, f.centre) - c.radius) > 0.01) {
          on = false;
          break;
        }
      }
      if (!on)
        continue;
      const double k = (c.radius + c.shift) / c.radius;
      for (Clipper2Lib::Point64& p : path) {
        const glm::dvec2 q(static_cast<double>(p.x) / kScale,
                           static_cast<double>(p.y) / kScale);
        const glm::dvec2 moved = f.centre + (q - f.centre) * k;
        p.x = static_cast<int64_t>(std::llround(moved.x * kScale));
        p.y = static_cast<int64_t>(std::llround(moved.y * kScale));
      }
      c.used = true;
      break;
    }
  }
}
// The circle aimed at for a bore the drill resolved by its face, or the
// shift 0 when it carries no band.
double AimAtBore(const Body& body, int idx) {
  if (tFits == nullptr)
    return 0.0;
  for (FitCircle& c : *tFits) {
    if (c.jobPartId != body.jobPartId || c.face.featureId != body.createdBy ||
        body.elementMap.Resolve(c.face.name) != std::optional<int>(idx))
      continue;
    c.used = true;
    return c.shift;
  }
  return 0.0;
}

// The inner wires of a level face that are genuine OPENINGS: an inner wire
// with nothing standing in it. It is the same question `useOutline` asks
// about a pocket floor (below), asked for the opposite purpose -- there to
// know what may be driven over, here to know what may be cut out -- so it is
// one test and not two. A BOSS has an inner wire too, and cutting round the
// inside of one removes the boss.

std::vector<Loop> FaceOpenings(const std::vector<Body>& bodies,
                               const FaceRegion& region) {
  std::vector<Loop> holes;
  for (const Loop& l : region.loops)
    if (l.hole)
      holes.push_back(l);
  if (holes.empty())
    return holes;
  Clipper2Lib::Paths64 section;
  for (const Loop& l : SectionLoops(bodies, region.z + kIslandProbe))
    section.push_back(ToClipper(l.pts));
  const Clipper2Lib::Paths64 standing =
      Clipper2Lib::Union(section, Clipper2Lib::FillRule::EvenOdd);
  std::vector<Loop> out;
  for (const Loop& l : holes) {
    const bool material =
        !Clipper2Lib::Intersect(Clipper2Lib::Paths64{ToClipper(l.pts)},
                                standing, Clipper2Lib::FillRule::NonZero)
             .empty();
    if (!material)
      out.push_back(l);
  }
  return out;
}

// Clearing ROUND a part (docs/cam/operations.md#clear-round): a clearing
// operation whose target is the part's own outside wall, not a floor. A
// vise-held part cut from a blank has its waste round it, and with no floor
// to pick nothing but a profile could take it -- which leaves the frame
// standing for the operator to saw off.
// How far past the stock's edge a clear-round region reaches, in cutters:
// the radius, so the cutter's centre can stand on the edge, and a tenth more.
constexpr double kRoundGrow = 0.55;

bool ClearsRound(const Operation& op, const std::vector<Body>& bodies) {
  if (op.targets.empty())
    return false;
  const Target& t = op.targets.front();
  if (!IsWallFace(bodies, t.ref, t.jobPartId))
    return false;
  const WallContour wall = PickedWallContour(bodies, t.ref, 0.02, t.jobPartId);
  return wall.ok && !wall.opening;
}

// The region a clear-round pass clears at each of `passes`: the stock,
// grown by the cutter's radius and a tenth more so its edges are cut clean
// (they are open: nothing stands beyond them), less everything of the part
// at or above the pass -- the projection from the operation's top, with its
// openings filled, so an overhang above a pass is not undercut and the
// cutter never goes into the part's own pockets.
std::vector<Clipper2Lib::Paths64>
ClearRoundRegions(const Setup& setup, const Operation& op, const Tool& tool,
                  const std::vector<Body>& bodies,
                  const std::vector<double>& passes) {
  std::vector<Clipper2Lib::Paths64> out(passes.size());
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, bodies, lo, hi))
    return out;
  const double g = kRoundGrow * tool.diameter;
  const Clipper2Lib::Path64 stock =
      ToClipper(std::vector<glm::dvec2>{{lo.x - g, lo.y - g},
                                        {hi.x + g, lo.y - g},
                                        {hi.x + g, hi.y + g},
                                        {lo.x - g, hi.y + g}});
  const std::vector<std::vector<Loop>> projected =
      ProjectedOutlines(bodies, op.topZ, passes);
  for (size_t i = 0; i < passes.size() && i < projected.size(); i++) {
    Clipper2Lib::Paths64 part;
    for (const Loop& l : projected[i])
      if (!l.hole)
        part.push_back(ToClipper(l.pts));
    out[i] = Clipper2Lib::Difference(
        Clipper2Lib::Paths64{stock},
        Clipper2Lib::Union(part, Clipper2Lib::FillRule::NonZero),
        Clipper2Lib::FillRule::NonZero);
  }
  return out;
}

// The air a clear-round region reaches into: its grown edge less the stock
// and a quarter cutter round it. That quarter is cut as if it were stock:
// told the stock's own edge was clear, the pass left a 0.6 mm sliver along it.
Clipper2Lib::Paths64 ClearRoundAir(const Setup& setup, const Tool& tool,
                                   const std::vector<Body>& bodies) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, bodies, lo, hi))
    return {};
  const auto rect = [&](double g) {
    return ToClipper(std::vector<glm::dvec2>{{lo.x - g, lo.y - g},
                                             {hi.x + g, lo.y - g},
                                             {hi.x + g, hi.y + g},
                                             {lo.x - g, hi.y + g}});
  };
  return Clipper2Lib::Difference(
      Clipper2Lib::Paths64{rect(kRoundGrow * tool.diameter)},
      Clipper2Lib::Paths64{rect(0.25 * tool.diameter)},
      Clipper2Lib::FillRule::NonZero);
}

// `openEdges` false leaves the floor as it is drawn, for measuring it: see
// FloorWidthRefusal below.
Clipper2Lib::Paths64 TargetPaths(const Operation& op,
                                 const std::vector<Body>& bodies, bool holesToo,
                                 double toolRadius, std::string& error,
                                 bool openEdges = true) {
  if (op.targets.empty())
    return {};
  // A wall names one contour at every depth — the case an opening that goes
  // all the way through has, because it has no floor to name.
  if (IsWallFace(bodies, op.targets.front().ref,
                 op.targets.front().jobPartId)) {
    const WallContour wall = PickedWallContour(
        bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
    if (!wall.ok) {
      error = wall.why;
      return {};
    }
    return Clipper2Lib::Paths64{ToClipper(wall.loop.pts)};
  }
  const FaceRegion region = PickedFaceRegion(
      bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
  if (!region.ok) {
    error = region.why;
    return {};
  }
  Clipper2Lib::Paths64 outer, islands;
  for (const Loop& l : region.loops)
    (l.hole ? islands : outer).push_back(ToClipper(l.pts));
  // An OPEN edge is not a wall. Where the floor's edge has nothing standing
  // just above the floor beside it -- a step down, the part's outside, the
  // end of a dome -- the cutter may run over it, so the region clearing
  // reads grows by the tool's radius across it; a wall stands above the
  // floor and trims the growth back. Treated as walled, the queen's crown
  // notch -- 3.4 mm wide, its floor 2.3 mm along the axis before the dome
  // falls away -- was refused for a 3 mm cutter as "2.3 mm across".
  if (openEdges && holesToo && toolRadius > 0.0) {
    Clipper2Lib::Paths64 section;
    for (const Loop& l : SectionLoops(bodies, region.z + kIslandProbe))
      section.push_back(ToClipper(l.pts));
    const Clipper2Lib::Paths64 standing = Clipper2Lib::InflatePaths(
        Clipper2Lib::Union(section, Clipper2Lib::FillRule::EvenOdd),
        0.02 * kScale, Clipper2Lib::JoinType::Round,
        Clipper2Lib::EndType::Polygon);
    const Clipper2Lib::Paths64 reach = Clipper2Lib::InflatePaths(
        outer, toolRadius * kScale, Clipper2Lib::JoinType::Round,
        Clipper2Lib::EndType::Polygon);
    const Clipper2Lib::Paths64 open = Clipper2Lib::Difference(
        reach, standing, Clipper2Lib::FillRule::NonZero);
    if (!open.empty())
      outer = Clipper2Lib::Union(outer, open, Clipper2Lib::FillRule::NonZero);
  }
  if (!holesToo || islands.empty())
    return outer;
  // `useOutline` passes over the OPENINGS in a floor. An inner wire is only
  // an opening if nothing stands in it: a hole has no material at or above
  // the floor, and the cutter bridges it harmlessly — a hole narrower than
  // the cutter is the case the option exists for. A BOSS has the same kind of
  // wire and the opposite meaning, and driving over one is a full-width cut
  // at full depth into standing material. So the option drops openings and
  // keeps islands, whatever it is set to: no flag may point the cutter at
  // material nobody asked it to remove.
  if (op.useOutline) {
    // The material standing just above the floor. EVEN-ODD over every loop of
    // the section, because the answer is a question of NESTING: inside the
    // part is filled, inside the pocket is not, inside a boss standing in
    // that pocket is again. Taking only the outward-wound loops instead reads
    // the part's own outline as "material everywhere", which keeps every hole
    // as an island and makes the option do nothing at all.
    Clipper2Lib::Paths64 section;
    for (const Loop& l : SectionLoops(bodies, region.z + kIslandProbe))
      section.push_back(ToClipper(l.pts));
    const Clipper2Lib::Paths64 standing =
        Clipper2Lib::Union(section, Clipper2Lib::FillRule::EvenOdd);
    Clipper2Lib::Paths64 keep;
    for (const Clipper2Lib::Path64& island : islands) {
      const bool material =
          !Clipper2Lib::Intersect(Clipper2Lib::Paths64{island}, standing,
                                  Clipper2Lib::FillRule::NonZero)
               .empty();
      // Something stands in it: an island, always, whatever the option says.
      if (material) {
        keep.push_back(island);
        continue;
      }
      // Nothing stands in it — but SIZE decides whether crossing is a favour
      // or a waste. An opening the cutter cannot enter is not an obstacle it
      // can go round: the detour only leaves a sliver. An opening it CAN
      // enter is a hole the job removes anyway, and clearing across it is
      // time spent cutting air — the whole middle of a flange recess, when
      // the driver cutout takes that middle out regardless.
      const Clipper2Lib::Paths64 room = Clipper2Lib::InflatePaths(
          Clipper2Lib::Paths64{island}, -toolRadius * kScale,
          Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon);
      if (!room.empty())
        keep.push_back(island); // the tool fits inside: go around it
    }
    islands = std::move(keep);
    if (islands.empty())
      return outer;
  }
  return Clipper2Lib::Difference(outer, islands,
                                 Clipper2Lib::FillRule::NonZero);
}

// Why a clearing region the cutter could not enter refused it, measured on
// the FLOOR as drawn -- not on `region`, which for a picked floor has grown
// by the tool's radius across its open edges. That growth is air beyond the
// part, and its width is set by the cutter asking, so a slot 0.99 mm wide
// read as "1.57 mm across" for a 3 mm cutter: the chess bishop's first mitre,
// whose sketch drew 3.2 mm ends 18 degrees off its sides, not square. Growth
// only ever adds, so a floor the cutter fits is a region it fits, and the
// measurement cannot blame a floor that was not the reason.
std::string FloorWidthRefusal(const Operation& op,
                              const std::vector<Body>& bodies,
                              const Clipper2Lib::Paths64& region, bool picked,
                              const Tool& tool) {
  std::string ignored;
  const Clipper2Lib::Paths64 floor =
      picked ? TargetPaths(op, bodies, /*holesToo=*/true, tool.diameter * 0.5,
                           ignored, /*openEdges=*/false)
             : Clipper2Lib::Paths64{};
  return TooTightForTool(floor.empty() ? region : floor, tool, op.stockToLeave,
                         "pocket");
}

// Profile: offset the section outward (or inward) by the tool radius and
// follow it, once per depth pass.
// Defined with the other loop surgery below (R4); the profile and the pocket
// both call it.
std::vector<glm::dvec2> AddCornerRelief(const std::vector<glm::dvec2>& pts,
                                        bool materialInside, double length,
                                        CornerRelief style,
                                        std::vector<glm::dvec2>* corners);

// ProjectedOutlines, from the context's cache, from a worker later, or right
// here -- the same three-way surfacing has
// (docs/cam/README.md#background-work). Empty when deferred, and `tp` is then
// pending: a profile has nothing to follow until its outlines land.
std::optional<std::vector<std::vector<Loop>>>
OutlinesThroughCache(const Setup& setup, const std::vector<Body>& bodies,
                     double topZ, const std::vector<double>& passes,
                     Toolpath& tp, GenerateContext* ctx) {
  constexpr double kDeflection = 0.02;
  const bool cached =
      ctx != nullptr && ctx->outlineCache != nullptr && ctx->bodiesKey != 0;
  if (!cached)
    return ProjectedOutlines(bodies, topZ, passes, kDeflection);
  // The bodies arrive FRAMED, so the framing is part of what they are: the
  // caller's fingerprint alone is one per job, and handed setup B -- or the
  // next index of one operation -- setup A's outlines.
  const uint64_t key =
      OutlineKey(ctx->bodiesKey, FramingKey(setup), topZ, passes, kDeflection);
  if (const OutlineResult* hit = ctx->outlineCache->Find(key))
    return hit->outlines;
  OutlineRequest req;
  req.key = key;
  req.bodies = bodies;
  req.topZ = topZ;
  req.passes = passes;
  req.deflection = kDeflection;
  if (ctx->defer) {
    // The worker gets shapes nobody else holds. Its booleans are
    // non-destructive, but the frame may MESH these bodies while it reads
    // them, and a triangulation is written onto the shared TShape -- the
    // race GenerateMeshDetached exists for. Copied HERE, on the thread that
    // owns the originals, so not even the copy reads them concurrently.
    for (Body& b : req.bodies)
      if (b.shape.IsValid())
        b.shape = CADShape(BRepBuilderAPI_Copy(b.shape.GetShape()).Shape());
    ctx->pendingOutline.push_back(std::move(req));
    tp.pending = true;
    return std::nullopt;
  }
  OutlineResult r = OutlinePass(req);
  std::vector<std::vector<Loop>> out = r.outlines;
  ctx->outlineCache->Put(key, std::move(r));
  return out;
}

void GenerateProfile(const Setup& setup, const Operation& op, const Tool& tool,
                     const std::vector<Body>& bodies, Toolpath& tp,
                     GenerateContext* ctx) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  const double radius = tool.diameter * 0.5;
  const double bottom = op.bottomZ + op.stockToLeave;
  const std::vector<double> passes = DepthPasses(op);
  std::string why;
  // A wall names its contour by POSITION, not by value. The outline it lies
  // on changes with depth wherever a feature opens part-way down, so the
  // mid-height loop `PickedWallContour` returns is right at one height and
  // wrong at the rest — the pick is re-resolved per pass below, against the
  // projection. A floor region and a targetless cut keep their own shapes.
  const bool wallPick =
      !op.targets.empty() &&
      IsWallFace(bodies, op.targets.front().ref, op.targets.front().jobPartId);
  WallContour wall;
  if (wallPick) {
    wall = PickedWallContour(bodies, op.targets.front().ref, 0.02,
                             op.targets.front().jobPartId);
    if (!wall.ok) {
      tp.error = wall.why;
      return;
    }
  }
  // A point ON each contour this operation follows, which is how a pick
  // survives depth: the loop it names can merge with a neighbour lower down,
  // and proximity finds the merged boundary where an index or a shape would
  // not. One seed for a wall; one per OPENING for a face cut inside.
  std::vector<glm::dvec2> seeds;
  if (wallPick)
    seeds.push_back(wall.at);
  // Cutting INSIDE a level face means cutting its openings. It cannot mean
  // anything else -- offsetting the face's own outline inward carves a trench
  // a whisker inside the part, full depth, all the way round, which is the
  // failure the refusal below describes and which this pick used to walk
  // straight into, since `TargetPaths` handed a profile the outer wire alone.
  bool openingsPick = false;
  if (!wallPick && !op.targets.empty() && op.side == Side::Inside) {
    const FaceRegion region = PickedFaceRegion(
        bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
    if (!region.ok) {
      tp.error = region.why;
      return;
    }
    std::vector<Loop> openings = FaceOpenings(bodies, region);
    // The bores the drill takes are not this cutter's business.
    if (op.drilledBores > 0.0) {
      std::vector<Loop> keep;
      for (const Loop& l : openings)
        if (l.boreDiameter <= 0.0 ||
            l.boreDiameter > op.drilledBores + kStockFit)
          keep.push_back(l);
      openings = std::move(keep);
    }
    if (openings.empty()) {
      // Both refusals open with the same words on purpose: "nothing to cut
      // in that face" is the state a caller acts on -- a part with no
      // openings is a part that needs no openings operation -- and the rest
      // of the sentence says which kind of nothing it is.
      tp.error =
          op.drilledBores > 0.0
              ? "nothing to cut in that face: every opening in it is a bore "
                "of " +
                    Round2(op.drilledBores) +
                    " mm or less, which `drilled_bores` leaves to the drill"
              : "nothing to cut in that face: it has no openings. Pick a face "
                "that HAS one (every opening in it is cut), or the wall of "
                "the opening you mean -- an inside cut on a face with none "
                "would follow the part's own outline and trench round it.";
      return;
    }
    for (const Loop& l : openings)
      if (!l.pts.empty())
        seeds.push_back(l.pts.front());
    openingsPick = true;
  }
  const Clipper2Lib::Paths64 picked =
      wallPick || openingsPick ? Clipper2Lib::Paths64{}
                               : TargetPaths(op, bodies, /*holesToo=*/false,
                                             tool.diameter * 0.5, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  // The outline the cutter may follow at each depth: the part's projection
  // from the operation's top down to that pass, NOT its cross-section there
  // (ProjectedOutlines, and docs/cam/operations.md#profile for the gouge that
  // is).
  const std::optional<std::vector<std::vector<Loop>>> projectedOr =
      OutlinesThroughCache(setup, bodies, op.topZ, passes, tp, ctx);
  if (!projectedOr)
    return; // pending: the worker is sectioning
  const std::vector<std::vector<Loop>>& projected = *projectedOr;
  // Without a target a profile follows the part's own outline, which is what
  // "cut it out" means and is nonsense for an inside cut: offsetting the
  // perimeter INWARD by a tool radius carves a trench a whisker inside the
  // part, the full depth, all the way round. The first dogfooding round asked
  // for a slot this way and got a 15 mm gouge round the whole outline.
  if (seeds.empty() && picked.empty() && op.side == Side::Inside) {
    tp.error = "an inside profile needs the contour to cut inside of — pick "
               "the wall of the opening. Without one this would follow the "
               "part's own outline and cut a trench round it.";
    return;
  }
  // The contour of the LAST level that had one, kept so a through cut can
  // carry it down past the bottom of the model. `bottomZ` is deliberately
  // below the part — into the spoilboard — and sectioning finds nothing down
  // there, so stopping when the model runs out leaves the part welded to the
  // sheet: the exact failure tabs exist to control.
  Clipper2Lib::Paths64 lastFound;
  // Why nothing came out, if nothing does — see the same field in the pocket.
  std::string tooTight;
  // Where each loop's previous depth pass finished, so the next one carries on
  // from there instead of flying back across the opening at safe height --
  // unless it has a lead-out, which always ends at `leadAt` (below) and so
  // starts its ramp from there, whatever this says.
  std::vector<double> resume;
  // Where each loop's lead-out left the wall. A lap with a lead-out ends at
  // its loop's pts[0] (EmitLoop), so the arcs stack only if pts[0] stays put
  // from pass to pass -- and a wall re-resolved at each depth is a new
  // polygon, free to start anywhere. Each pass is turned to start nearest
  // the last one's lead point.
  std::vector<std::optional<glm::dvec2>> leadAt;
  // Which loop each seed names at each depth. The nearest one, unless the
  // nearest is more than a tool radius from the pick: then the picked wall is
  // not at this depth at all, and what is nearest is some OTHER wall. An
  // opening under a flange recess is the case -- above the recess floor the
  // projection holds only the recess's wall, so the top pass of the opening
  // went round the recess (in air: the pocket had cleared it) and the next
  // pass took the opening's first two stepdowns in one bite. Such a depth
  // cuts the picked wall's loop from the nearest depth that has it, the one
  // below first: the column above an opening is waste to the stock top.
  std::vector<std::vector<const Loop*>> seedLoop(
      passes.size(), std::vector<const Loop*>(seeds.size(), nullptr));
  for (size_t k = 0; k < seeds.size(); k++) {
    std::vector<const Loop*> nearestAt(passes.size(), nullptr);
    std::vector<bool> onPick(passes.size(), false);
    bool anyOnPick = false;
    for (size_t pass = 0; pass < passes.size(); pass++) {
      double best = std::numeric_limits<double>::max();
      for (const Loop& l : projected[pass]) {
        const double d = DistanceToLoop(l.pts, seeds[k]);
        if (d < best) {
          best = d;
          nearestAt[pass] = &l;
        }
      }
      onPick[pass] = nearestAt[pass] != nullptr && best <= radius;
      anyOnPick = anyOnPick || onPick[pass];
    }
    for (size_t pass = 0; pass < passes.size(); pass++) {
      seedLoop[pass][k] = nearestAt[pass];
      if (!anyOnPick || onPick[pass])
        continue;
      for (size_t step = 1; step < passes.size(); step++) {
        if (pass + step < passes.size() && onPick[pass + step]) {
          seedLoop[pass][k] = nearestAt[pass + step];
          break;
        }
        if (pass >= step && onPick[pass - step]) {
          seedLoop[pass][k] = nearestAt[pass - step];
          break;
        }
      }
    }
  }
  for (size_t pass = 0; pass < passes.size(); pass++) {
    const double z = passes[pass];
    std::vector<Loop> solid;
    if (picked.empty()) {
      if (!seeds.empty()) {
        // The loop of THIS depth's projection that each pick lies on. One may
        // have merged with a neighbour since the mid-height section named it;
        // a merged boundary is the right one to follow, and proximity finds
        // it either way. Two seeds landing on the same merged loop cut it
        // once.
        for (const Loop* nearest : seedLoop[pass]) {
          if (nearest == nullptr)
            continue;
          bool already = false;
          for (const Loop& have : solid)
            already = already || have.pts == nearest->pts;
          if (!already)
            solid.push_back(*nearest);
        }
      } else {
        for (const Loop& l : projected[pass])
          if (!l.hole)
            solid.push_back(l);
      }
      for (Loop& l : solid)
        MakeCounterClockwise(l);
    }
    const double delta = op.side == Side::Inside ? -(radius + op.stockToLeave)
                         : op.side == Side::On   ? 0.0
                                                 : radius + op.stockToLeave;
    Clipper2Lib::Paths64 contour = picked.empty() ? ToClipper(solid) : picked;
    if (contour.empty() && lastFound.empty() && picked.empty() &&
        seeds.empty()) {
      // A pass below the model with no pass above it to inherit from -- a
      // ONE-pass through cut, which a finishing pass is (R6). The outline
      // to carry down is the model's own at its bottom.
      double modelBottom = std::numeric_limits<double>::max();
      for (const Body& b : bodies) {
        glm::dvec3 bmin, bmax;
        b.shape.GetBoundingBox(bmin, bmax);
        modelBottom = std::min(modelBottom, static_cast<double>(bmin.z));
      }
      if (modelBottom < op.topZ) {
        const std::optional<std::vector<std::vector<Loop>>> atBottomOr =
            OutlinesThroughCache(setup, bodies, op.topZ, {modelBottom + 1e-3},
                                 tp, ctx);
        if (!atBottomOr) {
          // Pending, and nothing drawn yet is honest: a half-cut profile
          // would read as the whole operation.
          tp.moves.clear();
          tp.tabs.clear();
          tp.reliefs.clear();
          tp.skipped.clear();
          return;
        }
        const std::vector<std::vector<Loop>>& atBottom = *atBottomOr;
        std::vector<Loop> low;
        if (!atBottom.empty())
          for (const Loop& l : atBottom[0])
            if (!l.hole)
              low.push_back(l);
        for (Loop& l : low)
          MakeCounterClockwise(l);
        contour = ToClipper(low);
      }
    }
    if (contour.empty())
      contour = lastFound; // below the model: keep cutting what was there
    else
      lastFound = contour;
    if (contour.empty())
      continue; // above the model, where there has never been anything
    // A toleranced round wall is followed at its band's middle -- on this
    // pass's copy, so the next pass starts from the wall as modelled.
    AimAtBands(contour, z);
    const double lead = op.leadArcs && op.side != Side::On ? radius : 0.0;
    Clipper2Lib::Paths64 paths;
    if (op.side != Side::Inside) {
      // Outside or on: one offset over every contour at once, because two
      // outlines close together must UNION rather than cross each other.
      paths = delta == 0.0 ? contour : Offset(contour, delta);
    } else {
      // Inside: the contours are separate openings and cannot merge, so each
      // is offset on its own -- which is what lets an opening that cannot be
      // cut say so BY NAME instead of taking the whole operation down with
      // it. One operation now follows every opening in a face, and one narrow
      // slot among five is not a reason to cut none of them.
      for (const Clipper2Lib::Path64& src : contour) {
        const Clipper2Lib::Paths64 one =
            Offset(Clipper2Lib::Paths64{src}, delta);
        if (one.empty()) {
          if (tooTight.empty())
            tooTight = TooTightForTool(Clipper2Lib::Paths64{src}, tool,
                                       op.stockToLeave, "opening");
          if (pass == 0)
            tp.skipped.push_back(TooTightForTool(
                Clipper2Lib::Paths64{src}, tool, op.stockToLeave, "opening"));
          continue;
        }
        // A lead arc inside an opening needs a radius more of room than the
        // cut itself: that opening is left, saying so, rather than swept into
        // the far wall.
        if (lead > 0.0 &&
            Offset(Clipper2Lib::Paths64{src}, delta - radius).empty()) {
          if (pass == 0)
            tp.skipped.push_back(
                "an opening here has no room for a lead arc (it needs " +
                Round2(radius) +
                " mm beyond the cut): turn lead arcs off for this operation, "
                "or use a smaller tool");
          continue;
        }
        paths.insert(paths.end(), one.begin(), one.end());
      }
    }
    for (size_t loop = 0; loop < paths.size(); loop++) {
      Clipper2Lib::Path64& path = paths[loop];
      Orient(path, op.climb, op.side == Side::Inside);
      // Tabs on EVERY pass, not only the last one. A tab is a pillar from the
      // bottom of the cut up to `tabHeight`, and a pass that ran through it
      // on the way down left only the part of it below that pass — asking for
      // a 3.8 mm tab and getting 1 mm, which is a part that breaks free under
      // the cutter and is thrown. EmitLoop lifts to max(z, tabZ), so a pass
      // that never reaches the tab top is unaffected.
      //
      // Each loop remembers where its previous pass finished, PER LOOP: a
      // contour can offset into several, and handing loop B the point where
      // loop A stopped would start it somewhere on B that means nothing.
      if (resume.size() <= loop)
        resume.resize(loop + 1, 0.0);
      std::vector<glm::dvec2> pts = FromClipper(path);
      if (op.cornerRelief != CornerRelief::None && op.side != Side::On) {
        std::vector<glm::dvec2> corners;
        pts = AddCornerRelief(pts, op.side == Side::Outside, radius,
                              op.cornerRelief, &corners);
        // Told to the verifier once per corner, not once per pass.
        if (pass == 0)
          for (const glm::dvec2& c : corners)
            tp.reliefs.push_back({c, radius * 2.0});
      }
      if (lead > 0.0 && pts.size() >= 2) {
        if (leadAt.size() <= loop)
          leadAt.resize(loop + 1);
        if (!leadAt[loop]) {
          // The first pass picks the middle of the loop's longest edge: a
          // straight wall to arc off, not a corner, whose rounded offset
          // turns the tangent under the arc.
          size_t longest = 0;
          double best = -1.0;
          for (size_t i = 0; i < pts.size(); i++) {
            const double l = glm::length(pts[(i + 1) % pts.size()] - pts[i]);
            if (l > best) {
              best = l;
              longest = i;
            }
          }
          leadAt[loop] = (pts[longest] + pts[(longest + 1) % pts.size()]) * 0.5;
        }
        pts = StartAtPoint(pts, *leadAt[loop]);
        leadAt[loop] = pts.front();
      }
      // The tabs are recorded from the LAST pass only. Every pass lifts over
      // them, but it is the bottom one that leaves them standing, and a wall
      // pick re-resolves its contour at each depth -- so the bottom pass is
      // the one whose polygon says where the pillars actually are.
      const bool lastPass = pass + 1 == passes.size();
      // Down between passes when nothing else has to happen in between: one
      // loop, no lead-out to leave by, and a pass still to come. The next
      // pass ramps on from where this lap ended (EmitLoop, `stillDown`).
      // Several loops alternate pass by pass, so the tool has to travel.
      const bool stayDown = lead <= 0.0 && paths.size() == 1 && !lastPass;
      resume[loop] =
          EmitLoop(tp, setup, tool, pts, z, setup.safeZ, stockTop, op.tabCount,
                   op.tabWidth, bottom + op.tabHeight,
                   pass == 0 ? op.topZ : passes[pass - 1], Entry::Ramp,
                   resume[loop], /*retract=*/!stayDown, lead,
                   op.side == Side::Outside, lastPass ? &tp.tabs : nullptr);
    }
  }
  // A pass that stayed down for a next one that never came (the contour ran
  // out below it) leaves the tool in the work: out, before anything else.
  if (!tp.moves.empty() && tp.moves.back().to.z < setup.safeZ - 1e-9)
    Emit(tp, Move::Kind::Rapid,
         {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
  // Every opening skipped is an operation that cut nothing, so it fails as
  // one -- in the words of the first thing it could not do, not in a generic
  // "produced no toolpath". With SOME openings cut, the skipped ones stay a
  // warning and the rest of the job goes ahead.
  if (tp.moves.empty() && tp.error.empty())
    tp.error = !tp.skipped.empty() ? tp.skipped.front() : tooTight;
}

// The pocket's wall ring with its corner relief (R4), the inner rings as
// they are: a bone on an inner ring would reach past the wall ring into the
// wall. The material is OUTSIDE a pocket's ring. Told to the verifier once,
// on the first depth pass.
std::vector<glm::dvec2> WallRelief(const Operation& op, const Tool& tool,
                                   const std::vector<glm::dvec2>& ring,
                                   bool wallRing, Toolpath& tp) {
  if (!wallRing || op.cornerRelief == CornerRelief::None)
    return ring;
  const double radius = tool.diameter * 0.5;
  std::vector<glm::dvec2> corners;
  std::vector<glm::dvec2> out = AddCornerRelief(
      ring, /*materialInside=*/false, radius, op.cornerRelief, &corners);
  if (tp.reliefs.empty())
    for (const glm::dvec2& c : corners)
      tp.reliefs.push_back({c, radius * 2.0});
  return out;
}

// Pocket: clear the voids inside the part's outline. Concentric offsets
// inward until the region closes — islands fall out of the boolean, because
// material inside a pocket is simply not part of the region.
void GeneratePocket(const Setup& setup, const Operation& op, const Tool& tool,
                    const std::vector<Body>& bodies, Toolpath& tp) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  const double radius = tool.diameter * 0.5;
  const double step = std::max(0.05, op.stepover) * tool.diameter;
  // A clicked floor face IS the region to clear, at every depth: its outer
  // wire is the pocket wall and its inner wires are islands
  // (docs/cam/operations.md#pocket-and-adaptive).
  std::string why;
  // Round the part, the region is the stock less the part, per pass.
  const bool round = ClearsRound(op, bodies);
  const std::vector<double> passes = DepthPasses(op);
  const std::vector<Clipper2Lib::Paths64> roundRegions =
      round ? ClearRoundRegions(setup, op, tool, bodies, passes)
            : std::vector<Clipper2Lib::Paths64>{};
  const Clipper2Lib::Paths64 picked =
      round ? Clipper2Lib::Paths64{}
            : TargetPaths(op, bodies, /*holesToo=*/true, tool.diameter * 0.5,
                          why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  double entryZ = op.topZ;
  // Why nothing came out, if nothing does. Kept across the depths and used
  // only when the whole operation ends up empty: a pocket that clears at one
  // depth and closes up at the next is doing the right thing, and a message
  // about that would be noise.
  std::string tooTight;
  for (size_t pass = 0; pass < passes.size(); pass++) {
    const double z = passes[pass];
    Clipper2Lib::Paths64 region = round ? roundRegions[pass] : picked;
    if (round && region.empty())
      continue;
    if (region.empty()) {
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      if (loops.empty())
        continue;
      Clipper2Lib::Paths64 outers, holes;
      for (const Loop& l : loops)
        (l.hole ? holes : outers).push_back(ToClipper(l.pts));
      if (holes.empty())
        continue; // nothing enclosed at this level
      // The void = what the outline encloses minus the material.
      region = Clipper2Lib::Difference(holes, Clipper2Lib::Paths64{},
                                       Clipper2Lib::FillRule::NonZero);
    }
    // Collect the concentric rings before emitting any: the LAST non-empty
    // inset brackets the region's inscribed radius (a region shrinks to
    // nothing exactly when the offset passes it), and that is what sizes the
    // helix. First ring sits one radius in from the wall (plus stock).
    AimAtBands(region, z); // a toleranced round wall: its band's middle
    std::vector<Clipper2Lib::Paths64> rings;
    double inset = radius + op.stockToLeave;
    double lastInset = 0.0;
    Clipper2Lib::Paths64 ring = Offset(region, -inset);
    while (!ring.empty()) {
      lastInset = inset;
      rings.push_back(std::move(ring));
      inset += step;
      ring = Offset(region, -inset);
    }
    if (rings.empty()) {
      // The region closed up before the first pass — the cutter is wider than
      // the narrowest part of what it was asked to clear.
      if (tooTight.empty())
        tooTight = FloorWidthRefusal(op, bodies, region, !picked.empty(), tool);
      continue;
    }

    // The loops, each at the inset LEVEL it came from (0 = the wall).
    struct PocketLoop {
      Clipper2Lib::Path64 path;
      std::vector<glm::dvec2> pts;
      int level = 0;
      int parent = -1;
      std::vector<int> kids;
    };
    std::vector<PocketLoop> loops;
    // Drop a loop that clears nothing
    // (docs/cam/operations.md#pocket-and-adaptive). Insetting goes on until the
    // region vanishes, so where two walls converge the last loop can fall in a
    // band its neighbours have already swept: in the baffle's W170S recess, a
    // whole lap of air at r = 82 on every depth pass. What the loops so far
    // sweep is kept as a union of their tool-radius bands, from the walls
    // inward; a loop whose own band adds nothing to it is not cut. A wall never
    // goes.
    Clipper2Lib::Paths64 swept;
    for (size_t level = 0; level < rings.size(); level++)
      for (const Clipper2Lib::Path64& path : rings[level]) {
        const Clipper2Lib::Paths64 band = Clipper2Lib::InflatePaths(
            Clipper2Lib::Paths64{path}, radius * kScale,
            Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Joined);
        if (level > 0) {
          const Clipper2Lib::Paths64 fresh = Clipper2Lib::Difference(
              band, swept, Clipper2Lib::FillRule::NonZero);
          // A thousandth, which is arithmetic, not a whisker of material:
          // at 0.05 this dropped the baffle's middle ring, whose neighbours
          // stopped 0.05 mm apart, and left a hair of MDF 3 mm tall all the
          // way round the recess.
          if (Offset(fresh, -0.001).empty())
            continue;
        }
        swept = Clipper2Lib::Union(swept, band, Clipper2Lib::FillRule::NonZero);
        PocketLoop l;
        l.path = path;
        Orient(l.path, op.climb, /*insideMaterial=*/true);
        l.pts = FromClipper(l.path);
        l.level = static_cast<int>(level);
        loops.push_back(std::move(l));
      }
    // Each loop's parent: the nearest loop one level nearer the wall (or
    // further, where a level was dropped). On an annulus that is the loop on
    // the same side of the band, a stepover away -- not the one across it.
    for (size_t i = 0; i < loops.size(); i++) {
      double best = std::numeric_limits<double>::max();
      for (size_t j = 0; j < loops.size(); j++) {
        if (loops[j].level >= loops[i].level || loops[i].pts.empty())
          continue;
        const double d = DistanceToLoop(loops[j].pts, loops[i].pts.front()) +
                         1e3 * (loops[i].level - 1 - loops[j].level);
        if (d < best) {
          best = d;
          loops[i].parent = static_cast<int>(j);
        }
      }
      if (loops[i].parent >= 0)
        loops[loops[i].parent].kids.push_back(static_cast<int>(i));
    }
    // INSIDE OUT, walls last: every loop after the loops it contains, so the
    // full-width first cut is in the middle of the region and the wall is a
    // stepover-wide finishing cut instead of the slot that forms it. And
    // depth-first down each branch, so the next loop is a neighbour a
    // stepover away and EmitRing links it at depth -- cut level by level, an
    // annulus alternated between its two edges, 14 mm apart, and re-entered
    // by ramp on every ring. FreeCAD's default order; the linking is
    // Kiri:Moto's and blendercam's.
    std::vector<int> order;
    std::vector<bool> done(loops.size(), false);
    glm::dvec2 at(0.0);
    bool haveAt = false;
    const std::function<void(int)> visit = [&](int i) {
      std::vector<int> kids = loops[i].kids;
      while (!kids.empty()) {
        // The nearest branch first, from wherever the tool will be.
        auto next = kids.begin();
        if (haveAt) {
          double best = std::numeric_limits<double>::max();
          for (auto k = kids.begin(); k != kids.end(); ++k) {
            const double d = DistanceToLoop(loops[*k].pts, at);
            if (d < best) {
              best = d;
              next = k;
            }
          }
        }
        const int k = *next;
        kids.erase(next);
        visit(k);
      }
      order.push_back(i);
      done[i] = true;
      if (!loops[i].pts.empty()) {
        at = loops[i].pts.front();
        haveAt = true;
      }
    };
    for (;;) {
      int root = -1;
      double best = std::numeric_limits<double>::max();
      for (size_t i = 0; i < loops.size(); i++)
        if (!done[i] && loops[i].parent < 0) {
          const double d = haveAt ? DistanceToLoop(loops[i].pts, at)
                                  : static_cast<double>(i);
          if (d < best) {
            best = d;
            root = static_cast<int>(i);
          }
        }
      if (root < 0)
        break;
      visit(root);
    }
    if (order.empty())
      continue;

    // Does the ramp fit on the loop we cut first? If not, the descent on
    // that loop would be far steeper than asked (C6), and a helix goes down
    // inside it instead -- when that loop is the innermost, which is where
    // the helix is sized for.
    const PocketLoop& first = loops[order.front()];
    const double descend = std::max(0.0, entryZ - z);
    const double rampLen =
        descend > 1e-9 ? descend / std::tan(glm::radians(kRampAngleDeg)) : 0.0;
    // The tool CENTRE's circle: the inscribed radius less the cutter and the
    // stock being left. A helix narrower than a tenth of the cutter is a
    // plunge with extra steps, so that is where the fallback stays.
    const double helixRadius = lastInset - radius - op.stockToLeave;
    const bool helix = rampLen > Perimeter(first.pts) &&
                       first.level + 1 == static_cast<int>(rings.size()) &&
                       helixRadius > tool.diameter * 0.1 && descend > 1e-9;
    if (helix) {
      glm::dvec2 centre(0.0);
      for (const glm::dvec2& p : first.pts)
        centre += p;
      centre /= static_cast<double>(first.pts.size());
      EmitHelix(tp, tool, centre, helixRadius, std::max(entryZ, z), z,
                setup.safeZ, stockTop, kDeflection);
    }
    for (const int i : order)
      EmitRing(tp, setup, tool,
               WallRelief(op, tool, loops[i].pts, loops[i].level == 0, tp), z,
               stockTop, step, entryZ);
    Emit(tp, Move::Kind::Rapid,
         {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
    entryZ = z;
  }
  if (tp.moves.empty() && tp.error.empty())
    tp.error = tooTight;
}

void EmitRing(Toolpath& tp, const Setup& setup, const Tool& tool,
              const std::vector<glm::dvec2>& pts, double z, double stockTop,
              double step, double entryZ) {
  if (pts.size() < 2)
    return;
  const bool atDepth =
      !tp.moves.empty() && std::abs(tp.moves.back().to.z - z) < 1e-9;
  if (atDepth) {
    const glm::dvec2 here(tp.moves.back().to.x, tp.moves.back().to.y);
    // The nearest POINT on the ring, not its nearest vertex: a lap ends
    // wherever its ramp put it, often mid-edge, and a rectangle's nearest
    // vertex from there is a corner 20 mm off -- which failed the link below
    // on every ring and re-entered each one from safe height.
    const std::vector<glm::dvec2> rotated = StartAtPoint(pts, here);
    // A step-over and a half: enough for the neighbouring ring, not enough to
    // reach a separate region.
    if (glm::length(rotated.front() - here) <= step * 1.5) {
      (void)EmitLoop(tp, setup, tool, rotated, z, setup.safeZ, stockTop, 0, 0.0,
                     z, z, Entry::AtDepth, 0.0, /*retract=*/false);
      return;
    }
  }
  (void)EmitLoop(tp, setup, tool, pts, z, setup.safeZ, stockTop, 0, 0.0, z,
                 entryZ, Entry::Ramp, 0.0, /*retract=*/false);
}

void GeneratePocket(const Setup& setup, const Operation& op, const Tool& tool,
                    const std::vector<Body>& bodies, Toolpath& tp);

// Adaptive: the same region a pocket clears, handed to libarea's Adaptive2d
// (C8). Everything about WHICH region and at what depths is shared with the
// pocket; only the clearing strategy differs, which is the point of putting
// the split here rather than in a second operation type's worth of geometry.
void GenerateAdaptive(const Setup& setup, const Operation& op, const Tool& tool,
                      const std::vector<Body>& bodies, Toolpath& tp,
                      GenerateContext* ctx) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  if (!AdaptiveAvailable()) {
    // Not a refusal: the job still has to cut. The concentric pocket is the
    // strategy this build has, and saying so beats producing nothing.
    GeneratePocket(setup, op, tool, bodies, tp);
    tp.strategy = "concentric (this build has no adaptive clearing)";
    return;
  }
  std::string why;
  // Round the part, the region is the stock less the part, per pass, and it
  // is cleared from INSIDE that region: Adaptive2d's own outside clearing
  // wants the stock as a second boundary, which this bridge does not hand
  // it.
  const bool round = ClearsRound(op, bodies);
  const std::vector<double> passes = DepthPasses(op);
  const std::vector<Clipper2Lib::Paths64> roundRegions =
      round ? ClearRoundRegions(setup, op, tool, bodies, passes)
            : std::vector<Clipper2Lib::Paths64>{};
  const Clipper2Lib::Paths64 roundAir =
      round ? ClearRoundAir(setup, tool, bodies) : Clipper2Lib::Paths64{};
  Operation clearing = op;
  if (round)
    clearing.side = Side::Inside;
  const Clipper2Lib::Paths64 picked =
      round ? Clipper2Lib::Paths64{}
            : TargetPaths(op, bodies, /*holesToo=*/true, tool.diameter * 0.5,
                          why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  double entryZ = op.topZ;
  // Why nothing came out, if nothing does — see the same field in the pocket.
  std::string tooTight;
  // One pass was handed to the caller's worker instead of run here. Every
  // later pass that is not cached goes the same way — the whole operation is
  // queued in one go, in depth order — and nothing below the queueing is
  // allowed to run on a partial result.
  bool deferred = false;
  for (size_t pass = 0; pass < passes.size(); pass++) {
    const double z = passes[pass];
    Clipper2Lib::Paths64 region = round ? roundRegions[pass] : picked;
    if (round && region.empty())
      continue;
    if (region.empty()) {
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      Clipper2Lib::Paths64 holes;
      for (const Loop& l : loops)
        if (l.hole)
          holes.push_back(ToClipper(l.pts));
      if (holes.empty())
        continue;
      region = Clipper2Lib::Difference(holes, Clipper2Lib::Paths64{},
                                       Clipper2Lib::FillRule::NonZero);
    }
    if (region.empty())
      continue;
    AimAtBands(region, z); // a toleranced round wall: its band's middle
    std::vector<Loop> asLoops;
    for (const Clipper2Lib::Path64& path : region) {
      Loop l;
      l.pts = FromClipper(path);
      l.hole = false;
      asLoops.push_back(std::move(l));
    }
    // Round a part, the ring between the stock and the region's grown edge
    // is air: handed over as already clear, so the pass does not cut it.
    for (const Clipper2Lib::Path64& path : roundAir) {
      Loop l;
      l.pts = FromClipper(path);
      l.hole = true;
      asLoops.push_back(std::move(l));
    }
    // The pass: from the cache, from a worker later, or right here.
    const bool cached = ctx != nullptr && ctx->cache != nullptr;
    const uint64_t key =
        cached ? AdaptiveKey(setup, clearing, tool, asLoops, z, entryZ) : 0;
    const AdaptiveResult* hit = cached ? ctx->cache->Find(key) : nullptr;
    if (hit == nullptr && cached && ctx->defer) {
      ctx->pending.push_back(
          MakeAdaptiveRequest(setup, clearing, tool, asLoops, z, entryZ));
      deferred = true;
      entryZ = z;
      continue;
    }
    if (deferred) {
      entryZ = z;
      continue;
    }
    AdaptiveResult computed;
    if (hit == nullptr) {
      computed = AdaptiveClear(setup, clearing, tool, asLoops, z, entryZ);
      if (cached)
        ctx->cache->Put(key, computed);
    }
    const AdaptiveResult& cleared = hit != nullptr ? *hit : computed;
    if (cleared.error.empty() && cleared.moves.empty() &&
        clearing.side == Side::Outside) {
      // Adaptive is the one clearing strategy that reads `side`, and outside
      // a region whose own boundary is the stock there is nothing to clear —
      // it returns silence rather than a complaint. The concentric pocket
      // ignores `side` entirely, so the same operation cut one way and
      // vanished the other, under the generic "produced no toolpath".
      tp.error = "this operation clears OUTSIDE its boundary, and there is "
                 "nothing out there to cut — a pocket clears inside "
                 "(side: inside)";
      return;
    }
    if (!cleared.error.empty()) {
      // Adaptive declined this region — a vertex layout the vendored library
      // throws on, a shape it cannot lead into, a start point it cannot find.
      // The pocket still has to be cleared, and the concentric strategy is
      // right here and cuts the same region: falling back is what the
      // no-library branch above already does, and giving up instead left the
      // user clicking "Clear a pocket" at a face that never changed
      // (docs/cam/operations.md#pocket-and-adaptive). Which strategy cut it is
      // REPORTED, not hidden — a job that quietly changed strategy is a job
      // whose feeds were chosen for the other one.
      //
      // Declined WITH moves too. Adaptive2d flags trouble on an output it
      // still fills -- a lead it could not plan, area it left -- and keeping
      // that output beside the error posted a program for an operation that
      // reported failure: the chess bishop's 3.2 mm mitre came out as 9229
      // moves AND an error, its setup counted failed, and the verifier
      // passed the file. A half-trusted pass is not a result; the rings
      // cut the same notch in 343 mm, cleanly, with nothing to report.
      tp.moves.clear();
      GeneratePocket(setup, op, tool, bodies, tp);
      tp.strategy =
          "concentric — adaptive declined this shape (" + cleared.error + ")";
      return;
    }
    if (cleared.error.empty() && cleared.moves.empty() && tooTight.empty()) {
      // Adaptive returns SILENCE for a region its cutter cannot enter — no
      // moves and no complaint — so the width has to be measured here or the
      // operation lands on the generic "check its depths". This is the path a
      // real pocket takes: a derived pocket deep enough comes back as
      // `adaptive`, so it is the one a person actually hits.
      tooTight = FloorWidthRefusal(op, bodies, region, !picked.empty(), tool);
    }
    tp.moves.insert(tp.moves.end(), cleared.moves.begin(), cleared.moves.end());
    entryZ = z;
  }
  if (deferred) {
    // Nothing this operation shows may be a partial cut: the caller decides
    // what to draw in the meantime (the previous toolpath, by preference).
    tp.moves.clear();
    tp.error.clear();
    tp.strategy.clear();
    tp.reliefs.clear();
    tp.tabs.clear();
    tp.pending = true;
    return;
  }
  if (tp.moves.empty() && tp.error.empty())
    tp.error = tooTight;
}

// Face: clear the stock top down to topZ over the whole stock rectangle.
void GenerateFace(const Setup& setup, const Operation& op, const Tool& tool,
                  const std::vector<Body>& bodies, Toolpath& tp) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, bodies, lo, hi)) {
    tp.error = "no bodies, so there is no stock to face";
    return;
  }
  const double radius = tool.diameter * 0.5;
  const double step = std::max(0.05, op.stepover) * tool.diameter;
  std::vector<glm::dvec2> rect = {
      {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
  const Clipper2Lib::Paths64 region{ToClipper(rect)};
  double entryZ = op.topZ;
  for (const double z : DepthPasses(op)) {
    // Start with the cutter centred ON the stock edge so the outside is
    // cleaned up, then spiral inward.
    double inset = 0.0;
    Clipper2Lib::Paths64 ring = Offset(region, -inset);
    while (!ring.empty()) {
      for (Clipper2Lib::Path64& path : ring) {
        Orient(path, op.climb, /*insideMaterial=*/true);
        EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, stockTop,
                 0, 0.0, z, entryZ);
      }
      inset += step;
      if (inset > glm::length(hi - lo))
        break; // guard: a degenerate stock must not spin forever
      ring = Offset(region, -inset);
    }
    entryZ = z;
    (void)radius;
  }
}

// The radius of a cylindrical face, for the one question a drill has to ask:
// is this tool the size of this hole?
bool BoreRadius(const Body& body, int idx, double& radius) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++) {
    if (i != idx)
      continue;
    BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
    if (surf.GetType() != GeomAbs_Cylinder)
      return false;
    radius = surf.Cylinder().Radius();
    return radius > 0.0;
  }
  return false;
}

// Corner relief (R4). `pts` is a TOOL-CENTRE loop -- the wall pass, one
// radius off the material -- and `materialInside` says which side of it the
// part is on (an outside profile: inside the loop; a pocket or an inside
// profile: outside it). A sharp corner of that loop is a corner of the
// material the round cutter cannot reach: on an outside profile it shows as
// a REFLEX vertex (the part's inner corner), on a pocket as a CONVEX one
// (the pocket's corner), and in both the material's corner lies away from the
// corner's own small wedge -- along `u - v`, the incoming direction minus the
// outgoing. Out that way by `length` and back is the dogbone; along the
// longer of the two walls and back is the T-bone. Corners gentler than 30
// degrees are left alone: the arcs an offset puts on convex corners are many
// small kinks, and a relief on each would be a serrated wall.
std::vector<glm::dvec2> AddCornerRelief(const std::vector<glm::dvec2>& pts,
                                        bool materialInside, double length,
                                        CornerRelief style,
                                        std::vector<glm::dvec2>* corners) {
  const size_t n = pts.size();
  if (n < 3 || style == CornerRelief::None || length <= 1e-9)
    return pts;
  double area2 = 0.0;
  for (size_t i = 0; i < n; i++) {
    const glm::dvec2& a = pts[i];
    const glm::dvec2& b = pts[(i + 1) % n];
    area2 += a.x * b.y - b.x * a.y;
  }
  const bool ccw = area2 > 0.0;
  std::vector<glm::dvec2> out;
  out.reserve(n + 12);
  constexpr double kMinTurnDeg = 30.0;
  for (size_t i = 0; i < n; i++) {
    const glm::dvec2& prev = pts[(i + n - 1) % n];
    const glm::dvec2& p = pts[i];
    const glm::dvec2& next = pts[(i + 1) % n];
    out.push_back(p);
    const glm::dvec2 din = p - prev, dout = next - p;
    const double lin = glm::length(din), lout = glm::length(dout);
    if (lin < 1e-9 || lout < 1e-9)
      continue;
    const glm::dvec2 u = din / lin, v = dout / lout;
    const double cross = u.x * v.y - u.y * v.x;
    const double turn =
        glm::degrees(std::atan2(std::abs(cross), glm::dot(u, v)));
    if (turn < kMinTurnDeg)
      continue;
    const bool leftTurn = cross > 0.0;
    const bool convex = leftTurn == ccw; // w.r.t. the loop's interior
    const bool reachable = materialInside ? convex : !convex;
    if (reachable)
      continue; // the cutter gets into this corner already
    glm::dvec2 dir;
    if (style == CornerRelief::Dogbone) {
      dir = u - v;
      if (glm::length(dir) < 1e-9)
        continue; // a 180-degree turn has no bisector to speak of
      dir = glm::normalize(dir);
    } else {
      dir = lin >= lout ? u : -v;
    }
    out.push_back(p + dir * length);
    out.push_back(p);
    if (corners != nullptr)
      corners->push_back(p);
  }
  return out;
}

// Drill: one plunge per target hole, at the hole's axis.
void GenerateDrill(const Setup& setup, const Operation& op, const Tool& tool,
                   const std::vector<Body>& bodies, Toolpath& tp) {
  if (op.targets.empty()) {
    tp.error = "drill needs target holes; pick the cylindrical faces to drill";
    return;
  }
  int found = 0;
  for (const Target& t : op.targets) {
    const ElementRef& ref = t.ref;
    for (const Body& body : bodies) {
      if (body.createdBy != ref.featureId)
        continue;
      const std::optional<int> idx = body.elementMap.Resolve(ref.name);
      if (!idx)
        continue;
      glm::dvec3 origin(0.0), axis(0.0), xAxis(0.0);
      if (!body.shape.FaceAxis(*idx, origin, axis, xAxis))
        continue; // not a cylinder: silently skipping would drill nothing
      // A plunge goes straight down, so a bore lying on its side is not a
      // hole this operation can make — it would be drilled somewhere the
      // part has no hole at all.
      if (std::abs(axis.z) < 0.99) {
        tp.error = "that hole does not point at the spindle — index the setup "
                   "to bring it up, or drill it in another setup";
        return;
      }
      // The size the cutter makes IS the size of the hole. A 3 mm tool
      // plunged into a 6 mm bore leaves a 6 mm hole half cut and nothing said
      // so: the first dogfooding round posted exactly that, and it showed up
      // only as 42 mm2 of "leftover" among the tabs and the slug.
      double radius = 0.0;
      if (BoreRadius(body, *idx, radius)) {
        // A toleranced bore is cut to its band's middle: the rings, the
        // wall's last, are offset from that circle and not the modelled one.
        radius += AimAtBore(body, *idx);
        const double dia = radius * 2.0;
        if (tool.diameter > dia + 1e-6) {
          tp.error = "tool " + std::to_string(op.toolNumber) + " is " +
                     Round2(tool.diameter) + " mm and that hole is " +
                     Round2(dia) + " mm — it would not fit in";
          return;
        }
        if (tool.diameter < dia - 1e-3) {
          // A hole WIDER than the tool is bored (R3): a helix down at the
          // ramp angle on the circle the tool centre has to walk, then a
          // flat turn at the bottom -- and for a hole wider than two tools,
          // rings from the centre outwards by the stepover, the wall last.
          // The user pointed at a hole; the tool decides how it is made, the
          // same rule Break an edge follows. This used to be a refusal
          // ("a plunge makes a hole the size of the cutter"), which is true
          // and was the wrong answer.
          found++;
          const glm::dvec2 centre(origin.x, origin.y);
          const double wall = radius - tool.diameter * 0.5 - op.stockToLeave;
          if (wall < 1e-6) {
            tp.error = "tool " + std::to_string(op.toolNumber) + " is " +
                       Round2(tool.diameter) + " mm and that hole is " +
                       Round2(dia) + " mm: nothing is left to bore once " +
                       Round2(op.stockToLeave) + " mm is left on the wall";
            return;
          }
          glm::dvec3 stockLo(0.0), stockHi(0.0);
          const double stockTop =
              StockBoundsFramed(setup, bodies, stockLo, stockHi) ? stockHi.z
                                                                 : setup.safeZ;
          const double step = std::max(
              tool.diameter * std::clamp(op.stepover, 0.05, 0.95), 0.1);
          std::vector<double> rings;
          // A centre pass only when the cutter cannot reach the middle from
          // the wall ring: a 7 mm hole with a 6 mm tool is one ring.
          for (double r = wall; r > 1e-9; r -= step)
            rings.push_back(r);
          if (rings.back() > step * 0.5 && wall > tool.diameter * 0.5)
            rings.push_back(std::min(rings.back(), step * 0.5));
          std::sort(rings.begin(), rings.end()); // inner first, the wall last
          for (const double r : rings)
            EmitHelix(tp, tool, centre, r, op.topZ, op.bottomZ, setup.safeZ,
                      stockTop, kDeflection);
          // Out of the finished hole straight up, then over: one rapid to
          // the centre at safe Z from the wall ring's bottom climbed and
          // travelled at once.
          Emit(tp, Move::Kind::Rapid,
               {tp.moves.back().to.x, tp.moves.back().to.y, setup.safeZ}, 0.0);
          Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
          break;
        }
      }
      found++;
      const glm::dvec3 centre(origin.x, origin.y, 0.0);
      Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
      // Down to the top of the cut at rapid, and only then at feed. Feeding
      // the whole way from Safe Z spends seconds per hole cutting air — the
      // same reason a profile rapids to the stock top before it ramps.
      if (op.topZ < setup.safeZ - 1e-9)
        Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, op.topZ}, 0.0);
      if (op.peck > 1e-9 && op.topZ - op.bottomZ > op.peck + 1e-9) {
        // Pecks (R3): down one peck, all the way back out to the hole's top
        // so the chips clear (the G83 reading, not G73's short lift), rapid
        // back to just above the floor of the last peck, and on.
        double z = op.topZ;
        while (z > op.bottomZ + 1e-9) {
          const double next = std::max(op.bottomZ, z - op.peck);
          Emit(tp, Move::Kind::Plunge, {centre.x, centre.y, next},
               tool.feedPlunge);
          z = next;
          if (z > op.bottomZ + 1e-9) {
            Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, op.topZ}, 0.0);
            Emit(tp, Move::Kind::Rapid,
                 {centre.x, centre.y, z + std::min(op.peck * 0.1, 0.5)}, 0.0);
          }
        }
      } else {
        Emit(tp, Move::Kind::Plunge, {centre.x, centre.y, op.bottomZ},
             tool.feedPlunge);
      }
      Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
      break;
    }
  }
  if (found == 0)
    tp.error = "none of the drill targets resolved to a cylindrical face";
}

void GenerateEdgeRound(const Setup& setup, const Operation& op,
                       const Tool& tool, const std::vector<Body>& bodies,
                       Toolpath& tp) {
  // Above this there can be no material, so it is the lowest height an entry
  // may RAPID to; below it the descent feeds (EmitDescent).
  glm::dvec3 stockLo(0.0), stockHi(0.0);
  const double stockTop = StockBoundsFramed(setup, bodies, stockLo, stockHi)
                              ? stockHi.z
                              : setup.safeZ;
  if (op.targets.empty()) {
    tp.error = "an edge break follows a wall — pick the vertical face whose "
               "top edge to round or chamfer";
    return;
  }
  const WallContour wall = PickedWallContour(
      bodies, op.targets.front().ref, 0.02, op.targets.front().jobPartId);
  if (!wall.ok) {
    tp.error = wall.why;
    return;
  }
  const EdgeBreak brk =
      EdgeBreakFor(tool, op.edgeSize, std::max(0.0, op.topZ - op.bottomZ));
  if (!brk.ok) {
    tp.error = brk.why;
    return;
  }
  const double r = tool.diameter * 0.5;
  const double top = op.topZ;
  // An opening is cut from inside itself, so "into the material" is the
  // direction that GROWS its contour; round the outside of a part and it is
  // the one that shrinks it.
  const double sign = wall.opening ? 1.0 : -1.0;
  const Clipper2Lib::Paths64 base{ToClipper(wall.loop.pts)};

  const auto lap = [&](double delta, double z) {
    Clipper2Lib::Paths64 paths =
        std::abs(delta) < 1e-9 ? base : Offset(base, delta);
    for (Clipper2Lib::Path64& path : paths) {
      Orient(path, op.climb, wall.opening);
      EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, stockTop,
               /*tabCount=*/0, /*tabWidth=*/0.0, /*tabZ=*/0.0,
               /*entryZ=*/top);
    }
  };

  if (tool.type == ToolType::VBit) {
    // One pass: the axis on the contour, down by the bevel's own depth.
    lap(0.0, top - brk.depth);
    return;
  }
  // Arc passes from a graze on the flat top round to the wall. The step is
  // the cusp control the stepover already means everywhere else.
  const double R = brk.size;
  const double arc = R * glm::half_pi<double>();
  const double step = std::max(1e-3, op.stepover * tool.diameter);
  const int passes = std::max(1, static_cast<int>(std::ceil(arc / step)));
  for (int i = 0; i <= passes; i++) {
    const double t =
        glm::half_pi<double>() *
        (1.0 - static_cast<double>(i) / static_cast<double>(passes));
    const double inward = R - (R + r) * std::cos(t);
    // Every Z here is the tool's LOWEST POINT, the control point the post and
    // the verifier both use — so the ball's centre height is r above it.
    lap(sign * inward, top - R + (R + r) * std::sin(t) - r);
  }
}

// Which generators read `op.targets.front()` and stop there. Drill walks the
// list itself (it is what "drill these holes" means) and Face takes no target
// at all; the other four resolve ONE contour, so several targets on one of
// them is several runs -- see GenerateFramed.

void GenerateOne(const Setup& setup, const Operation& op, const Tool& tool,
                 const std::vector<Body>& bodies, Toolpath& tp,
                 GenerateContext* ctx) {
  switch (op.type) {
  case OpType::Profile:
    GenerateProfile(setup, op, tool, bodies, tp, ctx);
    break;
  case OpType::Pocket:
    GeneratePocket(setup, op, tool, bodies, tp);
    break;
  case OpType::Face:
    GenerateFace(setup, op, tool, bodies, tp);
    break;
  case OpType::Drill:
    GenerateDrill(setup, op, tool, bodies, tp);
    break;
  case OpType::Adaptive:
    GenerateAdaptive(setup, op, tool, bodies, tp, ctx);
    break;
  case OpType::EdgeRound:
    GenerateEdgeRound(setup, op, tool, bodies, tp);
    break;
  case OpType::SurfaceParallel:
  case OpType::SurfaceWaterline:
  case OpType::SurfaceRotary:
    SurfaceThroughCache(setup, op, tool, bodies, tp, ctx);
    break;
  case OpType::Engrave:
    GenerateEngrave(setup, op, tool, bodies, tp, ctx);
    break;
  case OpType::ThreadMill:
    GenerateThreadMill(setup, op, tool, bodies, tp);
    break;
  case OpType::Count:
    // Not a type. Named so the switch stays exhaustive and a REAL new type
    // still breaks the build rather than landing here.
    tp.error = "not an operation type";
    break;
  }
}

} // namespace

Toolpath GenerateFramed(const Setup& setup, const Operation& op,
                        const std::vector<Body>& bodies, GenerateContext* ctx) {
  Toolpath tp;
  tp.operationId = op.id;
  tp.toolNumber = op.toolNumber;

  const Tool* tool = setup.FindTool(op.toolNumber);
  if (tool == nullptr) {
    tp.error = "no tool " + std::to_string(op.toolNumber) + " in this setup";
    return tp;
  }
  if (tool->diameter <= 0.0) {
    tp.error = "tool diameter must be positive";
    return tp;
  }
  if (op.bottomZ >= op.topZ) {
    tp.error = "the bottom of the cut must be below its top";
    return tp;
  }
  // A thread mill's head is a disc on a neck: it cuts threads and nothing
  // else (docs/cam/operations.md#thread-mill).
  if (tool->type == ToolType::ThreadMill && op.type != OpType::ThreadMill) {
    tp.error = "tool " + std::to_string(op.toolNumber) +
               " is a thread mill -- it cuts threads and nothing else";
    return tp;
  }

  // The toleranced walls these bodies carry, for every strategy below to
  // aim at, and the floor this operation ends on when its distance from a
  // reference face is toleranced; the ones the path followed are declared
  // on it.
  FitScope fits(bodies);
  const Operation aimed = fits.AimAtFloor(op);

  // ONE RUN PER TARGET, for the generators that resolve a single contour.
  // Before this they read targets.front() and dropped the rest in silence:
  // five bolt holes picked into one operation bored the first and left four
  // standing, and the file posted at full length with nothing to say about
  // it. What comes out is what N one-target operations would have produced,
  // in the order the targets were picked; a target that cannot be followed
  // goes in `skipped` rather than failing the four that can.
  if (op.targets.size() > 1 && FollowsOneTargetAtATime(op.type)) {
    Operation one = aimed;
    for (size_t i = 0; i < op.targets.size(); i++) {
      one.targets = {op.targets[i]};
      Toolpath sub;
      GenerateOne(setup, one, *tool, bodies, sub, ctx);
      if (sub.error.empty() && sub.moves.empty() && !sub.pending)
        sub.error = "it produced no toolpath — check the operation's "
                    "depths against the model";
      if (!sub.error.empty()) {
        tp.skipped.push_back("target " + std::to_string(i + 1) + " of " +
                             std::to_string(op.targets.size()) + " (" +
                             op.targets[i].ref.name + "): " + sub.error);
        continue;
      }
      tp.pending = tp.pending || sub.pending;
      if (tp.strategy.empty())
        tp.strategy = sub.strategy;
      // A declared arc names moves by index, so it moves with them.
      for (Toolpath::Arc arc : sub.arcs) {
        arc.first += tp.moves.size();
        arc.last += tp.moves.size();
        tp.arcs.push_back(arc);
      }
      tp.threads.insert(tp.threads.end(), sub.threads.begin(),
                        sub.threads.end());
      tp.moves.insert(tp.moves.end(), sub.moves.begin(), sub.moves.end());
      tp.reliefs.insert(tp.reliefs.end(), sub.reliefs.begin(),
                        sub.reliefs.end());
      tp.tabs.insert(tp.tabs.end(), sub.tabs.begin(), sub.tabs.end());
      tp.grooves.insert(tp.grooves.end(), sub.grooves.begin(),
                        sub.grooves.end());
    }
    // Every target refused: there is no operation left, so it fails as one.
    if (tp.moves.empty() && !tp.pending && !tp.skipped.empty())
      tp.error = "no target of this operation could be followed. " +
                 tp.skipped.front();
  } else {
    GenerateOne(setup, aimed, *tool, bodies, tp, ctx);
  }
  if (tp.moves.empty() && tp.error.empty() && !tp.pending)
    tp.error = "this operation produced no toolpath — check its depths "
               "against the model";
  if (tp.error.empty())
    fits.Declare(op.id, tp);
  // Cut what fits: clipped HERE, at the one place every toolpath is made,
  // and not in GenerateAll -- the panel regenerates a verb's operation on
  // its own through this function, and a clip that lived only in the full
  // pass left the verb's path running off the bed while the RPC's file
  // stopped at it (found live, the morning after it shipped).
  // Kept out of the chuck and the tailstock
  // (docs/cam/rotary.md#chuck-and-tailstock): every toolpath of a setup held in
  // the rotary, continuous or indexed, is clipped to the window along X between
  // them -- the tool's radius and the clearance inside each. The travel's
  // clip-retract-reenter, with its own count: the sampler part's whole-length
  // passes cut through the jaws and down to 5 mm from the axis over the
  // tailstock's centre.
  if (tp.error.empty() && tool != nullptr) {
    const HoldingEnvelope env = HoldingEnvelopeOf(setup, bodies);
    if (env.ok) {
      const double r = tool->diameter * 0.5;
      const glm::dvec3 lo(env.MinX(r), -1e9, -1e9);
      const glm::dvec3 hi(std::min(env.MaxX(r), 1e9), 1e9, 1e9);
      const double before = tp.clippedMm;
      if (lo.x < hi.x)
        ClipToMachine(tp, lo, hi, setup.safeZ, tool->feedPlunge);
      else
        tp.moves.clear(); // the window is shut: nothing may be cut
      tp.holdingClippedMm = tp.clippedMm - before;
      tp.clippedMm = before;
      if (tp.moves.empty() || tp.CutLength() <= 0.0) {
        tp.moves.clear();
        tp.error = "everything this operation would cut lies in the chuck or "
                   "the tailstock (the part is held from X" +
                   Round2(env.x0) + " to X" + Round2(env.chuckTo) +
                   (env.tail ? ", the centre in at X" + Round2(env.x1) : "") +
                   ")";
      }
    }
  }
  glm::dvec3 bedLo(0.0), bedHi(0.0);
  if (tp.error.empty() && MachineBoundsFramed(setup, bodies, bedLo, bedHi)) {
    ClipToMachine(tp, bedLo, bedHi, setup.safeZ,
                  tool != nullptr ? tool->feedPlunge : 100.0);
    // Clipped away ENTIRELY: this operation is off the bed, which is not the
    // same as empty. The "produced no toolpath" check above runs BEFORE the
    // clip, so a station beyond the travel came back with no moves, no error
    // and `failed: 0` -- a file that posts at full length with a whole
    // station missing from it (found on a 1122 mm baffle, 900 mm of Y).
    if (tp.clippedMm > 0.0 && tp.CutLength() <= 0.0) {
      tp.moves.clear();
      tp.error =
          "every cut of this operation lies beyond " +
          (setup.machine.name.empty() ? std::string("the machine")
                                      : setup.machine.name) +
          "'s travel, so none of it is in the file: move the part on the "
          "stock, or cut it in a second setup with the sheet slid along";
    }
  }
  // The index this setup is cut at rides on every move, so the post emits it
  // and the machine is never asked to guess which way the part is facing.
  // How the part is CLAMPED never gets here: it is already baked into the
  // coordinates, and a 3-axis shop must not be handed an axis it lacks.
  if (EmitsRotary(setup))
    for (Move& m : tp.moves) {
      m.hasA = true;
      m.a = setup.rotationA;
    }
  return tp;
}

std::vector<Toolpath> GenerateAtIndices(
    const Setup& setup, const Operation& op, const std::vector<Body>& bodies,
    const GenerateAtOneIndex& one, const std::vector<SketchCurves>* curves) {
  const auto refuse = [&](std::string why) {
    Toolpath t;
    t.operationId = op.id;
    t.toolNumber = op.toolNumber;
    t.error = std::move(why);
    return std::vector<Toolpath>{std::move(t)};
  };
  if (!setup.machine.hasRotaryAxis)
    return refuse("index_a_deg turns the part between cuts, and " +
                  (setup.machine.name.empty() ? std::string("this machine")
                                              : setup.machine.name) +
                  " has no rotary axis (has_rotary_axis)");
  Operation still = op;
  still.indexA.clear();
  // A floor is cut at the index that turns it to the spindle, and at no
  // other: the rook's four crenel floors are one pocket's four targets, and
  // at A90 three of them face sideways or down. Other targets -- a wall, a
  // bore -- are followed at every index.
  // An engraving's sketch plane is the same kind of thing as a floor: it is
  // cut at the index that faces it to the spindle
  // (docs/cam/operations.md#engrave).
  const bool engraving = op.type == OpType::Engrave;
  const bool floors =
      !op.targets.empty() &&
      (OpTypeHas(op.type, OpField::NeedsFlatFloor) || engraving);
  std::vector<bool> presented(op.targets.size(), false);
  std::vector<Toolpath> out;
  for (const double a : op.indexA) {
    Setup turned = setup;
    turned.rotationA = a;
    const std::vector<Body> framed = BodiesInSetupFrame(turned, bodies);
    Operation here = still;
    if (floors) {
      here.targets.clear();
      for (size_t i = 0; i < op.targets.size(); i++) {
        glm::dvec3 n(0.0);
        const SketchCurves* c = engraving && curves != nullptr
                                    ? FindSketchCurves(*curves, op.targets[i])
                                    : nullptr;
        const bool faces = engraving
                               ? c != nullptr && EngraveFrameOf(turned, *c).ok
                               : FaceNormalOf(framed, op.targets[i].ref, n,
                                              op.targets[i].jobPartId) &&
                                     n.z > 0.99;
        if (faces) {
          here.targets.push_back(op.targets[i]);
          presented[i] = true;
        }
      }
      if (here.targets.empty())
        continue; // nothing faces the spindle at this index
    }
    std::vector<Toolpath> pieces = one(turned, here, framed);
    if (out.empty())
      for (const Toolpath& p : pieces) {
        Toolpath t;
        t.operationId = p.operationId;
        t.toolNumber = p.toolNumber;
        t.finish = p.finish;
        out.push_back(std::move(t));
      }
    const std::string where = "at A" + Round2(a);
    for (size_t i = 0; i < pieces.size() && i < out.size(); i++) {
      Toolpath& tp = out[i];
      Toolpath& piece = pieces[i];
      if (!piece.error.empty()) {
        tp.skipped.push_back(where + ": " + piece.error);
        continue;
      }
      for (const std::string& s : piece.skipped)
        tp.skipped.push_back(where + ": " + s);
      tp.pending = tp.pending || piece.pending;
      if (tp.strategy.empty())
        tp.strategy = piece.strategy;
      tp.clippedMm += piece.clippedMm;
      tp.holdingClippedMm += piece.holdingClippedMm;
      // Stamped on EVERY move, A0 included: GenerateFramed stamps only a
      // non-zero index (a 3-axis shop is never handed an axis it lacks), and
      // here the machine has one and is told where each piece is cut.
      for (Move& m : piece.moves) {
        m.hasA = true;
        m.a = a;
      }
      // The join: up to safe Z over where the next piece starts, turning to
      // its angle -- a rapid that turns A, which the post retracts before.
      if (!tp.moves.empty() && !piece.moves.empty()) {
        Move turn;
        turn.kind = Move::Kind::Rapid;
        turn.to = glm::dvec3(piece.moves.front().to.x, piece.moves.front().to.y,
                             setup.safeZ);
        turn.hasA = true;
        turn.a = a;
        tp.moves.push_back(turn);
      }
      tp.moves.insert(tp.moves.end(), piece.moves.begin(), piece.moves.end());
      // Reliefs, tabs and grooves are not carried: each is a place in ONE
      // angle's frame, and a file that turns is checked by the rotary
      // inspection, which reads none of them
      // (docs/cam/rotary.md#verifying-a-turning-part).
    }
  }
  if (out.empty())
    return refuse("none of this operation's floors faces the spindle at any "
                  "of its indices");
  for (size_t i = 0; floors && i < op.targets.size(); i++)
    if (!presented[i])
      for (Toolpath& tp : out)
        tp.skipped.push_back("target " + std::to_string(i + 1) + " of " +
                             std::to_string(op.targets.size()) + " (" +
                             op.targets[i].ref.name +
                             "): faces the spindle at none of the indices");
  for (Toolpath& tp : out)
    if (tp.moves.empty() && !tp.pending && !tp.skipped.empty())
      tp.error = "this operation could be cut at none of its indices. " +
                 tp.skipped.front();
  return out;
}

Toolpath Generate(const Setup& setup, const Operation& op,
                  const std::vector<Body>& bodies, GenerateContext* ctx) {
  const SectionMemoScope sections;
  if (CutAtIndices(op))
    return GenerateAtIndices(
               setup, op, bodies,
               [ctx](const Setup& s, const Operation& o,
                     const std::vector<Body>& framed) {
                 return std::vector<Toolpath>{
                     GenerateFramed(s, o, framed, ctx)};
               },
               ctx != nullptr ? &ctx->sketchCurves : nullptr)
        .front();
  return GenerateFramed(setup, op, BodiesInSetupFrame(setup, bodies), ctx);
}

} // namespace cam
