#include "core/Command.h"

#include "core/io/Serialization.h"

#include <algorithm>
#include <set>

uint64_t NextActionStamp() {
  static uint64_t clock = 0;
  return ++clock;
}

AddFeatureCommand::AddFeatureCommand(std::unique_ptr<Feature> feature)
    : m_feature(std::move(feature)) {
  m_description =
      "Add " + m_feature->TypeName() + " '" + m_feature->GetName() + "'";
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
  m_description =
      "Remove " + m_removed->TypeName() + " '" + m_removed->GetName() + "'";
  return true;
}

void RemoveFeatureCommand::Undo(Document& doc) {
  doc.InsertFeature(std::move(m_removed), m_index);
}

RemoveFeaturesCommand::RemoveFeaturesCommand(std::vector<size_t> indices)
    : m_indices(std::move(indices)) {}

std::string RemoveFeaturesCommand::Description() const {
  return m_description.empty() ? "Remove features" : m_description;
}

bool RemoveFeaturesCommand::Execute(Document& doc) {
  std::sort(m_indices.begin(), m_indices.end());
  m_indices.erase(std::unique(m_indices.begin(), m_indices.end()),
                  m_indices.end());
  if (m_indices.empty() || m_indices.back() >= doc.FeatureCount()) {
    return false;
  }
  m_removed.clear();
  m_removed.resize(m_indices.size());
  {
    Document::BulkEdit bulk(doc); // one Regenerate when the guard releases
    // Highest first, so the lower indices are still the ones we were given.
    for (size_t k = m_indices.size(); k-- > 0;) {
      m_removed[k] = doc.RemoveFeature(m_indices[k]);
    }
  }
  if (m_removed.size() == 1) {
    m_description = "Remove " + m_removed[0]->TypeName() + " '" +
                    m_removed[0]->GetName() + "'";
  } else {
    m_description = "Remove " + std::to_string(m_removed.size()) + " features";
  }
  return true;
}

void RemoveFeaturesCommand::Undo(Document& doc) {
  Document::BulkEdit bulk(doc);
  // Lowest first: each insert lands at its original index because everything
  // below it is already back.
  for (size_t k = 0; k < m_indices.size(); k++) {
    doc.InsertFeature(std::move(m_removed[k]), m_indices[k]);
  }
}

MoveFeatureCommand::MoveFeatureCommand(size_t from, size_t to)
    : m_from(from), m_to(to) {}

std::string MoveFeatureCommand::Description() const {
  return "Reorder feature";
}

bool MoveFeatureCommand::Execute(Document& doc) {
  m_broken.clear();
  if (m_from == m_to) {
    return false;
  }
  // Baseline: which features already carry an error, keyed by stable id (the
  // move shifts indices). A pre-existing error must not block reordering
  // features unrelated to it.
  std::set<uint64_t> preErrored;
  for (const auto& f : doc.GetFeatures()) {
    if (!f->GetError().empty()) {
      preErrored.insert(f->GetId());
    }
  }
  if (!doc.MoveFeature(m_from, m_to)) {
    return false;
  }
  // Trial-regen validity: any feature that was clean before but errors now
  // means the new order dangles a reference (e.g. an extrude ahead of the
  // sketch it consumes). Revert and reject, naming the first casualty.
  for (const auto& f : doc.GetFeatures()) {
    if (!f->GetError().empty() && !preErrored.contains(f->GetId())) {
      m_broken = f->GetName();
      doc.MoveFeature(m_to, m_from); // restore the original order
      return false;
    }
  }
  return true;
}

void MoveFeatureCommand::Undo(Document& doc) { doc.MoveFeature(m_to, m_from); }

ModifyFeatureCommand::ModifyFeatureCommand(size_t index, nlohmann::json before,
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

SetVariableCommand::SetVariableCommand(std::string name, std::string expression,
                                       std::string comment)
    : m_name(std::move(name)), m_expression(std::move(expression)),
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
    m_name = s.name; // capture the auto-generated name for redo/description
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
  if (!s)
    return false;
  m_snapshot = DocumentIO::StudyToJson(*s);
  return doc.RemoveStudy(m_id);
}

void RemoveStudyCommand::Undo(Document& doc) {
  doc.RestoreStudy(DocumentIO::StudyFromJson(m_snapshot));
}

ModifyStudyCommand::ModifyStudyCommand(std::string description,
                                       nlohmann::json before,
                                       nlohmann::json after)
    : m_description(std::move(description)), m_before(std::move(before)),
      m_after(std::move(after)) {}

std::string ModifyStudyCommand::Description() const { return m_description; }

bool ModifyStudyCommand::Execute(Document& doc) {
  return doc.ReplaceStudy(DocumentIO::StudyFromJson(m_after));
}

void ModifyStudyCommand::Undo(Document& doc) {
  doc.ReplaceStudy(DocumentIO::StudyFromJson(m_before));
}

// ---- Configurations -------------------------------------------------------

