// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Flexible sub-assemblies (v129,
// docs/assemblies/README.md#flexible-sub-assemblies): the expansion that lets
// a placed sub-assembly's own mechanism move in its parent, around a solver
// that does not know it exists.
#include "core/AssemblySolve.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace assembly {

namespace {

glm::dmat4 OccMatrix(const Occurrence& o) {
  return glm::translate(glm::dmat4(1.0), o.position) *
         glm::mat4_cast(o.orientation);
}

void Decompose(const glm::dmat4& m, glm::dvec3& position,
               glm::dquat& orientation) {
  position = glm::dvec3(m[3]);
  orientation = glm::normalize(glm::quat_cast(glm::dmat3(m)));
}

// Synthetic ids: far above anything a document's counter reaches, so an
// expanded id can never be mistaken for one of the parent's.
constexpr uint64_t kSyntheticBase = uint64_t{1} << 56;

void ApplyDof(Mate& m, const InnerMateDof& d) {
  m.rotation = d.rotation;
  m.offset = d.offset;
  m.dx = d.dx;
  m.dy = d.dy;
  m.ballRotation = d.ballRotation;
}

InnerMateDof DofOf(const Mate& m) {
  InnerMateDof d;
  d.mate = m.id;
  d.rotation = m.rotation;
  d.offset = m.offset;
  d.dx = m.dx;
  d.dy = m.dy;
  d.ballRotation = m.ballRotation;
  return d;
}

std::string Quoted(const std::string& name, uint64_t id) {
  return name.empty() ? "#" + std::to_string(id) : "'" + name + "'";
}

} // namespace

glm::dmat4 InnerPlacement(const Occurrence& owner, const Occurrence& inner) {
  if (owner.flexible)
    if (const InnerPose* p = owner.FindInnerPose(inner.id))
      return glm::translate(glm::dmat4(1.0), p->position) *
             glm::mat4_cast(p->orientation);
  return OccMatrix(inner);
}

std::string FlexibleRefusal(const Occurrence& owner,
                            const AssemblyResolver& asmById) {
  if (owner.refKind != OccurrenceRef::Assembly)
    return "'" + owner.name +
           "' places a part, which has no mechanism of its own — flexible "
           "is for a placed sub-assembly";
  const AssemblyDocument* sub = asmById ? asmById(owner.partId) : nullptr;
  if (sub == nullptr)
    return "the sub-assembly '" + owner.name + "' places is not loaded";
  bool anyFixed = false;
  for (const Occurrence& o : sub->GetOccurrences()) {
    if (o.flexible)
      return "'" + o.name + "' inside it is flexible itself, and only one " +
             "level moves: make '" + o.name +
             "' rigid in that assembly, or place its parts there directly";
    if (o.fixed && !sub->OccurrenceSuppressedIn(owner.configurationId, o.id))
      anyFixed = true;
  }
  for (const Relation& r : sub->GetRelations())
    if (r.kind == RelationKind::Freewheel || r.kind == RelationKind::Click)
      return "its relation " + Quoted(r.name, r.id) + " is a " +
             (r.kind == RelationKind::Freewheel ? "freewheel" : "click") +
             ", whose history (its lead) would have to be kept per "
             "placement — a flexible sub-assembly takes gear, chain, rack, "
             "screw, linear and cam relations";
  if (!anyFixed)
    return "nothing inside it is fixed, so nothing stands still for its "
           "mechanism to move against — fix one occurrence in that assembly "
           "(its frame, the housing)";
  return {};
}

uint64_t FlexibleExpansion::MateId(const std::vector<uint64_t>& path,
                                   uint64_t mate) const {
  if (path.empty())
    return mate;
  if (path.size() != 1)
    return 0;
  const auto it = mateIdOf.find({path.front(), mate});
  return it == mateIdOf.end() ? 0 : it->second;
}

const FlexibleUnit* FlexibleExpansion::UnitFor(uint64_t root) const {
  for (const FlexibleUnit& u : units)
    if (u.root == root)
      return &u;
  return nullptr;
}

