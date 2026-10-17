// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Internal (non-public) shared helpers for the Serialize* domain translation
// units carved out of Serialization.cpp. Only the serialize TUs include this;
// consumers use core/Serialization.h. The tiny value/ref converters + the
// format constants live here (inline, global scope so both DocumentIO code and
// the file-local helpers resolve them unqualified); the heavier file/envelope
// helpers stay defined in Serialization.cpp.

#include "core/ElementMap.h" // ElementRef

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
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
inline constexpr int kVersionLanded = 181;

inline constexpr int kVersionPending = 1;
inline constexpr int kVersion = kVersionLanded + kVersionPending;

// What a branch's loader migration compares against, `if (version <
// kVersionNEXT)`; the landing replaces the name with the number.
inline constexpr int kVersionNEXT = kVersionLanded + 1;
inline constexpr int kVersionNEXT2 = kVersionLanded + 2;
inline constexpr int kVersionNEXT3 = kVersionLanded + 3;

// Positions are read and written as doubles: a JSON number is one, and a
// sketch plane at z = 13.4 read through float lands 0.4 um below a face at
// 13.4 (docs/sketching/README.md#plane). A float field takes the narrowing
// where it is assigned.
inline nlohmann::json Vec3ToJson(const glm::dvec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}
inline glm::dvec3 Vec3FromJson(const nlohmann::json& j) {
  return glm::dvec3(j.at(0).get<double>(), j.at(1).get<double>(),
                    j.at(2).get<double>());
}

// Quaternion as [x, y, z, w] (glm stores (w,x,y,z); JSON order matches the
// a2-design occurrence schema).
inline nlohmann::json QuatToJson(const glm::dquat& q) {
  return nlohmann::json::array({q.x, q.y, q.z, q.w});
}
inline glm::dquat QuatFromJson(const nlohmann::json& j) {
  return glm::dquat(j.at(3).get<double>(), j.at(0).get<double>(),
                    j.at(1).get<double>(), j.at(2).get<double>()); // (w,x,y,z)
}

inline nlohmann::json Vec2ToJson(const glm::dvec2& v) {
  return nlohmann::json::array({v.x, v.y});
}
inline glm::dvec2 Vec2FromJson(const nlohmann::json& j) {
  return glm::dvec2(j.at(0).get<double>(), j.at(1).get<double>());
}

inline nlohmann::json ElementRefJson(const ElementRef& ref) {
  return nlohmann::json{{"feature_id", ref.featureId}, {"element", ref.name}};
}
inline ElementRef ElementRefFromJson(const nlohmann::json& j) {
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
