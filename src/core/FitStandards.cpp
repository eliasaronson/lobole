#include "core/FitStandards.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace fits {
namespace {

// ISO 286-1 nominal size steps, "over the previous, up to and INCLUDING this"
// — so 30 is an 18..30 size and 30.001 is a 30..50 one. The first step is
// 0 < D <= 3.
constexpr std::array<double, 13> kStepMax = {3,   6,   10,  18,  30,  50, 80,
                                             120, 180, 250, 315, 400, 500};
constexpr int kSteps = 13;

// Standard tolerance grades IT4..IT13, micrometres, one row per grade.
// IT4..IT11 are where fits live; IT12/IT13 are here so a coarse general
// tolerance has somewhere to land. Grades below IT4 and above IT13 are
// deliberately absent rather than guessed.
constexpr int kGradeMin = 4, kGradeMax = 13;
constexpr int kIt[kGradeMax - kGradeMin + 1][kSteps] = {
    /* IT4  */ {3, 4, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20},
    /* IT5  */ {4, 5, 6, 8, 9, 11, 13, 15, 18, 20, 23, 25, 27},
    /* IT6  */ {6, 8, 9, 11, 13, 16, 19, 22, 25, 29, 32, 36, 40},
    /* IT7  */ {10, 12, 15, 18, 21, 25, 30, 35, 40, 46, 52, 57, 63},
    /* IT8  */ {14, 18, 22, 27, 33, 39, 46, 54, 63, 72, 81, 89, 97},
    /* IT9  */ {25, 30, 36, 43, 52, 62, 74, 87, 100, 115, 130, 140, 155},
    /* IT10 */ {40, 48, 58, 70, 84, 100, 120, 140, 160, 185, 210, 230, 250},
    /* IT11 */ {60, 75, 90, 110, 130, 160, 190, 220, 250, 290, 320, 360, 400},
    /* IT12 */
    {100, 120, 150, 180, 210, 250, 300, 350, 400, 460, 520, 570, 630},
    /* IT13 */
    {140, 180, 220, 270, 330, 390, 460, 540, 630, 720, 810, 890, 970},
};

// Fundamental deviations, micrometres, one row per letter. For d..h this is
// |es|, the shaft's UPPER deviation, and it is negative (the shaft sits below
// nominal). For k..p it is ei, the LOWER deviation, and positive. h is zero by
// definition and is the basis every shaft-basis fit hangs off.
struct Letter {
  const char* name;
  bool below; // true: es = -dev (clearance side). false: ei = +dev.
  int dev[kSteps];
};
constexpr Letter kLetters[] = {
    {"d", true, {20, 30, 40, 50, 65, 80, 100, 120, 145, 170, 190, 210, 230}},
    {"e", true, {14, 20, 25, 32, 40, 50, 60, 72, 85, 100, 110, 125, 135}},
    {"f", true, {6, 10, 13, 16, 20, 25, 30, 36, 43, 50, 56, 62, 68}},
    {"g", true, {2, 4, 5, 6, 7, 9, 10, 12, 14, 15, 17, 18, 20}},
    {"h", true, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    // k is the one letter whose deviation depends on the GRADE as well as the
    // size: these values hold for IT4..IT7, and k is zero everywhere else.
    {"k", false, {0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4, 5}},
    {"m", false, {2, 4, 6, 7, 8, 9, 11, 13, 15, 17, 20, 21, 23}},
    {"n", false, {4, 8, 10, 12, 15, 17, 20, 23, 27, 31, 34, 37, 40}},
    {"p", false, {6, 12, 15, 18, 22, 26, 32, 37, 43, 50, 56, 62, 68}},
};

// j and J: the near-symmetric classes, neither a fundamental deviation
// applied at every grade nor symmetric. Each grade has its OWN tabulated
// deviation, so they are rows per grade, and a grade the standard leaves
// blank is refused rather than borrowed from a neighbour.
//
// Shaft j: ei, the LOWER deviation (negative), es = ei + IT. ISO 286-1:2010
// Table 2 (column j: "IT5 and IT6", "IT7", "IT8"); checked against every
// cell of ISO 286-2:2010 Table 24, where a "+-x" cell is the same band
// (j6 over 250 mm, j7 at 250..315). j8 is defined only up to 3 mm.
constexpr int kJShaftEi56[kSteps] = {-2, -2,  -2,  -3,  -4,  -5, -7,
                                     -9, -11, -13, -16, -18, -20};
constexpr int kJShaftEi7[kSteps] = {-4,  -4,  -5,  -6,  -8,  -10, -12,
                                    -15, -18, -21, -26, -28, -32};
constexpr int kJShaftEi8UpTo3 = -6; // j8: the first size step only
// Hole J: ES, the UPPER deviation (positive), EI = ES - IT. ISO 286-1:2010
// Table 3 (column J: IT6, IT7, IT8), checked against ISO 286-2:2010 Table 8
// (J7 at 3..6 is printed +-6, "identical with JS7"; the same band). J9 and
// up are not J at all: the standard's footnote makes them symmetric, JS9.
// EI = ES - IT is 286-2's own Figure 1 rule. One cell of the copy of
// Table 8 checked here reads otherwise: J6 at 250..315 as +25/-5, a band of
// 30 where IT6 is 32. The rule gives -7, which is what is carried.
constexpr int kJHoleEs[3][kSteps] = {
    /* J6 */ {2, 5, 5, 6, 8, 10, 13, 16, 18, 22, 25, 29, 33},
    /* J7 */ {4, 6, 8, 10, 12, 14, 18, 22, 26, 30, 36, 39, 43},
    /* J8 */ {6, 10, 12, 15, 20, 24, 28, 34, 41, 47, 55, 60, 66},
};
constexpr const char* kNearSymmetric = "j";

// js/JS is not a fundamental deviation at all — it is "symmetric about
// nominal", so it carries no row and is handled where the band is built.
constexpr const char* kSymmetric = "js";

void Fail(std::string* error, std::string why) {
  if (error)
    *error = std::move(why);
}

std::string Join(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); i++) {
    if (i)
      out += i + 1 == v.size() ? " and " : ", ";
    out += v[i];
  }
  return out;
}

