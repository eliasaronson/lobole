// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/ThreadFeature.h"

#include "common/Logger.h"
#include "core/Angles.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/HelicalSweep.h"
#include "core/ThreadStandards.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepTools_History.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

// A square end's footprint, merged away. The tool's end face lies ON the
// shoulder (or a blind hole's floor), so the cut draws its outline there:
// arcs that split one plane into coplanar pieces and bound no volume. The
// pieces are merged -- only planar faces the cut made (not faces of `body`
// as it was), across edges with such a face on both sides; every other edge
// is kept, so no other face is rebuilt and none is renamed. A whole-body
// Refine would merge them too, and name every face afresh.
TopoDS_Shape MergeFootprint(const TopoDS_Shape& body,
                            const TopoDS_Shape& result,
                            BRepTools_History& history) {
  TopTools_IndexedMapOfShape original;
  TopExp::MapShapes(body, TopAbs_FACE, original);
  const auto mergeable = [&](const TopoDS_Shape& f) {
    return !original.Contains(f) &&
           BRepAdaptor_Surface(TopoDS::Face(f), Standard_False).GetType() ==
               GeomAbs_Plane;
  };
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndUniqueAncestors(result, TopAbs_EDGE, TopAbs_FACE,
                                      edgeFaces);
  TopTools_MapOfShape keep;
  bool any = false;
  for (int i = 1; i <= edgeFaces.Extent(); i++) {
    const TopTools_ListOfShape& faces = edgeFaces(i);
    bool both = faces.Extent() == 2;
    for (const TopoDS_Shape& f : faces)
      both = both && mergeable(f);
    if (both)
      any = true;
    else
      keep.Add(edgeFaces.FindKey(i));
  }
  if (!any)
    return TopoDS_Shape(); // nothing drawn on a plane: as it is
  // The edges stay as they are, so a face is only rebuilt by being merged.
  // A merge the kernel cannot do leaves the cut as it was: the arcs are
  // untidy, not wrong.
  try {
    ShapeUpgrade_UnifySameDomain unify(result, /*UnifyEdges=*/Standard_False,
                                       /*UnifyFaces=*/Standard_True,
                                       /*ConcatBSplines=*/Standard_False);
    unify.KeepShapes(keep);
    unify.Build();
    if (unify.Shape().IsNull())
      return TopoDS_Shape();
    history.Merge(unify.History());
    return unify.Shape();
  } catch (const Standard_Failure& e) {
    LOG_WARN("modelled thread: the footprint on the shoulder was not merged "
             "({})",
             e.GetMessageString());
    return TopoDS_Shape();
  }
}

// The groove a modelled thread cuts, as an axial profile for HelicalSweep:
// the space between two teeth of the ISO 68-1 / ASME B1.1 basic profile
// (60 degree flanks, fundamental triangle H = P*sqrt(3)/2), in the half-plane
// through the axis, x = radius and y = height about the helix point. Another
// flank angle `a` (a custom thread's) keeps the profile's proportions: the
// fundamental triangle is H = P / (2 tan a) and the flats stay P/8 and P/4,
// since a flat k*H from the apex is 2 k H tan a = k P wide.
//
// External (a boss): the root flat P/4 wide at the basic minor radius
// D/2 - 5H/8, the flanks opening outward past the crest to `reach` so the
// cut leaves no sliver of the blank. Internal (a hole wall): the root flat
// P/8 at the major radius D/2, the flanks opening inward past the wall.
// Either way the groove is capped just under a pitch wide, so neighbouring
// turns never touch.
std::vector<gp_Pnt2d> GrooveProfile(double pitch, double rootRadius,
                                    double reach, bool external,
                                    double flankDeg) {
  const double tanFlank = std::tan(angles::ToRadians(flankDeg));
  const double halfRoot = external ? pitch / 8.0 : pitch / 16.0;
  const double halfCap = 0.475 * pitch;      // 0.95 P: clear of the next turn
  const double sign = external ? 1.0 : -1.0; // which way the groove opens
  const double span = std::abs(reach - rootRadius);
  const double toCap = (halfCap - halfRoot) / tanFlank; // flank meets the cap
  std::vector<gp_Pnt2d> out;
  out.emplace_back(rootRadius, -halfRoot);
  if (toCap < span) {
    out.emplace_back(rootRadius + sign * toCap, -halfCap);
    out.emplace_back(reach, -halfCap);
    out.emplace_back(reach, halfCap);
    out.emplace_back(rootRadius + sign * toCap, halfCap);
  } else {
    const double h = halfRoot + span * tanFlank;
    out.emplace_back(reach, -h);
    out.emplace_back(reach, h);
  }
  out.emplace_back(rootRadius, halfRoot);
  return out;
}

} // namespace

