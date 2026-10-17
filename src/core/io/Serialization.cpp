// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/Serialization.h"
#include "core/drawing/ViewProjection.h"
#include "core/io/SerializePrintSettings.h"

#include "core/MotionStudy.h"
#include "core/Requirements.h"
#include "core/io/GeometryCache.h"
#include "core/io/SerializationInternal.h"

#include "common/Logger.h"
#include "core/Feature.h"
#include "core/Occurrence.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/DerivedFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SweepFeature.h"
#include "core/fem/FemSolver.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>

using nlohmann::json;

namespace DocumentIO {

namespace {

// Path classification (F3): a `.dsgn` path or existing directory is the
// directory format; picking the index.json inside one opens the project.
bool IsDirFormatPath(const std::string& path) {
  if (path.ends_with(".dsgn"))
    return true;
  std::error_code ec;
  return std::filesystem::is_directory(path, ec);
}

std::string NormalizeProjectPath(const std::string& path) {
  const std::filesystem::path p(path);
  if (p.filename() == "index.json" &&
      p.parent_path().string().ends_with(".dsgn"))
    return p.parent_path().string();
  return path;
}

// "DatumPlane" -> "datum-plane": readable feature filenames. The stable id
// prefix is the identity; this suffix is cosmetic.
std::string KebabType(const std::string& type) {
  std::string out;
  for (char c : type) {
    if (std::isupper(static_cast<unsigned char>(c))) {
      if (!out.empty())
        out += '-';
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      out += c;
    }
  }
  return out;
}

// Display units off an envelope (v64). Absent = the default, which is what
// every document written before v64 means. An unknown STRING is refused, not
// substituted: a file that says "inch" and loads as millimetres is wrong in
// every dimension it shows and looks perfectly fine (CLAUDE.md).
units::System UnitsFromEnvelope(const json& j) {
  units::System sys;
  if (!j.contains("units"))
    return sys;
  const json u = j.at("units");
  const auto refuse = [](const std::string& what, const std::string& got,
                         const std::vector<const char*>& accepted) {
    std::string list;
    for (const char* a : accepted)
      list += (list.empty() ? "" : ", ") + std::string(a);
    throw std::runtime_error("unknown " + what + " unit '" + got +
                             "' (expected one of: " + list + ")");
  };
  if (u.contains("length")) {
    const std::string v = u.at("length").get<std::string>();
    if (!units::LengthFromString(v, sys.length))
      refuse("length", v, units::LengthNames());
  }
  if (u.contains("mass")) {
    const std::string v = u.at("mass").get<std::string>();
    if (!units::MassFromString(v, sys.mass))
      refuse("mass", v, units::MassNames());
  }
  if (u.contains("angle")) {
    const std::string v = u.at("angle").get<std::string>();
    if (!units::AngleFromString(v, sys.angle))
      refuse("angle", v, units::AngleNames());
  }
  return sys;
}

// The sheet-metal rules off an envelope (v108). Absent = not a sheet part,
// which is what every document written before v108 means. An unknown relief
// style is REFUSED rather than read as rectangular: a relief that is quietly
// the wrong shape is a corner that cracks at the brake, and the document
// loads looking perfect.
std::optional<sheet::SheetMetalRules> SheetMetalFromEnvelope(const json& j) {
  if (!j.contains("sheet_metal"))
    return std::nullopt;
  const json m = j.at("sheet_metal");
  sheet::SheetMetalRules r;
  r.material = m.value("material", std::string{});
  r.gauge = m.value("gauge", std::string{});
  r.thickness = m.value("thickness", 0.0);
  r.bendRadius = m.value("bend_radius", 0.0);
  r.kFactor = m.value("k_factor", 0.0);
  if (m.contains("relief")) {
    const std::string v = m.at("relief").get<std::string>();
    if (!sheet::ReliefStyleFromString(v, r.reliefStyle))
      throw std::runtime_error(
          "unknown sheet-metal relief style '" + v +
          "' (expected one of: " + sheet::ReliefStyleList() + ")");
  }
  r.reliefWidth = m.value("relief_width", 0.0);
  r.reliefDepth = m.value("relief_depth", 0.0);
  if (const std::string why = sheet::Validate(r); !why.empty())
    throw std::runtime_error("sheet-metal rules: " + why);
  return r;
}

// Written only when the part IS sheet metal, so every document that is not
// re-saves byte-identically and the goldens stay valid. The derived fields
// (bend radius, K, relief size) are written only when overridden, for the
// same reason: a stored 1.5 that came from the table would silently stop
// tracking the table.
void SheetMetalToEnvelope(const std::optional<sheet::SheetMetalRules>& rules,
                          json& j) {
  if (!rules)
    return;
  json m = {{"material", rules->material},
            {"gauge", rules->gauge},
            {"thickness", rules->thickness}};
  if (rules->bendRadius > 0.0)
    m["bend_radius"] = rules->bendRadius;
  if (rules->kFactor > 0.0)
    m["k_factor"] = rules->kFactor;
  if (rules->reliefStyle != sheet::ReliefStyle::Rectangular)
    m["relief"] = sheet::ReliefStyleName(rules->reliefStyle);
  if (rules->reliefWidth > 0.0)
    m["relief_width"] = rules->reliefWidth;
  if (rules->reliefDepth > 0.0)
    m["relief_depth"] = rules->reliefDepth;
  j["sheet_metal"] = std::move(m);
}

// The tube shop, v119. Absent means the part is not bent from tube, which is
// what every document written before v119 means.
//
// It carries the MATERIAL and the BENDER, and deliberately no size: unlike a
// sheet part, which is cut from one blank, a tube frame is several sizes at
// once and each member's Pipe carries its own. That is the one place this
// parts company with `sheet_metal`.
std::optional<tube::TubeRules> TubeFromEnvelope(const json& j) {
  if (!j.contains("tube"))
    return std::nullopt;
  const json m = j.at("tube");
  tube::TubeRules r;
  r.material = m.value("material", std::string{});
  if (m.contains("former_radii")) {
    for (const auto& v : m.at("former_radii"))
      r.formerRadii.push_back(v.get<double>());
  }
  r.minStraight = m.value("min_straight", 0.0);
  r.tailLength = m.value("tail_length", 0.0);
  r.maxThinning = m.value("max_thinning", 0.0);
  if (const std::string why = tube::Validate(r); !why.empty())
    throw std::runtime_error("tube rules: " + why);
  return r;
}

// Written only when the part IS bent tube, so every document that is not
// re-saves byte-identically and the goldens stay valid. The derived fields
// are written only when overridden, for the same reason the sheet's are: a
// stored value that came from the stock would silently stop tracking it.
void TubeToEnvelope(const std::optional<tube::TubeRules>& rules, json& j) {
  if (!rules)
    return;
  json m = {{"material", rules->material}};
  if (!rules->formerRadii.empty())
    m["former_radii"] = rules->formerRadii;
  if (rules->minStraight > 0.0)
    m["min_straight"] = rules->minStraight;
  if (rules->tailLength > 0.0)
    m["tail_length"] = rules->tailLength;
  if (rules->maxThinning > 0.0)
    m["max_thinning"] = rules->maxThinning;
  j["tube"] = std::move(m);
}

// How the part is printed (v143). Refused on load when the table does not
// carry the pair, naming what it does: a document that says "sla" or "abs"
// was written by a build that had data this one lacks, and reading it as
// unprinted — or as PETG — would report the wrong thing to model.
//
// v169: a part printed with one of the user's print profiles carries the
// profile's name and an EMBEDDED copy of it -- `conditions`,
// `stl_deflection_mm` and `rows` -- the way a CAM setup embeds its machine
// (docs/tolerances/README.md#print-profiles). The copy is what the part
// reads, whichever store opens it; a copy the profile rules refuse is
// refused here too. v175: and the profile's `layer_height_mm` and
// `z_gap_mm` when it states them.
// v179: a tolerance's `printed`
// (docs/tolerances/README.md#printed-press-fits). Only "press" is a printed
// tolerance; anything else is refused, never read as an unprinted one.
bool TolerancePrintedFromJson(const json& tj) {
  if (!tj.contains("printed"))
    return false;
  const json& v = tj.at("printed");
  if (!v.is_string() || v.get<std::string>() != "press")
    throw std::runtime_error(
        "tolerance " + tj.value("ref", json::object()).dump() + ": printed " +
        v.dump() + " is not a printed tolerance; accepted: press");
  if (tj.contains("class") || tj.contains("lower") || tj.contains("upper"))
    throw std::runtime_error("a printed press tolerance carries no class and "
                             "no lower/upper: its band is the print "
                             "profile's");
  return true;
}

std::optional<printfit::Process> PrintFromEnvelope(const json& j) {
  if (!j.contains("print"))
    return std::nullopt;
  const json& m = j.at("print");
  printfit::Process p{m.value("process", std::string{}),
                      m.value("material", std::string{}), std::nullopt};
  if (m.contains("profile")) {
    if (!m.contains("rows"))
      throw std::runtime_error("print process: profile '" +
                               m.value("profile", std::string{}) +
                               "' carries no rows");
    json pj = m;
    pj["name"] = m.at("profile");
    pj.erase("profile");
    try {
      p.profile = DocumentIO::PrintProfileFromJson(pj);
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("print process: ") + e.what());
    }
  } else if (m.contains("rows")) {
    throw std::runtime_error(
        "print process: `rows` without a `profile` name — the built-in table "
        "is not embedded, and rows nobody named cannot be told apart from it");
  }
  if (const std::string why = printfit::Validate(p); !why.empty())
    throw std::runtime_error("print process: " + why);
  return p;
}

// Written only when the part IS printed, so every other document re-saves
// byte-identically and the goldens stay valid.
void PrintToEnvelope(const std::optional<printfit::Process>& p, json& j) {
  if (!p)
    return;
  json m{{"process", p->process}, {"material", p->material}};
  if (p->profile) {
    json pj = DocumentIO::PrintProfileToJson(*p->profile);
    m["profile"] = p->profile->name;
    for (const char* key :
         {"conditions", "stl_deflection_mm", "layer_height_mm", "z_gap_mm",
          "bed_mm", "bed_exclude_mm", "line_width_mm", "rows"})
      if (pj.contains(key))
        m[key] = pj.at(key);
  }
  j["print"] = std::move(m);
}

// The same three keys, written only when they differ from the default.
void UnitsToEnvelope(const units::System& u, json& j) {
  if (u.IsDefault())
    return;
  j["units"] = {{"length", units::ToString(u.length)},
                {"mass", units::ToString(u.mass)},
                {"angle", units::ToString(u.angle)}};
}

json DocumentEnvelope(const Document& doc) {
  json j;
  j["format"] = kFormat;
  if (!doc.Generator().is_null())
    j["generator"] = doc.Generator();
  if (!doc.GetCatalogs().empty())
    j["catalogs"] = catalog::Serialize(doc.GetCatalogs());
  // v150: project tables a single-part file carries (the project's, folded
  // in for the write like its variables).
  if (!doc.GetTables().empty())
    j["tables"] = lookuptable::ToJson(doc.GetTables());
  // v160: the project's requirements a single-part file carries
  // (docs/requirements/README.md#saved-form), folded in like the tables.
  if (doc.GetRequirementsRaw().is_array() && !doc.GetRequirementsRaw().empty())
    j["requirements"] = doc.GetRequirementsRaw();
  // v161: the template library a single-part file carries, the same way
  // (docs/modeling/templates.md#the-library).
  if (!doc.GetTemplates().empty())
    j["templates"] = tmpl::LibraryToJson(doc.GetTemplates());
  j["version"] = kVersion;
  if (doc.Rollback() >= 0) {
    j["rollback"] = doc.Rollback();
  }
  // Omitted at the default (true) so existing projects re-save byte-identically
  // and the goldens stay valid — the recipe format is untouched by baking.
  if (!doc.CommitGeometry()) {
    j["commit_geometry"] = false;
  }
  if (!doc.GetParameters().empty()) {
    j["parameters"] = json::array();
    for (const Parameter& p : doc.GetParameters()) {
      json pj = {{"name", p.name}, {"expression", p.expression}};
      if (!p.comment.empty()) {
        pj["comment"] = p.comment;
      }
      j["parameters"].push_back(std::move(pj));
    }
  }
  // Tolerances (v98). Omitted when empty so every existing document re-saves
  // byte-identically and the goldens stay valid.
  if (!doc.GetTolerances().empty()) {
    j["tolerances"] = json::array();
    for (const Tolerance& t : doc.GetTolerances()) {
      json tj = {{"id", t.id}, {"ref", ElementRefJson(t.ref)}};
      // Present only for a distance tolerance, so a size one is written
      // exactly as v98 wrote it.
      if (t.IsDistance())
        tj["ref2"] = ElementRefJson(t.ref2);
      // The class is what is stored; a deviation pair is the fallback for a
      // drawing that states limits no class expresses. Writing both would
      // invite them to disagree.
      if (t.printedPress) {
        tj["printed"] = "press"; // v179: the band is the print profile's
      } else if (!t.isoClass.empty()) {
        tj["class"] = t.isoClass;
      } else {
        tj["lower"] = t.lower;
        tj["upper"] = t.upper;
      }
      if (!t.comment.empty()) {
        tj["comment"] = t.comment;
      }
      j["tolerances"].push_back(std::move(tj));
    }
  }
  // The active configuration (v31), omitted at base (0) so pre-v31 documents
  // re-save byte-identically. The configurations themselves are written per
  // save path (inline / configurations/ dir), like studies.
  if (doc.ActiveConfiguration() != 0) {
    j["active_configuration"] = doc.ActiveConfiguration();
  }
  // Display units (v64), omitted at the default (mm/kg/deg) for the same
  // reason: every document written before this re-saves byte-identically.
  // Presentation only — storage is mm/g/deg whatever this says.
  UnitsToEnvelope(doc.Units(), j);
  // Sheet-metal rules (v108), omitted when the part is not sheet metal.
  SheetMetalToEnvelope(doc.SheetMetal(), j);
  TubeToEnvelope(doc.Tube(), j);
  PrintToEnvelope(doc.PrintProcess(), j); // v143
  // v181: the slicer settings, only when a part states some.
  if (!doc.PrintSettings().Empty())
    j["print_settings"] =
        DocumentIO::PartPrintSettingsToJson(doc.PrintSettings());
  return j;
}

// ---- Assemblies (A2) ------------------------------------------------------
// An assembly shares the document format string + version, discriminated by
// `kind: "assembly"`. OccurrenceToJson/FromJson are public (defined near
// FeatureToJson); these are the internal file-layout helpers. See
// docs/assemblies/README.md#the-assembly-document.

json AssemblyEnvelope(const AssemblyDocument& asmDoc) {
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  j["kind"] = "assembly";
  UnitsToEnvelope(asmDoc.Units(), j); // v64
  return j;
}

// A filesystem-safe, lowercase [a-z0-9-] slug for an occurrence's or mate's
// cosmetic label (user-set, unlike a feature's PascalCase TypeName). The
// stable id is the real identity — files load by the numeric prefix — so a
// lossy slug is fine; this only keeps the filename readable and safe.
std::string LabelSlug(const std::string& label) {
  std::string out;
  for (char c : label) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc))
      out += static_cast<char>(std::tolower(uc));
    else if (!out.empty() && out.back() != '-')
      out += '-';
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out;
}

