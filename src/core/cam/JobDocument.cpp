// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/JobDocument.h"
#include "core/cam/CamOpFields.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace cam {

uint64_t JobDocument::AddPart(uint64_t sourceDocId, const std::string& name) {
  JobPart part;
  part.id = m_nextId++;
  part.sourceDocId = sourceDocId;
  part.name = name;
  m_parts.push_back(std::move(part));
  return m_parts.back().id;
}

bool JobDocument::RemovePart(uint64_t id) {
  bool found = false;
  for (auto it = m_parts.begin(); it != m_parts.end(); ++it) {
    if (it->id == id) {
      m_parts.erase(it);
      found = true;
      break;
    }
  }
  if (!found)
    return false;
  // Everything that named it goes too. A placement for a part the job no
  // longer holds puts a ghost in the stock, and an operation target pointing
  // at one is the dangling reference the id exists to prevent -- it would
  // resolve against whatever body happened to answer to the same element
  // name, which is the wrong bracket, cut for real.
  for (Setup& setup : m_setups) {
    std::erase_if(setup.placements,
                  [id](const Placement& p) { return p.jobPartId == id; });
    for (Operation& op : setup.operations)
      std::erase_if(op.targets,
                    [id](const Target& t) { return t.jobPartId == id; });
    if (setup.topFace.jobPartId == id)
      setup.topFace = Target{};
  }
  return true;
}

JobPart* JobDocument::FindPart(uint64_t id) {
  for (JobPart& p : m_parts)
    if (p.id == id)
      return &p;
  return nullptr;
}

const JobPart* JobDocument::FindPart(uint64_t id) const {
  for (const JobPart& p : m_parts)
    if (p.id == id)
      return &p;
  return nullptr;
}

Setup& JobDocument::AddSetup(const std::string& name) {
  Setup setup;
  setup.id = m_nextId++;
  setup.name =
      name.empty() ? ("Setup " + std::to_string(m_setups.size() + 1)) : name;
  // One tool by default, because a setup with no tools can produce nothing
  // and the first thing anyone does is add a 6 mm end mill.
  Tool tool;
  tool.number = 1;
  tool.name = "6 mm flat";
  setup.tools.push_back(tool);
  m_setups.push_back(std::move(setup));
  return m_setups.back();
}

bool JobDocument::RemoveSetup(uint64_t id) {
  for (auto it = m_setups.begin(); it != m_setups.end(); ++it) {
    if (it->id == id) {
      m_setups.erase(it);
      return true;
    }
  }
  return false;
}

Setup* JobDocument::FindSetup(uint64_t id) {
  for (Setup& s : m_setups)
    if (s.id == id)
      return &s;
  return nullptr;
}

const Setup* JobDocument::FindSetup(uint64_t id) const {
  for (const Setup& s : m_setups)
    if (s.id == id)
      return &s;
  return nullptr;
}

// ---- Numbers that are expressions -----------------------------------------
// (docs/cam/README.md#numbers-that-are-expressions)

