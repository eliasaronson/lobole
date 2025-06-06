#include "CADObject.h"
#include <iostream>

CADObject::CADObject(const std::string& name, CADPrimitiveType type)
    : SceneObject(name)
    , m_primitiveType(type)
    , m_position(0.0f, 0.0f, 0.0f)
    , m_rotationAxis(0.0f, 0.0f, 1.0f)
    , m_rotationAngle(0.0)
    , m_scale(1.0)
    , m_meshDirty(true) {
}

void CADObject::Update(float deltaTime) {
    // Update CAD object logic here
}

void CADObject::Render() {
    // Rendering is handled by the viewport using the mesh
}

void CADObject::SetShape(const CADShape& shape) {
    m_shape = shape;
    m_meshDirty = true;
}

std::unique_ptr<CADObject> CADObject::CreateBox(const std::string& name, 
                                               double width, double height, double depth) {
    auto object = std::make_unique<CADObject>(name, CADPrimitiveType::Box);
    
    CADShape shape = CADKernel::Instance().CreateBox(width, height, depth);
    object->SetShape(shape);
    
    return object;
}

std::unique_ptr<CADObject> CADObject::CreateSphere(const std::string& name, 
                                                   const glm::vec3& center, double radius) {
    auto object = std::make_unique<CADObject>(name, CADPrimitiveType::Sphere);
    
    CADShape shape = CADKernel::Instance().CreateSphere(center, radius);
    object->SetShape(shape);
    
    return object;
}

std::unique_ptr<CADObject> CADObject::CreateCylinder(const std::string& name,
                                                     const glm::vec3& center, const glm::vec3& direction,
                                                     double radius, double height) {
    auto object = std::make_unique<CADObject>(name, CADPrimitiveType::Cylinder);
    
    CADShape shape = CADKernel::Instance().CreateCylinder(center, direction, radius, height);
    object->SetShape(shape);
    
    return object;
}

double CADObject::GetVolume() const {
    return m_shape.Volume();
}

void CADObject::GetBoundingBox(glm::vec3& min, glm::vec3& max) const {
    m_shape.GetBoundingBox(min, max);
}

void CADObject::SetPosition(const glm::vec3& position) {
    glm::vec3 delta = position - m_position;
    m_position = position;
    
    if (m_shape.IsValid()) {
        m_shape.Translate(delta);
        m_meshDirty = true;
    }
}

void CADObject::SetRotation(const glm::vec3& axis, double angle) {
    m_rotationAxis = axis;
    m_rotationAngle = angle;
    UpdateTransform();
}

void CADObject::SetScale(double scale) {
    if (scale > 0.0 && scale != m_scale) {
        double factor = scale / m_scale;
        m_scale = scale;
        
        if (m_shape.IsValid()) {
            m_shape.Scale(factor);
            m_meshDirty = true;
        }
    }
}

void CADObject::Union(const CADObject& other) {
    if (m_shape.IsValid() && other.m_shape.IsValid()) {
        m_shape = m_shape.Union(other.m_shape);
        m_meshDirty = true;
    }
}

void CADObject::Intersect(const CADObject& other) {
    if (m_shape.IsValid() && other.m_shape.IsValid()) {
        m_shape = m_shape.Intersection(other.m_shape);
        m_meshDirty = true;
    }
}

void CADObject::Subtract(const CADObject& other) {
    if (m_shape.IsValid() && other.m_shape.IsValid()) {
        m_shape = m_shape.Difference(other.m_shape);
        m_meshDirty = true;
    }
}

CADShape::Mesh CADObject::GetMesh() const {
    return m_shape.GenerateMesh();
}

void CADObject::UpdateTransform() {
    // This is a simplified transform update
    // In a real implementation, you'd want to store the original shape
    // and apply all transforms from scratch
    m_meshDirty = true;
}