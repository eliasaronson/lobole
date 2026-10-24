// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/ImportSvg.h"

#include <LDOMParser.hxx>
#include <LDOM_Document.hxx>
#include <LDOM_Element.hxx>
#include <LDOM_Node.hxx>
#include <TCollection_AsciiString.hxx>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>

namespace svgimport {

namespace {

using sketchimport::Affine2;
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr int kMaxDepth = 32;
constexpr double kMmPerPx = 25.4 / 96.0;

// Millimetres per unit of the lengths a root's width/height may carry.
const std::map<std::string, double>& AbsoluteUnits() {
  static const std::map<std::string, double> k = {{"mm", 1.0},
                                                  {"cm", 10.0},
                                                  {"in", 25.4},
                                                  {"pt", 25.4 / 72.0},
                                                  {"pc", 25.4 / 6.0}};
  return k;
}
// What `units` may name: one user unit when the root does not fix it.
const std::map<std::string, double>& OptionUnits() {
  static const std::map<std::string, double> k = {
      {"mm", 1.0},  {"cm", 10.0},     {"m", 1000.0},
      {"in", 25.4}, {"px", kMmPerPx}, {"pt", 25.4 / 72.0}};
  return k;
}

std::string Trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    b--;
  return std::string(s.substr(a, b - a));
}

std::string Attr(const LDOM_Element& el, const char* name) {
  const LDOMString v = el.getAttribute(name);
  if (v.Type() == LDOMBasicString::LDOM_Integer) {
    Standard_Integer i = 0;
    v.GetInteger(i);
    return std::to_string(i);
  }
  return v.GetString();
}

std::string Tag(const LDOM_Element& el) {
  std::string t = el.getTagName().GetString();
  if (t.rfind("svg:", 0) == 0)
    t = t.substr(4);
  return t;
}

// A number at s[pos], SVG's grammar: sign, digits, a point, an exponent.
// "1.5.5" is two numbers, "-1-2" two more.
bool ReadNumber(std::string_view s, size_t& pos, double& out) {
  while (pos < s.size() &&
         (std::isspace(static_cast<unsigned char>(s[pos])) || s[pos] == ','))
    pos++;
  size_t end = pos;
  if (end < s.size() && (s[end] == '+' || s[end] == '-'))
    end++;
  bool digits = false, dot = false;
  while (end < s.size()) {
    if (std::isdigit(static_cast<unsigned char>(s[end]))) {
      digits = true;
    } else if (s[end] == '.' && !dot) {
      dot = true;
    } else {
      break;
    }
    end++;
  }
  if (!digits)
    return false;
  if (end < s.size() && (s[end] == 'e' || s[end] == 'E')) {
    size_t e = end + 1;
    if (e < s.size() && (s[e] == '+' || s[e] == '-'))
      e++;
    if (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) {
      while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e])))
        e++;
      end = e;
    }
  }
  const char* first = s.data() + pos;
  if (*first == '+')
    first++;
  const auto res = std::from_chars(first, s.data() + end, out);
  if (res.ec != std::errc() || !std::isfinite(out))
    return false;
  pos = end;
  return true;
}

std::vector<double> Numbers(std::string_view s) {
  std::vector<double> out;
  size_t pos = 0;
  double v = 0.0;
  while (ReadNumber(s, pos, v))
    out.push_back(v);
  return out;
}

// A length: a number with an optional unit, split.
bool SplitLength(const std::string& text, double& value, std::string& unit) {
  const std::string t = Trim(text);
  size_t pos = 0;
  if (!ReadNumber(t, pos, value))
    return false;
  unit = Trim(std::string_view(t).substr(pos));
  return true;
}

// A CSS declaration from `style`, else the presentation attribute.
std::string Property(const LDOM_Element& el, const std::string& name) {
  const std::string style = Attr(el, "style");
  size_t pos = 0;
  while (pos < style.size()) {
    size_t semi = style.find(';', pos);
    if (semi == std::string::npos)
      semi = style.size();
    const std::string decl = style.substr(pos, semi - pos);
    const size_t colon = decl.find(':');
    if (colon != std::string::npos && Trim(decl.substr(0, colon)) == name)
      return Trim(decl.substr(colon + 1));
    pos = semi + 1;
  }
  return Trim(Attr(el, name.c_str()));
}

