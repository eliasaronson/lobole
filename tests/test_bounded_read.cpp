// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// cad/BoundedRead.h: OCC's B-Rep readers take each keyword with an
// unbounded `IS >> buffer` into char[255] when OCC is built as C++17, so a
// damaged stream overran its stack (Ubuntu 24.04's 7.6: a reader-fuzz case
// of a derived part's snapshot, "stack smashing detected" in
// BinTools_ShapeSet::ReadGeometry). The bounded stream caps every word.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "cad/BoundedRead.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BinTools.hxx>
#include <GProp_GProps.hxx>

#include <cstring>
#include <sstream>
#include <string>

namespace {
double Volume(const TopoDS_Shape& s) {
  GProp_GProps p;
  BRepGProp::VolumeProperties(s, p);
  return p.Mass();
}
} // namespace

TEST_CASE("a bounded stream caps every word a formatted read takes",
          "[kernel][io]") {
  const std::string text =
      std::string(1000, 'x') + " short " + std::string(500, 'y') + "\n";
  cad::BoundedWordBuf buf(text);
  std::istream in(&buf);
  buf.Bind(&in);
  // An array far longer than the limit, so only the stream's width bounds
  // the read: what OCC's char* extraction has when it was built as C++17.
  static char word[2048];
  in >> word;
  CHECK(std::strlen(word) < size_t(cad::BoundedWordBuf::kWordLimit));
  // The rest of the long run is the next word, cut the same way, and the
  // stream goes on: a damaged file fails to read, it does not overrun.
  in >> word;
  CHECK(std::strlen(word) < size_t(cad::BoundedWordBuf::kWordLimit));
  // Binary reads are untouched, and the stream seeks.
  in.seekg(0);
  char raw[200];
  in.read(raw, sizeof raw);
  CHECK(in.gcount() == 200);
  CHECK(std::string(raw, 200) == std::string(200, 'x'));
  CHECK(in.tellg() == std::streampos(200));
}

TEST_CASE("a B-Rep reads through the bounded stream as it was written",
          "[kernel][io]") {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
  std::ostringstream bin(std::ios::binary);
  BinTools::Write(box, bin);
  const TopoDS_Shape fromBin = cad::ReadBinaryBrep(bin.str());
  REQUIRE_FALSE(fromBin.IsNull());
  CHECK(Volume(fromBin) == Catch::Approx(6000.0));

  std::ostringstream txt;
  BRepTools::Write(box, txt);
  const TopoDS_Shape fromText = cad::ReadTextBrep(txt.str());
  REQUIRE_FALSE(fromText.IsNull());
  CHECK(Volume(fromText) == Catch::Approx(6000.0));
}

TEST_CASE("a damaged B-Rep is unreadable, not an overrun", "[kernel][io]") {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 20.0, 30.0).Shape();
  std::ostringstream bin(std::ios::binary);
  BinTools::Write(box, bin);
  std::string bytes = bin.str();
  // The header line, then a run with no space where the first section's
  // keyword belongs: what overran char[255] in the reader.
  const size_t header = bytes.find('\n') + 1;
  bytes =
      bytes.substr(0, header) + std::string(4000, 'L') + bytes.substr(header);
  CHECK(cad::ReadBinaryBrep(bytes).IsNull());
  std::ostringstream txt;
  BRepTools::Write(box, txt);
  std::string text = txt.str();
  const size_t at = text.find("Locations");
  REQUIRE(at != std::string::npos);
  text.replace(at, 9, std::string(4000, 'L'));
  CHECK(cad::ReadTextBrep(text).IsNull());
}
