#include "core/sketch/SketchText.h"

#include "common/RuntimePaths.h"

#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Font_FTFont.hxx>
#include <GeomConvert_BSplineCurveToBezierCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_Line.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <StdPrs_BRepFont.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace sketch {

namespace {

// One segment of a glyph contour in the font's own frame, normalised so the
// cap height is 1 and the glyph's origin (pen position, baseline) is (0,0).
// A line has 2 points; a Bezier its 3 or 4 poles.
struct RawSegment {
  std::vector<glm::dvec2> pts;
  bool line = true;
};
struct RawContour {
  bool closed = true;
  std::vector<RawSegment> segs;
};
struct RawGlyph {
  std::vector<RawContour> contours;
  double advance = 0.0; // normalised (hershey; outline asks FreeType)
};

// ---------------------------------------------------------------------------
// Fonts

struct FontEntry {
  const char* name;
  bool stroke;
  const char* file; // under DataDir()/fonts (outline only)
};
constexpr FontEntry kFonts[] = {
    {"sans", false, "LiberationSans-Regular.ttf"},
    {"hershey", true, nullptr},
};

const FontEntry* FindFont(std::string_view name) {
  for (const FontEntry& f : kFonts)
    if (name == f.name)
      return &f;
  return nullptr;
}

std::string CharName(char32_t c) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "U+%04X", static_cast<unsigned>(c));
  std::string printable;
  if (c >= 0x20 && c < 0x7F) {
    printable = std::string("'") + static_cast<char>(c) + "' (" + buf + ")";
  } else {
    // Re-encode as UTF-8 so the refusal shows the character itself.
    std::string u;
    if (c < 0x80) {
      u += static_cast<char>(c);
    } else if (c < 0x800) {
      u += static_cast<char>(0xC0 | (c >> 6));
      u += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      u += static_cast<char>(0xE0 | (c >> 12));
      u += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      u += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      u += static_cast<char>(0xF0 | (c >> 18));
      u += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      u += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      u += static_cast<char>(0x80 | (c & 0x3F));
    }
    printable = "'" + u + "' (" + buf + ")";
  }
  return printable;
}

std::expected<std::u32string, std::string> DecodeUtf8(const std::string& s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int len = 0;
    char32_t cp = 0;
    if (c < 0x80) {
      len = 1;
      cp = c;
    } else if ((c & 0xE0) == 0xC0) {
      len = 2;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      len = 4;
      cp = c & 0x07;
    } else {
      return std::unexpected("text is not valid UTF-8 (byte " +
                             std::to_string(i) + ")");
    }
    if (i + static_cast<size_t>(len) > s.size())
      return std::unexpected("text is not valid UTF-8 (truncated at byte " +
                             std::to_string(i) + ")");
    for (int k = 1; k < len; k++) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80)
        return std::unexpected("text is not valid UTF-8 (byte " +
                               std::to_string(i + k) + ")");
      cp = (cp << 6) | (cc & 0x3F);
    }
    out.push_back(cp);
    i += static_cast<size_t>(len);
  }
  return out;
}

// ---- Hershey Roman Simplex --------------------------------------------------

constexpr const char* kHersheyRecords[] = {
#include "core/sketch/HersheyRomanSimplex.inc"
};
static_assert(std::size(kHersheyRecords) == 95,
              "Hershey Roman Simplex: one record per printable ASCII char");

struct HersheyFont {
  std::array<RawGlyph, 95> glyphs;
  TextFontMetrics metrics;
};