Affine2 Translate(double x, double y) {
  return {glm::dmat2(1.0), glm::dvec2(x, y)};
}

glm::dmat2 Rot(double a) {
  const double c = std::cos(a), s = std::sin(a);
  return glm::dmat2(c, s, -s, c);
}

struct Style {
  bool stroke = false;
  double strokeWidth = 1.0;
};

class Reader {
public:
  explicit Reader(const Options& options) : m_opt(options) {}

  sketchimport::Drawing Run(const std::string& path) {
    LDOMParser parser;
    if (parser.parse(path.c_str())) { // true is an ERROR here
      TCollection_AsciiString data;
      throw std::runtime_error("'" + path + "' is not XML LDOMParser reads: " +
                               std::string(parser.GetError(data).ToCString()));
    }
    LDOM_Document doc = parser.getDocument();
    const LDOM_Element root = doc.getDocumentElement();
    if (root.isNull() || Tag(root) != "svg")
      throw std::runtime_error("'" + path + "' is not an SVG: its root is <" +
                               (root.isNull() ? std::string() : Tag(root)) +
                               ">");
    Index(root);
    const Affine2 rootXf = RootTransform(root);
    Style style;
    Children(root, rootXf, style, 0);
    if (m_stroked > 0)
      m_out.notes.push_back(
          std::to_string(m_stroked) +
          (m_stroked == 1 ? " stroked shape" : " stroked shapes") +
          " imported as centrelines: a stroke's width is not geometry (a "
          "logo of thick strokes needs Stroke to Path in its own program)");
    return std::move(m_out);
  }

private:
  const Options& m_opt;
  sketchimport::Drawing m_out;
  std::map<std::string, LDOM_Element> m_ids;
  int m_stroked = 0;
  int m_ordinal = 0;

  void Index(const LDOM_Element& el) {
    const std::string id = Attr(el, "id");
    if (!id.empty() && !m_ids.contains(id))
      m_ids.emplace(id, el);
    for (LDOM_Node n = el.getFirstChild(); !n.isNull(); n = n.getNextSibling())
      if (n.getNodeType() == LDOM_Node::ELEMENT_NODE)
        Index(static_cast<const LDOM_Element&>(n));
  }

  std::string Who(const LDOM_Element& el) {
    const std::string id = Attr(el, "id");
    return "<" + Tag(el) + (id.empty() ? "" : " id=\"" + id + "\"") + ">";
  }

