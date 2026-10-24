// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/SweepFeature.h"

#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"
#include "core/Measure.h"
#include "core/features/CurveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepFill_TypeOfContact.hxx>
#include <BRepOffsetAPI_MakePipeShell.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

TopoDS_Wire ResolveSweepPathWire(const RegenContext& ctx, uint64_t pathId) {
  const Feature* f = ctx.FindById(pathId);
  if (!f)
    return TopoDS_Wire();
  if (const auto* sk = dynamic_cast<const SketchFeature*>(f))
    return sketch::BuildOpenPathWire(sk->plane, sk->entities);
  if (const auto* cv = dynamic_cast<const CurveFeature*>(f)) {
    const TopoDS_Shape& shape = cv->EdgeShape().GetShape();
    if (shape.IsNull())
      return TopoDS_Wire();
    // A bent path is several edges — straights and arcs — so the curve holds
    // a WIRE, and a spine is a wire either way.
    if (shape.ShapeType() == TopAbs_WIRE)
      return TopoDS::Wire(shape);
    if (shape.ShapeType() != TopAbs_EDGE)
      return TopoDS_Wire();
    BRepBuilderAPI_MakeWire wire(TopoDS::Edge(shape));
    return wire.IsDone() ? wire.Wire() : TopoDS_Wire();
  }
  return TopoDS_Wire();
}

std::string SweepPathError(const RegenContext& ctx, uint64_t pathId,
                           const std::string& role) {
  const Feature* f = ctx.FindById(pathId);
  if (f != nullptr && !f->GetError().empty())
    return role + " '" + f->GetName() +
           "' failed to build, so there is nothing to sweep along: fix that "
           "feature first";
  return role + " must be an open sketch chain or a 3D curve";
}

CADShape SweepSectionsAlongSpine(const std::vector<TopoDS_Wire>& profiles,
                                 const TopoDS_Wire& spine, SweepFrame frame,
                                 const TopoDS_Wire& guide, bool scaleToGuide,
                                 const gp_Dir& binormal) {
  if (profiles.empty() || spine.IsNull())
    return CADShape();
  try {
    // MakePipeShell (not MakePipe) so sharp path corners are handled: a plain
    // MakePipe truncates the sweep at the first corner. RoundCorner rounds the
    // spine's corners; the trihedron mode controls how the section is carried.
    // Multiple profiles skin a solid through all sections along the spine (the
    // guided-loft path, mirroring FreeCAD's PartDesign Pipe).
    BRepOffsetAPI_MakePipeShell shell(spine);
    shell.SetTransitionMode(BRepBuilderAPI_RoundCorner);
    if (!guide.IsNull()) {
      // A guide curve (auxiliary spine) supersedes the frame: it carries the
      // sections as they travel — the "two directions" sweep/loft.
      // ContactOnBorder makes them scale to touch the guide; NoContact only
      // reorients them.
      shell.SetMode(guide, /*CurvilinearEquivalence=*/Standard_True,
                    scaleToGuide ? BRepFill_ContactOnBorder
                                 : BRepFill_NoContact);
    } else {
      switch (frame) {
      case SweepFrame::Frenet:
        shell.SetMode(/*IsFrenet=*/Standard_True);
        break;
      case SweepFrame::CorrectedFrenet:
        shell.SetMode(/*IsFrenet=*/Standard_False);
        break;
      case SweepFrame::Fixed:
        break; // leave OCC's default discrete trihedron
      case SweepFrame::Binormal:
        shell.SetMode(binormal);
        break;
      }
    }
    for (const TopoDS_Wire& p : profiles) {
      if (p.IsNull())
        return CADShape();
      shell.Add(p);
    }
    shell.Build();
    if (!shell.IsDone() || !shell.MakeSolid())
      return CADShape();
    return CADShape(shell.Shape());
  } catch (const Standard_Failure&) {
    return CADShape();
  }
}