namespace {

std::string Num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

std::string Quoted(const std::string& name, uint64_t id) {
  return name.empty() ? std::to_string(id) : "'" + name + "'";
}

void Vec2(const std::function<void(const std::string&, const ExprSlot&)>& visit,
          const std::string& owner, glm::dvec2& v, Vec2Exprs& texts,
          ExprQuantity q, const std::string& path) {
  for (int k = 0; k < 2; k++)
    visit(owner, ExprSlot(texts[static_cast<size_t>(k)], v[k], q,
                          path + "/" + std::to_string(k)));
}

void Vec3(const std::function<void(const std::string&, const ExprSlot&)>& visit,
          const std::string& owner, glm::dvec3& v, Vec3Exprs& texts,
          ExprQuantity q, const std::string& path) {
  VisitVec3Exprs([&](const ExprSlot& s) { visit(owner, s); }, v, texts, q,
                 path);
}

// A tool's numbers, at their keys in ToolToJson: the tip angle only on a
// tool that has one, the thread mill's head only on a thread mill.
void VisitTool(Tool& t, const ExprVisitor& visit) {
  visit(ExprSlot(t.diameter, ExprQuantity::Length, "diameter"));
  visit(ExprSlot(t.flutes, "flutes"));
  visit(ExprSlot(t.fluteLength, ExprQuantity::Length, "flute_length"));
  visit(ExprSlot(t.feedCut, ExprQuantity::Other, "feed_cut"));
  visit(ExprSlot(t.feedPlunge, ExprQuantity::Other, "feed_plunge"));
  visit(ExprSlot(t.spindleRpm, ExprQuantity::Other, "spindle_rpm"));
  if (t.type != ToolType::Flat)
    visit(ExprSlot(t.tipAngleDeg, ExprQuantity::Angle, "tip_angle"));
  visit(ExprSlot(t.chiploadMin, ExprQuantity::Length, "chipload_min"));
  visit(ExprSlot(t.chiploadMax, ExprQuantity::Length, "chipload_max"));
  visit(ExprSlot(t.overallLength, ExprQuantity::Length, "overall_length"));
  visit(ExprSlot(t.collarDiameter, ExprQuantity::Length, "collar_diameter"));
  visit(ExprSlot(t.shankDiameter, ExprQuantity::Length, "shank_diameter"));
  if (t.type == ToolType::ThreadMill) {
    visit(ExprSlot(t.tipFlat, ExprQuantity::Length, "tip_flat"));
    visit(ExprSlot(t.neckDiameter, ExprQuantity::Length, "neck_diameter"));
    visit(ExprSlot(t.neckLength, ExprQuantity::Length, "neck_length"));
  }
}

} // namespace

void VisitSetupExprs(Setup& s,
                     const std::function<void(const std::string& owner,
                                              const ExprSlot&)>& visit) {
  {
    const std::string in = " in setup " + Quoted(s.name, s.id);
    const std::string owner = "setup " + Quoted(s.name, s.id);
    const std::string at;
    const auto one = [&](const std::string& who, const std::string& path) {
      return [&visit, who, path](const ExprSlot& slot) {
        visit(who, slot.WithPath(path + slot.Path()));
      };
    };
    visit(owner, ExprSlot(s.safeZ, ExprQuantity::Length, at + "safe_z"));
    visit(owner, ExprSlot(s.rapidFeed, ExprQuantity::Other, at + "rapid_feed"));
    visit(owner, ExprSlot(s.rotationA, ExprQuantity::Angle, at + "rotation_a"));
    visit(owner, ExprSlot(s.gripLength, ExprQuantity::Length, at + "grip_mm"));
    Vec2(visit, owner, s.reachOffset, s.reachOffsetExpr, ExprQuantity::Length,
         at + "reach_offset");
    Vec3(visit, owner, s.stock.margin, s.stock.marginExpr, ExprQuantity::Length,
         at + "stock/margin");
    visit(owner, ExprSlot(s.stock.bottomMargin, ExprQuantity::Length,
                          at + "stock/bottom_margin"));
    if (s.stock.mode == StockMode::Fixed) {
      Vec3(visit, owner, s.stock.size, s.stock.sizeExpr, ExprQuantity::Length,
           at + "stock/size");
      // Where the part sits in the stock: fractions of the room around it.
      Vec3(visit, owner, s.stock.place, s.stock.placeExpr, ExprQuantity::Other,
           at + "stock/place");
    }
    for (size_t k = 0; k < s.tools.size(); k++)
      VisitTool(s.tools[k],
                one("tool T" + std::to_string(s.tools[k].number) + in,
                    at + "tools/" + std::to_string(k) + "/"));
    for (size_t k = 0; k < s.placements.size(); k++) {
      Placement& p = s.placements[k];
      const std::string who = "placement " + std::to_string(p.id) + in;
      const std::string pat = at + "placements/" + std::to_string(k) + "/";
      Vec2(visit, who, p.pos, p.posExpr, ExprQuantity::Length, pat + "pos");
      visit(who, ExprSlot(p.rotZdeg, ExprQuantity::Angle, pat + "rot_z"));
    }
    for (size_t k = 0; k < s.fixtures.size(); k++) {
      Fixture& f = s.fixtures[k];
      const std::string who = "fixture " + Quoted(f.name, f.id) + in;
      const std::string fat = at + "fixtures/" + std::to_string(k) + "/";
      if (f.IsBox())
        Vec3(visit, who, f.box, f.boxExpr, ExprQuantity::Length, fat + "box");
      if (f.seat) {
        // Seated, its pose is solved from the seat: the seat's numbers are
        // the ones that place it.
        visit(who, ExprSlot(f.seat->alongMm, ExprQuantity::Length,
                            fat + "seat/along_mm"));
        visit(who, ExprSlot(f.seat->overMm, ExprQuantity::Length,
                            fat + "seat/over_mm"));
        visit(who, ExprSlot(f.seat->spinDeg, ExprQuantity::Angle,
                            fat + "seat/spin_deg"));
      } else {
        Vec3(visit, who, f.pos, f.posExpr, ExprQuantity::Length, fat + "pos");
        visit(who, ExprSlot(f.rotZdeg, ExprQuantity::Angle, fat + "rot_z"));
      }
    }
    for (size_t k = 0; k < s.operations.size(); k++) {
      Operation& op = s.operations[k];
      VisitOpExprs(op, one("operation " + Quoted(op.name, op.id) + in,
                           at + "operations/" + std::to_string(k) + "/"));
    }
  }
}