ThreadFeature::ThreadFeature(const std::string& name, ElementRef face,
                             std::string designation, double pitch)
    : face(std::move(face)), designation(std::move(designation)), pitch(pitch) {
  m_name = name;
}

std::string ThreadFeature::PitchRefusal() const {
  std::string why;
  const auto resolved = threads::Resolve(designation, &why);
  if (resolved)
    return threads::PitchError(resolved->standard, designation, pitch);
  if (!why.empty())
    return {}; // not a row at all: WhyRefused says so, pitch aside
  if (!(pitch > 1e-6) || !std::isfinite(pitch))
    return "custom thread '" + designation +
           "' needs its pitch, a positive number of mm (a table "
           "designation such as M8 or 3/8-16 UNC takes its row's)";
  return {};
}

std::string ThreadFeature::WhyRefused() const {
  // A designation that names a series we carry must be in its table; one
  // that names none is a custom thread with its own pitch.
  std::string why;
  const auto resolved = threads::Resolve(designation, &why);
  if (!resolved && !why.empty())
    return why;
  if (resolved) {
    if (std::abs(flankDeg - threads::kIsoFlankDeg) > 1e-9) {
      char buf[200];
      snprintf(buf, sizeof(buf),
               "%s is the standard's 60-degree profile; a %g-degree flank "
               "is a custom thread (a designation that names no series, with "
               "its own pitch)",
               designation.c_str(), flankDeg);
      return buf;
    }
    if (!resolved->threadClass.empty() && !threadClass.empty() &&
        threadClass != resolved->threadClass)
      return "the designation says class " + resolved->threadClass +
             " and thread_class says " + threadClass + "; give one";
  }
  // The table's pitch or none: another is refused, never replaced by the
  // table's (docs/modeling/holes-and-threads.md#thread).
  if (std::string e = PitchRefusal(); !e.empty())
    return e;
  if (!(flankDeg >= threads::kMinFlankDeg &&
        flankDeg <= threads::kMaxFlankDeg)) {
    char buf[160];
    snprintf(buf, sizeof(buf),
             "flank angle %g degrees is outside %g-%g (30 is the ISO "
             "profile, 45 prints without support)",
             flankDeg, threads::kMinFlankDeg, threads::kMaxFlankDeg);
    return buf;
  }
  return {};
}

