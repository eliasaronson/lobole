// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The input hash of a study run (docs/simulation/README.md#studies-as-jobs):
// 64 bits over EXACTLY what produces the answer, so a run whose hash matches
// a stored result is answered from it rather than computed again.
//
// What goes in is the caller's business (motion::InputKey,
// fem::StudyInputKey); this is the hasher and the pieces they share. The
// geometry is the exact B-Rep as BinTools writes it, without triangulation
// (a mesh hung on a shape is not geometry), which is how CAM keys its
// surfaces across sessions too (cam::BodiesContentKey). A body's name and
// material go in beside it: a clash names its bodies, and a solve reads the
// material's numbers.
//
// FNV-1a, 64 bits: a cache key, not a signature. Two different inputs
// colliding would hand back the wrong run, at odds of one in 2^64 per pair.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class CADShape;
struct Body;

namespace inputhash {

class Hasher {
public:
  void Bytes(const void* p, size_t n);
  void U64(uint64_t v) { Bytes(&v, sizeof v); }
  // A double by its bits: 0.1 + 0.2 and 0.3 are different inputs.
  void F64(double v);
  void Str(std::string_view s);
  // Canonical text: nlohmann's object keys are sorted, numbers shortest
  // round-trip, so equal values dump equal bytes.
  void Json(const nlohmann::json& j);
  // The exact B-Rep (BinTools, no triangles), memoized per shape identity
  // (GeometryId + placement) so hashing an assembly that places one bolt
  // forty times writes it once.
  void Shape(const CADShape& shape);
  // Each body: name, material (with the library's numbers for it),
  // reference flag, the thread regions a contact check reads, the face
  // names a load resolves through, and the shape.
  void Bodies(const std::vector<Body>& bodies);
  uint64_t Digest() const { return h_; }

private:
  uint64_t h_ = 1469598103934665603ULL;
};

// "0x" + 16 hex digits: how a hash goes on the wire and to disk (a JSON
// number past 2^53 is not safe in every reader).
std::string Hex(uint64_t hash);
// The inverse; false on anything that is not that form.
[[nodiscard]] bool FromHex(const std::string& text, uint64_t& out);

// The session's results by input hash: the last `capacity` stored, oldest
// dropped first. Main thread only.
template <class T> class Store {
public:
  explicit Store(size_t capacity) : capacity_(capacity) {}
  std::shared_ptr<const T> Find(uint64_t hash) const {
    const auto it = entries_.find(hash);
    return it == entries_.end() ? nullptr : it->second;
  }
  void Put(uint64_t hash, std::shared_ptr<const T> value) {
    if (hash == 0 || capacity_ == 0)
      return;
    if (!entries_.contains(hash)) {
      while (entries_.size() >= capacity_) {
        entries_.erase(order_.front());
        order_.pop_front();
      }
      order_.push_back(hash);
    }
    entries_[hash] = std::move(value);
  }
  void Clear() {
    entries_.clear();
    order_.clear();
  }
  size_t Size() const { return entries_.size(); }

private:
  size_t capacity_;
  std::map<uint64_t, std::shared_ptr<const T>> entries_;
  std::deque<uint64_t> order_;
};

} // namespace inputhash
