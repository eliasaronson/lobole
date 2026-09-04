#include "core/drawing/DrawingProjection.h"

#ifdef HAVE_OPENCASCADE

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Curve.hxx>
#include <Geom_Surface.hxx>
#include <HLRAlgo_EdgeIterator.hxx>
#include <HLRAlgo_EdgesBlock.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRAlgo_WiresBlock.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_Curve.hxx>
#include <HLRBRep_Data.hxx>
#include <HLRBRep_EdgeData.hxx>
#include <HLRBRep_FaceData.hxx>
#include <HLRBRep_PolyAlgo.hxx>
#include <HLRBRep_PolyHLRToShape.hxx>
#include <OSD.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ2d.hxx>
#include <gp_Dir.hxx>
#include <gp_Elips2d.hxx>
#include <gp_Trsf.hxx>

#include <chrono>
#include <mutex>
#include <unordered_map>

#endif // HAVE_OPENCASCADE

namespace drawing {

#ifdef HAVE_OPENCASCADE

namespace {

// A rigid glm placement as an OCC transform (the MeshExport::ToTrsf pattern).
// glm is column-major; gp_Trsf::SetValues takes the 3x4 row-major affine
// block and enforces rigidity, which occurrence transforms guarantee.
gp_Trsf ToTrsf(const glm::dmat4& m) {
  gp_Trsf t;
  t.SetValues(m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1],
              m[3][1], m[0][2], m[1][2], m[2][2], m[3][2]);
  return t;
}

// Exact HLR is the historically crashiest corner of OCC: convert SIGSEGV and
// friends into Standard_Failure exceptions (process-wide, installed once) so
// a bad view sets an error string instead of killing the app. The `false`
// leaves floating-point traps unarmed.
void InstallSignalHandlersOnce() {
  static std::once_flag flag;
  std::call_once(flag, [] { OSD::SetSignal(Standard_False); });
}

// One projected body's lookup state: the moved shape fed to the algo plus its
// indexed sub-shape maps. Moved() shares TShapes with the original, so map
// order here matches a map walk of the untransformed shape index-for-index —
// the indices SourceRef promises (the ElementMap contracts).
struct BodyMaps {
  uint64_t sourceId = 0;
  TopoDS_Shape moved;
  TopTools_IndexedMapOfShape edges;
  TopTools_IndexedMapOfShape faces;
};

// Fallback identity for sub-shapes the HLR outliner rebuilt or split (they
// fail IsSame against the input): split pieces share the source's stored
// geometry handle (Geom_Curve / Geom_Surface), so the raw handle pointer plus
// the location distinguishes the source — the location matters because
// instances of one part (the assembly case) share every geometry handle and
// differ only in placement.
struct GeomEntry {
  TopLoc_Location loc;
  uint64_t sourceId = 0;
  int index = -1;
};
using GeomLookup = std::unordered_map<const void*, std::vector<GeomEntry>>;

const void* StoredCurveOf(const TopoDS_Edge& edge, TopLoc_Location& loc) {
  double first = 0.0, last = 0.0;
  Handle(Geom_Curve) c = BRep_Tool::Curve(edge, loc, first, last);
  return c.IsNull() ? nullptr : c.get();
}

const void* StoredSurfaceOf(const TopoDS_Face& face, TopLoc_Location& loc) {
  Handle(Geom_Surface) s = BRep_Tool::Surface(face, loc);
  return s.IsNull() ? nullptr : s.get();
}

// Geometry-handle fallback lookup: same stored handle, equal location.
bool FindByGeom(const GeomLookup& lookup, const void* geom,
                const TopLoc_Location& loc, SourceRef& out, bool asFace) {
  if (!geom)
    return false;
  const auto it = lookup.find(geom);
  if (it == lookup.end())
    return false;
  for (const GeomEntry& entry : it->second) {
    if (entry.loc == loc) {
      out.sourceId = entry.sourceId;
      (asFace ? out.faceIndex : out.edgeIndex) = entry.index;
      return true;
    }
  }
  // Single candidate with a mismatched location: still the right source
  // geometry (locations can compose differently through the outliner);
  // ambiguity only matters when instances collide.
  if (it->second.size() == 1) {
    out.sourceId = it->second.front().sourceId;
    (asFace ? out.faceIndex : out.edgeIndex) = it->second.front().index;
    return true;
  }
  return false;
}

// Chord-deflection subdivision of gc over [u1,u2] into `out`. Emits the point
// at u1; the caller seeds the first point and intervals stay in order.
void SampleCurve(const HLRBRep_Curve& gc, double u1, double u2,
                 double deflection, int depth, std::vector<glm::dvec2>& out) {
  const gp_Pnt2d p1 = gc.Value(u1);
  const gp_Pnt2d p2 = gc.Value(u2);
  const double um = 0.5 * (u1 + u2);
  const gp_Pnt2d pm = gc.Value(um);
  // Deviation of the curve midpoint from the chord midpoint bounds the chord
  // error to first order — the standard recursive flattening test.
  const gp_Pnt2d chordMid(0.5 * (p1.X() + p2.X()), 0.5 * (p1.Y() + p2.Y()));
  if (depth <= 0 || pm.Distance(chordMid) <= deflection) {
    out.emplace_back(p2.X(), p2.Y());
    return;
  }
  SampleCurve(gc, u1, um, deflection, depth - 1, out);
  SampleCurve(gc, um, u2, deflection, depth - 1, out);
}

// Every point of one polyline edge out of the poly shaper's compounds. The
// shaper's edges are already IN the view plane (z is depth), so the projection
// is just dropping z.
void AppendPolyEdge(const TopoDS_Edge& edge, CurveKind kind, bool visible,
                    std::vector<ProjectedCurve>& out) {
  TopLoc_Location loc;
  const Handle(Poly_Polygon3D) poly = BRep_Tool::Polygon3D(edge, loc);
  ProjectedCurve pc;
  pc.kind = kind;
  pc.visible = visible;
  // No SourceRef: a triangulated silhouette has no model edge behind it. That
  // is the mode's defining limitation, not an oversight — see the header.
  if (!poly.IsNull()) {
    const TColgp_Array1OfPnt& pts = poly->Nodes();
    pc.points.reserve(pts.Length());
    for (int i = pts.Lower(); i <= pts.Upper(); ++i) {
      const gp_Pnt p = pts(i).Transformed(loc.Transformation());
      pc.points.emplace_back(p.X(), p.Y());
    }
  } else {
    double f = 0.0, l = 0.0;
    const Handle(Geom_Curve) c = BRep_Tool::Curve(edge, loc, f, l);
    if (!c.IsNull()) {
      for (double u : {f, l}) {
        const gp_Pnt p = c->Value(u).Transformed(loc.Transformation());
        pc.points.emplace_back(p.X(), p.Y());
      }
    } else {
      // The shaper's edges carry NEITHER a polygon nor a curve — they are
      // built straight from their two vertices, and asking for a geometry
      // they do not have is why the first version of this returned 12 edges
      // and 0 curves on a box. The vertices ARE the segment.
      TopoDS_Vertex v1, v2;
      TopExp::Vertices(edge, v1, v2);
      if (v1.IsNull() || v2.IsNull())
        return;
      for (const TopoDS_Vertex& v : {v1, v2}) {
        const gp_Pnt p = BRep_Tool::Pnt(v);
        pc.points.emplace_back(p.X(), p.Y());
      }
    }
  }
  if (pc.points.size() >= 2)
    out.push_back(std::move(pc));
}

std::optional<ProjectionResult>
ProjectPoly(const std::vector<InputBody>& bodies, const gp_Ax2& viewCS,
            const ProjectionOptions& options, std::string* error) {
  using Clock = std::chrono::steady_clock;
  ProjectionResult result;
  result.mode = ProjectionMode::Poly;

  Handle(HLRBRep_PolyAlgo) algo = new HLRBRep_PolyAlgo();
  int loaded = 0;
  const auto t0 = Clock::now();
  for (const InputBody& body : bodies) {
    if (body.shape.IsNull())
      continue;
    TopoDS_Shape moved =
        body.shape.Moved(TopLoc_Location(ToTrsf(body.transform)));
    // Poly HLR reads triangulations, so a shape that carries none produces
    // nothing at all — silently, which is the one way this mode could lie.
    // Meshing here rather than trusting the caller is what makes it honest.
    BRepMesh_IncrementalMesh mesher(moved, std::max(options.deflection, 0.01),
                                    Standard_False, 0.5, Standard_True);
    algo->Load(moved);
    ++loaded;
  }
  if (loaded == 0) {
    if (error)
      *error = "nothing to project";
    return std::nullopt;
  }
  const auto t1 = Clock::now();
  algo->Projector(HLRAlgo_Projector(viewCS));
  algo->Update();
  const auto t2 = Clock::now();
  result.stats.updateMs =
      std::chrono::duration<double, std::milli>(t1 - t0).count(); // meshing
  result.stats.hideMs =
      std::chrono::duration<double, std::milli>(t2 - t1).count();

  HLRBRep_PolyHLRToShape shaper;
  shaper.Update(algo);
  const struct {
    TopoDS_Shape shape;
    CurveKind kind;
    bool visible;
  } groups[] = {
      {shaper.VCompound(), CurveKind::Sharp, true},
      {shaper.OutLineVCompound(), CurveKind::Outline, true},
      {shaper.HCompound(), CurveKind::Sharp, false},
      {shaper.OutLineHCompound(), CurveKind::Outline, false},
  };
  for (const auto& g : groups) {
    if (!options.hiddenLines && !g.visible)
      continue;
    if (g.shape.IsNull())
      continue;
    for (TopExp_Explorer ex(g.shape, TopAbs_EDGE); ex.More(); ex.Next()) {
      ++result.stats.dsEdges; // edges the shaper produced, before extraction
      AppendPolyEdge(TopoDS::Edge(ex.Current()), g.kind, g.visible,
                     result.curves);
    }
  }
  return result;
}

} // namespace

