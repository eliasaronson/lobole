#include "core/drawing/DrawingExport.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace DrawingExport {

namespace {

// Sheet extent in mm for the current orientation, from the shared chrome.
glm::dvec2 Extent(const Sheet& sheet) { return ChromeOf(sheet).extent; }

// XML-escape text for an SVG attribute / element body.
std::string Esc(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out += c;
    }
  }
  return out;
}

// Trim a double to a compact fixed form (0.001 mm precision is well under
// plotter resolution and keeps the file small + diffable).
std::string Num(double v) {
  if (std::abs(v) < 1e-9)
    v = 0.0; // avoid "-0"
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(3);
  os << v;
  std::string s = os.str();
  // Strip trailing zeros / dot.
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0')
      s.pop_back();
    if (s.back() == '.')
      s.pop_back();
  }
  return s;
}

// Escapes a UTF-8 display string into PDF WinAnsi bytes: the three
// non-ASCII glyphs dimension text uses (Ø, °, ×) map to their WinAnsi code
// points; other non-ASCII is replaced with '?'. Parens/backslash escaped.
std::string PdfText(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == 0xC3 && i + 1 < s.size() &&
        static_cast<unsigned char>(s[i + 1]) == 0x98) { // Ø
      out += static_cast<char>(0xD8);
      i += 2;
    } else if (c == 0xC3 && i + 1 < s.size() &&
               static_cast<unsigned char>(s[i + 1]) == 0x97) { // × (callouts)
      out += static_cast<char>(0xD7);
      i += 2;
    } else if (c == 0xC2 && i + 1 < s.size() &&
               static_cast<unsigned char>(s[i + 1]) == 0xB0) { // °
      out += static_cast<char>(0xB0);
      i += 2;
    } else if (c < 0x80) {
      if (c == '\\' || c == '(' || c == ')')
        out += '\\';
      out += static_cast<char>(c);
      ++i;
    } else {
      out += '?';
      ++i;
    }
  }
  return out;
}

} // namespace

