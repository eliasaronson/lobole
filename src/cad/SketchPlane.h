#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <string>

// Named for their GEOMETRY (normal = +Z, +Y, +X respectively). Which one is
// called "Front"/"Top"/"Right" follows the world frame — see
// SketchPlane::StandardPlaneName and core/Conventions.h.
enum class PlaneType { XY, XZ, YZ };

// A sketch plane: origin, normal and in-plane U axis (V = normal x U), in
// double precision like the rest of the model. A plane is where features
// start, and a value rounded here becomes a face microns off the one it was
// meant to meet (docs/sketching/README.md#plane). Only GetPlaneMatrix, which
// exists for the renderers, narrows to float.
class SketchPlane {
public:
  SketchPlane(PlaneType type, const glm::dvec3& origin = glm::dvec3(0.0));
  SketchPlane(const glm::dvec3& origin, const glm::dvec3& normal,
              const glm::dvec3& uAxis);
  ~SketchPlane() = default;

  // Plane properties
  const glm::dvec3& GetOrigin() const { return m_origin; }
  const glm::dvec3& GetNormal() const { return m_normal; }
  const glm::dvec3& GetUAxis() const { return m_uAxis; }
  const glm::dvec3& GetVAxis() const { return m_vAxis; }

  PlaneType GetType() const { return m_type; }
  const std::string& GetName() const { return m_name; }

  // "Front Plane" / "Top Plane" / "Right Plane" for a base plane type,
  // derived from the world frame (core/Conventions.h).
  static const char* StandardPlaneName(PlaneType type);
  // Combo-row spelling, name + geometry: "Top (XY)". Index 0/1/2 = XY/XZ/YZ,
  // matching the serialized base-plane order.
  static const char* BasePlaneLabel(int index);

  // Coordinate transformations, in float for the renderers.
  glm::mat4 GetPlaneMatrix() const;        // Local to world transform
  glm::mat4 GetInversePlaneMatrix() const; // World to local transform

  // Convert between 3D world coordinates and 2D plane coordinates
  glm::dvec2 WorldToPlane(const glm::dvec3& worldPos) const;
  glm::dvec3 PlaneToWorld(const glm::dvec2& planePos) const;

  // Plane visibility and interaction
  bool IsVisible() const { return m_visible; }
  void SetVisible(bool visible) { m_visible = visible; }

  bool IsActive() const { return m_active; }
  void SetActive(bool active) { m_active = active; }

  // Grid properties
  float GetGridSize() const { return m_gridSize; }
  void SetGridSize(float size) { m_gridSize = size; }

  int GetGridSubdivisions() const { return m_gridSubdivisions; }
  void SetGridSubdivisions(int subdivisions) {
    m_gridSubdivisions = subdivisions;
  }

private:
  void UpdateAxes();
  void SetNameFromType();

  PlaneType m_type;
  std::string m_name;

  glm::dvec3 m_origin;
  glm::dvec3 m_normal;
  glm::dvec3 m_uAxis; // U direction (first tangent vector)
  glm::dvec3 m_vAxis; // V direction (second tangent vector)

  bool m_visible = true;
  bool m_active = false;

  // Grid settings
  float m_gridSize = 1.0f;
  int m_gridSubdivisions = 10;
};
