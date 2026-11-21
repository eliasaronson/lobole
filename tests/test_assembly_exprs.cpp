// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// An assembly's numbers that are expressions of the project's variables
// (docs/assemblies/README.md#numbers-that-are-expressions): a mate bound to
// a variable turns when it changes, a limit narrowed past the mate is the
// mate's error and keeps its last good value, a lone assembly refuses a
// variable it cannot see, and rename and delete reach every one. The probe
// at the end is the plan's "nothing new arrives unbindable" for the
// assembly document.

#include "TestIO.h"
#include "core/Angles.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"
#include "ui/AppActionsInternal.h" // SlotDoc
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/DocumentSlots.h"
#include "ui/VariableActions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

double Rad(double deg) { return angles::ToRadians(deg); }

Connector Origin(uint64_t occ) {
  Connector c;
  c.occurrenceId = occ;
  c.anchor.origin = true;
  return c;
}

// A project with one part, and an assembly placing it twice: a fixed base
// and a lid on a revolute hinge.
struct Rig {
  ui::AppState state;
  uint64_t base = 0, lid = 0, hinge = 0;
  size_t asmSlot = 0;
};

void MakeRig(Rig& r) {
  r.state.document = std::make_unique<Document>();
  r.state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(r.state);
  const uint64_t part = r.state.parts.at(0).id;
  r.asmSlot = ui::AddAssembly(r.state, "Box");
  AssemblyDocument& a = *r.state.assembly;
  Occurrence b;
  b.name = "Base";
  b.partId = part;
  b.fixed = true;
  r.base = a.AddOccurrence(b);
  Occurrence l;
  l.name = "Lid";
  l.partId = part;
  r.lid = a.AddOccurrence(l);
  Mate h;
  h.name = "Hinge";
  h.type = MateType::Revolute;
  h.a = Origin(r.base);
  h.b = Origin(r.lid);
  h.flipped = false;
  r.hinge = a.AddMate(h);
  ui::SolveActiveAssembly(r.state);
}

// Where the lid's X axis points: what turning the hinge moves.
glm::dvec3 LidX(Rig& r) {
  const Occurrence* o = r.state.assembly->FindById(r.lid);
  return glm::normalize(o->orientation) * glm::dvec3(1, 0, 0);
}

} // namespace

TEST_CASE("A mate bound to a variable turns when the variable changes",
          "[variables][assembly]") {
  Rig r;
  MakeRig(r);
  REQUIRE(ui::SetProjectVariable(r.state, "lid_open", "30", ""));
  Mate* hinge = r.state.assembly->FindMateById(r.hinge);
  hinge->rotation.expr = "lid_open";
  // A variable change re-evaluates every assembly and re-solves it.
  REQUIRE(ui::SetProjectVariable(r.state, "lid_open", "90", ""));
  hinge = r.state.assembly->FindMateById(r.hinge);
  CHECK_THAT(hinge->rotation.value, WithinAbs(Rad(90.0), 1e-9));
  const glm::dvec3 x90 = LidX(r);
  CHECK_THAT(x90.y, WithinAbs(1.0, 1e-6)); // turned a quarter about Z

  REQUIRE(ui::SetProjectVariable(r.state, "lid_open", "lid_open * 0 + 45", ""));
  CHECK_THAT(r.state.assembly->FindMateById(r.hinge)->rotation.value,
             WithinAbs(Rad(45.0), 1e-9));
  CHECK_THAT(LidX(r).x, WithinAbs(std::cos(Rad(45.0)), 1e-6));
  CHECK(r.state.assembly->ExprProblems().empty());
}

