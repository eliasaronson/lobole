// The setup sheet's page (docs/cam/setup.md#the-setup-sheet): a PDF a
// viewer opens -- every object where the xref says it is, as many pages as
// the tables take, text a base-14 font can draw. Built in every
// configuration, as the post is.

#include "core/cam/CamSetupSheet.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

namespace {

// Every "n 0 obj" sits at the offset the xref gives it.
bool XrefHolds(const std::string& pdf) {
  const size_t xref = pdf.rfind("\nxref\n");
  if (xref == std::string::npos)
    return false;
  const size_t start = pdf.find("startxref\n");
  if (start == std::string::npos ||
      std::strtoull(pdf.c_str() + start + 10, nullptr, 10) != xref + 1)
    return false;
  size_t at = pdf.find('\n', xref + 6); // past "0 N"
  at = pdf.find('\n', at + 1);          // past the free entry
  for (int n = 1;; n++) {
    const size_t line = at + 1;
    if (pdf.compare(line, 7, "trailer") == 0)
      return true;
    const size_t off = std::strtoull(pdf.c_str() + line, nullptr, 10);
    const std::string head = std::to_string(n) + " 0 obj";
    if (pdf.compare(off, head.size(), head) != 0)
      return false;
    at = pdf.find('\n', line);
  }
}

cam::SetupSheet Sheet(size_t rows) {
  cam::SetupSheet s;
  s.title = "Base plate - Top";
  s.subtitle = "Setup sheet - test";
  s.facts = {{"Program", "base_plate - Top.nc"},
             {"Work zero", "X0 Y0 at the stock's centre (G54)"}};
  cam::SetupSheet::Table t;
  t.heading = "Operations";
  t.columns = {"Op", "Name", "Notes"};
  t.widths = {0.1, 0.4, 0.5};
  for (size_t i = 0; i < rows; i++)
    t.rows.push_back({std::to_string(i + 1), "Bearing seat (finish)",
                      "bore Ø47.000 K7 46.982-47.007, aimed at 46.9945"});
  s.tables.push_back(t);
  s.notes = {"Gauge it before the setup is broken."};
  s.footer = "base_plate - Top.nc";
  return s;
}

} // namespace

TEST_CASE("setup sheet: a PDF whose objects are where its xref says",
          "[cam][setup_sheet]") {
  cam::SetupSheet s = Sheet(4);
  // A 2 x 2 picture, red-green over blue-white.
  s.imageWidth = 2;
  s.imageHeight = 2;
  s.rgb = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
  const std::string pdf = cam::SetupSheetPdf(s);
  CHECK(pdf.rfind("%PDF-1.4\n", 0) == 0);
  CHECK(pdf.find("%%EOF") != std::string::npos);
  CHECK(XrefHolds(pdf));
  CHECK(pdf.find("/Count 1") != std::string::npos);
  CHECK(pdf.find("/Subtype /Image /Width 2 /Height 2") != std::string::npos);
  CHECK(pdf.find("/Im1 Do") != std::string::npos);
  // Text drawn in WinAnsi: the diameter sign is one byte, parentheses are
  // escaped, and the footer names the program.
  CHECK(pdf.find(std::string("bore ") + static_cast<char>(0xD8) + "47") !=
        std::string::npos);
  CHECK(pdf.find("Bearing seat \\(finish\\)") != std::string::npos);
  CHECK(pdf.find("(base_plate - Top.nc) Tj") != std::string::npos);
  CHECK(pdf.find("(page 1 of 1) Tj") != std::string::npos);
}

TEST_CASE("setup sheet: a long table runs onto more pages, its header again",
          "[cam][setup_sheet]") {
  const std::string pdf = cam::SetupSheetPdf(Sheet(120));
  CHECK(XrefHolds(pdf));
  CHECK(pdf.find("/Count 1") == std::string::npos);
  CHECK(pdf.find("(Operations \\(continued\\)) Tj") != std::string::npos);
  CHECK(pdf.find("(page 2 of ") != std::string::npos);
  // No picture: no image object, and none drawn.
  CHECK(pdf.find("/Subtype /Image") == std::string::npos);
  CHECK(pdf.find("/Im1 Do") == std::string::npos);
}
