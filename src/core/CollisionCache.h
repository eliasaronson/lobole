// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Measure.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include <glm/glm.hpp>

namespace collide {

// Session-local exact overlap results. Keys use exact input matrices, never
// rounded poses. Retaining shapes prevents address reuse from creating a hit.
// The fixed overlap algorithm/tolerance is part of this process's code. This
// cache must not be persisted as-is across kernel/application versions.
//
// A CLEAR answer is also found by the pair's RELATIVE pose (B in A's frame),
// matched to within kRelativeTolerance per matrix entry (1e-9 mm in
// translation; OpenCASCADE's own confusion distance is 1e-7 mm), so a pair
// that moved together, or a re-run whose solve landed a hair away from the
// last one, is a hit. Only clear answers: an overlap's centroid and bounds
// are in world space and would have to be moved, and a clash is rare enough
// to recompute
// (docs/assemblies/README.md#exact-contact-reuse-across-motion-runs).
class ExactContactCache {
public:
  struct Answer {
    std::optional<Measure::SetContact> contact;
    bool cached = false;
  };
  explicit ExactContactCache(size_t capacity = 50000) : capacity_(capacity) {}

  Answer Check(const CADShape& a, const glm::dmat4& worldA, const CADShape& b,
               const glm::dmat4& worldB) {
    Key key{};
    key.a = reinterpret_cast<uintptr_t>(a.GeometryId());
    key.b = reinterpret_cast<uintptr_t>(b.GeometryId());
    key.localA = a.PlacementId();
    key.localB = b.PlacementId();
    bool finite = key.a != 0 && key.b != 0;
    size_t i = 0;
    for (const auto* world : {&worldA, &worldB})
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
          const double value = (*world)[c][r];
          finite = finite && std::isfinite(value);
          key.poses[i++] = std::bit_cast<uint64_t>(value);
        }
    const RelKey rkey{key.a, key.b, key.localA, key.localB};
    const glm::dmat4 rel = glm::inverse(worldA) * worldB;
    if (finite) {
      std::lock_guard lock(mutex_);
      if (const auto it = entries_.find(key); it != entries_.end() &&
                                              it->second.a.SameGeometryAs(a) &&
                                              it->second.b.SameGeometryAs(b))
        return {it->second.contact, true};
      if (const auto it = relative_.find(rkey); it != relative_.end())
        for (const RelEntry& e : it->second)
          if (SameRelative(e.rel, rel) && e.a.SameGeometryAs(a) &&
              e.b.SameGeometryAs(b))
            return {e.contact, true};
    }
    std::vector<Body> ba(1), bb(1);
    ba[0].shape = a;
    bb[0].shape = b;
    auto contact = Measure::ContactBetween(ba, worldA, bb, worldB, false);
    if (contact && finite && capacity_ > 0) {
      std::lock_guard lock(mutex_);
      if (!entries_.contains(key)) {
        while (entries_.size() >= capacity_) {
          entries_.erase(order_.front());
          order_.pop_front();
        }
        entries_.emplace(key, Entry{a, b, *contact});
        order_.push_back(key);
      } else {
        entries_.at(key) = Entry{a, b, *contact};
      }
      if (!contact->overlaps && contact->volume <= 1e-9) {
        while (relativeCount_ >= capacity_ && !relativeOrder_.empty()) {
          auto& bucket = relative_[relativeOrder_.front()];
          if (!bucket.empty()) {
            bucket.erase(bucket.begin());
            relativeCount_--;
          }
          if (bucket.empty())
            relative_.erase(relativeOrder_.front());
          relativeOrder_.pop_front();
        }
        relative_[rkey].push_back(RelEntry{rel, a, b, *contact});
        relativeOrder_.push_back(rkey);
        relativeCount_++;
      }
    }
    return {contact, false};
  }

  // Entries of a relative pose B-in-A agree when every matrix entry does to
  // within this.
  static constexpr double kRelativeTolerance = 1e-9;
  static bool SameRelative(const glm::dmat4& x, const glm::dmat4& y) {
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        if (!(std::abs(x[c][r] - y[c][r]) <= kRelativeTolerance))
          return false;
    return true;
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    order_.clear();
    relative_.clear();
    relativeOrder_.clear();
    relativeCount_ = 0;
  }
  size_t Size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
  }

private:
  struct Key {
    uintptr_t a = 0, b = 0;
    uint64_t localA = 0, localB = 0;
    std::array<uint64_t, 32> poses{};
    auto operator<=>(const Key&) const = default;
  };
  struct Entry {
    CADShape a, b;
    Measure::SetContact contact;
  };
  struct RelKey {
    uintptr_t a = 0, b = 0;
    uint64_t localA = 0, localB = 0;
    auto operator<=>(const RelKey&) const = default;
  };
  struct RelEntry {
    glm::dmat4 rel;
    CADShape a, b;
    Measure::SetContact contact;
  };
  size_t capacity_;
  mutable std::mutex mutex_;
  std::map<Key, Entry> entries_;
  std::deque<Key> order_;
  std::map<RelKey, std::vector<RelEntry>> relative_;
  std::deque<RelKey> relativeOrder_; // insertion order, one per entry
  size_t relativeCount_ = 0;
};
} // namespace collide
