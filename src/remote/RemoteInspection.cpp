// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteInspection.h"
#include "remote/InspectionContracts.h"

#include "cad/CADKernel.h"
#include "common/BuildInfo.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/FitStandards.h"
#include "core/FrameMath.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/MeshCollide.h"
#include "core/Occurrence.h"
#include "core/Parallel.h"
#include "core/ThreadStandards.h"
#include "core/ToleranceStack.h"
#include "core/features/CurveFeature.h"
#include "core/features/DerivedFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/io/Serialization.h"
#include "core/tube/CutList.h"
#include "core/tube/Rules.h"
#include "remote/RemoteHelpers.h"
#include "render/ClipPlanes.h"
#include "ui/AppState.h"

#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/DerivedActions.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h" // ConfiguredPartDocument
#include "ui/JobActions.h"
#include "ui/PrintCheckActions.h"
#include "ui/PrintProfileActions.h"
#include "ui/RendererSync.h"
#include "ui/TemplateActions.h"
#include "ui/ToleranceActions.h"
#include "ui/ViewActions.h"
#include <OSD.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_DataMapOfShapeInteger.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using nlohmann::json;

namespace remote {

// Declared in RemoteHelpers.h so every domain module can resolve a `part`
// the same way — see the note there on why a read must not activate a tab.
Document* ResolvePartParam(ui::AppState& state, const json& part) {
  ui::EnsureParts(state);
  uint64_t id = 0;
  // A numeric `part` is tried as a STABLE id first (what occurrences
  // reference); if no part carries that id it falls back to the slot INDEX from
  // list_parts, so passing the visible 0..N index no longer silently returns
  // another part.
  std::optional<int> indexFallback;
  if (part.is_number_unsigned() || part.is_number_integer()) {
    id = part.get<uint64_t>();
    indexFallback = part.get<int>();
  } else if (part.is_string()) {
    const std::string s = part.get<std::string>();
    id = ui::PartIdByName(state, s);
    if (id == 0) { // allow a stable id / index passed as a string
      try {
        id = std::stoull(s);
        indexFallback = std::stoi(s);
      } catch (...) {
        id = 0;
      }
    }
  }
  Document* d = id != 0 ? ui::PartDocById(state, id) : nullptr;
  if (d == nullptr && indexFallback && *indexFallback >= 0 &&
      *indexFallback < static_cast<int>(state.parts.size())) {
    d = ui::PartDocById(state, state.parts[*indexFallback].id);
  }
  if (d == nullptr)
    throw std::runtime_error("no part with id or index " + part.dump() +
                             " — pass a part name, stable id or slot index "
                             "(see list_parts)");
  return d;
}

// Declared in RemoteHelpers.h — the read-only counterpart, shared by every
// module that answers a question ABOUT a part rather than changing one.
const Document& SubjectPart(ui::AppState& state, Document* active,
                            const RpcParams& p) {
  if (p.contains("part"))
    return *ResolvePartParam(state, p.at("part"));
  if (active == nullptr)
    throw std::runtime_error(
        "there is no active part on this tab, so name the one you mean: "
        "`part` takes a name, a stable id or a slot index");
  return *active;
}

json PartRevision(const Document& part, uint64_t partId) {
  // A process nonce prevents a document ID from another server matching.
  static const std::string session = [] {
    std::random_device random;
    return std::to_string(random()) + "-" + std::to_string(random()) + "-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count());
  }();
  return json{{"session", session},
              {"part_id", partId},
              {"instance", part.InstanceId()},
              {"version", part.Version()}};
}

namespace {

// A feature of `doc` by id or by name, for a filter; refused by name.
Feature* FeatureParam(Document& doc, const json& j, const char* what) {
  if (j.is_number_unsigned() || j.is_number_integer()) {
    if (Feature* f = doc.FindFeatureById(j.get<uint64_t>()))
      return f;
    throw std::runtime_error(std::string(what) + ": no feature with id " +
                             j.dump() + " in this part (get_state lists them)");
  }
  if (j.is_string()) {
    // A sketch is often named for the feature it drives ("Slab", and the
    // Slab extrude): it makes no faces, so it is not what a face was made
    // by. Anything else sharing a name is ambiguous and said so.
    std::vector<Feature*> hits;
    for (size_t i = 0; i < doc.FeatureCount(); i++)
      if (doc.GetFeature(i)->GetName() == j.get<std::string>() &&
          doc.GetFeature(i)->TypeName() != "Sketch")
        hits.push_back(doc.GetFeature(i));
    if (hits.size() == 1)
      return hits.front();
    if (hits.empty()) {
      for (size_t i = 0; i < doc.FeatureCount(); i++)
        if (doc.GetFeature(i)->GetName() == j.get<std::string>())
          throw std::runtime_error(
              std::string(what) + ": " + j.dump() +
              " is a Sketch, which makes no faces -- name the feature that "
              "used it (an extrude, a cut, a hole)");
      throw std::runtime_error(std::string(what) + ": no feature named " +
                               j.dump() +
                               " in this part (get_state lists them)");
    }
    std::string ids;
    for (const Feature* f : hits)
      ids += (ids.empty() ? "" : ", ") + std::to_string(f->GetId()) + " (" +
             f->TypeName() + ")";
    throw std::runtime_error(
        std::string(what) + ": " + std::to_string(hits.size()) +
        " features are named " + j.dump() + ": " + ids + " -- pass the id");
  }
  throw std::runtime_error(std::string(what) +
                           " is a feature id or a feature name");
}

// list_elements' filters: the elements a feature made, in a role.
struct ElementFilter {
  bool hasMadeBy = false, hasRole = false;
  uint64_t madeBy = 0;
  // A Template instance's features (docs/modeling/templates.md#names): what
  // the instance made is what any of them made.
  std::set<uint64_t> madeByAlso;
  std::string madeByLabel; // "feature 4 (Pocket, Extrude)"
  topo::ElementRole role = topo::ElementRole::Unknown;
  std::string roleAsked;
  uint32_t entity = 0;
  uint64_t via = 0; // a Derived feature of the listed part

  bool Any() const { return hasMadeBy || hasRole || entity != 0 || via != 0; }

  // Everything but the role and entity: what the "why" counts roles over.
  bool MatchesMaker(const topo::ElementOrigin& o) const {
    if (via != 0) {
      if (o.derived.empty() || o.derived.front().derivedId != via)
        return false;
    } else if (hasMadeBy && !o.derived.empty()) {
      return false; // a copy's ids are its SOURCE's: say `via`
    }
    return !hasMadeBy || o.madeBy == madeBy || madeByAlso.contains(o.madeBy);
  }
  bool Matches(const topo::ElementOrigin& o) const {
    if (!MatchesMaker(o))
      return false;
    if (hasRole && o.role != role)
      return false;
    return entity == 0 || o.entity == entity;
  }

  std::string WhyNothing(const std::vector<Body>& bodies) const {
    std::map<std::string, int> roles;
    for (const Body& b : bodies)
      for (const auto& [idx, name] : b.elementMap.names) {
        const topo::ElementOrigin o = topo::DecodeElementName(name);
        if (MatchesMaker(o))
          roles[topo::ElementRoleName(o.role)]++;
      }
    const std::string who = hasMadeBy ? madeByLabel : "the filter";
    if (roles.empty())
      return who +
             " made no face that is on this part now: it made none (a "
             "sketch, a datum), everything it made was cut away since, or " +
             (via != 0 ? "it is not in that Derived copy"
                       : "the faces came in through a Derived copy (say "
                         "`via`)");
    std::string made;
    for (const auto& [r, n] : roles)
      made += (made.empty() ? "" : ", ") + r + " x" + std::to_string(n);
    std::string asked = roleAsked;
    if (entity != 0)
      asked += (asked.empty() ? "" : " ") + std::string("from entity ") +
               std::to_string(entity);
    std::string out = who + " made " + made + " -- none " + asked;
    if (roles.count("positional"))
      out += ". It names its faces by position (a shell, loft, sweep, "
             "import...), so they have no role but `positional`";
    return out;
  }
};

ElementFilter ParseElementFilter(ui::AppState& state, Document* listed,
                                 const RpcParams& p) {
  ElementFilter f;
  Document* makers = listed; // where `made_by` names a feature
  if (p.contains("via")) {
    if (listed == nullptr)
      throw std::runtime_error("via names a Derived feature of a part: pass "
                               "`part` with it");
    Feature* d = FeatureParam(*listed, p.at("via"), "via");
    const auto* derived = dynamic_cast<const DerivedFeature*>(d);
    if (derived == nullptr)
      throw std::runtime_error("via: feature " + std::to_string(d->GetId()) +
                               " is a " + d->TypeName() +
                               ", not a Derived copy");
    f.via = d->GetId();
    makers = ui::PartDocById(state, derived->sourcePart);
  }
  if (p.contains("made_by")) {
    f.hasMadeBy = true;
    const json& mb = p.at("made_by");
    const int owner =
        makers != nullptr && mb.is_number_integer() &&
                makers->FindFeatureById(mb.get<uint64_t>()) == nullptr
            ? makers->OwnerIndex(mb.get<uint64_t>())
            : -1;
    if (owner >= 0) {
      // A feature a Template instance expanded into: not in the tree, but
      // the faces it made are named under its id.
      f.madeBy = mb.get<uint64_t>();
      f.madeByLabel =
          "feature " + std::to_string(f.madeBy) +
          " (inside the template instance '" +
          makers->GetFeature(static_cast<size_t>(owner))->GetName() + "')";
    } else if (makers != nullptr) {
      Feature* m = FeatureParam(*makers, mb, "made_by");
      f.madeBy = m->GetId();
      f.madeByLabel = "feature " + std::to_string(m->GetId()) + " (" +
                      m->GetName() + ", " + m->TypeName() + ")";
      // A pattern's copies keep its SOURCE's names with an instance (#2),
      // so the faces a pattern made are its source's, every copy included.
      if (const auto* pat = dynamic_cast<const PatternFeature*>(m)) {
        f.madeBy = pat->sourceId;
        f.madeByLabel += ", whose copies are feature " +
                         std::to_string(pat->sourceId) + "'s";
      }
      // A template instance made what its expanded features made.
      m->VisitOwnedIds([&](uint64_t id) { f.madeByAlso.insert(id); });
    } else if (mb.is_number()) {
      f.madeBy = mb.get<uint64_t>();
      f.madeByLabel = "feature " + std::to_string(f.madeBy);
    } else {
      throw std::runtime_error("made_by by name needs `part` on a job tab");
    }
  }
  if (p.contains("role")) {
    f.roleAsked = p.at("role").get<std::string>();
    std::string r = f.roleAsked;
    if (r == "floor")
      r = "end_cap";
    else if (r == "walls")
      r = "side";
    if (!topo::ElementRoleFromString(r, f.role))
      throw std::runtime_error("role '" + f.roleAsked +
                               "' is not one of: " + topo::ElementRoleList() +
                               " (and floor = end_cap, walls = side)");
    f.hasRole = true;
  }
  f.entity = p.value("entity", uint32_t{0});
  return f;
}

// For each edge of the body (ElementMap::edgeNames order), the names of the
// faces it bounds -- two for an ordinary edge, one for a seam. An unnamed
// face is left out.
std::vector<std::vector<std::string>> EdgeFaceNames(const Body& body) {
  const TopoDS_Shape& shape = body.shape.GetShape();
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  TopTools_IndexedDataMapOfShapeListOfShape parents;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, parents);
  // face -> its index in explorer order, the element map's face order
  TopTools_DataMapOfShapeInteger faceIndex;
  int k = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), k++)
    if (!faceIndex.IsBound(ex.Current()))
      faceIndex.Bind(ex.Current(), k);
  std::vector<std::vector<std::string>> out(edges.Extent());
  for (int e = 1; e <= edges.Extent(); e++) {
    const int at = parents.FindIndex(edges(e));
    if (at == 0)
      continue;
    for (const TopoDS_Shape& f : parents(at)) {
      if (!faceIndex.IsBound(f))
        continue;
      const std::string* n = body.elementMap.NameOf(faceIndex.Find(f));
      if (n != nullptr && std::find(out[e - 1].begin(), out[e - 1].end(), *n) ==
                              out[e - 1].end())
        out[e - 1].push_back(*n);
    }
  }
  return out;
}

// What made an element, on its list_elements row.
void OriginJson(const topo::ElementOrigin& o, json& row) {
  row["made_by"] = o.madeBy;
  row["role"] = topo::ElementRoleName(o.role);
  if (o.entity != 0)
    row["entity"] = o.entity;
  if (!o.instances.empty()) {
    std::string inst;
    for (const std::string& i : o.instances)
      inst += (inst.empty() ? "" : "#") + i;
    row["instance"] = inst;
  }
  if (!o.derived.empty()) {
    json d = json::array();
    for (const topo::DerivedStep& s : o.derived)
      d.push_back(s.derivedId);
    row["derived"] = d;
  }
}

// The part a design-intent edit (a tolerance, the print process) acts on:
// `part` when given — ANY part, not only the active one, so a generator or
// an assembly tab can record intent on the parts it placed — else the active
// part. Paired with CommandsFor, so the edit lands in THAT part's undo
// history.
Document& PartToEdit(ui::AppState& state, Document* active, const RpcParams& p,
                     const char* what) {
  if (p.contains("part"))
    return *ResolvePartParam(state, p.at("part"));
  if (active == nullptr)
    throw std::runtime_error(
        std::string(what) +
        ": no active part document — pass `part` (a name, stable id or "
        "slot index, see list_parts) to edit any part from this tab");
  return *active;
}

// The undo history `part` keeps: the active tab's, or the one its slot
// holds while stashed. An edit to a stashed part undoes when that part's
// tab is undone, like any edit made on it.
CommandStack& CommandsFor(ui::AppState& state, const Document& part) {
  if (state.document.get() == &part && state.commands)
    return *state.commands;
  for (ui::PartSlot& slot : state.parts)
    if (slot.doc.get() == &part) {
      if (!slot.commands)
        slot.commands = std::make_unique<CommandStack>();
      return *slot.commands;
    }
  throw std::runtime_error("that part has no undo history of its own (a "
                           "configuration's evaluated copy?) — switch to its "
                           "tab and edit it there");
}

// The active part document, required by the part-tab-only inspection RPCs.
Document& RequirePartDoc(Document* doc, const char* what) {
  if (doc == nullptr)
    throw std::runtime_error(
        std::string(what) +
        ": no active part document. On an assembly tab pass `part` or "
        "occurrence refs; on a JOB tab pass `part` — list_job_parts names "
        "the parts that job cuts, and a machining target is picked on one "
        "of them");
  return *doc;
}

// V1 contact classification: a pair either overlaps (clash), sits within the
// touch tolerance (touching — a boltable joint), or is clear by `distance`.
// An assembly-wide pairwise sweep, bounded.
//
// Both sweeps below are O(occurrences^2) in EXACT geometry checks, and an
// exact check is an OpenCASCADE boolean or extrema. On the SO-101 arm (253
// occurrences, 31,878 pairs — a hundred of them PCB parts sharing one 25 mm
// board) `interference` with no pair named ran for 48 minutes without
// answering and blocked every other RPC, `ping` included, because it did a
// boolean for every pair and reported all of them.
//
// Two rules make it bounded. A pair whose world boxes are further apart
// than the tolerance cannot touch, so one box test settles it. What is left
// is checked CLOSEST FIRST — the pairs most likely to clash — until the
// budget is spent, and whatever is left over is REPORTED rather than
// quietly dropped.
//
// A pair cap alone is NOT a time bound: an exact check between two large
// solids is an OpenCASCADE boolean, and 4000 of them on this arm still ran
// past ten minutes. The sweep therefore also stops at a deadline, and says
// how far it got. An answer that arrives with its coverage stated is worth
// more than one that never arrives.
struct SweepPlan {
  std::vector<std::pair<size_t, size_t>> exact; // closest box gap first
  std::vector<double> gap;                      // box gap, same order
  size_t boxCleared = 0;                        // settled by the box test alone
  size_t overBudget = 0;      // needed an exact check, did not get one
  double overBudgetGap = 0.0; // the smallest box gap left unchecked
  double elapsed = 0.0;       // seconds spent on the exact checks
};

// Runs the plan's exact pairs, IN PARALLEL, then hands each result to
// `record(i, j, result)` on the calling thread in PLAN ORDER.
//
// The split into `compute` and `record` is not a style choice. `record`
// touches shared json, a union-find and a set; `compute` touches nothing but
// its own two shapes. Only the second of those can run on twenty-two threads
// at once, and a contact check is 161 ms of OpenCASCADE boolean — a
// 49-sample motion study on this engine is 20,403 of them, 3292 s on one
// core. Recording in plan order is what keeps the response byte-identical to
// a serial sweep, so the two can be diffed (`sweep_parallel_smoke.py`).
//
// Pairs are DISPATCHED in plan order — closest box gap first — and the
// deadline stops new dispatches rather than cancelling running ones, so the
// set that gets checked is still a PREFIX of the plan and `truncation` still
// means "the closest N". A boolean already running cannot be preempted; the
// reported `elapsed_s` is the honest number either way.
//
// OpenCASCADE fills caches lazily ON the shape, and one body appears in many
// pairs, so shapes must be warm before the workers start. They are:
// `Measure::WorldBounds` runs over every entity, single-threaded, while the
// plan is being built.
//
// `cancel` (an async contact_report's job) stops new dispatches the way the
// deadline does, and `fraction` is written with the share of pairs done.
//
// The workers run under parallel::For, so a pair whose own check is
// parallel inside (ExactDistance's face pairs) gets this worker's share of
// the machine rather than a machine's worth of threads per worker.
template <typename C, typename R>
size_t RunSweep(SweepPlan& plan, double deadlineSeconds, C&& compute,
                R&& record, const std::atomic<bool>* cancel = nullptr,
                std::atomic<double>* fraction = nullptr) {
  using Result = decltype(compute(size_t{0}, size_t{0}));
  const auto start = std::chrono::steady_clock::now();
  const bool timed = deadlineSeconds > 0.0;
  const size_t n = plan.exact.size();
  const auto elapsedNow = [&start] {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
  };

  std::vector<Result> results(n);
  std::atomic<size_t> next{0};
  std::atomic<size_t> stopAt{n}; // first pair the deadline turned away
  std::atomic<size_t> done{0};
  const auto worker = [&] {
    for (;;) {
      const size_t k = next.fetch_add(1, std::memory_order_relaxed);
      if (k >= stopAt.load(std::memory_order_relaxed))
        return;
      // Every pair, not every eighth: one contact check between two imported
      // servos is seconds, so sampling the clock in batches overran a 20 s
      // deadline by 100 s. Reading a steady_clock is nanoseconds.
      if ((timed && elapsedNow() > deadlineSeconds) ||
          (cancel != nullptr && cancel->load(std::memory_order_relaxed))) {
        size_t seen = stopAt.load(std::memory_order_relaxed);
        while (k < seen && !stopAt.compare_exchange_weak(
                               seen, k, std::memory_order_relaxed))
          ;
        return;
      }
      results[k] = compute(plan.exact[k].first, plan.exact[k].second);
      const size_t finished = done.fetch_add(1) + 1;
      if (fraction != nullptr)
        fraction->store(static_cast<double>(finished) / static_cast<double>(n));
    }
  };

  // One slot per worker thread, each pulling from the shared counter.
  //
  // Each worker hands a check that is parallel inside (ExactDistance's face
  // pairs) the WHOLE machine, not parallel::For's even share of it. A
  // sweep's pairs are uneven: in a planetary gear set the sun against the
  // ring it sits inside asks 2000 face pairs while every meshing pair asks
  // about 100, so a share of 22 / 7 left the long one on three threads for a
  // minute while the rest sat idle. Oversubscribing while several long ones
  // overlap is the cheaper error; the scheduler time-slices them.
  const size_t threads = std::min<size_t>(n, parallel::Workers());
  const size_t whole = parallel::Workers();
  parallel::For(threads, [&](size_t) {
    const size_t was = parallel::detail::tlShare;
    parallel::detail::tlShare = whole;
    worker();
    parallel::detail::tlShare = was;
  });

  const size_t checked = std::min(stopAt.load(), n);
  for (size_t k = 0; k < checked; ++k)
    record(plan.exact[k].first, plan.exact[k].second, results[k]);

  plan.elapsed = elapsedNow();
  if (checked < n) {
    plan.overBudgetGap = plan.gap[checked];
    plan.overBudget += n - checked;
    plan.exact.resize(checked);
    plan.gap.resize(checked);
  }
  return checked;
}

SweepPlan PlanSweep(const std::vector<glm::dvec3>& mins,
                    const std::vector<glm::dvec3>& maxs, double gapTol,
                    size_t maxPairs) {
  struct Cand {
    double gap;
    size_t i, j;
  };
  std::vector<Cand> cands;
  SweepPlan plan;
  for (size_t i = 0; i < mins.size(); ++i)
    for (size_t j = i + 1; j < mins.size(); ++j) {
      const glm::dvec3 d = glm::max(
          glm::max(mins[i] - maxs[j], mins[j] - maxs[i]), glm::dvec3(0.0));
      const double gap = static_cast<double>(glm::length(d));
      if (gap > gapTol) {
        plan.boxCleared++;
        continue;
      }
      cands.push_back({gap, i, j});
    }
  std::stable_sort(cands.begin(), cands.end(),
                   [](const Cand& a, const Cand& b) { return a.gap < b.gap; });
  for (size_t k = 0; k < cands.size(); ++k) {
    if (plan.exact.size() >= maxPairs) {
      plan.overBudget = cands.size() - k;
      plan.overBudgetGap = cands[k].gap;
      break;
    }
    plan.exact.push_back({cands[k].i, cands[k].j});
    plan.gap.push_back(cands[k].gap);
  }
  return plan;
}

// What the sweep did, always in the response: a bounded answer that does
// not say it was bounded is the "correct-looking wrong answer" again.
// `notChecked` is the pairs the sweep DECLINED to start — since the face-budget
// fix, a pair of imported meshes whose exact boolean would be unbounded. It has
// to reach the receipt: a sweep that skipped two 54k-face bodies and reports
// `verified` is the false pass this receipt exists to make impossible, and the
// list is built by the caller AFTER this function used to run.
void AttachSweepCoverage(nlohmann::json& out, const SweepPlan& plan,
                         size_t entities, size_t maxPairs,
                         double deadlineSeconds, size_t notChecked = 0) {
  out["occurrences"] = entities;
  out["pairs_checked"] = plan.exact.size();
  out["pairs_box_cleared"] = plan.boxCleared;
  out["elapsed_s"] = plan.elapsed;
  std::string truncation;
  if (notChecked != 0)
    truncation = std::to_string(notChecked) +
                 " pair(s) were too heavy to check exactly and were NOT "
                 "started (see not_checked). ";
  if (plan.overBudget != 0) {
    truncation +=
        "checked the " + std::to_string(plan.exact.size()) +
        " closest pairs, then stopped (max_pairs " + std::to_string(maxPairs) +
        ", deadline_s " + std::to_string(deadlineSeconds) + "); " +
        std::to_string(plan.overBudget) +
        " more have overlapping or near boxes and were NOT checked exactly, "
        "the closest of them " +
        std::to_string(plan.overBudgetGap) +
        " mm apart. Raise the limits, name a pair {a, b}, or sweep a smaller "
        "assembly.";
    out["truncated"] = true;
    out["pairs_unchecked"] = plan.overBudget;
    out["truncation"] = truncation;
  }

  // The same facts as a RECEIPT (core/Receipt.h). The fields above have been
  // honest for a long time, but a caller has to know to read `truncated`
  // before believing `clash: false` — and the drone round's "EMPTY exact
  // clash sweep" is what that costs. `standing` makes it unmissable: a
  // truncated sweep is UNVERIFIED, not clean.
  receipt::Receipt r;
  const receipt::Oracle oracle{"designer/exact-clash", "1"};
  const bool foundClash = out.value("clash", false);
  if (plan.overBudget != 0 || notChecked != 0) {
    r.Unverifiable(foundClash ? "no pair interferes (a clash was already "
                                "found, and the sweep did not finish)"
                              : "no pair interferes",
                   oracle, truncation);
  } else {
    receipt::Claim c("no pair interferes", oracle,
                     foundClash ? receipt::Verdict::Fail
                                : receipt::Verdict::Pass);
    c.basis = receipt::Basis::Verified; // a real boolean on the real B-Rep
    c.value = static_cast<double>(plan.exact.size());
    c.unit = "pairs";
    c.note = "every candidate pair was checked exactly; " +
             std::to_string(plan.boxCleared) +
             " more were cleared by bounding box";
    r.Add(std::move(c));
  }
  AttachReceipt(out, r);
}

// WHERE an overlap is, for anything that reports one. A pair name and a
// volume identify the two parts and nothing about which of their features
// are in each other; on two plates carrying three bosses and a pin apiece
// that is a guessing game, and it cost four build-and-look cycles on the
// carburettor round. Omitted entirely when the pair merely touches or is
// clear, so its presence means "there is solid in solid, here".
// What a MESH check relieves of a designed overlap: nothing. Triangles carry
// no threads, grips or bands, so an engaged thread, a ring in its groove and
// a press fit all read as the overlap they are, and only the exact check
// tells them from a clash.
constexpr const char* kMeshRelievesNothing =
    "nothing: an engaged thread, a ring in its groove and a press fit read as "
    "the overlap they are; mode exact relieves them";

// The press fits a contact check relieved, and the round overlaps it would
// not (docs/tolerances/README.md#press-fits): a fit is not a clash, and a
// clash next to a banded face says why it is not a fit.
json PressFitsJson(const Measure::SetContact& c) {
  json out = json::array();
  for (const Measure::SetContact::PressFit& f : c.pressFits)
    out.push_back({{"fit", f.fit},
                   {"diameter_mm", f.diameter},
                   {"interference_mm", f.interference},
                   {"max_interference_mm", f.maxInterference},
                   {"engaged_length_mm", f.length},
                   {"volume_mm3", f.volume}});
  return out;
}
json FitsRefusedJson(const Measure::SetContact& c) {
  json out = json::array();
  for (const Measure::SetContact::FitRefused& f : c.fitsRefused) {
    json r{{"fit", f.fit}, {"reason", f.reason}};
    if (f.maxInterference != 0.0 || f.interference != 0.0) {
      r["interference_mm"] = f.interference;
      r["max_interference_mm"] = f.maxInterference;
    }
    if (f.excess > 0.0)
      r["excess_mm"] = f.excess;
    out.push_back(std::move(r));
  }
  return out;
}
// Onto a pair's entry: what was relieved as a press fit, and why a round
// overlap was not. Omitted when there is neither.
void AddPressFits(json& j, const Measure::SetContact& c) {
  if (!c.pressFits.empty()) {
    j["press_fits"] = PressFitsJson(c);
    j["press_fit_volume"] = c.pressFitVolume;
  }
  if (!c.fitsRefused.empty())
    j["press_fit_refused"] = FitsRefusedJson(c);
}

void AddOverlapSite(json& j, const Measure::SetContact& c) {
  j["engaged_thread_pairs"] = c.engagedThreadPairs;
  j["thread_engagement_volume"] = c.threadEngagementVolume;
  j["ring_grip_pairs"] = c.ringGripPairs;
  j["ring_grip_volume"] = c.ringGripVolume;
  j["press_fit_pairs"] = c.pressFits.size();
  AddPressFits(j, c);
  if (!c.overlaps)
    return;
  remote::AddOverlapSite(j, c.centroid, c.overlapMin, c.overlapMax);
}
const char* ContactState(const Measure::SetContact& c, double touchTol) {
  if (c.volume > 1e-9)
    return "clash";
  return c.distance <= touchTol ? "touching" : "clear";
}

