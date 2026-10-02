#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// A setup sheet: the printed page that goes to the machine with a program
// (docs/cam/setup.md#the-setup-sheet). What it says is gathered where the job
// is (ui::SetupSheetOf); this is the page -- a minimal PDF in the drawing
// export's style (base-14 Helvetica, nothing embedded but the picture), A4
// portrait, as many pages as the tables take. GUI-free, so it is built and
// tested in every configuration.
namespace cam {

struct SetupSheet {
  std::string title;    // "Base plate - Top"
  std::string subtitle; // the project, the date
  // Label and value, beside the picture: the program, the stock, the zero.
  struct Fact {
    std::string label, value;
  };
  std::vector<Fact> facts;
  // A table across the page: its heading, columns (with their share of the
  // width) and rows of text, each cell wrapped to its column.
  struct Table {
    std::string heading;
    std::vector<std::string> columns;
    std::vector<double> widths; // fractions of the page's width, summing ~1
    std::vector<std::vector<std::string>> rows;
  };
  std::vector<Table> tables;
  // The notes, one paragraph each: the bands aimed at, the comp, what to
  // gauge before the setup is broken.
  std::vector<std::string> notes;
  // The picture of the setup, top right: 8-bit RGB, row 0 at the TOP. Empty
  // for none (a headless instance has no renderer); the caption says why.
  uint32_t imageWidth = 0, imageHeight = 0;
  std::vector<uint8_t> rgb;
  std::string imageCaption;
  // The plan, from above, in the PROGRAM's coordinates (mm, X0 Y0 the work
  // zero): the stock's outline, the part's, each fixture's footprint. Drawn
  // with the zero marked and the axes named, because the picture cannot
  // show where the zero is. Empty stock: no plan.
  using Polyline = std::vector<std::array<double, 2>>;
  struct Plan {
    Polyline stock;
    std::vector<Polyline> part;
    std::vector<Polyline> fixtures;
    std::string caption;
  };
  Plan plan;
  // The page's footer, beside its number: the program's file name.
  std::string footer;
};

// Writes `sheet` as a PDF at `path`. False with `error` when it cannot be
// written.
bool WriteSetupSheetPdf(const SetupSheet& sheet, const std::string& path,
                        std::string* error = nullptr);

// The same page as PDF bytes, for a test to read.
[[nodiscard]] std::string SetupSheetPdf(const SetupSheet& sheet);

} // namespace cam
