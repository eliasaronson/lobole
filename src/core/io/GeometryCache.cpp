// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/GeometryCache.h"
#include "core/io/Serialization.h" // ElementMapToJson

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
// 2: bends. A sheet body's BendRegions are derived like its element map, and
// a baked open executes nothing to re-derive them — so before this the bend
// lines vanished from a reopened project's DXF and check_sheet_metal checked
// nothing while reporting ok. An older blob is ignored, which rebuilds and
// re-stamps them.
// 3: threads. A body's ThreadRegions (what a screw may engage) and
// ThreadMarks (what a thread mill reads) are stamped by the Thread and Hole
// features the same way, so a reopened pan/tilt head lost both: every screw
// in a tapped hole read as an exact clash of its thread's depth, and the
// base plate's 3/8-16 had no thread for CAM to mill.
// 4: holes. A body's HoleMarks (what a CAM drill reads of a hole's point,
// and the job's list of what it does not cut) are stamped by the Hole the
// same way; and a Hole's derived drill point changes what an unchanged
// recipe builds, so a blob baked before it is rebuilt rather than read.
// 5: ring grips. A retaining ring's and its groove's RingGrips (what a ring
// in its groove may overlap) are stamped by their features the same way, so
// a reopened head would have read every circlip as a clash with its shaft.
// 6: derived_from. A Derived copy's body names its source body's creator,
// which is how it reads the size bands its snapshot carries and its source
// part's per-body print settings; baked without it, a reopened copy of a
// printed pin read its press as a clash, and a plate printed the copy at
// the part's settings rather than the source body's.
constexpr int kGeometryFormat = 6;

// A bend record, whole. It is not document data — the features stamp it on
// every regeneration — but a baked open regenerates nothing, so the blob has
// to carry it for the same reason it carries the element map.
json V3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }
glm::dvec3 V3(const json& j, const glm::dvec3& fallback = {}) {
  if (!j.is_array() || j.size() != 3)
    return fallback;
  return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}
json RefJson(const ElementRef& r) {
  return json{{"feature", r.featureId}, {"element", r.name}};
}
ElementRef RefFromJson(const json& j) {
  if (!j.is_object())
    return {};
  return ElementRef{j.value("feature", uint64_t{0}),
                    j.value("element", std::string{})};
}

json BendToJson(const BendRegion& b) {
  json trims = json::array();
  for (const BendRegion::Trim& t : b.trims)
    trims.push_back({{"point", V3(t.point)}, {"normal", V3(t.normal)}});
  json place = json::array();
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      place.push_back(b.parentPlace[c][r]);
  return json{{"feature", b.featureId},
              {"axis_origin", V3(b.axisOrigin)},
              {"axis_dir", V3(b.axisDir)},
              {"radius", b.radius},
              {"crushed", b.crushed},
              {"width", b.width},
              {"angle", b.angle},
              {"k_factor", b.kFactor},
              {"k_source", b.kSource},
              {"parent_face", RefJson(b.parentFace)},
              {"child_face", RefJson(b.childFace)},
              {"inner_face", RefJson(b.innerFace)},
              {"outer_face", RefJson(b.outerFace)},
              {"parent_dir", V3(b.parentDir)},
              {"up_dir", V3(b.upDir)},
              {"allowance", b.allowance},
              {"flat_a", V3(b.flatA)},
              {"flat_b", V3(b.flatB)},
              {"flat", b.flat},
              {"unfolded_by", b.unfoldedBy},
              {"parent_place", std::move(place)},
              {"trims", std::move(trims)}};
}

json ThreadRegionToJson(const ThreadRegion& t) {
  return json{{"face", RefJson(t.face)},
              {"designation", t.designation},
              {"length", t.length},
              {"minimum_engagement", t.minimumEngagement},
              {"external", t.external}};
}

ThreadRegion ThreadRegionFromJson(const json& j) {
  ThreadRegion t;
  t.face = RefFromJson(j.value("face", json()));
  t.designation = j.value("designation", std::string{});
  t.length = j.value("length", 0.0);
  t.minimumEngagement = j.value("minimum_engagement", 0.0);
  t.external = j.value("external", false);
  return t;
}

json RingGripToJson(const RingGrip& g) {
  return json{{"face", RefJson(g.face)},
              {"feature", g.featureId},
              {"standard", g.standard},
              {"size", g.size},
              {"groove", g.groove}};
}

