// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cad/Integrate.h"

#include <Adaptor3d_Curve.hxx>
#include <Adaptor3d_Surface.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp_Domain.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp_MeshProps.hxx>
#include <BRepGProp_Sinert.hxx>
#include <BRepGProp_Vinert.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_BezierCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Geom_Surface.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <math.hxx>
#include <math_Jacobi.hxx>
#include <math_Matrix.hxx>
#include <math_Vector.hxx>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <numbers>

namespace cad {

namespace {

// The protected figures of a GProps, read without the divide and multiply
// CentreOfMass and StaticMoments would put them through.
template <class Props> struct Raw : Props {
  FaceFigure Figure() const {
    FaceFigure f;
    f.volume = this->dim;
    f.moment = glm::dvec3(this->g.X(), this->g.Y(), this->g.Z()) * this->dim;
    const gp_Mat& m = this->inertia;
    f.inertia = {m.Value(1, 1), m.Value(2, 2), m.Value(3, 3),
                 m.Value(1, 2), m.Value(1, 3), m.Value(2, 3)};
    return f;
  }
};

struct RawMesh : BRepGProp_MeshProps {
  RawMesh() : BRepGProp_MeshProps(BRepGProp_MeshProps::Vinert) {}
  FaceFigure Figure() const {
    FaceFigure f;
    f.volume = dim;
    f.moment = glm::dvec3(g.X(), g.Y(), g.Z()) * dim;
    f.inertia = {inertia.Value(1, 1), inertia.Value(2, 2), inertia.Value(3, 3),
                 inertia.Value(1, 2), inertia.Value(1, 3), inertia.Value(2, 3)};
    return f;
  }
};

FaceFigure Add(const FaceFigure& a, const FaceFigure& b) {
  FaceFigure s;
  s.volume = a.volume + b.volume;
  s.moment = a.moment + b.moment;
  for (int i = 0; i < 6; i++)
    s.inertia[i] = a.inertia[i] + b.inertia[i];
  s.area = a.area + b.area;
  return s;
}

// |a - b|, figure by figure.
FaceFigure Gap(const FaceFigure& a, const FaceFigure& b) {
  FaceFigure d;
  d.volume = std::abs(a.volume - b.volume);
  d.moment = glm::abs(a.moment - b.moment);
  for (int i = 0; i < 6; i++)
    d.inertia[i] = std::abs(a.inertia[i] - b.inertia[i]);
  d.area = std::abs(a.area - b.area);
  return d;
}

double MaxOf(const glm::dvec3& v) { return std::max({v.x, v.y, v.z}); }
double MaxOf(const std::array<double, 6>& v) {
  return *std::max_element(v.begin(), v.end());
}

// The volume group (volume, moments, inertia) is within `tol` of `scale`.
bool VolumeWithin(const FaceFigure& gap, const FaceFigure& scale, double tol) {
  return gap.volume <= tol * scale.volume &&
         MaxOf(gap.moment) <= tol * scale.moment.x &&
         MaxOf(gap.inertia) <= tol * scale.inertia[0];
}
bool AreaWithin(const FaceFigure& gap, const FaceFigure& scale, double tol) {
  return gap.area <= tol * scale.area;
}

double ThreadSeconds() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

bool IsLineOrConic(GeomAbs_CurveType t) {
  return t == GeomAbs_Line || t == GeomAbs_Circle || t == GeomAbs_Ellipse ||
         t == GeomAbs_Hyperbola || t == GeomAbs_Parabola;
}

// The p-curve BRepGProp_Face::Load would integrate, before it reverses it.
Handle(Geom2d_Curve)
    PCurve(const TopoDS_Edge& e, const TopoDS_Face& fwd, double& a, double& b) {
  return BRep_Tool::CurveOnSurface(e, fwd, a, b);
}

void Knots(const Handle(Geom_BSplineCurve) & c, std::vector<double>& out) {
  for (int i = 1; i <= c->NbKnots(); i++)
    out.push_back(c->Knot(i));
}

// Where the surface is only piecewise smooth: its knot lines, U and V.
void SurfaceKnots(const Adaptor3d_Surface& s, std::vector<double>& uk,
                  std::vector<double>& vk, int depth = 0) {
  switch (s.GetType()) {
  case GeomAbs_BSplineSurface: {
    const Handle(Geom_BSplineSurface) b = s.BSpline();
    for (int i = 1; i <= b->NbUKnots(); i++)
      uk.push_back(b->UKnot(i));
    for (int i = 1; i <= b->NbVKnots(); i++)
      vk.push_back(b->VKnot(i));
    break;
  }
  case GeomAbs_SurfaceOfRevolution: {
    const Handle(Adaptor3d_Curve) c = s.BasisCurve();
    if (!c.IsNull() && c->GetType() == GeomAbs_BSplineCurve)
      Knots(c->BSpline(), vk);
    break;
  }
  case GeomAbs_SurfaceOfExtrusion: {
    const Handle(Adaptor3d_Curve) c = s.BasisCurve();
    if (!c.IsNull() && c->GetType() == GeomAbs_BSplineCurve)
      Knots(c->BSpline(), uk);
    break;
  }
  case GeomAbs_OffsetSurface:
    if (depth < 4) {
      const Handle(Adaptor3d_Surface) basis = s.BasisSurface();
      if (!basis.IsNull())
        SurfaceKnots(*basis, uk, vk, depth + 1);
    }
    break;
  default:
    break;
  }
}

// The values of `knots` strictly inside (lo, hi), sorted.
std::vector<double> Inside(std::vector<double> knots, double lo, double hi) {
  const double eps = 1e-12 * std::max(1.0, hi - lo);
  std::vector<double> out;
  for (double k : knots)
    if (k > lo + eps && k < hi - eps)
      out.push_back(k);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end(),
                        [&](double x, double y) { return y - x <= eps; }),
            out.end());
  return out;
}

