#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include "CADKernel.h"

#ifdef HAVE_OPENCASCADE
#pragma message("Compiling with OpenCASCADE support")

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Solid.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Circ.hxx>
#include <GeomAbs_CurveType.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>
#include <Geom_Surface.hxx>
#include <GeomLProp_SLProps.hxx>
#include <gp_Pnt2d.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <gp_Trsf.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <Standard_Handle.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <BRep_Builder.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TopLoc_Location.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <StlAPI_Reader.hxx>
#include <StlAPI_Writer.hxx>
#include <BRepTools.hxx>
#include <Interface_Static.hxx>
#include <Standard_Version.hxx>
#include <Standard_Failure.hxx>
#include <Precision.hxx>

// CADShape implementation
CADShape::CADShape() {
}

CADShape::CADShape(const TopoDS_Shape& shape) : m_shape(shape) {
}

CADShape::~CADShape() {
}

bool CADShape::IsValid() const {
    return !m_shape.IsNull();
}

double CADShape::Volume() const {
    if (!IsValid()) return 0.0;

    GProp_GProps props;
    BRepGProp::VolumeProperties(m_shape, props);
    return props.Mass();
}

bool CADShape::ComputeMassProps(MassProps& out) const {
    if (!IsValid()) return false;

    GProp_GProps volumeProps;
    BRepGProp::VolumeProperties(m_shape, volumeProps);
    out.volume = volumeProps.Mass();
    const gp_Pnt com = volumeProps.CentreOfMass();
    out.centerOfMass = glm::vec3(com.X(), com.Y(), com.Z());

    // A GProps' inertia matrix is about its system location — anchor a second
    // pass at the center of mass so the matrix is the physical one.
    GProp_GProps aboutCom(com);
    BRepGProp::VolumeProperties(m_shape, aboutCom);
    const gp_Mat inertia = aboutCom.MatrixOfInertia();
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            out.inertia[r][c] = inertia.Value(r + 1, c + 1);

    GProp_GProps surfaceProps;
    BRepGProp::SurfaceProperties(m_shape, surfaceProps);
    out.surfaceArea = surfaceProps.Mass();
    return true;
}

void CADShape::GetBoundingBox(glm::vec3& min, glm::vec3& max) const {
    if (!IsValid()) {
        min = max = glm::vec3(0.0f);
        return;
    }
    
    Bnd_Box box;
    BRepBndLib::Add(m_shape, box);
    
    if (!box.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        min = glm::vec3(xmin, ymin, zmin);
        max = glm::vec3(xmax, ymax, zmax);
    } else {
        min = max = glm::vec3(0.0f);
    }
}

CADShape CADShape::Union(const CADShape& other) const {
    if (!IsValid() || !other.IsValid()) return CADShape();
    
    BRepAlgoAPI_Fuse fuse(m_shape, other.m_shape);
    if (fuse.IsDone()) {
        return CADShape(fuse.Shape());
    }
    return CADShape();
}

CADShape CADShape::Intersection(const CADShape& other) const {
    if (!IsValid() || !other.IsValid()) return CADShape();
    
    BRepAlgoAPI_Common common(m_shape, other.m_shape);
    if (common.IsDone()) {
        return CADShape(common.Shape());
    }
    return CADShape();
}

CADShape CADShape::Difference(const CADShape& other) const {
    if (!IsValid() || !other.IsValid()) return CADShape();
    
    BRepAlgoAPI_Cut cut(m_shape, other.m_shape);
    if (cut.IsDone()) {
        return CADShape(cut.Shape());
    }
    return CADShape();
}

CADShape CADShape::Transformed(const glm::mat4& m) const {
    if (m_shape.IsNull()) return CADShape();
    gp_Trsf t;
    try {
        // glm is column-major (m[col][row]); SetValues wants row-major 3x4.
        t.SetValues(m[0][0], m[1][0], m[2][0], m[3][0],
                    m[0][1], m[1][1], m[2][1], m[3][1],
                    m[0][2], m[1][2], m[2][2], m[3][2]);
    } catch (const Standard_Failure&) {
        return CADShape(m_shape); // sheared matrix: OCC can't represent it
    }
    return CADShape(m_shape.Moved(TopLoc_Location(t)));
}