// Restores occurrences into `asm` in the given order and pins the id counter
// past the highest restored id so future adds never collide.
void LoadOccurrences(AssemblyDocument& asmDoc,
                     const std::vector<json>& occJsons) {
  uint64_t maxId = 0;
  for (const json& oj : occJsons) {
    Occurrence o = OccurrenceFromJson(oj);
    maxId = std::max(maxId, o.id);
    asmDoc.GetOccurrences().push_back(std::move(o));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// Restores mates into `asm` in the given order and pins the id counter past
// the highest restored id. Mates share the id space with occurrences, so
// pinning here (after LoadOccurrences) leaves the counter above both.
void LoadMates(AssemblyDocument& asmDoc, const std::vector<json>& mateJsons) {
  uint64_t maxId = 0;
  for (const json& mj : mateJsons) {
    Mate m = MateFromJson(mj);
    maxId = std::max(maxId, m.id);
    asmDoc.GetMates().push_back(std::move(m));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// Exploded views (v27) follow the same restore-and-pin discipline.
void LoadExplodedViews(AssemblyDocument& asmDoc,
                       const std::vector<json>& viewJsons) {
  uint64_t maxId = 0;
  for (const json& vj : viewJsons) {
    ExplodedView v = ExplodedViewFromJson(vj);
    maxId = std::max(maxId, v.id);
    asmDoc.GetExplodedViews().push_back(std::move(v));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// Relations (v83) follow the same restore-and-pin discipline.
void LoadRelations(AssemblyDocument& asmDoc,
                   const std::vector<json>& relJsons) {
  uint64_t maxId = 0;
  for (const json& rj : relJsons) {
    Relation r = RelationFromJson(rj);
    maxId = std::max(maxId, r.id);
    asmDoc.GetRelations().push_back(std::move(r));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// Clearance assertions (v97): the same again.
void LoadAssertions(AssemblyDocument& asmDoc, const std::vector<json>& jsons) {
  uint64_t maxId = 0;
  for (const json& aj : jsons) {
    ClearanceAssertion a = ClearanceAssertionFromJson(aj);
    maxId = std::max(maxId, a.id);
    asmDoc.GetAssertions().push_back(std::move(a));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// Motion studies (v87): the same again.
void LoadMotionStudies(AssemblyDocument& asmDoc,
                       const std::vector<json>& jsons) {
  uint64_t maxId = 0;
  for (const json& sj : jsons) {
    MotionStudy s = MotionStudyFromJson(sj);
    maxId = std::max(maxId, s.id);
    asmDoc.GetMotionStudies().push_back(std::move(s));
  }
  asmDoc.EnsureNextIdAtLeast(maxId + 1);
}

// FEM studies on an assembly (v106): the same again, through RestoreStudy
// so a load's id also raises the counter.
void LoadAssemblyStudies(AssemblyDocument& asmDoc,
                         const std::vector<json>& jsons) {
  for (const json& sj : jsons)
    asmDoc.RestoreStudy(StudyFromJson(sj));
}

// Binary sibling of WriteTextAtomic for reference-image pixel files (v49):
// same temp+rename + identical-content skip, but byte-exact (no text-mode
// translation).
bool WriteBytesAtomic(const std::filesystem::path& target,
                      const std::vector<unsigned char>& bytes,
                      std::string* error) {
  if (!AtomicTargetOk(target, error))
    return false;
  {
    // Read the file only when its size could make it the same bytes.
    std::error_code sizeEc;
    const auto size = std::filesystem::file_size(target, sizeEc);
    // (Compared as bytes: a char and an unsigned char above 0x7f never
    // compare equal, which rewrote every unchanged binary file.)
    std::ifstream existing;
    if (!sizeEc && size == bytes.size())
      existing.open(target, std::ios::binary);
    if (existing.is_open()) {
      std::string current((std::istreambuf_iterator<char>(existing)),
                          std::istreambuf_iterator<char>());
      if (current.size() == bytes.size() &&
          (bytes.empty() ||
           std::memcmp(current.data(), bytes.data(), bytes.size()) == 0))
        return true;
    }
  }
  const std::filesystem::path tmp = target.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + tmp.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }
  std::error_code ec;
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    if (error)
      *error = "rename failed: " + ec.message();
    std::filesystem::remove(tmp, ec); // never leave the half behind
    return false;
  }
  return true;
}

// ---- Reference-image (v49) asset transport --------------------------------
// Pixel bytes live outside the feature JSON so it stays small and diffable: an
// assets/<name> file in the directory format, a base64 blob under a document-
// root "assets" map in the single-file form. FeatureToJson/FromJson carry only
// the metadata + `asset` name; these helpers move the bytes.

std::string Base64Encode(const std::vector<unsigned char>& in) {
  static constexpr char kTab[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= in.size(); i += 3) {
    const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) |
                       uint32_t(in[i + 2]);
    out.push_back(kTab[(n >> 18) & 63]);
    out.push_back(kTab[(n >> 12) & 63]);
    out.push_back(kTab[(n >> 6) & 63]);
    out.push_back(kTab[n & 63]);
  }
  if (i + 1 == in.size()) {
    const uint32_t n = uint32_t(in[i]) << 16;
    out.push_back(kTab[(n >> 18) & 63]);
    out.push_back(kTab[(n >> 12) & 63]);
    out.push_back('=');
    out.push_back('=');
  } else if (i + 2 == in.size()) {
    const uint32_t n = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
    out.push_back(kTab[(n >> 18) & 63]);
    out.push_back(kTab[(n >> 12) & 63]);
    out.push_back(kTab[(n >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::vector<unsigned char> Base64Decode(const std::string& in) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
    return -1; // '=' padding and any whitespace are skipped
  };
  std::vector<unsigned char> out;
  out.reserve(in.size() / 4 * 3);
  int buf = 0, bits = 0;
  for (char c : in) {
    const int v = val(c);
    if (v < 0)
      continue;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((buf >> bits) & 0xFF));
    }
  }
  return out;
}

// Asset names actually referenced by a reference image somewhere in the doc —
// so serialization persists only live assets and prunes orphans.
std::set<std::string> ReferencedAssets(const Document& doc) {
  std::set<std::string> names;
  for (const auto& f : doc.GetFeatures()) {
    if (auto* sk = dynamic_cast<const SketchFeature*>(f.get()))
      for (const auto& img : sk->images)
        if (!img.asset.empty())
          names.insert(img.asset);
    // A Derived feature's snapshot B-Reps (v84) live in the same store.
    if (auto* d = dynamic_cast<const DerivedFeature*>(f.get()))
      for (const DerivedBody& b : d->snapshot)
        names.insert(b.asset);
  }
  return names;
}

// Directory format (docs/versioning/format.md#the-directory-form): one file
// per feature named by stable id, an ordered index written LAST (a torn
// save leaves a stale-but-loadable index), document-level parameters file.
namespace {

// The id IS the filename and the index entry, so an item with id 0 writes
// "0-box.json" and lists 0 — which the loader then reads back as "no id", and
// which every reference naming the item's real id fails to resolve. Every id
// space starts at 1, so 0 means something upstream lost it.
//
// A tripwire, not an expected failure: ids are assigned at every entry point
// (Document::InsertFeature assigns one, ReplaceFeature inherits the old one).
// It exists because that second guarantee was MISSING until F7 found it — the
// modify_feature RPC sends parameters with no "id", and the replacement
// landed with 0 and wrote exactly that file. Refusing is consistent with how
// this loader already treats an inconsistent project: say what is wrong
// rather than write something that reads back differently.
uint64_t IdForFile(uint64_t id, const char* what, const std::string& name) {
  if (id == 0) {
    throw std::runtime_error(
        std::string("cannot save: ") + what + " \"" + name +
        "\" has no id, so it would be written as \"0-…\" and read back as a "
        "different thing — this is a bug, please report it");
  }
  return id;
}

// A study or motion result beside its study (v154): `<base>.result.bin`
// holds the arrays the json carries as a binary value (kResultData), and
// `<base>.result.json` the rest. The bin goes first, so a save torn between
// the two leaves a json whose `bin_bytes` disagrees with its bin, which the
// load drops (ReadResultFiles). Either file whose bytes are the ones on disk
// is not written: an unchanged result keeps its mtime.
bool WriteResultFiles(const std::filesystem::path& dir, const std::string& base,
                      json j, std::set<std::string>& wanted, std::string* error,
                      const char* suffix = ".result") {
  std::vector<unsigned char> bytes;
  if (j.contains(kResultData)) {
    bytes = std::move(j.at(kResultData).get_binary());
    j.erase(kResultData);
  }
  const std::string bin = base + suffix + ".bin";
  const std::string text = base + suffix + ".json";
  if (!WriteBytesAtomic(dir / bin, bytes, error) ||
      !WriteTextAtomic(dir / text, j.dump() + "\n", error))
    return false;
  wanted.insert(bin);
  wanted.insert(text);
  return true;
}

// Removes what a save no longer writes from a collection directory: the
// study (.json) and result (.json, .bin) files not in `wanted`.
void PruneCollection(const std::filesystem::path& dir,
                     const std::set<std::string>& wanted) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const auto ext = entry.path().extension();
    if ((ext == ".json" || ext == ".bin") &&
        !wanted.contains(entry.path().filename().string()))
      std::filesystem::remove(entry.path(), ec);
  }
}

} // namespace

bool SaveDir(const Document& doc, const std::string& dirPath,
             std::string* error) {
  namespace fs = std::filesystem;
  try {
    const fs::path dir(dirPath);
    const fs::path featuresDir = dir / "features";
    fs::create_directories(featuresDir);

    json index = DocumentEnvelope(doc);
    index["features"] = json::array();
    std::set<std::string> wanted;
    for (const auto& feature : doc.GetFeatures()) {
      const std::string file =
          std::to_string(
              IdForFile(feature->GetId(), "feature", feature->GetName())) +
          "-" + KebabType(feature->TypeName()) + ".json";
      if (!WriteTextAtomic(featuresDir / file,
                           FeatureToJson(*feature).dump(2) + "\n", error))
        return false;
      index["features"].push_back(feature->GetId());
      wanted.insert(file);
    }
    // Deleted features leave no stale files behind.
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(featuresDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wanted.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }
    // Baked geometry (docs/versioning/README.md#baked-geometry): the built
    // bodies beside the recipe, so the next open is a load. Best-effort — a
    // project that cannot bake is still a valid project, just a slower one to
    // open.
    if (doc.CommitGeometry()) {
      std::string bakeError;
      if (!SaveGeometry(doc, dirPath, &bakeError))
        LOG_WARN("Could not bake geometry: {}", bakeError);
    } else {
      RemoveGeometry(dirPath);
    }
    // The app creates and commits the project's repo itself (GitVault), so it
    // is the app's job to say how the blob should be treated there.
    WriteRepositoryHints(dirPath, doc.CommitGeometry());
    // FEM studies (v29): file-per-study under studies/, id list in the index.
    // Both the key and the directory stay absent when there are no studies, so
    // a pre-v29 document re-saves byte-identically (the exploded-views v27
    // discipline). Each result is written beside its study (topology v33,
    // static and modal v153), with the input hash that produced it.
    if (!doc.GetStudies().empty()) {
      const fs::path studyDir = dir / "studies";
      fs::create_directories(studyDir);
      index["studies"] = json::array();
      std::set<std::string> wantedStudies;
      for (const fem::Study& s : doc.GetStudies()) {
        const std::string base =
            std::to_string(IdForFile(s.id, "study", s.name)) + "-" +
            LabelSlug(s.name);
        const std::string file = base + ".json";
        if (!WriteTextAtomic(studyDir / file, StudyToJson(s).dump(2) + "\n",
                             error))
          return false;
        index["studies"].push_back(s.id);
        wantedStudies.insert(file);
        // Its result (topology v33, every kind v153).
        const fem::StudyRuntime* rt = doc.StudyResults(s.id);
        json resultJson;
        if (rt && StudyResultToJson(*rt, resultJson) &&
            !WriteResultFiles(studyDir, base, std::move(resultJson),
                              wantedStudies, error))
          return false;
      }
      PruneCollection(studyDir, wantedStudies);
    }
    // Configurations (v31): file-per-config under configurations/, id list in
    // the index (the active id is in the envelope). Absent when none, so a
    // pre-v31 document re-saves byte-identically.
    if (!doc.GetConfigurations().empty()) {
      const fs::path cfgDir = dir / "configurations";
      fs::create_directories(cfgDir);
      index["configurations"] = json::array();
      std::set<std::string> wantedConfigs;
      for (const Configuration& c : doc.GetConfigurations()) {
        const std::string file =
            std::to_string(IdForFile(c.id, "configuration", c.name)) + "-" +
            LabelSlug(c.name) + ".json";
        if (!WriteTextAtomic(cfgDir / file,
                             ConfigurationToJson(c).dump(2) + "\n", error))
          return false;
        index["configurations"].push_back(c.id);
        wantedConfigs.insert(file);
      }
      for (const auto& entry : fs::directory_iterator(cfgDir, ec)) {
        if (entry.path().extension() == ".json" &&
            !wantedConfigs.contains(entry.path().filename().string()))
          fs::remove(entry.path(), ec);
      }
    }
    // Reference-image pixel files (v49) under assets/, pruned like features/.
    // The dir stays absent when there are no images, so a pre-v49 document
    // re-saves byte-identically.
    {
      const std::set<std::string> wantedAssets = ReferencedAssets(doc);
      if (!wantedAssets.empty()) {
        const fs::path assetDir = dir / "assets";
        fs::create_directories(assetDir);
        for (const std::string& name : wantedAssets) {
          const std::vector<unsigned char>* bytes = doc.GetAsset(name);
          if (!bytes || bytes->empty())
            continue;
          if (!WriteBytesAtomic(assetDir / name, *bytes, error))
            return false;
        }
        for (const auto& entry : fs::directory_iterator(assetDir, ec))
          if (!wantedAssets.contains(entry.path().filename().string()))
            fs::remove(entry.path(), ec);
      }
    }
    // Parameters live in the index envelope (small, diff-friendly there).
    if (!WriteTextAtomic(dir / "index.json", index.dump(2) + "\n", error))
      return false;
    LOG_INFO("Saved document ({} features) to {}", doc.FeatureCount(), dirPath);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

namespace {

// "features/3-sketch.json", for messages: the bare filename is ambiguous
// across features/, studies/, views/… and the absolute path is noise.
std::string ProjectRelative(const std::filesystem::path& path) {
  return (path.parent_path().filename() / path.filename()).string();
}

// Reads and parses one project file, saying WHICH file when it cannot.
//
// nlohmann reports a line and column and no filename, and in a project that
// is a git repository the usual cause is not a typo: a merge left conflict
// markers in the file. The directory format merges well precisely so people
// WILL merge these projects (one file per feature, one key per line), which
// makes "unresolved markers" a first-class failure worth naming.
json ParseProjectFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot read " + ProjectRelative(path));
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  if (text.starts_with("<<<<<<<") ||
      text.find("\n<<<<<<<") != std::string::npos) {
    throw std::runtime_error(
        ProjectRelative(path) +
        " still has merge conflict markers — finish the merge (git status "
        "shows what is unresolved), then open the project again");
  }
  try {
    return json::parse(text);
  } catch (const std::exception& e) {
    throw std::runtime_error(ProjectRelative(path) + ": " + e.what());
  }
}

// A result beside its study (v154): the json, with its .result.bin read
// back in as the binary value it was written from (WriteResultFiles). A v153
// result holds its arrays inline and has no bin. nullopt, with a warning,
// when the bin is missing or not the size the json says: a result is
// derived, and one whose halves disagree (a torn save, a copy of one file)
// is dropped and run again rather than read.
std::optional<json> ReadResultFiles(const std::filesystem::path& path) {
  json j = ParseProjectFile(path);
  if (!j.contains("bin_bytes"))
    return j;
  const size_t want = j.at("bin_bytes").get<size_t>();
  std::filesystem::path bin = path;
  bin.replace_extension(".bin");
  std::ifstream in(bin, std::ios::binary);
  std::vector<unsigned char> bytes;
  if (in)
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
  if (!in.is_open() || bytes.size() != want) {
    LOG_WARN("Dropping the saved result {}: its {} is {} (expected {} "
             "bytes); the study runs again",
             ProjectRelative(path), bin.filename().string(),
             in.is_open() ? std::to_string(bytes.size()) + " bytes"
                          : std::string("missing"),
             want);
    return std::nullopt;
  }
  j[kResultData] = json::binary(std::move(bytes));
  return j;
}

// Records `path` under the id its filename starts with, REFUSING a second
// file that claims the same id.
//
// This used to be `byId[std::stoull(name)] = path`, which silently kept the
// last one. Two files never share an id when the app writes them — but a
// merge makes it easily: git takes both "15-box.json" and "15-hole.json"
// without a word when two people each added a feature offline and the id
// counter handed them both 15. The overwrite then dropped one person's work
// and left the document with two entries claiming one id, which every
// reference (sketch_id, body ids, mates, configuration overrides) then
// resolves ambiguously. Loudly refusing is the only safe answer; the fix in
// the file is to renumber one of them.
void InsertIdFile(std::map<uint64_t, std::filesystem::path>& byId,
                  const std::filesystem::path& path, const char* what) {
  const uint64_t id = std::stoull(path.filename().string());
  const auto [it, inserted] = byId.emplace(id, path);
  if (!inserted) {
    throw std::runtime_error(
        "two " + std::string(what) + " files claim id " + std::to_string(id) +
        " (" + ProjectRelative(it->second) + " and " + ProjectRelative(path) +
        ") — a merge kept both; renumber one of them (its \"id\" field and "
        "its filename) and reopen");
  }
}

// A study's result beside it: "<id>-<slug>.result.json".
bool IsResultFile(const std::filesystem::path& path) {
  const std::string stem = path.stem().string(); // strips ".json"
  return stem.size() > 7 && stem.compare(stem.size() - 7, 7, ".result") == 0;
}

// A drawing view's saved projection beside it (v162):
// "<id>-<slug>.projection.json".
bool IsProjectionFile(const std::filesystem::path& path) {
  const std::string stem = path.stem().string(); // strips ".json"
  return stem.size() > 11 &&
         stem.compare(stem.size() - 11, 11, ".projection") == 0;
}

// The index lists the ids, in order. A duplicate there means the same file
// loads twice — two features, one id — which is the other half of the merge
// hazard above and just as ambiguous downstream.
void CheckIndexIdsUnique(const json& ids, const char* what) {
  std::set<uint64_t> seen;
  for (const auto& idJson : ids) {
    const uint64_t id = idJson.get<uint64_t>();
    if (!seen.insert(id).second) {
      throw std::runtime_error(
          "index.json lists " + std::string(what) + " id " +
          std::to_string(id) +
          " twice — a merge kept both sides; keep one and reopen");
    }
  }
}

} // namespace

std::unique_ptr<Document> LoadDir(const std::string& dirPath,
                                  std::string* error,
                                  const ProjectScope& scope) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error)
        *error = "Not a Lobole project directory: " + dirPath;
      return nullptr;
    }
    json index = ParseProjectFile(fs::path(dirPath) / "index.json");
    if (index.value("format", "") != kFormat) {
      if (error)
        *error = "Not a Lobole document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "part") {
      if (error)
        *error = "This is a " + index.value("kind", "part") +
                 ", not a part: " + dirPath;
      return nullptr;
    }

    auto doc = std::make_unique<Document>();
    doc->SetProjectSymbols(scope.symbols);
    doc->SetProjectParameters(scope.parameters);
    doc->SetCommitGeometry(index.value("commit_geometry", true));
    doc->SetCatalogsRaw(index.value("catalogs", json::array()));
    doc->SetTablesRaw(lookuptable::ListFromJson(index.value("tables", json())));
    // Refused, not dropped, when one names a measure this build does not
    // know: the requirement would load as something it is not.
    doc->SetRequirementsRaw(req::ListToJson(
        req::ListFromJson(index.value("requirements", json()))));
    doc->SetTemplatesRaw(
        tmpl::LibraryFromJson(index.value("templates", json())));
    doc->SetGeneratorRaw(index.value("generator", json(nullptr)));
    for (const auto& pj : index.value("parameters", json::array())) {
      doc->SetParameter(pj.at("name").get<std::string>(),
                        pj.at("expression").get<std::string>(),
                        pj.value("comment", std::string{}));
    }
    // Tolerances (v98). They reference elements of features, but nothing
    // about them is evaluated at load: a tolerance is design intent about a
    // face, so a ref that no longer resolves stays recorded and is reported
    // as unresolved when something LOOKS, rather than being dropped here.
    for (const auto& tj : index.value("tolerances", json::array())) {
      Tolerance t;
      t.id = tj.value("id", uint64_t{0});
      t.ref = ElementRefFromJson(tj.value("ref", nlohmann::json::object()));
      t.ref2 = ElementRefFromJson(tj.value("ref2", nlohmann::json::object()));
      t.isoClass = tj.value("class", std::string{});
      t.lower = tj.value("lower", 0.0);
      t.upper = tj.value("upper", 0.0);
      t.printedPress = TolerancePrintedFromJson(tj);
      t.comment = tj.value("comment", std::string{});
      doc->RestoreTolerance(std::move(t));
    }
    // Sheet-metal rules (v108) BEFORE the features: a flange reads the
    // gauge during Execute, and AddFeature below regenerates.
    doc->SetSheetMetal(SheetMetalFromEnvelope(index));
    doc->SetTube(TubeFromEnvelope(index));
    doc->SetPrintProcess(PrintFromEnvelope(index));
    if (index.contains("print_settings")) // v181
      doc->SetPrintSettings(
          DocumentIO::PartPrintSettingsFromJson(index.at("print_settings")));
    // Feature files are found by their id prefix — the type suffix is
    // cosmetic and may lag a (hypothetical) rename.
    std::map<uint64_t, fs::path> byId;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "features", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      InsertIdFile(byId, entry.path(), "feature");
    }
    // One regeneration for the whole file, not one per feature (Document::
    // BulkEdit). The guard is released explicitly below — BEFORE the topology
    // results are stamped, since they must carry the settled version.
    auto bulk = std::make_optional<Document::BulkEdit>(*doc);
    CheckIndexIdsUnique(index.at("features"), "feature");
    for (const auto& idJson : index.at("features")) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = byId.find(id);
      if (it == byId.end()) {
        if (error)
          *error = "missing feature file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      doc->AddFeature(FeatureFromJson(ParseProjectFile(it->second)));
    }
    // Reference-image bytes (v49) from assets/ into the doc store (absent dir ⇒
    // nothing to do). Only assets an image references are loaded.
    for (const std::string& name : ReferencedAssets(*doc)) {
      std::ifstream ain(fs::path(dirPath) / "assets" / name, std::ios::binary);
      if (ain) {
        std::string data((std::istreambuf_iterator<char>(ain)),
                         std::istreambuf_iterator<char>());
        doc->SetAsset(name,
                      std::vector<unsigned char>(data.begin(), data.end()));
      }
    }
    // Study results (v33, v153) loaded below but stored after the final
    // regen.
    std::vector<std::pair<uint64_t, fem::StudyRuntime>> pendingResults;
    // FEM studies (v29): id-prefix files under studies/, index order. A pre-v29
    // document has no `studies` key ⇒ the loop is empty.
    {
      std::map<uint64_t, fs::path> studyById, resultById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "studies", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        // A result file is "<id>-<slug>.result.json": route it to the
        // result map, not the study-def map.
        if (IsResultFile(entry.path()))
          InsertIdFile(resultById, entry.path(), "study result");
        else
          InsertIdFile(studyById, entry.path(), "study");
      }
      CheckIndexIdsUnique(index.value("studies", json::array()), "study");
      for (const auto& idJson : index.value("studies", json::array())) {
        const uint64_t id = idJson.get<uint64_t>();
        const auto it = studyById.find(id);
        if (it == studyById.end()) {
          if (error)
            *error = "missing study file for id " + std::to_string(id);
          return nullptr;
        }
        std::ifstream sin(it->second);
        doc->RestoreStudy(StudyFromJson(ParseProjectFile(it->second)));
        // A persisted result loads into the transient runtime cache.
        // Defer storing it until after the final regeneration below so it can
        // be stamped with the settled document version (else the rollback /
        // active-config regen would leave it spuriously stale).
        const auto rit = resultById.find(id);
        if (rit != resultById.end())
          if (auto rj = ReadResultFiles(rit->second))
            pendingResults.emplace_back(id, StudyResultFromJson(*rj));
      }
    }
    // Configurations (v31): id-prefix files under configurations/, index order.
    {
      std::map<uint64_t, fs::path> cfgById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "configurations", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        InsertIdFile(cfgById, entry.path(), "configuration");
      }
      CheckIndexIdsUnique(index.value("configurations", json::array()),
                          "configuration");
      for (const auto& idJson : index.value("configurations", json::array())) {
        const auto it = cfgById.find(idJson.get<uint64_t>());
        if (it == cfgById.end()) {
          if (error)
            *error = "missing configuration file for id " +
                     std::to_string(idJson.get<uint64_t>());
          return nullptr;
        }
        std::ifstream cin(it->second);
        doc->RestoreConfiguration(
            ConfigurationFromJson(ParseProjectFile(it->second)));
      }
    }
    doc->SetRollback(index.value("rollback", -1));
    // Apply the active configuration last so the final regeneration reflects
    // it.
    doc->SetActiveConfiguration(
        index.value("active_configuration", uint64_t{0}));
    doc->SetUnits(UnitsFromEnvelope(index)); // v64
    // Adopt the baked bodies BEFORE the regeneration below: it is that
    // regeneration which decides whether they still describe the recipe.
    LoadGeometry(*doc, dirPath);
    bulk.reset(); // the one regeneration for everything loaded above
    // Now that regeneration has settled, stamp the persisted results with
    // the final version so they read as current (v33).
    // A result whose saved input hash no longer matches what the reopened
    // part would hand a run was saved stale, or the part moved under it: it
    // reads stale, and is not reused (v148). All are stored before any is
    // checked: a shape study's inputs include its topology result's.
    std::vector<std::pair<uint64_t, uint64_t>> hashed;
    for (auto& [id, rt] : pendingResults) {
      rt.solvedAtVersion = doc->Version();
      if (rt.inputHash != 0)
        hashed.emplace_back(id, rt.inputHash);
      doc->StoreStudyResults(id, std::move(rt));
    }
    for (const auto& [id, hash] : hashed) {
      const fem::Study* st = doc->FindStudy(id);
      if (st == nullptr || fem::StudyInputKey(*doc, *st) != hash) {
        fem::StudyRuntime rt = *doc->StudyResults(id);
        rt.solvedAtVersion = UINT64_MAX; // no version reaches it
        doc->StoreStudyResults(id, std::move(rt));
      }
    }
    LOG_INFO("Loaded document ({} features) from {}", doc->FeatureCount(),
             dirPath);
    return doc;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load project directory {}: {}", dirPath, e.what());
    return nullptr;
  }
}