// A face is suspect when the fixed rule's one Gauss rule per edge (along the
// edge, and across u from the face's lower bound) runs over a place where
// the integrand is only piecewise smooth: a boundary p-curve that is not a
// line or a conic, or a knot line of the surface inside the face's bounds.
bool Suspect(const TopoDS_Face& face) {
  const TopoDS_Face fwd = TopoDS::Face(face.Oriented(TopAbs_FORWARD));
  for (BRepGProp_Domain d(face); d.More(); d.Next()) {
    const TopoDS_Edge& e = d.Value();
    if (e.Orientation() != TopAbs_FORWARD && e.Orientation() != TopAbs_REVERSED)
      continue;
    double a = 0, b = 0;
    const Handle(Geom2d_Curve) c = PCurve(e, fwd, a, b);
    if (c.IsNull())
      continue;
    if (!IsLineOrConic(Geom2dAdaptor_Curve(c, a, b).GetType()))
      return true;
  }
  const BRepAdaptor_Surface s(fwd);
  std::vector<double> uk, vk;
  SurfaceKnots(s, uk, vk);
  return !Inside(uk, s.FirstUParameter(), s.LastUParameter()).empty() ||
         !Inside(vk, s.FirstVParameter(), s.LastVParameter()).empty();
}

struct Gauss {
  std::vector<double> x, w;
  explicit Gauss(int n) {
    n = std::min(n, math::GaussPointsMax());
    math_Vector p(1, n), q(1, n);
    math::GaussPoints(n, p);
    math::GaussWeights(n, q);
    for (int i = 1; i <= n; i++) {
      x.push_back(p(i));
      w.push_back(q(i));
    }
  }
};

// One boundary edge of a suspect face, cut where its integrand is only
// piecewise smooth.
struct Edge {
  Geom2dAdaptor_Curve curve;
  double sign = 1.0; // -1 for a REVERSED edge: integrated forward, negated
  std::vector<double> breaks;
  int order = 10;
};