// The .jhf record decoder: "<number:5><count:3><left><right><pairs>", each
// coordinate a character offset from 'R', " R" lifting the pen, Y DOWN.
// `count` includes the left/right pair.
struct HersheyDecoded {
  int left = 0, right = 0;
  std::vector<std::vector<glm::ivec2>> strokes;
};
HersheyDecoded DecodeHershey(std::string_view rec) {
  HersheyDecoded d;
  const int count = std::stoi(std::string(rec.substr(5, 3)));
  d.left = rec[8] - 'R';
  d.right = rec[9] - 'R';
  std::vector<glm::ivec2> stroke;
  for (int k = 1; k < count; k++) {
    const size_t at = 10 + 2 * static_cast<size_t>(k - 1);
    if (at + 1 >= rec.size())
      break;
    const char cx = rec[at], cy = rec[at + 1];
    if (cx == ' ' && cy == 'R') { // pen up
      if (stroke.size() >= 2)
        d.strokes.push_back(stroke);
      stroke.clear();
      continue;
    }
    stroke.push_back({cx - 'R', cy - 'R'});
  }
  if (stroke.size() >= 2)
    d.strokes.push_back(stroke);
  return d;
}

const HersheyFont& Hershey() {
  static const HersheyFont font = [] {
    HersheyFont f;
    std::array<HersheyDecoded, 95> dec;
    for (size_t i = 0; i < 95; i++)
      dec[i] = DecodeHershey(kHersheyRecords[i]);
    // Cap height from 'H', measured once: its top to its foot, which is
    // also the baseline.
    int top = 1 << 20, foot = -(1 << 20);
    for (const auto& s : dec['H' - 32].strokes)
      for (const glm::ivec2& p : s) {
        top = std::min(top, p.y);
        foot = std::max(foot, p.y);
      }
    const double cap = static_cast<double>(foot - top);
    double asc = 0.0, desc = 0.0;
    for (size_t i = 0; i < 95; i++) {
      RawGlyph& g = f.glyphs[i];
      g.advance = (dec[i].right - dec[i].left) / cap;
      for (const auto& s : dec[i].strokes) {
        RawContour c;
        c.closed = s.front() == s.back();
        for (size_t k = 0; k + 1 < s.size(); k++) {
          const auto norm = [&](glm::ivec2 p) {
            return glm::dvec2((p.x - dec[i].left) / cap, (foot - p.y) / cap);
          };
          RawSegment seg;
          seg.pts = {norm(s[k]), norm(s[k + 1])};
          if (seg.pts[0] == seg.pts[1])
            continue; // a dot drawn as a zero-length move
          for (const glm::dvec2& p : seg.pts) {
            asc = std::max(asc, p.y);
            desc = std::min(desc, p.y);
          }
          c.segs.push_back(std::move(seg));
        }
        if (!c.segs.empty())
          g.contours.push_back(std::move(c));
      }
    }
    f.metrics.ascender = asc;
    f.metrics.descender = desc;
    f.metrics.capHeightEm = 0.0; // no em: the data is in Hershey's units
    return f;
  }();
  return font;
}

// ---- Outline fonts (OpenCASCADE StdPrs_BRepFont) ---------------------------

// The size the font is Init'd at: glyphs come out in units of em/kInitEm.
constexpr double kInitEm = 100.0;

struct OutlineFont {
  Handle(StdPrs_BRepFont) font;
  double cap = 1.0; // cap height in Init units, from 'H'
  TextFontMetrics metrics;
  std::map<char32_t, RawGlyph> glyphs;
  std::string error; // non-empty: the font could not be loaded
};

// FreeType and StdPrs_BRepFont are not thread-safe, and regeneration runs
// features in parallel: every touch of an outline font holds this.
std::mutex& FontMutex() {
  static std::mutex m;
  return m;
}

