// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/TemplateFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/features/WormFeature.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// The regeneration cache remembers more than one history
// (docs/architecture.md#the-document-model-and-regeneration): a hit may come
// from an evaluation made before an undo or a value set back, and restores
// what Execute left on the feature from the clone the entry keeps. The risk is
// a hit that restores STALE state — a tool shape, a datum frame, a sketch's
// resolved plane, an error — so the comparison here is against a document
// read back from its own saved file, every object fresh, rebuilt with no cache
// at all: everything a from-scratch rebuild produces, not just volumes.
namespace {

void Vec(std::ostringstream& out, const glm::dvec3& v) {
  out << "(" << v.x << "," << v.y << "," << v.z << ")";
}

std::string ShapeText(const CADShape& s) {
  std::ostringstream out;
  out.precision(12);
  if (!s.IsValid())
    return "none";
  glm::dvec3 lo(0.0), hi(0.0);
  s.GetBoundingBox(lo, hi);
  out << "vol=" << s.Volume() << " solids=" << s.SolidCount()
      << " faces=" << s.FaceCount() << " bbox=";
  // To a micrometre: a box is read off the triangulation when the shape has
  // one, and whether it does is not part of the evaluation.
  out << std::fixed << std::setprecision(4);
  Vec(out, lo);
  Vec(out, hi);
  return out.str();
}

std::string MapText(const ElementMap& m) {
  std::string out = "{";
  for (const auto& [index, name] : m.names)
    out += std::to_string(index) + ":" + name + ",";
  return out + "}";
}

std::string BodyText(const Body& b) {
  std::ostringstream out;
  out << b.name << " createdBy=" << b.createdBy << " " << ShapeText(b.shape)
      << " elements=" << MapText(b.elementMap) << " threads=";
  for (const ThreadMark& t : b.threads)
    out << t.face.name << "/" << t.featureId << "/" << t.designation << ";";
  out << " holes=";
  for (const HoleMark& h : b.holes)
    out << h.bore.name << "/" << h.featureId << "/" << h.diameter << ";";
  out << " bends=" << b.bends.size() << " regions=" << b.threadRegions.size()
      << " grips=" << b.ringGrips.size() << " reference=" << b.reference;
  return out.str();
}

// What a feature's evaluation left on it, beyond the bodies: the readable
// half of every member CopyDerivedState restores.
std::string FeatureText(const Feature& f) {
  std::ostringstream out;
  out.precision(12);
  nlohmann::json j = DocumentIO::FeatureToJson(f);
  out << j.dump() << " error=" << f.GetError() << " warning=" << f.GetWarning()
      << " after=" << f.ErrorAfterEarlierFailure()
      << " tool=" << (f.ToolShape() ? ShapeText(*f.ToolShape()) : "-")
      << " toolmap=" << (f.ToolMap() ? MapText(*f.ToolMap()) : "-");
  if (const auto* p = dynamic_cast<const DatumPlaneFeature*>(&f)) {
    out << " plane=";
    Vec(out, p->OutputPlane().GetOrigin());
    Vec(out, p->OutputPlane().GetNormal());
  }
  if (const auto* a = dynamic_cast<const DatumAxisFeature*>(&f)) {
    out << " axis=";
    Vec(out, a->Origin());
    Vec(out, a->Direction());
  }
  if (const auto* fr = dynamic_cast<const FrameFeature*>(&f)) {
    out << " frame=";
    for (int c = 0; c < 4; c++)
      Vec(out, glm::dvec3(fr->Matrix()[c]));
  }
  if (const auto* c = dynamic_cast<const CurveFeature*>(&f)) {
    out << " curve=" << c->Polyline().size() << " " << ShapeText(c->EdgeShape())
        << " bends=" << c->Bends().size();
    for (const glm::dvec3& p : c->Polyline())
      Vec(out, p);
  }
  if (const auto* t = dynamic_cast<const ThreadFeature*>(&f)) {
    const auto& o = t->overlay;
    out << " overlay=" << o.valid << "," << o.radius << "," << o.faceRadius
        << "," << o.v0 << "," << o.v1 << "," << o.external << "," << o.cut;
  }
  if (const auto* w = dynamic_cast<const WormFeature*>(&f))
    out << " worm=" << w->ReportJson().dump();
  if (const auto* w = dynamic_cast<const WormWheelFeature*>(&f)) {
    // `reused` says whether THIS regeneration found the wheel in the
    // process-wide memo, which a from-scratch rebuild in the same process
    // always does: not part of the evaluation.
    nlohmann::json r = w->ReportJson();
    r.erase("reused");
    out << " wheel=" << r.dump();
  }
  if (const auto* t = dynamic_cast<const TemplateFeature*>(&f)) {
    out << " expanded=[";
    for (const auto& inner : t->Expanded())
      out << FeatureText(*inner) << "|";
    out << "]";
  }
  return out.str();
}

std::vector<std::string> Signature(const Document& doc) {
  std::vector<std::string> sig;
  for (const Body& b : doc.GetBodies())
    sig.push_back("body " + BodyText(b));
  for (size_t i = 0; i < doc.FeatureCount(); i++)
    sig.push_back("feature " + std::to_string(i) + " " +
                  FeatureText(*doc.GetFeature(i)));
  return sig;
}

std::vector<fs::path> GoldenDocuments() {
  std::vector<fs::path> files;
  const fs::path dir = fs::path(TestSourceDir()) / "tests" / "golden";
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.path().extension() != ".json")
      continue;
    if (name.find(".expect.") != std::string::npos)
      continue;
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

// The document as a from-scratch rebuild sees it: saved, read back into
// fresh objects, regenerated with nothing cached. Memoized on the saved text,
// because the sequence below returns to the same few states again and again
// and a rebuild is what this test spends its time on.
std::map<std::string, std::vector<std::string>> g_scratch;

std::vector<std::string> FromScratch(const Document& doc, const fs::path& tmp) {
  REQUIRE(DocumentIO::Save(doc, tmp.string()).has_value());
  std::string text;
  {
    std::ifstream in(tmp);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  if (const auto it = g_scratch.find(text); it != g_scratch.end())
    return it->second;
  auto loaded = DocumentIO::Load(tmp.string());
  INFO((loaded ? std::string() : loaded.error()));
  REQUIRE(loaded.has_value());
  (*loaded)->ClearRegenCache();
  (*loaded)->Regenerate();
  return g_scratch[text] = Signature(**loaded);
}

void CheckMatches(const Document& doc, const fs::path& tmp,
                  const std::string& step) {
  INFO("after " << step);
  const std::vector<std::string> cached = Signature(doc);
  const std::vector<std::string> scratch = FromScratch(doc, tmp);
  REQUIRE(cached.size() == scratch.size());
  for (size_t i = 0; i < cached.size(); i++) {
    INFO("row " << i);
    CHECK(cached[i] == scratch[i]);
  }
}

// The index of the first feature at or after `from` with a literal numeric
// field, or npos.
size_t NumericFeatureFrom(const Document& doc, size_t from) {
  for (size_t i = from; i < doc.FeatureCount(); i++) {
    bool any = false;
    std::unique_ptr<Feature> probe = doc.GetFeature(i)->Clone();
    probe->VisitExprs([&](std::string& text, double&) {
      if (text.empty())
        any = true;
    });
    if (any)
      return i;
  }
  return std::string::npos;
}

// The feature at `index` with its first literal numeric field nudged.
nlohmann::json Nudged(const Document& doc, size_t index) {
  std::unique_ptr<Feature> f = doc.GetFeature(index)->Clone();
  bool done = false;
  f->VisitExprs([&](std::string& text, double& value) {
    if (done || !text.empty())
      return;
    value = value == 0.0 ? 0.5 : value * 1.1;
    done = true;
  });
  return DocumentIO::FeatureToJson(*f);
}

size_t UpTo(const Document& doc) {
  return doc.Rollback() < 0 ? doc.FeatureCount()
                            : std::min(doc.FeatureCount(),
                                       static_cast<size_t>(doc.Rollback()));
}

} // namespace

namespace {

// Hobbing a worm or modelling a thread costs seconds an evaluation: those
// documents get the late edit only, each in a test case of its own so ctest
// runs them beside the rest.
const std::vector<std::string> kHeavy = {"v140-modeled-thread", "v146-worm",
                                         "v173-thread-flank"};

bool IsHeavy(const fs::path& file) {
  return std::find(kHeavy.begin(), kHeavy.end(), file.stem().string()) !=
         kHeavy.end();
}

// Edits an early, a middle and a late numeric field, sets each back, undoes
// and redoes through the stack, removes a feature and undoes, moves one and
// back -- comparing with a from-scratch rebuild after every step, and
// asserting that every return to a state seen before executes nothing.
void RunSequence(const fs::path& file) {
  const fs::path tmp = fs::temp_directory_path() /
                       ("lobole_regen_cache_" + std::to_string(::getpid()) +
                        "_" + file.stem().string() + ".json");
  {
    INFO("golden " << file.filename().string());
    g_scratch.clear();
    auto loaded = DocumentIO::Load(file.string());
    REQUIRE(loaded.has_value());
    Document& doc = **loaded;
    if (doc.FeatureCount() == 0)
      return;
    // Settle: a hand-written golden holds unsolved sketches, whose Execute
    // writes the solved entities back into the very fields the key is made
    // of, so the first regeneration after load keys on what the file said
    // and every later one on what the solver made of it. A saved document
    // is already solved.
    doc.Regenerate();
    CommandStack stack;
    CheckMatches(doc, tmp, "load");

    const bool heavy = IsHeavy(file);
    const size_t n = doc.FeatureCount();
    std::vector<size_t> picks;
    std::vector<size_t> from = {0, n / 2, n - 1};
    if (heavy)
      from = {n - 1};
    for (size_t start : from) {
      const size_t i = NumericFeatureFrom(doc, start);
      if (i != std::string::npos &&
          std::find(picks.begin(), picks.end(), i) == picks.end())
        picks.push_back(i);
    }
    for (size_t k : picks) {
      INFO("edit at " << k);
      const nlohmann::json before =
          DocumentIO::FeatureToJson(*doc.GetFeature(k));
      const nlohmann::json after = Nudged(doc, k);
      if (after == before)
        continue; // the field is not one the feature keeps (clamped, say)
      REQUIRE(stack.Execute(
          std::make_unique<ModifyFeatureCommand>(k, before, after), doc));
      const size_t upTo = UpTo(doc);
      if (k < upTo) {
        // An edit executes exactly the tail it invalidated.
        CHECK(doc.LastRegenStats().cached == k);
        CHECK(doc.LastRegenStats().cached + doc.LastRegenStats().executed <=
              upTo);
      }
      CheckMatches(doc, tmp, "edit");
      REQUIRE(stack.Execute(
          std::make_unique<ModifyFeatureCommand>(k, after, before), doc));
      CHECK(doc.LastRegenStats().executed == 0);
      CheckMatches(doc, tmp, "set back");
      stack.Undo(doc); // back to the edit
      CHECK(doc.LastRegenStats().executed == 0);
      CheckMatches(doc, tmp, "undo");
      stack.Undo(doc); // back to the original
      CHECK(doc.LastRegenStats().executed == 0);
      CheckMatches(doc, tmp, "undo twice");
      stack.Redo(doc);
      CHECK(doc.LastRegenStats().executed == 0);
      CheckMatches(doc, tmp, "redo");
      stack.Redo(doc);
      CHECK(doc.LastRegenStats().executed == 0);
      CheckMatches(doc, tmp, "redo twice");
    }

    // Remove a feature in the middle and undo the removal.
    if (n >= 2 && !heavy) {
      const size_t r = n / 2;
      if (stack.Execute(std::make_unique<RemoveFeatureCommand>(r), doc)) {
        CheckMatches(doc, tmp, "remove");
        stack.Undo(doc);
        CHECK(doc.LastRegenStats().executed == 0);
        CheckMatches(doc, tmp, "remove undone");
        stack.Redo(doc);
        CHECK(doc.LastRegenStats().executed == 0);
        CheckMatches(doc, tmp, "remove redone");
        stack.Undo(doc);
      }
    }
    // Move the last feature one up and back, when the move is allowed.
    if (n >= 2 && !heavy) {
      if (stack.Execute(std::make_unique<MoveFeatureCommand>(n - 1, n - 2),
                        doc)) {
        CheckMatches(doc, tmp, "move");
        stack.Undo(doc);
        CHECK(doc.LastRegenStats().executed == 0);
        CheckMatches(doc, tmp, "move undone");
      }
    }
  }
  std::error_code ec;
  fs::remove(tmp, ec);
}

} // namespace

TEST_CASE("cached_evaluation_matches_from_scratch",
          "[regen][determinism][slow]") {
  const std::vector<fs::path> files = GoldenDocuments();
  REQUIRE(!files.empty());
  for (const fs::path& file : files)
    if (!IsHeavy(file))
      RunSequence(file);
}

TEST_CASE("cached_evaluation_matches_from_scratch: modeled thread",
          "[regen][determinism][slow]") {
  RunSequence(fs::path(TestSourceDir()) /
              "tests/golden/v140-modeled-thread.json");
}

TEST_CASE("cached_evaluation_matches_from_scratch: worm",
          "[regen][determinism][slow]") {
  RunSequence(fs::path(TestSourceDir()) / "tests/golden/v146-worm.json");
}

TEST_CASE("cached_evaluation_matches_from_scratch: thread flank",
          "[regen][determinism][slow]") {
  RunSequence(fs::path(TestSourceDir()) /
              "tests/golden/v173-thread-flank.json");
}

TEST_CASE("Regen cache: a long edit sequence stays under the cap and keeps "
          "the current chain",
          "[regen]") {
  Document doc;
  // 40 features: one block, then 39 small boxes, each its own body.
  for (int i = 0; i < 40; i++)
    doc.AddFeature(std::make_unique<BoxFeature>("box" + std::to_string(i),
                                                glm::dvec3(i * 20.0, 0.0, 0.0),
                                                10.0, 10.0, 10.0));
  REQUIRE(doc.FeatureCount() == 40);
  CommandStack stack;
  const size_t cap = doc.RegenCacheCap();
  CHECK(cap == 256);
  // Every edit near the top makes a new history of ~40 entries; 30 of them
  // are far past what the cap holds.
  for (int step = 0; step < 30; step++) {
    const size_t k = static_cast<size_t>(step % 3);
    const nlohmann::json before = DocumentIO::FeatureToJson(*doc.GetFeature(k));
    nlohmann::json after = before;
    after["width"] = 10.0 + step + 1;
    REQUIRE(stack.Execute(
        std::make_unique<ModifyFeatureCommand>(k, before, after), doc));
    CHECK(doc.RegenCacheSize() <= cap);
    // The current chain is never evicted: every feature's input is there.
    for (size_t i = 0; i < doc.FeatureCount(); i++)
      CHECK(doc.CachedBodiesAfter(i) != nullptr);
    // A regeneration of the same state executes nothing.
    doc.Regenerate();
    CHECK(doc.LastRegenStats().executed == 0);
  }
  // The most recent history is still remembered: one undo is free.
  stack.Undo(doc);
  CHECK(doc.LastRegenStats().executed == 0);
}

TEST_CASE("Regen cache: a hit restores the cut tool a Pattern reads",
          "[regen][pattern]") {
  // A Pattern of a cut replicates the cut's TOOL (ExtrudeFeature::m_tool),
  // which only the cut's Execute writes. Edit the cut and set it back: every
  // feature is then a hit, and the cut's object (fresh from JSON, as
  // ModifyFeatureCommand makes it) holds no tool unless the hit restored
  // one. Then edit the PATTERN, which executes and reads that tool.
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, 0), 60, 20, 5));
  auto sk =
      std::make_unique<SketchFeature>("HoleSk", SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Circle{{10, 10}, 3.0});
  doc.AddFeature(std::move(sk));
  const uint64_t skId = doc.GetFeature(1)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Hole", skId, 5.0,
                                                  ExtrudeMode::Cut, false));
  const uint64_t cutId = doc.GetFeature(2)->GetId();
  auto pat =
      std::make_unique<PatternFeature>("Holes x4", PatternKind::Linear, cutId);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 4;
  pat->spacing = 12.0;
  doc.AddFeature(std::move(pat));
  REQUIRE(doc.GetFeature(3)->GetError().empty());

  CommandStack stack;
  const nlohmann::json cut = DocumentIO::FeatureToJson(*doc.GetFeature(2));
  nlohmann::json shallow = cut;
  shallow["distance"] = 2.0;
  REQUIRE(stack.Execute(std::make_unique<ModifyFeatureCommand>(2, cut, shallow),
                        doc));
  REQUIRE(stack.Execute(std::make_unique<ModifyFeatureCommand>(2, shallow, cut),
                        doc));
  CHECK(doc.LastRegenStats().executed == 0);
  REQUIRE(doc.GetFeature(2)->ToolShape() != nullptr);

  const nlohmann::json row = DocumentIO::FeatureToJson(*doc.GetFeature(3));
  nlohmann::json wider = row;
  wider["spacing"] = 13.0;
  REQUIRE(stack.Execute(std::make_unique<ModifyFeatureCommand>(3, row, wider),
                        doc));
  CHECK(doc.LastRegenStats().executed == 1);
  double volume = 0.0;
  for (const Body& b : doc.GetBodies())
    volume += b.shape.Volume();
  const double hole = 3.1415926535897932 * 9.0 * 5.0;
  CHECK(std::abs(volume - (6000.0 - 4.0 * hole)) < 1e-3 * 6000.0);
}
