#include "core/MotionStudy.h"
#include "core/Angles.h"

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
  // A study that names its configuration is judged UNDER it: its holds and
  // suppressions are the ones the run will see, whatever the tab has active.
  if (study.configuration) {
    const uint64_t want = *study.configuration;
    if (want != 0 && asmDoc.FindConfiguration(want) == nullptr) {
      std::string names;
      for (const Configuration& c : asmDoc.GetConfigurations())
        names += (names.empty() ? "" : ", ") + std::to_string(c.id) + " (" +
                 c.name + ")";
      return "the study runs under configuration " + std::to_string(want) +
             ", which no longer exists; configurations: " +
             (names.empty() ? std::string("none") : names) +
             " (0 = the base, unset = whichever is active)";
    }
    if (asmDoc.ActiveConfiguration() != want) {
      AssemblyDocument under = asmDoc;
      under.SetActiveConfigurationRaw(want);
      return WhyNot(under, study);
    }
  }
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
  if (asmDoc.MateSuppressed(m->id))
    return "driver mate " + std::to_string(m->id) + " (" + m->name +
           ") is out of the active configuration — activate one that keeps "
           "it, or drive another mate";
  if (const MateOverride* o = asmDoc.MateOverrideFor(m->id))
    if ((study.driverDof == MateDof::Rotation && o->rotation) ||
        (study.driverDof == MateDof::Offset && o->offset))
      return "driver mate " + std::to_string(m->id) + " (" + m->name + ")'s " +
             (study.driverDof == MateDof::Rotation ? "rotation" : "offset") +
             " is held by the active configuration, so driving it would move "
             "nothing";
  if (study.samples < 2)
    return "a motion study needs at least 2 samples";
  return {};
}

