#include "remote/RemoteCatalog.h"
#include "core/catalog/Catalog.h"
#include "ui/AppState.h"
#include "ui/CatalogActions.h"
#include <fstream>
namespace remote {
namespace {
const MethodDoc docs[] = {
    {"preview_catalog",
     "Read CSV/JSON bearing records without mutation. options: id, revision, "
     "source, license, units (mm|in), storage? (reference default|embedded), "
     "mapping? (canonical field to column). Returns metadata, hash and up to 5 "
     "sample records. Unknown fields are reported, not engineering rules.",
     "path, options", NotInBatch},
    {"import_catalog",
     "Install the exact previewed file as an immutable catalog revision. "
     "expected_hash comes from preview_catalog. Reference storage omits "
     "records from saves; embedded explicitly includes them. No geometry "
     "changes; undoable across tabs.",
     "path, options, expected_hash", NotInBatch},
    {"list_catalogs",
     "List project catalog revisions and resolution status, without private "
     "record data.",
     "", remote::NoFlags,
     "catalogs[]{id,revision,source,license,schema,units,storage,hash,resolved,"
     "record_count?}"},
    {"query_catalog",
     "Get a designation or a page (limit <=100) of records from a resolved "
     "catalog. Optional hash pins the expected content. Missing references "
     "must be resolved by importing matching data.",
     "catalog, revision, designation, hash, offset, limit"},
    {"catalog_requirements",
     "Preview bearing-seat limits against explicit measurements in mm: "
     "shaft_diameter, housing_diameter, shoulder_diameter, fillet_radius. "
     "Missing limits/observations are unverified. Does not change geometry or "
     "select fits. hash pins the record revision.",
     "catalog, revision, designation, hash, measurements"},
    {"export_catalog",
     "Export a versioned catalog package as reference metadata or embedded "
     "data. mode is required. Embedded export is refused for reference-only "
     "imports. No files referenced by records are copied.",
     "catalog, revision, hash, path, mode", NotInBatch},
};
const catalog::Json& Entry(const ui::AppState& state, const RpcParams& p) {
  const auto& entry =
      catalog::Find(state.catalogs, p.at("catalog").get<std::string>(),
                    p.at("revision").get<std::string>());
  if (p.contains("hash") && p.at("hash") != entry.at("hash"))
    throw std::runtime_error("catalog handle is stale: content hash differs");
  return entry;
}
} // namespace
std::span<const MethodDoc> CatalogMethodDocs() { return docs; }
std::optional<nlohmann::json> HandleCatalogRpc(ui::AppState& state,
                                               const std::string& m,
                                               const RpcParams& p) {
  using catalog::Json;
  if (m == "preview_catalog") {
    auto entry =
        catalog::ReadFile(p.at("path").get<std::string>(), p.at("options"));
    auto result = catalog::Summary(entry);
    result["sample"] = Json::array();
    for (size_t i = 0;
         i < std::min<size_t>(
                 5, entry.contains("records") ? entry.at("records").size() : 0);
         ++i)
      result["sample"].push_back(entry["records"][i]);
    return result;
  }
  if (m == "import_catalog") {
    auto entry =
        catalog::ReadFile(p.at("path").get<std::string>(), p.at("options"));
    if (p.at("expected_hash") != entry.at("hash"))
      throw std::runtime_error("catalog changed after preview; preview again");
    bool changed = ui::InstallCatalog(state, entry);
    return Json{{"catalog",
                 catalog::Summary(catalog::Find(state.catalogs, entry.at("id"),
                                                entry.at("revision")))},
                {"changed", changed}};
  }
  if (m == "list_catalogs") {
    Json entries = Json::array();
    for (const auto& e : state.catalogs)
      entries.push_back(catalog::Summary(e));
    return Json{{"catalogs", entries}};
  }
  if (m == "query_catalog") {
    const auto& e = Entry(state, p);
    if (p.contains("designation"))
      return Json{{"catalog", catalog::Summary(e)},
                  {"record",
                   catalog::Record(e, p.at("designation").get<std::string>())}};
    if (!e.contains("records"))
      throw std::runtime_error(
          "unresolved catalog: import matching data first");
    const int offset = p.value("offset", 0), limit = p.value("limit", 20);
    if (offset < 0 || limit < 1 || limit > 100)
      throw std::runtime_error("offset >=0 and limit 1..100 required");
    Json records = Json::array();
    const auto& rows = e.at("records");
    for (size_t i = static_cast<size_t>(offset);
         i < rows.size() && records.size() < static_cast<size_t>(limit); ++i)
      records.push_back(rows[i]);
    const size_t next = static_cast<size_t>(offset) + records.size();
    return Json{
        {"catalog", catalog::Summary(e)},
        {"records", records},
        {"next_offset", next < rows.size() ? Json(next) : Json(nullptr)}};
  }
  if (m == "catalog_requirements") {
    const auto& e = Entry(state, p);
    if (!p.contains("hash"))
      throw std::runtime_error(
          "hash is required for a pinned requirement preview");
    return catalog::SeatRequirements(e, p.at("designation").get<std::string>(),
                                     p.value("measurements", Json::object()));
  }
  if (m == "export_catalog") {
    auto e = Entry(state, p);
    const auto mode = p.at("mode").get<std::string>();
    if (mode != "reference" && mode != "embedded")
      throw std::runtime_error("mode must be reference or embedded");
    if (mode == "embedded" &&
        (e.at("storage") != "embedded" || !e.contains("records")))
      throw std::runtime_error(
          "embedded export requires an explicitly embedded, resolved import");
    e["storage"] = mode;
    auto package = Json{{"format", "designer-catalog"},
                        {"version", 1},
                        {"catalogs", catalog::Serialize(Json::array({e}))}};
    const auto path = p.at("path").get<std::string>();
    std::ofstream out(path, std::ios::binary);
    out << package.dump(2) << '\n';
    if (!out)
      throw std::runtime_error("cannot write catalog package");
    return Json{{"path", path}, {"mode", mode}, {"hash", e.at("hash")}};
  }
  return std::nullopt;
}
} // namespace remote
