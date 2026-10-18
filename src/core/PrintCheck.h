// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

struct Body;

// Will this print? (docs/tolerances/README.md#checking-a-print): the one
// printability check every printed part runs -- check_print, the Print
// process section's Check print, and every print-in-place generator. Nine
// items, each a measured value against a limit:
//
//   bed        the lowest point along the print direction is on the bed
//   shells     the print comes off the bed as one piece per solid
//   overhang   no downward face is steeper than the process allows
//   bridges    no ceiling spans further than the process bridges
//   clearance  no two bodies are closer than the print's gap a side
//   chords     the STL's chords cannot eat the gap
//   stacking   every piece starts on the bed, and no face lies within the
//              minimum air over another piece
//   bed_fit    the plate fits the printer's build volume (v179)
//   slender    no piece stands taller than the limit times the least width
//              it stands on, nor over a narrower section of itself
//
// Measured on the B-Rep wherever the B-Rep can answer: the bed from an
// optimal box without triangulation, every overhang angle from the surface
// normal evaluated on the face (never a facet's), clearances by BRepExtrema.
// What a tessellation still decides -- WHERE on a face it crosses the limit,
// so the area past it, and where a ceiling's span is widest -- is said in the
// item's note. No document and no I/O: the caller derives the limits from
// the part's print process (printfit).
namespace printcheck {

enum class Status { Pass, Fail, Skipped };
// "pass" | "fail" | "skipped"
[[nodiscard]] const char* StatusName(Status s);

// A point is on the bed within this of it, along the print direction.
inline constexpr double kBedTolMm = 1e-3;
// Two bodies this close are one piece: what interference's touch_tol is.
inline constexpr double kTouchMm = 1e-3;
// A downward face within this of horizontal everywhere is a ceiling: a bridge,
// not an overhang.
inline constexpr double kCeilingDeg = 1.0;
// A gap modelled within this share of the one required prints the same: FDM
// holds about ±0.2 mm, and an arc-approximated involute comes out microns
// short of its gap.
inline constexpr double kGapToleranceShare = 0.1;
// The chords may take at most this share of the gap a side.
inline constexpr double kChordShare = 0.2;
// An angle past a limit by less than this is AT the limit: a 45° cone's
// normal evaluates to 45 within rounding.
inline constexpr double kAngleEpsDeg = 1e-6;
// The tallest a piece may stand over the least width it stands on, when the
// print profile states no slender_ratio: NOT MEASURED. FDM guides put a
// free-standing pin's safe height at a few to ten times its width; 8 is
// inside that, and the clock's pillars, 24.5:1, came off the bed.
inline constexpr double kSlenderRatio = 8.0;
// The most sections the slender item reads a piece at, evenly up its height.
inline constexpr int kSlenderSections = 256;

struct Options {
  glm::dvec3 up{0.0, 0.0, 1.0}; // the print direction, unit; the bed is
                                // the plane dot(up, p) = 0 below it
  double maxOverhangDeg = 45.0; // from vertical: 0 a wall, 90 a ceiling
  double overhangAllowanceMm2 = 0.0;
  double maxBridgeMm = 10.0;
  // The gap a side the print needs; nullopt skips clearance and chords.
  std::optional<double> gapSideMm;
  // The chord deviation the part's STL is exported at.
  std::optional<double> stlDeflectionMm;
  // The vertical air a face needs over another piece to come away from it
  // (printfit::MinAirMm); nullopt skips stacking.
  std::optional<double> minAirMm;
  // The printer's build volume, x and y on the bed and z up it (the
  // profile's bed_mm); nullopt skips bed_fit, saying `bedNote`.
  std::optional<glm::dvec3> bedMm;
  std::string bedNote;
  // v181: the bed's keep-out rectangles [x0, y0, x1, y1], mm in the bed's
  // coordinates (the profile's bed_exclude_mm). Stated, they make bed_fit
  // POSITIONAL: the part's frame is the bed's, and every piece must lie on
  // it and clear of each zone.
  std::vector<std::array<double, 4>> bedExcludeMm;
  // The slender item's limit: a piece's height over the least width of what
  // it stands on, and over any narrower section, the height above that.
  double slenderRatio = kSlenderRatio;
  // Per body (an index into the bodies), what a brim adds to its footprint's
  // width, both sides together, mm; empty or 0 = no brim. A piece's brim is
  // the widest of its bodies'.
  std::vector<double> brimMm;
};

// Where an item's worst value is: the body (an index into the bodies the
// check was given), the face (TopExp_Explorer order, the picking order) and
// the point, in the bodies' frame.
struct Where {
  int body = -1;
  int face = -1;
  glm::dvec3 point{0.0};
};

struct BodyInfo {
  int solids = 0;
  bool watertight = false;
  int freeEdges = 0;
  double lowestMm = 0.0; // along up
};

struct Bed {
  Status status = Status::Skipped;
  double lowestMm = 0.0;
  double tolMm = kBedTolMm;
  double contactAreaMm2 = 0.0; // downward planar faces lying on the bed
  std::vector<int> onBed;      // bodies whose lowest point is on it
};

struct Shells {
  Status status = Status::Skipped;
  int pieces = 0;
  int expected = 0; // one per solid: a body may be several (loose discs)
  bool watertight = false;
  // Bodies whose solids come off the bed as one piece (touching or
  // overlapping), each group of solids that joined; and, for the record,
  // the bodies made of more than one solid.
  std::vector<std::vector<int>> joined;
  std::vector<int> split;
};

struct FaceOver {
  Where where; // the face's worst point
  double worstDeg = 0.0;
  double areaMm2 = 0.0; // past the limit
};

struct Overhang {
  Status status = Status::Skipped;
  double limitDeg = 0.0;
  double allowanceMm2 = 0.0;
  double areaMm2 = 0.0;  // past the limit
  double worstDeg = 0.0; // the steepest downward face not on the bed and
                         // not a ceiling; 0 when nothing faces down
  std::optional<Where> worst;
  std::vector<FaceOver> faces; // past the limit, worst first
  std::string note;
};

struct Bridge {
  Where where; // the face, and the centre of its widest span
  double spanMm = 0.0;
  double areaMm2 = 0.0;
  double heightMm = 0.0; // above the bed
};

struct Bridges {
  Status status = Status::Skipped;
  double limitMm = 0.0;
  double worstSpanMm = 0.0;
  double areaMm2 = 0.0;
  std::vector<Bridge> faces; // widest first
  std::string note;
};

struct Pair {
  int a = -1, b = -1;         // bodies
  int solidA = 0, solidB = 0; // which solid of each
  double distanceMm = 0.0;    // 0 when they touch or overlap
  bool overlap = false;
};

struct Clearance {
  Status status = Status::Skipped;
  double requiredMm = 0.0;  // the gap a side
  double toleranceMm = 0.0; // passes down to required - tolerance
  std::optional<double> minMm;
  std::vector<Pair> pairs; // every pair measured, closest first
  int pruned = 0;          // pairs whose oriented boxes are further apart
  std::string note;
};

struct Chords {
  Status status = Status::Skipped;
  double deflectionMm = 0.0;
  double limitMm = 0.0;
  std::string note;
};

// A piece: solids that touch print as one (the shells item's).
struct Piece {
  std::vector<int> bodies; // every body with a solid in it, in order
  double lowestMm = 0.0;   // above the print's lowest point
};

// A piece that does not start on the bed: printed into the air.
struct Lifted {
  int piece = -1;
  double heightMm = 0.0; // its lowest point above the print's
  Where where;           // its lowest face
};

// One piece printed over another within the minimum air.
struct Stacked {
  int above = -1, below = -1; // pieces
  double areaMm2 = 0.0;       // of the upper piece's faces under the limit
  double minAirMm = 0.0;      // the least vertical air between them
  Where where;                // the upper piece's face there, and the point
};

struct Stacking {
  Status status = Status::Skipped;
  double limitMm = 0.0;     // the minimum air
  double toleranceMm = 0.0; // passes down to limit - tolerance
  std::vector<Piece> pieces;
  std::vector<Lifted> lifted; // highest first
  std::vector<Stacked> pairs; // least air first
  int samples = 0;            // points the air was measured from
  std::string note;
};

// A piece that is not where a bed with keep-out zones lets it be: its
// footprint box (in the bed's x and y) off the bed (`zone` -1), or the
// piece itself (not merely its box) into zone `zone`.
struct BedIntrusion {
  int body = -1;
  int zone = -1;
  glm::dvec2 minMm{0.0}, maxMm{0.0}; // the body's footprint
  double overlapMm2 = 0.0;           // with the zone (0 off the bed)
};

// The plate in the printer: its extent on the bed (x along the bed's x,
// the print direction's own x when it is not z) and up it, against the
// build volume, with what does not fit per axis. Without keep-out zones the
// extent, not a position: a slicer places the plate, so a plate fits when
// its box does. With them (v181) the part's frame is the bed's -- printed
// up +Z, x and y from the bed's front-left corner -- and every piece must
// lie on the bed and miss every zone: that is where a print job puts it.
struct BedFit {
  Status status = Status::Skipped;
  glm::dvec3 bedMm{0.0};
  glm::dvec3 extentMm{0.0};
  glm::dvec3 shortfallMm{0.0}; // extent - bed where positive, else 0
  bool positional = false;     // zones stated: positions judged too
  std::vector<std::array<double, 4>> zonesMm;
  std::vector<BedIntrusion> intrusions;
  std::string note;
};

// A section of a piece narrower than the footprint it stands on: what
// carries everything above it, and breaks or sways first.
struct Neck {
  double atMm = 0.0;    // above the piece's lowest point
  double widthMm = 0.0; // the section's least width (its convex hull's)
  double aboveMm = 0.0; // the piece's height over it
  double ratio = 0.0;   // aboveMm / widthMm
};

// One piece (the stacking item's: solids that touch), standing.
struct SlenderPiece {
  int piece = -1;
  std::vector<int> bodies; // every body with a solid in it, in order
  double heightMm = 0.0;   // its top over its lowest point
  // The least width of the convex hull of its first-layer contact: the
  // downward planar faces on its lowest point, as the bed item reads them.
  // 0 when it touches the bed on an edge or a point.
  double footprintMm = 0.0;
  double brimMm = 0.0;      // what its brim adds to that width
  double ratio = 0.0;       // heightMm / (footprintMm + brimMm)
  std::optional<Neck> neck; // its worst section narrower than the footprint
  double worstRatio = 0.0;  // the larger of ratio and the neck's
  // Past the limit without its brim, within it with: the brim is what
  // holds it.
  bool brimHolds = false;
  Where where; // the body, and the footprint's centre or the neck's
};

struct Slender {
  Status status = Status::Skipped;
  double limit = 0.0;
  std::vector<SlenderPiece> pieces; // every piece, the worst first
  int sections = 0;                 // sections read, all pieces together
  std::string note;
};

// bed_fit alone, for `bodies` printed up `up` on a bed of `bedMm` with
// `zonesMm` kept out (export_print_job checks every plate with it). Zones
// with an `up` that is not +Z cannot be judged (the part's frame is not the
// bed's) and fail, saying so.
[[nodiscard]] BedFit
FitOnBed(const std::vector<const Body*>& bodies, const glm::dvec3& up,
         const glm::dvec3& bedMm,
         const std::vector<std::array<double, 4>>& zonesMm);

struct Report {
  std::vector<BodyInfo> bodies;
  Bed bed;
  Shells shells;
  Overhang overhang;
  Bridges bridges;
  Clearance clearance;
  Chords chords;
  Stacking stacking;
  BedFit bedFit;
  Slender slender;
  // Every item that ran passed.
  [[nodiscard]] bool Ok() const;
};

// The check over `bodies`, as one print. Throws std::runtime_error on an
// empty list, a body with no shape, or Options out of range (a zero `up`, a
// limit not in its range, a brim per body that is not one per body): the caller
// refuses those before it gets here.
[[nodiscard]] Report Check(const std::vector<const Body*>& bodies,
                           const Options& opts);

} // namespace printcheck
