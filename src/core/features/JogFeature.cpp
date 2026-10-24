// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/JogFeature.h"
#include "cad/NonDestructiveBoolean.h"

#include "core/Angles.h"
#include "core/ElementHistory.h"
#include "core/features/SketchFeature.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace {

gp_Pnt P(const glm::dvec3& v) { return gp_Pnt(v.x, v.y, v.z); }
glm::dvec3 G(const gp_XYZ& v) { return {v.X(), v.Y(), v.Z()}; }

TopoDS_Face Rect(const gp_Pnt& o, const gp_Vec& a, const gp_Vec& b) {
  BRepBuilderAPI_MakePolygon poly(o, o.Translated(a), o.Translated(a + b),
                                  o.Translated(b), Standard_True);
  return BRepBuilderAPI_MakeFace(poly.Wire(), Standard_True).Face();
}

// Everything of `s` on the +`n` side of the plane through `at`.
CADShape Side(const CADShape& s, const gp_Pnt& at, const gp_Dir& n,
              double size) {
  const gp_Dir a = std::abs(n.Z()) < 0.9
                       ? gp_Dir(gp_Vec(n).Crossed(gp_Vec(0, 0, 1)))
                       : gp_Dir(gp_Vec(n).Crossed(gp_Vec(1, 0, 0)));
  const gp_Dir b(gp_Vec(n).Crossed(gp_Vec(a)));
  const CADShape half(
      BRepPrimAPI_MakePrism(
          Rect(at.Translated(-gp_Vec(a) * size - gp_Vec(b) * size),
               gp_Vec(a) * (2.0 * size), gp_Vec(b) * (2.0 * size)),
          gp_Vec(n) * size)
          .Shape());
  BRepAlgoAPI_Common op;
  if (!cad::BuildNonDestructive(op, s.GetShape(), half.GetShape()))
    return {};
  CADShape out(op.Shape());
  return out.IsValid() && out.Volume() > 1e-12 ? out : CADShape{};
}

bool Inside(const CADShape& s, const gp_Pnt& p) {
  BRepClass3d_SolidClassifier c(s.GetShape(), p, 1e-6);
  return c.State() == TopAbs_IN;
}

} // namespace

JogFeature::JogFeature(const std::string& name, uint64_t sketchId,
                       double height)
    : sketchId(sketchId), height(height) {
  m_name = name;
}

