#include "remote/RemoteInspection.h"
#include "remote/InspectionContracts.h"

#include "cad/CADKernel.h"
#include "common/BuildInfo.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/FitStandards.h"
#include "core/FrameMath.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/MeshCollide.h"
#include "core/Occurrence.h"
#include "core/Parallel.h"
#include "core/features/FrameFeature.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "render/ClipPlanes.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/DerivedActions.h"
#include "ui/DocumentSlots.h"
#include "ui/JobActions.h"
#include "ui/RendererSync.h"
#include "ui/ViewActions.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
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

namespace {

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
template <typename C, typename R>
size_t RunSweep(SweepPlan& plan, double deadlineSeconds, C&& compute,
                R&& record) {
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
  const auto worker = [&] {
    for (;;) {
      const size_t k = next.fetch_add(1, std::memory_order_relaxed);
      if (k >= stopAt.load(std::memory_order_relaxed))
        return;
      // Every pair, not every eighth: one contact check between two imported
      // servos is seconds, so sampling the clock in batches overran a 20 s
      // deadline by 100 s. Reading a steady_clock is nanoseconds.
      if (timed && elapsedNow() > deadlineSeconds) {
        size_t seen = stopAt.load(std::memory_order_relaxed);
        while (k < seen && !stopAt.compare_exchange_weak(
                               seen, k, std::memory_order_relaxed))
          ;
        return;
      }
      results[k] = compute(plan.exact[k].first, plan.exact[k].second);
    }
  };

  const size_t threads = std::min<size_t>(n, parallel::Workers());
  if (threads <= 1) {
    worker();
  } else {
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (size_t t = 1; t < threads; ++t)
      pool.emplace_back(worker);
    worker(); // this thread pulls its share rather than idling
    for (std::thread& t : pool)
      t.join();
  }

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

SweepPlan PlanSweep(const std::vector<glm::vec3>& mins,
                    const std::vector<glm::vec3>& maxs, double gapTol,
                    size_t maxPairs) {
  struct Cand {
    double gap;
    size_t i, j;
  };
  std::vector<Cand> cands;
  SweepPlan plan;
  for (size_t i = 0; i < mins.size(); ++i)
    for (size_t j = i + 1; j < mins.size(); ++j) {
      const glm::vec3 d = glm::max(
          glm::max(mins[i] - maxs[j], mins[j] - maxs[i]), glm::vec3(0.0f));
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
void AttachSweepCoverage(nlohmann::json& out, const SweepPlan& plan,
                         size_t entities, size_t maxPairs,
                         double deadlineSeconds) {
  out["occurrences"] = entities;
  out["pairs_checked"] = plan.exact.size();
  out["pairs_box_cleared"] = plan.boxCleared;
  out["elapsed_s"] = plan.elapsed;
  std::string truncation;
  if (plan.overBudget != 0) {
    truncation =
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
  if (plan.overBudget != 0) {
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
void AddOverlapSite(json& j, const Measure::SetContact& c) {
  j["engaged_thread_pairs"] = c.engagedThreadPairs;
  j["thread_engagement_volume"] = c.threadEngagementVolume;
  if (!c.overlaps)
    return;
  j["centroid"] = {c.centroid.x, c.centroid.y, c.centroid.z};
  j["bbox_min"] = {c.overlapMin.x, c.overlapMin.y, c.overlapMin.z};
  j["bbox_max"] = {c.overlapMax.x, c.overlapMax.y, c.overlapMax.z};
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
    glm::vec3 min, max;
  };
  std::vector<Ent> ents;
  for (const Body& b : doc.GetBodies()) {
    Ent e{&b, {}, {}, {}};
    e.single.push_back(b);
    if (Measure::WorldBounds(e.single, glm::mat4(1.0f), e.min, e.max))
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
      const glm::vec3 gap = glm::max(
          glm::max(ents[i].min - ents[j].max, ents[j].min - ents[i].max),
          glm::vec3(0.0f));
      if (glm::length(gap) > window)
        continue;
      const auto c = Measure::ContactBetween(ents[i].single, glm::mat4(1.0f),
                                             ents[j].single, glm::mat4(1.0f));
      if (c && (c->volume > 1e-9 || c->distance <= touchTol))
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
     "feature_errors, bodies:[...B-Rep checks]}.",
     "body", remote::NeedsPart},
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
     "d e f g h js k m n p and their uppercase holes. Worst case over "
     "independent limits — no distribution, no form or position error. "
     "`part` (on a side, or shared) resolves faces against another part "
     "without switching tab, so a bore and its shaft can live in two.",
     "hole, intent, part, shaft, size"},
    {"set_tolerance",
     "Record what a face is allowed to vary by: {feature_id, element, "
     "class: \"H7\"} — or {lower, upper} in mm for limits no class "
     "expresses, never both. Design intent only: it moves no surface, so it "
     "rebuilds nothing. One per element; setting a second REPLACES the "
     "first. The class is validated against the face's measured size when "
     "you set it, not when something later looks. Acts on the ACTIVE part, "
     "like every other mutation; list_tolerances reads any part.",
     "class, comment, element, feature_id, lower, upper", remote::NeedsPart},
    {"list_tolerances",
     "Every tolerance on this part, with the limits its class implies at the "
     "face's measured size. A ref whose element no longer resolves is listed "
     "`resolved: false` rather than dropped — the intent outlives the "
     "topology that carried it.",
     "part", remote::NeedsPart},
    {"clear_tolerance", "Remove one tolerance by {id}.", "id",
     remote::NeedsPart},
    {"check_fits",
     "Every fit this assembly declares, checked. A Revolute or Cylindrical "
     "mate already names the two faces that mate, so nothing has to pair "
     "them up: where both faces carry a tolerance, the fit is computed and "
     "reported. The mate type is the intent — a joint that TURNS cannot have "
     "an interference fit, so that combination FAILS and a transition fit "
     "warns. Anything not fully answerable (an anchor that is a frame not a "
     "face, one side untoleranced, two faces of different basic size) is "
     "`unverified` with the reason, never a pass; the summary counts them.",
     ""},
    {"mass_properties",
     "Mass (`mass_g`)/volume/center_of_mass/bbox per body and total (density "
     "from "
     "materials). The bbox here is MEASURED off the surfaces; get_state's is "
     "a cheap conservative bound that can read ~60% over on a swept surface. "
     "`part` reads another part without switching to it; from an assembly tab "
     "with no `part`, aggregates every visible occurrence in world space.",
     "body, part"},
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
     "a, b, distance_kind, kind, mode, part"},
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
     "element, feature_id, frame, occurrence, part"},
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
     "body: <index> for one body of a many-bodied part.",
     "body, max_faces, part"},
    {"interference",
     "Pairwise check between bodies or occurrences: {clash, volume, distance, "
     "state: clash|touching|clear}. touch_tol (mm, default 0.001) splits "
     "touching from clear; the no-pair assembly sweep stays clash-only — use "
     "contact_report for the full picture. That sweep is BOUNDED: pairs whose "
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
     "anything that must be certain). ",
     "a, b, deadline_s, max_pairs, mode, touch_tol"},
    {"contact_report",
     "Contact graph: connected components (len 1 = one connected "
     "structure), per-pair state/distance, near_misses (gap <= near_tol, "
     "default 1.0 mm), clashes; bounded like the interference sweep "
     "(closest-first up to max_pairs 4000 / deadline_s 20 s, with the coverage "
     "in the "
     "response). Assembly tab: over occurrences, plus "
     "intended-vs-realized mate diffs when mates exist. Part tab: over the "
     "part's BODIES — catches a bracket floating inside one part, which "
     "the occurrence-level sweep cannot see. Each CLASH also carries "
     "`centroid` and `bbox_min`/`bbox_max`: WHERE the solid-in-solid is, "
     "so a pair of names and a volume is not the whole answer.",
     "deadline_s, max_pairs, near_tol, touch_tol"},
    {"query_geometry",
     "Declarative element query with a built-in assertion: filter faces/"
     "edges by geometry_type, normal/axis (+angle_tol_deg), radius "
     "(+radius_tol), area/length bounds, near_point (+max_distance); "
     "expected_count makes the result carry ok true/false. Covers UNNAMED "
     "elements too; matches carry stable names where they exist.",
     "angle_tol_deg, axis, body, element_type, expected_count, geometry_type, "
     "max_area, max_distance, max_length, max_results, min_area, min_length, "
     "near_point, normal, part, radius, radius_tol"},
    {"compare_geometry",
     "Boolean-diff similarity: {golden_path} (STEP/BREP reference) vs the "
     "active doc, or {a, b} bodies/occurrences. Returns iou/dice/precision/"
     "recall, volume_ratio, center_offset, bbox_iou, topology counts and "
     "plain-language verdicts; {missing_path}/{extra_path} export the diff "
     "solids as BREP. materialize:true DRAWS them on the part instead — "
     "missing red, extra green — so one screenshot shows WHERE it differs "
     "(part tab only; clear_geometry_delta turns it off).",
     "a, b, extra_path, golden_path, materialize, missing_path"},
    {"clear_geometry_delta",
     "Remove the compare_geometry materialize overlay, leaving the plain "
     "model. Harmless when there is none.",
     ""},
    {"section",
     "Cross-section area where a plane cuts the bodies; on an assembly tab, "
     "every visible occurrence in place. Omit `offset` to cut through the "
     "middle of what is shown (the chosen offset comes back). "
     "{regions: true} also returns each connected ISLAND the cut crosses "
     "({area, centroid}, biggest first) and `region_count`. The total cannot "
     "tell you whether the material is where you think it is: a barrel whose "
     "bolt bosses hang off its deck flange sections at 3293.8 where the sound "
     "one sections at 2586.9, and the difference is a boss standing as its "
     "own island. Islands are not a verdict — a fork has two tines — they are "
     "a question you can ask cheaply.",
     "base_plane, normal, offset, origin, part, regions"},
    {"set_section",
     "Enable/disable the live section clip {base_plane|origin+normal, "
     "offset}. Works on part AND assembly tabs, with filled cut caps on "
     "both; omit `offset` to cut through the middle of what is shown. "
     "Shorthand for one cut — add_section/update_section address several.",
     "add, base_plane, enabled, offset, reverse"},
    {"add_section",
     "Add a section cut and return its id {base_plane: xy|xz|yz, plane_id? "
     "(a datum plane), offset?, tilt_u?, tilt_v? (degrees), reverse?}. Up to "
     "four; they persist with the project.",
     "base_plane, id, offset, plane_id, reverse, tilt_u, tilt_v, visible"},
    {"list_sections",
     "Every section cut: id, source (base plane / datum / face, with "
     "resolved:false when a named datum/face is gone — such a cut stops "
     "cutting), offset, tilt, reverse, visible, and the cut plane "
     "{normal, d}.",
     ""},
    {"update_section",
     "Change one cut {id, base_plane?, plane_id?, offset?, tilt_u?, tilt_v?, "
     "reverse?, visible?}. Only the fields present are touched.",
     "base_plane, id, offset, plane_id, reverse, tilt_u, tilt_v, visible"},
    {"remove_section", "Remove one cut {id}, or all of them with {all: true}.",
     "all, id"},
};
std::span<const MethodDoc> InspectionMethodDocs() { return InspectionMethods; }

std::optional<json> HandleInspectionRpc(ui::AppState& state, Document* docIn,
                                        const std::string& m,
                                        const RpcParams& p) {
  Document* doc = docIn;
  if (m == "set_section") {
    // The viewport section cut. params: {enabled, base_plane: "xy"|"xz"|"yz",
    // offset?, reverse?} or {enabled, origin, normal}. Disabled clears every
    // section. This edits the SAME list the Section tool shows, so a script
    // and the tool window are never two different truths.
    if (!p.value("enabled", true)) {
      ui::ClearSections(state);
      return json::object();
    }
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      const int basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
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
      return json{
          {"id", id}, {"offset", sec->offset}, {"reverse", sec->reverse}};
    }
    throw std::runtime_error(
        "set_section wants base_plane (\"xy\"|\"xz\"|\"yz\") with an "
        "optional offset/reverse, or enabled=false to clear");
  }
  if (m == "add_section" || m == "update_section") {
    const bool adding = m == "add_section";
    ui::AppState::SectionView* sec = nullptr;
    if (adding) {
      const std::string bp = p.value("base_plane", std::string("xy"));
      const int basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
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
        const std::string bp = p.at("base_plane").get<std::string>();
        sec->basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
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
    glm::vec3 origin(0.0f), normal(0.0f, 0.0f, 1.0f);
    double usedOffset = 0.0;
    bool derived = false;
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      const int basePlane = bp == "xz" ? 1 : bp == "yz" ? 2 : 0;
      normal = basePlane == 1   ? glm::vec3(0, 1, 0)
               : basePlane == 2 ? glm::vec3(1, 0, 0)
                                : glm::vec3(0, 0, 1);
      // Omitting `offset` means the same here as in set_section: the middle
      // of what this tab shows. Two readings of one word would be worse than
      // either.
      derived = !p.contains("offset");
      usedOffset =
          derived
              ? static_cast<double>(ui::CenteredSectionOffset(state, basePlane))
              : p.at("offset").get<double>();
      origin = normal * static_cast<float>(usedOffset);
    } else {
      const auto& o = p.at("origin");
      const auto& nrm = p.at("normal");
      origin = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
      normal = {nrm[0].get<float>(), nrm[1].get<float>(), nrm[2].get<float>()};
    }
    // On an ASSEMBLY tab the same question is about the assembled thing, so
    // every visible occurrence's bodies are placed by its solved transform
    // and the area is the sum over all of them. Refusing here (which it used
    // to do) makes "how much material does this cut cross?" unanswerable for
    // exactly the models where it matters most.
    std::vector<Body> placed;
    const std::vector<Body>* bodies = nullptr;
    // A read, so it reaches another part by reference rather than by
    // switching to it (docs/agent-scale-followup-plan.md, W2).
    Document* sectionDoc = doc;
    if (p.contains("part"))
      sectionDoc = ResolvePartParam(state, p.at("part"));
    if (sectionDoc != nullptr) {
      bodies = &sectionDoc->GetBodies();
    } else if (state.assembly) {
      // A5: the leaf parts of the whole tree, placed.
      for (const assembly::FlatInstance& inst :
           ui::FlattenActiveAssembly(state)) {
        if (!inst.visible)
          continue;
        const glm::mat4 xf = inst.world;
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
    // The ISLANDS the cut crosses, biggest first. Off by default because the
    // total is what most callers want and this costs a second boolean.
    if (p.value("regions", false)) {
      const auto regions =
          Measure::CrossSectionRegions(*bodies, origin, normal);
      if (!regions)
        throw std::runtime_error("section regions failed — see the log");
      json arr = json::array();
      for (const Measure::SectionRegion& r : *regions)
        arr.push_back(
            {{"area", r.area},
             {"centroid", {r.centroid.x, r.centroid.y, r.centroid.z}}});
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
        std::vector<Body> out;
        for (const assembly::FlatInstance& inst :
             ui::FlattenActiveAssembly(state)) {
          if (!inst.visible || inst.rootOccurrenceId != rootId)
            continue;
          for (const Body& b : inst.part->GetBodies()) {
            if (b.reference)
              continue; // a derived ghost is not the occurrence's matter
            Body nb = b;
            nb.shape = b.shape.Transformed(inst.world);
            out.push_back(std::move(nb));
          }
        }
        return out;
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
      const auto pairVolume = [&](const Target& ta,
                                  const Target& tb) -> double {
        if (ta.bodies.empty() || tb.bodies.empty())
          throw std::runtime_error(
              "'" + (ta.bodies.empty() ? ta.label : tb.label) +
              "' resolves to no geometry (missing, hidden or empty)");
        const auto v = Measure::InterferenceVolume(ta.bodies, glm::mat4(1.0f),
                                                   tb.bodies, glm::mat4(1.0f));
        if (!v)
          throw std::runtime_error(
              "interference check failed — an OpenCASCADE boolean/extrema "
              "threw, usually on an invalid or empty body; run "
              "check_geometry on both sides");
        return *v;
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
        std::vector<glm::vec3> mins(occs.size()), maxs(occs.size());
        std::vector<size_t> keep;
        for (size_t i = 0; i < occs.size(); ++i)
          if (Measure::WorldBounds(occs[i].bodies, glm::mat4(1.0f), mins[i],
                                   maxs[i]))
            keep.push_back(i);
        std::vector<glm::vec3> km, kx;
        for (size_t i : keep) {
          km.push_back(mins[i]);
          kx.push_back(maxs[i]);
        }
        const size_t maxPairs = p.value("max_pairs", size_t{4000});
        const double deadline = p.value("deadline_s", 20.0);
        SweepPlan plan = PlanSweep(km, kx, 0.0, maxPairs);
        json pairs = json::array();
        bool any = false;
        // -1 = clear. In mesh mode a hit carries no volume: a volume
        // measured off triangles is the TESSELLATION's volume, and reporting
        // one would invite exactly the false precision this method is not
        // for — so mesh answers 0 and exact answers the volume it found.
        RunSweep(
            plan, deadline,
            [&](size_t ki, size_t kj) -> double {
              const Target& ta = occs[keep[ki]];
              const Target& tb = occs[keep[kj]];
              if (mode == "mesh")
                return pairMesh(ta, tb) ? 0.0 : -1.0;
              const double v = pairVolume(ta, tb);
              return v > 1e-9 ? v : -1.0;
            },
            [&](size_t ki, size_t kj, double v) {
              if (v < 0.0)
                return;
              any = true;
              json hit{{"a", occs[keep[ki]].label},
                       {"b", occs[keep[kj]].label},
                       {"clash", true}};
              if (mode != "mesh")
                hit["volume"] = v;
              pairs.push_back(std::move(hit));
            });
        json out{{"clash", any}, {"pairs", pairs}, {"mode", mode}};
        if (mode == "mesh")
          out["deflection_mm"] = meshDeflection.load();
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
        return json{{"clash", hit},  {"state", hit ? "clash" : "clear"},
                    {"mode", mode},  {"deflection_mm", meshDeflection.load()},
                    {"a", ta.label}, {"b", tb.label}};
      }
      const auto c = Measure::ContactBetween(ta.bodies, glm::mat4(1.0f),
                                             tb.bodies, glm::mat4(1.0f));
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
        Measure::ContactBetween(va, glm::mat4(1.0f), vb, glm::mat4(1.0f));
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
      return glm::vec3(a.at(0).get<float>(), a.at(1).get<float>(),
                       a.at(2).get<float>());
    };
    std::optional<glm::vec3> wantNormal, wantAxis, nearPoint;
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
        glm::vec3 point(0.0f), normal(0.0f), axis(0.0f);
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
        if (wantNormal && (glm::length(normal) < 0.5f ||
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
        Document* pd = ui::PartDocById(state, found->partId);
        if (pd == nullptr)
          throw std::runtime_error("occurrence part document is missing");
        std::vector<Body> placed;
        const glm::mat4 xf = ui::OccurrenceMatrix(*found);
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
    const auto c =
        Measure::CompareBodySets(refA, glm::mat4(1.0f), candB, glm::mat4(1.0f),
                                 wantMissing ? &missingShape : nullptr,
                                 wantExtra ? &extraShape : nullptr);
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
    const auto boxVol = [](const glm::vec3& mn, const glm::vec3& mx) {
      const glm::vec3 d = glm::max(mx - mn, glm::vec3(0.0f));
      return double(d.x) * d.y * d.z;
    };
    const glm::vec3 imn = glm::max(c->minA, c->minB);
    const glm::vec3 imx = glm::min(c->maxA, c->maxB);
    const double boxInter = boxVol(imn, imx);
    const double boxUnion =
        boxVol(c->minA, c->maxA) + boxVol(c->minB, c->maxB) - boxInter;
    const double bboxIou = boxUnion > 1e-9 ? boxInter / boxUnion : 0.0;

    std::vector<std::string> verdicts;
    char buf[160];
    if (iou > 0.99)
      verdicts.push_back("identical within tolerance");
    else if (iou > 0.95)
      verdicts.push_back("excellent match");
    else if (iou > 0.8)
      verdicts.push_back("good match");
    else if (iou > 0.5)
      verdicts.push_back("partial match");
    else
      verdicts.push_back("poor match");
    if (precision < recall - 0.05) {
      snprintf(buf, sizeof buf,
               "over-generating: %.1f%% of the candidate is extra geometry",
               (1.0 - precision) * 100.0);
      verdicts.push_back(buf);
    } else if (recall < precision - 0.05) {
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

    json out = {{"iou", iou},
                {"dice", dice},
                {"precision", precision},
                {"recall", recall},
                {"volume_ratio", volumeRatio},
                {"center_offset", centerOffset},
                {"bbox_iou", bboxIou},
                {"volume_reference", c->volumeA},
                {"volume_candidate", c->volumeB},
                {"volume_common", c->common},
                {"volume_missing", c->missing},
                {"volume_extra", c->extra},
                {"surface_reference", c->surfaceA},
                {"surface_candidate", c->surfaceB},
                {"faces", {{"reference", c->facesA}, {"candidate", c->facesB}}},
                {"edges", {{"reference", c->edgesA}, {"candidate", c->edgesB}}},
                {"verdicts", verdicts}};
    if (p.contains("missing_path")) {
      const std::string path = p.at("missing_path").get<std::string>();
      if (!CADKernel::Instance().SaveBREP(path, missingShape))
        throw std::runtime_error("cannot write " + path +
                                 " — does the parent directory exist?");
      out["missing_path"] = path;
    }
    if (p.contains("extra_path")) {
      const std::string path = p.at("extra_path").get<std::string>();
      if (!CADKernel::Instance().SaveBREP(path, extraShape))
        throw std::runtime_error("cannot write " + path +
                                 " — does the parent directory exist?");
      out["extra_path"] = path;
    }
    if (materialize) {
      // The overlay is drawn in the ACTIVE PART's coordinate space, so it
      // needs a part tab to be drawn on. Refusing beats an overlay floating
      // somewhere plausible-looking and wrong.
      if (doc == nullptr)
        throw std::runtime_error(
            "materialize needs a part tab — the delta is drawn in the active "
            "part's coordinate space");
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
    if (doc != nullptr) {
      const double touchTol = p.value("touch_tol", 1e-3);
      const double nearTol = p.value("near_tol", 1.0);
      struct BEnt {
        const Body* body;
        std::vector<Body> single; // ContactBetween takes body sets
        glm::vec3 min, max;
      };
      std::vector<BEnt> ents;
      json skipped = json::array();
      for (const Body& b : doc->GetBodies()) {
        if (b.reference)
          continue; // a derived ghost is not this part's matter
        BEnt e{&b, {}, {}, {}};
        e.single.push_back(b);
        if (!Measure::WorldBounds(e.single, glm::mat4(1.0f), e.min, e.max)) {
          skipped.push_back(b.name);
          continue;
        }
        ents.push_back(std::move(e));
      }
      const auto boxGap = [](const BEnt& a, const BEnt& b) {
        const glm::vec3 gap =
            glm::max(glm::max(a.min - b.max, b.min - a.max), glm::vec3(0.0f));
        return static_cast<double>(glm::length(gap));
      };
      std::vector<size_t> parent(ents.size());
      for (size_t i = 0; i < parent.size(); ++i)
        parent[i] = i;
      const std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
        return parent[i] == i ? i : parent[i] = find(parent[i]);
      };
      json pairs = json::array(), nearMisses = json::array(),
           clashes = json::array();
      for (size_t i = 0; i < ents.size(); ++i)
        for (size_t j = i + 1; j < ents.size(); ++j) {
          if (boxGap(ents[i], ents[j]) > nearTol)
            continue;
          const auto c = Measure::ContactBetween(
              ents[i].single, glm::mat4(1.0f), ents[j].single, glm::mat4(1.0f));
          if (!c)
            continue;
          const char* st = ContactState(*c, touchTol);
          pairs.push_back({{"a", ents[i].body->name},
                           {"b", ents[j].body->name},
                           {"state", st},
                           {"distance", c->distance},
                           {"volume", c->volume}});
          if (std::string_view(st) != "clear")
            parent[find(i)] = find(j);
          if (std::string_view(st) == "clash") {
            json clash{{"a", ents[i].body->name},
                       {"b", ents[j].body->name},
                       {"volume", c->volume}};
            AddOverlapSite(clash, *c);
            clashes.push_back(std::move(clash));
          } else if (c->distance > touchTol && c->distance <= nearTol)
            nearMisses.push_back({{"a", ents[i].body->name},
                                  {"b", ents[j].body->name},
                                  {"distance", c->distance}});
        }
      std::map<size_t, std::vector<std::string>> comps;
      for (size_t i = 0; i < ents.size(); ++i)
        comps[find(i)].push_back(ents[i].body->name);
      std::vector<std::vector<std::string>> componentList;
      for (auto& [root, names] : comps)
        componentList.push_back(std::move(names));
      std::sort(
          componentList.begin(), componentList.end(),
          [](const auto& a, const auto& b) { return a.size() > b.size(); });
      json out = {{"scope", "bodies"},
                  {"bodies", ents.size()},
                  {"connected", componentList.size() <= 1},
                  {"components", componentList},
                  {"pairs", pairs},
                  {"near_misses", nearMisses},
                  {"clashes", clashes},
                  {"touch_tol", touchTol},
                  {"near_tol", nearTol}};
      if (!skipped.empty())
        out["skipped"] = skipped;
      return out;
    }
    if (state.assembly == nullptr)
      throw std::runtime_error(
          "contact_report needs a part or assembly tab — switch tabs with "
          "set_active_part (see list_parts)");
    const double touchTol = p.value("touch_tol", 1e-3);
    const double nearTol = p.value("near_tol", 1.0);
    // A5: one node per TOP-LEVEL occurrence, whose geometry is its whole
    // flattened subtree in world space. That is what makes "is the gangway
    // attached to the dock" a question about two nodes and the hinges
    // between them, instead of thirty parts — and the reason a clash BETWEEN
    // two parts inside one sub-assembly does not appear here: it belongs to
    // that assembly's own report, on its own tab.
    struct Ent {
      const Occurrence* occ;
      std::vector<Body> bodies; // already placed in world space
      glm::mat4 xf{1.0f};       // identity: `bodies` carry the placement
      glm::vec3 min, max;
    };
    std::vector<Ent> ents;
    json skipped = json::array();
    std::map<uint64_t, std::vector<Body>> byRoot;
    for (const assembly::FlatInstance& inst :
         ui::FlattenActiveAssembly(state)) {
      if (!inst.visible)
        continue;
      for (const Body& b : inst.part->GetBodies()) {
        // A derived ghost is not the occurrence's matter: the gripper body
        // carrying a copy of the servo it was drawn on reported a 15 cm3
        // clash with that servo — its own ghost.
        if (b.reference)
          continue;
        Body nb = b;
        nb.shape = b.shape.Transformed(inst.world);
        byRoot[inst.rootOccurrenceId].push_back(std::move(nb));
      }
    }
    for (const Occurrence& o : state.assembly->GetOccurrences()) {
      if (!o.visible || state.assembly->OccurrenceSuppressed(o.id))
        continue;
      const auto it = byRoot.find(o.id);
      Ent e;
      e.occ = &o;
      if (it != byRoot.end())
        e.bodies = it->second;
      if (e.bodies.empty() ||
          !Measure::WorldBounds(e.bodies, e.xf, e.min, e.max)) {
        skipped.push_back(o.name); // nothing resolvable under this occurrence
        continue;
      }
      ents.push_back(std::move(e));
    }
    std::vector<size_t> parent(ents.size());
    for (size_t i = 0; i < parent.size(); ++i)
      parent[i] = i;
    const std::function<size_t(size_t)> find = [&](size_t i) -> size_t {
      return parent[i] == i ? i : parent[i] = find(parent[i]);
    };
    json pairs = json::array(), nearMisses = json::array(),
         clashes = json::array();
    std::set<std::pair<uint64_t, uint64_t>> touchingIds;
    // Closest first, and bounded: on a 253-occurrence arm this loop is
    // 31,878 pairs, and the ones sharing a PCB all pass the box test.
    std::vector<glm::vec3> mins, maxs;
    for (const Ent& e : ents) {
      mins.push_back(e.min);
      maxs.push_back(e.max);
    }
    const size_t maxPairs = p.value("max_pairs", size_t{4000});
    const double deadline = p.value("deadline_s", 20.0);
    SweepPlan plan = PlanSweep(mins, maxs, nearTol, maxPairs);
    RunSweep(
        plan, deadline,
        [&](size_t i, size_t j) {
          return Measure::ContactBetween(ents[i].bodies, ents[i].xf,
                                         ents[j].bodies, ents[j].xf);
        },
        [&](size_t i, size_t j, const std::optional<Measure::SetContact>& c) {
          if (!c)
            return;
          const char* st = ContactState(*c, touchTol);
          // The ids as well as the names: sixteen occurrences answer to
          // "ISO 4762 M3x10", and a reported pair that cannot say WHICH
          // one sends a caller hunting through list_occurrences.
          pairs.push_back({{"a", ents[i].occ->name},
                           {"b", ents[j].occ->name},
                           {"a_id", ents[i].occ->id},
                           {"b_id", ents[j].occ->id},
                           {"state", st},
                           {"distance", c->distance},
                           {"volume", c->volume}});
          if (std::string_view(st) != "clear") {
            parent[find(i)] = find(j);
            touchingIds.insert({std::min(ents[i].occ->id, ents[j].occ->id),
                                std::max(ents[i].occ->id, ents[j].occ->id)});
          }
          if (std::string_view(st) == "clash") {
            json clash{{"a", ents[i].occ->name},
                       {"b", ents[j].occ->name},
                       {"a_id", ents[i].occ->id},
                       {"b_id", ents[j].occ->id},
                       {"volume", c->volume}};
            AddOverlapSite(clash, *c);
            clashes.push_back(std::move(clash));
          } else if (c->distance > touchTol && c->distance <= nearTol)
            nearMisses.push_back({{"a", ents[i].occ->name},
                                  {"b", ents[j].occ->name},
                                  {"a_id", ents[i].occ->id},
                                  {"b_id", ents[j].occ->id},
                                  {"distance", c->distance}});
        });
    std::map<size_t, std::vector<std::string>> comps;
    for (size_t i = 0; i < ents.size(); ++i)
      comps[find(i)].push_back(ents[i].occ->name);
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
                {"touch_tol", touchTol},
                {"near_tol", nearTol}};
    AttachSweepCoverage(out, plan, ents.size(), maxPairs, deadline);
    if (!skipped.empty())
      out["skipped"] = skipped;
    // Intended vs realized: only meaningful when the assembly HAS mates —
    // in a fixed-occurrence assembly every touching pair is "unmated".
    if (!state.assembly->GetMates().empty()) {
      json unrealized = json::array(), unmated = json::array();
      std::set<std::pair<uint64_t, uint64_t>> mated;
      for (const Mate& mt : state.assembly->GetMates()) {
        const auto key =
            std::make_pair(std::min(mt.a.occurrenceId, mt.b.occurrenceId),
                           std::max(mt.a.occurrenceId, mt.b.occurrenceId));
        mated.insert(key);
        if (touchingIds.find(key) == touchingIds.end())
          unrealized.push_back({{"mate", mt.name}, {"id", mt.id}});
      }
      for (const auto& [ia, ib] : touchingIds)
        if (mated.find({ia, ib}) == mated.end()) {
          const Occurrence* a = state.assembly->FindById(ia);
          const Occurrence* b = state.assembly->FindById(ib);
          if (a != nullptr && b != nullptr)
            unmated.push_back({{"a", a->name}, {"b", b->name}});
        }
      out["unrealized_mates"] = unrealized;
      out["unmated_contacts"] = unmated;
    }
    return out;
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
      const assembly::PartResolver resolve = [&](uint64_t id) {
        return ui::PartDocById(state, id);
      };
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
          glm::vec3 bmin, bmax;
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
  if (m == "check_fit") {
    // ISO 286 limits and fits (docs/tolerances/plan.md, T1). Two jobs in one
    // method because they are one question asked at two depths: `hole` alone
    // is "what may this bore be", `hole` + `shaft` is "will they go
    // together".
    //
    // Nothing here touches geometry. A size tolerance leaves nominal alone —
    // that is what separates it from a clearance-hole fit, which picks a
    // different diameter outright (parts::HoleDiameter).
    //
    // params: {hole: {class, feature_id?, element?, part?}, shaft?: {...},
    //          size?, intent?}
    const auto measureSide = [&](const json& s,
                                 const char* which) -> std::optional<double> {
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
      return el->radius * 2.0;
    };

    if (!p.contains("hole"))
      throw std::runtime_error(
          "check_fit: `hole` is required, as {class: \"H7\"} at least");
    const json& holeJson = p.at("hole");
    if (!holeJson.contains("class"))
      throw std::runtime_error(
          "check_fit: hole.class is required, e.g. \"H7\"");
    const std::string holeCls = holeJson.at("class").get<std::string>();
    const std::optional<double> holeMeasured = measureSide(holeJson, "hole");

    const bool hasShaft = p.contains("shaft");
    std::string shaftCls;
    std::optional<double> shaftMeasured;
    if (hasShaft) {
      const json& shaftJson = p.at("shaft");
      if (!shaftJson.contains("class"))
        throw std::runtime_error(
            "check_fit: shaft.class is required, e.g. \"g6\"");
      shaftCls = shaftJson.at("class").get<std::string>();
      shaftMeasured = measureSide(shaftJson, "shaft");
    }

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
    } else if (holeMeasured && shaftMeasured) {
      constexpr double kAgree = 1e-5; // mm, far under the finest grade (3 um)
      if (std::abs(*holeMeasured - *shaftMeasured) > kAgree)
        throw std::runtime_error(
            "check_fit: the two faces are not the same basic size — the hole "
            "measures " +
            mm(*holeMeasured) + " and the shaft " + mm(*shaftMeasured) +
            ". An ISO fit is a band either side of ONE nominal, so a "
            "clearance already modelled into the geometry would be counted "
            "twice. Model both at the same size, or pass `size` to say which "
            "nominal the classes are against.");
      size = *holeMeasured;
      sizeFrom = "measured";
    } else if (holeMeasured || shaftMeasured) {
      size = holeMeasured ? *holeMeasured : *shaftMeasured;
      sizeFrom = "measured";
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
    // What a face is ALLOWED to vary by (docs/tolerances/plan.md, T2).
    // params: {feature_id, element, class? | lower?+upper?, comment?, part?}
    Document& part = RequirePartDoc(doc, "set_tolerance");
    const ElementRef ref{p.at("feature_id").get<uint64_t>(),
                         p.at("element").get<std::string>()};
    const auto el = Measure::Describe(part.GetBodies(), ref);
    if (!el)
      throw std::runtime_error("set_tolerance: element '" + ref.name +
                               "' does not resolve (list_elements)");
    // Only round features are carried yet, and a tolerance nothing can check
    // is a silent no-op — so it is refused rather than stored.
    if (!el->hasRadius)
      throw std::runtime_error(
          "set_tolerance: " + ref.name + " is a " + el->shape +
          ", and only round features carry a size tolerance so far — a "
          "cylindrical face or a circular edge");
    const double nominal = el->radius * 2.0;

    const bool hasClass = p.contains("class");
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
    state.commands->Execute(std::move(cmd), part);
    const Tolerance* stored = part.FindTolerance(raw->ToleranceId());
    json out{{"id", stored->id},
             {"element", stored->ref.name},
             {"feature_id", stored->ref.featureId},
             {"nominal", nominal}};
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
             {"element", t.ref.name}};
      if (!t.comment.empty())
        e["comment"] = t.comment;
      const auto el = Measure::Describe(part.GetBodies(), t.ref);
      // Kept, not dropped: a tolerance whose face an upstream edit renamed is
      // still what somebody decided about this part, and saying so is more
      // use than quietly having fewer tolerances than yesterday.
      e["resolved"] = el && el->hasRadius;
      if (t.isoClass.empty()) {
        e["lower"] = t.lower;
        e["upper"] = t.upper;
      } else {
        e["class"] = t.isoClass;
      }
      if (el && el->hasRadius) {
        const double nominal = el->radius * 2.0;
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
    Document& part = RequirePartDoc(doc, "clear_tolerance");
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!part.FindTolerance(id))
      throw std::runtime_error("clear_tolerance: no tolerance with id " +
                               std::to_string(id) +
                               " (list_tolerances shows them)");
    state.commands->Execute(std::make_unique<ClearToleranceCommand>(id), part);
    return json{{"ok", true}, {"id", id}};
  }
  if (m == "check_fits") {
    // Every fit the assembly ALREADY declares. A Revolute or Cylindrical mate
    // names the two faces that mate, so no pairing entity has to exist — and
    // the mate type doubles as the intent, because a joint that turns cannot
    // be an interference fit.
    if (!state.assembly)
      throw std::runtime_error(
          "check_fits needs an assembly tab — a fit is between two placed "
          "parts, and only an assembly says which faces meet");

    // One side of one mate, resolved as far as it goes.
    struct Side {
      const Document* part = nullptr;
      const Tolerance* tol = nullptr;
      std::optional<double> diameter;
      std::string occurrence;
      std::string reason; // non-empty = this side stopped here
    };
    const auto resolveSide = [&](const Connector& c, const char* which) {
      Side side;
      const Occurrence* occ = state.assembly->FindById(c.occurrenceId);
      if (!occ) {
        side.reason = std::string(which) + " occurrence is missing";
        return side;
      }
      side.occurrence = occ->name;
      if (!c.innerPath.empty()) {
        side.reason = std::string(which) + " (" + occ->name +
                      ") is inside a sub-assembly, which check_fits does not "
                      "reach into yet";
        return side;
      }
      if (c.anchor.frameId != 0 || c.anchor.origin || !c.anchor.face.IsSet()) {
        side.reason = std::string(which) + " (" + occ->name +
                      ") is mated on a " +
                      (c.anchor.frameId != 0 ? "datum frame" : "part origin") +
                      ", not a face — there is no surface to give a size";
        return side;
      }
      Document* pd = ui::PartDocById(state, occ->partId);
      if (!pd) {
        side.reason = std::string(which) + " (" + occ->name +
                      ") has no part document loaded";
        return side;
      }
      side.part = pd;
      side.tol = pd->ToleranceFor(c.anchor.face);
      if (!side.tol) {
        side.reason = std::string(which) + " (" + occ->name + ") face " +
                      c.anchor.face.name +
                      " carries no tolerance (set_tolerance on that part)";
        return side;
      }
      const auto el = Measure::Describe(pd->GetBodies(), c.anchor.face);
      if (!el || !el->hasRadius) {
        side.reason = std::string(which) + " (" + occ->name + ") face " +
                      c.anchor.face.name +
                      " no longer resolves to a round face";
        return side;
      }
      side.diameter = el->radius * 2.0;
      return side;
    };

    json entries = json::array();
    int verified = 0, unverified = 0, failed = 0, warned = 0;
    for (const Mate& mate : state.assembly->GetMates()) {
      if (mate.type != MateType::Revolute && mate.type != MateType::Cylindrical)
        continue; // only a round joint is a fit
      json e{{"mate", mate.id},
             {"mate_name", mate.name},
             {"mate_type",
              mate.type == MateType::Revolute ? "revolute" : "cylindrical"}};
      const Side a = resolveSide(mate.a, "a");
      const Side b = resolveSide(mate.b, "b");
      const auto stop = [&](const std::string& reason) {
        e["status"] = "unverified";
        e["reason"] = reason;
        unverified++;
        entries.push_back(std::move(e));
      };
      if (!a.reason.empty() || !b.reason.empty()) {
        stop(!a.reason.empty() ? a.reason : b.reason);
        continue;
      }
      // Which side is the bore is said by the CLASS, not guessed from the
      // geometry: an uppercase class is a hole, lowercase a shaft. Two of the
      // same is not a fit anyone can compute, and picking one to flip would
      // be inventing the answer.
      const bool aHole =
          !a.tol->isoClass.empty() &&
          std::isupper(static_cast<unsigned char>(a.tol->isoClass[0])) != 0;
      const bool bHole =
          !b.tol->isoClass.empty() &&
          std::isupper(static_cast<unsigned char>(b.tol->isoClass[0])) != 0;
      if (a.tol->isoClass.empty() || b.tol->isoClass.empty()) {
        stop(
            "a fit needs an ISO class on both sides; " +
            std::string(a.tol->isoClass.empty() ? a.occurrence : b.occurrence) +
            " states explicit deviations, which say nothing about which side "
            "is the bore");
        continue;
      }
      if (aHole == bHole) {
        stop("both sides are " + std::string(aHole ? "hole" : "shaft") +
             " classes (" + a.tol->isoClass + " and " + b.tol->isoClass +
             ") — one of them has to be the bore");
        continue;
      }
      const Side& hole = aHole ? a : b;
      const Side& shaft = aHole ? b : a;
      constexpr double kAgree = 1e-5;
      if (std::abs(*hole.diameter - *shaft.diameter) > kAgree) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "the two faces are not the same basic size — the bore "
                      "measures %.4f mm and the shaft %.4f mm",
                      *hole.diameter, *shaft.diameter);
        stop(buf);
        continue;
      }
      std::string why;
      const auto fit = fits::Fit(*hole.diameter, hole.tol->isoClass,
                                 shaft.tol->isoClass, &why);
      if (!fit) {
        stop(why);
        continue;
      }
      e["size"] = *hole.diameter;
      e["hole"] = {{"occurrence", hole.occurrence},
                   {"class", hole.tol->isoClass},
                   {"min", fit->hole.Min(*hole.diameter)},
                   {"max", fit->hole.Max(*hole.diameter)}};
      e["shaft"] = {{"occurrence", shaft.occurrence},
                    {"class", shaft.tol->isoClass},
                    {"min", fit->shaft.Min(*hole.diameter)},
                    {"max", fit->shaft.Max(*hole.diameter)}};
      e["kind"] = fits::FitKindName(fit->kind);
      e["min_clearance"] = fit->minClearance;
      e["max_clearance"] = fit->maxClearance;
      // The mate type IS the intent, and this is the whole reason pairing off
      // mates is worth more than a pairing table: a Revolute joint says these
      // two turn against each other, and an interference fit says they do not.
      if (fit->kind == fits::FitKind::Interference) {
        e["status"] = "fail";
        e["reason"] = "this joint turns, but the fit is interference — the "
                      "shaft is always larger than the bore, so it is pressed "
                      "in, not journalled";
        failed++;
      } else if (fit->kind == fits::FitKind::Transition) {
        e["status"] = "warn";
        e["reason"] = "a transition fit may come out tight or loose on any "
                      "given pair, so this joint is not guaranteed to turn";
        warned++;
      } else {
        e["status"] = "pass";
        verified++;
      }
      entries.push_back(std::move(e));
    }
    // Never a green badge over unknowns: unverified outranks pass.
    const std::string status = failed            ? "fail"
                               : unverified      ? "unverified"
                               : warned          ? "warn"
                               : entries.empty() ? "none"
                                                 : "pass";
    return json{{"fits", std::move(entries)}, {"status", status},
                {"passed", verified},         {"warned", warned},
                {"failed", failed},           {"unverified", unverified}};
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
    // docs/inspect/measure-v2.md rather than a refactor smuggled into this
    // change, because interference's version is load-bearing for the
    // contact sweep and shares a prefetched body map with it.
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
        glm::mat4 world(1.0f);
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
        } else if (const Document* pd = ui::PartDocById(state, occ->partId)) {
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
    const Measure::Target ta{sideA->bodies, glm::mat4(1.0f), sideA->ref};
    if (p.contains("b")) {
      const auto sideB = resolveSide(p.at("b"), "b");
      const Measure::Target tb{sideB->bodies, glm::mat4(1.0f), sideB->ref};
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
        const glm::vec3 delta = d->pointB - d->pointA;
        out["delta"] = {delta.x, delta.y, delta.z};
        // Two WHOLE shapes that interpenetrate are 0 apart, which is true
        // and the least useful available fact. Say whether they clash.
        if (const auto ov = Measure::OverlapBetween(ta, tb)) {
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
    const auto v3 = [](const glm::vec3& v) { return json{v.x, v.y, v.z}; };
    const auto frameJson = [&](const glm::mat4& mtx, const char* space,
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
                                std::string& source) -> glm::mat4 {
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
        glm::vec3 o, z, x;
        bool circular = false;
        std::string why;
        if (!frame::ElementFrame(part.GetBodies(), ref, o, z, x, circular, why))
          throw std::runtime_error(why);
        source = std::string(circular ? "axis of " : "plane of ") + ref.name;
        return frame::Build(o, x, z);
      }
      source = "origin";
      return glm::mat4(1.0f);
    };
    if (p.contains("occurrence")) {
      if (!state.assembly)
        throw std::runtime_error(
            "frame: `occurrence` needs an assembly tab (switch with "
            "set_active_part, see list_parts); on a part tab pass `frame` or "
            "feature_id + element");
      const json& v = p.at("occurrence");
      const Occurrence* occ = nullptr;
      glm::mat4 world(1.0f);
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
      glm::mat4 local(1.0f);
      if (p.contains("frame") || p.contains("feature_id") ||
          p.contains("element")) {
        if (occ->refKind == OccurrenceRef::Assembly)
          throw std::runtime_error(
              "'" + label +
              "' is a sub-assembly, which has no frames or faces of its own: "
              "address the part inside it by path (\"Unit/Part\")");
        const Document* part = ui::PartDocById(state, occ->partId);
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
    const glm::mat4 local = localFrame(part, source);
    return frameJson(local, "part", source);
  }
  const auto snapshotRevision = [](const Document& part, uint64_t partId) {
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
  };
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
    const auto& bodies =
        p.contains("part") ? ResolvePartParam(state, p.at("part"))->GetBodies()
        : state.job != nullptr
            ? ui::JobBodies(state)
            : RequirePartDoc(doc, "list_elements").GetBodies();
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
      faceCount += bodies[i].elementMap.names.size();
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
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && i != static_cast<size_t>(only))
        continue;
      json faces = json::array(), edges = json::array();
      for (const auto& [idx, name] : bodies[i].elementMap.names) {
        json f = {{"index", idx}, {"name", name}};
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
      for (const auto& [idx, name] : bodies[i].elementMap.edgeNames) {
        json e = {{"index", idx}, {"name", name}};
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
      out.push_back({{"body", i},
                     {"name", bodies[i].name},
                     {"created_by", bodies[i].createdBy},
                     {"job_part", bodies[i].jobPartId},
                     {"faces", faces},
                     {"edges", edges}});
    }
    return json{{"bodies", out}};
  }
  return std::nullopt;
}

} // namespace remote
