// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeasureInsertion.h"

#include "cad/NonDestructiveBoolean.h"
#include "common/Logger.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/MeasureSections.h"
#include "core/ThreadStandards.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <unordered_map>

namespace Measure {

namespace {

// The clash check's fuzz (Measure.cpp's kContactFuzz): faces that only
// touch share a face, and an unfuzzed boolean over them returns a sliver.
constexpr double kFuzz = 1.0e-3;
// An overlap below this is the boolean's noise, not matter in matter.
constexpr double kVolumeTol = 1.0e-9;
// The distance query's noise on analytic faces, mm.
constexpr double kGapTol = 1.0e-6;

gp_Trsf Rigid(const glm::dmat4& m) {
  const glm::dquat q = glm::normalize(glm::quat_cast(glm::dmat3(m)));
  gp_Trsf t;
  t.SetRotation(gp_Quaternion(q.x, q.y, q.z, q.w));
  t.SetTranslationPart(gp_Vec(m[3][0], m[3][1], m[3][2]));
  return t;
}

double VolumeOf(const TopoDS_Shape& s) {
  GProp_GProps p;
  BRepGProp::VolumeProperties(s, p);
  return std::max(0.0, p.Mass());
}

struct Reading {
  bool interfering = false;
  double depthMm = 0.0;   // allowance - gap; the gap negative when overlapped
  double volumeMm3 = 0.0; // the model's own overlap
};

struct KernelError {
  std::string what;
};

gp_Trsf Pose(double u, double rotationDeg, bool positive) {
  gp_Trsf rot;
  rot.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                  rotationDeg * std::numbers::pi / 180.0);
  gp_Trsf tr;
  tr.SetTranslation(gp_Vec(0.0, 0.0, positive ? u : -u));
  return tr * rot;
}

namespace C = Clipper2Lib;
using sections::Band;
using sections::kScale;

// The polygons' own noise on a gap: chords stand in for curves, mm.
constexpr double kGap2D = 1.0e-3;
// Two band ends nearer than this along the axis meet, not overlap, mm.
constexpr double kZTol = 1.0e-6;

int64_t Units(double mm) {
  return static_cast<int64_t>(std::ceil(mm * kScale));
}

C::Rect64 Grown(C::Rect64 r, int64_t g) {
  r.left -= g;
  r.top -= g;
  r.right += g;
  r.bottom += g;
  return r;
}

bool Meet(const C::Rect64& a, const C::Rect64& b) {
  return a.left <= b.right && b.left <= a.right && a.top <= b.bottom &&
         b.top <= a.bottom;
}

// A moving band's region turned to one orientation, and grown by the
// allowance (less the polygons' noise) for the gap test.
struct Turned {
  C::Paths64 region, grown;
  C::Rect64 box{}, grownBox{};
};

struct PairReading {
  bool interfering = false;
  double depthMm = 0.0; // as Reading's
  double areaMm2 = 0.0; // the regions' overlap in the model
};

// The thickest piece of an overlap: twice its area over its perimeter
// (holes included), the 2D twin of the clash boolean's skin thickness.
void Overlap(const C::PolyPath64& node, double& area, double& thick) {
  for (const auto& outer : node) {
    double a = C::Area(outer->Polygon());
    double len = C::Length(outer->Polygon(), true);
    for (const auto& hole : *outer) {
      a += C::Area(hole->Polygon());
      len += C::Length(hole->Polygon(), true);
      Overlap(*hole, area, thick); // islands in the holes
    }
    a = std::abs(a);
    area += a;
    if (len > 0.0)
      thick = std::max(thick, 2.0 * a / len);
  }
}

PairReading ReadPair(const Band& a, const Turned& at, const Band& b,
                     double allowance) {
  PairReading r;
  const double reach = allowance + kFuzz;
  if (a.rMax + reach < b.rMin || b.rMax + reach < a.rMin)
    return r;
  if (!Meet(Grown(at.box, Units(reach)), b.box))
    return r;
  C::Clipper64 clip;
  clip.AddSubject(at.region);
  clip.AddClip(b.region);
  C::PolyTree64 tree;
  clip.Execute(C::ClipType::Intersection, C::FillRule::NonZero, tree);
  double area = 0.0, thick = 0.0;
  Overlap(tree, area, thick);
  area /= kScale * kScale;
  thick /= kScale;
  // A mesh's slice is off the surface by up to its deflection, each side:
  // an overlap no thicker than that is the meshes' noise.
  if (thick > std::max(kFuzz, a.noiseMm + b.noiseMm)) {
    r.interfering = true;
    r.depthMm = allowance + thick;
    r.areaMm2 = area;
    return r;
  }
  if (!(allowance > 0.0))
    return r;
  if (area > 0.0) {
    // Faces on faces: no gap at all.
    r.interfering = true;
    r.depthMm = allowance;
    return r;
  }
  if (!Meet(at.grownBox, b.box))
    return r;
  const C::Paths64 lens =
      C::Intersect(at.grown, b.region, C::FillRule::NonZero);
  if (lens.empty())
    return r;
  const double gap =
      sections::BoundaryGap(at.region, b.region, C::GetBounds(lens), allowance);
  // A gap exactly the allowance is line to line as made, not a press: the
  // polygons' noise is kept on the free side of it.
  if (gap < allowance - kGap2D) {
    r.interfering = true;
    r.depthMm = allowance - gap;
  }
  return r;
}

// A union of open intervals of the slide, measured.
struct Intervals {
  std::vector<std::pair<double, double>> v;
  void Add(double lo, double hi) {
    if (hi > lo)
      v.emplace_back(lo, hi);
  }
  std::vector<std::pair<double, double>> Merged() const {
    auto s = v;
    std::sort(s.begin(), s.end());
    std::vector<std::pair<double, double>> out;
    for (const auto& [lo, hi] : s)
      if (!out.empty() && lo <= out.back().second)
        out.back().second = std::max(out.back().second, hi);
      else
        out.emplace_back(lo, hi);
    return out;
  }
  double Length() const {
    double n = 0.0;
    for (const auto& [lo, hi] : Merged())
      n += hi - lo;
    return n;
  }
  double Sup() const {
    double s = 0.0;
    for (const auto& [lo, hi] : v)
      s = std::max(s, hi);
    return s;
  }
  bool Covers(double lo, double hi) const {
    for (const auto& [a, b] : Merged())
      if (a <= lo && b >= hi)
        return true;
    return false;
  }
};

// The placed solids of a side, carried into the axis frame.
std::vector<TopoDS_Shape> Solids(const std::vector<Body>& bodies,
                                 const glm::dmat4& xf, const gp_Trsf& toAxis,
                                 std::vector<int>* bodyOf = nullptr) {
  std::vector<TopoDS_Shape> out;
  const TopLoc_Location loc(toAxis * Rigid(xf));
  for (size_t i = 0; i < bodies.size(); i++) {
    const Body& b = bodies[i];
    if (b.reference || b.shape.GetShape().IsNull())
      continue;
    const TopoDS_Shape placed = b.shape.GetShape().Moved(loc);
    for (TopExp_Explorer e(placed, TopAbs_SOLID); e.More(); e.Next()) {
      out.push_back(e.Current());
      if (bodyOf)
        bodyOf->push_back(static_cast<int>(i));
    }
  }
  return out;
}

TopoDS_Compound Compound(const std::vector<TopoDS_Shape>& shapes) {
  BRep_Builder bb;
  TopoDS_Compound c;
  bb.MakeCompound(c);
  for (const TopoDS_Shape& s : shapes)
    bb.Add(c, s);
  return c;
}

// A thread of a side, carried into the axis frame, when it lies on the
// axis (a screw's along its path): its span along the axis.
struct AxisThread {
  std::string designation;
  double majorDia = 0.0, pitch = 0.0;
  bool external = false;
  double lo = 0.0, hi = 0.0;
};

std::vector<AxisThread> AxisThreads(const std::vector<Body>& bodies,
                                    const glm::dmat4& xf,
                                    const gp_Trsf& toAxis) {
  std::vector<AxisThread> out;
  const gp_Trsf place = toAxis * Rigid(xf);
  // On the axis: along it, and its line within a tenth of a millimetre.
  const auto onAxis = [](const gp_Ax1& ax) {
    return std::abs(ax.Direction().Z()) > 1.0 - 1e-6 &&
           std::hypot(ax.Location().X(), ax.Location().Y()) < 0.1;
  };
  for (const Body& b : bodies) {
    if (b.reference || b.shape.GetShape().IsNull())
      continue;
    for (const ThreadSpan& t : ThreadSpansOf(b, xf)) {
      gp_Pnt p(t.start.x, t.start.y, t.start.z);
      gp_Dir d(t.axis.x, t.axis.y, t.axis.z);
      p.Transform(toAxis);
      d.Transform(toAxis);
      if (!onAxis(gp_Ax1(p, d)))
        continue;
      const double z1 = p.Z() + d.Z() * t.length;
      out.push_back({t.spec.designation, t.spec.majorDia, t.spec.pitch,
                     t.external, std::min(p.Z(), z1), std::max(p.Z(), z1)});
    }
    if (b.threads.empty())
      continue;
    const TopoDS_Shape placed =
        b.shape.GetShape().Moved(TopLoc_Location(place));
    std::vector<TopoDS_Shape> order;
    for (TopExp_Explorer e(placed, TopAbs_FACE); e.More(); e.Next())
      order.push_back(e.Current());
    for (const ThreadMark& m : b.threads) {
      const auto index = b.elementMap.Resolve(m.face.name);
      if (!index || *index < 0 || *index >= static_cast<int>(order.size()))
        continue;
      BRepAdaptor_Surface surface(TopoDS::Face(order[*index]));
      if (surface.GetType() != GeomAbs_Cylinder)
        continue;
      const gp_Cylinder cyl = surface.Cylinder();
      if (!onAxis(cyl.Axis()))
        continue;
      double v0 = surface.FirstVParameter(), v1 = surface.LastVParameter();
      if (v0 > v1)
        std::swap(v0, v1);
      if (m.length > 0.0)
        v1 = std::min(v1, v0 + m.length);
      const double dz = cyl.Axis().Direction().Z();
      const double z0 = cyl.Location().Z() + dz * v0,
                   z1 = cyl.Location().Z() + dz * v1;
      out.push_back({m.designation,
                     m.majorDia > 0.0 ? m.majorDia : 2.0 * cyl.Radius(),
                     m.pitch, m.external, std::min(z0, z1), std::max(z0, z1)});
    }
  }
  return out;
}

} // namespace

