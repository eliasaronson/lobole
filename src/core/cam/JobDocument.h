#pragma once

#include "core/cam/CamJob.h"

#include <cstdint>
#include <string>
#include <vector>

// A JOB: what is in the vise, and how it gets cut
// (docs/cam/README.md#the-model).
//
// A job is a project document, the third of a shape `AssemblyDocument` and
// `DrawingDocument` already have: it references parts by their stable
// project-document id and owns no geometry of its own. An assembly says where
// parts sit in a product; a drawing says how a part is documented; a job says
// how it is MADE.
//
// It exists because a `cam::Setup` used to live in `Document::m_camSetups`,
// which made every job about exactly one part. Four brackets out of one sheet
// of ply -- the most ordinary job a router does -- was inexpressible.
namespace cam {

// One part this job cuts. `sourceDocId` is the same contract as
// `DrawingView::sourceDocId`: a reference, never a copy, so a model edit
// reaches the job instead of orphaning it. `id` is local to the job and is
// what an operation's target names -- two parts in one job can easily own the
// same feature id, so the ElementRef alone is not an answer.
struct JobPart {
  uint64_t id = 0;
  uint64_t sourceDocId = 0;
  // Display only; the source document owns the name. Kept so a job whose
  // source has been deleted can still say what is missing rather than
  // printing a number.
  std::string name;
  // Which configuration of the source this job cuts (v135); 0 = whatever it
  // has active. A blank of pawns and one of rooks are one part in two
  // configurations, not two Derived copies of it.
  uint64_t configuration = 0;
};

class JobDocument {
public:
  const std::vector<JobPart>& GetParts() const { return m_parts; }
  std::vector<JobPart>& GetParts() { return m_parts; }
  size_t PartCount() const { return m_parts.size(); }

  // Appends a part, assigning a fresh stable id (returned). A source document
  // may appear more than once -- that is two differently-fixtured takes on
  // one part, and refusing it here would be a guess about the job.
  uint64_t AddPart(uint64_t sourceDocId, const std::string& name);
  bool RemovePart(uint64_t id);
  JobPart* FindPart(uint64_t id);
  const JobPart* FindPart(uint64_t id) const;

  const std::vector<Setup>& GetSetups() const { return m_setups; }
  std::vector<Setup>& GetSetups() { return m_setups; }
  size_t SetupCount() const { return m_setups.size(); }

  Setup& AddSetup(const std::string& name = "");
  bool RemoveSetup(uint64_t id);
  Setup* FindSetup(uint64_t id);
  const Setup* FindSetup(uint64_t id) const;

  // One never-reused counter for parts, setups and operations alike (the
  // AssemblyDocument rule: an id names a thing unambiguously across the file).
  uint64_t AllocateId() { return m_nextId++; }
  // Loader hook: pin the counter past the highest restored id.
  void EnsureNextIdAtLeast(uint64_t next) {
    if (next > m_nextId)
      m_nextId = next;
  }
  uint64_t NextId() const { return m_nextId; }

  // Monotonic change counter (the Document/DrawingDocument pattern).
  uint64_t Version() const { return m_version; }
  void BumpVersion() { m_version++; }

  // Whether the job has changed since it was saved (Document::SaveStamp for
  // a job). Version() is only a cache key here — most edits move no body and
  // do not bump it — so an edit counts through MarkEdited, which
  // ui::MarkJobEdited and every JobUndoGuard that records a step call.
  uint64_t SaveStamp() const { return m_version + m_edits; }
  void MarkEdited() { ++m_edits; }

private:
  uint64_t m_edits = 0;
  std::vector<JobPart> m_parts;
  std::vector<Setup> m_setups;
  uint64_t m_nextId = 1;
  uint64_t m_version = 0;
};

} // namespace cam