TEST_CASE("A limit narrowed past where the mate stands is the mate's error, "
          "and keeps its last good value",
          "[variables][assembly]") {
  Rig r;
  MakeRig(r);
  REQUIRE(ui::SetProjectVariable(r.state, "swing", "120", ""));
  Mate* hinge = r.state.assembly->FindMateById(r.hinge);
  hinge->rotation = Rad(60.0);
  hinge->rotationRange = MateRange{Rad(-120.0), Rad(120.0)};
  hinge->rotationRange->min.expr = "-swing";
  hinge->rotationRange->max.expr = "swing";
  REQUIRE(ui::SetProjectVariable(r.state, "swing", "100", ""));
  hinge = r.state.assembly->FindMateById(r.hinge);
  CHECK_THAT(hinge->rotationRange->max.value, WithinAbs(Rad(100.0), 1e-9));
  CHECK(hinge->exprError.empty());

  // 40 excludes where the hinge stands (60): refused as the mate's error.
  REQUIRE(ui::SetProjectVariable(r.state, "swing", "40", ""));
  hinge = r.state.assembly->FindMateById(r.hinge);
  CHECK_THAT(hinge->rotationRange->max.value, WithinAbs(Rad(100.0), 1e-9));
  CHECK_THAT(hinge->rotation.value, WithinAbs(Rad(60.0), 1e-9));
  CHECK_THAT(hinge->exprError, ContainsSubstring("outside its range"));
  CHECK_THAT(hinge->error, ContainsSubstring("outside its range"));
  REQUIRE(r.state.assembly->ExprProblems().size() == 1);
  CHECK(r.state.assembly->ExprProblems()[0].owner == "mate 'Hinge'");
  CHECK(r.state.assembly->ExprProblems()[0].id == r.hinge);
  // The texts stand: widening it again applies them.
  CHECK(hinge->rotationRange->max.expr == "swing");
  REQUIRE(ui::SetProjectVariable(r.state, "swing", "75", ""));
  hinge = r.state.assembly->FindMateById(r.hinge);
  CHECK_THAT(hinge->rotationRange->max.value, WithinAbs(Rad(75.0), 1e-9));
  CHECK(hinge->exprError.empty());
  CHECK(hinge->error.empty());

  // Upside down is the mate's error too.
  hinge->rotationRange->min.expr = "swing";
  hinge->rotationRange->max.expr = "-swing";
  REQUIRE(ui::SetProjectVariable(r.state, "swing", "70", ""));
  hinge = r.state.assembly->FindMateById(r.hinge);
  CHECK_THAT(hinge->exprError, ContainsSubstring("above its max"));
  CHECK_THAT(hinge->rotationRange->max.value, WithinAbs(Rad(75.0), 1e-9));
}

TEST_CASE("A lone assembly refuses a variable it cannot see",
          "[variables][assembly]") {
  AssemblyDocument a;
  Occurrence o;
  o.name = "Base";
  o.partId = 1;
  o.fixed = true;
  const uint64_t base = a.AddOccurrence(o);
  Mate m;
  m.name = "Slide";
  m.type = MateType::Slider;
  m.a = Origin(base);
  m.b = Origin(base);
  m.offset = 4.0;
  m.offset.expr = "travel";
  const uint64_t id = a.AddMate(m);
  SymbolTable none;
  CHECK(assembly::EvaluateExprs(a, none));
  const Mate* slide = a.FindMateById(id);
  CHECK(slide->offset.value == 4.0); // its last good value
  CHECK_THAT(slide->exprError, ContainsSubstring("travel"));
  REQUIRE(a.ExprProblems().size() == 1);
  CHECK_THAT(a.ExprProblems()[0].error, ContainsSubstring("offset"));
  // Asked again with nothing changed, nothing changed.
  CHECK_FALSE(assembly::EvaluateExprs(a, none));
}