RingGrip RingGripFromJson(const json& j) {
  RingGrip g;
  g.face = RefFromJson(j.value("face", json()));
  g.featureId = j.value("feature", uint64_t{0});
  g.standard = j.value("standard", std::string{});
  g.size = j.value("size", 0.0);
  g.groove = j.value("groove", false);
  return g;
}

json ThreadMarkToJson(const ThreadMark& t) {
  return json{{"face", RefJson(t.face)},
              {"feature", t.featureId},
              {"designation", t.designation},
              {"major_dia", t.majorDia},
              {"pitch", t.pitch},
              {"class", t.threadClass},
              {"right_hand", t.rightHand},
              {"external", t.external},
              {"length", t.length},
              {"modeled", t.modeled},
              {"flank_deg", t.flankDeg}};
}

ThreadMark ThreadMarkFromJson(const json& j) {
  ThreadMark t;
  t.face = RefFromJson(j.value("face", json()));
  t.featureId = j.value("feature", uint64_t{0});
  t.designation = j.value("designation", std::string{});
  t.majorDia = j.value("major_dia", 0.0);
  t.pitch = j.value("pitch", 0.0);
  t.threadClass = j.value("class", std::string{});
  t.rightHand = j.value("right_hand", true);
  t.external = j.value("external", false);
  t.length = j.value("length", 0.0);
  t.modeled = j.value("modeled", false);
  // Absent in a blob from before flank angles, when every thread was 30.
  t.flankDeg = j.value("flank_deg", 30.0);
  return t;
}

json HoleMarkToJson(const HoleMark& h) {
  return json{{"bore", RefJson(h.bore)},
              {"feature", h.featureId},
              {"name", h.feature},
              {"diameter", h.diameter},
              {"depth", h.depth},
              {"through", h.through},
              {"point_angle_deg", h.pointAngleDeg},
              {"thread", h.thread}};
}

HoleMark HoleMarkFromJson(const json& j) {
  HoleMark h;
  h.bore = RefFromJson(j.value("bore", json()));
  h.featureId = j.value("feature", uint64_t{0});
  h.feature = j.value("name", std::string{});
  h.diameter = j.value("diameter", 0.0);
  h.depth = j.value("depth", 0.0);
  h.through = j.value("through", false);
  h.pointAngleDeg = j.value("point_angle_deg", 180.0);
  h.thread = j.value("thread", std::string{});
  return h;
}

BendRegion BendFromJson(const json& j) {
  BendRegion b;
  b.featureId = j.value("feature", uint64_t{0});
  b.axisOrigin = V3(j.value("axis_origin", json()), b.axisOrigin);
  b.axisDir = V3(j.value("axis_dir", json()), b.axisDir);
  b.radius = j.value("radius", 0.0);
  b.crushed = j.value("crushed", false);
  b.width = j.value("width", 0.0);
  b.angle = j.value("angle", 0.0);
  b.kFactor = j.value("k_factor", 0.0);
  b.kSource = j.value("k_source", std::string{});
  b.parentFace = RefFromJson(j.value("parent_face", json()));
  b.childFace = RefFromJson(j.value("child_face", json()));
  b.innerFace = RefFromJson(j.value("inner_face", json()));
  b.outerFace = RefFromJson(j.value("outer_face", json()));
  b.parentDir = V3(j.value("parent_dir", json()), b.parentDir);
  b.upDir = V3(j.value("up_dir", json()), b.upDir);
  b.allowance = j.value("allowance", 0.0);
  b.flatA = V3(j.value("flat_a", json()), b.flatA);
  b.flatB = V3(j.value("flat_b", json()), b.flatB);
  b.flat = j.value("flat", false);
  b.unfoldedBy = j.value("unfolded_by", uint64_t{0});
  const json place = j.value("parent_place", json::array());
  if (place.is_array() && place.size() == 16) {
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        b.parentPlace[c][r] = place[c * 4 + r].get<double>();
  }
  for (const json& tj : j.value("trims", json::array())) {
    BendRegion::Trim t;
    t.point = V3(tj.value("point", json()), t.point);
    t.normal = V3(tj.value("normal", json()), t.normal);
    b.trims.push_back(t);
  }
  return b;
}