bool ThreadFeature::Execute(RegenContext& ctx) {
  overlay = {};
  if (face.name.empty())
    return true; // previewing: no face picked yet
  if (const std::string why = WhyRefused(); !why.empty()) {
    SetError(why);
    return false;
  }

  // The body that holds the face: a pattern's copies and a split's pieces
  // share the createdBy, and the first of them is not always it.
  std::string why;
  Body* const body = topo::FindOwningBody(ctx, face, topo::ElementKind::Face,
                                          "threaded face", &why);
  if (body == nullptr) {
    SetError(why);
    return false;
  }
  const auto idx = body->elementMap.Resolve(face.name);
  TopoDS_Face occFace;
  int i = 0;
  for (TopExp_Explorer ex(body->shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++)
    if (i == *idx) {
      occFace = TopoDS::Face(ex.Current());
      break;
    }
  if (occFace.IsNull()) {
    SetError("threaded face could not be resolved");
    return false;
  }

  try {
    BRepAdaptor_Surface surf(occFace);
    if (surf.GetType() != GeomAbs_Cylinder) {
      SetError("thread needs a cylindrical face");
      return false;
    }
    const gp_Cylinder cyl = surf.Cylinder();
    const double r = cyl.Radius();

    // WhyRefused has accepted the designation, its pitch and its class.
    const auto resolved = threads::Resolve(designation, nullptr);
    std::string cls = threadClass;
    if (resolved && !resolved->threadClass.empty())
      cls = resolved->threadClass;

    // A known designation must roughly fit the face it annotates.
    if (resolved) {
      const threads::Standard& std_ = resolved->standard;
      const double lo = threads::MinorDiameter(std_) * 0.5 * 0.8;
      const double hi = std_.majorDia * 0.5 * 1.2;
      if (r < lo || r > hi) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "%s does not fit this face (face %.3f, thread %.3f dia)",
                 designation.c_str(), 2.0 * r, std_.majorDia);
        SetError(buf);
        return false;
      }
    }

    // External boss vs hole wall: the face normal at mid-parameters points
    // away from the axis on a boss, toward it inside a hole.
    const double uMid = 0.5 * (surf.FirstUParameter() + surf.LastUParameter());
    const double vMid = 0.5 * (surf.FirstVParameter() + surf.LastVParameter());
    BRepGProp_Face gface(occFace);
    gp_Pnt p;
    gp_Vec n;
    gface.Normal(uMid, vMid, p, n);
    const gp_Pnt axisLoc = cyl.Location();
    const gp_Dir axisDir = cyl.Axis().Direction();
    const gp_Vec toP(axisLoc, p);
    const gp_Vec axial(axisDir.XYZ() * toP.Dot(gp_Vec(axisDir)));
    const gp_Vec radial = toP - axial;
    const bool external = n.Dot(radial) > 0.0;

    // The threaded span along the axis: the face's V range (V is axial for a
    // cylinder), optionally clipped to `length` from the V-min end.
    double faceV0 = surf.FirstVParameter(), faceV1 = surf.LastVParameter();
    if (faceV1 < faceV0)
      std::swap(faceV0, faceV1);
    const double v0 = faceV0;
    double v1 = faceV1;
    if (length > 1e-9)
      v1 = std::min(v1, v0 + length);

    // Drafting convention: the dashed circle sits at the thread depth —
    // minor diameter on a boss, major diameter inside a hole. Both are
    // pitch-derived (the basic profile's fundamental triangle at this flank
    // angle), so custom designations work without a table row.
    const double depth = threads::BasicDepth(pitch, flankDeg);
    overlay.valid = true;
    overlay.origin = glm::vec3(axisLoc.X(), axisLoc.Y(), axisLoc.Z());
    overlay.dir =
        glm::normalize(glm::vec3(axisDir.X(), axisDir.Y(), axisDir.Z()));
    overlay.radius = static_cast<float>(external ? r - depth : r + depth);
    overlay.faceRadius = static_cast<float>(r);
    overlay.v0 = static_cast<float>(v0);
    overlay.v1 = static_cast<float>(v1);
    overlay.external = external;

    if (resolved) {
      const std::string bad =
          threads::ClassError(resolved->standard, cls, external);
      if (!bad.empty()) {
        SetError(bad);
        return false;
      }
    }

    if (modeled) {
      // The real groove. Any failure is this feature's error: the body stays
      // as it was and the cosmetic overlay above stays drawn.
      const double major = resolved ? resolved->standard.majorDia : 0.0;
      if (!CutModeled(*body, cyl, external, v0, v1, faceV1, major))
        return false;
      overlay.cut = true;
    }
    // What the thread IS, on the body, for whatever reads bodies rather than
    // features: the CAM thread mill (docs/cam/operations.md#thread-mill).
    body->threads.push_back(
        {face, GetId(), resolved ? resolved->standard.designation : designation,
         resolved ? resolved->standard.majorDia : 0.0, pitch, cls, rightHanded,
         external, length, modeled, flankDeg});
    if (allowEngagement) {
      if (!resolved || minimumEngagement < 0 ||
          !std::isfinite(minimumEngagement)) {
        SetError("engagement needs a known thread designation and nonnegative "
                 "minimum length");
        return false;
      }
      body->threadRegions.push_back(
          {face, designation, length, minimumEngagement, external});
    }
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("thread failed: ") + e.GetMessageString());
    LOG_ERROR("ThreadFeature '{}': {}", m_name, GetError());
    return false;
  }
}

