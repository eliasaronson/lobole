// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "mutate/ReaderFuzz.h"
#include "mutate/Mutate.h"

#include "TestPaths.h"

#include "common/Logger.h"

#include "common/ImageLimits.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/MotionStudy.h"
#include "core/Occurrence.h"
#include "core/cam/JobDocument.h"
#include "core/drawing/DrawingDocument.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/CrashReports.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectMerge.h"
#include "core/io/ProjectPaths.h"
#include "core/io/Serialization.h"
#include "core/io/Settings.h"
#include "remote/InstanceRegistry.h"
#include "thirdparty/stb_image.h"
#include "thirdparty/stb_image_write.h"
#include "ui/AppActionsInternal.h" // SlotDoc and its kin
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/ProjectActions.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <unistd.h>

namespace readerfuzz {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---- files -----------------------------------------------------------------

std::string ReadBytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& p, const std::string& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

fs::path BuildDir() {
  return fs::path(TestBinaryPath()).parent_path().parent_path();
}

fs::path GoldenDir() { return fs::path(TestSourceDir()) / "tests" / "golden"; }

// Per process and family: families run side by side under ctest.
fs::path ScratchRoot(const std::string& family) {
  return fs::temp_directory_path() /
         ("lobole-reader-fuzz-" + std::to_string(::getpid()) + "-" + family);
}

std::vector<fs::path> Goldens(const fs::path& dir) {
  std::vector<fs::path> out;
  for (const auto& e : fs::directory_iterator(dir))
    if (e.is_regular_file() && e.path().extension() == ".json" &&
        !e.path().stem().string().ends_with(".expect"))
      out.push_back(e.path());
  std::sort(out.begin(), out.end());
  return out;
}

// ---- content ---------------------------------------------------------------
//
// A file's content is its set of non-null scalars, each named by file and
// JSON pointer: "features/7#/radius". A collection item's file is named by
// its id alone ("features/7"), since a rename moves its slug. Derived files
// (baked geometry, saved results and projections) are not content: they are
// rebuilt, and their drop is reported, not counted.

bool IsDerived(const std::string& rel) {
  const fs::path p(rel);
  for (const auto& part : p)
    if (part == "geometry")
      return true;
  const std::string name = p.filename().string();
  return name.ends_with(".result.json") || name.ends_with(".projection.json");
}

std::string NormFile(const std::string& rel) {
  fs::path p(rel);
  const std::string name = p.filename().string();
  size_t d = 0;
  while (d < name.size() && std::isdigit(static_cast<unsigned char>(name[d])))
    d++;
  if (d > 0 && d < name.size() && name[d] == '-' && p.has_parent_path())
    return (p.parent_path() / name.substr(0, d)).generic_string();
  return p.generic_string();
}

std::string Escape(const std::string& key) {
  std::string out;
  for (char c : key)
    out += c == '~'   ? std::string("~0")
           : c == '/' ? std::string("~1")
                      : std::string(1, c);
  return out;
}

// One leaf of content: where it is (its key, which names a collection
// item by its id), its SHAPE (every list position and item id a `*`, so a
// leaf that moved with its item still matches), and where it concretely is
// (the file and the JSON pointer), to change it.
struct Leaf {
  std::string shape;
  std::string rel;     // the file, relative ("" for a single file)
  std::string pointer; // concrete: list positions as indices
  json value;
};
using Leaves = std::map<std::string, Leaf>;

void AddLeaves(const json& j, const std::string& file, const std::string& rel,
               const std::string& ptr, const std::string& shape,
               const std::string& concrete, Leaves& out) {
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      const std::string k = "/" + Escape(it.key());
      AddLeaves(it.value(), file, rel, ptr + k, shape + k, concrete + k, out);
    }
  } else if (j.is_array()) {
    // An item with an id is named by it, not by where it stands: a list
    // in another order holds the same content.
    for (size_t i = 0; i < j.size(); i++) {
      const json& e = j[i];
      const bool item = e.is_object() && e.contains("id") &&
                        e.at("id").is_number_unsigned() &&
                        e.at("id").get<uint64_t>() != 0;
      AddLeaves(e, file, rel,
                ptr + "/" +
                    (item ? "id=" + std::to_string(e.at("id").get<uint64_t>())
                          : std::to_string(i)),
                shape + "/*", concrete + "/" + std::to_string(i), out);
    }
  } else if (!j.is_null()) {
    // The file's id is part of the shape's `*` too: features/7 -> features/*.
    std::string fileShape = file;
    if (const size_t slash = file.rfind('/');
        slash != std::string::npos && slash + 1 < file.size() &&
        std::isdigit(static_cast<unsigned char>(file[slash + 1])))
      fileShape = file.substr(0, slash) + "/*";
    out[file + "#" + ptr] = {fileShape + "#" + shape, rel, concrete, j};
  }
}

// The content of a file (named `single`) or of every JSON file of a tree.
Leaves LeavesOf(const fs::path& path, const std::string& single) {
  Leaves out;
  const auto addFile = [&](const fs::path& file, const std::string& name,
                           const std::string& rel) {
    try {
      AddLeaves(json::parse(ReadBytes(file)), name, rel, "", "", "", out);
    } catch (const std::exception&) {
      // Not JSON (or too deep): no content to count.
    }
  };
  std::error_code ec;
  if (!fs::is_directory(path, ec)) {
    addFile(path, single, "");
    return out;
  }
  for (auto it = fs::recursive_directory_iterator(path, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec)
      break;
    if (!it->is_regular_file(ec) || it->path().extension() != ".json")
      continue;
    const std::string rel =
        it->path().lexically_relative(path).generic_string();
    if (IsDerived(rel))
      continue;
    addFile(it->path(), NormFile(rel), rel);
  }
  return out;
}

// What `in` holds that `out` does not.
Leaves Lost(const Leaves& in, const Leaves& out) {
  Leaves lost;
  for (const auto& [key, leaf] : in)
    if (!out.contains(key))
      lost.emplace(key, leaf);
  return lost;
}

// What a round trip loses net, by shape: leaves of a shape it lost, less
// those of the same shape it wrote that were not there (an item whose id
// changed moves all its leaves to new keys; a default it writes). The
// unmutated input's net loss is its migrations and its defaults, and is
// taken out of a case's.
std::map<std::string, long> NetLoss(const Leaves& in, const Leaves& out) {
  std::map<std::string, long> net;
  for (const auto& [key, leaf] : in)
    if (!out.contains(key))
      net[leaf.shape]++;
  for (const auto& [key, leaf] : out)
    if (!in.contains(key))
      net[leaf.shape]--;
  return net;
}

// Every file under a tree (or the one file), relative, with its bytes.
std::map<std::string, std::string> Snapshot(const fs::path& path) {
  std::map<std::string, std::string> out;
  std::error_code ec;
  if (!fs::is_directory(path, ec)) {
    out[""] = ReadBytes(path);
    return out;
  }
  for (auto it = fs::recursive_directory_iterator(path, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec))
      out[it->path().lexically_relative(path).generic_string()] =
          ReadBytes(it->path());
  return out;
}

// A snapshot without the baked geometry: a body built fresh and the same
// body read back from its blob write different bytes (BinTools), so a
// second save of a part that rebuilt differs there and nowhere else.
std::map<std::string, std::string>
WithoutBlobs(std::map<std::string, std::string> snap) {
  // The whole of geometry/: its index's content hash covers the B-Reps'
  // bytes too.
  std::erase_if(snap, [](const auto& kv) {
    return kv.first.starts_with("geometry/") ||
           kv.first.find("/geometry/") != std::string::npos;
  });
  return snap;
}

// The first difference between two snapshots, or "".
std::string FirstDifference(const std::map<std::string, std::string>& a,
                            const std::map<std::string, std::string>& b) {
  for (const auto& [rel, bytes] : a) {
    const auto it = b.find(rel);
    if (it == b.end())
      return rel + " is only in the first";
    if (it->second != bytes)
      return rel + " differs";
  }
  for (const auto& [rel, bytes] : b)
    if (!a.contains(rel))
      return rel + " is only in the second";
  return "";
}

// ---- readers ---------------------------------------------------------------

// A loaded document that can be saved again.
struct Loaded {
  Outcome outcome;
  std::function<bool(const fs::path&, std::string*)> save;
};

std::map<std::string, size_t> PartCounts(const Document& d) {
  size_t failed = 0;
  for (size_t i = 0; i < d.FeatureCount(); i++)
    failed += !d.GetFeature(i)->GetError().empty();
  return {{"features", d.FeatureCount()},
          {"bodies", d.GetBodies().size()},
          {"failed", failed},
          {"studies", d.GetStudies().size()},
          {"configurations", d.GetConfigurations().size()}};
}

std::map<std::string, size_t> AssemblyCounts(const AssemblyDocument& a) {
  return {{"occurrences", a.OccurrenceCount()},
          {"mates", a.MateCount()},
          {"relations", a.GetRelations().size()},
          {"motion", a.GetMotionStudies().size()},
          {"studies", a.GetStudies().size()},
          {"configurations", a.GetConfigurations().size()}};
}

