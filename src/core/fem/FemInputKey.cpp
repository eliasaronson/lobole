// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The input hash of an FEM run (docs/simulation/README.md#studies-as-jobs).

#include "cad/CADMaterials.h"
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
constexpr const char* kFemAlgorithm = "fem/4";

// The study as evaluated: what the mesher and solver are handed.
nlohmann::json Definition(const Study& study) {
  nlohmann::json j = DocumentIO::StudyToJson(study);
  j.erase("name"); // a label
  j.erase("id");
  return j;
}

// A plastic study also reads each body's hardening curve
// (docs/simulation/README.md#plasticity). Only then, so every other study
// keeps the hash it had before the library had curves.
void HashHardening(inputhash::Hasher& h, const Study& study,
                   const std::vector<Body>& bodies) {
  if (!study.nonlinear.plasticity)
    return;
  h.Str("hardening");
  for (const Body& b : bodies) {
    const CADMaterials::MaterialProperties& m =
        CADMaterials::MaterialLibrary::GetMaterialOrDefault(b.material);
    h.U64(m.hardening.size());
    for (const auto& [stress, strain] : m.hardening) {
      h.F64(stress);
      h.F64(strain);
    }
  }
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
  HashHardening(h, study, doc.GetBodies());
  // A static study of a topology result reads that result: its density is
  // an input (docs/topopt/README.md#a-static-study-of-the-optimised-shape).
  if (study.shapeFrom.study != 0) {
    const StudyRuntime* src = doc.StudyResults(study.shapeFrom.study);
    h.U64(src != nullptr ? src->densityDigest : 0);
  }
  return h.Digest();
}

uint64_t RunInputKey(uint64_t studyKeyWithMode,
                     const ConvergeOptions& converge) {
  if (studyKeyWithMode == 0 || !converge.hasLayerNormal ||
      converge.mode == ConvergeMode::Never)
    return studyKeyWithMode;
  inputhash::Hasher h;
  h.U64(studyKeyWithMode);
  h.Str("layer_normal");
  for (int k = 0; k < 3; ++k)
    h.F64(converge.layerNormal[k]);
  return h.Digest();
}

uint64_t RunInputKey(const Document& doc, const Study& study,
                     const ConvergeOptions& converge) {
  Study withMode = study;
  withMode.converge = converge.mode;
  return RunInputKey(StudyInputKey(doc, withMode), converge);
}

void RestampByInputs(Document& doc) {
  // A shape study's inputs and staleness read its topology result's: the
  // results it is cut from first.
  for (const bool shapes : {false, true})
    for (const Study& s : doc.GetStudies()) {
      if ((s.shapeFrom.study != 0) != shapes)
        continue;
      const StudyRuntime* rt = doc.StudyResults(s.id);
      if (rt == nullptr || rt->inputHash == 0 || !doc.StudyResultsStale(s.id))
        continue;
      if (StudyInputKey(doc, s) == rt->inputHash)
        doc.MarkStudyResultsCurrent(s.id);
    }
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
    if (ctx.placedBodies) {
      const std::vector<Body> bodies = ctx.placedBodies(pt.occurrenceId);
      h.Bodies(bodies);
      HashHardening(h, study, bodies);
    }
  }
  return h.Digest();
}

} // namespace fem
