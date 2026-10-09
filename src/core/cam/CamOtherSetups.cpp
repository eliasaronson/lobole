// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/CamOtherSetups.h"

#include "core/Parallel.h"
#include "core/cam/CamGcode.h"
#include "core/cam/CamSurfaceStore.h" // BodiesContentKey
#include "core/cam/CamToolpath.h"
#include "core/io/Serialization.h" // CamSetupToJson: the memo's key

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <numbers>

namespace cam {

bool SetupStock::Removes(const glm::dvec3& model, double tol) const {
  return RemovesAt(glm::dvec3(modelToSetup * glm::dvec4(model, 1.0)), tol);
}

bool SetupStock::RemovesAt(const glm::dvec3& q, double tol) const {
  if (!rotary) {
    const HeightMap& h = height;
    const int i = static_cast<int>(std::floor((q.x - h.x0) / h.res));
    const int j = static_cast<int>(std::floor((q.y - h.y0) / h.res));
    if (i < 0 || j < 0 || i >= h.nx || j >= h.ny)
      return false;
    if (q.z < zLo - tol || q.z > zHi + tol)
      return false;
    return q.z > h.At(i, j) - tol;
  }
  const RadiusMap& m = radius;
  const int i = static_cast<int>(std::floor((q.x - m.x0) / m.res));
  if (i < 0 || i >= m.nx || m.nTheta <= 0)
    return false;
  const double r = std::hypot(q.y, q.z);
  if (r > m.rMax + tol)
    return false;
  double theta = std::atan2(q.z, q.y);
  if (theta < 0.0)
    theta += 2.0 * std::numbers::pi;
  const int j = std::clamp(
      static_cast<int>(std::floor(theta / (2.0 * std::numbers::pi) * m.nTheta)),
      0, m.nTheta - 1);
  return r > m.At(i, j) - tol;
}

uint64_t OtherSetups::Claims(const glm::dvec3& from, const glm::dvec3& to,
                             double tol) const {
  if (stocks.empty())
    return 0;
  const double len = glm::length(to - from);
  const int n =
      std::clamp(static_cast<int>(std::ceil(len / std::max(tol, 0.1))), 1, 400);
  // Asked for every leftover cell of a check, and on the chess board's face
  // after the epoxy that is half of its four million cells: the pockets the
  // top setup cut for the epoxy stand, in the face setup's model, as 3 mm of
  // excess each. Sampling each of those columns point by point -- every
  // point taken through two 4x4 maps into model space and out, and looked up
  // in the other setup's map -- was 330 ms of a check that costs 21 ms where
  // the other setup's cells are 1,761. So the column's two ends are mapped
  // into each setup's frame once (the map is affine: the samples between
  // are the same lerp there), and a column that stays in one cell of a
  // height map there -- a face-up and a flipped setup of one block, which is
  // the common case -- looks that cell up once and compares heights.
  struct Column {
    glm::dvec3 a{0.0}, d{0.0}; // the column in the setup's frame: a + d t
    bool oneCell = false;      // a height map's single cell, looked up
    double zMin = 0.0, zMax = 0.0, hCut = 0.0;
  };
  constexpr size_t kInline = 4; // a job of up to five setups allocates nothing
  std::array<Column, kInline> inlineCols;
  std::array<size_t, kInline> inlineCount{};
  std::vector<Column> heapCols;
  std::vector<size_t> heapCount;
  Column* col = inlineCols.data();
  size_t* count = inlineCount.data();
  if (stocks.size() > kInline) {
    heapCols.resize(stocks.size());
    heapCount.assign(stocks.size(), 0);
    col = heapCols.data();
    count = heapCount.data();
  }
  const glm::dvec4 fromModel = toModel * glm::dvec4(from, 1.0);
  const glm::dvec4 toModelPt = toModel * glm::dvec4(to, 1.0);
  for (size_t s = 0; s < stocks.size(); s++) {
    const SetupStock& st = stocks[s];
    Column& c = col[s];
    c.a = glm::dvec3(st.modelToSetup * fromModel);
    c.d = glm::dvec3(st.modelToSetup * toModelPt) - c.a;
    if (st.rotary)
      continue;
    // The samples' own arithmetic at both ends: x and y are monotone in t,
    // so a column whose ends land in one cell keeps every sample in it.
    const HeightMap& h = st.height;
    const glm::dvec3 end = c.a + c.d * 1.0;
    const auto cellOf = [&h](const glm::dvec3& q) {
      return glm::ivec2(static_cast<int>(std::floor((q.x - h.x0) / h.res)),
                        static_cast<int>(std::floor((q.y - h.y0) / h.res)));
    };
    const glm::ivec2 ij = cellOf(c.a);
    if (ij != cellOf(end))
      continue;
    c.oneCell = true;
    if (ij.x < 0 || ij.y < 0 || ij.x >= h.nx || ij.y >= h.ny) {
      c.zMin = 1.0; // outside its stock: removes nothing (RemovesAt)
      c.zMax = 0.0;
      continue;
    }
    c.zMin = st.zLo - tol;
    c.zMax = st.zHi + tol;
    c.hCut = h.At(ij.x, ij.y) - tol;
  }
  // In one cell, a sample is removed when its z lies in one interval, and z
  // is monotone along the column -- so a setup that removes both ends
  // removes every sample between, and is the answer: it has the most, and
  // the first of equals is the one the count below would name. With one
  // other setup that does not, nothing else can cover what it misses.
  const auto removesZ = [](const Column& c, double z) {
    return !(z < c.zMin || z > c.zMax) && z > c.hCut;
  };
  bool allOneCell = true;
  for (size_t s = 0; s < stocks.size(); s++) {
    const Column& c = col[s];
    allOneCell &= c.oneCell;
    if (c.oneCell && removesZ(c, c.a.z + c.d.z * 0.0) &&
        removesZ(c, c.a.z + c.d.z * 1.0)) {
      // An earlier setup that is not one cell may still count as many.
      bool earlierMayTie = false;
      for (size_t e = 0; e < s; e++)
        earlierMayTie |= !col[e].oneCell;
      if (!earlierMayTie)
        return stocks[s].id;
    }
  }
  if (allOneCell && stocks.size() == 1)
    return 0;
  for (int k = 0; k <= n; k++) {
    const double t = static_cast<double>(k) / n;
    bool any = false;
    for (size_t s = 0; s < stocks.size(); s++) {
      const Column& c = col[s];
      bool removes = false;
      if (c.oneCell) {
        // RemovesAt's own tests, on the same z.
        removes = removesZ(c, c.a.z + c.d.z * t);
      } else {
        removes = stocks[s].RemovesAt(c.a + c.d * t, tol);
      }
      if (removes) {
        count[s]++;
        any = true;
      }
    }
    if (!any)
      return 0;
  }
  const size_t best = static_cast<size_t>(
      std::max_element(count, count + stocks.size()) - count);
  return stocks[best].id;
}

std::string OtherSetups::NameOf(uint64_t id) const {
  for (const SetupStock& s : stocks)
    if (s.id == id)
      return s.name;
  return "";
}

bool ModelToSetup(const Setup& setup, const std::vector<Body>& rawBodies,
                  glm::dmat4& out, std::string* why) {
  bool have = false;
  for (const Body& b : rawBodies) {
    const glm::dmat4 m = BodyToSetup(setup, b.jobPartId);
    if (!have) {
      out = m;
      have = true;
      continue;
    }
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++)
        if (std::abs(m[c][r] - out[c][r]) > 1e-9) {
          if (why != nullptr)
            *why = "its parts are clamped or placed differently from one "
                   "another, so a point of leftover has no one place on the "
                   "part";
          return false;
        }
  }
  if (!have) {
    if (why != nullptr)
      *why = "it has no part";
    return false;
  }
  return true;
}

