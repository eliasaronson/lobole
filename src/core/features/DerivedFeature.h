#pragma once

// Derived (docs/modeling/derived.md): a COPY of another part's
// bodies carried inside this part's file, the way the Import feature carries
// a STEP's, updated on request the way a drawing is.
//
// Regeneration never resolves the source document — Execute loads the
// snapshot from the host document's asset store — so a part with a Derived
// feature stays a pure function of its own file. Determinism, checkpoints,
// rollback, merge and the golden corpus see one more feature with data in
// it. There is no shared regeneration graph, no ordering, no cycle to solve
// at regen time; the one cycle that matters (A derives B derives A) is
// refused at creation, like an assembly occurrence that would loop.

#include "core/Feature.h"

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
};

class DerivedFeature : public Feature {
public:
  DerivedFeature(const std::string& name, uint64_t sourcePart);

  std::string TypeName() const override { return "Derived"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<DerivedFeature>(*this);
  }

  // inherits the SOURCE body's material when its own is empty, so Document's
  // material re-stamp must leave it alone.
  bool StampsOwnMaterial() const override { return true; }

  bool Execute(RegenContext& ctx) override;
  // Names no feature of THIS history, on purpose. `sourcePart` is a part
  // slot, and `bodies` are created_by ids in the SOURCE document — neither is
  // an index into the tree this feature lives in. What the snapshot depends
  // on is tracked by the chain hash instead (D1).
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
  // Where the copy sits in THIS part. Identity unless made from an
  // in-context edit, where it is OccMatrix(edited)^-1 * OccMatrix(source).
  glm::vec3 position{0.0f};
  glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
  // true: a ghost to draw on — absent from mass, BOM, export, CAM and
  // booleans. false: an ordinary body of this part (a mould from its
  // casting).
  bool reference = true;
  // The source's Document::TipChainHash when the snapshot was taken. The
  // stale check compares against the source's current one; Version() would
  // not do, it restarts at 0 on every load.
  size_t sourceChainHash = 0;
  std::vector<DerivedBody> snapshot;

  glm::mat4 Placement() const;
  bool IdentityPlacement() const;

  // "<this id>/<source created_by>/<source name>": the source's name under
  // this feature, which survives an update exactly when the source's own
  // name does. The created_by segment keeps two source bodies' identical
  // face names apart, as the Import feature's body ordinal does.
  std::string DerivedName(uint64_t sourceCreatedBy,
                          const std::string& sourceElementName) const;

private:
  // Decoded B-Reps by asset name. Assets are content-addressed, so a hit is
  // always the right shape; a Clone carries the cache along harmlessly.
  mutable std::map<std::string, CADShape> m_decoded;
};

namespace derived {

using PartResolver = std::function<const Document*(uint64_t)>;

// Replaces `feature`'s snapshot with `source`'s current bodies (regenerated,
// at the source's own tip), storing their B-Reps as assets on `host`. Returns
// the refusal in words, or empty on success: a source with no bodies, a
// `bodies` filter naming a created_by the source does not have (the ones it
// has are listed). The source's own reference bodies are never copied — a
// ghost of a ghost is not the source's geometry. Does not touch
// `feature.sourceName`; the caller knows the slot's name.
[[nodiscard]] std::string TakeSnapshot(DerivedFeature& feature,
                                       const Document& source, Document& host);

// Whether the source has moved since the snapshot was taken: nullopt when
// the source is not loaded (nothing to compare against — not stale, not
// fresh, unknown), otherwise the chain-hash comparison. Never auto-updates
// (the FEM rule).
[[nodiscard]] std::optional<bool> IsStale(const DerivedFeature& feature,
                                          const PartResolver& partById);

// Would `hostId` deriving from `sourceId` close a cycle over derived edges?
// Walks the source's Derived features through `partById`; `why` (optional)
// names the chain.
[[nodiscard]] bool WouldCycle(uint64_t hostId, uint64_t sourceId,
                              const PartResolver& partById,
                              std::string* why = nullptr);

// The asset name a snapshot body's bytes get: "derived-<content hash>.brep".
std::string AssetNameFor(const std::vector<unsigned char>& bytes);

} // namespace derived
