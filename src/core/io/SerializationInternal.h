// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Internal (non-public) shared helpers for the Serialize* domain translation
// units carved out of Serialization.cpp. Only the serialize TUs include this;
// consumers use core/Serialization.h. The tiny value/ref converters + the
// format constants live here (inline, global scope so both DocumentIO code and
// the file-local helpers resolve them unqualified); the heavier file/envelope
// helpers stay defined in Serialization.cpp.

#include "core/ElementMap.h"      // ElementRef
#include "core/io/ProjectPaths.h" // IsPlainComponent
#include "core/io/ReadContext.h"  // RefuseUnknownKeys, FileContext

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace drawing {
struct ViewProjection; // core/drawing/ViewProjection.h
}

inline constexpr const char* kFormat = "designer-document";
// The format version. What each one changed is docs/versioning/format.md's
// history table, and only there: a comment history here was the same few
// lines on every branch.
//
// A branch never picks the number. It raises kVersionPending by one per
// format change and writes `vNEXT` (`vNEXT2`, ...) wherever the number would
// go; tools/merge-branch.sh numbers it as it lands, moving kVersionLanded and
// resetting kVersionPending
// (docs/versioning/format.md#a-bump-takes-its-number-when-it-lands). Keep a
// line between the two: a landing that moves one and a branch that raises
// the other then never conflict.
inline constexpr int kVersionLanded = 214;

inline constexpr int kVersionPending = 1;
inline constexpr int kVersion = kVersionLanded + kVersionPending;

// What a branch's loader migration compares against, `if (version <
// kVersionNEXT)`; the landing replaces the name with the number.
inline constexpr int kVersionNEXT = kVersionLanded + 1;
inline constexpr int kVersionNEXT2 = kVersionLanded + 2;
inline constexpr int kVersionNEXT3 = kVersionLanded + 3;

// A saved feature as this build reads it: what changed in meaning between
// `version` and now, applied to its JSON before FeatureFromJson. An Import
// before v190 stays where it was (`up_axis` none), where a new one decides
// from the file's header (SerializeFeatures.cpp).
[[nodiscard]] nlohmann::json MigratedFeatureJson(nlohmann::json j, int version);

// An asset name from a document becomes a file in the project's assets/
// directory, so it must be one plain file name (core/io/ProjectPaths.h):
// refused, never cleaned up into something else. Empty passes: an image may
// have no asset.
inline const std::string& CheckedAssetName(const std::string& name) {
  if (!name.empty() && !projectpaths::IsPlainComponent(name))
    throw std::runtime_error("asset name '" + name +
                             "' is not a plain file name");
  return name;
}

// Positions are read and written as doubles: a JSON number is one, and a
// sketch plane at z = 13.4 read through float lands 0.4 um below a face at
// 13.4 (docs/sketching/README.md#plane). A float field takes the narrowing
// where it is assigned.
inline nlohmann::json Vec3ToJson(const glm::dvec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}
// A fixed-size array, refused when it is any other size: a fourth number
// would otherwise be dropped with no word, and a save would lose it.
inline void RequireArrayOf(const nlohmann::json& j, size_t n,
                           const char* shape) {
  if (!j.is_array() || j.size() != n)
    throw std::runtime_error(std::string("expected ") + shape + ", got " +
                             j.dump());
}
inline glm::dvec3 Vec3FromJson(const nlohmann::json& j) {
  RequireArrayOf(j, 3, "[x, y, z]");
  return glm::dvec3(j.at(0).get<double>(), j.at(1).get<double>(),
                    j.at(2).get<double>());
}

// A reference the writer writes only when it is set (a feature and an
// element): one that names an element of no feature is refused, as the
// writer would drop the element without a word.
inline void RequireSetOrEmpty(const ElementRef& ref, const char* what) {
  if (!ref.name.empty() && ref.featureId == 0)
    throw std::runtime_error(std::string(what) + ": '" + ref.name +
                             "' is an element of no feature");
}