// Assembly directory format (mirrors SaveDir): one file per occurrence named
// by its stable id, an ordered index written LAST, `kind: "assembly"`.
bool SaveAssemblyDir(const AssemblyDocument& asmDoc, const std::string& dirPath,
                     std::string* error) {
  namespace fs = std::filesystem;
  try {
    const fs::path dir(dirPath);
    const fs::path occDir = dir / "occurrences";
    fs::create_directories(occDir);

    json index = AssemblyEnvelope(asmDoc);
    index["occurrences"] = json::array();
    std::set<std::string> wantedOcc;
    for (const Occurrence& o : asmDoc.GetOccurrences()) {
      const std::string file =
          std::to_string(IdForFile(o.id, "occurrence", o.name)) + "-" +
          LabelSlug(o.name) + ".json";
      if (!WriteTextAtomic(occDir / file, OccurrenceToJson(o).dump(2) + "\n",
                           error))
        return false;
      index["occurrences"].push_back(o.id);
      wantedOcc.insert(file);
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(occDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedOcc.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    // Mates mirror the occurrence block: one file per mate under mates/, the
    // ordered id list in the same index.json (written last). A v23 assembly
    // has none — the mates/ dir simply stays empty.
    const fs::path mateDir = dir / "mates";
    index["mates"] = json::array();
    if (!asmDoc.GetMates().empty())
      fs::create_directories(mateDir);
    std::set<std::string> wantedMate;
    for (const Mate& m : asmDoc.GetMates()) {
      const std::string file = std::to_string(IdForFile(m.id, "mate", m.name)) +
                               "-" + LabelSlug(m.name) + ".json";
      if (!WriteTextAtomic(mateDir / file, MateToJson(m).dump(2) + "\n", error))
        return false;
      index["mates"].push_back(m.id);
      wantedMate.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(mateDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedMate.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    // Exploded views (v27) mirror mates: file-per-view under exploded/, the
    // id list in the index. Both the key and the dir are absent when there
    // are none, so a v26 assembly stays byte-identical.
    const fs::path viewDir = dir / "exploded";
    if (!asmDoc.GetExplodedViews().empty()) {
      index["exploded"] = json::array();
      fs::create_directories(viewDir);
    }
    std::set<std::string> wantedView;
    for (const ExplodedView& v : asmDoc.GetExplodedViews()) {
      const std::string file = std::to_string(IdForFile(v.id, "view", v.name)) +
                               "-" + LabelSlug(v.name) + ".json";
      if (!WriteTextAtomic(viewDir / file, ExplodedViewToJson(v).dump(2) + "\n",
                           error))
        return false;
      index["exploded"].push_back(v.id);
      wantedView.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(viewDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedView.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    // Relations (v83) mirror exploded views: file-per-relation under
    // relations/, the id list in the index; both absent when there are none.
    const fs::path relDir = dir / "relations";
    if (!asmDoc.GetRelations().empty()) {
      index["relations"] = json::array();
      fs::create_directories(relDir);
    }
    std::set<std::string> wantedRel;
    for (const Relation& r : asmDoc.GetRelations()) {
      const std::string file =
          std::to_string(IdForFile(r.id, "relation", r.name)) + "-" +
          LabelSlug(r.name) + ".json";
      if (!WriteTextAtomic(relDir / file, RelationToJson(r).dump(2) + "\n",
                           error))
        return false;
      index["relations"].push_back(r.id);
      wantedRel.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(relDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedRel.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    // Clearance assertions (v97) mirror relations: file-per-assertion under
    // assertions/, the id list in the index; both absent when there are none.
    const fs::path assertDir = dir / "assertions";
    if (!asmDoc.GetAssertions().empty()) {
      index["assertions"] = json::array();
      fs::create_directories(assertDir);
    }
    std::set<std::string> wantedAssert;
    for (const ClearanceAssertion& a : asmDoc.GetAssertions()) {
      const std::string file =
          std::to_string(IdForFile(a.id, "assertion", a.name)) + "-" +
          LabelSlug(a.name) + ".json";
      if (!WriteTextAtomic(assertDir / file,
                           ClearanceAssertionToJson(a).dump(2) + "\n", error))
        return false;
      index["assertions"].push_back(a.id);
      wantedAssert.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(assertDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedAssert.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    // Motion studies (v87) mirror relations: file-per-study under motion/.
    const fs::path motionDir = dir / "motion";
    if (!asmDoc.GetMotionStudies().empty()) {
      index["motion"] = json::array();
      fs::create_directories(motionDir);
    }
    std::set<std::string> wantedMotion;
    for (const MotionStudy& s : asmDoc.GetMotionStudies()) {
      const std::string file =
          std::to_string(IdForFile(s.id, "motion", s.name)) + "-" +
          LabelSlug(s.name) + ".json";
      if (!WriteTextAtomic(motionDir / file,
                           MotionStudyToJson(s).dump(2) + "\n", error))
        return false;
      index["motion"].push_back(s.id);
      wantedMotion.insert(file);
      // Its result (v153), with the input hash that produced it.
      if (const auto result = asmDoc.MotionResult(s.id);
          result &&
          !WriteResultFiles(motionDir, file.substr(0, file.size() - 5),
                            MotionResultToJson(*result), wantedMotion, error))
        return false;
    }
    PruneCollection(motionDir, wantedMotion);

    // FEM studies (v106) mirror the part's: file-per-study under studies/,
    // absent when none, each result beside its study (v153).
    const fs::path asmStudyDir = dir / "studies";
    if (!asmDoc.GetStudies().empty()) {
      index["studies"] = json::array();
      fs::create_directories(asmStudyDir);
    }
    std::set<std::string> wantedStudies;
    for (const fem::Study& s : asmDoc.GetStudies()) {
      const std::string file =
          std::to_string(IdForFile(s.id, "study", s.name)) + "-" +
          LabelSlug(s.name) + ".json";
      if (!WriteTextAtomic(asmStudyDir / file, StudyToJson(s).dump(2) + "\n",
                           error))
        return false;
      index["studies"].push_back(s.id);
      wantedStudies.insert(file);
      json resultJson;
      if (const fem::StudyRuntime* rt = asmDoc.StudyResults(s.id);
          rt != nullptr && StudyResultToJson(*rt, resultJson) &&
          !WriteResultFiles(asmStudyDir, file.substr(0, file.size() - 5),
                            std::move(resultJson), wantedStudies, error))
        return false;
    }
    PruneCollection(asmStudyDir, wantedStudies);

    // Configurations (v31): file-per-config + active id in the index; absent
    // when none so older assemblies stay byte-identical.
    if (asmDoc.ActiveConfiguration() != 0)
      index["active_configuration"] = asmDoc.ActiveConfiguration();
    const fs::path cfgDir = dir / "configurations";
    if (!asmDoc.GetConfigurations().empty()) {
      index["configurations"] = json::array();
      fs::create_directories(cfgDir);
    }
    std::set<std::string> wantedCfg;
    for (const Configuration& c : asmDoc.GetConfigurations()) {
      const std::string file =
          std::to_string(IdForFile(c.id, "configuration", c.name)) + "-" +
          LabelSlug(c.name) + ".json";
      if (!WriteTextAtomic(cfgDir / file, ConfigurationToJson(c).dump(2) + "\n",
                           error))
        return false;
      index["configurations"].push_back(c.id);
      wantedCfg.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(cfgDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedCfg.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    if (!WriteTextAtomic(dir / "index.json", index.dump(2) + "\n", error))
      return false;
    LOG_INFO("Saved assembly ({} occurrences, {} mates) to {}",
             asmDoc.OccurrenceCount(), asmDoc.MateCount(), dirPath);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

std::unique_ptr<AssemblyDocument> LoadAssemblyDir(const std::string& dirPath,
                                                  std::string* error) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error)
        *error = "Not a Lobole project directory: " + dirPath;
      return nullptr;
    }
    json index = ParseProjectFile(fs::path(dirPath) / "index.json");
    if (index.value("format", "") != kFormat) {
      if (error)
        *error = "Not a Lobole document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "assembly") {
      if (error)
        *error = "Not an assembly: " + dirPath;
      return nullptr;
    }

    // Occurrence files are found by their id prefix — the label suffix is
    // cosmetic and may lag a rename.
    std::map<uint64_t, fs::path> byId;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "occurrences", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      InsertIdFile(byId, entry.path(), "occurrence");
    }
    std::vector<json> occJsons;
    CheckIndexIdsUnique(index.value("occurrences", json::array()),
                        "occurrence");
    for (const auto& idJson : index.value("occurrences", json::array())) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = byId.find(id);
      if (it == byId.end()) {
        if (error)
          *error = "missing occurrence file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      occJsons.push_back(ParseProjectFile(it->second));
    }
    auto asmDoc = std::make_unique<AssemblyDocument>();
    LoadOccurrences(*asmDoc, occJsons);

    // Mates (v24): found by id prefix under mates/, loaded in index order. A
    // v23 assembly has no `mates` key ⇒ the loop is empty.
    std::map<uint64_t, fs::path> mateById;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "mates", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      InsertIdFile(mateById, entry.path(), "mate");
    }
    std::vector<json> mateJsons;
    CheckIndexIdsUnique(index.value("mates", json::array()), "mate");
    for (const auto& idJson : index.value("mates", json::array())) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = mateById.find(id);
      if (it == mateById.end()) {
        if (error)
          *error = "missing mate file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      mateJsons.push_back(ParseProjectFile(it->second));
    }
    LoadMates(*asmDoc, mateJsons);

    // Exploded views (v27): id-prefix files under exploded/, index order. A
    // pre-v27 assembly has no `exploded` key ⇒ the loop is empty.
    std::map<uint64_t, fs::path> viewById;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "exploded", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      InsertIdFile(viewById, entry.path(), "view");
    }
    std::vector<json> viewJsons;
    for (const auto& idJson : index.value("exploded", json::array())) {
      const uint64_t id = idJson.get<uint64_t>();
      const auto it = viewById.find(id);
      if (it == viewById.end()) {
        if (error)
          *error = "missing exploded-view file for id " + std::to_string(id);
        return nullptr;
      }
      std::ifstream fin(it->second);
      viewJsons.push_back(ParseProjectFile(it->second));
    }
    LoadExplodedViews(*asmDoc, viewJsons);

    // Relations (v83): id-prefix files under relations/, index order.
    {
      std::map<uint64_t, fs::path> relById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "relations", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        InsertIdFile(relById, entry.path(), "relation");
      }
      std::vector<json> relJsons;
      for (const auto& idJson : index.value("relations", json::array())) {
        const uint64_t id = idJson.get<uint64_t>();
        const auto it = relById.find(id);
        if (it == relById.end()) {
          if (error)
            *error = "missing relation file for id " + std::to_string(id);
          return nullptr;
        }
        relJsons.push_back(ParseProjectFile(it->second));
      }
      LoadRelations(*asmDoc, relJsons);
    }

    // Clearance assertions (v97): id-prefix files under assertions/.
    {
      std::map<uint64_t, fs::path> byId;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "assertions", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        InsertIdFile(byId, entry.path(), "assertion");
      }
      std::vector<json> jsons;
      for (const auto& idJson : index.value("assertions", json::array())) {
        const uint64_t id = idJson.get<uint64_t>();
        const auto it = byId.find(id);
        if (it == byId.end()) {
          if (error)
            *error = "missing assertion file for id " + std::to_string(id);
          return nullptr;
        }
        jsons.push_back(ParseProjectFile(it->second));
      }
      LoadAssertions(*asmDoc, jsons);
    }

    // Motion studies (v87): id-prefix files under motion/, index order; a
    // result (v153) beside its study.
    {
      std::map<uint64_t, fs::path> byId, resultById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "motion", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        if (IsResultFile(entry.path()))
          InsertIdFile(resultById, entry.path(), "motion result");
        else
          InsertIdFile(byId, entry.path(), "motion");
      }
      std::vector<json> jsons;
      for (const auto& idJson : index.value("motion", json::array())) {
        const uint64_t id = idJson.get<uint64_t>();
        const auto it = byId.find(id);
        if (it == byId.end()) {
          if (error)
            *error = "missing motion study file for id " + std::to_string(id);
          return nullptr;
        }
        jsons.push_back(ParseProjectFile(it->second));
      }
      LoadMotionStudies(*asmDoc, jsons);
      // Read current or stale by the app, which can resolve the parts its
      // input hash reads (ui::AdoptSavedMotionResults).
      for (const auto& [id, path] : resultById)
        if (asmDoc->FindMotionStudyById(id) != nullptr)
          if (auto rj = ReadResultFiles(path))
            asmDoc->StoreMotionResult(id,
                                      std::make_shared<const motion::Result>(
                                          MotionResultFromJson(*rj)));
    }

    // FEM studies (v106): id-prefix files under studies/, index order; a
    // result (v153) beside its study.
    {
      std::map<uint64_t, fs::path> byId, resultById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "studies", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        if (IsResultFile(entry.path()))
          InsertIdFile(resultById, entry.path(), "study result");
        else
          InsertIdFile(byId, entry.path(), "study");
      }
      CheckIndexIdsUnique(index.value("studies", json::array()), "study");
      std::vector<json> jsons;
      for (const auto& idJson : index.value("studies", json::array())) {
        const uint64_t id = idJson.get<uint64_t>();
        const auto it = byId.find(id);
        if (it == byId.end()) {
          if (error)
            *error = "missing study file for id " + std::to_string(id);
          return nullptr;
        }
        jsons.push_back(ParseProjectFile(it->second));
      }
      LoadAssemblyStudies(*asmDoc, jsons);
      // Stale until the app, which can resolve the parts the input hash
      // reads, finds it current (ui::AdoptStoredResults).
      for (const auto& [id, path] : resultById) {
        if (asmDoc->FindStudy(id) == nullptr)
          continue;
        auto rj = ReadResultFiles(path);
        if (!rj)
          continue;
        fem::StudyRuntime rt = StudyResultFromJson(*rj);
        rt.solvedAtVersion = UINT64_MAX;
        asmDoc->StoreStudyResults(id, std::move(rt));
      }
    }

    // Configurations (v31): id-prefix files under configurations/, index order.
    {
      std::map<uint64_t, fs::path> cfgById;
      for (const auto& entry :
           fs::directory_iterator(fs::path(dirPath) / "configurations", ec)) {
        if (entry.path().extension() != ".json")
          continue;
        InsertIdFile(cfgById, entry.path(), "configuration");
      }
      CheckIndexIdsUnique(index.value("configurations", json::array()),
                          "configuration");
      for (const auto& idJson : index.value("configurations", json::array())) {
        const auto it = cfgById.find(idJson.get<uint64_t>());
        if (it == cfgById.end()) {
          if (error)
            *error = "missing configuration file for id " +
                     std::to_string(idJson.get<uint64_t>());
          return nullptr;
        }
        std::ifstream cin(it->second);
        asmDoc->RestoreConfiguration(
            ConfigurationFromJson(ParseProjectFile(it->second)));
      }
    }
    asmDoc->SetActiveConfiguration(
        index.value("active_configuration", uint64_t{0}));
    asmDoc->SetUnits(UnitsFromEnvelope(index)); // v64

    LOG_INFO("Loaded assembly ({} occurrences, {} mates) from {}",
             asmDoc->OccurrenceCount(), asmDoc->MateCount(), dirPath);
    return asmDoc;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load assembly directory {}: {}", dirPath, e.what());
    return nullptr;
  }
}

} // namespace

static bool SaveImpl(const Document& doc, const std::string& path,
                     std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return SaveDir(doc, normalized, error);
  }
  try {
    json j = DocumentEnvelope(doc);
    j["features"] = json::array();
    for (const auto& feature : doc.GetFeatures()) {
      j["features"].push_back(FeatureToJson(*feature));
    }
    // FEM studies (v29), inline here (unlike the dir format's studies/ files);
    // absent when none so pre-v29 files stay byte-identical.
    if (!doc.GetStudies().empty()) {
      j["studies"] = json::array();
      for (const fem::Study& s : doc.GetStudies())
        j["studies"].push_back(StudyToJson(s));
    }
    // Configurations (v31), inline (the active id is in the envelope above).
    if (!doc.GetConfigurations().empty()) {
      j["configurations"] = json::array();
      for (const Configuration& c : doc.GetConfigurations())
        j["configurations"].push_back(ConfigurationToJson(c));
    }
    // Reference-image bytes (v49) base64 under a document-root "assets" map so
    // the single-file form stays self-contained. Absent when there are none.
    {
      json assets = json::object();
      for (const std::string& name : ReferencedAssets(doc)) {
        const std::vector<unsigned char>* bytes = doc.GetAsset(name);
        if (bytes && !bytes->empty())
          assets[name] = Base64Encode(*bytes);
      }
      if (!assets.empty())
        j["assets"] = std::move(assets);
    }

    std::ofstream out(path);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved document ({} features) to {}", doc.FeatureCount(), path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to save document to {}: {}", path, e.what());
    return false;
  }
}

