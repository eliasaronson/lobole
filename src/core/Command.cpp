#include "core/Command.h"

#include "core/Serialization.h"

AddFeatureCommand::AddFeatureCommand(std::unique_ptr<Feature> feature)
    : m_feature(std::move(feature)) {
  m_description = "Add " + m_feature->TypeName() + " '" +
                  m_feature->GetName() + "'";
}

std::string AddFeatureCommand::Description() const { return m_description; }

bool AddFeatureCommand::Execute(Document& doc) {
  m_index = doc.FeatureCount();
  doc.AddFeature(std::move(m_feature));
  return true;
}

void AddFeatureCommand::Undo(Document& doc) {
  m_feature = doc.RemoveFeature(m_index);
}

RemoveFeatureCommand::RemoveFeatureCommand(size_t index) : m_index(index) {}

std::string RemoveFeatureCommand::Description() const {
  return m_description.empty() ? "Remove feature" : m_description;
}

bool RemoveFeatureCommand::Execute(Document& doc) {
  m_removed = doc.RemoveFeature(m_index);
  if (!m_removed) {
    return false;
  }
  m_description = "Remove " + m_removed->TypeName() + " '" +
                  m_removed->GetName() + "'";
  return true;
}

void RemoveFeatureCommand::Undo(Document& doc) {
  doc.InsertFeature(std::move(m_removed), m_index);
}

ModifyFeatureCommand::ModifyFeatureCommand(size_t index,
                                           nlohmann::json before,
                                           nlohmann::json after)
    : m_index(index), m_before(std::move(before)), m_after(std::move(after)) {}

std::string ModifyFeatureCommand::Description() const {
  return "Edit " + m_after.value("type", "feature") + " '" +
         m_after.value("name", "") + "'";
}

bool ModifyFeatureCommand::Execute(Document& doc) {
  return doc.ReplaceFeature(m_index, DocumentIO::FeatureFromJson(m_after));
}

void ModifyFeatureCommand::Undo(Document& doc) {
  doc.ReplaceFeature(m_index, DocumentIO::FeatureFromJson(m_before));
}

SetVariableCommand::SetVariableCommand(std::string name,
                                       std::string expression,
                                       std::string comment)
    : m_name(std::move(name)),
      m_expression(std::move(expression)),
      m_comment(std::move(comment)) {}

std::string SetVariableCommand::Description() const {
  return "Set variable '" + m_name + "'";
}

bool SetVariableCommand::Execute(Document& doc) {
  if (const Parameter* prior = doc.FindParameter(m_name)) {
    m_existed = true;
    m_oldExpression = prior->expression;
    m_oldComment = prior->comment;
  } else {
    m_existed = false;
  }
  return doc.SetParameter(m_name, m_expression, m_comment);
}

void SetVariableCommand::Undo(Document& doc) {
  if (m_existed) {
    doc.SetParameter(m_name, m_oldExpression, m_oldComment);
  } else {
    doc.RemoveParameter(m_name);
  }
}

DeleteVariableCommand::DeleteVariableCommand(std::string name)
    : m_name(std::move(name)) {}

std::string DeleteVariableCommand::Description() const {
  return "Delete variable '" + m_name + "'";
}

bool DeleteVariableCommand::Execute(Document& doc) {
  const Parameter* prior = doc.FindParameter(m_name);
  if (prior == nullptr) {
    return false;
  }
  m_expression = prior->expression;
  m_comment = prior->comment;
  m_references.clear();
  return doc.RemoveParameter(m_name, &m_references);
}

void DeleteVariableCommand::Undo(Document& doc) {
  doc.SetParameter(m_name, m_expression, m_comment);
}

RenameVariableCommand::RenameVariableCommand(std::string oldName,
                                             std::string newName)
    : m_oldName(std::move(oldName)), m_newName(std::move(newName)) {}

std::string RenameVariableCommand::Description() const {
  return "Rename variable '" + m_oldName + "' to '" + m_newName + "'";
}

bool RenameVariableCommand::Execute(Document& doc) {
  return doc.RenameParameter(m_oldName, m_newName);
}

void RenameVariableCommand::Undo(Document& doc) {
  doc.RenameParameter(m_newName, m_oldName);
}

// ---- FEM studies ----------------------------------------------------------

AddStudyCommand::AddStudyCommand(std::string name) : m_name(std::move(name)) {}

std::string AddStudyCommand::Description() const {
  return m_name.empty() ? "Add study" : "Add study '" + m_name + "'";
}

bool AddStudyCommand::Execute(Document& doc) {
  if (m_id == 0) {
    fem::Study& s = doc.AddStudy(m_name);
    m_id = s.id;
    m_name = s.name;  // capture the auto-generated name for redo/description
  } else {
    // Redo: restore the same (empty) study with its original id so refs stay
    // stable across undo/redo, like AddFeatureCommand reusing its feature.
    fem::Study s;
    s.id = m_id;
    s.name = m_name;
    doc.RestoreStudy(std::move(s));
  }
  return true;
}

void AddStudyCommand::Undo(Document& doc) { doc.RemoveStudy(m_id); }

RemoveStudyCommand::RemoveStudyCommand(uint64_t studyId) : m_id(studyId) {}

std::string RemoveStudyCommand::Description() const { return "Remove study"; }

bool RemoveStudyCommand::Execute(Document& doc) {
  const fem::Study* s = doc.FindStudy(m_id);
  if (!s) return false;
  m_snapshot = DocumentIO::StudyToJson(*s);
  return doc.RemoveStudy(m_id);
}

void RemoveStudyCommand::Undo(Document& doc) {
  doc.RestoreStudy(DocumentIO::StudyFromJson(m_snapshot));
}

ModifyStudyCommand::ModifyStudyCommand(std::string description,
                                       nlohmann::json before, nlohmann::json after)
    : m_description(std::move(description)),
      m_before(std::move(before)),
      m_after(std::move(after)) {}

std::string ModifyStudyCommand::Description() const { return m_description; }

bool ModifyStudyCommand::Execute(Document& doc) {
  return doc.ReplaceStudy(DocumentIO::StudyFromJson(m_after));
}

void ModifyStudyCommand::Undo(Document& doc) {
  doc.ReplaceStudy(DocumentIO::StudyFromJson(m_before));
}

bool CommandStack::Execute(std::unique_ptr<Command> command, Document& doc) {
  if (!command->Execute(doc)) {
    return false;
  }
  m_undo.push_back(std::move(command));
  m_redo.clear();
  return true;
}

void CommandStack::Undo(Document& doc) {
  if (m_undo.empty()) {
    return;
  }
  m_undo.back()->Undo(doc);
  m_redo.push_back(std::move(m_undo.back()));
  m_undo.pop_back();
}

void CommandStack::Redo(Document& doc) {
  if (m_redo.empty()) {
    return;
  }
  m_redo.back()->Execute(doc);
  m_undo.push_back(std::move(m_redo.back()));
  m_redo.pop_back();
}

std::string CommandStack::UndoDescription() const {
  return m_undo.empty() ? std::string() : m_undo.back()->Description();
}

std::string CommandStack::RedoDescription() const {
  return m_redo.empty() ? std::string() : m_redo.back()->Description();
}
