#include "core/WormGear.h"

#include "cad/NonDestructiveBoolean.h"
#include "common/Logger.h"
#include "core/Angles.h"
#include "core/HelicalSweep.h"
#include "core/Occurrence.h"
#include "core/Parallel.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_ThruSections.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRep_Builder.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace worm {

namespace {

void Fail(std::string* why, const std::string& msg) {
  if (why)
    *why = msg;
}

std::string Fmt(const char* fmt, double a, double b = 0.0, double c = 0.0) {
  char buf[256];
  snprintf(buf, sizeof(buf), fmt, a, b, c);
  return buf;
}

double VolumeOf(const TopoDS_Shape& s) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(s, props);
  return props.Mass();
}

} // namespace

std::string Check(const PairSpec& spec, bool wheel) {
  if (!(spec.module > 1e-6) || !std::isfinite(spec.module))
    return "a worm needs a positive module (mm)";
  if (spec.starts < 1 || spec.starts > 8)
    return "a worm has 1 to 8 starts";
  if (!(spec.pressureDeg >= 10.0 && spec.pressureDeg <= 30.0))
    return "the pressure angle is 10 to 30 degrees (20 is usual)";
  if (!(spec.clearance >= 0.0 && spec.clearance <= 0.5))
    return "the root clearance is 0 to 0.5 of the module (0.25 is usual)";
  if (!(spec.wormPitchDia > 0.0) || !std::isfinite(spec.wormPitchDia))
    return "a worm needs a positive pitch diameter (mm)";
  const Sizes sz = Size(spec);
  if (!(sz.wormRootRadius > 0.1 * spec.module))
    return Fmt("the worm's pitch diameter (%.3f mm) leaves no root: it must "
               "exceed %.3f mm at this module",
               spec.wormPitchDia,
               2.0 * (1.0 + spec.clearance) * spec.module + 0.2 * spec.module);
  if (sz.leadAngle > M_PI / 4.0)
    return Fmt("a lead angle of %.1f degrees is a crossed helical gear, not a "
               "worm: more pitch diameter or fewer starts",
               angles::ToDegrees(sz.leadAngle));
  if (wheel && spec.teeth < 8)
    return "a worm wheel needs at least 8 teeth";
  return {};
}

Sizes Size(const PairSpec& spec) {
  Sizes s;
  const double m = spec.module;
  s.axialPitch = M_PI * m;
  s.lead = spec.starts * s.axialPitch;
  s.wormPitchRadius = spec.wormPitchDia / 2.0;
  s.wormTipRadius = s.wormPitchRadius + m;
  s.wormRootRadius = s.wormPitchRadius - (1.0 + spec.clearance) * m;
  s.leadAngle = spec.wormPitchDia > 0.0
                    ? std::atan(spec.starts * m / spec.wormPitchDia)
                    : 0.0;
  const double q = m > 0.0 ? spec.wormPitchDia / m : 0.0;
  s.faceWidth =
      std::min(2.0 * m * (0.5 + std::sqrt(q + 1.0)), 0.67 * spec.wormPitchDia);
  if (spec.teeth > 0) {
    s.wheelPitchRadius = spec.teeth * m / 2.0;
    s.centreDistance = s.wormPitchRadius + s.wheelPitchRadius;
    s.throatRadius = s.wormPitchRadius - m;
    s.wheelThroatRadius = s.centreDistance - s.throatRadius;
    s.wheelRootRadius = s.wheelPitchRadius - (1.0 + spec.clearance) * m;
    s.wheelTipRadius =
        std::min(s.wheelPitchRadius + 1.5 * m,
                 s.centreDistance - s.throatRadius * std::cos(M_PI / 4.0));
  }
  return s;
}

double ThroatArcDeg(const Sizes& sz, double face, double tipRadius) {
  if (!(sz.throatRadius > 0.0) || tipRadius <= sz.wheelThroatRadius)
    return 0.0;
  const double c =
      std::clamp((sz.centreDistance - tipRadius) / sz.throatRadius, -1.0, 1.0);
  const double byTip = std::acos(c);
  const double byFace =
      std::asin(std::clamp(face / 2.0 / sz.throatRadius, 0.0, 1.0));
  return angles::ToDegrees(2.0 * std::min(byTip, byFace));
}