void CADShape::Translate(const glm::vec3& offset) {
    if (!IsValid()) return;
    
    gp_Trsf transform;
    transform.SetTranslation(gp_Vec(offset.x, offset.y, offset.z));
    
    BRepBuilderAPI_Transform transformer(m_shape, transform);
    if (transformer.IsDone()) {
        m_shape = transformer.Shape();
    }
}

void CADShape::Rotate(const glm::vec3& axis, double angle) {
    if (!IsValid()) return;
    
    gp_Ax1 rotationAxis(gp_Pnt(0, 0, 0), gp_Dir(axis.x, axis.y, axis.z));
    gp_Trsf transform;
    transform.SetRotation(rotationAxis, angle);
    
    BRepBuilderAPI_Transform transformer(m_shape, transform);
    if (transformer.IsDone()) {
        m_shape = transformer.Shape();
    }
}

void CADShape::Scale(double factor) {
    if (!IsValid()) return;

    gp_Trsf transform;
    transform.SetScale(gp_Pnt(0, 0, 0), factor);

    BRepBuilderAPI_Transform transformer(m_shape, transform);
    if (transformer.IsDone()) {
        m_shape = transformer.Shape();
    }
}

void CADShape::RotateAbout(const glm::vec3& point, const glm::vec3& dir,
                           double angle) {
    if (!IsValid()) return;

    gp_Ax1 axis(gp_Pnt(point.x, point.y, point.z),
                gp_Dir(dir.x, dir.y, dir.z));
    gp_Trsf transform;
    transform.SetRotation(axis, angle);

    BRepBuilderAPI_Transform transformer(m_shape, transform);
    if (transformer.IsDone()) {
        m_shape = transformer.Shape();
    }
}

void CADShape::Mirror(const glm::vec3& point, const glm::vec3& normal) {
    if (!IsValid()) return;

    // The gp_Ax2's XY plane is the mirror plane; its main direction = normal.
    gp_Ax2 plane(gp_Pnt(point.x, point.y, point.z),
                 gp_Dir(normal.x, normal.y, normal.z));
    gp_Trsf transform;
    transform.SetMirror(plane);

    BRepBuilderAPI_Transform transformer(m_shape, transform);
    if (transformer.IsDone()) {
        m_shape = transformer.Shape();
    }
}

std::vector<CADShape> CADShape::SplitSolids() const {
    std::vector<CADShape> solids;
    if (!IsValid()) return solids;
    for (TopExp_Explorer exp(m_shape, TopAbs_SOLID); exp.More(); exp.Next()) {
        solids.emplace_back(exp.Current());
    }
    // A lone solid isn't wrapped in a compound, but TopExp still yields it;
    // if somehow nothing was found, fall back to the shape itself.
    if (solids.empty()) {
        solids.push_back(*this);
    }
    return solids;
}

bool CADShape::FacePlane(int faceIndex, glm::vec3& center, glm::vec3& normal,
                         glm::vec3& uAxis) const {
    if (!IsValid() || faceIndex < 0) return false;

    // Walk to the face in the same order GenerateMesh / the ElementMap use.
    int i = 0;
    TopoDS_Face face;
    for (TopExp_Explorer exp(m_shape, TopAbs_FACE); exp.More(); exp.Next(), i++) {
        if (i == faceIndex) { face = TopoDS::Face(exp.Current()); break; }
    }
    if (face.IsNull()) return false;

    // Sketch-on-face requires a planar face.
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Plane) return false;

    // Centroid.
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    const gp_Pnt c = props.CentreOfMass();
    center = glm::vec3(c.X(), c.Y(), c.Z());

    // The plane's axis is the normal; the face orientation gives outward sense.
    const gp_Dir nd = surf.Plane().Axis().Direction();
    glm::vec3 n(nd.X(), nd.Y(), nd.Z());
    if (face.Orientation() == TopAbs_REVERSED) n = -n;
    normal = glm::normalize(n);

    // Deterministic in-plane u-axis (matches the sketch-on-face baking
    // heuristic), so the sketch frame is stable across upstream edits.
    glm::vec3 u = std::abs(normal.y) < 0.9f ? glm::vec3(0, 1, 0)
                                            : glm::vec3(1, 0, 0);
    uAxis = glm::normalize(u - glm::dot(u, normal) * normal);
    return true;
}