std::expected<InsertionReport, std::string>
Insertion(const std::vector<Body>& moving, const glm::dmat4& xfMoving,
          const std::vector<Body>& fixed, const glm::dmat4& xfFixed,
          const glm::dvec3& axisPoint, const glm::dvec3& axisDir,
          const InsertionOptions& opt, const std::atomic<bool>* cancel,
          std::atomic<double>* fraction) {
  const double len = glm::length(axisDir);
  if (!(len > 1e-12))
    return std::unexpected("the axis has no direction");
  if (opt.samples < 2)
    return std::unexpected("samples is at least 2");
  if (opt.rotations < 1)
    return std::unexpected("rotations is at least 1");
  if (opt.allowanceMm < 0.0)
    return std::unexpected("allowance_mm is not negative: it is how much "
                           "nearer the faces come as made");
  // A thread goes on by turning along its helix, which a straight slide
  // reads as the flanks forced through each other the whole way.
  // Only the fixed side's bodies within reach of the path count (below):
  // a sequence's fixed side is everything placed before, threads and all.
  const auto threadedBody = [](const Body& b) {
    return !b.reference && (!b.threads.empty() || !b.threadRegions.empty());
  };
  const bool movingThreaded =
      std::any_of(moving.begin(), moving.end(), threadedBody);
  InsertionReport rep;
  rep.axisPoint = axisPoint;
  rep.axisDir = axisDir / len;
  rep.allowanceMm = opt.allowanceMm;
  rep.samples = opt.samples;
  const auto progress = [&](double f) {
    if (fraction)
      fraction->store(std::min(0.99, f));
  };
  try {
    const gp_Ax3 ax(gp_Pnt(axisPoint.x, axisPoint.y, axisPoint.z),
                    gp_Dir(rep.axisDir.x, rep.axisDir.y, rep.axisDir.z));
    gp_Trsf toAxis;
    toAxis.SetTransformation(ax);
    std::vector<int> bodyOfA, bodyOfB;
    const std::vector<TopoDS_Shape> solidsA = Solids(moving, xfMoving, toAxis,
                                                     &bodyOfA),
                                    solidsB = Solids(fixed, xfFixed, toAxis,
                                                     &bodyOfB);
    if (solidsA.empty() || solidsB.empty())
      return std::unexpected("one side places no visible matter (hidden, or "
                             "a part with no bodies)");
    // Each side kept, once, to what the other can ever come near: as wide
    // about the axis as the other reaches from it (and a millimetre more
    // than the allowance), which a turn about the axis keeps.
    const auto reachOf = [](const std::vector<sections::Solid>& v) {
      double r = 0.0;
      for (const auto& s : v)
        r = std::max(r, std::hypot(s.half, s.half));
      return r;
    };
    const double g = opt.allowanceMm + 1.0;
    std::vector<sections::Solid> prepA, prepB;
    for (const TopoDS_Shape& s : solidsA)
      prepA.push_back(sections::Prepare(s));
    const double reachB = reachOf(prepA) + g;
    // A fixed solid whose box lies wholly outside that reach never comes
    // near: an assembly sequence's later steps have everything placed
    // before them on the fixed side, most of it nowhere near the path.
    std::vector<int> keptBody;  // prepB's bodies, in `fixed`
    std::vector<size_t> keptAt; // prepB's solids, in solidsB
    for (size_t k = 0; k < solidsB.size(); k++) {
      const TopoDS_Shape& s = solidsB[k];
      Bnd_Box box;
      BRepBndLib::Add(s, box, Standard_True);
      if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        const double dx = x0 > 0.0 ? x0 : (x1 < 0.0 ? -x1 : 0.0);
        const double dy = y0 > 0.0 ? y0 : (y1 < 0.0 ? -y1 : 0.0);
        if (std::hypot(dx, dy) > reachB)
          continue;
      }
      prepB.push_back(sections::Prepare(s));
      keptBody.push_back(bodyOfB[k]);
      keptAt.push_back(k);
    }
    const double reachA = reachOf(prepB) + g;
    // A thread on the path: its face's box about the axis (a mark that does
    // not resolve to a face counts, as it cannot be placed).
    const gp_Trsf placeA = toAxis * Rigid(xfMoving),
                  placeB = toAxis * Rigid(xfFixed);
    const auto threadOnPath = [&](const Body& b, const gp_Trsf& place) {
      if (b.reference)
        return false;
      if (!b.threadRegions.empty())
        return true;
      std::vector<TopoDS_Shape> order;
      for (TopExp_Explorer e(b.shape.GetShape().Moved(TopLoc_Location(place)),
                             TopAbs_FACE);
           e.More(); e.Next())
        order.push_back(e.Current());
      for (const ThreadMark& m : b.threads) {
        const auto index = b.elementMap.Resolve(m.face.name);
        Bnd_Box box;
        if (index && *index >= 0 && *index < static_cast<int>(order.size())) {
          BRepBndLib::Add(order[*index], box, Standard_False);
        } else {
          // A modelled thread took its face: it is where the body's
          // free-form (helical) faces are.
          for (const TopoDS_Shape& f : order)
            switch (BRepAdaptor_Surface(TopoDS::Face(f)).GetType()) {
            case GeomAbs_Plane:
            case GeomAbs_Cylinder:
            case GeomAbs_Cone:
            case GeomAbs_Sphere:
            case GeomAbs_Torus:
              break;
            default:
              BRepBndLib::Add(f, box, Standard_False);
            }
        }
        if (box.IsVoid())
          return true;
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        const double r = 0.25 * std::max(x1 - x0, y1 - y0);
        if (std::hypot(0.5 * (x0 + x1), 0.5 * (y0 + y1)) <= std::max(1.0, r))
          return true;
      }
      return false;
    };
    if (movingThreaded && !opt.screw &&
        std::any_of(moving.begin(), moving.end(),
                    [&](const Body& b) { return threadOnPath(b, placeA); }) &&
        std::any_of(keptBody.begin(), keptBody.end(),
                    [&](int b) { return threadOnPath(fixed[b], placeB); }))
      return std::unexpected(
          "both sides carry modelled threads: a threaded pair goes on by "
          "turning, which a straight slide does not judge");
    const auto spanOf = [](const std::vector<sections::Solid>& v, double reach,
                           double& lo, double& hi) {
      lo = std::numeric_limits<double>::infinity();
      hi = -lo;
      for (const auto& s : v) {
        double a, b;
        if (sections::ReachSpan(s, reach, a, b)) {
          lo = std::min(lo, a);
          hi = std::max(hi, b);
        }
      }
      return hi > lo;
    };
    double za0, za1, zb0, zb1;
    const bool nearA = spanOf(prepA, reachA, za0, za1),
               nearB = spanOf(prepB, reachB, zb0, zb1);
    const auto clear = [&] {
      // Nothing of either ever comes near the other.
      InsertionTry t;
      t.pass = true;
      rep.best = t;
      rep.tries.push_back(t);
      rep.pass = true;
      if (fraction)
        fraction->store(1.0);
      return rep;
    };
    if (!nearA || !nearB)
      return clear();

    // Going on by its thread: each thread of the moving side meeting one of
    // the fixed side on the path is that pair's own fit to judge.
    struct Engaged {
      double aLo, aHi, bLo, bHi;
    };
    std::vector<Engaged> engaged;
    double screwPitch = 0.0; // a modelled thread's: it turns as it goes
    bool screwRight = true;
    std::string screwName;
    const auto screwExternal = [](const std::vector<Body>& side) {
      for (const Body& b : side)
        for (const ThreadMark& m : b.threads)
          if (m.modeled)
            return m.external;
      return true;
    };
    if (opt.screw) {
      const auto ta = AxisThreads(moving, xfMoving, toAxis),
                 tb = AxisThreads(fixed, xfFixed, toAxis);
      for (const AxisThread& x : ta)
        for (const AxisThread& y : tb) {
          const double over = std::min(x.hi, y.hi) - std::max(x.lo, y.lo);
          if (over <= kZTol)
            continue;
          InsertionThread it;
          it.moving = x.designation;
          it.fixed = y.designation;
          it.engagedMm = over;
          it.fits = x.external != y.external &&
                    std::abs(x.majorDia - y.majorDia) < 1e-6 &&
                    (x.pitch <= 0.0 || y.pitch <= 0.0 ||
                     std::abs(x.pitch - y.pitch) < 1e-9);
          rep.threads.push_back(it);
          if (it.fits)
            engaged.push_back({x.lo, x.hi, y.lo, y.hi});
        }
      // A modelled thread is its helix: the part turns at its pitch as it
      // goes, and the sections judge the flanks.
      if (rep.threads.empty()) {
        for (const Body& b : moving)
          for (const ThreadMark& m : b.threads)
            if (m.modeled && m.pitch > 0.0) {
              if (screwPitch > 0.0 && (std::abs(screwPitch - m.pitch) > 1e-9 ||
                                       screwRight != m.rightHand))
                return std::unexpected(
                    "it goes on by its thread, but it carries modelled "
                    "threads of more than one pitch or hand");
              screwPitch = m.pitch;
              screwRight = m.rightHand;
              screwName = m.designation;
            }
        if (screwPitch > 0.0) {
          InsertionThread it;
          it.moving = screwName.empty() ? "modelled" : screwName;
          it.fits = true;
          // The thread it goes into: a matching one within reach, else
          // the first that is there, which does not fit.
          bool matched = false;
          for (int k : keptBody)
            for (const ThreadMark& m : fixed[k].threads) {
              if (!m.modeled || m.external == screwExternal(moving) || matched)
                continue;
              const bool fits = std::abs(m.pitch - screwPitch) < 1e-9 &&
                                m.rightHand == screwRight;
              if (fits || it.fixed.empty()) {
                it.fixed = m.designation.empty() ? "modelled" : m.designation;
                it.fits = fits;
                matched = fits;
              }
            }
          rep.threads.push_back(it);
        }
      }
      if (rep.threads.empty())
        return std::unexpected(
            "it goes on by its thread, but no thread of the moving side "
            "meets one of the fixed side on the path (a Thread feature or "
            "a tapped Hole on each, coaxial with it, or a modelled thread on "
            "it)");
    }

    std::vector<bool> sides;
    if (opt.entry != InsertionEntry::Negative)
      sides.push_back(true);
    if (opt.entry != InsertionEntry::Positive)
      sides.push_back(false);
    // One resolution for every try, from the longer of the two travels.
    const auto travelOf = [&](bool positive) {
      if (opt.travelMm >= 0.0)
        return opt.travelMm;
      const double d = positive ? zb1 - za0 : za1 - zb0;
      return std::max(0.0, d + opt.allowanceMm + kFuzz);
    };
    double longest = 0.0;
    for (bool p : sides)
      longest = std::max(longest, travelOf(p));
    // A modelled thread is read finer: a slice short at a thread's end is
    // compared within a 24th of a turn of level.
    int samples = opt.samples;
    if (screwPitch > 0.0)
      samples =
          std::clamp(static_cast<int>(std::ceil(longest * 24.0 / screwPitch)),
                     opt.samples, 4000);
    rep.samples = samples;
    rep.stepMm = longest / samples;
    rep.leadInMm = opt.leadInMm >= 0.0 ? opt.leadInMm : rep.stepMm;
    // Slices no thinner than 20 um: a short travel (a valve that only
    // drops into its seat) would otherwise cut a block's fillets into a
    // quarter of a million. A turned path keeps its slices a step thick,
    // so every step lines the two sides' slices up.
    constexpr double kMinSlice = 0.02;
    rep.sliceMm = rep.stepMm <= 0.0 ? 1.0
                  : (opt.turnDeg != 0.0 || opt.screw)
                      ? rep.stepMm
                      : std::max(rep.stepMm, kMinSlice);
    // Kept between checks, the slices come from a ladder of thicknesses
    // (20 um doubled), no thicker than the step: two steps with nearly the
    // same travel then cut and reuse one set.
    const bool kept = opt.cache != nullptr && opt.turnDeg == 0.0 && !opt.screw;
    if (kept)
      rep.sliceMm =
          kMinSlice *
          std::exp2(std::floor(std::log2(rep.sliceMm / kMinSlice) + 1e-9));

    // The bands, both sides on one slice grid.
    using Clock = std::chrono::steady_clock;
    const auto since = [](Clock::time_point t) {
      return std::chrono::duration<double>(Clock::now() - t).count();
    };
    auto t0 = Clock::now();
    std::vector<Band> bandsA, bandsB;
    if (!kept) {
      const size_t n = prepA.size() + prepB.size();
      size_t k = 0;
      for (auto [prep, out, reach] : {std::tuple{&prepA, &bandsA, reachA},
                                      std::tuple{&prepB, &bandsB, reachB}}) {
        sections::BandOptions bo;
        bo.sliceMm = rep.sliceMm;
        bo.reachMm = reach;
        for (size_t i = 0; i < prep->size(); i++) {
          if (cancel && cancel->load())
            throw KernelError{"cancelled"};
          for (Band& b : sections::SolidBands((*prep)[i], bo,
                                              static_cast<int>(i), cancel))
            out->push_back(std::move(b));
          progress(0.5 * static_cast<double>(++k) / n);
        }
      }
    } else {
      // The direction's own frame: the axis through the world origin, along
      // the direction or its reverse, whichever points to the positive
      // side (the first component not zero) -- a direction and its reverse
      // share their cuts. The check's axis frame is that frame turned
      // (mirrored, when reversed) and shifted, so its bands are the ones
      // cut there carried over, and every solid's slices -- cut there or
      // carried -- lie on the one grid anchored at its origin.
      glm::dvec3 dir = rep.axisDir;
      for (int c = 0; c < 3; c++)
        if (std::abs(dir[c]) > 1e-9) {
          if (dir[c] < 0.0)
            dir = -dir;
          break;
        }
      gp_Trsf toDir;
      toDir.SetTransformation(
          gp_Ax3(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(dir.x, dir.y, dir.z)));
      const auto rotOf = [](const gp_Trsf& t) {
        glm::dmat3 m(1.0);
        for (int c = 0; c < 3; c++)
          for (int r = 0; r < 3; r++)
            m[c][r] = t.Value(r + 1, c + 1);
        return m;
      };
      // World into the direction's frame, and into the check's axis frame.
      const glm::dmat3 rot = rotOf(toDir), rotAxis = rotOf(toAxis);
      const gp_XYZ ta = toAxis.TranslationPart();
      const glm::dvec3 tAxis(ta.X(), ta.Y(), ta.Z());
      // The direction's frame into the axis frame: q = L q' + tAxis.
      const glm::dmat3 L = rotAxis * glm::transpose(rot);
      const double zSign = L[2][2] < 0.0 ? -1.0 : 1.0;
      const glm::dmat2 across(L[0][0], L[0][1], L[1][0], L[1][1]);
      const gp_Trsf axisToDir = toDir * toAxis.Inverted();
      const double h = rep.sliceMm = opt.cache->SliceFor(dir, rep.sliceMm);
      const auto cutInDir = [&](const TopoDS_Shape& solidInAxis, double lo,
                                double hi) {
        sections::BandOptions bo;
        bo.sliceMm = h;
        bo.zLo = lo;
        bo.zHi = hi;
        return sections::SolidBands(
            sections::Prepare(solidInAxis.Moved(TopLoc_Location(axisToDir))),
            bo, 0, cancel);
      };
      // What the moving side can come near along the axis, either way it
      // comes, a slice to spare: the window a fixed solid is cut over. In
      // the direction's frame, z' = zSign (z - tAxis.z).
      double wLo = za0 - h, wHi = za1 + h;
      for (bool p : sides)
        (p ? wHi : wLo) += (p ? 1.0 : -1.0) * travelOf(p);
      const auto toDirZ = [&](double z) { return zSign * (z - tAxis.z); };
      const double dLo = std::min(toDirZ(wLo), toDirZ(wHi)),
                   dHi = std::max(toDirZ(wLo), toDirZ(wHi));
      const size_t n = prepA.size() + prepB.size();
      size_t done = 0;
      const auto side = [&](const std::vector<sections::Solid>& prep,
                            const std::vector<uint64_t>& keys,
                            const std::vector<glm::dmat4>& places,
                            const glm::dmat4& xf, const auto& bodyOfSolid,
                            double reach, std::vector<Band>& out) {
        std::map<int, int> ordinal; // a body's solids, counted
        for (size_t i = 0; i < prep.size(); i++) {
          if (cancel && cancel->load())
            throw KernelError{"cancelled"};
          const int body = bodyOfSolid(i);
          const int nth = ordinal[body]++;
          const uint64_t key =
              static_cast<size_t>(body) < keys.size() && keys[body] != 0
                  ? (keys[body] << 12) | static_cast<uint64_t>(nth)
                  : 0;
          const glm::dmat4 place =
              xf * (static_cast<size_t>(body) < places.size()
                        ? places[body]
                        : glm::dmat4(1.0));
          std::vector<Band> inDir;
          // How it has moved since it was cut, in the direction's frame.
          glm::dmat2 turn(1.0);
          glm::dvec3 moved(0.0);
          bool have = false;
          // The solid's own span, the direction's frame: cut the window and
          // as much again either side, or whole when that covers it.
          double sLo = std::numeric_limits<double>::infinity(), sHi = -sLo;
          for (const sections::Solid::Face& f : prep[i].faces) {
            sLo = std::min({sLo, toDirZ(f.z0), toDirZ(f.z1)});
            sHi = std::max({sHi, toDirZ(f.z0), toDirZ(f.z1)});
          }
          const double wl = dHi - dLo;
          const bool whole = dLo - wl <= sLo && dHi + wl >= sHi;
          const double cutLo = whole ? -std::numeric_limits<double>::infinity()
                                     : dLo - wl,
                       cutHi = whole ? std::numeric_limits<double>::infinity()
                                     : dHi + wl;
          if (key != 0) {
            if (const auto* e = opt.cache->Find(key, dir, h)) {
              // Reusable when only turned about the axis and, with slices,
              // moved along it a whole number of them.
              const glm::dmat4 rel = place * glm::inverse(e->place);
              const glm::dmat3 r = rot * glm::dmat3(rel) * glm::transpose(rot);
              const glm::dvec3 t = rot * glm::dvec3(rel[3]);
              const bool aboutAxis = std::abs(r[2][2] - 1.0) < 1e-9 &&
                                     std::abs(r[0][2]) < 1e-9 &&
                                     std::abs(r[1][2]) < 1e-9;
              const bool sliced =
                  std::any_of(e->bands.begin(), e->bands.end(),
                              [](const Band& b) { return b.sliced; });
              const double k = t.z / h;
              // Its stretch, where it now is, holds this check's window.
              const bool covers =
                  e->zLo + t.z <= dLo + 1e-9 && e->zHi + t.z >= dHi - 1e-9;
              if (aboutAxis && covers &&
                  (!sliced || std::abs(k - std::round(k)) < 1e-6)) {
                turn = glm::dmat2(r[0][0], r[0][1], r[1][0], r[1][1]);
                moved = t;
                inDir = e->bands;
                have = true;
                opt.cache->stats.reused++;
                rep.solidsReused++;
              }
            }
          }
          if (!have) {
            sections::BandCache::Entry e;
            e.place = place;
            e.zLo = cutLo;
            e.zHi = cutHi;
            e.bands = cutInDir(prep[i].shape, cutLo, cutHi);
            if (cancel && cancel->load())
              throw KernelError{"cancelled"};
            inDir = e.bands;
            rep.solidsCut++;
            opt.cache->stats.cut++;
            if (key != 0)
              opt.cache->Put(key, dir, h, std::move(e));
          }
          // q_axis = L (turn q + moved) + tAxis.
          for (Band& b : sections::Moved(inDir, across * turn, zSign,
                                         L * moved + tAxis, reach)) {
            b.solid = static_cast<int>(i);
            out.push_back(std::move(b));
          }
          progress(0.5 * static_cast<double>(++done) / n);
        }
      };
      side(
          prepA, opt.movingKeys, opt.movingPlaces, xfMoving,
          [&](size_t i) { return bodyOfA[i]; }, reachA, bandsA);
      side(
          prepB, opt.fixedKeys, opt.fixedPlaces, xfFixed,
          [&](size_t i) { return bodyOfB[keptAt[i]]; }, reachB, bandsB);
    }
    rep.sectionsSeconds = since(t0);
    t0 = Clock::now();
    rep.bandsMoving = static_cast<int>(bandsA.size());
    rep.bandsFixed = static_cast<int>(bandsB.size());
    if (bandsA.empty() || bandsB.empty())
      return clear();

    // Orientations tried, and every band pair's reading at each: a pair's
    // reading does not depend on the slide, only on the turn.
    std::vector<double> turns;
    // A modelled thread turns a turn a pitch: right hand, anticlockwise
    // about the way it moves (clockwise seen from behind it).
    double turnDeg = opt.turnDeg;
    if (screwPitch > 0.0 && turnDeg == 0.0)
      turnDeg = (screwRight ? 1.0 : -1.0) * 360.0 * longest / screwPitch;
    const bool turning = turnDeg != 0.0;
    // A screw turns by its helix: a turn a pitch of the offset from the seat,
    // whichever side it comes from and however far -- so its steps are one
    // length on both sides (the longer travel's), and from the other side
    // it turns the other way about the axis. A bayonet's `turnDeg` is over
    // each try's own travel.
    const bool helix = screwPitch > 0.0 && opt.turnDeg == 0.0;
    const size_t orientations = turning ? 1 : opt.rotations;
    for (size_t k = 0; k < orientations; k++)
      turns.push_back(360.0 * k / opt.rotations);
    // A turned path's orientation at each of its steps, after the seat's;
    // a helix's from the other side after those.
    if (turning)
      for (int k = 1; k <= samples; k++)
        turns.push_back(turnDeg * k / samples);
    if (helix)
      for (int k = 1; k <= samples; k++)
        turns.push_back(-turnDeg * k / samples);
    const auto turnAt = [&](bool positive, int k) -> size_t {
      return static_cast<size_t>(helix && !positive && k > 0 ? samples + k : k);
    };
    const size_t nA = bandsA.size(), nB = bandsB.size();
    // A moving band's outline at an orientation, made when first asked:
    // the seat's orientation is kept, and one other at a time -- the tries
    // and a turned path's steps go through them in order, and a part
    // turned 64 ways at once is gigabytes on a long free-form solid.
    std::map<size_t, std::vector<std::optional<Turned>>> turned;
    std::vector<std::optional<C::Paths64>> grownAt0(nA);
    const auto turnedAt = [&](size_t ti, size_t i) -> const Turned& {
      auto it = turned.find(ti);
      if (it == turned.end()) {
        for (auto e = turned.begin(); e != turned.end();)
          e = e->first != 0 ? turned.erase(e) : std::next(e);
        it = turned.emplace(ti, std::vector<std::optional<Turned>>(nA)).first;
      }
      std::optional<Turned>& slot = it->second[i];
      if (!slot) {
        Turned t;
        t.region = sections::Rotated(bandsA[i].region, turns[ti]);
        t.box = C::GetBounds(t.region);
        if (opt.allowanceMm > kGap2D) {
          // An offset turns with what it offsets: grown once, turned after.
          std::optional<C::Paths64>& g = grownAt0[i];
          if (!g)
            g = C::InflatePaths(bandsA[i].region,
                                (opt.allowanceMm - kGap2D) * kScale,
                                C::JoinType::Round, C::EndType::Polygon, 2.0,
                                0.25 * kGap2D * kScale);
          t.grown = sections::Rotated(*g, turns[ti]);
          t.grownBox = C::GetBounds(t.grown);
        }
        slot = std::move(t);
      }
      return *slot;
    };
    // Every pair read, by orientation: a reading does not depend on the
    // slide. Bounded, so a check that would read past it is refused rather
    // than left to take the machine's memory.
    constexpr size_t kPairCeiling = 4000000;
    std::unordered_map<uint64_t, PairReading> readings;
    // The slides at which a pair's reading holds: where their spans
    // overlap, an open interval from the band ends -- exact for prismatic
    // bands. Two slices compare two sections that stand for each other only
    // near where they line up, so a slice against a slice holds within half
    // a slice of that (the slices of the two sides tile the slide between
    // them); `centre` is where they line up.
    struct Window {
      double lo, hi, centre;
    };
    const auto windowOf = [](const Band& a, const Band& b, bool positive) {
      Window w;
      const double ma = a.zSection, mb = b.zSection;
      if (positive) {
        w.lo = b.z0 - a.z1;
        w.hi = b.z1 - a.z0;
        w.centre = mb - ma;
      } else {
        w.lo = a.z0 - b.z1;
        w.hi = a.z1 - b.z0;
        w.centre = ma - mb;
      }
      if (a.sliced && b.sliced) {
        const double half = 0.5 * std::max(a.z1 - a.z0, b.z1 - b.z0);
        w.lo = std::max(w.lo, w.centre - half);
        w.hi = std::min(w.hi, w.centre + half);
      }
      return w;
    };
    // A band of the screw's thread against one of the thread it goes into:
    // judged by the threads' fit, not read as interference.
    const auto ByThread = [&](const Band& a, const Band& b) {
      return std::any_of(engaged.begin(), engaged.end(), [&](const Engaged& e) {
        return a.z0 >= e.aLo - kZTol && a.z1 <= e.aHi + kZTol &&
               b.z0 >= e.bLo - kZTol && b.z1 <= e.bHi + kZTol;
      });
    };
    const auto pair = [&](size_t ti, size_t i, size_t j) -> const PairReading& {
      const uint64_t k = (static_cast<uint64_t>(ti) * nA + i) * nB + j;
      auto it = readings.find(k);
      if (it == readings.end()) {
        if (readings.size() >= kPairCeiling)
          throw KernelError{
              "the check would read more than " + std::to_string(kPairCeiling) +
              " pairs of sections: give it a coarser resolution (fewer "
              "samples, fewer rotations)"};
        it = readings
                 .emplace(k, ByThread(bandsA[i], bandsB[j])
                                 ? PairReading{}
                                 : ReadPair(bandsA[i], turnedAt(ti, i),
                                            bandsB[j], opt.allowanceMm))
                 .first;
        rep.pairsRead++;
      }
      return it->second;
    };

    // The seat, at each orientation: what interferes where the part ends
    // up (the same whichever side it came from), how deep, how much, and
    // over what length of the axis.
    struct Seat {
      bool done = false;
      Reading at;
      double pressMm = 0.0;
    };
    std::vector<Seat> seats(turns.size());
    const auto seatAt = [&](size_t ti) -> const Seat& {
      Seat& s = seats[ti];
      if (s.done)
        return s;
      s.done = true;
      Intervals press;
      for (size_t i = 0; i < nA; i++)
        for (size_t j = 0; j < nB; j++) {
          const Band &a = bandsA[i], &b = bandsB[j];
          const double lo = std::max(a.z0, b.z0), hi = std::min(a.z1, b.z1);
          if (hi - lo <= kZTol)
            continue; // apart, or a face on a face: a contact, not a press
          const Window w = windowOf(a, b, true);
          if (!(w.lo < 0.0 && w.hi > 0.0))
            continue; // two slices that line up elsewhere
          const PairReading& r = pair(ti, i, j);
          if (!r.interfering)
            continue;
          s.at.interfering = true;
          s.at.depthMm = std::max(s.at.depthMm, r.depthMm);
          s.at.volumeMm3 += r.areaMm2 * (hi - lo);
          press.Add(lo, hi);
        }
      s.pressMm = press.Length();
      return s;
    };

    const double perTry = 1.0;
    const double total =
        perTry * static_cast<double>(sides.size() * orientations);
    double done = 0.0;
    const auto advance = [&](double n) {
      done += n;
      progress(0.5 + 0.5 * done / total);
    };

    bool haveBest = false;
    const auto run = [&](size_t side, size_t ti) {
      const bool positive = sides[side];
      InsertionTry t;
      t.positive = positive;
      t.rotationDeg = turns[ti];
      t.travelMm = travelOf(positive);
      if (rep.stepMm <= 0.0 || t.travelMm <= 0.0) {
        t.pass = true;
        return t;
      }
      const Seat& seat0 = seatAt(0);
      if (ti != 0) {
        // Turned so, does it still seat: interfere where it ends up no
        // more than the placed part does?
        const Seat& s = seatAt(ti);
        if (s.at.depthMm > seat0.at.depthMm + kFuzz ||
            s.at.volumeMm3 > seat0.at.volumeMm3 * (1.0 + 1e-3) + 1e-3) {
          t.seats = false;
          t.worstDepthMm = s.at.depthMm;
          t.worstVolumeMm3 = s.at.volumeMm3;
          return t;
        }
      }
      t.pressMm = seat0.pressMm;
      // Forced deeper than the seat is pressed: a bind, however short.
      const double deeperTol = 0.5 * opt.allowanceMm + kFuzz;
      const double seatDepth = seat0.at.depthMm;
      Intervals hit, deeper;
      double worstAt = -1.0;
      if (turning) {
        // Turned as it goes: the orientation changes along the path, so
        // the path is read step by step, each step counted a step long.
        const double step = helix ? rep.stepMm : t.travelMm / samples;
        const double sgn = positive ? 1.0 : -1.0;
        const int last =
            helix ? std::min(samples, static_cast<int>(
                                          std::ceil(t.travelMm / step - 1e-9)))
                  : samples;
        for (int k = last; k >= 1; k--) {
          if (cancel && cancel->load())
            throw KernelError{"cancelled"};
          const double u = step * k;
          bool any = false, deep = false;
          for (size_t i = 0; i < nA; i++)
            for (size_t j = 0; j < nB; j++) {
              const Band &a = bandsA[i], &b = bandsB[j];
              const double lo = std::max(a.z0 + sgn * u, b.z0),
                           hi = std::min(a.z1 + sgn * u, b.z1);
              if (hi - lo <= kZTol)
                continue;
              const PairReading& r = pair(turnAt(positive, k), i, j);
              if (!r.interfering)
                continue;
              any = true;
              deep = deep || r.depthMm > seatDepth + deeperTol;
              if (r.depthMm > t.worstDepthMm) {
                t.worstDepthMm = r.depthMm;
                worstAt = u;
                t.worstFixedBody = keptBody[b.solid];
              }
              t.worstVolumeMm3 =
                  std::max(t.worstVolumeMm3, r.areaMm2 * (hi - lo));
            }
          if (any)
            hit.Add(u - step, u);
          if (deep)
            deeper.Add(u - step, u);
        }
      } else {
        // Every pair whose spans overlap somewhere on the way: the slides
        // that do it are one open interval, from the band ends.
        struct Cand {
          size_t i, j;
          double lo, hi, centre;
        };
        std::vector<Cand> cands;
        for (size_t i = 0; i < nA; i++)
          for (size_t j = 0; j < nB; j++) {
            const Window w = windowOf(bandsA[i], bandsB[j], positive);
            const double lo = std::max(w.lo, 0.0),
                         hi = std::min(w.hi, t.travelMm);
            if (hi - lo > kZTol)
              cands.push_back({i, j, lo, hi, w.centre});
          }
        // Nearest the seat first: what binds as it goes home is read first.
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& x, const Cand& y) { return x.lo < y.lo; });
        for (const Cand& c : cands) {
          if (cancel && cancel->load())
            throw KernelError{"cancelled"};
          // Nothing left to learn from a pair whose slides are already
          // counted both as interfering and as deeper than the seat.
          if (hit.Covers(c.lo, c.hi) && deeper.Covers(c.lo, c.hi))
            continue;
          const PairReading& r = pair(ti, c.i, c.j);
          if (!r.interfering)
            continue;
          hit.Add(c.lo, c.hi);
          if (r.depthMm > seatDepth + deeperTol)
            deeper.Add(c.lo, c.hi);
          if (r.depthMm > t.worstDepthMm) {
            t.worstDepthMm = r.depthMm;
            // Read where the two line up: a slice pair's own offset.
            worstAt = bandsA[c.i].sliced && bandsB[c.j].sliced
                          ? std::clamp(c.centre, c.lo, c.hi)
                          : 0.5 * (c.lo + c.hi);
            t.worstFixedBody = keptBody[bandsB[c.j].solid];
          }
          const Band &a = bandsA[c.i], &b = bandsB[c.j];
          t.worstVolumeMm3 = std::max(
              t.worstVolumeMm3, r.areaMm2 * std::min(a.z1 - a.z0, b.z1 - b.z0));
          // A try that cannot beat the best found is abandoned.
          if (haveBest && hit.Length() - t.pressMm > rep.best.excessMm) {
            t.complete = false;
            break;
          }
        }
      }
      t.interferingMm = hit.Length();
      t.deeperMm = deeper.Length();
      t.fromMm = hit.Sup();
      t.worstAtMm = worstAt;
      t.excessMm = std::max(0.0, t.interferingMm - t.pressMm);
      t.pass = t.complete && t.excessMm <= rep.leadInMm + 1e-9 &&
               t.deeperMm <= rep.leadInMm + 1e-9;
      return t;
    };
    const auto consider = [&](const InsertionTry& t) {
      rep.tries.push_back(t);
      if (t.seats && t.complete &&
          (!haveBest || t.excessMm < rep.best.excessMm)) {
        rep.best = t;
        haveBest = true;
      }
      if (t.pass) {
        rep.pass = true;
        rep.best = t;
      }
      advance(perTry);
    };
    // Turned by `deg`, is either side the same matter again?
    const auto symmetric = [&](double deg) {
      for (const std::vector<Band>* side : {&bandsA, &bandsB}) {
        bool same = true;
        for (const Band& b : *side) {
          const C::Paths64 x = C::Xor(
              b.region, sections::Rotated(b.region, deg), C::FillRule::NonZero);
          double a = 0.0;
          for (const auto& p : x)
            a += C::Area(p);
          // As thin as the polygons' noise along the whole outline.
          if (std::abs(a) / (kScale * kScale) >
              kGap2D * std::max(b.perimeterMm, 1e-9)) {
            same = false;
            break;
          }
        }
        if (same)
          return true;
      }
      return false;
    };
    // The seated orientation first, from each side; then the others.
    for (size_t i = 0; i < sides.size() && !rep.pass; i++)
      consider(run(i, 0));
    // (A turned path has the one orientation: its steps' are its own.)
    for (size_t k = 1; k < orientations && !rep.pass; k++) {
      if (symmetric(turns[k])) {
        advance(perTry * sides.size());
        continue;
      }
      for (size_t i = 0; i < sides.size() && !rep.pass; i++)
        consider(run(i, k));
    }

    // A thread that does not fit the one it goes into: no turning helps.
    for (const InsertionThread& it : rep.threads)
      if (!it.fits)
        rep.pass = false;
    rep.pairsSeconds = since(t0);
    t0 = Clock::now();
    // The one exact boolean: at the worst offset of the answer, or at the
    // seat when nothing interferes on the way.
    if (opt.confirm) {
      const InsertionTry& b = rep.best;
      InsertionConfirm& c = rep.confirm;
      c.ran = true;
      c.atMm = b.worstAtMm >= 0.0 ? b.worstAtMm : 0.0;
      size_t ti = 0;
      if (turning) {
        // The turn at that step of the path.
        if (b.travelMm > 0.0)
          ti = turnAt(
              b.positive,
              static_cast<int>(std::lround(
                  c.atMm / (helix ? rep.stepMm : b.travelMm / samples))));
      } else {
        for (size_t k = 0; k < turns.size(); k++)
          if (turns[k] == b.rotationDeg)
            ti = k;
      }
      // What the sections say there: every pair whose spans overlap at it.
      const double u = b.positive ? c.atMm : -c.atMm;
      std::vector<std::string> why; // the interfering pairs, for the log
      for (size_t i = 0; i < nA; i++)
        for (size_t j = 0; j < nB; j++) {
          const Band &a = bandsA[i], &f = bandsB[j];
          const double lo = std::max(a.z0 + u, f.z0),
                       hi = std::min(a.z1 + u, f.z1);
          if (hi - lo <= kZTol)
            continue;
          if (!turning) {
            const Window w = windowOf(a, f, b.positive);
            if (c.atMm < w.lo - kZTol || c.atMm > w.hi + kZTol)
              continue;
          }
          const PairReading& r = pair(ti, i, j);
          if (r.areaMm2 > 0.0)
            c.expectedOverlap = true;
          else if (r.interfering)
            c.expectedGap = true;
          if (r.interfering)
            why.push_back(fmt::format(
                "moving {:.3f}..{:.3f}{} against {:.3f}..{:.3f}{}: depth "
                "{:.4f}, overlap {:.5f} mm2",
                a.z0, a.z1, a.sliced ? " (slice)" : "", f.z0, f.z1,
                f.sliced ? " (slice)" : "", r.depthMm, r.areaMm2));
        }
      if (c.expectedOverlap)
        c.expectedGap = false;
      const TopoDS_Shape placed = Compound(solidsA).Moved(
          TopLoc_Location(Pose(c.atMm, turns[ti], b.positive)));
      // What can come near it: the fixed solids the reach kept.
      std::vector<TopoDS_Shape> kept;
      for (const sections::Solid& x : prepB)
        kept.push_back(x.shape);
      const TopoDS_Compound f = Compound(kept);
      BRepAlgoAPI_Common common;
      if (!cad::BuildNonDestructive(common, placed, f, kFuzz) ||
          common.Shape().IsNull())
        throw KernelError{"the confirming boolean failed"};
      c.volumeMm3 = VolumeOf(common.Shape());
      c.overlaps = c.volumeMm3 > kVolumeTol;
      c.agrees = c.overlaps == c.expectedOverlap;
      if (c.agrees && c.expectedGap) {
        BRepExtrema_DistShapeShape dist;
        dist.SetMultiThread(true);
        dist.LoadS1(placed);
        dist.LoadS2(f);
        dist.SetFlag(Extrema_ExtFlag_MIN);
        if (!dist.Perform() || !dist.IsDone())
          throw KernelError{"the confirming distance query failed"};
        c.gapMm = dist.Value();
        c.agrees = c.gapMm < opt.allowanceMm - kGapTol;
      }
      // The boolean finding matter where the sections read none fails the
      // answer; the sections reading more than the boolean (a slice
      // standing for a cone's whole span, a mesh's noise) is the safe side
      // of it, and is only said.
      if (c.overlaps && !c.expectedOverlap)
        rep.pass = false;
      if (!c.agrees) {
        // Say what the sections read there, so a disagreement is looked at
        // rather than guessed at.
        LOG_WARN("insertion: the sections and the exact boolean disagree "
                 "{:.3f} mm from the seat (sections: {}; boolean: {:.6f} mm3"
                 ", gap {:.4f} mm)",
                 c.atMm,
                 c.expectedOverlap ? "overlap"
                 : c.expectedGap   ? "a gap under the allowance"
                                   : "clear",
                 c.volumeMm3, c.gapMm);
        for (size_t k = 0; k < why.size() && k < 8; k++)
          LOG_WARN("insertion:   {}", why[k]);
      }
      rep.confirmSeconds = since(t0);
    }
    if (fraction)
      fraction->store(1.0);
  } catch (const KernelError& e) {
    return std::unexpected(e.what);
  } catch (const std::runtime_error& e) {
    return std::unexpected(e.what());
  } catch (const Standard_Failure& e) {
    return std::unexpected(std::string("the kernel failed: ") +
                           (e.GetMessageString() ? e.GetMessageString() : ""));
  }
  return rep;
}

} // namespace Measure
