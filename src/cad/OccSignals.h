// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A fault inside an OpenCASCADE algorithm, caught as a failure of that call
// rather than the death of the app (cad/OccSignals.cpp).
//
// ArmOccSignalHandlers (cad/CADKernel.h) installs OCC's handler, but on Linux
// it turns a signal into an exception only inside an OCC_CATCH_SIGNALS scope,
// and that macro is empty in code not compiled with OCC_CONVERT_SIGNALS --
// which is all of ours. So a SIGSEGV inside BRepOffsetAPI_DraftAngle::Build
// on an ordinary random design (tools/fuzz.py) took the app down with every
// other open document. RunCatchingOccSignals runs `fn` inside such a scope,
// set up in OccSignals.cpp where the macro is real: a fault becomes an
// OSD_SIGSEGV (a Standard_Failure), returned as `why`.
//
// The last line of defence for the few algorithms known to fault on input
// they should refuse (a draft, a shell, a blend): the fault long-jumps out
// of the algorithm's frames without running their destructors, so what they
// held may leak. A process that survives with a leak beats one that does
// not survive.

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace occsig {

// Runs fn(ctx) in an OCC signal scope; false with `why` set when a signal or
// a Standard_Failure ended it.
bool CallCatchingSignals(void (*fn)(void*), void* ctx, std::string* why);

// `f()` in an OCC signal scope: nullopt when it returned, else why it did
// not. A std::exception from `f` propagates as usual.
template <class F> std::optional<std::string> RunCatchingOccSignals(F&& f) {
  auto thunk = [](void* p) {
    (*static_cast<std::remove_reference_t<F>*>(p))();
  };
  std::string why;
  if (CallCatchingSignals(thunk, &f, &why))
    return std::nullopt;
  return why;
}

} // namespace occsig
