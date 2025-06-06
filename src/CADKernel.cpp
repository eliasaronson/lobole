#include "CADKernel.h"

#ifdef HAVE_OPENCASCADE

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
#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <gp_Trsf.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <GProp_GProps.hxx>
#include <BRepGProp.hxx>
#include <Standard_Handle.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <StlAPI_Reader.hxx>
#include <StlAPI_Writer.hxx>
#include <Interface_Static.hxx>

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

CADShape::Mesh CADShape::GenerateMesh(double deflection) const {
    Mesh mesh;
    if (!IsValid()) return mesh;
    
    // Generate mesh
    BRepMesh_IncrementalMesh meshGen(m_shape, deflection);
    if (!meshGen.IsDone()) return mesh;
    
    // Extract triangular mesh from faces
    for (TopExp_Explorer faceExp(m_shape, TopAbs_FACE); faceExp.More(); faceExp.Next()) {
        const TopoDS_Face& face = TopoDS::Face(faceExp.Current());
        TopLoc_Location loc;
        Handle(Poly_Triangulation) triangulation = BRep_Tool::Triangulation(face, loc);
        
        if (!triangulation.IsNull()) {
            int baseIndex = mesh.vertices.size();
            
            // Add vertices (newer OpenCASCADE API)
            for (int i = 1; i <= triangulation->NbNodes(); i++) {
                gp_Pnt p = triangulation->Node(i).Transformed(loc);
                Vertex vertex;
                vertex.position = glm::vec3(p.X(), p.Y(), p.Z());
                vertex.normal = glm::vec3(0, 0, 1); // TODO: Calculate proper normals
                mesh.vertices.push_back(vertex);
            }
            
            // Add triangles (newer OpenCASCADE API)
            for (int i = 1; i <= triangulation->NbTriangles(); i++) {
                const Poly_Triangle& tri = triangulation->Triangle(i);
                int n1, n2, n3;
                tri.Get(n1, n2, n3);
                
                if (face.Orientation() == TopAbs_REVERSED) {
                    mesh.indices.push_back(baseIndex + n1 - 1);
                    mesh.indices.push_back(baseIndex + n3 - 1);
                    mesh.indices.push_back(baseIndex + n2 - 1);
                } else {
                    mesh.indices.push_back(baseIndex + n1 - 1);
                    mesh.indices.push_back(baseIndex + n2 - 1);
                    mesh.indices.push_back(baseIndex + n3 - 1);
                }
            }
        }
    }
    
    return mesh;
}

// CADKernel implementation
CADKernel& CADKernel::Instance() {
    static CADKernel instance;
    return instance;
}

CADShape CADKernel::CreateBox(const glm::vec3& corner1, const glm::vec3& corner2) {
    gp_Pnt p1 = ToOCCPoint(corner1);
    gp_Pnt p2 = ToOCCPoint(corner2);
    
    BRepPrimAPI_MakeBox boxMaker(p1, p2);
    if (boxMaker.IsDone()) {
        return CADShape(boxMaker.Shape());
    }
    return CADShape();
}

CADShape CADKernel::CreateBox(double width, double height, double depth) {
    BRepPrimAPI_MakeBox boxMaker(width, height, depth);
    if (boxMaker.IsDone()) {
        return CADShape(boxMaker.Shape());
    }
    return CADShape();
}

CADShape CADKernel::CreateSphere(const glm::vec3& center, double radius) {
    gp_Pnt centerPnt = ToOCCPoint(center);
    
    BRepPrimAPI_MakeSphere sphereMaker(centerPnt, radius);
    if (sphereMaker.IsDone()) {
        return CADShape(sphereMaker.Shape());
    }
    return CADShape();
}

CADShape CADKernel::CreateCylinder(const glm::vec3& center, const glm::vec3& direction, 
                                  double radius, double height) {
    gp_Pnt centerPnt = ToOCCPoint(center);
    gp_Dir dir = ToOCCDirection(direction);
    gp_Ax2 axis(centerPnt, dir);
    
    BRepPrimAPI_MakeCylinder cylinderMaker(axis, radius, height);
    if (cylinderMaker.IsDone()) {
        return CADShape(cylinderMaker.Shape());
    }
    return CADShape();
}

CADShape CADKernel::CreateCone(const glm::vec3& center, const glm::vec3& direction,
                              double radius1, double radius2, double height) {
    gp_Pnt centerPnt = ToOCCPoint(center);
    gp_Dir dir = ToOCCDirection(direction);
    gp_Ax2 axis(centerPnt, dir);
    
    BRepPrimAPI_MakeCone coneMaker(axis, radius1, radius2, height);
    if (coneMaker.IsDone()) {
        return CADShape(coneMaker.Shape());
    }
    return CADShape();
}

bool CADKernel::LoadSTEP(const std::string& filename, std::vector<CADShape>& shapes) {
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

bool CADKernel::SaveSTL(const std::string& filename, const CADShape& shape) {
    if (!shape.IsValid()) return false;
    
    StlAPI_Writer writer;
    return writer.Write(shape.GetShape(), filename.c_str());
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