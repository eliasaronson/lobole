// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/BlendFit.h"

#include "core/Measure.h"
#include "core/MeasureBlend.h"
#include "core/features/FilletFeature.h" // EdgeBlendOp

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <numbers>

namespace fit {

namespace {

std::atomic<std::uint64_t> g_built{0};

std::string NameOrIndex(const std::string* n, const char* kind, int index) {
  return n != nullptr ? *n : std::string(kind) + std::to_string(index);
}

// A round's setback per mm of radius where the faces meet at `theta` (the
// solid's interior angle); none at a tangent or concave edge, where a disc's
// rim bounds nothing.
std::optional<double> ConvexSetbackPerMm(double theta) {
  const double turn = std::numbers::pi - theta;
  if (turn < 1e-4)
    return std::nullopt;
  return std::tan(0.5 * turn);
}

std::string RimWhy(const std::string& edge, double radius) {
  return "edge '" + edge + "' is a circle of radius " + Mm(radius) + " mm";
}

} // namespace

std::string Mm(double v) {
  std::string s = std::format("{:.6g}", v);
  return s;
}

double RoundDown3(double v) {
  if (!(v > 0.0) || !std::isfinite(v))
    return 0.0;
  const int e = static_cast<int>(std::floor(std::log10(v)));
  const int k = 2 - e; // decimals kept
  // Scaled by an exact power of ten, so the division gives the double
  // nearest the decimal: 749 / 100 prints 7.49.
  if (k >= 0) {
    const double p = std::pow(10.0, k);
    return std::floor(v * p * (1.0 + 1e-12)) / p;
  }
  const double p = std::pow(10.0, -k);
  return std::floor(v / p * (1.0 + 1e-12)) * p;
}

std::vector<Bound> BlendBounds(const Body& body, const std::vector<int>& edges,
                               EdgeBlendOp op) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  Measure::BlendProbe probe(body);
  const char* blends = chamfer ? "chamfers" : "rounds";
  const char* aBlend = chamfer ? "a chamfer" : "a round";
  // How much of each face a blend of size 1 takes: its distance for a
  // chamfer, r / tan(theta/2) of a round where the faces meet at theta.
  const auto setbackPerMm = [&](int edge) -> std::optional<double> {
    if (chamfer)
      return 1.0;
    const auto theta = probe.Dihedral(edge);
    if (!theta)
      return std::nullopt;
    const double turn = std::abs(std::numbers::pi - *theta);
    if (turn < 1e-4)
      return std::nullopt; // tangent: nothing to round
    return std::tan(0.5 * turn);
  };
  const auto faceName = [&](int f) {
    return NameOrIndex(body.elementMap.NameOf(f), "face", f);
  };
  const auto edgeName = [&](int e) {
    return NameOrIndex(body.elementMap.EdgeNameOf(e), "edge", e);
  };
  std::vector<Bound> all;
  const auto offer = [&](BoundKind kind, double size, std::string why,
                         std::string element) {
    if (!(size > 0.0) || !std::isfinite(size))
      return;
    all.push_back(Bound{size, std::move(why), std::move(element), kind});
  };
  for (const int e : edges) {
    const auto faces = probe.Faces(e);
    const auto k = setbackPerMm(e);
    if (!faces || !k)
      continue;
    const bool convex =
        chamfer ? true : probe.Dihedral(e).value_or(0.0) < std::numbers::pi;
    for (const int f : *faces) {
      if (const auto w = probe.Width(e, f)) {
        const bool farPicked =
            std::find(edges.begin(), edges.end(), w->endsAt) != edges.end();
        const auto info = w->endsAt == e && convex
                              ? Measure::DescribeEdgeAt(body, e)
                              : std::nullopt;
        if (info && info->hasRadius) {
          // Across a disc to the same rim: the circle bounds it.
          offer(BoundKind::Rim, w->width / (2.0 * *k),
                RimWhy(edgeName(e), info->radius), edgeName(e));
        } else if (farPicked) {
          const double kFar =
              w->endsAt == e ? *k : setbackPerMm(w->endsAt).value_or(0.0);
          const double size = w->width / (*k + kFar);
          offer(BoundKind::Between, size,
                "face '" + faceName(f) + "' is " + Mm(w->width) +
                    " mm wide between the two picked edges, so the " + blends +
                    " meet at " + Mm(size) + " mm",
                faceName(f));
        } else {
          const double size = w->width / *k;
          offer(BoundKind::Width, size,
                "face '" + faceName(f) + "' runs " + Mm(w->width) +
                    " mm from edge '" + edgeName(e) + "', so " + aBlend +
                    " of " + Mm(size) + " mm takes all of it",
                faceName(f));
        }
      }
      if (!chamfer)
        if (const auto rc = probe.ConcaveRadius(e, f))
          offer(BoundKind::Curvature, *rc,
                "face '" + faceName(f) + "' curves with a radius of " +
                    Mm(*rc) + " mm toward the round",
                faceName(f));
    }
  }
  return all;
}

