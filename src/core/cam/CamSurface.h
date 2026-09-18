#pragma once

#include "core/cam/CamDropCutter.h" // CLSurface
#include "core/cam/CamJob.h"        // Loop

#include <glm/glm.hpp>

#include <string>
#include <vector>

// The PATTERN half of a finishing pass (docs/cam/surfacing.md).
//
// A strategy is a pattern swept over a contact surface, and the two are
// independent: the pattern is 2D, and the contact algorithm puts it on the
// part. That separation is why a surfacing op is cheap once the engine exists
// -- pycam's whole surfacing generator is 91 lines because the pattern is a
// motion grid it is handed, and blendercam's parallel strategy is 36.
//
// Everything here takes plain values and returns plain polylines: no Setup, no
// Operation, no Document, no OpenCASCADE. That is deliberate and it is the
// rotary seam. FreeCAD's continuous-rotary layer cost 28% of its 3-axis
// surfacing because its generators were a dependency-free numeric core the
// rotary op could call directly; a generator that reaches for a Setup is one
// that has to be rewritten rather than reused.
namespace cam {

// One pass, already dropped onto the cutter-location surface. Broken where
// the tool must lift: a pass that ran through a gap in the region would cut
// across whatever lies between, so a gap ENDS a path and starts another.
using SurfacePath = std::vector<glm::dvec3>;

struct ParallelRequest {
  // Spacing ACROSS the passes, in mm. Absolute here on purpose: `stepover` on
  // an Operation is a fraction of tool diameter, and the conversion belongs at
  // the boundary where the tool is known, not inside a numeric core. One
  // quantity, one unit, one place it is converted -- which is the thing every
  // surveyed CAM got wrong by spelling three different meanings "stepover".
  double stepover_mm = 1.0;

  // Spacing ALONG a pass, in mm: where the CL surface is read. A third
  // quantity, separate from both of the above, as it is in every tool
  // surveyed. Zero takes the CL grid's own resolution, which is the finest
  // answer the surface can give.
  double sampleStep_mm = 0.0;

  // Direction of the raster, degrees anticlockwise from +X.
  double cutAngleDeg = 0.0;

  // Alternate direction pass to pass. A finishing raster is conventionally a
  // zigzag: it is half climb by definition, which is why `climb` means
  // nothing on one and is refused rather than offered.
  bool zigzag = true;

  // Do not cut below this. The CL surface reports `floorZ` where the tool
  // reaches nothing, and a pass must lift there rather than plunge.
  double floorZ = 0.0;
};

struct SurfacePaths {
  std::vector<SurfacePath> passes;
  bool ok = false;
  std::string why;
};

// Raster `region` at `cutAngleDeg`, reading heights from `cl`.
//
// `region` is the silhouette to stay inside (PickedSurfaceRegion), outer loops
// first and holes after. Empty means the whole of `cl`.
[[nodiscard]] SurfacePaths ParallelPasses(const CLSurface& cl,
                                          const std::vector<Loop>& region,
                                          const ParallelRequest& req);

// Read the CL surface at a point, bilinearly. Exposed because it is what a
// caller needs to check one pass against the surface it was planned on, and
// what a test asserts with.
[[nodiscard]] double SampleCL(const CLSurface& cl, double x, double y);

} // namespace cam
