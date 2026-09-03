#include "ui/DerivedActions.h"

#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/io/Serialization.h"
#include "ui/AppActions.h"
#include "ui/AssemblyActions.h"
#include "ui/PreviewFeature.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <set>

namespace ui {

namespace {

uint64_t ActiveSlotId(const AppState& state) {
  return state.activePart < state.parts.size()
             ? state.parts[state.activePart].id
             : 0;
}

std::string SlotName(const AppState& state, uint64_t slotId) {
  for (const PartSlot& s : state.parts)
    if (s.id == slotId)
      return s.name;
  return "part " + std::to_string(slotId);
}

// The context assembly of an in-context edit, stashed while the part tab is
// active (so its document pointer is live), and the edited occurrence.
struct Context {
  const AssemblyDocument* asmDoc = nullptr;
  const Occurrence* edited = nullptr;
};
Context InContext(AppState& state) {
  Context out;
  if (!state.inContext.assemblySlotId)
    return out;
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Assembly && s.id == state.inContext.assemblySlotId)
      out.asmDoc = s.assembly.get();
  if (out.asmDoc)
    out.edited = out.asmDoc->FindById(state.inContext.occurrenceId);
  if (!out.edited)
    out.asmDoc = nullptr;
  return out;
}

// Loaded parts other than the active one, by slot id.
std::vector<uint64_t> OtherParts(const AppState& state) {
  std::vector<uint64_t> out;
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part && s.id != ActiveSlotId(state))
      out.push_back(s.id);
  return out;
}

std::string Refusal(AppState& state, uint64_t sourceId) {
  const uint64_t host = ActiveSlotId(state);
  if (sourceId == host)
    return "a part cannot derive from itself";
  if (PartDocById(state, sourceId) == nullptr) {
    std::string have;
    for (uint64_t id : OtherParts(state))
      have += (have.empty() ? "" : ", ") + SlotName(state, id) + " (" +
              std::to_string(id) + ")";
    return "no part with id " + std::to_string(sourceId) + " in this project" +
           (have.empty() ? " (there is no other part to derive from)"
                         : "; the parts are: " + have);
  }
  std::string why;
  if (derived::WouldCycle(host, sourceId, PartResolverFor(state), &why))
    return why;
  return {};
}

} // namespace

std::vector<DerivedSource> DerivedSourceCandidates(AppState& state) {
  std::vector<DerivedSource> out;
  // In context: the neighbours first, placed where the ghosts are. A
  // sub-assembly occurrence contributes its leaf parts, each at its own
  // placement, which is what "derive this neighbour" has to mean for it.
  const Context ctx = InContext(state);
  if (ctx.asmDoc) {
    const glm::mat4 inv = glm::inverse(OccurrenceMatrix(*ctx.edited));
    auto flat = assembly::Flatten(*ctx.asmDoc, PartResolverFor(state),
                                  AssemblyResolverFor(state));
    if (flat) {
      for (const assembly::FlatInstance& inst : *flat) {
        if (inst.rootOccurrenceId == ctx.edited->id || !inst.visible)
          continue;
        if (!Refusal(state, inst.partId).empty())
          continue;
        DerivedSource s;
        s.slotId = inst.partId;
        s.name = inst.path.empty() ? SlotName(state, inst.partId) : inst.path;
        s.occurrenceId = inst.rootOccurrenceId;
        s.placement = inv * inst.world;
        out.push_back(std::move(s));
      }
    }
  }
  for (uint64_t id : OtherParts(state)) {
    if (!Refusal(state, id).empty())
      continue;
    DerivedSource s;
    s.slotId = id;
    s.name = SlotName(state, id);
    out.push_back(std::move(s));
  }
  return out;
}

std::string PointDerivedAt(AppState& state, DerivedFeature& feature,
                           const DerivedSource& source) {
  if (!state.document)
    return "no active part";
  if (const std::string why = Refusal(state, source.slotId); !why.empty())
    return why;
  const Document* src = PartDocById(state, source.slotId);
  DerivedFeature trial = feature;
  trial.sourcePart = source.slotId;
  trial.sourceName = SlotName(state, source.slotId);
  if (source.occurrenceId != 0) {
    trial.position = glm::vec3(source.placement[3]);
    trial.orientation =
        glm::normalize(glm::quat_cast(glm::mat3(source.placement)));
  }
  if (const std::string why =
          derived::TakeSnapshot(trial, *src, *state.document);
      !why.empty())
    return why;
  feature = std::move(trial);
  return {};
}