Locking SelfLocking(double leadAngleRad, double mu) {
  Locking l;
  l.mu = mu;
  const double rho = std::atan(mu);
  l.frictionAngleDeg = angles::ToDegrees(rho);
  l.efficiency = std::tan(leadAngleRad) / std::tan(leadAngleRad + rho);
  l.locks = leadAngleRad < rho;
  return l;
}

std::vector<gp_Pnt2d> WormSpaceProfile(const PairSpec& spec, double backlash,
                                       double reach, std::string* why) {
  const Sizes sz = Size(spec);
  const double m = spec.module;
  const double tanA = std::tan(angles::ToRadians(spec.pressureDeg));
  const double r1 = sz.wormPitchRadius;
  // The space at the pitch line is half the axial pitch plus the backlash
  // (half of it off each flank); straight flanks widen it outward.
  const double e = sz.axialPitch / 2.0 + backlash;
  const auto half = [&](double x) { return e / 2.0 + (x - r1) * tanA; };
  const double root = sz.wormRootRadius, tip = sz.wormTipRadius;
  if (!(half(root) > 0.02 * m)) {
    Fail(why, "the worm's tooth space closes before its root: lower the "
              "pressure angle");
    return {};
  }
  if (!(sz.axialPitch - 2.0 * half(tip) > 0.05 * m)) {
    Fail(why, Fmt("the worm's tooth comes to a point before its tip (%.3f mm "
                  "of backlash at module %.3f): less backlash or a lower "
                  "pressure angle",
                  backlash, m));
    return {};
  }
  reach = std::max(reach, tip + 0.1 * m);
  return {{root, -half(root)}, {tip, -half(tip)}, {reach, -half(tip)},
          {reach, half(tip)},  {tip, half(tip)},  {root, half(root)}};
}

CADShape WormSpaceTool(const gp_Ax3& frame, double z0, double z1,
                       bool squareStart, bool squareEnd, double toothZ,
                       const PairSpec& spec, double backlash, double reach,
                       std::string* why) {
  const std::string bad = Check(spec, false);
  if (!bad.empty()) {
    Fail(why, bad);
    return CADShape();
  }
  HelicalSweepSpec h;
  h.frame = frame;
  h.z0 = z0;
  h.z1 = z1;
  h.squareStart = squareStart;
  h.squareEnd = squareEnd;
  const Sizes sz = Size(spec);
  h.lead = sz.lead;
  h.starts = spec.starts;
  h.rightHanded = spec.rightHanded;
  // The profile is a SPACE centred on y = 0, so the space's centre is half an
  // axial pitch off the tooth.
  h.pinPhase = true;
  h.phaseHeight = toothZ + sz.axialPitch / 2.0;
  h.profile = WormSpaceProfile(spec, backlash, reach, why);
  if (h.profile.empty())
    return CADShape();
  return HelicalSweep(h, why);
}

namespace {

// The hob as a membership test, in the wheel's local frame: Z the wheel's
// axis, the hob's axis through (a, 0, 0) along +Y, its angle 0 facing -X
// (towards the wheel), and a tooth centred on y = 0 there — the frame
// HelicalSweep builds the worm in (pinned phase, docs/modeling/worm-gears.md
// #phase). The hob is the worm with its tip raised by the root clearance and
// its tooth thickened by the crowning; it is taken as infinitely long, which
// a real hob is wherever it can reach the wheel.
struct Hob {
  double a = 0, r1 = 0, r2 = 0, px = 0, pz = 0, sense = 1, tanA = 0;
  double halfThick = 0; // half the tooth's axial thickness at the pitch line
  double tipR = 0;