FlexibleExpansion ExpandFlexible(const AssemblyDocument& parent,
                                 const PartResolver& partById,
                                 const AssemblyResolver& asmById) {
  (void)partById;
  FlexibleExpansion ex;
  ex.doc = parent;
  AssemblyDocument& doc = ex.doc;
  uint64_t next = kSyntheticBase;
  std::vector<MateOverride> holds; // the units' own configurations' holds

  for (const Occurrence& F : parent.GetOccurrences()) {
    if (!F.flexible)
      continue;
    FlexibleUnit unit;
    unit.root = F.id;
    // Hidden by the parent's configuration: nothing to move.
    if (parent.OccurrenceSuppressed(F.id))
      continue;
    unit.error = FlexibleRefusal(F, asmById);
    if (!unit.error.empty()) {
      ex.units.push_back(std::move(unit));
      continue;
    }
    const AssemblyDocument& sub = *asmById(F.partId);
    unit.sub = &sub;
    const uint64_t cfgId = F.configurationId;
    const Configuration* cfg = sub.ConfigurationFor(cfgId);
    const auto suppressed = [&](uint64_t occ) {
      return cfg != nullptr && cfg->Suppresses(occ);
    };

    // What MOVES: every occurrence reached through its mates from a fixed
    // one. The rest cannot move relative to the unit and fold into it —
    // they ride with the root, as a rigid unit's parts always did.
    std::set<uint64_t> fixedSet, moving;
    for (const Occurrence& o : sub.GetOccurrences())
      if (o.fixed && !suppressed(o.id))
        fixedSet.insert(o.id);
    std::vector<const Mate*> active;
    for (const Mate& m : sub.GetMates())
      if (!sub.MateSuppressedIn(cfgId, m.id))
        active.push_back(&m);
    std::vector<uint64_t> frontier(fixedSet.begin(), fixedSet.end());
    std::set<uint64_t> reached(fixedSet.begin(), fixedSet.end());
    while (!frontier.empty()) {
      const uint64_t at = frontier.back();
      frontier.pop_back();
      for (const Mate* m : active)
        for (const auto& [from, to] :
             {std::pair{m->a.occurrenceId, m->b.occurrenceId},
              {m->b.occurrenceId, m->a.occurrenceId}})
          if (from == at && !reached.count(to) && !suppressed(to) &&
              sub.FindById(to) != nullptr) {
            reached.insert(to);
            frontier.push_back(to);
          }
    }
    for (uint64_t id : reached)
      if (!fixedSet.count(id))
        moving.insert(id);
    // A rider on a moving part moves with it (one level, as attachments do).
    for (const Occurrence& o : sub.GetOccurrences())
      if (o.attachedTo != 0 && !o.fixed && !suppressed(o.id) &&
          moving.count(o.attachedTo))
        moving.insert(o.id);

    const glm::dmat4 rootWorld = OccMatrix(F);
    for (const Occurrence& k : sub.GetOccurrences()) {
      if (!moving.count(k.id))
        continue;
      Occurrence o = k;
      o.id = next++;
      o.fixed = false;
      o.flexible = false;
      o.innerPoses.clear();
      o.innerMateDofs.clear();
      o.visible = true;
      Decompose(rootWorld * InnerPlacement(F, k), o.position, o.orientation);
      unit.moving.push_back(k.id);
      ex.occOf[o.id] = {F.id, k.id};
      ex.occIdOf[{F.id, k.id}] = o.id;
      doc.GetOccurrences().push_back(std::move(o));
    }
    for (uint64_t kid : unit.moving) {
      Occurrence* o = doc.FindById(ex.occIdOf[{F.id, kid}]);
      if (o->attachedTo != 0) {
        const auto it = ex.occIdOf.find({F.id, o->attachedTo});
        o->attachedTo = it != ex.occIdOf.end() ? it->second : 0;
        if (o->attachedTo == 0)
          o->attachSeat = {};
      }
    }
    // A connector on a moving occurrence is re-pointed at its expanded
    // twin; one on anything else lands on the root, with the path in —
    // which ConnectorLocalFrame composes at the unit's stored place.
    const auto remap = [&](const Connector& c) {
      Connector out = c;
      const auto it = ex.occIdOf.find({F.id, c.occurrenceId});
      if (it != ex.occIdOf.end()) {
        out.occurrenceId = it->second;
      } else {
        out.occurrenceId = F.id;
        out.innerPath.insert(out.innerPath.begin(), c.occurrenceId);
      }
      return out;
    };
    for (const Mate* m : active) {
      Mate cp = *m;
      cp.a = remap(m->a);
      cp.b = remap(m->b);
      if (cp.a.occurrenceId == F.id && cp.b.occurrenceId == F.id)
        continue; // both ends fold into the root: rigid inside the unit
      cp.id = next++;
      cp.error.clear();
      cp.residualMm = 0.0;
      if (const InnerMateDof* d = F.FindInnerMateDof(m->id))
        ApplyDof(cp, *d);
      ex.mateOf[cp.id] = {F.id, m->id};
      ex.mateIdOf[{F.id, m->id}] = cp.id;
      unit.mates.push_back(m->id);
      if (cfg != nullptr)
        if (const MateOverride* o = cfg->OverrideForMate(m->id)) {
          MateOverride h = *o;
          h.mate = cp.id;
          holds.push_back(h);
        }
      doc.GetMates().push_back(std::move(cp));
    }
    for (const Relation& r : sub.GetRelations()) {
      if (sub.RelationSuppressedIn(cfgId, r.id))
        continue;
      const auto a = ex.mateIdOf.find({F.id, r.mateA});
      const auto b = ex.mateIdOf.find({F.id, r.mateB});
      if (a == ex.mateIdOf.end() || b == ex.mateIdOf.end())
        continue; // a mate folded into the unit: nothing there moves
      Relation cp = r;
      cp.id = next++;
      cp.mateA = a->second;
      cp.mateB = b->second;
      if (cp.rideMate != 0) {
        const auto rm = ex.mateIdOf.find({F.id, cp.rideMate});
        cp.rideMate = rm != ex.mateIdOf.end() ? rm->second : 0;
      }
      cp.error.clear();
      doc.GetRelations().push_back(std::move(cp));
    }
    ex.any = true;
    ex.units.push_back(std::move(unit));
  }

  // The expanded roots are ordinary occurrences now: rigid to the solver,
  // their insides already added beside them.
  for (Occurrence& o : doc.GetOccurrences())
    if (o.flexible) {
      o.flexible = false;
      o.innerPoses.clear();
      o.innerMateDofs.clear();
    }

  // The parent's connectors into a moving inner part follow it.
  for (Mate& m : doc.GetMates())
    for (Connector* c : {&m.a, &m.b}) {
      if (c->innerPath.empty())
        continue;
      const auto it = ex.occIdOf.find({c->occurrenceId, c->innerPath.front()});
      if (it == ex.occIdOf.end())
        continue;
      c->occurrenceId = it->second;
      c->innerPath.erase(c->innerPath.begin());
    }
  // ...and its relations naming an inner mate by path name the twin.
  for (Relation& r : doc.GetRelations()) {
    for (auto [mate, path] : {std::pair{&r.mateA, &r.mateAPath},
                              std::pair{&r.mateB, &r.mateBPath}}) {
      if (path->empty())
        continue;
      const uint64_t id = ex.MateId(*path, *mate);
      if (id == 0) {
        const FlexibleUnit* u = ex.UnitFor(path->front());
        const Occurrence* o = parent.FindById(path->front());
        std::string why;
        if (o == nullptr)
          why = "the placement its path names is gone";
        else if (!o->flexible)
          why = "it names a mate inside '" + o->name +
                "', which is not flexible — set_occurrence_flexible to let "
                "its mechanism move here";
        else if (u != nullptr && !u->error.empty())
          why = "'" + o->name + "' stays rigid this solve: " + u->error;
        else if (path->size() != 1)
          why = "its path goes more than one level down, and only one level "
                "of sub-assembly is flexible";
        else
          why = "mate " + std::to_string(*mate) + " is not one of '" + o->name +
                "''s moving mates (gone, suppressed by its "
                "configuration, or between parts that do not move)";
        ex.relationErrors[r.id] = why;
      }
      *mate = id; // 0 when unknown: the solve reports it gone, then `why`
      path->clear();
    }
  }

  // The units' own configurations hold their mates: as holds of whatever
  // configuration the expanded parent runs under.
  if (!holds.empty()) {
    Configuration* active = nullptr;
    if (doc.ActiveConfiguration() != 0)
      active = doc.FindConfiguration(doc.ActiveConfiguration());
    if (active == nullptr) {
      Configuration c;
      c.id = next++;
      c.name = "(flexible sub-assembly holds)";
      doc.GetConfigurations().push_back(std::move(c));
      doc.SetActiveConfigurationRaw(doc.GetConfigurations().back().id);
      active = &doc.GetConfigurations().back();
    }
    for (const MateOverride& h : holds)
      active->mateOverrides.push_back(h);
  }
  return ex;
}