namespace {

// What one other setup leaves, as `OtherSetupsOf` reads it from its posted
// file: its machined map, or why it could not be read. Everything but where
// it stands in the job's order, which is the asking setup's business.
struct Machined {
  bool usable = false;
  SetupStock stock; // `earlier` unset
  std::string why;  // when not usable: "... because in it <why>"
};

Machined InspectOther(const SetupProgram& p, const glm::dmat4& modelToSetup,
                      const std::vector<Body>& rawBodies, double res) {
  const Setup& s = *p.setup;
  Machined out;
  out.stock.id = s.id;
  out.stock.name = s.name;
  out.stock.modelToSetup = modelToSetup;
  if (RotatesWhileCutting(ProgramInSetupSpace(p.gcode, s))) {
    RotaryInspection ri =
        InspectRotary(p.gcode, s, rawBodies, std::max(res, 0.5));
    if (ri.machined.r.empty()) {
      out.why = ri.why;
      return out;
    }
    out.stock.rotary = true;
    out.stock.radius = std::move(ri.machined);
  } else {
    glm::dvec3 lo(0.0), hi(0.0);
    Inspection in = Inspect(p.gcode, s, rawBodies, res);
    if (!in.ok || !StockBounds(s, rawBodies, lo, hi)) {
      out.why = "there is no stock to cut";
      return out;
    }
    out.stock.height = std::move(in.machined);
    out.stock.zLo = lo.z;
    out.stock.zHi = hi.z;
  }
  out.usable = true;
  return out;
}

// The key is exactly what `InspectOther` reads: the posted file, the setup
// whole as it is saved (its framing, stock, tools, machine, placements and
// name -- the operations too, which the file already follows), the part's
// geometry (`BodiesContentKey`: the exact B-rep and the ids a body is placed
// by) and the resolution. Compared whole, not by hash alone.
struct MemoKey {
  std::string gcode;
  std::string setup;
  uint64_t bodies = 0;
  double res = 0.0;
  bool operator==(const MemoKey&) const = default;
};

uint64_t HashOf(const MemoKey& k) {
  uint64_t h = 1469598103934665603ULL;
  const auto bytes = [&h](const void* p, size_t n) {
    const unsigned char* c = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= c[i];
      h *= 1099511628211ULL;
    }
  };
  bytes(k.gcode.data(), k.gcode.size());
  bytes(k.setup.data(), k.setup.size());
  bytes(&k.bodies, sizeof k.bodies);
  bytes(&k.res, sizeof k.res);
  return h;
}

