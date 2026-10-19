// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeasureInsertion.h"

#include "cad/NonDestructiveBoolean.h"
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
#include <numbers>

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
  if (thick > kFuzz) {
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
                                 const glm::dmat4& xf, const gp_Trsf& toAxis) {
  std::vector<TopoDS_Shape> out;
  const TopLoc_Location loc(toAxis * Rigid(xf));
  for (const Body& b : bodies) {
    if (b.reference || b.shape.GetShape().IsNull())
      continue;
    const TopoDS_Shape placed = b.shape.GetShape().Moved(loc);
    for (TopExp_Explorer e(placed, TopAbs_SOLID); e.More(); e.Next())
      out.push_back(e.Current());
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
  const auto threaded = [](const std::vector<Body>& side) {
    return std::any_of(side.begin(), side.end(), [](const Body& b) {
      return !b.reference && (!b.threads.empty() || !b.threadRegions.empty());
    });
  };
  if (threaded(moving) && threaded(fixed) && !opt.screw)
    return std::unexpected(
        "both sides carry modelled threads: a threaded pair goes on by "
        "turning, which a straight slide does not judge");
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
    const std::vector<TopoDS_Shape> solidsA = Solids(moving, xfMoving, toAxis),
                                    solidsB = Solids(fixed, xfFixed, toAxis);
    if (solidsA.empty() || solidsB.empty())
      return std::unexpected("one side places no visible matter (hidden, or "
                             "a part with no bodies)");
    std::vector<sections::Solid> prepA, prepB;
    for (const TopoDS_Shape& s : solidsA)
      prepA.push_back(sections::Prepare(s));
    for (const TopoDS_Shape& s : solidsB)
      prepB.push_back(sections::Prepare(s));
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
    const double reachA = reachOf(prepB) + g, reachB = reachOf(prepA) + g;
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
      if (rep.threads.empty())
        return std::unexpected(
            "it goes on by its thread, but no thread of the moving side "
            "meets one of the fixed side on the path (a Thread feature or "
            "a tapped Hole on each, coaxial with it)");
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
    rep.stepMm = longest / opt.samples;
    rep.leadInMm = opt.leadInMm >= 0.0 ? opt.leadInMm : rep.stepMm;
    rep.sliceMm = rep.stepMm > 0.0 ? rep.stepMm : 1.0;

    // The bands, both sides on one slice grid.
    using Clock = std::chrono::steady_clock;
    const auto since = [](Clock::time_point t) {
      return std::chrono::duration<double>(Clock::now() - t).count();
    };
    auto t0 = Clock::now();
    std::vector<Band> bandsA, bandsB;
    {
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
    const bool turning = opt.turnDeg != 0.0;
    for (int k = 0; k < (turning ? 1 : opt.rotations); k++)
      turns.push_back(360.0 * k / opt.rotations);
    // A turned path's orientation at each of its steps, after the seat's.
    if (turning)
      for (int k = 1; k <= opt.samples; k++)
        turns.push_back(opt.turnDeg * k / opt.samples);
    const size_t nA = bandsA.size(), nB = bandsB.size();
    std::vector<std::vector<Turned>> turned(turns.size());
    std::vector<std::vector<int8_t>> known(turns.size());
    std::vector<std::vector<PairReading>> readings(turns.size());
    const auto turnedAt = [&](size_t ti) -> const std::vector<Turned>& {
      auto& v = turned[ti];
      if (v.empty()) {
        v.reserve(nA);
        for (const Band& b : bandsA) {
          Turned t;
          t.region = sections::Rotated(b.region, turns[ti]);
          t.box = C::GetBounds(t.region);
          if (opt.allowanceMm > kGap2D) {
            t.grown =
                C::InflatePaths(t.region, (opt.allowanceMm - kGap2D) * kScale,
                                C::JoinType::Round, C::EndType::Polygon, 2.0,
                                0.25 * kGap2D * kScale);
            t.grownBox = C::GetBounds(t.grown);
          }
          v.push_back(std::move(t));
        }
        known[ti].assign(nA * nB, 0);
        readings[ti].resize(nA * nB);
      }
      return v;
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
      const auto& t = turnedAt(ti);
      const size_t k = i * nB + j;
      if (!known[ti][k]) {
        readings[ti][k] =
            ByThread(bandsA[i], bandsB[j])
                ? PairReading{}
                : ReadPair(bandsA[i], t[i], bandsB[j], opt.allowanceMm);
        known[ti][k] = 1;
        rep.pairsRead++;
      }
      return readings[ti][k];
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
        perTry * static_cast<double>(sides.size() * turns.size());
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
        const double step = t.travelMm / opt.samples;
        const double sgn = positive ? 1.0 : -1.0;
        for (int k = opt.samples; k >= 1; k--) {
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
              const PairReading& r = pair(static_cast<size_t>(k), i, j);
              if (!r.interfering)
                continue;
              any = true;
              deep = deep || r.depthMm > seatDepth + deeperTol;
              if (r.depthMm > t.worstDepthMm) {
                t.worstDepthMm = r.depthMm;
                worstAt = u;
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
          double lo, hi;
        };
        std::vector<Cand> cands;
        for (size_t i = 0; i < nA; i++)
          for (size_t j = 0; j < nB; j++) {
            const Band &a = bandsA[i], &b = bandsB[j];
            double lo, hi;
            if (positive) {
              lo = b.z0 - a.z1;
              hi = b.z1 - a.z0;
            } else {
              lo = a.z0 - b.z1;
              hi = a.z1 - b.z0;
            }
            lo = std::max(lo, 0.0);
            hi = std::min(hi, t.travelMm);
            if (hi - lo > kZTol)
              cands.push_back({i, j, lo, hi});
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
            worstAt = 0.5 * (c.lo + c.hi);
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
    for (size_t k = 1; k < turns.size() && !rep.pass; k++) {
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
          ti = static_cast<size_t>(
              std::lround(c.atMm / (b.travelMm / opt.samples)));
      } else {
        for (size_t k = 0; k < turns.size(); k++)
          if (turns[k] == b.rotationDeg)
            ti = k;
      }
      // What the sections say there: every pair whose spans overlap at it.
      const double u = b.positive ? c.atMm : -c.atMm;
      for (size_t i = 0; i < nA; i++)
        for (size_t j = 0; j < nB; j++) {
          const Band &a = bandsA[i], &f = bandsB[j];
          const double lo = std::max(a.z0 + u, f.z0),
                       hi = std::min(a.z1 + u, f.z1);
          if (hi - lo <= kZTol)
            continue;
          const PairReading& r = pair(ti, i, j);
          if (r.areaMm2 > 0.0)
            c.expectedOverlap = true;
          else if (r.interfering)
            c.expectedGap = true;
        }
      if (c.expectedOverlap)
        c.expectedGap = false;
      const TopoDS_Shape placed = Compound(solidsA).Moved(
          TopLoc_Location(Pose(c.atMm, turns[ti], b.positive)));
      const TopoDS_Compound f = Compound(solidsB);
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
      if (!c.agrees)
        rep.pass = false;
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