// Whether `j` has `key`, as an array of `n` numbers. Absent is false; present
// in any other shape is REFUSED, naming it: a size check that only skipped a
// wrong one dropped the value with no word, and a save lost it for good.
inline bool HasArrayOf(const nlohmann::json& j, const char* key, size_t n) {
  const auto it = j.find(key);
  if (it == j.end())
    return false;
  if (!it->is_array() || it->size() != n ||
      !std::all_of(it->begin(), it->end(),
                   [](const nlohmann::json& v) { return v.is_number(); }))
    throw std::runtime_error(std::string(key) + ": expected " +
                             std::to_string(n) + " numbers, got " +
                             it->dump().substr(0, 80));
  return true;
}

// An id a feature names (`sketch_id`, `path_id`, ...): a whole number, or
// absent (0). A float -- 2^64 written as 1.8e19, a fraction -- read through
// get<uint64_t>() became some other id, or 0 and the reference was dropped.
inline uint64_t IdField(const nlohmann::json& j, const char* key,
                        bool required) {
  const auto it = j.find(key);
  if (it == j.end()) {
    if (required)
      (void)j.at(key); // nlohmann's own "not found", which callers name
    return 0;
  }
  if (!readctx::IsWhole(*it))
    throw std::runtime_error(std::string(key) + ": " + it->dump() +
                             " is not an id");
  return it->get<uint64_t>();
}

// A list of items, refused when two carry one id or one carries an id that
// is not a whole number above 0 (a fraction read as the whole one below it,
// and two items then shared it). Items with no id are let through: a v1
// feature has none.
inline void RefuseRepeatedIds(const nlohmann::json& list, const char* key,
                              bool required = false) {
  if (!list.is_array())
    return;
  std::vector<std::pair<uint64_t, size_t>> seen;
  for (size_t i = 0; i < list.size(); i++) {
    const nlohmann::json& item = list[i];
    // `required`: an item written without one would be saved as id 0.
    if (required && item.is_object() && !item.contains("id"))
      throw std::runtime_error(std::string(key) + "[" + std::to_string(i) +
                               "] has no id");
    if (!item.is_object() || !item.contains("id"))
      continue;
    const nlohmann::json& id = item.at("id");
    if (!readctx::IsWhole(id) || id.get<uint64_t>() == 0)
      throw std::runtime_error(std::string(key) + "[" + std::to_string(i) +
                               "]: its id " + id.dump() +
                               " is not a whole number above 0");
    for (const auto& [other, at] : seen)
      if (other == id.get<uint64_t>())
        throw std::runtime_error(std::string(key) + "[" + std::to_string(i) +
                                 "] has id " + id.dump() + ", which " + key +
                                 "[" + std::to_string(at) + "] has too");
    seen.emplace_back(id.get<uint64_t>(), i);
  }
}

// A list of ids, each once. An id listed twice means nothing it does not
// mean once -- and a damaged file listing one a million times (a Cope's
// `mitre`) was a million booleans. Refused, naming the id.
inline std::vector<uint64_t> UniqueIds(const nlohmann::json& list,
                                       const char* key) {
  if (!list.is_array())
    throw std::runtime_error(std::string(key) + ": expected a list of ids");
  std::vector<uint64_t> out;
  for (const nlohmann::json& v : list) {
    if (!readctx::IsWhole(v))
      throw std::runtime_error(std::string(key) + ": " + v.dump() +
                               " is not an id");
    const uint64_t id = v.get<uint64_t>();
    if (std::find(out.begin(), out.end(), id) != out.end())
      throw std::runtime_error(std::string(key) + " lists " +
                               std::to_string(id) + " twice");
    out.push_back(id);
  }
  return out;
}

// A plane's normal and in-plane axis as a file states them, refused when
// they make no frame: a zero or non-finite vector, or an axis along the
// normal (the in-plane V would be zero, every point of the plane NaN, and
// the save would write those NaNs as nulls it cannot read back).
inline void RequirePlaneFrame(const glm::dvec3& normal, const glm::dvec3& u,
                              const char* what) {
  const auto finite = [](const glm::dvec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
  };
  const double n = glm::length(normal);
  if (!finite(normal) || !finite(u) || !(n > 1e-12) ||
      !(glm::length(glm::cross(normal / n, u)) > 1e-9))
    throw std::runtime_error(std::string(what) +
                             ": its normal and its in-plane axis make no "
                             "frame (one is zero, or they are parallel)");
}