  // The root's units and viewBox as one affine to millimetres, Y flipped.
  Affine2 RootTransform(const LDOM_Element& root) {
    double w = 0.0, h = 0.0;
    std::string wu, hu;
    const bool hasW = SplitLength(Attr(root, "width"), w, wu);
    const bool hasH = SplitLength(Attr(root, "height"), h, hu);
    const std::vector<double> vb = Numbers(Attr(root, "viewBox"));
    const bool hasVb = vb.size() == 4 && vb[2] > 0.0 && vb[3] > 0.0;
    const auto& abs = AbsoluteUnits();
    const bool absolute =
        hasW && hasH && abs.contains(wu) && abs.contains(hu) && hasVb;
    double sx = 1.0, sy = 1.0;
    glm::dvec2 off(0.0);
    if (absolute) {
      if (!m_opt.units.empty())
        throw std::runtime_error(
            "units: the SVG fixes its own scale (width " + Attr(root, "width") +
            ", height " + Attr(root, "height") +
            " with a viewBox), so `units` would contradict it");
      const double W = w * abs.at(wu), H = h * abs.at(hu);
      sx = W / vb[2];
      sy = H / vb[3];
      if (Trim(Attr(root, "preserveAspectRatio")).rfind("none", 0) != 0 &&
          std::abs(sx - sy) > 1e-12 * std::max(sx, sy)) {
        // The default, xMidYMid meet: one scale, centred.
        const double s = std::min(sx, sy);
        off = 0.5 * glm::dvec2(W - vb[2] * s, H - vb[3] * s);
        sx = sy = s;
      }
      m_out.unit = "mm";
      m_out.unitSource = "the root's width " + Attr(root, "width") +
                         " and height " + Attr(root, "height") +
                         " with its viewBox";
    } else {
      std::string unit = m_opt.units.empty() ? "px" : m_opt.units;
      const auto it = OptionUnits().find(unit);
      if (it == OptionUnits().end())
        throw std::runtime_error("units: unknown '" + unit +
                                 "'. Accepted: " + AcceptedUnits());
      double perUser = 1.0;
      if (hasVb && hasW && (wu.empty() || wu == "px") && w > 0.0)
        perUser = w / vb[2];
      sx = sy = perUser * it->second;
      m_out.unit = unit;
      m_out.unitSource =
          m_opt.units.empty()
              ? "the default: user units are CSS px (1/96 in), as the SVG "
                "declares no absolute size"
              : "units (the SVG declares no absolute size)";
    }
    // user -> viewport (mm) -> Y up.
    Affine2 xf{glm::dmat2(sx, 0.0, 0.0, sy),
               off - glm::dvec2(sx * (hasVb ? vb[0] : 0.0),
                                sy * (hasVb ? vb[1] : 0.0))};
    const Affine2 flip{glm::dmat2(1.0, 0.0, 0.0, -1.0), glm::dvec2(0.0)};
    return flip.Then(xf);
  }

  Affine2 ParseTransform(const LDOM_Element& el) {
    const std::string t = Attr(el, "transform");
    Affine2 out;
    size_t pos = 0;
    while (pos < t.size()) {
      while (
          pos < t.size() &&
          (std::isspace(static_cast<unsigned char>(t[pos])) || t[pos] == ','))
        pos++;
      if (pos >= t.size())
        break;
      const size_t open = t.find('(', pos);
      const size_t close = open == std::string::npos ? open : t.find(')', open);
      if (close == std::string::npos)
        throw std::runtime_error(Who(el) + ": transform '" + t +
                                 "' is not one this reader parses");
      const std::string fn = Trim(std::string_view(t).substr(pos, open - pos));
      const std::vector<double> a =
          Numbers(std::string_view(t).substr(open + 1, close - open - 1));
      Affine2 m;
      const double deg = std::numbers::pi / 180.0;
      if (fn == "matrix" && a.size() == 6) {
        m = {glm::dmat2(a[0], a[1], a[2], a[3]), glm::dvec2(a[4], a[5])};
      } else if (fn == "translate" && (a.size() == 1 || a.size() == 2)) {
        m = Translate(a[0], a.size() == 2 ? a[1] : 0.0);
      } else if (fn == "scale" && (a.size() == 1 || a.size() == 2)) {
        m = {glm::dmat2(a[0], 0.0, 0.0, a.size() == 2 ? a[1] : a[0]),
             glm::dvec2(0.0)};
      } else if (fn == "rotate" && (a.size() == 1 || a.size() == 3)) {
        m = {Rot(a[0] * deg), glm::dvec2(0.0)};
        if (a.size() == 3)
          m = Translate(a[1], a[2]).Then(m).Then(Translate(-a[1], -a[2]));
      } else if (fn == "skewX" && a.size() == 1) {
        m = {glm::dmat2(1.0, 0.0, std::tan(a[0] * deg), 1.0), glm::dvec2(0.0)};
      } else if (fn == "skewY" && a.size() == 1) {
        m = {glm::dmat2(1.0, std::tan(a[0] * deg), 0.0, 1.0), glm::dvec2(0.0)};
      } else {
        throw std::runtime_error(Who(el) + ": transform " + fn + "(...) " +
                                 "with " + std::to_string(a.size()) +
                                 " numbers is not one SVG defines");
      }
      out = out.Then(m);
      pos = close + 1;
    }
    return out;
  }

