#pragma once
// A verification receipt: what was checked, by what, and how it knows.
//
// We have a strict honesty rule for MUTATION — refuse rather than substitute,
// and a mutating RPC that leaves a feature broken says so in its own response
// — and had none for ANALYSIS. Every check reported a clean result and a
// result it could not compute in the same shape, so "swept, nothing hit" and
// "could not sweep three of the pairs" both arrived as an empty clash list.
// The drone round recorded exactly that as "EMPTY exact clash sweep".
//
// `run_motion_study` already got this right, alone and by hand: its
// collision_status is error / not_checked / incomplete / unverified / passed /
// failed, and `unverified` is neither a pass nor a failure. This is that
// discipline as a type, so every oracle states it the same way and none of
// them can forget.
//
// The shape is vcad's (crates/vcad-receipt), and two of its rules are the
// load-bearing ones:
//
//   * `Unverifiable` is not a pass and not a failure. An oracle that could not
//     run — missing input, budget spent, engine unavailable — says so, and a
//     receipt carrying one can never roll up as verified.
//   * A verdict does not say what produced it. An FEM factor of safety is a
//     PREDICTION; a bounding box is a measurement of the actual B-Rep. Both
//     can read "pass", so the basis is carried beside the verdict, and a
//     receipt whose passes rest on predictions rolls up Provisional — never
//     Verified.
#include <optional>
#include <string>
#include <vector>

namespace receipt {

// Bump on a breaking change to the wire shape.
inline constexpr const char* kSchema = "designer.receipt/1";

enum class Verdict {
  Pass,         // the oracle ran and the claim holds
  Fail,         // the oracle ran and the claim does not hold
  Unverifiable, // the oracle could not check it. Not clean, not failed
};

enum class Basis {
  Predicted, // a solver's estimate, not confirmed against the real thing
  Verified,  // the trusted oracle (the kernel, the rule) ran for real
  Measured,  // confirmed against the physical world. Nothing produces this
             // yet; it is here so a caliper reading has somewhere to land
};

// The roll-up of a whole receipt. Deliberately NOT the same vocabulary as a
// single claim: "this receipt is provisional" is not a thing one claim can be.
enum class Standing {
  Verified,    // every claim passed, every one on a verified basis
  Provisional, // every claim passed, but a pass rests on a prediction
  Failed,      // at least one claim does not hold
  Unverified,  // something could not be checked — or nothing was
};

struct Oracle {
  std::string id;      // "occ/BRepCheck_Analyzer", "designer/exact-clash"
  std::string version; // OCC version, solver build, rule-pack version
};

struct Claim {
  std::string name; // what is being claimed, in words
  Oracle oracle;
  Verdict verdict = Verdict::Unverifiable;
  Basis basis = Basis::Verified;
  std::optional<double> value; // what the oracle got, when it is a number
  std::string unit;            // "mm", "mm3", "MPa" — never implied
  std::string note;            // why. REQUIRED reading for Unverifiable

  Claim() = default;
  Claim(std::string n, Oracle o, Verdict v)
      : name(std::move(n)), oracle(std::move(o)), verdict(v) {}
};

class Receipt {
public:
  void Add(Claim c) { claims_.push_back(std::move(c)); }

  // A claim that could not be checked. Named separately because the note is
  // not optional here: an unverifiable claim with no reason is useless to the
  // caller, who has to decide whether to care.
  void Unverifiable(std::string name, Oracle oracle, std::string why) {
    Claim c(std::move(name), std::move(oracle), Verdict::Unverifiable);
    c.note = std::move(why);
    claims_.push_back(std::move(c));
  }

  const std::vector<Claim>& Claims() const { return claims_; }
  bool Empty() const { return claims_.empty(); }

  // The roll-up. Order is the point: a failure outranks an unknown, an
  // unknown outranks a prediction, and an empty receipt is never verified.
  Standing Roll() const {
    if (claims_.empty())
      return Standing::Unverified;
    bool anyFail = false, anyUnknown = false, anyPredicted = false;
    for (const Claim& c : claims_) {
      if (c.verdict == Verdict::Fail)
        anyFail = true;
      else if (c.verdict == Verdict::Unverifiable)
        anyUnknown = true;
      else if (c.basis == Basis::Predicted)
        anyPredicted = true;
    }
    if (anyFail)
      return Standing::Failed;
    if (anyUnknown)
      return Standing::Unverified;
    return anyPredicted ? Standing::Provisional : Standing::Verified;
  }

private:
  std::vector<Claim> claims_;
};

inline const char* ToString(Verdict v) {
  switch (v) {
  case Verdict::Pass:
    return "pass";
  case Verdict::Fail:
    return "fail";
  case Verdict::Unverifiable:
    return "unverifiable";
  }
  return "unverifiable";
}

inline const char* ToString(Basis b) {
  switch (b) {
  case Basis::Predicted:
    return "predicted";
  case Basis::Verified:
    return "verified";
  case Basis::Measured:
    return "measured";
  }
  return "predicted";
}

inline const char* ToString(Standing s) {
  switch (s) {
  case Standing::Verified:
    return "verified";
  case Standing::Provisional:
    return "provisional";
  case Standing::Failed:
    return "failed";
  case Standing::Unverified:
    return "unverified";
  }
  return "unverified";
}

// One line a human reads first, so a caller who ignores the claims array
// still cannot mistake an unknown for a pass.
inline std::string Summary(const Receipt& r) {
  switch (r.Roll()) {
  case Standing::Verified:
    return "verified: every claim checked and holds";
  case Standing::Provisional:
    return "PROVISIONAL: every claim holds, but at least one is a prediction "
           "the trusted oracle has not confirmed";
  case Standing::Failed:
    return "FAILED: at least one claim does not hold";
  case Standing::Unverified:
    return r.Empty() ? "UNVERIFIED: nothing was checked"
                     : "UNVERIFIED: at least one claim could not be checked — "
                       "this is not a clean result";
  }
  return "UNVERIFIED";
}

} // namespace receipt
