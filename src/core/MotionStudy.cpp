// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MotionStudy.h"
#include "cad/KernelThreads.h"
#include "core/Angles.h"

#include "core/CollisionCache.h"
#include "core/ContactFollower.h"
#include "core/Document.h"
#include "core/FollowerCheck.h"
#include "core/InputHash.h"
#include "core/Measure.h"
#include "core/MeshCollide.h"
#include "core/Parallel.h"
#include "core/io/Serialization.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

namespace motion {

namespace {

// An assembly document's own content: what it places, how they are mated
// and coupled, its configurations and which one is active. The saved form,
// so a field added to the format is an input here without anyone listing it.
void HashAssembly(inputhash::Hasher& h, const AssemblyDocument& d) {
  h.U64(d.GetOccurrences().size());
  for (const Occurrence& o : d.GetOccurrences())
    h.Json(DocumentIO::OccurrenceToJson(o));
  h.U64(d.GetMates().size());
  for (const Mate& m : d.GetMates())
    h.Json(DocumentIO::MateToJson(m));
  h.U64(d.GetRelations().size());
  for (const Relation& r : d.GetRelations())
    h.Json(DocumentIO::RelationToJson(r));
  h.U64(d.GetConfigurations().size());
  for (const Configuration& c : d.GetConfigurations())
    h.Json(DocumentIO::ConfigurationToJson(c));
  h.U64(d.ActiveConfiguration());
}

} // namespace

uint64_t InputKey(const AssemblyDocument& asmDoc,
                  const assembly::PartResolver& parts,
                  const assembly::AssemblyResolver& assemblies,
                  const MotionStudy& study) {
  inputhash::Hasher h;
  // The algorithm's own version: a change to what a run computes (the clash
  // rule, the walk to the start) bumps it, so no answer stored before is
  // handed out as one from after.
  // 2: refined steps and branch jumps (v179); 3: a mesh check's touching
  // pairs, and no branch watched on a coordinate a relation sets (v180).
  h.Str("motion/3");
  nlohmann::json definition = DocumentIO::MotionStudyToJson(study);
  definition.erase("name"); // a label: two studies alike share an answer
  definition.erase("id");
  h.Json(definition);
  HashAssembly(h, asmDoc);
  for (const auto& [id, version] :
       assembly::SubtreeVersions(asmDoc, parts, assemblies)) {
    (void)version; // session-local: the content is what is hashed
    if (const AssemblyDocument* sub = assemblies ? assemblies(id) : nullptr) {
      h.U64(id);
      HashAssembly(h, *sub);
    }
  }
  for (const auto& [part, configuration] :
       assembly::SubtreePartConfigurations(asmDoc, assemblies)) {
    h.U64(part);
    h.U64(configuration);
    if (const Document* doc = parts ? parts(part, configuration) : nullptr)
      h.Bodies(doc->GetBodies());
    else
      h.U64(0);
  }
  return h.Digest();
}

uint64_t AssemblyInputKey(const AssemblyDocument& asmDoc,
                          const assembly::PartResolver& parts,
                          const assembly::AssemblyResolver& assemblies) {
  inputhash::Hasher h;
  h.Str("assembly/1");
  HashAssembly(h, asmDoc);
  for (const auto& [id, version] :
       assembly::SubtreeVersions(asmDoc, parts, assemblies)) {
    (void)version;
    if (const AssemblyDocument* sub = assemblies ? assemblies(id) : nullptr) {
      h.U64(id);
      HashAssembly(h, *sub);
    }
  }
  for (const auto& [part, configuration] :
       assembly::SubtreePartConfigurations(asmDoc, assemblies)) {
    h.U64(part);
    h.U64(configuration);
    if (const Document* doc = parts ? parts(part, configuration) : nullptr)
      h.Bodies(doc->GetBodies());
    else
      h.U64(0);
  }
  return h.Digest();
}

double ValueAt(const MotionStudy& study, double t) {
  return study.from + t * (study.to - study.from);
}

std::string WhyNotTrace(const AssemblyDocument& asmDoc,
                        const std::vector<TracePoint>& points) {
  std::set<std::string> names;
  for (size_t i = 0; i < points.size(); i++) {
    const TracePoint& p = points[i];
    const std::string at = "trace " + std::to_string(i + 1) + ": ";
    if (p.name.empty())
      return at + "a traced point needs a name, which is how the reply "
                  "keys its path";
    if (!names.insert(p.name).second)
      return at + "the name '" + p.name +
             "' is already traced; each point needs its own";
    if (!std::isfinite(p.pointMm.x) || !std::isfinite(p.pointMm.y) ||
        !std::isfinite(p.pointMm.z))
      return at + "point_mm is not three finite numbers";
    const Occurrence* o = asmDoc.FindById(p.occurrence);
    if (o == nullptr) {
      std::string have;
      for (const Occurrence& c : asmDoc.GetOccurrences())
        have += (have.empty() ? "" : ", ") + std::to_string(c.id) + " (" +
                c.name + ")";
      return at + "no occurrence " + std::to_string(p.occurrence) +
             " in this assembly; occurrences: " +
             (have.empty() ? std::string("none") : have);
    }
    if (p.inner != 0 && !o->flexible)
      return at + "occurrence " + std::to_string(p.occurrence) + " (" +
             o->name +
             ") is not a FLEXIBLE placement, so nothing moves inside it: "
             "trace the point on the occurrence itself";
  }
  return {};
}

bool TracePoints(const Result& result, const std::vector<TracePoint>& points,
                 std::vector<TracePath>* out, std::string* why) {
  std::vector<TracePath> paths;
  for (const TracePoint& p : points) {
    TracePath path;
    path.name = p.name;
    bool recorded = false;
    for (const Sample& s : result.samples) {
      const glm::dmat4* world = nullptr;
      if (p.inner == 0) {
        for (const auto& [id, m] : s.worlds)
          if (id == p.occurrence)
            world = &m;
      } else {
        for (const Sample::LeafWorld& l : s.leaves)
          if (l.root == p.occurrence && l.occurrence == p.inner)
            world = &l.world;
      }
      recorded = recorded || world != nullptr;
      if (world == nullptr || !s.solved) {
        // No pose (a sample that could not even be flattened) or one that
        // did not close: no point, never the last good one's.
        path.points.push_back(std::nullopt);
        path.unsolved++;
        continue;
      }
      const glm::dvec3 w = glm::dvec3(*world * glm::dvec4(p.pointMm, 1.0));
      path.points.push_back(w);
      path.min = path.any ? glm::min(path.min, w) : w;
      path.max = path.any ? glm::max(path.max, w) : w;
      path.any = true;
    }
    if (!recorded && !result.samples.empty()) {
      if (why) {
        if (p.inner != 0) {
          std::string moving;
          std::set<uint64_t> seen;
          for (const Sample::LeafWorld& l : result.samples.front().leaves)
            if (l.root == p.occurrence && seen.insert(l.occurrence).second)
              moving +=
                  (moving.empty() ? "" : ", ") + std::to_string(l.occurrence);
          *why = "trace '" + p.name + "': occurrence " +
                 std::to_string(p.inner) + " does not move inside placement " +
                 std::to_string(p.occurrence) +
                 ", so the run records no pose of its own for it; trace the "
                 "point on the placement itself. Moving there: " +
                 (moving.empty() ? std::string("none") : moving);
        } else {
          *why = "trace '" + p.name +
                 "': the run recorded no pose for "
                 "occurrence " +
                 std::to_string(p.occurrence);
        }
      }
      return false;
    }
    paths.push_back(std::move(path));
  }
  if (out)
    *out = std::move(paths);
  return true;
}

std::string WhyNot(const AssemblyDocument& asmDoc, const MotionStudy& study,
                   const assembly::AssemblyResolver& assemblies) {
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
      return WhyNot(under, study, assemblies);
    }
  }
  // A driver inside a flexible placement (v129): the placement's own
  // configuration holds and suppresses its mates, and says so.
  if (!study.driverPath.empty()) {
    std::string why;
    const Mate* m = assembly::MateAtPath(asmDoc, study.driverPath,
                                         study.driverMate, assemblies, &why);
    if (m == nullptr)
      return "driver: " + why;
    if (!assembly::MateHasDof(m->type, study.driverDof))
      return "driver mate " + std::to_string(m->id) + " (" + m->name +
             ") has no " +
             (study.driverDof == MateDof::Rotation ? "rotation" : "offset") +
             " to drive";
    const Occurrence* F = asmDoc.FindById(study.driverPath.front());
    const std::string refusal = assembly::FlexibleRefusal(*F, assemblies);
    if (!refusal.empty())
      return "driver: '" + F->name + "' cannot move here: " + refusal;
    const AssemblyDocument* sub = assemblies(F->partId);
    if (sub->MateSuppressedIn(F->configurationId, m->id))
      return "driver mate " + std::to_string(m->id) + " (" + m->name +
             ") is out of the configuration '" + F->name + "' runs under";
    if (const Configuration* c = sub->ConfigurationFor(F->configurationId))
      if (const MateOverride* o = c->OverrideForMate(m->id))
        if ((study.driverDof == MateDof::Rotation && o->rotation) ||
            (study.driverDof == MateDof::Offset && o->offset))
          return "driver mate " + std::to_string(m->id) + " (" + m->name +
                 ") is held by configuration '" + c->name + "' of '" + F->name +
                 "', so driving it would move nothing";
    if (study.samples < 2)
      return "a motion study needs at least 2 samples";
    return {};
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

glm::dmat4 OccurrenceWorld(const Occurrence& o) {
  return glm::translate(glm::dmat4(1.0), o.position) *
         glm::mat4_cast(o.orientation);
}

// What a clash check keys a body by: its top-level occurrence and, inside a
// FLEXIBLE placement (v129), the occurrence of that placement's sub-assembly
// it belongs to — a plunger and its barrel are two keys under one root, and
// are checked against each other. {root, 0} everywhere else.
using Key = std::pair<uint64_t, uint64_t>;

// Which keys move relative to each other over the study, read off a
// kinematics-only pass through every sample.
//
// Two parts bolted together touch — that is what a bolted joint is — and
// they carry an engaged thread or a press fit that only an exact check
// relieves, so checking every fixed joint by triangles at every sample made
// `clean` impossible on a real engine. It is also the wrong question to ask
// 48 times: a pair whose relative pose never changes
// has the same answer at every pose. So a rigid pair is checked ONCE, at the
// first sample, exactly, and skipped after. Skipping it outright hid three
// standing clashes in a pump that passed four exact studies.
class RelativeMotion {
public:
  // The expanded document: a synthetic occurrence is recorded as the
  // {root, inner} it stands for.
  void Record(const assembly::FlexibleExpansion& ex) {
    for (const Occurrence& o : ex.doc.GetOccurrences()) {
      const auto it = ex.occOf.find(o.id);
      const Key k = it != ex.occOf.end() ? it->second : Key{o.id, 0};
      track_[k].push_back(OccurrenceWorld(o));
    }
  }
  // True when the transform taking one into the other is not the same at
  // every sample — or when either was not tracked, which is checked rather
  // than assumed still. An inner key with no track of its own folds into
  // its root and moves as the root does.
  bool Moves(const Key& a, const Key& b) const {
    const auto ia = Find(a), ib = Find(b);
    if (ia == track_.end() || ib == track_.end())
      return true;
    const size_t n = std::min(ia->second.size(), ib->second.size());
    if (n < 2)
      return true;
    const glm::dmat4 first = glm::inverse(ia->second[0]) * ib->second[0];
    for (size_t k = 1; k < n; k++) {
      const glm::dmat4 rel = glm::inverse(ia->second[k]) * ib->second[k];
      for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
          if (std::abs(rel[c][r] - first[c][r]) > 1e-6)
            return true;
    }
    return false;
  }

private:
  std::map<Key, std::vector<glm::dmat4>>::const_iterator
  Find(const Key& k) const {
    const auto it = track_.find(k);
    return it != track_.end() ? it : track_.find(Key{k.first, 0});
  }
  std::map<Key, std::vector<glm::dmat4>> track_;
};

// Walk the driver from wherever the document has it to the study's first
// value, at no more than the study's own sample spacing, solving each step —
// the way the scrub walks (ui::ApplyMotionPose). Setting the first sample
// straight away was a JUMP from the document's pose: from a crank left at
// 180 degrees, a jump to 0 can land a slider-crank on its mirrored branch,
// and every later sample follows it there — the pistons of a whole run out
// through the bottom of the crankcase, each one a clean solve.
void WalkToStart(assembly::FlexibleExpansion& ex, Mate& driver,
                 const assembly::PartResolver& parts,
                 const assembly::AssemblyResolver& assemblies,
                 const MotionStudy& study) {
  const double from = assembly::MateDofValue(driver, study.driverDof);
  const double to = ValueAt(study, 0.0);
  const double stride =
      std::abs(study.to - study.from) / std::max(study.samples - 1, 1);
  if (!(stride > 1e-12) || std::abs(to - from) <= stride)
    return; // within one step: sample 0's own solve is that step
  const int steps = static_cast<int>(std::ceil(std::abs(to - from) / stride));
  for (int k = 1; k < steps; k++) { // the last step is sample 0 itself
    assembly::SetMateDof(driver, study.driverDof,
                         from + (to - from) * double(k) / steps);
    assembly::PropagateRelations(ex.doc, driver.id, parts);
    assembly::Solve(ex.doc, parts, assemblies, {driver.id, study.driverDof},
                    /*diagnose=*/false);
  }
}

// ---- Branches (docs/assemblies/README.md#branch-jumps) ---------------------
//
// Each sample's solve is seeded from the last, and a step too large for the
// seed can land on a mirror branch that closes every loop exactly: the
// Jansen leg at 45 degrees a step. So every free joint coordinate is
// watched between steps. A coordinate's RATE (its move per unit of the
// driver) changes smoothly along a branch -- by about its second derivative
// times the step -- and jumps where the solve did: by the whole flip at
// any step, or by the change of slope where the linkage passes a singular
// pose. A step whose rates moved more than kRateJump of the coordinate's
// largest rate so far is re-solved at half the step from where it started,
// and so on down to the study's max_refine halvings: a flip that was the
// step's goes away, and the rate change that is the linkage's stays.

// How far a coordinate's rate may move between steps, as a share of its
// largest rate so far (or of a tenth of the fastest coordinate's, for one
// that has barely moved).
constexpr double kRateJump = 0.5;
constexpr double kSlowShare = 0.1;
// At the finest step, a change that keeps more than this share of itself
// when the step is doubled is not the curvature's: smooth, it halves (0.5),
// a flip doubles (2), and a kink keeps 2/3 or more in one of the two steps
// either side of it, wherever in them it falls.
constexpr double kScaleKept = 0.6;
// A rate change whose move over the step is within what a loop's solve
// leaves open is the solve's leftover, not a branch: a loop closes to 1e-3
// mm (AssemblyLoopSolve's kCloseTolMm), a ten-thousandth of a radian at a
// 10 mm lever. A follower starting a cycloidal rise from rest moves its
// link by less than that over the first fine steps, and read as a flip.
constexpr double kOpenMm = 1e-3;
constexpr double kOpenRad = 1e-4;

// What Solve changes in the document a run steps: every placement and every
// mate's value. Taken before a step, put back to take it again finer.
struct Snapshot {
  std::vector<Occurrence> occurrences;
  std::vector<Mate> mates;
  explicit Snapshot(const AssemblyDocument& d)
      : occurrences(d.GetOccurrences()), mates(d.GetMates()) {}
  // Element by element, so pointers into the document (the driver) stay.
  void Restore(AssemblyDocument& d) const {
    auto& o = d.GetOccurrences();
    auto& m = d.GetMates();
    for (size_t k = 0; k < o.size() && k < occurrences.size(); k++)
      o[k] = occurrences[k];
    for (size_t k = 0; k < m.size() && k < mates.size(); k++)
      m[k] = mates[k];
  }
};

class BranchWatch {
public:
  BranchWatch(assembly::FlexibleExpansion& ex, Mate& driver,
              const assembly::PartResolver& parts,
              const assembly::AssemblyResolver& assemblies,
              const MotionStudy& study)
      : m_ex(ex), m_driver(driver), m_parts(parts), m_assemblies(assemblies),
        m_study(study) {
    const std::set<std::pair<uint64_t, bool>> set =
        SetForward(ex.doc, driver, study);
    const auto& mates = ex.doc.GetMates();
    for (size_t k = 0; k < mates.size(); k++)
      for (const bool rot : {true, false})
        if (assembly::MateHasDof(mates[k].type,
                                 rot ? MateDof::Rotation : MateDof::Offset) &&
            set.count({mates[k].id, rot}) == 0)
          m_coords.push_back({k, rot});
    m_maxRate.assign(m_coords.size(), 0.0);
    // The inputs: what a relation sets, the driver aside. Where one of them
    // changes its rate at once -- a cam's follower dropping off a step and
    // landing, an intermittent's window -- the loop it drives changes its
    // rate with it, and that is the input's kink, not a branch.
    for (size_t k = 0; k < mates.size(); k++)
      for (const bool rot : {true, false})
        if (mates[k].id != driver.id &&
            assembly::MateHasDof(mates[k].type,
                                 rot ? MateDof::Rotation : MateDof::Offset) &&
            set.count({mates[k].id, rot}) != 0)
          m_inputs.push_back({k, rot});
    m_maxInRate.assign(m_inputs.size(), 0.0);
  }

  // The coordinates nothing CHOOSES: the driver's, and every one a relation
  // sets as a function of another (PropagateRelations: a gear, a cam, an
  // intermittent, a screw, a member of a relation over several mates),
  // walked outward from the driver as propagation walks. They have no
  // branch to jump to -- a cam's follower leaving its base circle after a
  // dwell changes its rate at once, and is the lift table, not a flip --
  // so only what the loop solve picks is watched.
  static std::set<std::pair<uint64_t, bool>>
  SetForward(const AssemblyDocument& doc, const Mate& driver,
             const MotionStudy& study) {
    using Coord = std::pair<uint64_t, bool>;
    std::set<Coord> set{{driver.id, study.driverDof == MateDof::Rotation}};
    const auto rot = [](MateDof d) { return d == MateDof::Rotation; };
    const std::vector<Relation>& rels = doc.GetRelations();
    std::vector<bool> done(rels.size(), false);
    // Outward from what is set: a relation with a side set sets the other.
    // What the walk never reaches is applied A to B, as an unprompted
    // propagation applies it.
    for (bool reached = true; reached;) {
      reached = false;
      for (size_t i = 0; i < rels.size(); i++) {
        if (done[i] || doc.RelationSuppressed(rels[i].id))
          continue;
        const Relation& r = rels[i];
        if (r.HasTerms()) {
          std::vector<Coord> open;
          for (const RelationTerm& t : r.terms)
            if (set.count({t.mate, rot(t.dof)}) == 0)
              open.push_back({t.mate, rot(t.dof)});
          if (open.size() < r.terms.size() && !open.empty()) {
            set.insert(open.begin(), open.end());
            done[i] = reached = true;
          }
          continue;
        }
        const Coord a{r.mateA, rot(r.dofA)}, b{r.mateB, rot(r.dofB)};
        if (set.count(a) != 0 || set.count(b) != 0) {
          set.insert(a);
          set.insert(b);
          done[i] = reached = true;
        }
      }
    }
    for (size_t i = 0; i < rels.size(); i++) {
      if (done[i] || doc.RelationSuppressed(rels[i].id))
        continue;
      const Relation& r = rels[i];
      if (r.HasTerms()) {
        if (!r.terms.empty())
          set.insert({r.terms.back().mate, rot(r.terms.back().dof)});
      } else {
        set.insert({r.mateB, rot(r.dofB)});
      }
    }
    return set;
  }

  // The sample at `value` from the pose at the last one, `prev`: one step
  // when the rates hold, finer where they do not.
  assembly::SolveReport Advance(double prev, double value, Sample& s) {
    if (!m_last) // the first sample, or after one that did not solve
      return Restart(value);
    const double h = value - prev;
    const int maxDepth = std::max(0, m_study.maxRefine);
    int depth = m_haveRate ? 0 : maxDepth;
    double pos = prev;
    assembly::SolveReport rep;
    for (;;) {
      const double hs = h / double(1 << depth);
      const bool last = std::abs(value - pos) <= std::abs(hs) * (1.0 + 1e-9);
      const double target = last ? value : pos + hs;
      const Snapshot before(m_ex.doc);
      rep = SolveAt(target);
      const bool solved = rep.erroredMates == 0 && rep.loopsClosed;
      const std::vector<double> now = Read();
      const std::vector<double> nowIn = Read(m_inputs);
      const double step = target - pos;
      std::vector<double> rate(now.size());
      for (size_t k = 0; k < now.size(); k++)
        rate[k] = Move(k, now[k], (*m_last)[k]) / step;
      std::vector<double> inRate(nowIn.size());
      for (size_t k = 0; k < nowIn.size(); k++)
        inRate[k] = MoveOf(m_inputs[k].second, nowIn[k], m_lastIn[k]) / step;
      // Did an input change its rate at once over this step?
      bool inputKink = false;
      if (m_haveRate)
        for (size_t k = 0; k < inRate.size(); k++)
          if (std::abs(inRate[k] - m_lastInRate[k]) >
                  kRateJump * std::max(m_maxInRate[k], 1e-9) &&
              std::abs(inRate[k] - m_lastInRate[k]) * std::abs(step) >
                  (m_inputs[k].second ? kOpenRad : kOpenMm))
            inputKink = true;
      // The worst coordinate: its rate change against what it may be.
      double worst = 0.0;
      size_t worstK = 0;
      if (m_haveRate) {
        double fastest = 0.0;
        for (double r : m_maxRate)
          fastest = std::max(fastest, r);
        for (size_t k = 0; k < rate.size(); k++) {
          const double ref =
              std::max({m_maxRate[k], kSlowShare * fastest, 1e-9});
          if (std::abs(rate[k] - m_lastRate[k]) * std::abs(step) <= Open(k))
            continue;
          const double q = std::abs(rate[k] - m_lastRate[k]) / ref;
          if (q > worst) {
            worst = q;
            worstK = k;
          }
        }
      }
      bool jumped = m_haveRate && worst > kRateJump && !inputKink;
      if (jumped && solved && depth == maxDepth) {
        // The last rate was measured over a longer step, so a fast but
        // smooth joint (a knee near straight, a slider at its dead centre)
        // can change its rate by more than a share of what it has done so
        // far. So at the finest step the change is asked again, scale by
        // scale: the rate over this step against the rate over the same
        // step BACK from where it started, and the same over twice the
        // step. Along a branch the change is the curvature times the step,
        // and halves with it; at a kink it stays; across a flip it doubles.
        const Snapshot after(m_ex.doc);
        const auto at = [&](double v, std::vector<double>& out) {
          before.Restore(m_ex.doc);
          const assembly::SolveReport r = SolveAt(v);
          out = Read();
          return r.erroredMates == 0 && r.loopsClosed;
        };
        std::vector<double> back1, back2, fwd2;
        const bool ok = at(pos - step, back1) && at(pos - 2.0 * step, back2) &&
                        at(pos + 2.0 * step, fwd2);
        after.Restore(m_ex.doc);
        if (ok) {
          const std::vector<double>& c0 = *m_last;
          double fastest = 0.0;
          for (double r : m_maxRate)
            fastest = std::max(fastest, r);
          double q = 0.0;
          size_t qk = 0;
          double fine = 0.0, coarse = 0.0;
          for (size_t k = 0; k < now.size(); k++) {
            const double d1 = std::abs(Move(k, now[k], c0[k]) / step -
                                       Move(k, c0[k], back1[k]) / step);
            const double d2 = std::abs(Move(k, fwd2[k], c0[k]) / (2.0 * step) -
                                       Move(k, c0[k], back2[k]) / (2.0 * step));
            const double ref =
                std::max({m_maxRate[k], kSlowShare * fastest, 1e-9});
            if (d1 * std::abs(step) <= Open(k))
              continue;
            if (d1 / ref > q) {
              q = d1 / ref;
              qk = k;
              fine = d1;
              coarse = d2;
            }
          }
          jumped = q > kRateJump && fine > kScaleKept * coarse;
          worstK = qk;
          if (jumped)
            m_lastRate[qk] = Move(qk, c0[qk], back1[qk]) / step;
        }
      }
      if ((!solved || jumped) && depth < maxDepth) {
        before.Restore(m_ex.doc);
        depth++;
        s.refined = true;
        continue;
      }
      if (!solved) {
        // Locked at the finest step too: the sample is what one solve at
        // its value gives, and is reported unsolved.
        if (!last)
          rep = SolveAt(value);
        m_last.reset();
        return rep;
      }
      if (jumped) {
        Sample::BranchJump j;
        const Mate& m = m_ex.doc.GetMates()[m_coords[worstK].first];
        j.rotation = m_coords[worstK].second;
        j.name = m.name;
        j.mate = m.id;
        if (const auto it = m_ex.mateOf.find(m.id); it != m_ex.mateOf.end()) {
          j.path = {it->second.first};
          j.mate = it->second.second;
        }
        j.jump = rate[worstK] * step;
        j.expected = m_lastRate[worstK] * step;
        j.atValue = target;
        j.stepValue = step;
        if (!s.branchJump ||
            std::abs(j.jump - j.expected) >
                std::abs(s.branchJump->jump - s.branchJump->expected))
          s.branchJump = j;
        // The rate after a jump says nothing of the branch it landed on:
        // the next step measures it afresh, at the finest step.
        m_haveRate = false;
      } else {
        // An input's kink moves the loop at whatever rate it lands with
        // (a lever swung down by a drop in one step): that is not a rate
        // the loop keeps, so it does not raise what it may change by.
        if (!inputKink) {
          for (size_t k = 0; k < rate.size(); k++)
            m_maxRate[k] = std::max(m_maxRate[k], std::abs(rate[k]));
          for (size_t k = 0; k < inRate.size(); k++)
            m_maxInRate[k] = std::max(m_maxInRate[k], std::abs(inRate[k]));
        }
        m_lastRate = rate;
        m_lastInRate = inRate;
        m_haveRate = true;
      }
      m_last = now;
      m_lastIn = nowIn;
      if (last)
        return rep;
      pos = target;
      depth = m_haveRate ? std::max(0, depth - 1) : maxDepth;
    }
  }

  // The first sample: one solve, and the coordinates to watch from.
  assembly::SolveReport Restart(double value) {
    assembly::SolveReport rep = SolveAt(value);
    m_haveRate = false;
    if (rep.erroredMates == 0 && rep.loopsClosed) {
      m_last = Read();
      m_lastIn = Read(m_inputs);
    } else
      m_last.reset();
    return rep;
  }

private:
  assembly::SolveReport SolveAt(double value) {
    assembly::SetMateDof(m_driver, m_study.driverDof, value);
    assembly::PropagateRelations(m_ex.doc, m_driver.id, m_parts);
    return assembly::Solve(m_ex.doc, m_parts, m_assemblies,
                           {m_driver.id, m_study.driverDof},
                           /*diagnose=*/false);
  }
  std::vector<double> Read() const { return Read(m_coords); }
  std::vector<double>
  Read(const std::vector<std::pair<size_t, bool>>& coords) const {
    std::vector<double> v;
    v.reserve(coords.size());
    const auto& mates = m_ex.doc.GetMates();
    for (const auto& [k, rot] : coords)
      v.push_back(assembly::MateDofValue(mates[k], rot ? MateDof::Rotation
                                                       : MateDof::Offset));
    return v;
  }
  // A rotation's move is the shortest way round: a solve that wraps an
  // angle has not turned it a whole turn.
  static double MoveOf(bool rotation, double now, double was) {
    const double d = now - was;
    return rotation ? std::remainder(d, 2.0 * 3.14159265358979323846) : d;
  }
  double Move(size_t k, double now, double was) const {
    return MoveOf(m_coords[k].second, now, was);
  }
  double Open(size_t k) const {
    return m_coords[k].second ? kOpenRad : kOpenMm;
  }

  assembly::FlexibleExpansion& m_ex;
  Mate& m_driver;
  const assembly::PartResolver& m_parts;
  const assembly::AssemblyResolver& m_assemblies;
  const MotionStudy& m_study;
  std::vector<std::pair<size_t, bool>> m_coords; // mate index, rotation?
  std::optional<std::vector<double>> m_last;
  std::vector<double> m_lastRate, m_maxRate;
  // What relations set (the driver aside), watched for their own kinks.
  std::vector<std::pair<size_t, bool>> m_inputs;
  std::vector<double> m_lastIn, m_lastInRate, m_maxInRate;
  bool m_haveRate = false;
};

// Of a run's progress, the share the solves take when clashes are checked
// too; the checks have the rest.
constexpr double kSolveShare = 0.1;

// One placed body at one sample, for the pair sweep.
struct Placed {
  Key key; // {top-level occurrence, inner occurrence or 0}
  std::string name;
  const Body* body = nullptr;
  glm::dmat4 world{1.0};
  glm::dvec3 lo{0.0}, hi{0.0}; // world box
};

// A mesh per LOCAL shape, built once for the whole run and placed by matrix
// at every sample — the only reason 48 samples of a 250-occurrence sweep is
// tractable at all.
// One run asks for one deflection (MotionStudy::clashDeflectionMm, 0 =
// derived per body), so the cache need not key on it.
std::shared_ptr<const collide::Mesh>
MeshFor(MeshCache& cache, const CADShape& shape, double deflectionMm) {
  const void* key = shape.GeometryId();
  const auto it = cache.find(key);
  if (it != cache.end())
    return it->second;
  auto mesh = collide::Mesh::Build(shape, deflectionMm);
  cache[key] = mesh;
  return mesh;
}

bool WorldBox(const Body& b, const glm::dmat4& world, glm::dvec3& lo,
              glm::dvec3& hi) {
  std::vector<Body> one(1);
  one[0].shape = b.shape;
  return Measure::WorldBounds(one, world, lo, hi);
}

// One pair's answer.
struct Hit {
  bool cached = false;
  bool hit = false, known = false;
  bool touching = false; // a mesh check: met on the surface only
  double volume = 0.0, deflection = 0.0;
  glm::dvec3 centroid{0.0}, bboxMin{0.0}, bboxMax{0.0};
  // The press fits the exact check relieved (Measure::SetContact).
  std::vector<Measure::SetContact::PressFit> pressFits;
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
  // The contact cache keys on the shapes alone, so a pair whose answer
  // depends on more than its shapes -- an engaged thread, a ring's grip, a
  // press fit's bands -- is asked directly.
  const bool fitted = !ba[0].fitBands.empty() && !bb[0].fitBands.empty();
  const auto answer =
      contacts && ba[0].threadRegions.empty() && bb[0].threadRegions.empty() &&
              ba[0].ringGrips.empty() && bb[0].ringGrips.empty() && !fitted
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
    h.pressFits = c->pressFits;
  }
  return h;
}