// Round-8 retro: connected components of a part's bodies (largest first) —
// the compact core of contact_report's body scope, reused by
// check_geometry's automatic floating-bodies warning so the class is
// opt-OUT instead of opt-in.
std::vector<std::vector<std::string>>
BodyComponentNames(const Document& doc, double touchTol, double window) {
  struct Ent {
    const Body* body;
    std::vector<Body> single;
    glm::dvec3 min, max;
  };
  std::vector<Ent> ents;
  for (const Body& b : doc.GetBodies()) {
    Ent e{&b, {}, {}, {}};
    e.single.push_back(b);
    if (Measure::WorldBounds(e.single, glm::dmat4(1.0), e.min, e.max))
      ents.push_back(std::move(e));
  }
  std::vector<size_t> parent(ents.size());
  for (size_t i = 0; i < parent.size(); ++i)
    parent[i] = i;
  const std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
    return parent[i] == i ? i : parent[i] = find(parent[i]);
  };
  for (size_t i = 0; i < ents.size(); ++i)
    for (size_t j = i + 1; j < ents.size(); ++j) {
      const glm::dvec3 gap = glm::max(
          glm::max(ents[i].min - ents[j].max, ents[j].min - ents[i].max),
          glm::dvec3(0.0));
      if (glm::length(gap) > window)
        continue;
      // The TRIANGLES decide it, not a boolean. This is a yes/no — are these
      // two bodies part of the same lump — and paying `ContactBetween` for it
      // means an OCC boolean plus BRepExtrema on every pair whose bounding
      // boxes overlap. A bicycle wheel's rim, spoked hub and tyre are
      // concentric, so all three pairs qualified, and `check_geometry` on one
      // wheel took 96 s of which 0.2 was the validity check it is named
      // after. The mesh answer is `distance` +/- `tolerance` (the sum of the
      // two deflections), so "not clearly apart" is the connective test, and
      // it errs toward CONNECTED — a false alarm about floating geometry is
      // worse than a missed one at a tenth of a millimetre, and
      // contact_report gives the exact gaps when they matter.
      bool joined = false;
      const Measure::Target ta{&ents[i].single, glm::dmat4(1.0), {}};
      const Measure::Target tb{&ents[j].single, glm::dmat4(1.0), {}};
      const collide::Gap mesh = Measure::MeshGap(ta, tb);
      if (mesh.known) {
        joined = mesh.distance <= touchTol + mesh.tolerance;
      } else {
        // No triangles to ask (a shape that will not tessellate): the exact
        // answer is the only one there is.
        const auto c = Measure::ContactBetween(ents[i].single, glm::dmat4(1.0),
                                               ents[j].single, glm::dmat4(1.0));
        joined = c && (c->volume > 1e-9 || c->distance <= touchTol);
      }
      if (joined)
        parent[find(i)] = find(j);
    }
  std::map<size_t, std::vector<std::string>> comps;
  for (size_t i = 0; i < ents.size(); ++i)
    comps[find(i)].push_back(ents[i].body->name);
  std::vector<std::vector<std::string>> out;
  for (auto& [root, names] : comps)
    out.push_back(std::move(names));
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  return out;
}

} // namespace

// Inspection RPCs (section clip, interference, mass properties, measure,
// element listing) carved out of RemoteServer::HandleSemantic. `docIn` is
// null on an assembly/drawing tab.
// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc InspectionMethods[] = {
    {"check_geometry",
     "Did the model build? {ok, valid, body_count, "
     "feature_errors, bodies:[...B-Rep checks]}. On a multi-body part it "
     "also warns about FLOATING bodies: `connected` and `body_components` "
     "are decided off the tessellations (the answer `interference "
     "mode=mesh` gives, within the sum of the two deflections), because it "
     "is a yes/no riding on a cheap check — contact_report on the same tab "
     "gives the exact gaps. Naming a `body` checks that one and skips the "
     "connectivity pass entirely.",
     "body?:int", remote::NeedsPart,
     "ok, valid, "
     "bodies[]{body,name,valid,bad_faces,bad_edges,bad_wires,reasons}, "
     "body_count, "
     "feature_errors[]{index,name,error,after_earlier_failure?}, "
     "receipt{schema,standing,summary,claims}, "
     "stale_derived?[]{id,name,source}, "
     "templates_changed?[]{id,name,template,revision}, body_components?, "
     "connected?, warnings?"},
    {"check_print",
     "Will this PART print? (docs/tolerances/README.md#checking-a-print) "
     "Six items over its bodies (or `bodies`: names or creating-feature "
     "ids) as ONE print along `direction` (default [0,0,1], the bed the "
     "plane below it through the origin), each {status: pass|fail|skipped, "
     "the measured value, the limit, note}: `bed` (the lowest point within "
     "1 µm of the bed), `shells` (one piece per body: bodies within 1 µm "
     "of each other are one piece, and a print-in-place pair that touches "
     "fails), `overhang` (downward faces steeper than `max_overhang_deg` "
     "FROM VERTICAL, 0 a wall and 90 a ceiling: angles from the B-Rep's "
     "surface normals, so a 45° flank reads 45; area past it in mm², the "
     "worst angle and where), `bridges` (downward faces within 1° of "
     "horizontal off the bed: each span, its inscribed diameter in mm, "
     "against `max_bridge_mm`), `clearance` (the minimum distance between "
     "any two bodies in mm against the gap a side, passing down to a tenth "
     "under it) and `chords` (the STL chord deviation export_stl meshes at "
     "against a fifth of the gap a side). Every limit defaults to the "
     "part's print process (set_print_process): the process's overhang and "
     "bridge, the profile's smallest gap a side and its STL deflection; an "
     "unprinted part skips what has no limit unless it is stated. `ok` is "
     "every item that ran passing. Read-only; `part` reads any part from "
     "any tab.",
     "part?:string|int, bodies?:any[], direction?:number[3], "
     "max_overhang_deg?:number>0<90, overhang_allowance_mm2?:number>=0, "
     "max_bridge_mm?:number>0, min_gap_mm?:number>0, "
     "stl_deflection_mm?:number>0",
     remote::NoFlags,
     "ok, process, direction, bodies[]{body,name,solids,watertight,"
     "lowest_mm}, bed{status,lowest_mm,tol_mm,contact_area_mm2,on_bed}, "
     "shells{status,pieces,expected,watertight,joined,split}, "
     "overhang{status,limit_deg,limit_from,allowance_mm2,area_mm2,"
     "worst_deg,worst?,faces[],note}, bridges{status,limit_mm,limit_from,"
     "worst_span_mm,area_mm2,faces[],note}, clearance{status,required_mm,"
     "required_from,tolerance_mm,min_mm?,pairs[],pruned,note}, "
     "chords{status,deflection_mm,deflection_from,limit_mm,note}, "
     "receipt{schema,standing,summary,claims}"},
    {"check_fit",
     "ISO 286 limits and fits. {hole:{class:\"H7\"}} alone gives that "
     "class's band; adding {shaft:{class:\"g6\"}} gives the fit between "
     "them — `kind` (clearance|transition|interference) and the signed "
     "`min_clearance`/`max_clearance`, where NEGATIVE is interference. The "
     "nominal comes from `size`, or is MEASURED when a side also names a "
     "face ({feature_id, element, part?}) — what the kernel built is what "
     "gets made. Two measured faces that disagree are REFUSED naming both: "
     "an ISO fit is defined on one common basic size, so a clearance "
     "modelled into the geometry AND asked for as a class is counted twice. "
     "`intent` adds a verdict. Carried: sizes to 500 mm, IT4-IT13, shafts "
     "d e f g h js k m n p and their uppercase holes — but shaft `k` only at "
     "IT4-IT7 (it is `h` elsewhere), and holes K/M/N only to IT8 and P to "
     "IT7, which is as far as the standard defines the delta correction they "
     "need. Past that it REFUSES naming the limit, because dropping delta "
     "anyway is wrong by exactly one grade step. Worst case over independent "
     "limits — no distribution, no form or position error. "
     "`part` (on a side, or shared) resolves faces against another part "
     "without switching tab, so a bore and its shaft can live in two. "
     "A PRINTED part (set_print_process) is judged at what it PRINTS: a "
     "measured face there carries the process allowance in its model, so "
     "its basic size is the modelled one less the allowance (`size_from: "
     "printed`), and `print` says for each printed side what to MODEL — "
     "\"H7 on a printed Ø8 bore (fdm petg): model Ø8.2\" — whether it is "
     "(`compensated`) and whether the class is finer than the process holds. "
     "A side with no face may say {print: {process, material}} to ask before "
     "there is a model.",
     "hole, intent, part, shaft, size", remote::NoFlags,
     "size, size_from, hole{class,lower,upper,min,max,it_grade,it}, "
     "measured?{hole?,shaft?}, "
     "shaft?{class,lower,upper,min,max,it_grade,it}, kind?, min_clearance?, "
     "max_clearance?, note?, intent?, intent_ok?, print?{hole?,shaft?}, "
     "print_notes?"},
    {"list_print_profiles",
     "Your print profiles, from ~/.config/lobole/print_profiles/*.json "
     "(docs/tolerances/README.md#print-profiles): each a named copy of an "
     "allowance table for one printer, filament and slicer preset — {name, "
     "process, material, conditions, stl_deflection_mm (the chord deviation "
     "a printed part's STL is exported at: the stated one, else a tenth of "
     "the smallest gap a side), stl_deflection_stated, layer_height_mm? "
     "(the slicer preset's layer), z_gap_mm? (the vertical gap between "
     "parts printed one over another that comes apart, whole layers; both "
     "absent when not stated), rows[]{kind, "
     "from_mm, to_mm (absent = and up), allowance_mm, source}}. A printed "
     "part names one (set_print_process {profile}) and EMBEDS a copy. "
     "`defaults` are the built-in tables, read-only, that a new profile "
     "starts from (get_print_process's `tables`).",
     "", remote::NoFlags,
     "profiles[]{name,process,material,conditions?,stl_deflection_mm,"
     "stl_deflection_stated,layer_height_mm?,z_gap_mm?,rows[]{kind,from_mm,"
     "to_mm?,allowance_mm,"
     "source?}}, defaults[]{process,material,conditions,rows[]}, dir"},
    {"save_print_profile",
     "Write a print profile to your store {profile: {name, process, "
     "material, conditions?, stl_deflection_mm?, layer_height_mm?, "
     "z_gap_mm? (whole layers of layer_height_mm), rows?: [{kind: hole|shaft|"
     "internal_thread|external_thread|gap, from_mm?, to_mm? (absent = and "
     "up), allowance_mm (signed, on the DIAMETER; a gap's is the whole "
     "diametral clearance), source?}]}}, replacing one of that name; without "
     "`rows` it starts as the built-in table of its process and material, "
     "the defaults to correct. Or {from_part: true, part?} saves the part's "
     "embedded copy (\"Save as mine\"). Refused: no name, an unknown "
     "process/material or kind (naming the accepted ones), a size range "
     "empty, overlapping or out of order within its kind, an allowance of "
     "the wrong sign for its kind (a hole/internal thread not negative, a "
     "gap positive, a shaft/external thread not positive), a z_gap_mm "
     "without a layer_height_mm or not whole layers of it (naming the two "
     "nearest that are), or a name that "
     "would share a file with another. Open parts keep their copy; "
     "get_print_process then reports `match: differs`.",
     "from_part, part, profile", remote::NotInBatch,
     "name, process, material, conditions?, stl_deflection_mm, "
     "stl_deflection_stated, layer_height_mm?, z_gap_mm?, rows[]"},
    {"delete_print_profile",
     "Remove a print profile from your store {name}. A part printed with it "
     "keeps its copy and reports `match: missing`.",
     "name", remote::NotInBatch},
    {"set_print_process",
     "Declare how this PART is printed: {process, material} — `fdm` with "
     "`petg` or `pla`, the built-in tables, the defaults — or {profile}, one "
     "of YOUR print profiles (list_print_profiles), whose copy the part "
     "embeds and reads from then on — or {clear: true} for a part that is "
     "not printed. Picking the same profile again after it changed in the "
     "store replaces the copy (\"use mine\"). It lives on the part because "
     "a part is made one way, "
     "whichever machine or tab opens it. The one geometry it drives is a "
     "Hole's clearance fit, modelled at the fit's size plus the hole "
     "allowance (M6 normal in PETG: Ø6.8 to print Ø6.6), so those holes "
     "rebuild; otherwise it changes what check_fit, check_fits and the Thread "
     "reports say to MODEL, since a printer makes a hole smaller than its "
     "model and a shaft larger (a PETG Ø8 H7 bore: model Ø8.2). An unknown "
     "process, or a material the table has no data for, is REFUSED naming "
     "what is carried — never borrowed from a neighbour. Undoable. The "
     "answer lists every Thread on the part with what it should be modelled "
     "at. `part` (name, stable id or slot index) sets it on any part, not "
     "only the active one, in that part's undo history.",
     "clear, material, part, process, profile", remote::NoFlags,
     "print{process,material,profile?,match?,conditions?,rows?,"
     "layer_height_mm?,z_gap_mm?,stl_deflection_mm}, "
     "threads[]{feature_id,name,designation,"
     "side,modeled,known,pitch,allowance_mm,modelled_major,prints_major,"
     "want_major?,model_major?,model_face?,notes,sentence,confirm}"},
    {"get_print_process",
     "This part's print process (null when it is not printed) and the whole "
     "allowance table: per process and material, the signed allowance ON "
     "THE DIAMETER for a hole, a shaft, an internal and an external thread "
     "(model = basic + allowance), the size range each row covers, the "
     "conditions it assumes and where each number comes from. Every number "
     "is community practice and CAD-tool defaults, to be confirmed with a "
     "test coupon (generators/print_coupon.py) and kept in a print profile. "
     "A part printed with a profile reports its name, the rows of its "
     "embedded copy and `match` against your store: same, differs (your "
     "profile of that name says something else: set_print_process {profile} "
     "uses yours, save_print_profile {from_part} keeps the part's) or "
     "missing. `stl_deflection_mm` is what export_stl meshes it at. A "
     "printed part also lists its Threads with what to model for each.",
     "part", remote::Deferrable,
     "print{process,material,profile?,match?,conditions?,rows?,"
     "layer_height_mm?,z_gap_mm?,stl_deflection_mm}, "
     "tables[]{process,material,conditions,"
     "repeatability_mm,repeatability_source,min_reliable_thread_mm,rows[]{"
     "kind,from_mm,to_mm,allowance_mm,source}}, confirm, threads?[]"},
    {"print_allowance",
     "What to MODEL for one printed diameter: {kind: hole|shaft|"
     "internal_thread|external_thread|gap, size_mm} (the size it should "
     "PRINT at) or, for a thread, {designation: \"M8\"} for the table's "
     "major. A `gap` is two surfaces printed together that must come apart "
     "(print-in-place): size_mm is the inner one's diameter, model_mm the "
     "outer's, and allowance_mm the whole diametral clearance. "
     "The process is the part's (set_print_process), {process, material} "
     "given here, or {profile}, one of yours. Answers the signed "
     "`allowance_mm` on the diameter, `model_mm`, the row's source, and what "
     "the STL export takes off a round surface at `stl_deflection_mm` (when "
     "omitted, the one export_stl meshes this print at). Drives no "
     "geometry: a generator applies it where it models the part. Refused: "
     "an unknown kind, process, material or profile, naming what is "
     "accepted.",
     "designation, kind, material, part, process, profile, size_mm, "
     "stl_deflection_mm",
     remote::Deferrable,
     "profile?, process, material, kind, basic_mm, allowance_mm, model_mm, "
     "source, "
     "confirm, stl_deflection_mm, stl_chord_loss_mm, stl_note, designation?, "
     "pitch?, note?"},
    {"set_tolerance",
     "Record what a face's SIZE is allowed to vary by: {feature_id, element, "
     "class: \"H7\"} — or {lower, upper} in mm for limits no class "
     "expresses, never both. Only a round face has a size of its own, and "
     "anything else is refused saying so. Adding {b: {feature_id, element}} "
     "tolerances "
     "the DISTANCE between the two faces instead (a shoulder-to-seat length, "
     "a slot width), which takes lower/upper and REFUSES a class: a class "
     "says how a shaft or a bore may vary, not how far apart two faces are. "
     "Design intent only: it moves no surface, so it rebuilds nothing. One "
     "per subject — a face's own size, and each ordered pair of faces — so "
     "setting a second REPLACES the first. The class is validated against "
     "the face's measured size when you set it, not when something later "
     "looks. `part` (name, stable id or slot index) records it on ANY "
     "part — a generator need not switch tabs — in that part's own undo "
     "history; without it, the active part.",
     "b, class, comment, element, feature_id, lower, part, upper",
     remote::NoFlags},
    {"list_tolerances",
     "Every tolerance on this part, each tagged `kind: size|distance`, with "
     "the limits it implies at the measured nominal — the face's diameter, "
     "or the distance between the two faces. A ref whose element no longer "
     "resolves is listed `resolved: false` rather than dropped: the intent "
     "outlives the topology that carried it.",
     "part", remote::NoFlags,
     "tolerances[]{id,feature_id,element,kind,resolved,comment?,b?,lower?,"
     "upper?,class?,nominal?,min?,max?,error?}"},
    {"clear_tolerance",
     "Remove one tolerance by {id}, from the active part or `part` (name, "
     "stable id or slot index).",
     "id, part", remote::NoFlags},
    {"stack_up",
     "Worst case along a STATED chain: {terms: [{tolerance: id, subtract?}]} "
     "gives the summed nominal and the min/max the bands allow. The chain is "
     "stated and not inferred — a mate names the two faces that TOUCH, but a "
     "stack alternates a contact and a length inside a part, and nothing says "
     "which of a part's distances lies along the axis. What it does NOT "
     "account for is correlation: two lengths driven by one variable cancel "
     "rather than add, and a tolerance's nominal is measured, so there is no "
     "path back to the variable that drove it. The terms must be independent, "
     "and the answer says so. A term may also be a bare tolerance id when it "
     "adds. Refused: fewer than two terms (one tolerance is a tolerance, and "
     "summing it would dress a lookup up as an analysis), an unknown id, and "
     "a term whose geometry no longer resolves — rather than counting that "
     "one as zero and silently shortening the chain.",
     "part, terms", remote::NeedsPart},
    {"check_fits",
     "Every fit this assembly declares, checked. A round mate already names "
     "the two faces that mate, so nothing has to pair them up: where both "
     "faces carry a tolerance, the fit is computed. The mate type is the "
     "intent — a joint that MOVES (revolute, slider, cylindrical) cannot "
     "have an interference fit, so that combination FAILS and a transition "
     "fit warns. A FASTENED round joint is `reported` and not judged: "
     "nothing-moves is equally true of a shrunk-on gear and of a bracket "
     "bolted through a clearance hole, and the two want opposite fits. "
     "Reaches into sub-assemblies, naming each side by its path. Anything "
     "not fully answerable is `unverified` WITH the reason and never a pass — "
     "an anchor that is a frame rather than a face, one side untoleranced, "
     "two faces of different basic size, a side stating explicit deviations "
     "(which say nothing about which side is the bore), or both sides the "
     "same case. The summary counts each kind apart. A side on a PRINTED "
     "part is at the size it prints (its model carries the allowance) and "
     "carries `print` with what to model; one modelled at the bare size is "
     "unverified as two basic sizes, and the reason says what to model.",
     "", remote::NoFlags,
     "fits[]{mate,mate_name,mate_type,status,reason?,size?,hole?{occurrence,"
     "class,min,max,print?},shaft?{...},kind?,"
     "min_clearance?,max_clearance?,note?}, "
     "status, passed, warned, failed, reported, unverified, "
     "receipt{schema,standing,summary,claims}"},
    {"mass_properties",
     "Mass (`mass_g`)/volume/center_of_mass/bbox per body and total (density "
     "from "
     "materials). The bbox here is MEASURED off the surfaces; get_state's is "
     "a cheap conservative bound that can read ~60% over on a swept surface. "
     "`part` reads another part without switching to it; from an assembly tab "
     "with no `part`, aggregates every visible occurrence in world space.",
     "part?:string|int, body?:int", remote::NoFlags,
     "bodies[]{body, name, volume, surface_area, center_of_mass, inertia, "
     "material, bbox_min?, bbox_max?, density?, mass_g}, total{volume, "
     "center_of_mass, mass_g}"},
    // This row said "point/distance/angle/radius" for a year and the handler
    // implemented distance. `describe` is what an agent trusts INSTEAD of
    // guessing, so a row that promises a branch nobody wrote is the
    // "refuse, don't substitute" rule inverted — we refuse the call and
    // advertise the capability. It now names exactly what runs.
    {"measure",
     "Geometry queries over stable element names. Each of {a} and {b} names "
     "one thing: {feature_id, element} for a face or edge, {feature_id} alone "
     "for a whole BODY, or {occurrence: id|name|\"Unit/Part\"} for a whole "
     "placed part — and each carries its own {part} or {occurrence}, so a "
     "measurement can span two parts or two occurrences. {a}: that thing's "
     "own measure (face area / edge length + shape kind + radius/diameter; a "
     "body reports volume, surface area and bounding size). {a,b}: distance, "
     "closest points and a SIGNED delta, plus overlap_volume/clash for two "
     "whole shapes. distance_kind: minimum (default) | maximum (clearance) | "
     "centres. kind:'angle' or 'both' adds the angle, taken between the "
     "outward normals when both are planar faces (adjacent box faces read 90, "
     "opposite ones 180) and unsigned in 0-90 when either direction's sign is "
     "arbitrary, as a line edge's is; `basis` says which. "
     "mode: exact (default) or mesh — a minimum distance off the "
     "TRIANGLES, +/- the `tolerance_mm` it reports (the two meshes' "
     "deflections summed, since each can err its own way), for when an "
     "exact "
     "one is pathological (two lofted propellers 151 mm apart take 55 s, "
     "because every face pair is equally far and nothing prunes).",
     "a:object, b?:object, kind?:'distance'|'angle'|'both', "
     "distance_kind?:string, mode?:'exact'|'mesh', part?:string|int"},
    {"frame",
     "The frame of a thing — origin, unit x/y/z, and a column-major 4x4 "
     "`matrix` — in PART space on a part tab, or in WORLD space with "
     "{occurrence}. {frame: <Frame id|name>} = a named datum frame (v86); "
     "{feature_id, element} = a planar face (centroid + outward normal + "
     "in-plane x), a cylindrical/conical/spherical face or a circular edge "
     "(centre + axis); {occurrence: id|name|path} alone = that occurrence's "
     "placement, and with frame / feature_id+element = that anchor placed "
     "in the world. Nothing = the part origin. This is exactly the frame a "
     "mate connector with the same spec uses, so a script positions "
     "against it and never transcribes a coordinate.",
     "part?:string|int, frame?:string|int, feature_id?:int>=0, "
     "element?:string, occurrence?:string|int"},
    {"geometry_snapshot",
     "Immutable, bounded edge measurements in part coordinates. Requires a "
     "stable part_id and body index; never switches tabs. fields defaults to "
     "center,radius; circular filters circles and arcs. max_elements bounds "
     "the edges scanned, before analysis. Revision is process/document scoped.",
     "part_id, body, circular, fields, max_elements"},
    {"validate_snapshot",
     "Check a geometry snapshot revision. Stale snapshots raise a structured "
     "error; this does not refresh or silently resolve old measurements.",
     "revision"},
    {"list_elements",
     "Stable face/edge names per body (the toponaming handles "
     "measure/fillet/mates use). Bounded by max_faces (default 4000, the "
     "faces it would DESCRIBE — each is an OCC surface analysis): pass "
     "body: <index> for one body of a many-bodied part. `unnamed_faces` says "
     "how many faces the body has that the element map never named: they are "
     "not in this list and CANNOT be picked, threaded, filleted or fitted, "
     "so a zero there is what makes the rest of the list a complete answer "
     "rather than merely a long one. Edges are named only when something "
     "needs them (a plain box has six named faces and no named edges), so "
     "there is no such count for them. "
     "Every face and edge says what MADE it (`made_by`, a feature id) and "
     "its `role`: start_cap | end_cap (a cut's floor) | side (swept from "
     "sketch `entity`) | generated (a fillet's or chamfer's face, or one a "
     "boolean made along a seam) | bore | bottom | "
     "counterbore | counterbore_floor | countersink | point (a Hole's; "
     "point is a blind floor's drill point) | outside | "
     "inner_side | outer_side | shield (a generated bearing's; its bore is "
     "`bore`) | ring_side (a generated retaining ring's; its inside and "
     "outside are `bore` and `outside`) | groove_floor | groove_flank (a "
     "RetainingRingGroove's) | positional "
     "(a feature that names by index: shell, loft, sweep, import...) | "
     "boundary (named from its edges; no role) | pole | seam; plus "
     "`instance` for a pattern copy and `derived` for one come in through "
     "Derived features. Filter by them: `made_by` (feature id or name; a "
     "Template instance stands for every feature it expanded into, and "
     "one of those ids is accepted on its own), "
     "`role` (also `floor` = end_cap, `walls` = side), `entity`, and `via` "
     "(a Derived feature: then `made_by` names a feature of ITS source "
     "part) -- the faces a feature made in a role, instead of a filter by "
     "normal and position. A filter that matches nothing says why. "
     "{part, configuration: name|id} lists the part as it is in that "
     "configuration -- what a placement or a job part in it is made of. Each "
     "edge lists the `faces` it joins, by name: the edges where a top "
     "meets its walls are the ones whose faces include one of each.",
     "part?:string|int, configuration?:string|int, body?:int, entity?:int>=0, "
     "made_by?:string|int, role?:string, via?:string|int, max_faces?:int>=0",
     remote::NoFlags,
     "bodies[]{body,name,created_by,job_part,faces[]{index,name,made_by,"
     "role,entity?,instance?,derived?,kind,area,centroid,normal,radius?,"
     "axis?},edges[]{...,faces},unnamed_faces}, matched_faces?, "
     "matched_edges?, "
     "why?"},
    {"curve_points",
     "The polyline a Curve3D actually BUILT, sampled to `deflection_mm` "
     "(default 0.1) with the arc length of each point from the start. A bent "
     "path's `points` are the CORNERS and the metal turns SHORT of them — "
     "with a 152 mm former on a right angle, 63 mm short — so anything asking "
     "where a member really runs has to reconstruct the arcs, and a caller "
     "that reconstructs them owns a second copy of the geometry that can "
     "drift from this one. `curve` names the Curve3D; omit it on a part with "
     "exactly one and it is taken.",
     "curve, deflection_mm, part", Deferrable,
     "curve, name, kind, length_mm, points, at_mm"},
    {"tube_schedule",
     "The cut list and the bend schedule for every tube MEMBER of the part. "
     "It walks the BODIES: a member is a length of tube that EXISTS, so a "
     "curve whose tube was consumed is not one, and a member a pattern or a "
     "mirror made is (half a symmetric frame used to go unordered). Identical "
     "members collapse into one row carrying `quantity` and the `bodies` it "
     "stands for. `cut_length_mm` is the METAL, measured along the member's "
     "own path, so a coped end comes off it and `coped_off_mm` says how much; "
     "`path_length_mm` is the node-to-node centreline it was swept along, "
     "which is 8-9 mm longer per coped end and not what a saw is set to. Per "
     "bend: the angle, the former's radius, the free straight before it and "
     "the ROLL — how far the tube turns about its own axis between one bend "
     "and the next. A bend is `tight` against the MATERIAL and the WALL when "
     "the part declares its stock (set_tube_rules, and check_tube is the full "
     "answer); without that it falls back to a bare R < 1.5 x D and says so "
     "in `tight_rule`. `curve` narrows it to one path. The sheet side's "
     "answer to the same question is list_bends.",
     "part, curve", Deferrable,
     "tubes[]{curve, name, quantity, bodies, cut_length_mm, path_length_mm, "
     "length_mm, bends, coped_off_mm?, outer_diameter_mm?, wall_mm?}, "
     "count, members"},
    {"interference",
     "Pairwise check between bodies or occurrences: {clash, volume, distance, "
     "state: clash|touching|clear}. touch_tol (mm, default 0.001) splits "
     "touching from clear; the no-pair assembly sweep stays clash-only — use "
     "contact_report for the full picture. The no-pair sweep reports its "
     "findings under BOTH `pairs` and `clashes` — they are the same list, "
     "because it only ever reports clashes, and contact_report spells the "
     "same thing `clashes`. That sweep is BOUNDED: pairs whose "
     "boxes do not overlap cannot clash and cost one box test, the rest are "
     "checked closest-first up to max_pairs (4000) or deadline_s (20 s, "
     "checked between pairs — a single boolean can overrun it) and the "
     "response "
     "says what it checked, box-cleared and left unchecked. A part INSIDE a "
     "sub-assembly is "
     "addressed by path — \"Gangway/Board 19\", or the id chain \"2/17\" "
     "when a name repeats — which is the only way to ask a question that "
     "spans the boundary (read-only; a sub-assembly is edited on its tab). "
     "mode: exact (default — a real boolean, and the clash VOLUME) or "
     "mesh (triangles + an AABB tree: ~10x faster, a yes/no and no "
     "volume, decided to the reported deflection_mm; re-run exact on "
     "anything that must be certain). On an assembly every pair names its "
     "sides `a_name`/`b_name` (and `a`/`b`, the same labels) plus "
     "`a_id`/`b_id` when both are occurrences of this assembly — the "
     "vocabulary run_motion_study and contact_report use. WHERE: an exact "
     "clash (named pair or sweep) carries `centroid_mm` and "
     "`bbox_min_mm`/`bbox_max_mm`, the overlap solid's centre and world "
     "bounds in millimetres; a mesh check knows only THAT they overlap, so "
     "its sweep entries carry `within_min_mm`/`within_max_mm` instead — the "
     "region the two sides' boxes share, which holds the overlap somewhere. "
     "DESIGNED overlaps are relieved by the exact check and reported apart "
     "(docs/inspect/README.md#designed-overlaps): an engaged thread "
     "(`engaged_thread_pairs`), a ring in its groove (`ring_grip_pairs`) "
     "and a PRESS FIT — two coaxial round faces, one inside the other, whose "
     "size bands (a tolerance, a bearing's ISO 492 band, read at the printed "
     "size on a printed part) allow the interference: a named pair gives "
     "`press_fit_pairs` and `press_fits` [{fit, diameter_mm, "
     "interference_mm, max_interference_mm, engaged_length_mm, "
     "volume_mm3}], the exact sweep a top-level `press_fits` [{a, b, fits, "
     "volume_mm3}]. A round overlap beside a banded face that is NOT a fit "
     "(past the band, a clearance fit, no band on one side, off-axis) stays "
     "a clash with `press_fit_refused` [{fit, reason, excess_mm?}]. Mesh "
     "mode relieves none of these, and says so in `relieves`.",
     "a?:string|int, b?:string|int, mode?:'exact'|'mesh', max_pairs?:int>=0, "
     "deadline_s?:number, touch_tol?:number"},
    {"contact_report",
     "Contact graph: connected components (len 1 = one connected "
     "structure), per-pair state/distance, near_misses (gap <= near_tol, "
     "default 1.0 mm), clashes. near_tol and touch_tol are both MILLIMETRES "
     "(touch_tol defaults to 0.001, and splits touching from clear). Bounded "
     "like the interference sweep "
     "(closest-first up to max_pairs 4000 / deadline_s 20 s, with the coverage "
     "in the "
     "response). Assembly tab: over occurrences, plus "
     "intended-vs-realized mate diffs when mates exist. Part tab: over the "
     "part's BODIES — catches a bracket floating inside one part, which "
     "the occurrence-level sweep cannot see. Each CLASH also carries "
     "`centroid_mm` and `bbox_min_mm`/`bbox_max_mm` (world millimetres): "
     "WHERE the solid-in-solid is, so a pair of names and a volume is not "
     "the whole answer. A pair whose "
     "two shapes carry more than `exact_face_budget` faces between them "
     "(default 20000 — an imported mesh is one face per TRIANGLE) is NOT "
     "checked and is listed in `not_checked` with its face count: the "
     "deadline stops new dispatches and cannot preempt a boolean already "
     "running, so one such pair does not slow the sweep down, it hangs it. "
     "A PRESS FIT the bands allow is relieved, not a clash: its pair is "
     "`touching` and carries `press_fits`, and `press_fits` lists every one "
     "(docs/tolerances/README.md#press-fits); a clash beside a banded round "
     "face carries `press_fit_refused` saying why it is not a fit. "
     "mode 'mesh' screens every pair on the tessellation first: a pair "
     "whose triangles cross goes to the exact boolean, and one whose "
     "triangles do not has its exact distance measured, which settles it "
     "without a boolean when it is positive and neither side is inside "
     "the other (`checked`: distance | boolean per pair, `mesh_cleared` "
     "the count settled by distance). The answer is the exact one either "
     "way: same states, distances, clashes and press fits. {async: true} runs "
     "it as a background job (`contact` in "
     "background_jobs, one at a time) and answers `job`: contact_status "
     "{job} reports progress and then the report, cancel_background_job "
     "{kind: 'contact'} stops it. The job is cancelled when the connection "
     "that started it closes, unless `outlive_client: true`.",
     "async, deadline_s, exact_face_budget, max_pairs, mode, near_tol, "
     "outlive_client, touch_tol",
     remote::NoFlags,
     "connected, clash, components, pairs[]{a, b, state, distance, volume, "
     "checked?, press_fits?}, "
     "near_misses[]{a, b, distance}, clashes[]{a, b, volume, centroid_mm, "
     "bbox_min_mm, bbox_max_mm, press_fit_refused?}, press_fits[]{a, b, "
     "fits, volume_mm3}, touch_tol, "
     "near_tol, mode, mesh_cleared?, deflection_mm?, pairs_checked, "
     "elapsed_s, receipt, scope?, bodies?, occurrences?, not_checked?, "
     "started?, running?, job?, kind?, outlives_client?"},
    {"contact_status",
     "An async contact_report by its `job` id, from any tab: {job, kind, "
     "label, running: true, fraction, elapsed_seconds, cancellable, "
     "cancel_requested, outlives_client} while it runs; {running: false, "
     "outcome (done|cancelled|failed), seconds, error?, cancel_reason?, "
     "report?} once it has ended, `report` being exactly what the "
     "synchronous call would have answered (a cancelled run keeps none). "
     "An id that names no contact job is refused, naming those there are.",
     "job", remote::NoFlags,
     "job, kind, running, fraction?, label?, elapsed_seconds?, "
     "cancellable?, cancel_requested?, outlives_client?, outcome?, "
     "seconds?, error?, cancel_reason?, report?"},
    {"query_geometry",
     "Declarative element query with a built-in assertion: filter faces/"
     "edges by geometry_type, normal/axis (+angle_tol_deg), radius "
     "(+radius_tol), area/length bounds, near_point (+max_distance); "
     "expected_count makes the result carry ok true/false. Covers UNNAMED "
     "elements too; matches carry stable names where they exist. Every "
     "measure here is MILLIMETRES — radius, radius_tol, max_distance, "
     "min_length, max_length — with areas in mm2 (min_area, max_area) and "
     "angle_tol_deg in degrees, as its name says.",
     "angle_tol_deg, axis, body, element_type, expected_count, geometry_type, "
     "max_area, max_distance, max_length, max_results, min_area, min_length, "
     "near_point, normal, part, radius, radius_tol"},
    {"compare_geometry",
     "Boolean-diff similarity: {golden_path} (STEP/BREP reference) vs the "
     "active doc, or {a, b} bodies/occurrences. `metrics`: \"all\" (the "
     "default) or \"volume\" — the overlap metrics cost THREE OCC booleans, "
     "which on a part carrying toroidal fillets is seconds (13.2 s for one "
     "filleted brace against 0.03 s for a tube with none), so a caller "
     "reading volume_ratio should ask for \"volume\" and get size, "
     "placement and the face/edge census in milliseconds. In that mode "
     "iou/dice/precision/recall and the diff volumes are ABSENT rather than "
     "zero, and the receipt claims the volume rather than the shape. "
     "Returns iou/dice/precision/"
     "recall, volume_ratio, center_offset, bbox_iou, topology counts and "
     "plain-language verdicts; {missing_path}/{extra_path} export the diff "
     "solids as BREP. materialize:true DRAWS them on the part instead — "
     "missing red, extra green — so one screenshot shows WHERE it differs "
     "(part tab only; clear_geometry_delta turns it off). "
     "{align: true} puts the candidate's centre of mass on the reference's "
     "first, so the answer is about the SHAPE and not about where it sits: "
     "a re-drawn tube whose volume matched to four figures scored iou 0.333 "
     "because the reference was 900 mm away. Off by default — a golden taken "
     "AT a position is a real thing to check — and `center_offset` still "
     "says how far it moved.",
     "a, align, b, extra_path, golden_path, materialize, metrics, "
     "missing_path"},
    {"clear_geometry_delta",
     "Remove the compare_geometry materialize overlay, leaving the plain "
     "model. Harmless when there is none.",
     ""},
    {"section",
     "Cross-section area where a plane cuts the bodies; on an assembly tab, "
     "every visible occurrence in place. `offset` is MILLIMETRES along the "
     "plane's normal; omit it to cut through the "
     "middle of what is shown (the chosen offset comes back). "
     "{regions: true} also returns each connected ISLAND the cut crosses "
     "({area, centroid}, biggest first) and `region_count`. The total cannot "
     "tell you whether the material is where you think it is: a barrel whose "
     "bolt bosses hang off its deck flange sections at 3293.8 where the sound "
     "one sections at 2586.9, and the difference is a boss standing as its "
     "own island. Islands are not a verdict — a fork has two tines — they are "
     "a question you can ask cheaply. {loops: true} adds each island's "
     "OUTLINE — its wires, the outer one flagged, as segments of kind "
     "line | arc (centre and radius) | other with their endpoints. An area "
     "and a centroid say a brace is 34,466 mm2 somewhere near the middle; "
     "recovering the SHAPE of a part you did not draw — the whole job when "
     "porting one in — needs the boundary, and without it a brace was "
     "measured in screenshots and then redrawn to a rule. "
     "{enclosed: true} adds `enclosed_area` (mm2): the area the cut RINGS "
     "rather than crosses, with the bodies' cut faces joined first -- four "
     "separate wall bodies meeting at the corners enclose the floor "
     "between them, a plate encloses its bore; a ring with a gap (a door "
     "or window at that height) encloses 0 and says so in "
     "`enclosed_note`. {bodies: [names or feature ids]} cuts only those bodies "
     "of a "
     "part (the outer walls, not the slab under them); an unknown one is "
     "refused naming the bodies there are.",
     "base_plane, bodies, enclosed, loops, normal, offset, origin, part, "
     "regions"},
    {"set_section",
     "Enable/disable the live section clip {base_plane: xy|xz|yz, offset?, "
     "reverse?}. Works on part AND assembly tabs, with filled cut caps on "
     "both; `offset` is MILLIMETRES along the normal, and omitting it cuts "
     "through the middle of what is shown. The DEFAULT keeps the half below "
     "the plane (z <= offset for xy, y <= for xz, x <= for yz) and reverse "
     "keeps the half above; the response says which as `keeps` (e.g. "
     "\"y <= 0\"). Look at the cut from the side that was removed — seen "
     "from the kept side it looks like no cut at all. "
     "Shorthand for one cut — add_section/update_section address several, "
     "and cut on a datum plane (plane_id) or tilted (tilt_u/tilt_v) for a "
     "plane that is not a base plane.",
     "add, base_plane, enabled, offset, reverse"},
    {"add_section",
     "Add a section cut and return its id {base_plane: xy|xz|yz, plane_id? "
     "(a datum plane), offset?, tilt_u?, tilt_v? (degrees), reverse?}. Up to "
     "four; they persist with the project. Like set_section, the default "
     "keeps the half opposite the plane's normal (below: z <= offset for "
     "xy, y <= for xz, x <= for yz) and reverse keeps the other half.",
     "base_plane, id, offset, plane_id, reverse, tilt_u, tilt_v, visible"},
    {"list_sections",
     "Every section cut: id, source (base plane / datum / face, with "
     "resolved:false when a named datum/face is gone — such a cut stops "
     "cutting), offset, tilt, reverse, visible, and the cut plane "
     "{normal, d}.",
     "", remote::NoFlags,
     "sections[]{id,offset,tilt_u,tilt_v,reverse,visible,normal,d,source}"},
    {"update_section",
     "Change one cut {id, base_plane?, plane_id?, offset? (mm along the "
     "normal), tilt_u?, tilt_v? (degrees), reverse? (false keeps the half "
     "opposite the normal, true the half it points into), visible?}. Only "
     "the fields present are touched.",
     "base_plane, id, offset, plane_id, reverse, tilt_u, tilt_v, visible"},
    {"remove_section", "Remove one cut {id}, or all of them with {all: true}.",
     "all, id"},
};
std::span<const MethodDoc> InspectionMethodDocs() { return InspectionMethods; }

