#include "remote/RemoteConfigurations.h"
#include "core/Angles.h"
#include "core/AssemblySolve.h"

#include "core/Command.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"

#include <memory>
#include <vector>

using nlohmann::json;

namespace remote {

// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc ConfigurationMethods[] = {
    {"add_configuration",
     "Add a configuration to the active part (parameter overrides + feature "
     "suppression) or assembly (occurrence suppression).",
     "name?:string"},
    {"remove_configuration", "Remove a configuration by `id`.", "id"},
    {"list_configurations",
     "List configurations and the active one - of the active tab, or of "
     "`part` (id or name) from any tab.",
     "part?:string|int", remote::NoFlags,
     "configurations[]{id, name, and then the tab's own: on a PART "
     "parameter_overrides, suppressed_features, unsuppressed_features; on "
     "an ASSEMBLY suppressed_occurrences, suppressed_mates, "
     "suppressed_relations, mate_overrides}, active"},
    {"set_configuration",
     "Edit a configuration's overrides/suppressions. PART: "
     "parameter_overrides [{name, expression}], suppressed_features [ids], "
     "unsuppressed_features [ids] - features the authored model keeps OFF "
     "that this configuration turns on, and the last word over both "
     "suppression lists (a flat pattern is exactly this). "
     "ASSEMBLY: suppressed_occurrences, suppressed_mates, "
     "suppressed_relations [ids], mate_overrides [{mate, rotation (radians) "
     "| rotation_deg, offset (mm)}] — DOFs the configuration holds; an "
     "overlay, so leaving it restores the mates. Each list replaces; the "
     "other kind's keys are refused. Editing the active one re-solves.",
     "id, name, parameter_overrides, suppressed_features, "
     "unsuppressed_features, "
     "suppressed_occurrences, suppressed_mates, suppressed_relations, "
     "mate_overrides"},
    {"set_active_configuration",
     "Activate a configuration (0 = base) and regenerate (part) or re-solve "
     "(assembly).",
     "id?:int>=0"},
};
std::span<const MethodDoc> ConfigurationMethodDocs() {
  return ConfigurationMethods;
}

bool HandleConfigurationRpc(ui::AppState& state, Document& doc,
                            const std::string& m, const RpcParams& p,
                            json& out) {
  if (m == "add_configuration") {
    auto cmd = std::make_unique<AddConfigurationCommand>(
        p.value("name", std::string{}));
    AddConfigurationCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), doc);
    const Configuration* c = doc.FindConfiguration(raw->ConfigurationId());
    out = {{"id", c->id}, {"name", c->name}};
    AttachRegen(out, &doc);
    return true;
  }
  if (m == "remove_configuration") {
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!doc.FindConfiguration(id))
      throw std::runtime_error("no such configuration");
    state.commands->Execute(std::make_unique<RemoveConfigurationCommand>(id),
                            doc);
    out = json::object();
    AttachRegen(out, &doc);
    return true;
  }
  if (m == "list_configurations") {
    json arr = json::array();
    for (const Configuration& c : doc.GetConfigurations()) {
      json overrides = json::array();
      for (const auto& [name, expr] : c.parameterOverrides)
        overrides.push_back({{"name", name}, {"expression", expr}});
      arr.push_back({{"id", c.id},
                     {"name", c.name},
                     {"parameter_overrides", overrides},
                     {"suppressed_features", c.suppressedFeatures},
                     {"unsuppressed_features", c.unsuppressedFeatures}});
    }
    out = {{"configurations", arr}, {"active", doc.ActiveConfiguration()}};
    return true;
  }
  if (m == "set_configuration") {
    // An ASSEMBLY configuration's keys, sent to a part, used to be ignored in
    // silence — a caller told it had hidden an occurrence had not.
    for (const char* k : {"suppressed_occurrences", "suppressed_mates",
                          "suppressed_relations", "mate_overrides"})
      if (p.contains(k))
        throw std::runtime_error(
            std::string(k) +
            " belongs to an ASSEMBLY configuration; a part configuration "
            "takes parameter_overrides, suppressed_features and "
            "unsuppressed_features");
    // Full-field edit: name / parameter_overrides / suppressed_features each
    // replace when present. The caller reads list_configurations, adjusts, and
    // sends the desired state back (one ModifyConfigurationCommand).
    const uint64_t id = p.at("id").get<uint64_t>();
    const Configuration* cur = doc.FindConfiguration(id);
    if (!cur)
      throw std::runtime_error("no such configuration");
    json before = DocumentIO::ConfigurationToJson(*cur);
    Configuration copy = *cur;
    if (p.contains("name"))
      copy.name = p.at("name").get<std::string>();
    if (p.contains("parameter_overrides")) {
      copy.parameterOverrides.clear();
      for (const json& oj : p.at("parameter_overrides"))
        copy.parameterOverrides.emplace_back(
            oj.at("name").get<std::string>(),
            oj.at("expression").get<std::string>());
    }
    if (p.contains("suppressed_features"))
      copy.suppressedFeatures =
          p.at("suppressed_features").get<std::vector<uint64_t>>();
    if (p.contains("unsuppressed_features"))
      copy.unsuppressedFeatures =
          p.at("unsuppressed_features").get<std::vector<uint64_t>>();
    json after = DocumentIO::ConfigurationToJson(copy);
    state.commands->Execute(
        std::make_unique<ModifyConfigurationCommand>(
            "Edit configuration", std::move(before), std::move(after)),
        doc);
    out = {{"id", id}};
    // Editing the ACTIVE configuration re-applies it, so an override that
    // breaks a feature breaks it here, not on the next activation.
    AttachRegen(out, &doc);
    return true;
  }
  if (m == "set_active_configuration") {
    const uint64_t id = p.value("id", uint64_t{0});
    if (id != 0 && !doc.FindConfiguration(id))
      throw std::runtime_error("no such configuration");
    const json preDigest = BodyDigestJson(doc);
    state.commands->Execute(std::make_unique<SetActiveConfigurationCommand>(id),
                            doc);
    // Echo the resulting geometry so a headless caller sees the effect.
    //
    // `bodies` and `volumes` are a hand-rolled subset of what `digest` now
    // carries, kept because configuration_smoke.py reads `volumes` and
    // breaking a working caller to de-duplicate a response is a bad trade.
    // New callers should read `digest`.
    //
    // The bug that put `regen` here: activating a configuration whose override
    // breaks a feature returned exactly this — a success, one body, and a
    // plausible volume. On a 20 mm box whose fillet failed, `volumes` read
    // 8000.0, i.e. the UNFILLETED box, so the response looked healthy while
    // the model was broken.
    json vols = json::array();
    for (const Body& b : doc.GetBodies())
      vols.push_back(b.shape.Volume());
    out = {{"active", doc.ActiveConfiguration()},
           {"bodies", doc.GetBodies().size()},
           {"volumes", vols},
           {"digest", DigestJson(preDigest, doc)}};
    AttachRegen(out, &doc);
    return true;
  }
  return false;
}

