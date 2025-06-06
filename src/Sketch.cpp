#include "Sketch.h"
#include <algorithm>
#include <cmath>

// SketchCircle implementation
std::vector<glm::vec2> SketchCircle::GetPoints() const {
    std::vector<glm::vec2> points;
    const int segments = 32;
    
    for (int i = 0; i <= segments; i++) {
        float angle = (2.0f * M_PI * i) / segments;
        float x = m_center.x + m_radius * cos(angle);
        float y = m_center.y + m_radius * sin(angle);
        points.emplace_back(x, y);
    }
    
    return points;
}

// SketchRectangle implementation
std::vector<glm::vec2> SketchRectangle::GetPoints() const {
    return {
        glm::vec2(m_corner1.x, m_corner1.y),
        glm::vec2(m_corner2.x, m_corner1.y),
        glm::vec2(m_corner2.x, m_corner2.y),
        glm::vec2(m_corner1.x, m_corner2.y),
        glm::vec2(m_corner1.x, m_corner1.y)  // Close the rectangle
    };
}

bool SketchRectangle::IsValid() const {
    return abs(m_corner2.x - m_corner1.x) > 1e-6f && abs(m_corner2.y - m_corner1.y) > 1e-6f;
}

// Sketch implementation
Sketch::Sketch(std::shared_ptr<SketchPlane> plane, const std::string& name)
    : m_name(name), m_plane(plane), m_active(false), m_mode(SketchMode::Select), m_drawing(false) {
}

void Sketch::AddElement(std::shared_ptr<SketchElement> element) {
    if (element && element->IsValid()) {
        m_elements.push_back(element);
    }
}

void Sketch::RemoveElement(size_t index) {
    if (index < m_elements.size()) {
        m_elements.erase(m_elements.begin() + index);
    }
}

void Sketch::ClearElements() {
    m_elements.clear();
}

void Sketch::SelectElement(size_t index) {
    if (index < m_elements.size()) {
        m_elements[index]->SetSelected(true);
    }
}

void Sketch::DeselectAll() {
    for (auto& element : m_elements) {
        element->SetSelected(false);
    }
}

std::vector<size_t> Sketch::GetSelectedElements() const {
    std::vector<size_t> selected;
    for (size_t i = 0; i < m_elements.size(); i++) {
        if (m_elements[i]->IsSelected()) {
            selected.push_back(i);
        }
    }
    return selected;
}

bool Sketch::IsValid() const {
    return std::all_of(m_elements.begin(), m_elements.end(),
                      [](const std::shared_ptr<SketchElement>& element) {
                          return element->IsValid();
                      });
}

bool Sketch::IsClosed() const {
    // Simple check - look for closed elements like circles or rectangles
    return std::any_of(m_elements.begin(), m_elements.end(),
                      [](const std::shared_ptr<SketchElement>& element) {
                          return element->IsClosed();
                      });
}

std::vector<std::vector<glm::vec2>> Sketch::GetClosedProfiles() const {
    std::vector<std::vector<glm::vec2>> profiles;
    
    for (const auto& element : m_elements) {
        if (element->IsClosed()) {
            profiles.push_back(element->GetPoints());
        }
    }
    
    return profiles;
}

void Sketch::OnMouseClick(const glm::vec2& planePos) {
    if (m_mode == SketchMode::Select) {
        // TODO: Implement selection logic
        return;
    }
    
    if (!m_drawing) {
        // Start drawing a new element
        m_drawing = true;
        m_startPoint = planePos;
        
        switch (m_mode) {
            case SketchMode::Point:
                m_currentElement = std::make_shared<SketchPoint>(planePos);
                AddElement(m_currentElement);
                m_drawing = false; // Points are created immediately
                break;
                
            case SketchMode::Line:
                m_currentElement = std::make_shared<SketchLine>(planePos, planePos);
                break;
                
            case SketchMode::Circle:
                m_currentElement = std::make_shared<SketchCircle>(planePos, 0.0f);
                break;
                
            case SketchMode::Rectangle:
                m_currentElement = std::make_shared<SketchRectangle>(planePos, planePos);
                break;
                
            default:
                break;
        }
    } else {
        // Finish drawing the current element
        if (m_currentElement) {
            AddElement(m_currentElement);
            m_currentElement.reset();
        }
        m_drawing = false;
    }
}

void Sketch::OnMouseMove(const glm::vec2& planePos) {
    if (m_drawing && m_currentElement) {
        switch (m_mode) {
            case SketchMode::Line:
                if (auto line = std::dynamic_pointer_cast<SketchLine>(m_currentElement)) {
                    line->SetEnd(planePos);
                }
                break;
                
            case SketchMode::Circle:
                if (auto circle = std::dynamic_pointer_cast<SketchCircle>(m_currentElement)) {
                    float radius = glm::distance(m_startPoint, planePos);
                    circle->SetRadius(radius);
                }
                break;
                
            case SketchMode::Rectangle:
                if (auto rect = std::dynamic_pointer_cast<SketchRectangle>(m_currentElement)) {
                    rect->SetCorner2(planePos);
                }
                break;
                
            default:
                break;
        }
    }
}

void Sketch::OnMouseRelease(const glm::vec2& planePos) {
    // Currently handled in OnMouseClick for simplicity
}