std::vector<Bound> RimBounds(const Body& body, const std::vector<int>& edges,
                             EdgeBlendOp op) {
  std::vector<Bound> out;
  const TopoDS_Shape& shape = body.shape.GetShape();
  if (shape.IsNull())
    return out;
  TopTools_IndexedMapOfShape all;
  TopExp::MapShapes(shape, TopAbs_EDGE, all);
  std::optional<TopTools_IndexedDataMapOfShapeListOfShape> faces;
  for (const int e : edges) {
    if (e < 0 || e + 1 > all.Extent())
      continue;
    const TopoDS_Edge& edge = TopoDS::Edge(all(e + 1));
    if (BRep_Tool::Degenerated(edge) || !BRep_Tool::IsClosed(edge))
      continue;
    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Circle)
      continue;
    if (!faces) {
      faces.emplace();
      TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, *faces);
    }
    // A planar face bounded by this circle alone: a disc to its own rim.
    bool disc = false;
    if (const int at = faces->FindIndex(edge); at != 0)
      for (const TopoDS_Shape& f : faces->FindFromIndex(at)) {
        if (BRepAdaptor_Surface(TopoDS::Face(f)).GetType() != GeomAbs_Plane)
          continue;
        int bounding = 0;
        for (TopExp_Explorer x(f, TopAbs_EDGE); x.More(); x.Next())
          bounding++;
        disc = disc || bounding == 1;
      }
    if (!disc)
      continue;
    double k = 1.0; // a chamfer takes its distance
    if (op != EdgeBlendOp::Chamfer) {
      const auto theta = Measure::DihedralAngleAt(body, e);
      const auto perMm = theta ? ConvexSetbackPerMm(*theta) : std::nullopt;
      if (!perMm)
        continue;
      k = *perMm;
    }
    const std::string* n = body.elementMap.EdgeNameOf(e);
    const std::string name = NameOrIndex(n, "edge", e);
    const double r = curve.Circle().Radius();
    out.push_back(Bound{r / k, RimWhy(name, r), name, BoundKind::Rim});
  }
  return out;
}

std::optional<Bound> Least(const std::vector<Bound>& bounds) {
  // Of two equal, the one that holds: a cylinder's end is bounded by its rim
  // and by its side's curvature alike, and only the rim is trusted.
  std::optional<Bound> least;
  for (const Bound& b : bounds)
    if (!least || b.size < least->size * (1.0 - 1e-9) ||
        (b.size <= least->size * (1.0 + 1e-9) && Refuses(b.kind) &&
         !Refuses(least->kind)))
      least = b;
  return least;
}

std::optional<Bound> BlendBound(const Body& body, const std::vector<int>& edges,
                                EdgeBlendOp op) {
  return Least(BlendBounds(body, edges, op));
}

bool Refuses(BoundKind kind) {
  // The sweep (tests/test_blend_fit_sweep.cpp) over the kernel cases' and
  // the action matrix's bodies: past a face's width, the width between two
  // picked edges or a face's curvature OpenCASCADE built hundreds of blends,
  // many at four times the bound (a wedge whose face narrows to a point at
  // one station, a round that runs off its face); past a disc's rim, none.
  switch (kind) {
  case BoundKind::Rim:
    return true;
  case BoundKind::Width:
  case BoundKind::Between:
  case BoundKind::Curvature:
    return false;
  }
  return false;
}

std::optional<Bound> RefusingBound(const std::vector<Bound>& bounds,
                                   double asked) {
  std::optional<Bound> least;
  for (const Bound& b : bounds)
    if (Refuses(b.kind) && asked > b.size * (1.0 + kRefuseMargin) &&
        (!least || b.size < least->size))
      least = b;
  return least;
}

