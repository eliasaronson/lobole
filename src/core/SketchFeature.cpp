#include "core/SketchFeature.h"

SketchFeature::SketchFeature(const std::string& name, const SketchPlane& p)
    : plane(p) {
  m_name = name;
}