Hit MeshHit(const Placed& a, const Placed& b, MeshCache& meshes,
            double deflectionMm) {
  Hit h;
  const auto ma = MeshFor(meshes, a.body->shape, deflectionMm);
  const auto mb = MeshFor(meshes, b.body->shape, deflectionMm);
  h.deflection = std::max(ma->Deflection(), mb->Deflection());
  const collide::Mesh::Contact c = ma->Classify(a.world, *mb, b.world);
  h.hit = c == collide::Mesh::Contact::Overlapping;
  h.touching = c == collide::Mesh::Contact::Touching;
  return h;
}

bool BoxesOverlap(const Placed& a, const Placed& b) {
  return !(a.lo.x > b.hi.x || b.lo.x > a.hi.x || a.lo.y > b.hi.y ||
           b.lo.y > a.hi.y || a.lo.z > b.hi.z || b.lo.z > a.hi.z);
}

// Every visible, non-reference body of the flattened assembly, placed.
// `flexible` holds the roots whose insides are keyed per leaf.
std::vector<Placed> PlaceBodies(const AssemblyDocument& asmDoc,
                                const std::vector<assembly::FlatInstance>& flat,
                                const std::set<uint64_t>& flexible) {
  std::vector<Placed> placed;
  for (const assembly::FlatInstance& inst : flat) {
    if (!inst.visible || inst.part == nullptr)
      continue;
    const Occurrence* root = asmDoc.FindById(inst.rootOccurrenceId);
    Key key{inst.rootOccurrenceId, 0};
    std::string name = root ? root->name : "";
    if (flexible.count(inst.rootOccurrenceId) && !inst.innerIds.empty()) {
      key.second = inst.innerIds.front();
      // The occurrence of the unit this leaf belongs to: the leaf itself,
      // or the rigid sub-assembly inside the unit that holds it.
      name += "/" + (inst.levels.size() >= 2 ? inst.levels[1].name : inst.name);
    }
    for (const Body& b : inst.part->GetBodies()) {
      if (b.reference || !b.shape.IsValid())
        continue;
      Placed p{key, name, &b, inst.world};
      if (WorldBox(b, inst.world, p.lo, p.hi))
        placed.push_back(std::move(p));
    }
  }
  return placed;
}