Answer LargestThatBuilds(double asked, const std::optional<Bound>& bound,
                         const std::function<bool(double)>& builds,
                         const std::function<bool()>& stop,
                         SearchOptions options) {
  Answer a;
  a.asked = asked;
  a.fails = asked;
  double lo = 0.0; // the largest that built; 0 = none yet
  double seedBuilt = 0.0; // the seed's candidate, when it built
  // A candidate whose build was stopped part way (`builds` threw Stopped)
  // is neither: the search ends with the bracket it had.
  const auto tryOne = [&](double c) {
    g_built.fetch_add(1, std::memory_order_relaxed);
    try {
      if (builds(c)) {
        lo = c;
        a.fits = c;
        return true;
      }
    } catch (const Stopped&) {
      a.stopped = true;
      return false;
    }
    a.fails = c;
    return false;
  };
  // Just under the bound: rounded down from a thousandth below it, so two
  // rounds that would meet exactly leave a sliver between them.
  if (bound && bound->size < asked) {
    const double c = RoundDown3(bound->size * (1.0 - 1e-3));
    if (c > 0.0 && c < a.fails) {
      if (stop()) {
        a.stopped = true;
        return a;
      }
      if (tryOne(c)) {
        if (options.seedIsBound) {
          a.bound = bound; // nothing above the bound fits
          return a;
        }
        seedBuilt = c;
      }
      if (a.stopped)
        return a;
    }
  }
  for (int step = 0; step < options.steps; step++) {
    // Down by a factor while nothing has built, so eight steps reach a
    // thousandth of the size; then halving the bracket by ratio, so the
    // answer is as close to the limit in proportion whatever its size.
    const double c =
        RoundDown3(lo > 0.0 ? std::sqrt(lo * a.fails) : 0.4 * a.fails);
    if (c <= lo || c >= a.fails)
      break; // converged at three digits
    if (stop()) {
      a.stopped = true;
      return a;
    }
    tryOne(c);
    if (a.stopped)
      return a;
  }
  // A seed that built and nothing above it did: the bound is why that is
  // the limit, though it was not trusted to be.
  if (seedBuilt > 0.0 && a.fits == seedBuilt)
    a.bound = bound;
  return a;
}

std::uint64_t CandidatesBuilt() { return g_built.load(); }

std::optional<Answer> Memo::For(const CADShape& input,
                                const std::vector<int>& elements, int operation,
                                double asked) const {
  if (op != operation || edges != elements || answer.stopped ||
      body.GetShape().IsNull() || !body.GetShape().IsEqual(input.GetShape()) ||
      asked < answer.fails)
    return std::nullopt;
  Answer a = answer;
  a.asked = asked;
  return a;
}

std::string Sentence(EdgeBlendOp op, const Answer& a, const std::string& places,
                     const std::string& cause) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  const std::string what = chamfer ? "chamfer" : "fillet";
  const std::string param = chamfer ? "distance" : "radius";
  std::string s = what + " " + param + " " + Mm(a.asked) + " mm does not fit: ";
  if (a.stopped) {
    if (a.fits)
      s += "a " + param + " of " + Mm(*a.fits) + " mm builds and " +
           Mm(a.fails) + " mm does not (the search was stopped)";
    else
      s += Mm(a.fails) + " mm does not build either (the search was stopped)";
    return s + places;
  }
  if (a.fits) {
    s += "the largest " + param + " that fits is " + Mm(*a.fits) + " mm";
    if (a.bound)
      s += " (" + a.bound->why + ")";
    return s + places;
  }
  s += "no " + param + " builds on these edges, down to " + Mm(a.fails) + " mm";
  if (!cause.empty())
    s += " (" + cause + ")";
  return s + "; fewer edges in one " + what + " usually builds";
}

FitHint HintOf(const Answer& a) {
  FitHint h;
  h.param = "radius";
  h.askedMm = a.asked;
  h.fitsMm = a.fits;
  h.failsMm = a.fails;
  h.basis = "built";
  if (a.bound)
    h.element = a.bound->element;
  h.stopped = a.stopped;
  return h;
}

std::string BoundSentence(EdgeBlendOp op, const Answer& a, const Bound& past,
                          const std::string& places) {
  const bool chamfer = op == EdgeBlendOp::Chamfer;
  const std::string param = chamfer ? "distance" : "radius";
  std::string s = std::string(chamfer ? "chamfer " : "fillet ") + param + " " +
                  Mm(a.asked) + " mm does not fit: " + past.why;
  if (a.stopped)
    return s +
           (a.fits ? "; a " + param + " of " + Mm(*a.fits) + " mm builds and " +
                         Mm(a.fails) + " mm does not (the search was stopped)"
                   : std::string("; the search was stopped")) +
           places;
  if (a.fits)
    return s + "; the largest " + param + " that fits is " + Mm(*a.fits) +
           " mm" + places;
  return s + "; no " + param + " builds on these edges, down to " +
         Mm(a.fails) + " mm; fewer edges in one " +
         (chamfer ? "chamfer" : "fillet") + " usually builds";
}

FitHint BoundHint(const Answer& a) {
  FitHint h = HintOf(a);
  h.basis = "bound";
  return h;
}

} // namespace fit
