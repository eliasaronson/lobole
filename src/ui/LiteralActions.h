// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The literals lint and extract_variables, against the project
// (docs/variables/README.md#literals-that-should-be-expressions): the RPCs
// and the Variables panel's Literals section both call these, so the two
// surfaces find the same things and refuse the same things in the same
// words.

#include "core/Literals.h"

#include <string>
#include <vector>

class AssemblyDocument;
class Document;

namespace ui {

struct AppState;

// The variables `doc` can see, each knowing the kinds it is used as across
// every part of the project.
std::vector<literals::Variable> LintVariables(AppState& state,
                                              const Document& doc);

// Every variable name in the project and in every part: what a new
// variable's name must not collide with or be shadowed by.
std::set<std::string> TakenVariableNames(AppState& state);

literals::Report LintDocument(AppState& state, const Document& doc);

// Plans an extraction on the ACTIVE part (options.taken is filled here).
literals::ExtractPlan PlanExtractVariables(AppState& state,
                                           literals::ExtractOptions options);

// Applies a plan to the active part as ONE undo step: in `project` scope
// the new variables are project variables and the step is on the project
// history (it puts the features back too); in part scope they are the
// part's own and the step is on its command stack. Returns "" or why
// nothing was changed.
[[nodiscard]] std::string
ApplyExtractVariables(AppState& state, const literals::ExtractPlan& plan,
                      bool projectScope);

// ---- An assembly's literals ------------------------------------------------
// The same lint and extraction over an assembly's numbers
// (AssemblyDocument::VisitExprs), against the project's variables, which
// are all an assembly reads. A finding's and a use's `feature` is the
// owner ("mate 'Hinge'"), its featureId 0, and its field's path the slot's
// ("mates/0/limits/rotation/1"), what extract_variables' `fields` names.

// Every unbound number of `a` equal to a project variable it could mean.
// `checked` counts the literals compared.
std::vector<literals::LiteralFinding>
LintAssembly(AppState& state, AssemblyDocument& a, size_t* checked = nullptr);

// Plans an extraction on `a`: one project variable per literal (equal ones
// of one owner sharing it; of several, with mergeEqual), or with reuse the
// one existing variable it equals. options.featureIds is not used here;
// options.paths limits it to those slots, an unknown one refused naming
// what exists.
literals::ExtractPlan PlanExtractAssembly(AppState& state, AssemblyDocument& a,
                                          literals::ExtractOptions options);

// Applies it: the new variables are project variables (one step on the
// project history), and binding the slots is one step on the assembly's.
// Returns "" or why nothing was changed.
[[nodiscard]] std::string
ApplyExtractAssembly(AppState& state, AssemblyDocument& a,
                     const literals::ExtractPlan& plan);

} // namespace ui
