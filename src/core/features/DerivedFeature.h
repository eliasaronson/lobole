// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Derived (docs/modeling/derived.md): a COPY of another part's
// bodies and sketches carried inside this part's file, the way the Import
// feature carries a STEP's, brought up to date at the project's gates
// (ui::RefreshLinks) while it follows its source.
//
// Regeneration never resolves the source document — Execute loads the
// snapshot from the host document's asset store — so a part with a Derived
// feature stays a pure function of its own file. Determinism, checkpoints,
// rollback, merge and the golden corpus see one more feature with data in
// it. There is no shared regeneration graph, no ordering, no cycle to solve
// at regen time; the one cycle that matters (A derives B derives A) is
// refused at creation, like an assembly occurrence that would loop.

#include "cad/SketchPlane.h"
#include "core/Feature.h"
#include "core/features/SketchFeature.h" // sketch::Entity

#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class Document;

// One body of the snapshot. The B-Rep itself is a document ASSET (binary
// BinTools, content-addressed: "derived-<hash>.brep"), kept at document scope
// for the same reason reference images are (v49): undo/redo rebuild features
// from JSON, and JSON never carries the bytes. Everything else — the source
// body's name, producer and element names — rides in the feature JSON, so
// the chain hash covers it and an update invalidates downstream features.
struct DerivedBody {
  std::string asset;
  std::string name;
  std::string material;
  uint64_t sourceCreatedBy = 0;
  // The SOURCE's names, unprefixed; Execute rewrites them under this
  // feature's id (DerivedFeature::DerivedName).
  ElementMap elementMap;
  bool hasDisplayColor = false;
  glm::vec3 displayColor{0.0f};
  // v182: the size bands the source stamped on this body (its ISO and
  // printed tolerances, a bearing's rings), as the SOURCE read them -- its
  // print process, not the host's -- with the source's face names
  // (`face.featureId` unused). fitband::Stamp puts them on the copy under
  // DerivedName, so a press fit on the source is one on the copy an
  // assembly places (docs/modeling/derived.md).
  std::vector<FitBand> fitBands;
};

// One copied SKETCH: the source sketch's solved entities (after its own
// Execute -- expressions, constraints, its external geometry) on its plane,
// both in the SOURCE part's coordinates; the feature's placement moves the
// plane. It is reference geometry in this part: a sketch here stands on its
// plane (SketchFeature::planeRef + planeRefSketch) and projects its entities
// as external geometry named "<derived id>/<source sketch id>/<entity id>"
// (DerivedFeature::SketchEntityName), never a profile on its own.
struct DerivedSketch {
  uint64_t sourceSketch = 0;
  std::string name;
  SketchPlane plane{PlaneType::XY};
  std::vector<sketch::Entity> entities;
};

class DerivedFeature : public Feature {
public:
  DerivedFeature(const std::string& name, uint64_t sourcePart);

  std::string TypeName() const override { return "Derived"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<DerivedFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<DerivedFeature>(executed);
  }

  // inherits the SOURCE body's material when its own is empty, so Document's
  // material re-stamp must leave it alone.
  bool StampsOwnMaterial() const override { return true; }

  bool Execute(RegenContext& ctx) override;
  // Names no feature of THIS history, on purpose. `sourcePart` is a part
  // slot, and `bodies` are created_by ids in the SOURCE document — neither is
  // an index into the tree this feature lives in. What the snapshot depends
  // on is its content key (`sourceKey`) instead.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  std::string WhyIncomplete() const override;

  // The part slot the snapshot was taken from, and its name at that time —
  // the name is what error messages say once the source is gone.
  uint64_t sourcePart = 0;
  std::string sourceName;
  // The source's configuration the copy is taken in (v133), by id; 0 = the
  // one the source has active when the snapshot is taken. One part with a
  // configuration per variant (a chess piece per rank) is then a family a
  // Derived can take any member of, where before it could only copy the
  // variant that happened to be showing.
  uint64_t sourceConfiguration = 0;
  // Which of the source's bodies, by their created_by id. Empty = all.
  std::vector<uint64_t> bodies;
  // ...and/or by NAME (v85). One Import feature creates every body of a
  // STEP — the Raspberry Pi camera arrives as 197, from pads to the lens —
  // so a created_by filter cannot pick one of them out. Empty = no name
  // filter; the two narrow together when both are given.
  std::vector<std::string> bodyNames;
  // The source's sketches to copy, by feature id and/or by name (v193).
  // A Derived that names a sketch copies only the bodies it names too: a
  // shared outline is wanted without the part it came from.
  std::vector<uint64_t> sketches;
  std::vector<std::string> sketchNames;
  // Where the copy sits in THIS part. Identity unless made from an
  // in-context edit, where it is OccMatrix(edited)^-1 * OccMatrix(source).
  glm::dvec3 position{0.0};
  glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
  // true: a ghost to draw on — absent from mass, BOM, export, CAM and
  // booleans. false: an ordinary body of this part (a mould from its
  // casting).
  bool reference = true;
  // v193: whether the copy keeps up with its source on its own. A
  // following link is retaken at the project's gates (ui::RefreshLinks);
  // off, it stays as taken until Update, and reads stale when behind.
  bool follow = true;
  // v193: the content key of what the snapshot holds (derived::KeyOf):
  // the copied bodies' exact B-Rep and the copied sketches' solved
  // entities. The copy is stale exactly when taking it again would give
  // another key. 0 = not recorded (a file from before v193): the key is
  // then computed from the snapshot itself.
  uint64_t sourceKey = 0;
  std::vector<DerivedBody> snapshot;
  std::vector<DerivedSketch> sketchSnapshot;

