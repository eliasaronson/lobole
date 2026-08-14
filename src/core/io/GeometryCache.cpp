#include "core/io/GeometryCache.h"

#include "common/Logger.h"
#include "core/Document.h"
#include "core/Feature.h"

#include <nlohmann/json.hpp>

#include <BinTools.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>
#include <TopoDS_Shape.hxx>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace DocumentIO {
namespace {

// Bumped only when the blob's own layout changes. Independent of the recipe's
// kVersion, which this does not touch: an older build ignores a directory it
// does not know, a newer one rebuilds when the blob is absent, and both are
// already correct.
constexpr int kGeometryFormat = 1;

fs::path GeometryDir(const std::string& dir) {
  return fs::path(dir) / "geometry";
}

json ElementMapToJson(const ElementMap& map) {
  json faces = json::object();
  for (const auto& [index, name] : map.names)
    faces[std::to_string(index)] = name;
  json edges = json::object();
  for (const auto& [index, name] : map.edgeNames)
    edges[std::to_string(index)] = name;
  return json{{"faces", std::move(faces)}, {"edges", std::move(edges)}};
}

ElementMap ElementMapFromJson(const json& j) {
  ElementMap map;
  for (const auto& [key, value] : j.value("faces", json::object()).items())
    map.names[std::stoi(key)] = value.get<std::string>();
  for (const auto& [key, value] : j.value("edges", json::object()).items())
    map.edgeNames[std::stoi(key)] = value.get<std::string>();
  return map;
}

// Do these two files hold the same bytes? Cheap next to writing a B-Rep, and
// it is what keeps an unchanged shape's file untouched.
bool SameBytes(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (!fs::exists(b, ec) || fs::file_size(a, ec) != fs::file_size(b, ec))
    return false;
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  if (!fa || !fb)
    return false;
  constexpr size_t kChunk = 64 * 1024;
  std::vector<char> bufA(kChunk), bufB(kChunk);
  while (fa && fb) {
    fa.read(bufA.data(), kChunk);
    fb.read(bufB.data(), kChunk);
    const auto got = fa.gcount();
    if (got != fb.gcount())
      return false;
    if (std::memcmp(bufA.data(), bufB.data(), static_cast<size_t>(got)) != 0)
      return false;
    if (got == 0)
      break;
  }
  return true;
}

// The hash already on disk, or 0. Lets an unchanged save skip the write
// entirely — repeated saves must not rewrite megabytes or churn git history.
size_t StoredHash(const std::string& dir) {
  std::ifstream in(GeometryDir(dir) / "index.json");
  if (!in)
    return 0;
  try {
    const json j = json::parse(in);
    if (j.value("geometry_format", 0) != kGeometryFormat)
      return 0;
    return j.value("chain_hash", size_t{0});
  } catch (const std::exception&) {
    return 0;
  }
}

} // namespace

bool SaveGeometry(const Document& doc, const std::string& dir,
                  std::string* error) {
  const size_t hash = doc.TipChainHash();
  if (doc.GetBodies().empty() || hash == 0) {
    RemoveGeometry(dir); // nothing to bake; do not leave a stale blob behind
    return true;
  }
  // Never bake a half-built model: a blob is a promise that this is what the
  // recipe produces, and a failed feature means it is not.
  for (const auto& feature : doc.GetFeatures()) {
    if (!feature->GetError().empty()) {
      RemoveGeometry(dir);
      return true;
    }
  }
  if (StoredHash(dir) == hash)
    return true; // already on disk, byte for byte

  try {
    const fs::path geom = GeometryDir(dir);
    fs::create_directories(geom);
    json index{{"geometry_format", kGeometryFormat},
               {"chain_hash", hash},
               {"occ_version", std::string(OCC_VERSION_COMPLETE)},
               {"bodies", json::array()}};
    // Advisories are products of execution, and a baked open executes
    // nothing; carry them so a model does not lose its warnings on reopen.
    json warnings = json::object();
    for (const auto& feature : doc.GetFeatures()) {
      if (!feature->GetWarning().empty())
        warnings[std::to_string(feature->GetId())] = feature->GetWarning();
    }
    if (!warnings.empty())
      index["warnings"] = std::move(warnings);
    std::vector<std::string> wanted;
    size_t i = 0;
    for (const Body& body : doc.GetBodies()) {
      const std::string file = std::to_string(i++) + ".brep";
      const fs::path target = geom / file;
      const fs::path tmp = geom / (file + ".tmp");
      if (!BinTools::Write(body.shape.GetShape(), tmp.string().c_str())) {
        if (error)
          *error = "could not write " + file;
        return false;
      }
      // Identical content keeps its mtime, like WriteTextAtomic: a cosmetic
      // edit (renaming a feature) changes the chain hash and so lands here,
      // but the SHAPE is unchanged, and rsync, backups and file watchers
      // should see exactly what changed. Git already ignores it — content
      // addressing means identical bytes add nothing to history.
      if (SameBytes(tmp, target)) {
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
      } else {
        fs::rename(tmp, target);
      }
      json entry{{"brep", file},
                 {"name", body.name},
                 {"material", body.material},
                 {"created_by", body.createdBy},
                 {"element_map", ElementMapToJson(body.elementMap)}};
      if (body.hasDisplayColor) {
        entry["display_color"] = {body.displayColor.x, body.displayColor.y,
                                  body.displayColor.z};
      }
      index["bodies"].push_back(std::move(entry));
      wanted.push_back(file);
    }
    // The index is written LAST, so a torn save leaves an index that either
    // does not exist or does not match — never one that points at half a body
    // list. Same reasoning as the recipe's feature index.
    const fs::path indexTmp = geom / "index.json.tmp";
    {
      std::ofstream out(indexTmp, std::ios::binary | std::ios::trunc);
      out << index.dump(2) << "\n";
      if (!out) {
        if (error)
          *error = "could not write the geometry index";
        return false;
      }
    }
    fs::rename(indexTmp, geom / "index.json");
    // Bodies that no longer exist leave no stale .brep behind.
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(geom, ec)) {
      const std::string name = entry.path().filename().string();
      if (entry.path().extension() == ".brep" &&
          std::find(wanted.begin(), wanted.end(), name) == wanted.end()) {
        fs::remove(entry.path(), ec);
      }
    }
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_WARN("Could not bake geometry into {}: {}", dir, e.what());
    return false;
  }
}

