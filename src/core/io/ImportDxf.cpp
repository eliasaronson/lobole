// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/ImportDxf.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>

namespace dxfimport {

namespace {

using sketchimport::Affine2;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr int kMaxInsertDepth = 16;

struct Pair {
  int code;
  std::string value;
};

// One entity: its type and its group codes, in file order.
struct Record {
  std::string type;
  std::vector<Pair> codes;
  int ordinal = 0; // position in the file, for an entity with no handle

  [[nodiscard]] const std::string* Get(int code) const {
    for (const Pair& p : codes)
      if (p.code == code)
        return &p.value;
    return nullptr;
  }
  [[nodiscard]] std::string Str(int code, std::string fallback = {}) const {
    const std::string* v = Get(code);
    return v != nullptr ? *v : fallback;
  }
  [[nodiscard]] std::string Name() const {
    const std::string* h = Get(5);
    return type + (h != nullptr ? " (handle " + *h + ")"
                                : " (entity " + std::to_string(ordinal) + ")");
  }
};

std::string Trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
    a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
    b--;
  return std::string(s.substr(a, b - a));
}

double Num(const Record& r, const std::string& v) {
  const std::string t = Trim(v);
  double out = 0.0;
  const char* first = t.data();
  const char* last = t.data() + t.size();
  if (first != last && *first == '+')
    first++;
  const auto res = std::from_chars(first, last, out);
  if (res.ec != std::errc() || res.ptr != last || !std::isfinite(out))
    throw std::runtime_error(r.Name() + ": '" + t + "' is not a number");
  return out;
}

double NumOr(const Record& r, int code, double fallback) {
  const std::string* v = r.Get(code);
  return v != nullptr ? Num(r, *v) : fallback;
}

int IntOr(const Record& r, int code, int fallback) {
  const std::string* v = r.Get(code);
  if (v == nullptr)
    return fallback;
  return static_cast<int>(std::lround(Num(r, *v)));
}

// The file as code/value pairs. CRLF or LF; the code line is an integer.
std::vector<Pair> Tokenize(std::string_view text) {
  std::vector<Pair> out;
  size_t pos = 0;
  int line = 0;
  const auto next = [&](std::string_view& got) {
    if (pos >= text.size())
      return false;
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos)
      end = text.size();
    got = text.substr(pos, end - pos);
    if (!got.empty() && got.back() == '\r')
      got.remove_suffix(1);
    pos = end + 1;
    line++;
    return true;
  };
  std::string_view codeLine, valueLine;
  while (next(codeLine)) {
    const std::string c = Trim(codeLine);
    if (c.empty() && pos >= text.size())
      break;
    int code = 0;
    const auto res = std::from_chars(c.data(), c.data() + c.size(), code);
    if (res.ec != std::errc() || res.ptr != c.data() + c.size())
      throw std::runtime_error("line " + std::to_string(line) + ": '" + c +
                               "' is not a DXF group code (is it a DXF?)");
    if (!next(valueLine))
      throw std::runtime_error("the file ends in the middle of a group");
    out.push_back({code, std::string(valueLine)});
  }
  return out;
}

struct Layered {
  sketch::Entity entity;
  std::string layer;
};

struct Block {
  glm::dvec2 base{0.0};
  double baseZ = 0.0;
  std::vector<Record> records;
};

// How a record lands in the drawing: the inserts it sits in, composed.
struct Ctx {
  Affine2 xf;
  double zScale = 1.0;
  double zOff = 0.0;
  std::string insertLayer; // what layer "0" means inside a block
  std::string uneven;      // the INSERT that scales unevenly ("" = none)
  int depth = 0;
  std::vector<std::string> stack; // block names, for a cycle
};

class Reader {
public:
  explicit Reader(const Options& options) : m_opt(options) {}

