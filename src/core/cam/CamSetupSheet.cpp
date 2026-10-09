// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/CamSetupSheet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace cam {

namespace {

// A4 portrait, millimetres; the PDF's points are 72 to the inch.
constexpr double kW = 210.0, kH = 297.0, kMargin = 12.0;
constexpr double kPt = 72.0 / 25.4;
constexpr double kContentW = kW - 2.0 * kMargin;
// Where the page's body stops, above the footer.
constexpr double kBottom = kH - kMargin - 6.0;

// Helvetica's advance widths, ASCII 32..126, in thousandths of an em (the
// AFM's). Bold is wider by about this much; a wrap that errs wide only
// breaks a line early.
constexpr int kHelvetica[95] = {
    278, 278, 355, 556, 556,  889, 667, 191, 333, 333, 389, 584, 278, 333,
    278, 278, 556, 556, 556,  556, 556, 556, 556, 556, 556, 556, 278, 278,
    584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278,
    500, 667, 556, 833, 722,  778, 667, 778, 722, 667, 611, 722, 667, 944,
    667, 667, 611, 278, 278,  278, 469, 556, 333, 556, 556, 500, 556, 556,
    278, 556, 556, 222, 222,  500, 222, 833, 556, 556, 556, 556, 333, 500,
    278, 556, 500, 722, 500,  500, 500, 334, 260, 334, 584};
constexpr double kBoldWider = 1.08;

// UTF-8 to the WinAnsi bytes the base-14 fonts draw: ASCII as it is, the few
// signs a machinist's page uses (degrees, plus-minus, the diameter, micro,
// times, dashes) mapped, anything else a '?'.
std::string WinAnsi(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out += static_cast<char>(c);
      i++;
      continue;
    }
    uint32_t cp = 0;
    size_t n = 0;
    if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n = 3;
    }
    if (n == 0 || i + n >= s.size()) {
      out += '?';
      i++;
      continue;
    }
    for (size_t k = 1; k <= n && i + k < s.size(); k++)
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += n + 1;
    if (cp == 0x2013)
      out += static_cast<char>(0x96);
    else if (cp == 0x2014)
      out += static_cast<char>(0x97);
    else if (cp == 0x2026)
      out += static_cast<char>(0x85);
    else if (cp == 0x2264)
      out += "<=";
    else if (cp == 0x2265)
      out += ">=";
    else if (cp >= 0xA0 && cp <= 0xFF)
      out += static_cast<char>(cp);
    else
      out += '?';
  }
  return out;
}

double Advance(unsigned char c) {
  if (c >= 32 && c <= 126)
    return kHelvetica[c - 32];
  switch (c) {
  case 0xB0:
    return 400; // degree
  case 0xD8:
    return 778; // O slash
  case 0x97:
    return 1000; // em dash
  default:
    return 584;
  }
}

// A WinAnsi string's width in mm at `size` points.
double Width(const std::string& ansi, double size, bool bold) {
  double em = 0.0;
  for (const char c : ansi)
    em += Advance(static_cast<unsigned char>(c));
  return em / 1000.0 * size / kPt * (bold ? kBoldWider : 1.0);
}

// `text` broken into lines no wider than `width` mm, at its spaces; a word
// wider than the line is broken where it must be.
std::vector<std::string> Wrap(const std::string& text, double width,
                              double size, bool bold) {
  std::vector<std::string> lines;
  std::istringstream paragraphs(text);
  for (std::string para; std::getline(paragraphs, para);) {
    std::istringstream words(para);
    std::string line;
    for (std::string word; words >> word;) {
      const std::string tryLine = line.empty() ? word : line + " " + word;
      if (Width(tryLine, size, bold) <= width) {
        line = tryLine;
        continue;
      }
      if (!line.empty())
        lines.push_back(line);
      line.clear();
      while (Width(word, size, bold) > width && word.size() > 1) {
        size_t cut = word.size() - 1;
        while (cut > 1 && Width(word.substr(0, cut), size, bold) > width)
          cut--;
        lines.push_back(word.substr(0, cut));
        word = word.substr(cut);
      }
      line = word;
    }
    lines.push_back(line);
  }
  if (lines.empty())
    lines.push_back("");
  return lines;
}

std::string Escape(const std::string& ansi) {
  std::string out;
  for (const char c : ansi) {
    if (c == '\\' || c == '(' || c == ')')
      out += '\\';
    out += c;
  }
  return out;
}