bool ThreadFeature::CutModeled(Body& body, const gp_Cylinder& cyl,
                               bool external, double v0, double v1,
                               double faceV1, double tableMajor) {
  const auto t0 = std::chrono::steady_clock::now();
  const double r = cyl.Radius();
  // 5H/8, the basic profile's depth at this flank angle.
  const double depth = threads::BasicDepth(pitch, flankDeg);
  // The major diameter: the table's, else the face's own on a boss, and the
  // face taken as the basic minor in a hole.
  const double major = tableMajor > 0.0 ? tableMajor
                       : external       ? 2.0 * r
                                        : 2.0 * r + 2.0 * depth;
  const double margin = 0.1 * pitch; // past the blank, so no sliver is left
  double root = 0.0, reach = 0.0, probe = 0.0;
  char buf[160];
  if (external) {
    root = major / 2.0 - depth;
    reach = std::max(r, major / 2.0) + margin;
    probe = 0.5 * (root + r);
    if (root <= 1e-6 || root >= r) {
      snprintf(buf, sizeof(buf),
               "the modelled thread's root (%.3f dia) is not inside this "
               "%.3f mm boss",
               2.0 * root, 2.0 * r);
      SetError(buf);
      return false;
    }
  } else {
    root = major / 2.0;
    reach = std::min(r, major / 2.0 - depth) - margin;
    probe = 0.5 * (r + root);
    if (root <= r || reach <= 1e-6) {
      snprintf(buf, sizeof(buf),
               "the modelled thread's root (%.3f dia) is not outside this "
               "%.3f mm hole",
               2.0 * root, 2.0 * r);
      SetError(buf);
      return false;
    }
  }

  // A span end that is the face's own end and opens onto air is left open:
  // the groove runs out through the end face instead of stopping flush with
  // it (coincident faces are what booleans get wrong), and the tool needs no
  // clip there. An end against material — a boss's shoulder, a blind hole's
  // floor — or a `length` that stops mid-face is cut square. "Air" is asked
  // of the body over the whole reach an open end's cutter has (up to two
  // pitches past it) at four angles, in the groove's radial band.
  const gp_Pnt o = cyl.Location();
  const gp_Dir d = cyl.Axis().Direction();
  const gp_Dir x = cyl.Position().XDirection();
  const gp_Dir y = cyl.Position().YDirection();
  const auto opensOnAir = [&](double v, double outward) {
    for (int k = 1; k <= 4; k++)
      for (int a = 0; a < 4; a++) {
        const gp_Vec radial = gp_Vec(x) * std::cos(a * M_PI / 2.0) +
                              gp_Vec(y) * std::sin(a * M_PI / 2.0);
        const gp_Pnt p = o.Translated(
            gp_Vec(d) * (v + outward * 0.5 * k * pitch) + radial * probe);
        if (body.shape.ContainsPoint(glm::dvec3(p.X(), p.Y(), p.Z())))
          return false;
      }
    return true;
  };
  const bool openStart = opensOnAir(v0, -1.0);
  const bool openEnd = std::abs(v1 - faceV1) < 1e-9 && opensOnAir(v1, 1.0);

  HelicalSweepSpec spec;
  spec.frame = gp_Ax3(o, d, x);
  spec.z0 = v0;
  spec.z1 = v1;
  spec.squareStart = !openStart;
  spec.squareEnd = !openEnd;
  spec.lead = pitch;
  spec.starts = 1;
  spec.rightHanded = rightHanded;
  // Not on the face's seam (u = 0): a cutter face in the seam plane makes the
  // boolean intersect along the seam edge.
  spec.startRad = 0.5 * M_PI;
  spec.profile = GrooveProfile(pitch, root, reach, external, flankDeg);
  std::string why;
  CADShape tool = HelicalSweep(spec, &why);
  if (!tool.IsValid()) {
    SetError("the modelled thread could not be built (" + why +
             "); the thread stays cosmetic");
    return false;
  }
  ElementMap toolMap;
  topo::NamePrimitiveElements(tool, std::to_string(GetId()), toolMap);
  const TopoDS_Shape blank = body.shape.GetShape();
  CheckedBoolean cut = CheckedBooleanNamed(
      m_name, BooleanOp::Subtract, body.name, body.shape, body.elementMap, tool,
      toolMap, GetId(), {},
      [&](const TopoDS_Shape& result, BRepTools_History& history) {
        return MergeFootprint(blank, result, history);
      });
  if (!cut.refusal.empty() || !cut.shape.IsValid()) {
    SetError("the modelled thread's cut failed (" +
             (cut.refusal.empty() ? cut.error : cut.refusal) +
             "); the thread stays cosmetic");
    return false;
  }
  if (cut.fuzzy > 0.0)
    SetWarning(cut.warning);
  body.shape = std::move(cut.shape);
  body.elementMap = std::move(cut.map);
  LOG_INFO("ThreadFeature '{}': modelled {:.1f} turns in {} ms", m_name,
           (v1 - v0) / pitch,
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
               .count());
  return true;
}
