// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "remote/InspectionContracts.h"
#include "remote/RpcErrors.h"

#include <Standard_ConstructionError.hxx>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// What the dispatcher's catch ladder makes of each exception family
// (docs/scripting/rpc.md#structured-errors): a handler that fell over is
// `internal`, a deliberate refusal is not. The ladder itself calls
// DescribeCurrentException; tests/hostile_params_smoke.py --self-test drives
// the same families through a running server (`debug_fault`).

using nlohmann::json;

namespace {

template <class Throw> remote::CaughtError Caught(Throw&& thrower) {
  try {
    thrower();
  } catch (...) {
    return remote::DescribeCurrentException("m");
  }
  FAIL("nothing was thrown");
  return {};
}

std::string Code(const remote::CaughtError& c) {
  return c.diagnostic.is_null() ? std::string()
                                : c.diagnostic.value("code", "");
}

} // namespace

TEST_CASE("an exception from a handler that fell over is internal",
          "[remote][rpc]") {
  const auto range = Caught([] { (void)std::vector<int>{}.at(3); });
  CHECK(Code(range) == "internal");
  CHECK(range.diagnostic["observed"] == "std::out_of_range");
  CHECK(range.diagnostic["operation"] == "m");

  CHECK(Code(Caught([] { (void)std::optional<int>{}.value(); })) == "internal");
  CHECK(Code(Caught([] { (void)json::object().at("x"); })) == "internal");
  CHECK(Code(Caught([] { throw std::invalid_argument("x"); })) == "internal");
  CHECK(Code(Caught([] { throw 42; })) == "internal");

  const auto occ =
      Caught([] { throw Standard_ConstructionError("zero direction"); });
  CHECK(Code(occ) == "internal");
  CHECK(occ.diagnostic["observed"] == "Standard_ConstructionError");
  CHECK(occ.message == "OpenCASCADE error: zero direction");
}

TEST_CASE("a deliberate refusal is not internal", "[remote][rpc]") {
  const auto said = Caught([] { throw std::runtime_error("no sketch 9"); });
  CHECK(said.diagnostic.is_null());
  CHECK(said.message == "no sketch 9");
  // nlohmann's wrong-type error is the designed type-error path.
  CHECK(Code(Caught([] { (void)json("x").get<int>(); })).empty());
  CHECK(Code(Caught([] {
          throw std::filesystem::filesystem_error(
              "cannot open", std::make_error_code(std::errc::io_error));
        })).empty());
  // A contract refusal keeps its own diagnostic.
  const auto contract = Caught([] {
    throw remote::ContractError("m", "invalid_argument", "/params/x", "int",
                                "y");
  });
  CHECK(Code(contract) == "invalid_argument");
}

TEST_CASE("every non-finite number in an answer is named by its pointer",
          "[remote][rpc]") {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  json answer = {{"id", 1},
                 {"result",
                  {{"bodies", json::array({{{"volume", nan}}, {{"v", 1.0}}})},
                   {"a/b~c", -inf},
                   {"ok", 2.5}}}};
  const std::vector<std::string> bad = remote::NonFinitePointers(answer);
  REQUIRE(bad.size() == 2);
  CHECK(bad[0] == "/result/a~1b~0c");
  CHECK(bad[1] == "/result/bodies/0/volume");
  CHECK(remote::NonFinitePointers(json{{"x", 1e308}}).empty());
  CHECK(remote::NonFinitePointers(json(nan)) == std::vector<std::string>{""});
}