// The second estimate of one face: the same Green's-theorem integral the
// fixed rule computes (an outer rule along each p-curve, an inner one across
// u from the face's lower u bound), with each p-curve cut at its own knots
// and where it crosses a knot line of the surface, the inner span cut at the
// surface's u knots, and every piece halved `level` times.
class SecondEstimate {
public:
  SecondEstimate(const TopoDS_Face& face, const gp_Pnt& about) : about_(about) {
    surface_.Load(face);
    surface_.Bounds(u1_, u2_, v1_, v2_);
    const TopoDS_Face fwd = TopoDS::Face(face.Oriented(TopAbs_FORWARD));
    BRepAdaptor_Surface s(fwd);
    std::vector<double> uk, vk;
    SurfaceKnots(s, uk, vk);
    uKnots_ = Inside(uk, u1_, u2_);
    vKnots_ = Inside(vk, v1_, v2_);
    // A closed u direction (an angle) is cut into quarter turns or less, so
    // the inner rule starts on spans its trigonometry converges on.
    if (s.IsUPeriodic() && s.UPeriod() > 0.0) {
      std::vector<double> cuts{u1_};
      cuts.insert(cuts.end(), uKnots_.begin(), uKnots_.end());
      cuts.push_back(u2_);
      std::vector<double> more;
      for (size_t i = 0; i + 1 < cuts.size(); i++) {
        const int n = int(
            std::ceil((cuts[i + 1] - cuts[i]) / (0.25 * s.UPeriod()) - 1e-9));
        for (int k = 1; k < n; k++)
          more.push_back(cuts[i] + (cuts[i + 1] - cuts[i]) * k / n);
      }
      innerCuts_ = uKnots_;
      innerCuts_.insert(innerCuts_.end(), more.begin(), more.end());
      std::sort(innerCuts_.begin(), innerCuts_.end());
    } else {
      innerCuts_ = uKnots_;
    }

    for (BRepGProp_Domain d(face); d.More(); d.Next()) {
      const TopoDS_Edge& e = d.Value();
      if (e.Orientation() != TopAbs_FORWARD &&
          e.Orientation() != TopAbs_REVERSED)
        continue;
      double a = 0, b = 0;
      const Handle(Geom2d_Curve) c = PCurve(e, fwd, a, b);
      if (c.IsNull()) {
        ok_ = false; // the fixed rule stops here too
        continue;
      }
      if (!(b > a))
        continue;
      Edge edge;
      edge.curve.Load(c, a, b);
      edge.sign = e.Orientation() == TopAbs_REVERSED ? -1.0 : 1.0;
      std::vector<double> ks;
      if (edge.curve.GetType() == GeomAbs_BSplineCurve) {
        const Handle(Geom2d_BSplineCurve) bs = edge.curve.BSpline();
        for (int i = 1; i <= bs->NbKnots(); i++)
          ks.push_back(bs->Knot(i));
        edge.order = std::max(10, 2 * (bs->Degree() + 1));
      } else if (edge.curve.GetType() == GeomAbs_BezierCurve) {
        edge.order = std::max(10, 2 * (edge.curve.Bezier()->Degree() + 1));
      }
      std::vector<double> spans = Inside(ks, a, b);
      spans.insert(spans.begin(), a);
      spans.push_back(b);
      edge.breaks = spans;
      for (size_t i = 0; i + 1 < spans.size(); i++)
        Crossings(edge.curve, spans[i], spans[i + 1], edge.breaks);
      std::sort(edge.breaks.begin(), edge.breaks.end());
      edges_.push_back(std::move(edge));
    }
  }

  bool Ok() const { return ok_; }

  FaceFigure At(int level) {
    const int split = 1 << level;
    FaceFigure f;
    for (const Edge& e : edges_) {
      const Gauss& g = Rule(e.order);
      for (size_t i = 0; i + 1 < e.breaks.size(); i++) {
        const double ta = e.breaks[i], tb = e.breaks[i + 1];
        for (int k = 0; k < split; k++) {
          const double l1 = ta + (tb - ta) * k / split;
          const double l2 = ta + (tb - ta) * (k + 1) / split;
          const double lm = 0.5 * (l1 + l2), lr = 0.5 * (l2 - l1);
          for (size_t j = 0; j < g.x.size(); j++) {
            gp_Pnt2d p;
            gp_Vec2d d;
            e.curve.D1(lm + lr * g.x[j], p, d);
            const double dul = e.sign * d.Y() * g.w[j] * lr;
            if (dul == 0.0)
              continue;
            const double u = std::clamp(p.X(), u1_, u2_);
            const double v = std::clamp(p.Y(), v1_, v2_);
            Inner(u, v, dul, split, f);
          }
        }
      }
    }
    return f;
  }

private:
  // Where u(t) or v(t) crosses a knot line of the surface within [ta, tb].
  void Crossings(const Geom2dAdaptor_Curve& c, double ta, double tb,
                 std::vector<double>& out) const {
    if (uKnots_.empty() && vKnots_.empty())
      return;
    constexpr int kSamples = 32;
    const auto coord = [&](double t, bool isU) {
      const gp_Pnt2d p = c.Value(t);
      return isU ? p.X() : p.Y();
    };
    for (int axis = 0; axis < 2; axis++) {
      const bool isU = axis == 0;
      const std::vector<double>& knots = isU ? uKnots_ : vKnots_;
      if (knots.empty())
        continue;
      double t0 = ta, c0 = coord(ta, isU);
      for (int s = 1; s <= kSamples; s++) {
        const double t1 = ta + (tb - ta) * s / kSamples;
        const double c1 = coord(t1, isU);
        for (double k : knots) {
          if ((c0 - k) * (c1 - k) >= 0.0)
            continue;
          double lo = t0, hi = t1;
          for (int it = 0; it < 80 && hi - lo > 1e-15 * (tb - ta); it++) {
            const double mid = 0.5 * (lo + hi);
            if ((coord(lo, isU) - k) * (coord(mid, isU) - k) <= 0.0)
              hi = mid;
            else
              lo = mid;
          }
          out.push_back(0.5 * (lo + hi));
        }
        t0 = t1;
        c0 = c1;
      }
    }
  }