  sketchimport::Drawing Run(std::string_view text) {
    if (text.starts_with("AutoCAD Binary DXF"))
      throw std::runtime_error("a binary DXF is not read; save it as ASCII "
                               "DXF");
    const std::vector<Pair> pairs = Tokenize(text);
    std::vector<Record> entities;
    // Walk the sections.
    size_t i = 0;
    int ordinal = 0;
    const auto readRecords = [&](const char* end, std::vector<Record>& into) {
      // From pairs[i] (a code 0), records until a code 0 `end`.
      while (i < pairs.size()) {
        if (pairs[i].code != 0) {
          i++;
          continue;
        }
        const std::string type = Trim(pairs[i].value);
        if (type == end)
          return;
        Record r;
        r.type = type;
        r.ordinal = ++ordinal;
        i++;
        while (i < pairs.size() && pairs[i].code != 0)
          r.codes.push_back(pairs[i++]);
        into.push_back(std::move(r));
      }
    };
    while (i < pairs.size()) {
      if (pairs[i].code == 0 && Trim(pairs[i].value) == "SECTION" &&
          i + 1 < pairs.size() && pairs[i + 1].code == 2) {
        const std::string section = Trim(pairs[i + 1].value);
        i += 2;
        if (section == "HEADER") {
          ReadHeader(pairs, i);
        } else if (section == "TABLES") {
          std::vector<Record> recs;
          readRecords("ENDSEC", recs);
          for (const Record& r : recs)
            if (r.type == "LAYER")
              m_layers.insert(Trim(r.Str(2)));
        } else if (section == "BLOCKS") {
          std::vector<Record> recs;
          readRecords("ENDSEC", recs);
          ReadBlocks(recs);
        } else if (section == "ENTITIES") {
          readRecords("ENDSEC", entities);
        } else {
          // CLASSES, OBJECTS, THUMBNAILIMAGE, ACDSDATA: nothing drawn.
          while (i < pairs.size() &&
                 !(pairs[i].code == 0 && Trim(pairs[i].value) == "ENDSEC"))
            i++;
        }
      }
      i++;
    }
    for (const Record& r : entities)
      m_layers.insert(Trim(r.Str(8, "0")));
    for (const auto& [name, block] : m_blocks)
      for (const Record& r : block.records)
        m_layers.insert(Trim(r.Str(8, "0")));
    CheckLayerNames();
    DecideUnits();

    Ctx top;
    const double mm = units::MmPerLength(m_unit);
    top.xf.lin = glm::dmat2(mm);
    top.zScale = mm;
    Emit(entities, top);

    sketchimport::Drawing out;
    out.notImported = std::move(m_notImported);
    out.unit = units::ToString(m_unit);
    out.unitSource = m_unitSource;
    for (Layered& l : m_out) {
      if (!m_opt.layers.empty() &&
          std::find(m_opt.layers.begin(), m_opt.layers.end(), l.layer) ==
              m_opt.layers.end())
        continue;
      if (std::find(m_opt.constructionLayers.begin(),
                    m_opt.constructionLayers.end(),
                    l.layer) != m_opt.constructionLayers.end())
        sketch::SetConstruction(l.entity, true);
      out.entities.push_back(std::move(l.entity));
    }
    return out;
  }

private:
  const Options& m_opt;
  std::set<std::string> m_layers;
  std::map<std::string, Block> m_blocks;
  std::vector<Layered> m_out;
  std::map<std::string, int> m_notImported;
  int m_insunits = 0;
  bool m_haveInsunits = false;
  units::Length m_unit = units::Length::Millimetre;
  std::string m_unitSource;
  bool m_haveZ = false;
  double m_z = 0.0;
  std::string m_zWho;

  void ReadHeader(const std::vector<Pair>& pairs, size_t& i) {
    while (i < pairs.size() &&
           !(pairs[i].code == 0 && Trim(pairs[i].value) == "ENDSEC")) {
      if (pairs[i].code == 9 && Trim(pairs[i].value) == "$INSUNITS" &&
          i + 1 < pairs.size() && pairs[i + 1].code == 70) {
        Record r{"$INSUNITS", {}, 0};
        m_insunits = static_cast<int>(std::lround(Num(r, pairs[i + 1].value)));
        m_haveInsunits = true;
        i++;
      }
      i++;
    }
  }

  void ReadBlocks(const std::vector<Record>& recs) {
    Block* current = nullptr;
    std::string name;
    for (const Record& r : recs) {
      if (r.type == "BLOCK") {
        name = Trim(r.Str(2));
        Block b;
        b.base = {NumOr(r, 10, 0.0), NumOr(r, 20, 0.0)};
        b.baseZ = NumOr(r, 30, 0.0);
        current = &(m_blocks[name] = std::move(b));
      } else if (r.type == "ENDBLK") {
        current = nullptr;
      } else if (current != nullptr) {
        current->records.push_back(r);
      }
    }
  }