bool SaveSVG(const DrawingDocument& doc,
             const std::vector<drawing::SheetView>& views,
             const std::vector<drawing::ResolvedDim>& dims,
             const std::string& path, std::string* error) {
  try {
    const Sheet& sheet = doc.GetSheet();
    const SheetChrome chrome = ChromeOf(sheet);
    const double W = chrome.extent.x, H = chrome.extent.y;
    // Sheet mm is Y-up; SVG is Y-down. Flip Y about the sheet height so the
    // written coordinates read as sheet mm and text stays upright.
    const auto fy = [H](double y) { return H - y; };

    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << Num(W)
        << "mm\" height=\"" << Num(H) << "mm\" viewBox=\"0 0 " << Num(W) << " "
        << Num(H) << "\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n";
    out << "  <rect x=\"0\" y=\"0\" width=\"" << Num(W) << "\" height=\""
        << Num(H) << "\" fill=\"white\"/>\n";

    // Drawing border (shared chrome; ISO ~10 mm margin).
    const double m = chrome.margin;
    out << "  <rect x=\"" << Num(m) << "\" y=\"" << Num(m) << "\" width=\""
        << Num(W - 2 * m) << "\" height=\"" << Num(H - 2 * m)
        << "\" fill=\"none\" stroke=\"#141418\" stroke-width=\"0.5\"/>\n";

    // View geometry: visible edges solid, hidden edges dashed.
    for (const drawing::SheetView& v : views) {
      if (v.curves.empty())
        continue;
      out << "  <g id=\"view-" << v.viewId << "\">\n";
      for (const drawing::SheetCurve& c : v.curves) {
        if (c.points.size() < 2)
          continue;
        out << "    <polyline points=\"";
        for (size_t i = 0; i < c.points.size(); ++i) {
          if (i)
            out << ' ';
          out << Num(c.points[i].x) << ',' << Num(fy(c.points[i].y));
        }
        out << "\" fill=\"none\" stroke=\"#141418\" stroke-width=\""
            << (c.visible ? "0.35" : "0.25") << '"';
        if (!c.visible)
          out << " stroke-dasharray=\"1.6 1.2\"";
        out << "/>\n";
      }
      out << "  </g>\n";
    }

    // Dimensions: extension/dimension lines (linear) or a centre->rim leader
    // (radial), filled arrowheads, and the value text. Dangling dims are
    // skipped in output (the viewport flags them; a shop drawing omits them).
    const auto line = [&](glm::dvec2 a, glm::dvec2 b, double w) {
      out << "    <line x1=\"" << Num(a.x) << "\" y1=\"" << Num(fy(a.y))
          << "\" x2=\"" << Num(b.x) << "\" y2=\"" << Num(fy(b.y))
          << "\" stroke=\"#141418\" stroke-width=\"" << Num(w) << "\"/>\n";
    };
    // A filled arrowhead at `tip` pointing along unit `dir`.
    const auto arrow = [&](glm::dvec2 tip, glm::dvec2 dir) {
      const glm::dvec2 n(-dir.y, dir.x);
      const double len = 2.2, w = 0.8; // mm
      const glm::dvec2 base = tip - dir * len;
      const glm::dvec2 p1 = base + n * w, p2 = base - n * w;
      out << "    <polygon points=\"" << Num(tip.x) << ',' << Num(fy(tip.y))
          << ' ' << Num(p1.x) << ',' << Num(fy(p1.y)) << ' ' << Num(p2.x) << ','
          << Num(fy(p2.y)) << "\" fill=\"#141418\"/>\n";
    };
    const auto label = [&](glm::dvec2 at, const std::string& text) {
      out << "    <text x=\"" << Num(at.x) << "\" y=\"" << Num(fy(at.y))
          << "\" fill=\"#141418\" text-anchor=\"middle\">" << Esc(text)
          << "</text>\n";
    };
    if (!dims.empty()) {
      out << "  <g font-family=\"Helvetica, sans-serif\" font-size=\"3.5\">\n";
      for (const drawing::ResolvedDim& d : dims) {
        if (!d.resolved)
          continue;
        if (d.radial) {
          line(d.center, d.rim, 0.25);
          arrow(d.rim, glm::normalize(d.rim - d.center));
        } else if (d.leader) { // callout: rim -> label leader
          line(d.rim, d.labelPos, 0.25);
          if (glm::length(d.rim - d.labelPos) > 1e-9)
            arrow(d.rim, glm::normalize(d.rim - d.labelPos));
        } else if (d.kind == DrawingDimKind::Angle) {
          // v1 angle: just the value, placed between the two edges.
        } else {                     // linear
          line(d.ext1, d.dim1, 0.2); // extension lines
          line(d.ext2, d.dim2, 0.2);
          line(d.dim1, d.dim2, 0.25); // dimension line
          const double L = glm::length(d.dim2 - d.dim1);
          if (L > 1e-6) {
            const glm::dvec2 u = (d.dim2 - d.dim1) / L;
            arrow(d.dim1, u);
            arrow(d.dim2, -u);
          }
        }
        // A small white gap behind the text keeps it legible over the line.
        label(d.labelPos, d.text);
      }
      out << "  </g>\n";
    }

    // Title block: a labelled table in the bottom-right corner of the border.
    const double tbW = chrome.titleBlockMax.x - chrome.titleBlockMin.x;
    const double tbH = chrome.titleBlockMax.y - chrome.titleBlockMin.y;
    const double x0 = chrome.titleBlockMin.x,
                 y0 = chrome.titleBlockMin.y; // bottom-left (sheet mm)
    struct Row {
      const char* label;
      std::string value;
    };
    const Row rows[] = {{"PROJECT", sheet.titleBlock.project},
                        {"DOCUMENT", sheet.titleBlock.document},
                        {"AUTHOR", sheet.titleBlock.author},
                        {"DATE", sheet.titleBlock.date}};
    const int nRows = 4;
    const double rowH = tbH / nRows;
    out << "  <g font-family=\"Helvetica, sans-serif\" font-size=\"3.5\">\n";
    out << "    <rect x=\"" << Num(x0) << "\" y=\"" << Num(fy(y0 + tbH))
        << "\" width=\"" << Num(tbW) << "\" height=\"" << Num(tbH)
        << "\" fill=\"none\" stroke=\"#141418\" stroke-width=\"0.35\"/>\n";
    for (int i = 0; i < nRows; ++i) {
      // Rows top-to-bottom: row 0 is the topmost (highest sheet y).
      const double ry =
          y0 + tbH - (i + 1) * rowH; // row's bottom edge (sheet mm)
      if (i > 0)
        out << "    <line x1=\"" << Num(x0) << "\" y1=\"" << Num(fy(ry + rowH))
            << "\" x2=\"" << Num(x0 + tbW) << "\" y2=\"" << Num(fy(ry + rowH))
            << "\" stroke=\"#141418\" stroke-width=\"0.2\"/>\n";
      const double ty = fy(ry + 1.0); // text baseline near the row bottom
      out << "    <text x=\"" << Num(x0 + 2.0) << "\" y=\"" << Num(ty)
          << "\" fill=\"#787878\">" << Esc(rows[i].label) << "</text>\n";
      out << "    <text x=\"" << Num(x0 + 34.0) << "\" y=\"" << Num(ty)
          << "\" fill=\"#141418\">" << Esc(rows[i].value) << "</text>\n";
    }
    out << "  </g>\n";
    out << "</svg>\n";

    std::ofstream f(path);
    if (!f) {
      if (error)
        *error = "cannot open file for writing: " + path;
      return false;
    }
    f << out.str();
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

bool SavePDF(const DrawingDocument& doc,
             const std::vector<drawing::SheetView>& views,
             const std::vector<drawing::ResolvedDim>& dims,
             const std::string& path, std::string* error) {
  try {
    const Sheet& sheet = doc.GetSheet();
    const SheetChrome chrome = ChromeOf(sheet);
    const double k = 72.0 / 25.4; // points per millimetre
    const double W = chrome.extent.x, H = chrome.extent.y;
    // PDF user space is Y-up with the origin bottom-left — same as sheet mm —
    // so coordinates are just scaled, never flipped.
    const auto P = [k](double mm) { return Num(mm * k); };

    std::ostringstream cs; // the page content stream (path + text operators)
    cs << "1 1 1 rg 0 0 " << P(W) << ' ' << P(H) << " re f\n"; // white page
    cs << "0 0 0 RG 0 0 0 rg\n";

    // Border (shared chrome).
    const double m = chrome.margin;
    cs << P(1.5 * 0.35) << " w " << P(m) << ' ' << P(m) << ' ' << P(W - 2 * m)
       << ' ' << P(H - 2 * m) << " re S\n";

    const auto poly = [&](const std::vector<glm::dvec2>& pts, double w,
                          bool dash) {
      if (pts.size() < 2)
        return;
      cs << P(w) << " w ";
      if (dash)
        cs << "[" << P(1.6) << ' ' << P(1.2) << "] 0 d ";
      cs << P(pts[0].x) << ' ' << P(pts[0].y) << " m ";
      for (size_t i = 1; i < pts.size(); ++i)
        cs << P(pts[i].x) << ' ' << P(pts[i].y) << " l ";
      cs << "S\n";
      if (dash)
        cs << "[] 0 d\n";
    };
    const auto seg = [&](glm::dvec2 a, glm::dvec2 b, double w) {
      cs << P(w) << " w " << P(a.x) << ' ' << P(a.y) << " m " << P(b.x) << ' '
         << P(b.y) << " l S\n";
    };
    const auto arrow = [&](glm::dvec2 tip, glm::dvec2 dir) {
      const glm::dvec2 n(-dir.y, dir.x);
      const double len = 2.2, w = 0.8;
      const glm::dvec2 base = tip - dir * len, p1 = base + n * w,
                       p2 = base - n * w;
      cs << P(tip.x) << ' ' << P(tip.y) << " m " << P(p1.x) << ' ' << P(p1.y)
         << " l " << P(p2.x) << ' ' << P(p2.y) << " l f\n";
    };
    const auto text = [&](glm::dvec2 at, double sizeMm, const std::string& s,
                          double gray, bool center) {
      double x = at.x;
      // Rough centring: Helvetica averages ~0.5em/char.
      if (center)
        x -= 0.25 * sizeMm * static_cast<double>(PdfText(s).size());
      cs << gray << ' ' << gray << ' ' << gray << " rg BT /F1 " << P(sizeMm)
         << " Tf " << P(x) << ' ' << P(at.y) << " Td (" << PdfText(s)
         << ") Tj ET\n";
      cs << "0 0 0 rg\n";
    };

    for (const drawing::SheetView& v : views)
      for (const drawing::SheetCurve& c : v.curves)
        poly(c.points, c.visible ? 0.35 : 0.25, !c.visible);

    for (const drawing::ResolvedDim& d : dims) {
      if (!d.resolved)
        continue;
      if (d.radial) {
        seg(d.center, d.rim, 0.25);
        arrow(d.rim, glm::normalize(d.rim - d.center));
      } else if (d.leader) { // callout: rim -> label leader
        seg(d.rim, d.labelPos, 0.25);
        if (glm::length(d.rim - d.labelPos) > 1e-9)
          arrow(d.rim, glm::normalize(d.rim - d.labelPos));
      } else if (d.kind != DrawingDimKind::Angle) {
        seg(d.ext1, d.dim1, 0.2);
        seg(d.ext2, d.dim2, 0.2);
        seg(d.dim1, d.dim2, 0.25);
        const double L = glm::length(d.dim2 - d.dim1);
        if (L > 1e-6) {
          const glm::dvec2 u = (d.dim2 - d.dim1) / L;
          arrow(d.dim1, u);
          arrow(d.dim2, -u);
        }
      }
      text(d.labelPos, 3.5, d.text, 0.08, true);
    }

    // Title block.
    const double tbW = chrome.titleBlockMax.x - chrome.titleBlockMin.x;
    const double tbH = chrome.titleBlockMax.y - chrome.titleBlockMin.y;
    const double x0 = chrome.titleBlockMin.x, y0 = chrome.titleBlockMin.y;
    struct Row {
      const char* label;
      std::string value;
    };
    const Row rows[] = {{"PROJECT", sheet.titleBlock.project},
                        {"DOCUMENT", sheet.titleBlock.document},
                        {"AUTHOR", sheet.titleBlock.author},
                        {"DATE", sheet.titleBlock.date}};
    const int nRows = 4;
    const double rowH = tbH / nRows;
    cs << P(0.35) << " w " << P(x0) << ' ' << P(y0) << ' ' << P(tbW) << ' '
       << P(tbH) << " re S\n";
    for (int i = 0; i < nRows; ++i) {
      const double ry = y0 + tbH - (i + 1) * rowH;
      if (i > 0)
        seg({x0, ry + rowH}, {x0 + tbW, ry + rowH}, 0.2);
      text({x0 + 2.0, ry + 1.0}, 3.5, rows[i].label, 0.47, false);
      text({x0 + 34.0, ry + 1.0}, 3.5, rows[i].value, 0.08, false);
    }

    const std::string content = cs.str();
    std::vector<std::string> objs = {
        "<</Type /Catalog /Pages 2 0 R>>",
        "<</Type /Pages /Kids [3 0 R] /Count 1>>",
        "<</Type /Page /Parent 2 0 R /MediaBox [0 0 " + Num(W * k) + ' ' +
            Num(H * k) +
            "] /Resources <</Font <</F1 5 0 R>>>> /Contents 4 0 R>>",
        "<</Length " + std::to_string(content.size()) + ">>\nstream\n" +
            content + "\nendstream",
        "<</Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
        "/WinAnsiEncoding>>"};

    std::string pdf = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objs.size(); ++i) {
      offsets.push_back(pdf.size());
      pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n";
    pdf += "0000000000 65535 f \n";
    for (size_t off : offsets) {
      char line[24];
      std::snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
      pdf += line;
    }
    pdf += "trailer\n<</Size " + std::to_string(objs.size() + 1) +
           " /Root 1 0 R>>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) {
      if (error)
        *error = "cannot open file for writing: " + path;
      return false;
    }
    f << pdf;
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

} // namespace DrawingExport
