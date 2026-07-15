#include "remote/RemoteConfigurations.h"

#include "core/Command.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/Serialization.h"
#include "ui/AppState.h"

#include <memory>
#include <vector>

using nlohmann::json;

namespace remote {

bool HandleConfigurationRpc(ui::AppState& state, Document& doc,
                            const std::string& m, const json& p, json& out) {
  if (m == "add_configuration") {
    auto cmd =
        std::make_unique<AddConfigurationCommand>(p.value("name", std::string{}));
    AddConfigurationCommand* raw = cmd.get();
    state.commands->Execute(std::move(cmd), doc);
    const Configuration* c = doc.FindConfiguration(raw->ConfigurationId());
    out = {{"id", c->id}, {"name", c->name}};
    return true;
  }
  if (m == "remove_configuration") {
    const uint64_t id = p.at("id").get<uint64_t>();
    if (!doc.FindConfiguration(id))
      throw std::runtime_error("no such configuration");
    state.commands->Execute(std::make_unique<RemoveConfigurationCommand>(id), doc);
    out = json::object();
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
    if (!cur) throw std::runtime_error("no such configuration");
    json before = DocumentIO::ConfigurationToJson(*cur);
    Configuration copy = *cur;
    if (p.contains("name")) copy.name = p.at("name").get<std::string>();
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
        std::make_unique<ModifyConfigurationCommand>("Edit configuration",
                                                     std::move(before),
                                                     std::move(after)),
        doc);
    out = {{"id", id}};
    return true;
  }
  if (m == "set_active_configuration") {
    const uint64_t id = p.value("id", uint64_t{0});
    if (id != 0 && !doc.FindConfiguration(id))
      throw std::runtime_error("no such configuration");
    state.commands->Execute(std::make_unique<SetActiveConfigurationCommand>(id),
                            doc);
    // Echo the resulting geometry so a headless caller sees the effect.
    json vols = json::array();
    for (const Body& b : doc.GetBodies()) vols.push_back(b.shape.Volume());
    out = {{"active", doc.ActiveConfiguration()},
           {"bodies", doc.GetBodies().size()},
           {"volumes", vols}};
    return true;
  }
  return false;
}

}  // namespace remote