static std::unique_ptr<Document> LoadImpl(const std::string& path,
                                          std::string* error,
                                          const ProjectScope& scope) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadDir(normalized, error, scope);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error)
        *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = ParseProjectFile(path);

    if (j.value("format", "") != kFormat) {
      if (error)
        *error = "Not a Lobole document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "part") {
      if (error)
        *error =
            "This is a " + j.value("kind", "part") + ", not a part: " + path;
      return nullptr;
    }

    auto doc = std::make_unique<Document>();
    doc->SetProjectSymbols(scope.symbols);
    doc->SetProjectParameters(scope.parameters);
    doc->SetCommitGeometry(j.value("commit_geometry", true));
    doc->SetCatalogsRaw(j.value("catalogs", json::array()));
    doc->SetTablesRaw(lookuptable::ListFromJson(j.value("tables", json())));
    // Refused, not dropped, when one names a measure this build does not
    // know: the requirement would load as something it is not.
    doc->SetRequirementsRaw(
        req::ListToJson(req::ListFromJson(j.value("requirements", json()))));
    doc->SetTemplatesRaw(tmpl::LibraryFromJson(j.value("templates", json())));
    doc->SetGeneratorRaw(j.value("generator", json(nullptr)));
    // Parameters first: features' expressions evaluate against them on the
    // regeneration each AddFeature triggers.
    for (const auto& pj : j.value("parameters", json::array())) {
      doc->SetParameter(pj.at("name").get<std::string>(),
                        pj.at("expression").get<std::string>(),
                        pj.value("comment", std::string{}));
    }
    // Tolerances (v98). They reference elements of features, but nothing
    // about them is evaluated at load: a tolerance is design intent about a
    // face, so a ref that no longer resolves stays recorded and is reported
    // as unresolved when something LOOKS, rather than being dropped here.
    for (const auto& tj : j.value("tolerances", json::array())) {
      Tolerance t;
      t.id = tj.value("id", uint64_t{0});
      t.ref = ElementRefFromJson(tj.value("ref", nlohmann::json::object()));
      t.ref2 = ElementRefFromJson(tj.value("ref2", nlohmann::json::object()));
      t.isoClass = tj.value("class", std::string{});
      t.lower = tj.value("lower", 0.0);
      t.upper = tj.value("upper", 0.0);
      t.printedPress = TolerancePrintedFromJson(tj);
      t.comment = tj.value("comment", std::string{});
      doc->RestoreTolerance(std::move(t));
    }
    // Sheet-metal rules (v108) BEFORE the features, for the reason above.
    doc->SetSheetMetal(SheetMetalFromEnvelope(j));
    doc->SetTube(TubeFromEnvelope(j));
    doc->SetPrintProcess(PrintFromEnvelope(j));
    if (j.contains("print_settings")) // v181
      doc->SetPrintSettings(
          DocumentIO::PartPrintSettingsFromJson(j.at("print_settings")));
    // Assets (v49 image bytes, v84 derived snapshots): decode the base64
    // "assets" map into the doc store BEFORE the features regenerate — a
    // Derived feature reads its B-Reps from it during Execute.
    if (j.contains("assets"))
      for (const auto& [name, b64] : j.at("assets").items())
        doc->SetAsset(name, Base64Decode(b64.get<std::string>()));
    {
      Document::BulkEdit bulk(*doc); // one regeneration, not one per feature
      for (const auto& fj : j.at("features")) {
        doc->AddFeature(FeatureFromJson(fj));
      }
    }
    for (const auto& sj : j.value("studies", json::array())) { // v29
      doc->RestoreStudy(StudyFromJson(sj));
    }
    for (const auto& cj : j.value("configurations", json::array())) { // v31
      doc->RestoreConfiguration(ConfigurationFromJson(cj));
    }
    doc->SetRollback(j.value("rollback", -1)); // v19; absent = end
    doc->SetActiveConfiguration(j.value("active_configuration", uint64_t{0}));
    doc->SetUnits(UnitsFromEnvelope(j)); // v64
    LOG_INFO("Loaded document ({} features) from {}", doc->FeatureCount(),
             path);
    return doc;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load document from {}: {}", path, e.what());
    return nullptr;
  }
}

