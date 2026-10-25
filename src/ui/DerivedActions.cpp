// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/DerivedActions.h"
#include "ui/DrawingActions.h" // ConfiguredPartDocument

#include "common/Logger.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/features/SketchFeature.h"
#include "core/io/ReadContext.h"
#include "core/io/Serialization.h"
#include "ui/AssemblyActions.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h" // BeginPreparedSketch

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace ui {

namespace {

uint64_t ActiveSlotId(const AppState& state) {
  return state.activePart >= 0 &&
                 static_cast<size_t>(state.activePart) < state.parts.size()
             ? state.parts[state.activePart].id
             : 0;
}

std::string SlotName(const AppState& state, uint64_t slotId) {
  for (const PartSlot& s : state.parts)
    if (s.id == slotId)
      return s.name;
  return "part " + std::to_string(slotId);
}

// The part a slot id names, active or stashed, WITHOUT the gate audit: the
// gate's own reads (and the existence and cycle checks, which read no
// geometry) are not a part handed to another document.
Document* RawPart(AppState& state, uint64_t slotId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].kind != DocKind::Part || state.parts[i].id != slotId)
      continue;
    return static_cast<int>(i) == state.activePart ? state.document.get()
                                                   : state.parts[i].doc.get();
  }
  return nullptr;
}

// The slot id of a part document, 0 when it is none of the project's.
uint64_t SlotIdOf(AppState& state, const Document& doc) {
  for (size_t i = 0; i < state.parts.size(); i++)
    if (state.parts[i].kind == DocKind::Part &&
        RawPart(state, state.parts[i].id) == &doc)
      return state.parts[i].id;
  return 0;
}

// While the gate or the audit is reading, the resolvers it goes through do
// not audit again: the gate reads parts it is about to bring up to date.
int g_quiet = 0;
struct Quiet {
  Quiet() { g_quiet++; }
  ~Quiet() { g_quiet--; }
  Quiet(const Quiet&) = delete;
  Quiet& operator=(const Quiet&) = delete;
};

// The source a link reads: the part in the configuration the copy is taken
// in (the configured copy ConfiguredPartDocument keeps). Null, with `why`,
// when it is not there.
const Document* SourceOf(AppState& state, const DerivedFeature& d,
                         std::string* why) {
  const Quiet quiet;
  if (RawPart(state, d.sourcePart) == nullptr) {
    if (why)
      *why = "its source '" + d.sourceName + "' (id " +
             std::to_string(d.sourcePart) + ") is not in this project";
    return nullptr;
  }
  std::string err;
  const Document* src =
      ConfiguredPartDocument(state, d.sourcePart, d.sourceConfiguration, &err);
  if (src == nullptr && why)
    *why = "its source '" + d.sourceName + "': " + err;
  return src;
}

// A source that is rolled back is mid-edit: what it would give is not the
// part, so it is not taken (docs/modeling/derived.md#copies-that-keep-up).
bool RolledBack(const Document& doc) {
  return doc.Rollback() >= 0 || doc.ViewRollback() >= 0;
}

// What taking a link now would give, by the source's stamp: the key, or the
// refusal. Cached, so a check on a source that has not moved takes nothing.
struct CurrentTake {
  bool ok = false;
  uint64_t key = 0;
  std::string why;
};

std::string StampOf(AppState& state, const DerivedFeature& d) {
  const Document* live = RawPart(state, d.sourcePart);
  if (live == nullptr)
    return {};
  nlohmann::json filter = {{"b", d.bodies},
                           {"bn", d.bodyNames},
                           {"s", d.sketches},
                           {"sn", d.sketchNames},
                           {"c", d.sourceConfiguration}};
  return std::to_string(live->InstanceId()) + ":" +
         std::to_string(live->Version()) + ":" +
         std::to_string(live->SaveStamp()) + ":" +
         std::to_string(live->ConfigurationsRevision()) + ":" + filter.dump();
}

std::map<std::string, CurrentTake>& TakeCache() {
  static std::map<std::string, CurrentTake> cache;
  return cache;
}

