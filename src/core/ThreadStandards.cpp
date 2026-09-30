#include "core/ThreadStandards.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace threads {

namespace {

constexpr double kInch = 25.4;

// A unified row in the units its standard is written in: major diameter and
// tap drill in inches, threads per inch. Converted once, here, so no caller
// ever sees an inch.
Standard Unified(const char* designation, Series series, double majorIn,
                 double tpi, double tapDrillIn) {
  return {designation, majorIn * kInch, kInch / tpi, series,
          tapDrillIn * kInch};
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    b--;
  return s.substr(a, b - a);
}

const Standard* Exact(const std::string& d) {
  for (const Standard& s : All())
    if (s.designation == d)
      return &s;
  return nullptr;
}

bool IsClassToken(const std::string& t) {
  for (Series s : {Series::Metric, Series::UNC})
    for (bool ext : {true, false}) {
      const auto& cs = Classes(s, ext);
      if (std::find(cs.begin(), cs.end(), t) != cs.end())
        return true;
    }
  return false;
}

bool IsUnified(Series s) { return s == Series::UNC || s == Series::UNF; }

// "M" then a digit: the designation claims to be ISO metric.
bool ClaimsMetric(const std::string& d) {
  return d.size() >= 2 && (d[0] == 'M' || d[0] == 'm') &&
         std::isdigit(static_cast<unsigned char>(d[1]));
}

// A UN series suffix, or a size-TPI pair ("1/4-20", "#10-32", "10-32").
bool ClaimsUnified(const std::string& d) {
  for (const char* series : {"UNC", "UNF", "UNEF", "UNS", "UNJ"})
    if (d.find(series) != std::string::npos)
      return true;
  size_t i = 0;
  if (i < d.size() && d[i] == '#')
    i++;
  const size_t digits0 = i;
  while (i < d.size() &&
         (std::isdigit(static_cast<unsigned char>(d[i])) || d[i] == '/'))
    i++;
  if (i == digits0 || i >= d.size() || d[i] != '-')
    return false;
  i++;
  return i < d.size() && std::isdigit(static_cast<unsigned char>(d[i]));
}

std::string Join(const std::vector<std::string>& v) {
  std::string out;
  for (const std::string& s : v)
    out += (out.empty() ? "" : ", ") + s;
  return out;
}

std::string Accepted(bool unified) {
  std::vector<std::string> names;
  for (const Standard& s : All())
    if (IsUnified(s.series) == unified)
      names.push_back(s.designation);
  return Join(names);
}

// Spellings of a row other than its own, with no class suffix.
const Standard* Alias(const std::string& d) {
  if (const Standard* s = Exact(d))
    return s;
  // "M8x1.25": the coarse row with its pitch written out.
  if (ClaimsMetric(d)) {
    const size_t x = d.find_first_of("xX");
    if (x != std::string::npos) {
      char* end = nullptr;
      const double dia = std::strtod(d.c_str() + 1, &end);
      if (end != d.c_str() + x)
        return nullptr;
      const double p = std::strtod(d.c_str() + x + 1, &end);
      if (*end != '\0')
        return nullptr;
      for (const Standard& s : All())
        if (s.series == Series::Metric && std::abs(s.majorDia - dia) < 1e-9 &&
            std::abs(s.pitch - p) < 1e-9)
          return &s;
    }
    return nullptr;
  }
  // "10-32 UNF": a number size without its '#'.
  if (!d.empty() && std::isdigit(static_cast<unsigned char>(d[0])))
    return Exact("#" + d);
  return nullptr;
}

} // namespace

std::string SeriesName(Series s) {
  switch (s) {
  case Series::Metric:
    return "ISO metric";
  case Series::UNC:
    return "UNC";
  case Series::UNF:
    return "UNF";
  }
  return "?";
}

double MinorDiameter(const Standard& s) {
  return s.majorDia - 1.0825 * s.pitch;
}

double TapDrill(const Standard& s) {
  return s.tapDrill > 0.0 ? s.tapDrill : s.majorDia - s.pitch;
}

