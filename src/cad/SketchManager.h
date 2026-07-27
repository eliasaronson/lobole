#pragma once

#include "cad/Sketch.h"
#include "cad/SketchPlane.h"
#include "render/Camera.h"
#include <memory>
#include <vector>

enum class SketchManagerMode {
  ThreeD, // 3D modeling mode
  Sketch  // 2D sketching mode
};

class SketchManager {
public:
  SketchManager();
  ~SketchManager() = default;

  // Mode management
  SketchManagerMode GetMode() const { return m_mode; }
  bool IsSketchMode() const { return m_mode == SketchManagerMode::Sketch; }

  // Plane management
  void InitializeDefaultPlanes();
  const std::vector<std::shared_ptr<SketchPlane>>& GetPlanes() const {
    return m_planes;
  }
  std::shared_ptr<SketchPlane> GetActivePlane() const { return m_activePlane; }

  // Start sketching on a plane
  bool StartSketch(std::shared_ptr<SketchPlane> plane);
  bool StartSketch(PlaneType planeType);
  void ExitSketch();

  // Active sketch management
  std::shared_ptr<Sketch> GetActiveSketch() const { return m_activeSketch; }
  bool HasActiveSketch() const { return m_activeSketch != nullptr; }

  // Sketch operations
  void SetSketchMode(SketchMode mode);
  SketchMode GetSketchMode() const;

  // Mouse interaction (in sketch mode)
  void OnMouseClick(double x, double y, const Camera& camera);
  void OnMouseMove(double x, double y, const Camera& camera);
  void OnMouseRelease(double x, double y, const Camera& camera);

  // Ray casting for plane intersection
  glm::vec3 RayPlaneIntersection(const glm::vec3& rayOrigin,
                                 const glm::vec3& rayDirection,
                                 std::shared_ptr<SketchPlane> plane) const;

  // Screen to world ray conversion
  void ScreenToWorldRay(double screenX, double screenY, const Camera& camera,
                        glm::vec3& rayOrigin, glm::vec3& rayDirection) const;

  // Sketch validation and operations
  bool CanExtrude() const;
  bool CanCut() const;

private:
  SketchManagerMode m_mode;

  // Planes
  std::vector<std::shared_ptr<SketchPlane>> m_planes;
  std::shared_ptr<SketchPlane> m_activePlane;

  // Active sketch
  std::shared_ptr<Sketch> m_activeSketch;

  // Screen/viewport info (needed for ray casting)
  mutable int m_viewportWidth = 1280;
  mutable int m_viewportHeight = 720;

public:
  void SetViewportSize(int width, int height) {
    m_viewportWidth = width;
    m_viewportHeight = height;
  }
};