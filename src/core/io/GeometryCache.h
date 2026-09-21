#pragma once

#include <string>

class Document;

namespace DocumentIO {

// Baked geometry: the built bodies stored beside the recipe, so opening a
// project is a load rather than a rebuild.
//
// A .dsgn directory is a recipe — a feature list that is re-executed on every
// open. That is honest and diffable, and on a 148-feature part it cost 259 s
// against 0.4 s to save it (docs/performance/scale-round.md). Every commercial
// CAD file stores the resulting B-Rep alongside the history for exactly this
// reason, and so does this:
//
//   <project>.dsgn/geometry/
//     index.json    format version, the document's tip chain hash, the OCC
//                   version, and per body its name/material/producer plus the
//                   element map (face and edge names, transient in memory)
//     0.brep …      one binary B-Rep per body
//
// The blob is only ever an accelerator. It is keyed by the same chain hash the
// regeneration prefix cache uses (Document::TipChainHash), so a stale one
// cannot be mistaken for the truth: the hash matches and it loads, or it does
// not and the document rebuilds. A missing, truncated or foreign-format blob
// is ignored in silence.
//
// Only the TIP is stored — not the bodies after every feature. That is what
// would make the first edit after opening fast too, but it is N whole B-Reps
// on disk for a benefit only the current editor sees. See
// docs/versioning/baked-geometry.md.

// Writes `doc`'s current bodies into <dir>/geometry/. Skips silently (and
// returns true) when the document has no bodies, when any feature carries an
// error — a half-built model is not worth baking — or when the blob on disk
// already carries this exact chain hash, so an unchanged save touches nothing
// and adds nothing to git history.
bool SaveGeometry(const Document& doc, const std::string& dir,
                  std::string* error = nullptr);

// Reads <dir>/geometry/ into `doc` as baked geometry, to be adopted only if
// its chain hash still matches after the features load
// (Document::AdoptBakedGeometry). Returns false when there is nothing usable
// to read — never an error the user must act on.
bool LoadGeometry(Document& doc, const std::string& dir);

// What <dir>/geometry/ holds, without reading the bodies: so an open that had
// to rebuild can say WHY (docs/ui/project-open-progress-plan.md). Present
// means this build can read it; whether it still matches the recipe is only
// known after the regeneration.
enum class BakedGeometryOnDisk { None, OlderFormat, NewerFormat, Present };
BakedGeometryOnDisk InspectGeometry(const std::string& dir);

// Writes the repository hints a project needs for the blob, next to it:
// `.gitattributes` marks the B-Reps binary and non-mergeable (derived data
// has no sensible merge — the hash decides which side is right), and when
// `commit` is false a `.gitignore` keeps `geometry/` out of the repo for
// people who would rather pay the rebuild than carry megabytes. Both files
// are additive; a project that has neither behaves as it always did.
bool WriteRepositoryHints(const std::string& dir, bool commit,
                          std::string* error = nullptr);

// Removes <dir>/geometry/ (turning the per-project setting off, or after a
// save that could not bake). Missing directory = success.
bool RemoveGeometry(const std::string& dir);

} // namespace DocumentIO