// A contact relation's cam and follower, as the mesh check judges them.
// They stand tangent by design, and on a curved flank a tessellation
// pierces at the tangency whatever its deflection (a concave flank's chords
// lie outside it), so where the mesh calls the pair overlapping the follower
// check's cuts across the width they share decide instead
// (docs/assemblies/relations.md#the-follower-check): touching when the
// follower enters none of them by more than the contact tolerance at the
// sample's a and b, else the clash it is. The cam and the follower can
// only meet within the width they share, which is what the cuts span.
struct ContactPair {
  uint64_t cam = 0, follower = 0;
  uint64_t mateA = 0, mateB = 0;
  bool turn = false;
  std::shared_ptr<const contact::Model> model;
  std::vector<follower::Level> levels;
};

std::vector<ContactPair> ContactPairs(const AssemblyDocument& asmDoc,
                                      const assembly::PartResolver& parts) {
  std::vector<ContactPair> out;
  for (const Relation& r : asmDoc.GetRelations()) {
    if (r.kind != RelationKind::Contact || asmDoc.RelationSuppressed(r.id))
      continue;
    std::string why;
    auto m = contact::ModelFor(asmDoc, r, parts, &why);
    if (!m)
      continue; // the relation reports its own error; the mesh decides
    ContactPair p;
    p.cam = m->cam;
    p.follower = m->follower;
    p.mateA = r.mateA;
    p.mateB = r.mateB;
    p.turn = r.dofB == MateDof::Rotation;
    p.levels = follower::Levels(*m, &why);
    if (p.levels.empty())
      continue;
    p.model = std::move(m);
    out.push_back(std::move(p));
  }
  return out;
}

