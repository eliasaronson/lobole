// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/Document.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

// A process-wide monotonic "action clock". Every user-visible undoable action
// draws a stamp from it — feature commands (below), plus the snapshot-based
// project-variable / assembly / drawing histories in AppState. Comparing the
// top-of-stack stamps lets the global Undo route to whichever source the user
// most recently touched, without merging their separate stacks (Phase V).
uint64_t NextActionStamp();

// A reversible edit of the Document. Every model modification — from the
// GUI, scripts, or an AI assistant — goes through a Command so that undo,
// history, and replay all see the same stream of edits.
class Command {
public:
  virtual ~Command() = default;

  virtual std::string Description() const = 0;
  virtual bool Execute(Document& doc) = 0;
  virtual void Undo(Document& doc) = 0;

  // Action-clock stamp (see NextActionStamp), assigned when the command is
  // pushed onto a CommandStack; travels with the command through undo/redo.
  uint64_t Stamp() const { return m_stamp; }
  void SetStamp(uint64_t stamp) { m_stamp = stamp; }

private:
  uint64_t m_stamp = 0;
};

class AddFeatureCommand : public Command {
public:
  explicit AddFeatureCommand(std::unique_ptr<Feature> feature);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  // Held here while not applied; moved into the document on Execute and
  // reclaimed on Undo.
  std::unique_ptr<Feature> m_feature;
  std::string m_description;
  size_t m_index = 0;
};

class RemoveFeatureCommand : public Command {
public:
  explicit RemoveFeatureCommand(size_t index);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  size_t m_index;
  std::unique_ptr<Feature> m_removed;
  std::string m_description;
};

// Removes SEVERAL features as one undo step (a multi-selection's Delete).
// Indices are taken in any order and removed highest-first inside a BulkEdit,
// so the document regenerates once for the batch rather than once per
// feature. Refuses (removes nothing) if any index is out of range: a partial
// delete that undoes as a whole would restore features that were never
// removed. Undo reinserts each feature at its original index, lowest-first.
class RemoveFeaturesCommand : public Command {
public:
  explicit RemoveFeaturesCommand(std::vector<size_t> indices);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  std::vector<size_t> m_indices; // sorted ascending after Execute
  std::vector<std::unique_ptr<Feature>> m_removed; // parallel to m_indices
  std::string m_description;
};

// Reorders one feature (drag in the feature tree). Execute performs the move
// and regenerates; if the new order dangles a reference — some feature that was
// error-free now fails to regenerate — it reverts and returns false, naming the
// broken feature through BrokenFeature() so the UI can explain the refusal.
// Undo moves the feature back to its original position.
class MoveFeatureCommand : public Command {
public:
  MoveFeatureCommand(size_t from, size_t to);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

  // After a rejected Execute: the name of the feature the move would break
  // (empty when the move succeeded, or failed for a non-dependency reason).
  const std::string& BrokenFeature() const { return m_broken; }

private:
  size_t m_from, m_to;
  std::string m_broken;
};

// Parameter edit of one feature, snapshotted as JSON before/after. Execute
// rebuilds the feature from the "after" snapshot, so it is idempotent: the
// UI can mutate the feature live during a drag and still push this command
// normally on release (the redundant execute is a no-op state-wise).
class ModifyFeatureCommand : public Command {
public:
  ModifyFeatureCommand(size_t index, nlohmann::json before,
                       nlohmann::json after);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  size_t m_index;
  nlohmann::json m_before, m_after;
};

// Adds a parameter or redefines an existing one. Undo restores the previous
// definition, or removes the parameter if it was newly added.
class SetVariableCommand : public Command {
public:
  SetVariableCommand(std::string name, std::string expression,
                     std::string comment = "");

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  std::string m_name, m_expression, m_comment;
  // Prior definition, captured on Execute for Undo.
  bool m_existed = false;
  std::string m_oldExpression, m_oldComment;
};

// Deletes a parameter. Fails (see References()) if any expression still
// references it, so references never dangle silently. Undo re-adds the
// parameter at the end of the table (original position is not preserved).
class DeleteVariableCommand : public Command {
public:
  explicit DeleteVariableCommand(std::string name);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

