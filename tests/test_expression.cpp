#include "core/Expression.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

double Eval(const std::string& text, const SymbolTable& symbols = {}) {
  const auto value = ExpressionEngine::Default().EvaluateText(text, symbols);
  INFO(text << " -> " << (value ? std::string{} : value.error().message));
  REQUIRE(value.has_value());
  return *value;
}

ExprError EvalError(const std::string& text, const SymbolTable& symbols = {}) {
  const auto value = ExpressionEngine::Default().EvaluateText(text, symbols);
  INFO(text);
  REQUIRE_FALSE(value.has_value());
  return value.error();
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

  CHECK_FALSE(engine.Compile("").has_value());
  CHECK_FALSE(engine.Compile("1 +").has_value());

  {
    const auto r = engine.Compile("width $ 2");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().offset == 6);
  }
  {
    const auto r = engine.Compile("1 + foo(2)");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("unknown function") != std::string::npos);
    CHECK(r.error().offset == 4);
  }

  CHECK_FALSE(engine.Compile("sin(1, 2)").has_value());
  CHECK_FALSE(engine.Compile("min(1)").has_value());
  CHECK_FALSE(engine.Compile("(1 + 2").has_value());
  CHECK_FALSE(engine.Compile("1 2").has_value());
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
  const auto compiled = ExpressionEngine::Default().Compile(
      "width * 2 + sin(angle) - width + pi");
  REQUIRE(compiled.has_value());
  const std::set<std::string> expected = {"width", "angle"};
  CHECK((*compiled)->Dependencies() == expected);  // no functions, no pi
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
  // Unit names are reserved (C2) so a suffix can't be shadowed by a param.
  CHECK_FALSE(engine.IsValidParameterName("mm"));
  CHECK_FALSE(engine.IsValidParameterName("cm"));
  CHECK_FALSE(engine.IsValidParameterName("m"));
  CHECK_FALSE(engine.IsValidParameterName("in"));
  CHECK_FALSE(engine.IsValidParameterName("ft"));
  CHECK_FALSE(engine.IsValidParameterName("deg"));
  CHECK_FALSE(engine.IsValidParameterName("rad"));
  // A name that merely starts with a unit is still fine.
  CHECK(engine.IsValidParameterName("mm_gap"));
  CHECK(engine.IsValidParameterName("inner"));
}

TEST_CASE("expression unit-suffixed literals convert to base units",
          "[expression]") {
  // Length → millimetres, with and without a space.
  CHECK_THAT(Eval("10mm"), WithinRel(10.0, 1e-9));
  CHECK_THAT(Eval("10 mm"), WithinRel(10.0, 1e-9));
  CHECK_THAT(Eval("1cm"), WithinRel(10.0, 1e-9));
  CHECK_THAT(Eval("1 m"), WithinRel(1000.0, 1e-9));
  CHECK_THAT(Eval("1in"), WithinRel(25.4, 1e-9));
  CHECK_THAT(Eval("1.5in"), WithinRel(38.1, 1e-9));
  CHECK_THAT(Eval("1ft"), WithinRel(304.8, 1e-9));
  // Angle → degrees.
  CHECK_THAT(Eval("90deg"), WithinRel(90.0, 1e-9));
  CHECK_THAT(Eval("1rad"), WithinRel(57.29577951308232, 1e-9));
  // Mixed arithmetic: `10mm + 1in` = 10 + 25.4.
  CHECK_THAT(Eval("10mm + 1in"), WithinRel(35.4, 1e-9));
  // A bare number is already base units; units bind only to the literal.
  CHECK_THAT(Eval("10mm + 5"), WithinRel(15.0, 1e-9));
  // Units bind to the literal, not the whole product: 2in * 3 = 152.4.
  CHECK_THAT(Eval("2in * 3"), WithinRel(152.4, 1e-9));
  // Units in a sub-expression and with functions.
  CHECK_THAT(Eval("(1cm + 1cm) * 2"), WithinRel(40.0, 1e-9));
}

TEST_CASE("expression units win after a number, identifiers elsewhere",
          "[expression]") {
  // `2 in` is a unit suffix (50.8), NOT 2 * a variable `in` (which can't
  // exist — reserved).
  CHECK_THAT(Eval("2in"), WithinRel(50.8, 1e-9));
  // A unit name after an operator (not a number) is a plain identifier again:
  // `4 + m` is 4 plus the variable m — confirm it's an unknown-VARIABLE error
  // (an identifier), not a parse error and not a unit.
  const ExprError e = EvalError("4 + m");
  CHECK(e.message.find("unknown variable 'm'") != std::string::npos);
  // A standalone unit-like name is an identifier, not a number.
  const ExprError e2 = EvalError("mm");
  CHECK(e2.message.find("unknown variable 'mm'") != std::string::npos);
  // But directly after a number it IS a unit: `4 m` = 4000 mm.
  CHECK_THAT(Eval("4 m"), WithinRel(4000.0, 1e-9));
}

TEST_CASE("expression units are invisible to dependencies and rename",
          "[expression]") {
  const auto& engine = ExpressionEngine::Default();
  // A unit suffix never enters the dependency set.
  const auto compiled = engine.Compile("gap + 10mm");
  REQUIRE(compiled.has_value());
  CHECK((*compiled)->Dependencies() == std::set<std::string>{"gap"});
  // Rename leaves unit-bearing literals byte-for-byte untouched.
  CHECK(engine.RenameIdentifier("gap + 10mm", "gap", "g") == "g + 10mm");
  CHECK(engine.RenameIdentifier("2in * width", "width", "w") == "2in * w");
}