  // How far inside the hob, placed for a wheel turn of theta, the point
  // (x, y, z) lies (mm, positive inside): the rolling places it by turning
  // the wheel theta about Z and the worm the turn that screws its thread
  // r2*theta along its axis — so a point of the wheel is taken back through
  // both turns into the hob's own frame.
  double Margin(double x, double y, double z, double theta) const {
    const double c = std::cos(theta), s = std::sin(theta);
    const double qx = c * x + s * y, qy = -s * x + c * y;
    const double dx = qx - a;
    const double rho = std::hypot(dx, z);
    const double radial = tipR - rho;
    if (radial <= 0.0)
      return radial;
    const double phi = std::atan2(z, -dx);
    double d = qy - sense * pz * phi / (2.0 * M_PI) + r2 * theta;
    d -= px * std::floor(d / px + 0.5);
    const double hw = std::min(halfThick - (rho - r1) * tanA, 0.5 * px);
    return std::min(hw - std::abs(d), radial);
  }
};

// Whether the hob, somewhere along its rolling, cuts the wheel point at
// radius R, angle beta, height z: the best margin over the wheel turns at
// which the hob can reach it at all.
struct Envelope {
  Hob hob;
  double pitchAngle = 0.0;
  int stepsPerPitch = 0; // 0: follow the rolling continuously

  double Best(double x, double y, double z, double lo, double hi) const {
    double best = -1e300;
    if (stepsPerPitch > 0) {
      // The discrete hobbing loop: only the positions a hob stepped
      // pitch/n at a time stands at, offset by an irrational fraction.
      const double d = pitchAngle / stepsPerPitch;
      const double off = 0.2763932;
      for (long k = static_cast<long>(std::ceil(lo / d - off));
           (k + off) * d <= hi; k++)
        best = std::max(best, hob.Margin(x, y, z, (k + off) * d));
      return best;
    }
    const double h = pitchAngle / 48.0;
    const int n = std::max(2, static_cast<int>(std::ceil((hi - lo) / h)));
    double at = lo;
    for (int i = 0; i <= n; i++) {
      const double t = lo + (hi - lo) * i / n;
      const double v = hob.Margin(x, y, z, t);
      if (v > best) {
        best = v;
        at = t;
      }
    }
    // Golden section about the best sample: the margin is unimodal within a
    // step of its peak (a peak is a tooth passing, ~a tooth wide).
    const double g = 0.5 * (std::sqrt(5.0) - 1.0);
    double l = at - h, r = at + h;
    double c = r - g * (r - l), e = l + g * (r - l);
    double fc = hob.Margin(x, y, z, c), fe = hob.Margin(x, y, z, e);
    for (int i = 0; i < 40; i++) {
      if (fc > fe) {
        r = e;
        e = c;
        fe = fc;
        c = r - g * (r - l);
        fc = hob.Margin(x, y, z, c);
      } else {
        l = c;
        c = e;
        fc = fe;
        e = l + g * (r - l);
        fe = hob.Margin(x, y, z, e);
      }
    }
    return std::max(best, std::max(fc, fe));
  }

