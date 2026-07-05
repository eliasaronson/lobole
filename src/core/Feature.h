#pragma once

#include "CADKernel.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

// A named solid produced by regenerating the feature list.
struct Body {
  std::string name;
  CADShape shape;
};

// One step in the parametric history. Features are executed in order by
// Document::Regenerate() to rebuild the model from scratch, so they must be
// deterministic and self-contained (all inputs are parameters, never live
// geometry).
class Feature {
public:
  virtual ~Feature() = default;

  const std::string& GetName() const { return m_name; }
  void SetName(const std::string& name) { m_name = name; }

  virtual std::string TypeName() const = 0;

  // Applies this feature to the model. Returns false on failure, which
  // aborts regeneration of subsequent features.
  virtual bool Execute(std::vector<Body>& bodies) = 0;

protected:
  std::string m_name;
};

class BoxFeature : public Feature {
public:
  BoxFeature(const std::string& name, const glm::vec3& position, double width,
             double height, double depth);

  std::string TypeName() const override { return "Box"; }
  bool Execute(std::vector<Body>& bodies) override;

  glm::vec3 position;
  double width, height, depth;
};