// A printed side of a fit on the wire
// (docs/tolerances/README.md#printed-parts).
json PrintedDiameterJson(const ui::PrintedDiameter& d, const std::string& cls) {
  json j{{"process", d.process.process},      {"material", d.process.material},
         {"kind", d.hole ? "bore" : "shaft"}, {"basic", d.basic},
         {"allowance_mm", d.allowance},       {"model", d.model},
         {"sentence", d.Sentence(cls)},       {"source", d.source},
         {"confirm", printfit::ConfirmNote()}};
  if (d.modelled) {
    j["modelled"] = *d.modelled;
    j["prints"] = d.Prints();
    j["compensated"] = d.Compensated();
  }
  if (!d.bandNote.empty())
    j["band_note"] = d.bandNote;
  return j;
}

json PrintedThreadJson(const ui::PrintedThread& t) {
  json j{{"feature_id", t.featureId},
         {"name", t.name},
         {"designation", t.designation},
         {"side", t.external ? "external" : "internal"},
         {"modeled", t.modeled},
         {"known", t.known},
         {"pitch", t.pitch},
         {"allowance_mm", t.allowance},
         {"modelled_major", t.modelledMajor},
         {"prints_major", t.printsMajor},
         {"notes", t.notes},
         {"sentence", t.Sentence()},
         {"confirm", printfit::ConfirmNote()}};
  if (t.known) {
    j["want_major"] = t.wantMajor;
    j["model_major"] = t.modelMajor;
    j["model_face"] = t.modelFace;
  }
  return j;
}

