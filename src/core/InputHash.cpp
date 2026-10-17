// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/InputHash.h"

#include "cad/CADKernel.h"
#include "cad/CADMaterials.h"
#include "core/Feature.h"

#include <BinTools.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_TShape.hxx>

#include <algorithm>
#include <bit>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

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
  // points: a topology run hashed its envelope, meshed it in place, and the
  // project saved the meshed shape, so the reopened part hashed otherwise
  // and its result read stale and ran again. So what is hashed is a copy
  // with those flags set one way, in BinTools' format 1, which writes no UV
  // points.
  //
  // And the copy is the shape AS THE PROJECT READS IT BACK: through BinTools,
  // the baked geometry's own format. Reading a direction normalises it
  // again, so a unit vector that came off a rotation returns a unit in the
  // last place away (a 6005's chamfer line, -0.70710678118654746 saved and
  // -0.70710678118654757 read; a circle's axis -0 regenerated and +0 read),
  // and the pan/tilt head's motion studies read stale after every reopen.
  //
  // Once is not enough: reading is not idempotent. A plane's frame is
  // rebuilt from its normal and X direction as it is read (gp_Ax3), and for
  // some frames the rebuilt X flips between two values a last place apart on
  // every read. The engine's cam pulley has one (a tooth's flank), so the
  // pulley read back from a project hashed otherwise than the one
  // regenerated, and every requirement over its motion study opened stale
  // and was measured again. So the shape is read back until a read repeats
  // one before it, and the smallest digest of that cycle (one digest when
  // the reads settle) stands for the shape: a reopened shape is one read
  // further along the same sequence and reaches the same cycle. Each read
  // is written for the next, and those bytes are what is hashed.
  //
  // Eight reads were not enough for every shape: the engine's one-piece
  // camshaft (eight lobes over tapered undersides) drifts a last place at a
  // time for longer before its reads repeat, so the regenerated camshaft
  // hashed one read short of its cycle and the one reopened from the file
  // hashed on it, and every requirement over the engine opened stale
  // (tests/test_printed_features.cpp, tests/fixtures/engine-camshaft.json).
  // Most shapes settle in two or three reads; the bound only costs the
  // ones that drift.
  constexpr int kMaxReads = 32;
  const auto write = [](const TopoDS_Shape& s) {
    std::stringstream bin;
    BinTools::Write(s, bin, /*theWithTriangles=*/false,
                    /*theWithNormals=*/false, BinTools_FormatVersion_VERSION_1);
    return bin;
  };
  std::vector<uint64_t> seen;
  std::stringstream bin = write(shape.GetShape());
  uint64_t digest = 0;
  for (int pass = 0; pass < kMaxReads; ++pass) {
    TopoDS_Shape copy;
    BinTools::Read(copy, bin);
    NeutralFlags(copy);
    bin = write(copy);
    Hasher h;
    h.Str(bin.view());
    const uint64_t d = h.Digest();
    const auto again = std::find(seen.begin(), seen.end(), d);
    if (again != seen.end()) {
      digest = *std::min_element(again, seen.end());
      break;
    }
    seen.push_back(d);
    digest = d; // past kMaxReads: the last read
  }
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
    U64(b.ringGrips.size());
    for (const RingGrip& g : b.ringGrips) {
      U64(g.face.featureId);
      Str(g.face.name);
      Str(g.standard);
      F64(g.size);
      U64(g.groove ? 1 : 0);
    }
    // A press fit's bands decide what a clash check relieves. Only when
    // there are any, so a body without keeps the hash it always had.
    if (!b.fitBands.empty()) {
      Str("fit_bands");
      U64(b.fitBands.size());
      for (const FitBand& f : b.fitBands) {
        U64(f.face.featureId);
        Str(f.face.name);
        F64(f.lower);
        F64(f.upper);
        F64(f.allowance);
        U64(static_cast<uint64_t>(f.side));
        Str(f.label);
        if (f.printedPress) // v179; only when set, so others keep theirs
          Str("printed_press");
      }
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
