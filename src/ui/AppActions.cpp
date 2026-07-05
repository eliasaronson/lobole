#include "ui/AppActions.h"

#include "CADMeshConverter.h"
#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h"
#include "OutlineRenderer.h"
#include "core/Feature.h"

#include <cfloat>
#include <filesystem>

namespace ui {

void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc) {
  state.document = std::move(doc);
  state.commands = std::make_unique<CommandStack>();
  state.selectedFeature = -1;
  // Force a resync even if the version counters happen to collide
  state.syncedDocumentVersion = UINT64_MAX;
}

void FitCameraToDocument(AppState& state) {
  if (!state.document || !state.camera || state.document->GetBodies().empty())
    return;

  glm::vec3 min(FLT_MAX), max(-FLT_MAX);
  for (const auto& body : state.document->GetBodies()) {
    glm::vec3 bmin, bmax;
    body.shape.GetBoundingBox(bmin, bmax);
    min = glm::min(min, bmin);
    max = glm::max(max, bmax);
  }
  const glm::vec3 center = (min + max) * 0.5f;
  const float radius = glm::length(max - min) * 0.5f;
  state.camera->SetTarget(center);
  state.camera->SetDistance(std::max(radius * 2.5f, 1.0f));
}

void SyncDocumentRenderers(AppState& state) {
  if (!state.document || !state.meshRenderer)
    return;
  if (state.document->Version() == state.syncedDocumentVersion)
    return;
  state.syncedDocumentVersion = state.document->Version();

  for (const auto& m : state.documentMeshes) {
    state.meshRenderer->RemoveMesh(m.meshId);
    if (state.outlineRenderer)
      state.outlineRenderer->RemoveMesh(m.outlineId);
    if (state.faceSelector)
      state.faceSelector->RemoveMesh(m.meshId);
  }
  state.documentMeshes.clear();

  // Muted, theme-matched palette cycled per body so parts read apart
  static const glm::vec3 kBodyPalette[] = {
      {0.58f, 0.66f, 0.78f}, // steel blue
      {0.80f, 0.62f, 0.42f}, // amber
      {0.55f, 0.74f, 0.62f}, // sage
      {0.76f, 0.58f, 0.70f}, // mauve
      {0.72f, 0.72f, 0.55f}, // olive
  };
  size_t bodyIndex = 0;
  for (const auto& body : state.document->GetBodies()) {
    CADShape::Mesh cadMesh = body.shape.GenerateMesh(0.1);
    if (cadMesh.vertices.empty())
      continue;

    std::vector<Vertex3D> vertices = CADMeshConverter::ConvertCADMeshToVertices(
        cadMesh, kBodyPalette[bodyIndex++ % std::size(kBodyPalette)]);
    uint32_t meshId = state.meshRenderer->AddMesh(vertices, cadMesh.indices);
    uint32_t outlineId = 0;
    if (state.outlineRenderer)
      outlineId = state.outlineRenderer->AddMesh(vertices, cadMesh.indices);
    if (state.faceSelector)
      state.faceSelector->AddMesh(meshId, vertices, cadMesh.indices,
                                  cadMesh.triangleFaces);
    state.documentMeshes.push_back({meshId, outlineId});
  }
  LOG_INFO("Document synced: {} features, {} bodies",
           state.document->FeatureCount(), state.document->GetBodies().size());
}

void LoadSTEPFile(AppState& state, const std::string& filename) {
  if (!state.document || !state.commands)
    return;

  const std::string name = std::filesystem::path(filename).stem().string();
  auto feature = std::make_unique<ImportFeature>(name, filename);
  state.commands->Execute(
      std::make_unique<AddFeatureCommand>(std::move(feature)),
      *state.document);

  if (state.document->GetBodies().empty()) {
    LOG_ERROR("Import of '{}' produced no bodies", filename);
  } else {
    FitCameraToDocument(state);
  }
}

} // namespace ui
