#pragma once

// Helpers shared between the CamToolpath*.cpp files.
//
// CamToolpath.cpp was 3,306 lines and six subjects
// (docs/architecture.md#source-layout). Splitting it left 21 helpers that more
// than one of the six needs -- the Clipper2 conversions, the move emitters,
// the ramp geometry. They were file-static; they are `cam::detail` now, which
// is the only linkage change the split makes.
//
// NOT a public interface: nothing outside src/core/cam includes this. The
// public surface is still CamToolpath.h, unchanged.

#include "core/cam/CamJob.h"

// Only the polygon helpers need Clipper2, and only a CAM build compiles them.
// CamSetup / CamPicks / CamTravel include this header for the clipper-free
// half and are built either way -- they are the job document's frames, picks
// and bounds, not toolpath generation (see CMakeLists.txt).
#ifdef HAVE_CAM
#include <clipper2/clipper.h>
#endif

#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

namespace cam {
namespace detail {

// Clipper2 works in integers. 1e4 puts the quantum at 0.1 µm — four orders
// below anything a mill can hold, and far enough from int64's range that a
// metre-scale part cannot overflow. ONE constant, because two would disagree.
constexpr double kScale = 1.0e4;

// The chord tolerance curved edges are flattened to. One constant, so a
// helix is no coarser than the walls it sits between (it is also
// SectionLoops' default).
constexpr double kDeflection = 0.02;

#ifdef HAVE_CAM
Clipper2Lib::Path64 ToClipper(const std::vector<glm::dvec2>& pts);

std::vector<glm::dvec2> FromClipper(const Clipper2Lib::Path64& path);

Clipper2Lib::Paths64 ToClipper(const std::vector<Loop>& loops);

// Offsets a set of closed paths. Positive grows, negative shrinks; an empty
// result means the region closed up, which is how concentric clearing knows
// it is done.
Clipper2Lib::Paths64 Offset(const Clipper2Lib::Paths64& paths, double deltaMm);

// Traversal direction. The convention, stated once because every machinist
// checks it on the first cut: with a right-hand cutter and the tool OUTSIDE
// the material, climb milling runs clockwise seen from +Z; inside a pocket it
// runs counter-clockwise. Clipper hands back outers CCW and holes CW.
void Orient(Clipper2Lib::Path64& path, bool climb, bool insideMaterial);
#endif // HAVE_CAM

// Offsetting is signed against the winding: a positive delta grows a
// positively wound path and shrinks a negatively wound one. Callers say what
// they mean with the sign, so every contour handed to `Offset` arrives in one
// convention — Clipper hands the holes of a union back wound the other way,
// and an opening IS a hole.
void MakeCounterClockwise(Loop& l);

double Perimeter(const std::vector<glm::dvec2>& pts);

// Walks one wire into an ordered polyline, honouring edge orientation so the
// points come out in sequence rather than in map order.
std::vector<glm::dvec2> WireToLoop(const TopoDS_Wire& wire, double deflection);

// A millimetre figure for a message. std::to_string on a double gives
// "3.000000", which reads as machine noise in a sentence meant for a person.
std::string Round2(double mm);

#ifdef HAVE_CAM
// Why the cutter could not get into `region`, in the words a person can act
// on: how wide the place actually is, how wide this tool is, and what would
// fit. Empty when the tool DOES fit, so the caller keeps looking for the real
// reason rather than blaming the tool for something else.
//
// This exists because the generic "produced no toolpath — check its depths"
// blamed the depths for a job whose depths were right and whose cutter was
// too fat, which is the one failure a beginner hits first and the one they
// have no way to diagnose.
std::string TooTightForTool(const Clipper2Lib::Paths64& region,
                            const Tool& tool, double stockToLeave,
                            const char* what);
#endif // HAVE_CAM

void Emit(Toolpath& tp, Move::Kind kind, const glm::dvec3& to, double feed);

// Rapid over, plunge, cut the loop, retract. Tabs raise the tool over evenly
// spaced spans so the part stays attached on the final pass.
//
// The subtlety that bit on the first run: a tab span has to SPLIT the
// segments it crosses. A profiled rectangle is four segments of 40 mm, and
// testing "is the end of this segment inside a tab?" steps straight over
// every tab on the part — silently, with the file looking plausible. So the
// loop is walked by arc length and cut at each tab boundary, with an explicit
// vertical move there, which also makes the tab square-walled rather than a
// diagonal ramp.
// Entry ramp angle. Shallow enough for a non-centre-cutting end mill to
// enter metal, steep enough not to spend a lap descending. A knob later if
// anyone needs one; a fixed sane value beats a vertical plunge today.
constexpr double kRampAngleDeg = 3.0;

// Rotate a closed loop so it begins at the point nearest `from`. A ring cut
// straight after its neighbour should start where the tool already is, not at
// whatever point the offsetter happened to emit first — otherwise "link at
// depth" drags the cutter most of the way round the ring to get started.
std::vector<glm::dvec2> StartNearest(const std::vector<glm::dvec2>& pts,
                                     const glm::dvec2& from);

// The same loop, starting at the point on it nearest `at` (inserted as a
// vertex when it falls mid-edge). Not the nearest VERTEX: a lead-out held at
// the middle of a wall has to stay there on every pass, and an offset polygon
// has no vertex there to find.
std::vector<glm::dvec2> StartAtPoint(const std::vector<glm::dvec2>& pts,
                                     const glm::dvec2& at);

// steep, so instead the tool spirals down inside the region and the rings
// are then cut from the middle outwards.
//
// The circle it walks is the TOOL CENTRE's, so `radius` must already have the
// cutter's own radius taken out of it — a helix sized from the wall instead
// of the centre is a cut through the wall.
void EmitHelix(Toolpath& tp, const Tool& tool, const glm::dvec2& centre,
               double radius, double fromZ, double toZ, double safeZ,
               double stockTop, double deflection);

// Cut the loop at depth `z`, entering from `entryZ` by RAMPING along the path
// rather than plunging straight down — a vertical plunge with a
// non-centre-cutting mill rubs rather than cuts. The ramp runs along the
// toolpath itself, so it needs no room the cut does not already have; the lap
// then starts where the ramp finished, which is why the point list is
// rotated rather than restarted.
//
// C6: `entry` says how the tool gets to depth. `Ramp` is the above.
// `AtDepth` means the caller already descended (a helix) and the tool is
// standing in cleared material at `z` — so the loop is joined with a cut
// move instead of a retract-and-ramp, which is the whole point of clearing
// inside-out after a helix.
enum class Entry { Ramp, AtDepth };

// Cuts one closed loop at depth `z`, entering by ramp (or straight in, at
// depth) and returns WHERE IT FINISHED as an arc length along the loop.
//
// `startS` is where to begin, and the return value is what to pass as the
// next depth pass's `startS`. That chaining is the whole point: the lap
// deliberately overshoots the ramp so the ramped-down stretch still gets cut
// at full depth, so every pass ends `rampLen` past where it began — and a
// pass that then restarts at pts[0] has to lift, fly back ACROSS the opening
// at safe height, and drop again, once per pass. Starting where the last one
// stopped puts the lift and the drop at the same point instead, which is no
// travel at all.
// `leadOut` > 0: leave the wall on a tangent arc of that radius (R5) before
// retracting, curving AWAY from the material -- `materialInside` says which
// side of the loop that is. A tool that stops against the wall and lifts
// leaves a mark where it dwelt; one that arcs off does not. A lead-out
// OVERRIDES `startS`: the lap ends at pts[0], so the arcs of successive
// passes stack and each cuts one stepdown of the waste, not the whole depth
// from the stock top (docs/cam/operations.md#profile).
// `tabsOut`: where the tabs of THIS lap ended up, appended. Only the caller
// knows which pass leaves them standing, and only this function knows where
// along the contour they fall, so the answer is handed back rather than
// worked out twice.
double EmitLoop(Toolpath& tp, const Setup& setup, const Tool& tool,
                const std::vector<glm::dvec2>& pts, double z, double safeZ,
                double stockTop, int tabCount, double tabWidth, double tabZ,
                double entryZ, Entry entry = Entry::Ramp, double startS = 0.0,
                bool retract = true, double leadOut = 0.0,
                bool materialInside = true,
                std::vector<Toolpath::Tab>* tabsOut = nullptr);

// Nothing to do: the part is clamped as modelled and the table is square. The
// common case, and worth not building a matrix for — this is on the per-point
// path that draws every backplot.
bool SetupIsSquare(const Setup& setup);

// The shortest distance from `p` to a loop's boundary. Which loop a wall
// belongs to is decided by this rather than by containment: a wall lies ON
// its own contour, and "on" is unreliable to test with point-in-polygon.
double DistanceToLoop(const std::vector<glm::dvec2>& pts, const glm::dvec2& p);

// --- breaking an edge -------------------------------------------------------
//
// A chamfer and a roundover are the same operation with a different tool: run
// the cutter round the wall's TOP EDGE, offset and lowered so its own shape
// leaves the break. The tool decides which — a V-bit cuts a bevel in one
// pass, a ball nose walks an arc in as many as the cusp needs — so this is one
// verb, not two, and a flat mill is refused instead of ploughing a square
// step nobody asked for.
//
// Geometry (wall at the origin, material inward, top at z = 0):
//   V-bit  the cone's flank IS the bevel. Put the axis ON the contour and
//          plunge w·cot(half-angle): the tip lands exactly on the wall at the
//          bottom of the bevel, so nothing below the break is touched.
//   ball   the finished surface is a quarter-round of radius R, its axis R in
//          from the wall and R below the top. The ball rides the air side of
//          it: centre = axis + (R + r)·(cos t, sin t), t from the top (a
//          graze on the flat) down to the wall.
struct EdgeBreak {
  double size = 0.0;  // roundover radius / bevel width, mm
  double depth = 0.0; // how far below the top edge the TOOL reaches
  bool ok = false;
  std::string why;
};

// The largest break this tool can make within `window` mm of wall below the
// top edge, or the size asked for, checked against the tool.
EdgeBreak EdgeBreakFor(const Tool& tool, double asked, double window);

// A surfacing operation (planar or rotary), from the surface cache, from a
// worker later, or generated right here (CamSurfaceStrategies.cpp).
void SurfaceThroughCache(const Setup& setup, const Operation& op,
                         const Tool& tool, const std::vector<Body>& bodies,
                         Toolpath& tp, GenerateContext* ctx);

// An engraving (CamEngrave.cpp, docs/cam/operations.md#engrave): every
// target's sketch curves from `ctx->sketchCurves`, label by label.
void GenerateEngrave(const Setup& setup, const Operation& op, const Tool& tool,
                     const std::vector<Body>& bodies, Toolpath& tp,
                     const GenerateContext* ctx);

// A thread mill (CamThreadMill.cpp, docs/cam/operations.md#thread-mill): one
// target, the thread its face carries, bodies in setup space.
void GenerateThreadMill(const Setup& setup, const Operation& op,
                        const Tool& tool, const std::vector<Body>& bodies,
                        Toolpath& tp);

} // namespace detail

// Defined at `cam` scope in the .cpp files but NOT part of the public
// surface: CamToolpath.h never declared them, because until the split they
// were only ever called from further down the same file.

// Assumes `bodies` are ALREADY in setup space. The public StockBounds frames
// first; internal callers that framed once do not want it done twice.
// How much a fixed workpiece may fall short of the part before it is refused.
// An OCC bounding box carries a gap — it is why the CAM round recorded a mesh
// bbox growing 4 um — so an 18 mm board holding an 18 mm part measures a
// hair short of fitting, every time. A micron of shortfall is that gap; a
// millimetre is a part that does not come out of this material.
constexpr double kStockFit = 1e-3;

bool ModelBoundsFramed(const std::vector<Body>& bodies, glm::dvec3& min,
                       glm::dvec3& max);

bool StockBoundsFramed(const Setup& setup, const std::vector<Body>& bodies,
                       glm::dvec3& min, glm::dvec3& max);

bool FindVerticalWallFace(const std::vector<Body>& bodies,
                          const ElementRef& ref, TopoDS_Face& face,
                          glm::dvec2& onWall, uint64_t jobPartId = 0);

Toolpath GenerateFramed(const Setup& setup, const Operation& op,
                        const std::vector<Body>& bodies, GenerateContext* ctx);

// An operation with `indexA`
// (docs/cam/rotary.md#one-operation-at-several-indices): `one` run once per
// angle, on a copy of the setup turned to it and the MODEL-space `bodies`
// framed for that copy, and each role's pieces
// joined -- a rapid at safe Z carrying the next angle between them, so the
// post retracts and turns there, and every move stamped with its A. A piece
// that fails goes in `skipped`, named by its angle; the operation fails only
// when every angle did. A pocket's, face's or adaptive's targets are FLOORS,
// each cut only at the index that turns it to the spindle. Refused whole on a
// machine with no rotary axis. Called only when `CutAtIndices(op)`.
using GenerateAtOneIndex = std::function<std::vector<Toolpath>(
    const Setup&, const Operation&, const std::vector<Body>&)>;
// An engraving's targets are presented like floors, by their sketch planes
// in `curves`.
std::vector<Toolpath>
GenerateAtIndices(const Setup& setup, const Operation& op,
                  const std::vector<Body>& bodies,
                  const GenerateAtOneIndex& one,
                  const std::vector<SketchCurves>* curves = nullptr);

} // namespace cam