void CollapseFlexible(const FlexibleExpansion& ex, AssemblyDocument& parent,
                      SolveReport* report) {
  const AssemblyDocument& doc = ex.doc;
  std::unordered_map<uint64_t, const Occurrence*> occ;
  for (const Occurrence& o : doc.GetOccurrences())
    occ[o.id] = &o;
  std::unordered_map<uint64_t, const Mate*> mate;
  for (const Mate& m : doc.GetMates())
    mate[m.id] = &m;
  std::unordered_map<uint64_t, const Relation*> rel;
  for (const Relation& r : doc.GetRelations())
    rel[r.id] = &r;

  for (Occurrence& o : parent.GetOccurrences())
    if (const auto it = occ.find(o.id); it != occ.end()) {
      o.position = it->second->position;
      o.orientation = it->second->orientation;
    }
  for (Mate& m : parent.GetMates())
    if (const auto it = mate.find(m.id); it != mate.end()) {
      const Mate& e = *it->second;
      ApplyDof(m, DofOf(e));
      m.error = e.error;
      m.residualMm = e.residualMm;
    }
  for (Relation& r : parent.GetRelations())
    if (const auto it = rel.find(r.id); it != rel.end()) {
      r.error = it->second->error;
      r.lead = it->second->lead;
      r.rideApplied = it->second->rideApplied;
      if (const auto why = ex.relationErrors.find(r.id);
          why != ex.relationErrors.end())
        r.error = why->second;
    }

  std::set<uint64_t> redundant;
  if (report != nullptr)
    redundant.insert(report->redundantMates.begin(),
                     report->redundantMates.end());
  for (const FlexibleUnit& u : ex.units) {
    Occurrence* F = parent.FindById(u.root);
    if (F == nullptr)
      continue;
    if (!u.error.empty()) {
      if (report != nullptr) {
        report->innerMates.push_back({u.root, 0, F->name, u.error, 0.0, false});
        report->erroredMates++;
      }
      continue;
    }
    const glm::dmat4 inv = glm::inverse(OccMatrix(*F));
    F->innerPoses.clear();
    for (uint64_t k : u.moving) {
      const Occurrence* e = occ.at(ex.occIdOf.at({u.root, k}));
      InnerPose p;
      p.occurrence = k;
      Decompose(inv * OccMatrix(*e), p.position, p.orientation);
      F->innerPoses.push_back(p);
    }
    F->innerMateDofs.clear();
    for (uint64_t m : u.mates) {
      const uint64_t id = ex.mateIdOf.at({u.root, m});
      const Mate& e = *mate.at(id);
      if (e.type != MateType::Fastened) {
        InnerMateDof d = DofOf(e);
        d.mate = m;
        F->innerMateDofs.push_back(d);
      }
      if (report != nullptr && (!e.error.empty() || redundant.count(id)))
        report->innerMates.push_back({u.root, m, e.name, e.error, e.residualMm,
                                      e.error.empty() && redundant.count(id)});
    }
  }
  if (report != nullptr) {
    // Synthetic ids mean nothing to the caller: the inner rows above say it.
    std::erase_if(report->redundantMates,
                  [&](uint64_t id) { return ex.mateOf.count(id) != 0; });
    std::erase_if(report->diagnoses, [&](const LoopDiagnosis& d) {
      for (uint64_t id : d.conflicting)
        if (ex.mateOf.count(id))
          return true;
      for (const LoopTiltTrial& t : d.suspects)
        if (ex.mateOf.count(t.mate))
          return true;
      return false;
    });
  }
}

