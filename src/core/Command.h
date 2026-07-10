#pragma once

#include "core/Document.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

// A reversible edit of the Document. Every model modification — from the
// GUI, scripts, or an AI assistant — goes through a Command so that undo,
// history, and replay all see the same stream of edits.
class Command {
public:
  virtual ~Command() = default;

  virtual std::string Description() const = 0;
  virtual bool Execute(Document& doc) = 0;
  virtual void Undo(Document& doc) = 0;
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
  std::string m_expression, m_comment;  // captured for Undo
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

class CommandStack {
public:
  // Executes the command and pushes it onto the undo stack (clearing any
  // redoable commands). Returns false and discards the command if it failed.
  bool Execute(std::unique_ptr<Command> command, Document& doc);

  bool CanUndo() const { return !m_undo.empty(); }
  bool CanRedo() const { return !m_redo.empty(); }
  void Undo(Document& doc);
  void Redo(Document& doc);

  std::string UndoDescription() const;
  std::string RedoDescription() const;

private:
  std::vector<std::unique_ptr<Command>> m_undo;
  std::vector<std::unique_ptr<Command>> m_redo;
};
