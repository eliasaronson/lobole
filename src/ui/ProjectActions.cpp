// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ProjectActions.h"
#include "render/ClipPlanes.h" // kMaxClipPlanes: the sections a project holds
#include "ui/DerivedActions.h" // RefreshLinks (the open and save gates)
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"
#include "ui/RendererSync.h"
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"

#include "cad/CADKernel.h" // ArmOccSignalHandlers
#include "core/Parallel.h"
#include "core/Units.h"
#include "core/io/GeometryCache.h"
#include "core/io/ProjectPaths.h"
#include "core/io/ReadContext.h"
#include "ui/AppActionsInternal.h" // SharedBody / PartRenderCacheStore
#include "ui/CamPanel.h"           // RefreshCamToolpaths (job undo)
#include "ui/CamTool.h"            // RunCamVerification (job undo)
#include "ui/JobActions.h"

#include "imgui.h" // IsAnyItemActive (the undo guard coalesces a drag)         // JobBodies / JobRenderVersion (a job tab)
#include <chrono>
#include <map>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <unistd.h> // getpid
#endif

#include "cad/CADMaterials.h"
#include "cad/CADMeshConverter.h"
#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/ElementHistory.h" // topo::ResolveEdgeOnBody (blend edge picks)
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/Requirements.h"
#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingExport.h"
#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/BooleanFeature.h"
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/FrameFeature.h"
#include "core/fem/FemTopo.h"
#include "core/io/Autosaver.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectStamp.h"
#include "core/io/RecentProjects.h"
#include "core/io/Serialization.h"
#include "render/Camera.h"
#include "render/FieldRenderer.h"
#include "render/IBLTextures.h"
#include "render/MeshRenderer.h"
#include "render/OutlineRenderer.h"
#include "render/RayTracingRenderer.h"
#include "render/SketchRenderer.h"

#include "core/features/DraftFeature.h"
#include "core/features/LoftFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/PipeFeature.h"
#include "core/features/RefineFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/RibFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/features/SweepFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/sketch/SketchProfile.h"
#include "ui/AssemblyActions.h"
#include "ui/CamPlayback.h"   // SyncCamPlayback (SyncRenderersToDocument)
#include "ui/FastenerTool.h"  // SyncFastenerGhost (SyncRenderersToDocument)
#include "ui/FeatureEditor.h" // PickSlot (thread face slot)
#include "ui/Placement.h"
#include "ui/PreviewFeature.h"
#include "ui/SelectionPipeline.h" // ResolveFaceItem (Hole select-first)
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // SketchUVToScreen
#include <nlohmann/json.hpp>

#include "thirdparty/stb_image.h" // stbi_info_from_memory (impl in IBL.cpp)

#include <glm/gtc/matrix_transform.hpp> // glm::translate
#include <glm/gtc/quaternion.hpp>       // glm::mat4_cast

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <unordered_map>

// Opening, saving, autosaving and recovering a PROJECT: the multi-slot save
// and load, sidecars, the on-disk stamp that catches an external edit, git
// remotes, and comparing a version's geometry against the working tree.
// Declared in ui/ProjectActions.h; the layer is described in
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

namespace {

// The project's requirements, one file each under `requirements/`
// (docs/requirements/README.md#saved-form). A file no longer wanted is
// removed; an unchanged one is not rewritten.
bool WriteRequirements(const AppState& state, const std::filesystem::path& dir,
                       std::string* error) {
  namespace fs = std::filesystem;
  const fs::path rdir = dir / "requirements";
  std::error_code ec;
  if (!state.requirements.empty())
    fs::create_directories(rdir, ec);
  std::set<std::string> wanted;
  for (const req::Requirement& r : state.requirements) {
    const std::string file =
        std::to_string(r.id) + "-" + req::Slug(r.name) + ".json";
    if (!DocumentIO::WriteTextAtomic(rdir / file, req::ToJson(r).dump(2) + "\n",
                                     error))
      return false;
    wanted.insert(file);
  }
  for (const auto& entry : fs::directory_iterator(rdir, ec))
    if (entry.path().extension() == ".json" &&
        !wanted.contains(entry.path().filename().string()))
      fs::remove(entry.path(), ec);
  if (state.requirements.empty())
    fs::remove(rdir, ec); // only when empty: a stray file keeps it
  return true;
}

std::vector<req::Requirement>
ReadRequirements(const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  std::vector<req::Requirement> out;
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(dir / "requirements", ec))
    if (entry.path().extension() == ".json")
      files.push_back(entry.path());
  // By name: a directory lists in the filesystem's order, which differs
  // between machines, and a duplicate id is refused at whichever file came
  // second.
  std::sort(files.begin(), files.end());
  std::map<uint64_t, std::string> ids; // each id's file
  for (const fs::path& f : files) {
    const std::string name = "requirements/" + f.filename().string();
    readctx::InFile(name, [&] {
      std::ifstream in(f, std::ios::binary);
      if (!in)
        throw std::runtime_error("cannot be read");
      out.push_back(req::FromJson(
          readctx::ParseStrict(std::string((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>()))));
      if (const auto [it, added] = ids.emplace(out.back().id, name); !added)
        throw std::runtime_error(it->second + " also has id " +
                                 std::to_string(out.back().id));
    });
  }
  std::sort(out.begin(), out.end(),
            [](const req::Requirement& a, const req::Requirement& b) {
              return a.id < b.id;
            });
  return out;
}

// A lone part's file names no document id: it reopens with a fresh one, and
// its requirements are re-pointed at it (AdoptProject). They fold into it
// only when they name it, or none.
bool LoneRequirementsFit(const AppState& state) {
  for (const req::Requirement& r : state.requirements)
    if (r.measure.contains("document") &&
        (state.parts.empty() || r.Document() != state.parts[0].id))
      return false;
  return true;
}

} // namespace

// The declared clearances an assembly saved before v160 (`assertions/`)
// are clearance requirements now (docs/requirements/README.md
// #clearance-assertions): each is moved out of its assembly into the
// project's list, its floor the bound and its `why` the source. The last
// verdict does not come along: it was keyed on a bounding-box hash, not on
// the input hash a requirement's evidence names, so it reads unchecked.
void AdoptAssertionsAsRequirements(AppState& state) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].kind != DocKind::Assembly)
      continue;
    AssemblyDocument* a = SlotAssembly(state, i);
    if (a == nullptr || a->GetAssertions().empty())
      continue;
    for (const ClearanceAssertion& c : a->GetAssertions()) {
      req::Requirement r;
      r.id = state.nextRequirementId++;
      r.name = c.name;
      r.source = c.why;
      r.measure = {{"kind", "clearance"},
                   {"document", state.parts[i].id},
                   {"a", c.a},
                   {"b", c.b},
                   {"mode", c.mode}};
      r.bound.kind = "at_least";
      r.bound.value = c.minMm;
      state.requirements.push_back(std::move(r));
    }
    a->GetAssertions().clear();
    // Moved, and saved where they now live on the next save.
    state.projectMetaDirty = true;
  }
}

namespace {
// Imports made before the project had a home stage their .brep sidecars
// under the app state dir (ImportStepAsAssembly). A save gives them a home:
// copy each staged file into <project>/imported/ (keeping the per-import
// subfolder) and repoint the features, so the saved project is
// self-contained and the staging area gets reclaimed.
void AdoptStagedImports(AppState& state, const std::filesystem::path& dir) {
  namespace fs = std::filesystem;
  const fs::path staging = fs::path(Autosaver::StateDir()) / "import-staging";
  std::error_code ec;
  for (size_t i = 0; i < state.parts.size(); i++) {
    Document* doc = SlotDoc(state, i);
    if (!doc)
      continue;
    for (size_t f = 0; f < doc->FeatureCount(); f++) {
      auto* imp = dynamic_cast<ImportFeature*>(doc->GetFeature(f));
      if (!imp)
        continue;
      const fs::path src(imp->path);
      const fs::path rel = src.lexically_relative(staging);
      // The first COMPONENT, not the first two characters: `native()` is a
      // wstring on Windows, and "..foo" is not an escape from the staging dir.
      if (rel.empty() || *rel.begin() == "..")
        continue; // not staged: an original STEP, or already project-owned
      // A path from a shared project: `<staging>/x/../../anything` passes the
      // test above, and this moves the file it names, then deletes it. A
      // staged import is a plain subfolder and a plain file, nothing else.
      if (std::distance(rel.begin(), rel.end()) != 2 ||
          std::any_of(rel.begin(), rel.end(), [](const fs::path& part) {
            return part == "." || part == "..";
          })) {
        LOG_WARN("Not adopting import path '{}': not a staged file", imp->path);
        continue;
      }
      const fs::path dest = dir / "imported" / rel;
      fs::create_directories(dest.parent_path(), ec);
      ec.clear();
      if (!fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec) ||
          ec) {
        LOG_WARN("Could not adopt staged import '{}': {}", imp->path,
                 ec.message());
        ec.clear();
        continue;
      }
      imp->path = dest.string();
      fs::remove(src, ec);
      fs::remove(src.parent_path(), ec); // rmdir-if-empty; failure is fine
      ec.clear();
    }
  }
}
} // namespace

