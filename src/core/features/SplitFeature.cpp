#include "core/features/SplitFeature.h"

#include "Logger.h"
#include "core/ElementHistory.h"
#include "core/features/DatumPlaneFeature.h" // ResolveParentPlane

#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <Standard_Failure.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>

SplitFeature::SplitFeature(const std::string& name, uint64_t bodyId)
    : bodyId(bodyId) {
  m_name = name;
}

bool SplitFeature::Execute(RegenContext& ctx) {
  const auto body = std::find_if(
      ctx.bodies.begin(), ctx.bodies.end(),
      [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the body to split no longer exists");
    return false;
  }

  // Resolve the knife to a point + normal defining the cutting plane.
  glm::vec3 origin{0.0f};
  glm::vec3 normal{0.0f, 0.0f, 1.0f};
  if (knife == SplitKnifeKind::Plane) {
    const auto plane = ResolveParentPlane(ctx, datumId, basePlane);
    if (!plane) {
      SetError("the split plane's datum no longer exists");
      return false;
    }
    normal = plane->GetNormal();
    origin = plane->GetOrigin() + normal * static_cast<float>(offset);
  } else { // Face
    if (face.featureId != bodyId) {
      SetError("the knife face must belong to the split body");
      return false;
    }
    const auto idx = body->elementMap.Resolve(face.name);
    if (!idx) {
      SetError("the knife face '" + face.name + "' no longer exists");
      return false;
    }
    glm::vec3 uAxis;
    if (!body->shape.FacePlane(*idx, origin, normal, uAxis)) {
      SetError("the knife face is not planar");
      return false;
    }
  }

  // Build the cutting tool: a planar face large enough to span the body,
  // centered on the body's projection onto the plane so an off-origin body is
  // still fully crossed. Half-extent = the bounding-box diagonal (generous).
  glm::vec3 bmin, bmax;
  body->shape.GetBoundingBox(bmin, bmax);
  const glm::vec3 center = (bmin + bmax) * 0.5f;
  const glm::vec3 proj = center - glm::dot(center - origin, normal) * normal;
  const double h = std::max(1e-3, static_cast<double>(glm::length(bmax - bmin)));

  try {
    const gp_Pln pln(gp_Pnt(proj.x, proj.y, proj.z),
                     gp_Dir(normal.x, normal.y, normal.z));
    BRepBuilderAPI_MakeFace mf(pln, -h, h, -h, h);
    if (!mf.IsDone()) {
      SetError("could not build the cutting plane");
      return false;
    }

    BRepAlgoAPI_Splitter splitter;
    TopTools_ListOfShape args, tools;
    TopoDS_Face knifeFace = mf.Face();
    TopoDS_Shape knifeShape = knifeFace; // upcast via a non-const lvalue
    args.Append(body->shape.GetShape());
    tools.Append(knifeShape);
    splitter.SetArguments(args);
    splitter.SetTools(tools);
    splitter.Build();
    if (!splitter.IsDone()) {
      SetError("split operation failed");
      return false;
    }

    CADShape resultShape(splitter.Shape());
    std::vector<CADShape> pieces = resultShape.SplitSolids();
    if (pieces.size() < 2) {
      SetError("the plane does not divide the body into separate pieces");
      return false;
    }

    // Inherit the body's material unless this feature overrides it; capture
    // everything needed before erasing the source (the iterator dies with it).
    const std::string material =
        m_material.empty() ? body->material : m_material;
    ctx.bodies.erase(body);

    // Every piece is stamped with this feature's id (the v1 identity
    // limitation); the "<id>/bN" element-name prefix still keeps each piece's
    // faces individually resolvable, as multi-body import does.
    int n = 0;
    for (CADShape& piece : pieces) {
      n++;
      ElementMap map;
      topo::NamePrimitiveElements(
          piece, std::to_string(GetId()) + "/b" + std::to_string(n), map);
      ctx.bodies.push_back({m_name + " " + std::to_string(n), std::move(piece),
                            material, GetId()});
      ctx.bodies.back().elementMap = std::move(map);
    }
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("split failed: ") + e.GetMessageString());
    LOG_ERROR("SplitFeature '{}': {}", m_name, GetError());
    return false;
  }
}