TEST_CASE("Every kind of assembly number evaluates in its own unit",
          "[variables][assembly]") {
  AssemblyDocument a;
  Occurrence o;
  o.name = "Base";
  o.partId = 1;
  o.fixed = true;
  o.positionExpr = {"w", "", "w / 2"};
  const uint64_t base = a.AddOccurrence(o);
  Mate m;
  m.name = "Turn";
  m.type = MateType::Cylindrical;
  m.a = Origin(base);
  m.b = Origin(base);
  m.rotation.expr = "a"; // degrees, held in radians
  m.offset.expr = "w";   // mm
  const uint64_t mate = a.AddMate(m);
  MotionStudy s;
  s.name = "Sweep";
  s.driverMate = mate;
  s.from.expr = "-a";
  s.to.expr = "a";
  s.samples.expr = "n";
  a.AddMotionStudy(s);
  Relation g;
  g.name = "Lead";
  g.kind = RelationKind::Screw;
  g.mateA = g.mateB = mate;
  g.dofB = MateDof::Offset;
  g.ratio.expr = "w / 360";
  g.phase.expr = "w"; // b slides: mm
  a.AddRelation(g);
  AssemblySequence seq;
  SequenceStep pose;
  pose.pose.push_back({mate, MateDof::Rotation, ExprValue{}});
  pose.pose[0].value.expr = "a"; // degrees, as stored
  seq.steps.push_back(pose);
  seq.allowanceMm.expr = "w / 100";
  a.SetSequence(seq);

  SymbolTable vars{{"w", 20.0}, {"a", 30.0}, {"n", 12.0}};
  CHECK(assembly::EvaluateExprs(a, vars));
  CHECK(a.ExprProblems().empty());
  const Occurrence* placed = a.FindById(base);
  CHECK(placed->position == glm::dvec3(20.0, 0.0, 10.0));
  const Mate* turn = a.FindMateById(mate);
  CHECK_THAT(turn->rotation.value, WithinAbs(Rad(30.0), 1e-12));
  CHECK(turn->offset.value == 20.0);
  const MotionStudy& sweep = a.GetMotionStudies().at(0);
  CHECK_THAT(sweep.from.value, WithinAbs(Rad(-30.0), 1e-12));
  CHECK(sweep.samples.value == 12);
  const Relation& lead = a.GetRelations().at(0);
  CHECK_THAT(lead.ratio.value, WithinAbs(20.0 / 360.0, 1e-12));
  CHECK(lead.phase.value == 20.0);
  CHECK(a.GetSequence().steps[0].pose[0].value.value == 30.0);
  CHECK_THAT(a.GetSequence().allowanceMm.value, WithinAbs(0.2, 1e-12));

  // A count that comes out fractional is the study's problem, not rounded.
  vars["n"] = 12.5;
  CHECK(assembly::EvaluateExprs(a, vars));
  CHECK(a.GetMotionStudies().at(0).samples.value == 12);
  REQUIRE(a.ExprProblems().size() == 1);
  CHECK(a.ExprProblems()[0].owner == "motion study 'Sweep'");
  CHECK_THAT(a.ExprProblems()[0].error, ContainsSubstring("12.5"));
}

TEST_CASE("Rename and delete reach a part, an assembly and a study at once",
          "[variables][assembly]") {
  Rig r;
  MakeRig(r);
  REQUIRE(ui::SetProjectVariable(r.state, "gap", "3", ""));
  // A part feature reads it...
  Document& part = *ui::SlotDoc(r.state, 0);
  auto box =
      std::make_unique<BoxFeature>("Plate", glm::dvec3(0.0), 10.0, 10.0, 10.0);
  box->height.expr = "gap * 2";
  part.AddFeature(std::move(box));
  // ...a mate and a motion study of the assembly...
  AssemblyDocument& a = *r.state.assembly;
  a.FindMateById(r.hinge)->offset.expr = "gap";
  MotionStudy s;
  s.name = "Open";
  s.driverMate = r.hinge;
  s.from = 0.0;
  s.to = Rad(90.0);
  s.clashDeflectionMm.expr = "gap / 10";
  a.AddMotionStudy(s);
  // ...and the assembly's FEM study.
  fem::Study st;
  st.id = a.AllocateId();
  st.name = "Rig study";
  fem::Load l;
  l.id = a.AllocateId();
  l.magnitude.expr = "gap * 100";
  st.loads.push_back(l);
  a.GetStudies().push_back(st);

  std::vector<std::string> refs;
  CHECK_FALSE(ui::RemoveProjectVariable(r.state, "gap", &refs));
  const auto has = [&](const std::string& s) {
    return std::find(refs.begin(), refs.end(), s) != refs.end();
  };
  CHECK(has("Part 1: feature 'Plate'"));
  CHECK(has("Box: mate 'Hinge'"));
  CHECK(has("Box: motion study 'Open'"));
  CHECK(has("Box: study 'Rig study'"));

  const uint64_t stamp = a.SaveStamp();
  REQUIRE(ui::RenameProjectVariable(r.state, "gap", "clearance"));
  AssemblyDocument& after = *r.state.assembly;
  CHECK(after.FindMateById(r.hinge)->offset.expr == "clearance");
  CHECK(after.GetMotionStudies().at(0).clashDeflectionMm.expr ==
        "clearance / 10");
  CHECK(after.GetStudies().at(0).loads.at(0).magnitude.expr ==
        "clearance * 100");
  CHECK(static_cast<const BoxFeature*>(ui::SlotDoc(r.state, 0)->GetFeature(0))
            ->height.expr == "clearance * 2");
  CHECK(after.SaveStamp() != stamp); // saved, so dirty
  // Undo puts every one back.
  REQUIRE(ui::UndoProjectVars(r.state));
  CHECK(r.state.assembly->FindMateById(r.hinge)->offset.expr == "gap");
  CHECK(r.state.assembly->GetMotionStudies().at(0).clashDeflectionMm.expr ==
        "gap / 10");
}

