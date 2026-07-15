#pragma once

// Internal (non-public) shared helpers for the AppActions* domain translation
// units carved out of AppActions.cpp. Only those TUs include this. The tiny
// LineVertex constructor is used by every overlay/marker builder across the
// split, so it lives here (inline); the heavier tab-slot accessors
// (SlotDoc/SlotAssembly) are declared here and defined in AppActions.cpp.

#include "MeshRenderer.h"  // LineVertex3D

#include <glm/glm.hpp>

class Document;
class AssemblyDocument;

namespace ui {

struct AppState;

inline LineVertex3D LineVertex(const glm::vec3& pos, const glm::vec3& color) {
  LineVertex3D v;
  v.position = pos;
  v.color = color;
  return v;
}

}  // namespace ui