bool CADShape::FaceAxis(int faceIndex, glm::vec3& origin, glm::vec3& axis,
                        glm::vec3& xAxis) const {
  if (!IsValid() || faceIndex < 0) return false;

  // Walk to the face in the same order FacePlane / GenerateMesh / picking use.
  int i = 0;
  TopoDS_Face face;
  for (TopExp_Explorer exp(m_shape, TopAbs_FACE); exp.More(); exp.Next(), i++) {
    if (i == faceIndex) { face = TopoDS::Face(exp.Current()); break; }
  }
  if (face.IsNull()) return false;

  BRepAdaptor_Surface surf(face);
  if (surf.GetType() != GeomAbs_Cylinder) return false;

  const gp_Ax1 ax = surf.Cylinder().Axis();
  const gp_Pnt ap = ax.Location();
  const gp_Dir ad = ax.Direction();
  const glm::vec3 axisPoint(ap.X(), ap.Y(), ap.Z());
  axis = glm::normalize(glm::vec3(ad.X(), ad.Y(), ad.Z()));

  // Origin = the face centroid projected onto the axis, so the frame sits at
  // the middle of the actual (possibly partial) cylindrical patch rather than
  // at OCC's arbitrary axis location.
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  const gp_Pnt c = props.CentreOfMass();
  const glm::vec3 centroid(c.X(), c.Y(), c.Z());
  origin = axisPoint + glm::dot(centroid - axisPoint, axis) * axis;

  // Deterministic perpendicular (matches FacePlane's u-axis heuristic).
  glm::vec3 u = std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  xAxis = glm::normalize(u - glm::dot(u, axis) * axis);
  return true;
}

void CADShape::FaceCircleCenters(int faceIndex, std::vector<glm::vec3>& centers,
                                 std::vector<int>& edgeIndices) const {
  centers.clear();
  edgeIndices.clear();
  if (!IsValid() || faceIndex < 0) return;
  int i = 0;
  TopoDS_Face face;
  for (TopExp_Explorer exp(m_shape, TopAbs_FACE); exp.More(); exp.Next(), i++) {
    if (i == faceIndex) { face = TopoDS::Face(exp.Current()); break; }
  }
  if (face.IsNull()) return;

  // Global edge order (0-based) matches ElementMap::edgeNames / edge picking.
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m_shape, TopAbs_EDGE, edges);
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge& edge = TopoDS::Edge(ex.Current());
    if (BRep_Tool::Degenerated(edge)) continue;
    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() != GeomAbs_Circle) continue;
    const gp_Pnt c = curve.Circle().Location();
    const int idx = edges.FindIndex(edge) - 1; // -> 0-based edgeNames index
    if (idx < 0) continue;
    centers.emplace_back(c.X(), c.Y(), c.Z());
    edgeIndices.push_back(idx);
  }
}

bool CADShape::EdgeSnapPoint(int edgeIndex, glm::vec3& point) const {
  if (!IsValid() || edgeIndex < 0) return false;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m_shape, TopAbs_EDGE, edges);
  if (edgeIndex >= edges.Extent()) return false;
  const TopoDS_Edge& edge = TopoDS::Edge(edges(edgeIndex + 1)); // 1-based map
  if (edge.IsNull()) return false;
  BRepAdaptor_Curve curve(edge);
  if (curve.GetType() == GeomAbs_Circle) {
    const gp_Pnt c = curve.Circle().Location();
    point = glm::vec3(c.X(), c.Y(), c.Z());
  } else {
    // Midpoint of the parameter range for a non-circular edge.
    const gp_Pnt m =
        curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
    point = glm::vec3(m.X(), m.Y(), m.Z());
  }
  return true;
}

