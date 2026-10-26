// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cad/BoundedRead.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BinTools.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cstring>

namespace cad {

BoundedWordBuf::int_type BoundedWordBuf::underflow() {
  Arm();
  return m_at < m_end ? traits_type::to_int_type(*m_at) : traits_type::eof();
}

BoundedWordBuf::int_type BoundedWordBuf::uflow() {
  Arm();
  return m_at < m_end ? traits_type::to_int_type(*m_at++) : traits_type::eof();
}

BoundedWordBuf::int_type BoundedWordBuf::pbackfail(int_type c) {
  if (m_at == m_begin)
    return traits_type::eof();
  --m_at;
  return traits_type::eq_int_type(c, traits_type::eof())
             ? traits_type::not_eof(c)
             : c;
}

std::streamsize BoundedWordBuf::xsgetn(char* out, std::streamsize n) {
  const std::streamsize take = std::min<std::streamsize>(n, m_end - m_at);
  if (take > 0) {
    std::memcpy(out, m_at, static_cast<size_t>(take));
    m_at += take;
  }
  return take;
}

std::streamsize BoundedWordBuf::showmanyc() {
  return m_at < m_end ? m_end - m_at : -1;
}

BoundedWordBuf::pos_type
BoundedWordBuf::seekoff(off_type off, std::ios_base::seekdir dir,
                        std::ios_base::openmode which) {
  if (!(which & std::ios_base::in))
    return pos_type(off_type(-1));
  const off_type base = dir == std::ios_base::beg   ? 0
                        : dir == std::ios_base::cur ? m_at - m_begin
                                                    : m_end - m_begin;
  const off_type to = base + off;
  if (to < 0 || to > m_end - m_begin)
    return pos_type(off_type(-1));
  m_at = m_begin + to;
  return pos_type(to);
}

BoundedWordBuf::pos_type
BoundedWordBuf::seekpos(pos_type pos, std::ios_base::openmode which) {
  return seekoff(off_type(pos), std::ios_base::beg, which);
}

namespace {
template <typename ReadFn>
TopoDS_Shape ReadBounded(std::string_view bytes, std::string* error,
                         ReadFn&& read) {
  BoundedWordBuf buf(bytes);
  std::istream in(&buf);
  buf.Bind(&in);
  in.width(BoundedWordBuf::kWordLimit);
  TopoDS_Shape shape;
  try {
    read(shape, in);
  } catch (const Standard_Failure& e) {
    shape.Nullify();
    if (error != nullptr)
      *error = e.GetMessageString() ? e.GetMessageString() : "OCC error";
  }
  return shape;
}
} // namespace

TopoDS_Shape ReadBinaryBrep(std::string_view bytes, std::string* error) {
  return ReadBounded(bytes, error, [](TopoDS_Shape& s, std::istream& in) {
    BinTools::Read(s, in); // void: an unreadable stream leaves it null
  });
}

TopoDS_Shape ReadTextBrep(std::string_view bytes, std::string* error) {
  return ReadBounded(bytes, error, [](TopoDS_Shape& s, std::istream& in) {
    BRep_Builder builder;
    BRepTools::Read(s, in, builder);
  });
}

} // namespace cad
