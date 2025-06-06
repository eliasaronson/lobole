#pragma once

#include "SketchPlane.h"
#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <string>

enum class SketchElementType {
    Point,
    Line,
    Circle,
    Arc,
    Rectangle,
    Polygon
};

class SketchElement {
public:
    SketchElement(SketchElementType type) : m_type(type), m_selected(false) {}
    virtual ~SketchElement() = default;
    
    SketchElementType GetType() const { return m_type; }
    bool IsSelected() const { return m_selected; }
    void SetSelected(bool selected) { m_selected = selected; }
    
    virtual std::vector<glm::vec2> GetPoints() const = 0;
    virtual bool IsValid() const = 0;
    virtual bool IsClosed() const = 0;

protected:
    SketchElementType m_type;
    bool m_selected;
};

class SketchPoint : public SketchElement {
public:
    SketchPoint(const glm::vec2& position) 
        : SketchElement(SketchElementType::Point), m_position(position) {}
    
    std::vector<glm::vec2> GetPoints() const override { return {m_position}; }
    bool IsValid() const override { return true; }
    bool IsClosed() const override { return false; }
    
    const glm::vec2& GetPosition() const { return m_position; }
    void SetPosition(const glm::vec2& pos) { m_position = pos; }

private:
    glm::vec2 m_position;
};

class SketchLine : public SketchElement {
public:
    SketchLine(const glm::vec2& start, const glm::vec2& end)
        : SketchElement(SketchElementType::Line), m_start(start), m_end(end) {}
    
    std::vector<glm::vec2> GetPoints() const override { return {m_start, m_end}; }
    bool IsValid() const override { return glm::distance(m_start, m_end) > 1e-6f; }
    bool IsClosed() const override { return false; }
    
    const glm::vec2& GetStart() const { return m_start; }
    const glm::vec2& GetEnd() const { return m_end; }
    void SetStart(const glm::vec2& start) { m_start = start; }
    void SetEnd(const glm::vec2& end) { m_end = end; }

private:
    glm::vec2 m_start, m_end;
};

class SketchCircle : public SketchElement {
public:
    SketchCircle(const glm::vec2& center, float radius)
        : SketchElement(SketchElementType::Circle), m_center(center), m_radius(radius) {}
    
    std::vector<glm::vec2> GetPoints() const override;
    bool IsValid() const override { return m_radius > 1e-6f; }
    bool IsClosed() const override { return true; }
    
    const glm::vec2& GetCenter() const { return m_center; }
    float GetRadius() const { return m_radius; }
    void SetCenter(const glm::vec2& center) { m_center = center; }
    void SetRadius(float radius) { m_radius = radius; }

private:
    glm::vec2 m_center;
    float m_radius;
};

class SketchRectangle : public SketchElement {
public:
    SketchRectangle(const glm::vec2& corner1, const glm::vec2& corner2)
        : SketchElement(SketchElementType::Rectangle), m_corner1(corner1), m_corner2(corner2) {}
    
    std::vector<glm::vec2> GetPoints() const override;
    bool IsValid() const override;
    bool IsClosed() const override { return true; }
    
    const glm::vec2& GetCorner1() const { return m_corner1; }
    const glm::vec2& GetCorner2() const { return m_corner2; }
    void SetCorner1(const glm::vec2& corner) { m_corner1 = corner; }
    void SetCorner2(const glm::vec2& corner) { m_corner2 = corner; }

private:
    glm::vec2 m_corner1, m_corner2;
};

enum class SketchMode {
    Select,
    Point,
    Line,
    Circle,
    Rectangle
};

class Sketch {
public:
    Sketch(std::shared_ptr<SketchPlane> plane, const std::string& name = "Sketch");
    ~Sketch() = default;
    
    // Sketch properties
    const std::string& GetName() const { return m_name; }
    void SetName(const std::string& name) { m_name = name; }
    
    std::shared_ptr<SketchPlane> GetPlane() const { return m_plane; }
    
    bool IsActive() const { return m_active; }
    void SetActive(bool active) { m_active = active; }
    
    // Drawing mode
    SketchMode GetMode() const { return m_mode; }
    void SetMode(SketchMode mode) { m_mode = mode; }
    
    // Elements management
    void AddElement(std::shared_ptr<SketchElement> element);
    void RemoveElement(size_t index);
    void ClearElements();
    
    const std::vector<std::shared_ptr<SketchElement>>& GetElements() const { return m_elements; }
    size_t GetElementCount() const { return m_elements.size(); }
    
    // Selection
    void SelectElement(size_t index);
    void DeselectAll();
    std::vector<size_t> GetSelectedElements() const;
    
    // Sketch validation
    bool IsValid() const;
    bool IsClosed() const; // Check if sketch forms closed profiles
    std::vector<std::vector<glm::vec2>> GetClosedProfiles() const;
    
    // Mouse interaction
    void OnMouseClick(const glm::vec2& planePos);
    void OnMouseMove(const glm::vec2& planePos);
    void OnMouseRelease(const glm::vec2& planePos);
    
    // Temporary drawing state (for active drawing)
    bool IsDrawing() const { return m_drawing; }
    std::shared_ptr<SketchElement> GetCurrentElement() const { return m_currentElement; }

private:
    std::string m_name;
    std::shared_ptr<SketchPlane> m_plane;
    bool m_active;
    
    SketchMode m_mode;
    std::vector<std::shared_ptr<SketchElement>> m_elements;
    
    // Drawing state
    bool m_drawing;
    std::shared_ptr<SketchElement> m_currentElement;
    glm::vec2 m_startPoint;
};