// ---- The probe: nothing new arrives unbindable ------------------------------
//
// Every slot AssemblyDocument::VisitExprs hands out is given a sentinel text,
// the assembly is written as a file, and (1) each sentinel must land at its
// slot's path, as `<last key>_expr`, and (2) every number in the file must be
// either a visited slot or on the short list below of numbers that are
// deliberately not bindable, each with its reason. A new field then has to
// choose: a slot, or a line here saying why not.

namespace {

// Numbers that do not take an expression, by path with every index written
// `*`; a trailing "/**" is the whole subtree.
const std::vector<std::pair<std::string, std::string>> kNotBindable = {
    {"version", "the format's"},
    {"active_configuration", "an id"},
    {"occurrences/*/id", "an id"},
    {"occurrences/*/part_id", "an id"},
    {"occurrences/*/configuration_id", "an id"},
    {"occurrences/*/transform/orientation/*",
     "a quaternion has no one number to bind (a mate's twist does)"},
    {"occurrences/*/attached_to", "an id"},
    {"occurrences/*/attach_offset/*",
     "a rider's placement on its seat, derived from the seat on every solve"},
    {"occurrences/*/attach_rotation/*", "the same, a quaternion"},
    {"occurrences/*/attach_seat/**", "ids and names"},
    {"occurrences/*/inner_poses/**",
     "where the flexible placement's solve left its parts: an output"},
    {"occurrences/*/inner_mate_dofs/*/mate", "an id"},
    {"occurrences/*/inner_mate_dofs/*/dx",
     "a planar's slide inside a flexible placement, held by its solve "
     "(set_mate "
     "{path} poses rotation and offset)"},
    {"occurrences/*/inner_mate_dofs/*/dy", "the same"},
    {"occurrences/*/inner_mate_dofs/*/ball_rotation/*", "a quaternion"},
    {"mates/*/id", "an id"},
    {"mates/*/a/**", "a connector: ids and names"},
    {"mates/*/b/**", "a connector: ids and names"},
    {"mates/*/ball_rotation/*",
     "a quaternion has no one number to bind; its cone does"},
    {"exploded/*/id", "an id"},
    {"exploded/*/steps/*/occurrence", "an id"},
    {"exploded/*/steps/*/path/*", "an id"},
    {"relations/*/id", "an id"},
    {"relations/*/mate_a", "an id"},
    {"relations/*/mate_b", "an id"},
    {"relations/*/mate_a_path/*", "an id"},
    {"relations/*/mate_b_path/*", "an id"},
    {"relations/*/ratio",
     "where it is derived -- from teeth, a thread, a worm's hand -- or a "
     "cam's, "
     "a contact's or an intermittent relation's, which have none; every other "
     "ratio is a slot"},
    {"relations/*/teeth_a",
     "a tooth count derives the ratio; binding a count that makes geometry "
     "is not built"},
    {"relations/*/teeth_b", "the same"},
    {"relations/*/teeth", "a freewheel's notches, the same"},
    {"relations/*/terms/*/teeth", "the same"},
    {"relations/*/terms/*/mate", "an id"},
    {"relations/*/terms/*/path/*", "an id"},
    {"relations/*/terms/*/coefficient",
     "an epicyclic's and a planet's come from their teeth (a linear_n's is a "
     "slot)"},
    {"relations/*/thread_part", "an id"},
    {"relations/*/thread_feature", "an id"},
    {"relations/*/lead",
     "a freewheel's or a click's history, kept by the solve"},
    {"relations/*/ride/**",
     "a toothed freewheel's ride: ids, and a lift that is not bound (not "
     "built)"},
    {"relations/*/slots", "a Geneva's slot count derives its step and window"},
    {"relations/*/table/**",
     "an intermittent law's shape, fractions of the window"},
    {"relations/*/profile/**", "ids"},
    {"relations/*/cam", "an id"},
    {"relations/*/follower", "an id"},
    {"relations/*/cam_body", "an id"},
    {"relations/*/follower_body", "an id"},
    {"motion/*/id", "an id"},
    {"motion/*/driver_mate", "an id"},
    {"motion/*/driver_path/*", "an id"},
    {"motion/*/configuration", "an id"},
    {"motion/*/max_refine", "a solver setting, not a dimension"},
    {"studies/**",
     "a FEM study's own numbers, walked by the study's own visitor"},
    {"assertions/**",
     "a clearance as files before v160 held it, moved into the requirements "
     "on open (their bounds take expressions)"},
    {"sequence/steps/*/occurrence", "an id"},
    {"sequence/steps/*/occurrences/*", "an id"},
    {"sequence/steps/*/mate", "an id"},
    {"sequence/steps/*/pose/*/mate", "an id"},
    {"sequence/steps/*/direction/*",
     "a unit vector: a direction, not a dimension"},
    {"sequence/steps/*/origin_mm/*",
     "a point on the path's line, read with its direction"},
    {"configurations/*/id", "an id"},
    {"configurations/*/suppressed_features/*", "an id"},
    {"configurations/*/suppressed_mates/*", "an id"},
    {"configurations/*/suppressed_relations/*", "an id"},
    {"configurations/*/mate_overrides/*/mate", "an id"},
};

std::string Generic(const std::string& path) {
  std::string out;
  size_t from = 0;
  while (from <= path.size()) {
    const size_t slash = path.find('/', from);
    const std::string seg = path.substr(
        from, slash == std::string::npos ? std::string::npos : slash - from);
    const bool index = !seg.empty() &&
                       seg.find_first_not_of("0123456789") == std::string::npos;
    out += (out.empty() ? "" : "/") + (index ? std::string("*") : seg);
    if (slash == std::string::npos)
      break;
    from = slash + 1;
  }
  return out;
}

bool Listed(const std::string& generic) {
  for (const auto& [pattern, why] : kNotBindable) {
    if (pattern == generic)
      return true;
    if (pattern.ends_with("/**") &&
        generic.starts_with(pattern.substr(0, pattern.size() - 2)))
      return true;
  }
  return false;
}

// Every number in `j`, by path.
void Numbers(const nlohmann::json& j, const std::string& at,
             std::vector<std::string>& out) {
  if (j.is_number()) {
    out.push_back(at);
  } else if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it)
      Numbers(it.value(), at.empty() ? it.key() : at + "/" + it.key(), out);
  } else if (j.is_array()) {
    for (size_t i = 0; i < j.size(); i++)
      Numbers(j[i], at + "/" + std::to_string(i), out);
  }
}