std::map<std::string, size_t> DrawingCounts(const DrawingDocument& d) {
  return {{"views", d.ViewCount()},
          {"dims", d.DimCount()},
          {"notes", d.NoteCount()}};
}

std::map<std::string, size_t> JobCounts(const cam::JobDocument& j) {
  size_t ops = 0;
  for (const cam::Setup& s : j.GetSetups())
    ops += s.operations.size();
  return {{"parts", j.PartCount()},
          {"setups", j.SetupCount()},
          {"operations", ops}};
}

template <class Doc, class LoadFn, class SaveFn, class CountFn>
Loaded LoadWith(const fs::path& path, LoadFn load, SaveFn save, CountFn count) {
  Loaded l;
  readctx::CollectDrops drops;
  auto r = load(path.string());
  l.outcome.drops = drops.Take();
  if (!r) {
    l.outcome.message = r.error();
    return l;
  }
  l.outcome.loaded = true;
  std::shared_ptr<Doc> doc(std::move(*r));
  l.outcome.counts = count(*doc);
  l.save = [doc, save](const fs::path& to, std::string* error) {
    auto s = save(*doc, to.string());
    if (!s && error)
      *error = s.error();
    return s.has_value();
  };
  return l;
}

Loaded LoadProject(const fs::path& path) {
  Loaded l;
  auto read = ui::ReadProject(path.string());
  if (!read) {
    l.outcome.message = read.error();
    return l;
  }
  l.outcome.drops = read->drops;
  auto state = std::make_shared<ui::AppState>();
  std::string error;
  if (!ui::AdoptProject(*state, std::move(*read), &error)) {
    l.outcome.message = error;
    return l;
  }
  l.outcome.loaded = true;
  size_t features = 0, occurrences = 0, views = 0, setups = 0;
  for (size_t i = 0; i < state->parts.size(); i++) {
    if (const Document* d = ui::SlotDoc(*state, i))
      features += d->FeatureCount();
    if (const AssemblyDocument* a = ui::SlotAssembly(*state, i))
      occurrences += a->OccurrenceCount();
    if (const DrawingDocument* d = ui::SlotDrawing(*state, i))
      views += d->ViewCount();
    if (const cam::JobDocument* j = ui::SlotJob(*state, i))
      setups += j->SetupCount();
  }
  l.outcome.counts = {{"documents", state->parts.size()},
                      {"features", features},
                      {"occurrences", occurrences},
                      {"views", views},
                      {"setups", setups},
                      {"requirements", state->requirements.size()},
                      {"sections", state->sections.size()},
                      {"parameters", state->projectParameters.size()}};
  l.save = [state](const fs::path& to, std::string* error) {
    return ui::SaveProject(*state, to.string(), error, true);
  };
  return l;
}

Loaded LoadAs(Family family, const fs::path& path) {
  switch (family) {
  case Family::PartFile:
  case Family::PartDir:
    return LoadWith<Document>(
        path, [](const std::string& p) { return DocumentIO::Load(p); },
        [](const Document& d, const std::string& p) {
          return DocumentIO::Save(d, p);
        },
        PartCounts);
  case Family::AssemblyFile:
  case Family::AssemblyDir:
    return LoadWith<AssemblyDocument>(
        path, [](const std::string& p) { return DocumentIO::LoadAssembly(p); },
        [](const AssemblyDocument& d, const std::string& p) {
          return DocumentIO::SaveAssembly(d, p);
        },
        AssemblyCounts);
  case Family::DrawingFile:
  case Family::DrawingDir:
    return LoadWith<DrawingDocument>(
        path, [](const std::string& p) { return DocumentIO::LoadDrawing(p); },
        [](const DrawingDocument& d, const std::string& p) {
          return DocumentIO::SaveDrawing(d, p);
        },
        DrawingCounts);
  case Family::JobFile:
  case Family::JobDir:
    return LoadWith<cam::JobDocument>(
        path, [](const std::string& p) { return DocumentIO::LoadJob(p); },
        [](const cam::JobDocument& d, const std::string& p) {
          return DocumentIO::SaveJob(d, p);
        },
        JobCounts);
  case Family::Project:
  case Family::ExternalCorpus:
    return LoadProject(path);
  default:
    break;
  }
  Loaded l;
  l.outcome.message = "not a document family";
  return l;
}

} // namespace

const char* FamilyName(Family f) {
  switch (f) {
  case Family::PartFile:
    return "part";
  case Family::AssemblyFile:
    return "assembly";
  case Family::DrawingFile:
    return "drawing";
  case Family::JobFile:
    return "job";
  case Family::PartDir:
    return "part-dir";
  case Family::AssemblyDir:
    return "assembly-dir";
  case Family::DrawingDir:
    return "drawing-dir";
  case Family::JobDir:
    return "job-dir";
  case Family::Project:
    return "project";
  case Family::Settings:
    return "settings";
  case Family::Profiles:
    return "profiles";
  case Family::CrashReports:
    return "crash";
  case Family::Instances:
    return "instances";
  case Family::Recovery:
    return "recovery";
  case Family::Vault:
    return "vault";
  case Family::Images:
    return "images";
  case Family::ExternalCorpus:
    return "corpus";
  }
  return "?";
}

std::optional<Family> FamilyFromName(const std::string& name) {
  for (int i = 0; i <= static_cast<int>(Family::ExternalCorpus); i++)
    if (name == FamilyName(static_cast<Family>(i)))
      return static_cast<Family>(i);
  return std::nullopt;
}

std::string Summary::Text() const {
  std::ostringstream s;
  s << "reader fuzz " << family << ": " << cases << " cases (" << refused
    << " refused, " << loaded << " loaded) in " << seconds << " s; slowest "
    << slowest << " s (" << slowestId << "); " << failures.size() << " failed";
  return s.str();
}

// ---- the corpus ------------------------------------------------------------

