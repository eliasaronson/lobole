// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/AssemblySolve.h"
#include "core/Occurrence.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace collide {
class Mesh;
class ExactContactCache;
} // namespace collide

// Running a MotionStudy (docs/assemblies/README.md#motion-studies): the
// driver DOF set to each sample value, the assembly solved there, every
// top-level occurrence's placement recorded, and — when the study asks —
// every occurrence pair checked for a clash. The result is the collision
// table that is the kinematic gate: "rotate the crank through 720 degrees
// at 15-degree steps and confirm zero collision at every step. Print the
// table."
//
// A sample whose loops do not close is reported with its residual and
// marked unsolved. It is NEVER drawn in the last good pose's place: an
// animation that quietly holds a frame is exactly the substitution this
// project refuses, and a motion study that hid one would be worthless as a
// gate.
namespace motion {

struct Clash {
  uint64_t a = 0, b = 0; // top-level occurrence ids
  // v129: inside a FLEXIBLE placement a clash is between its LEAVES, so
  // each side also names the occurrence of that placement's sub-assembly it
  // is (the path is {a, aInner}); 0 = the top-level occurrence as a whole.
  uint64_t aInner = 0, bInner = 0;
  std::string aName, bName;
  // The pair never moves relative to each other over the study (fastened,
  // or both carried by the same moving part). Such a pair is checked ONCE,
  // at the first sample, since no pose can change its answer — and a clash
  // it finds is a standing clash, not one the motion made.
  bool rigid = false;
  // WHERE the overlap is, for the same reason contact_report carries it: a
  // pair of names is a guessing game on an assembly with eight of each. An
  // exact check already builds the intersection to weigh it, so this is free
  // there: its volume, centroid and world bounds. A MESH check only answers
  // yes, and leaves `known` false.
  bool known = false;
  double volume = 0.0;
  glm::dvec3 centroid{0.0};
  glm::dvec3 bboxMin{0.0}, bboxMax{0.0};
  // What IS known either way: the region the two bodies' world boxes share.
  // The overlap lies inside it; a mesh check cannot say where.
  glm::dvec3 withinMin{0.0}, withinMax{0.0};
};

// A pair whose overlap is a PRESS FIT the bands allow
// (docs/tolerances/README.md#press-fits): relieved, not a clash, and listed
// once per pair over the whole run — the first sample that found it.
struct PressFit {
  uint64_t a = 0, b = 0;
  uint64_t aInner = 0, bInner = 0; // as Clash
  std::string aName, bName;
  std::string fit;           // "Ø47 K7 / 6005 outside (ISO 492 normal)"
  double interference = 0.0; // as made, on the diameter, mm
  double maxInterference = 0.0;
  double volume = 0.0; // relieved, mm3
};

struct UnknownContact {
  uint64_t a = 0, b = 0;
  std::string aName, bName;
  std::string reason = "exact contact calculation failed";
  uint64_t aInner = 0, bInner = 0; // as Clash
};

struct Sample {
  double t = 0.0;     // 0..1 along the study
  double value = 0.0; // the driver DOF at this sample (radians or mm)
  bool solved = true; // every mate placed, every loop closed
  int erroredMates = 0;
  double residualMm = 0.0;
  // Placement of every top-level occurrence at this sample, by id. The
  // export and the scrub read these; nothing else in the document does.
  std::vector<std::pair<uint64_t, glm::dmat4>> worlds;
  // v129: every MOVING occurrence inside a flexible placement, in world
  // space, keyed by its path {root, occurrence}. What folds into the root
  // is where the root is.
  struct LeafWorld {
    uint64_t root = 0, occurrence = 0;
    glm::dmat4 world{1.0};
  };
  std::vector<LeafWorld> leaves;
  std::vector<Clash> clashes;
  std::vector<UnknownContact> unknownContacts;
  // Every moving mate's DOF values at this sample (radians, mm): what the
  // study actually turned and slid, so a caller need not recover an angle
  // from a quaternion.
  struct MateValue {
    uint64_t mate = 0;
    double rotation = 0.0, offset = 0.0;
    // v129: the flexible placement an inner mate is in (empty = this
    // document's), and what the mate is, since an inner one cannot be
    // looked up in this document.
    std::vector<uint64_t> path;
    std::string name;
    bool turns = false, slides = false;
  };
  std::vector<MateValue> mates;
};

struct Result {
  std::vector<Sample> samples;
  // Pairs rigid to each other over the whole study — bolted joints, a part
  // and the frame it is fastened to. The motion cannot change whether they
  // collide, so they are checked once, at `rigidCheckedAt` (the first sample
  // whose clashes were checked), and skipped at every later sample:
  // `pairsRigidChecked` body pairs checked there, `pairsRigid` skips after.
  // Checked EXACTLY whatever `clashMode` says: touching is what a fastened
  // pair does, and a mesh check calls touching a clash.
  size_t pairsRigid = 0;
  size_t pairsRigidChecked = 0;
  int rigidCheckedAt = -1;   // -1: never (no clash check ran)
  bool clean = true;         // no clash at any sample, and every one solved
  size_t worstSample = 0;    // the sample with the most clashes (or the
                             // first unsolved one)
  double deflectionMm = 0.0; // the mesh check's deflection, 0 for exact
  size_t pairsCached = 0;
  // Pair checks answered by an earlier sample of THIS run whose relative
  // pose was the same (a dwell, a full turn): counted in pairsChecked, not
  // computed again.
  size_t pairsRepeated = 0;
  size_t pairsUnknown = 0;
  // Every press fit an exact check relieved (PressFit, above). A mesh check
  // relieves none, and reports such a pair as the clash its triangles see.
  std::vector<PressFit> pressFits;
  bool checkedClashes = false;
  size_t pairsChecked = 0; // over all samples
  double seconds = 0.0;
  bool cancelled = false;
  std::string error; // a refusal (driver gone, DOF missing); empty = ran
  // The input hash it was run for (InputKey); 0 = unknown.
  uint64_t inputHash = 0;
};

// Runs `study` against a COPY of `asmDoc`: the document handed in is not
// touched, which is what lets a run happen on a worker thread beside a live
// tab. `cancelled` is polled between samples; `progress` gets 0..1.
// `meshes` is a cache of collision meshes by CADShape::GeometryId, filled
// on demand — or PRE-FILLED by the caller, which is how a run on a worker
// thread avoids tessellating (the one step that mutates a shape) off the
// main thread.
using MeshCache = std::map<const void*, std::shared_ptr<const collide::Mesh>>;
Result Run(const AssemblyDocument& asmDoc, const assembly::PartResolver& parts,
           const assembly::AssemblyResolver& assemblies,
           const MotionStudy& study,
           const std::function<bool()>& cancelled = nullptr,
           const std::function<void(double)>& progress = nullptr,
           MeshCache* meshes = nullptr,
           collide::ExactContactCache* contacts = nullptr);

// The input hash of a run of `study` on `asmDoc` (docs/simulation/README.md
// #studies-as-jobs): the study's definition (its name aside), every
// occurrence with its placement, every mate with its current value (the run
// starts from the pose the document is in), relation and configuration and
// the active one, through every sub-assembly, and every (part,
// configuration) placed, by its bodies' exact B-Rep, names and materials.
uint64_t InputKey(const AssemblyDocument& asmDoc,
                  const assembly::PartResolver& parts,
                  const assembly::AssemblyResolver& assemblies,
                  const MotionStudy& study);

// The same inputs without a study: the assembly's own content through every
// sub-assembly, and every placed (part, configuration) by its bodies. What
// a requirement on the assembly (a clearance, check_seat, check_fasteners)
// was measured against (docs/requirements/README.md#evidence).
uint64_t AssemblyInputKey(const AssemblyDocument& asmDoc,
                          const assembly::PartResolver& parts,
                          const assembly::AssemblyResolver& assemblies);

// The driver value at parameter t (0..1): `from + t * (to - from)`.
double ValueAt(const MotionStudy& study, double t);

// Why this study cannot run — a driver mate that is gone or has no such
// DOF — or empty when it can. The same message Run() puts in `error`.
// `assemblies` is needed only for a driver inside a flexible placement.
std::string WhyNot(const AssemblyDocument& asmDoc, const MotionStudy& study,
                   const assembly::AssemblyResolver& assemblies = nullptr);

// A point TRACED through a run (docs/assemblies/README.md#tracing-a-point):
// a point fixed on one occurrence, in that occurrence's own (part)
// coordinates, followed through every sample. What a linkage is FOR is
// usually a path -- a foot, a coupler point, a cam follower's tip -- and
// every generator that wanted one transformed the run's poses by hand.
//
// `occurrence` is a top-level occurrence; with `inner` set, the point is on
// that MOVING occurrence inside the flexible placement `occurrence` (the
// path a Sample::LeafWorld is keyed by).
struct TracePoint {
  std::string name;
  uint64_t occurrence = 0;
  uint64_t inner = 0;
  glm::dvec3 pointMm{0.0};
};

struct TracePath {
  std::string name;
  // One per sample, in world mm; nullopt where the sample did NOT solve. An
  // unsolved sample's pose is the solver's last attempt, not the mechanism,
  // and a path drawn through it would be the substitution this file
  // refuses.
  std::vector<std::optional<glm::dvec3>> points;
  size_t unsolved = 0;
  // The extent over the solved samples; `any` false when there were none.
  bool any = false;
  glm::dvec3 min{0.0}, max{0.0};
};

// Why `points` cannot be traced on `asmDoc` -- an empty or repeated name, an
// occurrence that is not there, a path whose root is not a flexible
// placement -- or empty when they can. Checked BEFORE a run, so a typo does
// not cost one. (Whether an inner occurrence MOVES is known only from the
// run; TracePoints says so.)
std::string WhyNotTrace(const AssemblyDocument& asmDoc,
                        const std::vector<TracePoint>& points);

// Every point through every sample of `result`. Returns false, with `why`,
// when a point's occurrence has no pose in the run (an inner occurrence that
// does not move inside its placement is not recorded: trace the point on
// the placement itself).
bool TracePoints(const Result& result, const std::vector<TracePoint>& points,
                 std::vector<TracePath>* out, std::string* why);

// The collision table as text, one line per sample: what the plan asked to
// be PRINTED, and what an agent pastes into its report.
std::string Table(const Result& result, const MotionStudy& study);

// What a stored result SAYS, as one line and a severity — the panel's
// verdict, kept here so it is tested without a GUI. Asks whether the
// mechanism MOVED through every sample before it asks about collisions: a
// preview that locks at 90 degrees must not read like one that works.
struct Verdict {
  enum class Level { Good, Neutral, Bad };
  Level level = Level::Neutral;
  std::string text;
};
Verdict Summarize(const Result& result, const MotionStudy& study, bool stale);

} // namespace motion