// Converts one glyph edge to its segment(s) in traversal order. False for a
// curve kind a TrueType outline cannot produce.
bool AppendEdge(const TopoDS_Edge& edge, double scale,
                std::vector<RawSegment>& out) {
  double f = 0.0, l = 0.0;
  Handle(Geom_Curve) curve = BRep_Tool::Curve(edge, f, l);
  if (curve.IsNull())
    return false;
  while (const auto trimmed = Handle(Geom_TrimmedCurve)::DownCast(curve))
    curve = trimmed->BasisCurve();
  const bool reversed = edge.Orientation() == TopAbs_REVERSED;
  const auto toUV = [&](const gp_Pnt& p) {
    return glm::dvec2(p.X() * scale, p.Y() * scale);
  };
  std::vector<RawSegment> segs;
  if (Handle(Geom_Line)::DownCast(curve)) {
    RawSegment s;
    s.pts = {toUV(curve->Value(f)), toUV(curve->Value(l))};
    segs.push_back(std::move(s));
  } else if (const auto bez = Handle(Geom_BezierCurve)::DownCast(curve)) {
    Handle(Geom_BezierCurve) piece = bez;
    if (std::abs(f) > 1e-12 || std::abs(l - 1.0) > 1e-12) {
      piece = Handle(Geom_BezierCurve)::DownCast(bez->Copy());
      piece->Segment(f, l);
    }
    RawSegment s;
    s.line = false;
    for (int i = 1; i <= piece->NbPoles(); i++)
      s.pts.push_back(toUV(piece->Pole(i)));
    segs.push_back(std::move(s));
  } else if (const auto bs = Handle(Geom_BSplineCurve)::DownCast(curve)) {
    GeomConvert_BSplineCurveToBezierCurve split(bs, f, l, 1e-9);
    for (int k = 1; k <= split.NbArcs(); k++) {
      const Handle(Geom_BezierCurve) arc = split.Arc(k);
      RawSegment s;
      s.line = false;
      for (int i = 1; i <= arc->NbPoles(); i++)
        s.pts.push_back(toUV(arc->Pole(i)));
      segs.push_back(std::move(s));
    }
  } else {
    return false;
  }
  if (reversed) {
    std::reverse(segs.begin(), segs.end());
    for (RawSegment& s : segs)
      std::reverse(s.pts.begin(), s.pts.end());
  }
  for (RawSegment& s : segs)
    if (!(s.line && glm::length(s.pts.back() - s.pts.front()) < 1e-12))
      out.push_back(std::move(s));
  return true;
}

// The glyph's contours, extracted from the faces RenderGlyph builds. Caller
// holds FontMutex.
std::expected<RawGlyph, std::string> RenderOutlineGlyph(OutlineFont& of,
                                                        char32_t c) {
  RawGlyph g;
  const TopoDS_Shape shape =
      of.font->RenderGlyph(static_cast<Standard_Utf32Char>(c));
  if (shape.IsNull())
    return g; // no ink (a space)
  const double scale = 1.0 / of.cap;
  for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
    const TopoDS_Face face = TopoDS::Face(fx.Current());
    for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
      RawContour contour;
      for (BRepTools_WireExplorer ex(TopoDS::Wire(wx.Current()), face);
           ex.More(); ex.Next())
        if (!AppendEdge(ex.Current(), scale, contour.segs))
          return std::unexpected("glyph " + CharName(c) +
                                 " has a curve that is neither a line nor "
                                 "a Bezier");
      if (contour.segs.size() < 2 &&
          !(contour.segs.size() == 1 && !contour.segs[0].line))
        continue;
      // Close exactly: each segment starts where the last ended.
      for (size_t k = 1; k < contour.segs.size(); k++)
        contour.segs[k].pts.front() = contour.segs[k - 1].pts.back();
      contour.segs.back().pts.back() = contour.segs.front().pts.front();
      g.contours.push_back(std::move(contour));
    }
  }
  return g;
}