// Whether a sample's pose stands the pair's follower on its cam, by the
// cuts across the width: nullopt when the pair is no contact relation's.
std::optional<bool> ContactTouches(const std::vector<ContactPair>& pairs,
                                   const Key& ka, const Key& kb,
                                   const Sample& s) {
  if (ka.second != 0 || kb.second != 0)
    return std::nullopt;
  for (const ContactPair& p : pairs) {
    if (!((ka.first == p.cam && kb.first == p.follower) ||
          (ka.first == p.follower && kb.first == p.cam)))
      continue;
    std::optional<double> a, b;
    for (const Sample::MateValue& mv : s.mates) {
      if (!mv.path.empty())
        continue;
      if (mv.mate == p.mateA)
        a = mv.rotation;
      if (mv.mate == p.mateB)
        b = p.turn ? mv.rotation : mv.offset;
    }
    if (!a || !b)
      return std::nullopt;
    return follower::AcrossWidth(*p.model, p.levels, *a, *b).ok;
  }
  return std::nullopt;
}

// Every sample's clash check, in ONE batch, into `out.samples` and the run's
// tallies. `placed[i]` holds sample i's bodies (empty when it could not be
// flattened); `rigidAt` is the sample the rigid pairs are checked at.
//
// One batch rather than one per sample: every sample's pose is known before
// any check runs, and a pair that costs seconds at every pose (a worm
// against its wheel) used to serialise the run, each sample's boolean
// waiting for the last one's while the other cores sat idle. Returns the
// number of leading samples whose every check completed: all of them unless
// `cancelled` stopped the batch.
size_t CheckAllClashes(const std::vector<std::vector<Placed>>& placed,
                       int rigidAt, const MotionStudy& study,
                       const RelativeMotion& motion, MeshCache& meshes,
                       collide::ExactContactCache* contacts,
                       const std::vector<ContactPair>& contactPairs,
                       const std::function<bool()>& cancelled,
                       const std::function<void(double)>& progress,
                       Result& out) {
  const bool exact = study.clashMode == "exact";
  // Candidates first, then the booleans on every core. Only pairs whose
  // boxes overlap can clash, and that clears most of any assembly for
  // nothing. A contact check is ~161 ms and one engine study is 20,403 of
  // them: run serially it was 3292 s, 85% of a whole generator run.
  struct Cand {
    size_t s, a, b;
    bool rigid;
    std::pair<Key, Key> key;
    // An earlier candidate of this run asking the same question: the same
    // two bodies at the same relative pose (to within the exact cache's
    // tolerance) -- a dwell, or a full turn come round. Its answer is this
    // one's when it found them clear; a clash is asked again, since where it
    // is is in world space.
    long repeatOf = -1;
  };
  std::vector<Cand> cands;
  for (size_t s = 0; s < placed.size(); s++) {
    const std::vector<Placed>& ps = placed[s];
    const bool checkRigid = static_cast<int>(s) == rigidAt;
    for (size_t a = 0; a < ps.size(); a++)
      for (size_t b = a + 1; b < ps.size(); b++) {
        if (ps[a].key == ps[b].key)
          continue; // a part does not clash with itself
        const bool rigid = !motion.Moves(ps[a].key, ps[b].key);
        if (rigid && !checkRigid) {
          out.pairsRigid++;
          continue; // checked at the first sample; nothing since has changed
        }
        if (!BoxesOverlap(ps[a], ps[b]))
          continue;
        if (rigid)
          out.pairsRigidChecked++;
        cands.push_back(
            {s,
             a,
             b,
             rigid,
             {std::min(ps[a].key, ps[b].key), std::max(ps[a].key, ps[b].key)},
             -1});
      }
  }

  // Repeats, for the exact checks (a mesh check is cheap): keyed by the two
  // bodies, then matched by relative pose.
  {
    std::map<std::pair<const Body*, const Body*>,
             std::vector<std::pair<glm::dmat4, size_t>>>
        seen;
    for (size_t k = 0; k < cands.size(); k++) {
      Cand& cd = cands[k];
      if (!(exact || cd.rigid))
        continue;
      const Placed& pa = placed[cd.s][cd.a];
      const Placed& pb = placed[cd.s][cd.b];
      const glm::dmat4 rel = glm::inverse(pa.world) * pb.world;
      auto& bucket = seen[{pa.body, pb.body}];
      for (const auto& [r, idx] : bucket)
        if (collide::ExactContactCache::SameRelative(r, rel)) {
          cd.repeatOf = static_cast<long>(idx);
          break;
        }
      if (cd.repeatOf < 0)
        bucket.emplace_back(rel, k);
    }
  }

  // Mesh mode caches per shape in a shared map, so every mesh it will want
  // is built HERE, single-threaded, before anything reads it.
  if (!exact)
    for (const Cand& c : cands)
      if (!c.rigid) {
        MeshFor(meshes, placed[c.s][c.a].body->shape, study.clashDeflectionMm);
        MeshFor(meshes, placed[c.s][c.b].body->shape, study.clashDeflectionMm);
      }

  // The callbacks are the caller's and need not be thread-safe: the workers
  // take turns at them.
  std::mutex callbacks;
  std::atomic<bool> stop{false};
  std::atomic<size_t> finished{0};
  size_t toRun = 0;
  for (const Cand& c : cands)
    toRun += c.repeatOf < 0 ? 1 : 0;
  std::vector<Hit> hits(cands.size());
  std::vector<char> done(cands.size(), 0);
  const auto check = [&](size_t k) {
    if (stop.load())
      return;
    if (cancelled) {
      std::lock_guard lock(callbacks);
      if (cancelled()) {
        stop.store(true);
        return;
      }
    }
    const Cand& cd = cands[k];
    const Placed& pa = placed[cd.s][cd.a];
    const Placed& pb = placed[cd.s][cd.b];
    // A rigid pair is always asked exactly: a fastened pair's engaged thread
    // and press fit are relieved only by an exact check.
    hits[k] = exact || cd.rigid
                  ? ExactHit(pa, pb, contacts)
                  : MeshHit(pa, pb, meshes, study.clashDeflectionMm);
    done[k] = 1;
    const size_t n = finished.fetch_add(1) + 1;
    if (progress && toRun > 0) {
      std::lock_guard lock(callbacks);
      progress(kSolveShare + (1.0 - kSolveShare) *
                                 std::min(1.0, static_cast<double>(n) /
                                                   static_cast<double>(toRun)));
    }
  };
  std::vector<size_t> first;
  for (size_t k = 0; k < cands.size(); k++)
    if (cands[k].repeatOf < 0)
      first.push_back(k);
  // An exact check is booleans, and on a kernel whose error-handler stack
  // is shared, more threads of them are slower (cad/KernelThreads.h). A
  // mesh check is arithmetic, with no lock to queue on.
  const bool booleans =
      exact || std::any_of(cands.begin(), cands.end(),
                           [](const Cand& c) { return c.rigid; });
  const size_t cap =
      booleans ? cad::BooleanWorkers(parallel::Workers()) : parallel::Workers();
  parallel::For(first.size(), [&](size_t i) { check(first[i]); }, cap);
  // A repeat takes its original's answer when that was clear, and is asked
  // itself when it was a clash or unknown.
  std::vector<size_t> again;
  for (size_t k = 0; k < cands.size(); k++) {
    const long r = cands[k].repeatOf;
    if (r < 0 || !done[r])
      continue;
    const Hit& h = hits[r];
    if (h.known && !h.hit) {
      hits[k] = h;
      hits[k].cached = false;
      done[k] = 1;
      out.pairsRepeated++;
    } else {
      again.push_back(k);
    }
  }
  parallel::For(again.size(), [&](size_t i) { check(again[i]); }, cap);

  // How far the batch got: the leading samples whose every check is done.
  size_t complete = placed.size();
  for (size_t k = 0; k < cands.size(); k++)
    if (!done[k]) {
      complete = std::min(complete, cands[k].s);
      break; // candidates are in sample order
    }

  // The merge keeps the FIRST hit per key pair, in candidate order -- a rule
  // that is inherently sequential, so it lives here and not in the workers.
  // `pairsChecked` counts every check answered, repeats included.
  std::vector<std::map<std::pair<Key, Key>, Clash>> found(placed.size());
  std::vector<std::set<std::pair<Key, Key>>> touched(placed.size());
  std::set<std::pair<Key, Key>> pressFitted;
  for (const PressFit& f : out.pressFits)
    pressFitted.insert({std::min(Key{f.a, f.aInner}, Key{f.b, f.bInner}),
                        std::max(Key{f.a, f.aInner}, Key{f.b, f.bInner})});
  for (size_t k = 0; k < cands.size(); k++) {
    const Cand& cd = cands[k];
    if (cd.s >= complete)
      break;
    Sample& s = out.samples[cd.s];
    Hit h = hits[k];
    if (!exact && !cd.rigid && h.hit) {
      const Placed& qa = placed[cd.s][cd.a];
      const Placed& qb = placed[cd.s][cd.b];
      if (const auto on = ContactTouches(contactPairs, qa.key, qb.key, s);
          on && *on) {
        h.hit = false;
        h.touching = true;
      }
    }
    const Placed& pa = placed[cd.s][cd.a];
    const Placed& pb = placed[cd.s][cd.b];
    const bool aFirst = pa.key == cd.key.first;
    const Placed& p1 = aFirst ? pa : pb;
    const Placed& p2 = aFirst ? pb : pa;
    out.pairsChecked++;
    out.pairsCached += h.cached ? 1 : 0;
    if ((exact || cd.rigid) && !h.known) {
      UnknownContact u;
      u.a = p1.key.first;
      u.aInner = p1.key.second;
      u.b = p2.key.first;
      u.bInner = p2.key.second;
      u.aName = p1.name;
      u.bName = p2.name;
      s.unknownContacts.push_back(std::move(u));
      out.pairsUnknown++;
      out.clean = false;
    }
    out.deflectionMm = std::max(out.deflectionMm, h.deflection);
    // A press fit, once per pair over the run: the first sample that
    // relieved it (docs/tolerances/README.md#press-fits).
    if (!h.pressFits.empty() && pressFitted.insert(cd.key).second) {
      PressFit f;
      f.a = p1.key.first;
      f.aInner = p1.key.second;
      f.b = p2.key.first;
      f.bInner = p2.key.second;
      f.aName = p1.name;
      f.bName = p2.name;
      for (const Measure::SetContact::PressFit& pf : h.pressFits) {
        f.fit += (f.fit.empty() ? "" : "; ") + pf.fit;
        f.interference = std::max(f.interference, pf.interference);
        f.maxInterference = std::max(f.maxInterference, pf.maxInterference);
        f.volume += pf.volume;
      }
      out.pressFits.push_back(std::move(f));
    }
    if (h.touching && !h.hit && touched[cd.s].insert(cd.key).second) {
      Touch t;
      t.a = p1.key.first;
      t.aInner = p1.key.second;
      t.b = p2.key.first;
      t.bInner = p2.key.second;
      t.aName = p1.name;
      t.bName = p2.name;
      s.touching.push_back(std::move(t));
    }
    if (!h.hit || found[cd.s].count(cd.key))
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
    c.a = p1.key.first;
    c.aInner = p1.key.second;
    c.b = p2.key.first;
    c.bInner = p2.key.second;
    c.aName = p1.name;
    c.bName = p2.name;
    found[cd.s][cd.key] = c;
  }
  for (size_t s = 0; s < complete; s++)
    for (auto& [key, c] : found[s])
      out.samples[s].clashes.push_back(c);
  return complete;
}

