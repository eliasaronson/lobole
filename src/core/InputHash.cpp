#include "core/InputHash.h"

#include "cad/CADKernel.h"
#include "cad/CADMaterials.h"
#include "core/Feature.h"

#include <BinTools.hxx>

#include <bit>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <utility>

namespace inputhash {

namespace {

// BinTools bytes cost a few milliseconds a part, and an assembly hash visits
// every placement of every part: memoized by the shape's identity. Each entry
// holds its CADShape, so the TShape behind a GeometryId stays alive and the
// address cannot be recycled into a false hit (the ExactContactCache rule).
struct ShapeMemo {
  struct Entry {
    CADShape shape;
    uint64_t digest = 0;
  };
  std::mutex mutex;
  std::map<std::pair<const void*, uint64_t>, Entry> entries;
  std::deque<std::pair<const void*, uint64_t>> order;
  static constexpr size_t kCapacity = 4096;
};

ShapeMemo& Memo() {
  static ShapeMemo memo;
  return memo;
}

uint64_t ShapeDigest(const CADShape& shape) {
  if (!shape.IsValid())
    return 0;
  const std::pair<const void*, uint64_t> key{shape.GeometryId(),
                                             shape.PlacementId()};
  ShapeMemo& memo = Memo();
  {
    std::lock_guard lock(memo.mutex);
    const auto it = memo.entries.find(key);
    if (it != memo.entries.end() && it->second.shape.SameGeometryAs(shape))
      return it->second.digest;
  }
  std::ostringstream brep(std::ios::binary);
  BinTools::Write(shape.GetShape(), brep, /*theWithTriangles=*/false,
                  /*theWithNormals=*/false, BinTools_FormatVersion_CURRENT);
  Hasher h;
  h.Str(brep.str());
  const uint64_t digest = h.Digest();
  std::lock_guard lock(memo.mutex);
  if (!memo.entries.contains(key)) {
    while (memo.entries.size() >= ShapeMemo::kCapacity) {
      memo.entries.erase(memo.order.front());
      memo.order.pop_front();
    }
    memo.order.push_back(key);
  }
  memo.entries[key] = {shape, digest};
  return digest;
}

} // namespace

void Hasher::Bytes(const void* p, size_t n) {
  const unsigned char* c = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; i++) {
    h_ ^= c[i];
    h_ *= 1099511628211ULL;
  }
}

void Hasher::F64(double v) {
  if (v == 0.0)
    v = 0.0; // -0 and +0 are one input
  U64(std::bit_cast<uint64_t>(v));
}

void Hasher::Str(std::string_view s) {
  U64(s.size());
  Bytes(s.data(), s.size());
}

void Hasher::Json(const nlohmann::json& j) { Str(j.dump()); }

void Hasher::Shape(const CADShape& shape) { U64(ShapeDigest(shape)); }

void Hasher::Bodies(const std::vector<Body>& bodies) {
  U64(bodies.size());
  for (const Body& b : bodies) {
    Str(b.name);
    Str(b.material);
    // The numbers a solve reads, not only the slug: a library edit is an
    // input change even where the name is not.
    const CADMaterials::MaterialProperties& m =
        CADMaterials::MaterialLibrary::GetMaterial(
            CADMaterials::MaterialLibrary::MaterialTypeFromString(b.material));
    F64(m.density);
    F64(m.youngGPa);
    F64(m.poisson);
    F64(m.yieldMPa);
    U64(b.reference ? 1 : 0);
    U64(b.threadRegions.size());
    for (const ThreadRegion& t : b.threadRegions) {
      U64(t.face.featureId);
      Str(t.face.name);
      Str(t.designation);
      F64(t.length);
      F64(t.minimumEngagement);
      U64(t.external ? 1 : 0);
    }
    // A load or a refinement resolves its face through these.
    U64(b.createdBy);
    U64(b.elementMap.names.size());
    for (const auto& [index, name] : b.elementMap.names) {
      U64(static_cast<uint64_t>(index));
      Str(name);
    }
    Shape(b.shape);
  }
}

std::string Hex(uint64_t hash) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "0x%016llx",
                static_cast<unsigned long long>(hash));
  return buf;
}

bool FromHex(const std::string& text, uint64_t& out) {
  if (text.size() != 18 || text[0] != '0' || text[1] != 'x')
    return false;
  uint64_t v = 0;
  for (size_t i = 2; i < text.size(); i++) {
    const char c = text[i];
    int d = -1;
    if (c >= '0' && c <= '9')
      d = c - '0';
    else if (c >= 'a' && c <= 'f')
      d = c - 'a' + 10;
    if (d < 0)
      return false;
    v = (v << 4) | static_cast<uint64_t>(d);
  }
  out = v;
  return true;
}

} // namespace inputhash