namespace {
bool SamePath(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty())
    return false;
  std::error_code ec;
  return std::filesystem::weakly_canonical(a, ec) ==
         std::filesystem::weakly_canonical(b, ec);
}
} // namespace

static bool SaveProjectUnguarded(AppState& state, const std::string& path,
                                 std::string* error,
                                 bool overwriteExternalChanges,
                                 bool overwriteLossy);

bool SaveWouldLoseDrops(const AppState& state, const std::string& path) {
  return state.openDrops.Lossy() && SamePath(path, state.openDropsPath);
}

bool SaveProject(AppState& state, const std::string& path, std::string* error,
                 bool overwriteExternalChanges, bool overwriteLossy) {
  // The gate (docs/modeling/derived.md#copies-that-keep-up): what is saved
  // is each part with its copies of the others up to date.
  RefreshLinks(state);
  const bool saved = SaveProjectUnguarded(
      state, path, error, overwriteExternalChanges, overwriteLossy);
  // Written over the project the drops came from: they are gone from it
  // now, and the log is where they are kept.
  if (saved && SamePath(path, state.openDropsPath) &&
      !state.openDrops.Empty()) {
    LOG_WARN("Saved {} over what its open could not keep:\n{}", path,
             state.openDrops.Summary());
    state.openDrops = {};
    state.openDropsPath.clear();
  }
  return saved;
}

static bool WriteProject(AppState& state, const std::string& path,
                         std::string* error);

std::string ProjectWriteTarget(const std::string& path) {
  namespace fs = std::filesystem;
  std::string p = path;
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
    p.pop_back();
  if (!p.ends_with(DocumentIO::kLegacyProjectExtension))
    return path;
  return fs::path(p).replace_extension(DocumentIO::kProjectExtension).string();
}

static bool SaveProjectUnguarded(AppState& state, const std::string& path,
                                 std::string* error,
                                 bool overwriteExternalChanges,
                                 bool overwriteLossy) {
  namespace fs = std::filesystem;
  // `.dsgn` is the extension before the rename: still opened, never written
  // (docs/versioning/README.md#the-dsgn-extension). Saving the project that
  // was opened from one writes `<name>.lob` beside it instead; any other
  // `.dsgn` would be a new one, and is refused.
  const std::string target = ProjectWriteTarget(path);
  const bool migrating = target != path;
  if (migrating) {
    if (!SamePath(path, state.projectPath)) {
      if (error)
        *error = "'" + path +
                 "' is a .dsgn, the project extension before the rename, "
                 "which is read but no longer written: save to '" +
                 target + "'";
      return false;
    }
    std::error_code ec;
    if (fs::exists(target, ec)) {
      if (error)
        *error = "this project was opened from '" + path +
                 "', which is saved as '" + target +
                 "' now -- and that already exists. Open it, or save this "
                 "project under another name";
      return false;
    }
  }
  // The open kept going past content it could not keep (a file its index
  // did not list, ...): saving back over that project would lose it for
  // good, so the save is refused, naming each, unless the caller says to
  // overwrite anyway. A Save As elsewhere is not this path, and goes on.
  if (!overwriteLossy && SaveWouldLoseDrops(state, path)) {
    state.lossySaveBlocked = true;
    if (error)
      *error = "this project opened without some of what is in it -- " +
               state.openDrops.Summary(/*lossyOnly=*/true) +
               " -- and saving over it would lose that for good: save a "
               "copy elsewhere, or overwrite anyway";
    return false;
  }
  // F8: the project moved under us. Writing now would replace whatever landed
  // there — a `git checkout` in the terminal panel, another editor, a second
  // instance — with what happens to be in memory, silently. Only guards a
  // save back to the path we opened; a Save As to a new home has nothing to
  // compare against.
  if (!overwriteExternalChanges && path == state.projectPath &&
      ProjectChangedOnDisk(state)) {
    state.diskChanged = true;
    state.diskChangeBlockedSave = true;
    if (error)
      *error = "the project changed on disk since you opened it — reload it, "
               "or save anyway to replace what is there";
    return false;
  }
  if (migrating) {
    // COPIED, not moved: the .lob starts as everything the .dsgn holds --
    // its git history, adopted imports, assets, baked geometry -- and the
    // write below lands over the copy. Moving .git out would leave the
    // .dsgn a project without its history; the .dsgn is never changed.
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
      fs::copy(path, target,
               fs::copy_options::recursive | fs::copy_options::copy_symlinks,
               ec);
      if (ec) {
        std::error_code rmEc;
        fs::remove_all(target, rmEc); // a half copy is no project
        if (error)
          *error = "could not copy '" + path + "' to '" + target +
                   "': " + ec.message() + ". Nothing was saved";
        return false;
      }
    }
    LOG_INFO("save: {} is saved as {} (the .dsgn is left as it was)", path,
             target);
  }
  return WriteProject(state, target, error);
}