  // After a failed Execute: the parameters/features that blocked deletion.
  const std::vector<std::string>& References() const { return m_references; }

private:
  std::string m_name;
  std::string m_expression, m_comment; // captured for Undo
  std::vector<std::string> m_references;
};

// Renames a parameter and rewrites every referencing expression.
class RenameVariableCommand : public Command {
public:
  RenameVariableCommand(std::string oldName, std::string newName);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  std::string m_oldName, m_newName;
};

// ---- FEM studies ----------------------------------------------------------

// Adds a new (empty) study. Undo removes it. The created id is captured on
// Execute so redo reuses it.
class AddStudyCommand : public Command {
public:
  // `type` is part of the add, so a redo brings back the study that was
  // added rather than a static one with its id.
  explicit AddStudyCommand(std::string name = "",
                           fem::AnalysisType type = fem::AnalysisType::Static);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

  uint64_t StudyId() const { return m_id; }

private:
  std::string m_name;
  fem::AnalysisType m_type = fem::AnalysisType::Static;
  uint64_t m_id = 0;
};

// Removes a study, snapshotting it for Undo (which re-adds it at the end of
// the list — original position is not preserved, like DeleteVariableCommand).
class RemoveStudyCommand : public Command {
public:
  explicit RemoveStudyCommand(uint64_t studyId);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  uint64_t m_id;
  nlohmann::json m_snapshot; // StudyToJson, captured on Execute
};

// One study edit (mesh size, refinement add/remove, load add/remove),
// snapshotted as StudyToJson before/after — one command for every study
// mutation, since undo semantics are identical. Idempotent like
// ModifyFeatureCommand: the GUI can mutate live then push this on release.
class ModifyStudyCommand : public Command {
public:
  ModifyStudyCommand(std::string description, nlohmann::json before,
                     nlohmann::json after);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  std::string m_description;
  nlohmann::json m_before, m_after;
};

// ---- Tolerances (docs/tolerances/README.md) ---------------------------------

// Sets or replaces the tolerance on one element. Upsert, because a face
// cannot be two sizes at once: Undo restores whatever was on that element
// before, INCLUDING nothing.
class SetToleranceCommand : public Command {
public:
  explicit SetToleranceCommand(Tolerance tolerance);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

  uint64_t ToleranceId() const { return m_after.id; }

private:
  Tolerance m_after;
  Tolerance m_before;
  bool m_hadBefore = false;
};

// Clears the tolerance on an element, snapshotting it for Undo (which
// restores it with its original id, so references survive undo/redo).
class ClearToleranceCommand : public Command {
public:
  explicit ClearToleranceCommand(uint64_t toleranceId);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  uint64_t m_id;
  Tolerance m_removed;
  bool m_had = false;
};

// ---- Sheet metal (docs/sheetmetal/README.md) --------------------------------

// Sets (or clears, with nullopt) the document's sheet-metal rules. One
// command for the whole object rather than one per field: the gauge, the
// radius and the K are read together by every flange, so a half-applied edit
// is a part briefly made of a thickness nobody chose. Geometry-bearing —
// unlike a tolerance this DOES rebuild, which is why Execute regenerates.
class SetSheetMetalRulesCommand : public Command {
public:
  explicit SetSheetMetalRulesCommand(
      std::optional<sheet::SheetMetalRules> rules);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  std::optional<sheet::SheetMetalRules> m_after;
  std::optional<sheet::SheetMetalRules> m_before;
};

// Sets (or clears, with nullopt) how the part is printed
// (docs/tolerances/README.md#printed-parts). Drives no geometry, so unlike
// the sheet's rules it rebuilds nothing — it changes what the fit checks
// report.
class SetPrintProcessCommand : public Command {
public:
  explicit SetPrintProcessCommand(std::optional<printfit::Process> process);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  std::optional<printfit::Process> m_after;
  std::optional<printfit::Process> m_before;
};

// Sets a part's slicer settings (core/PrintSettings.h), the part's and its
// bodies' overrides together. Drives no geometry.
class SetPrintSettingsCommand : public Command {
public:
  explicit SetPrintSettingsCommand(printset::PartSettings settings);

  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  printset::PartSettings m_after;
  printset::PartSettings m_before;
};