  void CheckLayerNames() const {
    const auto check = [&](const std::vector<std::string>& names,
                           const char* what) {
      for (const std::string& n : names)
        if (!m_layers.contains(n)) {
          std::string have;
          for (const std::string& l : m_layers)
            have += (have.empty() ? "" : ", ") + l;
          throw std::runtime_error(std::string(what) + ": the file has no " +
                                   "layer '" + n + "'. Its layers: " + have);
        }
    };
    check(m_opt.layers, "layers");
    check(m_opt.constructionLayers, "construction_layers");
  }

  void DecideUnits() {
    static const std::map<int, units::Length> kCodes = {
        {1, units::Length::Inch},
        {4, units::Length::Millimetre},
        {5, units::Length::Centimetre},
        {6, units::Length::Metre}};
    if (m_haveInsunits && m_insunits != 0) {
      const auto it = kCodes.find(m_insunits);
      if (it == kCodes.end())
        throw std::runtime_error(
            "$INSUNITS " + std::to_string(m_insunits) +
            " is not a unit this reader takes. Accepted: 1 (in), 4 (mm), 5 "
            "(cm), 6 (m)");
      if (m_opt.units && *m_opt.units != it->second)
        throw std::runtime_error(
            std::string("units: '") + units::ToString(*m_opt.units) +
            "' contradicts the file, which declares $INSUNITS " +
            std::to_string(m_insunits) + " (" + units::ToString(it->second) +
            ")");
      m_unit = it->second;
      m_unitSource = "$INSUNITS " + std::to_string(m_insunits);
      return;
    }
    if (m_opt.units) {
      m_unit = *m_opt.units;
      m_unitSource = "units (the file declares none)";
    } else {
      m_unit = units::Length::Millimetre;
      m_unitSource = "the default: the file declares no unit";
    }
  }

  // The extrusion: +Z is the plane as drawn, -Z the mirrored OCS (the
  // arbitrary-axis algorithm maps OCS x to -X). Anything else is a curve
  // standing off the plane, refused.
  double Extrusion(const Record& r) const {
    const double x = NumOr(r, 210, 0.0), y = NumOr(r, 220, 0.0),
                 z = NumOr(r, 230, 1.0);
    const double len = std::sqrt(x * x + y * y + z * z);
    if (len > 0.0 && std::abs(x / len) < 1e-9 && std::abs(y / len) < 1e-9)
      return z > 0.0 ? 1.0 : -1.0;
    throw std::runtime_error(
        r.Name() + ": its extrusion (" + Trim(r.Str(210, "0")) + ", " +
        Trim(r.Str(220, "0")) + ", " + Trim(r.Str(230, "1")) +
        ") is not +Z or -Z, so it is not in the "
        "drawing's plane");
  }

  void CheckZ(const Record& r, const Ctx& ctx, double localZ) {
    const double z = ctx.zOff + ctx.zScale * localZ;
    if (!m_haveZ) {
      m_haveZ = true;
      m_z = z;
      m_zWho = r.Name();
      return;
    }
    if (std::abs(z - m_z) > 1e-6 * std::max(1.0, std::abs(m_z)))
      throw std::runtime_error(
          r.Name() + " lies at z = " + std::to_string(z) + " mm, " + m_zWho +
          " at z = " + std::to_string(m_z) +
          " mm: a drawing at mixed elevations is not one plane");
  }

  std::string LayerOf(const Record& r, const Ctx& ctx) const {
    std::string layer = Trim(r.Str(8, "0"));
    if (layer == "0" && !ctx.insertLayer.empty())
      return ctx.insertLayer;
    return layer;
  }

  void Add(const Record& r, const Ctx& ctx, const sketch::Entity& local) {
    m_out.push_back(
        {sketchimport::TransformEntity(local, ctx.xf), LayerOf(r, ctx)});
  }

  void Conic(const Record& r, const Ctx& ctx, glm::dvec2 c, const glm::dmat2& L,
             double t0, double t1, bool full) {
    if (!ctx.uneven.empty())
      throw std::runtime_error(ctx.uneven + " scales its block unevenly over " +
                               r.Name() + ", which would make a curve the " +
                               "drawing does not have");
    m_out.push_back({sketchimport::ConicEntity(ctx.xf.Apply(c), ctx.xf.lin * L,
                                               t0, t1, full),
                     LayerOf(r, ctx)});
  }