void RecordMates(const AssemblyDocument& doc, const std::vector<uint64_t>& path,
                 const std::vector<const Mate*>& mates, Sample& s) {
  for (const Mate* m : mates) {
    const bool turns = assembly::MateHasDof(m->type, MateDof::Rotation);
    const bool slides = assembly::MateHasDof(m->type, MateDof::Offset);
    if (!turns && !slides)
      continue; // fastened: nothing to report
    Sample::MateValue v;
    v.mate = m->id;
    v.path = path;
    v.name = m->name;
    v.turns = turns;
    v.slides = slides;
    if (turns)
      v.rotation = assembly::MateDofValue(*m, MateDof::Rotation);
    if (slides)
      v.offset = assembly::MateDofValue(*m, MateDof::Offset);
    s.mates.push_back(std::move(v));
  }
  (void)doc;
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
  out.error = WhyNot(asmDocIn, study, assemblies);
  if (!out.error.empty())
    return out;

  // A COPY: the run drives a mate through its whole range and the document
  // on the tab must not be left at the last sample. It carries the study's
  // own configuration when it names one, which the solve then applies.
  AssemblyDocument asmDoc = asmDocIn;
  if (study.configuration)
    asmDoc.SetActiveConfigurationRaw(*study.configuration);
  // Flexible placements (v129) are expanded ONCE, and every sample runs on
  // the expanded copy; each sample is collapsed back into `asmDoc`, which is
  // what the clash check and the recorded poses read.
  assembly::FlexibleExpansion ex =
      assemblies ? assembly::ExpandFlexible(asmDoc, parts, assemblies)
                 : assembly::FlexibleExpansion{asmDoc};
  const uint64_t driverId = ex.MateId(study.driverPath, study.driverMate);
  Mate* driver = ex.doc.FindMateById(driverId);
  if (driver == nullptr) {
    out.error = "the driver mate is not among the moving mates of '" +
                std::to_string(
                    study.driverPath.empty() ? 0 : study.driverPath.front()) +
                "' this run expanded";
    return out;
  }
  WalkToStart(ex, *driver, parts, assemblies, study);
  std::set<uint64_t> flexible;
  for (const assembly::FlexibleUnit& u : ex.units)
    if (u.error.empty())
      flexible.insert(u.root);

  MeshCache own;
  MeshCache& meshes = meshesIn ? *meshesIn : own;
  const double solveShare = study.checkClash ? kSolveShare : 1.0;

  // Pass A: every sample solved ONCE, in order -- each solve starts from the
  // last, so a closed loop stays on its branch -- with its placements
  // recorded. The same pass records which pairs move relative to each other,
  // so the checks can ask the rigid ones once instead of at every sample.
  RelativeMotion motion;
  BranchWatch watch(ex, *driver, parts, assemblies, study);
  std::vector<std::vector<Placed>> placed;
  std::vector<char> flattened;
  for (int i = 0; i < study.samples; i++) {
    if (cancelled && cancelled()) {
      out.cancelled = true;
      out.clean = false;
      break;
    }
    Sample s;
    s.t = static_cast<double>(i) / static_cast<double>(study.samples - 1);
    s.value = ValueAt(study, s.t);

    // Stepped from the last sample, finer where a coordinate jumps.
    assembly::SolveReport rep =
        i == 0 ? watch.Restart(s.value)
               : watch.Advance(out.samples.back().value, s.value, s);
    if (s.refined)
      out.refinedSamples++;
    if (s.branchJump)
      out.branchJumps++;
    if (study.checkClash)
      motion.Record(ex);
    // Collapsed whenever there were units at all, so one that could not
    // move still reports why, sample by sample.
    const bool collapse = !ex.units.empty();
    if (collapse)
      assembly::CollapseFlexible(ex, asmDoc, &rep);
    const AssemblyDocument& solved = collapse ? asmDoc : ex.doc;
    s.erroredMates = rep.erroredMates;
    s.residualMm = rep.residualMm;
    s.solved = rep.erroredMates == 0 && rep.loopsClosed;
    {
      std::vector<const Mate*> own;
      for (const Mate& m : solved.GetMates())
        own.push_back(&m);
      RecordMates(solved, {}, own, s);
      for (const assembly::FlexibleUnit& u : ex.units) {
        if (!u.error.empty())
          continue;
        std::vector<const Mate*> inner;
        for (uint64_t m : u.mates)
          inner.push_back(ex.doc.FindMateById(ex.mateIdOf.at({u.root, m})));
        const size_t before = s.mates.size();
        RecordMates(ex.doc, {u.root}, inner, s);
        // ...by the sub-assembly's own ids, which is how a caller names them.
        for (size_t k = before; k < s.mates.size(); k++)
          s.mates[k].mate = ex.mateOf.at(s.mates[k].mate).second;
      }
    }

    auto flat = assembly::Flatten(solved, parts, assemblies);
    if (!flat) {
      s.solved = false;
      out.samples.push_back(std::move(s));
      placed.emplace_back();
      flattened.push_back(0);
      continue;
    }
    // Placements: one per top-level occurrence, from the solved document,
    // and one per moving occurrence inside a flexible placement.
    for (const Occurrence& o : solved.GetOccurrences())
      s.worlds.emplace_back(o.id, OccurrenceWorld(o));
    for (const assembly::FlexibleUnit& u : ex.units)
      if (u.error.empty())
        for (uint64_t k : u.moving)
          s.leaves.push_back(
              {u.root, k,
               OccurrenceWorld(*ex.doc.FindById(ex.occIdOf.at({u.root, k})))});
    // The bodies placed here, for the checks: single-threaded, since a world
    // box fills a cache on the shape.
    placed.push_back(study.checkClash ? PlaceBodies(solved, *flat, flexible)
                                      : std::vector<Placed>{});
    flattened.push_back(1);
    out.samples.push_back(std::move(s));
    if (progress)
      progress(solveShare * static_cast<double>(i + 1) /
               static_cast<double>(study.samples));
  }

  // Pass B: the checks, all samples at once. A run cancelled during the
  // solves checks nothing: a sample is finished when it is checked.
  if (study.checkClash) {
    if (out.cancelled) {
      out.samples.clear();
    } else {
      // The rigid pairs go at the first sample that is checked at all: the
      // first one, unless it could not be flattened.
      for (size_t i = 0; i < placed.size(); i++)
        if (flattened[i]) {
          out.rigidCheckedAt = static_cast<int>(i);
          break;
        }
      const size_t complete = CheckAllClashes(
          placed, out.rigidCheckedAt, study, motion, meshes, contacts,
          study.clashMode == "exact" ? std::vector<ContactPair>{}
                                     : ContactPairs(asmDoc, parts),
          cancelled, progress, out);
      if (complete < out.samples.size()) {
        out.samples.resize(complete);
        out.cancelled = true;
      }
    }
  }

  // The verdict over what was finished.
  size_t worstCount = 0;
  bool worstSet = false;
  for (size_t i = 0; i < out.samples.size(); i++) {
    const Sample& s = out.samples[i];
    const bool bad = !s.solved || s.branchJump || !s.clashes.empty() ||
                     !s.unknownContacts.empty();
    if (bad)
      out.clean = false;
    // The worst sample: the first unsolved one wins outright, else the one
    // with the most clashes.
    const auto kinematic = [](const Sample& x) {
      return !x.solved || x.branchJump.has_value();
    };
    if (kinematic(s) &&
        (!worstSet || !kinematic(out.samples[out.worstSample]))) {
      out.worstSample = i;
      worstSet = true;
      worstCount = SIZE_MAX;
    } else if (!kinematic(s) &&
               s.clashes.size() + s.unknownContacts.size() > worstCount &&
               (!worstSet || !kinematic(out.samples[out.worstSample]))) {
      out.worstSample = i;
      worstCount = s.clashes.size() + s.unknownContacts.size();
      worstSet = true;
    }
  }
  if (out.cancelled)
    out.clean = false;
  out.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  return out;
}

