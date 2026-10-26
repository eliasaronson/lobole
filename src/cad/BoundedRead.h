// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <TopoDS_Shape.hxx>

#include <istream>
#include <streambuf>
#include <string>
#include <string_view>

// Reading a B-Rep from bytes that may be damaged (a file's asset, a cache
// on disk, a .brep the user opened) without letting OpenCASCADE overrun its
// own stack.
//
// OCC's readers take each section keyword with `char buffer[255]; IS >>
// buffer;` (BinTools_*Set::Read, BRepTools/TopTools sets). Compiled as
// C++17 or older -- Ubuntu's 7.6 and 7.9 are -- that extraction is
// unbounded, so a damaged stream whose bytes run on without a space where a
// keyword belongs writes past `buffer`: "stack smashing detected" in
// BinTools_ShapeSet::ReadGeometry on Ubuntu 24.04, from a reader-fuzz case
// that drops a run of a derived part's snapshot bytes. (Built as C++20 the
// operator takes the array's bound, which is why nix's OCC never showed it.)
//
// The stream here has no buffer of its own, so every character a formatted
// read takes passes through it, and each time it sets the istream's width
// to kWordLimit: the next word extracted stops there. Binary reads (read(),
// sgetn) are a copy and are not touched; numbers ignore width.
namespace cad {

class BoundedWordBuf : public std::streambuf {
public:
  // Longer than any keyword OCC writes ("PolygonOnTriangulations"), and
  // well inside the smallest buffer it reads one into.
  static constexpr std::streamsize kWordLimit = 64;

  explicit BoundedWordBuf(std::string_view bytes)
      : m_begin(bytes.data()), m_end(bytes.data() + bytes.size()),
        m_at(bytes.data()) {}
  void Bind(std::istream* stream) { m_stream = stream; }

protected:
  int_type underflow() override;
  int_type uflow() override;
  int_type pbackfail(int_type c) override;
  std::streamsize xsgetn(char* out, std::streamsize n) override;
  std::streamsize showmanyc() override;
  pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                   std::ios_base::openmode which) override;
  pos_type seekpos(pos_type pos, std::ios_base::openmode which) override;

private:
  void Arm() {
    if (m_stream != nullptr)
      m_stream->width(kWordLimit);
  }
  const char* m_begin;
  const char* m_end;
  const char* m_at;
  std::istream* m_stream = nullptr;
};

// BinTools::Read over `bytes`; a null shape when they do not read, with
// OCC's message in `error` when it threw.
TopoDS_Shape ReadBinaryBrep(std::string_view bytes,
                            std::string* error = nullptr);

// BRepTools::Read (the text .brep format) over `bytes`, the same way.
TopoDS_Shape ReadTextBrep(std::string_view bytes, std::string* error = nullptr);

} // namespace cad