std::string F(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

// Line height for a size in points, mm.
double Lead(double size) { return size / kPt * 1.25; }

class Pages {
public:
  std::vector<std::string> pages;
  double y = kMargin;

  std::ostringstream& Out() { return out_; }
  void NewPage() {
    Flush();
    y = kMargin;
  }
  void Flush() {
    if (!out_.str().empty() || pages.empty())
      pages.push_back(out_.str());
    out_.str("");
    out_.clear();
  }
  // Text with its baseline at `baseline` mm from the top.
  void Text(double x, double baseline, const std::string& ansi, double size,
            bool bold, double grey = 0.0) {
    out_ << F(grey) << " g BT /" << (bold ? "F2 " : "F1 ") << F(size) << " Tf "
         << F(x * kPt) << ' ' << F((kH - baseline) * kPt) << " Td ("
         << Escape(ansi) << ") Tj ET\n";
  }
  void Rule(double x0, double yy, double x1, double width = 0.25,
            double grey = 0.0) {
    out_ << F(grey) << " G " << F(width) << " w " << F(x0 * kPt) << ' '
         << F((kH - yy) * kPt) << " m " << F(x1 * kPt) << ' '
         << F((kH - yy) * kPt) << " l S\n";
  }
  void Fill(double x, double top, double w, double h, double grey) {
    out_ << F(grey) << " g " << F(x * kPt) << ' ' << F((kH - top - h) * kPt)
         << ' ' << F(w * kPt) << ' ' << F(h * kPt) << " re f\n";
  }
  void Box(double x, double top, double w, double h, double grey) {
    out_ << F(grey) << " G 0.3 w " << F(x * kPt) << ' '
         << F((kH - top - h) * kPt) << ' ' << F(w * kPt) << ' ' << F(h * kPt)
         << " re S\n";
  }
  // A polyline through page points (mm, from the top), closed when asked,
  // stroked or filled.
  void Path(const std::vector<std::array<double, 2>>& pts, bool closed,
            double width, double grey, bool dashed, bool fill = false) {
    if (pts.size() < 2)
      return;
    out_ << (dashed ? "[2 1.2] 0 d " : "") << F(grey) << (fill ? " g " : " G ")
         << F(width) << " w ";
    for (size_t i = 0; i < pts.size(); i++)
      out_ << F(pts[i][0] * kPt) << ' ' << F((kH - pts[i][1]) * kPt)
           << (i == 0 ? " m " : " l ");
    out_ << (fill     ? "h f"
             : closed ? "h S"
                      : "S")
         << (dashed ? " [] 0 d" : "") << "\n";
  }
  void Image(double x, double top, double w, double h) {
    out_ << "q " << F(w * kPt) << " 0 0 " << F(h * kPt) << ' ' << F(x * kPt)
         << ' ' << F((kH - top - h) * kPt) << " cm /Im1 Do Q\n";
  }

private:
  std::ostringstream out_;
};

constexpr double kBody = 7.5; // table and fact text, points
constexpr double kHead = 9.5; // a section's heading
constexpr double kTitle = 15.0;

// One table, across the page, breaking onto a new page between rows and
// repeating its header there.
void DrawTable(Pages& p, const SetupSheet::Table& t) {
  std::vector<double> widths = t.widths;
  if (widths.size() != t.columns.size())
    widths.assign(t.columns.size(),
                  1.0 / std::max<size_t>(1, t.columns.size()));
  double total = 0.0;
  for (const double w : widths)
    total += w;
  for (double& w : widths)
    w = w / total * kContentW;
  constexpr double pad = 1.0;
  const double lead = Lead(kBody);
  const auto rowLines = [&](const std::vector<std::string>& cells, bool bold) {
    std::vector<std::vector<std::string>> lines;
    for (size_t c = 0; c < widths.size(); c++)
      lines.push_back(Wrap(WinAnsi(c < cells.size() ? cells[c] : ""),
                           widths[c] - 2.0 * pad, kBody, bold));
    return lines;
  };
  const auto height = [&](const std::vector<std::vector<std::string>>& l) {
    size_t n = 1;
    for (const auto& cell : l)
      n = std::max(n, cell.size());
    return static_cast<double>(n) * lead + 1.6;
  };
  const auto drawRow = [&](const std::vector<std::vector<std::string>>& l,
                           bool header) {
    const double h = height(l);
    if (header)
      p.Fill(kMargin, p.y, kContentW, h, 0.88);
    double x = kMargin;
    for (size_t c = 0; c < l.size(); c++) {
      for (size_t k = 0; k < l[c].size(); k++)
        p.Text(x + pad, p.y + lead * static_cast<double>(k + 1) - 0.2, l[c][k],
               kBody, header);
      x += widths[c];
    }
    p.y += h;
    p.Rule(kMargin, p.y, kMargin + kContentW, 0.2, 0.6);
  };
  const auto head = rowLines(t.columns, true);
  const auto heading = [&](bool continued) {
    p.y += Lead(kHead);
    p.Text(kMargin, p.y, WinAnsi(t.heading + (continued ? " (continued)" : "")),
           kHead, true);
    p.y += 1.5;
    drawRow(head, true);
  };
  if (p.y + Lead(kHead) + height(head) * 2.0 > kBottom)
    p.NewPage();
  heading(false);
  for (const auto& row : t.rows) {
    const auto l = rowLines(row, false);
    if (p.y + height(l) > kBottom) {
      p.NewPage();
      heading(true);
    }
    drawRow(l, false);
  }
  p.y += 2.0;
}

} // namespace

