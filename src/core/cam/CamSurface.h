#pragma once

#include "core/Feature.h"           // Body
#include "core/cam/CamDropCutter.h" // CLSurface
#include "core/cam/CamJob.h"        // Loop
#include "core/cam/CamRotary.h"     // RadiusMap

#include <glm/glm.hpp>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The PATTERN half of a finishing pass
// (docs/cam/operations.md#3-axis-surfacing).
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

  // The widest a band between two neighbouring laps may be, measured across
  // the surface, in mm. 0 is no limit. Evenly spaced levels spread apart
  // where the surface flattens, and the cap above the highest lap is not cut
  // at all: 0.6 mm left on a dome's top at the derived step. Levels are
  // added in Z wherever the band between two is wider than this, and over
  // the cap, until none is.
  double maxGap_mm = 0.0;

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
// together, and on shallow ones its laps would spread out -- so levels are
// added there, and over the cap, until no band between laps is wider than
// `maxGap_mm`. A separate verb
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

// One pass of a rotary RASTER: a run ALONG the rotary axis at a constant part
// angle. `pts` are (x mm, radius mm) -- where the tool's TIP goes -- and the
// whole pass is cut with the part turned so that `thetaDeg` faces the
// spindle.
//
// Constant A per pass, changing between them: the part turns only on the
// link. That keeps arcs fittable (the post refuses to fit an arc across a
// CHANGING A, because an arc word carries no A) -- and it is NOT continuous
// 4-axis, which is the spiral below.
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

// The grid a rotary pass is planned on, in mm along the axis (and of arc at
// the part's widest radius round it), and the cusp it was derived from. A
// finish's is `DerivedDropRes` at the cutter's own cusp; a ROUGH's -- layers
// down to a skin -- is derived from the skin it leaves, and is coarser
// (docs/cam/rotary.md#layers-rest-and-the-rough-verb).
struct RotaryGrid {
  double res = 0.0;
  double cusp = 0.0;
};
[[nodiscard]] RotaryGrid RotaryPassGrid(const Tool& tool, const Operation& op);

// Continuous 4-axis (docs/cam/rotary.md#continuous-surface_rotary): ONE helix
// from the low end of the map to the high end, the part turning while the tool
// cuts. FreeCAD's `rotary_spiral` and Kiri:Moto's lathe mode are the two
// surveyed answers, and this takes the first's shape with the second's ends.
struct RotarySpiralRequest {
  // The spacing between neighbouring turns, in mm MEASURED ALONG THE
  // SURFACE: the axial advance per revolution on a flat stretch, less where
  // the surface climbs, so a shoulder gets turns as close as a flat gets
  // them and the scallop stays the one a planar raster leaves at that
  // stepover.
  double pitch_mm = 1.0;
  // Spacing ALONG the helix, in mm of arc at the part's widest point. The
  // angular step is derived from it; zero takes the map's own cell.
  double sampleStep_mm = 0.0;
  // A point is dropped when it lies within this of the line through its
  // neighbours (in x, radius and arc at the widest radius). Zero keeps every
  // sample.
  double tolerance_mm = 0.0;
  // The radius to cut at where the tool reaches nothing: a helix cannot
  // break without a relink, so an empty cell is crossed in the air, outside
  // the blank. Must be > 0.
  double airRadius = 0.0;
  // Radii below this are raised to it: a roughing LAYER's floor. Zero is the
  // finish, which follows the surface.
  double floorRadius = 0.0;
};

// Passes that FOLLOW a twist (docs/cam/rotary.md#patterns-rotary_pattern):
// helices at `lead_mm` per turn, each running the length of the map, stepped
// round one from the next. In the unrolled (x, arc) plane they are parallel
// lines at the twist's angle, and an infinite lead is the parallel pattern.
struct RotaryHelicalRequest {
  // Axial advance per turn of a pass, signed for the hand. Must be non-zero:
  // no twist is the parallel pattern, and the caller says so.
  double lead_mm = 0.0;
  // Spacing between passes in mm, measured PERPENDICULAR to them at the
  // widest radius, and closer where the surface is steep across them.
  double stepover_mm = 1.0;
  // Spacing ALONG a pass, in mm of path at the widest radius; 0 = the cell.
  double sampleStep_mm = 0.0;
  // Decimation tolerance in (x, r, arc); 0 keeps every sample.
  double tolerance_mm = 0.0;
  // A radius floor for a roughing layer; 0 follows the surface.
  double floorRadius = 0.0;
  // Alternate passes run back, so each starts where the last one ended.
  bool zigzag = true;
};

// A point on the helix. `thetaDeg` is UNWOUND: it grows monotonically through
// every revolution, because the axis turns that way and a value wrapped at
// 360 would read as the part spinning back a full turn.
struct RotaryPathPoint {
  double x = 0.0;
  double r = 0.0;
  double thetaDeg = 0.0;
};