size_t BytesOf(const MemoKey& k, const Machined& m) {
  return k.gcode.size() + k.setup.size() +
         m.stock.height.z.size() * sizeof(float) +
         m.stock.radius.r.size() * sizeof(float) + sizeof(Machined);
}

// Process-wide, least recently used out first. Bounded by bytes: a board's
// machined map at 0.25 mm is 16 MB.
class Memo {
public:
  static constexpr size_t kMaxBytes = size_t{256} << 20;

  std::shared_ptr<const Machined> Find(const MemoKey& key, uint64_t hash) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Entry& e : m_entries)
      if (e.hash == hash && e.key == key) {
        e.used = ++m_clock;
        m_hits++;
        return e.value;
      }
    m_misses++;
    return nullptr;
  }

  void Put(MemoKey key, uint64_t hash, std::shared_ptr<const Machined> value) {
    const size_t bytes = BytesOf(key, *value);
    if (bytes > kMaxBytes)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const Entry& e : m_entries)
      if (e.hash == hash && e.key == key)
        return; // another check landed it first
    m_entries.push_back(
        {std::move(key), hash, std::move(value), bytes, ++m_clock});
    m_bytes += bytes;
    while (m_bytes > kMaxBytes) {
      auto oldest = std::min_element(
          m_entries.begin(), m_entries.end(),
          [](const Entry& x, const Entry& y) { return x.used < y.used; });
      m_bytes -= oldest->bytes;
      m_entries.erase(oldest);
    }
  }

  OtherSetupsMemoStats Stats() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_hits, m_misses, m_entries.size(), m_bytes};
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
    m_bytes = 0;
    m_hits = m_misses = 0;
  }

private:
  struct Entry {
    MemoKey key;
    uint64_t hash = 0;
    std::shared_ptr<const Machined> value;
    size_t bytes = 0;
    uint64_t used = 0;
  };
  std::mutex m_mutex;
  std::vector<Entry> m_entries;
  size_t m_bytes = 0;
  uint64_t m_clock = 0;
  size_t m_hits = 0, m_misses = 0;
};