const std::vector<Standard>& All() {
  static const std::vector<Standard> kSeries = [] {
    // ISO 262 preferred sizes: coarse series, then the common fine pitches.
    std::vector<Standard> v = {
        // The small end matters: an FPV stack, a camera and a 20x20 video
        // transmitter are all M2, and a table that started at M3 could not
        // declare the thread those parts are held together with — while the
        // clearance-hole table beside it has had M2 all along.
        {"M1.6", 1.6, 0.35},      {"M2", 2.0, 0.4},
        {"M2.5", 2.5, 0.45},      {"M3", 3.0, 0.5},
        {"M4", 4.0, 0.7},         {"M5", 5.0, 0.8},
        {"M6", 6.0, 1.0},         {"M8", 8.0, 1.25},
        {"M10", 10.0, 1.5},       {"M12", 12.0, 1.75},
        {"M16", 16.0, 2.0},       {"M20", 20.0, 2.5},
        {"M24", 24.0, 3.0},       {"M30", 30.0, 3.5},
        {"M8x1", 8.0, 1.0},       {"M10x1.25", 10.0, 1.25},
        {"M10x1", 10.0, 1.0},     {"M12x1.5", 12.0, 1.5},
        {"M12x1.25", 12.0, 1.25}, {"M16x1.5", 16.0, 1.5},
        {"M20x1.5", 20.0, 1.5},
    };
    // ASME B1.1 unified coarse and fine, #0 to 1": major diameter (in),
    // threads per inch, and the 75%-thread tap drill a shop chart gives
    // (number, letter or fractional drill, in inches). A camera takes
    // 1/4-20 UNC and a tripod 3/8-16 UNC.
    const Series C = Series::UNC, F = Series::UNF;
    const Standard unified[] = {
        Unified("#1-64 UNC", C, 0.073, 64, 0.0595),      // #53
        Unified("#2-56 UNC", C, 0.086, 56, 0.0700),      // #50
        Unified("#3-48 UNC", C, 0.099, 48, 0.0785),      // #47
        Unified("#4-40 UNC", C, 0.112, 40, 0.0890),      // #43
        Unified("#5-40 UNC", C, 0.125, 40, 0.1015),      // #38
        Unified("#6-32 UNC", C, 0.138, 32, 0.1065),      // #36
        Unified("#8-32 UNC", C, 0.164, 32, 0.1360),      // #29
        Unified("#10-24 UNC", C, 0.190, 24, 0.1495),     // #25
        Unified("#12-24 UNC", C, 0.216, 24, 0.1770),     // #16
        Unified("1/4-20 UNC", C, 0.250, 20, 0.2010),     // #7
        Unified("5/16-18 UNC", C, 0.3125, 18, 0.2570),   // F
        Unified("3/8-16 UNC", C, 0.375, 16, 0.3125),     // 5/16
        Unified("7/16-14 UNC", C, 0.4375, 14, 0.3680),   // U
        Unified("1/2-13 UNC", C, 0.500, 13, 0.421875),   // 27/64
        Unified("9/16-12 UNC", C, 0.5625, 12, 0.484375), // 31/64
        Unified("5/8-11 UNC", C, 0.625, 11, 0.53125),    // 17/32
        Unified("3/4-10 UNC", C, 0.750, 10, 0.65625),    // 21/32
        Unified("7/8-9 UNC", C, 0.875, 9, 0.765625),     // 49/64
        Unified("1-8 UNC", C, 1.000, 8, 0.875),          // 7/8
        Unified("#0-80 UNF", F, 0.060, 80, 0.046875),    // 3/64
        Unified("#1-72 UNF", F, 0.073, 72, 0.0595),      // #53
        Unified("#2-64 UNF", F, 0.086, 64, 0.0700),      // #50
        Unified("#3-56 UNF", F, 0.099, 56, 0.0820),      // #45
        Unified("#4-48 UNF", F, 0.112, 48, 0.0935),      // #42
        Unified("#5-44 UNF", F, 0.125, 44, 0.1040),      // #37
        Unified("#6-40 UNF", F, 0.138, 40, 0.1130),      // #33
        Unified("#8-36 UNF", F, 0.164, 36, 0.1360),      // #29
        Unified("#10-32 UNF", F, 0.190, 32, 0.1590),     // #21
        Unified("#12-28 UNF", F, 0.216, 28, 0.1820),     // #14
        Unified("1/4-28 UNF", F, 0.250, 28, 0.2130),     // #3
        Unified("5/16-24 UNF", F, 0.3125, 24, 0.2720),   // I
        Unified("3/8-24 UNF", F, 0.375, 24, 0.3320),     // Q
        Unified("7/16-20 UNF", F, 0.4375, 20, 0.390625), // 25/64
        Unified("1/2-20 UNF", F, 0.500, 20, 0.453125),   // 29/64
        Unified("9/16-18 UNF", F, 0.5625, 18, 0.515625), // 33/64
        Unified("5/8-18 UNF", F, 0.625, 18, 0.578125),   // 37/64
        Unified("3/4-16 UNF", F, 0.750, 16, 0.6875),     // 11/16
        Unified("7/8-14 UNF", F, 0.875, 14, 0.8125),     // 13/16
        Unified("1-12 UNF", F, 1.000, 12, 0.921875),     // 59/64
    };
    v.insert(v.end(), std::begin(unified), std::end(unified));
    return v;
  }();
  return kSeries;
}

