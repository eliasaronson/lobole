#pragma once

#ifdef HAVE_OPENCASCADE

#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Ax2.hxx>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

class CADShape {
public:
    CADShape();
    CADShape(const TopoDS_Shape& shape);
    ~CADShape();

    // Shape operations
    bool IsValid() const;
    double Volume() const;
    void GetBoundingBox(glm::vec3& min, glm::vec3& max) const;
    
    // Boolean operations
    CADShape Union(const CADShape& other) const;
    CADShape Intersection(const CADShape& other) const;
    CADShape Difference(const CADShape& other) const;
    
    // Transformation
    void Translate(const glm::vec3& offset);
    void Rotate(const glm::vec3& axis, double angle);
    void Scale(double factor);
    
    // Access to underlying shape
    const TopoDS_Shape& GetShape() const { return m_shape; }
    
    // Mesh generation for rendering
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
    };
    
    struct Mesh {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        // B-Rep face id per triangle (indices.size()/3 entries). Faces are
        // emitted contiguously, so each face also owns a contiguous vertex
        // range - which picking and highlighting rely on.
        std::vector<uint32_t> triangleFaces;
    };

    Mesh GenerateMesh(double deflection = 0.1) const;

private:
    TopoDS_Shape m_shape;
};

class CADKernel {
public:
    static CADKernel& Instance();
    
    // Primitive creation
    CADShape CreateBox(const glm::vec3& corner1, const glm::vec3& corner2);
    CADShape CreateBox(double width, double height, double depth);
    
    CADShape CreateSphere(const glm::vec3& center, double radius);
    
    CADShape CreateCylinder(const glm::vec3& center, const glm::vec3& direction, 
                           double radius, double height);
    
    CADShape CreateCone(const glm::vec3& center, const glm::vec3& direction,
                       double radius1, double radius2, double height);
    
    // File I/O
    bool LoadSTEP(const std::string& filename, std::vector<CADShape>& shapes);
    bool SaveSTEP(const std::string& filename, const std::vector<CADShape>& shapes);
    
    bool LoadSTL(const std::string& filename, CADShape& shape);
    bool SaveSTL(const std::string& filename, const CADShape& shape);

private:
    CADKernel() = default;
    
    // Helper functions
    gp_Pnt ToOCCPoint(const glm::vec3& point) const;
    gp_Vec ToOCCVector(const glm::vec3& vector) const;
    gp_Dir ToOCCDirection(const glm::vec3& direction) const;
    glm::vec3 FromOCCPoint(const gp_Pnt& point) const;
    glm::vec3 FromOCCVector(const gp_Vec& vector) const;
};

#else

// Dummy implementations when OpenCASCADE is not available
class CADShape {
public:
    CADShape() = default;
    bool IsValid() const { return false; }
    double Volume() const { return 0.0; }
    void GetBoundingBox(glm::vec3& min, glm::vec3& max) const { min = max = glm::vec3(0); }
    
    struct Vertex {
        glm::vec3 position;
        glm::vec3 normal;
    };
    
    struct Mesh {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> triangleFaces;
    };

    Mesh GenerateMesh(double deflection = 0.1) const { return Mesh(); }
};

class CADKernel {
public:
    static CADKernel& Instance() { static CADKernel instance; return instance; }
    
    CADShape CreateBox(const glm::vec3& corner1, const glm::vec3& corner2) { return CADShape(); }
    CADShape CreateBox(double width, double height, double depth) { return CADShape(); }
    CADShape CreateSphere(const glm::vec3& center, double radius) { return CADShape(); }
    CADShape CreateCylinder(const glm::vec3& center, const glm::vec3& direction, 
                           double radius, double height) { return CADShape(); }
    CADShape CreateCone(const glm::vec3& center, const glm::vec3& direction,
                       double radius1, double radius2, double height) { return CADShape(); }
};

#endif