static bool SaveAssemblyImpl(const AssemblyDocument& asmDoc,
                             const std::string& path, std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return SaveAssemblyDir(asmDoc, normalized, error);
  }
  try {
    json j = AssemblyEnvelope(asmDoc);
    j["occurrences"] = json::array();
    for (const Occurrence& o : asmDoc.GetOccurrences()) {
      j["occurrences"].push_back(OccurrenceToJson(o));
    }
    j["mates"] = json::array();
    for (const Mate& m : asmDoc.GetMates()) {
      j["mates"].push_back(MateToJson(m));
    }
    if (!asmDoc.GetExplodedViews().empty()) { // absent when none (v27)
      j["exploded"] = json::array();
      for (const ExplodedView& v : asmDoc.GetExplodedViews())
        j["exploded"].push_back(ExplodedViewToJson(v));
    }
    if (!asmDoc.GetRelations().empty()) { // absent when none (v83)
      j["relations"] = json::array();
      for (const Relation& r : asmDoc.GetRelations())
        j["relations"].push_back(RelationToJson(r));
    }
    if (!asmDoc.GetMotionStudies().empty()) { // absent when none (v87)
      j["motion"] = json::array();
      for (const MotionStudy& s : asmDoc.GetMotionStudies())
        j["motion"].push_back(MotionStudyToJson(s));
    }
    if (!asmDoc.GetStudies().empty()) { // absent when none (v106)
      j["studies"] = json::array();
      for (const fem::Study& s : asmDoc.GetStudies())
        j["studies"].push_back(StudyToJson(s));
    }
    if (!asmDoc.GetAssertions().empty()) { // absent when none (v97)
      j["assertions"] = json::array();
      for (const ClearanceAssertion& a : asmDoc.GetAssertions())
        j["assertions"].push_back(ClearanceAssertionToJson(a));
    }
    if (asmDoc.ActiveConfiguration() != 0) // v31
      j["active_configuration"] = asmDoc.ActiveConfiguration();
    if (!asmDoc.GetConfigurations().empty()) {
      j["configurations"] = json::array();
      for (const Configuration& c : asmDoc.GetConfigurations())
        j["configurations"].push_back(ConfigurationToJson(c));
    }
    std::ofstream out(path);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved assembly ({} occurrences) to {}", asmDoc.OccurrenceCount(),
             path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to save assembly to {}: {}", path, e.what());
    return false;
  }
}