namespace {

glm::mat4 OccurrenceWorld(const Occurrence& o) {
  return glm::translate(glm::mat4(1.0f), o.position) *
         glm::mat4_cast(o.orientation);
}

// Which top-level occurrences move relative to each other over the study,
// read off a kinematics-only pass through every sample.
//
// Two parts bolted together touch — that is what a bolted joint is — and a
// mesh check reports touching as a clash, so checking every fixed joint at
// every sample made `clean` impossible on a real engine. It is also the
// wrong question to ask 48 times: a pair whose relative pose never changes
// has the same answer at every pose. So a rigid pair is checked ONCE, at the
// first sample, exactly, and skipped after. Skipping it outright hid three
// standing clashes in a pump that passed four exact studies.
class RelativeMotion {
public:
  void Record(const AssemblyDocument& doc) {
    for (const Occurrence& o : doc.GetOccurrences())
      track_[o.id].push_back(OccurrenceWorld(o));
  }
  // True when the transform taking one into the other is not the same at
  // every sample — or when either was not tracked, which is checked rather
  // than assumed still.
  bool Moves(uint64_t a, uint64_t b) const {
    const auto ia = track_.find(a), ib = track_.find(b);
    if (ia == track_.end() || ib == track_.end())
      return true;
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
  }

private:
  std::map<uint64_t, std::vector<glm::mat4>> track_;
};

// The kinematics-only pass: every sample solved on its own copy, in order,
// placements recorded.
RelativeMotion TrackMotion(const AssemblyDocument& asmDoc,
                           const assembly::PartResolver& parts,
                           const assembly::AssemblyResolver& assemblies,
                           const MotionStudy& study) {
  RelativeMotion motion;
  AssemblyDocument probe = asmDoc;
  Mate* driver = probe.FindMateById(study.driverMate);
  for (int i = 0; i < study.samples; i++) {
    const double t =
        static_cast<double>(i) / static_cast<double>(study.samples - 1);
    assembly::SetMateDof(*driver, study.driverDof, ValueAt(study, t));
    assembly::PropagateRelations(probe, driver->id, parts);
    assembly::Solve(probe, parts, assemblies, {driver->id, study.driverDof},
                    /*diagnose=*/false);
    motion.Record(probe);
  }
  return motion;
}

// One placed body at one sample, for the pair sweep.
struct Placed {
  uint64_t root = 0; // top-level occurrence
  std::string rootName;
  const Body* body = nullptr;
  glm::mat4 world{1.0f};
  glm::vec3 lo{0.0f}, hi{0.0f}; // world box
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

// One pair's answer.
struct Hit {
  bool cached = false;
  bool hit = false, known = false;
  double volume = 0.0, deflection = 0.0;
  glm::dvec3 centroid{0.0}, bboxMin{0.0}, bboxMax{0.0};
};

// The boolean. ContactBetween, not InterferenceVolume: it is the same boolean
// and it hands back WHERE the overlap is as well as how much, which is what
// a caller needs to act on the answer. It is also the one the named-pair
// `interference` RPC and contact_report take, so the checks agree rather
// than differing by a fuzz nobody could see. Volume only: a study asks
// whether two parts collide, never how far apart they are when they do not,
// and the separation pass is two thirds of a contact check.
Hit ExactHit(const Placed& a, const Placed& b,
             collide::ExactContactCache* contacts) {
  Hit h;
  std::vector<Body> ba(1), bb(1);
  ba[0] = *a.body;
  bb[0] = *b.body;
  const auto answer =
      contacts && ba[0].threadRegions.empty() && bb[0].threadRegions.empty()
          ? contacts->Check(ba[0].shape, a.world, bb[0].shape, b.world)
          : collide::ExactContactCache::Answer{
                Measure::ContactBetween(ba, a.world, bb, b.world, false),
                false};
  h.cached = answer.cached;
  if (const auto& c = answer.contact) {
    h.known = true;
    h.volume = c->volume;
    h.centroid = c->centroid;
    h.bboxMin = c->overlapMin;
    h.bboxMax = c->overlapMax;
    h.hit = c->volume > 1e-9;
  }
  return h;
}

Hit MeshHit(const Placed& a, const Placed& b, MeshCache& meshes) {
  Hit h;
  const auto ma = MeshFor(meshes, a.body->shape);
  const auto mb = MeshFor(meshes, b.body->shape);
  h.deflection = std::max(ma->Deflection(), mb->Deflection());
  h.hit = ma->Overlaps(glm::dmat4(a.world), *mb, glm::dmat4(b.world));
  return h;
}

bool BoxesOverlap(const Placed& a, const Placed& b) {
  return !(a.lo.x > b.hi.x || b.lo.x > a.hi.x || a.lo.y > b.hi.y ||
           b.lo.y > a.hi.y || a.lo.z > b.hi.z || b.lo.z > a.hi.z);
}

// Every visible, non-reference body of the flattened assembly, placed.
std::vector<Placed>
PlaceBodies(const AssemblyDocument& asmDoc,
            const std::vector<assembly::FlatInstance>& flat) {
  std::vector<Placed> placed;
  for (const assembly::FlatInstance& inst : flat) {
    if (!inst.visible || inst.part == nullptr)
      continue;
    const Occurrence* root = asmDoc.FindById(inst.rootOccurrenceId);
    for (const Body& b : inst.part->GetBodies()) {
      if (b.reference || !b.shape.IsValid())
        continue;
      Placed p{inst.rootOccurrenceId, root ? root->name : "", &b, inst.world};
      if (WorldBox(b, inst.world, p.lo, p.hi))
        placed.push_back(std::move(p));
    }
  }
  return placed;
}

// One sample's clash check, into `s` and the run's tallies in `out`.
// `checkRigid` is true at the one sample the rigid pairs are checked at.
void CheckClashes(const AssemblyDocument& asmDoc,
                  const std::vector<assembly::FlatInstance>& flat,
                  const MotionStudy& study, const RelativeMotion& motion,
                  bool checkRigid, MeshCache& meshes,
                  collide::ExactContactCache* contacts, Sample& s,
                  Result& out) {
  const std::vector<Placed> placed = PlaceBodies(asmDoc, flat);
  const bool exact = study.clashMode == "exact";
  // Candidates first, then the booleans on every core. Only pairs whose
  // boxes overlap can clash, and that clears most of any assembly for
  // nothing. A contact check is ~161 ms and one engine study is 20,403 of
  // them: run serially it was 3292 s, 85% of a whole generator run.
  struct Cand {
    size_t a, b;
    bool rigid;
    std::pair<uint64_t, uint64_t> key;
  };
  std::vector<Cand> cands;
  for (size_t a = 0; a < placed.size(); a++)
    for (size_t b = a + 1; b < placed.size(); b++) {
      if (placed[a].root == placed[b].root)
        continue; // a part does not clash with itself
      const bool rigid = !motion.Moves(placed[a].root, placed[b].root);
      if (rigid && !checkRigid) {
        out.pairsRigid++;
        continue; // checked at the first sample; nothing since has changed
      }
      if (!BoxesOverlap(placed[a], placed[b]))
        continue;
      if (rigid)
        out.pairsRigidChecked++;
      cands.push_back({a,
                       b,
                       rigid,
                       {std::min(placed[a].root, placed[b].root),
                        std::max(placed[a].root, placed[b].root)}});
    }

  // Mesh mode caches per shape in a shared map, so every mesh it will want
  // is built HERE, single-threaded, before anything reads it.
  if (!exact)
    for (const Cand& c : cands)
      if (!c.rigid) {
        MeshFor(meshes, placed[c.a].body->shape);
        MeshFor(meshes, placed[c.b].body->shape);
      }
  std::vector<Hit> hits(cands.size());
  parallel::For(cands.size(), [&](size_t k) {
    const Cand& cd = cands[k];
    // A rigid pair is always asked exactly: touching is what fastened parts
    // do, and a mesh check would call every bolted joint a clash.
    hits[k] = exact || cd.rigid ? ExactHit(placed[cd.a], placed[cd.b], contacts)
                                : MeshHit(placed[cd.a], placed[cd.b], meshes);
  });

  // The merge keeps the FIRST hit per root pair, in candidate order — a rule
  // that is inherently sequential, so it lives here and not in the workers.
  // `pairsChecked` counts every boolean actually run.
  std::map<std::pair<uint64_t, uint64_t>, Clash> found;
  for (size_t k = 0; k < cands.size(); k++) {
    const Cand& cd = cands[k];
    const Hit& h = hits[k];
    const Placed& pa = placed[cd.a];
    const Placed& pb = placed[cd.b];
    out.pairsChecked++;
    out.pairsCached += h.cached ? 1 : 0;
    if ((exact || cd.rigid) && !h.known) {
      out.pairsUnknown++;
      s.unknownContacts.push_back({pa.root, pb.root, pa.rootName, pb.rootName});
      out.clean = false;
    }
    out.deflectionMm = std::max(out.deflectionMm, h.deflection);
    if (!h.hit || found.count(cd.key))
      continue;
    Clash c;
    c.rigid = cd.rigid;
    c.known = h.known;
    c.volume = h.volume;
    c.centroid = h.centroid;
    c.bboxMin = h.bboxMin;
    c.bboxMax = h.bboxMax;
    c.withinMin = glm::dvec3(glm::max(pa.lo, pb.lo));
    c.withinMax = glm::dvec3(glm::min(pa.hi, pb.hi));
    const bool aFirst = pa.root == cd.key.first;
    c.a = cd.key.first;
    c.b = cd.key.second;
    c.aName = aFirst ? pa.rootName : pb.rootName;
    c.bName = aFirst ? pb.rootName : pa.rootName;
    found[cd.key] = c;
  }
  for (auto& [key, c] : found)
    s.clashes.push_back(c);
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
  // on the tab must not be left at the last sample. It carries the study's
  // own configuration when it names one, which the solve then applies.
  AssemblyDocument asmDoc = asmDocIn;
  if (study.configuration)
    asmDoc.SetActiveConfigurationRaw(*study.configuration);
  Mate* driver = asmDoc.FindMateById(study.driverMate);

  MeshCache own;
  MeshCache& meshes = meshesIn ? *meshesIn : own;
  size_t worstCount = 0;
  bool worstSet = false;

  // Pass A: which pairs move relative to each other, so the clash pass
  // checks the rigid ones once instead of at every sample.
  const RelativeMotion motion =
      study.checkClash ? TrackMotion(asmDoc, parts, assemblies, study)
                       : RelativeMotion{};

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
        asmDoc, parts, assemblies, {driver->id, study.driverDof},
        /*diagnose=*/false);
    s.erroredMates = rep.erroredMates;
    s.residualMm = rep.residualMm;
    s.solved = rep.erroredMates == 0 && rep.loopsClosed;
    for (const Mate& m : asmDoc.GetMates()) {
      const bool turns = assembly::MateHasDof(m.type, MateDof::Rotation);
      const bool slides = assembly::MateHasDof(m.type, MateDof::Offset);
      if (!turns && !slides)
        continue; // fastened: nothing to report
      Sample::MateValue v;
      v.mate = m.id;
      if (turns)
        v.rotation = assembly::MateDofValue(m, MateDof::Rotation);
      if (slides)
        v.offset = assembly::MateDofValue(m, MateDof::Offset);
      s.mates.push_back(v);
    }

