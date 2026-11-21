// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Which reference cases verify the procedure a result came from
// (docs/simulation/reference-cases.md): each a published benchmark or a
// closed form, run through the RPC by tools/fem_reference.py. A study's
// receipt and the describe rows name them, so an answer says what it rests
// on. Every id is a directory of tests/fem_reference/ whose expected.json
// names the same procedure; `tools/fem_reference.py check` (the headless
// suite's fem_reference_table) fails on a name that is not one.

#include "core/fem/FemStudy.h"

#include <string_view>

namespace fem {

struct ReferenceProcedure {
  std::string_view procedure; // what a result's receipt names
  std::string_view cases;     // the case ids, comma-separated
};

inline constexpr ReferenceProcedure kReferenceProcedures[] = {
    {"static tet10",
     "le1, kirsch, lame_rate, lame_tpu, bar, cantilever, mh_beam, mh_cylinder, "
     "bearing_lug"},
    {"modal tet10", "fv32, modal_beam, free_beam"},
    {"buckling tet10", "euler_column"},
    {"thermal tet10", "thermal_pipe"},
    {"contact (CalculiX)", "contact_block"},
    {"plasticity (CalculiX)", "plastic_bar"},
};

// The procedure that solves `study`: a contact or a plastic study's are
// CalculiX's, every other the in-house solver's by analysis type. Null for
// a topology study, which no case verifies.
inline const ReferenceProcedure* ReferenceProcedureOf(const Study& study) {
  std::string_view want;
  if (study.nonlinear.plasticity)
    want = "plasticity (CalculiX)";
  else if (HasContact(study))
    want = "contact (CalculiX)";
  else if (study.type == AnalysisType::Static && !study.nonlinear.On())
    want = "static tet10";
  else if (study.type == AnalysisType::Modal)
    want = "modal tet10";
  else if (study.type == AnalysisType::Buckling)
    want = "buckling tet10";
  else if (study.type == AnalysisType::Thermal)
    want = "thermal tet10";
  for (const ReferenceProcedure& p : kReferenceProcedures)
    if (p.procedure == want)
      return &p;
  return nullptr;
}

} // namespace fem