// Where a slot's text is written: `<last key>_expr` in place of its last
// key ("mates/0/limits/rotation/1" -> "mates/0/limits/rotation_expr/1").
std::string ExprPathOf(const std::string& path) {
  std::vector<std::string> segs;
  size_t from = 0;
  while (true) {
    const size_t slash = path.find('/', from);
    segs.push_back(path.substr(from, slash - from));
    if (slash == std::string::npos)
      break;
    from = slash + 1;
  }
  for (size_t i = segs.size(); i-- > 0;)
    if (segs[i].find_first_not_of("0123456789") != std::string::npos) {
      segs[i] += "_expr";
      break;
    }
  std::string out;
  for (const std::string& s : segs)
    out += (out.empty() ? "" : "/") + s;
  return out;
}

const nlohmann::json* At(const nlohmann::json& j, const std::string& path) {
  const nlohmann::json* at = &j;
  size_t from = 0;
  while (true) {
    const size_t slash = path.find('/', from);
    const std::string seg = path.substr(from, slash - from);
    if (at->is_array()) {
      const size_t i = std::stoul(seg);
      if (i >= at->size())
        return nullptr;
      at = &(*at)[i];
    } else if (at->is_object() && at->contains(seg)) {
      at = &at->at(seg);
    } else {
      return nullptr;
    }
    if (slash == std::string::npos)
      return at;
    from = slash + 1;
  }
}