  // The inner integral across u from the lower bound to `u`, at `v`.
  void Inner(double u, double v, double dul, int split, FaceFigure& f) {
    if (u == u1_)
      return;
    const double lo = std::min(u1_, u), hi = std::max(u1_, u);
    const double sign = u >= u1_ ? 1.0 : -1.0;
    const Gauss& g = Rule(kInnerOrder);
    double a = lo;
    auto span = [&](double sa, double sb) {
      for (int k = 0; k < split; k++) {
        const double x1 = sa + (sb - sa) * k / split;
        const double x2 = sa + (sb - sa) * (k + 1) / split;
        const double um = 0.5 * (x1 + x2), ur = 0.5 * (x2 - x1);
        for (size_t j = 0; j < g.x.size(); j++) {
          gp_Pnt p;
          gp_Vec n;
          surface_.Normal(um + ur * g.x[j], v, p, n);
          const double w = sign * dul * ur * g.w[j];
          const double x = p.X() - about_.X(), y = p.Y() - about_.Y(),
                       z = p.Z() - about_.Z();
          const double dv = (x * n.X() + y * n.Y() + z * n.Z()) * w;
          f.volume += dv / 3.0;
          f.moment += glm::dvec3(x, y, z) * (0.25 * dv);
          const double d5 = 0.2 * dv;
          f.inertia[0] += (y * y + z * z) * d5;
          f.inertia[1] += (x * x + z * z) * d5;
          f.inertia[2] += (x * x + y * y) * d5;
          f.inertia[3] -= x * y * d5;
          f.inertia[4] -= x * z * d5;
          f.inertia[5] -= y * z * d5;
          f.area += n.Magnitude() * w;
        }
      }
    };
    for (double k : innerCuts_) {
      if (k <= lo)
        continue;
      if (k >= hi)
        break;
      span(a, k);
      a = k;
    }
    span(a, hi);
  }

  // A map, so a reference to one rule outlives the making of another.
  const Gauss& Rule(int n) { return rules_.try_emplace(n, n).first->second; }

  static constexpr int kInnerOrder = 12;
  gp_Pnt about_;
  BRepGProp_Face surface_;
  double u1_ = 0, u2_ = 0, v1_ = 0, v2_ = 0;
  std::vector<double> uKnots_, vKnots_, innerCuts_;
  std::vector<Edge> edges_;
  std::map<int, Gauss> rules_;
  bool ok_ = true;
};