CurrentTake Current(AppState& state, const DerivedFeature& d) {
  const std::string stamp = StampOf(state, d);
  auto& cache = TakeCache();
  if (!stamp.empty())
    if (const auto it = cache.find(stamp); it != cache.end())
      return it->second;
  CurrentTake out;
  if (const Document* src = SourceOf(state, d, &out.why)) {
    derived::Taken taken;
    out.why = derived::Take(d, *src, taken);
    out.ok = out.why.empty();
    out.key = taken.key;
  }
  if (!stamp.empty()) {
    if (cache.size() > 4096)
      cache.clear();
    cache[stamp] = out;
  }
  return out;
}

// Links the gate tried and could not take, by host instance and feature, at
// the source stamp it tried at: the audit does not count those (the gate
// was passed; the link stays stale and says why).
std::map<std::pair<uint64_t, uint64_t>, std::string>& Refused() {
  static std::map<std::pair<uint64_t, uint64_t>, std::string> refused;
  return refused;
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

derived::PartResolver RawResolver(AppState& state) {
  return
      [&state](uint64_t id) -> const Document* { return RawPart(state, id); };
}

std::string Refusal(AppState& state, uint64_t hostId, uint64_t sourceId) {
  if (sourceId == hostId)
    return "a part cannot derive from itself";
  if (RawPart(state, sourceId) == nullptr) {
    std::string have;
    for (uint64_t id : OtherParts(state))
      have += (have.empty() ? "" : ", ") + SlotName(state, id) + " (" +
              std::to_string(id) + ")";
    return "no part with id " + std::to_string(sourceId) + " in this project" +
           (have.empty() ? " (there is no other part to derive from)"
                         : "; the parts are: " + have);
  }
  std::string why;
  if (derived::WouldCycle(hostId, sourceId, RawResolver(state), &why))
    return why;
  return {};
}

// The undo history `part` keeps: the active tab's, or its stashed slot's.
CommandStack* CommandsOf(AppState& state, const Document& part) {
  if (state.document.get() == &part)
    return state.commands.get();
  for (PartSlot& slot : state.parts)
    if (slot.doc.get() == &part) {
      if (!slot.commands)
        slot.commands = std::make_unique<CommandStack>();
      return slot.commands.get();
    }
  return nullptr;
}

// One command replacing each of `replacements` (feature id -> the updated
// feature) in `host`, on its own undo stack: one undo step however many.
std::string
ReplaceAll(AppState& state, Document& host,
           const std::vector<std::pair<uint64_t, DerivedFeature>>& replacements,
           const std::string& description) {
  CommandStack* stack = CommandsOf(state, host);
  if (stack == nullptr)
    return "that part has no undo history of its own";
  std::vector<std::unique_ptr<Command>> parts;
  for (const auto& [id, updated] : replacements) {
    size_t index = 0;
    Feature* f = host.FindFeatureById(id, &index);
    if (f == nullptr)
      return "no feature with id " + std::to_string(id);
    nlohmann::json before = DocumentIO::FeatureToJson(*f);
    nlohmann::json after = DocumentIO::FeatureToJson(updated);
    if (before != after)
      parts.push_back(std::make_unique<ModifyFeatureCommand>(
          index, std::move(before), std::move(after)));
  }
  if (parts.empty())
    return {};
  std::unique_ptr<Command> command =
      parts.size() == 1
          ? std::move(parts.front())
          : std::make_unique<CompositeCommand>(std::move(parts), description);
  if (!stack->Execute(std::move(command), host))
    return "the update did not apply";
  return {};
}

// True while the person is in the middle of something on the active part --
// a sketch open, a tool's preview feature -- which an update must not pull
// out from under them: its features are replaced, and the session holds
// them.
bool ActivePartBusy(AppState& state) {
  return state.sketchEdit.activeSketchId != 0 || HasPendingFeature(state);
}

} // namespace