// One of everything an assembly holds.
AssemblyDocument Everything() {
  AssemblyDocument a;
  Occurrence base;
  base.name = "Base";
  base.partId = 1;
  base.fixed = true;
  const uint64_t b = a.AddOccurrence(base);
  Occurrence flex;
  flex.name = "Unit";
  flex.partId = 2;
  flex.refKind = OccurrenceRef::Assembly;
  flex.flexible = true;
  flex.innerPoses.push_back({5, glm::dvec3(1, 2, 3), glm::dquat(1, 0, 0, 0)});
  InnerMateDof d;
  d.mate = 6;
  d.rotation = Rad(10.0);
  d.offset = 2.0;
  d.dx = 1.0;
  d.dy = 1.0;
  flex.innerMateDofs.push_back(d);
  const uint64_t f = a.AddOccurrence(flex);
  Occurrence rider;
  rider.name = "Screw";
  rider.partId = 1;
  rider.attachedTo = b;
  rider.attachOffset = glm::dvec3(1, 2, 3);
  a.AddOccurrence(rider);
  std::vector<uint64_t> mates;
  for (MateType t : {MateType::Fastened, MateType::Revolute, MateType::Slider,
                     MateType::Cylindrical, MateType::Planar, MateType::Ball}) {
    Mate m;
    m.name = "m";
    m.type = t;
    m.a = Origin(b);
    m.b = Origin(f);
    m.dx = 1.0;
    m.dy = 1.0;
    m.ballRotation = glm::dquat(0.9, 0.1, 0.1, 0.1);
    for (RangedValue v : assembly::RangedValuesFor(t)) {
      if (v == RangedValue::Cone)
        m.coneRad = ExprAngle::FromDegrees(30.0);
      else
        (v == RangedValue::Rotation ? m.rotationRange
         : v == RangedValue::Offset ? m.offsetRange
         : v == RangedValue::Dx     ? m.dxRange
                                    : m.dyRange) = MateRange{-1e3, 1e3};
    }
    mates.push_back(a.AddMate(m));
  }
  ExplodedView v;
  v.name = "Apart";
  ExplodeStep s;
  s.occurrenceId = b;
  s.offset = glm::dvec3(0, 0, 10);
  v.steps.push_back(s);
  a.AddExplodedView(v);
  const uint64_t rev = mates[1], slide = mates[2];
  Relation gear;
  gear.name = "Pair";
  gear.mateA = rev;
  gear.mateB = rev;
  a.AddRelation(gear);
  Relation teeth = gear;
  teeth.name = "Toothed";
  teeth.teethA = 12;
  teeth.teethB = 24;
  a.AddRelation(teeth);
  Relation cam;
  cam.name = "Cam";
  cam.kind = RelationKind::Cam;
  cam.mateA = rev;
  cam.mateB = slide;
  cam.dofB = MateDof::Offset;
  cam.lift = {{0.0, 0.0}, {180.0, 5.0}};
  a.AddRelation(cam);
  Relation step;
  step.name = "Carry";
  step.kind = RelationKind::Intermittent;
  step.mateA = rev;
  step.mateB = rev;
  step.intermittent.step = 36.0;
  step.intermittent.durationDeg = 90.0;
  a.AddRelation(step);
  Relation sum;
  sum.name = "Sum";
  sum.kind = RelationKind::LinearN;
  sum.terms.push_back({rev, {}, MateDof::Rotation, ExprValue{1.0}, 0});
  sum.terms.push_back({slide, {}, MateDof::Offset, ExprValue{2.0}, 0});
  a.AddRelation(sum);
  MotionStudy ms;
  ms.name = "Sweep";
  ms.driverMate = rev;
  ms.from = 0.0;
  ms.to = Rad(90.0);
  ms.clashDeflectionMm = 0.1;
  ms.maxRefine = 3;
  a.AddMotionStudy(ms);
  AssemblySequence seq;
  SequenceStep place;
  place.occurrences = {f};
  place.direction = glm::dvec3(0, 0, 1);
  place.origin = glm::dvec3(1, 1, 1);
  place.mateId = rev;
  place.distanceMm = 5.0;
  place.turnDeg = 90.0;
  seq.steps.push_back(place);
  SequenceStep pose;
  pose.pose.push_back({rev, MateDof::Rotation, ExprValue{10.0}});
  seq.steps.push_back(pose);
  seq.allowanceMm = 0.1;
  a.SetSequence(seq);
  Configuration& c = a.AddConfiguration("Held");
  MateOverride hold;
  hold.mate = rev;
  hold.rotation = ExprAngle::FromDegrees(5.0);
  hold.offset = ExprValue{1.0};
  c.mateOverrides.push_back(hold);
  c.suppressedMates.push_back(slide);
  fem::Study& st = a.AddStudy("Rig");
  fem::Load l;
  l.id = a.AllocateId();
  l.magnitude = 10.0;
  st.loads.push_back(l);
  return a;
}

