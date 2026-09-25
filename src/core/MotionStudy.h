#pragma once

#include "core/AssemblySolve.h"
#include "core/Occurrence.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <memory>
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
  std::vector<std::pair<uint64_t, glm::mat4>> worlds;
  // v129: every MOVING occurrence inside a flexible placement, in world
  // space, keyed by its path {root, occurrence}. What folds into the root
  // is where the root is.
  struct LeafWorld {
    uint64_t root = 0, occurrence = 0;
    glm::mat4 world{1.0f};
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
  size_t pairsUnknown = 0;
  bool checkedClashes = false;
  size_t pairsChecked = 0; // over all samples
  double seconds = 0.0;
  bool cancelled = false;
  std::string error; // a refusal (driver gone, DOF missing); empty = ran
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

// The driver value at parameter t (0..1): `from + t * (to - from)`.
double ValueAt(const MotionStudy& study, double t);

// Why this study cannot run — a driver mate that is gone or has no such
// DOF — or empty when it can. The same message Run() puts in `error`.
// `assemblies` is needed only for a driver inside a flexible placement.
std::string WhyNot(const AssemblyDocument& asmDoc, const MotionStudy& study,
                   const assembly::AssemblyResolver& assemblies = nullptr);

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
