#include "ui/ImportActions.h"
#include "ui/RendererSync.h"
#include "ui/ViewActions.h"

#include "core/Units.h"
#include "ui/AppActionsInternal.h" // SharedBody / PartRenderCacheStore
#include "ui/CamPanel.h"           // RefreshCamToolpaths (job undo)
#include "ui/CamTool.h"            // RunCamVerification (job undo)
#include "ui/JobActions.h"

#include "imgui.h" // IsAnyItemActive (the undo guard coalesces a drag)         // JobBodies / JobRenderVersion (a job tab)
#include <chrono>
#include <utility>

#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/ElementHistory.h" // topo::ResolveEdgeOnBody (blend edge picks)
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/FrameFeature.h"
#include "core/fem/FemTopo.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectStamp.h"
#include "core/io/RecentProjects.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"

#include "core/features/DraftFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/AssemblyActions.h"
#include "ui/CamPlayback.h"   // SyncCamPlayback (SyncRenderersToDocument)
#include "ui/FastenerTool.h"  // SyncFastenerGhost (SyncRenderersToDocument)
#include "ui/FeatureEditor.h" // PickSlot (thread face slot)
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (Hole select-first)
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include "thirdparty/stb_image.h" // stbi_info_from_memory (impl in IBL.cpp)

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>

// Bringing something in from outside: a mesh or STEP file as a feature, and
// the reference images a sketch is traced over (insert, calibrate, delete).
// Declared in ui/ImportActions.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// Reference images (v49): read + decode dims, append a centered ReferenceImage
// to `sk`, and stash the raw bytes in the document asset store. Returns the new
// image id, or 0 on read failure. The pixel bytes live on the Document (not the
// feature) so they survive undo/redo; see
// docs/sketching/README.md#reference-images-underlays.
static uint32_t AppendReferenceImage(AppState& state, SketchFeature* sk,
                                     const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return 0;
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (data.empty())
    return 0;
  std::vector<unsigned char> bytes(data.begin(), data.end());

  sketch::ReferenceImage img;
  int w = 0, h = 0, comp = 0;
  if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w,
                            &h, &comp)) {
    img.widthPx = w;
    img.heightPx = h;
  }
  img.unitsPerPixel = w > 0 ? 4.0 / w : 0.01; // ~4 units wide until calibrated
  img.opacity = 0.5;
  const uint32_t id = sk->AddImage(img);
  sketch::ReferenceImage& ri = sk->images[sk->IndexOfImage(id)];
  std::string ext = std::filesystem::path(path).extension().string();
  if (ext.empty())
    ext = ".png";
  ri.asset = std::to_string(sk->GetId()) + "-" + std::to_string(id) + ext;
  ri.origin = -0.5f * glm::vec2(float(ri.widthPx), float(ri.heightPx)) *
              float(ri.unitsPerPixel); // centre on the plane origin
  state.document->SetAsset(ri.asset, std::move(bytes));
  return id;
}

uint32_t AddReferenceImageToSketch(AppState& state, const std::string& path) {
  if (!state.document || !state.commands)
    return 0;
  size_t index = 0;
  SketchFeature* sk = ResolveActiveSketch(state, &index);
  if (!sk) {
    state.sketchEdit.actionMsg =
        "Open a sketch for editing before adding a reference image.";
    return 0;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  const uint32_t id = AppendReferenceImage(state, sk, path);
  if (id == 0) {
    state.sketchEdit.actionMsg = "Could not read image: " + path;
    return 0;
  }
  state.document->Regenerate();
  state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                              index, before, DocumentIO::FeatureToJson(*sk)),
                          *state.document);
  state.sketchEdit.selectedImageId = id;
  return id;
}

void CalibrateReferenceImageAction(AppState& state, uint32_t imageId,
                                   glm::vec2 p1, glm::vec2 p2, double distance,
                                   bool alignAxis) {
  if (!state.document || !state.commands)
    return;
  size_t index = 0;
  SketchFeature* sk = ResolveActiveSketch(state, &index);
  if (!sk)
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  if (!sk->CalibrateImage(imageId, p1, p2, distance, alignAxis ? 0 : -1)) {
    state.sketchEdit.actionMsg =
        "Calibration needs two distinct points and a positive distance.";
    return;
  }
  state.document->Regenerate();
  state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                              index, before, DocumentIO::FeatureToJson(*sk)),
                          *state.document);
}

void DeleteReferenceImageAction(AppState& state, uint32_t imageId) {
  if (!state.document || !state.commands)
    return;
  size_t index = 0;
  SketchFeature* sk = ResolveActiveSketch(state, &index);
  if (!sk)
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  if (!sk->DeleteImage(imageId))
    return;
  state.document->Regenerate();
  state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                              index, before, DocumentIO::FeatureToJson(*sk)),
                          *state.document);
  if (state.sketchEdit.selectedImageId == imageId)
    state.sketchEdit.selectedImageId = 0;
}

bool ImportAsFeature(AppState& state, const std::string& filename,
                     std::string* error) {
  return ImportAsFeature(state, filename, nullptr, error);
}

bool ImportAsFeature(AppState& state, const std::string& filename,
                     CADKernel::ImportedModel* parsed, std::string* error,
                     units::Length unit, bool keepOrigin) {
  if (!state.document || !state.commands) {
    if (error)
      *error = "no active part document";
    return false;
  }

  const size_t before = state.document->GetBodies().size();
  const std::string name = std::filesystem::path(filename).stem().string();
  auto feature = std::make_unique<ImportFeature>(name, filename);
  feature->lengthUnit = unit;
  feature->keepOrigin = keepOrigin;
  // The import worker already read this file to decide it was a single
  // product; hand that over rather than parse it again (ImportFeature::
  // SeedGeometry). Only the first Execute uses it.
  if (parsed != nullptr)
    feature->SeedGeometry(std::move(*parsed));
  state.commands->Execute(
      std::make_unique<AddFeatureCommand>(std::move(feature)), *state.document);

  if (state.document->GetBodies().size() <= before) {
    // The feature carries the specific reason (bad read, empty file, …).
    std::string why = "produced no bodies";
    if (const size_t n = state.document->FeatureCount())
      if (const Feature* f = state.document->GetFeature(n - 1);
          f && !f->GetError().empty())
        why = f->GetError();
    if (error)
      *error = "import of '" + filename + "': " + why;
    LOG_ERROR("Import of '{}': {}", filename, why);
    return false;
  }
  FitCameraToDocument(state);
  return true;
}

} // namespace ui