const std::vector<std::string>& Classes(Series s, bool external) {
  // ISO 965-1's usual tolerance classes (lower-case external, upper-case
  // internal) and ASME B1.1's three classes a side.
  static const std::vector<std::string> kMetricExt = {"6g", "4h", "6h", "4g",
                                                      "8g", "6e", "6f"};
  static const std::vector<std::string> kMetricInt = {"6H", "4H", "5H", "7H",
                                                      "8H", "5G", "6G", "7G"};
  static const std::vector<std::string> kUnifiedExt = {"2A", "1A", "3A"};
  static const std::vector<std::string> kUnifiedInt = {"2B", "1B", "3B"};
  if (IsUnified(s))
    return external ? kUnifiedExt : kUnifiedInt;
  return external ? kMetricExt : kMetricInt;
}

std::optional<Resolved> Resolve(const std::string& designation,
                                std::string* error) {
  if (error)
    error->clear();
  const std::string d = Trim(designation);
  if (const Standard* s = Alias(d))
    return Resolved{*s, {}};

  // A class suffix: "M8-6g", "3/8-16 UNC-2B".
  const size_t dash = d.rfind('-');
  if (dash != std::string::npos && dash + 1 < d.size()) {
    const std::string cls = d.substr(dash + 1);
    if (IsClassToken(cls)) {
      const std::string base = Trim(d.substr(0, dash));
      if (const Standard* s = Alias(base)) {
        const auto& ext = Classes(s->series, true);
        const auto& in = Classes(s->series, false);
        if (std::find(ext.begin(), ext.end(), cls) == ext.end() &&
            std::find(in.begin(), in.end(), cls) == in.end()) {
          if (error)
            *error = "class " + cls + " is not a " + SeriesName(s->series) +
                     " class; accepted: " + Join(ext) + " (external), " +
                     Join(in) + " (internal)";
          return std::nullopt;
        }
        return Resolved{*s, cls};
      }
    }
  }

  const bool unified = ClaimsUnified(d);
  if (!unified && !ClaimsMetric(d))
    return std::nullopt; // free text: a custom thread

  if (error) {
    std::string hint;
    // "1/4-20" names a size and a pitch but no series; say which one exists
    // rather than picking it.
    for (const char* suffix : {" UNC", " UNF"})
      if (Alias(d + suffix) != nullptr)
        hint = " (did you mean '" + d + suffix + "'?)";
    *error = "'" + d + "' is not in the " +
             (unified ? std::string("unified (UNC/UNF)")
                      : std::string("ISO metric")) +
             " thread table" + hint + "; accepted: " + Accepted(unified) +
             ", optionally with a class suffix such as " +
             (unified ? "-2A or -2B" : "-6g or -6H") +
             ". Any other text is a custom thread with its own pitch";
  }
  return std::nullopt;
}

std::optional<Standard> Lookup(const std::string& designation) {
  const auto r = Resolve(designation, nullptr);
  if (!r)
    return std::nullopt;
  return r->standard;
}

