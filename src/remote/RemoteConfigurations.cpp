#include "remote/RemoteConfigurations.h"

#include "core/Command.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"
#include "ui/AppActions.h"
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
     "suppression) or assembly (occurrence suppression)."},
    {"remove_configuration", "Remove a configuration by name."},
    {"list_configurations", "List configurations and the active one."},
    {"set_configuration", "Edit a configuration's overrides/suppressions."},
    {"set_active_configuration", "Activate a configuration and regenerate."},
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
                     {"suppressed_features", c.suppressedFeatures}});
    }
    out = {{"configurations", arr}, {"active", doc.ActiveConfiguration()}};
    return true;
  }
  if (m == "set_configuration") {
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
    asm_.RemoveConfiguration(id);
    out = json::object();
    return true;
  }
  if (m == "list_configurations") {
    json arr = json::array();
    for (const Configuration& c : asm_.GetConfigurations())
      arr.push_back({{"id", c.id},
                     {"name", c.name},
                     {"suppressed_occurrences", c.suppressedFeatures}});
    out = {{"configurations", arr}, {"active", asm_.ActiveConfiguration()}};
    return true;
  }
  if (m == "set_configuration") {
    const uint64_t id = p.at("id").get<uint64_t>();
    Configuration* c = asm_.FindConfiguration(id);
    if (!c)
      throw std::runtime_error("no such configuration");
    ui::SnapshotAssembly(state);
    if (p.contains("name"))
      c->name = p.at("name").get<std::string>();
    if (p.contains("suppressed_occurrences"))
      c->suppressedFeatures =
          p.at("suppressed_occurrences").get<std::vector<uint64_t>>();
    asm_.BumpVersion();
    out = {{"id", id}};
    return true;
  }
  if (m == "set_active_configuration") {
    const uint64_t id = p.value("id", uint64_t{0});
    if (id != 0 && !asm_.FindConfiguration(id))
      throw std::runtime_error("no such configuration");
    ui::SnapshotAssembly(state);
    asm_.SetActiveConfiguration(id);
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