// ---- Configurations (the study command trio, verbatim) --------------------

// Adds a new (empty) configuration; the created id is captured for redo.
class AddConfigurationCommand : public Command {
public:
  explicit AddConfigurationCommand(std::string name = "");
  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;
  uint64_t ConfigurationId() const { return m_id; }

private:
  std::string m_name;
  uint64_t m_id = 0;
};

// Removes a configuration, snapshotting it (and the active id, restored on
// undo since removing the active config falls back to base).
class RemoveConfigurationCommand : public Command {
public:
  explicit RemoveConfigurationCommand(uint64_t id);
  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  uint64_t m_id;
  nlohmann::json m_snapshot;
  uint64_t m_activeBefore = 0;
};

// One configuration edit (rename, override add/remove, suppression toggle),
// snapshotted as ConfigurationToJson before/after — idempotent like
// ModifyStudyCommand.
class ModifyConfigurationCommand : public Command {
public:
  ModifyConfigurationCommand(std::string description, nlohmann::json before,
                             nlohmann::json after);
  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  std::string m_description;
  nlohmann::json m_before, m_after;
};

// Switches the active configuration (0 = base). Regenerates on both apply and
// undo.
class SetActiveConfigurationCommand : public Command {
public:
  explicit SetActiveConfigurationCommand(uint64_t id);
  std::string Description() const override;
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

private:
  uint64_t m_id;
  uint64_t m_before = 0;
};

// N commands applied as ONE undo step (the `batch` RPC). Built from commands
// already on the stack — they ran individually and are collapsed afterwards —
// so it is only ever pushed with PushApplied. Undo runs them in reverse.
class CompositeCommand : public Command {
public:
  CompositeCommand(std::vector<std::unique_ptr<Command>> parts,
                   std::string description);

  std::string Description() const override { return m_description; }
  bool Execute(Document& doc) override;
  void Undo(Document& doc) override;

  size_t Size() const { return m_parts.size(); }

private:
  std::vector<std::unique_ptr<Command>> m_parts;
  std::string m_description;
};

class CommandStack {
public:
  // Executes the command and pushes it onto the undo stack (clearing any
  // redoable commands). Returns false and discards the command if it failed.
  bool Execute(std::unique_ptr<Command> command, Document& doc);

  // Pushes a command whose effect is ALREADY applied to the document (live
  // UI edits followed by a snapshot command). Undo/redo behave exactly as
  // with Execute, but the current feature objects are left untouched — an
  // Execute here would ReplaceFeature with an identical copy, dangling every
  // pointer the caller still holds (the material-change segfault).
  void PushApplied(std::unique_ptr<Command> command);

  // How many undoable commands are on the stack — the mark a `batch` takes
  // before running so it can collapse or discard exactly what it pushed.
  size_t UndoDepth() const { return m_undo.size(); }

  // Collapses everything pushed since `depth` into ONE undo step. Fewer than
  // two commands is a no-op (one command is already one step). The document
  // is untouched: the commands are applied and stay applied.
  void CoalesceSince(size_t depth, const std::string& description);

  // Undoes and DISCARDS everything pushed since `depth`, newest first — a
  // failed batch leaving nothing half-applied. Returns how many it undid.
  size_t RollbackSince(size_t depth, Document& doc);

  bool CanUndo() const { return !m_undo.empty(); }
  bool CanRedo() const { return !m_redo.empty(); }
  void Undo(Document& doc);
  void Redo(Document& doc);

  // Action-clock stamp of the top undoable / redoable command (0 when empty).
  // Backs the global Undo/Redo recency arbitration against the project-var
  // stack (Phase V).
  uint64_t TopUndoStamp() const {
    return m_undo.empty() ? 0 : m_undo.back()->Stamp();
  }
  uint64_t TopRedoStamp() const {
    return m_redo.empty() ? 0 : m_redo.back()->Stamp();
  }

  std::string UndoDescription() const;
  std::string RedoDescription() const;

private:
  std::vector<std::unique_ptr<Command>> m_undo;
  std::vector<std::unique_ptr<Command>> m_redo;
};
