// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// An assembly's numbers that are expressions of the project's variables
// (docs/assemblies/README.md#numbers-that-are-expressions): the one walk
// over them, and their evaluation, which leaves a value that cannot stand
// where it was and says why.

#include "core/AssemblySolve.h"
#include "core/Occurrence.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

std::string Label(const char* kind, uint64_t id, const std::string& name) {
  return std::string(kind) + " " +
         (name.empty() ? std::to_string(id) : "'" + name + "'");
}

std::string At(const char* list, size_t i) {
  return std::string(list) + "/" + std::to_string(i) + "/";
}

// A coordinate of a vector the geometry reads whole: its value and its text.
void VisitVec3(
    const std::function<void(const std::string&, const ExprSlot&)>& visit,
    const std::string& owner, glm::dvec3& v, std::array<std::string, 3>& texts,
    const std::string& path) {
  for (int k = 0; k < 3; k++)
    visit(owner, ExprSlot(texts[k], v[k], ExprQuantity::Length,
                          path + "/" + std::to_string(k)));
}

// A range's ends: an angle's held in radians, its text in degrees.
void VisitRange(
    const std::function<void(const std::string&, const ExprSlot&)>& visit,
    const std::string& owner, std::optional<MateRange>& r, bool angle,
    const std::string& path) {
  if (!r)
    return;
  if (angle) {
    visit(owner, ExprSlot::Radians(r->min.expr, r->min.value, path + "/0"));
    visit(owner, ExprSlot::Radians(r->max.expr, r->max.value, path + "/1"));
  } else {
    visit(owner, ExprSlot(r->min, ExprQuantity::Length, path + "/0"));
    visit(owner, ExprSlot(r->max, ExprQuantity::Length, path + "/1"));
  }
}

// A value in a mate DOF's unit: radians held (a degrees text) for a turn.
ExprSlot DofSlot(ExprValue& v, MateDof dof, const std::string& path) {
  return dof == MateDof::Rotation ? ExprSlot::Radians(v.expr, v.value, path)
                                  : ExprSlot(v, ExprQuantity::Length, path);
}

} // namespace

namespace assembly {

void VisitRelationExprs(Relation& r, const ExprVisitor& visit) {
  if (r.HasTerms()) {
    // The constant of sum(coefficient * dof): radians, as the RPC has it.
    visit(ExprSlot::Radians(r.phase.expr, r.phase.value, "constant"));
    // An epicyclic's and a planet's coefficients come from their teeth.
    if (r.kind == RelationKind::LinearN)
      for (size_t k = 0; k < r.terms.size(); k++)
        visit(ExprSlot(r.terms[k].coefficient, ExprQuantity::Other,
                       "terms/" + std::to_string(k) + "/coefficient"));
    return;
  }
  // A ratio made from teeth, a thread or a worm's hand is theirs; a cam, a
  // contact and an intermittent relation have none.
  const bool ratio =
      r.teethA == 0 && r.threadPart == 0 && r.kind != RelationKind::Cam &&
      r.kind != RelationKind::Contact && r.kind != RelationKind::Intermittent &&
      r.kind != RelationKind::Worm;
  if (ratio)
    visit(ExprSlot(r.ratio, ExprQuantity::Other, "ratio"));
  if (r.kind != RelationKind::Contact)
    visit(DofSlot(r.phase, r.dofB, "phase"));
  if (r.liftExpr.size() != r.lift.size())
    r.liftExpr.resize(r.lift.size());
  for (size_t k = 0; k < r.lift.size(); k++) {
    const std::string at = "lift/" + std::to_string(k);
    visit(ExprSlot(r.liftExpr[k][0], r.lift[k].first, ExprQuantity::Angle,
                   at + "/0"));
    // In the follower's unit: mm for a slide, DEGREES for a turn.
    visit(ExprSlot(r.liftExpr[k][1], r.lift[k].second,
                   r.dofB == MateDof::Rotation ? ExprQuantity::Angle
                                               : ExprQuantity::Length,
                   at + "/1"));
  }
  if (r.kind == RelationKind::Intermittent) {
    Relation::Intermittent& im = r.intermittent;
    // A Geneva's step, window and period are its slots'.
    if (im.slots == 0) {
      visit(ExprSlot(im.step,
                     r.dofB == MateDof::Rotation ? ExprQuantity::Angle
                                                 : ExprQuantity::Length,
                     "step"));
      visit(ExprSlot(im.periodDeg, ExprQuantity::Angle, "period_deg"));
      visit(ExprSlot(im.durationDeg, ExprQuantity::Angle, "duration_deg"));
    }
    visit(ExprSlot(im.startDeg, ExprQuantity::Angle, "start_deg"));
  }
}

std::string EvaluateRelationExprs(Relation& r, const SymbolTable& symbols) {
  std::string first;
  VisitRelationExprs(r, [&](const ExprSlot& slot) {
    if (std::string why = EvaluateOne(slot, symbols);
        !why.empty() && first.empty())
      first = slot.Path() + "_expr '" + slot.Expr() + "': " + why;
  });
  return first;
}

} // namespace assembly