Memo& TheMemo() {
  static Memo memo;
  return memo;
}

} // namespace

OtherSetupsMemoStats OtherSetupsMemo() { return TheMemo().Stats(); }

void ForgetOtherSetups() { TheMemo().Clear(); }

OtherSetups OtherSetupsOf(const Setup& self,
                          const std::vector<SetupProgram>& others,
                          const std::vector<Body>& rawBodies, double res) {
  OtherSetups out;
  if (others.empty())
    return out;
  glm::dmat4 mine(1.0);
  std::string why;
  if (!ModelToSetup(self, rawBodies, mine, &why)) {
    out.notChecked.push_back("the job's other setups, because in this one " +
                             why);
    return out;
  }
  out.toModel = glm::inverse(mine);

  // Each other setup's answer, from the memo or inspected afresh
  // (docs/cam/simulation.md#each-other-setup-inspected-once).
  struct Slot {
    const SetupProgram* program = nullptr;
    glm::dmat4 modelToSetup{1.0};
    std::string notMapped; // ModelToSetup's refusal
    MemoKey key;
    uint64_t hash = 0;
    std::shared_ptr<const Machined> machined;
  };
  std::vector<Slot> slots;
  const uint64_t bodiesKey = BodiesContentKey(rawBodies);
  for (const SetupProgram& p : others) {
    if (p.setup == nullptr || p.gcode.empty())
      continue; // cuts nothing: its stock is whole, and claims nothing
    Slot slot;
    slot.program = &p;
    if (!ModelToSetup(*p.setup, rawBodies, slot.modelToSetup, &why)) {
      slot.notMapped = why;
      slots.push_back(std::move(slot));
      continue;
    }
    slot.key = {p.gcode, DocumentIO::CamSetupToJson(*p.setup).dump(), bodiesKey,
                res};
    slot.hash = HashOf(slot.key);
    slot.machined = TheMemo().Find(slot.key, slot.hash);
    slots.push_back(std::move(slot));
  }
  // The ones the memo did not have, at once: they are independent. Each
  // inspection is itself split into bands, and a loop nested in another's
  // shares its worker's part of the machine (core/Parallel.h). Each works
  // on its own copy of the bodies, whose bounding boxes are cached on the
  // object: shared, two threads would fill the same cache.
  std::vector<Slot*> missing;
  for (Slot& slot : slots)
    if (slot.notMapped.empty() && slot.machined == nullptr)
      missing.push_back(&slot);
  parallel::For(missing.size(), [&](size_t k) {
    Slot& slot = *missing[k];
    const std::vector<Body> bodies = rawBodies;
    auto machined = std::make_shared<const Machined>(
        InspectOther(*slot.program, slot.modelToSetup, bodies, res));
    TheMemo().Put(slot.key, slot.hash, machined);
    slot.machined = std::move(machined);
  });

  for (const Slot& slot : slots) {
    const std::string label =
        "setup '" + slot.program->setup->name + "', because in it";
    if (!slot.notMapped.empty()) {
      out.notChecked.push_back(label + " " + slot.notMapped);
      continue;
    }
    if (!slot.machined->usable) {
      out.notChecked.push_back(label + " " + slot.machined->why);
      continue;
    }
    SetupStock stock = slot.machined->stock;
    stock.earlier = slot.program->earlier;
    out.stocks.push_back(std::move(stock));
  }
  return out;
}