CADShape::Mesh CADShape::GenerateMesh(double deflection) const {
    Mesh mesh;
    if (!IsValid()) return mesh;

    // Generate mesh. A fixed absolute deflection facets curved surfaces badly
    // (a small cylinder becomes a visible ~16-gon whose flat facets flicker as
    // the camera orbits). Scale the linear deflection to the model's size and
    // add an angular deflection so circles stay smooth at any scale.
    Bnd_Box bbox;
    BRepBndLib::Add(m_shape, bbox);
    double linDeflection = deflection;
    if (!bbox.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        const double diag = std::sqrt((xmax - xmin) * (xmax - xmin) +
                                      (ymax - ymin) * (ymax - ymin) +
                                      (zmax - zmin) * (zmax - zmin));
        if (diag > Precision::Confusion())
            linDeflection = diag * 0.002; // 0.2% of the bounding diagonal
    }
    const double angDeflection = 0.2; // radians (~11.5°) → ~32 segments / circle
    BRepMesh_IncrementalMesh meshGen(m_shape, linDeflection, Standard_False,
                                     angDeflection, Standard_True);
    if (!meshGen.IsDone()) return mesh;

    // Extract triangular mesh, one contiguous vertex/triangle block per
    // B-Rep face, with exact surface normals where the surface provides them
    uint32_t faceId = 0;
    for (TopExp_Explorer faceExp(m_shape, TopAbs_FACE); faceExp.More();
         faceExp.Next(), faceId++) {
        const TopoDS_Face& face = TopoDS::Face(faceExp.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, loc);
        if (triangulation.IsNull()) continue;

        const bool reversed = (face.Orientation() == TopAbs_REVERSED);
        int baseIndex = mesh.vertices.size();

        Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
        const bool hasUV = triangulation->HasUVNodes() && !surface.IsNull();

        for (int i = 1; i <= triangulation->NbNodes(); i++) {
            gp_Pnt p = triangulation->Node(i).Transformed(loc);
            Vertex vertex;
            vertex.position = glm::vec3(p.X(), p.Y(), p.Z());
            vertex.normal = glm::vec3(0.0f); // triangle fallback fills these
            if (hasUV) {
                const gp_Pnt2d uv = triangulation->UVNode(i);
                GeomLProp_SLProps props(surface, uv.X(), uv.Y(), 1,
                                        Precision::Confusion());
                if (props.IsNormalDefined()) {
                    gp_Dir n = props.Normal().Transformed(loc.Transformation());
                    vertex.normal = glm::vec3(n.X(), n.Y(), n.Z());
                    if (reversed) vertex.normal = -vertex.normal;
                }
            }
            mesh.vertices.push_back(vertex);
        }

        for (int i = 1; i <= triangulation->NbTriangles(); i++) {
            const Poly_Triangle& tri = triangulation->Triangle(i);
            int n1, n2, n3;
            tri.Get(n1, n2, n3);

            if (reversed) {
                mesh.indices.push_back(baseIndex + n1 - 1);
                mesh.indices.push_back(baseIndex + n3 - 1);
                mesh.indices.push_back(baseIndex + n2 - 1);
            } else {
                mesh.indices.push_back(baseIndex + n1 - 1);
                mesh.indices.push_back(baseIndex + n2 - 1);
                mesh.indices.push_back(baseIndex + n3 - 1);
            }
            mesh.triangleFaces.push_back(faceId);
        }
    }

    // Fallback for vertices whose surface normal was undefined: average the
    // adjacent triangle normals (winding above makes them outward-facing)
    std::vector<glm::vec3> accumulated(mesh.vertices.size(), glm::vec3(0.0f));
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const glm::vec3& v0 = mesh.vertices[mesh.indices[t]].position;
        const glm::vec3& v1 = mesh.vertices[mesh.indices[t + 1]].position;
        const glm::vec3& v2 = mesh.vertices[mesh.indices[t + 2]].position;
        glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
        accumulated[mesh.indices[t]] += n;
        accumulated[mesh.indices[t + 1]] += n;
        accumulated[mesh.indices[t + 2]] += n;
    }
    for (size_t i = 0; i < mesh.vertices.size(); i++) {
        glm::vec3& n = mesh.vertices[i].normal;
        if (glm::dot(n, n) < 0.25f) n = accumulated[i];
        float len = glm::length(n);
        n = (len > 1e-8f) ? n / len : glm::vec3(0, 0, 1);
    }

    return mesh;
}