CADShape SweepProfileAlongSpine(const TopoDS_Wire& profile,
                                const TopoDS_Wire& spine, SweepFrame frame,
                                const TopoDS_Wire& guide,
                                const gp_Dir& binormal) {
  return SweepSectionsAlongSpine(std::vector<TopoDS_Wire>{profile}, spine,
                                 frame, guide, /*scaleToGuide=*/false,
                                 binormal);
}

SweepFeature::SweepFeature(const std::string& name, uint64_t profileSketchId,
                           uint64_t pathSketchId, ExtrudeMode mode)
    : profileSketchId(profileSketchId), pathSketchId(pathSketchId), mode(mode) {
  m_name = name;
}

bool SweepFeature::Execute(RegenContext& ctx) {
  const auto* prof =
      dynamic_cast<const SketchFeature*>(ctx.FindById(profileSketchId));
  if (!prof) {
    SetError("sweep needs a profile sketch (missing or suppressed)");
    return false;
  }
  // A sketch that failed this regeneration still holds its last plane and
  // profile; building from those puts the feature where the reference used
  // to be (a rib on a plane 9e15 mm out spun a boolean for minutes).
  if (!prof->GetError().empty()) {
    SetError("profile sketch '" + prof->GetName() +
             "' failed: " + prof->GetError());
    return false;
  }

  const auto loops = sketch::BuildLoops(prof->entities);
  const TopoDS_Wire profileWire = sketch::BuildProfileWire(prof->plane, loops);
  if (profileWire.IsNull()) {
    SetError("sweep profile has no single closed loop");
    return false;
  }
  // The path can be a sketch (lines/arcs/splines) or a Curve3D (fit-spline,
  // helix, control-poly) — curves-3d Phase 1.
  const TopoDS_Wire spine = ResolveSweepPathWire(ctx, pathSketchId);
  if (spine.IsNull()) {
    SetError(SweepPathError(ctx, pathSketchId, "sweep path"));
    return false;
  }
  // Optional guide curve ("two directions", curves-3d Phase 3): a second
  // sketch/Curve3D that orients the section along the sweep.
  TopoDS_Wire guide;
  if (guideId != 0) {
    guide = ResolveSweepPathWire(ctx, guideId);
    if (guide.IsNull()) {
      SetError(SweepPathError(ctx, guideId, "sweep guide"));
      return false;
    }
  }

  CADShape result = SweepProfileAlongSpine(profileWire, spine, frame, guide);
  // A sweep can also come back as something OCC calls a solid, with a
  // plausible volume, whose faces are NOT CONNECTED to one another — one
  // loose piece per path edge. A profile 7 degrees off perpendicular to the
  // spine start is enough to produce it, and the kernel says nothing:
  // MakePipeShell reports done, MakeSolid returns true, the body's digest
  // counts one solid, and the STEP writes a solid that no reader can read
  // back as one. Only BRepCheck sees it. Refusing here is what stops a part
  // that looks right in the viewport from being exported as loose surfaces.
  const bool disconnected =
      result.IsValid() && !Measure::CheckGeometry(result).valid;
  if (!result.IsValid() || result.Volume() < 1e-9 || disconnected) {
    if (disconnected) {
      const Measure::GeometryReport rep = Measure::CheckGeometry(result);
      std::string why;
      for (const std::string& r : rep.reasons)
        why += (why.empty() ? "" : ", ") + r;
      SetError("sweep produced an unusable solid (" +
               (why.empty() ? std::string("failed B-Rep validity") : why) +
               ") — the profile must be perpendicular to the path where the "
               "path STARTS; a few degrees off is enough");
      return false;
    }
    SetError("sweep failed (profile not perpendicular to the path start?)");
    return false;
  }
  ElementMap map;
  topo::NamePrimitiveElements(result, std::to_string(GetId()), map);
  return ApplyBodyModeReporting(ctx.bodies, std::move(result), std::move(map),
                                mode, *this);
}
