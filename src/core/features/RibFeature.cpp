// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/RibFeature.h"

#include "common/Logger.h"

#include "core/ElementHistory.h"
#include "core/features/BooleanFeature.h" // BooleanOp
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"
#include <BRep_Tool.hxx>
#include <Geom_Curve.hxx>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GeomAbs_JoinType.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <cfloat>

RibFeature::RibFeature(const std::string& name, uint64_t sketchId,
                       double thickness, bool flipDirection, uint64_t bodyId)
    : sketchId(sketchId), thickness(thickness), flipDirection(flipDirection),
      bodyId(bodyId) {
  m_name = name;
}

namespace {
// Bbox corner projections onto a direction: the [min, max] span of the shape
// along `dir`, for classifying cut components against the profile plane.
void ProjectionSpan(const CADShape& shape, const glm::dvec3& dir, double& lo,
                    double& hi) {
  glm::dvec3 mn, mx;
  shape.GetBoundingBox(mn, mx);
  lo = DBL_MAX;
  hi = -DBL_MAX;
  for (int c = 0; c < 8; c++) {
    const glm::dvec3 corner((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y,
                            (c & 4) ? mx.z : mn.z);
    const double p = glm::dot(corner, dir);
    lo = std::min(lo, p);
    hi = std::max(hi, p);
  }
}
} // namespace

bool RibFeature::Execute(RegenContext& ctx) {
  const auto* sk = dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (!sk) {
    SetError("rib needs a profile sketch (missing or suppressed)");
    return false;
  }
  // A sketch that failed this regeneration still holds its last plane and
  // profile; building from those puts the feature where the reference used
  // to be (a rib on a plane 9e15 mm out spun a boolean for minutes).
  if (!sk->GetError().empty()) {
    SetError("profile sketch '" + sk->GetName() +
             "' failed: " + sk->GetError());
    return false;
  }
  if (thickness <= 1e-9) {
    SetError("rib thickness must be positive");
    return false;
  }
  const TopoDS_Wire wire = sketch::BuildOpenPathWire(sk->plane, sk->entities);
  if (wire.IsNull()) {
    SetError("rib profile must be one open chain of lines/arcs");
    return false;
  }
  if (ctx.bodies.empty()) {
    SetError("rib needs a body to attach to");
    return false;
  }

  int target = -1;
  if (bodyId != 0) {
    for (size_t i = 0; i < ctx.bodies.size(); i++)
      if (ctx.bodies[i].createdBy == bodyId) {
        target = static_cast<int>(i);
        break;
      }
    if (target < 0) {
      SetError("rib target body no longer exists");
      return false;
    }
  }

  glm::dvec3 N = glm::normalize(sk->plane.GetNormal());
  if (flipDirection)
    N = -N;

  try {
    // The slab contour: closed offset of the open profile at thickness/2.
    // MakeOffset computes in 2D and misbehaves off the XY plane, so move the
    // wire into the sketch plane's local frame, offset there, and move the
    // result back. Round ends (GeomAbs_Arc) — a spanning rib's ends are cut
    // away by the walls it runs into anyway.
    const glm::dvec3 o = sk->plane.GetOrigin();
    const glm::dvec3 u = sk->plane.GetUAxis();
    const glm::dvec3 pn = sk->plane.GetNormal();
    const gp_Ax3 planeAx(gp_Pnt(o.x, o.y, o.z), gp_Dir(pn.x, pn.y, pn.z),
                         gp_Dir(u.x, u.y, u.z));
    gp_Trsf toLocal;
    toLocal.SetTransformation(planeAx); // world -> plane-local (plane = XY)
    TopoDS_Wire localWire = TopoDS::Wire(
        BRepBuilderAPI_Transform(wire, toLocal, /*copy=*/true).Shape());

    // OCC quirk: MakeOffset silently fails on a SINGLE-edge open wire (a lone
    // line/arc profile). Splitting the edge at its curve midpoint makes it a
    // 2-edge chain, which offsets fine.
    {
      int edgeCount = 0;
      TopoDS_Edge only;
      for (TopExp_Explorer ex(localWire, TopAbs_EDGE); ex.More(); ex.Next()) {
        edgeCount++;
        only = TopoDS::Edge(ex.Current());
      }
      if (edgeCount == 1) {
        Standard_Real p0 = 0.0, p1 = 0.0;
        Handle(Geom_Curve) curve = BRep_Tool::Curve(only, p0, p1);
        if (!curve.IsNull()) {
          const Standard_Real pm = 0.5 * (p0 + p1);
          BRepBuilderAPI_MakeWire mkw(
              BRepBuilderAPI_MakeEdge(curve, p0, pm).Edge(),
              BRepBuilderAPI_MakeEdge(curve, pm, p1).Edge());
          if (mkw.IsDone())
            localWire = mkw.Wire();
        }
      }
    }

    BRepOffsetAPI_MakeOffset off;
    off.Init(GeomAbs_Arc);
    off.AddWire(localWire);
    off.Perform(thickness / 2.0);
    if (!off.IsDone()) {
      SetError("rib profile offset failed");
      return false;
    }
    TopoDS_Wire contour;
    for (TopExp_Explorer ex(off.Shape(), TopAbs_WIRE); ex.More(); ex.Next()) {
      contour = TopoDS::Wire(ex.Current());
      break;
    }
    if (contour.IsNull()) {
      SetError("rib profile offset produced no contour");
      return false;
    }
    contour = TopoDS::Wire(
        BRepBuilderAPI_Transform(contour, toLocal.Inverted(), /*copy=*/true)
            .Shape());

    const gp_Pln pln(planeAx);
    BRepBuilderAPI_MakeFace mkFace(pln, contour);
    if (!mkFace.IsDone()) {
      SetError("rib slab face construction failed");
      return false;
    }

    // Long enough to pass fully through anything it could hit: every body's
    // combined bbox plus the profile's distance to it (the hole through-all
    // recipe, with margin).
    glm::dvec3 lo(DBL_MAX), hi(-DBL_MAX);
    for (const Body& b : ctx.bodies) {
      glm::dvec3 mn, mx;
      b.shape.GetBoundingBox(mn, mx);
      lo = glm::min(lo, mn);
      hi = glm::max(hi, mx);
    }
    const double L = 2.0 * (glm::length(glm::dvec3(hi - lo)) +
                            glm::length(o - glm::dvec3(lo + hi) * 0.5)) +
                     thickness + 1.0;

    BRepPrimAPI_MakePrism prism(mkFace.Face(),
                                gp_Vec(N.x * L, N.y * L, N.z * L));
    if (!prism.IsDone()) {
      SetError("rib prism construction failed");
      return false;
    }
    CADShape slab(prism.Shape());
    if (!slab.IsValid()) {
      SetError("rib slab is invalid (thickness too large for the profile?)");
      return false;
    }

    if (target < 0) {
      // Nearest overlapping body (the ApplyBodyMode::Add rule), else last.
      glm::dvec3 smin, smax;
      slab.GetBoundingBox(smin, smax);
      target = static_cast<int>(ctx.bodies.size()) - 1;
      for (int i = static_cast<int>(ctx.bodies.size()) - 1; i >= 0; i--) {
        glm::dvec3 bmin, bmax;
        ctx.bodies[i].shape.GetBoundingBox(bmin, bmax);
        const bool overlap = smin.x <= bmax.x && smax.x >= bmin.x &&
                             smin.y <= bmax.y && smax.y >= bmin.y &&
                             smin.z <= bmax.z && smax.z >= bmin.z;
        if (overlap) {
          target = i;
          break;
        }
      }
    }
    Body& body = ctx.bodies[target];

    // Up-to-body trim: what remains of the slab outside the body, split into
    // solids; the component(s) touching the profile plane are the rib.
    ElementMap cutMap;
    const CADShape cut = topo::BooleanNamed(BooleanOp::Subtract, slab, {},
                                            body.shape, {}, GetId(), cutMap);
    if (!cut.IsValid()) {
      SetError("rib trim against the body failed");
      return false;
    }
    const double base = glm::dot(o, N); // every plane point projects here
    const double eps = std::max(1e-4 * L, 1e-5);
    std::vector<CADShape> kept;
    for (CADShape& comp : cut.SplitSolids()) {
      double clo, chi;
      ProjectionSpan(comp, N, clo, chi);
      if (clo - base > eps)
        continue; // starts past the plane: beyond the body, not rib material
      if (chi - base > L - eps) {
        SetError("rib profile does not reach the body");
        return false;
      }
      kept.push_back(std::move(comp));
    }
    if (kept.empty()) {
      SetError("rib is entirely inside the body");
      return false;
    }

    // Fresh primitive-style names (rib faces come from the offset contour,
    // not sketch entities), then fuse into the target with propagation.
    CADShape fused = body.shape;
    ElementMap fusedMap = body.elementMap;
    for (size_t i = 0; i < kept.size(); i++) {
      ElementMap m;
      const std::string tag =
          std::to_string(GetId()) + (i == 0 ? "" : "b" + std::to_string(i));
      topo::NamePrimitiveElements(kept[i], tag, m);
      ElementMap outMap;
      CADShape f = topo::BooleanNamed(BooleanOp::Union, fused, fusedMap,
                                      kept[i], m, GetId(), outMap);
      if (!f.IsValid()) {
        SetError("rib fuse failed");
        return false;
      }
      fused = std::move(f);
      fusedMap = std::move(outMap);
    }
    body.shape = std::move(fused);
    body.elementMap = std::move(fusedMap);
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("rib failed: ") + e.GetMessageString());
    LOG_ERROR("RibFeature '{}': {}", m_name, GetError());
    return false;
  }
}