  // A geometry attribute: a plain number, or px.
  double Len(const LDOM_Element& el, const char* name, double fallback = 0.0) {
    const std::string raw = Attr(el, name);
    if (Trim(raw).empty())
      return fallback;
    double v = 0.0;
    std::string unit;
    if (!SplitLength(raw, v, unit) || !(unit.empty() || unit == "px"))
      throw std::runtime_error(Who(el) + ": " + name + "=\"" + raw +
                               "\" is not a length in user units");
    return v;
  }

  void Add(const sketch::Entity& userSpace, const Affine2& ctm) {
    m_out.entities.push_back(sketchimport::TransformEntity(userSpace, ctm));
  }
  void Line(glm::dvec2 a, glm::dvec2 b, const Affine2& ctm) {
    if (a != b)
      Add(sketch::Line{a, b}, ctm);
  }
  void Conic(glm::dvec2 c, const glm::dmat2& L, double t0, double t1, bool full,
             const Affine2& ctm) {
    m_out.entities.push_back(
        sketchimport::ConicEntity(ctm.Apply(c), ctm.lin * L, t0, t1, full));
  }

  void Children(const LDOM_Element& el, const Affine2& ctm, const Style& style,
                int depth) {
    for (LDOM_Node n = el.getFirstChild(); !n.isNull(); n = n.getNextSibling())
      if (n.getNodeType() == LDOM_Node::ELEMENT_NODE)
        Walk(static_cast<const LDOM_Element&>(n), ctm, style, depth);
  }