  // A polyline's vertices (OCS) as lines and bulge arcs.
  struct Vtx {
    glm::dvec2 p;
    double bulge = 0.0;
  };
  void Polyline(const Record& r, const Ctx& ctx, double e,
                const std::vector<Vtx>& v, bool closed) {
    const Affine2 ocs{glm::dmat2(e, 0.0, 0.0, 1.0), glm::dvec2(0.0)};
    const size_t n = v.size();
    const size_t segs = closed ? n : (n > 0 ? n - 1 : 0);
    for (size_t k = 0; k < segs; k++) {
      const glm::dvec2 a = v[k].p, b = v[(k + 1) % n].p;
      const double bulge = v[k].bulge;
      if (glm::length(b - a) <= 0.0)
        continue;
      if (bulge == 0.0) {
        Add(r, ctx, sketchimport::TransformEntity(sketch::Line{a, b}, ocs));
        continue;
      }
      // Sweep 4·atan(bulge), CCW from a to b when positive; the centre sits
      // on the chord's left, (1 − b²)/(4b) chords from its middle.
      const glm::dvec2 chord = b - a;
      const glm::dvec2 left(-chord.y, chord.x);
      const glm::dvec2 c =
          0.5 * (a + b) + left * ((1.0 - bulge * bulge) / (4.0 * bulge));
      const double radius = glm::length(a - c);
      double t0 = std::atan2(a.y - c.y, a.x - c.x);
      double t1 = std::atan2(b.y - c.y, b.x - c.x);
      if (bulge < 0.0)
        std::swap(t0, t1); // clockwise a→b is CCW b→a, stored reversed
      while (t1 <= t0)
        t1 += kTwoPi;
      Ctx inner = ctx;
      inner.xf = ctx.xf.Then(ocs);
      Conic(r, inner, c, glm::dmat2(radius), t0, t1, false);
    }
  }