static std::unique_ptr<AssemblyDocument>
LoadAssemblyImpl(const std::string& path, std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadAssemblyDir(normalized, error);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error)
        *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = ParseProjectFile(path);
    if (j.value("format", "") != kFormat) {
      if (error)
        *error = "Not a Lobole document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "assembly") {
      if (error)
        *error = "Not an assembly: " + path;
      return nullptr;
    }
    auto asmDoc = std::make_unique<AssemblyDocument>();
    LoadOccurrences(
        *asmDoc,
        j.value("occurrences", json::array()).get<std::vector<json>>());
    LoadMates(*asmDoc,
              j.value("mates", json::array()).get<std::vector<json>>());
    LoadExplodedViews(
        *asmDoc, j.value("exploded", json::array()).get<std::vector<json>>());
    LoadRelations(*asmDoc,
                  j.value("relations", json::array()).get<std::vector<json>>());
    LoadMotionStudies(
        *asmDoc, j.value("motion", json::array()).get<std::vector<json>>());
    LoadAssemblyStudies(
        *asmDoc, j.value("studies", json::array()).get<std::vector<json>>());
    LoadAssertions(
        *asmDoc, j.value("assertions", json::array()).get<std::vector<json>>());
    for (const auto& cj : j.value("configurations", json::array())) // v31
      asmDoc->RestoreConfiguration(ConfigurationFromJson(cj));
    asmDoc->SetActiveConfiguration(
        j.value("active_configuration", uint64_t{0}));
    asmDoc->SetUnits(UnitsFromEnvelope(j)); // v64
    LOG_INFO("Loaded assembly ({} occurrences, {} mates) from {}",
             asmDoc->OccurrenceCount(), asmDoc->MateCount(), path);
    return asmDoc;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load assembly from {}: {}", path, e.what());
    return nullptr;
  }
}

// ---- Drawings (v30) --------------------------------------------------------
// A drawing serializes like a part/assembly: kind:"drawing", the sheet inline,
// one file per view and per dim in the dir format. Projected geometry and
// measured dimension values are derived, never stored (design doc §2).

namespace {

json SheetJson(const Sheet& s) {
  json tb = json::object();
  const TitleBlock& t = s.titleBlock;
  if (!t.project.empty())
    tb["project"] = t.project;
  if (!t.document.empty())
    tb["document"] = t.document;
  if (!t.author.empty())
    tb["author"] = t.author;
  if (!t.date.empty())
    tb["date"] = t.date;
  if (!t.notes.empty())
    tb["notes"] = t.notes;
  if (!t.material.empty()) // v147
    tb["material"] = t.material;
  if (!t.scale.empty()) // v147
    tb["scale"] = t.scale;
  json j = {{"size", SheetSizeToString(s.size)}, {"landscape", s.landscape}};
  if (!tb.empty())
    j["title_block"] = std::move(tb);
  return j;
}

Sheet SheetFromJson(const json& j) {
  Sheet s;
  // Unknown here used to leave `s.size` at its default: an A2 sheet loaded
  // as A4 with no complaint, and every view's placement and scale was then
  // relative to different paper. The dimension-kind loader below and
  // ParseSheetSizeOrThrow on the RPC side already refuse; this is the same
  // rule, finally applied to the whole loader.
  const std::string sizeStr = j.value("size", std::string("a4"));
  if (!SheetSizeFromString(sizeStr, s.size))
    throw std::runtime_error("unknown sheet size '" + sizeStr +
                             "' (expected one of: a4, a3, a2, a1, a0, letter)");
  s.landscape = j.value("landscape", true);
  const json tb = j.value("title_block", json::object());
  s.titleBlock.project = tb.value("project", std::string{});
  s.titleBlock.document = tb.value("document", std::string{});
  s.titleBlock.author = tb.value("author", std::string{});
  s.titleBlock.date = tb.value("date", std::string{});
  s.titleBlock.notes = tb.value("notes", std::string{});
  s.titleBlock.material = tb.value("material", std::string{});
  s.titleBlock.scale = tb.value("scale", std::string{});
  return s;
}

json ProjRefJson(const ProjRef& r) {
  json j = {{"view", r.viewId}, {"end", r.end}};
  if (r.occurrenceId)
    j["occurrence"] = r.occurrenceId;
  if (r.edge.IsSet())
    j["edge"] = ElementRefJson(r.edge);
  if (r.silFace.IsSet())
    j["sil_face"] = ElementRefJson(r.silFace);
  if (!r.extreme.empty()) // v134
    j["extreme"] = r.extreme;
  return j;
}

ProjRef ProjRefFromJson(const json& j) {
  ProjRef r;
  r.viewId = j.value("view", uint64_t{0});
  r.occurrenceId = j.value("occurrence", uint64_t{0});
  r.end = j.value("end", 0);
  r.extreme = j.value("extreme", std::string{});
  if (!IsProjRefExtreme(r.extreme))
    throw std::runtime_error("unknown dimension ref extreme '" + r.extreme +
                             "' (expected left, right, top or bottom)");
  if (j.contains("edge"))
    r.edge = ElementRefFromJson(j.at("edge"));
  if (j.contains("sil_face"))
    r.silFace = ElementRefFromJson(j.at("sil_face"));
  return r;
}

json DrawingEnvelope() {
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  j["kind"] = "drawing";
  return j;
}

} // namespace

json DrawingViewToJson(const DrawingView& v) {
  json j = {{"id", v.id},
            {"name", v.name},
            {"source", v.sourceDocId},
            {"orientation", ViewOrientationToString(v.orientation)},
            {"scale", v.scale},
            {"position", {v.position.x, v.position.y}},
            {"show_hidden", v.showHidden},
            {"quality", v.quality},
            {"hidden", v.hidden}};
  if (v.baseViewId)
    j["base_view"] = v.baseViewId;
  if (!v.scaleExpr.empty())
    j["scale_expr"] = v.scaleExpr;
  if (v.configuration != 0) // v115
    j["configuration"] = v.configuration;
  if (v.rotationDeg != 0.0) // v134
    j["rotation_deg"] = v.rotationDeg;
  if (v.section.on) { // v147
    const glm::dvec3& p = v.section.point;
    j["section"] = {{"point", {p.x, p.y, p.z}}, {"label", v.section.label}};
  }
  if (v.detail.on) { // v151
    const glm::dvec3& c = v.detail.center;
    j["detail"] = {{"view", v.detail.parentViewId},
                   {"center", {c.x, c.y, c.z}},
                   {"radius_mm", v.detail.radius},
                   {"label", v.detail.label}};
  }
  return j;
}