  void Walk(const LDOM_Element& el, const Affine2& parent,
            const Style& inherited, int depth) {
    if (depth > kMaxDepth)
      throw std::runtime_error(Who(el) + ": nested deeper than " +
                               std::to_string(kMaxDepth) +
                               " (a <use> that refers to itself?)");
    m_ordinal++;
    const std::string tag = Tag(el);
    // Another namespace's element (an editor's sodipodi:namedview, its
    // metadata) draws nothing.
    if (tag.find(':') != std::string::npos)
      return;
    static const std::set<std::string> kSilent = {
        "defs",           "title",   "desc",   "metadata",
        "style",          "script",  "symbol", "linearGradient",
        "radialGradient", "pattern", "marker", "filter",
        "clipPath",       "mask",    "font",   "font-face"};
    if (kSilent.contains(tag))
      return; // defined, not drawn (a symbol draws through a <use>)
    if (Property(el, "display") == "none" ||
        Property(el, "visibility") == "hidden") {
      m_out.notImported["hidden"]++;
      return;
    }
    if (!Property(el, "clip-path").empty() &&
        Property(el, "clip-path") != "none")
      throw std::runtime_error(Who(el) + " is clipped (clip-path): the cut "
                                         "would not be the picture");
    if (!Property(el, "mask").empty() && Property(el, "mask") != "none")
      throw std::runtime_error(Who(el) + " is masked: the cut would not be "
                                         "the picture");
    Style style = inherited;
    if (const std::string s = Property(el, "stroke"); !s.empty())
      style.stroke = s != "none";
    if (const std::string w = Property(el, "stroke-width"); !w.empty()) {
      double v = 0.0;
      std::string unit;
      if (SplitLength(w, v, unit))
        style.strokeWidth = v;
    }
    const Affine2 ctm = parent.Then(ParseTransform(el));
    const bool drawn = tag == "path" || tag == "line" || tag == "polyline" ||
                       tag == "polygon" || tag == "rect" || tag == "circle" ||
                       tag == "ellipse";
    if (drawn && style.stroke && style.strokeWidth > 0.0)
      m_stroked++;
    if (tag == "g" || tag == "a" || tag == "switch") {
      Children(el, ctm, style, depth + 1);
    } else if (tag == "svg") {
      // A nested viewport: placed at x, y; its viewBox maps onto its size.
      Affine2 inner = ctm.Then(Translate(Len(el, "x"), Len(el, "y")));
      const std::vector<double> vb = Numbers(Attr(el, "viewBox"));
      if (vb.size() == 4 && vb[2] > 0.0 && vb[3] > 0.0) {
        const double s = std::min(Len(el, "width", vb[2]) / vb[2],
                                  Len(el, "height", vb[3]) / vb[3]);
        inner =
            inner.Then(Affine2{glm::dmat2(s), -s * glm::dvec2(vb[0], vb[1])});
      }
      Children(el, inner, style, depth + 1);
    } else if (tag == "use") {
      std::string href = Attr(el, "href");
      if (href.empty())
        href = Attr(el, "xlink:href");
      if (href.size() < 2 || href[0] != '#' || !m_ids.contains(href.substr(1)))
        throw std::runtime_error(Who(el) + " refers to '" + href +
                                 "', which this file does not define");
      const LDOM_Element& target = m_ids.at(href.substr(1));
      const Affine2 placed = ctm.Then(Translate(Len(el, "x"), Len(el, "y")));
      if (Tag(target) == "symbol") {
        if (!Trim(Attr(target, "viewBox")).empty())
          throw std::runtime_error(Who(el) + ": a <symbol> with a viewBox is " +
                                   "not read");
        Children(target, placed, style, depth + 1);
      } else {
        Walk(target, placed, style, depth + 1);
      }
    } else if (tag == "path") {
      Path(el, ctm);
    } else if (tag == "line") {
      Line({Len(el, "x1"), Len(el, "y1")}, {Len(el, "x2"), Len(el, "y2")}, ctm);
    } else if (tag == "polyline" || tag == "polygon") {
      const std::vector<double> v = Numbers(Attr(el, "points"));
      std::vector<glm::dvec2> pts;
      for (size_t i = 0; i + 1 < v.size(); i += 2)
        pts.push_back({v[i], v[i + 1]});
      for (size_t i = 0; i + 1 < pts.size(); i++)
        Line(pts[i], pts[i + 1], ctm);
      if (tag == "polygon" && pts.size() > 2)
        Line(pts.back(), pts.front(), ctm);
    } else if (tag == "rect") {
      Rect(el, ctm);
    } else if (tag == "circle") {
      const double r = Len(el, "r");
      if (r > 0.0)
        Conic({Len(el, "cx"), Len(el, "cy")}, glm::dmat2(r), 0.0, kTwoPi, true,
              ctm);
    } else if (tag == "ellipse") {
      const double rx = Len(el, "rx"), ry = Len(el, "ry");
      if (rx > 0.0 && ry > 0.0)
        Conic({Len(el, "cx"), Len(el, "cy")}, glm::dmat2(rx, 0.0, 0.0, ry), 0.0,
              kTwoPi, true, ctm);
    } else if (tag == "text") {
      m_out.notImported["text"]++;
    } else if (tag == "image") {
      if (m_out.notImported["image"]++ == 0)
        m_out.notes.push_back("an image is not geometry: add_reference_image "
                              "puts one under a sketch to trace");
    } else {
      throw std::runtime_error(
          Who(el) +
          " is not read. Drawn: path, line, polyline, polygon, rect, circle, "
          "ellipse (in svg, g, a, use/symbol); left out: text, image, hidden "
          "elements");
    }
  }

  void Rect(const LDOM_Element& el, const Affine2& ctm) {
    const double x = Len(el, "x"), y = Len(el, "y");
    const double w = Len(el, "width"), h = Len(el, "height");
    if (!(w > 0.0) || !(h > 0.0))
      return; // the spec draws nothing
    const std::string rxs = Trim(Attr(el, "rx")), rys = Trim(Attr(el, "ry"));
    double rx = rxs.empty() || rxs == "auto" ? -1.0 : Len(el, "rx");
    double ry = rys.empty() || rys == "auto" ? -1.0 : Len(el, "ry");
    if (rx < 0.0 && ry < 0.0)
      rx = ry = 0.0;
    else if (rx < 0.0)
      rx = ry;
    else if (ry < 0.0)
      ry = rx;
    rx = std::min(rx, w / 2.0);
    ry = std::min(ry, h / 2.0);
    if (rx <= 0.0 || ry <= 0.0) {
      Line({x, y}, {x + w, y}, ctm);
      Line({x + w, y}, {x + w, y + h}, ctm);
      Line({x + w, y + h}, {x, y + h}, ctm);
      Line({x, y + h}, {x, y}, ctm);
      return;
    }
    // Rounded corners are quarter ellipses; the sides between them lines.
    const glm::dmat2 L(rx, 0.0, 0.0, ry);
    const double q = std::numbers::pi / 2.0;
    Line({x + rx, y}, {x + w - rx, y}, ctm);
    Conic({x + w - rx, y + ry}, L, 3.0 * q, 4.0 * q, false, ctm);
    Line({x + w, y + ry}, {x + w, y + h - ry}, ctm);
    Conic({x + w - rx, y + h - ry}, L, 0.0, q, false, ctm);
    Line({x + w - rx, y + h}, {x + rx, y + h}, ctm);
    Conic({x + rx, y + h - ry}, L, q, 2.0 * q, false, ctm);
    Line({x, y + h - ry}, {x, y + ry}, ctm);
    Conic({x + rx, y + ry}, L, 2.0 * q, 3.0 * q, false, ctm);
  }