std::optional<ProjectionResult>
ProjectView(const std::vector<InputBody>& bodies, const glm::dvec3& viewDir,
            const glm::dvec3& up, const ProjectionOptions& options,
            std::string* error) {
  if (bodies.empty()) {
    if (error)
      *error = "nothing to project";
    return std::nullopt;
  }
  const double dirLen = glm::length(viewDir);
  const double upLen = glm::length(up);
  if (dirLen < 1e-12 || upLen < 1e-12 ||
      glm::length(glm::cross(viewDir / dirLen, up / upLen)) < 1e-9) {
    if (error)
      *error = "degenerate view direction";
    return std::nullopt;
  }

  InstallSignalHandlersOnce();

  try {
    // Viewing coordinate system: Z toward the viewer, X right, Y = `up`.
    const glm::dvec3 d = viewDir / dirLen;
    const glm::dvec3 right = glm::normalize(glm::cross(d, up));
    const glm::dvec3 z = -d;
    const gp_Ax2 viewCS(gp_Pnt(0, 0, 0), gp_Dir(z.x, z.y, z.z),
                        gp_Dir(right.x, right.y, right.z));

    if (options.mode == ProjectionMode::Poly)
      return ProjectPoly(bodies, viewCS, options, error);

    Handle(HLRBRep_Algo) algo = new HLRBRep_Algo();
    std::vector<BodyMaps> maps;
    maps.reserve(bodies.size());
    GeomLookup curveToEdge, surfaceToFace;
    for (const InputBody& body : bodies) {
      if (body.shape.IsNull())
        continue;
      BodyMaps& bm = maps.emplace_back();
      bm.sourceId = body.sourceId;
      bm.moved = body.shape.Moved(TopLoc_Location(ToTrsf(body.transform)));
      TopExp::MapShapes(bm.moved, TopAbs_EDGE, bm.edges);
      TopExp::MapShapes(bm.moved, TopAbs_FACE, bm.faces);
      for (int i = 1; i <= bm.edges.Extent(); ++i) {
        TopLoc_Location loc;
        const void* key = StoredCurveOf(TopoDS::Edge(bm.edges.FindKey(i)), loc);
        if (key)
          curveToEdge[key].push_back({loc, bm.sourceId, i - 1});
      }
      for (int i = 1; i <= bm.faces.Extent(); ++i) {
        TopLoc_Location loc;
        const void* key =
            StoredSurfaceOf(TopoDS::Face(bm.faces.FindKey(i)), loc);
        if (key)
          surfaceToFace[key].push_back({loc, bm.sourceId, i - 1});
      }
      algo->Add(bm.moved);
    }
    if (maps.empty()) {
      if (error)
        *error = "nothing to project";
      return std::nullopt;
    }

    using Clock = std::chrono::steady_clock;
    ProjectionResult result;

    algo->Projector(HLRAlgo_Projector(viewCS));
    const auto t0 = Clock::now();
    algo->Update();
    const auto t1 = Clock::now();
    // PAIRWISE, not one Hide() call — same work, driven from here.
    //
    // Hide() takes no progress range and no cancel flag (OCC 7.9), so an 84 s
    // view was 84 s of nothing: no progress, no way to stop it, and the RPC
    // server blocked behind it. HLRBRep_Algo derives from
    // HLRBRep_InternalAlgo, which exposes Hide(i) — one shape against itself
    // — and Hide(i, j) — one shape against another. That IS the decomposition
    // Hide() performs internally, so driving it here changes nothing about
    // the result and makes the loop ours: it reports where it is and it can
    // be asked to stop (docs/agent-scale-followup-plan.md, W3).
    const int nbShapes = algo->NbShapes();
    bool cancelled = false;
    for (int i = 1; i <= nbShapes && !cancelled; ++i) {
      algo->Hide(i);
      for (int j = 1; j <= nbShapes && !cancelled; ++j) {
        if (i != j)
          algo->Hide(i, j);
        if (options.cancelled && options.cancelled())
          cancelled = true;
      }
    }
    if (cancelled) {
      if (error)
        *error = "cancelled";
      return std::nullopt;
    }
    const auto t2 = Clock::now();
    result.stats.updateMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.stats.hideMs =
        std::chrono::duration<double, std::milli>(t2 - t1).count();

    Handle(HLRBRep_Data) ds = algo->DataStructure();
    if (ds.IsNull()) {
      if (error)
        *error = "HLR produced no data structure";
      return std::nullopt;
    }
    const int nbEdges = ds->NbEdges();
    result.stats.dsEdges = nbEdges;

    // Face pass: which DS face generated each DS edge. A silhouette (an edge
    // the projection creates, with no source model edge) belongs to exactly
    // the face whose horizon it is — it appears only in that face's wire
    // blocks. The OutLine flag is NOT this signal: HLR sets it on a face's
    // real boundary edges when the face is viewed edge-on, so the true
    // "is this a silhouette" test is "does this edge resolve to a model edge"
    // (below), and this map only supplies the generating face for the ones
    // that don't. First membership wins (silhouettes belong to one face).
    std::vector<int> genFaceOf(nbEdges + 1, 0);
    for (int f = 1; f <= ds->NbFaces(); ++f) {
      Handle(HLRAlgo_WiresBlock) wires =
          ds->FDataArray().ChangeValue(f).Wires();
      if (wires.IsNull())
        continue;
      for (int w = 1; w <= wires->NbWires(); ++w) {
        Handle(HLRAlgo_EdgesBlock) block = wires->Wire(w);
        if (block.IsNull())
          continue;
        for (int i = 1; i <= block->NbEdges(); ++i) {
          const int edge = block->Edge(i);
          if (edge >= 1 && edge <= nbEdges && genFaceOf[edge] == 0)
            genFaceOf[edge] = f;
        }
      }
    }

    for (int e = 1; e <= nbEdges; ++e) {
      HLRBRep_EdgeData& ed = ds->EDataArray().ChangeValue(e);
      const TopoDS_Edge srcEdge = TopoDS::Edge(ds->EdgeMap().FindKey(e));
      if (BRep_Tool::Degenerated(srcEdge))
        continue;
      if (ed.Vertical())
        continue; // parallel to the view: projects to a point

      // Resolve as a REAL model edge first — by identity, then by shared
      // stored curve (the outliner rebuilds/splits input edges, breaking
      // IsSame while preserving geometry handles). An edge that resolves to
      // no model edge at all is a projection-generated silhouette (below);
      // this order matters because a real edge seen edge-on (e.g. a bore
      // rim) must stay attached to its source edge, not be mistaken for one.
      CurveKind kind = CurveKind::Sharp;
      SourceRef source;
      for (const BodyMaps& bm : maps) {
        const int idx = bm.edges.FindIndex(srcEdge);
        if (idx > 0) {
          source.sourceId = bm.sourceId;
          source.edgeIndex = idx - 1;
          break;
        }
      }
      if (source.edgeIndex < 0) {
        TopLoc_Location loc;
        const void* curve = StoredCurveOf(srcEdge, loc);
        FindByGeom(curveToEdge, curve, loc, source, /*asFace=*/false);
      }
      if (source.edgeIndex >= 0) {
        if (ed.Rg1Line())
          kind = ed.RgNLine() ? CurveKind::Sewn : CurveKind::Smooth;
      } else if (const int genFace = genFaceOf[e]; genFace > 0) {
        // No source model edge: a projection-generated silhouette, named by
        // the face whose horizon it is.
        kind = CurveKind::Outline;
        const TopoDS_Shape& face = ds->FaceMap().FindKey(genFace);
        for (const BodyMaps& bm : maps) {
          const int idx = bm.faces.FindIndex(face);
          if (idx > 0) {
            source.sourceId = bm.sourceId;
            source.faceIndex = idx - 1;
            break;
          }
        }
        if (source.faceIndex < 0) {
          // The outliner split the generating face along the silhouette;
          // the halves share the source face's stored surface.
          TopLoc_Location loc;
          const void* surface = StoredSurfaceOf(TopoDS::Face(face), loc);
          FindByGeom(surfaceToFace, surface, loc, source, /*asFace=*/true);
        }
      }

      HLRBRep_Curve& gc = ed.ChangeGeometry();
      const GeomAbs_CurveType type = gc.GetType();
      const auto emit = [&](double u1, double u2, bool visible) {
        if (!(u2 > u1))
          return;
        ProjectedCurve& curve = result.curves.emplace_back();
        curve.kind = kind;
        curve.visible = visible;
        curve.curveType = static_cast<int>(type);
        curve.source = source;
        const gp_Pnt2d start = gc.Value(u1);
        curve.points.emplace_back(start.X(), start.Y());
        if (type == GeomAbs_Line) {
          const gp_Pnt2d end = gc.Value(u2);
          curve.points.emplace_back(end.X(), end.Y());
        } else {
          SampleCurve(gc, u1, u2, options.deflection, 12, curve.points);
        }
        if (type == GeomAbs_Circle) {
          const gp_Circ2d circle = gc.Circle();
          curve.conicCenter = {circle.Location().X(), circle.Location().Y()};
          curve.conicRadius = circle.Radius();
          curve.closed = (u2 - u1) >= 2.0 * M_PI - 1e-6;
        } else if (type == GeomAbs_Ellipse) {
          const gp_Elips2d ellipse = gc.Ellipse();
          curve.conicCenter = {ellipse.Location().X(), ellipse.Location().Y()};
          curve.conicRadius = ellipse.MajorRadius();
          curve.closed = (u2 - u1) >= 2.0 * M_PI - 1e-6;
        }
        if (!source.IsResolved())
          ++result.stats.unresolvedSources;
      };

      HLRAlgo_EdgeIterator it;
      for (it.InitVisible(ed.Status()); it.MoreVisible(); it.NextVisible()) {
        double u1 = 0, u2 = 0;
        float tol1 = 0, tol2 = 0;
        it.Visible(u1, tol1, u2, tol2);
        emit(u1, u2, true);
      }
      if (options.hiddenLines) {
        for (it.InitHidden(ed.Status()); it.MoreHidden(); it.NextHidden()) {
          double u1 = 0, u2 = 0;
          float tol1 = 0, tol2 = 0;
          it.Hidden(u1, tol1, u2, tol2);
          emit(u1, u2, false);
        }
      }
    }

    return result;
  } catch (const Standard_Failure& failure) {
    if (error) {
      const char* msg = failure.GetMessageString();
      *error = std::string("HLR projection failed: ") +
               (msg && *msg ? msg : failure.DynamicType()->Name());
    }
    return std::nullopt;
  } catch (const std::exception& ex) {
    if (error)
      *error = std::string("HLR projection failed: ") + ex.what();
    return std::nullopt;
  }
}

#else // !HAVE_OPENCASCADE

std::optional<ProjectionResult>
ProjectView(const std::vector<InputBody>&, const glm::dvec3&, const glm::dvec3&,
            const ProjectionOptions&, std::string* error) {
  if (error)
    *error = "built without OpenCASCADE";
  return std::nullopt;
}

#endif // HAVE_OPENCASCADE

} // namespace drawing
