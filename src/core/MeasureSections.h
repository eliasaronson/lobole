// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A solid cut into bands along an axis (docs/inspect/README.md#insertion):
// spans of the axis over which its cross-section does not change, each with
// that cross-section as a 2D region. An extrusion, a bore, a straight tooth
// band is ONE band whatever its length; where a face is not along the axis
// (a cone, a fillet, a helical flank) the span is sliced `sliceMm` thick on
// a grid anchored at the axis origin, so two solids sliced with one
// thickness have aligned slices where they sit. A prismatic band's section
// is the B-rep's own (an exact planar section, its curves discretized to
// `deflectionMm`); slices are cut from one fine triangulation of the solid
// (`meshDeflectionMm`), as a slice already stands for its thickness.
//
// What two placed solids do as one slides along the axis is then answered
// band pair by band pair: whether their regions come near (a 2D polygon
// intersection, independent of the slide) and over which slide offsets
// their spans overlap (an interval from the band ends) -- no 3D boolean.

#include <clipper2/clipper.h>

#include <TopoDS_Shape.hxx>

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace Measure::sections {

// Clipper's integer grid: one unit is 10 nm.
constexpr double kScale = 1.0e5;

struct Band {
  double z0 = 0.0, z1 = 0.0;     // along the axis, z0 < z1
  Clipper2Lib::Paths64 region;   // the section; holes wound against outers
  Clipper2Lib::Rect64 box{};     // of the region
  double rMin = 0.0, rMax = 0.0; // radial extent about the axis, mm
  double perimeterMm = 0.0;
  bool sliced = false; // a slice of a span whose section varies
  double zSection = 0.0; // where the section was cut
  double noiseMm = 0.0;  // how far its outline may be off: a mesh's deflection
  int solid = 0;       // which solid of its side it came from
};

struct BandOptions {
  // Slice thickness where the section varies along the axis, mm.
  double sliceMm = 0.25;
  // Keep only what lies within this radius of the axis, mm.
  double reachMm = std::numeric_limits<double>::infinity();
  // The chord's furthest from an edge it stands for, mm: an exact
  // section's edges, discretized.
  double deflectionMm = 2.0e-4;
  // The same for a slice, which is cut from a triangulation of the solid
  // (a twisted flank's boolean section is seconds a slice).
  double meshDeflectionMm = 5.0e-4;
  // Where the slice grid is anchored along the axis: grid points at
  // gridOriginMm + k * sliceMm. Two solids sliced on one grid line up.
  double gridOriginMm = 0.0;
};

// One solid, placed so the axis is the Z axis through the origin, with
// each face's box (read once: both the reach and the bands need them).
struct Solid {
  TopoDS_Shape shape;
  struct Face {
    int index = 0; // in the solid's face map
    double z0 = 0.0, z1 = 0.0, rMin = 0.0;
  };
  std::vector<Face> faces;
  double half = 0.0; // the furthest any face reaches from the axis in x or y
};
Solid Prepare(const TopoDS_Shape& solidInAxisFrame);

// The span along the axis of the solid's faces that come within `reachMm`
// of the axis; false when none does.
bool ReachSpan(const Solid& s, double reachMm, double& lo, double& hi);

// The solid cut into bands. Empty regions are dropped. Throws
// std::runtime_error when the kernel fails to section it; `cancel` is
// polled between sections.
std::vector<Band> SolidBands(const Solid& s, const BandOptions& opt,
                             int solidIndex,
                             const std::atomic<bool>* cancel = nullptr);

// The region turned about the axis by `deg`.
Clipper2Lib::Paths64 Rotated(const Clipper2Lib::Paths64& p, double deg);

// Box, radial extent and perimeter, recomputed from `region`.
void Refresh(Band& b);

// Bands carried into another frame of the same axis line: each point of a
// region mapped by `across` (a turn about the axis, and a mirror where the
// axis is reversed -- the regions' winding kept), z by `zSign` (+1 or -1,
// the axis reversed), then shifted by `shift` (x, y across the axis, z
// along it); and kept to `reachMm` of the axis -- clipped where they cross
// it, dropped where they lie beyond, and two neighbouring prismatic bands
// left with one section merged again. Each band's extent is recomputed.
std::vector<Band> Moved(const std::vector<Band>& bands,
                        const glm::dmat2& across, double zSign,
                        const glm::dvec3& shift, double reachMm);

// The least distance between two regions' boundaries, mm, looking only at
// edges with a point within `window` (grown by `reachMm`); +inf if none.
double BoundaryGap(const Clipper2Lib::Paths64& a, const Clipper2Lib::Paths64& b,
                   const Clipper2Lib::Rect64& window, double reachMm);

// Bands kept between insertion checks (docs/inspect/README.md#insertion):
// an assembly sequence slides one part after another along a handful of
// directions past the same placed parts, and cutting those again for every
// step was nearly all of its time. A solid is cut once per direction and
// slice thickness, in that direction's own frame (the axis through the
// origin, whole -- no reach), and a later check moves the bands to its axis
// and its reach instead of cutting them again. A solid that has moved since
// (a pose) is moved with them when it turned only about the axis and, if any
// band is a slice, moved along it a whole number of slices; otherwise it is
// cut again. A direction and its reverse share one set of cuts (the bands
// mirrored), and checks along one direction share one slice thickness
// where their travels allow (SliceFor). Bounded: past `kCachePointCeiling`
// points of section the least recently used are dropped.
inline constexpr size_t kCachePointCeiling = 40000000;

class BandCache {
public:
  struct Entry {
    glm::dmat4 place{1.0};   // the solid's placement when it was cut
    std::vector<Band> bands; // in the direction's frame
    size_t points = 0;
    uint64_t used = 0;
  };
  // What a check found in it: bands reused (moved), and solids cut.
  struct Stats {
    int reused = 0, cut = 0, evicted = 0;
  };
  // The entry for solid `key` cut along `dir` `sliceMm` thick, if any; the
  // pointer lives until the next Put.
  const Entry* Find(uint64_t key, const glm::dvec3& dir, double sliceMm);
  void Put(uint64_t key, const glm::dvec3& dir, double sliceMm, Entry e);
  // The slice a check along `dir` that wants `sliceMm` (or finer) is cut
  // at: the thickest one already cut along `dir` no thicker than it and no
  // finer than a sixteenth of it, else `sliceMm` -- so steps with nearly the
  // same travel share their cuts.
  double SliceFor(const glm::dvec3& dir, double sliceMm);
  Stats stats;
  size_t Points() const { return m_points; }

private:
  using Key = std::tuple<uint64_t, int64_t, int64_t, int64_t, int64_t>;
  static Key KeyOf(uint64_t key, const glm::dvec3& dir, double sliceMm);
  std::map<Key, Entry> m_entries;
  std::map<std::tuple<int64_t, int64_t, int64_t>, std::vector<double>>
      m_slices; // by direction, the thicknesses cut along it
  size_t m_points = 0;
  uint64_t m_clock = 0;
  std::mutex m_mutex;
};

} // namespace Measure::sections
