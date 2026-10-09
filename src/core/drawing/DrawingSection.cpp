// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/drawing/DrawingSection.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>

namespace drawing {

namespace {

// One edge's points in its wire's direction of travel.
std::vector<glm::dvec3> EdgePoints(const TopoDS_Edge& edge, double deflection) {
  std::vector<glm::dvec3> pts;
  BRepAdaptor_Curve curve(edge);
  if (curve.GetType() == GeomAbs_Line) {
    for (double t : {curve.FirstParameter(), curve.LastParameter()}) {
      const gp_Pnt p = curve.Value(t);
      pts.push_back({p.X(), p.Y(), p.Z()});
    }
  } else {
    GCPnts_QuasiUniformDeflection sampler(curve, deflection);
    if (sampler.IsDone()) {
      for (int i = 1; i <= sampler.NbPoints(); ++i) {
        const gp_Pnt p = sampler.Value(i);
        pts.push_back({p.X(), p.Y(), p.Z()});
      }
    }
  }
  if (edge.Orientation() == TopAbs_REVERSED)
    std::reverse(pts.begin(), pts.end());
  return pts;
}

} // namespace

std::optional<SectionCut> CutBody(const TopoDS_Shape& shape,
                                  const glm::dvec3& point,
                                  const glm::dvec3& normal,
                                  std::string* error) {
  try {
    const glm::dvec3 n = glm::normalize(normal);
    Bnd_Box bounds;
    BRepBndLib::Add(shape, bounds);
    if (bounds.IsVoid()) {
      if (error)
        *error = "the body has no extent to cut";
      return std::nullopt;
    }
    double x0, y0, z0, x1, y1, z1;
    bounds.Get(x0, y0, z0, x1, y1, z1);
    const glm::dvec3 lo(x0, y0, z0), hi(x1, y1, z1);
    // Big enough to swallow the whole body on the plane's near side from
    // wherever the plane is: twice the distance to the farthest corner.
    const double reach =
        2.0 * (glm::length(hi - lo) + glm::length(point - 0.5 * (lo + hi))) +
        1.0;
    // The box stands on the plane and reaches toward the reader (-n).
    const gp_Dir zDir(-n.x, -n.y, -n.z);
    const gp_Dir xDir = gp_Ax2(gp_Pnt(0, 0, 0), zDir).XDirection();
    const gp_Dir yDir = zDir.Crossed(xDir);
    const gp_Pnt corner(point.x - reach * (xDir.X() + yDir.X()),
                        point.y - reach * (xDir.Y() + yDir.Y()),
                        point.z - reach * (xDir.Z() + yDir.Z()));
    const TopoDS_Shape tool =
        BRepPrimAPI_MakeBox(gp_Ax2(corner, zDir, xDir), 2.0 * reach,
                            2.0 * reach, reach)
            .Shape();

    // Default-constructed and built explicitly: the two-argument
    // constructor builds at once, before SetNonDestructive could apply, and
    // a destructive boolean writes onto the document's own body.
    BRepAlgoAPI_Cut cut;
    TopTools_ListOfShape args, tools;
    args.Append(shape);
    tools.Append(tool);
    cut.SetArguments(args);
    cut.SetTools(tools);
    cut.SetNonDestructive(true);
    cut.Build();
    if (!cut.IsDone()) {
      if (error)
        *error = "the section cut failed";
      return std::nullopt;
    }

    SectionCut out;
    out.shape = cut.Shape();
    TopTools_IndexedMapOfShape srcE, srcF, resE, resF;
    TopExp::MapShapes(shape, TopAbs_EDGE, srcE);
    TopExp::MapShapes(shape, TopAbs_FACE, srcF);
    TopExp::MapShapes(out.shape, TopAbs_EDGE, resE);
    TopExp::MapShapes(out.shape, TopAbs_FACE, resF);
    out.edgeOrigin.assign(resE.Extent(), -1);
    out.edgeFromFace.assign(resE.Extent(), -1);
    out.faceOrigin.assign(resF.Extent(), -1);

    const auto claim = [](const TopTools_IndexedMapOfShape& res,
                          std::vector<int>& slot, const TopoDS_Shape& s,
                          int source) {
      const int i = res.FindIndex(s);
      if (i > 0 && slot[i - 1] < 0)
        slot[i - 1] = source;
    };
    for (int i = 1; i <= srcE.Extent(); ++i) {
      claim(resE, out.edgeOrigin, srcE(i), i - 1);
      for (const TopoDS_Shape& m : cut.Modified(srcE(i)))
        claim(resE, out.edgeOrigin, m, i - 1);
    }
    for (int i = 1; i <= srcF.Extent(); ++i) {
      claim(resF, out.faceOrigin, srcF(i), i - 1);
      for (const TopoDS_Shape& m : cut.Modified(srcF(i)))
        claim(resF, out.faceOrigin, m, i - 1);
    }
    // An edge the plane made is a section of one of the body's faces: the
    // boolean records it as generated from that face (and from the tool's
    // face, which is not ours to name).
    for (int i = 1; i <= srcF.Extent(); ++i)
      for (const TopoDS_Shape& g : cut.Generated(srcF(i)))
        if (g.ShapeType() == TopAbs_EDGE) {
          const int r = resE.FindIndex(g);
          if (r > 0 && out.edgeOrigin[r - 1] < 0)
            claim(resE, out.edgeFromFace, g, i - 1);
        }
    // Failing that, by where it lies: an unclaimed edge on the plane borders
    // the cut face and one source face, and that face is what it sections.
    for (int r = 1; r <= resE.Extent(); ++r) {
      if (out.edgeOrigin[r - 1] >= 0 || out.edgeFromFace[r - 1] >= 0)
        continue;
      for (int f = 1; f <= resF.Extent(); ++f) {
        if (out.faceOrigin[f - 1] < 0)
          continue;
        bool has = false;
        for (TopExp_Explorer ex(resF(f), TopAbs_EDGE); ex.More() && !has;
             ex.Next())
          has = ex.Current().IsSame(resE(r));
        if (has) {
          out.edgeFromFace[r - 1] = out.faceOrigin[f - 1];
          break;
        }
      }
    }

    // The faces the cut exposed: on the plane, and no source face's.
    const double tol = 1e-6 * std::max(1.0, glm::length(hi - lo));
    for (int f = 1; f <= resF.Extent(); ++f) {
      if (out.faceOrigin[f - 1] >= 0)
        continue;
      const TopoDS_Face face = TopoDS::Face(resF(f));
      BRepAdaptor_Surface surf(face, false);
      if (surf.GetType() != GeomAbs_Plane)
        continue;
      const gp_Pln pln = surf.Plane();
      const gp_Pnt o = pln.Location();
      const double d = n.x * (o.X() - point.x) + n.y * (o.Y() - point.y) +
                       n.z * (o.Z() - point.z);
      const gp_Dir pn = pln.Axis().Direction();
      if (std::abs(d) > tol ||
          std::abs(std::abs(pn.X() * n.x + pn.Y() * n.y + pn.Z() * n.z) - 1.0) >
              1e-6)
        continue;
      for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
        std::vector<glm::dvec3> loop;
        for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face);
             e.More(); e.Next()) {
          std::vector<glm::dvec3> pts = EdgePoints(e.Current(), 0.02);
          if (!loop.empty() && !pts.empty())
            pts.erase(pts.begin()); // shared with the previous edge's end
          loop.insert(loop.end(), pts.begin(), pts.end());
        }
        if (loop.size() >= 3)
          out.cutLoops.push_back(std::move(loop));
      }
    }
    return out;
  } catch (const Standard_Failure& e) {
    if (error)
      *error = std::string("the section cut failed: ") +
               (e.GetMessageString() ? e.GetMessageString() : "OCC error");
    return std::nullopt;
  }
}

