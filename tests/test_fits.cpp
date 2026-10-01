#include "core/FitStandards.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using Catch::Approx;

namespace {
// The tables are published in micrometres and the API answers in mm; every
// expectation below is written in micrometres so it can be read straight off
// a standards table without a mental conversion.
constexpr double um = 0.001;

fits::Band Band(double nominal, const char* cls) {
  std::string why;
  const auto b = fits::Limits(nominal, cls, &why);
  INFO(why);
  REQUIRE(b.has_value());
  return *b;
}
} // namespace

// Spot values off ISO 286-1. These are the whole point of the file: the tables
// are transcribed data, and a transcription error produces a band that looks
// entirely reasonable and sends a part to be made wrong.
TEST_CASE("ISO 286 hole bands match published values", "[fits]") {
  auto h7_20 = Band(20.0, "H7");
  CHECK(h7_20.lower == Approx(0.0));
  CHECK(h7_20.upper == Approx(21 * um));
  CHECK(h7_20.grade == 7);
  CHECK(h7_20.it == Approx(21 * um));

  CHECK(Band(50.0, "H7").upper == Approx(25 * um));
  CHECK(Band(100.0, "H7").upper == Approx(35 * um));
  CHECK(Band(10.0, "H9").upper == Approx(36 * um));

  // Mirrored letters (EI = -es) — exact for A..H.
  auto g7 = Band(20.0, "G7");
  CHECK(g7.lower == Approx(7 * um));
  CHECK(g7.upper == Approx(28 * um));
  auto f8 = Band(20.0, "F8");
  CHECK(f8.lower == Approx(20 * um));
  CHECK(f8.upper == Approx(53 * um));

  // The delta correction, which is the half most likely to be got wrong.
  auto k7 = Band(20.0, "K7");
  CHECK(k7.upper == Approx(6 * um));
  CHECK(k7.lower == Approx(-15 * um));
  auto m7 = Band(20.0, "M7");
  CHECK(m7.upper == Approx(0.0));
  CHECK(m7.lower == Approx(-21 * um));
  auto n7 = Band(20.0, "N7");
  CHECK(n7.upper == Approx(-7 * um));
  CHECK(n7.lower == Approx(-28 * um));
  auto p7 = Band(20.0, "P7");
  CHECK(p7.upper == Approx(-14 * um));
  CHECK(p7.lower == Approx(-35 * um));
}

TEST_CASE("ISO 286 shaft bands match published values", "[fits]") {
  auto g6 = Band(20.0, "g6");
  CHECK(g6.upper == Approx(-7 * um));
  CHECK(g6.lower == Approx(-20 * um));

  auto h6 = Band(20.0, "h6");
  CHECK(h6.upper == Approx(0.0));
  CHECK(h6.lower == Approx(-13 * um));

  auto f7 = Band(20.0, "f7");
  CHECK(f7.upper == Approx(-20 * um));
  CHECK(f7.lower == Approx(-41 * um));

  auto k6 = Band(20.0, "k6");
  CHECK(k6.lower == Approx(2 * um));
  CHECK(k6.upper == Approx(15 * um));

  auto m6 = Band(25.0, "m6");
  CHECK(m6.lower == Approx(8 * um));
  CHECK(m6.upper == Approx(21 * um));

  auto n6 = Band(20.0, "n6");
  CHECK(n6.lower == Approx(15 * um));
  CHECK(n6.upper == Approx(28 * um));

  auto p6 = Band(50.0, "p6");
  CHECK(p6.lower == Approx(26 * um));
  CHECK(p6.upper == Approx(42 * um));

  // js is symmetric about nominal and hangs off no fundamental deviation.
  auto js6 = Band(100.0, "js6");
  CHECK(js6.upper == Approx(11 * um));
  CHECK(js6.lower == Approx(-11 * um));
}

// k is the one letter whose deviation is grade-dependent: the tabulated value
// holds for IT4..IT7 and k is h everywhere else. Reusing it at IT9 shifts the
// band by 2 um, which reads as a rounding difference rather than a bug.
TEST_CASE("k outside IT4..IT7 coincides with h", "[fits]") {
  CHECK(Band(20.0, "k9").lower == Approx(0.0));
  CHECK(Band(20.0, "k9").upper == Approx(52 * um));
  CHECK(Band(20.0, "k6").lower == Approx(2 * um));
}

