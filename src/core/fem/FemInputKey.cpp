// The input hash of an FEM run (docs/simulation/README.md#studies-as-jobs).

#include "core/Document.h"
#include "core/InputHash.h"
#include "core/Occurrence.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"

namespace fem {

namespace {

// The algorithm's version: a change to what a solve computes bumps it, so no
// answer stored before (topology results are saved with the project) is
// handed out as one from after.
constexpr const char* kFemAlgorithm = "fem/1";

// The study as evaluated: what the mesher and solver are handed.
nlohmann::json Definition(const Study& study) {
  nlohmann::json j = DocumentIO::StudyToJson(study);
  j.erase("name"); // a label
  j.erase("id");
  return j;
}

} // namespace

uint64_t StudyInputKey(const Document& doc, const Study& studyIn) {
  Study study = studyIn;
  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return 0;
  inputhash::Hasher h;
  h.Str(kFemAlgorithm);
  h.Json(Definition(study));
  h.Bodies(doc.GetBodies());
  return h.Digest();
}

uint64_t AssemblyStudyInputKey(const AssemblyDocument& asmDoc,
                               const Study& studyIn,
                               const AssemblyStudyContext& ctx) {
  Study study = studyIn;
  std::string err;
  const SymbolTable none;
  if (!EvaluateStudyExprs(study, ctx.symbols ? *ctx.symbols : none, &err))
    return 0;
  inputhash::Hasher h;
  h.Str(kFemAlgorithm);
  h.Str("assembly");
  h.Json(Definition(study));
  for (const Participant& pt : study.participants) {
    if (pt.role == ParticipantRole::Ignored)
      continue;
    const Occurrence* occ = asmDoc.FindById(pt.occurrenceId);
    if (occ == nullptr) {
      h.U64(0);
      continue;
    }
    h.Json(DocumentIO::OccurrenceToJson(*occ));
    if (ctx.placedBodies)
      h.Bodies(ctx.placedBodies(pt.occurrenceId));
  }
  return h.Digest();
}

} // namespace fem
