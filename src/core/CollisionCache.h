#pragma once

#include "core/Measure.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>

namespace collide {

// Session-local exact overlap results. Keys use exact input matrices, never
// rounded poses. Retaining shapes prevents address reuse from creating a hit.
// World matrices deliberately over-invalidate on common rigid movement; this
// avoids introducing inverse/rounding error into an exact acceptance gate.
// The fixed overlap algorithm/tolerance is part of this process's code. This
// cache must not be persisted as-is across kernel/application versions.
class ExactContactCache {
public:
  struct Answer {
    std::optional<Measure::SetContact> contact;
    bool cached = false;
  };
  explicit ExactContactCache(size_t capacity = 50000) : capacity_(capacity) {}

  Answer Check(const CADShape& a, const glm::mat4& worldA, const CADShape& b,
               const glm::mat4& worldB) {
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
          const float value = (*world)[c][r];
          finite = finite && std::isfinite(value);
          key.poses[i++] = std::bit_cast<uint32_t>(value);
        }
    if (finite) {
      std::lock_guard lock(mutex_);
      if (const auto it = entries_.find(key); it != entries_.end() &&
                                              it->second.a.SameGeometryAs(a) &&
                                              it->second.b.SameGeometryAs(b))
        return {it->second.contact, true};
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
    }
    return {contact, false};
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    order_.clear();
  }
  size_t Size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
  }

private:
  struct Key {
    uintptr_t a = 0, b = 0;
    uint64_t localA = 0, localB = 0;
    std::array<uint32_t, 32> poses{};
    auto operator<=>(const Key&) const = default;
  };
  struct Entry {
    CADShape a, b;
    Measure::SetContact contact;
  };
  size_t capacity_;
  mutable std::mutex mutex_;
  std::map<Key, Entry> entries_;
  std::deque<Key> order_;
};
} // namespace collide