TEST_CASE("Size steps are inclusive at the top", "[fits]") {
  // 30 belongs to 18..30; 30.001 to 30..50. Getting this backwards moves
  // every band at a step boundary.
  CHECK(Band(30.0, "H7").upper == Approx(21 * um));
  CHECK(Band(30.001, "H7").upper == Approx(25 * um));
  CHECK(Band(3.0, "H7").upper == Approx(10 * um));
  CHECK(Band(500.0, "H7").upper == Approx(63 * um));
}

TEST_CASE("Textbook fits come out where the books put them", "[fits]") {
  std::string why;

  // H7/g6 at 20: the standard close running fit, 7..41 um clearance.
  auto running = fits::Fit(20.0, "H7", "g6", &why);
  INFO(why);
  REQUIRE(running.has_value());
  CHECK(running->minClearance == Approx(7 * um));
  CHECK(running->maxClearance == Approx(41 * um));
  CHECK(running->kind == fits::FitKind::Clearance);

  // H7/p6 at 50: a press fit, 1..42 um INTERFERENCE, so both bounds negative.
  auto press = fits::Fit(50.0, "H7", "p6", &why);
  INFO(why);
  REQUIRE(press.has_value());
  CHECK(press->minClearance == Approx(-42 * um));
  CHECK(press->maxClearance == Approx(-1 * um));
  CHECK(press->kind == fits::FitKind::Interference);

  // H7/k6 at 20: a transition fit — the clearance range straddles zero, which
  // is what makes it one. The kind is decided by the bounds, not the letter.
  auto transition = fits::Fit(20.0, "H7", "k6", &why);
  INFO(why);
  REQUIRE(transition.has_value());
  CHECK(transition->minClearance == Approx(-15 * um));
  CHECK(transition->maxClearance == Approx(19 * um));
  CHECK(transition->kind == fits::FitKind::Transition);

  // H11/c11-style loose running, with the letters we do carry: H11/d11.
  auto loose = fits::Fit(20.0, "H11", "d11", &why);
  INFO(why);
  REQUIRE(loose.has_value());
  CHECK(loose->minClearance == Approx(65 * um));
  CHECK(loose->kind == fits::FitKind::Clearance);
}

TEST_CASE("Fits refuse rather than substitute", "[fits]") {
  std::string why;

  // Swapped sides. The mirror image solves perfectly well and is wrong.
  CHECK_FALSE(fits::Fit(20.0, "g6", "H7", &why).has_value());
  CHECK(why.find("SHAFT") != std::string::npos);
  CHECK_FALSE(fits::Fit(20.0, "H7", "G6", &why).has_value());
  CHECK(why.find("HOLE") != std::string::npos);

  // Sizes off the end of the table are not extrapolated.
  CHECK_FALSE(fits::Limits(501.0, "H7", &why).has_value());
  CHECK(why.find("500") != std::string::npos);
  CHECK_FALSE(fits::Limits(0.0, "H7", &why).has_value());
  CHECK_FALSE(fits::Limits(-5.0, "H7", &why).has_value());

  // Grades and letters outside the tables name what IS carried.
  CHECK_FALSE(fits::Limits(20.0, "H3", &why).has_value());
  CHECK(why.find("IT4") != std::string::npos);
  CHECK_FALSE(fits::Limits(20.0, "H14", &why).has_value());
  CHECK_FALSE(fits::Limits(20.0, "s6", &why).has_value());
  CHECK(why.find("shafts") != std::string::npos);

  // The delta correction is only defined so far, and past it we stop.
  CHECK_FALSE(fits::Limits(20.0, "P8", &why).has_value());
  CHECK(why.find("IT7") != std::string::npos);
  CHECK_FALSE(fits::Limits(20.0, "N9", &why).has_value());
  CHECK(fits::Limits(20.0, "N8", &why).has_value());

  // Malformed classes.
  CHECK_FALSE(fits::Limits(20.0, "7", &why).has_value());
  CHECK_FALSE(fits::Limits(20.0, "H", &why).has_value());
  CHECK_FALSE(fits::Limits(20.0, "", &why).has_value());
  CHECK_FALSE(fits::Limits(20.0, "Js6", &why).has_value());
  CHECK(why.find("mixes cases") != std::string::npos);
}