AddConfigurationCommand::AddConfigurationCommand(std::string name)
    : m_name(std::move(name)) {}

std::string AddConfigurationCommand::Description() const {
  return m_name.empty() ? "Add configuration"
                        : "Add configuration '" + m_name + "'";
}

bool AddConfigurationCommand::Execute(Document& doc) {
  if (m_id == 0) {
    Configuration& c = doc.AddConfiguration(m_name);
    m_id = c.id;
    m_name = c.name;
  } else {
    Configuration c;
    c.id = m_id;
    c.name = m_name;
    doc.RestoreConfiguration(std::move(c));
  }
  return true;
}

void AddConfigurationCommand::Undo(Document& doc) {
  doc.RemoveConfiguration(m_id);
}

RemoveConfigurationCommand::RemoveConfigurationCommand(uint64_t id)
    : m_id(id) {}

std::string RemoveConfigurationCommand::Description() const {
  return "Remove configuration";
}

bool RemoveConfigurationCommand::Execute(Document& doc) {
  const Configuration* c = doc.FindConfiguration(m_id);
  if (!c)
    return false;
  m_snapshot = DocumentIO::ConfigurationToJson(*c);
  m_activeBefore = doc.ActiveConfiguration();
  return doc.RemoveConfiguration(m_id); // may drop to base + regenerate
}

void RemoveConfigurationCommand::Undo(Document& doc) {
  doc.RestoreConfiguration(DocumentIO::ConfigurationFromJson(m_snapshot));
  doc.SetActiveConfiguration(m_activeBefore); // restores selection + regen
}

ModifyConfigurationCommand::ModifyConfigurationCommand(std::string description,
                                                       nlohmann::json before,
                                                       nlohmann::json after)
    : m_description(std::move(description)), m_before(std::move(before)),
      m_after(std::move(after)) {}

std::string ModifyConfigurationCommand::Description() const {
  return m_description;
}

bool ModifyConfigurationCommand::Execute(Document& doc) {
  return doc.ReplaceConfiguration(DocumentIO::ConfigurationFromJson(m_after));
}

void ModifyConfigurationCommand::Undo(Document& doc) {
  doc.ReplaceConfiguration(DocumentIO::ConfigurationFromJson(m_before));
}

SetActiveConfigurationCommand::SetActiveConfigurationCommand(uint64_t id)
    : m_id(id) {}

std::string SetActiveConfigurationCommand::Description() const {
  return "Set active configuration";
}

bool SetActiveConfigurationCommand::Execute(Document& doc) {
  m_before = doc.ActiveConfiguration();
  return doc.SetActiveConfiguration(m_id);
}

void SetActiveConfigurationCommand::Undo(Document& doc) {
  doc.SetActiveConfiguration(m_before);
}

bool CommandStack::Execute(std::unique_ptr<Command> command, Document& doc) {
  if (!command->Execute(doc)) {
    return false;
  }
  command->SetStamp(NextActionStamp());
  m_undo.push_back(std::move(command));
  m_redo.clear();
  return true;
}

void CommandStack::PushApplied(std::unique_ptr<Command> command) {
  command->SetStamp(NextActionStamp());
  m_undo.push_back(std::move(command));
  m_redo.clear();
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

// --- CompositeCommand --------------------------------------------------------

CompositeCommand::CompositeCommand(std::vector<std::unique_ptr<Command>> parts,
                                   std::string description)
    : m_parts(std::move(parts)), m_description(std::move(description)) {}

bool CompositeCommand::Execute(Document& doc) {
  // Redo: forward order, inside ONE bulk edit so the document regenerates
  // once — the same shape the batch had when it ran.
  Document::BulkEdit bulk(doc);
  bool ok = true;
  for (auto& part : m_parts)
    ok = part->Execute(doc) && ok;
  return ok;
}

void CompositeCommand::Undo(Document& doc) {
  Document::BulkEdit bulk(doc);
  for (auto it = m_parts.rbegin(); it != m_parts.rend(); ++it)
    (*it)->Undo(doc);
}

void CommandStack::CoalesceSince(size_t depth, const std::string& description) {
  if (depth >= m_undo.size() || m_undo.size() - depth < 2)
    return;
  std::vector<std::unique_ptr<Command>> parts;
  parts.reserve(m_undo.size() - depth);
  for (size_t i = depth; i < m_undo.size(); i++)
    parts.push_back(std::move(m_undo[i]));
  m_undo.resize(depth);
  auto composite =
      std::make_unique<CompositeCommand>(std::move(parts), description);
  composite->SetStamp(NextActionStamp());
  m_undo.push_back(std::move(composite));
}

size_t CommandStack::RollbackSince(size_t depth, Document& doc) {
  size_t undone = 0;
  if (depth >= m_undo.size())
    return undone;
  {
    // One regeneration for the whole unwind, not one per command.
    Document::BulkEdit bulk(doc);
    while (m_undo.size() > depth) {
      m_undo.back()->Undo(doc);
      m_undo.pop_back();
      undone++;
    }
  }
  return undone;
}