  void Emit(const std::vector<Record>& recs, const Ctx& ctx) {
    for (size_t k = 0; k < recs.size(); k++) {
      const Record& r = recs[k];
      const std::string& t = r.type;
      if (t == "LINE") {
        Extrusion(r);
        CheckZ(r, ctx, NumOr(r, 30, 0.0));
        CheckZ(r, ctx, NumOr(r, 31, 0.0));
        Add(r, ctx,
            sketch::Line{{NumOr(r, 10, 0.0), NumOr(r, 20, 0.0)},
                         {NumOr(r, 11, 0.0), NumOr(r, 21, 0.0)}});
      } else if (t == "POINT") {
        Extrusion(r);
        CheckZ(r, ctx, NumOr(r, 30, 0.0));
        Add(r, ctx, sketch::Point{{NumOr(r, 10, 0.0), NumOr(r, 20, 0.0)}});
      } else if (t == "CIRCLE" || t == "ARC") {
        const double e = Extrusion(r);
        CheckZ(r, ctx, e * NumOr(r, 30, 0.0));
        const glm::dvec2 c(e * NumOr(r, 10, 0.0), NumOr(r, 20, 0.0));
        const double radius = NumOr(r, 40, 0.0);
        if (!(radius > 0.0))
          throw std::runtime_error(r.Name() + ": radius " +
                                   r.Str(40, "(none)") + " is not positive");
        // OCS x is −X under a −Z extrusion: the circle's own frame mirrors.
        const glm::dmat2 L(e * radius, 0.0, 0.0, radius);
        if (t == "CIRCLE") {
          Conic(r, ctx, c, L, 0.0, kTwoPi, true);
        } else {
          const double deg = std::numbers::pi / 180.0;
          const double t0 = NumOr(r, 50, 0.0) * deg;
          double t1 = NumOr(r, 51, 0.0) * deg;
          while (t1 <= t0)
            t1 += kTwoPi;
          Conic(r, ctx, c, L, t0, t1, false);
        }
      } else if (t == "ELLIPSE") {
        const double e = Extrusion(r);
        CheckZ(r, ctx, NumOr(r, 30, 0.0));
        const glm::dvec2 c(NumOr(r, 10, 0.0), NumOr(r, 20, 0.0));
        const glm::dvec2 a(NumOr(r, 11, 0.0), NumOr(r, 21, 0.0));
        const double ratio = NumOr(r, 40, 1.0);
        if (!(glm::length(a) > 0.0) || !(ratio > 0.0))
          throw std::runtime_error(r.Name() + ": a degenerate ellipse");
        // The minor axis is extrusion × major: a −Z extrusion runs it the
        // other way, which the conic reads as a mirror.
        const glm::dvec2 minor = ratio * glm::dvec2(-e * a.y, e * a.x);
        const glm::dmat2 L(a, minor);
        double t0 = NumOr(r, 41, 0.0), t1 = NumOr(r, 42, kTwoPi);
        while (t1 <= t0)
          t1 += kTwoPi;
        const bool full = std::abs((t1 - t0) - kTwoPi) < 1e-9;
        Conic(r, ctx, c, L, t0, t1, full);
      } else if (t == "LWPOLYLINE") {
        const double e = Extrusion(r);
        CheckZ(r, ctx, e * NumOr(r, 38, 0.0));
        if (NumOr(r, 43, 0.0) != 0.0)
          throw std::runtime_error(r.Name() + ": a polyline with width " +
                                   Trim(r.Str(43)) +
                                   " is a filled band, not an outline");
        std::vector<Vtx> v;
        for (const Pair& p : r.codes) {
          if (p.code == 10)
            v.push_back({{Num(r, p.value), 0.0}, 0.0});
          else if (p.code == 20 && !v.empty())
            v.back().p.y = Num(r, p.value);
          else if (p.code == 42 && !v.empty())
            v.back().bulge = Num(r, p.value);
          else if ((p.code == 40 || p.code == 41) && Num(r, p.value) != 0.0)
            throw std::runtime_error(r.Name() + ": a polyline with width is "
                                                "a filled band, not an "
                                                "outline");
        }
        Polyline(r, ctx, e, v, (IntOr(r, 70, 0) & 1) != 0);
      } else if (t == "POLYLINE") {
        const int flags = IntOr(r, 70, 0);
        if ((flags & (8 | 16 | 64)) != 0)
          throw std::runtime_error(
              r.Name() + ": a " +
              ((flags & 8) != 0 ? "3D polyline" : "polygon mesh") +
              " is not a flat outline");
        const double e = Extrusion(r);
        CheckZ(r, ctx, e * NumOr(r, 30, 0.0));
        if (NumOr(r, 40, 0.0) != 0.0 || NumOr(r, 41, 0.0) != 0.0)
          throw std::runtime_error(r.Name() + ": a polyline with width is a "
                                              "filled band, not an outline");
        std::vector<Vtx> v;
        for (k++; k < recs.size() && recs[k].type == "VERTEX"; k++) {
          const Record& vr = recs[k];
          if (NumOr(vr, 40, 0.0) != 0.0 || NumOr(vr, 41, 0.0) != 0.0)
            throw std::runtime_error(r.Name() + ": a polyline with width is "
                                                "a filled band, not an "
                                                "outline");
          v.push_back(
              {{NumOr(vr, 10, 0.0), NumOr(vr, 20, 0.0)}, NumOr(vr, 42, 0.0)});
        }
        if (k >= recs.size() || recs[k].type != "SEQEND")
          k--; // no SEQEND: the next record is the next entity
        Polyline(r, ctx, e, v, (flags & 1) != 0);
      } else if (t == "SPLINE") {
        Spline(r, ctx);
      } else if (t == "INSERT") {
        Insert(r, ctx);
        if (IntOr(r, 66, 0) == 1) // attributes follow, then SEQEND
          for (k++; k < recs.size() && recs[k].type != "SEQEND"; k++)
            m_notImported[recs[k].type]++;
      } else if (t == "TEXT" || t == "MTEXT" || t == "DIMENSION" ||
                 t == "LEADER" || t == "MLEADER" || t == "MULTILEADER" ||
                 t == "HATCH" || t == "SOLID" || t == "IMAGE" ||
                 t == "VIEWPORT" || t == "ATTDEF" || t == "ATTRIB") {
        m_notImported[t == "MULTILEADER" ? "MLEADER" : t]++;
      } else if (t == "VERTEX" || t == "SEQEND") {
        // consumed by the POLYLINE before it
      } else {
        throw std::runtime_error(
            r.Name() +
            " is not read. Imported: LINE, CIRCLE, ARC, LWPOLYLINE, "
            "POLYLINE, ELLIPSE, SPLINE, POINT, INSERT; left out as "
            "annotation: TEXT, MTEXT, DIMENSION, LEADER, MLEADER, HATCH, "
            "SOLID, IMAGE, VIEWPORT, ATTDEF, ATTRIB");
      }
    }
  }

