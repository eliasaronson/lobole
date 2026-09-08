#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace catalog {
using Json = nlohmann::json;
// Stable content identity, independent of file layout, units and source path.
std::string Sha256(std::string_view bytes);
// options: id, revision, source, license, units, storage?, mapping?.
// Returns a normalized bearing catalog. Throws before any mutation on error.
Json Preview(std::string_view content, const std::string& format,
             const Json& options);
Json ReadFile(const std::string& path, const Json& options);
Json Summary(const Json& entry);
// Stored reference entries intentionally omit records and local paths.
Json Serialize(const Json& entries);
Json Deserialize(const Json& stored);
// Immutable (id, revision): same content resolves a reference, different
// content is refused. Returns false for an unchanged, already resolved import.
bool Install(Json& entries, const Json& entry);
const Json& Find(const Json& entries, const std::string& id,
                 const std::string& revision);
const Json& Record(const Json& entry, const std::string& designation);
// Explicit observations in mm; does not infer fits or alter geometry.
Json SeatRequirements(const Json& entry, const std::string& designation,
                      const Json& measurements);
} // namespace catalog