    auto flat = assembly::Flatten(asmDoc, parts, assemblies);
    if (!flat) {
      s.solved = false;
      out.samples.push_back(std::move(s));
      out.clean = false;
      continue;
    }
    // Placements: one per top-level occurrence, from the solved document.
    for (const Occurrence& o : asmDoc.GetOccurrences())
      s.worlds.emplace_back(o.id, OccurrenceWorld(o));

    if (study.checkClash) {
      // The rigid pairs go at the first sample that is checked at all: the
      // first one, unless it could not be flattened.
      const bool checkRigid = out.rigidCheckedAt < 0;
      if (checkRigid)
        out.rigidCheckedAt = i;
      CheckClashes(asmDoc, *flat, study, motion, checkRigid, meshes, contacts,
                   s, out);
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
    const double shown = deg ? angles::ToDegrees(s.value) : s.value;
    std::string clashes;
    for (const Clash& c : s.clashes) {
      if (!clashes.empty())
        clashes += ", ";
      clashes += c.aName + " x " + c.bName;
      if (c.rigid)
        clashes += " (rigid)";
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
  if (result.rigidCheckedAt >= 0)
    out += "Rigid pairs (no relative motion) checked once, exactly, at "
           "sample " +
           std::to_string(result.rigidCheckedAt) + ": " +
           std::to_string(result.pairsRigidChecked) + " pair checks; " +
           std::to_string(result.pairsRigid) + " skipped at later samples.\n";
  return out;
}

} // namespace motion

namespace motion {

Verdict Summarize(const Result& r, const MotionStudy& study, bool stale) {
  const bool deg = study.driverDof == MateDof::Rotation;
  const char* unit = deg ? "deg" : "mm";
  const auto shown = [&](const Sample& s) {
    return deg ? angles::ToDegrees(s.value) : s.value;
  };
  char line[512];
  Verdict v;
  if (stale) {
    v.text = "Stale: the model or the study changed since this run. Run "
             "again to see this revision.";
    return v;
  }
  if (!r.error.empty()) {
    v.level = Verdict::Level::Bad;
    v.text = "Could not run: " + r.error;
    return v;
  }
  if (r.cancelled) {
    std::snprintf(line, sizeof line,
                  "Cancelled: %zu of %d samples done, the rest not looked "
                  "at.",
                  r.samples.size(), study.samples);
    v.text = line;
    return v;
  }
  // Did it MOVE? Asked first, for a preview and a validation alike: a sample
  // that did not solve is a mechanism that locks or jumps there, and no
  // collision verdict means anything past it.
  size_t unsolved = 0;
  const Sample* first = nullptr;
  for (const Sample& s : r.samples)
    if (!s.solved) {
      ++unsolved;
      if (first == nullptr)
        first = &s;
    }
  if (first != nullptr) {
    const size_t idx = static_cast<size_t>(first - r.samples.data());
    std::snprintf(line, sizeof line,
                  "Does not solve at sample %zu (%.4g %s): residual %.3g mm, "
                  "%d errored mate%s; %zu of %zu samples unsolved.",
                  idx, shown(*first), unit, first->residualMm,
                  first->erroredMates, first->erroredMates == 1 ? "" : "s",
                  unsolved, r.samples.size());
    v.level = Verdict::Level::Bad;
    v.text = line;
    return v;
  }
  if (!r.checkedClashes) {
    std::snprintf(line, sizeof line,
                  "Moves through all %zu samples. Collisions not checked "
                  "(a preview): Validate to check them.",
                  r.samples.size());
    v.text = line;
    return v;
  }
  if (r.pairsUnknown) {
    std::snprintf(line, sizeof line,
                  "Incomplete: %zu pairs could not be checked.",
                  r.pairsUnknown);
    v.level = Verdict::Level::Bad;
    v.text = line;
    return v;
  }
  if (r.clean) {
    std::snprintf(line, sizeof line,
                  "Clean: moves through %zu samples, %zu pair checks, %.1f s.",
                  r.samples.size(), r.pairsChecked, r.seconds);
    v.level = Verdict::Level::Good;
    v.text = line;
    return v;
  }
  // Not clean, every sample solved: name the pair at the worst sample.
  const Sample& worst = r.samples.at(r.worstSample);
  std::string pair = "a clash";
  bool standing = false;
  if (!worst.clashes.empty()) {
    pair = worst.clashes.front().aName + " x " + worst.clashes.front().bName;
    standing = worst.clashes.front().rigid;
  }
  // A rigid pair's clash is not the motion's doing: it is there at rest, and
  // the fix is in the parts or where they are fastened, not in the travel.
  std::snprintf(line, sizeof line, "NOT clean: %s at sample %zu (%.4g %s)%s%s.",
                pair.c_str(), r.worstSample, shown(worst), unit,
                standing ? ", standing (the two do not move relative to each "
                           "other)"
                         : "",
                worst.clashes.size() > 1 ? ", and more there" : "");
  v.level = Verdict::Level::Bad;
  v.text = line;
  return v;
}

} // namespace motion
