#pragma once

#include "render/Scene.h"
#include "cad/CADKernel.h"
#include <glm/glm.hpp>
#include <string>

enum class CADPrimitiveType {
    Box,
    Sphere,
    Cylinder,
    Cone,
    Custom
};

class CADObject : public SceneObject {
public:
    CADObject(const std::string& name, CADPrimitiveType type = CADPrimitiveType::Custom);
    CADObject(const std::string& name, const CADShape& shape, CADPrimitiveType type = CADPrimitiveType::Custom);
    virtual ~CADObject() = default;

    // SceneObject overrides
    void Update(float deltaTime) override;
    void Render() override;

    // CAD-specific functionality
    void SetShape(const CADShape& shape);
    const CADShape& GetShape() const { return m_shape; }
    
    // Primitive creation
    static std::unique_ptr<CADObject> CreateBox(const std::string& name, 
                                               double width, double height, double depth);
    static std::unique_ptr<CADObject> CreateSphere(const std::string& name, 
                                                   const glm::vec3& center, double radius);
    static std::unique_ptr<CADObject> CreateCylinder(const std::string& name,
                                                     const glm::vec3& center, const glm::vec3& direction,
                                                     double radius, double height);

    // Properties
    CADPrimitiveType GetPrimitiveType() const { return m_primitiveType; }
    double GetVolume() const;
    void GetBoundingBox(glm::vec3& min, glm::vec3& max) const;
    
    // Transformation
    void SetPosition(const glm::vec3& position);
    void SetRotation(const glm::vec3& axis, double angle);
    void SetScale(double scale);
    
    glm::vec3 GetPosition() const { return m_position; }
    glm::vec3 GetRotationAxis() const { return m_rotationAxis; }
    double GetRotationAngle() const { return m_rotationAngle; }
    double GetScale() const { return m_scale; }
    
    // Boolean operations
    void Union(const CADObject& other);
    void Intersect(const CADObject& other);
    void Subtract(const CADObject& other);
    
    // Mesh for rendering
    CADShape::Mesh GetMesh() const;
    bool IsMeshDirty() const { return m_meshDirty; }
    void MarkMeshClean() { m_meshDirty = false; }

private:
    CADShape m_shape;
    CADPrimitiveType m_primitiveType;
    
    // Transform properties
    glm::vec3 m_position;
    glm::vec3 m_rotationAxis;
    double m_rotationAngle;
    double m_scale;
    
    // Rendering
    mutable bool m_meshDirty;
    
    void UpdateTransform();
};