// Quaternion as [x, y, z, w] (glm stores (w,x,y,z); JSON order matches the
// a2-design occurrence schema).
inline nlohmann::json QuatToJson(const glm::dquat& q) {
  return nlohmann::json::array({q.x, q.y, q.z, q.w});
}
inline glm::dquat QuatFromJson(const nlohmann::json& j) {
  RequireArrayOf(j, 4, "[x, y, z, w]");
  return glm::dquat(j.at(3).get<double>(), j.at(0).get<double>(),
                    j.at(1).get<double>(), j.at(2).get<double>()); // (w,x,y,z)
}

inline nlohmann::json Vec2ToJson(const glm::dvec2& v) {
  return nlohmann::json::array({v.x, v.y});
}
inline glm::dvec2 Vec2FromJson(const nlohmann::json& j) {
  RequireArrayOf(j, 2, "[x, y]");
  return glm::dvec2(j.at(0).get<double>(), j.at(1).get<double>());
}

inline nlohmann::json ElementRefJson(const ElementRef& ref) {
  return nlohmann::json{{"feature_id", ref.featureId}, {"element", ref.name}};
}
inline ElementRef ElementRefFromJson(const nlohmann::json& j) {
  readctx::RefuseUnknownKeys(j, {"feature_id", "element"}, "element ref");
  // Half a reference names nothing, and the writer, which writes only a
  // set one, would drop the half that is there.
  if (j.contains("feature_id") != j.contains("element"))
    throw std::runtime_error("element ref " + j.dump() +
                             " needs both feature_id and element");
  return ElementRef{j.value("feature_id", uint64_t{0}),
                    j.value("element", std::string{})};
}

// ---- A result's numeric arrays (v154) --------------------------------------
// A study or motion result keeps its metadata in <base>.result.json and its
// numeric arrays in <base>.result.bin beside it
// (docs/simulation/README.md#what-a-saved-result-keeps). In memory the
// bytes ride in the json as a binary value under kResultData, which the
// save splits off and the load puts back, so (de)serializing a result is
// still one json. Each array is named in the json by a reference
// {type, offset, count}: `count` elements of `type` at byte `offset`,
// little-endian. Types: f32 (a field: derived bulk data is float), f64 (a
// placement: the model is double), u16 and u32 (indices; u16 when every
// value fits), i32 (indices that may be -1: a cut's triangles on no face).
inline constexpr const char* kResultData = "data";

class ResultArrayWriter {
public:
  nlohmann::json F32(const std::vector<double>& v);
  nlohmann::json F32(const std::vector<glm::dvec3>& v); // x, y, z each
  nlohmann::json F64(const std::vector<double>& v);
  // Indices: u16, u32, or i32 when one is negative.
  nlohmann::json Index(const std::vector<int>& v);
  // The bytes, as the binary value the json carries under kResultData.
  nlohmann::json Take();

private:
  nlohmann::json Append(const char* type, const void* data, size_t count,
                        size_t size);
  std::vector<std::uint8_t> m_bytes;
};

class ResultArrayReader {
public:
  // `owner` is the result's json: its kResultData binary. `what` names the
  // result in a refusal ("static result").
  ResultArrayReader(const nlohmann::json& owner, std::string what);
  // `ref` must be a reference of the matching kind; `name` is the field,
  // for the refusal. Reals reads f32 or f64; Indices u16, u32 or i32.
  std::vector<double> Reals(const nlohmann::json& ref, const char* name) const;
  std::vector<glm::dvec3> Vec3s(const nlohmann::json& ref,
                                const char* name) const;
  std::vector<int> Indices(const nlohmann::json& ref, const char* name) const;

private:
  const std::uint8_t* Locate(const nlohmann::json& ref, const char* name,
                             std::string& type, size_t& count) const;
  const std::vector<std::uint8_t>* m_bytes = nullptr;
  std::string m_what;
};

// A drawing view's saved projection (v162,
// docs/drawings/README.md#saved-projections) as one json whose arrays ride
// under kResultData, like a study's result: the save splits them into
// <id>-<slug>.projection.bin. FromJson throws on a malformed one; the
// loader drops it with a warning rather than refusing the drawing.
nlohmann::json ViewProjectionToJson(const drawing::ViewProjection& p);
drawing::ViewProjection ViewProjectionFromJson(const nlohmann::json& j,
                                               const std::string& what);