// The write itself, to a target the checks above have passed.
static bool WriteProject(AppState& state, const std::string& path,
                         std::string* error) {
  namespace fs = std::filesystem;
  EnsureParts(state);
  // Directory-form targets can own import sidecars; adopt any still staged
  // before the write so the saved features point inside the project.
  if (DocumentIO::IsProjectFolderName(path) || fs::is_directory(path))
    AdoptStagedImports(state, fs::path(path));
  // A single part keeps the flat .lob form (index.json + features/). A lone
  // assembly, or any multi-document project, uses the project container.
  // Phase V: a single part IS the whole project, so its file carries the
  // PROJECT-scope variables. We fall back to the container form only when the
  // part also holds part-local variables (both scopes then need distinct homes
  // — flat has room for one `parameters` list).
  if (state.parts.size() == 1 && state.parts[0].kind == DocKind::Part &&
      state.document->GetParameters().empty() && state.tabFolders.empty() &&
      state.sections.empty() && state.parts[0].folder.empty() &&
      LoneRequirementsFit(state)) {
    // Fold the project scope into the doc for the write (they reload as project
    // scope), then restore the (empty) local table.
    std::vector<Parameter> localBackup = state.document->TakeParameters();
    auto catalogBackup = state.document->TakeCatalogs();
    state.document->SetCatalogsRaw(state.catalogs);
    state.document->SetParametersRaw(state.projectParameters);
    state.document->SetTablesRaw(state.projectTables);
    state.document->SetRequirementsRaw(req::ListToJson(state.requirements));
    state.document->SetTemplatesRaw(state.projectTemplates);
    auto r = DocumentIO::Save(*state.document, path);
    state.document->SetTablesRaw({});
    state.document->TakeRequirementsRaw();
    state.document->SetTemplatesRaw({});
    state.document->SetParametersRaw(std::move(localBackup));
    state.document->SetCatalogsRaw(catalogBackup);
    if (!r && error)
      *error = r.error();
    if (r)
      StampDiskState(state, path); // we just wrote it; disk and memory agree
    return r.has_value();
  }
  // A multi-document project is about to become a DIRECTORY. Writing it
  // under a .json name would work — and did: the W 170 S project saved as a
  // directory *named* w170s.json, in the full container form but WITHOUT
  // the git vault (which keys on .lob), silently dropping every save
  // message. Same format, different capability, decided by a filename —
  // refuse instead: .json stays the single-document interchange form.
  if (fs::path(path).extension() == ".json") {
    if (error)
      *error = "a multi-document project saves as a .lob directory "
               "(per-feature files + git history); .json is the "
               "single-document interchange form — save to '" +
               fs::path(path).stem().string() + ".lob'";
    return false;
  }
  // Multi-document: project.json (v2) + parts/<name>/ and assemblies/<name>/.
  try {
    const fs::path dir(path);
    nlohmann::json index;
    index["format"] = "designer-project";
    // Still 2: "sections" is an ADDITIVE key, and this envelope's precedent
    // (folders, project parameters — both added after v2 shipped) is that a
    // key a reader can ignore does not move the version. A bump here would
    // only invalidate every project file for nothing.
    index["version"] = 2;
    index["active"] = state.activePart;
    if (!state.catalogs.empty())
      index["catalogs"] = catalog::Serialize(state.catalogs);
    // Project tables: additive, like the parameters (read by lookup()).
    if (!state.projectTables.empty())
      index["tables"] = lookuptable::ToJson(state.projectTables);
    // The template library: additive, like the tables.
    if (!state.projectTemplates.empty())
      index["templates"] = tmpl::LibraryToJson(state.projectTemplates);
    // Phase V: project-scope variables live in the project envelope, beside
    // active/documents. Per-part index.json `parameters` are that part's LOCAL
    // scope (usually empty now that set_variable defaults to project).
    if (!state.projectParameters.empty()) {
      index["parameters"] = nlohmann::json::array();
      for (const Parameter& pv : state.projectParameters) {
        nlohmann::json pj = {{"name", pv.name}, {"expression", pv.expression}};
        if (!pv.comment.empty())
          pj["comment"] = pv.comment;
        index["parameters"].push_back(std::move(pj));
      }
    }
    // Folder registry (display order; may hold empty folders). Written only
    // when non-empty so pre-folder projects keep byte-identical indexes.
    if (!state.tabFolders.empty())
      index["folders"] = state.tabFolders;
    // Section cuts (I3 P3). PROJECT scope, not per-document: a cut is a view
    // of the whole project and applies on whichever tab you switch to, so
    // storing it per part would give the same cut several disagreeing copies.
    // Their presence is what forces this container form above — same rule
    // folders and project variables already follow.
    if (!state.sections.empty()) {
      index["sections"] = nlohmann::json::array();
      for (const AppState::SectionView& sec : state.sections) {
        nlohmann::json sj = {{"base_plane", sec.basePlane},
                             {"offset", sec.offset},
                             {"reverse", sec.reverse},
                             {"visible", sec.visible}};
        if (sec.planeId != 0)
          sj["plane_id"] = sec.planeId;
        if (sec.face.IsSet())
          sj["face"] = {{"feature_id", sec.face.featureId},
                        {"element", sec.face.name}};
        if (sec.tiltU != 0.0f)
          sj["tilt_u"] = sec.tiltU;
        if (sec.tiltV != 0.0f)
          sj["tilt_v"] = sec.tiltV;
        index["sections"].push_back(std::move(sj));
      }
    }
    index["documents"] = nlohmann::json::array();
    for (size_t i = 0; i < state.parts.size(); i++) {
      const PartSlot& slot = state.parts[i];
      const char* sub = slot.kind == DocKind::Assembly  ? "assemblies/"
                        : slot.kind == DocKind::Drawing ? "drawings/"
                        : slot.kind == DocKind::Job     ? "jobs/"
                                                        : "parts/";
      const char* type = slot.kind == DocKind::Assembly  ? "assembly"
                         : slot.kind == DocKind::Drawing ? "drawing"
                         : slot.kind == DocKind::Job     ? "job"
                                                         : "part";
      const std::string folder =
          std::string(sub) + projectpaths::DocFolder(slot.name, slot.id);
      const fs::path docDir = dir / folder;
      fs::create_directories(docDir);
      std::expected<void, std::string> r;
      switch (slot.kind) {
      case DocKind::Assembly:
        r = DocumentIO::SaveAssembly(*SlotAssembly(state, i), docDir.string());
        break;
      case DocKind::Drawing:
        r = DocumentIO::SaveDrawing(*SlotDrawing(state, i), docDir.string());
        break;
      case DocKind::Job:
        r = DocumentIO::SaveJob(*SlotJob(state, i), docDir.string());
        break;
      case DocKind::Part:
        r = DocumentIO::Save(*SlotDoc(state, i), docDir.string());
        break;
      }
      if (!r) {
        if (error)
          *error = r.error();
        return false;
      }
      nlohmann::json entry = {{"id", slot.id},
                              {"name", slot.name},
                              {"dir", folder},
                              {"type", type}};
      if (!slot.folder.empty()) // display grouping; omit at top level
        entry["folder"] = slot.folder;
      index["documents"].push_back(std::move(entry));
    }
    // Requirements (v160, docs/requirements/README.md#saved-form): one
    // file each under requirements/, so two people stating two of them, or
    // checking them on two branches, touch different files.
    if (!WriteRequirements(state, dir, error))
      return false;
    if (state.nextRequirementId > 1)
      index["requirements_next_id"] = state.nextRequirementId;
    std::ofstream out(dir / "project.json");
    if (!out) {
      if (error)
        *error = "cannot write project.json";
      return false;
    }
    out << index.dump(2) << "\n";
    // Re-stamp HERE, not only in MarkProjectSaved. A save writes the project,
    // so after it the disk is what we just put there — and leaving the stamp
    // stale meant a caller that saved WITHOUT marking (a script, a test, an
    // in-app step that saves twice) had its next save refused by F8's guard
    // as though someone else had written the project. The two were paired by
    // convention and nothing said so.
    StampDiskState(state, path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

namespace {

// Why a rebuilt part could not use its baked geometry, in the words the
// progress modal and the status line show.
std::string RebuildReason(DocumentIO::BakedGeometryOnDisk onDisk) {
  switch (onDisk) {
  case DocumentIO::BakedGeometryOnDisk::None:
    return "no saved geometry";
  case DocumentIO::BakedGeometryOnDisk::OlderFormat:
    return "the saved geometry is from an older build";
  case DocumentIO::BakedGeometryOnDisk::NewerFormat:
    return "the saved geometry is from a newer build";
  case DocumentIO::BakedGeometryOnDisk::Present:
    break;
  }
  return "the recipe changed since the geometry was saved";
}

// Did this load rebuild the part rather than adopt its baked bodies? A baked
// open executes only datums and counts every other feature cached; a rebuild
// starts from an empty cache.
bool WasRebuilt(const Document& doc) {
  const auto& stats = doc.LastRegenStats();
  return doc.FeatureCount() > 0 && stats.cached == 0 && stats.executed > 0;
}

// One document of a project, read into a slot. Pure: no AppState. Every
// refusal and every drop names its file relative to the project
// ("parts/stand/features/7-fillet.json"): the document's directory is this
// thread's context while it loads.
std::expected<PartSlot, std::string>
ReadSlot(const nlohmann::json& entry, size_t i,
         const std::filesystem::path& root, std::string* rebuiltReason,
         const DocumentIO::ProjectScope& scope) {
  const std::string name = entry.at("name").get<std::string>();
  const std::string rel = entry.at("dir").get<std::string>();
  auto inside = projectpaths::InsideProject(root, rel);
  if (!inside)
    return std::unexpected(readctx::Named(
        "document '" + name + "': " + inside.error(), "project.json"));
  const std::string dir = inside->string();
  const std::string type = entry.value("type", std::string("part"));
  // Named here, at the entry that names it: a load of a directory that is
  // not there would name only the file it then could not find in it. (A
  // document in the single-file form is a file, and loads as one.)
  std::error_code ec;
  if (!std::filesystem::is_directory(*inside, ec) &&
      !std::filesystem::is_regular_file(*inside, ec))
    return std::unexpected(readctx::Named("document '" + name + "': its dir '" +
                                              rel + "' is not there",
                                          "project.json"));
  // A single file names itself in what it refuses: the context is only
  // the directory it is in.
  const bool isFile = std::filesystem::is_regular_file(*inside, ec);
  const std::string parent =
      std::filesystem::path(rel).parent_path().generic_string();
  readctx::FileContext context(isFile ? (parent.empty() ? "" : parent + "/")
                               : rel.ends_with("/") ? rel
                                                    : rel + "/");
  PartSlot slot;
  slot.id = entry.value("id", static_cast<uint64_t>(i + 1)); // v1: 1..n
  slot.name = name;
  slot.folder = entry.value("folder", std::string{});
  if (type == "assembly") {
    slot.kind = DocKind::Assembly;
    auto r = DocumentIO::LoadAssembly(dir);
    if (!r)
      return std::unexpected(r.error());
    slot.assembly = std::move(*r);
  } else if (type == "drawing") {
    slot.kind = DocKind::Drawing;
    auto r = DocumentIO::LoadDrawing(dir);
    if (!r)
      return std::unexpected(r.error());
    slot.drawing = std::move(*r);
  } else if (type == "job") {
    slot.kind = DocKind::Job;
    auto r = DocumentIO::LoadJob(dir);
    if (!r)
      return std::unexpected(r.error());
    slot.job = std::move(*r);
  } else if (type == "part") {
    slot.kind = DocKind::Part;
    const auto onDisk = DocumentIO::InspectGeometry(dir);
    auto r = DocumentIO::Load(dir, scope);
    if (!r)
      return std::unexpected(r.error());
    if (WasRebuilt(**r))
      *rebuiltReason = RebuildReason(onDisk);
    // The scope lives only as long as the read; AdoptProject points the part
    // at the project's own (SyncProjectScope), whose regeneration then finds
    // every feature cached.
    (*r)->SetProjectSymbols(nullptr);
    (*r)->SetProjectParameters(nullptr);
    slot.doc = std::move(*r);
    slot.commands = std::make_unique<CommandStack>();
  } else {
    // CheckProjectIndex refused it already; never read as a part.
    return std::unexpected("document '" + name + "' has type '" + type + "'");
  }
  return slot;
}

// Each document's id once, a whole number above 0: two documents with one
// id are one document to every reference that names it.
void RefuseDocumentIds(const nlohmann::json& docs) {
  std::set<uint64_t> seen;
  for (const auto& d : docs) {
    if (!d.is_object() || !d.contains("id"))
      continue;
    const nlohmann::json& id = d.at("id");
    if (!readctx::IsWhole(id) || id.get<uint64_t>() == 0 ||
        !seen.insert(id.get<uint64_t>()).second)
      throw std::runtime_error("document id " + id.dump() +
                               (readctx::IsWhole(id) && id.get<uint64_t>()
                                    ? " is listed twice"
                                    : " is not a whole number above 0"));
  }
}

// project.json, checked whole BEFORE any document loads, so a refusal names
// the key and nothing half-loads: every key known (the envelope is v2, and
// a key this build does not know is refused like a document's), every
// document entry, section, parameter and folder of the shape it is written
// in.
void CheckProjectIndex(const nlohmann::json& index) {
  readctx::RefuseUnknownKeys(index,
                             {"format", "version", "active", "catalogs",
                              "tables", "templates", "parameters", "folders",
                              "sections", "documents", "parts",
                              "requirements_next_id"},
                             "project");
  if (index.contains("format") &&
      index.at("format") != nlohmann::json("designer-project"))
    throw std::runtime_error("not a Lobole project: its `format` is " +
                             index.at("format").dump());
  const nlohmann::json version = index.value("version", nlohmann::json(1));
  if (!version.is_number_integer() || version.get<int64_t>() < 1)
    throw std::runtime_error("project version " + version.dump() +
                             " is not a version (1 or 2)");
  if (version.get<int64_t>() > 2)
    throw std::runtime_error("project version " + version.dump() +
                             " is newer than this build reads (2)");
  if (index.contains("active") && !readctx::IsWhole(index.at("active")))
    throw std::runtime_error("`active` is " + index.at("active").dump() +
                             ", not the index of a document");
  const auto list = [&](const char* key) {
    const nlohmann::json l = index.value(key, nlohmann::json::array());
    if (!l.is_array())
      throw std::runtime_error(std::string("`") + key + "` is a list, not " +
                               l.type_name());
    return l;
  };
  // A project holds at least one document (EnsureParts), and the save
  // writes every one: a list that lost them all had nothing to open, and
  // the adopt activated a tab that was not there.
  if (list("documents").empty() && list("parts").empty())
    throw std::runtime_error("lists no documents (`documents`, or a v1 "
                             "`parts`); a project holds at least one");
  RefuseDocumentIds(list("documents"));
  for (const auto& d : list("documents")) {
    readctx::RefuseUnknownKeys(d, {"id", "name", "dir", "type", "folder"},
                               "document entry");
    // A v2 entry always says what it is: one without a type was a part only
    // in v1, whose `parts` list had none.
    if (!d.contains("type"))
      throw std::runtime_error("document entry " +
                               d.value("name", std::string("?")) +
                               " has no type");
    const std::string name = d.at("name").get<std::string>();
    d.at("dir").get<std::string>();
    // Refused, never read as a part: an entry a newer build wrote as some
    // other kind of document would load as a part that is not one.
    const std::string type = d.value("type", std::string("part"));
    if (type != "part" && type != "assembly" && type != "drawing" &&
        type != "job")
      throw std::runtime_error("document '" + name + "' has type '" + type +
                               "' (expected one of: part, assembly, drawing, "
                               "job)");
  }
  for (const auto& p : list("parts")) // v1
    readctx::RefuseUnknownKeys(p, {"name", "dir"}, "part entry");
  std::set<std::string> names;
  for (const auto& p : list("parameters")) {
    readctx::RefuseUnknownKeys(p, {"name", "expression", "comment"},
                               "project parameter");
    if (!names.insert(p.at("name").get<std::string>()).second)
      throw std::runtime_error("parameters: '" +
                               p.at("name").get<std::string>() +
                               "' is defined twice");
  }
  for (const auto& f : list("folders"))
    if (!f.is_string() || f.get<std::string>().empty())
      throw std::runtime_error("`folders` holds a name that is not one: " +
                               f.dump());
  // The app holds at most as many cuts as the renderer carries, and never
  // writes more: a list past it is damage (a grown one loaded every copy).
  const nlohmann::json sections = list("sections");
  if (sections.size() > static_cast<size_t>(kMaxClipPlanes))
    throw std::runtime_error("`sections` lists " +
                             std::to_string(sections.size()) +
                             " section cuts; a project holds at most " +
                             std::to_string(kMaxClipPlanes));
  for (const auto& s : sections) {
    readctx::RefuseUnknownKeys(s,
                               {"base_plane", "offset", "reverse", "visible",
                                "plane_id", "face", "tilt_u", "tilt_v"},
                               "section");
    if (s.contains("face"))
      readctx::RefuseUnknownKeys(s.at("face"), {"feature_id", "element"},
                                 "section face");
    // Read the way the adopt reads them, so a wrong type is refused here.
    (void)s.value("base_plane", 0);
    (void)s.value("plane_id", 0ull);
    (void)s.value("offset", 0.0f);
    (void)s.value("tilt_u", 0.0f);
    (void)s.value("tilt_v", 0.0f);
    (void)s.value("reverse", false);
    (void)s.value("visible", true);
  }
  (void)index.value("active", 0);
  (void)index.value("requirements_next_id", uint64_t{1});
  // What the adopt reads for keeps, read once here so it refuses here.
  (void)catalog::Deserialize(index.value("catalogs", nlohmann::json::array()));
  (void)lookuptable::ListFromJson(index.value("tables", nlohmann::json()));
  (void)tmpl::LibraryFromJson(index.value("templates", nlohmann::json()));
}

} // namespace

std::expected<LoadedProject, std::string> ReadProject(const std::string& path,
                                                      OpenProgress* progress) {
  namespace fs = std::filesystem;
  LoadedProject out;
  out.path = path;
  const fs::path projFile = fs::path(path) / "project.json";
  std::error_code existsEc;
  if (!fs::exists(projFile) && fs::is_directory(path, existsEc) &&
      !fs::exists(fs::path(path) / "index.json", existsEc))
    // Neither form: a container whose project.json went (its documents'
    // directories are still there), or no project at all.
    return std::unexpected(readctx::Named(
        "missing, and so is index.json: not a Lobole project "
        "(a project of several documents keeps its list in project.json)",
        "project.json"));
  if (!fs::exists(projFile)) {
    // Not a multi-document project: a single part.
    if (progress)
      progress->total.store(1);
    readctx::CollectDrops drops;
    const auto onDisk = DocumentIO::InspectGeometry(path);
    auto loaded = DocumentIO::Load(path);
    if (!loaded)
      return std::unexpected(loaded.error());
    if (WasRebuilt(**loaded)) {
      out.rebuiltParts = 1;
      out.rebuildReason = RebuildReason(onDisk);
    }
    out.single = std::move(*loaded);
    out.drops = drops.Take();
    if (progress)
      progress->done.store(1);
    return out;
  }
  try {
    out.container = true;
    readctx::InFile("project.json", [&] {
      std::ifstream in(projFile, std::ios::binary);
      if (!in)
        throw std::runtime_error("cannot be read");
      out.index =
          readctx::ParseStrict(std::string((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>()));
      CheckProjectIndex(out.index);
    });
    (void)ReadRequirements(fs::path(path)); // refused here, kept by the adopt
    // v2 has a typed `documents` list; v1 had a flat `parts` list (all parts,
    // no ids/types). Normalize v1 into the same shape with defaulted reads.
    nlohmann::json docs;
    if (out.index.contains("documents")) {
      docs = out.index.at("documents");
    } else {
      docs = nlohmann::json::array();
      for (const auto& p : out.index.value("parts", nlohmann::json::array()))
        docs.push_back(
            {{"name", p.at("name")}, {"dir", p.at("dir")}, {"type", "part"}});
    }
    const size_t n = docs.size();
    if (progress)
      progress->total.store(static_cast<int>(n));

    // The project variables, evaluated BEFORE any part loads, so a part that
    // uses one regenerates right the first time and keeps its baked
    // geometry. Loaded without them, the chess set's piece part came up with
    // king_h unknown, every feature on it failed, and each open rebuilt it
    // from scratch before the adopt put it right.
    std::vector<Parameter> projectParams;
    readctx::InFile("project.json", [&] {
      for (const auto& pj :
           out.index.value("parameters", nlohmann::json::array()))
        projectParams.push_back({pj.at("name").get<std::string>(),
                                 pj.at("expression").get<std::string>(),
                                 pj.value("comment", std::string{}),
                                 0.0,
                                 {}});
    });
    SymbolTable projectSymbols;
    {
      // The tables first, for a variable that reads one. A malformed table
      // fails the open where the envelope is read for keeps.
      SymbolTable cells;
      try {
        lookuptable::AddCellSymbols(lookuptable::ListFromJson(out.index.value(
                                        "tables", nlohmann::json())),
                                    cells);
      } catch (const std::exception&) {
      }
      EvaluateParameterTable(projectParams, projectSymbols, &cells, {});
    }
    const DocumentIO::ProjectScope scope{&projectSymbols, &projectParams};

    // Every document is independent of the others until the adopt wires the
    // project scope in, so they load side by side, collected by index and
    // taken in order afterwards so the result does not depend on which
    // finished first.
    std::vector<std::expected<PartSlot, std::string>> slots;
    slots.reserve(n);
    for (size_t i = 0; i < n; i++)
      slots.emplace_back(std::unexpected(std::string("cancelled")));
    std::vector<std::string> reasons(n);
    std::vector<readctx::LoadReport> drops(n);
    parallel::For(n, [&](size_t i) {
      if (progress && progress->cancel && progress->cancel->load())
        return;
      // OCC's signal handler is per thread; a pool thread has none yet.
      ArmOccSignalHandlers();
      if (progress && progress->delayEachMs > 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(progress->delayEachMs));
        if (progress->cancel && progress->cancel->load())
          return;
      }
      const std::string name = docs[i].value("name", std::string{});
      if (progress) {
        std::lock_guard lock(progress->mutex);
        progress->building.push_back(name);
      }
      // This thread's own collector: a pool thread sees no other.
      readctx::CollectDrops collect;
      try {
        slots[i] = ReadSlot(docs[i], i, fs::path(path), &reasons[i], scope);
      } catch (const std::exception& e) {
        slots[i] = std::unexpected(std::string(e.what()));
      }
      drops[i] = collect.Take();
      if (progress) {
        {
          std::lock_guard lock(progress->mutex);
          auto& b = progress->building;
          if (auto it = std::find(b.begin(), b.end(), name); it != b.end())
            b.erase(it);
        }
        progress->done.fetch_add(1);
      }
    });
    if (progress && progress->cancel && progress->cancel->load())
      return std::unexpected(std::string("cancelled"));
    std::set<std::string> distinctReasons;
    for (size_t i = 0; i < n; i++) {
      if (!slots[i])
        return std::unexpected(slots[i].error());
      out.parts.push_back(std::move(*slots[i]));
      out.drops.Append(drops[i]);
      if (!reasons[i].empty()) {
        out.rebuiltParts++;
        distinctReasons.insert(reasons[i]);
      }
    }
    if (distinctReasons.size() == 1)
      out.rebuildReason = *distinctReasons.begin();
    return out;
  } catch (const std::exception& e) {
    return std::unexpected(std::string(e.what()));
  }
}

namespace {
// The parts the open gate brought up to date, and the project it opened:
// MarkProjectOpened stamps every part saved and then marks these unsaved.
std::pair<std::string, std::vector<uint64_t>>& TakenAtOpen() {
  static std::pair<std::string, std::vector<uint64_t>> taken;
  return taken;
}
} // namespace

bool AdoptProject(AppState& state, LoadedProject&& loaded, std::string* error) {
  namespace fs = std::filesystem;
  const std::string path = loaded.path;
  state.openRebuiltParts = loaded.rebuiltParts;
  state.openRebuildReason = loaded.rebuildReason;
  // What the read kept going past, and where it came from: a save back over
  // it is refused while any of it is lossy (SaveProject).
  state.openDrops = loaded.drops;
  state.openDropsPath = path;
  state.lossySaveBlocked = false;
  // The previous project's cuts are not this one's. Cleared for BOTH branches
  // here; the container branch refills them from the index below.
  ClearSections(state);
  if (!loaded.container) {
    ReplaceDocument(state, std::move(loaded.single));
    state.parts.clear();
    state.tabFolders.clear();
    state.nextPartId = 1;
    EnsureParts(state, fs::path(path).stem().string());
    // The lone part's file names no document id; it is the one document,
    // so every requirement that names one names it.
    for (req::Requirement& r : state.requirements)
      if (r.measure.contains("document"))
        r.measure["document"] = state.parts.front().id;
    StampDiskState(state, path); // F8 — see the container branch below
    return true;
  }
  try {
    const nlohmann::json& index = loaded.index;
    const auto loadedCatalogs =
        catalog::Deserialize(index.value("catalogs", nlohmann::json::array()));
    // Refused, not dropped: expressions read these, and a project that lost
    // one would load with every lookup in it broken.
    auto loadedTables =
        lookuptable::ListFromJson(index.value("tables", nlohmann::json()));
    // Refused too: a requirement naming a measure this build does not know
    // would load as something it is not.
    auto loadedRequirements = ReadRequirements(fs::path(path));
    // Refused the same way: an instance never reads the library, but a
    // project that silently lost a template could not update one.
    auto loadedTemplates =
        tmpl::LibraryFromJson(index.value("templates", nlohmann::json()));

    // This branch never goes through ReplaceDocument, so it releases the
    // per-part render cache itself — same id-reuse hazard — and the drawing
    // projections, whose run in flight would land on a reused slot id.
    ReleasePartRenderCache(state);
    ForgetDrawingProjections(state);
    state.document = nullptr;
    state.assembly = nullptr;
    state.drawing = nullptr;
    state.commands = nullptr;
    state.parts = std::move(loaded.parts);
    uint64_t maxId = 0;
    for (const PartSlot& slot : state.parts)
      maxId = std::max(maxId, slot.id);
    state.nextPartId = maxId + 1;
    // Folder registry: the saved list (empty folders and their order), plus
    // any slot folder it doesn't know (pre-registry saves, hand-edits).
    state.tabFolders.clear();
    for (const auto& f : index.value("folders", nlohmann::json::array()))
      if (f.is_string() && !f.get<std::string>().empty())
        state.tabFolders.push_back(f.get<std::string>());
    for (const PartSlot& slot : state.parts)
      if (!slot.folder.empty() &&
          std::find(state.tabFolders.begin(), state.tabFolders.end(),
                    slot.folder) == state.tabFolders.end())
        state.tabFolders.push_back(slot.folder);

    // Sections (I3 P3). Absent = none, which is every project saved before
    // this shipped; ids are re-minted because they are only row identity.
    for (const auto& sj : index.value("sections", nlohmann::json::array())) {
      if (!sj.is_object())
        continue;
      AppState::SectionView sec;
      sec.id = state.nextSectionId++;
      sec.basePlane = sj.value("base_plane", 0);
      sec.planeId = sj.value("plane_id", 0ull);
      if (sj.contains("face") && sj["face"].is_object()) {
        sec.face.featureId = sj["face"].value("feature_id", 0ull);
        sec.face.name = sj["face"].value("element", std::string{});
      }
      sec.offset = sj.value("offset", 0.0f);
      sec.tiltU = sj.value("tilt_u", 0.0f);
      sec.tiltV = sj.value("tilt_v", 0.0f);
      sec.reverse = sj.value("reverse", false);
      sec.visible = sj.value("visible", true);
      state.sections.push_back(sec);
    }
    if (!state.sections.empty())
      state.sectionActiveId = state.sections.front().id;

    size_t wantActive = static_cast<size_t>(index.value("active", 0));
    if (wantActive >= state.parts.size())
      wantActive = 0;
    // Exact boxes remembered by shape identity belong to the project that
    // was open: its shapes are gone (CADShape::RememberTightBox).
    CADShape::ForgetSharedTightBoxes();
    ActivateSlot(state, wantActive);
    ResetActiveTabUiState(state);

    // Phase V: load the project-scope variables from the envelope, wire every
    // part's Document to them, and regenerate all (Load regenerated each part
    // WITHOUT the project scope, so a feature referencing a project var errored
    // — SyncProjectScope re-evaluates it correctly).
    state.catalogs = loadedCatalogs;
    state.catalogHistory = {};
    state.projectParameters.clear();
    state.projectTables = std::move(loadedTables);
    state.projectTemplates = std::move(loadedTemplates);
    state.projectVarHistory = {};
    state.requirements = std::move(loadedRequirements);
    state.nextRequirementId = index.value("requirements_next_id", uint64_t{1});
    for (const req::Requirement& r : state.requirements)
      state.nextRequirementId = std::max(state.nextRequirementId, r.id + 1);
    AdoptAssertionsAsRequirements(state);
    state.requirementsEdits++;
    for (const auto& pj : index.value("parameters", nlohmann::json::array()))
      state.projectParameters.push_back({pj.at("name").get<std::string>(),
                                         pj.at("expression").get<std::string>(),
                                         pj.value("comment", std::string{}),
                                         0.0,
                                         {}});
    SyncProjectScope(state);
    // The open gate (docs/modeling/derived.md#copies-that-keep-up): before
    // anything reads a part -- the assembly's solve below, the results'
    // inputs -- a copy its source moved past while on disk is taken. The
    // parts it changed are remembered so the open's stamp leaves them
    // unsaved, as an edit would.
    TakenAtOpen() = {path, {}};
    for (const LinkUpdate& u : RefreshLinks(state))
      TakenAtOpen().second.push_back(u.partId);
    // A4: an assembly active on open solves immediately (no stamps yet), so
    // mates re-solve on load — the format's stated contract.
    if (state.assembly && AssemblyPartsStale(state))
      SolveActiveAssembly(state);
    // The saved study and motion results whose inputs still hash as they
    // did read current (docs/simulation/README.md#studies-as-jobs). The
    // motion results the session held were the previous project's.
    state.motionResults.clear();
    state.motionResultRevisions.clear();
    AdoptStoredResults(state);
    // The opened drawing's views: each one's saved projection is installed
    // when its inputs still hash as they did, and the rest project, all on a
    // `drawing` job rather than on the frame
    // (docs/drawings/README.md#saved-projections).
    if (state.drawing && state.drawing->ViewCount() > 0)
      RequestDrawingUpdate(state);
    // Frame whatever the opened project put in front of us. This tested
    // `state.document` alone, which is null when the active tab is an
    // ASSEMBLY — so opening an assembly project left the camera at its
    // default, inside a 5 m dock, and the app looked empty.
    // A JOB tab too: opening a project whose active tab was a job left the
    // camera wherever the last project had it -- the chess set opened
    // looking at a corner of the jig from underneath it.
    if (state.document || state.assembly || state.job) {
      FitCameraToDocument(state);
      if (state.activePart >= 0 &&
          static_cast<size_t>(state.activePart) < state.parts.size())
        state.parts[state.activePart].everFramed = true;
    }
    // The other job tabs' exact boxes, off the frame, so the first visit to
    // one is not a second of CPU (docs/cam/gui.md#job-tab-frame-cost). The
    // last project's were let go above.
    StartJobBoundsWarm(state);
    // F8: we just READ this from disk, so memory and disk agree by
    // definition. Stamping here rather than in each caller is what makes
    // every reload path — restore, branch switch, finishing a merge, the
    // "changed on disk" banner's own Reload — clear the flag without having
    // to remember to. BOTH success paths need it: a single-part project
    // returns from the branch above without ever reaching this one.
    StampDiskState(state, path);
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = e.what();
    return false;
  }
}

bool OpenProject(AppState& state, const std::string& path, std::string* error) {
  auto loaded = ReadProject(path);
  if (!loaded) {
    if (error)
      *error = loaded.error();
    return false;
  }
  return AdoptProject(state, std::move(*loaded), error);
}

std::string NormalizeProjectPick(const std::string& path) {
  namespace fs = std::filesystem;
  fs::path p(path);
  if (p.filename() == "project.json")
    p = p.parent_path();
  for (fs::path a = p; !a.empty() && a != a.root_path(); a = a.parent_path())
    if (DocumentIO::IsProjectFolderName(a.string()))
      return a.string();
  return p.string();
}

bool IsProjectPath(const std::string& path) {
  namespace fs = std::filesystem;
  const fs::path root(NormalizeProjectPick(path));
  std::error_code ec;
  return DocumentIO::IsProjectFolderName(root.string()) ||
         root.extension() == ".json" || fs::is_directory(root, ec);
}

// The stamp a slot's dirty check (and its autosave) compares against:
// whichever document kind the slot holds. 0 for an empty slot. A part's,
// an assembly's and a job's is its SaveStamp, not its Version: a tolerance,
// the units, a study, a configuration that is not the active one, a job's
// operation list are all saved and none of them regenerates anything. A
// drawing bumps its version on every edit, so that is its stamp.
uint64_t SlotVersion(AppState& state, size_t i) {
  if (Document* d = SlotDoc(state, i))
    return d->SaveStamp();
  if (AssemblyDocument* a = SlotAssembly(state, i))
    return a->SaveStamp();
  if (DrawingDocument* dr = SlotDrawing(state, i))
    return dr->Version();
  if (cam::JobDocument* j = SlotJob(state, i))
    return j->SaveStamp();
  return 0;
}
namespace {
// SaveProject writes every slot, so a clean save stamps every slot — the
// active one through state.lastSavedVersion (its slot copy is stale while
// active; StashActive writes it back on tab switch).
void StampAllSaved(AppState& state) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    const uint64_t v = SlotVersion(state, i);
    if (static_cast<int>(i) == state.activePart)
      state.lastSavedVersion = v;
    else
      state.parts[i].lastSavedVersion = v;
  }
  if (state.parts.empty())
    state.lastSavedVersion = state.document ? state.document->SaveStamp() : 0;
  state.projectMetaDirty = false;
}

// Merged from main (ea33ff82). It was written immediately after
// StampAllSaved, and follows it here: the two helpers below are about the
// saved-version counter this file owns. SyncSketchViewRollback rides along
// with them because it is what needs SetViewRollbackKeepingDirt -- worth
// revisiting when a later wave of the stage-3 split moves dirty tracking.
// The saved-version counter for `doc`, or null when it is not a document this
// app is tracking. The active slot's counter lives on AppState (its slot copy
// is stale until StashActive writes it back), which is why this is not a
// plain index.
uint64_t* SavedVersionFor(AppState& state, const Document* doc) {
  if (doc == nullptr)
    return nullptr;
  if (state.parts.empty())
    return state.document.get() == doc ? &state.lastSavedVersion : nullptr;
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (SlotDoc(state, i) != doc)
      continue;
    return static_cast<int>(i) == state.activePart
               ? &state.lastSavedVersion
               : &state.parts[i].lastSavedVersion;
  }
  return nullptr;
}

// Moves a document's VIEW rollback without changing whether it counts as
// modified. Opening a sketch to look at it must not dirty the project: the
// marker never reaches the file, but SetViewRollback regenerates, every
// Regenerate bumps Version(), and dirty is measured as SaveStamp() != saved.
// So a document that was clean is re-stamped at its new version; one that was
// already dirty (the user has edited something) is left alone, in both
// directions.
void SetViewRollbackKeepingDirt(AppState& state, Document* doc, int index) {
  uint64_t* saved = SavedVersionFor(state, doc);
  const bool wasClean = saved != nullptr && doc->SaveStamp() == *saved;
  doc->SetViewRollback(index);
  if (wasClean)
    *saved = doc->SaveStamp();
}

// Every document this app holds, active slot included.
template <typename F> void ForEachDocument(AppState& state, F&& fn) {
  if (state.parts.empty()) {
    if (state.document != nullptr)
      fn(state.document.get());
    return;
  }
  for (size_t i = 0; i < state.parts.size(); i++)
    if (Document* d = SlotDoc(state, i); d != nullptr)
      fn(d);
}
} // namespace