std::string SnapshotDerived(AppState& state, DerivedFeature& feature) {
  if (!state.document)
    return "no active part";
  if (feature.sourcePart == 0)
    return "'" + feature.GetName() +
           "' has no source (its link was broken); it is a frozen copy";
  const Document* src = PartDocById(state, feature.sourcePart);
  if (src == nullptr)
    return "the source of '" + feature.GetName() + "' (" + feature.sourceName +
           ", id " + std::to_string(feature.sourcePart) +
           ") is not in this project";
  DerivedFeature trial = feature;
  trial.sourceName = SlotName(state, feature.sourcePart);
  if (const std::string why =
          derived::TakeSnapshot(trial, *src, *state.document);
      !why.empty())
    return why;
  feature = std::move(trial);
  return {};
}

std::optional<bool> DerivedStale(AppState& state,
                                 const DerivedFeature& feature) {
  if (feature.sourcePart == 0)
    return false; // a broken link is a frozen copy: never behind anything
  return derived::IsStale(feature, PartResolverFor(state));
}

void CreateDerived(AppState& state) {
  if (!state.document)
    return;
  auto d = std::make_unique<DerivedFeature>("Derived", 0);
  d->reference = true;
  const std::vector<DerivedSource> candidates = DerivedSourceCandidates(state);
  // One candidate is the answer; several is a question the window asks.
  if (candidates.size() == 1) {
    const std::string why = PointDerivedAt(state, *d, candidates[0]);
    if (why.empty())
      d->SetName(candidates[0].name + " (derived)");
    else
      state.sketchEdit.actionMsg = why;
  }
  BeginPreviewFeature(state, std::move(d));
}

namespace {

std::string ReplaceDerived(AppState& state, uint64_t featureId,
                           const DerivedFeature& replacement) {
  size_t index = 0;
  Feature* f = state.document->FindFeatureById(featureId, &index);
  if (f == nullptr)
    return "no feature with id " + std::to_string(featureId);
  const nlohmann::json before = DocumentIO::FeatureToJson(*f);
  const nlohmann::json after = DocumentIO::FeatureToJson(replacement);
  if (before == after)
    return {};
  if (!state.commands->Execute(
          std::make_unique<ModifyFeatureCommand>(index, before, after),
          *state.document))
    return "the update did not apply";
  return {};
}

} // namespace

std::string UpdateDerived(AppState& state, uint64_t featureId) {
  if (!state.document)
    return "no active part";
  auto* d =
      dynamic_cast<DerivedFeature*>(state.document->FindFeatureById(featureId));
  if (d == nullptr)
    return "feature " + std::to_string(featureId) + " is not a Derived";
  DerivedFeature updated = *d;
  if (const std::string why = SnapshotDerived(state, updated); !why.empty())
    return why;
  return ReplaceDerived(state, featureId, updated);
}

size_t UpdateAllDerived(AppState& state, std::string* why) {
  if (!state.document)
    return 0;
  std::vector<uint64_t> ids;
  for (const auto& f : state.document->GetFeatures())
    if (dynamic_cast<const DerivedFeature*>(f.get()) != nullptr)
      ids.push_back(f->GetId());
  size_t updated = 0;
  for (uint64_t id : ids) {
    const auto* d = dynamic_cast<const DerivedFeature*>(
        state.document->FindFeatureById(id));
    if (d == nullptr || d->sourcePart == 0)
      continue; // a broken link has nothing to update from
    const std::string w = UpdateDerived(state, id);
    if (w.empty())
      updated++;
    else if (why && why->empty())
      *why = w;
  }
  return updated;
}

std::string BreakDerivedLink(AppState& state, uint64_t featureId) {
  if (!state.document)
    return "no active part";
  auto* d =
      dynamic_cast<DerivedFeature*>(state.document->FindFeatureById(featureId));
  if (d == nullptr)
    return "feature " + std::to_string(featureId) + " is not a Derived";
  if (d->sourcePart == 0)
    return "'" + d->GetName() + "' is already unlinked";
  DerivedFeature frozen = *d;
  frozen.sourceName = "(unlinked from " + d->sourceName + ")";
  frozen.sourcePart = 0;
  return ReplaceDerived(state, featureId, frozen);
}

std::vector<const DerivedFeature*> StaleDerived(AppState& state,
                                                const Document& doc) {
  std::vector<const DerivedFeature*> out;
  for (const auto& f : doc.GetFeatures()) {
    const auto* d = dynamic_cast<const DerivedFeature*>(f.get());
    if (d != nullptr && DerivedStale(state, *d) == std::optional<bool>{true})
      out.push_back(d);
  }
  return out;
}

} // namespace ui