namespace {

json PrintTablesJson() {
  json tables = json::array();
  for (const printfit::Table& t : printfit::Tables()) {
    json rows = json::array();
    for (const printfit::Row& r : t.rows) {
      json row{{"kind", printfit::PrintedName(r.kind)},
               {"from_mm", r.fromMm},
               {"allowance_mm", r.allowanceMm},
               {"source", r.source}};
      row["to_mm"] = r.toMm >= 1e8 ? json(nullptr) : json(r.toMm);
      rows.push_back(std::move(row));
    }
    const printfit::Process pr{t.process, t.material};
    tables.push_back(
        {{"process", t.process},
         {"material", t.material},
         {"conditions", t.conditions},
         {"repeatability_mm", printfit::ProcessRepeatabilityMm(pr)},
         {"repeatability_source", printfit::ProcessRepeatabilitySource(pr)},
         {"min_reliable_thread_mm", printfit::MinReliableThreadMm(pr)},
         {"rows", std::move(rows)}});
  }
  return tables;
}

// A print process on the wire: the pair, and with a profile its name, the
// embedded copy's rows and how it stands against the store.
json ProcessJson(const ui::AppState& state,
                 const std::optional<printfit::Process>& p) {
  if (!p)
    return nullptr;
  json j{{"process", p->process}, {"material", p->material}};
  if (p->profile) {
    j["profile"] = p->profile->name;
    j["match"] = printfit::ProfileMatchName(
        printfit::MatchProfile(state.printProfiles, *p));
    if (!p->profile->table.conditions.empty())
      j["conditions"] = p->profile->table.conditions;
    json rows = json::array();
    for (const printfit::Row& r : p->profile->table.rows)
      rows.push_back(DocumentIO::PrintRowToJson(r));
    j["rows"] = std::move(rows);
    if (p->profile->layerHeightMm > 0.0)
      j["layer_height_mm"] = p->profile->layerHeightMm;
    if (p->profile->zGapMm > 0.0)
      j["z_gap_mm"] = p->profile->zGapMm;
  }
  j["stl_deflection_mm"] = printfit::StlDeflectionMm(*p);
  return j;
}

// The note a report adds when a part's profile is not the store's: the
// answer came from the part's own copy, and the user's says otherwise (or
// is not there).
std::string ProfileMatchNote(const ui::AppState& state,
                             const printfit::Process& p) {
  switch (printfit::MatchProfile(state.printProfiles, p)) {
  case printfit::ProfileMatch::Differs:
    return "modelled with a different '" + p.profile->name +
           "' than yours: answered from the part's copy (set_print_process "
           "{profile} uses yours)";
  case printfit::ProfileMatch::Missing:
    return "'" + p.profile->name +
           "' is not one of your print profiles: answered from the part's "
           "copy (save_print_profile {from_part: true} keeps it)";
  case printfit::ProfileMatch::Builtin:
  case printfit::ProfileMatch::Same:
    break;
  }
  return {};
}

// {process, material} -- a built-in table -- or {profile}, one of the
// user's: the one parser set_print_process, print_allowance and check_fit
// share. Refused, naming what is accepted; never substituted.
template <class Params>
printfit::Process ProcessFromParams(const ui::AppState& state, const Params& p,
                                    const std::string& method) {
  if (p.contains("profile")) {
    if (p.contains("process") || p.contains("material"))
      throw std::runtime_error(
          method + ": `profile` or {process, material}, not both — a "
                   "profile already says its process and material");
    const std::string name = p.at("profile").template get<std::string>();
    const printfit::Profile* mine =
        printfit::FindProfile(state.printProfiles, name);
    if (mine == nullptr) {
      std::string known;
      for (const printfit::Profile& pr : state.printProfiles)
        known += (known.empty() ? "" : ", ") + pr.name;
      throw std::runtime_error(
          method + ": no print profile called '" + name + "'. " +
          (known.empty() ? std::string("You have none yet: "
                                       "save_print_profile makes one")
                         : "Yours: " + known));
    }
    return printfit::FromProfile(*mine);
  }
  printfit::Process proc{p.at("process").template get<std::string>(),
                         p.at("material").template get<std::string>(),
                         std::nullopt};
  if (const std::string why = printfit::Validate(proc); !why.empty())
    throw std::runtime_error(method + ": " + why);
  return proc;
}

json PrintProfileWire(const ui::AppState& state, const printfit::Profile& pr) {
  json j = DocumentIO::PrintProfileToJson(pr);
  j["stl_deflection_mm"] = printfit::StlDeflectionMm(printfit::FromProfile(pr));
  j["stl_deflection_stated"] = pr.stlDeflectionMm > 0.0;
  (void)state;
  return j;
}

// Every thread a printed part carries, reported.
json PrintedThreadsJson(const Document& part) {
  json out = json::array();
  for (size_t i = 0; i < part.FeatureCount(); i++)
    if (const auto* th = dynamic_cast<const ThreadFeature*>(part.GetFeature(i)))
      if (const auto r = ui::PrintedThreadFor(part, *th))
        out.push_back(PrintedThreadJson(*r));
  return out;
}

// --- contact_report
// (docs/inspect/README.md#clashes-contact-and-floating-parts)
//
// Two halves, so the second can run as a job: GATHER (main thread: the
// tab's bodies, placed in world space, their boxes, the mates) and SWEEP
// (pure: the pairs, the union-find, the response). An async report gathers
// DEEP COPIES (CADShape::DetachedUnlocated), because the frame may mesh the
// document's own shapes while the worker reads them
// (docs/ui/long-operations.md).

// One node of the contact graph: a body on a part tab, a top-level
// occurrence's whole flattened subtree on an assembly tab.
struct ContactEnt {
  std::string name;
  uint64_t id = 0;          // the occurrence's; 0 on the bodies scope
  std::vector<Body> bodies; // placed in world space
  glm::dvec3 min{0.0}, max{0.0};
  size_t faces = 0; // how heavy an exact boolean against it would be
};

struct ContactSweep {
  bool bodiesScope = false;
  bool mesh = false; // mode: "mesh" — screen on triangles first
  std::vector<ContactEnt> ents;
  json skipped = json::array();
  double touchTol = 1e-3, nearTol = 1.0, deadline = 20.0;
  size_t maxPairs = 4000, faceBudget = 20000;
  // For the intended-vs-realized diff (assembly scope, mates present).
  struct MateRef {
    std::string name;
    uint64_t id = 0, a = 0, b = 0;
  };
  std::vector<MateRef> mates;
  std::map<uint64_t, std::string> occNames;
  bool copies = false; // the bodies are this sweep's own (an async run)
  // Tests only: how long each pair of an ASYNC run waits before it is
  // checked (DESIGNER_TEST_CONTACT_PAIR_MS), so a smoke can watch a run in
  // progress and cancel it before it ends, on any machine. A cancel ends
  // the wait.
  int pairDelayMs = 0;
};

// The parameters, read and checked before anything is gathered or started:
// a refused call changes nothing
// (docs/scripting/rpc.md#a-refused-call-changes-nothing).
ContactSweep ContactParams(const RpcParams& p) {
  ContactSweep sw;
  sw.touchTol = p.value("touch_tol", 1e-3);
  sw.nearTol = p.value("near_tol", 1.0);
  sw.maxPairs = p.value("max_pairs", size_t{4000});
  sw.deadline = p.value("deadline_s", 20.0);
  sw.faceBudget = p.value("exact_face_budget", size_t{20000});
  const std::string mode = p.value("mode", std::string("exact"));
  if (mode != "exact" && mode != "mesh")
    throw std::runtime_error("contact_report mode '" + mode +
                             "' is not one of: exact, mesh");
  sw.mesh = mode == "mesh";
  return sw;
}

// What the gather keeps of a body: the body itself on the synchronous path,
// a deep copy at the same placement on an async one.
Body ContactBody(const Body& b, const CADShape& placed, bool copy) {
  Body nb = b;
  nb.shape = copy ? placed.DetachedUnlocated().LocatedLike(placed) : placed;
  return nb;
}

void GatherContactBodies(ContactSweep& sw, const Document& doc) {
  sw.bodiesScope = true;
  for (const Body& b : doc.GetBodies()) {
    if (b.reference)
      continue; // a derived ghost is not this part's matter
    ContactEnt e;
    e.name = b.name;
    e.bodies.push_back(ContactBody(b, b.shape, sw.copies));
    if (!Measure::WorldBounds(e.bodies, glm::dmat4(1.0), e.min, e.max)) {
      sw.skipped.push_back(b.name);
      continue;
    }
    e.faces = static_cast<size_t>(e.bodies.front().shape.FaceCount());
    sw.ents.push_back(std::move(e));
  }
}

// A5: one node per TOP-LEVEL occurrence, whose geometry is its whole
// flattened subtree in world space. That is what makes "is the gangway
// attached to the dock" a question about two nodes and the hinges between
// them, instead of thirty parts — and the reason a clash BETWEEN two parts
// inside one sub-assembly does not appear here: it belongs to that
// assembly's own report, on its own tab.
void GatherContactOccurrences(ContactSweep& sw, ui::AppState& state) {
  std::map<uint64_t, std::vector<Body>> byRoot;
  for (const assembly::FlatInstance& inst : ui::FlattenActiveAssembly(state)) {
    if (!inst.visible)
      continue;
    for (const Body& b : inst.part->GetBodies()) {
      // A derived ghost is not the occurrence's matter: the gripper body
      // carrying a copy of the servo it was drawn on reported a 15 cm3
      // clash with that servo — its own ghost.
      if (b.reference)
        continue;
      byRoot[inst.rootOccurrenceId].push_back(
          ContactBody(b, b.shape.Transformed(inst.world), sw.copies));
    }
  }
  for (const Occurrence& o : state.assembly->GetOccurrences()) {
    sw.occNames[o.id] = o.name;
    if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
      continue;
    const auto it = byRoot.find(o.id);
    ContactEnt e;
    e.name = o.name;
    e.id = o.id;
    if (it != byRoot.end())
      e.bodies = it->second;
    if (e.bodies.empty() ||
        !Measure::WorldBounds(e.bodies, glm::dmat4(1.0), e.min, e.max)) {
      sw.skipped.push_back(o.name); // nothing resolvable under it
      continue;
    }
    for (const Body& b : e.bodies)
      e.faces += static_cast<size_t>(b.shape.FaceCount());
    sw.ents.push_back(std::move(e));
  }
  for (const Mate& mt : state.assembly->GetMates())
    sw.mates.push_back({mt.name, mt.id, mt.a.occurrenceId, mt.b.occurrenceId});
}

// The mesh screen: whether the tessellations of the two sides cross, or one
// lies inside the other (collide::Overlap). A pair whose triangles cross is
// overlapping to within the deflection and goes straight to the boolean,
// which weighs it; one whose triangles do not is asked for its exact
// distance first (Measure::ClearanceBetween), and needs no boolean when
// that proves it apart.
bool MeshCrosses(const ContactEnt& a, const ContactEnt& b, double& deflection) {
  for (const Body& x : a.bodies) {
    if (!x.shape.IsValid())
      continue;
    for (const Body& y : b.bodies) {
      if (!y.shape.IsValid())
        continue;
      const collide::Result r =
          collide::Overlap(x.shape, glm::dmat4(1.0), y.shape, glm::dmat4(1.0));
      deflection = std::max(deflection, r.deflection);
      if (r.intersects)
        return true;
    }
  }
  return false;
}

json RunContactSweep(ContactSweep& sw, const std::atomic<bool>* cancel,
                     std::atomic<double>* fraction) {
  std::vector<ContactEnt>& ents = sw.ents;
  // Every mesh built HERE, one thread, before the workers start: building
  // one writes a triangulation onto the shape, and the workers' extrema read
  // those shapes (core/Parallel.h). ExactDistance asks for them in both
  // modes.
  for (const ContactEnt& e : ents)
    for (const Body& b : e.bodies)
      if (b.shape.IsValid())
        collide::Warm(b.shape);

  std::vector<size_t> parent(ents.size());
  for (size_t i = 0; i < parent.size(); ++i)
    parent[i] = i;
  const std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
    return parent[i] == i ? i : parent[i] = find(parent[i]);
  };
  json pairs = json::array(), nearMisses = json::array(),
       clashes = json::array(), pressFits = json::array();
  std::set<std::pair<uint64_t, uint64_t>> touchingIds;
  // Closest first, and bounded: on a 253-occurrence arm this is 31,878
  // pairs, and the ones sharing a PCB all pass the box test.
  std::vector<glm::dvec3> mins, maxs;
  for (const ContactEnt& e : ents) {
    mins.push_back(e.min);
    maxs.push_back(e.max);
  }
  // The deadline stops new DISPATCHES; a boolean already running cannot be
  // preempted. That is fine between parts and useless against a mesh
  // import, where one pair is not slow but effectively unbounded: an STL
  // arrives as one planar face per triangle, and two 54k-face bodies took a
  // documented sweep past ten minutes with a deadline_s of 20 set. So the
  // heavy pairs are not started, and the report says which -- a sweep that
  // quietly never returns is worse than one that says what it did not check.
  json tooHeavy = json::array();
  const auto heavy = [&](size_t i, size_t j) {
    return sw.faceBudget > 0 && ents[i].faces + ents[j].faces > sw.faceBudget;
  };
  struct PairResult {
    std::optional<Measure::SetContact> contact;
    bool screened = false; // the mesh proved it clear: no boolean ran
    bool apart = false;    // the oriented boxes proved it out of range
  };
  std::atomic<double> deflection{0.0};
  std::atomic<size_t> screened{0};
  SweepPlan plan = PlanSweep(mins, maxs, sw.nearTol, sw.maxPairs);
  RunSweep(
      plan, sw.deadline,
      [&](size_t i, size_t j) -> PairResult {
        PairResult r;
        for (int waited = 0; waited < sw.pairDelayMs; waited += 10) {
          if (cancel != nullptr && cancel->load())
            break;
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (heavy(i, j))
          return r;
        // The bodies scope's second prefilter, for what the first cannot
        // reject: a long slanted member's axis-aligned box is mostly air,
        // so on a space frame nearly every pair survives it and pays for an
        // exact boolean. An oriented box follows the member.
        if (sw.bodiesScope &&
            Measure::SeparatedByOBB(ents[i].bodies, glm::dmat4(1.0),
                                    ents[j].bodies, glm::dmat4(1.0),
                                    sw.nearTol)) {
          r.apart = true;
          return r;
        }
        if (sw.mesh) {
          double d = 0.0;
          const bool crosses = MeshCrosses(ents[i], ents[j], d);
          double seen = deflection.load(std::memory_order_relaxed);
          while (d > seen && !deflection.compare_exchange_weak(
                                 seen, d, std::memory_order_relaxed))
            ;
          if (!crosses) {
            r.contact =
                Measure::ClearanceBetween(ents[i].bodies, glm::dmat4(1.0),
                                          ents[j].bodies, glm::dmat4(1.0));
            if (r.contact) {
              r.screened = true;
              screened.fetch_add(1, std::memory_order_relaxed);
              return r;
            }
          }
        }
        r.contact = Measure::ContactBetween(ents[i].bodies, glm::dmat4(1.0),
                                            ents[j].bodies, glm::dmat4(1.0));
        return r;
      },
      [&](size_t i, size_t j, const PairResult& r) {
        const auto sides = [&](json& o) {
          // The ids as well as the names: sixteen occurrences answer to
          // "ISO 4762 M3x10", and a reported pair that cannot say WHICH
          // one sends a caller hunting through list_occurrences.
          if (!sw.bodiesScope) {
            o["a_id"] = ents[i].id;
            o["b_id"] = ents[j].id;
          }
        };
        if (r.apart)
          return;
        if (!r.contact) {
          if (heavy(i, j)) {
            json h{{"a", ents[i].name},
                   {"b", ents[j].name},
                   {"faces", ents[i].faces + ents[j].faces},
                   {"why", "an exact boolean between these two would not "
                           "finish inside any deadline; measure them with "
                           "mode 'mesh', or raise exact_face_budget"}};
            sides(h);
            tooHeavy.push_back(std::move(h));
          }
          return;
        }
        const Measure::SetContact& c = *r.contact;
        const char* st = ContactState(c, sw.touchTol);
        json pair{{"a", ents[i].name}, {"b", ents[j].name}};
        sides(pair);
        pair["state"] = st;
        pair["distance"] = c.distance;
        pair["volume"] = c.volume;
        if (sw.mesh)
          pair["checked"] = r.screened ? "distance" : "boolean";
        AddPressFits(pair, c);
        pairs.push_back(std::move(pair));
        if (!c.pressFits.empty()) {
          json fit{{"a", ents[i].name}, {"b", ents[j].name}};
          sides(fit);
          fit["fits"] = PressFitsJson(c);
          fit["volume_mm3"] = c.pressFitVolume;
          pressFits.push_back(std::move(fit));
        }
        if (std::string_view(st) != "clear") {
          parent[find(i)] = find(j);
          touchingIds.insert({std::min(ents[i].id, ents[j].id),
                              std::max(ents[i].id, ents[j].id)});
        }
        if (std::string_view(st) == "clash") {
          json clash{{"a", ents[i].name}, {"b", ents[j].name}};
          sides(clash);
          clash["volume"] = c.volume;
          AddOverlapSite(clash, c);
          clashes.push_back(std::move(clash));
        } else if (c.distance > sw.touchTol && c.distance <= sw.nearTol) {
          json miss{{"a", ents[i].name}, {"b", ents[j].name}};
          sides(miss);
          miss["distance"] = c.distance;
          nearMisses.push_back(std::move(miss));
        }
      },
      cancel, fraction);

  std::map<size_t, std::vector<std::string>> comps;
  for (size_t i = 0; i < ents.size(); ++i)
    comps[find(i)].push_back(ents[i].name);
  std::vector<std::vector<std::string>> componentList;
  for (auto& [root, names] : comps)
    componentList.push_back(std::move(names));
  std::sort(componentList.begin(), componentList.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  json out = {{"connected", componentList.size() <= 1},
              {"components", componentList},
              {"pairs", pairs},
              {"near_misses", nearMisses},
              {"clashes", clashes},
              {"clash", !clashes.empty()},
              {"press_fits", pressFits},
              {"touch_tol", sw.touchTol},
              {"near_tol", sw.nearTol},
              {"mode", sw.mesh ? "mesh" : "exact"}};
  if (sw.mesh) {
    out["mesh_cleared"] = screened.load();
    out["deflection_mm"] = deflection.load();
  }
  AttachSweepCoverage(out, plan, ents.size(), sw.maxPairs, sw.deadline,
                      tooHeavy.size());
  if (sw.bodiesScope) {
    out.erase("occurrences");
    out["scope"] = "bodies";
    out["bodies"] = ents.size();
  }
  if (!sw.skipped.empty())
    out["skipped"] = sw.skipped;
  if (!tooHeavy.empty())
    out["not_checked"] = tooHeavy;
  // Intended vs realized: only meaningful when the assembly HAS mates — in
  // a fixed-occurrence assembly every touching pair is "unmated".
  if (!sw.bodiesScope && !sw.mates.empty()) {
    json unrealized = json::array(), unmated = json::array();
    std::set<std::pair<uint64_t, uint64_t>> mated;
    for (const ContactSweep::MateRef& mt : sw.mates) {
      const auto key =
          std::make_pair(std::min(mt.a, mt.b), std::max(mt.a, mt.b));
      mated.insert(key);
      if (touchingIds.find(key) == touchingIds.end())
        unrealized.push_back({{"mate", mt.name}, {"id", mt.id}});
    }
    for (const auto& [ia, ib] : touchingIds)
      if (mated.find({ia, ib}) == mated.end()) {
        const auto a = sw.occNames.find(ia), b = sw.occNames.find(ib);
        if (a != sw.occNames.end() && b != sw.occNames.end())
          unmated.push_back({{"a", a->second}, {"b", b->second}});
      }
    out["unrealized_mates"] = unrealized;
    out["unmated_contacts"] = unmated;
  }
  // An async run meshed its own copies; their cache entries go with them
  // (collide::Forget says why).
  if (sw.copies)
    for (const ContactEnt& e : ents)
      for (const Body& b : e.bodies)
        collide::Forget(b.shape);
  return out;
}

// An async contact_report: the sweep on a `contact` job. The report lands in
// AppState::contactReports by job id for contact_status to hand back.
uint64_t StartContactJob(ui::AppState& state, std::shared_ptr<ContactSweep> sw,
                         uint64_t owner) {
  struct Run {
    std::shared_ptr<ContactSweep> sweep;
    json report;
  };
  auto run = std::make_shared<Run>();
  run->sweep = std::move(sw);
  auto bg = std::make_unique<ui::BackgroundJob>();
  bg->kind = "contact";
  bg->label = std::string("Contact report (") +
              (run->sweep->bodiesScope ? "bodies" : "occurrences") + ", " +
              std::to_string(run->sweep->ents.size()) + ")";
  bg->cancellable = true;
  bg->owner = owner;
  bg->fraction.store(0.0); // the share of pairs checked: determinate
  ui::BackgroundJob* raw = bg.get();
  bg->thread = std::thread([run, raw]() {
    OSD::SetSignal(Standard_False);
    try {
      run->report = RunContactSweep(*run->sweep, &raw->cancel, &raw->fraction);
    } catch (const Standard_Failure& e) {
      raw->error =
          e.GetMessageString() ? e.GetMessageString() : "OpenCASCADE failure";
    } catch (const std::exception& e) {
      raw->error = e.what();
    }
    run->sweep.reset();    // the copies go on the worker, not on the frame
    raw->done.store(true); // last
  });
  bg->finish = [run, raw](ui::AppState& s) {
    // A cancelled run's report is a prefix nobody asked for: it ends as
    // `cancelled` (the registry records it) and keeps nothing.
    if (!raw->cancel.load() && !run->report.is_null()) {
      s.contactReports[raw->id] = std::move(run->report);
      // The last few, as finishedJobs keeps: a status asked just after the
      // end answers, and a session of async reports does not grow forever.
      while (s.contactReports.size() > 8)
        s.contactReports.erase(s.contactReports.begin());
    }
    return true;
  };
  return ui::StartBackgroundJob(state, std::move(bg)).id;
}

} // namespace

// "xy" | "xz" | "yz" -> 0 | 1 | 2. Anything else is REFUSED: every section
// handler used to read an unknown plane — "zx", "XY", "front" — as xy, and
// cut the model somewhere it was not asked to.
static int BasePlaneFromString(const std::string& bp) {
  if (bp == "xy")
    return 0;
  if (bp == "xz")
    return 1;
  if (bp == "yz")
    return 2;
  throw std::runtime_error("base_plane '" + bp + "' is not one of xy, xz, yz");
}

std::optional<json> HandleInspectionRpc(ui::AppState& state, Document* docIn,
                                        const std::string& m,
                                        const RpcParams& p) {
  Document* doc = docIn;
  if (m == "set_section") {
    // The viewport section cut. params: {enabled, base_plane: "xy"|"xz"|"yz",
    // offset?, reverse?}. Disabled clears every section. This edits the SAME
    // list the Section tool shows, so a script and the tool window are never
    // two different truths. (It once also promised {origin, normal}, in its
    // own description and here, and never had them.)
    if (!p.value("enabled", true)) {
      ui::ClearSections(state);
      return json::object();
    }
    if (p.contains("base_plane")) {
      const int basePlane =
          BasePlaneFromString(p.at("base_plane").get<std::string>());
      // One section unless asked otherwise: scripts that toggle a cut on and
      // off should not stack four of them up.
      const bool add = p.value("add", false);
      if (!add)
        ui::ClearSections(state);
      const uint64_t id = ui::AddSection(state, basePlane);
      if (id == 0)
        throw std::runtime_error(
            "at most " + std::to_string(kMaxClipPlanes) +
            " sections at once; remove one first (set_section enabled=false "
            "clears them)");
      ui::AppState::SectionView* sec = ui::FindSection(state, id);
      // No offset given: AddSection already centered it on what the tab
      // shows, which is the only offset guaranteed to cut something.
      if (p.contains("offset"))
        sec->offset = p.at("offset").get<float>();
      sec->reverse = p.value("reverse", false);
      // WHICH half stays, in words: the shaders drop what lies on the
      // normal's side, so the default keeps the half BELOW the plane. A cut
      // whose kept half faces the camera looks exactly like no cut at all.
      const char axis = basePlane == 1 ? 'y' : basePlane == 2 ? 'x' : 'z';
      char keeps[64];
      std::snprintf(keeps, sizeof keeps, "%c %s %.6g", axis,
                    sec->reverse ? ">=" : "<=", double(sec->offset));
      return json{{"id", id},
                  {"offset", sec->offset},
                  {"reverse", sec->reverse},
                  {"keeps", keeps}};
    }
    throw std::runtime_error(
        "set_section wants base_plane (\"xy\"|\"xz\"|\"yz\") with an "
        "optional offset/reverse, or enabled=false to clear");
  }
  if (m == "add_section" || m == "update_section") {
    const bool adding = m == "add_section";
    ui::AppState::SectionView* sec = nullptr;
    if (adding) {
      const int basePlane =
          BasePlaneFromString(p.value("base_plane", std::string("xy")));
      const uint64_t id = ui::AddSection(state, basePlane);
      if (id == 0)
        throw std::runtime_error("at most " + std::to_string(kMaxClipPlanes) +
                                 " sections at once; remove one first");
      sec = ui::FindSection(state, id);
    } else {
      sec = ui::FindSection(state, p.at("id").get<uint64_t>());
      if (sec == nullptr)
        throw std::runtime_error("no section with that id; list_sections "
                                 "shows the ones that exist");
      if (p.contains("base_plane")) {
        sec->basePlane =
            BasePlaneFromString(p.at("base_plane").get<std::string>());
        sec->planeId = 0; // a base plane and a datum are alternatives
        sec->face = ElementRef{};
      }
    }
    if (p.contains("plane_id")) {
      // A datum plane in the ACTIVE PART; the cut then follows it when the
      // datum moves, which is the only reason to name one.
      sec->planeId = p.at("plane_id").get<uint64_t>();
      sec->face = ElementRef{};
      if (sec->planeId != 0 && doc != nullptr &&
          doc->FindFeatureById(sec->planeId) == nullptr)
        throw std::runtime_error("no feature with that id in this part");
    }
    if (p.contains("offset"))
      sec->offset = p.at("offset").get<float>();
    if (p.contains("tilt_u"))
      sec->tiltU = p.at("tilt_u").get<float>();
    if (p.contains("tilt_v"))
      sec->tiltV = p.at("tilt_v").get<float>();
    if (p.contains("reverse"))
      sec->reverse = p.at("reverse").get<bool>();
    if (p.contains("visible"))
      sec->visible = p.at("visible").get<bool>();
    const glm::vec4 plane = ui::SectionPlaneOf(state, *sec);
    return json{{"id", sec->id},
                {"offset", sec->offset},
                {"tilt_u", sec->tiltU},
                {"tilt_v", sec->tiltV},
                {"reverse", sec->reverse},
                {"visible", sec->visible},
                {"normal", {plane.x, plane.y, plane.z}},
                {"d", plane.w}};
  }
  if (m == "list_sections") {
    json out = json::array();
    for (const ui::AppState::SectionView& sec : state.sections) {
      const glm::vec4 plane = ui::SectionPlaneOf(state, sec);
      json sj = {{"id", sec.id},
                 {"offset", sec.offset},
                 {"tilt_u", sec.tiltU},
                 {"tilt_v", sec.tiltV},
                 {"reverse", sec.reverse},
                 {"visible", sec.visible},
                 {"normal", {plane.x, plane.y, plane.z}},
                 {"d", plane.w}};
      // The source, named the way the tool window names it, so a script and
      // the window describe the same cut in the same words.
      if (sec.face.IsSet())
        sj["source"] =
            json{{"kind", "face"},
                 {"feature_id", sec.face.featureId},
                 {"element", sec.face.name},
                 // False = the face is gone, or this is an
                 // assembly tab; the cut is off until it resolves.
                 {"resolved", ui::SectionSourceResolved(state, sec)}};
      else if (sec.planeId != 0)
        sj["source"] =
            json{{"kind", "datum"},
                 {"plane_id", sec.planeId},
                 {"resolved", ui::SectionSourceResolved(state, sec)}};
      else
        sj["source"] = json{{"kind", "base_plane"},
                            {"base_plane", sec.basePlane == 1   ? "xz"
                                           : sec.basePlane == 2 ? "yz"
                                                                : "xy"}};
      out.push_back(std::move(sj));
    }
    return json{{"sections", out}};
  }
  if (m == "remove_section") {
    if (p.value("all", false)) {
      ui::ClearSections(state);
      return json::object();
    }
    const uint64_t id = p.at("id").get<uint64_t>();
    if (ui::FindSection(state, id) == nullptr)
      throw std::runtime_error("no section with that id; list_sections shows "
                               "the ones that exist");
    ui::RemoveSection(state, id);
    return json::object();
  }
  if (m == "section") {
    // Cross-section area where a plane cuts through all bodies (I3). Plane:
    // {base_plane: "xy"|"xz"|"yz", offset?: 0} OR {origin: [x,y,z],
    // normal: [x,y,z]}.
    glm::dvec3 origin(0.0), normal(0.0, 0.0, 1.0);
    double usedOffset = 0.0;
    bool derived = false;
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      if (bp != "xy" && bp != "xz" && bp != "yz")
        throw std::runtime_error("unknown base_plane '" + bp +
                                 "' (accepted: xy, xz, yz)");
      const int basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
      normal = basePlane == 1   ? glm::dvec3(0, 1, 0)
               : basePlane == 2 ? glm::dvec3(1, 0, 0)
                                : glm::dvec3(0, 0, 1);
      // Omitting `offset` means the same here as in set_section: the middle
      // of what this tab shows. Two readings of one word would be worse than
      // either.
      derived = !p.contains("offset");
      usedOffset =
          derived
              ? static_cast<double>(ui::CenteredSectionOffset(state, basePlane))
              : p.at("offset").get<double>();
      origin = normal * usedOffset;
    } else {
      const auto& o = p.at("origin");
      const auto& nrm = p.at("normal");
      origin = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
      normal = {nrm[0].get<double>(), nrm[1].get<double>(),
                nrm[2].get<double>()};
    }
    // On an ASSEMBLY tab the same question is about the assembled thing, so
    // every visible occurrence's bodies are placed by its solved transform
    // and the area is the sum over all of them. Refusing here (which it used
    // to do) makes "how much material does this cut cross?" unanswerable for
    // exactly the models where it matters most.
    std::vector<Body> placed;
    const std::vector<Body>* bodies = nullptr;
    // A read, so it reaches another part by reference rather than by
    // switching to it (docs/scripting/rpc.md#reads-name-their-part).
    Document* sectionDoc = doc;
    if (p.contains("part"))
      sectionDoc = ResolvePartParam(state, p.at("part"));
    if (p.contains("enclosed") && !p.at("enclosed").is_boolean())
      throw std::runtime_error("`enclosed` is true or false");
    if (p.contains("bodies") && sectionDoc == nullptr)
      throw std::runtime_error(
          "`bodies` picks bodies of a PART; on an assembly tab the section "
          "is of every visible occurrence (name the part with `part`)");
    if (sectionDoc != nullptr && p.contains("bodies")) {
      // Only the chosen bodies: the walls of a room and not the floor slab
      // under them, so what they enclose is the room.
      const std::set<uint64_t> chosen =
          ResolvePartBodies(*sectionDoc, p.at("bodies"), "bodies");
      for (const Body& b : sectionDoc->GetBodies())
        if (chosen.count(b.createdBy) != 0)
          placed.push_back(b);
      bodies = &placed;
    } else if (sectionDoc != nullptr) {
      bodies = &sectionDoc->GetBodies();
    } else if (state.assembly) {
      // A5: the leaf parts of the whole tree, placed.
      for (const assembly::FlatInstance& inst :
           ui::FlattenActiveAssembly(state)) {
        if (!inst.visible)
          continue;
        const glm::dmat4 xf = inst.world;
        for (const Body& b : inst.part->GetBodies()) {
          if (b.reference)
            continue; // a derived ghost is not the occurrence's matter
          Body nb = b;
          nb.shape = b.shape.Transformed(xf);
          placed.push_back(std::move(nb));
        }
      }
      bodies = &placed;
    } else {
      RequirePartDoc(sectionDoc, "section"); // the standard message
    }
    const auto area = Measure::CrossSectionArea(*bodies, origin, normal);
    if (!area)
      throw std::runtime_error(
          "section failed — the plane may cut no body at all; check the "
          "bodies' bbox_min/bbox_max in get_state against the plane");
    json out{{"area", *area}};
    if (derived)
      out["offset"] = usedOffset; // say where it chose to cut
    // The area the cut RINGS, not the area it crosses: the floor inside the
    // walls, the bore in the plate (docs/inspect/README.md#enclosed-area).
    if (p.value("enclosed", false)) {
      const auto enclosed =
          Measure::SectionEnclosedArea(*bodies, origin, normal);
      if (!enclosed)
        throw std::runtime_error("enclosed area failed -- see the log");
      out["enclosed_area"] = *enclosed;
      // Zero with material cut is the ring not closing HERE, which is worth
      // saying: a wall with a door or a window in it at this height is
      // open, and a plane above the openings encloses the room.
      if (*enclosed <= 0.0 && *area > 0.0)
        out["enclosed_note"] =
            "the cut material rings nothing at this plane: an opening (a "
            "door, a window, a gap between bodies) breaks the ring. Cut "
            "where it is whole, e.g. above the openings";
    }
    // The ISLANDS the cut crosses, biggest first. Off by default because the
    // total is what most callers want and this costs a second boolean.
    if (p.value("regions", false)) {
      const bool withLoops = p.value("loops", false);
      const auto regions =
          Measure::CrossSectionRegions(*bodies, origin, normal, withLoops);
      if (!regions)
        throw std::runtime_error("section regions failed — see the log");
      json arr = json::array();
      for (const Measure::SectionRegion& r : *regions) {
        json one{{"area", r.area},
                 {"centroid", {r.centroid.x, r.centroid.y, r.centroid.z}}};
        if (withLoops) {
          json loops = json::array();
          for (const Measure::SectionLoop& l : r.loops) {
            json segs = json::array();
            for (const Measure::SectionSegment& sg : l.segments) {
              json j{{"kind", sg.kind},
                     {"from", {sg.from.x, sg.from.y, sg.from.z}},
                     {"to", {sg.to.x, sg.to.y, sg.to.z}}};
              if (sg.kind == "arc") {
                j["center"] = {sg.center.x, sg.center.y, sg.center.z};
                j["radius"] = sg.radius;
              }
              segs.push_back(std::move(j));
            }
            loops.push_back({{"outer", l.outer}, {"segments", segs}});
          }
          one["loops"] = loops;
        }
        arr.push_back(std::move(one));
      }
      out["regions"] = arr;
      out["region_count"] = arr.size();
    }
    return out;
  }
  if (m == "interference") {
    // Clash detection (inspection I4). Part tab: two bodies interfere when
    // their intersection has volume — params: {a: index, b: index}. Active
    // ASSEMBLY tab: a/b are occurrence ids (numbers) or names (strings); each
    // occurrence's part bodies are placed by its solved occurrence transform
    // and the same check runs in world space. Headless-safe (pure geometry).
    if (doc == nullptr && state.assembly) {
      // A resolved side of the question: which occurrence, and its geometry
      // already placed in world space.
      struct Target {
        const Occurrence* occ = nullptr;
        std::vector<Body> bodies;
        std::string label;
      };
      const auto subtreeOf = [&](uint64_t rootId) {
        return ui::OccurrenceSubtreeBodies(state, rootId);
      };
      // One vocabulary with run_motion_study and contact_report: every pair
      // names its sides `a_name`/`b_name`, and carries `a_id`/`b_id` when
      // both are occurrences of THIS assembly (a path into a sub-assembly
      // names an inner occurrence, whose id means nothing up here). `a`/`b`
      // keep the label they always held.
      const auto nameSides = [&](json& j, const Target& ta, const Target& tb) {
        j["a_name"] = ta.label;
        j["b_name"] = tb.label;
        const auto top = [&](const Target& t) {
          return t.occ != nullptr &&
                 state.assembly->FindById(t.occ->id) == t.occ;
        };
        if (top(ta) && top(tb)) {
          j["a_id"] = ta.occ->id;
          j["b_id"] = tb.occ->id;
        }
      };
      auto targetOf = [&](const json& v) -> Target {
        Target t;
        if (v.is_number()) {
          t.occ = state.assembly->FindById(v.get<uint64_t>());
          if (t.occ == nullptr)
            throw std::runtime_error("occurrence id " + v.dump() +
                                     " not found (see list_occurrences)");
          t.label = t.occ->name;
          t.bodies = subtreeOf(t.occ->id);
          return t;
        }
        const std::string name = v.get<std::string>();
        for (const Occurrence& o : state.assembly->GetOccurrences())
          if (o.name == name) {
            t.occ = &o;
            t.label = o.name;
            t.bodies = subtreeOf(o.id); // a unit is its whole subtree
            return t;
          }
        // A5: a PATH into a sub-assembly ("Gangway/Board 19", or the id
        // chain "2/17"). Read-only, and the only way to ask a question that
        // spans the boundary — the board lives in the sub-assembly's
        // document and the hinge at the top, so no tab holds both.
        if (name.find('/') != std::string::npos) {
          auto hit = assembly::ResolvePath(*state.assembly, name,
                                           ui::PartResolverFor(state),
                                           ui::AssemblyResolverFor(state));
          if (!hit)
            throw std::runtime_error(hit.error());
          t.occ = hit->occ;
          t.label = name;
          // Its own geometry, at the COMPOSED transform the resolver
          // returned. Never re-derived from the id: ids are unique per
          // document, so an inner id collides with a top-level one.
          if (hit->occ->refKind == OccurrenceRef::Assembly) {
            if (const AssemblyDocument* sub =
                    ui::AssemblyDocById(state, hit->occ->partId)) {
              auto flat = assembly::Flatten(*sub, ui::PartResolverFor(state),
                                            ui::AssemblyResolverFor(state));
              if (flat)
                for (const assembly::FlatInstance& inst : *flat)
                  for (const Body& b : inst.part->GetBodies()) {
                    if (b.reference)
                      continue;
                    Body nb = b;
                    nb.shape = b.shape.Transformed(hit->world * inst.world);
                    t.bodies.push_back(std::move(nb));
                  }
            }
          } else if (const Document* pd =
                         ui::PartDocById(state, hit->occ->partId)) {
            for (const Body& b : pd->GetBodies()) {
              if (b.reference)
                continue;
              Body nb = b;
              nb.shape = b.shape.Transformed(hit->world);
              t.bodies.push_back(std::move(nb));
            }
          }
          return t;
        }
        throw std::runtime_error(
            "occurrence '" + name +
            "' not found (see list_occurrences; a part inside a "
            "sub-assembly is addressed by path, e.g. \"Gangway/Board 19\")");
      };
      // mode: "exact" (the default — a real boolean, and a volume) or "mesh"
      // (G3: triangles and an AABB tree; a yes/no, never a distance). A sweep
      // asks the same question over and over; a verdict asks it once.
      //
      // Called `mode` and not `method` on purpose: the python client takes
      // the RPC name as its own `method` argument, so a parameter of that
      // name can only be reached through request() — `help` and `describe`
      // are already stuck there and it is not a club to grow.
      const std::string mode = p.value("mode", std::string("exact"));
      if (mode != "exact" && mode != "mesh")
        throw std::runtime_error("interference mode '" + mode +
                                 "' is not one of: exact, mesh");
      // Written from every worker of the parallel sweep below, so it cannot
      // be a plain double: the pairs run on all cores and each wants to
      // widen the same maximum. A CAS loop, because atomic<double> has no
      // fetch_max.
      std::atomic<double> meshDeflection{0.0};
      const auto widenDeflection = [&meshDeflection](double d) {
        double seen = meshDeflection.load(std::memory_order_relaxed);
        while (d > seen && !meshDeflection.compare_exchange_weak(
                               seen, d, std::memory_order_relaxed))
          ;
      };
      const auto pairMesh = [&](const Target& ta, const Target& tb) {
        for (const Body& a : ta.bodies) {
          if (!a.shape.IsValid())
            continue;
          for (const Body& b : tb.bodies) {
            if (!b.shape.IsValid())
              continue;
            const collide::Result r = collide::Overlap(
                a.shape, glm::dmat4(1.0), b.shape, glm::dmat4(1.0));
            widenDeflection(r.deflection);
            if (r.intersects)
              return true;
          }
        }
        return false;
      };
      // The sweep's exact check: the overlap's volume AND where it is. The
      // same boolean InterferenceVolume runs, which weighs the intersection
      // it already built; the separation pass is not asked for.
      const auto pairContact = [&](const Target& ta, const Target& tb) {
        if (ta.bodies.empty() || tb.bodies.empty())
          throw std::runtime_error(
              "'" + (ta.bodies.empty() ? ta.label : tb.label) +
              "' resolves to no geometry (missing, hidden or empty)");
        if (const auto c = Measure::ContactBetween(ta.bodies, glm::dmat4(1.0),
                                                   tb.bodies, glm::dmat4(1.0),
                                                   /*wantDistance=*/false))
          return *c;
        // ContactBetween gives up on the set when one body pair's boolean
        // does not finish; InterferenceVolume skips that pair and weighs the
        // rest, which is the answer this sweep has always given. The site
        // is then not known, and is left out rather than guessed.
        const auto v = Measure::InterferenceVolume(ta.bodies, glm::dmat4(1.0),
                                                   tb.bodies, glm::dmat4(1.0));
        if (!v)
          throw std::runtime_error(
              "interference check failed — an OpenCASCADE boolean/extrema "
              "threw, usually on an invalid or empty body; run "
              "check_geometry on both sides");
        Measure::SetContact volumeOnly;
        volumeOnly.volume = *v;
        volumeOnly.distanceKnown = false;
        return volumeOnly;
      };
      if (!p.contains("a") && !p.contains("b")) {
        // No pair named: sweep every visible, unsuppressed pair — the
        // whole-assembly clash check a design review actually wants. Only
        // pairs whose boxes OVERLAP can clash, and only clashing pairs are
        // reported: this used to run a boolean for all 31,878 pairs of a
        // 253-occurrence arm and return every one of them.
        std::vector<Target> occs;
        for (const Occurrence& o : state.assembly->GetOccurrences()) {
          if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
            continue;
          Target t;
          t.occ = &o;
          t.label = o.name;
          t.bodies = subtreeOf(o.id);
          if (!t.bodies.empty())
            occs.push_back(std::move(t));
        }
        std::vector<glm::dvec3> mins(occs.size()), maxs(occs.size());
        std::vector<size_t> keep;
        for (size_t i = 0; i < occs.size(); ++i)
          if (Measure::WorldBounds(occs[i].bodies, glm::dmat4(1.0), mins[i],
                                   maxs[i]))
            keep.push_back(i);
        std::vector<glm::dvec3> km, kx;
        for (size_t i : keep) {
          km.push_back(mins[i]);
          kx.push_back(maxs[i]);
        }
        const size_t maxPairs = p.value("max_pairs", size_t{4000});
        const double deadline = p.value("deadline_s", 20.0);
        SweepPlan plan = PlanSweep(km, kx, 0.0, maxPairs);
        json pairs = json::array();
        bool any = false;
        // A pair's answer. In mesh mode a hit carries no volume and no
        // site: a volume measured off triangles is the TESSELLATION's
        // volume, and reporting one would invite exactly the false precision
        // this method is not for. Exact carries the overlap solid's.
        struct PairAnswer {
          bool clash = false;
          Measure::SetContact contact; // exact mode only
        };
        // The pairs the exact check relieved as press fits: not clashes, so
        // not in `pairs`, and listed apart so a clean sweep still says what
        // it took to be deliberate (docs/tolerances/README.md#press-fits).
        json pressFits = json::array();
        RunSweep(
            plan, deadline,
            [&](size_t ki, size_t kj) {
              const Target& ta = occs[keep[ki]];
              const Target& tb = occs[keep[kj]];
              PairAnswer a;
              if (mode == "mesh") {
                a.clash = pairMesh(ta, tb);
              } else {
                a.contact = pairContact(ta, tb);
                a.clash = a.contact.volume > 1e-9;
              }
              return a;
            },
            [&](size_t ki, size_t kj, const PairAnswer& a) {
              if (!a.contact.pressFits.empty()) {
                json fit{{"a", occs[keep[ki]].label},
                         {"b", occs[keep[kj]].label},
                         {"fits", PressFitsJson(a.contact)},
                         {"volume_mm3", a.contact.pressFitVolume}};
                nameSides(fit, occs[keep[ki]], occs[keep[kj]]);
                pressFits.push_back(std::move(fit));
              }
              if (!a.clash)
                return;
              any = true;
              json hit{{"a", occs[keep[ki]].label},
                       {"b", occs[keep[kj]].label},
                       {"clash", true}};
              nameSides(hit, occs[keep[ki]], occs[keep[kj]]);
              if (mode == "mesh") {
                AddOverlapBound(hit, glm::dvec3(glm::max(km[ki], km[kj])),
                                glm::dvec3(glm::min(kx[ki], kx[kj])));
              } else {
                hit["volume"] = a.contact.volume;
                if (a.contact.overlaps)
                  remote::AddOverlapSite(hit, a.contact.centroid,
                                         a.contact.overlapMin,
                                         a.contact.overlapMax);
                if (!a.contact.fitsRefused.empty())
                  hit["press_fit_refused"] = FitsRefusedJson(a.contact);
              }
              pairs.push_back(std::move(hit));
            });
        // Under BOTH names, and the duplication is the point. This sweep
        // only ever pushes clashing pairs, so its `pairs` holds what
        // contact_report calls `clashes` — while contact_report's own
        // `pairs` holds EVERY pair it looked at. One key, two meanings,
        // across two methods a caller uses interchangeably: reading
        // `clashes` off this response returned an empty list on a document
        // with 1,068,565 mm3 of one panel inside another, and the gate that
        // did it passed for a whole round.
        json out{{"clash", any},
                 {"pairs", pairs},
                 {"clashes", pairs},
                 {"mode", mode}};
        if (mode == "mesh") {
          out["deflection_mm"] = meshDeflection.load();
          out["relieves"] = kMeshRelievesNothing;
        } else {
          out["press_fits"] = pressFits;
        }
        // This sweep declines nothing: the face budget is contact_report's,
        // where a pair of imported meshes can hang a boolean.
        AttachSweepCoverage(out, plan, keep.size(), maxPairs, deadline);
        return out;
      }
      const Target ta = targetOf(p.at("a"));
      const Target tb = targetOf(p.at("b"));
      if (ta.occ == tb.occ)
        throw std::runtime_error("pick two different occurrences");
      const double touchTol = p.value("touch_tol", 1e-3);
      if (ta.bodies.empty() || tb.bodies.empty())
        throw std::runtime_error(
            "'" + (ta.bodies.empty() ? ta.label : tb.label) +
            "' resolves to no geometry (missing, hidden or empty)");
      if (mode == "mesh") {
        // The same yes/no the sweep gives, for ONE named pair: no volume,
        // no distance, and the deflection it was decided to.
        const bool hit = pairMesh(ta, tb);
        json out{{"clash", hit},  {"state", hit ? "clash" : "clear"},
                 {"mode", mode},  {"deflection_mm", meshDeflection.load()},
                 {"a", ta.label}, {"b", tb.label}};
        out["relieves"] = kMeshRelievesNothing;
        nameSides(out, ta, tb);
        return out;
      }
      const auto c = Measure::ContactBetween(ta.bodies, glm::dmat4(1.0),
                                             tb.bodies, glm::dmat4(1.0));
      if (!c)
        throw std::runtime_error(
            "interference check failed — an OpenCASCADE boolean/extrema "
            "threw, usually on an invalid or empty body; run "
            "check_geometry on both sides");
      json out{{"clash", c->volume > 1e-9},
               {"volume", c->volume},
               {"distance", c->distance},
               {"state", ContactState(*c, touchTol)},
               {"mode", mode},
               {"a", ta.label},
               {"b", tb.label}};
      nameSides(out, ta, tb);
      AddOverlapSite(out, *c);
      return out;
    }
    const auto& bodies = RequirePartDoc(doc, "interference").GetBodies();
    const double touchTol = p.value("touch_tol", 1e-3);
    const int ia = p.at("a").get<int>();
    const int ib = p.at("b").get<int>();
    if (ia < 0 || ib < 0 || ia >= static_cast<int>(bodies.size()) ||
        ib >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError(
          "body", ia < 0 || ia >= static_cast<int>(bodies.size()) ? ia : ib,
          bodies.size(), "get_state lists bodies"));
    std::vector<Body> va(1), vb(1);
    va[0] = bodies[ia];
    vb[0] = bodies[ib];
    const auto c =
        Measure::ContactBetween(va, glm::dmat4(1.0), vb, glm::dmat4(1.0));
    if (!c)
      throw std::runtime_error(
          "interference check failed — an OpenCASCADE boolean/extrema "
          "threw, usually on an invalid or empty body; run "
          "check_geometry on both sides");
    json out{{"clash", c->volume > 1e-9},
             {"volume", c->volume},
             {"distance", c->distance},
             {"state", ContactState(*c, touchTol)}};
    AddOverlapSite(out, *c);
    return out;
  }
  if (m == "query_geometry") {
    // Declarative geometry predicates with an assertion built in (survey
    // round 2, the expected_count idea): "there are 4 cylinder faces of
    // radius 3±0.01 with axis ±Z" is ONE call returning ok true/false —
    // instead of a zoo of named finders or measure-probing every element.
    // params: {part?, body?, element_type?: face|edge, geometry_type?,
    // normal? + angle_tol_deg?, axis? + angle_tol_deg?, radius? +
    // radius_tol?, min_area?/max_area?, min_length?/max_length?,
    // near_point? + max_distance?, expected_count?, max_results? (50)}
    const auto& bodies =
        p.contains("part") ? ResolvePartParam(state, p.at("part"))->GetBodies()
                           : RequirePartDoc(doc, "query_geometry").GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError("body", p.value("body", -1),
                                          bodies.size(),
                                          "get_state lists bodies"));
    const std::string elementType = p.value("element_type", "face");
    if (elementType != "face" && elementType != "edge")
      throw std::runtime_error("element_type must be face or edge");
    const bool wantFaces = elementType == "face";
    const std::string geomType = p.value("geometry_type", "");
    const double angleTol = glm::radians(p.value("angle_tol_deg", 5.0));
    const double radiusTol = p.value("radius_tol", 0.01);
    const size_t maxResults = p.value("max_results", 50u);
    const auto vec3 = [](const json& a) {
      return glm::dvec3(a.at(0).get<double>(), a.at(1).get<double>(),
                        a.at(2).get<double>());
    };
    std::optional<glm::dvec3> wantNormal, wantAxis, nearPoint;
    if (p.contains("normal"))
      wantNormal = glm::normalize(vec3(p.at("normal")));
    if (p.contains("axis"))
      wantAxis = glm::normalize(vec3(p.at("axis")));
    if (p.contains("near_point"))
      nearPoint = vec3(p.at("near_point"));
    const double maxDistance = p.value("max_distance", 1e30);
    std::optional<double> wantRadius, minArea, maxArea, minLength, maxLength;
    if (p.contains("radius"))
      wantRadius = p.at("radius").get<double>();
    if (p.contains("min_area"))
      minArea = p.at("min_area").get<double>();
    if (p.contains("max_area"))
      maxArea = p.at("max_area").get<double>();
    if (p.contains("min_length"))
      minLength = p.at("min_length").get<double>();
    if (p.contains("max_length"))
      maxLength = p.at("max_length").get<double>();