  glm::dmat4 Placement() const;
  bool IdentityPlacement() const;

  // "<this id>/<source created_by>/<source name>": the source's name under
  // this feature, which survives an update exactly when the source's own
  // name does. The created_by segment keeps two source bodies' identical
  // face names apart, as the Import feature's body ordinal does.
  std::string DerivedName(uint64_t sourceCreatedBy,
                          const std::string& sourceElementName) const;

  // "<this id>/<source sketch id>/<entity id>": a copied sketch entity, as
  // external geometry names it.
  std::string SketchEntityName(uint64_t sourceSketchId,
                               uint32_t entityId) const;
  // The copied sketch `sourceSketchId` (0 = the only one), or null.
  const DerivedSketch* FindSketch(uint64_t sourceSketchId) const;
  // Its plane in THIS part (the placement applied).
  SketchPlane PlacedPlane(const DerivedSketch& s) const;
  // The copied entity a SketchEntityName names, with its sketch; false when
  // the name is not one of this feature's sketch entities.
  bool ResolveSketchEntity(const std::string& name, const DerivedSketch*& sk,
                           const sketch::Entity*& entity) const;

private:
  // Decoded B-Reps by asset name. Assets are content-addressed, so a hit is
  // always the right shape; a Clone carries the cache along harmlessly.
  mutable std::map<std::string, CADShape> m_decoded;
};

namespace derived {

using PartResolver = std::function<const Document*(uint64_t)>;

// What taking `feature` from a source gives, before it is stored: the
// bodies (with their BinTools bytes), the sketches and their content key.
struct Taken {
  std::vector<DerivedBody> bodies;
  std::vector<std::vector<unsigned char>> bytes; // one per body
  std::vector<DerivedSketch> sketches;
  uint64_t key = 0;
};

// Takes `feature`'s filter from `source` (regenerated, at its own tip).
// Returns the refusal in words, or empty: a source with no bodies to give, a
// `bodies`/`body_names` filter the source does not satisfy (the ones it has
// are listed), a `sketches`/`sketch_names` filter naming a sketch it does
// not have or one that failed. The source's own reference bodies are never
// copied — a ghost of a ghost is not the source's geometry.
[[nodiscard]] std::string Take(const DerivedFeature& feature,
                               const Document& source, Taken& out);

// Take, then replace `feature`'s snapshot with it, storing the B-Reps as
// assets on `host` and recording the key. Does not touch
// `feature.sourceName`; the caller knows the slot's name.
[[nodiscard]] std::string TakeSnapshot(DerivedFeature& feature,
                                       const Document& source, Document& host);

// The content key of a snapshot as stored (sourceKey when recorded, else
// computed from the snapshot and `host`'s assets). Platform-stable: FNV
// over the B-Rep bytes and the named fields, never a JSON dump or std::hash,
// so a build that adds a key to a feature's JSON moves no copy.
[[nodiscard]] uint64_t StoredKey(const DerivedFeature& feature,
                                 const Document& host);

// Whether the copy is behind `source`: taking it again would give another
// key (or would be refused -- the source lost what it copies). The caller
// caches; this takes the snapshot afresh.
[[nodiscard]] bool IsStale(const DerivedFeature& feature,
                           const Document& source, const Document& host);

// Would `hostId` deriving from `sourceId` close a cycle over derived edges?
// Walks the source's Derived features -- the ones copying bodies and the ones
// copying sketches alike -- through `partById`; `why` (optional) names the
// chain.
[[nodiscard]] bool WouldCycle(uint64_t hostId, uint64_t sourceId,
                              const PartResolver& partById,
                              std::string* why = nullptr);

// The asset name a snapshot body's bytes get: "derived-<content hash>.brep".
std::string AssetNameFor(const std::vector<unsigned char>& bytes);

} // namespace derived