void SyncSketchViewRollback(AppState& state) {
  SketchViewRollback& applied = state.sketchViewRollback;
  Document* doc = state.document.get();
  const uint64_t wantSketch =
      doc != nullptr ? state.sketchEdit.activeSketchId : uint64_t{0};
  const uint64_t wantDoc = wantSketch != 0 ? doc->InstanceId() : uint64_t{0};
  const uint64_t version = doc != nullptr ? doc->Version() : uint64_t{0};
  // The overwhelmingly common frame: same sketch, same tab, nothing edited.
  if (wantSketch == applied.sketchId && wantDoc == applied.documentId &&
      version == applied.documentVersion)
    return;

  // The old marker need not be on the tab that is open now — switching tabs
  // is one of the ways a sketch session ends, and a marker left behind is a
  // model with its features missing and no sketch open to explain why.
  if (applied.documentId != 0 && applied.documentId != wantDoc) {
    const uint64_t stale = applied.documentId;
    ForEachDocument(state, [&](Document* d) {
      if (d->InstanceId() == stale && d->ViewRollback() >= 0)
        SetViewRollbackKeepingDirt(state, d, -1);
    });
  }

  if (wantSketch == 0) {
    if (doc != nullptr && doc->ViewRollback() >= 0)
      SetViewRollbackKeepingDirt(state, doc, -1);
    applied = {};
    return;
  }

  // -1 (nothing consumes this sketch, or it is gone) clears the marker: a
  // brand-new sketch, or the last feature in the tree, has nothing built from
  // it to be in the way. Rolling back to the sketch's own position instead
  // would blank every unrelated feature after it, which is worse than the
  // problem being fixed.
  SetViewRollbackKeepingDirt(state, doc, doc->FirstConsumerIndex(wantSketch));
  applied = {wantSketch, wantDoc, doc->Version()};
}

