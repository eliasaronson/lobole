// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/CamSurfaceStore.h"

#include <BinTools.hxx>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace cam {

namespace {

// FNV-1a, the hash every other CAM key here uses.
struct Fnv {
  uint64_t h = 1469598103934665603ULL;
  void Bytes(const void* p, size_t n) {
    const unsigned char* c = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= c[i];
      h *= 1099511628211ULL;
    }
  }
  void U(uint64_t v) { Bytes(&v, sizeof v); }
  void S(const std::string& s) {
    U(s.size());
    Bytes(s.data(), s.size());
  }
};

// The file: a magic word, the format, the salt and key it was written for,
// the two strings, then the moves packed field by field (no struct padding
// on disk, so the layout is the format and not the compiler's).
constexpr char kMagic[8] = {'D', 'S', 'G', 'N', 'S', 'U', 'R', 'F'};
constexpr uint32_t kFormat = 1;
constexpr size_t kMoveBytes = 2 + 5 * sizeof(double);

template <class T> void Put(std::string& out, const T& v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof v);
}

template <class T> bool Take(const std::string& in, size_t& at, T& v) {
  if (in.size() - at < sizeof v)
    return false;
  std::memcpy(&v, in.data() + at, sizeof v);
  at += sizeof v;
  return true;
}

bool TakeString(const std::string& in, size_t& at, std::string& s) {
  uint64_t n = 0;
  if (!Take(in, at, n) || in.size() - at < n)
    return false;
  s.assign(in.data() + at, n);
  at += n;
  return true;
}

} // namespace

uint64_t BodiesContentKey(const std::vector<Body>& bodies) {
  Fnv hash;
  hash.U(bodies.size());
  for (const Body& b : bodies) {
    hash.U(b.jobPartId);
    hash.U(b.createdBy);
    // A picked face resolves through these, so the same shape with its
    // names moved is a different input.
    hash.U(b.elementMap.names.size());
    for (const auto& [index, name] : b.elementMap.names) {
      hash.U(static_cast<uint64_t>(index));
      hash.S(name);
    }
    hash.U(b.elementMap.edgeNames.size());
    for (const auto& [index, name] : b.elementMap.edgeNames) {
      hash.U(static_cast<uint64_t>(index));
      hash.S(name);
    }
    if (!b.shape.IsValid()) {
      hash.U(0);
      continue;
    }
    // The exact B-rep, WITHOUT triangulation: a mesh the frame hung on the
    // shape is not geometry, and reading it would race the frame writing it
    // (docs/ui/long-operations.md#cam-toolpath-passes).
    std::ostringstream brep(std::ios::binary);
    BinTools::Write(b.shape.GetShape(), brep, /*theWithTriangles=*/false,
                    /*theWithNormals=*/false, BinTools_FormatVersion_CURRENT);
    hash.S(brep.str());
  }
  return hash.h;
}

SurfaceStore::SurfaceStore(std::filesystem::path dir, uint64_t maxBytes,
                           std::function<uint64_t()> salt)
    : m_dir(std::move(dir)), m_maxBytes(maxBytes), m_saltFn(std::move(salt)) {}

uint64_t SurfaceStore::Salt() const {
  std::call_once(m_saltOnce, [this] { m_salt = m_saltFn ? m_saltFn() : 0; });
  return m_salt;
}

std::filesystem::path SurfaceStore::PathOf(uint64_t key) const {
  // The salt is in the NAME as well as the header, so two builds never
  // overwrite each other's files: the older one's age out.
  Fnv name;
  name.U(key);
  name.U(Salt());
  char buf[32];
  std::snprintf(buf, sizeof buf, "%016llx.surf",
                static_cast<unsigned long long>(name.h));
  return m_dir / buf;
}