DrawingView DrawingViewFromJson(const json& j) {
  DrawingView v;
  v.id = j.value("id", uint64_t{0});
  v.name = j.value("name", std::string{});
  v.sourceDocId = j.value("source", uint64_t{0});
  const std::string orientStr = j.value("orientation", std::string("front"));
  if (!ViewOrientationFromString(orientStr, v.orientation))
    throw std::runtime_error(
        "unknown view orientation '" + orientStr +
        "' (expected one of: front, back, left, right, top, bottom, iso)");
  v.baseViewId = j.value("base_view", uint64_t{0});
  v.scale = j.value("scale", 1.0);
  v.scaleExpr = j.value("scale_expr", std::string{});
  const json p = j.value("position", json::array({0.0, 0.0}));
  if (p.is_array() && p.size() == 2)
    v.position = {p[0].get<double>(), p[1].get<double>()};
  v.showHidden = j.value("show_hidden", true);
  v.quality = j.value("quality", std::string("exact"));
  v.hidden = j.value("hidden", std::vector<uint64_t>{});
  v.configuration = j.value("configuration", uint64_t{0});
  v.rotationDeg = j.value("rotation_deg", 0.0);
  if (j.contains("section")) {
    const json& sj = j.at("section");
    const json p = sj.value("point", json::array());
    if (!p.is_array() || p.size() != 3)
      throw std::runtime_error("a section view's point is [x, y, z]");
    v.section.on = true;
    v.section.point = {p[0].get<double>(), p[1].get<double>(),
                       p[2].get<double>()};
    v.section.label = sj.value("label", std::string{});
  }
  if (j.contains("detail")) { // v151
    const json& dj = j.at("detail");
    const json c = dj.value("center", json::array());
    if (!c.is_array() || c.size() != 3)
      throw std::runtime_error("a detail view's center is [x, y, z]");
    v.detail.on = true;
    v.detail.parentViewId = dj.value("view", uint64_t{0});
    v.detail.center = {c[0].get<double>(), c[1].get<double>(),
                       c[2].get<double>()};
    v.detail.radius = dj.value("radius_mm", 0.0);
    v.detail.label = dj.value("label", std::string{});
    if (v.detail.parentViewId == 0 || !(v.detail.radius > 0.0))
      throw std::runtime_error(
          "a detail view names its parent `view` and a `radius_mm` above 0");
  }
  return v;
}

// Notes (v66) live inline in the drawing's index.json — a few short strings
// don't earn per-file diffs the way dims and views do.
json DrawingNoteToJson(const DrawingNote& n) {
  json j{{"id", n.id}, {"text", n.text}, {"pos", {n.pos.x, n.pos.y}}};
  if (n.viewId != 0)
    j["view"] = n.viewId;
  if (n.leader.edge.IsSet() || n.leader.silFace.IsSet())
    j["leader"] = ProjRefJson(n.leader);
  if (n.table != NoteTable::None) // v115
    j["table"] = NoteTableToString(n.table);
  if (n.style != NoteStyle::Text) // v147
    j["style"] = NoteStyleToString(n.style);
  if (n.style == NoteStyle::Fcf) { // v151
    json f = {{"symbol", GeoSymbolToString(n.fcf.symbol)},
              {"tolerance_mm", n.fcf.tolerance},
              {"datums", n.fcf.datums}};
    if (n.fcf.diameterZone)
      f["diameter"] = true;
    if (n.fcf.modifier != MaterialModifier::None)
      f["modifier"] = MaterialModifierToString(n.fcf.modifier);
    j["fcf"] = std::move(f);
  }
  if (n.attachDim != 0) // v151
    j["dim"] = n.attachDim;
  return j;
}

DrawingNote DrawingNoteFromJson(const json& j) {
  DrawingNote n;
  n.id = j.value("id", uint64_t{0});
  n.text = j.value("text", std::string{});
  if (j.contains("pos") && j.at("pos").is_array() && j.at("pos").size() == 2)
    n.pos = {j.at("pos").at(0).get<double>(), j.at("pos").at(1).get<double>()};
  n.viewId = j.value("view", uint64_t{0});
  if (j.contains("leader"))
    n.leader = ProjRefFromJson(j.at("leader"));
  const std::string table = j.value("table", std::string("none"));
  if (!NoteTableFromString(table, n.table))
    throw std::runtime_error(UnknownNoteTable(table));
  const std::string style = j.value("style", std::string("text"));
  if (!NoteStyleFromString(style, n.style))
    throw std::runtime_error(UnknownNoteStyle(style));
  if (n.style == NoteStyle::Fcf) { // v151
    if (!j.contains("fcf") || !j.at("fcf").is_object())
      throw std::runtime_error("an fcf note carries its frame in `fcf`");
    const json& f = j.at("fcf");
    const std::string symbol = f.value("symbol", std::string{});
    if (!GeoSymbolFromString(symbol, n.fcf.symbol))
      throw std::runtime_error(UnknownGeoSymbol(symbol));
    n.fcf.tolerance = f.value("tolerance_mm", 0.0);
    n.fcf.diameterZone = f.value("diameter", false);
    const std::string modifier = f.value("modifier", std::string("none"));
    if (!MaterialModifierFromString(modifier, n.fcf.modifier))
      throw std::runtime_error(UnknownMaterialModifier(modifier));
    n.fcf.datums = f.value("datums", std::vector<std::string>{});
    if (const std::string why = ValidateFcf(n.fcf); !why.empty())
      throw std::runtime_error(why);
  }
  n.attachDim = j.value("dim", uint64_t{0});
  return n;
}

json DrawingDimToJson(const DrawingDim& d) {
  json refs = json::array();
  for (const ProjRef& r : d.refs)
    refs.push_back(ProjRefJson(r));
  return json{{"id", d.id},
              {"kind", DrawingDimKindToString(d.kind)},
              {"view", d.viewId},
              {"refs", std::move(refs)},
              {"label_offset", {d.labelOffset.x, d.labelOffset.y}}};
}

DrawingDim DrawingDimFromJson(const json& j) {
  DrawingDim d;
  d.id = j.value("id", uint64_t{0});
  // An unknown kind string used to silently default to Distance — surface it
  // instead (the version gate is the real forward-compat guard, so this only
  // fires on corrupt/hand-edited files).
  const std::string kind = j.value("kind", std::string("distance"));
  if (!DrawingDimKindFromString(kind, d.kind))
    throw std::runtime_error("unknown drawing dimension kind '" + kind + "'");
  d.viewId = j.value("view", uint64_t{0});
  for (const json& rj : j.value("refs", json::array()))
    d.refs.push_back(ProjRefFromJson(rj));
  const json o = j.value("label_offset", json::array({0.0, 0.0}));
  if (o.is_array() && o.size() == 2)
    d.labelOffset = {o[0].get<double>(), o[1].get<double>()};
  return d;
}