// The fixed rule on one face, about `about`: exactly what
// BRepGProp::VolumeProperties and SurfaceProperties add for it.
bool FixedFigure(const TopoDS_Face& face, const gp_Pnt& about, bool volume,
                 bool area, FaceFigure& out) {
  TopLoc_Location l;
  const Handle(Geom_Surface) & surf = BRep_Tool::Surface(face, l);
  if (surf.IsNull()) {
    const Handle(Poly_Triangulation) & tri = BRep_Tool::Triangulation(face, l);
    if (tri.IsNull() || tri->NbNodes() == 0 || tri->NbTriangles() == 0)
      return false;
    if (volume) {
      RawMesh mv;
      mv.SetLocation(about);
      mv.Perform(tri, l, face.Orientation());
      out = mv.Figure();
    }
    if (area) {
      BRepGProp_MeshProps ms(BRepGProp_MeshProps::Sinert);
      ms.SetLocation(about);
      ms.Perform(tri, l, face.Orientation());
      out.area = ms.Mass();
    }
    return true;
  }
  BRepGProp_Face bf;
  BRepGProp_Domain bd;
  bf.Load(face);
  const bool natural = face.NbChildren() == 0;
  if (volume) {
    Raw<BRepGProp_Vinert> v;
    v.SetLocation(about);
    if (natural) {
      v.Perform(bf);
    } else {
      bd.Init(face);
      v.Perform(bf, bd);
    }
    out = v.Figure();
  }
  if (!area)
    return true;
  BRepGProp_Sinert s;
  s.SetLocation(about);
  if (natural) {
    s.Perform(bf);
  } else {
    bd.Init(face);
    s.Perform(bf, bd);
  }
  out.area = s.Mass();
  return true;
}

} // namespace