namespace {

// One valid input: a file or a tree, and what its unmutated read gives.
struct Input {
  std::string corpus; // the id's first part: "part/v17-full.json"
  fs::path path;      // the file, or the tree's root
  bool tree = false;
  // A different kind's index.json, for swap_kind.
  std::string foreign;
};

bool IsDocFamily(Family f) {
  switch (f) {
  case Family::PartFile:
  case Family::AssemblyFile:
  case Family::DrawingFile:
  case Family::JobFile:
  case Family::PartDir:
  case Family::AssemblyDir:
  case Family::DrawingDir:
  case Family::JobDir:
  case Family::Project:
  case Family::ExternalCorpus:
    return true;
  default:
    return false;
  }
}

// A study result made by hand (no solve), so a saved part carries the
// `.result.json` + `.result.bin` pair the reader must survive damage to.
fem::StudyRuntime HandMadeResult() {
  fem::StudyRuntime rt;
  rt.topology = true;
  rt.iterations = 3;
  rt.converged = true;
  rt.volumeFractionAchieved = 0.4;
  rt.complianceHistory = {3.0, 2.0, 1.5};
  fem::StudyBodyResult b;
  b.bodyIndex = 0;
  b.bodyName = "Body";
  for (int i = 0; i < 10; ++i)
    b.mesh.nodes.push_back(glm::dvec3(i, 0.5 * i, 0.25 * i));
  b.mesh.tets.push_back({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  b.density = {0.7};
  rt.bodies.push_back(std::move(b));
  rt.inputHash = 0x1234;
  return rt;
}

motion::Result HandMadeMotion(const AssemblyDocument& a) {
  motion::Result r;
  for (int i = 0; i < 3; i++) {
    motion::Sample s;
    s.t = i / 2.0;
    s.value = i * 0.5;
    for (const Occurrence& o : a.GetOccurrences())
      s.worlds.emplace_back(o.id, glm::dmat4(1.0));
    r.samples.push_back(std::move(s));
  }
  r.inputHash = 0x5678;
  return r;
}

void Require(bool ok, const std::string& what) {
  if (!ok)
    throw std::runtime_error("reader fuzz corpus: " + what);
}

// The directory form of each golden of a kind, as this build writes it.
template <class LoadFn, class SaveFn, class Tweak>
std::vector<Input> DirCorpus(const char* family, const fs::path& goldens,
                             const fs::path& scratch, LoadFn load, SaveFn save,
                             Tweak tweak) {
  std::vector<Input> out;
  for (const fs::path& g : Goldens(goldens)) {
    auto r = load(g.string());
    Require(r.has_value(), g.string() + " does not load");
    tweak(**r, g.stem().string());
    const fs::path dir = scratch / "corpus" / (g.stem().string() + ".dsgn");
    fs::remove_all(dir);
    Require(save(**r, dir.string()).has_value(), g.string() + " does not save");
    out.push_back(
        {std::string(family) + "/" + g.stem().string(), dir, true, {}});
  }
  return out;
}

std::vector<Input> BuildCorpus(Family family, const fs::path& scratch) {
  std::vector<Input> out;
  const fs::path g = GoldenDir();
  const auto files = [&](const fs::path& dir) {
    for (const fs::path& p : Goldens(dir))
      out.push_back(
          {std::string(FamilyName(family)) + "/" + p.filename().string(),
           p,
           false,
           {}});
  };
  const auto noTweak = [](auto&, const std::string&) {};
  switch (family) {
  case Family::PartFile:
    files(g);
    break;
  case Family::AssemblyFile:
    files(g / "assembly");
    break;
  case Family::DrawingFile:
    files(g / "drawing");
    break;
  case Family::JobFile:
    files(g / "job");
    break;
  case Family::PartDir:
    out = DirCorpus(
        "part-dir", g, scratch,
        [](const std::string& p) { return DocumentIO::Load(p); },
        [](const Document& d, const std::string& p) {
          return DocumentIO::Save(d, p);
        },
        [](Document& d, const std::string& stem) {
          // One part carries a saved study result (.result.json + .bin).
          if (stem == "v153-saved-study-results" && !d.GetStudies().empty())
            d.StoreStudyResults(d.GetStudies().front().id, HandMadeResult());
        });
    break;
  case Family::AssemblyDir:
    out = DirCorpus(
        "assembly-dir", g / "assembly", scratch,
        [](const std::string& p) { return DocumentIO::LoadAssembly(p); },
        [](const AssemblyDocument& d, const std::string& p) {
          return DocumentIO::SaveAssembly(d, p);
        },
        [](AssemblyDocument& a, const std::string& stem) {
          // One assembly carries a saved motion result.
          if (stem == "v87-motion-study" && !a.GetMotionStudies().empty())
            a.StoreMotionResult(
                a.GetMotionStudies().front().id,
                std::make_shared<const motion::Result>(HandMadeMotion(a)));
        });
    break;
  case Family::DrawingDir: {
    out = DirCorpus(
        "drawing-dir", g / "drawing", scratch,
        [](const std::string& p) { return DocumentIO::LoadDrawing(p); },
        [](const DrawingDocument& d, const std::string& p) {
          return DocumentIO::SaveDrawing(d, p);
        },
        noTweak);
    // The drawing whose views carry saved projections, as committed.
    const fs::path stored = g / "drawing" / "v162-stored-projection";
    const fs::path dir = scratch / "corpus" / "v162-stored-projection.dsgn";
    fs::remove_all(dir);
    fs::copy(stored, dir, fs::copy_options::recursive);
    out.push_back({"drawing-dir/v162-stored-projection.dsgn", dir, true, {}});
    break;
  }
  case Family::JobDir:
    out = DirCorpus(
        "job-dir", g / "job", scratch,
        [](const std::string& p) { return DocumentIO::LoadJob(p); },
        [](const cam::JobDocument& d, const std::string& p) {
          return DocumentIO::SaveJob(d, p);
        },
        noTweak);
    break;
  case Family::Project: {
    // One container of every kind: a part with a saved result, an
    // assembly with a saved motion result, a drawing with saved
    // projections, a job; project variables, a folder, a section cut and
    // a requirement.
    const fs::path dir = scratch / "corpus" / "project.dsgn";
    fs::remove_all(dir);
    fs::create_directories(dir);
    json docs = json::array();
    const auto entry = [&](uint64_t id, const std::string& name,
                           const std::string& type, const std::string& sub) {
      const std::string folder = sub + "/" + projectpaths::DocFolder(name, id);
      docs.push_back({{"id", id},
                      {"name", name},
                      {"dir", folder},
                      {"type", type},
                      {"folder", "Group"}});
      fs::create_directories(dir / folder); // a directory: the dir form
      return dir / folder;
    };
    {
      auto d = DocumentIO::Load((g / "v153-saved-study-results.json").string());
      Require(d.has_value(), "v153 does not load");
      (*d)->StoreStudyResults((*d)->GetStudies().front().id, HandMadeResult());
      Require(DocumentIO::Save(**d, entry(1, "Plate", "part", "parts").string())
                  .has_value(),
              "the part does not save");
    }
    {
      auto a = DocumentIO::LoadAssembly(
          (g / "assembly" / "v87-motion-study.json").string());
      Require(a.has_value(), "v87 does not load");
      (*a)->StoreMotionResult(
          (*a)->GetMotionStudies().front().id,
          std::make_shared<const motion::Result>(HandMadeMotion(**a)));
      Require(DocumentIO::SaveAssembly(
                  **a, entry(2, "Crank", "assembly", "assemblies").string())
                  .has_value(),
              "the assembly does not save");
    }
    {
      auto d = DocumentIO::LoadDrawing(
          (g / "drawing" / "v162-stored-projection").string());
      Require(d.has_value(), "v162 does not load");
      Require(DocumentIO::SaveDrawing(
                  **d, entry(3, "Sheet", "drawing", "drawings").string())
                  .has_value(),
              "the drawing does not save");
    }
    {
      auto j =
          DocumentIO::LoadJob((g / "job" / "v118-cam-surfacing.json").string());
      Require(j.has_value(), "v118 does not load");
      Require(DocumentIO::SaveJob(**j, entry(4, "Mill", "job", "jobs").string())
                  .has_value(),
              "the job does not save");
    }
    json requirement = json::parse(ReadBytes(g / "v160-requirements.json"))
                           .at("requirements")
                           .at(0);
    requirement["measure"]["document"] = 1;
    WriteBytes(dir / "requirements" / "1-pad-volume.json",
               requirement.dump(2) + "\n");
    const json index = {
        {"format", "designer-project"},
        {"version", 2},
        {"active", 0},
        {"parameters", json::array({{{"name", "w"}, {"expression", "10"}}})},
        {"folders", json::array({"Group"})},
        {"sections", json::array({{{"base_plane", 0},
                                   {"offset", 1.5},
                                   {"reverse", false},
                                   {"visible", true}}})},
        {"documents", docs},
        {"requirements_next_id", 2}};
    WriteBytes(dir / "project.json", index.dump(2) + "\n");
    out.push_back({"project/project.dsgn", dir, true, {}});
    break;
  }
  case Family::ExternalCorpus: {
    const char* root = std::getenv("LOBOLE_READER_CORPUS");
    if (root == nullptr)
      break;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec)
        break;
      const fs::path& p = it->path();
      if (it->is_directory(ec) && p.extension() == ".dsgn") {
        if (fs::exists(p / "project.json") || fs::exists(p / "index.json"))
          out.push_back({"corpus/" + fs::relative(p, root).generic_string(),
                         p,
                         true,
                         {}});
        it.disable_recursion_pending();
      }
    }
    std::sort(out.begin(), out.end(), [](const Input& a, const Input& b) {
      return a.corpus < b.corpus;
    });
    break;
  }
  default:
    break;
  }
  // swap_kind: a directory document handed another kind's index.json.
  if (family == Family::PartDir || family == Family::AssemblyDir ||
      family == Family::DrawingDir || family == Family::JobDir) {
    const json other = {
        {"format", "designer-document"},
        {"version", DocumentIO::DocumentFormatVersion()},
        {"kind", family == Family::DrawingDir ? "job" : "drawing"},
        {"views", json::array()},
        {"dims", json::array()},
        {"notes", json::array()},
        {"sheet", json::object()}};
    for (Input& in : out)
      in.foreign = other.dump(2) + "\n";
  }
  return out;
}

// ---- one case --------------------------------------------------------------

struct Case {
  std::string id, op;
  std::vector<mutate::Edit> edits; // a tree's (a file's is one Write of "")
  std::string bytes;               // a file's
};

// The ops whose damage leaves the document's MEANING alone, so a load must
// give the same bodies as the original: keys reordered, and damage to
// derived files only.
bool MeaningKept(const Case& c) {
  if (c.op == "reorder_keys")
    return true;
  if (c.edits.empty())
    return false;
  return std::all_of(c.edits.begin(), c.edits.end(),
                     [](const mutate::Edit& e) { return IsDerived(e.rel); });
}

std::string CaseFile(const Case& c) {
  const size_t colon = c.id.find(':');
  return colon == std::string::npos ? c.id : c.id.substr(0, colon);
}

double Budget() {
#ifdef LOBOLE_READER_FUZZ_SANITIZED
  return 8.0;
#else
  return 2.0;
#endif
}

} // namespace

namespace {

// An environment variable for as long as it lives.
class ScopedEnv {
public:
  ScopedEnv(const char* name, const std::string& value) : m_name(name) {
    if (const char* old = std::getenv(name))
      m_old = old;
    ::setenv(name, value.c_str(), 1);
  }
  ~ScopedEnv() {
    if (m_old)
      ::setenv(m_name, m_old->c_str(), 1);
    else
      ::unsetenv(m_name);
  }

private:
  const char* m_name;
  std::optional<std::string> m_old;
};

void CopyTree(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::remove_all(to, ec);
  fs::create_directories(to.parent_path(), ec);
  fs::copy(from, to,
           fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
}

// Every file under `root` -- for "nothing was written outside".
std::set<std::string> FilesUnder(const fs::path& root) {
  std::set<std::string> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file(ec))
      out.insert(it->path().lexically_relative(root).generic_string());
  return out;
}

std::vector<unsigned char> EncodeImage(bool jpeg) {
  constexpr int w = 24, h = 16;
  std::vector<unsigned char> pixels(w * h * 3);
  for (int i = 0; i < w * h; i++) {
    pixels[3 * i] = static_cast<unsigned char>(i * 7);
    pixels[3 * i + 1] = static_cast<unsigned char>(i * 13);
    pixels[3 * i + 2] = static_cast<unsigned char>(255 - i);
  }
  std::vector<unsigned char> out;
  const auto sink = [](void* ctx, void* data, int size) {
    auto* o = static_cast<std::vector<unsigned char>*>(ctx);
    const auto* b = static_cast<const unsigned char*>(data);
    o->insert(o->end(), b, b + size);
  };
  if (jpeg)
    stbi_write_jpg_to_func(sink, &out, w, h, 3, pixels.data(), 90);
  else
    stbi_write_png_to_func(sink, &out, w, h, 3, pixels.data(), w * 3);
  return out;
}

} // namespace

