// The roll-up rules are the whole point of a receipt, so they are gated
// (core/Receipt.h, docs/peer-survey-plan.md W2).
//
// Every one of these is a case where the old shape — a bare `ok` flag, or an
// empty list of problems — would have read as a clean result.
#include "core/Receipt.h"

#include <catch2/catch_test_macros.hpp>

using namespace receipt;

namespace {
Oracle kernel() { return {"occ/BRepCheck_Analyzer", "7.9"}; }

Claim pass(const char* name, Basis b = Basis::Verified) {
  Claim c(name, kernel(), Verdict::Pass);
  c.basis = b;
  return c;
}
} // namespace

TEST_CASE("an empty receipt is never verified", "[receipt]") {
  Receipt r;
  // The case that matters: an oracle that checked NOTHING must not be
  // indistinguishable from one that checked everything and found nothing
  // wrong. "EMPTY exact clash sweep" was exactly this.
  CHECK(r.Roll() == Standing::Unverified);
  CHECK(std::string(Summary(r)).find("nothing was checked") !=
        std::string::npos);
}

TEST_CASE("all verified passes roll up verified", "[receipt]") {
  Receipt r;
  r.Add(pass("every body is a valid B-Rep"));
  r.Add(pass("no feature errored"));
  CHECK(r.Roll() == Standing::Verified);
}

TEST_CASE("a prediction can never roll up as verified", "[receipt]") {
  Receipt r;
  r.Add(pass("every body is a valid B-Rep"));
  r.Add(pass("factor of safety >= 2", Basis::Predicted));
  // Both claims PASS. The receipt is still not verified, because one of them
  // is a solver's estimate and nothing has confirmed it against the part.
  CHECK(r.Roll() == Standing::Provisional);
  CHECK(std::string(Summary(r)).find("PROVISIONAL") != std::string::npos);
}

TEST_CASE("an unverifiable claim outranks a prediction", "[receipt]") {
  Receipt r;
  r.Add(pass("factor of safety >= 2", Basis::Predicted));
  r.Unverifiable("no pair interferes", {"designer/exact-clash", "1"},
                 "3 of 91 pairs were not checked: deadline_s spent");
  CHECK(r.Roll() == Standing::Unverified);
}

TEST_CASE("a failure outranks everything", "[receipt]") {
  Receipt r;
  r.Add(pass("every body is a valid B-Rep"));
  r.Unverifiable("no pair interferes", {"designer/exact-clash", "1"},
                 "budget spent");
  Claim bad("wall thickness >= 0.8", kernel(), Verdict::Fail);
  r.Add(bad);
  CHECK(r.Roll() == Standing::Failed);
}

TEST_CASE("an unverifiable claim must carry its reason", "[receipt]") {
  Receipt r;
  r.Unverifiable("no pair interferes", {"designer/exact-clash", "1"},
                 "deadline_s spent after 88 of 91 pairs");
  REQUIRE(r.Claims().size() == 1);
  // The note is what lets the caller decide whether to care. An unknown with
  // no reason is no more useful than the empty list it replaced.
  CHECK_FALSE(r.Claims()[0].note.empty());
  CHECK(r.Claims()[0].verdict == Verdict::Unverifiable);
}

TEST_CASE("a value carries its unit", "[receipt]") {
  Claim c("interference volume", {"designer/exact-clash", "1"}, Verdict::Pass);
  c.value = 0.0;
  c.unit = "mm3";
  Receipt r;
  r.Add(c);
  REQUIRE(r.Claims()[0].value.has_value());
  CHECK(r.Claims()[0].unit == "mm3");
}