const Mate* MateAtPath(const AssemblyDocument& doc,
                       const std::vector<uint64_t>& path, uint64_t mate,
                       const AssemblyResolver& asmById, std::string* why) {
  const auto fail = [&](std::string w) -> const Mate* {
    if (why)
      *why = std::move(w);
    return nullptr;
  };
  if (path.empty()) {
    const Mate* m = doc.FindMateById(mate);
    return m ? m
             : fail("no mate with id " + std::to_string(mate) +
                    "; list_mates shows the ones that exist");
  }
  const Occurrence* F = doc.FindById(path.front());
  if (F == nullptr)
    return fail("the path names occurrence " + std::to_string(path.front()) +
                ", which is not in this assembly");
  if (path.size() != 1)
    return fail("the path goes more than one level down; only one level of "
                "sub-assembly is flexible, so a path is one occurrence: [" +
                std::to_string(path.front()) + "]");
  if (!F->flexible)
    return fail("'" + F->name +
                "' is not flexible, so its mates do not move here — "
                "set_occurrence_flexible first");
  const AssemblyDocument* sub = asmById && F->refKind == OccurrenceRef::Assembly
                                    ? asmById(F->partId)
                                    : nullptr;
  if (sub == nullptr)
    return fail("the sub-assembly '" + F->name + "' places is not loaded");
  const Mate* m = sub->FindMateById(mate);
  if (m == nullptr) {
    std::string have;
    for (const Mate& c : sub->GetMates())
      if (MateHasDof(c.type, MateDof::Rotation) ||
          MateHasDof(c.type, MateDof::Offset))
        have += (have.empty() ? "" : ", ") + std::to_string(c.id) + " (" +
                c.name + ")";
    return fail("no mate " + std::to_string(mate) + " inside '" + F->name +
                "'; its mates with a degree of freedom: " +
                (have.empty() ? std::string("none") : have));
  }
  return m;
}