// Loads (once) and returns the outline font. Caller holds FontMutex.
OutlineFont& Outline(const FontEntry& entry) {
  static std::map<std::string, OutlineFont> fonts;
  auto [it, fresh] = fonts.try_emplace(entry.name);
  OutlineFont& of = it->second;
  if (!fresh)
    return of;
  const std::filesystem::path path =
      RuntimePaths::DataDir() / "fonts" / entry.file;
  if (!std::filesystem::exists(path)) {
    of.error = std::string("the bundled font file for \"") + entry.name +
               "\" is missing: " + path.string();
    return of;
  }
  try {
    of.font = new StdPrs_BRepFont();
    if (!of.font->Init(NCollection_String(path.string().c_str()), kInitEm, 0)) {
      of.error = "could not load the font file " + path.string();
      return of;
    }
    // Never borrow a missing glyph from a SYSTEM font: the same document
    // would build differently on two machines. HasSymbol refuses instead.
    of.font->FTFont()->SetUseUnicodeSubsetFallback(false);
    // Composite mode stays OFF (the default): one edge per line or Bezier.
    of.font->SetCompositeCurveMode(false);
    // Cap height, measured once from 'H'.
    of.cap = 1.0;
    auto h = RenderOutlineGlyph(of, U'H');
    double top = 0.0;
    if (h)
      for (const RawContour& c : h->contours)
        for (const RawSegment& s : c.segs)
          for (const glm::dvec2& p : s.pts)
            top = std::max(top, p.y);
    if (top <= 0.0) {
      of.error = std::string("font \"") + entry.name +
                 "\" has no 'H' to measure its cap height from";
      return of;
    }
    of.cap = top;
    of.metrics.ascender = of.font->Ascender() / of.cap;
    of.metrics.descender = of.font->Descender() / of.cap;
    of.metrics.capHeightEm = of.cap / kInitEm;
  } catch (const Standard_Failure& e) {
    of.error = std::string("font \"") + entry.name +
               "\": OpenCASCADE error: " + e.GetMessageString();
  }
  return of;
}

// ---- Layout ----------------------------------------------------------------

struct PlacedGlyph {
  const RawGlyph* glyph = nullptr; // into a font's cache (stable addresses)
  double x = 0.0;                  // pen position, mm (before alignment)
};
struct Layout {
  bool stroke = false;
  std::vector<PlacedGlyph> glyphs; // rendered characters only
  double width = 0.0;              // mm
  TextFontMetrics metrics;
};

std::expected<Layout, std::string> LayOut(const Text& t, bool withGlyphs) {
  const FontEntry* entry = FindFont(t.font);
  if (entry == nullptr)
    return std::unexpected("unknown font \"" + t.font +
                           "\" — the bundled fonts are " + TextFontList());
  if (!(t.height > 0.0) || !std::isfinite(t.height))
    return std::unexpected("text height must be positive (it is the CAP "
                           "height, mm), not " +
                           std::to_string(t.height));
  if (t.text.empty())
    return std::unexpected("text is empty");
  if (t.text.find('\n') != std::string::npos ||
      t.text.find('\r') != std::string::npos)
    return std::unexpected("text is one line: a newline is refused (use a "
                           "second label)");
  auto codes = DecodeUtf8(t.text);
  if (!codes)
    return std::unexpected(codes.error());
  const double h = t.height;
  Layout lay;
  lay.stroke = entry->stroke;
  double pen = 0.0; // normalised (cap = 1)
  const size_t n = codes->size();
  if (entry->stroke) {
    const HersheyFont& hf = Hershey();
    lay.metrics = hf.metrics;
    for (size_t i = 0; i < n; i++) {
      const char32_t c = (*codes)[i];
      if (c < 32 || c > 126)
        return std::unexpected("font \"" + t.font + "\" has no glyph for " +
                               CharName(c) +
                               " — it covers printable ASCII only");
      const RawGlyph& g = hf.glyphs[c - 32];
      if (withGlyphs && !g.contours.empty())
        lay.glyphs.push_back(
            {&g, pen * h + static_cast<double>(i) * t.spacing});
      pen += g.advance;
    }
  } else {
    std::lock_guard<std::mutex> lock(FontMutex());
    OutlineFont& of = Outline(*entry);
    if (!of.error.empty())
      return std::unexpected(of.error);
    lay.metrics = of.metrics;
    const Handle(Font_FTFont) & ft = of.font->FTFont();
    for (size_t i = 0; i < n; i++) {
      const char32_t c = (*codes)[i];
      if (!ft->HasSymbol(static_cast<Standard_Utf32Char>(c)))
        return std::unexpected("font \"" + t.font + "\" has no glyph for " +
                               CharName(c));
    }
    try {
      for (size_t i = 0; i < n; i++) {
        const char32_t c = (*codes)[i];
        if (withGlyphs) {
          auto it = of.glyphs.find(c);
          if (it == of.glyphs.end()) {
            auto g = RenderOutlineGlyph(of, c);
            if (!g)
              return std::unexpected(g.error());
            it = of.glyphs.emplace(c, std::move(*g)).first;
          }
          if (!it->second.contours.empty())
            lay.glyphs.push_back(
                {&it->second, pen * h + static_cast<double>(i) * t.spacing});
        }
        // Kerning from the font's `kern` table, when it has one; no shaping.
        const char32_t next = i + 1 < n ? (*codes)[i + 1] : 0;
        pen += of.font->AdvanceX(static_cast<Standard_Utf32Char>(c),
                                 static_cast<Standard_Utf32Char>(next)) /
               of.cap;
      }
    } catch (const Standard_Failure& e) {
      return std::unexpected(std::string("font \"") + t.font +
                             "\": OpenCASCADE error: " + e.GetMessageString());
    }
  }
  lay.width = pen * h + static_cast<double>(n > 0 ? n - 1 : 0) * t.spacing;
  return lay;
}