  double Cut(double R, double beta, double z) const {
    const double reach2 = hob.tipR * hob.tipR - z * z;
    if (reach2 <= 0.0)
      return -1.0;
    const double c = (hob.a - std::sqrt(reach2)) / R;
    if (c >= 1.0)
      return -1.0;
    const double w = std::acos(std::max(-1.0, c)) + pitchAngle / 48.0;
    return Best(R * std::cos(beta), R * std::sin(beta), z, beta - w, beta + w);
  }
};

// One transverse section of a tooth space: its two flanks, sampled at the
// same radii, as angles about the wheel's axis.
struct Section {
  std::vector<double> radii, left, right;
  bool ok = false;
  bool pointed = false; // the tooth vanished below the tip
};

// The tooth space at radius R of the section at height z: the two angles
// where the hob stops cutting, found outward from `mid` (an angle it cuts;
// searched for across a pitch when it does not), which is left at the
// space's middle for the next radius. `pointed` when a side runs half a pitch
// without meeting a tooth.
struct Span {
  bool ok = false, pointed = false;
  double l = 0.0, r = 0.0;
};
Span SpaceSpan(const Envelope& env, double R, double z, double& mid) {
  Span sp;
  const double p = env.pitchAngle;
  if (!(env.Cut(R, mid, z) > 0.0)) {
    double best = -1e300, at = mid;
    for (int k = 0; k <= 64; k++) {
      const double b = mid - 0.5 * p + p * k / 64.0;
      const double v = env.Cut(R, b, z);
      if (v > best) {
        best = v;
        at = b;
      }
    }
    if (!(best > 0.0))
      return sp; // nothing cut at this radius
    mid = at;
  }
  // Step outward from the cut angle until the hob no longer cuts, then
  // bisect. A side that runs most of a pitch without meeting a tooth has no
  // tooth left at this radius.
  const auto edge = [&](double inside, double dir, bool& pointed) {
    const double step = p / 64.0;
    double in = inside, out = inside;
    bool found = false;
    for (int k = 1; k <= 62; k++) {
      out = inside + dir * step * k;
      if (!(env.Cut(R, out, z) > 0.0)) {
        found = true;
        break;
      }
      in = out;
    }
    if (!found) {
      pointed = true;
      return inside + dir * 0.49 * p;
    }
    for (int k = 0; k < 36; k++) {
      const double m = 0.5 * (in + out);
      (env.Cut(R, m, z) > 0.0 ? in : out) = m;
    }
    return 0.5 * (in + out);
  };
  sp.l = edge(mid, -1.0, sp.pointed);
  sp.r = edge(mid, 1.0, sp.pointed);
  sp.ok = true;
  mid = 0.5 * (sp.l + sp.r);
  return sp;
}

// The tooth's crest width (mm, as an arc) at radius R of the section at z.
double ToothWidth(const Envelope& env, double R, double z, double mid) {
  const Span sp = SpaceSpan(env, R, z, mid);
  if (!sp.ok)
    return env.pitchAngle * R;
  return sp.pointed ? 0.0 : (env.pitchAngle - (sp.r - sp.l)) * R;
}

Section SpaceAt(const Envelope& env, double z, double rootR, double topR,
                double tipR, int points, double guess) {
  Section s;
  double mid = guess;
  for (int i = 0; i < points; i++) {
    const double u = static_cast<double>(i) / (points - 1);
    const double R = rootR + (topR - rootR) * std::pow(u, 1.6);
    const Span sp = SpaceSpan(env, R, z, mid);
    if (!sp.ok)
      return s; // the section is not a space
    if (sp.pointed && R <= tipR)
      s.pointed = true;
    s.radii.push_back(R);
    s.left.push_back(sp.l);
    s.right.push_back(sp.r);
  }
  s.ok = true;
  return s;
}

TopoDS_Wire SectionWire(const Section& s, double z, double tol) {
  const auto at = [&](double R, double b) {
    return gp_Pnt(R * std::cos(b), R * std::sin(b), z);
  };
  const int n = static_cast<int>(s.radii.size());
  Handle(TColgp_HArray1OfPnt) l = new TColgp_HArray1OfPnt(1, n);
  Handle(TColgp_HArray1OfPnt) r = new TColgp_HArray1OfPnt(1, n);
  for (int i = 0; i < n; i++) {
    l->SetValue(i + 1, at(s.radii[i], s.left[i]));
    r->SetValue(i + 1, at(s.radii[n - 1 - i], s.right[n - 1 - i]));
  }
  // Approximated, not interpolated: a flank is smooth, and a curve with a
  // pole per sample makes every face the loft builds from it that much
  // heavier for the booleans that follow.
  GeomAPI_PointsToBSpline li(l->Array1(), 3, 5, GeomAbs_C2, tol);
  GeomAPI_PointsToBSpline ri(r->Array1(), 3, 5, GeomAbs_C2, tol);
  if (!li.IsDone() || !ri.IsDone())
    return TopoDS_Wire();
  const Handle(Geom_BSplineCurve) lc = li.Curve(), rc = ri.Curve();
  const double top = s.radii.back(), bot = s.radii.front();
  const Handle(Geom_TrimmedCurve) topArc =
      GC_MakeArcOfCircle(at(top, s.left.back()),
                         at(top, 0.5 * (s.left.back() + s.right.back())),
                         at(top, s.right.back()))
          .Value();
  const Handle(Geom_TrimmedCurve) botArc =
      GC_MakeArcOfCircle(at(bot, s.right.front()),
                         at(bot, 0.5 * (s.left.front() + s.right.front())),
                         at(bot, s.left.front()))
          .Value();
  BRepBuilderAPI_MakeWire w;
  w.Add(BRepBuilderAPI_MakeEdge(lc).Edge());
  w.Add(BRepBuilderAPI_MakeEdge(topArc).Edge());
  w.Add(BRepBuilderAPI_MakeEdge(rc).Edge());
  w.Add(BRepBuilderAPI_MakeEdge(botArc).Edge());
  return w.IsDone() ? w.Wire() : TopoDS_Wire();
}

} // namespace