namespace {

// Whether an earlier setup left air at `model`, `margin` clear of what it
// left, so a seed errs toward material -- toward a finding.
bool AirAfter(const SetupStock& s, const glm::dvec3& model, double margin) {
  // Beyond a rotary setup's blank there was never any bar: a later setup's
  // box is the bounding box of that round (or square) bar, and its corners
  // past the bar's radius are air. Removes does not claim outside a stock,
  // rightly for a claim, and left the whole seed at the box's top.
  if (s.rotary) {
    const RadiusMap& m = s.radius;
    const glm::dvec3 q = glm::dvec3(s.modelToSetup * glm::dvec4(model, 1.0));
    const int k = static_cast<int>(std::floor((q.x - m.x0) / m.res));
    if (k < 0 || k >= m.nx || m.nTheta <= 0)
      return false; // off its length: not this setup's to say
    double th = std::atan2(q.z, q.y);
    if (th < 0.0)
      th += 2.0 * std::numbers::pi;
    const int t = std::clamp(
        static_cast<int>(std::floor(th / (2.0 * std::numbers::pi) * m.nTheta)),
        0, m.nTheta - 1);
    return std::hypot(q.y, q.z) >
           std::min(static_cast<double>(m.At(k, t)), m.rMax) + margin;
  }
  // Removes() calls a point air when it is `tol` INSIDE what a setup left;
  // a negative tol asks for `margin` of clearance instead.
  return s.Removes(model, -std::abs(margin));
}

std::vector<const SetupStock*> EarlierOf(const OtherSetups& others) {
  std::vector<const SetupStock*> earlier;
  for (const SetupStock& s : others.stocks)
    if (s.earlier)
      earlier.push_back(&s);
  return earlier;
}

bool AirAfterAny(const std::vector<const SetupStock*>& earlier,
                 const glm::dvec3& model, double margin) {
  return std::any_of(earlier.begin(), earlier.end(), [&](const SetupStock* s) {
    return AirAfter(*s, model, margin);
  });
}

} // namespace

void SeedFromEarlier(HeightMap& stock, const OtherSetups& others, double zLo,
                     double margin) {
  const std::vector<const SetupStock*> earlier = EarlierOf(others);
  if (earlier.empty() || stock.res <= 0.0)
    return;
  // Row by row on every core: each column is walked on its own, so the
  // order cannot show.
  parallel::For(static_cast<size_t>(stock.ny), [&](size_t row) {
    const int j = static_cast<int>(row);
    for (int i = 0; i < stock.nx; i++) {
      const double x = stock.CellX(i), y = stock.CellY(j);
      double z = stock.At(i, j);
      while (z > zLo &&
             AirAfterAny(earlier,
                         glm::dvec3(others.toModel * glm::dvec4(x, y, z, 1.0)),
                         margin))
        z -= stock.res;
      stock.Set(i, j, static_cast<float>(std::max(z, zLo)));
    }
  });
}

void SeedFromEarlier(RadiusMap& stock, const OtherSetups& others,
                     double margin) {
  const std::vector<const SetupStock*> earlier = EarlierOf(others);
  if (earlier.empty() || stock.res <= 0.0 || stock.nTheta <= 0)
    return;
  // The height map's walk, inward along each ray instead of down each
  // column: a turning setup after an indexed one, or after another turning,
  // starts from what those left.
  parallel::For(static_cast<size_t>(stock.nTheta), [&](size_t ray) {
    const int j = static_cast<int>(ray);
    const double th = stock.CellTheta(j);
    const double c = std::cos(th), s = std::sin(th);
    for (int i = 0; i < stock.nx; i++) {
      const double x = stock.CellX(i);
      double r = stock.At(i, j);
      while (r > 0.0 &&
             AirAfterAny(
                 earlier,
                 glm::dvec3(others.toModel * glm::dvec4(x, r * c, r * s, 1.0)),
                 margin))
        r -= stock.res;
      stock.Set(i, j, static_cast<float>(std::max(r, 0.0)));
    }
  });
}

double SeedMargin(double res) { return std::max(0.3, res); }

bool StartingStock(const Setup& setup, const std::vector<Body>& rawBodies,
                   double res, const OtherSetups* others, HeightMap& out) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, rawBodies, lo, hi))
    return false;
  out = MakeStock(lo, hi, res);
  if (others != nullptr && others->HasEarlier())
    SeedFromEarlier(out, *others, lo.z, SeedMargin(res));
  return true;
}

RadiusMap StartingRotaryStock(const Setup& setup,
                              const std::vector<Body>& rawBodies, double res,
                              const OtherSetups* others, std::string* why) {
  RadiusMap out =
      MakeRotaryBlank(setup, BodiesInSetupFrame(setup, rawBodies), res, why);
  if (!out.r.empty() && others != nullptr && others->HasEarlier())
    SeedFromEarlier(out, *others, SeedMargin(res));
  return out;
}

} // namespace cam