std::string SetupSheetPdf(const SetupSheet& sheet) {
  Pages p;
  // Title and subtitle, and a rule under them.
  p.y += Lead(kTitle) * 0.8;
  p.Text(kMargin, p.y, WinAnsi(sheet.title), kTitle, true);
  p.y += Lead(8.5);
  p.Text(kMargin, p.y, WinAnsi(sheet.subtitle), 8.5, false, 0.3);
  p.y += 2.5;
  p.Rule(kMargin, p.y, kMargin + kContentW, 0.6);
  p.y += 3.0;

  // The facts on the left, the picture on the right.
  const double top = p.y;
  constexpr double picW = 84.0;
  const double factsW = kContentW - picW - 5.0;
  constexpr double labelW = 27.0;
  const double lead = Lead(kBody);
  double fy = top;
  for (const SetupSheet::Fact& f : sheet.facts) {
    const std::vector<std::string> label =
        Wrap(WinAnsi(f.label), labelW - 1.0, kBody, true);
    const std::vector<std::string> value =
        Wrap(WinAnsi(f.value), factsW - labelW, kBody, false);
    const size_t n = std::max(label.size(), value.size());
    for (size_t k = 0; k < label.size(); k++)
      p.Text(kMargin, fy + lead * static_cast<double>(k + 1), label[k], kBody,
             true);
    for (size_t k = 0; k < value.size(); k++)
      p.Text(kMargin + labelW, fy + lead * static_cast<double>(k + 1), value[k],
             kBody, false);
    fy += lead * static_cast<double>(n) + 0.9;
  }
  const double picX = kMargin + kContentW - picW;
  double picH = picW * 0.75;
  if (sheet.imageWidth > 0 && sheet.imageHeight > 0 &&
      sheet.rgb.size() >= size_t{sheet.imageWidth} * sheet.imageHeight * 3) {
    picH = picW * static_cast<double>(sheet.imageHeight) /
           static_cast<double>(sheet.imageWidth);
    p.Image(picX, top, picW, picH);
  }
  p.Box(picX, top, picW, picH, 0.5);
  double py = top + picH;
  if (!sheet.imageCaption.empty())
    for (const std::string& l :
         Wrap(WinAnsi(sheet.imageCaption), picW, 6.5, false)) {
      py += Lead(6.5);
      p.Text(picX, py, l, 6.5, false, 0.3);
    }
  p.y = std::max(fy, py) + 3.0;

  // The plan, from above: the stock dashed, the fixtures grey, the part's
  // outline, the zero and its axes -- what the picture cannot show.
  if (sheet.plan.stock.size() >= 3) {
    constexpr double planH = 46.0, planW = 84.0;
    const double left = kMargin, top0 = p.y + Lead(kHead) + 1.5;
    p.Text(left, p.y + Lead(kHead), "Plan, from above", kHead, true);
    double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
    const auto grow = [&](const SetupSheet::Polyline& l) {
      for (const auto& q : l)
        for (int a = 0; a < 2; a++) {
          lo[a] = std::min(lo[a], q[static_cast<size_t>(a)]);
          hi[a] = std::max(hi[a], q[static_cast<size_t>(a)]);
        }
    };
    grow(sheet.plan.stock);
    for (const auto& l : sheet.plan.fixtures)
      grow(l);
    for (int a = 0; a < 2; a++) {
      lo[a] = std::min(lo[a], 0.0);
      hi[a] = std::max(hi[a], 0.0);
    }
    const double pad = 6.0;
    const double scale =
        std::min((planW - 2 * pad) / std::max(1e-6, hi[0] - lo[0]),
                 (planH - 2 * pad) / std::max(1e-6, hi[1] - lo[1]));
    const double cx = left + planW * 0.5, cy = top0 + planH * 0.5;
    const double mx = (lo[0] + hi[0]) * 0.5, my = (lo[1] + hi[1]) * 0.5;
    const auto page = [&](const std::array<double, 2>& q) {
      return std::array<double, 2>{cx + (q[0] - mx) * scale,
                                   cy - (q[1] - my) * scale};
    };
    const auto onPage = [&](const SetupSheet::Polyline& l) {
      SetupSheet::Polyline out;
      for (const auto& q : l)
        out.push_back(page(q));
      return out;
    };
    p.Box(left, top0, planW, planH, 0.75);
    for (const auto& f : sheet.plan.fixtures) {
      p.Path(onPage(f), true, 0.3, 0.8, false, /*fill=*/true);
      p.Path(onPage(f), true, 0.3, 0.45, false);
    }
    p.Path(onPage(sheet.plan.stock), true, 0.35, 0.25, /*dashed=*/true);
    for (const auto& l : sheet.plan.part)
      p.Path(onPage(l), true, 0.5, 0.0, false);
    // The zero: a ring and the two axes, named.
    const std::array<double, 2> z = page({0.0, 0.0});
    constexpr double arm = 9.0;
    SetupSheet::Polyline ring;
    for (int i = 0; i <= 16; i++) {
      const double a = 6.283185307179586 * i / 16.0;
      ring.push_back({z[0] + 1.3 * std::cos(a), z[1] + 1.3 * std::sin(a)});
    }
    p.Path(ring, true, 0.4, 0.0, false);
    p.Path({{z[0], z[1]}, {z[0] + arm, z[1]}}, false, 0.5, 0.0, false);
    p.Path({{z[0] + arm, z[1]},
            {z[0] + arm - 1.6, z[1] - 0.8},
            {z[0] + arm - 1.6, z[1] + 0.8}},
           true, 0.3, 0.0, false, true);
    p.Path({{z[0], z[1]}, {z[0], z[1] - arm}}, false, 0.5, 0.0, false);
    p.Path({{z[0], z[1] - arm},
            {z[0] - 0.8, z[1] - arm + 1.6},
            {z[0] + 0.8, z[1] - arm + 1.6}},
           true, 0.3, 0.0, false, true);
    p.Text(z[0] + arm + 0.8, z[1] + 1.0, "X+", 6.5, true);
    p.Text(z[0] + 1.0, z[1] - arm - 0.6, "Y+", 6.5, true);
    p.Text(z[0] + 1.6, z[1] + 3.2, "X0 Y0", 6.5, true);
    // Beside it, what it shows.
    double ty = top0;
    for (const std::string& l : Wrap(WinAnsi(sheet.plan.caption),
                                     kContentW - planW - 6.0, kBody, false)) {
      ty += lead;
      p.Text(left + planW + 5.0, ty, l, kBody, false);
    }
    p.y = std::max(top0 + planH, ty) + 3.0;
  }

  // The notes before the tables: the bands to gauge and what the tool table
  // holds are what a page that runs over must not push onto its second.
  if (!sheet.notes.empty()) {
    if (p.y + Lead(kHead) + lead * 3.0 > kBottom)
      p.NewPage();
    p.y += Lead(kHead);
    p.Text(kMargin, p.y, "Notes", kHead, true);
    p.y += 1.0;
    for (const std::string& note : sheet.notes) {
      const std::vector<std::string> lines =
          Wrap(WinAnsi(note), kContentW - 4.0, kBody, false);
      for (size_t k = 0; k < lines.size(); k++) {
        if (p.y + lead > kBottom)
          p.NewPage();
        p.y += lead;
        if (k == 0)
          p.Text(kMargin, p.y, "-", kBody, true);
        p.Text(kMargin + 4.0, p.y, lines[k], kBody, false);
      }
      p.y += 1.0;
    }
    p.y += 2.0;
  }

  for (const SetupSheet::Table& t : sheet.tables)
    DrawTable(p, t);

  p.Flush();

  // The footer, now that the pages are counted.
  const size_t count = p.pages.size();
  for (size_t i = 0; i < count; i++) {
    std::ostringstream foot;
    const double fy2 = kH - kMargin + 2.0;
    const auto text = [&](double x, const std::string& ansi) {
      foot << "0.35 g BT /F1 6.5 Tf " << F(x * kPt) << ' '
           << F((kH - fy2) * kPt) << " Td (" << Escape(ansi) << ") Tj ET\n";
    };
    foot << "0.6 G 0.3 w " << F(kMargin * kPt) << ' '
         << F((kH - fy2 + 3.0) * kPt) << " m " << F((kMargin + kContentW) * kPt)
         << ' ' << F((kH - fy2 + 3.0) * kPt) << " l S\n";
    text(kMargin, WinAnsi(sheet.footer));
    const std::string n =
        "page " + std::to_string(i + 1) + " of " + std::to_string(count);
    text(kMargin + kContentW - Width(n, 6.5, false), n);
    p.pages[i] += foot.str();
  }

  // The objects: catalog, pages, two fonts, the picture, then a page and its
  // contents for each page.
  const bool image =
      sheet.imageWidth > 0 && sheet.imageHeight > 0 &&
      sheet.rgb.size() >= size_t{sheet.imageWidth} * sheet.imageHeight * 3;
  std::vector<std::string> objs;
  const size_t firstPage = 6;
  std::string kids;
  for (size_t i = 0; i < count; i++)
    kids += std::to_string(firstPage + 2 * i) + " 0 R ";
  objs.push_back("<</Type /Catalog /Pages 2 0 R>>");
  objs.push_back("<</Type /Pages /Kids [" + kids + "] /Count " +
                 std::to_string(count) + ">>");
  objs.push_back("<</Type /Font /Subtype /Type1 /BaseFont /Helvetica "
                 "/Encoding /WinAnsiEncoding>>");
  objs.push_back("<</Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold "
                 "/Encoding /WinAnsiEncoding>>");
  if (image) {
    const size_t bytes = size_t{sheet.imageWidth} * sheet.imageHeight * 3;
    objs.push_back("<</Type /XObject /Subtype /Image /Width " +
                   std::to_string(sheet.imageWidth) + " /Height " +
                   std::to_string(sheet.imageHeight) +
                   " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length " +
                   std::to_string(bytes) + ">>\nstream\n" +
                   std::string(sheet.rgb.begin(),
                               sheet.rgb.begin() + static_cast<long>(bytes)) +
                   "\nendstream");
  } else {
    objs.push_back("<<>>"); // keeps the page objects' numbers fixed
  }
  for (size_t i = 0; i < count; i++) {
    objs.push_back(
        "<</Type /Page /Parent 2 0 R /MediaBox [0 0 " + F(kW * kPt) + ' ' +
        F(kH * kPt) + "] /Resources <</Font <</F1 3 0 R /F2 4 0 R>>" +
        (image ? std::string(" /XObject <</Im1 5 0 R>>") : "") +
        ">> /Contents " + std::to_string(firstPage + 2 * i + 1) + " 0 R>>");
    objs.push_back("<</Length " + std::to_string(p.pages[i].size()) +
                   ">>\nstream\n" + p.pages[i] + "\nendstream");
  }
  std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objs.size(); ++i) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
  }
  const size_t xref = pdf.size();
  pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n";
  pdf += "0000000000 65535 f \n";
  for (const size_t off : offsets) {
    char line[24];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", off);
    pdf += line;
  }
  pdf += "trailer\n<</Size " + std::to_string(objs.size() + 1) +
         " /Root 1 0 R>>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
  return pdf;
}

bool WriteSetupSheetPdf(const SetupSheet& sheet, const std::string& path,
                        std::string* error) {
  const std::string pdf = SetupSheetPdf(sheet);
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    if (error != nullptr)
      *error = "cannot open " + path + " for writing";
    return false;
  }
  f << pdf;
  f.close();
  if (!f) {
    if (error != nullptr)
      *error = "could not write " + path;
    return false;
  }
  return true;
}

} // namespace cam
