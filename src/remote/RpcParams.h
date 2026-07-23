#pragma once

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

namespace remote {

// A read-through view over an RPC request's `params` that RECORDS which
// top-level keys a handler actually consumed. The dispatcher then rejects a
// request carrying a key no handler read — a typo'd optional parameter (the
// `named` vs `name` class) that the old `params.value(...)` path silently
// ignored, so a mistake looked like a successful call that did nothing.
//
// The API is exactly the nlohmann::json subset the handlers use — at / value /
// contains — so every call site compiles unchanged; only the parameter TYPE
// moves from `const json&` to `const RpcParams&`. We validate the TOP LEVEL
// only: at() returns raw json for nested objects (a feature spec, a constraint
// list), which carry their own schema elsewhere. A handler that consumes the
// whole object opaquely (its params object IS a free-form spec) calls raw(),
// which opts that request out of the check entirely.
class RpcParams {
public:
  explicit RpcParams(const nlohmann::json& params) : params_(params) {}

  template <class K>
  bool contains(const K& key) const {
    seen_.emplace(key);
    return params_.contains(key);
  }

  template <class K>
  const nlohmann::json& at(const K& key) const {
    seen_.emplace(key);
    return params_.at(key);
  }

  template <class K>
  const nlohmann::json& operator[](const K& key) const {
    seen_.emplace(key);
    return params_[key];
  }

  template <class K, class T>
  auto value(const K& key, const T& fallback) const {
    seen_.emplace(key);
    return params_.value(key, fallback);
  }

  // Opaque passthrough: the caller reads arbitrary keys off the raw object
  // itself, so its vocabulary is unknown — disable the unknown-key check for
  // this request rather than flag every key as unread.
  const nlohmann::json& raw() const {
    passthrough_ = true;
    return params_;
  }

  // Top-level keys present in the request that no handler read. Empty when a
  // passthrough consumer ran, or when params isn't an object.
  std::vector<std::string> UnknownKeys() const {
    std::vector<std::string> unknown;
    if (passthrough_ || !params_.is_object())
      return unknown;
    for (auto it = params_.begin(); it != params_.end(); ++it)
      if (seen_.find(it.key()) == seen_.end())
        unknown.push_back(it.key());
    return unknown;
  }

  // The keys a handler DID look up — the method's accepted vocabulary, used to
  // offer a "did you mean" for a rejected key.
  const std::set<std::string>& SeenKeys() const { return seen_; }

private:
  const nlohmann::json& params_;
  mutable std::set<std::string> seen_;
  mutable bool passthrough_ = false;
};

} // namespace remote