static std::string BranchJumpText(const Sample::BranchJump& j, bool deg) {
  const auto value = [&](double v, bool rot) {
    char b[48];
    std::snprintf(b, sizeof b, "%.4g %s", rot ? angles::ToDegrees(v) : v,
                  rot ? "deg" : "mm");
    return std::string(b);
  };
  return "'" + j.name + "' " + (j.rotation ? "turned " : "slid ") +
         value(j.jump, j.rotation) + " where its rate gave " +
         value(j.expected, j.rotation) + ", over a step of " +
         value(j.stepValue, deg) + " to " + value(j.atValue, deg);
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
    if (s.branchJump) {
      if (!clashes.empty())
        clashes += ", ";
      clashes += "BRANCH JUMP " + BranchJumpText(*s.branchJump, deg);
    }
    std::snprintf(line, sizeof line, "%-6zu %-12.2f %-8s %-10.4f %s\n", i,
                  shown, s.solved ? "yes" : "NO", s.residualMm,
                  clashes.empty() ? "-" : clashes.c_str());
    out += line;
  }
  std::snprintf(line, sizeof line, "%s: %zu samples, %zu pair checks, %.2f s\n",
                !result.error.empty()    ? "ERROR"
                : result.cancelled       ? "INCOMPLETE"
                : result.branchJumps     ? "BRANCH JUMP"
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
  if (result.refinedSamples)
    out += std::to_string(result.refinedSamples) +
           " samples re-solved at a finer step, where a joint's rate jumped "
           "between samples.\n";
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
  // Did it stay on its branch? A jump that the finest step still makes is
  // a singular pose the linkage passes, and it may leave on either branch.
  for (size_t i = 0; i < r.samples.size(); i++)
    if (const auto& j = r.samples[i].branchJump) {
      v.level = Verdict::Level::Bad;
      v.text = "Jumps branch at sample " + std::to_string(i) + ": " +
               BranchJumpText(*j, deg) +
               ". The linkage passes a singular "
               "pose there; " +
               std::to_string(r.branchJumps) + " sample" +
               (r.branchJumps == 1 ? "" : "s") + " jumped.";
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