void AssemblyDocument::VisitExprs(
    const std::function<void(const std::string& owner, const ExprSlot&)>&
        visit) {
  for (size_t i = 0; i < m_occurrences.size(); i++) {
    Occurrence& o = m_occurrences[i];
    const std::string owner = Label("occurrence", o.id, o.name);
    const std::string at = At("occurrences", i);
    VisitVec3(visit, owner, o.position, o.positionExpr,
              at + "transform/position");
    for (size_t k = 0; k < o.innerMateDofs.size(); k++) {
      InnerMateDof& d = o.innerMateDofs[k];
      const std::string dat = at + "inner_mate_dofs/" + std::to_string(k);
      visit(owner, ExprSlot(d.rotation, dat + "/rotation"));
      visit(owner, ExprSlot(d.offset, ExprQuantity::Length, dat + "/offset"));
    }
  }
  for (size_t i = 0; i < m_mates.size(); i++) {
    Mate& m = m_mates[i];
    const std::string owner = Label("mate", m.id, m.name);
    const std::string at = At("mates", i);
    visit(owner, ExprSlot(m.rotation, at + "rotation"));
    visit(owner, ExprSlot(m.offset, ExprQuantity::Length, at + "offset"));
    visit(owner, ExprSlot(m.dx, ExprQuantity::Length, at + "dx"));
    visit(owner, ExprSlot(m.dy, ExprQuantity::Length, at + "dy"));
    VisitRange(visit, owner, m.rotationRange, true, at + "limits/rotation");
    VisitRange(visit, owner, m.offsetRange, false, at + "limits/offset");
    VisitRange(visit, owner, m.dxRange, false, at + "limits/dx");
    VisitRange(visit, owner, m.dyRange, false, at + "limits/dy");
    if (m.coneRad)
      visit(owner, ExprSlot(*m.coneRad, at + "limits/cone"));
  }
  for (size_t i = 0; i < m_explodedViews.size(); i++) {
    ExplodedView& v = m_explodedViews[i];
    const std::string owner = Label("exploded view", v.id, v.name);
    for (size_t k = 0; k < v.steps.size(); k++)
      VisitVec3(visit, owner, v.steps[k].offset, v.steps[k].offsetExpr,
                At("exploded", i) + "steps/" + std::to_string(k) + "/offset");
  }
  for (size_t i = 0; i < m_relations.size(); i++) {
    Relation& r = m_relations[i];
    const std::string owner = Label("relation", r.id, r.name);
    assembly::VisitRelationExprs(r, [&](const ExprSlot& slot) {
      visit(owner, slot.WithPath(At("relations", i) + slot.Path()));
    });
  }
  for (size_t i = 0; i < m_motionStudies.size(); i++) {
    MotionStudy& s = m_motionStudies[i];
    const std::string owner = Label("motion study", s.id, s.name);
    const std::string at = At("motion", i);
    visit(owner, DofSlot(s.from, s.driverDof, at + "from"));
    visit(owner, DofSlot(s.to, s.driverDof, at + "to"));
    visit(owner, ExprSlot(s.samples, at + "samples"));
    visit(owner, ExprSlot(s.clashDeflectionMm, ExprQuantity::Length,
                          at + "clash_deflection_mm"));
  }
  if (!m_sequence.Empty()) {
    visit("the assembly sequence",
          ExprSlot(m_sequence.allowanceMm, ExprQuantity::Length,
                   "sequence/allowance_mm"));
    for (size_t k = 0; k < m_sequence.steps.size(); k++) {
      SequenceStep& st = m_sequence.steps[k];
      const std::string owner = "sequence step " + std::to_string(k + 1);
      const std::string at = "sequence/steps/" + std::to_string(k) + "/";
      for (size_t q = 0; q < st.pose.size(); q++) {
        SequencePose& p = st.pose[q];
        const bool rot = p.dof == MateDof::Rotation;
        visit(owner, ExprSlot(p.value,
                              rot ? ExprQuantity::Angle : ExprQuantity::Length,
                              at + "pose/" + std::to_string(q) +
                                  (rot ? "/rotation_deg" : "/offset_mm")));
      }
      if (!st.IsPose()) {
        visit(owner, ExprSlot(st.distanceMm, ExprQuantity::Length,
                              at + "distance_mm"));
        visit(owner,
              ExprSlot(st.turnDeg, ExprQuantity::Angle, at + "turn_deg"));
      }
    }
  }
  for (size_t i = 0; i < m_configurations.size(); i++) {
    Configuration& c = m_configurations[i];
    const std::string owner = Label("configuration", c.id, c.name);
    for (size_t k = 0; k < c.mateOverrides.size(); k++) {
      MateOverride& o = c.mateOverrides[k];
      const std::string at =
          At("configurations", i) + "mate_overrides/" + std::to_string(k) + "/";
      if (o.rotation)
        visit(owner, ExprSlot(*o.rotation, at + "rotation"));
      if (o.offset)
        visit(owner, ExprSlot(*o.offset, ExprQuantity::Length, at + "offset"));
    }
  }
  for (size_t i = 0; i < m_studies.size(); i++) {
    fem::Study& s = m_studies[i];
    const std::string owner = "study '" + s.name + "'";
    const std::string at = At("studies", i);
    s.VisitExprs([&](const ExprSlot& slot) {
      // The study's own path, under its place in the assembly.
      visit(owner, slot.WithPath(at + slot.Path()));
    });
  }
}

