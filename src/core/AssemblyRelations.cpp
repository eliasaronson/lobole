// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Relations over several mates, and the checks a relation passes before it
// is made (docs/assemblies/relations.md#relations-over-several-mates).

#include "core/AssemblySolve.h"

#include <cmath>
#include <string>

namespace assembly {

namespace {
const char* DofWord(MateDof d) {
  return d == MateDof::Rotation ? "rotation" : "slide";
}

std::string MateWord(const Mate& m) {
  return "mate " + std::to_string(m.id) +
         (m.name.empty() ? std::string() : " (" + m.name + ")");
}
} // namespace

bool DeriveFromTeeth(Relation& r) {
  switch (r.kind) {
  case RelationKind::Gear:
  case RelationKind::Chain:
    if (r.teethA <= 0 || r.teethB <= 0)
      return false;
    // Meshing outside, the pair counter-rotates; a chain, a belt or a gear
    // meshing INSIDE a ring keeps the sense.
    r.ratio = static_cast<double>(r.teethA) / r.teethB *
              (r.kind == RelationKind::Chain || r.internal ? 1.0 : -1.0);
    return true;
  case RelationKind::Epicyclic: {
    if (r.terms.size() != 3 || r.terms[0].teeth <= 0 || r.terms[1].teeth <= 0)
      return false;
    // Willis: (sun - carrier) / (ring - carrier) = -zr/zs, so
    // zs*sun + zr*ring - (zs + zr)*carrier = 0.
    const double zs = r.terms[0].teeth, zr = r.terms[1].teeth;
    r.terms[0].coefficient = zs;
    r.terms[1].coefficient = zr;
    r.terms[2].coefficient = -(zs + zr);
    return true;
  }
  case RelationKind::Planet: {
    if (r.terms.size() != 3 || r.terms[0].teeth <= 0 || r.terms[2].teeth <= 0)
      return false;
    // On its carrier a planet meshes with the sun as an external pair:
    // planet_rel = -(zs/zp) * (sun - carrier).
    const double zs = r.terms[0].teeth, zp = r.terms[2].teeth;
    r.terms[0].coefficient = zs / zp;
    r.terms[1].coefficient = -zs / zp;
    r.terms[2].coefficient = 1.0;
    return true;
  }
  default:
    return false;
  }
}

Relation MakeEpicyclic(RelationTerm sun, RelationTerm ring,
                       RelationTerm carrier, int teethSun, int teethRing) {
  Relation r;
  r.kind = RelationKind::Epicyclic;
  r.mateA = r.mateB = 0;
  sun.teeth = teethSun;
  ring.teeth = teethRing;
  carrier.teeth = 0;
  for (RelationTerm* t : {&sun, &ring, &carrier})
    t->dof = MateDof::Rotation;
  r.terms = {std::move(sun), std::move(ring), std::move(carrier)};
  r.ratio = 1.0; // unused; never zero
  (void)DeriveFromTeeth(r);
  return r;
}

Relation MakePlanet(RelationTerm planet, RelationTerm sun, RelationTerm carrier,
                    int teethPlanet, int teethSun) {
  Relation r;
  r.kind = RelationKind::Planet;
  r.mateA = r.mateB = 0;
  planet.teeth = teethPlanet;
  sun.teeth = teethSun;
  carrier.teeth = 0;
  for (RelationTerm* t : {&sun, &carrier, &planet})
    t->dof = MateDof::Rotation;
  // The planet LAST: with the sun driven and nothing else, the walk holds
  // the carrier (the first open member) and solves the planet's spin.
  r.terms = {std::move(sun), std::move(carrier), std::move(planet)};
  r.ratio = 1.0;
  (void)DeriveFromTeeth(r);
  return r;
}

bool RelationTermHasTeeth(const Relation& r, size_t index) {
  if (r.kind == RelationKind::Epicyclic)
    return index == 0 || index == 1; // sun, ring
  if (r.kind == RelationKind::Planet)
    return index == 0 || index == 2; // sun, planet
  return false;
}

std::string RelationTermRole(const Relation& r, size_t index) {
  static const char* kEpicyclic[] = {"sun", "ring", "carrier"};
  static const char* kPlanet[] = {"sun", "carrier", "planet"};
  if (index < 3 && r.kind == RelationKind::Epicyclic)
    return kEpicyclic[index];
  if (index < 3 && r.kind == RelationKind::Planet)
    return kPlanet[index];
  return "term " + std::to_string(index + 1);
}

std::string RelationProblem(const AssemblyDocument& doc, const Relation& r,
                            const AssemblyResolver& asmById) {
  struct Side {
    std::string role;
    uint64_t mate;
    const std::vector<uint64_t>* path;
    MateDof dof;
  };
  std::vector<Side> sides;
  if (r.HasTerms()) {
    const bool fixed3 = r.kind != RelationKind::LinearN;
    if (fixed3 && r.terms.size() != 3)
      return std::string("an ") +
             (r.kind == RelationKind::Epicyclic
                  ? "epicyclic relation has three members: sun, ring and "
                    "carrier"
                  : "planet relation has three members: sun, carrier and "
                    "planet");
    if (r.terms.size() < 2)
      return "a linear_n relation needs at least two terms "
             "[{mate, dof, coefficient}, ...]";
    for (size_t i = 0; i < r.terms.size(); i++) {
      const RelationTerm& t = r.terms[i];
      const std::string role = RelationTermRole(r, i);
      if (t.mate == 0)
        return "no " + role + " picked — " +
               (r.kind == RelationKind::Epicyclic
                    ? "an epicyclic relation needs a sun, a ring and a "
                      "carrier, each a mate that turns"
                : r.kind == RelationKind::Planet
                    ? "a planet relation needs a planet, a sun and a carrier"
                    : "every term names a mate");
      if (!std::isfinite(t.coefficient) || t.coefficient == 0.0)
        return role + "'s coefficient is zero; a term that counts for "
                      "nothing is not a member — leave it out";
      if (fixed3 && RelationTermHasTeeth(r, i) && t.teeth <= 0)
        return "the " + role + "'s tooth count must be positive";
      sides.push_back({role, t.mate, &t.path, t.dof});
    }
  } else {
    sides.push_back({"a", r.mateA, &r.mateAPath, r.dofA});
    if (r.kind != RelationKind::Click) // a click is ONE mate's DOF
      sides.push_back({"b", r.mateB, &r.mateBPath, r.dofB});
    if ((r.kind == RelationKind::Gear || r.kind == RelationKind::Chain) &&
        (r.teethA > 0 || r.teethB > 0) && (r.teethA <= 0 || r.teethB <= 0))
      return "a gear ratio from teeth needs both tooth counts (positive)";
    if (r.internal && r.kind != RelationKind::Gear)
      return "internal is a gear's: a gear meshing inside a ring";
  }
  for (size_t i = 0; i < sides.size(); i++) {
    const Side& s = sides[i];
    std::string why;
    const Mate* m = MateAtPath(doc, *s.path, s.mate, asmById, &why);
    if (m == nullptr)
      return s.role + ": " + why;
    if (!MateHasDof(m->type, s.dof))
      return MateWord(*m) + " has no " + DofWord(s.dof) + " for the " + s.role +
             " — pick a mate that " +
             (s.dof == MateDof::Rotation ? "turns (revolute or cylindrical)"
                                         : "slides (slider or cylindrical)");
    for (size_t k = 0; k < i; k++)
      if (sides[k].mate == s.mate && *sides[k].path == *s.path &&
          sides[k].dof == s.dof)
        return MateWord(*m) + " is both the " + sides[k].role + " and the " +
               s.role + " — each member is a mate of its own";
  }
  return {};
}

} // namespace assembly