bool ProjectDirty(AppState& state) {
  if (state.projectMetaDirty)
    return true;
  if (state.parts.empty())
    return state.document &&
           state.document->SaveStamp() != state.lastSavedVersion;
  for (size_t i = 0; i < state.parts.size(); i++) {
    const uint64_t saved = static_cast<int>(i) == state.activePart
                               ? state.lastSavedVersion
                               : state.parts[i].lastSavedVersion;
    if (SlotVersion(state, i) != saved)
      return true;
  }
  return false;
}

std::string WindowTitle(AppState& state) {
  std::string name = "Untitled";
  if (!state.projectPath.empty())
    name = std::filesystem::path(state.projectPath).stem().string();
  return name + (ProjectDirty(state) ? "*" : "") + " - Loböle";
}

void MarkProjectSaved(AppState& state, const std::string& requested,
                      const std::string& message) {
  // SaveProject wrote a .dsgn's project to the .lob beside it; from here on
  // that is the project. The .dsgn's own sidecar goes too: what it held is
  // saved now.
  const std::string path = ProjectWriteTarget(requested);
  if (path != requested) {
    Autosaver::Clear(requested);
    state.savedAsNotice = "Saved as " + path + " -- " +
                          std::filesystem::path(requested).filename().string() +
                          " is left as it was";
    state.savedAsNoticeUntil = -2.0;
  }
  LOG_INFO("save: {}", path);
  state.projectPath = path;
  RecentProjects::Remember(path);
  StampAllSaved(state);
  // A save re-bakes the geometry, so the next open is a load again.
  state.openRebuiltParts = 0;
  state.openRebuildReason.clear();
  // The save supersedes both possible sidecars: this project's, and the
  // unsaved slot if the document lived there before its first save.
  Autosaver::Clear(path);
  Autosaver::Clear("");
  // Git-backed history (F5): a directory project is its own repository;
  // every clean save becomes a commit. No-ops without git.
  if (DocumentIO::IsProjectFolderName(path) && GitVault::Available()) {
    if (!GitVault::IsRepo(path)) {
      GitVault::Init(path);
    }
    std::string error;
    if (!GitVault::CommitAll(path, message, &error)) {
      LOG_WARN("Git commit failed: {}", error);
    }
  }
  // LAST, and after the commit: everything above is our own write, and a
  // stamp taken before it would read as an external change on the next save.
  StampDiskState(state, path);
}

