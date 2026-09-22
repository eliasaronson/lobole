#include "ui/StepExport.h"

#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "ui/AppActionsInternal.h" // SlotDoc
#include "ui/AssemblyActions.h"    // PartResolverFor, AssemblyResolverFor
#include "ui/BodyAppearance.h"
#include "ui/DocumentSlots.h" // SlotIndexById

#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ui {

namespace {

std::string SlotName(const AppState& state, uint64_t slotId) {
  const std::optional<size_t> i = SlotIndexById(state, slotId);
  return i ? state.parts[*i].name : std::string{};
}

// Every body of `part` in its on-screen colour, `palette` seeded the way the
// tab showing it seeds it; reference bodies advance the count (the part tab
// draws them) but are never written — they are another part's geometry.
std::vector<CADKernel::StepBody> PartBodies(const Document& part,
                                            size_t palette) {
  std::vector<CADKernel::StepBody> out;
  for (const Body& b : part.GetBodies()) {
    const BodyLook look = ResolveBodyLook(b, palette);
    if (b.reference || !b.shape.IsValid())
      continue;
    out.push_back({b.shape, b.name, look.Color()});
  }
  return out;
}

} // namespace

std::expected<CADKernel::StepExportModel, std::string>
BuildPartStepModel(const Document& part, const std::string& name,
                   int bodyIndex) {
  const std::vector<Body>& all = part.GetBodies();
  size_t nonReference = 0;
  for (const Body& b : all)
    nonReference += b.reference ? 0 : 1;
  if (nonReference == 0)
    return std::unexpected(
        all.empty() ? "no bodies to export"
                    : "only reference bodies (derived ghosts); nothing to "
                      "export");
  if (bodyIndex >= static_cast<int>(nonReference))
    return std::unexpected("body index out of range");

  // `bodyIndex` counts the non-reference bodies, as get_state lists them.
  std::vector<CADKernel::StepBody> bodies;
  size_t palette = 0, listed = 0;
  for (const Body& b : all) {
    const BodyLook look = ResolveBodyLook(b, palette);
    if (b.reference)
      continue;
    const bool wanted =
        bodyIndex < 0 || listed == static_cast<size_t>(bodyIndex);
    listed++;
    if (wanted && b.shape.IsValid())
      bodies.push_back({b.shape, b.name, look.Color()});
  }
  if (bodies.empty())
    return std::unexpected("the body to export has no valid geometry");

  CADKernel::StepExportModel model;
  model.parts.push_back({name, std::move(bodies)});
  model.rootIsAssembly = false;
  model.root = 0;
  return model;
}

std::expected<CADKernel::StepExportModel, std::string>
BuildAssemblyStepModel(AppState& state) {
  if (!state.assembly)
    return std::unexpected("the active document is not an assembly");
  // Flatten is the one tree walk (core/AssemblySolve.h); its leaves carry
  // the levels above them, which is all it takes to write the tree back.
  const auto flat = assembly::Flatten(*state.assembly, PartResolverFor(state),
                                      AssemblyResolverFor(state));
  if (!flat)
    return std::unexpected("the assembly cannot be walked: " + flat.error());

  CADKernel::StepExportModel model;
  model.rootIsAssembly = true;
  model.root = 0;
  model.assemblies.push_back(
      {state.parts[static_cast<size_t>(state.activePart)].name, {}});
  // What each assembly node already holds, by the occurrence id in the
  // document it mirrors: a sub-assembly placed twice is filled once.
  std::vector<std::unordered_set<uint64_t>> holds(1);

  // One product per part — a part regenerates in its document's active
  // configuration, so its id is the whole key. -1: nothing to write.
  std::unordered_map<uint64_t, long> partIndex;
  const auto partFor = [&](const assembly::FlatInstance& inst) -> long {
    const auto it = partIndex.find(inst.partId);
    if (it != partIndex.end())
      return it->second;
    // Seeded from the PART, as the assembly renderer seeds it.
    std::vector<CADKernel::StepBody> bodies =
        PartBodies(*inst.part, static_cast<size_t>(inst.partId));
    long index = -1;
    if (!bodies.empty()) {
      index = static_cast<long>(model.parts.size());
      model.parts.push_back({SlotName(state, inst.partId), std::move(bodies)});
    }
    partIndex.emplace(inst.partId, index);
    return index;
  };
  // One assembly per (document, configuration): the placement chooses the
  // configuration, and two configurations of one document differ.
  std::map<std::pair<uint64_t, uint64_t>, size_t> asmIndex;

  for (const assembly::FlatInstance& inst : *flat) {
    if (!inst.visible)
      continue;
    const long part = partFor(inst);
    if (part < 0)
      continue;
    size_t node = 0;
    for (const assembly::FlatInstance::Level& level : inst.levels) {
      const auto key = std::make_pair(level.assemblyId, level.configurationId);
      auto it = asmIndex.find(key);
      if (it == asmIndex.end()) {
        it = asmIndex.emplace(key, model.assemblies.size()).first;
        model.assemblies.push_back({SlotName(state, level.assemblyId), {}});
        holds.emplace_back();
      }
      const size_t child = it->second;
      if (holds[node].insert(level.occurrenceId).second)
        model.assemblies[node].components.push_back(
            {true, child, level.local, level.name});
      node = child;
    }
    if (holds[node].insert(inst.occurrenceId).second)
      model.assemblies[node].components.push_back(
          {false, static_cast<size_t>(part), inst.local, inst.name});
  }
  if (model.assemblies.front().components.empty())
    return std::unexpected(
        "nothing to export: the assembly places no visible geometry");
  return model;
}

StepExportCounts CountStepModel(const CADKernel::StepExportModel& model) {
  StepExportCounts counts;
  counts.products = model.parts.size();
  if (!model.rootIsAssembly) {
    counts.instances = model.root < model.parts.size() ? 1 : 0;
    counts.bodies =
        counts.instances ? model.parts[model.root].bodies.size() : 0;
    return counts;
  }
  // Placements multiply down the tree: a sub-assembly placed twice places
  // its contents twice. The model is acyclic (built from Flatten's tree).
  std::function<void(size_t)> walk = [&](size_t ai) {
    for (const CADKernel::StepComponent& c : model.assemblies[ai].components) {
      if (c.isAssembly) {
        walk(c.index);
      } else {
        counts.instances++;
        counts.bodies += model.parts[c.index].bodies.size();
      }
    }
  };
  if (model.root < model.assemblies.size())
    walk(model.root);
  return counts;
}

std::expected<StepExportCounts, std::string>
ExportStep(AppState& state, const Document* part, int bodyIndex,
           const std::string& path) {
  std::expected<CADKernel::StepExportModel, std::string> model;
  if (part == nullptr) {
    model = BuildAssemblyStepModel(state);
  } else {
    // The part is named after its tab, wherever it was asked for from.
    std::string name;
    for (size_t i = 0; i < state.parts.size(); i++)
      if (state.parts[i].kind == DocKind::Part && SlotDoc(state, i) == part)
        name = state.parts[i].name;
    model = BuildPartStepModel(*part, name, bodyIndex);
  }
  if (!model)
    return std::unexpected(model.error());
  std::string why;
  if (!CADKernel::Instance().SaveSTEPModel(path, *model, &why))
    return std::unexpected("STEP export failed writing " + path + ": " + why);
  return CountStepModel(*model);
}

} // namespace ui