TEST_CASE("A band is a band, wherever it sits", "[fits]") {
  // Min/Max apply the deviations to a nominal; the arithmetic is trivial and
  // the sign convention is the part worth pinning.
  auto shaft = Band(20.0, "g6");
  CHECK(shaft.Min(20.0) == Approx(19.980));
  CHECK(shaft.Max(20.0) == Approx(19.993));
  auto hole = Band(20.0, "H7");
  CHECK(hole.Min(20.0) == Approx(20.0));
  CHECK(hole.Max(20.0) == Approx(20.021));
}

// J and j, cell for cell off ISO 286-2:2010 Tables 8 (holes) and 24
// (shafts) at the sizes a bearing seat lives at, plus the edges of what the
// standard defines. Each grade has its own deviation, so every grade is
// pinned.
TEST_CASE("ISO 286 J and j bands match the published tables", "[fits]") {
  const auto check = [](double size, const char* cls, int upper, int lower) {
    INFO(cls << " at " << size);
    const fits::Band b = Band(size, cls);
    CHECK(b.upper == Approx(upper * um).margin(1e-9));
    CHECK(b.lower == Approx(lower * um).margin(1e-9));
  };
  check(2.0, "J6", 2, -4);
  check(2.0, "J7", 4, -6);
  check(2.0, "J8", 6, -8);
  check(5.0, "J7", 6, -6); // printed +-6, "identical with JS7"
  check(20.0, "J6", 8, -5);
  check(20.0, "J7", 12, -9);
  check(20.0, "J8", 20, -13);
  check(32.0, "J7", 14, -11); // a 6002's outside
  check(50.0, "J7", 14, -11); // 50 is the top of 30..50
  check(62.0, "J7", 18, -12);
  check(450.0, "J8", 66, -31);

  check(2.0, "j5", 2, -2); // printed +-2
  check(2.0, "j6", 4, -2);
  check(2.0, "j7", 6, -4);
  check(2.0, "j8", 8, -6);
  check(15.0, "j5", 5, -3); // a 6002's bore
  check(15.0, "j6", 8, -3);
  check(20.0, "j6", 9, -4);
  check(20.0, "j7", 13, -8);
  check(50.0, "j6", 11, -5);
  check(300.0, "j6", 16, -16); // printed +-16
  check(300.0, "j7", 26, -26); // printed +-26
  check(450.0, "j7", 31, -32);

  // J7/h6 on a 6002's Ø32 outside is a transition fit.
  std::string why;
  const auto pair = fits::Fit(32.0, "J7", "h6", &why);
  REQUIRE(pair.has_value());
  CHECK(pair->kind == fits::FitKind::Transition);
  CHECK(pair->minClearance == Approx(-11 * um));
  CHECK(pair->maxClearance == Approx(30 * um));
}

TEST_CASE("J and j refuse the grades and sizes the standard leaves blank",
          "[fits]") {
  std::string why;
  CHECK_FALSE(fits::Limits(20.0, "j8", &why).has_value());
  CHECK(why.find("j8 exists only up to 3 mm") != std::string::npos);
  CHECK(why.find("js8") != std::string::npos);
  CHECK_FALSE(fits::Limits(20.0, "j9", &why).has_value());
  CHECK(why.find("j5, j6 and j7") != std::string::npos);
  CHECK_FALSE(fits::Limits(20.0, "J9", &why).has_value());
  CHECK(why.find("JS9") != std::string::npos);
  CHECK_FALSE(fits::Limits(20.0, "J5", &why).has_value());
  CHECK(why.find("J6, J7 and J8") != std::string::npos);
  const auto holes = fits::HoleLetters();
  CHECK(std::find(holes.begin(), holes.end(), "J") != holes.end());
  const auto shafts = fits::ShaftLetters();
  CHECK(std::find(shafts.begin(), shafts.end(), "j") != shafts.end());
}
