#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/catalog/Catalog.h"
#include "core/io/Serialization.h"

namespace generated {
using Json = nlohmann::json;

inline Json Recipe(const Document& doc) {
  Json features = Json::array(), params = Json::array();
  for (const auto& feature : doc.GetFeatures())
    features.push_back(DocumentIO::FeatureToJson(*feature));
  for (const auto& p : doc.GetParameters())
    params.push_back({{"name", p.name},
                      {"expression", p.expression},
                      {"comment", p.comment}});
  Json configurations = Json::array();
  for (const auto& c : doc.GetConfigurations())
    configurations.push_back(DocumentIO::ConfigurationToJson(c));
  return {
      {"features", features},
      {"parameters", params},
      {"catalogs", doc.GetCatalogs()},
      {"configurations", configurations},
      {"active_configuration", doc.ActiveConfiguration()},
      {"rollback", doc.Rollback()},
      {"units",
       {units::ToString(doc.Units().length), units::ToString(doc.Units().mass),
        units::ToString(doc.Units().angle)}}};
}
inline std::string RecipeHash(const Document& doc) {
  return catalog::Sha256(Recipe(doc).dump());
}
inline Json Revision(const Document& doc) {
  return {{"instance", doc.InstanceId()},
          {"version", doc.Version()},
          {"recipe_hash", RecipeHash(doc)}};
}
inline bool Modified(const Document& doc) {
  return !doc.Generator().is_object() ||
         doc.Generator().value("recipe_hash", std::string{}) != RecipeHash(doc);
}

// Replayable recipe replacement preserving the Document and part identities.
// Carry the already-validated bodies so applying/undoing a staged build does
// not execute its booleans a second time. Datums still resolve via the existing
// baked-geometry path. This pilot deliberately excludes assets/configurations.
class UpdateCommand : public Command {
public:
  struct Snapshot {
    Json recipe, stamp;
    std::vector<Body> bodies;
    size_t tipHash = 0;
    std::map<uint64_t, std::string> warnings;
    explicit Snapshot(const Document& doc)
        : recipe(Recipe(doc)), stamp(doc.Generator()), bodies(doc.GetBodies()),
          tipHash(doc.TipChainHash()) {
      for (const auto& f : doc.GetFeatures())
        if (!f->GetWarning().empty())
          warnings[f->GetId()] = f->GetWarning();
    }
  };
  UpdateCommand(const Document& before, const Document& after, Json stamp)
      : before_(before), after_(after) {
    after_.stamp = std::move(stamp);
  }
  std::string Description() const override {
    return "Update generated component";
  }
  bool Execute(Document& doc) override {
    Apply(doc, after_);
    for (const auto& f : doc.GetFeatures())
      if (!f->GetError().empty()) {
        Apply(doc, before_);
        return false;
      }
    return true;
  }
  void Undo(Document& doc) override { Apply(doc, before_); }

private:
  Snapshot before_, after_;
  static void Apply(Document& doc, const Snapshot& snapshot) {
    Document::BulkEdit bulk(doc);
    const auto& features = snapshot.recipe.at("features");
    while (doc.FeatureCount() > features.size())
      doc.RemoveFeature(doc.FeatureCount() - 1);
    for (size_t i = 0; i < features.size(); ++i) {
      auto f = DocumentIO::FeatureFromJson(features[i]);
      if (i < doc.FeatureCount())
        doc.ReplaceFeature(i, std::move(f));
      else
        doc.AddFeature(std::move(f));
    }
    std::vector<Parameter> params;
    for (const auto& p : snapshot.recipe.at("parameters")) {
      Parameter param;
      param.name = p.at("name");
      param.expression = p.at("expression");
      param.comment = p.at("comment");
      params.push_back(std::move(param));
    }
    doc.SetParametersRaw(std::move(params));
    doc.SetCatalogsRaw(snapshot.recipe.at("catalogs"));
    doc.SetGeneratorRaw(snapshot.stamp);
    doc.ClearRegenCache();
    doc.AdoptBakedGeometry(snapshot.bodies, snapshot.tipHash,
                           snapshot.warnings);
  }
};
} // namespace generated