void JobDocument::VisitExprs(
    const std::function<void(const std::string& owner, const ExprSlot&)>&
        visit) {
  for (size_t i = 0; i < m_setups.size(); i++) {
    const std::string at = "cam/" + std::to_string(i) + "/";
    VisitSetupExprs(m_setups[i],
                    [&](const std::string& owner, const ExprSlot& slot) {
                      visit(owner, slot.WithPath(at + slot.Path()));
                    });
  }
}

namespace {

// What a number its owner holds must be, once evaluated; empty when it
// stands. The RPC refuses the same values where it takes them.
std::string Refusal(const ExprSlot& slot) {
  const std::string& path = slot.Path();
  const auto leaf = path.substr(path.rfind('/') + 1);
  const double v = slot.Value();
  if (leaf == "diameter" && !(v > 0.0))
    return "a tool's diameter must be above 0";
  if (leaf == "flutes" && v < 1.0)
    return "a tool has at least one flute";
  if (leaf == "count" && v < 0.0)
    return "a tab count is not below 0";
  if ((leaf == "stepdown" || leaf == "safe_z") && v < 0.0)
    return leaf + " is not below 0";
  return {};
}

} // namespace

bool EvaluateJobExprs(JobDocument& doc, const SymbolTable& symbols) {
  std::vector<double> before;
  doc.VisitExprs([&](const std::string&, const ExprSlot& s) {
    before.push_back(s.Value());
  });
  std::vector<JobExprProblem> problems;
  const auto note = [&](const std::string& owner, std::string why) {
    for (const JobExprProblem& p : problems)
      if (p.owner == owner)
        return;
    problems.push_back({owner, std::move(why)});
  };
  doc.VisitExprs([&](const std::string& owner, const ExprSlot& slot) {
    if (!slot.Bound())
      return;
    std::string why;
    if (!EvaluateSlot(slot, symbols, &why))
      note(owner, slot.Path() + "_expr '" + slot.Expr() + "': " + why);
    else if (std::string r = Refusal(slot); !r.empty())
      note(owner, slot.Path() + "_expr '" + slot.Expr() + "' comes out " +
                      Num(slot.Value()) + ": " + r);
  });
  // An owner that cannot stand keeps every value it had.
  size_t i = 0;
  bool moved = false;
  doc.VisitExprs([&](const std::string& owner, const ExprSlot& slot) {
    const double was = before[i++];
    for (const JobExprProblem& p : problems)
      if (p.owner == owner) {
        (void)slot.Set(was);
        return;
      }
    moved = moved || slot.Value() != was;
  });
  const bool problemsChanged = [&] {
    const auto& old = doc.ExprProblems();
    if (old.size() != problems.size())
      return true;
    for (size_t k = 0; k < old.size(); k++)
      if (old[k].owner != problems[k].owner ||
          old[k].error != problems[k].error)
        return true;
    return false;
  }();
  doc.SetExprProblems(std::move(problems));
  return moved || problemsChanged;
}

} // namespace cam