std::vector<std::vector<glm::vec3>> CADShape::GenerateEdgePolylines(
    double deflection) const {
    std::vector<std::vector<glm::vec3>> polylines;
    if (!IsValid()) return polylines;

    // Same model-relative deflection scaling as GenerateMesh, so edge
    // polylines match the mesh's fidelity.
    Bnd_Box bbox;
    BRepBndLib::Add(m_shape, bbox);
    double linDeflection = deflection;
    if (!bbox.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        const double diag = std::sqrt((xmax - xmin) * (xmax - xmin) +
                                      (ymax - ymin) * (ymax - ymin) +
                                      (zmax - zmin) * (zmax - zmin));
        if (diag > Precision::Confusion())
            linDeflection = diag * 0.002;
    }
    const double angDeflection = 0.2; // radians, matches GenerateMesh

    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(m_shape, TopAbs_EDGE, edges);
    polylines.resize(edges.Extent());
    for (int i = 1; i <= edges.Extent(); i++) {
        const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
        if (BRep_Tool::Degenerated(edge)) continue; // empty keeps indices aligned
        try {
            BRepAdaptor_Curve curve(edge);
            GCPnts_TangentialDeflection discretizer(
                curve, angDeflection, linDeflection, /*minPoints=*/2);
            auto& poly = polylines[i - 1];
            poly.reserve(discretizer.NbPoints());
            for (int p = 1; p <= discretizer.NbPoints(); p++) {
                const gp_Pnt pt = discretizer.Value(p);
                poly.emplace_back(pt.X(), pt.Y(), pt.Z());
            }
        } catch (const Standard_Failure&) {
            polylines[i - 1].clear(); // unparameterizable edge: skip, keep index
        }
    }
    return polylines;
}

// CADKernel implementation
CADKernel& CADKernel::Instance() {
    static CADKernel instance;
    static bool initialized = false;
    
    if (!initialized) {
        // Initialize OpenCASCADE - basic validation
        try {
            gp_Pnt testPoint(0.0, 0.0, 0.0);  // Basic test to ensure libraries are loaded
        } catch (...) {
            // OpenCASCADE initialization failed
        }
        initialized = true;
    }
    
    return instance;
}

CADShape CADKernel::CreateBox(const glm::vec3& corner1, const glm::vec3& corner2) {
    gp_Pnt p1 = ToOCCPoint(corner1);
    gp_Pnt p2 = ToOCCPoint(corner2);
    
    BRepPrimAPI_MakeBox boxMaker(p1, p2);
    if (!boxMaker.IsDone()) {
        boxMaker.Build();
    }
    
    if (boxMaker.IsDone()) {
        return CADShape(boxMaker.Shape());
    }
    return CADShape();
}

CADShape CADKernel::CreateBox(double width, double height, double depth) {
    // Validate input parameters against Precision::Confusion
    if (width <= Precision::Confusion() || height <= Precision::Confusion() || depth <= Precision::Confusion()) {
        return CADShape();
    }
    
    try {
        BRepPrimAPI_MakeBox boxMaker(width, height, depth);
        
        // Call Build() to ensure construction is complete
        if (!boxMaker.IsDone()) {
            boxMaker.Build();
        }
        
        if (boxMaker.IsDone()) {
            return CADShape(boxMaker.Shape());
        }
    } catch (const Standard_Failure& e) {
        // Log error if needed, but return empty shape
    }
    
    return CADShape();
}

CADShape CADKernel::CreateSphere(const glm::vec3& center, double radius) {
    if (radius <= Precision::Confusion()) {
        return CADShape();
    }
    
    try {
        gp_Pnt centerPnt = ToOCCPoint(center);
        BRepPrimAPI_MakeSphere sphereMaker(centerPnt, radius);
        
        // Call Build() to ensure construction is complete
        if (!sphereMaker.IsDone()) {
            sphereMaker.Build();
        }
        
        if (sphereMaker.IsDone()) {
            return CADShape(sphereMaker.Shape());
        }
    } catch (const Standard_Failure& e) {
        // Log error if needed, but return empty shape
    }
    
    return CADShape();
}