// Local (pre-rotation) offset that aligns the box to the anchor.
glm::dvec2 AlignOffset(const Text& t, const Layout& lay) {
  const double h = t.height;
  double ox = 0.0, oy = 0.0;
  switch (t.halign) {
  case TextHAlign::Left:
    ox = 0.0;
    break;
  case TextHAlign::Center:
    ox = -lay.width / 2.0;
    break;
  case TextHAlign::Right:
    ox = -lay.width;
    break;
  }
  switch (t.valign) {
  case TextVAlign::Baseline:
    oy = 0.0;
    break;
  case TextVAlign::Bottom:
    oy = -lay.metrics.descender * h;
    break;
  case TextVAlign::Middle:
    oy = -(lay.metrics.ascender + lay.metrics.descender) * h / 2.0;
    break;
  case TextVAlign::Top:
    oy = -lay.metrics.ascender * h;
    break;
  }
  return {ox, oy};
}

struct Placement {
  glm::dvec2 anchor, offset;
  double c, s;
  glm::vec2 operator()(glm::dvec2 local) const {
    const glm::dvec2 p = local + offset;
    return glm::vec2(anchor + glm::dvec2(c * p.x - s * p.y, s * p.x + c * p.y));
  }
};

std::array<glm::vec2, 4> Box(const Text& t, const Layout& lay,
                             const Placement& place) {
  const double lo = lay.metrics.descender * t.height;
  const double hi = lay.metrics.ascender * t.height;
  return {place({0.0, lo}), place({lay.width, lo}), place({lay.width, hi}),
          place({0.0, hi})};
}

} // namespace

const std::vector<std::string>& TextFontNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (const FontEntry& f : kFonts)
      v.emplace_back(f.name);
    return v;
  }();
  return names;
}

bool IsKnownTextFont(std::string_view font) {
  return FindFont(font) != nullptr;
}

bool IsStrokeFont(std::string_view font) {
  const FontEntry* f = FindFont(font);
  return f != nullptr && f->stroke;
}

std::string TextFontList() {
  std::string out;
  for (const FontEntry& f : kFonts)
    out += (out.empty() ? "" : ", ") + std::string(f.name) +
           (f.stroke ? " (single stroke)" : " (outline)");
  return out;
}

const char* TextHAlignName(TextHAlign a) {
  switch (a) {
  case TextHAlign::Left:
    return "left";
  case TextHAlign::Center:
    return "center";
  case TextHAlign::Right:
    return "right";
  }
  return "left";
}

const char* TextVAlignName(TextVAlign a) {
  switch (a) {
  case TextVAlign::Baseline:
    return "baseline";
  case TextVAlign::Bottom:
    return "bottom";
  case TextVAlign::Middle:
    return "middle";
  case TextVAlign::Top:
    return "top";
  }
  return "baseline";
}

