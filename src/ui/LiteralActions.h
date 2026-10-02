#pragma once

// The literals lint and extract_variables, against the project
// (docs/variables/README.md#literals-that-should-be-expressions): the RPCs
// and the Variables panel's Literals section both call these, so the two
// surfaces find the same things and refuse the same things in the same
// words.

#include "core/Literals.h"

#include <string>
#include <vector>

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

} // namespace ui