struct Sweep::Impl {
  Family family;
  bool every;
  uint64_t seed;
  fs::path scratch;
  Summary summary;
  fs::path lastCase;

  int shard = 0, shards = 1;

  // RunPlanted: a reader in place of the real one, and a cap on cases.
  const Planted* planted = nullptr;
  size_t limit = 0;
  const std::vector<std::pair<std::string, std::string>>* given = nullptr;

  Loaded Read(const fs::path& path) {
    if (planted == nullptr)
      return LoadAs(family, path);
    Loaded l;
    l.outcome = planted->read(path);
    if (l.outcome.loaded)
      l.save = [p = planted](const fs::path& to, std::string* error) {
        const bool ok = p->save(to);
        if (!ok && error)
          *error = "the planted reader would not save";
        return ok;
      };
    return l;
  }

  void Fail(const std::string& id, const std::string& why) {
    summary.failures.push_back(id + ": " + why);
  }

  void Note(const std::string& id) {
    std::cerr << "reader-fuzz case " << id << "\n";
    WriteBytes(lastCase, id + "\n");
  }

  mutate::Options FileOptions() {
    mutate::Options o;
    o.seed = seed;
    o.version = DocumentIO::DocumentFormatVersion();
    o.want = [this](const std::string& id) { return Want(id); };
    return o;
  }

  mutate::TreeOptions TreeOptions(const Input& in) {
    mutate::TreeOptions t;
    t.file = FileOptions();
    // A tree has many files: each gets a lighter truncation sweep than a
    // single-file document, whose every length under 1 KB is tried.
    t.file.truncateAll = 64;
    t.file.truncateEnds = 8;
    t.file.truncateSpread = 16;
    t.file.perOperator = 3;
    t.binary = t.file;
    t.binary.binary = true;
    t.binary.perOperator = 2;
    t.foreignIndex = in.foreign;
    t.outsideFile = (scratch / "outside.json").string();
    return t;
  }

  std::vector<Case> Cases(const Input& in) {
    std::vector<Case> out;
    if (given != nullptr && !given->empty()) {
      for (const auto& [id, bytes] : *given)
        out.push_back({id, "given", {}, bytes});
      return out;
    }
    if (in.tree) {
      for (mutate::TreeCase& c :
           mutate::TreeCases(in.corpus, in.path, TreeOptions(in)))
        out.push_back(
            {std::move(c.id), std::move(c.op), std::move(c.edits), {}});
    } else {
      const std::string name = in.path.filename().string();
      const std::string corpus =
          in.corpus.substr(0, in.corpus.size() - name.size() - 1);
      for (mutate::FileCase& c :
           mutate::FileCases(corpus, name, ReadBytes(in.path), FileOptions()))
        out.push_back(
            {std::move(c.id), std::move(c.op), {}, std::move(c.bytes)});
    }
    return out;
  }

  // The fixed sample: every operator at least once per family, about 1/50
  // of the rest. Asked as the cases are made, so a case not in the sample
  // is never built.
  std::set<std::string> seenOps;
  std::string only; // WriteCase: just this case
  bool Want(const std::string& id) { return Wanted(id, every); }
  bool Wanted(const std::string& id, bool all) {
    if (!only.empty())
      return id == only;
    // LOBOLE_READER_FUZZ_OPS=a,b: just those operators (chasing one).
    if (const char* ops = std::getenv("LOBOLE_READER_FUZZ_OPS")) {
      const size_t last = id.rfind(':');
      const size_t opAt = id.rfind(':', last - 1);
      const std::string op = id.substr(opAt + 1, last - opAt - 1);
      if (("," + std::string(ops) + ",").find("," + op + ",") ==
          std::string::npos)
        return false;
    }
    if (all)
      return true;
    const size_t last = id.rfind(':');
    const size_t opAt = id.rfind(':', last - 1);
    if (seenOps.insert(id.substr(opAt + 1, last - opAt - 1)).second)
      return true;
    return mutate::Hash(id) % 50 == 0;
  }

  Summary Run() {
    if (summary.family.empty())
      summary.family = FamilyName(family);
    const auto start = std::chrono::steady_clock::now();
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    // Whatever a reader writes beside what it reads -- the recent list,
    // the instance registry, settings -- lands in the scratch dir, never in
    // the user's own.
    ScopedEnv state("XDG_STATE_HOME", (scratch / "home" / "state").string());
    ScopedEnv config("XDG_CONFIG_HOME", (scratch / "home" / "config").string());
    // The readers log every refusal; a sweep of a hundred thousand of them
    // would be the whole of ctest's output. The case ids on stderr and the
    // last-case file are what a failure needs.
    const auto level = Logger::GetLogger()->level();
    Logger::GetLogger()->set_level(spdlog::level::off);
    struct RestoreLevel {
      spdlog::level::level_enum level;
      ~RestoreLevel() { Logger::GetLogger()->set_level(level); }
    } restore{level};
    WriteBytes(scratch / "outside.json", "{\"format\": \"outside\"}\n");
    lastCase = BuildDir() /
               (std::string("reader-fuzz-last-case.") + FamilyName(family) +
                (shards > 1 ? "-" + std::to_string(shard + 1) : std::string()));
    std::vector<Input> corpus;
    try {
      corpus = BuildCorpus(family, scratch);
    } catch (const std::exception& e) {
      Fail(FamilyName(family), e.what());
      return summary;
    }
    if (IsDocFamily(family)) {
      // LOBOLE_READER_FUZZ_ONLY=<text>: just the inputs whose name holds it
      // (tools/reader-fuzz.py, and a case being chased).
      const char* only = std::getenv("LOBOLE_READER_FUZZ_ONLY");
      for (const Input& in : corpus) {
        if (only != nullptr && in.corpus.find(only) == std::string::npos)
          continue;
        if (shards > 1 &&
            mutate::Hash(in.corpus) % static_cast<uint64_t>(shards) !=
                static_cast<uint64_t>(shard))
          continue;
        // The fixed sample reads a sixth of the corpus -- each input costs a
        // load and a save before its first case -- and always the inputs
        // that carry the side files (a saved result, a motion result, saved
        // projections) and the project.
        if (!every && only == nullptr && mutate::Hash(in.corpus) % 6 != 0 &&
            in.corpus.find("v153") == std::string::npos &&
            in.corpus.find("v87-") == std::string::npos &&
            in.corpus.find("v162") == std::string::npos &&
            in.corpus.find("v17-full") == std::string::npos &&
            family != Family::Project)
          continue;
        RunDocument(in);
      }
    } else {
      RunOther();
    }
    summary.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::error_code ec;
    fs::remove_all(scratch, ec);
    return summary;
  }

  // The unmutated input: what its round trip normalises away, and its
  // counts.
  struct Baseline {
    std::map<std::string, long> lost; // shapes the round trip loses, net
    std::map<std::string, size_t> counts;
    std::map<std::string, std::string> saved;
    double seconds = 0.0; // the read, and a read that rebuilds the geometry
  };

  fs::path SaveTarget(const Input& in, const std::string& name) const {
    return scratch / name /
           (in.tree ? std::string("doc.dsgn") : in.path.filename().string());
  }