nlohmann::json Written(const AssemblyDocument& a) {
  const fs::path path =
      fs::temp_directory_path() /
      ("lobole_asm_probe_" + std::to_string(::getpid()) + ".json");
  REQUIRE(testio::SaveAssembly(a, path.string()));
  std::ifstream in(path);
  nlohmann::json j = nlohmann::json::parse(in);
  fs::remove(path);
  return j;
}

} // namespace

TEST_CASE("Every number an assembly writes is a slot or says why it is not",
          "[variables][assembly][conventions]") {
  AssemblyDocument a = Everything();
  // (1) A sentinel through every slot lands where its path says.
  int n = 0;
  std::vector<std::pair<std::string, std::string>> sentinels;
  a.VisitExprs([&](const std::string&, const ExprSlot& slot) {
    const std::string text = "probe_" + std::to_string(n++);
    slot.Expr() = text;
    sentinels.push_back({slot.Path(), text});
  });
  REQUIRE(n > 40);
  const nlohmann::json j = Written(a);
  std::set<std::string> visited;
  for (const auto& [path, text] : sentinels) {
    INFO(path);
    const nlohmann::json* number = At(j, path);
    CHECK((number != nullptr && number->is_number()));
    const nlohmann::json* expr = At(j, ExprPathOf(path));
    REQUIRE(expr != nullptr);
    CHECK(*expr == text);
    visited.insert(path);
  }
  // (2) Every number is a slot or listed.
  std::vector<std::string> numbers;
  Numbers(j, "", numbers);
  for (const std::string& path : numbers) {
    INFO(path);
    CHECK((visited.contains(path) || Listed(Generic(path))));
  }
  // And it reads back with every text where it was.
  const fs::path file =
      fs::temp_directory_path() /
      ("lobole_asm_probe_rt_" + std::to_string(::getpid()) + ".json");
  REQUIRE(testio::SaveAssembly(a, file.string()));
  auto loaded = testio::LoadAssembly(file.string());
  fs::remove(file);
  REQUIRE(loaded != nullptr);
  std::vector<std::pair<std::string, std::string>> back;
  loaded->VisitExprs([&](const std::string&, const ExprSlot& slot) {
    back.push_back({slot.Path(), slot.Expr()});
  });
  CHECK(back == sentinels);
}
