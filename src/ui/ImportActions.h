// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/MeshExport.h" // MeshExport::Item
#include "core/AssemblySolve.h"
#include "core/ElementMap.h"               // ElementRef
#include "core/drawing/DrawingDimension.h" // drawing::ResolvedDim
#include "core/io/Autosaver.h"
#include "render/SketchRenderer.h" // Vertex3D, PrimVertex
#include "ui/AppState.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SketchPlane;
class SketchFeature;
class SplitFeature;
class FilletFeature; // defined in core/features/FilletFeature.h
struct RTStillDesc;  // defined in RayTracingRenderer.h
struct Ray;          // defined in FaceSelector.h

// Bringing something in from outside: files, and reference images.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

#include "core/ImportPlacement.h"
#include "core/Units.h"
#include "core/io/ImportUpAxis.h"
#include "core/mesh/Representation.h"

#include <nlohmann/json.hpp>

namespace ui {

// Imports a CAD file (.step/.stp, .brep, .stl) into the ACTIVE part as an
// undoable ImportFeature and fits the camera. False (with *error set) when
// nothing imported.
bool ImportAsFeature(AppState& state, const std::string& filename,
                     std::string* error = nullptr);

// Same, for a caller that has ALREADY parsed the file. The GUI import worker
// parses it to decide part-vs-assembly (ui/ImportJob.cpp); without this a
// single-product STEP is then read a SECOND time by the feature's first
// regen — the whole file twice, ~12 s each at 48 MB. `parsed` is moved from,
// and a later regen re-reads the path exactly as before.
// `unit` is the length unit an STL/BREP was drawn in (ImportFeature::
// lengthUnit); a STEP refuses anything but the default.
// `keepOrigin` keeps the file's own coordinates instead of centring the
// import on its bounding box — what a neighbour part or a fixture needs,
// and the only way a file's placement survives the round trip.
// `up` and `turnDeg` are ImportFeature::upAxis / turnDeg: which axis of the
// file is up (auto reads its header) and a turn about world Z after it. An
// import that `auto` turned says so in a viewport pill (state.importNote).
// `representation` and `minSides` are ImportFeature's, for a mesh: absent,
// the user's `mesh_representation` preference (MeshRepresentationSetting).
// `placement` is ImportFeature::placement: scaled, turned and moved after
// the up axis and the centring.
bool ImportAsFeature(
    AppState& state, const std::string& filename,
    CADKernel::ImportedModel* parsed, std::string* error,
    units::Length unit = units::Length::Millimetre, bool keepOrigin = false,
    interchange::UpAxis up = interchange::UpAxis::Auto, double turnDeg = 0.0,
    std::optional<mesh::Representation> representation = std::nullopt,
    int minSides = mesh::kDefaultMinSides,
    const importplace::Placement& placement = {});

// What a new mesh import becomes when it names nothing: the preference
// (docs/ui/settings.md), which the settings store keeps to the names
// mesh::RepresentationFromString reads.
mesh::Representation MeshRepresentationSetting(const AppState& state);
// What `path` is imported as: `asked` when given, the preference for a mesh,
// facets for anything else (which takes no representation).
// The features of `doc` that refer to `imp` ("'Sketch 3' (7), 'Offset' (9)"),
// or "": while any does, changing what its mesh becomes would renumber the
// faces they name, so the change is refused (the RPC) or offered disabled
// (the panel).
std::string ImportReferrers(const Document& doc, const ImportFeature& imp);
mesh::Representation
ResolveRepresentation(const AppState& state, const std::string& path,
                      std::optional<mesh::Representation> asked);

// A DXF or SVG into a sketch: the import_sketch RPC and File > Import share
// it. `sketchId` (non-zero) adds to that sketch; else a new Sketch on `plane`
// (a primitive `on`: "Top" | {feature_id, element} | {feature_id}; null =
// Top). The file is parsed and every parameter checked before anything is
// created, and the change is one undo step. Throws std::runtime_error with
// the refusal.
struct SketchImportRequest {
  std::string path;
  uint64_t sketchId = 0;
  nlohmann::json plane; // null = Top
  std::string units;    // "" = the file's, else the default
  std::vector<std::string> layers;
  std::vector<std::string> constructionLayers;
  bool keepOrigin = false;
};
struct SketchImportResult {
  uint64_t sketchId = 0;
  std::vector<uint32_t> entityIds;
  std::map<std::string, int> imported;    // by sketch entity type
  std::map<std::string, int> notImported; // by the file's kind
  std::vector<std::string> notes;
  std::string unit, unitSource;
  bool recentred = false;
  glm::dvec2 recentredBy{0.0};
  std::vector<glm::dvec2> openEnds; // sketch UV, after the centring
};
SketchImportResult ImportSketch(AppState& state,
                                const SketchImportRequest& req);
// One line for a pill: what came in, its unit and where that came from, what
// was left out, and whether it is open.
std::string SketchImportSummary(const SketchImportResult& r,
                                const std::string& path);

// A DXF or SVG into a sketch (docs/sketching/README.md#importing-dxf-and-svg)
// from File > Import: into the sketch being edited, else a new sketch on the
// preselected plane or face, else Top. One undo step; the pill says what came
// in and what did not. False (with *error) when the file was refused, which
// changes nothing.
bool ImportSketchFile(AppState& state, const std::string& filename,
                      std::string* error);
// Whether File > Import sends `filename` to ImportSketchFile (.dxf, .svg).
bool IsSketchImportFile(const std::string& filename);

// Reference-image underlays (v49). Add reads/decodes the file and centres it on
// the active sketch's plane; Calibrate scales it so p1..p2 spans `distance`
// (p1 fixed; alignAxis also rotates p1->p2 onto the U axis); Delete removes it.
// All commit one undoable ModifyFeatureCommand. See
// docs/sketching/README.md#reference-images-underlays.
uint32_t AddReferenceImageToSketch(AppState& state, const std::string& path);

void CalibrateReferenceImageAction(AppState& state, uint32_t imageId,
                                   glm::vec2 p1, glm::vec2 p2, double distance,
                                   bool alignAxis);

void DeleteReferenceImageAction(AppState& state, uint32_t imageId);

} // namespace ui
