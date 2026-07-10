#include "core/Expression.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

double Eval(const std::string& text, const SymbolTable& symbols = {}) {
  ExprError err;
  const auto value = ExpressionEngine::Default().EvaluateText(text, symbols, &err);
  INFO(text << " -> " << err.message);
  REQUIRE(value.has_value());
  return *value;
}

ExprError EvalError(const std::string& text, const SymbolTable& symbols = {}) {
  ExprError err;
  const auto value = ExpressionEngine::Default().EvaluateText(text, symbols, &err);
  INFO(text);
  REQUIRE_FALSE(value.has_value());
  return err;
}

}  // namespace

TEST_CASE("expression arithmetic and precedence", "[expression]") {
  CHECK_THAT(Eval("2"), WithinRel(2.0));
  CHECK_THAT(Eval("1.5e2"), WithinRel(150.0));
  CHECK_THAT(Eval(".5"), WithinRel(0.5));
  CHECK_THAT(Eval("1 + 2 * 3"), WithinRel(7.0));
  CHECK_THAT(Eval("(1 + 2) * 3"), WithinRel(9.0));
  CHECK_THAT(Eval("10 - 4 - 3"), WithinRel(3.0));  // left-assoc
  CHECK_THAT(Eval("12 / 4 / 3"), WithinRel(1.0));  // left-assoc
  CHECK_THAT(Eval("7 / 2"), WithinRel(3.5));
}

TEST_CASE("expression power and unary minus conventions", "[expression]") {
  // ^ is right-associative and binds tighter than unary minus.
  CHECK_THAT(Eval("2^3^2"), WithinRel(512.0));
  CHECK_THAT(Eval("-2^2"), WithinRel(-4.0));
  CHECK_THAT(Eval("(-2)^2"), WithinRel(4.0));
  CHECK_THAT(Eval("2^-3"), WithinRel(0.125));
  CHECK_THAT(Eval("--3"), WithinRel(3.0));
  CHECK_THAT(Eval("2 * -3"), WithinRel(-6.0));
}

TEST_CASE("expression functions use degrees", "[expression]") {
  CHECK_THAT(Eval("sin(90)"), WithinRel(1.0));
  CHECK_THAT(Eval("cos(180)"), WithinRel(-1.0));
  CHECK_THAT(Eval("tan(45)"), WithinRel(1.0));
  CHECK_THAT(Eval("sqrt(16)"), WithinRel(4.0));
  CHECK_THAT(Eval("abs(-3.5)"), WithinRel(3.5));
  CHECK_THAT(Eval("floor(2.9)"), WithinRel(2.0));
  CHECK_THAT(Eval("ceil(2.1)"), WithinRel(3.0));
  CHECK_THAT(Eval("min(3, 1, 2)"), WithinRel(1.0));
  CHECK_THAT(Eval("max(3, 1, 2)"), WithinRel(3.0));
  CHECK_THAT(Eval("pi"), WithinRel(3.14159265358979323846));
  CHECK_THAT(Eval("sin(30)"), WithinAbs(0.5, 1e-12));
}

TEST_CASE("expression variables", "[expression]") {
  const SymbolTable symbols = {{"width", 20.0}, {"count", 4.0}};
  CHECK_THAT(Eval("width * 2", symbols), WithinRel(40.0));
  CHECK_THAT(Eval("width / count", symbols), WithinRel(5.0));
  CHECK_THAT(Eval("width", symbols), WithinRel(20.0));
}

TEST_CASE("expression parse errors carry a span", "[expression]") {
  const auto& engine = ExpressionEngine::Default();

  ExprError err;
  CHECK(engine.Compile("", &err) == nullptr);

  CHECK(engine.Compile("1 +", &err) == nullptr);

  CHECK(engine.Compile("width $ 2", &err) == nullptr);
  CHECK(err.offset == 6);

  CHECK(engine.Compile("1 + foo(2)", &err) == nullptr);
  CHECK(err.message.find("unknown function") != std::string::npos);
  CHECK(err.offset == 4);

  CHECK(engine.Compile("sin(1, 2)", &err) == nullptr);
  CHECK(engine.Compile("min(1)", &err) == nullptr);
  CHECK(engine.Compile("(1 + 2", &err) == nullptr);
  CHECK(engine.Compile("1 2", &err) == nullptr);
}

TEST_CASE("expression evaluation errors", "[expression]") {
  CHECK(EvalError("nope").message.find("unknown variable 'nope'") !=
        std::string::npos);
  CHECK(EvalError("1 / 0").message.find("division by zero") !=
        std::string::npos);
  CHECK(EvalError("sqrt(-1)").message.find("sqrt") != std::string::npos);
  // Overflow must surface as an error, never as inf reaching geometry.
  CHECK(EvalError("10^400").message.find("finite") != std::string::npos);

  // The offending sub-expression is spanned: "2 * (1/0)" points at 1/0.
  const ExprError err = EvalError("2 * (1 / 0)");
  CHECK(err.offset == 5);
}

TEST_CASE("expression dependencies", "[expression]") {
  ExprError err;
  const auto compiled = ExpressionEngine::Default().Compile(
      "width * 2 + sin(angle) - width + pi", &err);
  REQUIRE(compiled != nullptr);
  const std::set<std::string> expected = {"width", "angle"};
  CHECK(compiled->Dependencies() == expected);  // no functions, no pi
}

TEST_CASE("expression rename preserves formatting", "[expression]") {
  const auto& engine = ExpressionEngine::Default();
  CHECK(engine.RenameIdentifier("width * 2", "width", "w") == "w * 2");
  CHECK(engine.RenameIdentifier("( width+width )*width2", "width", "len") ==
        "( len+len )*width2");  // width2 is a different identifier
  CHECK(engine.RenameIdentifier("sin(width)", "sin", "cos") ==
        "cos(width)");  // token-level: call sites rename too (names are
                        // reserved, so this only matters for robustness)
  CHECK(engine.RenameIdentifier("a + b", "width", "w") == "a + b");
  // Untokenizable text comes back unchanged rather than mangled.
  CHECK(engine.RenameIdentifier("width $ 2", "width", "w") == "width $ 2");
}

TEST_CASE("expression parameter name validation", "[expression]") {
  const auto& engine = ExpressionEngine::Default();
  CHECK(engine.IsValidParameterName("width"));
  CHECK(engine.IsValidParameterName("_x2"));
  CHECK(engine.IsValidParameterName("hole_r"));
  CHECK_FALSE(engine.IsValidParameterName(""));
  CHECK_FALSE(engine.IsValidParameterName("2x"));
  CHECK_FALSE(engine.IsValidParameterName("a-b"));
  CHECK_FALSE(engine.IsValidParameterName("a b"));
  CHECK_FALSE(engine.IsValidParameterName("pi"));
  CHECK_FALSE(engine.IsValidParameterName("sin"));
}