CADShape CADKernel::CreateCylinder(const glm::vec3& center, const glm::vec3& direction, 
                                  double radius, double height) {
    if (radius <= Precision::Confusion() || height <= Precision::Confusion()) {
        return CADShape();
    }
    
    try {
        gp_Pnt centerPnt = ToOCCPoint(center);
        gp_Dir dir = ToOCCDirection(direction);
        gp_Ax2 axis(centerPnt, dir);
        
        BRepPrimAPI_MakeCylinder cylinderMaker(axis, radius, height);
        
        // Call Build() to ensure construction is complete
        if (!cylinderMaker.IsDone()) {
            cylinderMaker.Build();
        }
        
        if (cylinderMaker.IsDone()) {
            return CADShape(cylinderMaker.Shape());
        }
    } catch (const Standard_Failure& e) {
        // Log error if needed, but return empty shape
    }
    
    return CADShape();
}

CADShape CADKernel::CreateCone(const glm::vec3& center, const glm::vec3& direction,
                              double radius1, double radius2, double height) {
    if (radius1 <= Precision::Confusion() || radius2 <= Precision::Confusion() || height <= Precision::Confusion()) {
        return CADShape();
    }
    
    try {
        gp_Pnt centerPnt = ToOCCPoint(center);
        gp_Dir dir = ToOCCDirection(direction);
        gp_Ax2 axis(centerPnt, dir);
        
        BRepPrimAPI_MakeCone coneMaker(axis, radius1, radius2, height);
        
        // Call Build() to ensure construction is complete
        if (!coneMaker.IsDone()) {
            coneMaker.Build();
        }
        
        if (coneMaker.IsDone()) {
            return CADShape(coneMaker.Shape());
        }
    } catch (const Standard_Failure& e) {
        // Log error if needed, but return empty shape
    }
    
    return CADShape();
}

bool CADKernel::LoadSTEP(const std::string& filename, std::vector<CADShape>& shapes) {
    if (!std::filesystem::exists(filename)) return false;

    STEPControl_Reader reader;
    IFSelect_ReturnStatus status = reader.ReadFile(filename.c_str());
    
    if (status != IFSelect_RetDone) return false;
    
    reader.TransferRoots();
    int nbShapes = reader.NbShapes();
    
    shapes.clear();
    for (int i = 1; i <= nbShapes; i++) {
        TopoDS_Shape shape = reader.Shape(i);
        if (!shape.IsNull()) {
            shapes.push_back(CADShape(shape));
        }
    }
    
    return !shapes.empty();
}

namespace {

std::string XcafLabelName(const TDF_Label& label) {
    Handle(TDataStd_Name) name;
    if (!label.IsNull() && label.FindAttribute(TDataStd_Name::GetID(), name)) {
        const TCollection_ExtendedString& ext = name->Get();
        TCollection_AsciiString ascii(ext);
        return std::string(ascii.ToCString());
    }
    return {};
}

// A rigid gp_Trsf as a glm placement. glm is column-major: m[col][row];
// gp_Trsf::Value is 1-based (row, col) with col 4 = translation.
glm::mat4 TrsfToMat4(const gp_Trsf& t) {
    glm::mat4 m(1.0f);
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 4; col++)
            m[col][row] = static_cast<float>(t.Value(row + 1, col + 1));
    return m;
}

} // namespace