void MarkProjectOpened(AppState& state, const std::string& path) {
  LOG_INFO("open: {}", path.empty() ? std::string("(new)") : path);
  // A new project, or one opened without AdoptProject: the last open's
  // drops were another project's.
  if (path.empty() || !SamePath(path, state.openDropsPath)) {
    state.openDrops = {};
    state.openDropsPath.clear();
  }
  state.projectPath = path;
  state.projectGeneration++;      // the autosaver's tracking no longer applies
  RecentProjects::Remember(path); // ignores "" (the unsaved slot)
  StampAllSaved(state);
  StampDiskState(state, path); // F8: memory and disk agree as of now
  // What the open gate took is not what is on disk: those parts read
  // unsaved, as an edit would leave them
  // (docs/modeling/derived.md#copies-that-keep-up).
  if (!path.empty() && SamePath(TakenAtOpen().first, path))
    for (uint64_t id : TakenAtOpen().second)
      for (PartSlot& slot : state.parts)
        if (slot.id == id)
          slot.lastSavedVersion = 0;
  TakenAtOpen() = {};
  // ...and a gate after the stamps, for a path that opened without one.
  RefreshLinks(state);
}

namespace {

namespace fs = std::filesystem;

// Write one document to `target` the way F2 does: temp + rename, so a crash
// between the two leaves the previous sidecar intact.
bool WriteSidecarAtomic(AppState& state, size_t slot, const fs::path& target,
                        std::string* error) {
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  const fs::path tmp = target.string() + ".tmp";
  std::expected<void, std::string> r;
  if (Document* d = SlotDoc(state, slot))
    r = DocumentIO::Save(*d, tmp.string());
  else if (AssemblyDocument* a = SlotAssembly(state, slot))
    r = DocumentIO::SaveAssembly(*a, tmp.string());
  else if (DrawingDocument* dr = SlotDrawing(state, slot))
    r = DocumentIO::SaveDrawing(*dr, tmp.string());
  else if (cam::JobDocument* j = SlotJob(state, slot))
    r = DocumentIO::SaveJob(*j, tmp.string());
  else
    return false;
  if (!r) {
    if (error)
      *error = r.error();
    fs::remove(tmp, ec);
    return false;
  }
  fs::rename(tmp, target, ec);
  if (ec) {
    if (error)
      *error = "autosave rename failed: " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

// "parts/Stand" — what project.json calls this document.
std::string SlotDir(const AppState& state, size_t i) {
  const PartSlot& slot = state.parts[i];
  const char* sub = slot.kind == DocKind::Assembly  ? "assemblies/"
                    : slot.kind == DocKind::Drawing ? "drawings/"
                    : slot.kind == DocKind::Job     ? "jobs/"
                                                    : "parts/";
  return std::string(sub) + projectpaths::DocFolder(slot.name, slot.id);
}

} // namespace

namespace {

// A volume a person can read, in the document's own units. The function this
// replaces carried the comment "2 100 mm³ / 0.34 cm³" over a body that was
// `%.0f mm³` and had never done either — the comment described a behaviour
// nobody had written.
std::string VolumeText(const AppState& state, double mm3) {
  return units::FormatVolume(mm3, state.ActiveUnits());
}

} // namespace

GeometryChange CompareVersionGeometry(AppState& state,
                                      const std::string& revision,
                                      const std::string& documentDir) {
  namespace fs = std::filesystem;
  GeometryChange out;
  if (state.projectPath.empty() || revision.empty())
    return out;

  // The part in front of us is one operand.
  Document* current = state.document.get();
  if (current == nullptr || current->GetBodies().empty()) {
    out.summary = "no solid to compare";
    return out;
  }

  // The other operand is the same document as it was. Extracted to a scratch
  // directory rather than touching the working tree — the point of a diff is
  // to look without changing anything.
  std::error_code ec;
#if defined(_WIN32)
  const int pid = ::_getpid();
#else
  const int pid = ::getpid();
#endif
  const fs::path scratch =
      fs::temp_directory_path() /
      ("designer-diff-" + std::to_string(pid) + "-" + revision.substr(0, 8));
  fs::remove_all(scratch, ec);
  // A project with one part is saved FLAT — the document IS the project
  // directory, with no `parts/<name>/` under it (DocumentIO's dispatch). So
  // the caller's guess at a subdir is tried first and the root second,
  // rather than duplicating SaveProject's rule for which form applies.
  std::string error;
  bool got = !documentDir.empty() &&
             GitVault::ExtractSubtree(state.projectPath, revision, documentDir,
                                      scratch.string(), &error);
  if (!got) {
    fs::remove_all(scratch, ec);
    got = GitVault::ExtractSubtree(state.projectPath, revision, "",
                                   scratch.string(), &error);
  }
  if (!got) {
    out.summary = error;
    return out;
  }
  auto older = DocumentIO::Load(scratch.string());
  fs::remove_all(scratch, ec);
  if (!older) {
    // The loader's own reason, naming the file in that version that it
    // refused: "does not load" alone sent the user looking.
    out.summary = "that version of this part does not load: " + older.error();
    return out;
  }

  // The diff SOLIDS, not just the volumes: the answer to "what changed?" is
  // a shape, and the panel puts it straight on the model.
  CADShape removedShape, addedShape;
  const auto cmp = Measure::CompareBodySets(
      (*older)->GetBodies(), glm::dmat4(1.0), current->GetBodies(),
      glm::dmat4(1.0), &removedShape, &addedShape);
  if (!cmp) {
    out.summary = "the shapes could not be compared";
    return out;
  }
  out.valid = true;
  out.oldVolume = cmp->volumeA;
  out.newVolume = cmp->volumeB;
  out.removed = cmp->missing; // in the OLD and not the new
  out.added = cmp->extra;     // in the NEW and not the old

  // A boolean on two tessellated solids never lands exactly on zero, so
  // "unchanged" is a proportion of the part rather than an equality.
  const double scale = std::max(out.oldVolume, out.newVolume);
  const double noise = scale * 1e-6;
  const bool grew = out.added > noise, shrank = out.removed > noise;
  if (!grew && !shrank)
    out.summary = "same shape";
  else if (grew && shrank)
    out.summary = VolumeText(state, out.added) + " added, " +
                  VolumeText(state, out.removed) + " removed";
  else if (grew)
    out.summary = VolumeText(state, out.added) + " added";
  else
    out.summary = VolumeText(state, out.removed) + " removed";

  // Red where material went away, green where it arrived — the same sense
  // as compare_geometry's materialize (red = in the reference and not here).
  // "same shape" gets no overlay: two empty solids would just look broken.
  if (grew || shrank)
    SetGeometryDelta(state, removedShape, addedShape,
                     "vs " + revision.substr(0, 8) + " — " + out.summary +
                         " (red gone, green new)");
  else
    ClearGeometryDelta(state);
  return out;
}

bool RevealItem(AppState& state, const std::string& document,
                const std::string& documentKind, uint64_t itemId, bool frame) {
  if (itemId == 0)
    return false; // an index or project.json: nothing to point at

  // The change may live in a document that is not the open tab, which is the
  // whole reason this is worth a click on a multi-document project.
  if (!document.empty()) {
    const DocKind want = documentKind == "assembly"  ? DocKind::Assembly
                         : documentKind == "drawing" ? DocKind::Drawing
                                                     : DocKind::Part;
    size_t found = state.parts.size();
    for (size_t i = 0; i < state.parts.size(); i++)
      if (state.parts[i].name == document && state.parts[i].kind == want)
        found = i;
    if (found == state.parts.size())
      return false; // the document itself is gone
    if (static_cast<int>(found) != state.activePart)
      SetActivePart(state, found);
  }

  // Only a part holds features; an occurrence or a drawing dim is selected
  // through its own set, which this does not reach into.
  Document* doc = state.document.get();
  if (doc == nullptr || doc->FindFeatureById(itemId) == nullptr)
    return false; // removed by this very change, most likely

  SelectFeature(state, itemId);
  if (frame)
    FitCameraToSelection(state);
  return true;
}

bool RecoverProject(AppState& state, const std::string& path,
                    std::string* error) {
  namespace fs = std::filesystem;
  if (!OpenProject(state, path, error))
    return false;
  MarkProjectOpened(state, path);

  // Every sidecar is READ before any is laid over the project, so the
  // recovery is all-or-nothing as it says, and one that does not load is
  // refused with the loader's own reason, naming the sidecar -- it used to
  // be skipped, and the rest recovered as if it had not been there.
  const fs::path dir(Autosaver::SidecarDir(path));
  struct Sidecar {
    size_t slot;
    std::unique_ptr<Document> part;
    std::unique_ptr<AssemblyDocument> assembly;
    std::unique_ptr<DrawingDocument> drawing;
    std::unique_ptr<cam::JobDocument> job;
  };
  std::vector<Sidecar> read;
  for (const std::string& rel : Autosaver::RecoverableDocuments(path)) {
    // "parts/Stand" -> the slot of that kind and name.
    const size_t slash = rel.find('/');
    if (slash == std::string::npos)
      continue;
    const std::string folder = rel.substr(0, slash);
    const std::string name = rel.substr(slash + 1);
    const fs::path file = dir / (rel + ".json");
    // The loader names the file it read; this names where it is.
    readctx::FileContext where(dir.filename().string() + "/" + folder + "/");
    for (size_t i = 0; i < state.parts.size(); i++) {
      const PartSlot& slot = state.parts[i];
      if (slot.name != name)
        continue;
      Sidecar sc{i, {}, {}, {}, {}};
      std::string failed;
      if (folder == "parts" && slot.kind == DocKind::Part) {
        auto loaded = DocumentIO::Load(file.string());
        loaded ? void(sc.part = std::move(*loaded))
               : void(failed = loaded.error());
      } else if (folder == "assemblies" && slot.kind == DocKind::Assembly) {
        auto loaded = DocumentIO::LoadAssembly(file.string());
        loaded ? void(sc.assembly = std::move(*loaded))
               : void(failed = loaded.error());
      } else if (folder == "drawings" && slot.kind == DocKind::Drawing) {
        auto loaded = DocumentIO::LoadDrawing(file.string());
        loaded ? void(sc.drawing = std::move(*loaded))
               : void(failed = loaded.error());
      } else if (folder == "jobs" && slot.kind == DocKind::Job) {
        auto loaded = DocumentIO::LoadJob(file.string());
        loaded ? void(sc.job = std::move(*loaded))
               : void(failed = loaded.error());
      } else {
        continue;
      }
      if (!failed.empty()) {
        if (error)
          *error = "the autosave of " + rel + " does not load: " + failed;
        LOG_ERROR("Could not recover {}: {}", path, failed);
        return false;
      }
      read.push_back(std::move(sc));
      break;
    }
  }
  for (Sidecar& sc : read) {
    PartSlot& slot = state.parts[sc.slot];
    const bool active = static_cast<int>(sc.slot) == state.activePart;
    if (sc.part) {
      if (active)
        ReplaceDocument(state, std::move(sc.part));
      else
        slot.doc = std::move(sc.part);
    } else if (sc.assembly) {
      (active ? state.assembly : slot.assembly) = std::move(sc.assembly);
    } else if (sc.drawing) {
      (active ? state.drawing : slot.drawing) = std::move(sc.drawing);
    } else if (sc.job) {
      (active ? state.job : slot.job) = std::move(sc.job);
    }
  }
  if (!read.empty()) {
    LOG_INFO("Recovered {} autosaved document(s) into {}", read.size(), path);
    // Unsaved by definition: the sidecars were never a save.
    state.projectMetaDirty = true;
  }
  return true;
}

int AutosaveProject(AppState& state, Autosaver& autosaver, double now) {
  namespace fs = std::filesystem;
  autosaver.NoteProject(state.projectPath, state.projectGeneration);
  // One document IS the project: the single-file sidecar, unchanged. This is
  // the shape F2 designed for and every existing autosave test covers.
  if (state.parts.size() <= 1) {
    if (!state.document)
      return 0;
    return autosaver.MaybeSave(*state.document, state.projectPath,
                               state.lastSavedVersion, now)
               ? 1
               : 0;
  }
  if (!autosaver.Due(now))
    return 0;

  int written = 0;
  for (size_t i = 0; i < state.parts.size(); i++) {
    const uint64_t version = SlotVersion(state, i);
    const uint64_t saved = static_cast<int>(i) == state.activePart
                               ? state.lastSavedVersion
                               : state.parts[i].lastSavedVersion;
    // Only a part has an instance id; the others rely on Forget() at open.
    const Document* doc = SlotDoc(state, i);
    const uint64_t instance = doc ? doc->InstanceId() : 0;
    if (!autosaver.NeedsSave(state.parts[i].id, instance, version, saved))
      continue;

    const fs::path target = fs::path(Autosaver::SidecarDir(state.projectPath)) /
                            (SlotDir(state, i) + ".json");
    std::string error;
    if (!WriteSidecarAtomic(state, i, target, &error)) {
      LOG_WARN("Autosave of {} failed: {}", state.parts[i].name, error);
      continue;
    }
    autosaver.NoteSaved(state.parts[i].id, instance, version);
    written++;
  }
  if (written > 0) {
    autosaver.MarkSaved(now);
    LOG_INFO("Autosaved {} document(s) to {}", written,
             Autosaver::SidecarDir(state.projectPath));
  }
  return written;
}

bool AddProjectRemote(AppState& state, const std::string& name,
                      const std::string& url, std::string* error) {
  if (state.projectPath.empty()) {
    if (error)
      *error = "save the project before sharing it";
    return false;
  }
  // Check BEFORE acting. Sharing a project also stops it committing built
  // geometry (below), and that needs a save — which F8's guard refuses when
  // the project moved on disk. This used to add the remote, have the save
  // refused, LOG_WARN, and return success: the user got a remote, no message
  // anywhere, and pushed .brep binaries to their team. Both features were
  // behaving correctly; only using them together showed it.
  //
  // So the whole operation is one decision rather than a half-applied one.
  if (ProjectChangedOnDisk(state)) {
    if (error)
      *error = "the project changed on disk — reload or save it before "
               "sharing, so the built geometry can be left behind";
    return false;
  }
  if (!GitVault::AddRemote(state.projectPath, name, url, error))
    return false;

  // C4: built geometry stops travelling. The flag is per DOCUMENT while this
  // is a decision about the project, so it is set across every part — and
  // then the ordinary save path does the rest, because SaveDir already knows
  // how to remove `geometry/` and write the .gitignore that keeps it out.
  bool hadGeometry = false;
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].kind != DocKind::Part)
      continue; // only parts bake geometry
    if (Document* doc = SlotDoc(state, i)) {
      hadGeometry = hadGeometry || doc->CommitGeometry();
      doc->SetCommitGeometry(false);
    }
  }
  if (!hadGeometry)
    return true;

  std::string saveError;
  if (!SaveProject(state, state.projectPath, &saveError)) {
    // Should not happen now that the disk state is checked above — but a
    // save can still fail (a full disk, a read-only tree), and silently
    // keeping geometry is what this whole fix is about. The remote IS added,
    // so undo it: half-applied is the state nobody can reason about.
    LOG_ERROR("Could not stop committing geometry, so the remote was not "
              "kept: {}",
              saveError);
    std::string ignored;
    GitVault::RemoveRemote(state.projectPath, name, &ignored);
    if (error)
      *error = "could not save the project, so it was not shared: " + saveError;
    return false;
  }
  MarkProjectSaved(state, state.projectPath, "Stop sharing built geometry");
  LOG_INFO("{} now has a remote; built geometry is no longer committed",
           state.projectPath);
  return true;
}

void StampDiskState(AppState& state, const std::string& path) {
  state.diskStampPath = path;
  state.diskStamp = DocumentIO::StampProject(path);
  state.diskStampHead.clear();
  // Read the way the poll reads it (no `git`), so the two always agree.
  if (GitVault::IsRepo(path))
    state.diskStampHead = GitVault::HeadHash(path);
  state.diskGeneration++; // a poll asked before this compared the old stamp
  state.diskChanged = false;
  state.diskCheckStamp = -1.0; // let the poll re-answer from the new baseline
  state.conflictsStale = true; // a different tree: rescan once, not on a clock
}

bool ProjectChangedOnDisk(AppState& state) {
  if (state.projectPath.empty() || !state.diskStamp.valid ||
      state.diskStampPath != state.projectPath)
    return false; // nothing to compare against is not a change
  return !DocumentIO::StampProject(state.projectPath).Matches(state.diskStamp);
}

void MarkProjectRecovered(AppState& state, const std::string& path) {
  state.projectPath = path;
  // Never equal to the live version: the recovered changes are unsaved.
  state.lastSavedVersion =
      state.document ? state.document->SaveStamp() - 1 : UINT64_MAX;
}

} // namespace ui