bool LoadGeometry(Document& doc, const std::string& dir) {
  const fs::path geom = GeometryDir(dir);
  std::ifstream in(geom / "index.json");
  if (!in)
    return false; // no blob: the document rebuilds, as it always did
  try {
    const json j = json::parse(in);
    if (j.value("geometry_format", 0) != kGeometryFormat) {
      LOG_INFO("Ignoring baked geometry in {}: format {} (this build reads {})",
               dir, j.value("geometry_format", 0), kGeometryFormat);
      return false;
    }
    // A different OCC build LOADS anyway: this geometry is what the author
    // saw, and rebuilding could shift face ordering — which moves the element
    // names that dimensions and mates reference. Worth a line in the log.
    const std::string occ = j.value("occ_version", std::string{});
    if (occ != OCC_VERSION_COMPLETE) {
      LOG_INFO("Baked geometry in {} was built by OCC {} (this build is {}); "
               "loading it as saved",
               dir, occ, OCC_VERSION_COMPLETE);
    }
    std::vector<Body> bodies;
    for (const json& bj : j.value("bodies", json::array())) {
      TopoDS_Shape shape;
      const fs::path brep = geom / bj.at("brep").get<std::string>();
      if (!BinTools::Read(shape, brep.string().c_str()) || shape.IsNull()) {
        LOG_INFO("Ignoring baked geometry in {}: {} is unreadable", dir,
                 brep.filename().string());
        return false;
      }
      Body body;
      body.name = bj.value("name", std::string{});
      body.shape = CADShape(shape);
      body.material = bj.value("material", std::string{});
      body.createdBy = bj.value("created_by", uint64_t{0});
      body.elementMap =
          ElementMapFromJson(bj.value("element_map", json::object()));
      if (bj.contains("display_color")) {
        const auto& c = bj.at("display_color");
        body.hasDisplayColor = true;
        body.displayColor = {c[0].get<float>(), c[1].get<float>(),
                             c[2].get<float>()};
      }
      bodies.push_back(std::move(body));
    }
    if (bodies.empty())
      return false;
    std::map<uint64_t, std::string> warnings;
    for (const auto& [key, value] : j.value("warnings", json::object()).items())
      warnings[std::stoull(key)] = value.get<std::string>();
    doc.AdoptBakedGeometry(std::move(bodies), j.value("chain_hash", size_t{0}),
                           std::move(warnings));
    return true;
  } catch (const Standard_Failure& e) {
    // OCC signals a corrupt or foreign-version .brep by THROWING, not by
    // returning false — a truncated blob must still degrade to a rebuild.
    LOG_INFO("Ignoring baked geometry in {}: {}", dir,
             e.GetMessageString() ? e.GetMessageString() : "unreadable shape");
    return false;
  } catch (const std::exception& e) {
    LOG_INFO("Ignoring baked geometry in {}: {}", dir, e.what());
    return false;
  }
}

bool WriteRepositoryHints(const std::string& dir, bool commit,
                          std::string* error) {
  try {
    const fs::path root(dir);
    // Marked binary so git never tries to diff or merge a B-Rep, and
    // non-mergeable so a conflict cannot interleave two shapes into garbage:
    // the resolution for derived data is "take either, the hash decides".
    const std::string attributes =
        "# Written by Designer. Baked geometry is derived data: binary, and\n"
        "# never merged — a conflicted .brep is resolved by rebuilding.\n"
        "geometry/*.brep binary -merge -diff\n";
    {
      std::ofstream out(root / ".gitattributes", std::ios::trunc);
      out << attributes;
      if (!out) {
        if (error)
          *error = "could not write .gitattributes";
        return false;
      }
    }
    const fs::path ignore = root / ".gitignore";
    if (commit) {
      // Turning the setting back on must un-ignore: leaving the rule would
      // silently keep the blob out of the repo the user just asked for.
      std::error_code ec;
      fs::remove(ignore, ec);
    } else {
      std::ofstream out(ignore, std::ios::trunc);
      out << "# Written by Designer: this project does not commit its baked\n"
             "# geometry, so opening a fresh clone rebuilds the model.\n"
             "geometry/\n";
      if (!out) {
        if (error)
          *error = "could not write .gitignore";
        return false;
      }
    }
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

bool RemoveGeometry(const std::string& dir) {
  std::error_code ec;
  fs::remove_all(GeometryDir(dir), ec);
  return !ec;
}

} // namespace DocumentIO