bool CADKernel::LoadSTEPStructured(const std::string& filename,
                                   ImportedModel& out) {
    out = {};
    if (!std::filesystem::exists(filename)) return false;

    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
    STEPCAFControl_Reader reader;
    reader.SetNameMode(Standard_True);
    reader.SetColorMode(Standard_True);
    if (reader.ReadFile(filename.c_str()) != IFSelect_RetDone) return false;
    try {
        if (!reader.Transfer(doc)) return false;
    } catch (const Standard_Failure&) {
        return false;
    }

    Handle(XCAFDoc_ShapeTool) shapes =
        XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) colors =
        XCAFDoc_DocumentTool::ColorTool(doc->Main());

    // One product entry per unique definition label, keyed by its entry
    // string — 100 identical screws stay 1 product + 100 instances.
    std::map<std::string, size_t> productIndex;
    auto productOf = [&](const TDF_Label& def) -> size_t {
        TCollection_AsciiString entry;
        TDF_Tool::Entry(def, entry);
        const std::string key(entry.ToCString());
        auto it = productIndex.find(key);
        if (it != productIndex.end()) return it->second;

        ImportedShape p;
        p.shape = CADShape(shapes->GetShape(def));
        p.name = XcafLabelName(def);
        Quantity_Color qc;
        if (colors->GetColor(def, XCAFDoc_ColorSurf, qc) ||
            colors->GetColor(def, XCAFDoc_ColorGen, qc) ||
            colors->GetColor(shapes->GetShape(def), XCAFDoc_ColorSurf, qc) ||
            colors->GetColor(shapes->GetShape(def), XCAFDoc_ColorGen, qc)) {
            p.hasColor = true;
            p.color = glm::vec3(static_cast<float>(qc.Red()),
                                static_cast<float>(qc.Green()),
                                static_cast<float>(qc.Blue()));
        }
        productIndex.emplace(key, out.products.size());
        out.products.push_back(std::move(p));
        return out.products.size() - 1;
    };

    // Walk the instance tree, composing locations down to leaf products.
    std::function<void(const TDF_Label&, const gp_Trsf&, const std::string&)>
        visit = [&](const TDF_Label& label, const gp_Trsf& parent,
                    const std::string& instName) {
            TDF_Label def = label;
            XCAFDoc_ShapeTool::GetReferredShape(label, def); // no-op if plain
            const gp_Trsf here =
                parent * XCAFDoc_ShapeTool::GetLocation(label).Transformation();
            if (XCAFDoc_ShapeTool::IsAssembly(def)) {
                TDF_LabelSequence comps;
                XCAFDoc_ShapeTool::GetComponents(def, comps);
                for (Standard_Integer i = 1; i <= comps.Length(); i++) {
                    std::string childName = XcafLabelName(comps.Value(i));
                    if (childName.empty()) {
                        TDF_Label childDef = comps.Value(i);
                        XCAFDoc_ShapeTool::GetReferredShape(comps.Value(i),
                                                            childDef);
                        childName = XcafLabelName(childDef);
                    }
                    visit(comps.Value(i), here, childName);
                }
            } else {
                ImportedInstance inst;
                inst.product = productOf(def);
                inst.transform = TrsfToMat4(here);
                inst.name = !instName.empty()
                                ? instName
                                : out.products[inst.product].name;
                out.instances.push_back(std::move(inst));
            }
        };

    TDF_LabelSequence roots;
    shapes->GetFreeShapes(roots);
    for (Standard_Integer i = 1; i <= roots.Length(); i++)
        visit(roots.Value(i), gp_Trsf(), XcafLabelName(roots.Value(i)));

    return !out.instances.empty();
}

bool CADKernel::SaveSTEP(const std::string& filename, const std::vector<CADShape>& shapes) {
    if (shapes.empty()) return false;
    
    STEPControl_Writer writer;
    
    for (const auto& shape : shapes) {
        if (shape.IsValid()) {
            writer.Transfer(shape.GetShape(), STEPControl_AsIs);
        }
    }
    
    return writer.Write(filename.c_str()) == IFSelect_RetDone;
}

bool CADKernel::LoadSTL(const std::string& filename, CADShape& shape) {
    StlAPI_Reader reader;
    TopoDS_Shape occShape;
    
    if (reader.Read(occShape, filename.c_str())) {
        shape = CADShape(occShape);
        return true;
    }
    return false;
}