namespace {

bool SaveDrawingDir(const DrawingDocument& dwg, const std::string& dirPath,
                    std::string* error) {
  namespace fs = std::filesystem;
  try {
    const fs::path dir(dirPath);
    fs::create_directories(dir);

    json index = DrawingEnvelope();
    index["sheet"] = SheetJson(dwg.GetSheet());

    const fs::path viewDir = dir / "views";
    index["views"] = json::array();
    if (!dwg.GetViews().empty())
      fs::create_directories(viewDir);
    std::set<std::string> wantedView;
    for (const DrawingView& v : dwg.GetViews()) {
      const std::string base = std::to_string(IdForFile(v.id, "view", v.name)) +
                               "-" + LabelSlug(v.name);
      const std::string file = base + ".json";
      if (!WriteTextAtomic(viewDir / file, DrawingViewToJson(v).dump(2) + "\n",
                           error))
        return false;
      index["views"].push_back(v.id);
      wantedView.insert(file);
      // Its last projection beside it (v162), as a study's result is: an
      // unchanged one is not rewritten, and one whose inputs nobody hashed
      // is not written at all.
      const auto projection = dwg.SavedProjection(v.id);
      if (projection && projection->inputHash != 0 &&
          !WriteResultFiles(viewDir, base, ViewProjectionToJson(*projection),
                            wantedView, error, ".projection"))
        return false;
    }
    std::error_code ec;
    PruneCollection(viewDir, wantedView);

    const fs::path dimDir = dir / "dims";
    index["dims"] = json::array();
    if (!dwg.GetDims().empty())
      fs::create_directories(dimDir);
    std::set<std::string> wantedDim;
    for (const DrawingDim& d : dwg.GetDims()) {
      const std::string file =
          std::to_string(
              IdForFile(d.id, "dimension", DrawingDimKindToString(d.kind))) +
          "-" + DrawingDimKindToString(d.kind) + ".json";
      if (!WriteTextAtomic(dimDir / file, DrawingDimToJson(d).dump(2) + "\n",
                           error))
        return false;
      index["dims"].push_back(d.id);
      wantedDim.insert(file);
    }
    for (const auto& entry : fs::directory_iterator(dimDir, ec)) {
      if (entry.path().extension() == ".json" &&
          !wantedDim.contains(entry.path().filename().string()))
        fs::remove(entry.path(), ec);
    }

    index["notes"] = json::array();
    for (const DrawingNote& n : dwg.GetNotes())
      index["notes"].push_back(DrawingNoteToJson(n));

    if (!WriteTextAtomic(dir / "index.json", index.dump(2) + "\n", error))
      return false;
    LOG_INFO("Saved drawing ({} views, {} dims, {} notes) to {}",
             dwg.ViewCount(), dwg.DimCount(), dwg.NoteCount(), dirPath);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

std::unique_ptr<DrawingDocument> LoadDrawingDir(const std::string& dirPath,
                                                std::string* error) {
  namespace fs = std::filesystem;
  try {
    std::ifstream in(fs::path(dirPath) / "index.json");
    if (!in) {
      if (error)
        *error = "Not a Lobole project directory: " + dirPath;
      return nullptr;
    }
    json index = ParseProjectFile(fs::path(dirPath) / "index.json");
    if (index.value("format", "") != kFormat) {
      if (error)
        *error = "Not a Lobole document: " + dirPath;
      return nullptr;
    }
    if (index.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (index.value("kind", "part") != "drawing") {
      if (error)
        *error = "Not a drawing: " + dirPath;
      return nullptr;
    }

    auto dwg = std::make_unique<DrawingDocument>();
    dwg->GetSheet() = SheetFromJson(index.value("sheet", json::object()));

    std::map<uint64_t, fs::path> viewById, projectionById;
    std::error_code ec;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "views", ec)) {
      if (entry.path().extension() != ".json")
        continue;
      if (IsProjectionFile(entry.path()))
        InsertIdFile(projectionById, entry.path(), "view projection");
      else
        InsertIdFile(viewById, entry.path(), "view");
    }
    uint64_t maxId = 0;
    CheckIndexIdsUnique(index.value("views", json::array()), "view");
    for (const auto& idJson : index.value("views", json::array())) {
      const auto it = viewById.find(idJson.get<uint64_t>());
      if (it == viewById.end())
        continue;
      std::ifstream vin(it->second);
      DrawingView v = DrawingViewFromJson(ParseProjectFile(it->second));
      maxId = std::max(maxId, v.id);
      // Its saved projection (v162), a candidate until the projecting job
      // hashes its inputs. One that does not read is dropped and the view
      // projects: it is derived, and never a reason to refuse the drawing.
      if (const auto pit = projectionById.find(v.id);
          pit != projectionById.end()) {
        try {
          if (const auto pj = ReadResultFiles(pit->second))
            dwg->SetSavedProjection(
                v.id,
                std::make_shared<const drawing::ViewProjection>(
                    ViewProjectionFromJson(*pj, ProjectRelative(pit->second))));
        } catch (const std::exception& e) {
          LOG_WARN("Dropping the saved projection {}: {}; the view projects "
                   "again",
                   ProjectRelative(pit->second), e.what());
        }
      }
      dwg->GetViews().push_back(std::move(v));
    }

    std::map<uint64_t, fs::path> dimById;
    for (const auto& entry :
         fs::directory_iterator(fs::path(dirPath) / "dims", ec)) {
      if (entry.path().extension() == ".json")
        InsertIdFile(dimById, entry.path(), "dimension");
    }
    for (const auto& idJson : index.value("dims", json::array())) {
      const auto it = dimById.find(idJson.get<uint64_t>());
      if (it == dimById.end())
        continue;
      std::ifstream din(it->second);
      DrawingDim d = DrawingDimFromJson(ParseProjectFile(it->second));
      maxId = std::max(maxId, d.id);
      dwg->GetDims().push_back(std::move(d));
    }
    for (const json& nj : index.value("notes", json::array())) {
      DrawingNote n = DrawingNoteFromJson(nj);
      maxId = std::max(maxId, n.id);
      dwg->GetNotes().push_back(std::move(n));
    }
    dwg->EnsureNextIdAtLeast(maxId + 1);
    LOG_INFO("Loaded drawing ({} views, {} dims, {} notes) from {}",
             dwg->ViewCount(), dwg->DimCount(), dwg->NoteCount(), dirPath);
    return dwg;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return nullptr;
  }
}

} // namespace

static bool SaveDrawingImpl(const DrawingDocument& dwg, const std::string& path,
                            std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return SaveDrawingDir(dwg, normalized, error);
  }
  try {
    json j = DrawingEnvelope();
    j["sheet"] = SheetJson(dwg.GetSheet());
    j["views"] = json::array();
    for (const DrawingView& v : dwg.GetViews())
      j["views"].push_back(DrawingViewToJson(v));
    j["dims"] = json::array();
    for (const DrawingDim& d : dwg.GetDims())
      j["dims"].push_back(DrawingDimToJson(d));
    j["notes"] = json::array();
    for (const DrawingNote& n : dwg.GetNotes())
      j["notes"].push_back(DrawingNoteToJson(n));
    std::ofstream out(path);
    if (!out) {
      if (error)
        *error = "Cannot open file for writing: " + path;
      return false;
    }
    out << j.dump(2) << "\n";
    LOG_INFO("Saved drawing ({} views) to {}", dwg.ViewCount(), path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to save drawing to {}: {}", path, e.what());
    return false;
  }
}

static std::unique_ptr<DrawingDocument> LoadDrawingImpl(const std::string& path,
                                                        std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  if (IsDirFormatPath(normalized)) {
    return LoadDrawingDir(normalized, error);
  }
  try {
    std::ifstream in(path);
    if (!in) {
      if (error)
        *error = "Cannot open file: " + path;
      return nullptr;
    }
    json j = ParseProjectFile(path);
    if (j.value("format", "") != kFormat) {
      if (error)
        *error = "Not a Lobole document: " + path;
      return nullptr;
    }
    if (j.value("version", 0) > kVersion) {
      if (error)
        *error = "Document version is newer than this build supports";
      return nullptr;
    }
    if (j.value("kind", "part") != "drawing") {
      if (error)
        *error = "Not a drawing: " + path;
      return nullptr;
    }
    auto dwg = std::make_unique<DrawingDocument>();
    dwg->GetSheet() = SheetFromJson(j.value("sheet", json::object()));
    uint64_t maxId = 0;
    for (const json& vj : j.value("views", json::array())) {
      DrawingView v = DrawingViewFromJson(vj);
      maxId = std::max(maxId, v.id);
      dwg->GetViews().push_back(std::move(v));
    }
    for (const json& dj : j.value("dims", json::array())) {
      DrawingDim d = DrawingDimFromJson(dj);
      maxId = std::max(maxId, d.id);
      dwg->GetDims().push_back(std::move(d));
    }
    for (const json& nj : j.value("notes", json::array())) {
      DrawingNote n = DrawingNoteFromJson(nj);
      maxId = std::max(maxId, n.id);
      dwg->GetNotes().push_back(std::move(n));
    }
    dwg->EnsureNextIdAtLeast(maxId + 1);
    LOG_INFO("Loaded drawing ({} views, {} dims) from {}", dwg->ViewCount(),
             dwg->DimCount(), path);
    return dwg;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load drawing from {}: {}", path, e.what());
    return nullptr;
  }
}

// Public boundary: adapt the internal *Impl helpers (which keep the string
// out-param idiom) to std::expected so the error can't be discarded and a
// caller can't read a document off the failure path.
std::expected<void, std::string> Save(const Document& doc,
                                      const std::string& path) {
  std::string error;
  if (SaveImpl(doc, path, &error))
    return {};
  return std::unexpected(error);
}

std::expected<std::unique_ptr<Document>, std::string>
Load(const std::string& path, const ProjectScope& scope) {
  std::string error;
  if (auto doc = LoadImpl(path, &error, scope))
    return doc;
  return std::unexpected(error);
}

std::string ProjectSaveTarget(const std::string& picked) {
  if (picked.empty())
    return picked;
  const std::filesystem::path p(picked);
  const std::string ext = p.extension().string();
  if (ext == ".dsgn")
    return picked;
  // ".json" is REPLACED (it is the flat form we are steering away from);
  // anything else is APPENDED to, so "hinge v2" keeps its dot and becomes
  // "hinge v2.dsgn" rather than losing the ".2".
  if (ext == ".json")
    return std::filesystem::path(picked).replace_extension(".dsgn").string();
  return picked + ".dsgn";
}

std::expected<void, std::string> SaveAssembly(const AssemblyDocument& asmDoc,
                                              const std::string& path) {
  std::string error;
  if (SaveAssemblyImpl(asmDoc, path, &error))
    return {};
  return std::unexpected(error);
}

std::expected<std::unique_ptr<AssemblyDocument>, std::string>
LoadAssembly(const std::string& path) {
  std::string error;
  if (auto doc = LoadAssemblyImpl(path, &error))
    return doc;
  return std::unexpected(error);
}

std::expected<void, std::string> SaveDrawing(const DrawingDocument& drawing,
                                             const std::string& path) {
  std::string error;
  if (SaveDrawingImpl(drawing, path, &error))
    return {};
  return std::unexpected(error);
}

std::expected<std::unique_ptr<DrawingDocument>, std::string>
LoadDrawing(const std::string& path) {
  std::string error;
  if (auto doc = LoadDrawingImpl(path, &error))
    return doc;
  return std::unexpected(error);
}

// ---- Jobs (docs/cam/README.md#the-model) ------------------------------

namespace {

json JobEnvelope() {
  json j;
  j["format"] = kFormat;
  j["version"] = kVersion;
  j["kind"] = "job";
  return j;
}

// The whole document, in one object. A job is a parts list and a few setups:
// nothing here carries a mesh, so nothing earns a file of its own and the
// directory and single-file forms differ only in where this lands.
json JobBodyJson(const cam::JobDocument& job) {
  json j = JobEnvelope();
  j["parts"] = json::array();
  for (const cam::JobPart& p : job.GetParts()) {
    json pj{{"id", p.id}, {"source", p.sourceDocId}, {"name", p.name}};
    if (p.configuration != 0) // v135
      pj["configuration"] = p.configuration;
    j["parts"].push_back(std::move(pj));
  }
  j["cam"] = json::array();
  for (const cam::Setup& setup : job.GetSetups())
    j["cam"].push_back(CamSetupToJson(setup));
  return j;
}

// The id counter is restored from the highest id actually present rather
// than from a stored `next_id`: a counter written to disk and an id written
// to disk can disagree, and only one of them is checkable.
std::unique_ptr<cam::JobDocument> JobFromBodyJson(const json& j) {
  auto job = std::make_unique<cam::JobDocument>();
  uint64_t maxId = 0;
  for (const json& pj : j.value("parts", json::array())) {
    cam::JobPart part;
    part.id = pj.value("id", uint64_t{0});
    part.sourceDocId = pj.value("source", uint64_t{0});
    part.name = pj.value("name", std::string{});
    part.configuration = pj.value("configuration", uint64_t{0});
    maxId = std::max(maxId, part.id);
    job->GetParts().push_back(std::move(part));
  }
  const int version = j.value("version", kVersion);
  for (const json& cj : j.value("cam", json::array())) {
    cam::Setup setup = CamSetupFromJson(cj);
    // A rotary op reads its stepdown from v123, as the depth between layers.
    // Before that the derivation stored one on every op and a rotary op
    // ignored it -- so reading it now would re-cut every v122 rotary job in
    // layers nobody asked for. It was never the cut, so it is dropped.
    if (version < 123)
      for (cam::Operation& op : setup.operations)
        if (op.type == cam::OpType::SurfaceRotary)
          op.stepdown = 0.0;
    // The same move for the raster at v125, and for the same reason: it
    // reads `stepdown` as roughing layers now, the derivation stored one on
    // every raster before, and honouring it would turn every saved finish
    // into a rough (docs/cam/operations.md#rough-a-surface-and-rest-machining).
    if (version < 125)
      for (cam::Operation& op : setup.operations)
        if (op.type == cam::OpType::SurfaceParallel)
          op.stepdown = 0.0;
    maxId = std::max(maxId, setup.id);
    for (const cam::Operation& op : setup.operations)
      maxId = std::max(maxId, op.id);
    // Placements and fixtures draw from the same counter. Placements were
    // left out of this rebuild until v126, so a placement made after a
    // reload could be handed an id an existing one already had.
    for (const cam::Placement& pl : setup.placements)
      maxId = std::max(maxId, pl.id);
    for (const cam::Fixture& f : setup.fixtures)
      maxId = std::max(maxId, f.id);
    job->GetSetups().push_back(std::move(setup));
  }
  job->EnsureNextIdAtLeast(maxId + 1);
  return job;
}

bool CheckJobEnvelope(const json& j, const std::string& where,
                      std::string* error) {
  if (j.value("format", "") != kFormat) {
    if (error)
      *error = "Not a Lobole document: " + where;
    return false;
  }
  if (j.value("version", 0) > kVersion) {
    if (error)
      *error = "Document version is newer than this build supports";
    return false;
  }
  if (j.value("kind", "part") != "job") {
    if (error)
      *error = "This is a " + j.value("kind", "part") + ", not a job: " + where;
    return false;
  }
  return true;
}

bool SaveJobImpl(const cam::JobDocument& job, const std::string& path,
                 std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  const bool dirForm = IsDirFormatPath(normalized);
  try {
    std::filesystem::path target(normalized);
    if (dirForm) {
      std::filesystem::create_directories(target);
      target /= "index.json";
    }
    if (!WriteTextAtomic(target, JobBodyJson(job).dump(2) + "\n", error))
      return false;
    LOG_INFO("Saved job ({} parts, {} setups) to {}", job.PartCount(),
             job.SetupCount(), path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to save job to {}: {}", path, e.what());
    return false;
  }
}

std::unique_ptr<cam::JobDocument> LoadJobImpl(const std::string& path,
                                              std::string* error) {
  const std::string normalized = NormalizeProjectPath(path);
  const std::filesystem::path file =
      IsDirFormatPath(normalized)
          ? std::filesystem::path(normalized) / "index.json"
          : std::filesystem::path(path);
  try {
    std::ifstream in(file);
    if (!in) {
      if (error)
        *error = "Cannot open file: " + file.string();
      return nullptr;
    }
    json j = ParseProjectFile(file);
    if (!CheckJobEnvelope(j, file.string(), error))
      return nullptr;
    return JobFromBodyJson(j);
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    LOG_ERROR("Failed to load job from {}: {}", file.string(), e.what());
    return nullptr;
  }
}

} // namespace

std::string JobFingerprint(const cam::JobDocument& job) {
  return JobBodyJson(job).dump();
}

std::expected<void, std::string> SaveJob(const cam::JobDocument& job,
                                         const std::string& path) {
  std::string error;
  if (SaveJobImpl(job, path, &error))
    return {};
  return std::unexpected(error);
}

std::expected<std::unique_ptr<cam::JobDocument>, std::string>
LoadJob(const std::string& path) {
  std::string error;
  if (auto job = LoadJobImpl(path, &error))
    return job;
  return std::unexpected(error);
}

} // namespace DocumentIO