namespace assembly {

std::string EvaluateOne(const ExprSlot& slot, const SymbolTable& symbols) {
  std::string why;
  if (EvaluateSlot(slot, symbols, &why))
    return {};
  return why;
}

namespace {

// Every value a slot holds, in order: what tells an evaluation that moved
// something from one that found everything where it was.
std::vector<double> Values(AssemblyDocument& doc) {
  std::vector<double> out;
  doc.VisitExprs([&](const std::string&, const ExprSlot& s) {
    if (s.Bound())
      out.push_back(s.Value());
  });
  return out;
}

// The numbers of a mate an evaluation may change, to put back.
struct MateNumbers {
  ExprAngle rotation;
  ExprValue offset, dx, dy;
  std::optional<MateRange> rotationRange, offsetRange, dxRange, dyRange;
  std::optional<ExprAngle> coneRad;
};

MateNumbers Take(const Mate& m) {
  return {m.rotation,    m.offset,  m.dx,      m.dy,     m.rotationRange,
          m.offsetRange, m.dxRange, m.dyRange, m.coneRad};
}

// Puts back the values, keeping the texts the evaluation was given: the
// next evaluation tries them again.
void PutBackValues(Mate& m, const MateNumbers& n) {
  const auto back = [](auto& now, const auto& was) { now.value = was.value; };
  back(m.rotation, n.rotation);
  back(m.offset, n.offset);
  back(m.dx, n.dx);
  back(m.dy, n.dy);
  const auto range = [&](std::optional<MateRange>& now,
                         const std::optional<MateRange>& was) {
    if (now && was) {
      now->min.value = was->min.value;
      now->max.value = was->max.value;
    }
  };
  range(m.rotationRange, n.rotationRange);
  range(m.offsetRange, n.offsetRange);
  range(m.dxRange, n.dxRange);
  range(m.dyRange, n.dyRange);
  if (m.coneRad && n.coneRad)
    m.coneRad->value = n.coneRad->value;
}

} // namespace

bool EvaluateExprs(AssemblyDocument& doc, const SymbolTable& symbols) {
  const std::vector<double> before = Values(doc);
  std::vector<MateNumbers> mates;
  for (const Mate& m : doc.GetMates())
    mates.push_back(Take(m));
  std::vector<MotionStudy> studies = doc.GetMotionStudies();

  // First failure per owner, keyed by the owner's label.
  std::vector<AssemblyExprProblem> problems;
  const auto note = [&](const std::string& owner, std::string why) {
    for (const AssemblyExprProblem& p : problems)
      if (p.owner == owner)
        return;
    problems.push_back({owner, 0, std::move(why)});
  };
  doc.VisitExprs([&](const std::string& owner, const ExprSlot& slot) {
    if (std::string why = EvaluateOne(slot, symbols); !why.empty())
      note(owner, slot.Path() + ": " + why);
  });

  // What the values came out as must stand, or the owner keeps what it had.
  for (size_t i = 0; i < doc.GetMates().size(); i++) {
    Mate& m = doc.GetMates()[i];
    const std::string owner = Label("mate", m.id, m.name);
    std::string why;
    for (const AssemblyExprProblem& p : problems)
      if (p.owner == owner)
        why = p.error;
    if (why.empty())
      why = MateRangeProblem(m);
    if (why.empty())
      why = MateOutsideRange(m);
    if (!why.empty()) {
      PutBackValues(m, mates[i]);
      note(owner, why);
      for (AssemblyExprProblem& p : problems)
        if (p.owner == owner)
          p.id = m.id;
    }
    m.exprError = why;
  }
  for (size_t i = 0; i < doc.GetMotionStudies().size(); i++) {
    MotionStudy& s = doc.GetMotionStudies()[i];
    const std::string owner = Label("motion study", s.id, s.name);
    std::string why;
    if (s.samples < 2)
      why = "samples: a motion study needs at least 2, and this one comes "
            "out " +
            std::to_string(s.samples.value);
    else if (s.clashDeflectionMm.Bound() || s.clashDeflectionMm > 0.0)
      why = MotionStudyDeflectionProblem(s);
    if (!why.empty()) {
      const MotionStudy& was = studies[i];
      s.from.value = was.from.value;
      s.to.value = was.to.value;
      s.samples.value = was.samples.value;
      s.clashDeflectionMm.value = was.clashDeflectionMm.value;
      note(owner, why);
    }
  }
  for (AssemblyExprProblem& p : problems)
    if (p.id == 0)
      for (const MotionStudy& s : doc.GetMotionStudies())
        if (p.owner == Label("motion study", s.id, s.name))
          p.id = s.id;

  const bool problemsChanged = [&] {
    const auto& old = doc.ExprProblems();
    if (old.size() != problems.size())
      return true;
    for (size_t i = 0; i < old.size(); i++)
      if (old[i].owner != problems[i].owner ||
          old[i].error != problems[i].error)
        return true;
    return false;
  }();
  doc.SetExprProblems(std::move(problems));
  return problemsChanged || Values(doc) != before;
}

} // namespace assembly