// -1 when the size is outside the tables.
int StepIndex(double nominalMm) {
  if (!(nominalMm > 0.0) || nominalMm > kStepMax[kSteps - 1])
    return -1;
  for (int i = 0; i < kSteps; i++)
    if (nominalMm <= kStepMax[i])
      return i;
  return -1;
}

const Letter* FindLetter(const std::string& lower) {
  for (const Letter& l : kLetters)
    if (lower == l.name)
      return &l;
  return nullptr;
}

// "H7" -> {"h", 7, hole}. Mixed case is refused rather than guessed at: "Js6"
// is as likely to be a typo for the hole as for the shaft.
struct Parsed {
  std::string letter; // lowercased
  int grade = 0;
  bool hole = false;
};
std::optional<Parsed> ParseClass(const std::string& cls, std::string* error) {
  size_t split = 0;
  while (split < cls.size() &&
         std::isalpha(static_cast<unsigned char>(cls[split])))
    split++;
  if (split == 0 || split == cls.size()) {
    Fail(error, "\"" + cls +
                    "\" is not a tolerance class — one is a letter and an IT "
                    "grade, like H7 (a hole) or g6 (a shaft)");
    return std::nullopt;
  }
  const std::string letters = cls.substr(0, split);
  bool anyUpper = false, anyLower = false;
  for (char c : letters) {
    anyUpper |= std::isupper(static_cast<unsigned char>(c)) != 0;
    anyLower |= std::islower(static_cast<unsigned char>(c)) != 0;
  }
  if (anyUpper && anyLower) {
    Fail(error, "\"" + cls +
                    "\" mixes cases — a hole class is upper (JS7), a shaft "
                    "class lower (js6)");
    return std::nullopt;
  }
  Parsed p;
  p.hole = anyUpper;
  for (char c : letters)
    p.letter += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const std::string digits = cls.substr(split);
  for (char c : digits)
    if (!std::isdigit(static_cast<unsigned char>(c))) {
      Fail(error, "\"" + cls + "\" has no IT grade — expected digits after \"" +
                      letters + "\", like " + letters + "7");
      return std::nullopt;
    }
  p.grade = std::atoi(digits.c_str());
  return p;
}

