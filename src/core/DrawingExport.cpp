#include "core/DrawingExport.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace DrawingExport {

namespace {

// Sheet extent in mm for the current orientation (landscape swaps portrait
// short/long), matching the viewport's SheetExtent.
glm::dvec2 Extent(const Sheet& sheet) {
  glm::dvec2 ext = SheetExtentMM(sheet.size);
  if (sheet.landscape) ext = {ext.y, ext.x};
  return ext;
}

// XML-escape text for an SVG attribute / element body.
std::string Esc(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out += c;
    }
  }
  return out;
}

// Trim a double to a compact fixed form (0.001 mm precision is well under
// plotter resolution and keeps the file small + diffable).
std::string Num(double v) {
  if (std::abs(v) < 1e-9) v = 0.0;  // avoid "-0"
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(3);
  os << v;
  std::string s = os.str();
  // Strip trailing zeros / dot.
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s;
}

}  // namespace

bool SaveSVG(const DrawingDocument& doc,
             const std::vector<drawing::SheetView>& views,
             const std::string& path, std::string* error) {
  try {
    const Sheet& sheet = doc.GetSheet();
    const glm::dvec2 ext = Extent(sheet);
    const double W = ext.x, H = ext.y;
    // Sheet mm is Y-up; SVG is Y-down. Flip Y about the sheet height so the
    // written coordinates read as sheet mm and text stays upright.
    const auto fy = [H](double y) { return H - y; };

    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << Num(W)
        << "mm\" height=\"" << Num(H) << "mm\" viewBox=\"0 0 " << Num(W) << " "
        << Num(H) << "\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n";
    out << "  <rect x=\"0\" y=\"0\" width=\"" << Num(W) << "\" height=\"" << Num(H)
        << "\" fill=\"white\"/>\n";

    // Drawing border (ISO ~10 mm margin).
    const double m = 10.0;
    out << "  <rect x=\"" << Num(m) << "\" y=\"" << Num(m) << "\" width=\""
        << Num(W - 2 * m) << "\" height=\"" << Num(H - 2 * m)
        << "\" fill=\"none\" stroke=\"#141418\" stroke-width=\"0.5\"/>\n";

    // View geometry: visible edges solid, hidden edges dashed.
    for (const drawing::SheetView& v : views) {
      if (v.curves.empty()) continue;
      out << "  <g id=\"view-" << v.viewId << "\">\n";
      for (const drawing::SheetCurve& c : v.curves) {
        if (c.points.size() < 2) continue;
        out << "    <polyline points=\"";
        for (size_t i = 0; i < c.points.size(); ++i) {
          if (i) out << ' ';
          out << Num(c.points[i].x) << ',' << Num(fy(c.points[i].y));
        }
        out << "\" fill=\"none\" stroke=\"#141418\" stroke-width=\""
            << (c.visible ? "0.35" : "0.25") << '"';
        if (!c.visible) out << " stroke-dasharray=\"1.6 1.2\"";
        out << "/>\n";
      }
      out << "  </g>\n";
    }

    // Title block: a labelled table in the bottom-right corner of the border.
    const double tbW = std::min(160.0, W - 2 * m);
    const double tbH = 32.0;
    const double x0 = W - m - tbW, y0 = m;  // bottom-left of the block (sheet mm)
    struct Row { const char* label; std::string value; };
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
      const double ry = y0 + tbH - (i + 1) * rowH;  // row's bottom edge (sheet mm)
      if (i > 0)
        out << "    <line x1=\"" << Num(x0) << "\" y1=\"" << Num(fy(ry + rowH))
            << "\" x2=\"" << Num(x0 + tbW) << "\" y2=\"" << Num(fy(ry + rowH))
            << "\" stroke=\"#141418\" stroke-width=\"0.2\"/>\n";
      const double ty = fy(ry + 1.0);  // text baseline near the row bottom
      out << "    <text x=\"" << Num(x0 + 2.0) << "\" y=\"" << Num(ty)
          << "\" fill=\"#787878\">" << Esc(rows[i].label) << "</text>\n";
      out << "    <text x=\"" << Num(x0 + 34.0) << "\" y=\"" << Num(ty)
          << "\" fill=\"#141418\">" << Esc(rows[i].value) << "</text>\n";
    }
    out << "  </g>\n";
    out << "</svg>\n";

    std::ofstream f(path);
    if (!f) {
      if (error) *error = "cannot open file for writing: " + path;
      return false;
    }
    f << out.str();
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

}  // namespace DrawingExport
