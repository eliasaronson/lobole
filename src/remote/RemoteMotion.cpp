#include "remote/RemoteMotion.h"

#include "core/Angles.h"
#include "remote/RemoteHelpers.h"

#include "core/AssemblySolve.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/MotionActions.h"

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <set>
#include <span>

using json = nlohmann::json;

namespace remote {
namespace {

// A motion study's from/to speak the JOINT's unit: DEGREES on the wire for a
// rotational dof, millimetres for a sliding one. The wire says degrees so the
// caller never has to know which kind the joint is; storage is radians either
// way, and MotionSample::value is documented "radians or mm" for that reason.
double RotAwareAngle(double wire, bool rot) {
  return rot ? angles::ToRadians(wire) : wire;
}

// describe: one row per `m ==` branch below — extend together.
constexpr MethodDoc MotionMethods[] = {
    {"add_motion_study",
     "Define a motion study on the active assembly: ONE mate's degree of "
     "freedom driven over a range, the assembly solved at every sample, and "
     "every occurrence pair checked for a clash. {driver: mate id, dof?: "
     "rotation|offset (inferred from a revolute or slider; a cylindrical "
     "mate must say which), from, to — DEGREES for a rotation, mm for an "
     "offset — samples? (default 48), check_clash? (default true), "
     "clash_mode?: mesh (default, ~10x faster) | exact (the boolean), "
     "name?}. Returns the study and, if it cannot run, `why_not`. Nothing "
     "runs until run_motion_study. ONE driver moves the whole mechanism "
     "when the rest follows through mates and RELATIONS (add_relation): a "
     "geared pair turns because the relation propagates, not because two "
     "DOFs were driven. Pick the driver with care where a mechanism has a "
     "SINGULAR configuration — a symmetric linkage that can also move "
     "antisymmetrically passes through one — because the solve continues "
     "from the previous sample and can come out of it on the other branch. "
     "The driver to choose is the DOF that cannot change on the branch you "
     "do not want; the clash check is what makes the jump visible.",
     "check_clash, clash_mode, dof, driver, from, name, samples, to"},
    {"set_motion_study",
     "Edit a study {study, name?, from?, to?, samples?, check_clash?, "
     "clash_mode?}; a stored result is dropped, since it described the old "
     "definition.",
     "check_clash, clash_mode, from, name, samples, study, to"},
    {"remove_motion_study", "Remove a study by id.", "study"},
    {"list_motion_studies",
     "Every study, with whether it has a result and whether one is running.",
     "", remote::NoFlags,
     "studies[]{type,id,name,driver_mate,driver_dof,from,to,samples,check_"
     "clash,clash_mode,unit,has_result,running,why_not?,stale?,clean?,"
     "collision_checked?}"},
    {"run_motion_study",
     "Run it: {study, async?, include_poses?}. Synchronous by default and "
     "returns the COLLISION TABLE — per sample: the driver value, whether "
     "it solved, the loop residual, and the clashing pairs (each with its "
     "`volume` and `centroid` under an EXACT check, which is how a pair of "
     "names becomes something to act on; a mesh check knows only THAT, and "
     "says nothing rather than a zero that reads like a measurement) — plus "
     "`clean`, `seated_contacts` and `appeared` (what TURNING changed: a "
     "mesh check calls touching a clash, and at rest a propeller touches "
     "its motor and its screws, so the pairs already in contact at the "
     "first sample are counted and the rest are listed), `worst_sample`, "
     "`table` (the same as text, for a report), "
     "`deflection_mm` for a mesh check, and `pairs_checked`. "
     "include_poses adds every occurrence's placement per sample. "
     "{async: true} runs it as a background job ('motion' in "
     "background_jobs) and motion_status reads the result when it lands. A "
     "sample that does not solve is reported unsolved with its residual — "
     "never drawn in the last good pose's place. The run happens on a COPY, "
     "so the assembly is left where it was; `leave_at` (\"last\", "
     "\"start\", or a sample index) scrubs it to one of the samples "
     "afterwards, which is how anything about the far end of the travel — a "
     "section, a mass property, a measurement — can be asked at all.",
     "async, include_poses, leave_at, study"},
    {"motion_status",
     "{study}: whether a run is in progress, and the stored result if one "
     "has landed (the same shape run_motion_study returns).",
     "include_poses, study"},
    {"set_motion_pose",
     "Scrub: drive the study's mate to {t} (0..1 along the range) and "
     "re-solve, so the viewport and every query show that pose. No undo "
     "step, like the explode slider — a scrub is a look, not an edit.",
     "study, t"},
    {"export_motion",
     "Write the run as a GLB with a glTF animation {study, path}: one node "
     "per occurrence, translation + rotation keyframes at every sample, "
     "24 frames per second, in METRES and Y-up as glTF defines (one root "
     "node carries both). Refuses without a run, and refuses when any "
     "sample is unsolved.",
     "path, study"},
};

json StudyJson(const AssemblyDocument& asmDoc, const MotionStudy& s) {
  json j = DocumentIO::MotionStudyToJson(s);
  const bool rot = s.driverDof == MateDof::Rotation;
  // The document stores the DOF's own unit; the surface speaks degrees for
  // a rotation. Both are shown so nobody has to remember which this is.
  j["unit"] = rot ? "deg" : "mm";
  j["from"] = rot ? angles::ToDegrees(s.from) : s.from;
  j["to"] = rot ? angles::ToDegrees(s.to) : s.to;
  const std::string why = motion::WhyNot(asmDoc, s);
  if (!why.empty())
    j["why_not"] = why;
  return j;
}

json ResultJson(const motion::Result& r, const MotionStudy& s,
                bool includePoses) {
  json samples = json::array();
  const bool rot = s.driverDof == MateDof::Rotation;
  for (size_t i = 0; i < r.samples.size(); i++) {
    const motion::Sample& sm = r.samples[i];
    json clashes = json::array();
    for (const motion::Clash& c : sm.clashes) {
      json j{{"a", c.a}, {"b", c.b}, {"a_name", c.aName}, {"b_name", c.bName}};
      // An exact check knows how much and where; a mesh one only knows THAT,
      // and says nothing rather than a zero that reads like a measurement.
      if (c.known) {
        j["volume"] = c.volume;
        j["centroid"] = {c.centroid.x, c.centroid.y, c.centroid.z};
      }
      clashes.push_back(std::move(j));
    }
    json sj{{"index", i},
            {"t", sm.t},
            {"value", rot ? angles::ToDegrees(sm.value) : sm.value},
            {"solved", sm.solved},
            {"errored_mates", sm.erroredMates},
            {"residual_mm", sm.residualMm},
            {"clashes", clashes}};
    sj["unknown_contacts"] = json::array();
    for (const auto& c : sm.unknownContacts)
      sj["unknown_contacts"].push_back({{"a", c.a},
                                        {"b", c.b},
                                        {"a_name", c.aName},
                                        {"b_name", c.bName},
                                        {"reason", c.reason}});
    if (includePoses) {
      json poses = json::array();
      for (const auto& [occId, m] : sm.worlds) {
        const glm::quat q = glm::normalize(glm::quat_cast(glm::mat3(m)));
        poses.push_back({{"occurrence", occId},
                         {"position", {m[3].x, m[3].y, m[3].z}},
                         {"orientation", {q.x, q.y, q.z, q.w}}});
      }
      sj["poses"] = poses;
    }
    samples.push_back(sj);
  }
  // What TURNING changed. A mesh sweep calls touching a clash, and at rest a
  // propeller touches its motor, its nut and four screws — so a study of a
  // working machine is never "clean" and the caller has to work out which
  // contacts are news. That is this: the pairs in contact at the FIRST
  // sample are seated, and anything else that shows up appeared.
  std::set<std::pair<uint64_t, uint64_t>> seated;
  if (!r.samples.empty())
    for (const motion::Clash& c : r.samples.front().clashes)
      seated.insert({std::min(c.a, c.b), std::max(c.a, c.b)});
  std::set<std::pair<uint64_t, uint64_t>> appeared;
  json appearedJson = json::array();
  for (const motion::Sample& sm : r.samples)
    for (const motion::Clash& c : sm.clashes) {
      const std::pair<uint64_t, uint64_t> key{std::min(c.a, c.b),
                                              std::max(c.a, c.b)};
      if (seated.count(key) || !appeared.insert(key).second)
        continue;
      appearedJson.push_back(
          {{"a", c.a}, {"b", c.b}, {"a_name", c.aName}, {"b_name", c.bName}});
    }
  json out{
      {"clean", r.clean && r.checkedClashes && !r.cancelled && r.error.empty()},
      {"seated_contacts", seated.size()},
      {"appeared", appearedJson},
      {"samples", samples},
      {"worst_sample", r.worstSample},
      {"pairs_checked", r.pairsChecked},
      {"pairs_cached", r.pairsCached},
      {"pairs_executed", r.pairsChecked - r.pairsCached},
      {"pairs_unknown", r.pairsUnknown},
      {"collision_status", !r.error.empty()    ? "error"
                           : !r.checkedClashes ? "not_checked"
                           : r.cancelled       ? "incomplete"
                           : r.pairsUnknown    ? "unverified"
                           : r.clean           ? "passed"
                                               : "failed"},
      {"pairs_rigid", r.pairsRigid},
      {"seconds", r.seconds},
      {"unit", rot ? "deg" : "mm"},
      {"table", motion::Table(r, s)}};
  if (s.clashMode == "mesh" && s.checkClash)
    out["deflection_mm"] = r.deflectionMm;
  if (r.cancelled)
    out["cancelled"] = true;
  if (!r.error.empty())
    out["error"] = r.error;

  // The receipt (core/Receipt.h). This handler had the discipline first and
  // alone — `collision_status` has told error / not_checked / incomplete /
  // unverified / passed / failed apart since v87, and `unverified` there is
  // neither a pass nor a failure. The receipt is that vocabulary made common,
  // so a caller reads one field the same way across twelve RPCs instead of
  // learning this one's.
  //
  // Mapping is deliberately total: every state of collision_status lands
  // somewhere, and the four that are not verdicts land on Unverifiable with
  // their reason, never on Pass.
  {
    receipt::Receipt receipt;
    const receipt::Oracle oracle{s.clashMode == "mesh"
                                     ? "designer/motion-mesh-clash"
                                     : "designer/motion-exact-clash",
                                 "1"};
    const std::string claim =
        "no pair comes into contact that was not touching at rest";
    if (!r.error.empty()) {
      receipt.Unverifiable(claim, oracle, "the study errored: " + r.error);
    } else if (!s.checkClash) {
      receipt.Unverifiable(
          claim, oracle,
          "this study was asked for kinematics only "
          "(check_clash false), so nothing looked for contact");
    } else if (r.cancelled) {
      receipt.Unverifiable(claim, oracle,
                           "cancelled after " + std::to_string(samples.size()) +
                               " of " + std::to_string(s.samples) +
                               " samples — the poses not reached were not "
                               "checked");
    } else if (r.pairsUnknown) {
      receipt.Unverifiable(claim, oracle,
                           std::to_string(r.pairsUnknown) +
                               " exact pair checks could not be verified");
    } else {
      receipt::Claim c(claim, oracle,
                       r.clean ? receipt::Verdict::Pass
                               : receipt::Verdict::Fail);
      // A mesh sweep answers off the tessellations. It is the right question
      // for "are there millimetres" and it is not the B-Rep, so it says so.
      c.basis = receipt::Basis::Verified;
      c.value = static_cast<double>(appearedJson.size());
      c.unit = "new contacts";
      c.note = std::to_string(r.pairsChecked) + " pair checks over " +
               std::to_string(samples.size()) + " samples" +
               (s.clashMode == "mesh"
                    ? ", off the tessellations at " +
                          std::to_string(r.deflectionMm) + " mm deflection"
                    : ", exact on the B-Rep");
      receipt.Add(std::move(c));
    }
    AttachReceipt(out, receipt);
  }
  return out;
}

AssemblyDocument& NeedAssembly(ui::AppState& state) {
  if (!state.assembly)
    throw std::runtime_error(
        "the active document is not an assembly — switch tabs with "
        "set_active_part (see list_parts)");
  return *state.assembly;
}

MotionStudy& NeedStudy(AssemblyDocument& asmDoc, const RpcParams& p) {
  const uint64_t id = p.at("study").get<uint64_t>();
  MotionStudy* s = asmDoc.FindMotionStudyById(id);
  if (!s) {
    std::vector<std::string> have;
    for (const MotionStudy& cand : asmDoc.GetMotionStudies())
      have.push_back(std::to_string(cand.id) + " (" + cand.name + ")");
    throw std::runtime_error("no motion study with id " + std::to_string(id) +
                             "; studies here: " + KnownList(have));
  }
  return *s;
}

} // namespace

std::span<const MethodDoc> MotionMethodDocs() { return MotionMethods; }

std::optional<json> HandleMotionRpc(ui::AppState& state, const std::string& m,
                                    const RpcParams& p) {
  if (m == "add_motion_study") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    MotionStudy s;
    const uint64_t driver = p.at("driver").get<uint64_t>();
    const Mate* mate = asmDoc.FindMateById(driver);
    if (!mate) {
      std::vector<std::string> have;
      for (const Mate& cand : asmDoc.GetMates())
        if (assembly::MateHasDof(cand.type, MateDof::Rotation) ||
            assembly::MateHasDof(cand.type, MateDof::Offset))
          have.push_back(std::to_string(cand.id) + " (" + cand.name + ")");
      throw std::runtime_error(
          "no mate with id " + std::to_string(driver) +
          "; mates with a degree of freedom: " + KnownList(have));
    }
    const bool canRot = assembly::MateHasDof(mate->type, MateDof::Rotation);
    const bool canSlide = assembly::MateHasDof(mate->type, MateDof::Offset);
    if (p.contains("dof")) {
      s.driverDof =
          DocumentIO::MateDofFromString(p.at("dof").get<std::string>());
    } else if (canRot && canSlide) {
      throw std::runtime_error("mate " + std::to_string(driver) +
                               " is cylindrical — it both turns and slides; "
                               "say dof: rotation or dof: offset");
    } else if (canRot) {
      s.driverDof = MateDof::Rotation;
    } else if (canSlide) {
      s.driverDof = MateDof::Offset;
    } else {
      throw std::runtime_error(
          "mate " + std::to_string(driver) + " (" +
          DocumentIO::MateToJson(*mate).at("mate_type").get<std::string>() +
          ") has no degree of freedom to drive — a revolute or cylindrical "
          "mate turns, a slider or cylindrical mate slides");
    }
    if ((s.driverDof == MateDof::Rotation && !canRot) ||
        (s.driverDof == MateDof::Offset && !canSlide))
      throw std::runtime_error("mate " + std::to_string(driver) + " has no " +
                               DocumentIO::MateDofToString(s.driverDof) +
                               " to drive");
    s.driverMate = driver;
    const bool rot = s.driverDof == MateDof::Rotation;
    s.from = RotAwareAngle(p.at("from").get<double>(), rot);
    s.to = RotAwareAngle(p.at("to").get<double>(), rot);
    if (std::abs(s.to - s.from) < 1e-12)
      throw std::runtime_error("from and to are equal — nothing would move");
    s.samples = p.value("samples", 48);
    if (s.samples < 2)
      throw std::runtime_error("samples must be at least 2");
    s.checkClash = p.value("check_clash", true);
    s.clashMode = p.value("clash_mode", std::string("mesh"));
    if (s.clashMode != "mesh" && s.clashMode != "exact")
      throw std::runtime_error("clash_mode '" + s.clashMode +
                               "' is not one of: mesh, exact");
    s.name = p.value("name", std::string{});
    if (s.name.empty())
      s.name = "Motion " + std::to_string(asmDoc.GetMotionStudies().size() + 1);
    ui::SnapshotAssembly(state);
    const uint64_t id = asmDoc.AddMotionStudy(std::move(s));
    return StudyJson(asmDoc, *asmDoc.FindMotionStudyById(id));
  }
  if (m == "set_motion_study") {
    // rpc-params: check_clash, clash_mode, from, name, samples, study, to
    AssemblyDocument& asmDoc = NeedAssembly(state);
    MotionStudy& s = NeedStudy(asmDoc, p);
    ui::SnapshotAssembly(state);
    const bool rot = s.driverDof == MateDof::Rotation;
    if (p.contains("name"))
      s.name = p.at("name").get<std::string>();
    if (p.contains("from"))
      s.from = RotAwareAngle(p.at("from").get<double>(), rot);
    if (p.contains("to"))
      s.to = RotAwareAngle(p.at("to").get<double>(), rot);
    if (p.contains("samples")) {
      s.samples = p.at("samples").get<int>();
      if (s.samples < 2)
        throw std::runtime_error("samples must be at least 2");
    }
    if (p.contains("check_clash"))
      s.checkClash = p.at("check_clash").get<bool>();
    if (p.contains("clash_mode")) {
      s.clashMode = p.at("clash_mode").get<std::string>();
      if (s.clashMode != "mesh" && s.clashMode != "exact")
        throw std::runtime_error("clash_mode '" + s.clashMode +
                                 "' is not one of: mesh, exact");
    }
    asmDoc.BumpVersion();
    state.motionResults.erase(s.id); // it described the old definition
    return StudyJson(asmDoc, s);
  }
  if (m == "remove_motion_study") {
    // rpc-params: study
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const uint64_t id = s.id;
    ui::SnapshotAssembly(state);
    asmDoc.RemoveMotionStudy(id);
    state.motionResults.erase(id);
    return json{{"removed", id}};
  }
  if (m == "list_motion_studies") {
    AssemblyDocument& asmDoc = NeedAssembly(state);
    json out = json::array();
    const bool running = ui::BackgroundJobRunning(state, "motion");
    for (const MotionStudy& s : asmDoc.GetMotionStudies()) {
      json j = StudyJson(asmDoc, s);
      const motion::Result* r = ui::MotionResultFor(state, s.id);
      j["has_result"] = r != nullptr;
      if (r) {
        j["stale"] = ui::MotionResultStale(state, s.id);
        j["clean"] = r->clean && r->checkedClashes && !j["stale"].get<bool>();
        j["collision_checked"] = r->checkedClashes;
      }
      j["running"] = running;
      out.push_back(j);
    }
    return json{{"studies", out}};
  }
  if (m == "run_motion_study") {
    // rpc-params: async, include_poses, leave_at, study
    AssemblyDocument& asmDoc = NeedAssembly(state);
    MotionStudy& s = NeedStudy(asmDoc, p);
    const bool includePoses = p.value("include_poses", false);
    if (p.value("async", false)) {
      std::string err;
      if (!ui::StartMotionRun(state, s.id, &err))
        throw std::runtime_error(err);
      return json{{"started", true}, {"running", true}, {"job", "motion"}};
    }
    const motion::Result r = ui::RunMotionNow(state, s.id);
    if (!r.error.empty())
      throw std::runtime_error(r.error);
    json out = ResultJson(r, s, includePoses);
    // A run happens on a COPY, so the assembly is left exactly where it was
    // — which means nothing about the far end of the travel can be measured
    // afterwards. `section` after a throttle study saw the shut throttle
    // twice, and the only way out was reading orientations from the poses
    // and doing quaternion arithmetic in the caller. `leave_at` scrubs the
    // live assembly to one of the samples instead, through the same walk
    // the scrub uses, so it arrives on the branch the run found rather than
    // on whichever one a jump would have landed on.
    if (p.contains("leave_at")) {
      const json& at = p.at("leave_at");
      const int n = static_cast<int>(r.samples.size());
      int idx = 0;
      if (at.is_string()) {
        const std::string w = at.get<std::string>();
        if (w == "last")
          idx = n - 1;
        else if (w == "start")
          idx = 0;
        else
          throw std::runtime_error(
              "leave_at: \"start\", \"last\", or a sample index 0.." +
              std::to_string(n - 1) + " — got " + at.dump());
      } else {
        idx = at.get<int>();
        if (idx < 0 || idx >= n)
          throw std::runtime_error("leave_at: sample " + std::to_string(idx) +
                                   " of a study with " + std::to_string(n) +
                                   " — the range is 0.." +
                                   std::to_string(n - 1));
      }
      std::string err;
      const double t = n > 1 ? double(idx) / double(n - 1) : 0.0;
      if (!ui::ApplyMotionPose(state, s.id, t, &err))
        throw std::runtime_error(err);
      out["left_at"] = idx;
    }
    return out;
  }
  if (m == "motion_status") {
    // rpc-params: include_poses, study
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const bool includePoses = p.value("include_poses", false);
    json out{{"running", ui::BackgroundJobRunning(state, "motion")}};
    if (const motion::Result* r = ui::MotionResultFor(state, s.id)) {
      out["result"] = ResultJson(*r, s, includePoses);
      out["stale"] = ui::MotionResultStale(state, s.id);
      if (out["stale"].get<bool>()) {
        out["result"]["last_clean"] = r->clean;
        out["result"]["clean"] = false;
        out["result"]["collision_status"] = "stale";
      }
      out["has_result"] = true;
    } else {
      out["has_result"] = false;
    }
    return out;
  }
  if (m == "set_motion_pose") {
    // rpc-params: study, t
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const double t = p.at("t").get<double>();
    if (t < 0.0 || t > 1.0)
      throw std::runtime_error("t is 0..1 along the study's range");
    std::string err;
    if (!ui::ApplyMotionPose(state, s.id, t, &err))
      throw std::runtime_error(err);
    const bool rot = s.driverDof == MateDof::Rotation;
    const double value = motion::ValueAt(s, t);
    json out{{"t", t},
             {"value", rot ? angles::ToDegrees(value) : value},
             {"unit", rot ? "deg" : "mm"}};
    int errored = 0;
    for (const Mate& mm : asmDoc.GetMates())
      if (!mm.error.empty())
        errored++;
    out["errored_mates"] = errored;
    return out;
  }
  if (m == "export_motion") {
    // rpc-params: path, study
    AssemblyDocument& asmDoc = NeedAssembly(state);
    const MotionStudy& s = NeedStudy(asmDoc, p);
    const std::string path = p.at("path").get<std::string>();
    std::string err;
    size_t nodes = 0, frames = 0;
    if (!ui::ExportMotionGlb(state, s.id, path, &err, &nodes, &frames))
      throw std::runtime_error(err);
    return json{{"path", path}, {"nodes", nodes}, {"frames", frames}};
  }
  return std::nullopt;
}

} // namespace remote
