// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Shape.hxx>

namespace cad {

// Builds `op` (a default-constructed BRepAlgoAPI_Common/Cut/Fuse) on `a` and
// `b` WITHOUT writing to either. An OpenCASCADE boolean stores the p-curves it
// computes on its inputs' edges unless told not to, and the flag must be set
// BEFORE the build: the two-shape constructor builds at once, so
// `BRepAlgoAPI_Cut op(a, b); op.SetNonDestructive(true);` is silently
// destructive. tests/test_conventions.cpp fails on that form
// (docs/inspect/README.md#query-booleans-are-non-destructive).
//
// `fuzzy` > 0 builds with OCC's fuzzy option at that tolerance (model units):
// geometry closer than it counts as coincident. It is a refused boolean's one
// retry (docs/modeling/README.md#a-refused-boolean-gets-one-fuzzy-retry), and
// it comes through here because the retry runs on operands that are still
// the document's — it must leave them as it found them.
template <class Op>
bool BuildNonDestructive(Op& op, const TopoDS_Shape& a, const TopoDS_Shape& b,
                         double fuzzy = 0.0) {
  TopTools_ListOfShape args, tools;
  args.Append(a);
  tools.Append(b);
  op.SetArguments(args);
  op.SetTools(tools);
  op.SetNonDestructive(Standard_True);
  if (fuzzy > 0.0)
    op.SetFuzzyValue(fuzzy);
  op.Build();
  return op.IsDone();
}

} // namespace cad