  void RunDocument(const Input& in) {
    Note(in.corpus + ":original");
    const auto t0 = std::chrono::steady_clock::now();
    Loaded orig = Read(in.path);
    Baseline base;
    base.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    if (!orig.outcome.loaded) {
      Fail(in.corpus,
           "the unmutated input does not load: " + orig.outcome.message);
      return;
    }
    if (!orig.outcome.drops.Empty())
      Fail(in.corpus, "the unmutated input loads with drops: " +
                          orig.outcome.drops.Summary());
    base.counts = orig.outcome.counts;
    {
      const fs::path o1 = SaveTarget(in, "o1");
      fs::remove_all(o1.parent_path());
      fs::create_directories(o1.parent_path());
      std::string error;
      if (!orig.save(o1, &error)) {
        Fail(in.corpus, "the unmutated input does not save: " + error);
        return;
      }
      base.lost = NetLoss(LeavesOf(in.path, "doc"), LeavesOf(o1, "doc"));
      base.saved = Snapshot(o1);
    }
    orig = {};
    const std::vector<Case> cases = Cases(in);
    // A case that damages the baked geometry rebuilds: its budget is the
    // rebuild's, timed once on a copy without the blob.
    if (in.tree && !cases.empty()) {
      const fs::path bare = scratch / "bare" / in.path.filename();
      fs::remove_all(bare.parent_path());
      fs::create_directories(bare.parent_path());
      fs::copy(in.path, bare, fs::copy_options::recursive);
      std::vector<fs::path> blobs;
      for (const auto& e : fs::recursive_directory_iterator(bare))
        if (e.is_directory() && e.path().filename() == "geometry")
          blobs.push_back(e.path());
      for (const fs::path& b : blobs)
        fs::remove_all(b);
      const auto r0 = std::chrono::steady_clock::now();
      (void)Read(bare);
      base.seconds = std::max(
          base.seconds,
          std::chrono::duration<double>(std::chrono::steady_clock::now() - r0)
              .count());
      fs::remove_all(bare.parent_path());
    }
    // The work copy the cases damage, restored after each.
    fs::path work = in.path;
    if (in.tree) {
      work = scratch / "work" / in.path.filename();
      fs::remove_all(work.parent_path());
      fs::create_directories(work.parent_path());
      fs::copy(in.path, work, fs::copy_options::recursive);
    } else {
      work = scratch / "work" / in.path.filename();
      fs::create_directories(work.parent_path());
    }
    // An input whose read takes a quarter second or more (a modeled thread,
    // a worm, a text sketch: most of its damaged cases rebuild it) runs an
    // eighth of its cases even in the full sweep, and every operator at
    // least once: its hundreds of rebuilds were most of the sweep's time.
    const bool heavy = every && planted == nullptr && base.seconds >= 0.25;
    std::set<std::string> heavyOps;
    for (const Case& c : cases) {
      if (limit != 0 && summary.cases >= limit)
        break;
      if (heavy && !heavyOps.insert(c.op).second && mutate::Hash(c.id) % 8 != 0)
        continue;
      RunCase(in, c, work, base);
    }
  }

  void
  Apply(const Case& c, const fs::path& work,
        std::vector<std::pair<std::string, std::optional<std::string>>>& undo) {
    for (const mutate::Edit& e : c.edits) {
      const fs::path p = work / e.rel;
      std::error_code ec;
      std::optional<std::string> before;
      if (fs::is_regular_file(p, ec))
        before = ReadBytes(p);
      undo.emplace_back(e.rel, before);
      fs::remove(p, ec);
      switch (e.kind) {
      case mutate::Edit::Kind::Write:
        WriteBytes(p, e.bytes);
        break;
      case mutate::Edit::Kind::Remove:
        break;
      case mutate::Edit::Kind::Symlink:
        fs::create_directories(p.parent_path(), ec);
        fs::create_symlink(e.target, p, ec);
        break;
      }
    }
  }

  void
  Restore(const fs::path& work,
          const std::vector<std::pair<std::string, std::optional<std::string>>>&
              undo) {
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
      const fs::path p = work / it->first;
      std::error_code ec;
      fs::remove(p, ec);
      if (it->second)
        WriteBytes(p, *it->second);
    }
  }

  // What a refusal must name: the damaged file, relative to what was read
  // -- or, for a file a directory operator removed or added, its directory
  // and its id.
  bool Names(const Case& c, const Input& in, const std::string& message) {
    if (!in.tree) {
      const std::string name = in.path.filename().string();
      return message.find(name) != std::string::npos &&
             message.size() > name.size() + 8;
    }
    for (const mutate::Edit& e : c.edits) {
      if (message.find(e.rel) != std::string::npos &&
          message.size() > e.rel.size() + 8)
        return true;
      const fs::path p(e.rel);
      const std::string dir = p.parent_path().generic_string();
      const std::string norm = NormFile(e.rel);
      const std::string id =
          norm.size() > dir.size() ? norm.substr(dir.size() + 1) : "";
      if (!dir.empty() && !id.empty() &&
          message.find(fs::path(dir).filename().string() + "/") !=
              std::string::npos &&
          message.find(id) != std::string::npos)
        return true;
      // Damage to a file another one names (an index's entry, a blob the
      // geometry index lists) may be refused at the file that names it.
      if (message.find("index.json") != std::string::npos &&
          (p.filename() == "index.json" || !dir.empty()))
        return true;
    }
    return false;
  }

  // Whether the reader reads `leaf`: the case's input with that one value
  // changed is refused, or saves to something other than `saved`.
  bool ReaderReads(const Input& in, const fs::path& work, const fs::path& saved,
                   const Leaf& leaf) {
    const fs::path copy = scratch / "perturbed" / work.filename();
    std::error_code ec;
    fs::remove_all(copy.parent_path(), ec);
    fs::create_directories(copy.parent_path(), ec);
    fs::copy(work, copy, fs::copy_options::recursive, ec);
    const fs::path file = in.tree ? copy / leaf.rel : copy;
    try {
      json j = json::parse(ReadBytes(file));
      json& v = j[json::json_pointer(leaf.pointer)];
      if (v.is_boolean())
        v = !v.get<bool>();
      else if (v.is_number_integer())
        v = v.get<int64_t>() + 7;
      else if (v.is_number())
        v = v.get<double>() * 2.0 + 7.0;
      else if (v.is_string())
        v = v.get<std::string>() + "x";
      WriteBytes(file, j.dump(2) + "\n");
    } catch (const std::exception&) {
      return false;
    }
    Loaded l = Read(copy);
    if (std::getenv("LOBOLE_READER_FUZZ_DEBUG"))
      std::cerr << "perturbed read " << l.outcome.loaded << " "
                << l.outcome.message << "\n";
    if (!l.outcome.loaded)
      return true;
    const fs::path s3 = SaveTarget(in, "s3");
    fs::remove_all(s3.parent_path(), ec);
    fs::create_directories(s3.parent_path(), ec);
    std::string error;
    if (!l.save(s3, &error))
      return true;
    const std::string diff = FirstDifference(WithoutBlobs(Snapshot(s3)),
                                             WithoutBlobs(Snapshot(saved)));
    if (std::getenv("LOBOLE_READER_FUZZ_DEBUG"))
      std::cerr << "perturbed " << leaf.pointer << ": " << diff << "\n";
    return !diff.empty();
  }

  void RunCase(const Input& in, const Case& c, const fs::path& work,
               const Baseline& base) {
    summary.cases++;
    summary.perOperator[c.op]++;
    std::vector<std::pair<std::string, std::optional<std::string>>> undo;
    if (in.tree)
      Apply(c, work, undo);
    else
      WriteBytes(work, c.bytes);
    Note(c.id);
    const auto t0 = std::chrono::steady_clock::now();
    Loaded l = Read(work);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    if (took > summary.slowest) {
      summary.slowest = took;
      summary.slowestId = c.id;
    }
    const double budget = std::max(Budget(), 4.0 * base.seconds);
    if (took > budget)
      Fail(c.id, "the read took " + std::to_string(took) +
                     " s, past its budget of " + std::to_string(budget) + " s");
    if (!l.outcome.loaded) {
      summary.refused++;
      if (MeaningKept(c) && c.op != "reorder_keys")
        Fail(c.id, "damage to a derived file refused the document: " +
                       l.outcome.message);
      else if (c.op == "reorder_keys")
        Fail(c.id, "keys in another order refused the document: " +
                       l.outcome.message);
      else if (!Names(c, in, l.outcome.message))
        Fail(c.id, "the refusal does not name the file: \"" +
                       l.outcome.message + "\"");
    } else {
      summary.loaded++;
      CheckLoaded(in, c, work, base, l);
    }
    if (in.tree)
      Restore(work, undo);
  }

  void CheckLoaded(const Input& in, const Case& c, const fs::path& work,
                   const Baseline& base, Loaded& l) {
    const readctx::LoadReport drops = l.outcome.drops; // l is let go below
    // Counts: what the load kept must be what the original had, unless
    // the load said it dropped something.
    for (const auto& [what, n] : base.counts) {
      if ((what == "bodies" || what == "failed") && !MeaningKept(c))
        continue;
      // A deleted or retyped key can leave a valid document that simply
      // has fewer items: an absent `notes` is every drawing before v66.
      // No reader can tell that from damage, so it is not held to the count.
      if (c.op == "delete_key" || c.op == "retype")
        continue;
      // Requirements are found by their files, which no index lists: one
      // removed is a project without it, as one deleted on purpose is.
      if (c.op == "rm_file" &&
          std::any_of(c.edits.begin(), c.edits.end(),
                      [](const mutate::Edit& e) {
                        return e.rel.find("requirements/") != std::string::npos;
                      }))
        continue;
      const auto it = l.outcome.counts.find(what);
      const size_t got = it == l.outcome.counts.end() ? 0 : it->second;
      if (got != n && !drops.Lossy())
        Fail(c.id, "loaded with " + std::to_string(got) + " " + what +
                       " where the original has " + std::to_string(n) +
                       ", and reported no drop");
    }
    // Round trip.
    const fs::path s1 = SaveTarget(in, "s1");
    fs::remove_all(s1.parent_path());
    fs::create_directories(s1.parent_path());
    std::string error;
    if (!l.save(s1, &error)) {
      Fail(c.id, "loaded, then would not save: " + error);
      return;
    }
    l = {};
    // What the case's input holds and its save does not, past what the
    // unmutated input's round trip loses anyway (counted by shape, so a
    // leaf that moved with its item, or whose item's id changed, still
    // matches the loss it is).
    const Leaves given = LeavesOf(work, "doc"), saved = LeavesOf(s1, "doc");
    const Leaves lost = Lost(given, saved);
    std::map<std::string, long> excess = NetLoss(given, saved);
    for (auto& [shape, n] : excess) {
      const auto it = base.lost.find(shape);
      n -= it == base.lost.end() ? 0 : it->second;
    }
    if (std::getenv("LOBOLE_READER_FUZZ_DEBUG"))
      for (const auto& [key, leaf] : lost)
        std::cerr << "lost " << key << " shape " << leaf.shape << " excess "
                  << excess[leaf.shape] << "\n";
    std::vector<std::string> unreported;
    size_t checks = 0; // each is a load and a save: a few per case
    for (const auto& [key, leaf] : lost) {
      if (excess[leaf.shape] <= 0)
        continue;
      const std::string file = key.substr(0, key.find('#'));
      const std::string ptr = key.substr(key.find('#') + 1);
      const bool covered = std::any_of(
          drops.drops.begin(), drops.drops.end(), [&](const readctx::Drop& d) {
            const std::string df =
                in.tree ? NormFile(d.path) : std::string("doc");
            return (df == file || d.path.empty()) && ptr.starts_with(d.pointer);
          });
      if (covered)
        continue;
      // A value the writer leaves out because it is the default is not
      // lost: the reader READ it. Changed, it changes what loads (or is
      // refused); a value the reader ignores changes nothing.
      if (checks++ < 16 && ReaderReads(in, work, s1, leaf))
        continue;
      unreported.push_back(key);
    }
    if (!unreported.empty()) {
      std::string list;
      for (size_t i = 0; i < unreported.size() && i < 4; i++)
        list += (i ? ", " : "") + unreported[i];
      Fail(c.id, "loaded and saved without " +
                     std::to_string(unreported.size()) +
                     " value(s) it was given, reporting none: " + list);
    }
    if (c.op == "reorder_keys") {
      const std::string diff =
          FirstDifference(WithoutBlobs(Snapshot(s1)), WithoutBlobs(base.saved));
      if (!diff.empty())
        Fail(c.id, "keys in another order did not load identically: " + diff);
    }
    // Idempotence: the saved copy loads and saves to the same bytes.
    Loaded again = Read(s1);
    if (!again.outcome.loaded) {
      Fail(c.id, "its own save does not load: " + again.outcome.message);
      return;
    }
    const fs::path s2 = SaveTarget(in, "s2");
    fs::remove_all(s2.parent_path());
    fs::create_directories(s2.parent_path());
    if (!again.save(s2, &error)) {
      Fail(c.id, "its own save loaded, then would not save: " + error);
      return;
    }
    again = {};
    const std::string diff =
        FirstDifference(WithoutBlobs(Snapshot(s1)), WithoutBlobs(Snapshot(s2)));
    if (!diff.empty())
      Fail(c.id, "a second save differs from the first: " + diff);
  }

  // A family outside the documents: its fixture tree, how its cases are
  // made, and what its reader must do with one.
  struct OtherSetup {
    fs::path fixture;
    std::string corpus;
    mutate::TreeOptions topt;
    std::function<std::string(const fs::path&, const Case&)> check;
    bool skip = false;
  };
  OtherSetup SetupOther();
  void RunOther();
};