fs::path GeometryDir(const std::string& dir) {
  return fs::path(dir) / "geometry";
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

// The name a blob's directory goes by in BodiesAreBlob: the same project
// reached by two spellings of its path is one blob.
std::string BlobSource(const std::string& dir) {
  std::error_code ec;
  const fs::path p = fs::weakly_canonical(GeometryDir(dir), ec);
  return ec ? GeometryDir(dir).string() : p.string();
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
  // Already on disk, byte for byte -- but only if the bodies in memory are
  // the ones that blob installed. The hash names the recipe, not the code
  // that executed it: a part regenerated by a build that changed what its
  // recipe produces carries the old blob's hash and different bodies, and
  // skipping here left the old bodies on disk (a 0.4 um skin over a
  // counterbore, fixed in the kernel, came back on every open).
  if (StoredHash(dir) == hash && doc.BodiesAreBlob(hash, BlobSource(dir)))
    return true;

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
      // A derived ghost stays a ghost across a baked open: without this a
      // reloaded jaw would weigh its servo (v84).
      if (body.reference)
        entry["reference"] = true;
      if (body.derivedFrom != 0)
        entry["derived_from"] = body.derivedFrom;
      if (!body.bends.empty()) {
        json bends = json::array();
        for (const BendRegion& b : body.bends)
          bends.push_back(BendToJson(b));
        entry["bends"] = std::move(bends);
      }
      if (!body.threadRegions.empty()) {
        json regions = json::array();
        for (const ThreadRegion& t : body.threadRegions)
          regions.push_back(ThreadRegionToJson(t));
        entry["thread_regions"] = std::move(regions);
      }
      if (!body.ringGrips.empty()) {
        json grips = json::array();
        for (const RingGrip& g : body.ringGrips)
          grips.push_back(RingGripToJson(g));
        entry["ring_grips"] = std::move(grips);
      }
      if (!body.threads.empty()) {
        json marks = json::array();
        for (const ThreadMark& t : body.threads)
          marks.push_back(ThreadMarkToJson(t));
        entry["threads"] = std::move(marks);
      }
      if (!body.holes.empty()) {
        json holes = json::array();
        for (const HoleMark& h : body.holes)
          holes.push_back(HoleMarkToJson(h));
        entry["holes"] = std::move(holes);
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
    // An index that says the same thing keeps its file too: a part this build
    // regenerated is written even under a matching hash, and when nothing
    // came out different, nothing on disk should look touched.
    if (SameBytes(indexTmp, geom / "index.json")) {
      std::error_code rmEc;
      fs::remove(indexTmp, rmEc);
    } else {
      fs::rename(indexTmp, geom / "index.json");
    }
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
      body.reference = bj.value("reference", false);
      body.derivedFrom = bj.value("derived_from", uint64_t{0});
      for (const json& bendJson : bj.value("bends", json::array()))
        body.bends.push_back(BendFromJson(bendJson));
      for (const json& t : bj.value("thread_regions", json::array()))
        body.threadRegions.push_back(ThreadRegionFromJson(t));
      for (const json& g : bj.value("ring_grips", json::array()))
        body.ringGrips.push_back(RingGripFromJson(g));
      for (const json& t : bj.value("threads", json::array()))
        body.threads.push_back(ThreadMarkFromJson(t));
      for (const json& h : bj.value("holes", json::array()))
        body.holes.push_back(HoleMarkFromJson(h));
      bodies.push_back(std::move(body));
    }
    if (bodies.empty())
      return false;
    std::map<uint64_t, std::string> warnings;
    const json warningsJson = j.value("warnings", json::object()); // see above
    for (const auto& [key, value] : warningsJson.items())
      warnings[std::stoull(key)] = value.get<std::string>();
    doc.AdoptBakedGeometry(std::move(bodies), j.value("chain_hash", size_t{0}),
                           std::move(warnings), BlobSource(dir));
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

BakedGeometryOnDisk InspectGeometry(const std::string& dir) {
  std::ifstream in(GeometryDir(dir) / "index.json");
  if (!in)
    return BakedGeometryOnDisk::None;
  try {
    const int format = json::parse(in).value("geometry_format", 0);
    if (format < kGeometryFormat)
      return BakedGeometryOnDisk::OlderFormat;
    if (format > kGeometryFormat)
      return BakedGeometryOnDisk::NewerFormat;
    return BakedGeometryOnDisk::Present;
  } catch (const std::exception&) {
    return BakedGeometryOnDisk::None; // unreadable is as good as absent
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
        "# Written by Lobole. Baked geometry is derived data: binary, and\n"
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
      out << "# Written by Lobole: this project does not commit its baked\n"
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
