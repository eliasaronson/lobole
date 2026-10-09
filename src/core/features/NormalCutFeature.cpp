// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/NormalCutFeature.h"

#include "core/features/SketchFeature.h"
#include "core/sheet/Unfold.h"
#include "core/sketch/SketchProfile.h"

#include <BRepAdaptor_Surface.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <algorithm>

NormalCutFeature::NormalCutFeature(const std::string& name, uint64_t sketchId)
    : sketchId(sketchId) {
  m_name = name;
}

bool NormalCutFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    SetError("this part has no sheet-metal rules (set_sheet_metal_rules) - a "
             "normal cut is a cut through a sheet");
    return false;
  }
  const auto* sketch =
      dynamic_cast<const SketchFeature*>(ctx.FindById(sketchId));
  if (sketch == nullptr) {
    SetError("the cut's sketch (id " + std::to_string(sketchId) +
             ") is not earlier in the history");
    return false;
  }
  std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch->entities);
  if (!regions.empty() && !sketch::FilterLoopsToRegions(loops, regions)) {
    SetError("a selected region no longer exists in the sketch - re-pick "
             "the profiles");
    return false;
  }
  if (loops.empty()) {
    SetError("the sketch has no closed profile to cut");
    return false;
  }

  // The profile as a face on the sketch plane: the base cap of a unit prism
  // of it, which is the one construction that already knows how to turn a
  // sketch's loops — holes included — into a face.
  const CADShape prism = sketch::BuildPrism(sketch->plane, loops, 1.0, false);
  if (!prism.IsValid()) {
    SetError("the sketch's profile could not be made into a face");
    return false;
  }
  const glm::dvec3 o = sketch->plane.GetOrigin();
  const glm::dvec3 n = sketch->plane.GetNormal();
  const gp_Pnt origin(o.x, o.y, o.z);
  const gp_Dir normal(n.x, n.y, n.z);
  // EVERY region the sketch holds, not just the first: three circles drawn
  // for three burner ports are three separate profiles, and stopping at the
  // first cut one port and left the other two burners passing through solid
  // steel — with nothing said about it.
  std::vector<TopoDS_Face> profiles;
  for (TopExp_Explorer ex(prism.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    const gp_Pln pln = surf.Plane();
    if (pln.Axis().Direction().IsParallel(normal, 1e-6) &&
        pln.Distance(origin) < 1e-4)
      profiles.push_back(face);
  }
  const TopoDS_Face profile = profiles.empty() ? TopoDS_Face{} : profiles[0];
  if (profile.IsNull()) {
    SetError("the sketch's profile could not be found on its plane");
    return false;
  }

  // The body: the one named, or the one the profile's middle lands on.
  auto body = ctx.bodies.end();
  if (bodyId != 0) {
    body = std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                        [&](const Body& b) { return b.createdBy == bodyId; });
  } else {
    BRepAdaptor_Surface surf(profile);
    const double u = (surf.FirstUParameter() + surf.LastUParameter()) / 2.0;
    const double v = (surf.FirstVParameter() + surf.LastVParameter()) / 2.0;
    const gp_Pnt mid = surf.Value(u, v);
    for (auto it = ctx.bodies.begin(); it != ctx.bodies.end(); ++it) {
      if (it->reference)
        continue;
      IntCurvesFace_ShapeIntersector inter;
      inter.Load(it->shape.GetShape(), 1e-7);
      inter.Perform(gp_Lin(mid, normal), -1e9, 1e9);
      if (inter.IsDone() && inter.NbPnt() > 0) {
        body = it;
        break;
      }
    }
  }
  if (body == ctx.bodies.end()) {
    SetError(bodyId != 0 ? "the sheet body to cut no longer exists"
                         : "the cut's profile does not land on any body");
    return false;
  }

  // All of them in ONE call: the cut's costly half — the ray tree, and the
  // part laid flat when some outline crosses a bend — is per body, and this
  // loop used to pay for it per hole.
  std::string why;
  auto cut =
      sheet::NormalCut(*body, *ctx.sheetMetal, profiles, normal, GetId(), &why);
  if (!cut) {
    SetError(why.empty() ? "the normal cut failed" : why);
    return false;
  }
  body->shape = std::move(cut->shape);
  body->elementMap = std::move(cut->map);
  body->bends = std::move(cut->bends);
  // An Unfold/Refold pair inside the cut stamped this feature's id on the
  // creases; once folded back up there is nothing of it left on them.
  return true;
}