std::optional<int> ItMicrons(int grade, int step, std::string* error) {
  if (grade < kGradeMin || grade > kGradeMax) {
    Fail(error, "IT" + std::to_string(grade) +
                    " is not carried — the table runs IT" +
                    std::to_string(kGradeMin) + " to IT" +
                    std::to_string(kGradeMax) + ", which covers every fit");
    return std::nullopt;
  }
  return kIt[grade - kGradeMin][step];
}

} // namespace

std::string FitKindName(FitKind kind) {
  switch (kind) {
  case FitKind::Clearance:
    return "clearance";
  case FitKind::Transition:
    return "transition";
  case FitKind::Interference:
    return "interference";
  }
  return "clearance";
}

std::optional<FitKind> FitKindFromName(const std::string& name) {
  if (name == "clearance")
    return FitKind::Clearance;
  if (name == "transition")
    return FitKind::Transition;
  if (name == "interference")
    return FitKind::Interference;
  return std::nullopt;
}

std::vector<std::string> ShaftLetters() {
  std::vector<std::string> out;
  for (const Letter& l : kLetters)
    out.emplace_back(l.name);
  out.emplace_back(kSymmetric);
  out.emplace_back(kNearSymmetric);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> HoleLetters() {
  std::vector<std::string> out;
  for (std::string s : ShaftLetters()) {
    for (char& c : s)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out.push_back(s);
  }
  return out;
}

double MaxNominal() { return kStepMax[kSteps - 1]; }

std::optional<Band> Limits(double nominalMm, const std::string& cls,
                           std::string* error) {
  if (error)
    error->clear();
  const int step = StepIndex(nominalMm);
  if (step < 0) {
    char had[32];
    std::snprintf(had, sizeof(had), "%g", nominalMm);
    Fail(error, std::string("nominal size ") + had +
                    " mm is outside ISO 286 as carried here — it runs above 0 "
                    "up to and including " +
                    std::to_string(static_cast<int>(MaxNominal())) + " mm");
    return std::nullopt;
  }
  const auto parsed = ParseClass(cls, error);
  if (!parsed)
    return std::nullopt;
  const auto it = ItMicrons(parsed->grade, step, error);
  if (!it)
    return std::nullopt;

  Band band;
  band.grade = parsed->grade;
  band.it = *it / 1000.0;

  // Symmetric: half the grade either way, and the only class whose band does
  // not hang off a fundamental deviation.
  if (parsed->letter == kSymmetric) {
    band.upper = *it / 2000.0;
    band.lower = -band.upper;
    return band;
  }

  if (parsed->letter == kNearSymmetric) {
    const std::string sizes = step == 0 ? "up to 3 mm" : "over 3 mm";
    if (!parsed->hole) {
      int ei = 0;
      if (parsed->grade == 5 || parsed->grade == 6)
        ei = kJShaftEi56[step];
      else if (parsed->grade == 7)
        ei = kJShaftEi7[step];
      else if (parsed->grade == 8 && step == 0)
        ei = kJShaftEi8UpTo3;
      else {
        Fail(error,
             "shaft class " + cls + " is not defined by ISO 286 " +
                 (parsed->grade == 8
                      ? std::string("at ") + sizes +
                            " — j8 exists only up to 3 mm. "
                      : std::string("— j is defined for j5, j6 and j7 at "
                                    "every size to 500 mm, and j8 up to 3 "
                                    "mm. ")) +
                 "For a symmetric band use js" + std::to_string(parsed->grade));
        return std::nullopt;
      }
      band.lower = ei / 1000.0;
      band.upper = (ei + *it) / 1000.0;
      return band;
    }
    if (parsed->grade < 6 || parsed->grade > 8) {
      Fail(error,
           "hole class " + cls +
               " is not defined by ISO 286 — J is defined for J6, J7 and J8 "
               "at every size to 500 mm" +
               (parsed->grade > 8
                    ? std::string("; from IT9 the standard makes J "
                                  "symmetric, which is JS") +
                          std::to_string(parsed->grade)
                    : std::string(". Use JS") + std::to_string(parsed->grade) +
                          " for a symmetric band"));
      return std::nullopt;
    }
    const int es = kJHoleEs[parsed->grade - 6][step];
    band.upper = es / 1000.0;
    band.lower = (es - *it) / 1000.0;
    return band;
  }

  const Letter* letter = FindLetter(parsed->letter);
  if (!letter) {
    Fail(error, "\"" + cls + "\" is not a class this table carries — shafts " +
                    Join(ShaftLetters()) + ", holes " + Join(HoleLetters()));
    return std::nullopt;
  }
  // k's tabulated deviation is defined for IT4..IT7 only; at every other
  // grade k coincides with h. Silently reusing the IT6 number at IT9 would
  // shift the whole band by 2 µm and look entirely reasonable.
  const int dev =
      (parsed->letter == "k" && (parsed->grade < 4 || parsed->grade > 7))
          ? 0
          : letter->dev[step];

  if (!parsed->hole) {
    if (letter->below) { // es = -dev, ei = es - IT
      band.upper = -dev / 1000.0;
      band.lower = (-dev - *it) / 1000.0;
    } else { // ei = +dev, es = ei + IT
      band.lower = dev / 1000.0;
      band.upper = (dev + *it) / 1000.0;
    }
    return band;
  }

  // Holes mirror the shaft rule (EI = -es), which is exact for A..H.
  if (letter->below) {
    band.lower = dev / 1000.0;
    band.upper = (dev + *it) / 1000.0;
    return band;
  }
  // K..P need the standard's delta correction, ES = -ei + (IT_n - IT_{n-1}).
  // It is defined for K/M/N up to IT8 and P up to IT7; past that the standard
  // switches rules. Dropping delta anyway is wrong by exactly one grade step
  // — a plausible number, and the wrong one — so refuse instead.
  const int maxGrade = parsed->letter == "p" ? 7 : 8;
  if (parsed->grade > maxGrade) {
    Fail(error, "hole class " + cls +
                    " is not carried — the delta correction " + parsed->letter +
                    " needs is only defined up to IT" +
                    std::to_string(maxGrade) + " for " + parsed->letter +
                    ". Use a finer grade, or state the deviations outright");
    return std::nullopt;
  }
  const auto prev = ItMicrons(parsed->grade - 1, step, error);
  if (!prev)
    return std::nullopt;
  const int delta = *it - *prev;
  band.upper = (-dev + delta) / 1000.0;
  band.lower = (-dev + delta - *it) / 1000.0;
  return band;
}

std::optional<Pair> Fit(double nominalMm, const std::string& holeCls,
                        const std::string& shaftCls, std::string* error) {
  if (error)
    error->clear();
  // Swapped sides give a mirror-image answer that looks entirely plausible,
  // so the case is checked before anything is computed and named explicitly.
  const auto holeParsed = ParseClass(holeCls, error);
  if (!holeParsed)
    return std::nullopt;
  if (!holeParsed->hole) {
    Fail(error, "\"" + holeCls +
                    "\" is a SHAFT class (lowercase); the hole side takes an "
                    "uppercase one like H7");
    return std::nullopt;
  }
  const auto shaftParsed = ParseClass(shaftCls, error);
  if (!shaftParsed)
    return std::nullopt;
  if (shaftParsed->hole) {
    Fail(error, "\"" + shaftCls +
                    "\" is a HOLE class (uppercase); the shaft side takes a "
                    "lowercase one like g6");
    return std::nullopt;
  }

  Pair pair;
  const auto hole = Limits(nominalMm, holeCls, error);
  if (!hole)
    return std::nullopt;
  const auto shaft = Limits(nominalMm, shaftCls, error);
  if (!shaft)
    return std::nullopt;
  pair.hole = *hole;
  pair.shaft = *shaft;
  // Worst case over two independent bands, and nothing more.
  pair.minClearance = hole->lower - shaft->upper;
  pair.maxClearance = hole->upper - shaft->lower;
  pair.kind = pair.minClearance >= 0.0   ? FitKind::Clearance
              : pair.maxClearance <= 0.0 ? FitKind::Interference
                                         : FitKind::Transition;
  return pair;
}

} // namespace fits
