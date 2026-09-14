#pragma once

#include "cad/MeshExport.h" // MeshExport::Item
#include "core/AssemblySolve.h"
#include "core/ElementMap.h"               // ElementRef
#include "core/drawing/DrawingDimension.h" // drawing::ResolvedDim
#include "core/io/Autosaver.h"
#include "render/SketchRenderer.h" // Vertex3D, PrimVertex
#include "ui/AppState.h"

#include <cstdint>
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
// docs/architecture-review-2026-09-06.md, stage 4.

#include "core/Units.h"

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
bool ImportAsFeature(AppState& state, const std::string& filename,
                     CADKernel::ImportedModel* parsed, std::string* error,
                     units::Length unit = units::Length::Millimetre,
                     bool keepOrigin = false);

// Reference-image underlays (v49). Add reads/decodes the file and centres it on
// the active sketch's plane; Calibrate scales it so p1..p2 spans `distance`
// (p1 fixed; alignAxis also rotates p1->p2 onto the U axis); Delete removes it.
// All commit one undoable ModifyFeatureCommand. See
// docs/sketching/reference-image-plan.md.
uint32_t AddReferenceImageToSketch(AppState& state, const std::string& path);

void CalibrateReferenceImageAction(AppState& state, uint32_t imageId,
                                   glm::vec2 p1, glm::vec2 p2, double distance,
                                   bool alignAxis);

void DeleteReferenceImageAction(AppState& state, uint32_t imageId);

} // namespace ui