bool WheelCutTools(const gp_Ax3& frame, double halfFace, double blankRadius,
                   const PairSpec& spec, double crowning,
                   const HobOptions& options, WheelTools* out, HobStats* stats,
                   std::string* why) {
  const auto t0 = std::chrono::steady_clock::now();
  const std::string bad = Check(spec, true);
  if (!bad.empty()) {
    Fail(why, bad);
    return false;
  }
  if (options.stepsPerPitch < 0 || options.stepsPerPitch > 256) {
    Fail(why, "hobbing takes 0 (continuous) to 256 steps per wheel pitch");
    return false;
  }
  const Sizes sz = Size(spec);
  const double m = spec.module;
  const double a = sz.centreDistance;
  const double pitchAngle = 2.0 * M_PI / spec.teeth;
  if (blankRadius < sz.wheelThroatRadius) {
    Fail(why, Fmt("the blank (%.3f mm dia) does not reach the wheel's throat "
                  "(%.3f mm dia): the teeth would have no tips",
                  2.0 * blankRadius, 2.0 * sz.wheelThroatRadius));
    return false;
  }
  if (!(crowning >= 0.0) || crowning > 0.25 * m) {
    Fail(why, "the hob's crowning is 0 to a quarter of the module per flank");
    return false;
  }

  Envelope env;
  env.pitchAngle = pitchAngle;
  env.stepsPerPitch = options.stepsPerPitch;
  env.hob.a = a;
  env.hob.r1 = sz.wormPitchRadius;
  env.hob.r2 = sz.wheelPitchRadius;
  env.hob.px = sz.axialPitch;
  env.hob.pz = sz.lead;
  env.hob.sense = spec.rightHanded ? 1.0 : -1.0;
  env.hob.tanA = std::tan(angles::ToRadians(spec.pressureDeg));
  env.hob.halfThick = sz.axialPitch / 4.0 + crowning;
  env.hob.tipR = sz.wormPitchRadius + (1.0 + spec.clearance) * m;

  // The space's middle leans with the lead angle: a guess from it.
  const double lean = std::tan(sz.leadAngle) / sz.wheelPitchRadius *
                      (spec.rightHanded ? -1.0 : 1.0);
  // The tip: the usual r2 + 1.5 m (already held to the 45-degree throat),
  // lowered until the thinnest tooth — at a face end, where the throat does
  // not reach — keeps a printable crest a quarter of a module wide.
  const double crest = 0.25 * m;
  const auto crestAt = [&](double R) {
    return std::min(ToothWidth(env, R, halfFace, -lean * halfFace),
                    ToothWidth(env, R, -halfFace, lean * halfFace));
  };
  double tipR = sz.wheelTipRadius;
  if (crestAt(tipR) < crest) {
    double lo = sz.wheelThroatRadius, hi = tipR;
    for (int k = 0; k < 40; k++) {
      const double mid = 0.5 * (lo + hi);
      (crestAt(mid) >= crest ? lo : hi) = mid;
    }
    tipR = lo;
  }

  // The hob reaches the wheel's tip only this far either side of the
  // mid-plane; a wider face would leave its ends uncut.
  const double reach = std::sqrt(
      std::max(0.0, env.hob.tipR * env.hob.tipR - (a - tipR) * (a - tipR)));
  const double hb = halfFace + 0.25 * m; // past both faces: no coincident caps
  if (hb >= reach) {
    Fail(why, Fmt("the face (%.3f mm) is wider than the worm can cut: at "
                  "most %.3f mm at this worm diameter",
                  2.0 * halfFace, 2.0 * (reach - 0.25 * m)));
    return false;
  }
  // An even count: a section ON the mid-plane lies along the throat torus's
  // equator, and the loft's faces then meet the torus tangentially there —
  // measured, every odd count gave an invalid wheel.
  int slices =
      options.slices > 1
          ? options.slices
          : std::max(10, static_cast<int>(std::ceil(2.0 * hb / (0.4 * m))) + 1);
  slices += slices % 2;
  // Twelve points a flank: measured, 24 made the booleans that follow fail
  // on some face widths (a heavier surface for no gain — twelve are already
  // within 0.01% of the volume), and interpolating through them rather than
  // approximating cost eight times as long.
  const int points = options.flankPoints > 3 ? options.flankPoints : 12;
  const double topR = tipR + 0.2 * m;

  try {
    // Each section on its own worker: the envelope is plain arithmetic.
    std::vector<Section> sections(slices);
    std::vector<double> zs(slices);
    for (int j = 0; j < slices; j++)
      zs[j] = -hb + 2.0 * hb * j / (slices - 1);
    parallel::For(static_cast<size_t>(slices), [&](size_t j) {
      const double z = zs[j];
      const double rootR =
          a - std::sqrt(env.hob.tipR * env.hob.tipR - z * z) + 1e-3 * m;
      sections[j] = SpaceAt(env, z, rootR, topR, tipR, points, -lean * z);
    });
    for (const Section& s : sections) {
      if (!s.ok) {
        Fail(why, "the hob's envelope has a section with no tooth space: the "
                  "pair's sizes do not meet");
        return false;
      }
      if (s.pointed) {
        Fail(why, "the wheel's teeth come to a point under the tip: fewer "
                  "starts, more teeth or a lower pressure angle");
        return false;
      }
    }

    BRepOffsetAPI_ThruSections loft(Standard_True, Standard_False, 1e-6);
    loft.CheckCompatibility(Standard_False);
    loft.SetMaxDegree(5);
    for (int j = 0; j < slices; j++) {
      const TopoDS_Wire w = SectionWire(sections[j], zs[j], 2e-3 * m);
      if (w.IsNull()) {
        Fail(why, "a section of the tooth space could not be drawn");
        return false;
      }
      loft.AddWire(w);
    }
    loft.Build();
    if (!loft.IsDone()) {
      Fail(why, "the tooth space could not be lofted through its sections");
      return false;
    }
    TopoDS_Shape space = loft.Shape();
    double spaceVol = VolumeOf(space);
    if (spaceVol < 0.0) {
      space.Reverse();
      spaceVol = -spaceVol;
    }
    if (!(spaceVol > 0.0) || !BRepCheck_Analyzer(space).IsValid()) {
      Fail(why, "the lofted tooth space is not a valid solid");
      return false;
    }

    gp_Trsf place;
    place.SetDisplacement(gp_Ax3(), frame);
    const gp_Ax1 wheelAxis(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
    TopoDS_Compound spaces;
    BRep_Builder builder;
    builder.MakeCompound(spaces);
    for (int i = 0; i < spec.teeth; i++) {
      gp_Trsf r;
      r.SetRotation(wheelAxis, i * pitchAngle);
      gp_Trsf t = place;
      t.Multiply(r);
      builder.Add(spaces, BRepBuilderAPI_Transform(space, t, true).Shape());
    }

    // The throat, and the tip trimmed where the blank is larger.
    TopoDS_Shape rim =
        BRepPrimAPI_MakeTorus(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), a,
                              sz.throatRadius)
            .Shape();
    if (blankRadius > tipR + 1e-6) {
      const double h = hb + m;
      const TopoDS_Shape outer =
          BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -h), gp_Dir(0, 0, 1)),
                                   blankRadius + m, 2.0 * h)
              .Shape();
      const TopoDS_Shape inner =
          BRepPrimAPI_MakeCylinder(
              gp_Ax2(gp_Pnt(0, 0, -h - m), gp_Dir(0, 0, 1)), tipR,
              2.0 * (h + m))
              .Shape();
      BRepAlgoAPI_Cut ring;
      if (!cad::BuildNonDestructive(ring, outer, inner)) {
        Fail(why, "trimming the blank to the wheel's tip failed");
        return false;
      }
      BRepAlgoAPI_Fuse both;
      if (!cad::BuildNonDestructive(both, rim, ring.Shape())) {
        Fail(why, "joining the throat and the tip failed");
        return false;
      }
      rim = both.Shape();
    }
    out->rim = CADShape(BRepBuilderAPI_Transform(rim, place, true).Shape());
    out->spaces = CADShape(spaces);
    if (stats) {
      stats->slices = slices;
      stats->flankPoints = points;
      stats->spaceVolume = spaceVol;
      stats->tipRadius = tipR;
      {
        const double r0 = a - env.hob.tipR + 1e-3 * m;
        const int n = 200;
        double mid = 0.0, area = 0.0, prev = 0.0;
        for (int i = 0; i <= n; i++) {
          const double R = r0 + (tipR - r0) * i / n;
          const Span sp = SpaceSpan(env, R, 0.0, mid);
          const double w = sp.ok ? (sp.r - sp.l) * R : 0.0;
          if (i > 0)
            area += 0.5 * (w + prev) * (tipR - r0) / n;
          prev = w;
        }
        stats->midArea = area;
      }
      stats->tipCrest = crestAt(tipR);
      stats->seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
              .count();
    }
    LOG_INFO(
        "worm wheel: {} teeth, {} sections x {} flank points, {} hob "
        "steps/pitch, {:.2f} s",
        spec.teeth, slices, points, options.stepsPerPitch,
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count());
    return true;
  } catch (const Standard_Failure& e) {
    Fail(why, std::string("hobbing the wheel failed: ") + e.GetMessageString());
    return false;
  }
}
double RelationRatio(const AssemblyDocument& asmDoc, const Mate& wormMate,
                     const Mate& wheelMate, bool samePath, bool topLevel,
                     const glm::mat4& wormFrame, const glm::mat4& wheelFrame,
                     int starts, int teeth, bool rightHanded,
                     std::string* why) {
  if (starts <= 0 || teeth <= 0) {
    Fail(why, "starts and teeth are positive counts");
    return 0.0;
  }
  const glm::dvec3 u = glm::normalize(glm::dvec3(glm::vec3(wormFrame[2])));
  const glm::dvec3 w = glm::normalize(glm::dvec3(glm::vec3(wheelFrame[2])));
  const glm::dvec3 pu{glm::vec3(wormFrame[3])}, pw{glm::vec3(wheelFrame[3])};
  const glm::dvec3 n = glm::cross(u, w);
  const double off = glm::dot(pu - pw, n);
  if (glm::length(n) < 1e-3 || std::abs(off) < 1e-6) {
    Fail(why, "a worm sits beside its wheel, its axis crossing the wheel's at "
              "the centre distance; these two axes are parallel or meet");
    return 0.0;
  }
  // +1 when the gear is its mate's B side (it turns +dof about the A Z).
  const auto gearSide = [&](const Mate& m, const Mate& other) {
    const uint64_t ma = m.a.occurrenceId, mb = m.b.occurrenceId;
    if (samePath) {
      const uint64_t oa = other.a.occurrenceId, ob = other.b.occurrenceId;
      if (ma == oa || ma == ob)
        return 1.0; // the A side is the shared carrier
      if (mb == oa || mb == ob)
        return -1.0;
    }
    if (topLevel) {
      const Occurrence* A = asmDoc.FindById(ma);
      const Occurrence* B = asmDoc.FindById(mb);
      if (B != nullptr && B->fixed && !(A != nullptr && A->fixed))
        return -1.0;
    }
    return 1.0;
  };
  const double geometry = (off > 0.0 ? 1.0 : -1.0) *
                          gearSide(wormMate, wheelMate) *
                          gearSide(wheelMate, wormMate);
  return -geometry * (rightHanded ? 1.0 : -1.0) * starts /
         static_cast<double>(teeth);
}

} // namespace worm
