#pragma once

#include "core/cam/CamDropCutter.h" // CLSurface
#include "core/cam/CamJob.h"        // Loop
#include "core/cam/CamRotary.h"     // RadiusMap

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

struct WaterlineRequest {
  // The Z spacing between levels, in mm. NOT a stepover, and deliberately a
  // separate field: waterline's meaningful control is how far apart the laps
  // are in Z, and an XY fraction of tool diameter cannot express it. FreeCAD
  // put a percentage-of-diameter `StepOver` on its waterline op, then hid it
  // for two of its three algorithms because it means nothing there -- while
  // the Z step it actually uses is a different property entirely.
  double zStep_mm = 1.0;

  // The band to lap, in mm. Levels run from `topZ` downward to `bottomZ`,
  // with the last landing exactly on it -- the same rule DepthPasses follows,
  // for the same reason: a lap a hair below the one asked for is a scrapped
  // part.
  double topZ = 0.0;
  double bottomZ = 0.0;

  // Spacing along a lap, in mm; zero takes the CL grid's own resolution.
  double sampleStep_mm = 0.0;

  double floorZ = 0.0;
};

// Constant-Z laps of `cl` through the band, clipped to `region`.
//
// Waterline is the other half of finishing and answers the opposite question
// to the raster: it is good on steep walls, where a raster's passes crowd
// together, and poor on shallow ones, where they spread out. A separate verb
// rather than a flag on one, because a single op cannot honestly carry both
// an XY fraction and a Z step under one name.
[[nodiscard]] SurfacePaths WaterlinePasses(const CLSurface& cl,
                                           const std::vector<Loop>& region,
                                           const WaterlineRequest& req);

// Read the CL surface at a point, bilinearly. Exposed because it is what a
// caller needs to check one pass against the surface it was planned on, and
// what a test asserts with.
[[nodiscard]] double SampleCL(const CLSurface& cl, double x, double y);

// --- the rotary pattern ---------------------------------------------------

// One pass of a continuous 4-axis finishing raster: a run ALONG the rotary
// axis at a constant part angle. `pts` are (x mm, radius mm) -- where the
// tool's TIP goes -- and the whole pass is cut with the part turned so that
// `thetaDeg` faces the spindle.
//
// Constant A per pass, changing between them, and that is the design rather
// than a simplification. It keeps arcs fittable (the post refuses to fit an
// arc across a CHANGING A, because an arc word carries no A), it keeps the
// feed question honest (a pass cuts at its own radius, so one scaling serves
// it), and it is what makes the rotary strategy a PATTERN over a surface
// like every other one here instead of a second kinematics.
struct RotaryPass {
  double thetaDeg = 0.0;
  std::vector<glm::dvec2> pts; // (x, radius)
};

struct RotaryParallelRequest {
  // Spacing ACROSS the passes, in mm of ARC at the part's widest point. The
  // angular step is derived from it, for the reason the grid's own step is:
  // an angle is not a spacing until you say at what radius.
  double stepover_mm = 1.0;
  // Spacing ALONG a pass, in mm. Zero takes the CL grid's own resolution.
  double sampleStep_mm = 0.0;
  // Alternate direction pass to pass, as a planar raster does.
  bool zigzag = true;
};

// Rasters `cl` into passes. Cells where the tool reaches nothing hold a
// radius of zero and BREAK a pass rather than being cut to: a pass that ran
// through a gap would drag the cutter across whatever lies between, which is
// the same rule the planar raster follows for its floor.
[[nodiscard]] std::vector<RotaryPass>
RotaryParallelPasses(const RadiusMap& cl, const RotaryParallelRequest& req);

} // namespace cam