Sweep::Sweep(Family family, bool every, uint64_t seed, int shard, int shards)
    : m(new Impl{family,
                 every,
                 seed,
                 ScratchRoot(std::string(FamilyName(family)) +
                             (shards > 1 ? "-" + std::to_string(shard) : "")),
                 {},
                 {}}) {
  m->shard = shard;
  m->shards = shards;
  if (shards > 1)
    m->summary.family = std::string(FamilyName(family)) + " " +
                        std::to_string(shard + 1) + "/" +
                        std::to_string(shards);
}

Sweep::~Sweep() { delete m; }

Summary Sweep::Run() { return m->Run(); }

// The readers outside a document: each family builds a small fixture tree,
// damages each file of it (and the tree), and runs its reader on a fresh
// copy. A reader here never refuses the start of the app; it keeps going
// past a bad file and says which (settings set the file aside with a note,
// a profile store names the file it could not read).
Sweep::Impl::OtherSetup Sweep::Impl::SetupOther() {
  OtherSetup o;
  o.fixture = scratch / "fixture";
  const fs::path& fixture = o.fixture;
  fs::create_directories(fixture);
  // What the family's reader does with one tree; "" when every rule held.
  auto& check = o.check;
  mutate::TreeOptions& topt = o.topt;
  topt.file = FileOptions();
  topt.file.perOperator = every ? 4 : 2;
  topt.binary = topt.file;
  topt.binary.binary = true;
  topt.everyFile = true;
  std::string& corpus = o.corpus;
  corpus = std::string(FamilyName(family)) + "/fixture";

  switch (family) {
  case Family::Settings: {
    // Every key, a keymap and a remembered window.
    settings::Settings s;
    s.uiFontScale = 1.2f;
    s.ssao = "full";
    s.gitUserName = "Fuzz";
    s.window = {10, 20, 800, 600, true};
    s.keys.Set("view.fit", "F");
    json j = settings::ToJson(s);
    j["from_a_newer_build"] = {{"kept", true}};
    WriteBytes(fixture / "config" / "lobole" / "settings.json",
               j.dump(2) + "\n");
    check = [](const fs::path& w, const Case&) -> std::string {
      ScopedEnv config("XDG_CONFIG_HOME", (w / "config").string());
      const fs::path file = w / "config" / "lobole" / "settings.json";
      const bool parses = [&] {
        try {
          return json::parse(ReadBytes(file)).is_object();
        } catch (const std::exception&) {
          return false;
        }
      }();
      const bool existed = fs::exists(file);
      std::string note;
      settings::Settings loaded;
      try {
        loaded = settings::Load(&note);
      } catch (const std::exception& e) {
        return std::string("Load threw: ") + e.what();
      }
      if (existed && !parses && note.find("settings") == std::string::npos)
        return "an unreadable settings file left no note naming it: \"" + note +
               "\"";
      // What loaded is a value the settings accept.
      settings::Settings again;
      std::string error;
      if (!settings::ApplyJson(again, settings::ToJson(loaded), &error))
        return "what loaded does not apply again: " + error;
      return "";
    };
    break;
  }
  case Family::Profiles: {
    cam::Machine a;
    a.name = "Shapeoko 4";
    a.travelMax = {800.0, 800.0, 100.0};
    cam::Machine b;
    b.name = "Big router";
    b.post = "linuxcnc";
    std::string error;
    Require(DocumentIO::SaveMachine((fixture / "machines").string(), a, &error),
            "a machine does not save: " + error);
    Require(DocumentIO::SaveMachine((fixture / "machines").string(), b, &error),
            "a machine does not save: " + error);
    auto printed =
        DocumentIO::Load((GoldenDir() / "v169-print-profile.json").string());
    Require(printed.has_value() && (*printed)->PrintProcess() &&
                (*printed)->PrintProcess()->profile,
            "v169 carries no print profile");
    Require(DocumentIO::SavePrintProfile((fixture / "print_profiles").string(),
                                         *(*printed)->PrintProcess()->profile,
                                         &error),
            "a print profile does not save: " + error);
    check = [](const fs::path& w, const Case&) -> std::string {
      for (const char* store : {"machines", "print_profiles"}) {
        const fs::path dir = w / store;
        size_t files = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec))
          files += e.is_regular_file() && e.path().extension() == ".json";
        std::string problems;
        size_t loaded = 0;
        try {
          if (std::string(store) == "machines") {
            for (const cam::Machine& m :
                 DocumentIO::LoadMachines(dir.string(), &problems)) {
              loaded++;
              // What loaded reads back as itself.
              const json j = DocumentIO::MachineToJson(m);
              if (DocumentIO::MachineToJson(DocumentIO::MachineFromJson(j)) !=
                  j)
                return "machine '" + m.name + "' does not read back as itself";
            }
          } else {
            for (const printfit::Profile& p :
                 DocumentIO::LoadPrintProfiles(dir.string(), &problems)) {
              loaded++;
              const json j = DocumentIO::PrintProfileToJson(p);
              if (DocumentIO::PrintProfileToJson(
                      DocumentIO::PrintProfileFromJson(j)) != j)
                return "print profile '" + p.name +
                       "' does not read back as itself";
            }
          }
        } catch (const std::exception& e) {
          return std::string("loading the ") + store + " threw: " + e.what();
        }
        // Every file loaded or was named as not loading.
        size_t named = 0;
        for (const auto& e : fs::directory_iterator(dir, ec))
          if (e.path().extension() == ".json" &&
              problems.find(e.path().filename().string() + ":") !=
                  std::string::npos)
            named++;
        if (loaded + named != files)
          return std::string(store) + ": " + std::to_string(files) +
                 " files, " + std::to_string(loaded) + " loaded and " +
                 std::to_string(named) + " named as unreadable (\"" + problems +
                 "\")";
      }
      return "";
    };
    break;
  }
  case Family::CrashReports: {
    // A dead session's marker, its crash file and log, an earlier report,
    // and an autosave it names (inside the fixture).
    const fs::path state = fixture / "state";
    const std::string stem = "20261023-120000-4242";
    const std::string old = "20261022-110000-4141";
    WriteBytes(fixture / "project.dsgn.autosave", "{}\n");
    WriteBytes(state / "logs" / (stem + ".log"), "line one\nline two\n");
    const json marker = {
        {"stem", stem},
        {"version", "0.1.0"},
        {"build", "abc, Release"},
        {"mode", "windowed"},
        {"os", "Linux"},
        {"pid", 4242},
        {"started", "2026-10-23 12:00:00"},
        {"started_epoch", 1792800000},
        {"started_ms", int64_t{1792800000000}},
        {"log", (state / "logs" / (stem + ".log")).string()},
        {"project", (fixture / "project.dsgn").string()},
        {"autosaves",
         json::array({(fixture / "project.dsgn.autosave").string()})}};
    WriteBytes(state / "sessions" / (stem + ".json"), marker.dump(2) + "\n");
    WriteBytes(state / "sessions" / (stem + ".crash"),
               "reason: SIGSEGV\nframe 0\n");
    const json report = {{"stem", old},
                         {"reason", "crashed: SIGSEGV"},
                         {"crashed", true},
                         {"started", "2026-10-22 11:00:00"},
                         {"mode", "windowed"},
                         {"version", "0.1.0"},
                         {"build", "abc"},
                         {"os", "Linux"},
                         {"project", ""},
                         {"autosaves", json::array()},
                         {"started_epoch", 1792700000},
                         {"started_ms", int64_t{1792700000000}},
                         {"first_frame", false}};
    WriteBytes(state / "crashes" / old / "report.json", report.dump(2) + "\n");
    check = [fixture](const fs::path& w, const Case&) -> std::string {
      const std::set<std::string> before = FilesUnder(w);
      try {
        // The paths the fixture names point into the fixture; a case reads
        // its own copy, so they are redirected there.
        for (const auto& e : fs::recursive_directory_iterator(w)) {
          if (!e.is_regular_file() || e.path().extension() != ".json")
            continue;
          const std::string text = ReadBytes(e.path());
          const std::string from = fixture.string(), to = w.string();
          std::string out;
          size_t last = 0;
          for (size_t at = text.find(from); at != std::string::npos;
               at = text.find(from, last)) {
            out.append(text, last, at - last).append(to);
            last = at + from.size();
          }
          out.append(text, last);
          WriteBytes(e.path(), out);
        }
        const auto collected = crash::CollectDeadSessions(w / "state");
        for (const crash::Report& r : collected)
          if (r.dir.lexically_relative(w / "state" / "crashes").empty() ||
              r.dir.lexically_relative(w / "state" / "crashes")
                  .string()
                  .starts_with(".."))
            return "a report went outside the state dir: " + r.dir.string();
        (void)crash::ListReports(w / "state");
        (void)crash::EarlyDeathsInARow(w / "state");
      } catch (const std::exception& e) {
        return std::string("collecting the reports threw: ") + e.what();
      }
      // Nothing it copied came from outside the fixture's copy: every new
      // file is a report's.
      for (const std::string& f : FilesUnder(w))
        if (!before.contains(f) && !f.starts_with("state/crashes/"))
          return "it wrote " + f + " outside the reports";
      return "";
    };
    break;
  }
  case Family::Instances: {
    const fs::path dir = fixture / "state" / "lobole" / "instances";
    WriteBytes(dir / "8811.json", json({{"app", "lobole"},
                                        {"pid", static_cast<int>(::getpid())},
                                        {"port", 8811},
                                        {"mode", "headless"},
                                        {"started", 1792800000}})
                                          .dump(2) +
                                      "\n");
    WriteBytes(dir / "8812.json", json({{"app", "lobole"},
                                        {"pid", static_cast<int>(::getpid())},
                                        {"port", 8812},
                                        {"mode", "gui"},
                                        {"started", 1792800001}})
                                          .dump(2) +
                                      "\n");
    check = [](const fs::path& w, const Case&) -> std::string {
      ScopedEnv st("XDG_STATE_HOME", (w / "state").string());
      try {
        for (const remote::InstanceInfo& i : remote::LiveInstances())
          if (i.pid <= 0 || i.port == 0)
            return "an instance with pid " + std::to_string(i.pid) +
                   " and port " + std::to_string(i.port) + " reads as live";
      } catch (const std::exception& e) {
        return std::string("LiveInstances threw: ") + e.what();
      }
      return "";
    };
    break;
  }
  case Family::Recovery: {
    // A container project with one sidecar per document (a flat project's
    // single sidecar is opened as the part file it is: the part families).
    {
      // The project family's container, with sidecars for two documents.
      Impl other{
          Family::Project, false, seed, scratch / "project-fixture", {}, {}};
      fs::create_directories(other.scratch);
      const std::vector<Input> project =
          BuildCorpus(Family::Project, other.scratch);
      Require(!project.empty(), "no project fixture");
      CopyTree(project.front().path, fixture / "box.dsgn");
      auto p = DocumentIO::Load((GoldenDir() / "v14-boolean.json").string());
      Require(p.has_value(), "v14 does not load");
      fs::create_directories(fixture / "box.dsgn.autosave.d" / "parts");
      fs::create_directories(fixture / "box.dsgn.autosave.d" / "assemblies");
      Require(DocumentIO::Save(**p, (fixture / "box.dsgn.autosave.d" / "parts" /
                                     "Plate.json")
                                        .string())
                  .has_value(),
              "a part sidecar does not save");
      auto a = DocumentIO::LoadAssembly(
          (GoldenDir() / "assembly" / "v87-motion-study.json").string());
      Require(a.has_value(), "v87 does not load");
      Require(DocumentIO::SaveAssembly(**a, (fixture / "box.dsgn.autosave.d" /
                                             "assemblies" / "Crank.json")
                                                .string())
                  .has_value(),
              "an assembly sidecar does not save");
    }
    // Only the sidecars are damaged: the projects are the other families'.
    topt.file.want = [this](const std::string& id) {
      return id.find(".autosave") != std::string::npos && Want(id);
    };
    topt.binary.want = topt.file.want;
    check = [](const fs::path& w, const Case& c) -> std::string {
      for (const char* project : {"box.dsgn"}) {
        ui::AppState state;
        std::string error;
        bool ok = false;
        try {
          ok = ui::RecoverProject(state, (w / project).string(), &error);
        } catch (const std::exception& e) {
          return std::string("RecoverProject threw: ") + e.what();
        }
        if (ok)
          continue;
        // A refusal names the sidecar it could not read.
        for (const mutate::Edit& e : c.edits) {
          const std::string name = fs::path(e.rel).filename().string();
          if (e.rel.starts_with(project) &&
              error.find(fs::path(name).stem().string()) == std::string::npos)
            return std::string("recovering ") + project +
                   " was refused without naming " + e.rel + ": \"" + error +
                   "\"";
        }
      }
      return "";
    };
    break;
  }
  case Family::Vault: {
    if (!GitVault::Available()) {
      o.skip = true; // nothing to run here; not a failure
      return o;
    }
    auto d = DocumentIO::Load((GoldenDir() / "v17-full.json").string());
    Require(d.has_value(), "v17 does not load");
    const fs::path repo = fixture / "vault.dsgn";
    Require(DocumentIO::Save(**d, repo.string()).has_value(),
            "the part does not save");
    Require(GitVault::Init(repo.string()), "git init failed");
    std::string error;
    Require(GitVault::CommitAll(repo.string(), "first", &error),
            "commit failed: " + error);
    (*d)->GetFeature(0)->SetName("renamed");
    Require(DocumentIO::Save(**d, repo.string()).has_value(),
            "the part does not save twice");
    Require(GitVault::CommitAll(repo.string(), "second", &error),
            "commit failed: " + error);
    corpus = "vault/fixture";
    // The project's own files are damaged, never git's. Every case costs
    // a commit, an extract and a load (six git processes), so this family
    // runs a few operators -- what changes what git hands the readers --
    // and leaves the rest of the damage to the part-dir family, which reads
    // these same files case by case.
    topt.file.want = [this](const std::string& id) {
      if (id.find("/.git") != std::string::npos)
        return false;
      static const char* const kOps[] = {
          ":truncate:",     ":flip:",      ":delete_key:",
          ":replace_word:", ":json_liar:", ":rm_file:",
          ":dup_file:",     ":unlist:",    ":empty_file:"};
      return std::any_of(std::begin(kOps), std::end(kOps),
                         [&](const char* op) {
                           return id.find(op) != std::string::npos;
                         }) &&
             Want(id);
    };
    topt.binary.want = topt.file.want;
    topt.file.perOperator = 1;
    topt.binary.perOperator = 1;
    topt.file.truncateAll = topt.file.truncateEnds = 2;
    topt.file.truncateSpread = 2;
    topt.binary.truncateAll = topt.binary.truncateEnds = 1;
    topt.binary.truncateSpread = 1;
    topt.everyFile = false; // one item of each kind, as a tree's files are
    check = [this](const fs::path& w, const Case& c) -> std::string {
      const std::string project = (w / "vault.dsgn").string();
      std::string error;
      try {
        // The damage is committed: a version of the project that is
        // damaged, which a compare extracts and loads.
        if (!GitVault::CommitAll(project, "damaged", &error))
          return ""; // nothing to commit: git saw no change
        const auto log = GitVault::Log(project, 5);
        if (log.empty())
          return "the log of a repository with commits is empty";
        const fs::path out = scratch / "extracted";
        fs::remove_all(out);
        if (!GitVault::ExtractSubtree(project, log.front().hash, "",
                                      out.string(), &error))
          return "the damaged version does not extract: " + error;
        auto loaded = DocumentIO::Load(out.string());
        if (!loaded) {
          bool named = false;
          for (const mutate::Edit& e : c.edits) {
            const std::string rel = fs::path(e.rel)
                                        .lexically_relative("vault.dsgn")
                                        .generic_string();
            named = named || loaded.error().find(rel) != std::string::npos ||
                    loaded.error().find("index.json") != std::string::npos;
          }
          if (!named)
            return "the damaged version was refused without its file: \"" +
                   loaded.error() + "\"";
        }
        (void)ProjectMerge::Scan(project);
        (void)ProjectMerge::Verify(project);
      } catch (const std::exception& e) {
        return std::string("the vault threw: ") + e.what();
      }
      return "";
    };
    break;
  }
  case Family::Images: {
    const auto png = EncodeImage(false), jpg = EncodeImage(true);
    WriteBytes(fixture / "image.png", std::string(png.begin(), png.end()));
    WriteBytes(fixture / "image.jpg", std::string(jpg.begin(), jpg.end()));
    check = [](const fs::path& w, const Case&) -> std::string {
      for (const char* name : {"image.png", "image.jpg"}) {
        const std::string bytes = ReadBytes(w / name);
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
        int iw = 0, ih = 0, comp = 0;
        const bool info = stbi_info_from_memory(
            data, static_cast<int>(bytes.size()), &iw, &ih, &comp);
        if (info && ImageSizeProblem(iw, ih) != nullptr)
          continue; // refused before any decode, as the app refuses it
        int dw = 0, dh = 0, dc = 0;
        unsigned char* pixels = stbi_load_from_memory(
            data, static_cast<int>(bytes.size()), &dw, &dh, &dc, 4);
        if (pixels != nullptr) {
          if (!info || dw != iw || dh != ih)
            return std::string(name) + " decodes as " + std::to_string(dw) +
                   "x" + std::to_string(dh) + " where its header says " +
                   std::to_string(iw) + "x" + std::to_string(ih);
          stbi_image_free(pixels);
        }
      }
      return "";
    };
    // Binary: byte-level damage only.
    topt.file.binary = true;
    break;
  }
  default:
    throw std::runtime_error("no reader for this family");
  }
  return o;
}

