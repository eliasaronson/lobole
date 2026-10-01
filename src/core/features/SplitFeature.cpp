#include "core/features/SplitFeature.h"

#include "common/Logger.h"
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
#include <cstdio>
#include <limits>

double CenteredSplitOffset(const glm::vec3& planeOrigin,
                           const glm::vec3& planeNormal, const glm::vec3& bmin,
                           const glm::vec3& bmax) {
  const glm::vec3 center = (bmin + bmax) * 0.5f;
  return static_cast<double>(glm::dot(center - planeOrigin, planeNormal));
}

SplitFeature::SplitFeature(const std::string& name, uint64_t bodyId)
    : bodyId(bodyId) {
  m_name = name;
}

bool SplitFeature::Execute(RegenContext& ctx) {
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(),
                   [&](const Body& b) { return b.createdBy == bodyId; });
  if (body == ctx.bodies.end()) {
    SetError("the body to split no longer exists");
    return false;
  }

  // Resolve the knife to a point + normal defining the cutting plane.
  glm::dvec3 origin{0.0};
  glm::dvec3 normal{0.0, 0.0, 1.0};
  if (knife == SplitKnifeKind::Plane) {
    const auto plane = ResolveParentPlane(ctx, datumId, basePlane);
    if (!plane) {
      SetError("the split plane's datum no longer exists");
      return false;
    }
    normal = plane->GetNormal();
    origin = plane->GetOrigin() + normal * offset;
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
    glm::dvec3 uAxis;
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
  const glm::dvec3 center = (glm::dvec3(bmin) + glm::dvec3(bmax)) * 0.5;
  const glm::dvec3 proj = center - glm::dot(center - origin, normal) * normal;
  const double h =
      std::max(1e-3, static_cast<double>(glm::length(bmax - bmin)));

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
      // "It didn't work" is not enough here: the two ways to fail look
      // identical in the viewport when the plane is edge-on. Say where the
      // knife is, in the one number that matters — distance along its own
      // normal — so the offset to type is arithmetic, not trial and error.
      double lo = std::numeric_limits<double>::max();
      double hi = std::numeric_limits<double>::lowest();
      for (int c = 0; c < 8; c++) {
        const glm::dvec3 corner((c & 1) ? bmax.x : bmin.x,
                                (c & 2) ? bmax.y : bmin.y,
                                (c & 4) ? bmax.z : bmin.z);
        const double d = glm::dot(corner, normal);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
      }
      const double knife = glm::dot(origin, normal);
      char msg[192];
      if (knife <= lo || knife >= hi) {
        std::snprintf(msg, sizeof(msg),
                      "the knife plane misses the body: it sits at %.3f along "
                      "its normal, and the body spans %.3f to %.3f",
                      knife, lo, hi);
      } else {
        std::snprintf(msg, sizeof(msg),
                      "the plane crosses the body at %.3f but does not "
                      "separate it into pieces",
                      knife);
      }
      SetError(msg);
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
