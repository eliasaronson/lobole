#include "app/GuiTests.h"

#include "app/guitests/GuiTestsCommon.h"

// The GUI suite's entry point. The 175 registrations used to live here in one
// 8,036-line function that took 40 s to compile; they are now one file per
// domain under guitests/, and this dispatches to them.
//
// Test names and categories did not change. tools/gui-tests-xvfb.sh builds its
// four-shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// rather than silently disappearing.
void RegisterGuiTests(ImGuiTestEngine* engine, ui::AppState* state) {
  s_App = state;

  RegisterShellGuiTests(engine);
  RegisterModelingGuiTests(engine);
  RegisterSketchGuiTests(engine);
  RegisterViewportGuiTests(engine);
  RegisterAssemblyGuiTests(engine);
  RegisterFlexibleGuiTests(engine);
  RegisterDrawingGuiTests(engine);
  RegisterCamGuiTests(engine);
  RegisterSimulationGuiTests(engine);
  RegisterProjectGuiTests(engine);
  RegisterTerminalGuiTests(engine);

  // Wrap every registered test so ResetAppForTest() runs before it — this is
  // what makes the suite order-independent. Done once here, after every
  // registrar has run, rather than per test.
  ImVector<ImGuiTest*> allTests;
  ImGuiTestEngine_GetTestList(engine, &allTests);
  for (ImGuiTest* test : allTests) {
    if (!test->TestFunc)
      continue;
    g_origTestFunc[test] = test->TestFunc;
    test->TestFunc = &RunTestWithReset;
  }
}