std::optional<TextHAlign> ParseTextHAlign(std::string_view s) {
  for (TextHAlign a : {TextHAlign::Left, TextHAlign::Center, TextHAlign::Right})
    if (s == TextHAlignName(a))
      return a;
  return std::nullopt;
}

std::optional<TextVAlign> ParseTextVAlign(std::string_view s) {
  for (TextVAlign a : {TextVAlign::Baseline, TextVAlign::Bottom,
                       TextVAlign::Middle, TextVAlign::Top})
    if (s == TextVAlignName(a))
      return a;
  return std::nullopt;
}

std::expected<TextFontMetrics, std::string>
TextFontMetricsOf(std::string_view font) {
  const FontEntry* entry = FindFont(font);
  if (entry == nullptr)
    return std::unexpected("unknown font \"" + std::string(font) +
                           "\" — the bundled fonts are " + TextFontList());
  if (entry->stroke)
    return Hershey().metrics;
  std::lock_guard<std::mutex> lock(FontMutex());
  const OutlineFont& of = Outline(*entry);
  if (!of.error.empty())
    return std::unexpected(of.error);
  return of.metrics;
}

std::expected<TextGeometry, std::string> BuildTextGeometry(const Text& t) {
  auto lay = LayOut(t, /*withGlyphs=*/true);
  if (!lay)
    return std::unexpected(lay.error());
  const Placement place{glm::dvec2(t.anchor), AlignOffset(t, *lay),
                        std::cos(t.angle), std::sin(t.angle)};
  TextGeometry out;
  out.stroke = lay->stroke;
  out.width = lay->width;
  out.box = Box(t, *lay, place);
  const double h = t.height;
  for (size_t gi = 0; gi < lay->glyphs.size(); gi++) {
    const PlacedGlyph& pg = lay->glyphs[gi];
    for (size_t ci = 0; ci < pg.glyph->contours.size(); ci++) {
      const RawContour& rc = pg.glyph->contours[ci];
      TextContour tc;
      tc.glyph = static_cast<uint32_t>(gi);
      tc.contour = static_cast<uint32_t>(ci);
      tc.closed = rc.closed;
      glm::vec2 prevEnd{0.0f};
      for (size_t si = 0; si < rc.segs.size(); si++) {
        const RawSegment& rs = rc.segs[si];
        std::vector<glm::vec2> pts;
        pts.reserve(rs.pts.size());
        for (const glm::dvec2& p : rs.pts)
          pts.push_back(place({pg.x + p.x * h, p.y * h}));
        // Float32 placement must not open a joint the font closed.
        if (si > 0)
          pts.front() = prevEnd;
        prevEnd = pts.back();
        if (rs.line) {
          Line l{pts.front(), pts.back()};
          l.id = t.id;
          tc.segments.emplace_back(l);
        } else {
          BSpline b;
          b.poles = std::move(pts);
          b.degree = static_cast<int>(b.poles.size()) - 1;
          b.id = t.id;
          tc.segments.emplace_back(std::move(b));
        }
      }
      if (tc.segments.empty())
        continue;
      if (rc.closed) {
        const glm::vec2 start = EntityPointUV(tc.segments.front(), 0);
        Entity& last = tc.segments.back();
        if (auto* l = std::get_if<Line>(&last))
          l->b = start;
        else if (auto* b = std::get_if<BSpline>(&last))
          b->poles.back() = start;
      }
      out.contours.push_back(std::move(tc));
    }
  }
  return out;
}

glm::vec2 TextPointUV(const Text& t, uint8_t which) {
  if (which == 0)
    return t.anchor;
  auto lay = LayOut(t, /*withGlyphs=*/false);
  if (!lay)
    return t.anchor;
  const Placement place{glm::dvec2(t.anchor), AlignOffset(t, *lay),
                        std::cos(t.angle), std::sin(t.angle)};
  const std::array<glm::vec2, 4> box = Box(t, *lay, place);
  return which >= 1 && which <= 4 ? box[which - 1] : t.anchor;
}

} // namespace sketch