  void Spline(const Record& r, const Ctx& ctx) {
    Extrusion(r);
    const int flags = IntOr(r, 70, 0);
    const int degree = IntOr(r, 71, 3);
    std::vector<double> knots, weights;
    std::vector<glm::dvec2> poles;
    int fit = 0;
    for (const Pair& p : r.codes) {
      if (p.code == 40)
        knots.push_back(Num(r, p.value));
      else if (p.code == 41)
        weights.push_back(Num(r, p.value));
      else if (p.code == 10)
        poles.push_back({Num(r, p.value), 0.0});
      else if (p.code == 20 && !poles.empty())
        poles.back().y = Num(r, p.value);
      else if (p.code == 30)
        CheckZ(r, ctx, Num(r, p.value));
      else if (p.code == 11)
        fit++;
    }
    if (poles.empty())
      throw std::runtime_error(
          r.Name() + (fit > 0 ? ": a spline given only by fit points is not "
                                "read (its curve is the writer's choice); "
                                "save it with control points"
                              : ": a spline with no control points"));
    // Rational only when the weights differ: equal weights are the
    // non-rational curve exactly.
    bool rational = false;
    for (double w : weights)
      if (std::abs(w - weights.front()) > 1e-12 * std::abs(weights.front()))
        rational = true;
    if ((flags & 4) != 0 && rational)
      throw std::runtime_error(r.Name() +
                               ": a rational spline (weighted poles) is not "
                               "read; the sketch's splines are non-rational");
    std::vector<sketch::BSpline> pieces;
    try {
      pieces = sketchimport::SplineToSketch(knots, poles, degree);
    } catch (const std::exception& e) {
      throw std::runtime_error(r.Name() + ": " + e.what());
    }
    for (sketch::BSpline& piece : pieces)
      Add(r, ctx, piece);
  }

  void Insert(const Record& r, const Ctx& ctx) {
    const std::string name = Trim(r.Str(2));
    const auto it = m_blocks.find(name);
    if (it == m_blocks.end())
      throw std::runtime_error(r.Name() + " places block '" + name +
                               "', which the file does not define");
    if (IntOr(r, 70, 1) > 1 || IntOr(r, 71, 1) > 1)
      throw std::runtime_error(r.Name() + ": an INSERT array (MINSERT) is "
                                          "not read");
    if (ctx.depth >= kMaxInsertDepth)
      throw std::runtime_error(r.Name() + ": blocks nested deeper than " +
                               std::to_string(kMaxInsertDepth));
    if (std::find(ctx.stack.begin(), ctx.stack.end(), name) != ctx.stack.end())
      throw std::runtime_error(r.Name() + ": block '" + name +
                               "' contains itself");
    const double e = Extrusion(r);
    const double sx = NumOr(r, 41, 1.0), sy = NumOr(r, 42, 1.0),
                 sz = NumOr(r, 43, 1.0);
    if (sx == 0.0 || sy == 0.0)
      throw std::runtime_error(r.Name() + ": a zero scale");
    const double rot = NumOr(r, 50, 0.0) * std::numbers::pi / 180.0;
    const glm::dvec2 at(NumOr(r, 10, 0.0), NumOr(r, 20, 0.0));
    const Block& b = it->second;
    // OCS mirror ∘ T(at) ∘ R(rot) ∘ S(sx, sy) ∘ T(−base).
    const double c = std::cos(rot), s = std::sin(rot);
    const glm::dmat2 R(c, s, -s, c);
    const glm::dmat2 S(sx, 0.0, 0.0, sy);
    const glm::dmat2 M(e, 0.0, 0.0, 1.0);
    Affine2 local{M * R * S, M * (at - R * S * b.base)};
    Ctx inner = ctx;
    inner.xf = ctx.xf.Then(local);
    inner.zOff = ctx.zOff + ctx.zScale * e * (NumOr(r, 30, 0.0) - sz * b.baseZ);
    inner.zScale = ctx.zScale * e * sz;
    const std::string layer = LayerOf(r, ctx);
    inner.insertLayer = layer;
    if (std::abs(std::abs(sx) - std::abs(sy)) >
        1e-9 * std::max(std::abs(sx), std::abs(sy)))
      inner.uneven = r.Name();
    inner.depth = ctx.depth + 1;
    inner.stack.push_back(name);
    Emit(b.records, inner);
  }
};

} // namespace

sketchimport::Drawing Parse(std::string_view text, const Options& options) {
  Reader reader(options);
  return reader.Run(text);
}

sketchimport::Drawing Read(const std::string& path, const Options& options) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read '" + path + "'");
  std::stringstream buf;
  buf << in.rdbuf();
  return Parse(buf.str(), options);
}

} // namespace dxfimport
