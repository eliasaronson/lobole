// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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
#include "core/Primitives.h"
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
#include "core/io/ImportDxf.h"
#include "core/io/ImportSvg.h"
#include "core/io/ProjectStamp.h"
#include "core/io/ReadContext.h" // CheckFeatureRoom
#include "core/io/RecentProjects.h"
#include "core/io/Serialization.h"
#include "core/io/SketchImport.h"
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

#include "common/ImageLimits.h"
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
// image id, or 0 on read failure, with the reason in `why` when the file was
// read but is not an image the app takes. The pixel bytes live on the
// Document (not the feature) so they survive undo/redo; see
// docs/sketching/README.md#reference-images-underlays.
static uint32_t AppendReferenceImage(AppState& state, SketchFeature* sk,
                                     const std::string& path,
                                     std::string* why) {
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
  if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w,
                             &h, &comp))
    w = h = 0;
  if (const char* problem = ImageSizeProblem(w, h)) {
    *why = "Cannot use " + path + ": " + problem +
           " (accepted: " + kImageFormats + ", up to " +
           std::to_string(kMaxImageSide) + " px a side).";
    return 0;
  }
  img.widthPx = w;
  img.heightPx = h;
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
  std::string why;
  const uint32_t id = AppendReferenceImage(state, sk, path, &why);
  if (id == 0) {
    state.sketchEdit.actionMsg =
        why.empty() ? "Could not read image: " + path : why;
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
                     units::Length unit, bool keepOrigin,
                     interchange::UpAxis up, double turnDeg) {
  if (!state.document || !state.commands) {
    if (error)
      *error = "no active part document";
    return false;
  }

  if (const std::string why = readctx::CheckFeatureRoom(*state.document, 1);
      !why.empty()) {
    if (error)
      *error = why;
    return false;
  }
  const size_t before = state.document->GetBodies().size();
  const std::string name = std::filesystem::path(filename).stem().string();
  auto feature = std::make_unique<ImportFeature>(name, filename);
  feature->lengthUnit = unit;
  feature->keepOrigin = keepOrigin;
  feature->upAxis = up;
  feature->turnDeg = turnDeg;
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
  // A part `auto` turned says so where the user is looking: a model that
  // arrives standing differently from the file is a surprise unless it is
  // explained, and the editor is where it is undone.
  if (const size_t n = state.document->FeatureCount())
    if (const auto* imp = dynamic_cast<const ImportFeature*>(
            state.document->GetFeature(n - 1));
        imp != nullptr &&
        imp->UpApplied().requested == interchange::UpAxis::Auto &&
        imp->UpApplied().applied != interchange::UpAxis::None) {
      state.importNote =
          "Stood up (" +
          std::string(interchange::ToString(imp->UpApplied().applied)) +
          " was up in the file): " + imp->UpApplied().because +
          ". Change it in the import's editor.";
      state.importNoteWarn = false;
      state.importNoteUntil = -1.0;
    }
  FitCameraToDocument(state);
  return true;
}