Integral Integrate(const TopoDS_Shape& shape, const IntegrateOptions& options) {
  Integral out;
  if (shape.IsNull())
    return out;
  Bnd_Box box;
  BRepBndLib::Add(shape, box, false);
  if (box.IsVoid())
    return out;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  const gp_Pnt about(0.5 * (x0 + x1), 0.5 * (y0 + y1), 0.5 * (z0 + z1));
  out.about = about;

  // The fixed rule on every face first: it sets the body's scale.
  std::vector<FaceReport> faces;
  std::vector<TopoDS_Face> shapes;
  int index = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), index++) {
    const TopoDS_Face& face = TopoDS::Face(ex.Current());
    if (face.Orientation() != TopAbs_FORWARD &&
        face.Orientation() != TopAbs_REVERSED)
      continue;
    FaceReport r;
    r.index = index;
    if (!FixedFigure(face, about, options.volume, options.area, r.fixed))
      continue;
    TopLoc_Location l;
    r.suspect = !BRep_Tool::Surface(face, l).IsNull() &&
                face.NbChildren() != 0 &&
                (options.everyFaceSuspect || Suspect(face));
    r.used = r.fixed;
    faces.push_back(r);
    shapes.push_back(face);
    out.scale.volume += std::abs(r.fixed.volume);
    out.scale.area += std::abs(r.fixed.area);
  }
  // The moments' scale is the volume's times the box's half diagonal (its
  // square for the inertia): a face's own moments can all but vanish about
  // the box's centre, a whole sphere's do, and a scale summed from them
  // would read rounding as disagreement.
  {
    const double reach = 0.5 * std::sqrt(box.SquareExtent());
    out.scale.moment = glm::dvec3(out.scale.volume * reach);
    out.scale.inertia.fill(out.scale.volume * reach * reach);
  }

  // A second estimate on every suspect face.
  for (size_t i = 0; i < faces.size(); i++) {
    FaceReport& r = faces[i];
    if (!r.suspect)
      continue;
    out.suspectFaces++;
    const double t0 = ThreadSeconds();
    SecondEstimate second(shapes[i], about);
    if (!second.Ok()) {
      // An edge with no p-curve: the fixed rule stopped there too, so none
      // of the face's figure is known.
      r.uncertainty = Gap(r.fixed, FaceFigure{});
      r.converged = false;
      out.unconverged.push_back(r.index);
      continue;
    }
    // Each group (the volume's figures, the area) stops at the level where
    // it converged, so it does not depend on whether the other was asked.
    const bool wantV = options.volume, wantA = options.area;
    FaceFigure prev = second.At(0), cur = prev;
    FaceFigure change = Gap(prev, r.fixed);
    if (wantV)
      r.volumeChanges.push_back(change.volume / out.scale.volume);
    FaceFigure curV = cur, changeV = change, curA = cur, changeA = change;
    bool doneV = !wantV, doneA = !wantA;
    int level = 0;
    while ((!doneV || !doneA) && level < options.maxHalvings) {
      level++;
      cur = second.At(level);
      change = Gap(cur, prev);
      if (wantV)
        r.volumeChanges.push_back(change.volume / out.scale.volume);
      prev = cur;
      if (!doneV) {
        curV = cur;
        changeV = change;
        doneV = VolumeWithin(change, out.scale, options.convergence);
      }
      if (!doneA) {
        curA = cur;
        changeA = change;
        doneA = AreaWithin(change, out.scale, options.convergence);
      }
    }
    const bool converged =
        (!wantV || (doneV && level > 0)) && (!wantA || (doneA && level > 0));
    r.seconds = ThreadSeconds() - t0;
    r.second = wantV ? curV : FaceFigure{};
    r.second.area = wantA ? curA.area : 0.0;
    r.halvings = level;
    r.converged = converged;
    if (!converged)
      out.unconverged.push_back(r.index);

    if (wantV) {
      const FaceFigure gap = Gap(r.fixed, curV);
      if (VolumeWithin(gap, out.scale, options.agreement)) {
        r.uncertainty = Add(gap, changeV);
      } else {
        r.volumeReplaced = true;
        r.used.volume = curV.volume;
        r.used.moment = curV.moment;
        r.used.inertia = curV.inertia;
        r.uncertainty = changeV;
      }
    }
    r.uncertainty.area = 0.0;
    if (wantA) {
      const FaceFigure gap = Gap(r.fixed, curA);
      if (AreaWithin(gap, out.scale, options.agreement)) {
        r.uncertainty.area = gap.area + changeA.area;
      } else {
        r.areaReplaced = true;
        r.used.area = curA.area;
        r.uncertainty.area = changeA.area;
      }
    }
  }

  // Summed in explorer order.
  double uV = 0.0, uS = 0.0, uJ = 0.0, uA = 0.0;
  for (const FaceReport& r : faces) {
    out.sum = Add(out.sum, r.used);
    uV += r.uncertainty.volume;
    uS += MaxOf(r.uncertainty.moment);
    uJ += MaxOf(r.uncertainty.inertia);
    uA += r.uncertainty.area;
  }
  if (options.keepFaces)
    out.faces = std::move(faces);

  const FaceFigure& s = out.sum;
  out.volume = s.volume;
  out.area = s.area;
  out.volumeUncertainty = uV;
  out.areaUncertainty = uA;
  out.inertiaUncertainty = uJ;
  if (std::abs(s.volume) < 1e-30) {
    out.centerOfMass = glm::dvec3(about.X(), about.Y(), about.Z());
    return out;
  }
  const glm::dvec3 d = s.moment / s.volume;
  out.centerOfMass = glm::dvec3(about.X(), about.Y(), about.Z()) + d;
  // About the centre of mass: J = I + V (|d|^2 E - d d^T).
  const double j[3][3] = {{s.inertia[0], s.inertia[3], s.inertia[4]},
                          {s.inertia[3], s.inertia[1], s.inertia[5]},
                          {s.inertia[4], s.inertia[5], s.inertia[2]}};
  const double dd = glm::dot(d, d);
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      out.inertia[r][c] =
          j[r][c] - s.volume * ((r == c ? dd : 0.0) - d[r] * d[c]);
  const double V = std::abs(s.volume), S = glm::length(s.moment);
  out.centerUncertainty = (uS + glm::length(d) * uV) / V;
  out.inertiaUncertainty =
      uJ + 2.0 * std::sqrt(3.0) * S * uS / V + S * S * uV / (V * V);
  return out;
}

double VolumeOf(const TopoDS_Shape& shape) {
  IntegrateOptions o;
  o.area = false;
  return Integrate(shape, o).volume;
}

void PrincipalAxes(const Integral& m, double moments[3], glm::dvec3 axes[3]) {
  math_Matrix a(1, 3, 1, 3);
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      a(r + 1, c + 1) = m.inertia[r][c];
  math_Jacobi j(a);
  for (int k = 0; k < 3; k++) {
    moments[k] = j.IsDone() ? j.Value(k + 1) : 0.0;
    glm::dvec3 v(k == 0, k == 1, k == 2);
    if (j.IsDone()) {
      math_Vector e(1, 3);
      j.Vector(k + 1, e);
      v = glm::dvec3(e(1), e(2), e(3));
    }
    const double len = glm::length(v);
    axes[k] = len > 0.0 ? v / len : glm::dvec3(k == 0, k == 1, k == 2);
  }
}

} // namespace cad