std::vector<std::pair<glm::dvec2, glm::dvec2>>
HatchRegion(const std::vector<std::vector<glm::dvec2>>& loops, double spacing,
            double angleRad) {
  std::vector<std::pair<glm::dvec2, glm::dvec2>> out;
  if (spacing <= 0.0)
    return out;
  // Rotate the region so the hatch runs along X, scan, rotate back.
  const double c = std::cos(angleRad), s = std::sin(angleRad);
  const auto toLocal = [&](glm::dvec2 p) {
    return glm::dvec2(c * p.x + s * p.y, -s * p.x + c * p.y);
  };
  const auto toSheet = [&](glm::dvec2 p) {
    return glm::dvec2(c * p.x - s * p.y, s * p.x + c * p.y);
  };
  std::vector<std::vector<glm::dvec2>> local;
  double ylo = 1e300, yhi = -1e300;
  for (const auto& loop : loops) {
    if (loop.size() < 3)
      continue;
    std::vector<glm::dvec2> l;
    for (const glm::dvec2& p : loop) {
      l.push_back(toLocal(p));
      ylo = std::min(ylo, l.back().y);
      yhi = std::max(yhi, l.back().y);
    }
    local.push_back(std::move(l));
  }
  if (local.empty())
    return out;
  // Lines on a grid through the origin, so neighbouring regions of one part
  // hatch in step.
  for (double y = std::ceil(ylo / spacing) * spacing; y <= yhi; y += spacing) {
    std::vector<double> xs;
    for (const auto& l : local)
      for (size_t i = 0; i < l.size(); ++i) {
        const glm::dvec2 a = l[i], b = l[(i + 1) % l.size()];
        // Half-open in y, so a vertex on the line counts once.
        if ((a.y <= y) == (b.y <= y))
          continue;
        xs.push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
      }
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2)
      if (xs[i + 1] - xs[i] > 1e-9)
        out.push_back({toSheet({xs[i], y}), toSheet({xs[i + 1], y})});
  }
  return out;
}

} // namespace drawing
