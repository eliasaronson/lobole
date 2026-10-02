#include "core/InputHash.h"

#include "cad/CADKernel.h"
#include "cad/CADMaterials.h"
#include "core/Feature.h"

#include <BRepTools.hxx>
#include <BinTools.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_TShape.hxx>

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

// Every sub-shape's Modified, Checked and Free flags, which say what has
// been done to a shape and where it sits, not what it is.
void NeutralFlags(const TopoDS_Shape& shape) {
  TopTools_MapOfShape seen;
  std::vector<TopoDS_Shape> todo{shape};
  while (!todo.empty()) {
    const TopoDS_Shape s = todo.back();
    todo.pop_back();
    if (s.IsNull() || !seen.Add(s))
      continue;
    const Handle(TopoDS_TShape) & t = s.TShape();
    t->Modified(false);
    t->Checked(false);
    t->Free(false);
    for (TopoDS_Iterator it(s, false, false); it.More(); it.Next())
      todo.push_back(it.Value());
  }
}

// "-0" (a whole token) as "0": the sign of a zero is not geometry.
std::string FoldNegativeZeros(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  const auto sep = [](char c) { return c == ' ' || c == '\n' || c == '\t'; };
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '-' && i + 1 < in.size() && in[i + 1] == '0' &&
        (i == 0 || sep(in[i - 1])) && (i + 2 == in.size() || sep(in[i + 2])))
      continue;
    out += in[i];
  }
  return out;
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
  // The geometry, not the bookkeeping OCC keeps on it, and the same however
  // the shape came to be. Meshing a face (Netgen for a study, BRepMesh for
  // the viewport) clears its TShape's `Checked` flag and its cached UV
  // points, and BinTools wrote both: a topology run hashed its envelope,
  // meshed it in place, and the project saved the meshed shape, so the
  // reopened part hashed otherwise and its result read stale and ran again.
  // And a shape read back from the project's baked geometry carries a +0
  // where the regenerated one had a -0 (a circle's axis): a reopened part
  // with a bore hashed otherwise too. So: a copy (the copy carries the
  // flags over) with those flags set one way, written as text in format 1
  // (no UV points), every "-0" read as "0".
  //
  // And the copy is the shape AS THE PROJECT READS IT BACK: through BinTools,
  // the baked geometry's own format. Reading a direction normalises it
  // again, so a unit vector that came off a rotation returns a unit in the
  // last place away (a 6005's chamfer line, -0.70710678118654746 saved and
  // -0.70710678118654757 read), and the pan/tilt head's motion studies read
  // stale after every reopen. Hashed as read back, the regenerated shape and
  // the reopened one are one input.
  std::stringstream bin;
  BinTools::Write(shape.GetShape(), bin, /*theWithTriangles=*/false,
                  /*theWithNormals=*/false, BinTools_FormatVersion_CURRENT);
  TopoDS_Shape copy;
  BinTools::Read(copy, bin);
  NeutralFlags(copy);
  std::ostringstream text;
  BRepTools::Write(copy, text, /*theWithTriangles=*/false,
                   /*theWithNormals=*/false, TopTools_FormatVersion_VERSION_1);
  Hasher h;
  h.Str(FoldNegativeZeros(text.str()));
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