  // SVG 1.1 F.6.5 (endpoint to centre) with F.6.6's radius scale-up.
  void ArcTo(glm::dvec2 p1, glm::dvec2 p2, double rx, double ry, double phiDeg,
             bool large, bool sweep, const Affine2& ctm) {
    if (p1 == p2)
      return; // the spec omits it
    rx = std::abs(rx);
    ry = std::abs(ry);
    if (rx == 0.0 || ry == 0.0) {
      Line(p1, p2, ctm);
      return;
    }
    const double phi = phiDeg * std::numbers::pi / 180.0;
    const double c = std::cos(phi), s = std::sin(phi);
    const glm::dvec2 d = 0.5 * (p1 - p2);
    const double x1 = c * d.x + s * d.y, y1 = -s * d.x + c * d.y;
    const double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1.0) {
      rx *= std::sqrt(lambda);
      ry *= std::sqrt(lambda);
    }
    const double num =
        rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double coef = den > 0.0 ? std::sqrt(std::max(0.0, num / den)) : 0.0;
    if (large == sweep)
      coef = -coef;
    const double cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
    const glm::dvec2 centre(c * cx1 - s * cy1 + 0.5 * (p1.x + p2.x),
                            s * cx1 + c * cy1 + 0.5 * (p1.y + p2.y));
    const auto angle = [](glm::dvec2 u, glm::dvec2 v) {
      return std::atan2(u.x * v.y - u.y * v.x, u.x * v.x + u.y * v.y);
    };
    const glm::dvec2 u((x1 - cx1) / rx, (y1 - cy1) / ry);
    const glm::dvec2 v((-x1 - cx1) / rx, (-y1 - cy1) / ry);
    const double theta1 = angle({1.0, 0.0}, u);
    double delta = angle(u, v);
    if (!sweep && delta > 0.0)
      delta -= kTwoPi;
    else if (sweep && delta < 0.0)
      delta += kTwoPi;
    const glm::dmat2 L = Rot(phi) * glm::dmat2(rx, 0.0, 0.0, ry);
    if (delta >= 0.0)
      Conic(centre, L, theta1, theta1 + delta, false, ctm);
    else
      Conic(centre, L, theta1 + delta, theta1, false, ctm);
  }

  void Path(const LDOM_Element& el, const Affine2& ctm) {
    const std::string d = Attr(el, "d");
    size_t pos = 0;
    glm::dvec2 cur(0.0), start(0.0), ctrl(0.0);
    char cmd = 0, prev = 0;
    const auto bad = [&]() {
      return std::runtime_error(Who(el) + ": its path data is malformed near " +
                                "character " + std::to_string(pos));
    };
    const auto num = [&]() {
      double v = 0.0;
      if (!ReadNumber(d, pos, v))
        throw bad();
      return v;
    };
    const auto flag = [&]() {
      while (
          pos < d.size() &&
          (std::isspace(static_cast<unsigned char>(d[pos])) || d[pos] == ','))
        pos++;
      if (pos >= d.size() || (d[pos] != '0' && d[pos] != '1'))
        throw bad();
      return d[pos++] == '1';
    };
    const auto more = [&]() {
      size_t p = pos;
      double v = 0.0;
      return ReadNumber(d, p, v);
    };
    while (true) {
      while (
          pos < d.size() &&
          (std::isspace(static_cast<unsigned char>(d[pos])) || d[pos] == ','))
        pos++;
      if (pos >= d.size())
        break;
      if (std::isalpha(static_cast<unsigned char>(d[pos]))) {
        cmd = d[pos++];
      } else if (cmd == 0 || cmd == 'Z' || cmd == 'z' || !more()) {
        throw bad(); // a closepath takes no numbers
      } else if (cmd == 'M') {
        cmd = 'L'; // numbers after a moveto are linetos
      } else if (cmd == 'm') {
        cmd = 'l';
      }
      const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
      const glm::dvec2 base = rel ? cur : glm::dvec2(0.0);
      const char up = static_cast<char>(std::toupper(cmd));
      switch (up) {
      case 'M': {
        const double x = num(), y = num();
        cur = start = base + glm::dvec2(x, y);
        break;
      }
      case 'L': {
        const double x = num(), y = num();
        const glm::dvec2 p = base + glm::dvec2(x, y);
        Line(cur, p, ctm);
        cur = p;
        break;
      }
      case 'H': {
        const glm::dvec2 p(rel ? cur.x + num() : num(), cur.y);
        Line(cur, p, ctm);
        cur = p;
        break;
      }
      case 'V': {
        const glm::dvec2 p(cur.x, rel ? cur.y + num() : num());
        Line(cur, p, ctm);
        cur = p;
        break;
      }
      case 'Z':
        Line(cur, start, ctm);
        cur = start;
        break;
      case 'C':
      case 'S': {
        glm::dvec2 c1;
        if (up == 'C') {
          const double x = num(), y = num();
          c1 = base + glm::dvec2(x, y);
        } else {
          const char p = static_cast<char>(std::toupper(prev));
          c1 = p == 'C' || p == 'S' ? 2.0 * cur - ctrl : cur;
        }
        const double x2 = num(), y2 = num(), x = num(), y = num();
        const glm::dvec2 c2 = base + glm::dvec2(x2, y2);
        const glm::dvec2 p = base + glm::dvec2(x, y);
        Add(sketch::BSpline{{cur, c1, c2, p}, 3}, ctm);
        ctrl = c2;
        cur = p;
        break;
      }
      case 'Q':
      case 'T': {
        glm::dvec2 c1;
        if (up == 'Q') {
          const double x = num(), y = num();
          c1 = base + glm::dvec2(x, y);
        } else {
          const char p = static_cast<char>(std::toupper(prev));
          c1 = p == 'Q' || p == 'T' ? 2.0 * cur - ctrl : cur;
        }
        const double x = num(), y = num();
        const glm::dvec2 p = base + glm::dvec2(x, y);
        Add(sketch::BSpline{{cur, c1, p}, 2}, ctm);
        ctrl = c1;
        cur = p;
        break;
      }
      case 'A': {
        const double rx = num(), ry = num(), phi = num();
        const bool large = flag(), sweep = flag();
        const double x = num(), y = num();
        const glm::dvec2 p = base + glm::dvec2(x, y);
        ArcTo(cur, p, rx, ry, phi, large, sweep, ctm);
        cur = p;
        break;
      }
      default:
        throw std::runtime_error(Who(el) + ": path command '" +
                                 std::string(1, cmd) + "' is not SVG's");
      }
      prev = cmd;
    }
  }
};

} // namespace

const char* AcceptedUnits() { return "mm, cm, m, in, px, pt"; }

sketchimport::Drawing Read(const std::string& path, const Options& options) {
  if (!std::filesystem::exists(path))
    throw std::runtime_error("cannot read '" + path + "'");
  if (!options.units.empty() && !OptionUnits().contains(options.units))
    throw std::runtime_error("units: unknown '" + options.units +
                             "'. Accepted: " + std::string(AcceptedUnits()));
  Reader reader(options);
  return reader.Run(path);
}

} // namespace svgimport