std::vector<DerivedSource> DerivedSourceCandidates(AppState& state) {
  std::vector<DerivedSource> out;
  const uint64_t host = ActiveSlotId(state);
  // In context: the neighbours first, placed where the ghosts are. A
  // sub-assembly occurrence contributes its leaf parts, each at its own
  // placement, which is what "derive this neighbour" has to mean for it.
  const Context ctx = InContext(state);
  if (ctx.asmDoc) {
    const glm::dmat4 inv = glm::inverse(OccurrenceMatrix(*ctx.edited));
    auto flat = assembly::Flatten(*ctx.asmDoc, PartResolverFor(state),
                                  AssemblyResolverFor(state));
    if (flat) {
      for (const assembly::FlatInstance& inst : *flat) {
        if (inst.rootOccurrenceId == ctx.edited->id || !inst.visible)
          continue;
        if (!Refusal(state, host, inst.partId).empty())
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
    if (!Refusal(state, host, id).empty())
      continue;
    DerivedSource s;
    s.slotId = id;
    s.name = SlotName(state, id);
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<SourceSketch> SourceSketches(AppState& state, uint64_t slotId) {
  std::vector<SourceSketch> out;
  if (const Document* doc = RawPart(state, slotId))
    for (const auto& f : doc->GetFeatures())
      if (dynamic_cast<const SketchFeature*>(f.get()) != nullptr)
        out.push_back({f->GetId(), f->GetName()});
  return out;
}

std::string PointDerivedAt(AppState& state, DerivedFeature& feature,
                           const DerivedSource& source) {
  if (!state.document)
    return "no active part";
  if (const std::string why =
          Refusal(state, ActiveSlotId(state), source.slotId);
      !why.empty())
    return why;
  std::string why;
  const Document* src = ConfiguredPartDocument(
      state, source.slotId, feature.sourceConfiguration, &why);
  if (src == nullptr)
    return why;
  DerivedFeature trial = feature;
  trial.sourcePart = source.slotId;
  trial.sourceName = SlotName(state, source.slotId);
  if (source.occurrenceId != 0) {
    trial.position = glm::dvec3(source.placement[3]);
    trial.orientation =
        glm::normalize(glm::quat_cast(glm::dmat3(source.placement)));
  }
  if (const std::string why =
          derived::TakeSnapshot(trial, *src, *state.document);
      !why.empty())
    return why;
  feature = std::move(trial);
  return {};
}

std::string SnapshotDerived(AppState& state, DerivedFeature& feature,
                            Document& host) {
  if (feature.sourcePart == 0)
    return "'" + feature.GetName() +
           "' has no source (its link was broken); it is a frozen copy";
  if (RawPart(state, feature.sourcePart) == nullptr)
    return "the source of '" + feature.GetName() + "' (" + feature.sourceName +
           ", id " + std::to_string(feature.sourcePart) +
           ") is not in this project";
  // A cycle is refused at the update as at the creation: a file merged or
  // opened from elsewhere can close one that no creation here allowed.
  if (const uint64_t hostId = SlotIdOf(state, host); hostId != 0) {
    std::string why;
    if (derived::WouldCycle(hostId, feature.sourcePart, RawResolver(state),
                            &why))
      return "'" + feature.GetName() + "': " + why;
  }
  std::string why;
  const Document* src = SourceOf(state, feature, &why);
  if (src == nullptr)
    return "'" + feature.GetName() + "': " + why;
  DerivedFeature trial = feature;
  trial.sourceName = SlotName(state, feature.sourcePart);
  if (const std::string why = derived::TakeSnapshot(trial, *src, host);
      !why.empty())
    return why;
  feature = std::move(trial);
  return {};
}

namespace {

std::optional<bool> Stale(AppState& state, const DerivedFeature& feature,
                          const Document& host, std::string* why,
                          std::set<uint64_t>& visiting) {
  if (feature.sourcePart == 0)
    return false; // a broken link is a frozen copy: never behind anything
  std::string reason;
  const Document* src = SourceOf(state, feature, &reason);
  if (src == nullptr)
    return std::nullopt;
  const CurrentTake now = Current(state, feature);
  if (!now.ok) {
    if (why)
      *why = "its source cannot give it: " + now.why;
    return true;
  }
  if (now.key != derived::StoredKey(feature, host)) {
    if (why)
      *why = "'" + feature.sourceName + "' has changed since it was taken";
    return true;
  }
  // Transitive: a copy of a part that is itself behind is behind. Read on
  // the live source, whose own links are the ones that move.
  if (!visiting.insert(feature.sourcePart).second)
    return false; // a cycle: refused elsewhere, not a staleness
  const Document* live = RawPart(state, feature.sourcePart);
  if (live != nullptr)
    for (const auto& f : live->GetFeatures()) {
      const auto* up = dynamic_cast<const DerivedFeature*>(f.get());
      if (up == nullptr)
        continue;
      std::string upWhy;
      if (Stale(state, *up, *live, &upWhy, visiting) ==
          std::optional<bool>{true}) {
        if (why)
          *why = "its source '" + feature.sourceName + "' is itself behind ('" +
                 up->GetName() + "': " + upWhy + ")";
        visiting.erase(feature.sourcePart);
        return true;
      }
    }
  visiting.erase(feature.sourcePart);
  return false;
}

} // namespace

std::optional<bool> DerivedStale(AppState& state, const DerivedFeature& feature,
                                 const Document* host, std::string* why) {
  if (host == nullptr)
    host = state.document.get();
  if (host == nullptr)
    return std::nullopt;
  std::set<uint64_t> visiting;
  if (const uint64_t id = SlotIdOf(state, *host); id != 0)
    visiting.insert(id);
  return Stale(state, feature, *host, why, visiting);
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

std::string UpdateDerived(AppState& state, uint64_t featureId) {
  if (!state.document)
    return "no active part";
  auto* d =
      dynamic_cast<DerivedFeature*>(state.document->FindFeatureById(featureId));
  if (d == nullptr)
    return "feature " + std::to_string(featureId) + " is not a Derived";
  DerivedFeature updated = *d;
  if (const std::string why = SnapshotDerived(state, updated, *state.document);
      !why.empty())
    return why;
  return ReplaceAll(state, *state.document, {{featureId, updated}},
                    "Update from source");
}

void UpdateAllDerived(AppState& state, std::vector<uint64_t>& updated,
                      std::vector<std::string>& skipped) {
  updated.clear();
  skipped.clear();
  if (!state.document)
    return;
  std::vector<std::pair<uint64_t, DerivedFeature>> replacements;
  for (const auto& f : state.document->GetFeatures()) {
    const auto* d = dynamic_cast<const DerivedFeature*>(f.get());
    if (d == nullptr || d->sourcePart == 0)
      continue; // a broken link has nothing to update from
    DerivedFeature copy = *d;
    if (const std::string why = SnapshotDerived(state, copy, *state.document);
        !why.empty()) {
      skipped.push_back(d->GetName() + ": " + why);
      continue;
    }
    updated.push_back(d->GetId());
    replacements.emplace_back(d->GetId(), std::move(copy));
  }
  if (const std::string why = ReplaceAll(
          state, *state.document, replacements,
          "Update " + std::to_string(replacements.size()) + " derived copies");
      !why.empty()) {
    updated.clear();
    skipped.push_back(why);
  }
}

std::string SetDerivedFollow(AppState& state, uint64_t featureId, bool follow) {
  if (!state.document)
    return "no active part";
  auto* d =
      dynamic_cast<DerivedFeature*>(state.document->FindFeatureById(featureId));
  if (d == nullptr)
    return "feature " + std::to_string(featureId) + " is not a Derived";
  DerivedFeature changed = *d;
  changed.follow = follow;
  return ReplaceAll(state, *state.document, {{featureId, changed}},
                    follow ? "Follow source" : "Stop following source");
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
  return ReplaceAll(state, *state.document, {{featureId, frozen}},
                    "Break link");
}

std::vector<const DerivedFeature*> StaleDerived(AppState& state,
                                                const Document& doc) {
  std::vector<const DerivedFeature*> out;
  for (const auto& f : doc.GetFeatures()) {
    const auto* d = dynamic_cast<const DerivedFeature*>(f.get());
    if (d != nullptr &&
        DerivedStale(state, *d, &doc) == std::optional<bool>{true})
      out.push_back(d);
  }
  return out;
}

// ---- Sketches ---------------------------------------------------------------

std::string SketchOnDerived(AppState& state, uint64_t derivedId,
                            uint64_t sourceSketchId) {
  if (!state.document)
    return "no active part";
  const auto* d = dynamic_cast<const DerivedFeature*>(
      state.document->FindFeatureById(derivedId));
  if (d == nullptr)
    return "feature " + std::to_string(derivedId) + " is not a Derived";
  const DerivedSketch* copied = d->FindSketch(sourceSketchId);
  if (copied == nullptr) {
    std::string have;
    for (const DerivedSketch& s : d->sketchSnapshot)
      have += (have.empty() ? "" : ", ") + s.name + " (" +
              std::to_string(s.sourceSketch) + ")";
    return "'" + d->GetName() + "' copies no sketch " +
           std::to_string(sourceSketchId) +
           (have.empty() ? std::string(" (it copies none)")
                         : " (it copies: " + have + ")");
  }
  auto sk = std::make_unique<SketchFeature>("Sketch on " + copied->name,
                                            d->PlacedPlane(*copied));
  sk->planeRef = derivedId;
  sk->planeRefSketch = copied->sourceSketch;
  for (const sketch::Entity& e : copied->entities)
    if (!std::holds_alternative<sketch::Text>(e))
      sk->AddExternalGeometry(
          ElementRef{derivedId, d->SketchEntityName(copied->sourceSketch,
                                                    sketch::EntityId(e))},
          sketch::ExternalKind::Edge);
  if (BeginPreparedSketch(state, std::move(sk)) == 0)
    return state.sketchEdit.actionMsg.empty() ? "the sketch was refused"
                                              : state.sketchEdit.actionMsg;
  return {};
}

std::string DerivedSketchBefore(AppState& state, uint64_t sourcePart,
                                uint64_t sourceSketch, size_t& beforeIndex,
                                uint64_t& derivedId) {
  derivedId = 0;
  if (!state.document)
    return "no active part";
  Document& doc = *state.document;
  // One already there, earlier, copying that sketch where it is.
  for (size_t i = 0; i < beforeIndex && i < doc.FeatureCount(); i++) {
    const auto* d = dynamic_cast<const DerivedFeature*>(doc.GetFeature(i));
    if (d != nullptr && d->sourcePart == sourcePart &&
        d->sourceConfiguration == 0 && d->IdentityPlacement() &&
        std::any_of(d->sketchSnapshot.begin(), d->sketchSnapshot.end(),
                    [&](const DerivedSketch& s) {
                      return s.sourceSketch == sourceSketch;
                    })) {
      derivedId = d->GetId();
      return {};
    }
  }
  if (const std::string why = readctx::CheckFeatureRoom(doc, 1); !why.empty())
    return why;
  auto d = std::make_unique<DerivedFeature>("Derived", 0);
  d->sketches = {sourceSketch};
  DerivedSource src;
  src.slotId = sourcePart;
  if (const std::string why = PointDerivedAt(state, *d, src); !why.empty())
    return why;
  d->SetName(d->sketchSnapshot.front().name + " from " + d->sourceName);
  if (!state.commands->Execute(
          std::make_unique<AddFeatureCommand>(std::move(d)), doc))
    return "the copy could not be added";
  const size_t added = doc.FeatureCount() - 1;
  derivedId = doc.GetFeature(added)->GetId();
  if (beforeIndex < added) {
    if (!state.commands->Execute(
            std::make_unique<MoveFeatureCommand>(added, beforeIndex), doc))
      return "the copy could not be placed before the sketch";
    beforeIndex++;
  }
  return {};
}

// ---- The gate ---------------------------------------------------------------

std::vector<LinkUpdate> RefreshLinks(AppState& state, bool notice) {
  std::vector<LinkUpdate> out;
  const Quiet quiet;
  // The parts and the links between them: P depends on S when P has a
  // following link to S. Ordered so a source is brought up to date before
  // what copies it.
  std::vector<uint64_t> parts;
  std::map<uint64_t, std::set<uint64_t>> needs; // part -> its sources
  for (const PartSlot& slot : state.parts) {
    if (slot.kind != DocKind::Part)
      continue;
    const Document* doc = RawPart(state, slot.id);
    if (doc == nullptr)
      continue;
    parts.push_back(slot.id);
    for (const auto& f : doc->GetFeatures())
      if (const auto* d = dynamic_cast<const DerivedFeature*>(f.get());
          d != nullptr && d->follow && d->sourcePart != 0 &&
          d->sourcePart != slot.id && RawPart(state, d->sourcePart) != nullptr)
        needs[slot.id].insert(d->sourcePart);
  }
  if (needs.empty())
    return out;
  std::vector<uint64_t> order;
  std::set<uint64_t> done;
  for (bool progress = true; progress;) {
    progress = false;
    for (uint64_t id : parts) {
      if (done.contains(id))
        continue;
      const std::set<uint64_t>& srcs = needs[id];
      if (std::all_of(srcs.begin(), srcs.end(),
                      [&](uint64_t s) { return done.contains(s); })) {
        order.push_back(id);
        done.insert(id);
        progress = true;
      }
    }
  }
  // What is left is in a cycle: its links are not taken (each was refused
  // where it was made; a file from elsewhere can still close one).

  for (uint64_t partId : order) {
    Document* host = RawPart(state, partId);
    if (host == nullptr || needs[partId].empty())
      continue;
    if (host == state.document.get() && ActivePartBusy(state))
      continue; // taken at the next gate, once the session ends
    std::vector<std::pair<uint64_t, DerivedFeature>> replacements;
    for (const auto& f : host->GetFeatures()) {
      const auto* d = dynamic_cast<const DerivedFeature*>(f.get());
      if (d == nullptr || !d->follow || d->sourcePart == 0)
        continue;
      const Document* live = RawPart(state, d->sourcePart);
      if (live == nullptr || RolledBack(*live))
        continue; // stays stale, and says why where staleness is read
      const CurrentTake now = Current(state, *d);
      const std::pair<uint64_t, uint64_t> memo{host->InstanceId(), d->GetId()};
      if (now.ok && now.key == derived::StoredKey(*d, *host)) {
        Refused().erase(memo);
        continue; // up to date
      }
      DerivedFeature updated = *d;
      if (const std::string why = SnapshotDerived(state, updated, *host);
          !why.empty()) {
        Refused()[memo] = StampOf(state, *d);
        LOG_WARN("link '{}' in '{}' not taken: {}", d->GetName(),
                 SlotName(state, partId), why);
        continue;
      }
      Refused().erase(memo);
      replacements.emplace_back(d->GetId(), std::move(updated));
    }
    if (replacements.empty())
      continue;
    std::map<uint64_t, std::string> errorsBefore;
    for (const auto& f : host->GetFeatures())
      errorsBefore[f->GetId()] = f->GetError();
    LinkUpdate update;
    update.partId = partId;
    update.partName = SlotName(state, partId);
    for (const auto& [id, feature] : replacements)
      update.features.push_back(id);
    if (const std::string why = ReplaceAll(
            state, *host, replacements,
            "Update " + std::to_string(replacements.size()) + " linked copies");
        !why.empty()) {
      LOG_WARN("links of '{}' not updated: {}", update.partName, why);
      continue;
    }
    for (const auto& f : host->GetFeatures()) {
      const auto it = errorsBefore.find(f->GetId());
      if (!f->GetError().empty() &&
          (it == errorsBefore.end() || it->second.empty()))
        update.broken.push_back({f->GetId(), f->GetName(), f->GetError()});
    }
    LOG_INFO("links: '{}' took {} update(s){}", update.partName,
             update.features.size(),
             update.broken.empty()
                 ? std::string()
                 : ", " + std::to_string(update.broken.size()) + " broken");
    out.push_back(std::move(update));
  }
  if (notice && !out.empty()) {
    state.linksNotice = LinksNoticeText(state, out);
    state.linksNoticeUntil = -2.0;
  }
  return out;
}

nlohmann::json LinksUpdatedJson(const std::vector<LinkUpdate>& updates) {
  nlohmann::json out = nlohmann::json::array();
  for (const LinkUpdate& u : updates) {
    nlohmann::json broken = nlohmann::json::array();
    for (const LinkUpdate::Broken& b : u.broken)
      broken.push_back(
          {{"feature_id", b.featureId}, {"name", b.name}, {"error", b.error}});
    out.push_back({{"part", u.partName},
                   {"part_id", u.partId},
                   {"features", u.features},
                   {"broken", std::move(broken)}});
  }
  return out;
}

std::string LinksNoticeText(AppState& state,
                            const std::vector<LinkUpdate>& updates) {
  std::string text;
  for (const LinkUpdate& u : updates) {
    std::set<std::string> sources;
    if (const Document* host = RawPart(state, u.partId))
      for (uint64_t id : u.features)
        if (const auto* d =
                dynamic_cast<const DerivedFeature*>(host->FindFeatureById(id)))
          sources.insert(d->sourceName);
    std::string from;
    for (const std::string& s : sources)
      from += (from.empty() ? "" : ", ") + s;
    std::string line = u.partName + " updated from " + from;
    for (size_t i = 0; i < u.broken.size(); i++)
      line += (i == 0 ? ": " : ", ") + u.broken[i].name + " failed";
    text += (text.empty() ? "" : "; ") + line;
  }
  return text;
}

// ---- The audit --------------------------------------------------------------

namespace {

bool g_audit = false;
int g_misses = 0;
std::string g_lastMiss;

bool AbortOnMiss() {
  static const bool abort = [] {
    const char* v = std::getenv("LOBOLE_LINK_AUDIT");
    return v != nullptr && std::string(v) == "1";
  }();
  return abort;
}

// The first following link of `doc`, or of a part upstream of it, that the
// gate would take now; empty when there is none.
std::string WouldTake(AppState& state, const Document& doc,
                      std::set<const Document*>& seen) {
  if (!seen.insert(&doc).second)
    return {};
  if (&doc == state.document.get() && ActivePartBusy(state))
    return {};
  for (const auto& f : doc.GetFeatures()) {
    const auto* d = dynamic_cast<const DerivedFeature*>(f.get());
    if (d == nullptr || !d->follow || d->sourcePart == 0)
      continue;
    const Document* live = RawPart(state, d->sourcePart);
    if (live == nullptr || RolledBack(*live))
      continue;
    const CurrentTake now = Current(state, *d);
    const auto memo = Refused().find({doc.InstanceId(), d->GetId()});
    const bool refusedHere =
        memo != Refused().end() && memo->second == StampOf(state, *d);
    if (!refusedHere && (!now.ok || now.key != derived::StoredKey(*d, doc)))
      return "'" + d->GetName() + "' is behind '" + d->sourceName + "'";
    if (std::string up = WouldTake(state, *live, seen); !up.empty())
      return up;
  }
  return {};
}

} // namespace

void SetLinkAudit(bool on) { g_audit = on; }
bool LinkAuditOn() { return g_audit || AbortOnMiss(); }
int LinkAuditMisses() { return g_misses; }
std::string LinkAuditLastMiss() { return g_lastMiss; }
void ResetLinkAudit() {
  g_misses = 0;
  g_lastMiss.clear();
}

void AuditPartRead(AppState& state, const Document& part) {
  if (g_quiet > 0 || !LinkAuditOn())
    return;
  const Quiet quiet;
  std::set<const Document*> seen;
  const std::string why = WouldTake(state, part, seen);
  if (why.empty())
    return;
  const uint64_t id = SlotIdOf(state, part);
  g_misses++;
  g_lastMiss = "part '" + SlotName(state, id) +
               "' was read without passing a gate: " + why +
               " (docs/modeling/derived.md#nothing-may-miss-a-gate)";
  LOG_ERROR("link audit: {}", g_lastMiss);
  if (AbortOnMiss()) {
    std::fprintf(stderr, "link audit: %s\n", g_lastMiss.c_str());
    std::fflush(stderr);
    std::abort();
  }
}

} // namespace ui
