#include "core/MotionStudy.h"

#include "core/CollisionCache.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/MeshCollide.h"
#include "core/Parallel.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

namespace motion {

double ValueAt(const MotionStudy& study, double t) {
  return study.from + t * (study.to - study.from);
}

std::string WhyNot(const AssemblyDocument& asmDoc, const MotionStudy& study) {
  const Mate* m = asmDoc.FindMateById(study.driverMate);
  if (m == nullptr) {
    std::string mates;
    for (const Mate& cand : asmDoc.GetMates())
      if (assembly::MateHasDof(cand.type, MateDof::Rotation) ||
          assembly::MateHasDof(cand.type, MateDof::Offset)) {
        if (!mates.empty())
          mates += ", ";
        mates += std::to_string(cand.id) + " (" + cand.name + ")";
      }
    return "driver mate " + std::to_string(study.driverMate) +
           " no longer exists; mates with a degree of freedom: " +
           (mates.empty() ? std::string("none") : mates);
  }
  if (!assembly::MateHasDof(m->type, study.driverDof))
    return "driver mate " + std::to_string(m->id) + " (" + m->name +
           ") has no " +
           (study.driverDof == MateDof::Rotation ? "rotation" : "offset") +
           " to drive — a revolute or cylindrical mate turns, a slider or "
           "cylindrical mate slides";
  if (study.samples < 2)
    return "a motion study needs at least 2 samples";
  return {};
}

namespace {

// One placed body at one sample, for the pair sweep.
struct Placed {
  uint64_t root = 0; // top-level occurrence
  std::string rootName;
  const Body* body = nullptr;
  glm::mat4 world{1.0f};
};

// A mesh per LOCAL shape, built once for the whole run and placed by matrix
// at every sample — the only reason 48 samples of a 250-occurrence sweep is
// tractable at all.
std::shared_ptr<const collide::Mesh> MeshFor(MeshCache& cache,
                                             const CADShape& shape) {
  const void* key = shape.GeometryId();
  const auto it = cache.find(key);
  if (it != cache.end())
    return it->second;
  auto mesh = collide::Mesh::Build(shape);
  cache[key] = mesh;
  return mesh;
}

bool WorldBox(const Body& b, const glm::mat4& world, glm::vec3& lo,
              glm::vec3& hi) {
  std::vector<Body> one(1);
  one[0].shape = b.shape;
  return Measure::WorldBounds(one, world, lo, hi);
}

} // namespace

Result Run(const AssemblyDocument& asmDocIn,
           const assembly::PartResolver& parts,
           const assembly::AssemblyResolver& assemblies,
           const MotionStudy& study, const std::function<bool()>& cancelled,
           const std::function<void(double)>& progress, MeshCache* meshesIn,
           collide::ExactContactCache* contacts) {
  const auto t0 = std::chrono::steady_clock::now();
  Result out;
  out.checkedClashes = study.checkClash;
  out.error = WhyNot(asmDocIn, study);
  if (!out.error.empty())
    return out;

  // A COPY: the run drives a mate through its whole range and the document
  // on the tab must not be left at the last sample.
  AssemblyDocument asmDoc = asmDocIn;
  Mate* driver = asmDoc.FindMateById(study.driverMate);

  MeshCache own;
  MeshCache& meshes = meshesIn ? *meshesIn : own;
  size_t worstCount = 0;
  bool worstSet = false;

  // Pass A: solve every sample and record the placements, so the clash pass
  // can skip the pairs that DO NOT MOVE relative to each other.
  //
  // Two parts bolted together touch — that is what a bolted joint is — and a
  // mesh check reports touching as a clash, so a real engine's every fixed
  // joint made `clean` impossible. It is also the wrong question: a pair
  // whose relative pose never changes cannot be made to collide BY THE
  // MOTION. Whether it interferes at rest is `contact_report`'s job, and it
  // answers exactly, which this does not.
  std::map<uint64_t, std::vector<glm::mat4>> track;
  {
    AssemblyDocument probe = asmDoc;
    Mate* probeDriver = probe.FindMateById(study.driverMate);
    for (int i = 0; i < study.samples; i++) {
      const double t =
          static_cast<double>(i) / static_cast<double>(study.samples - 1);
      assembly::SetMateDof(*probeDriver, study.driverDof, ValueAt(study, t));
      assembly::PropagateRelations(probe, probeDriver->id, parts);
      assembly::Solve(probe, parts, assemblies,
                      {probeDriver->id, study.driverDof});
      for (const Occurrence& o : probe.GetOccurrences())
        track[o.id].push_back(glm::translate(glm::mat4(1.0f), o.position) *
                              glm::mat4_cast(o.orientation));
    }
  }
  // A pair moves when the transform taking one into the other is not the
  // same at every sample.
  const auto movesRelative = [&](uint64_t a, uint64_t b) {
    const auto ia = track.find(a), ib = track.find(b);
    if (ia == track.end() || ib == track.end())
      return true; // unknown: check it
    const size_t n = std::min(ia->second.size(), ib->second.size());
    if (n < 2)
      return true;
    const glm::mat4 first = glm::inverse(ia->second[0]) * ib->second[0];
    for (size_t k = 1; k < n; k++) {
      const glm::mat4 rel = glm::inverse(ia->second[k]) * ib->second[k];
      for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
          if (std::abs(rel[c][r] - first[c][r]) > 1e-6)
            return true;
    }
    return false;
  };

  for (int i = 0; i < study.samples; i++) {
    if (cancelled && cancelled()) {
      out.cancelled = true;
      out.clean = false;
      break;
    }
    Sample s;
    s.t = static_cast<double>(i) / static_cast<double>(study.samples - 1);
    s.value = ValueAt(study, s.t);

    assembly::SetMateDof(*driver, study.driverDof, s.value);
    assembly::PropagateRelations(asmDoc, driver->id, parts);
    const assembly::SolveReport rep = assembly::Solve(
        asmDoc, parts, assemblies, {driver->id, study.driverDof});
    s.erroredMates = rep.erroredMates;
    s.residualMm = rep.residualMm;
    s.solved = rep.erroredMates == 0 && rep.loopsClosed;

    auto flat = assembly::Flatten(asmDoc, parts, assemblies);
    if (!flat) {
      s.solved = false;
      out.samples.push_back(std::move(s));
      out.clean = false;
      continue;
    }
    // Placements: one per top-level occurrence, from the solved document.
    for (const Occurrence& o : asmDoc.GetOccurrences()) {
      const glm::mat4 world = glm::translate(glm::mat4(1.0f), o.position) *
                              glm::mat4_cast(o.orientation);
      s.worlds.emplace_back(o.id, world);
    }

    if (study.checkClash) {
      std::vector<Placed> placed;
      for (const assembly::FlatInstance& inst : *flat) {
        if (!inst.visible || inst.part == nullptr)
          continue;
        const Occurrence* root = asmDoc.FindById(inst.rootOccurrenceId);
        for (const Body& b : inst.part->GetBodies()) {
          if (b.reference || !b.shape.IsValid())
            continue;
          placed.push_back(
              {inst.rootOccurrenceId, root ? root->name : "", &b, inst.world});
        }
      }
      // Boxes first: only pairs whose boxes overlap can clash, and that
      // clears most of any assembly at every sample for nothing.
      std::vector<glm::vec3> lo(placed.size()), hi(placed.size());
      std::vector<bool> ok(placed.size());
      for (size_t k = 0; k < placed.size(); k++)
        ok[k] = WorldBox(*placed[k].body, placed[k].world, lo[k], hi[k]);
      // Candidates first, then the booleans on every core. A contact
      // check is ~161 ms and this study is 20,403 of them: run serially it
      // was 3292 s, which was 85% of a whole generator run.
      struct Cand {
        size_t a, b;
        std::pair<uint64_t, uint64_t> key;
      };
      std::vector<Cand> cands;
      for (size_t a = 0; a < placed.size(); a++) {
        if (!ok[a])
          continue;
        for (size_t b = a + 1; b < placed.size(); b++) {
          if (!ok[b] || placed[a].root == placed[b].root)
            continue; // a part does not clash with itself
          if (!movesRelative(placed[a].root, placed[b].root)) {
            out.pairsRigid++;
            continue; // rigid to each other: the motion cannot make them
          }
          if (lo[a].x > hi[b].x || lo[b].x > hi[a].x || lo[a].y > hi[b].y ||
              lo[b].y > hi[a].y || lo[a].z > hi[b].z || lo[b].z > hi[a].z)
            continue;
          cands.push_back({a,
                           b,
                           {std::min(placed[a].root, placed[b].root),
                            std::max(placed[a].root, placed[b].root)}});
        }
      }
      // The serial loop used to skip a candidate whose ROOT pair had already
      // clashed at this sample. That rule is inherently sequential, so it
      // moves to the merge below, which keeps the FIRST in candidate order —
      // the same one it kept before. `pairsChecked` therefore counts every
      // boolean actually run, which is what it always claimed to be.
      struct Hit {
        bool cached = false;
        bool hit = false, known = false;
        double volume = 0.0, deflection = 0.0;
        glm::dvec3 centroid{0.0};
      };
      std::vector<Hit> hits(cands.size());
      // Mesh mode caches per shape in a shared map, so every mesh it will
      // want is built HERE, single-threaded, before anything reads it.
      if (study.clashMode != "exact")
        for (const Cand& c : cands) {
          MeshFor(meshes, placed[c.a].body->shape);
          MeshFor(meshes, placed[c.b].body->shape);
        }
      parallel::For(cands.size(), [&](size_t k) {
        const Cand& cd = cands[k];
        Hit& h = hits[k];
        if (study.clashMode == "exact") {
          std::vector<Body> ba(1), bb(1);
          ba[0] = *placed[cd.a].body;
          bb[0] = *placed[cd.b].body;
          // ContactBetween, not InterferenceVolume: it is the same boolean
          // and it hands back WHERE the overlap is as well as how much,
          // which is what a caller needs to act on the answer. It is also
          // the one the named-pair `interference` RPC and contact_report
          // take, so the two checks now agree rather than differing by a
          // fuzz nobody could see.
          // Volume only: a study asks whether the motion makes two parts
          // collide, never how far apart they are when it does not. The
          // separation pass is two thirds of a contact check, and this one
          // ran 20,403 of them.
          const auto answer =
              contacts && ba[0].threadRegions.empty() &&
                      bb[0].threadRegions.empty()
                  ? contacts->Check(ba[0].shape, placed[cd.a].world,
                                    bb[0].shape, placed[cd.b].world)
                  : collide::ExactContactCache::Answer{
                        Measure::ContactBetween(ba, placed[cd.a].world, bb,
                                                placed[cd.b].world, false),
                        false};
          h.cached = answer.cached;
          const auto& c = answer.contact;
          if (c) {
            h.known = true;
            h.volume = c->volume;
            h.centroid = c->centroid;
            h.hit = c->volume > 1e-9;
          }
        } else {
          const auto ma = MeshFor(meshes, placed[cd.a].body->shape);
          const auto mb = MeshFor(meshes, placed[cd.b].body->shape);
          h.deflection = std::max(ma->Deflection(), mb->Deflection());
          h.hit = ma->Overlaps(glm::dmat4(placed[cd.a].world), *mb,
                               glm::dmat4(placed[cd.b].world));
        }
      });
      std::map<std::pair<uint64_t, uint64_t>, Clash> found;
      for (size_t k = 0; k < cands.size(); k++) {
        const Cand& cd = cands[k];
        const Hit& h = hits[k];
        out.pairsChecked++;
        out.pairsCached += h.cached ? 1 : 0;
        if (study.clashMode == "exact" && !h.known) {
          out.pairsUnknown++;
          s.unknownContacts.push_back({placed[cd.a].root, placed[cd.b].root,
                                       placed[cd.a].rootName,
                                       placed[cd.b].rootName});
          out.clean = false;
        }
        out.deflectionMm = std::max(out.deflectionMm, h.deflection);
        if (!h.hit || found.count(cd.key))
          continue;
        Clash c;
        c.known = h.known;
        c.volume = h.volume;
        c.centroid = h.centroid;
        c.a = cd.key.first;
        c.b = cd.key.second;
        c.aName = placed[cd.a].root == cd.key.first ? placed[cd.a].rootName
                                                    : placed[cd.b].rootName;
        c.bName = placed[cd.a].root == cd.key.first ? placed[cd.b].rootName
                                                    : placed[cd.a].rootName;
        found[cd.key] = c;
      }
      for (auto& [key, c] : found)
        s.clashes.push_back(c);
    }

    const bool bad =
        !s.solved || !s.clashes.empty() || !s.unknownContacts.empty();
    if (bad)
      out.clean = false;
    // The worst sample: the first unsolved one wins outright, else the one
    // with the most clashes.
    if (!s.solved && (!worstSet || out.samples[out.worstSample].solved)) {
      out.worstSample = out.samples.size();
      worstSet = true;
      worstCount = SIZE_MAX;
    } else if (s.solved &&
               s.clashes.size() + s.unknownContacts.size() > worstCount &&
               (!worstSet || out.samples[out.worstSample].solved)) {
      out.worstSample = out.samples.size();
      worstCount = s.clashes.size() + s.unknownContacts.size();
      worstSet = true;
    }
    out.samples.push_back(std::move(s));
    if (progress)
      progress(static_cast<double>(i + 1) / static_cast<double>(study.samples));
  }
  out.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  return out;
}

std::string Table(const Result& result, const MotionStudy& study) {
  std::string out;
  const bool deg = study.driverDof == MateDof::Rotation;
  char line[256];
  std::snprintf(line, sizeof line, "%-6s %-12s %-8s %-10s %s\n", "#",
                deg ? "angle (deg)" : "offset (mm)", "solved", "residual",
                "clashes");
  out += line;
  for (size_t i = 0; i < result.samples.size(); i++) {
    const Sample& s = result.samples[i];
    const double shown = deg ? s.value * 180.0 / 3.14159265358979 : s.value;
    std::string clashes;
    for (const Clash& c : s.clashes) {
      if (!clashes.empty())
        clashes += ", ";
      clashes += c.aName + " x " + c.bName;
    }
    for (const auto& c : s.unknownContacts) {
      if (!clashes.empty())
        clashes += ", ";
      clashes += "UNKNOWN " + c.aName + " x " + c.bName + ": " + c.reason;
    }
    std::snprintf(line, sizeof line, "%-6zu %-12.2f %-8s %-10.4f %s\n", i,
                  shown, s.solved ? "yes" : "NO", s.residualMm,
                  clashes.empty() ? "-" : clashes.c_str());
    out += line;
  }
  std::snprintf(line, sizeof line, "%s: %zu samples, %zu pair checks, %.2f s\n",
                !result.error.empty()    ? "ERROR"
                : result.cancelled       ? "INCOMPLETE"
                : !result.checkedClashes ? "KINEMATICS ONLY"
                : result.pairsUnknown    ? "UNVERIFIED"
                : result.clean           ? "CLEAN"
                                         : "NOT CLEAN",
                result.samples.size(), result.pairsChecked, result.seconds);
  out += line;
  if (!result.error.empty())
    out += "Error: " + result.error + "\n";
  if (result.cancelled)
    out += "Cancelled before all samples completed.\n";
  if (result.pairsUnknown)
    out += std::to_string(result.pairsUnknown) +
           " exact pair checks could not be verified.\n";
  return out;
}

} // namespace motion