bool HandleAssemblyConfigurationRpc(ui::AppState& state, AssemblyDocument& asm_,
                                    const std::string& m, const RpcParams& p,
                                    json& out) {
  if (m == "add_configuration") {
    ui::SnapshotAssembly(state);
    const Configuration& c =
        asm_.AddConfiguration(p.value("name", std::string{}));
    out = {{"id", c.id}, {"name", c.name}};
    return true;
  }
  if (m == "remove_configuration") {
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!asm_.FindConfiguration(id))
      throw std::runtime_error("no such configuration");
    ui::SnapshotAssembly(state);
    const bool wasActive = asm_.ActiveConfiguration() == id;
    asm_.RemoveConfiguration(id);
    if (wasActive) // back to base: its mates and relations are all in again
      ui::SolveActiveAssembly(state);
    out = json::object();
    return true;
  }
  if (m == "list_configurations") {
    json arr = json::array();
    for (const Configuration& c : asm_.GetConfigurations()) {
      json holds = json::array();
      for (const MateOverride& o : c.mateOverrides) {
        json oj = {{"mate", o.mate}};
        if (o.rotation) {
          oj["rotation"] = *o.rotation;
          oj["rotation_deg"] = angles::ToDegrees(*o.rotation);
        }
        if (o.offset)
          oj["offset"] = *o.offset;
        holds.push_back(std::move(oj));
      }
      arr.push_back({{"id", c.id},
                     {"name", c.name},
                     {"suppressed_occurrences", c.suppressedFeatures},
                     {"suppressed_mates", c.suppressedMates},
                     {"suppressed_relations", c.suppressedRelations},
                     {"mate_overrides", holds}});
    }
    out = {{"configurations", arr}, {"active", asm_.ActiveConfiguration()}};
    return true;
  }
  if (m == "set_configuration") {
    const uint64_t id = p.at("id").get<uint64_t>();
    Configuration* c = asm_.FindConfiguration(id);
    if (!c)
      throw std::runtime_error("no such configuration");
    // A PART configuration's keys used to be ignored here in silence.
    for (const char* k : {"parameter_overrides", "suppressed_features",
                          "unsuppressed_features"})
      if (p.contains(k))
        throw std::runtime_error(
            std::string(k) +
            " belongs to a PART configuration; an assembly configuration "
            "takes suppressed_occurrences, suppressed_mates, "
            "suppressed_relations and mate_overrides");
    // Parse and check everything before touching the document, so a refusal
    // leaves neither a half-edit nor a stray undo step.
    Configuration next = *c;
    if (p.contains("name"))
      next.name = p.at("name").get<std::string>();
    if (p.contains("suppressed_occurrences"))
      next.suppressedFeatures =
          p.at("suppressed_occurrences").get<std::vector<uint64_t>>();
    // v129: a mate INSIDE a flexible placement is the sub-assembly's to hold
    // or suppress, through the configuration that placement runs under —
    // two configurations, the parent's and the sub's, fighting over one
    // mate is a model nobody can read.
    for (const char* k : {"suppressed_mates", "mate_overrides"})
      if (p.contains(k))
        for (const json& e : p.at(k))
          if (e.is_object() && e.contains("path"))
            throw std::runtime_error(
                std::string(k) +
                ": a mate inside a flexible sub-assembly placement is held "
                "or suppressed by the sub-assembly's OWN configuration — "
                "give it one there, and choose it for this placement with "
                "set_occurrence_configuration; this configuration takes "
                "this assembly's mates");
    if (p.contains("suppressed_mates")) {
      next.suppressedMates =
          p.at("suppressed_mates").get<std::vector<uint64_t>>();
      for (uint64_t mid : next.suppressedMates)
        if (!asm_.FindMateById(mid))
          throw std::runtime_error("suppressed_mates: no mate " +
                                   std::to_string(mid) +
                                   " (list_mates shows them)");
    }
    if (p.contains("suppressed_relations")) {
      next.suppressedRelations =
          p.at("suppressed_relations").get<std::vector<uint64_t>>();
      for (uint64_t rid : next.suppressedRelations)
        if (!asm_.FindRelationById(rid))
          throw std::runtime_error("suppressed_relations: no relation " +
                                   std::to_string(rid) +
                                   " (list_relations shows them)");
    }
    if (p.contains("mate_overrides")) {
      next.mateOverrides.clear();
      for (const json& oj : p.at("mate_overrides")) {
        MateOverride o;
        o.mate = oj.at("mate").get<uint64_t>();
        const Mate* mate = asm_.FindMateById(o.mate);
        if (!mate)
          throw std::runtime_error("mate_overrides: no mate " +
                                   std::to_string(o.mate) +
                                   " (list_mates shows them)");
        if (oj.contains("rotation") && oj.contains("rotation_deg"))
          throw std::runtime_error(
              "mate_overrides: give rotation (radians) OR rotation_deg, not "
              "both");
        if (oj.contains("rotation"))
          o.rotation = oj.at("rotation").get<double>();
        if (oj.contains("rotation_deg"))
          o.rotation = (angles::ToRadians(oj.at("rotation_deg").get<double>()));
        if (oj.contains("offset"))
          o.offset = oj.at("offset").get<double>();
        if (!o.rotation && !o.offset)
          throw std::runtime_error("mate_overrides: mate " +
                                   std::to_string(o.mate) +
                                   " holds nothing — give rotation, "
                                   "rotation_deg or offset");
        if (o.rotation && !assembly::MateHasDof(mate->type, MateDof::Rotation))
          throw std::runtime_error(
              "mate_overrides: mate " + std::to_string(o.mate) +
              " has no rotation to hold — a revolute, cylindrical or planar "
              "mate turns");
        next.mateOverrides.push_back(o);
      }
    }
    ui::SnapshotAssembly(state);
    *c = std::move(next);
    asm_.BumpVersion();
    out = {{"id", id}};
    // Editing the ACTIVE configuration re-applies it, as a part's regenerates.
    if (asm_.ActiveConfiguration() == id)
      ui::SolveActiveAssembly(state);
    return true;
  }
  if (m == "set_active_configuration") {
    const uint64_t id = p.value("id", uint64_t{0});
    if (id != 0 && !asm_.FindConfiguration(id))
      throw std::runtime_error("no such configuration");
    ui::SnapshotAssembly(state);
    asm_.SetActiveConfiguration(id);
    // Its suppressions and holds are applied by the SOLVE: without one the
    // variant was chosen and nothing moved.
    ui::SolveActiveAssembly(state);
    int visible = 0;
    for (const Occurrence& o : asm_.GetOccurrences())
      if (o.visible && !asm_.OccurrenceSuppressed(o.id))
        ++visible;
    out = {{"active", asm_.ActiveConfiguration()},
           {"visible_occurrences", visible}};
    return true;
  }
  return false;
}

} // namespace remote