std::string ClassError(const Standard& s, const std::string& threadClass,
                       bool external) {
  if (threadClass.empty())
    return {};
  const auto& ok = Classes(s.series, external);
  if (std::find(ok.begin(), ok.end(), threadClass) != ok.end())
    return {};
  const auto& other = Classes(s.series, !external);
  const std::string side = external ? "an external" : "an internal";
  if (std::find(other.begin(), other.end(), threadClass) != other.end())
    return "class " + threadClass + " is for " +
           (external ? "an internal" : "an external") + " " +
           SeriesName(s.series) + " thread, and this one is " +
           (external ? "external (a boss)" : "internal (a hole wall)") +
           "; accepted: " + Join(ok);
  return "class " + threadClass + " is not " + side + " " +
         SeriesName(s.series) + " class; accepted: " + Join(ok);
}

double BasicPitchDiameter(double majorDia, double pitch) {
  return majorDia - 0.649519 * pitch;
}

std::string DefaultClass(Series series, bool external) {
  return Classes(series, external).front();
}

std::optional<PitchBand> PitchDiameterBand(Series series, double majorDia,
                                           double pitch,
                                           const std::string& threadClass,
                                           bool external, std::string* error) {
  const auto& ok = Classes(series, external);
  if (std::find(ok.begin(), ok.end(), threadClass) == ok.end()) {
    if (error != nullptr)
      *error = "class '" + threadClass + "' is not " +
               (external ? "an external " : "an internal ") +
               SeriesName(series) + " class; accepted: " + Join(ok);
    return std::nullopt;
  }
  const double d2 = BasicPitchDiameter(majorDia, pitch);
  PitchBand band;
  band.threadClass = threadClass;
  if (IsUnified(series)) {
    // ASME B1.1, in inches: the 2A pitch-diameter tolerance at a length of
    // engagement of 9 pitches, then each class's multiple of it.
    const double D = majorDia / kInch, P = pitch / kInch;
    const double t2A = 0.0015 * std::cbrt(D) + 0.0015 * std::sqrt(9.0 * P) +
                       0.015 * std::pow(P, 2.0 / 3.0);
    const char grade = threadClass[0];
    double factor = 1.0;
    if (external)
      factor = grade == '1' ? 1.5 : grade == '3' ? 0.75 : 1.0;
    else
      factor = grade == '1' ? 1.95 : grade == '3' ? 0.975 : 1.3;
    const double tol = t2A * factor * kInch;
    if (external) {
      // 1A and 2A carry an allowance of 0.3 of the 2A tolerance; 3A none.
      const double allowance = grade == '3' ? 0.0 : 0.3 * t2A * kInch;
      band.max = d2 - allowance;
      band.min = band.max - tol;
    } else {
      band.min = d2; // every B class starts at basic
      band.max = d2 + tol;
    }
    return band;
  }
  // ISO 965-1. Td2(6) = 90 P^0.4 d^0.1 um, d the geometric mean of the
  // nominal diameter's range; TD2(6) = 1.32 Td2(6); grades 3..8 scale both.
  static constexpr double kRanges[] = {0.99, 1.4,  2.8,   5.6,   11.2, 22.4,
                                       45.0, 90.0, 180.0, 355.0, 600.0};
  double dMean = majorDia;
  for (size_t i = 0; i + 1 < std::size(kRanges); i++)
    if (majorDia > kRanges[i] && majorDia <= kRanges[i + 1]) {
      dMean = std::sqrt(kRanges[i] * kRanges[i + 1]);
      break;
    }
  const double td2 =
      90.0 * std::pow(pitch, 0.4) * std::pow(dMean, 0.1) * 1e-3; // mm
  const int grade = threadClass[0] - '0';
  const double gradeFactor = grade == 3   ? 0.5
                             : grade == 4 ? 0.63
                             : grade == 5 ? 0.8
                             : grade == 7 ? 1.25
                             : grade == 8 ? 1.6
                                          : 1.0;
  const char position = threadClass[1];
  // Fundamental deviations, um: G +(15 + 11P); e, f, g -(50|30|15 + 11P).
  const double p11 = 11.0 * pitch;
  if (external) {
    const double es = position == 'e'   ? -(50.0 + p11)
                      : position == 'f' ? -(30.0 + p11)
                      : position == 'g' ? -(15.0 + p11)
                                        : 0.0;
    band.max = d2 + es * 1e-3;
    band.min = band.max - td2 * gradeFactor;
  } else {
    const double ei = position == 'G' ? 15.0 + p11 : 0.0;
    band.min = d2 + ei * 1e-3;
    band.max = band.min + 1.32 * td2 * gradeFactor;
  }
  return band;
}

} // namespace threads