bool SurfaceStore::Load(uint64_t key, SurfaceResult& out) const {
  const std::filesystem::path path = PathOf(key);
  std::ifstream f(path, std::ios::binary);
  if (!f)
    return false;
  std::string in((std::istreambuf_iterator<char>(f)),
                 std::istreambuf_iterator<char>());
  f.close();
  size_t at = 0;
  const auto bad = [&] {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return false;
  };
  char magic[8];
  uint32_t format = 0;
  uint64_t salt = 0, stored = 0, count = 0;
  if (!Take(in, at, magic) || std::memcmp(magic, kMagic, sizeof magic) != 0 ||
      !Take(in, at, format) || format != kFormat || !Take(in, at, salt) ||
      salt != Salt() || !Take(in, at, stored) || stored != key)
    return bad();
  SurfaceResult r;
  if (!TakeString(in, at, r.error) || !TakeString(in, at, r.strategy) ||
      !Take(in, at, count) || (in.size() - at) != count * kMoveBytes)
    return bad();
  r.moves.resize(count);
  for (Move& m : r.moves) {
    uint8_t kind = 0, hasA = 0;
    (void)Take(in, at, kind);
    (void)Take(in, at, hasA);
    (void)Take(in, at, m.to.x);
    (void)Take(in, at, m.to.y);
    (void)Take(in, at, m.to.z);
    (void)Take(in, at, m.a);
    (void)Take(in, at, m.feed);
    if (kind > static_cast<uint8_t>(Move::Kind::Plunge) || hasA > 1)
      return bad();
    m.kind = static_cast<Move::Kind>(kind);
    m.hasA = hasA != 0;
  }
  out = std::move(r);
  // Used: the last to go.
  std::error_code ec;
  std::filesystem::last_write_time(
      path, std::filesystem::file_time_type::clock::now(), ec);
  return true;
}

void SurfaceStore::Save(uint64_t key, const SurfaceResult& result) const {
  std::string out;
  out.reserve(64 + result.error.size() + result.strategy.size() +
              result.moves.size() * kMoveBytes);
  out.append(kMagic, sizeof kMagic);
  Put(out, kFormat);
  Put(out, Salt());
  Put(out, key);
  Put(out, static_cast<uint64_t>(result.error.size()));
  out += result.error;
  Put(out, static_cast<uint64_t>(result.strategy.size()));
  out += result.strategy;
  Put(out, static_cast<uint64_t>(result.moves.size()));
  for (const Move& m : result.moves) {
    Put(out, static_cast<uint8_t>(m.kind));
    Put(out, static_cast<uint8_t>(m.hasA ? 1 : 0));
    Put(out, m.to.x);
    Put(out, m.to.y);
    Put(out, m.to.z);
    Put(out, m.a);
    Put(out, m.feed);
  }
  if (out.size() > m_maxBytes)
    return; // it would evict everything and still not fit
  std::error_code ec;
  std::filesystem::create_directories(m_dir, ec);
  const std::filesystem::path path = PathOf(key);
  // Unique per writer, so two threads saving the same key never share one.
  std::ostringstream tmpName;
  tmpName << path.filename().string() << ".tmp."
          << std::hash<std::thread::id>{}(std::this_thread::get_id());
  const std::filesystem::path tmp = m_dir / tmpName.str();
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f)
      return;
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!f) {
      f.close();
      std::filesystem::remove(tmp, ec);
      return;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return;
  }
  Trim();
}

void SurfaceStore::Trim() const {
  std::lock_guard<std::mutex> lock(m_trim);
  struct Entry {
    std::filesystem::path path;
    std::filesystem::file_time_type when;
    uint64_t bytes = 0;
  };
  std::vector<Entry> entries;
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(m_dir, ec)) {
    if (e.path().extension() != ".surf")
      continue;
    std::error_code fe;
    const uint64_t bytes = e.file_size(fe);
    if (fe)
      continue;
    const auto when = e.last_write_time(fe);
    if (fe)
      continue;
    entries.push_back({e.path(), when, bytes});
    total += bytes;
  }
  if (total <= m_maxBytes)
    return;
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.when < b.when; });
  for (const Entry& e : entries) {
    if (total <= m_maxBytes)
      break;
    std::error_code re;
    if (std::filesystem::remove(e.path, re))
      total -= e.bytes;
  }
}

} // namespace cam