bool CADKernel::SaveSTL(const std::string& filename, const CADShape& shape,
                        double deflection) {
    if (!shape.IsValid()) return false;

    // Re-mesh explicitly at the requested deflection instead of reusing
    // whatever triangulation the viewport happened to leave on the shape —
    // export quality is then a user choice, not an accident.
    BRepMesh_IncrementalMesh mesh(shape.GetShape(), deflection);
    mesh.Perform();

    StlAPI_Writer writer;
    writer.ASCIIMode() = false; // binary: smaller, faster, slicer-friendly
    return writer.Write(shape.GetShape(), filename.c_str());
}

bool CADKernel::SaveBREP(const std::string& filename, const CADShape& shape) {
    if (!shape.IsValid()) return false;
    return BRepTools::Write(shape.GetShape(), filename.c_str());
}

bool CADKernel::LoadBREP(const std::string& filename, CADShape& shape) {
    if (!std::filesystem::exists(filename)) return false;
    TopoDS_Shape s;
    BRep_Builder builder;
    if (!BRepTools::Read(s, filename.c_str(), builder) || s.IsNull())
        return false;
    shape = CADShape(s);
    return true;
}

bool CADKernel::SaveSTEPStructured(const std::string& filename,
                                   const ImportedModel& model,
                                   const std::string& assemblyName) {
    if (model.products.empty() || model.instances.empty()) return false;

    Handle(TDocStd_Document) doc;
    XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
    Handle(XCAFDoc_ShapeTool) shapes =
        XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    Handle(XCAFDoc_ColorTool) colors =
        XCAFDoc_DocumentTool::ColorTool(doc->Main());

    std::vector<TDF_Label> productLabels;
    for (const auto& p : model.products) {
        if (!p.shape.IsValid()) return false;
        TDF_Label label = shapes->AddShape(p.shape.GetShape(),
                                           /*makeAssembly=*/Standard_False);
        if (!p.name.empty())
            TDataStd_Name::Set(label, p.name.c_str());
        if (p.hasColor) {
            colors->SetColor(label,
                             Quantity_Color(p.color.r, p.color.g, p.color.b,
                                            Quantity_TOC_RGB),
                             XCAFDoc_ColorSurf);
        }
        productLabels.push_back(label);
    }

    TDF_Label assembly = shapes->NewShape();
    if (!assemblyName.empty())
        TDataStd_Name::Set(assembly, assemblyName.c_str());
    for (const auto& inst : model.instances) {
        if (inst.product >= productLabels.size()) return false;
        gp_Trsf t;
        try {
            const glm::mat4& m = inst.transform;
            t.SetValues(m[0][0], m[1][0], m[2][0], m[3][0],
                        m[0][1], m[1][1], m[2][1], m[3][1],
                        m[0][2], m[1][2], m[2][2], m[3][2]);
        } catch (const Standard_Failure&) {
            return false; // sheared transform: not representable in STEP
        }
        TDF_Label comp = shapes->AddComponent(assembly,
                                              productLabels[inst.product],
                                              TopLoc_Location(t));
        if (!inst.name.empty())
            TDataStd_Name::Set(comp, inst.name.c_str());
    }
    shapes->UpdateAssemblies();

    STEPCAFControl_Writer writer;
    try {
        if (!writer.Transfer(doc, STEPControl_AsIs)) return false;
        return writer.Write(filename.c_str()) == IFSelect_RetDone;
    } catch (const Standard_Failure&) {
        return false;
    }
}

// Helper functions
gp_Pnt CADKernel::ToOCCPoint(const glm::vec3& point) const {
    return gp_Pnt(point.x, point.y, point.z);
}

gp_Vec CADKernel::ToOCCVector(const glm::vec3& vector) const {
    return gp_Vec(vector.x, vector.y, vector.z);
}

gp_Dir CADKernel::ToOCCDirection(const glm::vec3& direction) const {
    return gp_Dir(direction.x, direction.y, direction.z);
}

glm::vec3 CADKernel::FromOCCPoint(const gp_Pnt& point) const {
    return glm::vec3(point.X(), point.Y(), point.Z());
}

glm::vec3 CADKernel::FromOCCVector(const gp_Vec& vector) const {
    return glm::vec3(vector.X(), vector.Y(), vector.Z());
}

#endif