#include "ui/FlexibleActions.h"

#include "cad/CADMaterials.h"

#include "ui/AppActionsInternal.h" // DisplayMatrix, ExplodeInnerOffset
#include "ui/AssemblyActions.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h" // ConfiguredPartDocument
#include "ui/SelectionPipeline.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

// Flexible sub-assemblies from the GUI (docs/assemblies/README.md
// #flexible-sub-assemblies). The RPC's set_occurrence_flexible and
// set_occurrence_configuration are these, so a refusal reads the same in the
// panel as over the socket.

namespace ui {

std::string FlexibleRefusalHere(AppState& state, const Occurrence& occ) {
  std::string why = assembly::FlexibleRefusal(occ, AssemblyResolverFor(state));
  if (!why.empty())
    return why;
  uint64_t hostId = 0;
  for (const PartSlot& slot : state.parts)
    if (slot.kind == DocKind::Assembly &&
        AssemblyDocById(state, slot.id) == state.assembly.get())
      hostId = slot.id;
  for (const PartSlot& slot : state.parts) {
    if (slot.kind != DocKind::Assembly)
      continue;
    const AssemblyDocument* other = AssemblyDocById(state, slot.id);
    if (other == nullptr || other == state.assembly.get())
      continue;
    for (const Occurrence& o : other->GetOccurrences())
      if (o.flexible && o.partId == hostId)
        return "this assembly is placed FLEXIBLY in '" + slot.name + "' (as '" +
               o.name +
               "'), and only one level moves — a flexible placement inside "
               "a flexible placement is refused";
  }
  return {};
}

bool SetOccurrenceFlexible(AppState& state, uint64_t occId, bool flexible,
                           std::string* why, assembly::SolveReport* report) {
  const auto refuse = [why](std::string w) {
    if (why)
      *why = std::move(w);
    return false;
  };
  if (!state.assembly)
    return refuse("the active document is not an assembly");
  Occurrence* occ = state.assembly->FindById(occId);
  if (occ == nullptr)
    return refuse("no occurrence with id " + std::to_string(occId));
  if (flexible && !occ->flexible) {
    const std::string no = FlexibleRefusalHere(state, *occ);
    if (!no.empty())
      return refuse("'" + occ->name + "' cannot be flexible: " + no);
  }
  SnapshotAssembly(state);
  if (!flexible) {
    // Rigid again: the unit stands as its own document has it, and a
    // relation into it reports that it is no longer flexible.
    occ->innerPoses.clear();
    occ->innerMateDofs.clear();
    // A moving part of it is no longer something to select on its own.
    if (SelectedInnerOccurrence(state).first == occId)
      SelectOccurrence(state, occId);
  }
  occ->flexible = flexible;
  state.assembly->BumpVersion();
  const assembly::SolveReport rep = SolveActiveAssemblyReport(state);
  if (report)
    *report = rep;
  return true;
}

bool SetOccurrenceMaterial(AppState& state, uint64_t occId,
                           const std::string& material, std::string* why) {
  const auto refuse = [why](std::string w) {
    if (why)
      *why = std::move(w);
    return false;
  };
  if (!state.assembly)
    return refuse("the active document is not an assembly");
  Occurrence* occ = state.assembly->FindById(occId);
  if (occ == nullptr)
    return refuse("no occurrence with id " + std::to_string(occId));
  if (!material.empty()) {
    if (occ->refKind == OccurrenceRef::Assembly)
      return refuse("'" + occ->name +
                    "' places a sub-assembly: set the material on the parts "
                    "inside it");
    CADMaterials::MaterialType unused;
    if (!CADMaterials::MaterialLibrary::TryMaterialTypeFromString(material,
                                                                  unused))
      return refuse("unknown material '" + material + "' (known: " +
                    CADMaterials::MaterialLibrary::SlugList() + ")");
  }
  if (occ->material == material)
    return true;
  SnapshotAssembly(state);
  occ->material = material;
  state.assembly->BumpVersion();
  return true;
}

bool SetOccurrenceConfiguration(AppState& state, uint64_t occId,
                                uint64_t configurationId, std::string* why,
                                assembly::SolveReport* report, bool* solved) {
  const auto refuse = [why](std::string w) {
    if (why)
      *why = std::move(w);
    return false;
  };
  if (solved)
    *solved = false;
  if (!state.assembly)
    return refuse("the active document is not an assembly");
  Occurrence* occ = state.assembly->FindById(occId);
  if (occ == nullptr)
    return refuse("no occurrence with id " + std::to_string(occId));
  const bool placesPart = occ->refKind == OccurrenceRef::Part;
  if (configurationId != 0 && placesPart) {
    // A part in a configuration: it has to have it, and build in it --
    // said here, not as a part that silently vanishes from the assembly.
    std::string bad;
    if (ConfiguredPartDocument(state, occ->partId, configurationId, &bad) ==
        nullptr)
      return refuse("'" + occ->name + "': " + bad);
  } else if (configurationId != 0) {
    const AssemblyDocument* sub = AssemblyDocById(state, occ->partId);
    if (sub == nullptr)
      return refuse("'" + occ->name + "' places nothing that is loaded");
    if (sub->FindConfiguration(configurationId) == nullptr) {
      std::string known;
      for (const Configuration& cfg : sub->GetConfigurations())
        known += (known.empty() ? "" : ", ") + cfg.name + " (" +
                 std::to_string(cfg.id) + ")";
      return refuse("no configuration " + std::to_string(configurationId) +
                    " in '" + occ->name +
                    "'. Known: " + (known.empty() ? "(none)" : known));
    }
  }
  SnapshotAssembly(state);
  occ->configurationId = configurationId;
  state.assembly->BumpVersion();
  // A flexible unit moves under its configuration, so choosing another
  // re-solves it (v129), and so does a part: its faces, which its mates
  // anchor on, are another configuration's now. A rigid sub-assembly only
  // shows other parts.
  if (occ->flexible || placesPart) {
    const assembly::SolveReport rep = SolveActiveAssemblyReport(state);
    if (report)
      *report = rep;
    if (solved)
      *solved = true;
  }
  return true;
}

namespace {
std::string MateName(const Mate& m) {
  return m.name.empty() ? "mate " + std::to_string(m.id) : m.name;
}
std::string OccName(const Occurrence& o) {
  return o.name.empty() ? "occurrence " + std::to_string(o.id) : o.name;
}
} // namespace

const AssemblyDocument* FlexibleSub(AppState& state,
                                    const Occurrence& placement) {
  if (!placement.flexible || placement.refKind != OccurrenceRef::Assembly)
    return nullptr;
  return AssemblyDocById(state, placement.partId);
}

std::vector<DofMateChoice> DofMateChoices(AppState& state) {
  std::vector<DofMateChoice> out;
  if (!state.assembly)
    return out;
  const auto add = [&out](std::vector<uint64_t> path, const Mate& m,
                          std::string label) {
    const bool rot = assembly::MateHasDof(m.type, MateDof::Rotation);
    const bool slide = assembly::MateHasDof(m.type, MateDof::Offset);
    if (!rot && !slide)
      return;
    out.push_back({std::move(path), m.id, std::move(label), rot, slide});
  };
  for (const Mate& m : state.assembly->GetMates())
    add({}, m, MateName(m));
  for (const Occurrence& F : state.assembly->GetOccurrences())
    if (const AssemblyDocument* sub = FlexibleSub(state, F))
      for (const Mate& m : sub->GetMates())
        add({F.id}, m, OccName(F) + " / " + MateName(m));
  return out;
}

std::string MateLabelAt(AppState& state, const std::vector<uint64_t>& path,
                        uint64_t mate) {
  if (!state.assembly)
    return "(missing)";
  const Mate* m = assembly::MateAtPath(*state.assembly, path, mate,
                                       AssemblyResolverFor(state));
  if (m == nullptr)
    return "(missing)";
  if (path.empty())
    return MateName(*m);
  const Occurrence* F = state.assembly->FindById(path.front());
  return (F ? OccName(*F) : std::string("?")) + " / " + MateName(*m);
}

std::optional<glm::dmat4>
MateConnectorWorldAt(AppState& state, const std::vector<uint64_t>& path,
                     const Connector& c) {
  if (path.empty())
    return ConnectorWorldFrame(state, c);
  if (!state.assembly)
    return std::nullopt;
  const Occurrence* F = state.assembly->FindById(path.front());
  const AssemblyDocument* sub = F ? AssemblyDocById(state, F->partId) : nullptr;
  const Occurrence* inner = sub ? sub->FindById(c.occurrenceId) : nullptr;
  if (inner == nullptr)
    return std::nullopt;
  const auto local = assembly::ConnectorLocalFrame(
      c, *sub, PartResolverFor(state), nullptr, AssemblyResolverFor(state));
  if (!local)
    return std::nullopt;
  return OccurrenceMatrix(*F) * assembly::InnerPlacement(*F, *inner) * *local;
}

std::vector<const Occurrence*>
MovingInnerOccurrences(AppState& state, const Occurrence& placement) {
  std::vector<const Occurrence*> out;
  if (const AssemblyDocument* sub = FlexibleSub(state, placement))
    for (const Occurrence& o : sub->GetOccurrences())
      if (placement.FindInnerPose(o.id) != nullptr)
        out.push_back(&o);
  return out;
}

bool IsMovingInner(AppState& state, const Occurrence& placement,
                   uint64_t inner) {
  return FlexibleSub(state, placement) != nullptr &&
         placement.FindInnerPose(inner) != nullptr;
}

std::vector<const Mate*> InnerDofMates(AppState& state,
                                       const Occurrence& placement) {
  std::vector<const Mate*> out;
  if (const AssemblyDocument* sub = FlexibleSub(state, placement))
    for (const Mate& m : sub->GetMates())
      if (assembly::MateHasDof(m.type, MateDof::Rotation) ||
          assembly::MateHasDof(m.type, MateDof::Offset))
        out.push_back(&m);
  return out;
}

const Mate* InnerMateDriving(AppState& state, const Occurrence& placement,
                             uint64_t inner) {
  const AssemblyDocument* sub = FlexibleSub(state, placement);
  if (sub == nullptr)
    return nullptr;
  for (const Mate& m : sub->GetMates())
    if (m.b.occurrenceId == inner && m.type != MateType::Fastened &&
        !sub->MateSuppressedIn(placement.configurationId, m.id))
      return &m;
  return nullptr;
}

void SelectInnerOccurrence(AppState& state, uint64_t placement,
                           uint64_t inner) {
  SelectOccurrence(state, 0); // replaces the occurrence kind, as a click does
  if (placement != 0 && inner != 0) {
    SelectionItem it{SelectionItem::Kind::Occurrence, placement, {}};
    it.innerPath = {inner};
    state.selection.items.push_back(std::move(it));
  }
}

std::pair<uint64_t, uint64_t> SelectedInnerOccurrence(const AppState& state) {
  for (const SelectionItem& it : state.selection.items)
    if (it.kind == SelectionItem::Kind::Occurrence && it.innerPath.size() == 1)
      return {it.id, it.innerPath.front()};
  return {0, 0};
}

bool IsInnerOccurrenceSelected(const AppState& state, uint64_t placement,
                               uint64_t inner) {
  for (const SelectionItem& it : state.selection.items)
    if (it.kind == SelectionItem::Kind::Occurrence && it.id == placement &&
        it.innerPath.size() == 1 && it.innerPath.front() == inner)
      return true;
  return false;
}

glm::dmat4 InnerDisplayMatrix(AppState& state, const Occurrence& placement,
                              const Occurrence& inner) {
  return glm::translate(glm::dmat4(1.0),
                        ExplodeInnerOffset(state, placement.id, {inner.id})) *
         DisplayMatrix(state, placement) *
         assembly::InnerPlacement(placement, inner);
}

} // namespace ui
