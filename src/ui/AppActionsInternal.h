#pragma once

// Internal (non-public) shared helpers for the AppActions* domain translation
// units carved out of AppActions.cpp. Only those TUs include this. The tiny
// LineVertex constructor is used by every overlay/marker builder across the
// split, so it lives here (inline); the heavier tab-slot accessors
// (SlotDoc/SlotAssembly) are declared here and defined in AppActions.cpp.

#include "render/MeshRenderer.h"  // LineVertex3D

#include <glm/glm.hpp>

#include <cstdint>

class Document;
class AssemblyDocument;
struct Occurrence;

namespace ui {

struct AppState;

inline LineVertex3D LineVertex(const glm::vec3& pos, const glm::vec3& color) {
  LineVertex3D v;
  v.position = pos;
  v.color = color;
  return v;
}

// Assembly-domain helpers defined in AppActions.cpp (they stay because the
// core lifecycle/renderer/drawing code also uses them) but called by the
// extracted AssemblyActions.cpp TU. OccurrenceMatrix and PartDocById moved to
// AppActions.h — the RPC layer resolves cross-part queries through them.
glm::vec3 ExplodeOffset(const AppState& state, uint64_t occId);
glm::mat4 DisplayMatrix(const AppState& state, const Occurrence& o);
extern const glm::vec3 kBodyPalette[5];

// Defined in AssemblyActions.cpp; called by SyncDocumentRenderers (AppActions).
void SyncAssemblyRenderers(AppState& state);
void SyncInContextGhosts(AppState& state);

}  // namespace ui
