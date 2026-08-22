#pragma once

#include "core/io/GitVault.h"

#include <cstdint>
#include <string>
#include <vector>

// What a merge conflict MEANS in a project (versioning F7).
//
// GitVault deals in paths and blobs; this deals in the things a person put in
// the document. "features/15-extrude.json is conflicted" is not something
// anyone can act on — "Extrude 'Gusset 3' in part Stand: you set distance 12,
// they set 18" is.
//
// The five classes the directory format actually produces, and why they are
// not all the same pick:
//
//   Item        one feature/mate/dimension file, edited on both sides. The
//               "pick mine or theirs" F5 described — and the RARE case, since
//               git already merges disjoint FIELDS of one feature correctly
//               (one key per line, sorted). A conflicted item file means the
//               two sides touched the same field.
//   Order       index.json — the ordered id lists. Picking a side here is
//               LOSSY in a way picking a feature is not: the other side's new
//               features stay on disk but drop out of the order, so they
//               vanish from the document (or the load refuses). Resolved by a
//               three-way union against the merge base, never by a pick.
//   DuplicateId two files claiming one id ("15-box.json" and "15-hole.json").
//               Git merges this CLEANLY — two adds, no overlap — so it never
//               appears in GitVault::Unmerged and no git-driven UI can see
//               it. It is found by walking the tree, and it is the class that
//               actually ate work before the loader started refusing it.
//   Project     project.json — which documents the project has.
//   Derived     geometry/*.brep. Already `binary -merge -diff` and keyed by
//               the chain hash, so either side is fine and the rebuild fixes
//               it. Auto-resolved; listed only so nothing is silent.
namespace ProjectMerge {

enum class ConflictKind {
  Item,
  Order,
  DuplicateId,
  Project,
  Derived,
  Other, // anything else in the tree — shown, resolvable only side-by-side
};

// One field of one item that the two sides disagree about.
struct FieldDiff {
  std::string field;
  std::string base, ours, theirs;
  bool inBase = false;
};

// One decision a person has to make.
struct Conflict {
  ConflictKind kind = ConflictKind::Other;
  std::string path; // repo-relative: "parts/Stand/features/15-extrude.json"
  std::string otherPath; // DuplicateId: the second file claiming the id

  // Where it lives, in the words the tabs use.
  std::string document;     // "Stand" — empty for project-level files
  std::string documentKind; // "part" | "assembly" | "drawing"

  // What it is.
  std::string itemKind; // "Extrude", "Mate", "Dimension", …
  std::string name;     // the user's name for it, when it has one
  uint64_t id = 0;

  std::vector<FieldDiff> fields;
  bool oursDeleted = false;
  bool theirsDeleted = false;

  // Resolvable without asking: Order (union) and Derived (rebuild).
  bool autoResolvable = false;

  // One line for the panel, already in CAD language.
  std::string summary;
};

struct Report {
  bool inMerge = false;
  std::vector<Conflict> conflicts;

  bool Empty() const { return conflicts.empty(); }
  // Conflicts a person actually has to decide (Order/Derived resolve
  // themselves), which is what "can I just finish this?" asks.
  int NeedsDecision() const;
};

// Everything wrong with the project right now: git's unmerged paths PLUS the
// duplicate ids git cannot see. Safe on a clean project (returns empty) and
// on a non-repo (duplicate ids are still found — a project can arrive that
// way over rsync or a zip, with no git anywhere).
Report Scan(const std::string& projectDir);

// Take one whole side of a conflict.
bool TakeSide(const std::string& projectDir, const Conflict& conflict,
              GitVault::MergeSide side, std::string* error = nullptr);

// Resolve the classes that have a right answer: Order by union merge,
// Derived by taking either side (the chain hash decides at load).
bool AutoResolve(const std::string& projectDir, const Conflict& conflict,
                 std::string* error = nullptr);

// Take one FIELD from one side, keeping the rest of the merged item. This is
// what makes an item conflict finer than mine/theirs: two sides that both
// edited an extrude, disagreeing only about `distance`, should not have to
// throw away the other's rename.
bool TakeField(const std::string& projectDir, const Conflict& conflict,
               const std::string& field, GitVault::MergeSide side,
               std::string* error = nullptr);

// Give one of two files claiming an id a fresh id, and list it in the index
// beside the other. This is the only resolution for a DuplicateId conflict:
// both features are real work, so neither side is "wrong" and there is
// nothing to pick between.
//
// It REFUSES rather than guesses when anything in the document still refers
// to the contested id — a `sketch_id: 15` cannot say which of the two
// features it meant, and picking one would quietly re-parent someone's
// geometry. The refusal names the referring file.
//
// Cross-document references (an assembly mate into a part) are deliberately
// out of scope here: the id spaces are per-document, so scanning the whole
// project would report collisions that are not references at all. Verify()
// is the backstop — it loads and regenerates everything.
bool RenumberDuplicate(const std::string& projectDir, const Conflict& conflict,
                       std::string* error = nullptr);

// Three-way union of two index.json texts against their base. Exposed for
// testing because it is the part with a right answer that is easy to get
// wrong: ids added on either side are kept, ids DELETED on either side stay
// deleted, and the order follows the base where both agree.
std::string UnionIndex(const std::string& base, const std::string& ours,
                       const std::string& theirs);

// One document that could not be loaded or rebuilt after a resolution.
struct Breakage {
  std::string document;     // "Stand"
  std::string documentKind; // "part"
  std::string reason;       // what the loader or the rebuild said
};

// Does the project on disk actually work? Every document is loaded and
// REGENERATED, because a resolution can parse perfectly and still dangle:
// take their side of a deletion and your extrude keeps a `sketch_id` that
// nothing answers. "git exited 0" is not the bar and never was — this is.
//
// Run it before committing a merge. An empty result means the merge is safe
// to conclude; anything in it names the document and what broke, and the
// caller's way back is GitVault::AbortMerge.
std::vector<Breakage> Verify(const std::string& projectDir);

// What a set of changed paths amounts to, in the words the tabs use (F11 G1).
//
// The History panel used to show sixty rows saying "save". The translation
// from `parts/Stand/features/15-extrude.json` to "a feature in part Stand"
// already existed here for the Conflicts panel; this is the same knowledge
// pointed at a commit's file list.
//
// Deliberately works from PATHS ALONE, because a log page is sixty commits
// and reading a blob per file would be sixty processes. The caller supplies
// `singleItemName` when it has cheaply read the one changed file (see
// DescribeChange), and gets a better sentence for it.
std::string SummarizePaths(const std::vector<std::string>& paths,
                           const std::string& singleItemKind = "",
                           const std::string& singleItemName = "");

// True when `paths` is one document-item file — the case worth spending a
// blob read on to name it properly.
bool IsSingleItem(const std::vector<std::string>& paths);

} // namespace ProjectMerge