void Sweep::Impl::RunOther() {
  OtherSetup o;
  try {
    o = SetupOther();
  } catch (const std::exception& e) {
    Fail(FamilyName(family), e.what());
    return;
  }
  if (o.skip) {
    summary.cases = 1;
    return;
  }
  const fs::path work = scratch / "case";
  for (mutate::TreeCase& tc : mutate::TreeCases(o.corpus, o.fixture, o.topt)) {
    Case c{std::move(tc.id), std::move(tc.op), std::move(tc.edits), {}};
    summary.cases++;
    summary.perOperator[c.op]++;
    CopyTree(o.fixture, work);
    std::vector<std::pair<std::string, std::optional<std::string>>> undo;
    Apply(c, work, undo);
    Note(c.id);
    const auto t0 = std::chrono::steady_clock::now();
    const std::string why = o.check(work, c);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    if (took > summary.slowest) {
      summary.slowest = took;
      summary.slowestId = c.id;
    }
    if (took > Budget() * (family == Family::Vault ? 4.0 : 1.0))
      Fail(c.id, "took " + std::to_string(took) + " s, past its budget");
    if (!why.empty())
      Fail(c.id, why);
    summary.loaded++;
  }
}

Outcome ReadAs(Family family, const fs::path& path) {
  if (IsDocFamily(family))
    return LoadAs(family, path).outcome;
  // Recovery is a reader with an answer of its own: the project, with
  // every autosave laid over it, or a refusal naming the one that does not
  // load.
  if (family == Family::Recovery) {
    Outcome o;
    ScopedEnv state("XDG_STATE_HOME", (path / "home-state").string());
    ScopedEnv config("XDG_CONFIG_HOME", (path / "home-config").string());
    ui::AppState app;
    try {
      o.loaded =
          ui::RecoverProject(app, (path / "box.dsgn").string(), &o.message);
    } catch (const std::exception& e) {
      o.message = std::string("threw: ") + e.what();
    }
    std::error_code ec;
    fs::remove_all(path / "home-state", ec);
    fs::remove_all(path / "home-config", ec);
    return o;
  }
  Sweep::Impl impl{
      family, false, 0, ScratchRoot(std::string("read-") + FamilyName(family)),
      {},     {}};
  std::error_code ec;
  fs::remove_all(impl.scratch, ec);
  fs::create_directories(impl.scratch, ec);
  Outcome o;
  try {
    const Sweep::Impl::OtherSetup setup = impl.SetupOther();
    o.message = setup.skip ? std::string() : setup.check(path, Case{});
    o.loaded = o.message.empty();
  } catch (const std::exception& e) {
    o.message = e.what();
  }
  fs::remove_all(impl.scratch, ec);
  return o;
}

