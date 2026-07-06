#pragma once
#include <cstdio>
// Minimal no-op shim for the handful of Base::Console diagnostic calls in
// PlaneGCS, so the solver vendors without FreeCAD.
namespace Base {
struct Console_ {
  void log(const char*, ...) {}
  void warning(const char*, ...) {}
  void message(const char*, ...) {}
  void error(const char*, ...) {}
};
inline Console_& Console() { static Console_ c; return c; }
}