std::optional<double> MateDofAt(const AssemblyDocument& doc,
                                const std::vector<uint64_t>& path,
                                uint64_t mate, MateDof dof,
                                const AssemblyResolver& asmById) {
  const Mate* m = MateAtPath(doc, path, mate, asmById);
  if (m == nullptr)
    return std::nullopt;
  if (!path.empty())
    if (const InnerMateDof* d =
            doc.FindById(path.front())->FindInnerMateDof(mate))
      return dof == MateDof::Rotation ? d->rotation : d->offset;
  return MateDofValue(*m, dof);
}

bool SetMateDofAt(AssemblyDocument& doc, const std::vector<uint64_t>& path,
                  uint64_t mate, MateDof dof, double value,
                  const AssemblyResolver& asmById) {
  if (path.empty()) {
    Mate* m = doc.FindMateById(mate);
    if (m == nullptr)
      return false;
    SetMateDof(*m, dof, value);
    return true;
  }
  const Mate* m = MateAtPath(doc, path, mate, asmById);
  if (m == nullptr)
    return false;
  Occurrence* F = doc.FindById(path.front());
  InnerMateDof* d = nullptr;
  for (InnerMateDof& cand : F->innerMateDofs)
    if (cand.mate == mate)
      d = &cand;
  if (d == nullptr) {
    F->innerMateDofs.push_back(DofOf(*m));
    d = &F->innerMateDofs.back();
  }
  if (dof == MateDof::Rotation)
    d->rotation = value;
  else
    d->offset = value;
  return true;
}

} // namespace assembly