namespace {

std::string LowerExt(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  for (char& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

const char* EntityKind(const sketch::Entity& e) {
  return std::visit(
      [](const auto& x) -> const char* {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, sketch::Point>)
          return "point";
        else if constexpr (std::is_same_v<T, sketch::Line>)
          return "line";
        else if constexpr (std::is_same_v<T, sketch::Circle>)
          return "circle";
        else if constexpr (std::is_same_v<T, sketch::Arc>)
          return "arc";
        else if constexpr (std::is_same_v<T, sketch::Ellipse>)
          return "ellipse";
        else if constexpr (std::is_same_v<T, sketch::EllipseArc>)
          return "ellipse_arc";
        else if constexpr (std::is_same_v<T, sketch::BSpline>)
          return "spline";
        else
          return "text";
      },
      e);
}

} // namespace

bool IsSketchImportFile(const std::string& filename) {
  const std::string ext = LowerExt(filename);
  return ext == ".dxf" || ext == ".svg";
}

SketchImportResult ImportSketch(AppState& state,
                                const SketchImportRequest& req) {
  if (!state.document || !state.commands)
    throw std::runtime_error("no active part document");
  Document& doc = *state.document;

  // Everything is read and checked before anything is created: the file is
  // parsed whole (a refusal names what it refused), the target resolved.
  const std::string ext = LowerExt(req.path);
  sketchimport::Drawing drawing;
  if (ext == ".dxf") {
    dxfimport::Options opt;
    if (!req.units.empty()) {
      units::Length u;
      if (!units::LengthFromString(req.units, u)) {
        std::string accepted;
        for (const char* n : units::LengthNames())
          accepted += (accepted.empty() ? "" : ", ") + std::string(n);
        throw std::runtime_error("units: unknown '" + req.units +
                                 "' for a DXF. Accepted: " + accepted);
      }
      opt.units = u;
    }
    opt.layers = req.layers;
    opt.constructionLayers = req.constructionLayers;
    drawing = dxfimport::Read(req.path, opt);
  } else if (ext == ".svg") {
    if (!req.layers.empty() || !req.constructionLayers.empty())
      throw std::runtime_error("layers: an SVG has no layers this reader "
                               "takes; they are for a DXF");
    svgimport::Options opt;
    opt.units = req.units;
    drawing = svgimport::Read(req.path, opt);
  } else {
    throw std::runtime_error("path: '" + req.path +
                             "' is not a drawing import_sketch reads "
                             "(accepted: .dxf, .svg; a 3D file goes in with "
                             "import_step)");
  }
  if (drawing.entities.empty())
    throw std::runtime_error("'" + req.path + "' has nothing to import" +
                             (drawing.notImported.empty()
                                  ? std::string()
                                  : std::string(" (only annotation, which is "
                                                "left out)")));

  SketchFeature* target = nullptr;
  size_t targetIndex = 0;
  std::optional<primitive::Placement> placement;
  primitive::On on;
  if (req.sketchId != 0) {
    target = dynamic_cast<SketchFeature*>(
        doc.FindFeatureById(req.sketchId, &targetIndex));
    if (target == nullptr)
      throw std::runtime_error("sketch_id: " + std::to_string(req.sketchId) +
                               " is not a sketch in this part");
  } else {
    on = primitive::OnFromJson(req.plane.is_null() ? nlohmann::json("Top")
                                                   : req.plane);
    auto resolved = primitive::Resolve(doc, on);
    if (!resolved)
      throw std::runtime_error("plane: " + resolved.error());
    placement = *resolved;
    // A new sketch: the one feature this adds.
    if (const std::string why = readctx::CheckFeatureRoom(doc, 1); !why.empty())
      throw std::runtime_error(why);
  }

  // Centred on the sketch origin unless the file's coordinates are wanted.
  SketchImportResult result;
  glm::dvec2 lo, hi;
  if (!req.keepOrigin && sketchimport::Bounds(drawing.entities, lo, hi)) {
    const glm::dvec2 shift = -0.5 * (lo + hi);
    if (shift != glm::dvec2(0.0)) {
      const sketchimport::Affine2 move{glm::dmat2(1.0), shift};
      for (sketch::Entity& e : drawing.entities)
        e = sketchimport::TransformEntity(e, move);
      result.recentredBy = shift;
      result.recentred = true;
    }
  }
  sketchimport::SnapEndpoints(drawing.entities, sketchimport::kChainTol);
  result.openEnds =
      sketchimport::OpenEnds(drawing.entities, sketchimport::kChainTol);
  for (const sketch::Entity& e : drawing.entities)
    result.imported[EntityKind(e)]++;
  result.notImported = drawing.notImported;
  result.notes = drawing.notes;
  result.unit = drawing.unit;
  result.unitSource = drawing.unitSource;

  // One undo step either way.
  const std::string name = std::filesystem::path(req.path).stem().string();
  if (target != nullptr) {
    const nlohmann::json before = DocumentIO::FeatureToJson(*target);
    for (sketch::Entity& e : drawing.entities)
      result.entityIds.push_back(target->AddEntity(std::move(e)));
    doc.Regenerate();
    // Read before the command: it replaces the feature, and `target` is
    // left pointing at the one it freed (ASan, regen_honesty_smoke).
    result.sketchId = target->GetId();
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(
            targetIndex, before, DocumentIO::FeatureToJson(*target)),
        doc);
  } else {
    auto sk = std::make_unique<SketchFeature>(name, placement->plane);
    switch (on.kind) {
    case primitive::OnKind::Face:
      sk->faceRef = on.face;
      break;
    case primitive::OnKind::PlaneFeature:
      sk->planeRef = on.featureId;
      break;
    case primitive::OnKind::WorldPlane:
      break;
    }
    for (sketch::Entity& e : drawing.entities)
      result.entityIds.push_back(sk->AddEntity(std::move(e)));
    state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(sk)),
                            doc);
    result.sketchId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  }
  return result;
}

std::string SketchImportSummary(const SketchImportResult& r,
                                const std::string& path) {
  size_t total = 0;
  for (const auto& [kind, n] : r.imported)
    total += static_cast<size_t>(n);
  std::string out = "Imported " + std::to_string(total) + " entities from " +
                    std::filesystem::path(path).filename().string() + " (" +
                    r.unit + ", from " + r.unitSource + ")";
  if (!r.notImported.empty()) {
    std::string left;
    for (const auto& [kind, n] : r.notImported)
      left += (left.empty() ? "" : ", ") + std::to_string(n) + " " + kind;
    out += "; left out: " + left;
  }
  if (!r.openEnds.empty())
    out += "; " + std::to_string(r.openEnds.size()) +
           " open ends, so it will not extrude as it is";
  return out;
}

bool ImportSketchFile(AppState& state, const std::string& filename,
                      std::string* error) {
  SketchImportRequest req;
  req.path = filename;
  // Into the sketch being edited, else onto the preselected face or datum
  // plane, else Top.
  if (state.sketchEdit.IsEditing()) {
    req.sketchId = state.sketchEdit.activeSketchId;
  } else if (const SelectionItem* item =
                 state.selection.First(SelectionItem::Kind::Face)) {
    uint32_t meshId = 0, faceIndex = 0;
    if (ResolveFaceItem(state, *item, meshId, faceIndex))
      if (const auto ref = FaceElementRef(state, meshId, faceIndex))
        req.plane = primitive::OnToJson(
            {primitive::OnKind::Face, PlaneType::XY, *ref, 0});
  } else if (auto* datum =
                 dynamic_cast<DatumPlaneFeature*>(SelectedFeature(state))) {
    req.plane = nlohmann::json{{"feature_id", datum->GetId()}};
  }
  try {
    const SketchImportResult r = ImportSketch(state, req);
    const std::string summary = SketchImportSummary(r, filename);
    if (state.sketchEdit.IsEditing())
      state.sketchEdit.actionMsg = summary;
    state.importNote = summary;
    state.importNoteWarn = !r.openEnds.empty();
    state.importNoteUntil = -1.0;
    return true;
  } catch (const std::exception& e) {
    const std::string why =
        "Import of " + std::filesystem::path(filename).filename().string() +
        " refused: " + e.what();
    if (error)
      *error = why;
    if (state.sketchEdit.IsEditing())
      state.sketchEdit.actionMsg = why;
    state.importNote = why;
    state.importNoteWarn = true;
    state.importNoteUntil = -1.0;
    return false;
  }
}

} // namespace ui