    size_t count = 0;
    json matches = json::array();
    constexpr int kElementCap = 20000; // runaway guard, not a real limit
    for (size_t bi = 0; bi < bodies.size(); bi++) {
      if (only >= 0 && bi != static_cast<size_t>(only))
        continue;
      // Stable names where toponaming assigned one; unnamed elements still
      // match (patterns aren't named yet) and are addressable by index.
      std::map<int, std::string> names;
      if (wantFaces)
        for (const auto& [idx, name] : bodies[bi].elementMap.names)
          names[idx] = name;
      else
        for (const auto& [idx, name] : bodies[bi].elementMap.edgeNames)
          names[idx] = name;
      for (int idx = 0; idx < kElementCap; idx++) {
        std::string kind;
        double area = 0.0, length = 0.0, radius = 0.0;
        bool hasRadius = false, hasAxis = false;
        glm::dvec3 point(0.0), normal(0.0), axis(0.0);
        if (wantFaces) {
          const auto fi = Measure::DescribeFaceAt(bodies[bi], idx);
          if (!fi)
            break;
          kind = fi->kind;
          area = fi->area;
          point = fi->centroid;
          normal = fi->normal;
          radius = fi->radius;
          hasRadius = fi->hasRadius;
          axis = fi->axis;
          hasAxis = fi->hasAxis;
        } else {
          const auto ei = Measure::DescribeEdgeAt(bodies[bi], idx);
          if (!ei)
            break;
          kind = ei->kind;
          length = ei->length;
          point = ei->midpoint;
          radius = ei->radius;
          hasRadius = ei->hasRadius;
          axis = ei->axis;
          hasAxis = ei->hasAxis;
        }
        if (!geomType.empty() && kind != geomType)
          continue;
        if (wantNormal && (glm::length(normal) < 0.5 ||
                           glm::dot(glm::normalize(normal), *wantNormal) <
                               std::cos(angleTol)))
          continue;
        // Axis sign is arbitrary (a cylinder axis has no outward), so |dot|.
        if (wantAxis &&
            (!hasAxis || std::abs(glm::dot(glm::normalize(axis), *wantAxis)) <
                             std::cos(angleTol)))
          continue;
        if (wantRadius &&
            (!hasRadius || std::abs(radius - *wantRadius) > radiusTol))
          continue;
        if (minArea && area < *minArea)
          continue;
        if (maxArea && area > *maxArea)
          continue;
        if (minLength && length < *minLength)
          continue;
        if (maxLength && length > *maxLength)
          continue;
        if (nearPoint && glm::length(point - *nearPoint) > maxDistance)
          continue;
        count++;
        if (matches.size() >= maxResults)
          continue; // keep counting, stop listing
        json entry = {
            {"body", bi},
            {"index", idx},
            {"kind", kind},
            {"created_by", bodies[bi].createdBy},
            {wantFaces ? "centroid" : "midpoint", {point.x, point.y, point.z}}};
        if (wantFaces) {
          entry["area"] = area;
          entry["normal"] = {normal.x, normal.y, normal.z};
        } else {
          entry["length"] = length;
        }
        if (hasRadius)
          entry["radius"] = radius;
        if (hasAxis)
          entry["axis"] = {axis.x, axis.y, axis.z};
        if (const auto it = names.find(idx); it != names.end())
          entry["element"] = it->second;
        matches.push_back(std::move(entry));
      }
    }
    json out = {{"count", count},
                {"matches", std::move(matches)},
                {"truncated", count > maxResults}};
    if (p.contains("expected_count")) {
      const size_t expected = p.at("expected_count").get<size_t>();
      out["expected_count"] = expected;
      out["ok"] = count == expected;
    }
    return out;
  }
  if (m == "compare_geometry") {
    // V4: boolean-diff similarity, the numeric "same shape?" oracle. The
    // REFERENCE (A) is the golden file when given, else body/occurrence `a`;
    // the CANDIDATE (B) is the active doc, else `b`. Metrics are arithmetic
    // over vol(A), vol(B), vol(A∩B); the verdict strings are what makes
    // them actionable for an agent.
    const auto bodiesOfDoc = [&]() -> std::vector<Body> {
      if (doc != nullptr)
        return doc->GetBodies();
      std::vector<Body> placed;
      if (state.assembly) {
        // A5: leaf parts of the whole tree, placed.
        for (const assembly::FlatInstance& inst :
             ui::FlattenActiveAssembly(state)) {
          if (!inst.visible)
            continue;
          for (const Body& b : inst.part->GetBodies()) {
            Body nb;
            nb.shape = b.shape.Transformed(inst.world);
            placed.push_back(std::move(nb));
          }
        }
      }
      return placed;
    };
    const auto loadGolden = [](const std::string& path) -> std::vector<Body> {
      std::vector<Body> out;
      const auto ext = std::filesystem::path(path).extension().string();
      if (ext == ".brep" || ext == ".brp") {
        CADShape s;
        if (!CADKernel::Instance().LoadBREP(path, s))
          throw std::runtime_error("cannot read BREP " + path);
        Body b;
        b.shape = s;
        out.push_back(std::move(b));
      } else {
        std::vector<CADShape> shapes;
        if (!CADKernel::Instance().LoadSTEP(path, shapes) || shapes.empty())
          throw std::runtime_error("cannot read STEP " + path);
        for (CADShape& s : shapes) {
          Body b;
          b.shape = s;
          out.push_back(std::move(b));
        }
      }
      return out;
    };
    std::vector<Body> refA, candB;
    if (p.contains("golden_path")) {
      refA = loadGolden(p.at("golden_path").get<std::string>());
      candB = bodiesOfDoc();
    } else if (doc != nullptr) {
      const auto& bodies = doc->GetBodies();
      const int ia = p.at("a").get<int>(), ib = p.at("b").get<int>();
      if (ia < 0 || ib < 0 || ia >= static_cast<int>(bodies.size()) ||
          ib >= static_cast<int>(bodies.size()))
        throw std::runtime_error(IndexError(
            "body", ia < 0 || ia >= static_cast<int>(bodies.size()) ? ia : ib,
            bodies.size(), "get_state lists bodies"));
      refA.emplace_back().shape = bodies[ia].shape;
      candB.emplace_back().shape = bodies[ib].shape;
    } else if (state.assembly) {
      const auto occBodies = [&](const json& v) {
        const Occurrence* found = nullptr;
        if (v.is_string()) {
          for (const Occurrence& o : state.assembly->GetOccurrences())
            if (o.name == v.get<std::string>())
              found = &o;
        } else {
          found = state.assembly->FindById(v.get<uint64_t>());
        }
        if (found == nullptr)
          throw std::runtime_error("occurrence " + v.dump() + " not found");
        const Document* pd = ui::OccurrencePartDoc(state, *found);
        if (pd == nullptr)
          throw std::runtime_error("occurrence part document is missing");
        std::vector<Body> placed;
        const glm::dmat4 xf = ui::OccurrenceMatrix(*found);
        for (const Body& b : pd->GetBodies()) {
          Body nb;
          nb.shape = b.shape.Transformed(xf);
          placed.push_back(std::move(nb));
        }
        return placed;
      };
      refA = occBodies(p.at("a"));
      candB = occBodies(p.at("b"));
    }
    if (refA.empty() || candB.empty())
      throw std::runtime_error("compare_geometry needs {golden_path} or "
                               "{a, b} with geometry on both sides");
    CADShape missingShape, extraShape;
    // `materialize` draws the delta on the model instead of only measuring
    // it: one screenshot then answers WHERE it differs, which no similarity
    // number can. It needs the same two solids the *_path exports do.
    const bool materialize = p.value("materialize", false);
    const bool wantMissing = p.contains("missing_path") || materialize;
    const bool wantExtra = p.contains("extra_path") || materialize;
    // Every refusal before the first file is written. The overlay is drawn
    // in the ACTIVE PART's coordinate space, so it needs a part tab to be
    // drawn on (refusing beats an overlay floating somewhere plausible and
    // wrong); and both paths are read here rather than each between the
    // writes, so a bad second path cannot leave the first file behind a
    // refused call.
    if (materialize && doc == nullptr)
      throw std::runtime_error(
          "materialize needs a part tab — the delta is drawn in the active "
          "part's coordinate space");
    std::optional<std::string> missingPath, extraPath;
    if (p.contains("missing_path"))
      missingPath = p.at("missing_path").get<std::string>();
    if (p.contains("extra_path"))
      extraPath = p.at("extra_path").get<std::string>();
    // `align`: put the candidate's centre of mass on the reference's before
    // the boolean, and answer about the SHAPE rather than about where it
    // happens to be. Without it a re-drawn port tube whose volume matched
    // its reference to four figures scored an IoU of 0.333, because the
    // reference sits where the cabinet puts it (z -1012..-762) and the part
    // is built about its own origin 900 mm away. Opt-in, because a golden
    // taken AT a position is a real thing to check and this would hide it;
    // `center_offset` still reports how far it moved.
    // Which metrics to compute. The overlap ones — IoU, dice, precision,
    // recall and the diff solids — cost three OCC booleans, which on a part
    // carrying toroidal fillets is SECONDS (13.2 s for one brace of the
    // smart speaker, 0.03 s for a tube with none). A caller reading
    // volume_ratio should not pay for them
    // (docs/inspect/README.md#comparing-against-a-reference).
    const std::string metrics = p.value("metrics", std::string("all"));
    if (metrics != "all" && metrics != "volume")
      throw std::runtime_error("unknown metrics '" + metrics +
                               "': accepted are \"all\" (the default — adds "
                               "iou/dice/precision/recall and the diff "
                               "solids, at the price of three booleans) and "
                               "\"volume\" (size, placement and topology)");
    const bool wantOverlap = metrics == "all";
    if (!wantOverlap &&
        (wantMissing || wantExtra || p.value("materialize", false)))
      throw std::runtime_error(
          "metrics \"volume\" computes no diff solids, so missing_path, "
          "extra_path and materialize have nothing to write — ask for "
          "metrics \"all\"");

    glm::dmat4 alignB(1.0);
    if (p.value("align", false)) {
      // Centres of mass only — the probe never needed the booleans.
      const auto probe = Measure::CompareBodySets(
          refA, glm::dmat4(1.0), candB, glm::dmat4(1.0), nullptr, nullptr,
          /*overlap=*/false);
      if (probe)
        alignB = glm::translate(glm::dmat4(1.0), glm::dvec3(probe->comA) -
                                                     glm::dvec3(probe->comB));
    }
    const auto c = Measure::CompareBodySets(
        refA, glm::dmat4(1.0), candB, alignB,
        wantMissing ? &missingShape : nullptr,
        wantExtra ? &extraShape : nullptr, wantOverlap);
    if (!c)
      throw std::runtime_error(
          "compare failed — an OpenCASCADE boolean threw, usually on an "
          "invalid or empty body; run check_geometry on both sides");

    const double unionV = c->volumeA + c->volumeB - c->common;
    const double iou = unionV > 1e-9 ? c->common / unionV : 0.0;
    const double dice = (c->volumeA + c->volumeB) > 1e-9
                            ? 2.0 * c->common / (c->volumeA + c->volumeB)
                            : 0.0;
    const double precision = c->volumeB > 1e-9 ? c->common / c->volumeB : 0.0;
    const double recall = c->volumeA > 1e-9 ? c->common / c->volumeA : 0.0;
    const double volumeRatio =
        c->volumeA > 1e-9 ? c->volumeB / c->volumeA : 0.0;
    const double centerOffset =
        glm::length(glm::dvec3(c->comA) - glm::dvec3(c->comB));
    const auto boxVol = [](const glm::dvec3& mn, const glm::dvec3& mx) {
      const glm::dvec3 d = glm::max(mx - mn, glm::dvec3(0.0));
      return double(d.x) * d.y * d.z;
    };
    const glm::dvec3 imn = glm::max(c->minA, c->minB);
    const glm::dvec3 imx = glm::min(c->maxA, c->maxB);
    const double boxInter = boxVol(imn, imx);
    const double boxUnion =
        boxVol(c->minA, c->maxA) + boxVol(c->minB, c->maxB) - boxInter;
    const double bboxIou = boxUnion > 1e-9 ? boxInter / boxUnion : 0.0;

    std::vector<std::string> verdicts;
    // Room for the longest sentence below: at 160 the census line lost its
    // ending ("...to compare the SHAPES") whenever the round-trip note was on.
    char buf[320];
    // A shape verdict needs the overlap. Without it this says what it CAN
    // support — size, placement, topology — and never "match", which only a
    // boolean can earn.
    if (!wantOverlap) {
      // The census is stated, not judged. It separates a shape faked at the
      // right volume (a frustum of stacked discs is cylinder:6 plane:7
      // against cone:1 plane:2 —
      // docs/inspect/README.md#comparing-against-a-reference), but a difference
      // is not by itself a difference of SHAPE: a STEP round-trip splits faces,
      // and this project's ring brace comes back 95/223 against the export's
      // 45/171 at an IoU of 0.991.
      snprintf(buf, sizeof buf,
               "size and placement only (metrics \"volume\"): census "
               "%d faces / %d edges against %d / %d%s — ask for metrics "
               "\"all\" to compare the SHAPES",
               c->facesB, c->edgesB, c->facesA, c->edgesA,
               (c->facesA == c->facesB && c->edgesA == c->edgesB)
                   ? " (the same)"
                   : " (a round trip splits faces; not a verdict)");
      verdicts.push_back(buf);
    } else if (iou > 0.99)
      verdicts.push_back("identical within tolerance");
    else if (iou > 0.95)
      verdicts.push_back("excellent match");
    else if (iou > 0.8)
      verdicts.push_back("good match");
    else if (iou > 0.5)
      verdicts.push_back("partial match");
    else
      verdicts.push_back("poor match");
    if (wantOverlap && precision < recall - 0.05) {
      snprintf(buf, sizeof buf,
               "over-generating: %.1f%% of the candidate is extra geometry",
               (1.0 - precision) * 100.0);
      verdicts.push_back(buf);
    } else if (wantOverlap && recall < precision - 0.05) {
      snprintf(buf, sizeof buf,
               "under-generating: %.1f%% of the reference is missing",
               (1.0 - recall) * 100.0);
      verdicts.push_back(buf);
    }
    if (volumeRatio > 1e-9 && (volumeRatio < 0.95 || volumeRatio > 1.05)) {
      snprintf(buf, sizeof buf, "%s by %.1f%%",
               volumeRatio < 1.0 ? "undersized" : "oversized",
               std::abs(1.0 - volumeRatio) * 100.0);
      verdicts.push_back(buf);
    }
    if (centerOffset > 1.0) {
      snprintf(buf, sizeof buf, "center of mass offset by %.1f mm",
               centerOffset);
      verdicts.push_back(buf);
    }

    // The overlap metrics are OMITTED in volume mode rather than reported as
    // zero: an iou of 0 means "these do not overlap at all", which is the
    // opposite of "not measured". `metrics` says which mode produced this.
    json out = {{"metrics", metrics},
                {"volume_ratio", volumeRatio},
                {"center_offset", centerOffset},
                {"bbox_iou", bboxIou},
                {"volume_reference", c->volumeA},
                {"volume_candidate", c->volumeB},
                {"surface_reference", c->surfaceA},
                {"surface_candidate", c->surfaceB},
                {"faces", {{"reference", c->facesA}, {"candidate", c->facesB}}},
                {"edges", {{"reference", c->edgesA}, {"candidate", c->edgesB}}},
                {"verdicts", verdicts}};
    if (wantOverlap) {
      out["iou"] = iou;
      out["dice"] = dice;
      out["precision"] = precision;
      out["recall"] = recall;
      out["volume_common"] = c->common;
      out["volume_missing"] = c->missing;
      out["volume_extra"] = c->extra;
    }
    {
      // A boolean diff on the real B-Rep, so VERIFIED — but the claim it
      // supports is "these are the same shape", and that is a judgement with
      // a threshold in it. The threshold is named in the note rather than
      // hidden in a verdict string, so a caller can disagree with it.
      receipt::Receipt receipt;
      constexpr double kSame = 0.999; // IoU at which we call it the same shape
      if (wantOverlap) {
        const receipt::Oracle oracle{"designer/boolean-diff", "1"};
        receipt::Claim same(
            "the candidate is the same shape as the reference", oracle,
            iou >= kSame ? receipt::Verdict::Pass : receipt::Verdict::Fail);
        same.basis = receipt::Basis::Verified;
        same.value = iou;
        same.unit = "IoU";
        same.note = "vol(A and B) / vol(A or B) on the exact solids, called "
                    "the same at " +
                    std::to_string(kSame) +
                    "; the `verdicts` strings say which way it differs";
        receipt.Add(std::move(same));
      } else {
        // No boolean ran, so the SHAPE claim cannot be made — and making it
        // from an unmeasured IoU of zero would fail every comparison that
        // asked for less. Claim what was measured instead.
        const receipt::Oracle oracle{"designer/volume-ratio", "1"};
        constexpr double kTol = 0.001; // 0.1% either way
        receipt::Claim sized(
            "the candidate has the same volume as the reference", oracle,
            std::abs(1.0 - volumeRatio) <= kTol ? receipt::Verdict::Pass
                                                : receipt::Verdict::Fail);
        sized.basis = receipt::Basis::Verified;
        sized.value = volumeRatio;
        sized.unit = "ratio";
        sized.note = "vol(B) / vol(A) on the exact solids. This is NOT a "
                     "shape check — two different shapes can share a volume "
                     "— and metrics \"volume\" ran no boolean; ask for "
                     "metrics \"all\" for that claim";
        receipt.Add(std::move(sized));
      }
      AttachReceipt(out, receipt);
    }
    if (missingPath) {
      if (!CADKernel::Instance().SaveBREP(*missingPath, missingShape))
        throw std::runtime_error("cannot write " + *missingPath +
                                 " — does the parent directory exist?");
      out["missing_path"] = *missingPath;
    }
    if (extraPath) {
      if (!CADKernel::Instance().SaveBREP(*extraPath, extraShape))
        throw std::runtime_error("cannot write " + *extraPath +
                                 " — does the parent directory exist?");
      out["extra_path"] = *extraPath;
    }
    if (materialize) {
      snprintf(buf, sizeof buf,
               "iou %.3f — %.0f mm³ missing (red), %.0f mm³ extra (green)", iou,
               c->missing, c->extra);
      ui::SetGeometryDelta(state, missingShape, extraShape, buf);
      // Say whether it will actually be SEEN: a --headless instance has no
      // renderer, and "materialized: true" over an invisible overlay is the
      // kind of half-truth that costs an agent a debugging round.
      out["materialized"] = static_cast<bool>(state.meshRenderer);
      if (!state.meshRenderer)
        out["note"] = "delta recorded, but this instance has no renderer to "
                      "draw it on — run under tools/headless-preview.sh to "
                      "screenshot it";
    }
    return out;
  }
  if (m == "clear_geometry_delta") {
    // The off-switch for materialize. Separate from compare_geometry because
    // an agent that wants the plain model back should not have to run a
    // comparison to get it.
    ui::ClearGeometryDelta(state);
    return json{{"cleared", true}};
  }
  if (m == "contact_report") {
    // V1: the whole-assembly connectivity gate. Box prefilter over every
    // visible, unsuppressed occurrence pair; exact contact on candidates;
    // union-find over touching/clashing edges. A scripted build asserts
    // len(components) == 1 and near_misses == [] the way it asserts a BOM.
    //
    // Round-7 finding: on a PART tab the same sweep runs over the part's
    // BODIES — the occurrence-level gate structurally cannot see a bracket
    // floating INSIDE one part (the ladder mount plate that touched
    // nothing), so multi-body parts get their own connectivity check.
    // The response is built by RunContactSweep, above:
    // rpc-returns-from: src/remote/RemoteInspection.cpp
    if (doc == nullptr && state.assembly == nullptr)
      throw std::runtime_error(
          "contact_report needs a part or assembly tab — switch tabs with "
          "set_active_part (see list_parts)");
    // Read by ContactParams:
    // rpc-params: deadline_s, exact_face_budget, max_pairs
    // rpc-params: mode, near_tol, touch_tol
    auto sw = std::make_shared<ContactSweep>(ContactParams(p));
    const bool async = p.value("async", false);
    const bool outlive = p.value("outlive_client", false);
    if (outlive && !async)
      throw std::runtime_error(
          "outlive_client is for an async report (async: true); a "
          "synchronous one ends with its call");
    if (async) {
      // One at a time: `cancel_background_job {kind: "contact"}` then says
      // which run it stops.
      if (const ui::BackgroundJob* running =
              ui::FindBackgroundJob(state, "contact"))
        throw std::runtime_error(
            "a contact report is already running (job " +
            std::to_string(running->id) +
            "): contact_status {job} reports it, cancel_background_job "
            "{kind: \"contact\"} stops it");
      sw->copies = true;
      if (const char* ms = std::getenv("DESIGNER_TEST_CONTACT_PAIR_MS"))
        sw->pairDelayMs = std::max(0, std::atoi(ms));
    }
    if (doc != nullptr)
      GatherContactBodies(*sw, *doc);
    else
      GatherContactOccurrences(*sw, state);
    if (!async)
      return RunContactSweep(*sw, nullptr, nullptr);
    const uint64_t owner = outlive ? 0 : state.rpcConnection;
    const uint64_t job = StartContactJob(state, sw, owner);
    return json{{"started", true},
                {"running", true},
                {"job", job},
                {"kind", "contact"},
                {"outlives_client", owner == 0}};
  }
  if (m == "contact_status") {
    // An async contact_report by its job id, from any tab: how far it is
    // while it runs, how it ended and (when it finished) the report.
    ui::ReapBackgroundJobs(state);
    const uint64_t id = p.at("job").get<uint64_t>();
    if (ui::BackgroundJob* j = ui::FindBackgroundJobById(state, id)) {
      if (j->kind != "contact")
        throw std::runtime_error("job " + std::to_string(id) + " is a '" +
                                 j->kind +
                                 "' job, not a contact report -- "
                                 "background_jobs lists it");
      const double fraction = j->fraction.load();
      return json{
          {"job", id},
          {"kind", "contact"},
          {"label", j->label},
          {"running", true},
          {"fraction", fraction >= 0.0 ? json(fraction) : json(nullptr)},
          {"elapsed_seconds", std::chrono::duration<double>(
                                  std::chrono::steady_clock::now() - j->started)
                                  .count()},
          {"cancellable", j->cancellable},
          {"cancel_requested", j->cancel.load()},
          {"outlives_client", j->owner == 0}};
    }
    if (const ui::FinishedJob* f = ui::FinishedJobById(state, id)) {
      if (f->kind != "contact")
        throw std::runtime_error("job " + std::to_string(id) + " is a '" +
                                 f->kind + "' job, not a contact report");
      json out{{"job", id},
               {"kind", "contact"},
               {"running", false},
               {"outcome", f->outcome},
               {"seconds", f->seconds}};
      if (!f->error.empty())
        out["error"] = f->error;
      if (!f->cancelReason.empty())
        out["cancel_reason"] = f->cancelReason;
      if (const auto it = state.contactReports.find(id);
          it != state.contactReports.end())
        out["report"] = it->second;
      return out;
    }
    std::string known;
    for (const ui::BackgroundJobInfo& j : ui::ListBackgroundJobs(state))
      if (j.kind == "contact")
        known +=
            (known.empty() ? "" : ", ") + std::to_string(j.id) + " (running)";
    for (const ui::FinishedJob& f : state.finishedJobs)
      if (f.kind == "contact")
        known += (known.empty() ? "" : ", ") + std::to_string(f.id) + " (" +
                 f.outcome + ")";
    throw std::runtime_error(
        "no contact report job " + std::to_string(id) + " — " +
        (known.empty() ? std::string("none has run (contact_report "
                                     "{async: true} starts one)")
                       : "known: " + known));
  }
  if (m == "mass_properties") {
    // Volume, surface area, center of mass, and inertia (about the CoM) per
    // body, plus a document total. Mass assumes model units are millimetres:
    // grams = density[kg/m^3] * volume[mm^3] * 1e-6; bodies without an
    // assigned material report mass null (density unknown), and so does the
    // total if any contributor is unknown. params: {body?: index}
    //
    // On an ASSEMBLY tab there is no part doc: aggregate every visible,
    // non-suppressed occurrence's part mass in WORLD space (each part's CoM
    // carried through its solved occurrence transform — the same placement the
    // interference/BOM paths use). The reply's `bodies` array carries one row
    // per occurrence; `total` is the combined world mass/volume/CoM.
    // An explicit `part` reads THAT part, from any tab and without switching
    // to one — the assembly aggregate below is what you get when you ask from
    // an assembly tab and name no part (W2).
    Document* massDoc = doc;
    if (p.contains("part"))
      massDoc = ResolvePartParam(state, p.at("part"));
    if (massDoc == nullptr && state.assembly) {
      const assembly::PartResolver resolve = ui::PartResolverFor(state);
      const assembly::AssemblyMass am = assembly::MassProperties(
          *state.assembly, resolve, ui::AssemblyResolverFor(state));
      json rows = json::array();
      for (const assembly::OccurrenceMass& om : am.occurrences) {
        json entry = {
            {"occurrence", om.occurrenceId},
            {"name", om.name},
            {"part_id", om.partId},
            {"volume", om.volume},
            {"center_of_mass",
             {om.centerOfMass.x, om.centerOfMass.y, om.centerOfMass.z}}};
        entry["mass_g"] = om.massKnown ? json(om.massG) : json(nullptr);
        rows.push_back(entry);
      }
      const glm::dvec3& com = am.centerOfMass;
      json out = {
          {"occurrences", rows},
          {"total",
           {{"volume", am.volume},
            {"center_of_mass", {com.x, com.y, com.z}},
            {"mass_g", am.massKnown ? json(am.massG) : json(nullptr)}}}};
      if (!am.skipped.empty())
        out["skipped"] = am.skipped;
      return out;
    }
    const auto& bodies = RequirePartDoc(massDoc, "mass_properties").GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError("body", p.value("body", -1),
                                          bodies.size(),
                                          "get_state lists bodies"));
    // The math is shared with the BOM (A4): Measure::MassOfBodies. A single
    // body's Body copy is cheap (the shape is a refcounted OCC handle).
    std::vector<Body> subset;
    if (only >= 0)
      subset.push_back(bodies[only]);
    std::string err;
    const auto mass = Measure::MassOfBodies(only >= 0 ? subset : bodies, &err);
    if (!mass)
      throw std::runtime_error(err);
    if (mass->bodies.empty())
      throw std::runtime_error(
          bodies.empty() ? "no bodies"
                         : "only reference bodies (derived ghosts), which "
                           "weigh nothing here; set reference: false on the "
                           "Derived feature to count one");
    json out = json::array();
    for (size_t i = 0; i < mass->bodies.size(); i++) {
      const Measure::BodyMass& bm = mass->bodies[i];
      json inertia = json::array();
      for (int r = 0; r < 3; r++)
        inertia.push_back(
            {bm.inertia[r][0], bm.inertia[r][1], bm.inertia[r][2]});
      json entry = {{"body", only >= 0 ? static_cast<size_t>(only) : i},
                    {"name", bm.name},
                    {"volume", bm.volume},
                    {"surface_area", bm.surfaceArea},
                    {"center_of_mass",
                     {bm.centerOfMass.x, bm.centerOfMass.y, bm.centerOfMass.z}},
                    {"inertia", inertia},
                    {"material", bm.material}};
      // The MEASURED box, evaluated off the surfaces rather than bounded by
      // their control points. get_state's is the cheap conservative one
      // because it is recomputed after every edit; this call is the one that
      // exists to measure, so it pays for the exact answer.
      {
        const size_t which = only >= 0 ? static_cast<size_t>(only) : i;
        if (which < bodies.size()) {
          glm::dvec3 bmin, bmax;
          bodies[which].shape.GetBoundingBox(bmin, bmax, /*tight=*/true);
          entry["bbox_min"] = {bmin.x, bmin.y, bmin.z};
          entry["bbox_max"] = {bmax.x, bmax.y, bmax.z};
        }
      }
      if (bm.massKnown) {
        entry["density"] = bm.density;
        entry["mass_g"] = bm.massG;
      } else {
        entry["mass_g"] = nullptr;
      }
      out.push_back(entry);
    }
    const glm::dvec3& com = mass->centerOfMass;
    return json{
        {"bodies", out},
        {"total",
         {{"volume", mass->volume},
          {"center_of_mass", {com.x, com.y, com.z}},
          {"mass_g", mass->massKnown ? json(mass->massG) : json(nullptr)}}}};
  }
  if (m == "check_geometry") {
    // B-Rep validity per body (OCC BRepCheck_Analyzer) — flags invalid faces/
    // edges/wires and the distinct reasons. Read-only; useful on imported STEP
    // and after booleans. params: {body?: index} (all bodies when omitted).
    //
    // `valid` is strictly "every EXISTING body is a valid B-Rep" — vacuously
    // true over zero bodies. The natural "did my model build?" probe is `ok`,
    // which also demands no feature errored; body_count exposes the vacuous
    // case ("valid over nothing") explicitly.
    Document& part = RequirePartDoc(doc, "check_geometry");
    const auto& bodies = part.GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError("body", p.value("body", -1),
                                          bodies.size(),
                                          "get_state lists bodies"));
    json rows = json::array();
    bool allValid = true;
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && static_cast<int>(i) != only)
        continue;
      const Measure::GeometryReport r = Measure::CheckGeometry(bodies[i].shape);
      allValid = allValid && r.valid;
      rows.push_back({{"body", i},
                      {"name", bodies[i].name},
                      {"valid", r.valid},
                      {"bad_faces", r.badFaces},
                      {"bad_edges", r.badEdges},
                      {"bad_wires", r.badWires},
                      {"reasons", r.reasons}});
    }
    json featureErrors = RegenSummaryJson(part)["failed"];
    const bool ok = allValid && featureErrors.empty();
    const size_t errorCount = featureErrors.size();
    json out = {{"valid", allValid},
                {"bodies", rows},
                {"body_count", bodies.size()},
                {"feature_errors", std::move(featureErrors)},
                {"ok", ok}};
    {
      // `valid` is vacuously true over zero bodies, which the comment above
      // has always said and the flag itself cannot. As a receipt it is a
      // claim that COULD NOT BE CHECKED rather than one that holds.
      receipt::Receipt r;
      const receipt::Oracle brep{"occ/BRepCheck_Analyzer",
                                 BuildInfo::KernelVersion()};
      const size_t examined = rows.size();
      if (examined == 0) {
        r.Unverifiable("every body is a valid B-Rep", brep,
                       "there are no bodies to check — `valid` is vacuously "
                       "true here, which is not the same as a part that built");
      } else {
        receipt::Claim c("every body is a valid B-Rep", brep,
                         allValid ? receipt::Verdict::Pass
                                  : receipt::Verdict::Fail);
        c.value = static_cast<double>(examined);
        c.unit = "bodies";
        r.Add(std::move(c));
      }
      receipt::Claim f("no feature errored", {"designer/regen", "1"},
                       errorCount == 0 ? receipt::Verdict::Pass
                                       : receipt::Verdict::Fail);
      f.value = static_cast<double>(errorCount);
      f.unit = "features";
      r.Add(std::move(f));
      AttachReceipt(out, r);
    }
    // Derived copies behind their source (v84): built fine, so `ok` stands,
    // but the geometry is yesterday's — the caller decides whether to
    // update_derived. Present only when there is something to say.
    {
      json stale = json::array();
      for (const DerivedFeature* d : ui::StaleDerived(state, part))
        stale.push_back({{"id", d->GetId()},
                         {"name", d->GetName()},
                         {"source", d->sourceName}});
      if (!stale.empty())
        out["stale_derived"] = std::move(stale);
    }
    // Template instances behind their library template
    // (docs/modeling/templates.md#revisions): built fine from the revision
    // they record, so `ok` stands; update_template_instances follows.
    {
      json changed = json::array();
      for (const auto& f : part.GetFeatures())
        if (const auto* tf = dynamic_cast<const TemplateFeature*>(f.get());
            tf != nullptr &&
            ui::TemplateChanged(state, *tf) == std::optional<bool>{true})
          changed.push_back({{"id", tf->GetId()},
                             {"name", tf->GetName()},
                             {"template", tf->templateName},
                             {"revision", tf->definition.revision}});
      if (!changed.empty())
        out["templates_changed"] = std::move(changed);
    }
    // Round-8 retro: floating geometry INSIDE a part warns by default —
    // the ladder's mount plate passed every opt-in gate because nobody
    // thought to ask. `ok` is untouched (disjoint multi-body parts can be
    // legitimate); scripts assert `connected` where it matters.
    if (bodies.size() >= 2 && only < 0) {
      const auto comps = BodyComponentNames(part, 1e-3, 1.0);
      out["body_components"] = comps;
      out["connected"] = comps.size() <= 1;
      if (comps.size() > 1) {
        json warnings = json::array();
        for (size_t i = 1; i < comps.size(); i++) {
          std::string names;
          for (const std::string& n : comps[i])
            names += (names.empty() ? "" : ", ") + n;
          warnings.push_back(
              "bodies [" + names +
              "] touch nothing else in this part — floating geometry inside "
              "a part is usually a mistake (contact_report on this tab lists "
              "the gaps)");
        }
        out["warnings"] = std::move(warnings);
      }
    }
    return out;
  }
  if (m == "list_print_profiles") {
    json out = json::array();
    for (const printfit::Profile& pr : state.printProfiles)
      out.push_back(PrintProfileWire(state, pr));
    // The built-in tables beside them: the defaults a profile starts from.
    return json{{"profiles", std::move(out)},
                {"defaults", PrintTablesJson()},
                {"dir", ui::PrintProfileDir()}};
  }
  if (m == "save_print_profile") {
    // The user's own table (docs/tolerances/README.md#print-profiles):
    // written to the store, never to a document. Every check is made before
    // the file is written, so a refusal changes nothing.
    const bool fromPart = p.value("from_part", false);
    if (fromPart == p.contains("profile"))
      throw std::runtime_error(
          "save_print_profile: `profile` {name, process, material, "
          "conditions?, stl_deflection_mm?, layer_height_mm?, z_gap_mm?, "
          "rows?} or `from_part: true` (the "
          "part's embedded copy), one of the two");
    if (!fromPart && p.contains("part"))
      throw std::runtime_error(
          "save_print_profile: `part` says whose copy `from_part` saves");
    printfit::Profile pr;
    if (fromPart) {
      const Document& part = p.contains("part")
                                 ? *ResolvePartParam(state, p.at("part"))
                                 : RequirePartDoc(doc, "save_print_profile");
      if (!part.PrintProcess() || !part.PrintProcess()->profile)
        throw std::runtime_error(
            "save_print_profile: this part is printed with no profile (" +
            std::string(part.PrintProcess() ? "the built-in table"
                                            : "not printed at all") +
            ") — there is no copy to save; pass `profile`");
      pr = *part.PrintProcess()->profile;
    } else {
      json pj = p.at("profile");
      if (!pj.is_object())
        throw std::runtime_error("save_print_profile: `profile` is an object");
      // No rows: the built-in table of its pair, the defaults to correct.
      if (!pj.contains("rows")) {
        const printfit::Table* base =
            printfit::FindBuiltin(pj.value("process", std::string()),
                                  pj.value("material", std::string()));
        if (base != nullptr) {
          json rows = json::array();
          for (const printfit::Row& r : base->rows)
            rows.push_back(DocumentIO::PrintRowToJson(r));
          pj["rows"] = std::move(rows);
          if (!pj.contains("conditions"))
            pj["conditions"] = base->conditions;
        }
      }
      try {
        pr = DocumentIO::PrintProfileFromJson(pj);
      } catch (const std::exception& e) {
        throw std::runtime_error(std::string("save_print_profile: ") +
                                 e.what());
      }
    }
    std::string error;
    if (!ui::SavePrintProfile(state, pr, &error))
      throw std::runtime_error("save_print_profile: " + error);
    // Open parts are not changed: a part keeps the copy it was modelled
    // with, and get_print_process says when it now differs.
    return PrintProfileWire(state, pr);
  }
  if (m == "delete_print_profile") {
    std::string error;
    const std::string name = p.at("name").get<std::string>();
    if (!ui::DeletePrintProfile(state, name, &error))
      throw std::runtime_error("delete_print_profile: " + error);
    return json{{"deleted", name}};
  }
  if (m == "set_print_process") {
    // How the part is printed (docs/tolerances/README.md#printed-parts).
    // Drives no geometry, so it rebuilds nothing; it is what the fit checks
    // read for the allowance to model.
    Document& part = PartToEdit(state, doc, p, "set_print_process");
    std::optional<printfit::Process> next;
    if (p.value("clear", false)) {
      if (p.contains("process") || p.contains("material") ||
          p.contains("profile"))
        throw std::runtime_error("set_print_process: `clear` or a {process, "
                                 "material} or a {profile}, not both");
    } else {
      if (!p.contains("profile") &&
          (!p.contains("process") || !p.contains("material")))
        throw std::runtime_error(
            "set_print_process needs both `process` and `material` (e.g. "
            "fdm, petg) — an allowance is a property of the pair, and half "
            "of one would be guessed — or `profile`, one of yours "
            "(list_print_profiles). Or `clear: true` for a part that is not "
            "printed. get_print_process lists what is carried.");
      next = ProcessFromParams(state, p, "set_print_process");
      // rpc-params: material, process, profile
    }
    // The same process again is no edit: no undo step, and no version
    // moved under a study solved on this part (a generator that re-declares
    // its print before reading a result read it stale). The same profile
    // with different rows IS an edit: it is how "use mine" is done.
    const auto& have = part.PrintProcess();
    const bool same = have == next;
    if (!same)
      CommandsFor(state, part)
          .Execute(std::make_unique<SetPrintProcessCommand>(next), part);
    return json{{"print", ProcessJson(state, part.PrintProcess())},
                {"threads", PrintedThreadsJson(part)}};
  }
  if (m == "check_print") {
    // The one printability check (docs/tolerances/README.md#checking-a-print):
    // ui::RunPrintCheck derives the limits from the part's print, and the
    // Print process section's Check print calls the same. Every parameter
    // is read and checked before anything is measured.
    const Document& part = SubjectPart(state, doc, p);
    std::optional<std::set<uint64_t>> only;
    if (p.contains("bodies"))
      only = ResolvePartBodies(part, p.at("bodies"), "bodies");
    ui::PrintCheckAsk ask;
    if (p.contains("direction")) {
      const json& d = p.at("direction");
      if (!d.is_array() || d.size() != 3 || !d[0].is_number() ||
          !d[1].is_number() || !d[2].is_number())
        throw std::runtime_error("check_print: `direction` is the print "
                                 "direction [x, y, z], not " +
                                 d.dump());
      const glm::dvec3 v(d[0].get<double>(), d[1].get<double>(),
                         d[2].get<double>());
      const double len = glm::length(v);
      if (!std::isfinite(len) || len < 1e-9)
        throw std::runtime_error("check_print: `direction` " + d.dump() +
                                 " has no direction; the bed's normal is a "
                                 "non-zero [x, y, z], [0, 0, 1] for a part "
                                 "printed as modelled");
      ask.up = v / len;
    }
    const auto number = [&](const char* key, bool given, double lo, double hi,
                            const char* what) -> std::optional<double> {
      if (!given)
        return std::nullopt;
      const json& v = p.at(key);
      if (!v.is_number() || !std::isfinite(v.get<double>()) ||
          !(v.get<double>() >= lo && v.get<double>() <= hi))
        throw std::runtime_error(std::string("check_print: `") + key + "` is " +
                                 what + ", not " + v.dump());
      return v.get<double>();
    };
    ask.maxOverhangDeg = number(
        "max_overhang_deg", p.contains("max_overhang_deg"), 1e-9, 90.0 - 1e-9,
        "the steepest overhang, in degrees from vertical, between 0 "
        "and 90 (45 is FDM's rule)");
    ask.overhangAllowanceMm2 = number(
        "overhang_allowance_mm2", p.contains("overhang_allowance_mm2"), 0.0,
        1e12, "an area in mm² past the limit that passes, 0 or more");
    ask.maxBridgeMm = number("max_bridge_mm", p.contains("max_bridge_mm"), 1e-9,
                             1e12, "the widest bridge in mm, more than 0");
    ask.minGapMm = number("min_gap_mm", p.contains("min_gap_mm"), 1e-9, 1e12,
                          "the gap a side in mm, more than 0");
    ask.stlDeflectionMm =
        number("stl_deflection_mm", p.contains("stl_deflection_mm"), 1e-9, 1e12,
               "the STL chord deviation in mm, more than 0");

    const ui::PrintCheckRun run =
        ui::RunPrintCheck(part, only ? &*only : nullptr, ask);
    const printcheck::Report& r = run.report;
    const auto& all = part.GetBodies();
    const auto bodyName = [&](int i) {
      return all[static_cast<size_t>(run.bodyIndex[static_cast<size_t>(i)])]
          .name;
    };
    const auto vec = [](const glm::dvec3& v) {
      return json::array({v.x, v.y, v.z});
    };
    const auto where = [&](const printcheck::Where& w) {
      const Body& b =
          all[static_cast<size_t>(run.bodyIndex[static_cast<size_t>(w.body)])];
      json out{{"body", run.bodyIndex[static_cast<size_t>(w.body)]},
               {"name", b.name},
               {"face", w.face},
               {"point_mm", vec(w.point)}};
      if (const std::string* n = b.elementMap.NameOf(w.face))
        out["element"] = {{"feature_id", b.createdBy}, {"name", *n}};
      return out;
    };
    const auto names = [&](const std::vector<int>& v) {
      json out = json::array();
      for (int i : v)
        out.push_back(bodyName(i));
      return out;
    };
    constexpr size_t kListed = 20;
    const auto status = [](printcheck::Status s) {
      return printcheck::StatusName(s);
    };

    json bodies = json::array();
    for (size_t i = 0; i < r.bodies.size(); ++i)
      bodies.push_back({{"body", run.bodyIndex[i]},
                        {"name", bodyName(static_cast<int>(i))},
                        {"solids", r.bodies[i].solids},
                        {"watertight", r.bodies[i].watertight},
                        {"lowest_mm", r.bodies[i].lowestMm}});
    json joined = json::array();
    for (const auto& g : r.shells.joined)
      joined.push_back(names(g));
    json overFaces = json::array();
    for (size_t i = 0; i < r.overhang.faces.size() && i < kListed; ++i) {
      json f = where(r.overhang.faces[i].where);
      f["worst_deg"] = r.overhang.faces[i].worstDeg;
      f["area_mm2"] = r.overhang.faces[i].areaMm2;
      overFaces.push_back(std::move(f));
    }
    json overhang{{"status", status(r.overhang.status)},
                  {"limit_deg", r.overhang.limitDeg},
                  {"limit_from", run.overhangFrom},
                  {"allowance_mm2", r.overhang.allowanceMm2},
                  {"area_mm2", r.overhang.areaMm2},
                  {"worst_deg", r.overhang.worstDeg},
                  {"faces", std::move(overFaces)},
                  {"note", r.overhang.note}};
    if (r.overhang.worst)
      overhang["worst"] = where(*r.overhang.worst);
    if (!run.overhangSource.empty())
      overhang["source"] = run.overhangSource;
    json bridgeFaces = json::array();
    for (size_t i = 0; i < r.bridges.faces.size() && i < kListed; ++i) {
      json f = where(r.bridges.faces[i].where);
      f["span_mm"] = r.bridges.faces[i].spanMm;
      f["area_mm2"] = r.bridges.faces[i].areaMm2;
      f["height_mm"] = r.bridges.faces[i].heightMm;
      bridgeFaces.push_back(std::move(f));
    }
    json bridges{{"status", status(r.bridges.status)},
                 {"limit_mm", r.bridges.limitMm},
                 {"limit_from", run.bridgeFrom},
                 {"worst_span_mm", r.bridges.worstSpanMm},
                 {"area_mm2", r.bridges.areaMm2},
                 {"faces", std::move(bridgeFaces)},
                 {"note", r.bridges.note}};
    if (!run.bridgeSource.empty())
      bridges["source"] = run.bridgeSource;
    json pairs = json::array();
    for (size_t i = 0; i < r.clearance.pairs.size() && i < kListed; ++i) {
      const printcheck::Pair& pr = r.clearance.pairs[i];
      pairs.push_back({{"a", bodyName(pr.a)},
                       {"b", bodyName(pr.b)},
                       {"solid_a", pr.solidA},
                       {"solid_b", pr.solidB},
                       {"distance_mm", pr.distanceMm},
                       {"overlap", pr.overlap}});
    }
    json clearance{{"status", status(r.clearance.status)},
                   {"required_mm", r.clearance.requiredMm},
                   {"required_from", run.gapFrom},
                   {"tolerance_mm", r.clearance.toleranceMm},
                   {"pairs", std::move(pairs)},
                   {"pruned", r.clearance.pruned},
                   {"note", r.clearance.note}};
    if (r.clearance.minMm)
      clearance["min_mm"] = *r.clearance.minMm;
    json out{
        {"ok", r.Ok()},
        {"process", run.process.empty() ? json(nullptr) : json(run.process)},
        {"direction", vec(run.options.up)},
        {"bodies", std::move(bodies)},
        {"bed",
         {{"status", status(r.bed.status)},
          {"lowest_mm", r.bed.lowestMm},
          {"tol_mm", r.bed.tolMm},
          {"contact_area_mm2", r.bed.contactAreaMm2},
          {"on_bed", names(r.bed.onBed)}}},
        {"shells",
         {{"status", status(r.shells.status)},
          {"pieces", r.shells.pieces},
          {"expected", r.shells.expected},
          {"watertight", r.shells.watertight},
          {"joined", std::move(joined)},
          {"split", names(r.shells.split)}}},
        {"overhang", std::move(overhang)},
        {"bridges", std::move(bridges)},
        {"clearance", std::move(clearance)},
        {"chords",
         {{"status", status(r.chords.status)},
          {"deflection_mm", r.chords.deflectionMm},
          {"deflection_from", run.deflectionFrom},
          {"limit_mm", r.chords.limitMm},
          {"note", r.chords.note}}}};

    receipt::Receipt rc;
    const receipt::Oracle oracle{"designer/print_check", "1"};
    const auto claim = [&](const std::string& what, printcheck::Status s,
                           double value, const char* unit,
                           const std::string& note) {
      if (s == printcheck::Status::Skipped) {
        rc.Unverifiable(what, oracle, note);
        return;
      }
      receipt::Claim c(what, oracle,
                       s == printcheck::Status::Pass ? receipt::Verdict::Pass
                                                     : receipt::Verdict::Fail);
      c.value = value;
      c.unit = unit;
      c.note = note;
      rc.Add(std::move(c));
    };
    claim("the part stands on the bed", r.bed.status, r.bed.lowestMm, "mm",
          "its lowest point along the print direction");
    claim("one piece per body", r.shells.status, r.shells.pieces, "pieces",
          "bodies within 1 µm of each other print as one");
    claim("no overhang past the limit", r.overhang.status, r.overhang.areaMm2,
          "mm2", r.overhang.note);
    claim("every bridge within the limit", r.bridges.status,
          r.bridges.worstSpanMm, "mm", r.bridges.note);
    claim("the bodies keep the gap", r.clearance.status,
          r.clearance.minMm.value_or(0.0), "mm", r.clearance.note);
    claim("the STL chords keep the gap", r.chords.status, r.chords.deflectionMm,
          "mm", r.chords.note);
    AttachReceipt(out, rc);
    return out;
  }
  if (m == "get_print_process") {
    const Document& part = p.contains("part")
                               ? *ResolvePartParam(state, p.at("part"))
                               : RequirePartDoc(doc, "get_print_process");
    json out{{"print", ProcessJson(state, part.PrintProcess())},
             {"tables", PrintTablesJson()},
             {"confirm", printfit::ConfirmNote()}};
    if (part.PrintProcess())
      out["threads"] = PrintedThreadsJson(part);
    return out;
  }
  if (m == "print_allowance") {
    // One printed diameter: what to MODEL for it. The process is the part's,
    // or stated here ({process, material}) to ask before there is a part.
    std::optional<printfit::Process> proc;
    if (p.contains("process") || p.contains("material") ||
        p.contains("profile")) {
      if (!p.contains("profile") &&
          (!p.contains("process") || !p.contains("material")))
        throw std::runtime_error(
            "print_allowance: `process` and `material` come as a pair");
      proc = ProcessFromParams(state, p, "print_allowance");
      // rpc-params: material, process, profile
    } else {
      const Document& part = p.contains("part")
                                 ? *ResolvePartParam(state, p.at("part"))
                                 : RequirePartDoc(doc, "print_allowance");
      proc = part.PrintProcess();
      if (!proc)
        throw std::runtime_error(
            "print_allowance: this part declares no print process — "
            "set_print_process {process, material} or {profile} on it, or "
            "pass them here");
    }
    if (!p.contains("kind"))
      throw std::runtime_error("print_allowance: `kind` is one of " +
                               printfit::PrintedNames());
    const std::string kindName = p.at("kind").get<std::string>();
    const auto kind = printfit::PrintedFromName(kindName);
    if (!kind)
      throw std::runtime_error("print_allowance: unknown kind '" + kindName +
                               "'. Accepted: " + printfit::PrintedNames());
    const bool thread = *kind == printfit::Printed::InternalThread ||
                        *kind == printfit::Printed::ExternalThread;
    double basic = 0.0;
    json out;
    if (p.contains("designation")) {
      if (!thread)
        throw std::runtime_error(
            "print_allowance: a `designation` names a thread; a " + kindName +
            " takes `size_mm`");
      if (p.contains("size_mm"))
        throw std::runtime_error(
            "print_allowance: `designation` or `size_mm`, not both — the "
            "designation already says the major diameter");
      const std::string des = p.at("designation").get<std::string>();
      std::string why;
      const auto res = threads::Resolve(des, &why);
      if (!res)
        throw std::runtime_error(
            "print_allowance: " +
            (why.empty() ? "'" + des +
                               "' is not a thread the tables carry — pass its "
                               "major as `size_mm`"
                         : why));
      basic = res->standard.majorDia;
      out["designation"] = res->standard.designation;
      out["pitch"] = res->standard.pitch;
    } else if (p.contains("size_mm")) {
      basic = p.at("size_mm").get<double>();
    } else {
      throw std::runtime_error(
          "print_allowance: the diameter to print, as `size_mm`" +
          std::string(thread ? " (the major) or a thread `designation`" : ""));
    }
    std::string why;
    const auto a = printfit::ForBasic(*proc, *kind, basic, &why);
    if (!a)
      throw std::runtime_error("print_allowance: " + why);
    // What export_stl would mesh this print at, unless asked about another.
    const double printed = printfit::StlDeflectionMm(*proc);
    const double deflection =
        p.value("stl_deflection_mm", printed > 0.0 ? printed : 0.1);
    if (proc->profile)
      out["profile"] = proc->profile->name;
    out.update(json{{"process", proc->process},
                    {"material", proc->material},
                    {"kind", kindName},
                    {"basic_mm", a->basicMm},
                    {"allowance_mm", a->allowanceMm},
                    {"model_mm", a->modelMm},
                    {"source", a->row->source},
                    {"confirm", printfit::ConfirmNote()},
                    {"stl_deflection_mm", deflection},
                    {"stl_chord_loss_mm", printfit::ChordLossMm(deflection)},
                    {"stl_note",
                     "an STL's chords sit inside a round surface by up to its "
                     "deflection, so a hole's diameter can come out up to "
                     "twice that small (a shaft's that large). The table "
                     "came through somebody's export: export the parts and "
                     "the print_coupon at the same deflection and the coupon "
                     "calibrates both"}});
    if (thread) {
      const double floor = printfit::MinReliableThreadMm(*proc);
      if (floor > 0.0 && basic < floor)
        out["note"] = "printed threads are unreliable below this size in " +
                      proc->process +
                      " (community practice) — tap a plain bore or use a "
                      "heat-set insert";
    }
    return out;
  }
  if (m == "check_fit") {
    // ISO 286 limits and fits (docs/tolerances/README.md#checks). Two jobs in
    // one method because they are one question asked at two depths: `hole`
    // alone is "what may this bore be", `hole` + `shaft` is "will they go
    // together".
    //
    // Nothing here touches geometry. A size tolerance leaves nominal alone —
    // that is what separates it from a clearance-hole fit, which picks a
    // different diameter outright (parts::HoleDiameter).
    //
    // params: {hole: {class, feature_id?, element?, part?}, shaft?: {...},
    //          size?, intent?}
    const auto measureSide =
        [&](const json& s, const char* which,
            const Document** measuredIn) -> std::optional<double> {
      const bool hasFeature = s.contains("feature_id");
      const bool hasElement = s.contains("element");
      if (!hasFeature && !hasElement)
        return std::nullopt;
      if (!hasFeature || !hasElement)
        throw std::runtime_error(
            std::string("check_fit: ") + which +
            " needs BOTH feature_id and element to name a face — "
            "list_elements gives the pair");
      const Document* d2 =
          s.contains("part")   ? ResolvePartParam(state, s.at("part"))
          : p.contains("part") ? ResolvePartParam(state, p.at("part"))
                               : &RequirePartDoc(doc, "check_fit");
      const ElementRef ref{s.at("feature_id").get<uint64_t>(),
                           s.at("element").get<std::string>()};
      const auto el = Measure::Describe(d2->GetBodies(), ref);
      if (!el)
        throw std::runtime_error(std::string("check_fit: ") + which +
                                 " element '" + ref.name +
                                 "' does not resolve (list_elements)");
      if (!el->hasRadius)
        throw std::runtime_error(
            std::string("check_fit: ") + which + " is a " + el->shape +
            ", which has no diameter. A fit is between round things — a "
            "cylindrical face or a circular edge on each side");
      *measuredIn = d2;
      return el->radius * 2.0;
    };
    // Whether a side is PRINTED (docs/tolerances/README.md#printed-parts): a
    // measured face on a part that declares a print process is, and a side
    // may say so itself with {print: {process, material}} — the way to ask
    // "what do I model for an H7 bore in PETG" before there is a model. The
    // two must agree: a face is printed one way.
    const auto sidePrint =
        [&](const json& s, const char* which,
            const Document* measuredIn) -> std::optional<printfit::Process> {
      std::optional<printfit::Process> declared;
      if (s.contains("print")) {
        const json& pj = s.at("print");
        if (!pj.is_object() ||
            !(pj.contains("profile") ||
              (pj.contains("process") && pj.contains("material"))))
          throw std::runtime_error(
              std::string("check_fit: ") + which +
              ".print is {process, material}, e.g. {process: \"fdm\", "
              "material: \"petg\"}, or {profile: \"<one of yours>\"}");
        declared = ProcessFromParams(state, pj, "check_fit");
      }
      std::optional<printfit::Process> part;
      if (measuredIn != nullptr)
        part = measuredIn->PrintProcess();
      if (declared && part &&
          printfit::Label(*declared) != printfit::Label(*part) &&
          !(declared->profile == std::nullopt &&
            declared->process == part->process &&
            declared->material == part->material))
        throw std::runtime_error(
            std::string("check_fit: ") + which + ".print says " +
            printfit::Label(*declared) +
            " but the part it is measured on is printed in " +
            printfit::Label(*part) +
            " (set_print_process) — a face is printed one way");
      // The face's own part, when there is one: it carries the copy the
      // face was modelled with.
      return part ? part : declared;
    };

    if (!p.contains("hole"))
      throw std::runtime_error(
          "check_fit: `hole` is required, as {class: \"H7\"} at least");
    const json& holeJson = p.at("hole");
    if (!holeJson.contains("class"))
      throw std::runtime_error(
          "check_fit: hole.class is required, e.g. \"H7\"");
    const std::string holeCls = holeJson.at("class").get<std::string>();
    const Document* holeDoc = nullptr;
    const std::optional<double> holeMeasured =
        measureSide(holeJson, "hole", &holeDoc);
    const std::optional<printfit::Process> holeProc =
        sidePrint(holeJson, "hole", holeDoc);

    const bool hasShaft = p.contains("shaft");
    std::string shaftCls;
    std::optional<double> shaftMeasured;
    std::optional<printfit::Process> shaftProc;
    if (hasShaft) {
      const json& shaftJson = p.at("shaft");
      if (!shaftJson.contains("class"))
        throw std::runtime_error(
            "check_fit: shaft.class is required, e.g. \"g6\"");
      shaftCls = shaftJson.at("class").get<std::string>();
      const Document* shaftDoc = nullptr;
      shaftMeasured = measureSide(shaftJson, "shaft", &shaftDoc);
      shaftProc = sidePrint(shaftJson, "shaft", shaftDoc);
    }
    // A printed, measured side is at what it PRINTS: the model carries the
    // allowance. The basic size below is decided from these.
    std::optional<ui::PrintedDiameter> holeFromModel, shaftFromModel;
    {
      std::string why;
      if (holeProc && holeMeasured) {
        holeFromModel = ui::PrintedForModelled(*holeProc, true, *holeMeasured,
                                               holeCls, &why);
        if (!holeFromModel)
          throw std::runtime_error("check_fit: hole: " + why);
      }
      if (shaftProc && shaftMeasured) {
        shaftFromModel = ui::PrintedForModelled(*shaftProc, false,
                                                *shaftMeasured, shaftCls, &why);
        if (!shaftFromModel)
          throw std::runtime_error("check_fit: shaft: " + why);
      }
    }
    const std::optional<double> holeBasic =
        holeFromModel ? std::optional<double>(holeFromModel->basic)
                      : holeMeasured;
    const std::optional<double> shaftBasic =
        shaftFromModel ? std::optional<double>(shaftFromModel->basic)
                       : shaftMeasured;

    // What nominal to work at. An explicit `size` wins; otherwise the model
    // is asked, and two answers that disagree are refused rather than
    // averaged or silently halved. See the describe row for why.
    const auto mm = [](double v) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.4f mm", v);
      return std::string(buf);
    };
    double size = 0.0;
    std::string sizeFrom;
    if (p.contains("size")) {
      size = p.at("size").get<double>();
      sizeFrom = "given";
    } else if (holeBasic && shaftBasic) {
      constexpr double kAgree = 1e-5; // mm, far under the finest grade (3 um)
      if (std::abs(*holeBasic - *shaftBasic) > kAgree) {
        std::string printed;
        if (holeFromModel)
          printed += "; the printed hole comes out " + mm(*holeBasic);
        if (shaftFromModel)
          printed += "; the printed shaft comes out " + mm(*shaftBasic);
        if (holeFromModel && !shaftFromModel)
          printed += " — to print " + mm(*shaftBasic) + " model the hole " +
                     mm(*shaftBasic + holeFromModel->allowance);
        else if (shaftFromModel && !holeFromModel)
          printed += " — to print " + mm(*holeBasic) + " model the shaft " +
                     mm(*holeBasic + shaftFromModel->allowance);
        throw std::runtime_error(
            "check_fit: the two faces are not the same basic size — the hole "
            "measures " +
            mm(*holeMeasured) + " and the shaft " + mm(*shaftMeasured) +
            printed +
            ". An ISO fit is a band either side of ONE nominal, so a "
            "clearance already modelled into the geometry would be counted "
            "twice. Model both at the same size, or pass `size` to say which "
            "nominal the classes are against.");
      }
      size = *holeBasic;
      sizeFrom = holeFromModel || shaftFromModel ? "printed" : "measured";
    } else if (holeBasic || shaftBasic) {
      size = holeBasic ? *holeBasic : *shaftBasic;
      sizeFrom = holeFromModel || shaftFromModel ? "printed" : "measured";
    } else {
      throw std::runtime_error(
          "check_fit: no nominal size — pass `size`, or name a face on either "
          "side with {feature_id, element} and it is measured");
    }

    const auto band = [&](const fits::Band& b, const std::string& cls) {
      return json{{"class", cls},       {"lower", b.lower},
                  {"upper", b.upper},   {"min", b.Min(size)},
                  {"max", b.Max(size)}, {"it_grade", b.grade},
                  {"it", b.it}};
    };

    json out{{"size", size}, {"size_from", sizeFrom}};
    if (holeMeasured || shaftMeasured) {
      json measured = json::object();
      if (holeMeasured)
        measured["hole"] = *holeMeasured;
      if (shaftMeasured)
        measured["shaft"] = *shaftMeasured;
      out["measured"] = std::move(measured);
    }

    // What to MODEL, per printed side, at the basic size decided above.
    {
      json print = json::object();
      json sentences = json::array();
      const auto printedSide = [&](const std::optional<printfit::Process>& pr,
                                   bool hole, const std::string& cls,
                                   const std::optional<double>& measured,
                                   const char* key) {
        if (!pr)
          return;
        std::string why;
        const auto d =
            ui::PrintedForBasic(*pr, hole, size, measured, cls, &why);
        if (!d)
          throw std::runtime_error(std::string("check_fit: ") + key + ": " +
                                   why);
        print[key] = PrintedDiameterJson(*d, cls);
        sentences.push_back(d->Sentence(cls));
        // A part printed with a profile that is not the store's says so:
        // the answer is from the part's own copy
        // (docs/tolerances/README.md#print-profiles).
        if (const std::string note = ProfileMatchNote(state, *pr);
            !note.empty())
          sentences.push_back(note);
      };
      printedSide(holeProc, true, holeCls, holeMeasured, "hole");
      if (hasShaft)
        printedSide(shaftProc, false, shaftCls, shaftMeasured, "shaft");
      if (!print.empty()) {
        out["print"] = std::move(print);
        out["print_notes"] = std::move(sentences);
      }
    }

    std::string why;
    if (!hasShaft) {
      const auto b = fits::Limits(size, holeCls, &why);
      if (!b)
        throw std::runtime_error("check_fit: " + why);
      out["hole"] = band(*b, holeCls);
      return out;
    }

    const auto pair = fits::Fit(size, holeCls, shaftCls, &why);
    if (!pair)
      throw std::runtime_error("check_fit: " + why);
    out["hole"] = band(pair->hole, holeCls);
    out["shaft"] = band(pair->shaft, shaftCls);
    out["kind"] = fits::FitKindName(pair->kind);
    out["min_clearance"] = pair->minClearance;
    out["max_clearance"] = pair->maxClearance;
    out["note"] = "worst case over two independent bands; form, position and "
                  "surface finish are not evaluated";
    if (p.contains("intent")) {
      const std::string intent = p.at("intent").get<std::string>();
      const auto want = fits::FitKindFromName(intent);
      if (!want)
        throw std::runtime_error(
            "check_fit: unknown intent '" + intent +
            "'. Accepted: clearance, transition, interference.");
      out["intent"] = intent;
      out["intent_ok"] = *want == pair->kind;
    }
    return out;
  }
  if (m == "set_tolerance") {
    // What a face's SIZE is allowed to vary by, or — with `b` — what the
    // DISTANCE between two faces is
    // (docs/tolerances/README.md#the-stored-tolerance). params: {feature_id,
    // element, b?: {feature_id, element},
    //          class? | lower?+upper?, comment?}
    Document& part = PartToEdit(state, doc, p, "set_tolerance");
    const ElementRef ref{p.at("feature_id").get<uint64_t>(),
                         p.at("element").get<std::string>()};
    const auto el = Measure::Describe(part.GetBodies(), ref);
    if (!el)
      throw std::runtime_error("set_tolerance: element '" + ref.name +
                               "' does not resolve (list_elements)");

    ElementRef ref2;
    double nominal = 0.0;
    const bool hasClass = p.contains("class");
    if (p.contains("b")) {
      const json& bj = p.at("b");
      if (!bj.contains("feature_id") || !bj.contains("element"))
        throw std::runtime_error(
            "set_tolerance: `b` needs both feature_id and element to name the "
            "face the distance is measured to");
      ref2 = ElementRef{bj.at("feature_id").get<uint64_t>(),
                        bj.at("element").get<std::string>()};
      if (ref2.featureId == ref.featureId && ref2.name == ref.name)
        throw std::runtime_error(
            "set_tolerance: `b` names the same face as `element` — a face is "
            "no distance from itself");
      if (!Measure::Describe(part.GetBodies(), ref2))
        throw std::runtime_error("set_tolerance: element '" + ref2.name +
                                 "' does not resolve (list_elements)");
      // An ISO class states how a shaft or a bore may vary. It says nothing
      // about the gap between two faces, so accepting one here would store a
      // band that means something else entirely.
      if (hasClass)
        throw std::runtime_error(
            "set_tolerance: an ISO class is a SIZE — it says how a shaft or a "
            "bore may vary, not how far apart two faces are. Give `lower` and "
            "`upper` in mm for a distance.");
      const auto dist = Measure::DistanceDetailed(part.GetBodies(), ref, ref2);
      if (!dist)
        throw std::runtime_error(
            "set_tolerance: the distance between those two faces could not be "
            "measured — an OpenCASCADE extrema failed");
      nominal = dist->distance;
    } else {
      // Only round features carry a SIZE yet, and a tolerance nothing can
      // check is a silent no-op — so it is refused rather than stored. A
      // planar face is not sizeless, though: name a second face and it
      // becomes a distance.
      if (!el->hasRadius)
        throw std::runtime_error(
            "set_tolerance: " + ref.name + " is a " + el->shape +
            ", which has no size of its own — only a cylindrical face or a "
            "circular edge does. For a length, name the face it is measured "
            "to as `b`.");
      nominal = el->radius * 2.0;
    }

    const bool hasLower = p.contains("lower"), hasUpper = p.contains("upper");
    if (hasClass && (hasLower || hasUpper))
      throw std::runtime_error(
          "set_tolerance: give a `class` OR a `lower`/`upper` pair, not both "
          "— two statements of the same band can disagree, and then which is "
          "the part made to?");
    if (!hasClass && !(hasLower && hasUpper))
      throw std::runtime_error(
          hasLower || hasUpper
              ? "set_tolerance: `lower` and `upper` come as a pair — half a "
                "band silently becomes an unbounded one"
              : "set_tolerance: needs a `class` (\"H7\") or a "
                "`lower`/`upper` pair in mm");

    Tolerance t;
    t.ref = ref;
    t.ref2 = ref2;
    t.comment = p.value("comment", std::string{});
    if (hasClass) {
      t.isoClass = p.at("class").get<std::string>();
      // Validated HERE, against this face's own size, rather than when
      // something later looks: a class that cannot apply to the face it is
      // being put on is a typo, and the moment to say so is now.
      std::string why;
      if (!fits::Limits(nominal, t.isoClass, &why))
        throw std::runtime_error("set_tolerance: " + why);
    } else {
      t.lower = p.at("lower").get<double>();
      t.upper = p.at("upper").get<double>();
      if (t.lower > t.upper)
        throw std::runtime_error(
            "set_tolerance: lower must not exceed upper — a reversed band "
            "admits nothing and reads as a valid one");
    }

    auto cmd = std::make_unique<SetToleranceCommand>(std::move(t));
    SetToleranceCommand* raw = cmd.get();
    CommandsFor(state, part).Execute(std::move(cmd), part);
    const Tolerance* stored = part.FindTolerance(raw->ToleranceId());
    json out{{"id", stored->id},
             {"element", stored->ref.name},
             {"feature_id", stored->ref.featureId},
             {"kind", stored->IsDistance() ? "distance" : "size"},
             {"nominal", nominal}};
    if (stored->IsDistance())
      out["b"] = {{"feature_id", stored->ref2.featureId},
                  {"element", stored->ref2.name}};
    if (!stored->isoClass.empty()) {
      out["class"] = stored->isoClass;
      std::string why;
      const auto b = fits::Limits(nominal, stored->isoClass, &why);
      out["min"] = b->Min(nominal);
      out["max"] = b->Max(nominal);
    } else {
      out["lower"] = stored->lower;
      out["upper"] = stored->upper;
      out["min"] = nominal + stored->lower;
      out["max"] = nominal + stored->upper;
    }
    return out;
  }
  if (m == "list_tolerances") {
    const Document& part = p.contains("part")
                               ? *ResolvePartParam(state, p.at("part"))
                               : RequirePartDoc(doc, "list_tolerances");
    json list = json::array();
    for (const Tolerance& t : part.GetTolerances()) {
      json e{{"id", t.id},
             {"feature_id", t.ref.featureId},
             {"element", t.ref.name},
             {"kind", t.IsDistance() ? "distance" : "size"}};
      if (!t.comment.empty())
        e["comment"] = t.comment;
      if (t.IsDistance())
        e["b"] = {{"feature_id", t.ref2.featureId}, {"element", t.ref2.name}};
      // A size resolves to a radius; a distance resolves to an extrema
      // between two faces, and either can stop resolving on its own.
      const auto el = Measure::Describe(part.GetBodies(), t.ref);
      std::optional<double> measured;
      if (t.IsDistance()) {
        if (el && Measure::Describe(part.GetBodies(), t.ref2))
          if (const auto d =
                  Measure::DistanceDetailed(part.GetBodies(), t.ref, t.ref2))
            measured = d->distance;
      } else if (el && el->hasRadius) {
        measured = el->radius * 2.0;
      }
      // Kept, not dropped: a tolerance whose face an upstream edit renamed is
      // still what somebody decided about this part, and saying so is more
      // use than quietly having fewer tolerances than yesterday.
      e["resolved"] = measured.has_value();
      if (t.isoClass.empty()) {
        e["lower"] = t.lower;
        e["upper"] = t.upper;
      } else {
        e["class"] = t.isoClass;
      }
      if (measured) {
        const double nominal = *measured;
        e["nominal"] = nominal;
        if (t.isoClass.empty()) {
          e["min"] = nominal + t.lower;
          e["max"] = nominal + t.upper;
        } else {
          std::string why;
          if (const auto b = fits::Limits(nominal, t.isoClass, &why)) {
            e["min"] = b->Min(nominal);
            e["max"] = b->Max(nominal);
          } else {
            e["error"] = why;
          }
        }
      }
      list.push_back(std::move(e));
    }
    return json{{"tolerances", std::move(list)}};
  }
  if (m == "clear_tolerance") {
    Document& part = PartToEdit(state, doc, p, "clear_tolerance");
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!part.FindTolerance(id))
      throw std::runtime_error("clear_tolerance: no tolerance with id " +
                               std::to_string(id) +
                               " (list_tolerances shows them)");
    CommandsFor(state, part)
        .Execute(std::make_unique<ClearToleranceCommand>(id), part);
    return json{{"ok", true}, {"id", id}};
  }
  if (m == "stack_up") {
    const Document& part = p.contains("part")
                               ? *ResolvePartParam(state, p.at("part"))
                               : RequirePartDoc(doc, "stack_up");
    if (!p.contains("terms") || !p.at("terms").is_array())
      throw std::runtime_error(
          "stack_up: `terms` is an array of {tolerance: id, subtract?}");
    std::vector<tolstack::Term> terms;
    for (const json& tj : p.at("terms")) {
      tolstack::Term t;
      if (tj.is_number_unsigned() || tj.is_number_integer()) {
        t.toleranceId = tj.get<uint64_t>();
      } else {
        if (!tj.contains("tolerance"))
          throw std::runtime_error(
              "stack_up: each term needs a `tolerance` id");
        t.toleranceId = tj.at("tolerance").get<uint64_t>();
        t.subtract = tj.value("subtract", false);
      }
      terms.push_back(t);
    }
    std::string why;
    const auto sum = tolstack::Sum(part, terms, &why);
    if (!sum)
      throw std::runtime_error("stack_up: " + why);
    json rows = json::array();
    for (const tolstack::TermResult& r : sum->terms)
      rows.push_back({{"tolerance", r.toleranceId},
                      {"what", r.what},
                      {"subtract", r.subtract},
                      {"nominal", r.nominal},
                      {"lower", r.lower},
                      {"upper", r.upper}});
    return json{
        {"terms", std::move(rows)},
        {"nominal", sum->nominal},
        {"min", sum->min},
        {"max", sum->max},
        {"note",
         "worst case over the stated terms, assumed INDEPENDENT. Two lengths "
         "driven by one variable cancel rather than add, and this cannot "
         "tell — a nominal is measured, so nothing links it back to what "
         "drove it. No distribution, and no form or position error."}};
  }
  if (m == "check_fits") {
    // The walk lives in ui::CheckFits so a person clicking "Check fits" and a
    // script calling this get the same verdict from the same code — the
    // arrangement check_fasteners already uses. This is only its wire form.
    if (!state.assembly)
      throw std::runtime_error(
          "check_fits needs an assembly tab — a fit is between two placed "
          "parts, and only an assembly says which faces meet");
    json entries = json::array();
    int verified = 0, unverified = 0, failed = 0, warned = 0, reported = 0;
    for (const ui::FitCheck& c : ui::CheckFits(state)) {
      json e{{"mate", c.mateId},
             {"mate_name", c.mateName},
             {"mate_type", c.mateType},
             {"status", c.status}};
      if (!c.reason.empty())
        e["reason"] = c.reason;
      if (c.computed) {
        e["size"] = c.size;
        e["hole"] = {{"occurrence", c.holeOccurrence},
                     {"class", c.holeClass},
                     {"min", c.holeMin},
                     {"max", c.holeMax}};
        e["shaft"] = {{"occurrence", c.shaftOccurrence},
                      {"class", c.shaftClass},
                      {"min", c.shaftMin},
                      {"max", c.shaftMax}};
        if (c.holePrint)
          e["hole"]["print"] = PrintedDiameterJson(*c.holePrint, c.holeClass);
        if (c.shaftPrint)
          e["shaft"]["print"] =
              PrintedDiameterJson(*c.shaftPrint, c.shaftClass);
        e["kind"] = c.kind;
        e["min_clearance"] = c.minClearance;
        e["max_clearance"] = c.maxClearance;
        e["note"] = "worst case over two independent bands; form, position "
                    "and surface finish are not evaluated";
      }
      if (c.status == "pass")
        verified++;
      else if (c.status == "warn")
        warned++;
      else if (c.status == "fail")
        failed++;
      else if (c.status == "reported")
        reported++;
      else
        unverified++;
      entries.push_back(std::move(e));
    }
    // Never a green badge over unknowns: unverified outranks pass.
    const std::string status = failed            ? "fail"
                               : unverified      ? "unverified"
                               : warned          ? "warn"
                               : verified        ? "pass"
                               : entries.empty() ? "none"
                                                 : "reported";
    json out{{"fits", std::move(entries)}, {"status", status},
             {"passed", verified},         {"warned", warned},
             {"failed", failed},           {"reported", reported},
             {"unverified", unverified}};
    {
      // The tolerances round arrived at this receipt's rule on its own — its
      // doc row says "anything not fully answerable is `unverified` WITH the
      // reason and never a pass" — in its own vocabulary. This is that
      // vocabulary made common, the same way run_motion_study's
      // collision_status was: a caller reads one field the same way across
      // every verdict RPC instead of learning each one's.
      //
      // `reported` is the interesting state. A FASTENED round joint is not
      // judged, because nothing-moves is equally true of a shrunk-on gear and
      // of a bracket bolted through a clearance hole. That is not a pass and
      // it is not a failure: it is a fit nobody stated the intent for.
      receipt::Receipt receipt;
      const receipt::Oracle oracle{"designer/iso286-fits", "1"};
      const std::string claim = "every declared fit matches what its mate is "
                                "for";
      if (entries.empty()) {
        receipt.Unverifiable(claim, oracle,
                             "no mate in this assembly has a tolerance on both "
                             "of its faces, so there is no fit to check");
      } else {
        receipt::Claim c(claim, oracle,
                         failed ? receipt::Verdict::Fail
                                : receipt::Verdict::Pass);
        c.basis = receipt::Basis::Verified;
        c.value = static_cast<double>(verified);
        c.unit = "fits";
        c.note = std::to_string(warned) + " warned, " +
                 std::to_string(reported) +
                 " reported but not judged (a fastened joint states no "
                 "intent)";
        receipt.Add(std::move(c));
        if (unverified)
          receipt.Unverifiable(
              "every declared fit was answerable", oracle,
              std::to_string(unverified) +
                  " fit(s) could not be computed; each says why in its own "
                  "entry");
      }
      AttachReceipt(out, receipt);
    }
    return out;
  }
  if (m == "measure") {
    // What ONE side of a measurement names. Each of `a` and `b` carries its
    // own {part} / {occurrence}, so a measurement can span two parts or two
    // occurrences — which the GUI has done since assembly measure shipped
    // and the RPC could not, because it resolved a single body set and used
    // it for both refs.
    //
    // NOTE: `interference` has a near-twin of the occurrence resolution
    // below. They are not yet one function; that is a recorded follow-up in
    // docs/inspect/README.md#what-it-does-not-do-yet rather than a refactor
    // smuggled into this change, because interference's version is load-bearing
    // for the contact sweep and shares a prefetched body map with it.
    struct Side {
      std::vector<Body> owned; // world-placed bodies, for an occurrence
      const std::vector<Body>* bodies = nullptr;
      ElementRef ref; // name empty => the whole set
      std::string label;
    };
    auto resolveSide = [&](const json& r, const char* which) {
      auto side = std::make_unique<Side>();
      if (r.contains("occurrence")) {
        if (!state.assembly)
          throw std::runtime_error(
              std::string("measure: ") + which +
              ".occurrence needs an assembly tab (or open the assembly and "
              "pass `occurrence`); use `part` for a part document");
        const json& v = r.at("occurrence");
        const Occurrence* occ = nullptr;
        glm::dmat4 world(1.0);
        std::string label;
        if (v.is_number_unsigned() || v.is_number_integer()) {
          occ = state.assembly->FindById(v.get<uint64_t>());
          if (!occ)
            throw std::runtime_error("measure: occurrence id " + v.dump() +
                                     " not found (see list_occurrences)");
          label = occ->name;
        } else {
          const std::string name = v.get<std::string>();
          for (const Occurrence& o : state.assembly->GetOccurrences())
            if (o.name == name) {
              occ = &o;
              label = o.name;
              break;
            }
          if (!occ && name.find('/') != std::string::npos) {
            // A5: a part INSIDE a sub-assembly, by path.
            auto hit = assembly::ResolvePath(*state.assembly, name,
                                             ui::PartResolverFor(state),
                                             ui::AssemblyResolverFor(state));
            if (!hit)
              throw std::runtime_error(hit.error());
            occ = hit->occ;
            world = hit->world;
            label = name;
          }
          if (!occ)
            throw std::runtime_error(
                "measure: occurrence '" + name +
                "' not found (see list_occurrences; a part inside a "
                "sub-assembly is addressed by path, e.g. \"Gangway/Board "
                "19\")");
        }
        // Every leaf under it, placed in world space — a sub-assembly
        // measures as the one thing it looks like, not as its parts.
        if (label == occ->name) {
          for (const assembly::FlatInstance& inst :
               ui::FlattenActiveAssembly(state)) {
            if (inst.rootOccurrenceId != occ->id)
              continue;
            for (const Body& b : inst.part->GetBodies()) {
              Body nb = b;
              nb.shape = b.shape.Transformed(inst.world);
              side->owned.push_back(std::move(nb));
            }
          }
        } else if (const Document* pd = ui::OccurrencePartDoc(state, *occ)) {
          for (const Body& b : pd->GetBodies()) {
            Body nb = b;
            nb.shape = b.shape.Transformed(world);
            side->owned.push_back(std::move(nb));
          }
        }
        if (side->owned.empty())
          throw std::runtime_error("measure: occurrence '" + label +
                                   "' resolved to no geometry");
        side->bodies = &side->owned;
        side->label = label;
      } else {
        const Document* doc2 =
            r.contains("part")   ? ResolvePartParam(state, r.at("part"))
            : p.contains("part") ? ResolvePartParam(state, p.at("part"))
                                 : &RequirePartDoc(doc, "measure");
        side->bodies = &doc2->GetBodies();
        side->label = "";
      }
      // A whole body: {feature_id} with no `element`. A whole occurrence:
      // no `element` either, and featureId 0 means every body under it.
      side->ref = ElementRef{r.value("feature_id", uint64_t{0}),
                             r.value("element", std::string{})};
      return side;
    };
    // Geometry queries over stable element names (I1). params:
    // {a: {feature_id, element}, b?: {feature_id, element}, part?}.
    // With `b`: minimum distance between the two elements. Without: the
    // element's own measure (face area / edge length). `part` (name or stable
    // id) resolves the elements against that part's bodies without switching
    // the active tab.
    const auto sideA = resolveSide(p.at("a"), "a");
    const Measure::Target ta{sideA->bodies, glm::dmat4(1.0), sideA->ref};
    if (p.contains("b")) {
      const auto sideB = resolveSide(p.at("b"), "b");
      const Measure::Target tb{sideB->bodies, glm::dmat4(1.0), sideB->ref};
      // What to measure between them. Refused, never substituted: a request
      // for an angle answered with a distance is plausible and wrong, and a
      // typo'd kind must not silently become the default.
      const std::string kind = p.value("kind", "distance");
      if (kind != "distance" && kind != "angle" && kind != "both") {
        throw std::runtime_error("measure: unknown kind '" + kind +
                                 "'. Accepted: distance, angle, both.");
      }
      // WHICH distance — minimum (default), maximum (the clearance
      // question), or centre to centre (hole spacing).
      Measure::DistanceKind dk = Measure::DistanceKind::Minimum;
      if (p.contains("distance_kind")) {
        const std::string dks = p.at("distance_kind").get<std::string>();
        if (!Measure::DistanceKindFromString(dks, dk)) {
          std::string names;
          for (const char* n : Measure::DistanceKindNames())
            names += (names.empty() ? "" : ", ") + std::string(n);
          throw std::runtime_error("measure: unknown distance_kind '" + dks +
                                   "'. Accepted: " + names + ".");
        }
      }
      json out;
      // `mode: mesh` answers the minimum distance from the TRIANGLES, with
      // the deflection it was decided to — the same bargain `interference`
      // offers, for the same reason. An exact distance between two solids
      // whose faces are all about equally far apart is pathological (two
      // lofted propellers 151 mm apart: 55 s), and a clearance question does
      // not need a micron: it needs to know there is 30 mm and not 2.
      const std::string mode = p.value("mode", "exact");
      if (mode != "exact" && mode != "mesh")
        throw std::runtime_error("measure: mode is exact (the default) or "
                                 "mesh, not '" +
                                 mode + "'");
      if (mode == "mesh") {
        if (kind == "angle")
          throw std::runtime_error(
              "measure: an angle is exact or nothing — mesh mode answers a "
              "DISTANCE, and an angle off a tessellation is a lie about a "
              "surface's direction");
        if (!ta.Whole() || !tb.Whole())
          throw std::runtime_error(
              "measure: mesh mode takes two WHOLE shapes (a body or an "
              "occurrence). A face's distance is exact or it is nothing: a "
              "triangle is not on the face it stands for");
        if (dk != Measure::DistanceKind::Minimum)
          throw std::runtime_error(
              "measure: mesh mode answers the MINIMUM distance; " +
              std::string(Measure::ToString(dk)) + " is exact only");
        const collide::Gap gap = Measure::MeshGap(ta, tb);
        if (!gap.known)
          throw std::runtime_error(
              "measure: one of these has no triangles to measure — an empty "
              "body, or one whose tessellation failed");
        out["distance"] = gap.distance;
        out["distance_kind"] = Measure::ToString(dk);
        out["mode"] = mode;
        out["deflection_mm"] = gap.deflection;
        // What the answer is bounded by, which is the SUM of the two
        // meshes' deflections and not the coarser one: each tessellation
        // can be off by its own, in opposite directions. A caller asking
        // whether there is 2 mm of clearance subtracts THIS.
        out["tolerance_mm"] = gap.tolerance;
        out["clash"] = gap.distance <= 0.0;
        return out;
      }
      if (kind != "angle") {
        const auto d = Measure::DistanceBetween(ta, tb, dk);
        if (!d)
          throw std::runtime_error("measure: an element did not resolve");
        out["distance"] = d->distance;
        out["distance_kind"] = Measure::ToString(dk);
        out["point_a"] = {d->pointA.x, d->pointA.y, d->pointA.z};
        out["point_b"] = {d->pointB.x, d->pointB.y, d->pointB.z};
        // SIGNED, b - a. Which way it is matters: 40 mm in +X and 40 mm in
        // -X are a part and its mirror image.
        const glm::dvec3 delta = d->pointB - d->pointA;
        out["delta"] = {delta.x, delta.y, delta.z};
        // Two WHOLE shapes that interpenetrate are 0 apart, which is true
        // and the least useful available fact. Say whether they clash.
        // The minimum just measured is passed on: two shapes measurably
        // apart need no boolean to say they do not overlap.
        const double apart =
            dk == Measure::DistanceKind::Minimum ? d->distance : -1.0;
        if (const auto ov = Measure::OverlapBetween(ta, tb, apart)) {
          out["overlap_volume"] = ov->volume;
          out["clash"] = ov->volume > 0.0;
        }
      }
      if (kind != "distance") {
        if (ta.Whole() || tb.Whole()) {
          throw std::runtime_error(
              "measure: an angle needs a face or an edge on both sides — a "
              "whole body or occurrence has no single direction to take one "
              "against.");
        }
        const auto ang = Measure::Angle(*ta.bodies, ta.ref, ta.xf, *tb.bodies,
                                        tb.ref, tb.xf);
        if (!ang) {
          // Name WHICH element has no direction, and what does — a bare
          // "could not" leaves the caller guessing which ref to change.
          const bool aOk = Measure::DirectionOf(*ta.bodies, ta.ref).has_value();
          const bool bOk = Measure::DirectionOf(*tb.bodies, tb.ref).has_value();
          const std::string who = !aOk && !bOk ? "neither element"
                                  : !aOk       ? "element a"
                                               : "element b";
          throw std::runtime_error(
              "measure: " + who +
              " defines a direction to take an angle against. A plane, line, "
              "circle, cylinder, cone or torus does; a sphere or a spline "
              "does not.");
        }
        out["angle"] = ang->degrees;
        out["acute"] = ang->acute;
        out["parallel"] = ang->parallel;
        out["perpendicular"] = ang->perpendicular;
        out["basis"] = ang->basis;
      }
      return out;
    }
    if (p.contains("kind") || p.contains("distance_kind")) {
      throw std::runtime_error(
          "measure: `kind` and `distance_kind` choose what to measure BETWEEN "
          "two things — pass `b` as well, or drop them for a's own measure.");
    }
    // A WHOLE body or occurrence on its own reports what a part HAS.
    if (ta.Whole()) {
      const auto d = Measure::DescribeTarget(ta);
      if (!d)
        throw std::runtime_error("measure: that body or occurrence did not "
                                 "resolve to any geometry");
      return json{{"kind", "solid"},
                  {"volume", d->volume},
                  {"surface_area", d->surfaceArea},
                  {"size", {d->size.x, d->size.y, d->size.z}},
                  {"solids", d->solids}};
    }

    const auto e = Measure::Describe(*ta.bodies, ta.ref);
    if (!e)
      throw std::runtime_error("measure: element did not resolve");
    json out = {{"kind", e->kind}, {"shape", e->shape}};
    if (e->kind == "face")
      out["area"] = e->area;
    else
      out["length"] = e->length;
    // A radius wherever one is defined: a circular/arc edge, and now a
    // cylinder/sphere/torus FACE too — `query_geometry` has returned the
    // face radius all along off the same classification, so an agent could
    // read a bore's diameter and the person clicking that bore could not.
    if (e->hasRadius) {
      out["radius"] = e->radius;
      out["diameter"] = e->radius * 2.0;
    }
    if (e->hasAxis)
      out["axis"] = {e->axis.x, e->axis.y, e->axis.z};
    // How a person would name it, so a caller reporting a measurement can
    // say "the top face" instead of "1/face3".
    out["name"] = Measure::HumanName(*ta.bodies, ta.ref, state.ActiveUnits());
    return out;
  }
  if (m == "frame") {
    const auto v3 = [](const glm::dvec3& v) { return json{v.x, v.y, v.z}; };
    const auto frameJson = [&](const glm::dmat4& mtx, const char* space,
                               const std::string& source) {
      json out{{"origin", v3(frame::Origin(mtx))},
               {"x", v3(frame::XAxis(mtx))},
               {"y", v3(frame::YAxis(mtx))},
               {"z", v3(frame::ZAxis(mtx))},
               {"space", space},
               {"source", source}};
      json m = json::array();
      for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
          m.push_back(mtx[c][r]);
      out["matrix"] = std::move(m);
      return out;
    };
    // The frame in a PART's own space: a named Frame, an element, or the
    // origin. The same resolution a mate connector performs.
    const auto localFrame = [&](const Document& part,
                                std::string& source) -> glm::dmat4 {
      if (p.contains("frame")) {
        const json& fv = p.at("frame");
        const FrameFeature* hit = nullptr;
        std::string frames;
        for (const auto& f : part.GetFeatures()) {
          const auto* fr = dynamic_cast<const FrameFeature*>(f.get());
          if (fr == nullptr)
            continue;
          if ((fv.is_number() && fr->GetId() == fv.get<uint64_t>()) ||
              (fv.is_string() && fr->GetName() == fv.get<std::string>()))
            hit = fr;
          if (!frames.empty())
            frames += ", ";
          frames += std::to_string(fr->GetId()) + " (" + fr->GetName() + ")";
        }
        if (hit == nullptr)
          throw std::runtime_error(
              "no Frame " + fv.dump() + " here; frames: " +
              (frames.empty() ? std::string("none (add_feature type Frame)")
                              : frames));
        if (!hit->GetError().empty())
          throw std::runtime_error("frame '" + hit->GetName() +
                                   "' did not resolve: " + hit->GetError());
        source = "frame " + std::to_string(hit->GetId()) + " (" +
                 hit->GetName() + ")";
        return hit->Matrix();
      }
      if (p.contains("feature_id") || p.contains("element")) {
        if (!p.contains("feature_id") || !p.contains("element"))
          throw std::runtime_error(
              "an element frame needs BOTH feature_id (the body's created_by, "
              "get_state) and element (its name, list_elements)");
        const ElementRef ref{p.at("feature_id").get<uint64_t>(),
                             p.at("element").get<std::string>()};
        glm::dvec3 o, z, x;
        bool circular = false;
        std::string why;
        if (!frame::ElementFrame(part.GetBodies(), ref, o, z, x, circular, why))
          throw std::runtime_error(why);
        source = std::string(circular ? "axis of " : "plane of ") + ref.name;
        return frame::Build(o, x, z);
      }
      source = "origin";
      return glm::dmat4(1.0);
    };
    if (p.contains("occurrence")) {
      if (!state.assembly)
        throw std::runtime_error(
            "frame: `occurrence` needs an assembly tab (switch with "
            "set_active_part, see list_parts); on a part tab pass `frame` or "
            "feature_id + element");
      const json& v = p.at("occurrence");
      const Occurrence* occ = nullptr;
      glm::dmat4 world(1.0);
      std::string label;
      bool placed = false;
      if (v.is_number_unsigned() || v.is_number_integer()) {
        occ = state.assembly->FindById(v.get<uint64_t>());
        if (!occ)
          throw std::runtime_error("frame: occurrence id " + v.dump() +
                                   " not found (see list_occurrences)");
        label = occ->name;
      } else {
        const std::string name = v.get<std::string>();
        for (const Occurrence& o : state.assembly->GetOccurrences())
          if (o.name == name) {
            occ = &o;
            label = o.name;
            break;
          }
        if (!occ && name.find('/') != std::string::npos) {
          auto hit = assembly::ResolvePath(*state.assembly, name,
                                           ui::PartResolverFor(state),
                                           ui::AssemblyResolverFor(state));
          if (!hit)
            throw std::runtime_error(hit.error());
          occ = hit->occ;
          world = hit->world;
          label = name;
          placed = true;
        }
        if (!occ)
          throw std::runtime_error(
              "frame: occurrence '" + name +
              "' not found (see list_occurrences; a part inside a "
              "sub-assembly is addressed by path, e.g. \"Unit/Part\")");
      }
      if (!placed) {
        // The SOLVED placement, from the flattened tree — never the raw
        // transform, which lags the solve.
        for (const assembly::FlatInstance& inst :
             ui::FlattenActiveAssembly(state))
          if (inst.rootOccurrenceId == occ->id && inst.innerIds.empty()) {
            world = inst.world;
            placed = true;
            break;
          }
        if (!placed)
          world = ui::OccurrenceMatrix(*occ);
      }
      std::string source = "placement of " + label;
      glm::dmat4 local(1.0);
      if (p.contains("frame") || p.contains("feature_id") ||
          p.contains("element")) {
        if (occ->refKind == OccurrenceRef::Assembly)
          throw std::runtime_error(
              "'" + label +
              "' is a sub-assembly, which has no frames or faces of its own: "
              "address the part inside it by path (\"Unit/Part\")");
        const Document* part = ui::OccurrencePartDoc(state, *occ);
        if (part == nullptr)
          throw std::runtime_error("the part of '" + label + "' is not loaded");
        std::string inner;
        local = localFrame(*part, inner);
        source = inner + " on " + label;
      }
      json out = frameJson(world * local, "world", source);
      out["occurrence"] = label;
      return out;
    }
    const Document& part = p.contains("part")
                               ? *ResolvePartParam(state, p.at("part"))
                               : RequirePartDoc(doc, "frame");
    std::string source;
    const glm::dmat4 local = localFrame(part, source);
    return frameJson(local, "part", source);
  }
  const auto snapshotRevision = PartRevision;
  if (m == "validate_snapshot") {
    const auto& previous = p.at("revision");
    const auto partId = previous.at("part_id").get<uint64_t>();
    ui::EnsureParts(state);
    auto* part = ui::PartDocById(state, partId);
    if (!part)
      throw ContractError(m, "stale_snapshot", "/params/revision",
                          "an existing stable part ID", previous);
    const auto revision = snapshotRevision(*part, partId);
    if (previous != revision)
      throw ContractError(m, "stale_snapshot", "/params/revision", revision,
                          previous);
    return json{{"valid", true}, {"revision", revision}};
  }
  if (m == "geometry_snapshot") {
    // Main-thread dispatch makes revision capture and measurement atomic.
    const auto partId = p.at("part_id").get<uint64_t>();
    ui::EnsureParts(state);
    auto* part = ui::PartDocById(state, partId);
    if (!part)
      throw ContractError(m, "unknown_part", "/params/part_id",
                          "an existing stable part ID", partId);
    const auto revision = snapshotRevision(*part, partId);
    const auto bodyIndex = p.at("body").get<size_t>();
    const auto& bodies = part->GetBodies();
    if (bodyIndex >= bodies.size())
      throw ContractError(m, "unknown_body", "/params/body",
                          {{"body_count", bodies.size()}}, bodyIndex);
    const auto& body = bodies[bodyIndex];
    const auto budget = p.value("max_elements", size_t{2000});
    if (body.elementMap.edgeNames.size() > budget)
      throw ContractError(m, "budget_exceeded", "/params/max_elements",
                          {{"edges_to_scan", body.elementMap.edgeNames.size()}},
                          budget);
    const bool circular = p.value("circular", false);
    const auto fields = p.value("fields", json::array({"center", "radius"}));
    const auto wants = [&](const char* field) {
      return std::find(fields.begin(), fields.end(), field) != fields.end();
    };
    json elements = json::array();
    for (const auto& [index, name] : body.elementMap.edgeNames) {
      const auto edge = Measure::DescribeEdgeAt(body, index);
      if (!edge)
        throw ContractError(m, "unresolved_element", "/params/body",
                            "measurable edge", name);
      if (circular && edge->kind != "circle" && edge->kind != "arc")
        continue;
      const ElementRef ref{body.createdBy, name};
      json entry{{"index", index},
                 {"kind", edge->kind},
                 {"ref", {{"feature_id", body.createdBy}, {"element", name}}}};
      if (wants("radius") && edge->hasRadius)
        entry["radius"] = edge->radius;
      if (wants("length"))
        entry["length"] = edge->length;
      if (wants("midpoint"))
        entry["midpoint"] = {edge->midpoint.x, edge->midpoint.y,
                             edge->midpoint.z};
      if (wants("center")) {
        const auto center = Measure::CentreOf(bodies, ref);
        if (!center)
          throw ContractError(m, "unresolved_element", "/params/body",
                              "edge center", name);
        entry["center"] = {center->x, center->y, center->z};
      }
      elements.push_back(std::move(entry));
    }
    return json{{"revision", revision},
                {"body", bodyIndex},
                {"space", "part"},
                {"elements", std::move(elements)}};
  }
  if (m == "curve_points") {
    const Document& src = SubjectPart(state, doc, p);
    const uint64_t want = p.value("curve", uint64_t{0});
    const CurveFeature* curve = nullptr;
    int found = 0;
    for (size_t i = 0; i < src.FeatureCount(); i++) {
      const auto* c = dynamic_cast<const CurveFeature*>(src.GetFeature(i));
      if (c == nullptr)
        continue;
      found++;
      if (want == 0 || c->GetId() == want)
        curve = c;
    }
    if (curve == nullptr) {
      throw std::runtime_error(
          want == 0
              ? "this part has no Curve3D to sample"
              : "no Curve3D with id " + std::to_string(want) + " in this part");
    }
    if (want == 0 && found > 1) {
      throw std::runtime_error(
          "this part has " + std::to_string(found) +
          " curves, so name the one you mean with `curve` — get_state lists "
          "them with their ids");
    }
    const double deflection = p.value("deflection_mm", 0.1);
    if (deflection <= 0.0)
      throw std::runtime_error("`deflection_mm` must be positive");

    json points = json::array(), at = json::array();
    double length = 0.0;
    for (const auto& [pt, s_mm] :
         tube::SampleCurve(curve->EdgeShape(), deflection)) {
      points.push_back({pt.x, pt.y, pt.z});
      at.push_back(s_mm);
      length = s_mm;
    }
    json out = json::object();
    out["curve"] = curve->GetId();
    out["name"] = curve->GetName();
    out["kind"] = Curve3DKindName(curve->kind);
    out["length_mm"] = length;
    out["points"] = std::move(points);
    out["at_mm"] = std::move(at);
    return out;
  }

  if (m == "tube_schedule") {
    const Document& src = SubjectPart(state, doc, p);
    // ONE derivation, shared with export_cut_list and export_bend_schedule,
    // so the schedule an agent queries and the CSV a shop is sent cannot
    // disagree about what the members are or how long they are cut.
    const std::vector<tube::MemberRow> rows =
        tube::Members(src, p.value("curve", uint64_t{0}));

    json tubes = json::array();
    int members = 0;
    for (const tube::MemberRow& row : rows) {
      members += row.quantity;
      json bends = json::array();
      int n = 0;
      for (const tube::Bend& b : row.bends) {
        json j = {{"index", ++n},
                  {"angle_deg", b.angleDeg},
                  {"radius_mm", b.radius},
                  {"straight_before_mm", b.straightBefore},
                  {"roll_deg", b.rollDeg}};
        if (row.outerDiameter > 1e-9) {
          j["radius_over_diameter"] = b.radius / row.outerDiameter;
          // Against the declared stock where there is some, and only against
          // a bare ratio where there is not — the literal was the complaint
          // (docs/tube/README.md#the-cut-list-and-the-bend-schedule).
          if (src.Tube()) {
            const double minR = tube::MinBendRadius(
                src.Tube()->material, row.outerDiameter, row.wallThickness);
            j["min_radius_mm"] = minR;
            j["tight"] = b.radius < minR;
          } else {
            j["tight"] = b.radius < 1.5 * row.outerDiameter;
            j["tight_rule"] =
                "no tube rules are set, so this is a bare R < 1.5 x D — "
                "set_tube_rules gives the limit the MATERIAL and the WALL "
                "actually impose, and check_tube is the full answer";
          }
        }
        bends.push_back(std::move(j));
      }
      json t = {
          {"curve", row.curveId},           {"name", row.name},
          {"quantity", row.quantity},       {"bodies", row.bodies},
          {"cut_length_mm", row.cutLength}, {"path_length_mm", row.pathLength},
          {"length_mm", row.cutLength}, // the name it shipped with
          {"bends", std::move(bends)}};
      if (row.pathLength - row.cutLength > 1e-3)
        t["coped_off_mm"] = row.pathLength - row.cutLength;
      if (row.outerDiameter > 1e-9) {
        t["outer_diameter_mm"] = row.outerDiameter;
        t["wall_mm"] = row.wallThickness;
      }
      tubes.push_back(std::move(t));
    }
    // The count BEFORE the move: `tubes` is moved into the object first, so
    // asking its size afterwards answered 0 with a tube in the array.
    const int count = static_cast<int>(tubes.size());
    return json{
        {"tubes", std::move(tubes)}, {"count", count}, {"members", members}};
  }

  if (m == "list_elements") {
    // Stable element names (topological naming) for each body's faces and
    // edges — what Fillet/Chamfer edge refs {feature_id, element} point at
    // (feature_id = the body's created_by). Each face carries its surface
    // kind + area, each edge its curve kind + length (+ radius when
    // circular), so agents can identify bore rims / seams without
    // measure-probing every name. Document-only: works headless.
    // params: {body?: index, part?: name|stable-id (cross-part, no tab
    // switch)}
    // On a JOB tab there is no active part document and yet there is plenty
    // to list: every part the job cuts. Each body carries the `job_part` it
    // came from, which is the other half of a machining target -- so an agent
    // can go straight from here to add_cam_op without knowing to switch tabs
    // and without a second call to work out which part it is looking at.
    Document* listed =
        p.contains("part")     ? ResolvePartParam(state, p.at("part"))
        : state.job != nullptr ? nullptr
                               : &RequirePartDoc(doc, "list_elements");
    // {configuration}: the part as it is in one of its configurations --
    // what a job part or a placement in it is made of, whose faces carry
    // that configuration's features (v135).
    const Document* shown = listed;
    if (p.contains("configuration")) {
      if (!p.contains("part"))
        throw std::runtime_error("configuration needs `part`: whose?");
      const json& c = p.at("configuration");
      uint64_t cfg = 0;
      std::string known;
      for (const Configuration& k : listed->GetConfigurations()) {
        known += (known.empty() ? "" : ", ") + k.name;
        if ((c.is_number() && k.id == c.get<uint64_t>()) ||
            (c.is_string() && k.name == c.get<std::string>()))
          cfg = k.id;
      }
      if (cfg == 0)
        throw std::runtime_error(
            "no configuration " + c.dump() +
            " in that part. Known: " + (known.empty() ? "(none)" : known));
      uint64_t slotId = 0; // the slot `listed` is
      for (const ui::PartSlot& slot : state.parts)
        if (slot.kind == ui::DocKind::Part &&
            ui::PartDocById(state, slot.id) == listed)
          slotId = slot.id;
      std::string why;
      shown = ui::ConfiguredPartDocument(state, slotId, cfg, &why);
      if (shown == nullptr)
        throw std::runtime_error(why);
    }
    const auto& bodies =
        shown != nullptr ? shown->GetBodies() : ui::JobBodies(state);
    const ElementFilter filter = ParseElementFilter(state, listed, p);
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error(IndexError("body", p.value("body", -1),
                                          bodies.size(),
                                          "get_state lists bodies"));
    // Describing a face is an OpenCASCADE surface analysis (~10 ms), so
    // this is bounded by how many it would describe, not by how many
    // bodies. A part holding a derived 197-body camera has 17,186 faces:
    // the call took 172 s, when `body` would have made it instant.
    const size_t maxFaces = p.value("max_faces", size_t{4000});
    size_t faceCount = 0, describedBodies = 0;
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && i != static_cast<size_t>(only))
        continue;
      for (const auto& [idx, name] : bodies[i].elementMap.names)
        faceCount += filter.Matches(topo::DecodeElementName(name)) ? 1 : 0;
      describedBodies++;
    }
    if (faceCount > maxFaces)
      throw std::runtime_error(
          "that is " + std::to_string(faceCount) + " faces across " +
          std::to_string(describedBodies) +
          (describedBodies == 1 ? " body" : " bodies") +
          ", and describing one is an OCC surface analysis" +
          (only >= 0 ? std::string(" — raise max_faces (now ") +
                           std::to_string(maxFaces) + ") to list it anyway"
                     : std::string(" — pass body: <index> to list ONE body "
                                   "(get_state lists them, in this order), "
                                   "or raise max_faces (now ") +
                           std::to_string(maxFaces) + ")"));
    json out = json::array();
    size_t matchedFaces = 0, matchedEdges = 0;
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && i != static_cast<size_t>(only))
        continue;
      json faces = json::array(), edges = json::array();
      for (const auto& [idx, name] : bodies[i].elementMap.names) {
        const topo::ElementOrigin origin = topo::DecodeElementName(name);
        if (!filter.Matches(origin))
          continue;
        matchedFaces++;
        json f = {{"index", idx}, {"name", name}};
        OriginJson(origin, f);
        if (const auto fi = Measure::DescribeFaceAt(bodies[i], idx)) {
          f["kind"] = fi->kind;
          f["area"] = fi->area;
          // Centroid: lets an importer match a specific face geometrically
          // (an importer's shell/draft face queries -> our stable face names).
          f["centroid"] = {fi->centroid.x, fi->centroid.y, fi->centroid.z};
          // Outward unit surface normal at the centroid: lets an agent tell a
          // +X wall from a -X wall without a separate select_face round-trip.
          f["normal"] = {fi->normal.x, fi->normal.y, fi->normal.z};
          // Radius/axis of a cylinder, sphere, cone or torus. Computed here
          // since C3 and emitted only by query_geometry until now, so the
          // obvious listing did not carry the obvious number.
          if (fi->hasRadius)
            f["radius"] = fi->radius;
          if (fi->hasAxis)
            f["axis"] = {fi->axis.x, fi->axis.y, fi->axis.z};
        }
        faces.push_back(f);
      }
      const std::vector<std::vector<std::string>> joins =
          bodies[i].elementMap.edgeNames.empty()
              ? std::vector<std::vector<std::string>>{}
              : EdgeFaceNames(bodies[i]);
      for (const auto& [idx, name] : bodies[i].elementMap.edgeNames) {
        const topo::ElementOrigin origin = topo::DecodeElementName(name);
        if (!filter.Matches(origin))
          continue;
        matchedEdges++;
        json e = {{"index", idx}, {"name", name}};
        OriginJson(origin, e);
        // The faces it joins, by name: "the edges where the top meets the
        // walls" is what a fillet or a chamfer is, and an edge's own name
        // comes from whichever face named it first, so it cannot say.
        if (idx >= 0 && static_cast<size_t>(idx) < joins.size())
          e["faces"] = joins[static_cast<size_t>(idx)];
        if (const auto ei = Measure::DescribeEdgeAt(bodies[i], idx)) {
          e["kind"] = ei->kind;
          e["length"] = ei->length;
          if (ei->hasRadius)
            e["radius"] = ei->radius;
          // Midpoint: lets an importer match a specific edge geometrically
          // (an importer's fillet/chamfer edge queries → our stable edge
          // names).
          e["midpoint"] = {ei->midpoint.x, ei->midpoint.y, ei->midpoint.z};
        }
        edges.push_back(e);
      }
      // What is NOT here. This listing walks the element map, so a FACE the
      // map never named is invisible: it cannot be threaded, picked,
      // filleted or fitted, and the listing looks complete. A bore drilled
      // in one end of a handle came back with no cylinder at all while the
      // hole was plainly in the solid, and the only clue anywhere was that a
      // screw in it read as interference
      // (docs/modeling/toponaming.md#how-each-operation-names-its-output).
      //
      // Faces only: an EDGE is named when something needs it, so a plain box
      // has six named faces and no named edges, and counting those would be
      // noise rather than a signal.
      int shapeFaces = 0;
      for (TopExp_Explorer ex(bodies[i].shape.GetShape(), TopAbs_FACE);
           ex.More(); ex.Next())
        ++shapeFaces;
      out.push_back(
          {{"body", i},
           {"name", bodies[i].name},
           {"created_by", bodies[i].createdBy},
           {"job_part", bodies[i].jobPartId},
           {"faces", faces},
           {"edges", edges},
           {"unnamed_faces",
            std::max(0, shapeFaces - static_cast<int>(
                                         bodies[i].elementMap.names.size()))}});
    }
    json result{{"bodies", out}};
    if (filter.Any()) {
      result["matched_faces"] = matchedFaces;
      result["matched_edges"] = matchedEdges;
      if (matchedFaces + matchedEdges == 0)
        result["why"] = filter.WhyNothing(bodies);
    }
    return result;
  }
  return std::nullopt;
}

} // namespace remote