bool JogFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules - set the material and gauge "
             "first (set_sheet_metal_rules)");
    return false;
  }
  const sheet::SheetMetalRules& rules = *ctx.sheetMetal;
  const double t = rules.thickness;
  const auto* sketch =
      dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (sketch == nullptr) {
    SetError("the jog's sketch (id " + std::to_string(sketchId) +
             ") is not earlier in the history");
    return false;
  }
  // A sketch that failed this regeneration still holds its last plane and
  // profile; building from those puts the feature where the reference used
  // to be (a rib on a plane 9e15 mm out spun a boolean for minutes).
  if (!sketch->GetError().empty()) {
    SetError("the jog's sketch '" + sketch->GetName() +
             "' failed: " + sketch->GetError());
    return false;
  }
  const sketch::Line* line = nullptr;
  int lines = 0;
  for (const sketch::Entity& ent : sketch->entities)
    if (const auto* l = std::get_if<sketch::Line>(&ent);
        l != nullptr && !l->construction) {
      line = l;
      ++lines;
    }
  if (lines != 1) {
    SetError("a jog's sketch holds exactly one line - it has " +
             std::to_string(lines));
    return false;
  }
  const double theta = angles::ToRadians(angleDeg);
  if (theta <= 1e-6 || theta > angles::kPi / 2.0 + 1e-9) {
    SetError("a jog climbs at more than 0 and at most 90 degrees (got " +
             std::to_string(angleDeg) + ")");
    return false;
  }
  const double R = rules.EffectiveBendRadius();
  // Face to face, the two bends alone climb (2R + t)(1 - cos a); the wall
  // between them makes up the rest. Under that there is no wall to make.
  const double floor = (2.0 * R + t) * (1.0 - std::cos(theta));
  if (height < floor - 1e-9) {
    SetError("a jog of " + std::to_string(height) + " mm is lower than its " +
             "two bends: at radius " + std::to_string(R) + " mm and " +
             std::to_string(angleDeg) + " degrees they alone climb " +
             std::to_string(floor) + " mm");
    return false;
  }
  const double wallLen = (height - floor) / std::sin(theta);

  gp_Pnt a = P(sketch->plane.PlaneToWorld(line->a));
  gp_Pnt b = P(sketch->plane.PlaneToWorld(line->b));
  if (a.Distance(b) < 1e-9) {
    SetError("the jog's line has no length");
    return false;
  }

  ArmOccSignalHandlers();
  try {
    // The panel the line is drawn on: a flat face of a body that holds both
    // ends of the line, one thickness deep.
    Body* body = nullptr;
    gp_Dir up;
    for (Body& candidate : ctx.bodies) {
      for (TopExp_Explorer ex(candidate.shape.GetShape(), TopAbs_FACE);
           ex.More() && body == nullptr; ex.Next()) {
        const TopoDS_Face face = TopoDS::Face(ex.Current());
        BRepAdaptor_Surface surf(face);
        if (surf.GetType() != GeomAbs_Plane)
          continue;
        const gp_Pln pln = surf.Plane();
        // A line drawn on the face sits on it to whatever placed the face
        // (a face placed through float is microns off), not to machine
        // precision.
        if (pln.Distance(a) > 1e-4 || pln.Distance(b) > 1e-4)
          continue;
        gp_Dir n = pln.Axis().Direction();
        if (face.Orientation() == TopAbs_REVERSED)
          n.Reverse();
        const gp_Pnt mid((a.XYZ() + b.XYZ()) / 2.0);
        if (!Inside(candidate.shape, mid.Translated(gp_Vec(n) * (-t / 2.0))) ||
            Inside(candidate.shape, mid.Translated(gp_Vec(n) * -(t + t / 2.0))))
          continue;
        body = &candidate;
        up = n;
        // Onto the face exactly: the gap between the plane and the face is
        // not a coordinate the bends should inherit.
        const gp_Vec off(pln.Location(), a);
        a.Translate(gp_Vec(n) * -off.Dot(gp_Vec(n)));
        const gp_Vec offB(pln.Location(), b);
        b.Translate(gp_Vec(n) * -offB.Dot(gp_Vec(n)));
      }
      if (body != nullptr)
        break;
    }
    if (body == nullptr) {
      SetError("the jog's line is not drawn on a sheet panel - sketch it on "
               "the panel's face");
      return false;
    }
    for (const BendRegion& bend : body->bends)
      if (bend.flat) {
        // A crease lying flat carries its fold relative to where its panel
        // sits; stepping that panel would have to be threaded through every
        // one of them. Fold it back first.
        SetError("this body has bends laid flat - refold them before "
                 "stepping it");
        return false;
      }

    const gp_Dir e(gp_Vec(a, b));
    gp_Dir pd(gp_Vec(up).Crossed(gp_Vec(e)));
    if (flip)
      pd.Reverse();
    glm::dvec3 lo, hi;
    body->shape.GetBoundingBox(lo, hi);
    const double size = glm::length(hi - lo) * 4.0 + 100.0;

    // The bends run the whole width of the panel at the line.
    const CADShape across =
        Side(Side(body->shape, a.Translated(gp_Vec(pd) * -1e-3), pd, size),
             a.Translated(gp_Vec(pd) * 1e-3), pd.Reversed(), size);
    if (!across.IsValid()) {
      SetError("the jog's line does not cross the panel");
      return false;
    }
    double s0 = 1e30, s1 = -1e30;
    for (TopExp_Explorer ex(across.GetShape(), TopAbs_VERTEX); ex.More();
         ex.Next()) {
      const double s = gp_Vec(a, BRep_Tool::Pnt(TopoDS::Vertex(ex.Current())))
                           .Dot(gp_Vec(e));
      s0 = std::min(s0, s);
      s1 = std::max(s1, s);
    }
    const double width = s1 - s0;
    const gp_Pnt Tf = a.Translated(gp_Vec(e) * s0);

    // Bend one: up from the panel. Bend two: back to level, its picked face
    // the outside of the first.
    const gp_Dir n1(gp_Vec(pd).Crossed(gp_Vec(up)));
    const gp_Pnt C1 = Tf.Translated(gp_Vec(up) * R);
    const gp_Vec W1 =
        gp_Vec(pd) * std::sin(theta) - gp_Vec(up) * std::cos(theta);
    const gp_Dir cd1(gp_Vec(pd) * std::cos(theta) +
                     gp_Vec(up) * std::sin(theta));
    const gp_Pnt Tf2 = C1.Translated(W1 * (R + t) + gp_Vec(cd1) * wallLen);
    const gp_Dir pd2 = cd1;
    const gp_Dir up2(W1);
    const gp_Dir n2(gp_Vec(pd2).Crossed(gp_Vec(up2)));
    const gp_Pnt C2 = Tf2.Translated(gp_Vec(up2) * R);
    const gp_Vec along = gp_Vec(e) * width;

    const CADShape bend1(BRepPrimAPI_MakeRevol(Rect(Tf, gp_Vec(up) * -t, along),
                                               gp_Ax1(C1, n1), theta)
                             .Shape());
    const CADShape bend2(
        BRepPrimAPI_MakeRevol(Rect(Tf2, gp_Vec(up2) * -t, along),
                              gp_Ax1(C2, n2), theta)
            .Shape());
    CADShape wall;
    if (wallLen > 1e-9)
      wall = CADShape(
          BRepPrimAPI_MakePrism(Rect(C1.Translated(W1 * R), W1 * t, along),
                                gp_Vec(cd1) * wallLen)
              .Shape());

    // The moved side, carried: its picked face lands `height` above where it
    // was, and nothing about it turns.
    gp_Trsf carry;
    carry.SetTranslation(gp_Vec(Tf, C2.Translated(gp_Vec(up) * (R + t))));
    const CADShape kept = Side(body->shape, Tf, pd.Reversed(), size);
    const CADShape moved = Side(body->shape, Tf, pd, size);
    if (!kept.IsValid() || !moved.IsValid()) {
      SetError("the jog's line is at the edge of the panel - there is no "
               "panel on one side of it to step");
      return false;
    }

    // Built up from the pieces with names kept: the kept side's through a
    // common, the moved side's through the same, then moved.
    ElementMap none, keptMap, movedMap;
    std::string why;
    CADShape keptPart =
        topo::BooleanNamed(BooleanOp::Intersect, body->shape, body->elementMap,
                           kept, none, GetId(), keptMap, &why);
    CADShape movedPart =
        topo::BooleanNamed(BooleanOp::Intersect, body->shape, body->elementMap,
                           moved, none, GetId(), movedMap, &why);
    if (!keptPart.IsValid() || !movedPart.IsValid()) {
      SetError(why.empty() ? "the panel could not be split at the jog" : why);
      return false;
    }
    movedPart = CADShape(
        BRepBuilderAPI_Transform(movedPart.GetShape(), carry, Standard_True)
            .Shape());
    // Both halves of every face and edge the step cuts across came out of
    // the SAME intersect under the same tag, so they carry the same name —
    // and a name that resolves to two elements resolves to whichever comes
    // first, which put a wall picked on the near half onto the far one. The
    // carried side says it was carried.
    const std::string stepped = ";S" + std::to_string(GetId());
    for (auto& [idx, name] : movedMap.names)
      name += stepped;
    for (auto& [idx, name] : movedMap.edgeNames)
      name += stepped;

    CADShape result = keptPart;
    ElementMap resultMap = keptMap;
    const auto join = [&](const CADShape& piece, ElementMap map) {
      if (!piece.IsValid())
        return true;
      ElementMap out;
      std::string err;
      CADShape fused = topo::BooleanNamed(BooleanOp::Union, result, resultMap,
                                          piece, map, GetId(), out, &err);
      if (!fused.IsValid() || fused.Volume() < result.Volume() * (1.0 - 1e-9))
        return false;
      result = std::move(fused);
      resultMap = std::move(out);
      return true;
    };
    int k = 0;
    for (const CADShape* piece :
         std::initializer_list<const CADShape*>{&bend1, &wall, &bend2}) {
      if (!piece->IsValid())
        continue;
      ElementMap map;
      topo::NamePrimitiveElements(
          *piece, std::to_string(GetId()) + "/j" + std::to_string(k++), map);
      if (!join(*piece, map)) {
        SetError("the jog's bends would not join the panel");
        return false;
      }
    }
    if (!join(movedPart, movedMap)) {
      SetError("the moved side of the jog would not join its bend");
      return false;
    }

    // Every bend already on the moved side rode up with it.
    for (BendRegion& old : body->bends) {
      const gp_Pnt probe =
          gp_Pnt(old.axisOrigin.x, old.axisOrigin.y, old.axisOrigin.z);
      if (gp_Vec(Tf, probe).Dot(gp_Vec(pd)) <= 0.0)
        continue;
      old.axisOrigin = G(probe.Transformed(carry).XYZ());
      for (BendRegion::Trim& trim : old.trims) {
        const gp_Pnt tp(trim.point.x, trim.point.y, trim.point.z);
        trim.point = G(tp.Transformed(carry).XYZ());
      }
    }
    const auto record = [&](const gp_Pnt& C, const gp_Dir& parent,
                            const gp_Dir& face) {
      BendRegion bend;
      bend.featureId = GetId();
      bend.axisOrigin = G(C.XYZ());
      bend.axisDir = G(e.XYZ());
      bend.radius = R;
      bend.width = width;
      bend.angle = theta;
      bend.kFactor = rules.EffectiveKFactor(R);
      bend.kSource = rules.KSource(R);
      bend.parentDir = G(parent.XYZ());
      bend.upDir = G(face.XYZ());
      body->bends.push_back(std::move(bend));
    };
    record(C1, pd, up);
    record(C2, pd2, up2);
    body->shape = std::move(result);
    body->elementMap = std::move(resultMap);
    return true;
  } catch (const Standard_Failure& ex) {
    SetError(std::string("jog failed: ") + ex.GetMessageString());
    return false;
  }
}