bool IsDocumentFamily(Family f) { return IsDocFamily(f); }

Summary
RunPlanted(const fs::path& file, const Planted& planted, size_t limit,
           const std::vector<std::pair<std::string, std::string>>& cases) {
  Sweep::Impl impl{Family::PartFile, true, 0, ScratchRoot("planted"), {}, {}};
  impl.planted = &planted;
  impl.limit = limit;
  impl.given = &cases;
  impl.summary.family = "planted";
  fs::remove_all(impl.scratch);
  fs::create_directories(impl.scratch);
  impl.lastCase = impl.scratch / "last-case";
  const auto level = Logger::GetLogger()->level();
  Logger::GetLogger()->set_level(spdlog::level::off);
  impl.RunDocument({"part/" + file.filename().string(), file, false, {}});
  Logger::GetLogger()->set_level(level);
  std::error_code ec;
  fs::remove_all(impl.scratch, ec);
  return impl.summary;
}

std::optional<Family> WriteCase(const std::string& id, uint64_t seed,
                                const fs::path& out, std::string* error) {
  const auto family = FamilyFromName(id.substr(0, id.find('/')));
  if (!family) {
    if (error)
      *error =
          "no reader family is called '" + id.substr(0, id.find('/')) + "'";
    return std::nullopt;
  }
  Sweep::Impl impl{
      *family, true,
      seed,    ScratchRoot(std::string("case-") + FamilyName(*family)),
      {},      {}};
  impl.only = id;
  fs::remove_all(impl.scratch);
  fs::create_directories(impl.scratch);
  WriteBytes(impl.scratch / "outside.json", "{\"format\": \"outside\"}\n");
  if (!IsDocFamily(*family)) {
    Sweep::Impl::OtherSetup o = impl.SetupOther();
    for (mutate::TreeCase& tc :
         mutate::TreeCases(o.corpus, o.fixture, o.topt)) {
      if (tc.id != id)
        continue;
      CopyTree(o.fixture, out);
      Case c{tc.id, tc.op, tc.edits, {}};
      std::vector<std::pair<std::string, std::optional<std::string>>> undo;
      impl.Apply(c, out, undo);
      if (error)
        *error = o.check(out, c); // what the reader made of it
      std::error_code ec;
      fs::remove_all(impl.scratch, ec);
      return family;
    }
    if (error)
      *error = "no case has the id '" + id + "'";
    return std::nullopt;
  }
  for (const Input& in : BuildCorpus(*family, impl.scratch)) {
    if (!id.starts_with(in.corpus))
      continue;
    for (const Case& c : impl.Cases(in)) {
      if (c.id != id)
        continue;
      fs::remove_all(out);
      if (in.tree) {
        fs::copy(in.path, out, fs::copy_options::recursive);
        std::vector<std::pair<std::string, std::optional<std::string>>> undo;
        impl.Apply(c, out, undo);
      } else {
        WriteBytes(out, c.bytes);
      }
      std::error_code ec;
      fs::remove_all(impl.scratch, ec);
      return family;
    }
  }
  if (error)
    *error = "no case has the id '" + id + "'";
  return std::nullopt;
}

} // namespace readerfuzz
