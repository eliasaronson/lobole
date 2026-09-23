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
  std::string aName, bName;
  // WHERE the overlap is, for the same reason contact_report carries it: a
  // pair of names is a guessing game on an assembly with eight of each. An
  // exact check already builds the intersection to weigh it, so this is free
  // there; a MESH check only answers yes, and leaves `known` false.
  bool known = false;
  double volume = 0.0;
  glm::dvec3 centroid{0.0};
};

struct UnknownContact {
  uint64_t a = 0, b = 0;
  std::string aName, bName;
  std::string reason = "exact contact calculation failed";
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
  std::vector<Clash> clashes;
  std::vector<UnknownContact> unknownContacts;
  // Every moving mate's DOF values at this sample (radians, mm): what the
  // study actually turned and slid, so a caller need not recover an angle
  // from a quaternion.
  struct MateValue {
    uint64_t mate = 0;
    double rotation = 0.0, offset = 0.0;
  };
  std::vector<MateValue> mates;
};

struct Result {
  std::vector<Sample> samples;
  // Pairs skipped because they are rigid to each other over the whole study
  // — bolted joints, a part and the frame it is fastened to. The motion
  // cannot make them collide; `contact_report` says whether they already do.
  size_t pairsRigid = 0;
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
std::string WhyNot(const AssemblyDocument& asmDoc, const MotionStudy& study);

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