// The helix over `cl`, with ONE closed revolution at constant x at each end.
// A helix started exactly at the end of the part covers its first half-turn
// with only the tool's radius -- FreeCAD's does -- and a ring there is what
// Kiri's rings pattern gets right. Empty when the map is.
[[nodiscard]] std::vector<RotaryPathPoint>
RotarySpiralPath(const RadiusMap& cl, const RotarySpiralRequest& req);

// The helical passes over `cl`, in cutting order. A cell the tool reaches
// nothing in ENDS a pass, as it does a parallel one.
[[nodiscard]] std::vector<std::vector<RotaryPathPoint>>
RotaryHelicalPasses(const RadiusMap& cl, const RotaryHelicalRequest& req);

// The part's dominant twist, in degrees of theta per mm of x, signed for the
// hand; 0 when the part has none to find (too few columns that are not
// round). For each pair of neighbouring columns that vary round the part, the
// theta shift that best lines one up with the other, to a sub-cell fit,
// searched +-16 cells; the median of those. A 45-degree twist at radius 25 is
// 360 / (2 pi 25) = 2.29.
[[nodiscard]] double DeriveRotaryTwist(const RadiusMap& cl);

// --- surfacing off the frame (docs/ui/long-operations.md#cam-toolpath-passes)
//
// A finishing pass is minutes of work and it ran INLINE, on the frame: the
// rotary rolling pin takes seventeen of them, and the frame HUD shows single
// `ui` frames of 10 s and 72 s. The app is not slow there, it is stopped.
//
// The unit is the whole OPERATION, where adaptive clearing's is one pass:
// a finishing pass is one computation from the cutter-location surface
// outward and has no natural interior seam. Both are "a keyed computation
// whose result a later regenerate reads out of a cache", which is all the
// queue needs of either.
//
// Safe to move because it was written that way before the move was taken:
// the patterns take plain values, and every model sample goes through
// `GenerateMeshDetached`, which deep-copies before meshing precisely because
// a triangulation rides on the shared `TShape`.
struct SurfaceRequest {
  uint64_t key = 0;
  Setup setup;
  Operation op;
  Tool tool;
  std::vector<Body> bodies; // already framed, as the strategy wants them
};

struct SurfaceResult {
  std::vector<Move> moves;
  std::string error;
  std::string strategy;
};

// The key for one surfacing operation.
//
// `bodiesKey` is the caller's fingerprint of the geometry -- the core cannot
// take one cheaply, and the UI already keeps exactly this in
// `JobPassesKey`; the disk store passes `BodiesContentKey` instead
// (CamSurfaceStore.h). Everything else is the `setup.`/`op.`/`tool.` fields
// the surfacing strategies read, and the same gate the adaptive key has
// applies: a field read but not hashed is a stale result nobody can see.
[[nodiscard]] uint64_t SurfaceKey(const Setup& setup, const Operation& op,
                                  const Tool& tool, uint64_t bodiesKey);

// The finishing op a rest op cleans up after, when it is cut BEFORE the rest
// op in this setup's cutting order: an enabled surface op of the same kind
// using `restFromTool`'s cutter. Null when there is none, or it comes after --
// a rest pass then knows nothing about what is left and enters from the stock
// top (docs/cam/operations.md#surface-entries).
[[nodiscard]] const Operation* FinishedBefore(const Setup& setup,
                                              const Operation& op);

class SurfaceStore; // CamSurfaceStore.h

// The same pass, from a request -- the form a worker thread calls. With a
// store, it is read from there when it was cut before and written there
// when it is cut now (SurfacePassStored).
[[nodiscard]] SurfaceResult SurfacePass(const SurfaceRequest& request,
                                        const SurfaceStore* store = nullptr);

// Results by key. Same shape and same reasoning as `AdaptiveCache`, with a
// smaller entry count because a surfacing result is much larger: the pin's
// finishing pass alone is 531,432 moves.
//
// Memory only, keyed on the session's `bodiesKey`. The disk behind it is the
// optional `store`, keyed on the geometry itself: a miss here is read from
// there by whoever cuts the pass, never on the frame.
class SurfaceCache {
public:
  [[nodiscard]] const SurfaceResult* Find(uint64_t key);
  void Put(uint64_t key, SurfaceResult result);
  [[nodiscard]] bool Contains(uint64_t key) const;
  [[nodiscard]] size_t Size() const { return m_entries.size(); }
  // Empties the memory; the store keeps what it has.
  void Clear();
  size_t hits = 0;
  size_t misses = 0;

  void SetStore(std::shared_ptr<const SurfaceStore> store) {
    m_store = std::move(store);
  }
  [[nodiscard]] const std::shared_ptr<const SurfaceStore>& Store() const {
    return m_store;
  }

private:
  std::shared_ptr<const SurfaceStore> m_store;
  static constexpr size_t kMaxEntries = 16;
  static constexpr size_t kMaxMoves = 4'000'000;
  std::unordered_map<uint64_t, SurfaceResult> m_entries;
  std::deque<uint64_t> m_order;
  size_t m_moves = 0;
};

} // namespace cam
