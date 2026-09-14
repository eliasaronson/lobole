#include "app/guitests/GuiTestsCommon.h"

// Terminal GUI tests, carved out of the single 8,036-line RegisterGuiTests.
//
// Test names and categories are UNCHANGED: tools/gui-tests-xvfb.sh builds its
// shard partition from the binary's own --gui-test-list and asserts every
// listed test ran, so a case renamed or lost in the split fails the runner
// loudly instead of silently disappearing.
void RegisterTerminalGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;
  (void)t;
#ifdef HAVE_TERMINAL
  t = IM_REGISTER_TEST(engine, "terminal", "off_by_default_docks_right_on");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");

    // Off by default: the window does not exist at all.
    IM_CHECK(!s_App->settings.terminalPanel);
    // NoError: this asks a question whose answer is expected to be "no". A
    // bare WindowInfo LOGS an error when it cannot find the window, and
    // ImGuiTestContext::WindowInfo starts with `if (IsError()) return
    // ItemInfoNull()` — so one expected-null lookup poisons the context and
    // EVERY later lookup returns null, whatever the UI is doing.
    //
    // It only bit after a test that had previously turned this panel on: the
    // window object then still exists but is not being submitted, which is a
    // different path inside WindowInfo and the one that logs. That is why it
    // passed alone and failed behind cam/tool_window_follows_the_viewport,
    // and why five fixes aimed at docking, timing and the PTY all missed.
    IM_CHECK(
        ctx->WindowInfo("//Terminal###TerminalPanel", ImGuiTestOpFlags_NoError)
            .Window == nullptr);

    s_App->settings.terminalPanel = true;
    // Wait for the CONDITION, not for a frame count. Turning the panel on
    // creates a window, docks it, and forks a shell through a PTY, and a
    // fixed Yield(6) covered all three only on an idle machine — under four
    // parallel shards the spawn misses the deadline and the test fails on a
    // null window. It passed 4/4 in isolation, which is how a load-dependent
    // wait advertises itself.
    // 90 frames is ~1.5 s — generous for a fork+PTY and quick to fail.
    //
    // DIAGNOSED, not fixed (2026-08-22, second session). It is NOT a timeout
    // and NOT the PTY. Instrumenting DrawTerminalPanel through the failing
    // pair shows the window IS created, IS active and IS docked (DockId 4) —
    // with `Hidden` true, so Begin() returns false, the body is never
    // submitted, and ###term_claude below cannot be found. WindowInfo also
    // declines to return a hidden window, which is the null seen here.
    //
    // NOTE it passes under the default 4-shard run, which puts the two tests
    // in different processes — `--serial` is 177/178. A green suite is not
    // evidence this is fixed.
    //
    // Reproduces deterministically and serially, no load involved:
    //   gui-tests-xvfb.sh -f "tool_window_follows_the_viewport,\
    //                         off_by_default_docks_right_on" --serial
    // and that CAM test is the only one that turns this panel on and off and
    // rebuilds the dock layout (legitimately — it narrows the viewport).
    //
    // Four fixes tried and REJECTED, so nobody repeats them: starting from a
    // rebuilt default layout; waiting on FindWindowByName instead of
    // WindowInfo; ctx->ItemClick(win->TabId); poking
    // DockNode->SelectedTabId. None makes the body submit. The remaining
    // suspect is which node the window lands in after a rebuild, not when.
    ImGuiWindow* win = nullptr;
    for (int i = 0; i < 90; i++) {
      ctx->Yield();
      // NoError, or the first miss poisons the context and the loop can
      // never succeed however long it waits (see the note above).
      win = ctx->WindowInfo("//Terminal###TerminalPanel",
                            ImGuiTestOpFlags_NoError)
                .Window;
      if (win != nullptr && win->DockId != 0 && ui::TerminalRunning(*s_App))
        break;
    }
    IM_CHECK(win != nullptr);
    IM_CHECK(win->DockId != 0); // docked, not floating over the model tree
    // And it opened a shell by itself: the chooser it used to show was a
    // one-shot menu you could only get back to by restarting the app.
    IM_CHECK(ui::TerminalRunning(*s_App));
    // The Claude Code action stays available for the whole session, rather
    // than being a mode chosen once.
    IM_CHECK(ctx->ItemExists("//Terminal###TerminalPanel/###term_claude"));
    // On the RIGHT: its left edge is past the middle of the viewport.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    IM_CHECK(win->Pos.x > vp->Pos.x + vp->Size.x * 0.5f);

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
  };

  // The layout rebuild, on purpose. Until now the only thing that exercised
  // it was cam/tool_window_follows_the_viewport, which toggles this panel and
  // rebuilds the dock layout TWICE as a side effect of testing something
  // else — and for a while that combination left the terminal window docked,
  // active and `Hidden`, so its body was never submitted and every widget
  // inside it vanished. It reproduced only when those two tests shared a
  // process, which the 4-shard partition usually prevented, so the suite was
  // green while `--serial` was 177/178 (docs/versioning/f7-handoff.md).
  //
  // Inheriting coverage from another domain's test is how that stayed hidden:
  // nothing NAMED the behaviour, so nothing failed for the right reason. This
  // does, and it asserts the thing that actually broke — a widget INSIDE the
  // panel, which requires Begin() to have returned true.
  t = IM_REGISTER_TEST(engine, "terminal", "survives_a_dock_layout_rebuild");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const auto body = [&]() {
      return ctx->ItemExists("//Terminal###TerminalPanel/###term_claude");
    };
    const bool had = s_App->settings.terminalPanel;

    s_App->settings.terminalPanel = true;
    ctx->Yield(30);
    IM_CHECK(body());

    // Rebuilt while the panel is ON.
    s_App->buildDefaultDockLayout = true;
    ctx->Yield(12);
    IM_CHECK(body());

    // Rebuilt while it is OFF — this deletes the node the panel lived in,
    // and is the case that once re-homed the window into the CENTRAL node,
    // on top of the whole 3D viewport (docs/ui/gui-test-isolation.md).
    s_App->settings.terminalPanel = false;
    s_App->buildDefaultDockLayout = true;
    ctx->Yield(12);

    // ...and back. The panel must come back submitted, and on the right.
    s_App->settings.terminalPanel = true;
    ctx->Yield(30);
    IM_CHECK(body());
    ImGuiWindow* win = ImGui::FindWindowByName("Terminal###TerminalPanel");
    IM_CHECK(win != nullptr);
    IM_CHECK(win->DockId != 0);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    IM_CHECK(win->Pos.x > vp->Pos.x + vp->Size.x * 0.5f);
    // Not swallowing the viewport, which is what the central-node re-home
    // looked like: the panel's rect came back exactly equal to the viewport.
    IM_CHECK(win->Size.x < vp->Size.x * 0.9f);

    s_App->settings.terminalPanel = had;
    ctx->Yield(3);
  };

  t = IM_REGISTER_TEST(engine, "terminal", "runs_a_command_and_shows_output");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    s_App->settings.terminalPanel = true;
    ctx->Yield(6);

    ui::StartTerminalCommand(*s_App, "/bin/sh",
                             {"-c", "printf 'hello-from-panel'; sleep 30"});
    // Frames are what pump the session, so this is also the proof that the
    // panel drains its PTY while drawing.
    bool seen = false;
    for (int i = 0; i < 200 && !seen; i++) {
      ctx->Yield();
      const term::Screen* screen = ui::TerminalScreenOf(*s_App);
      if (screen != nullptr)
        seen = screen->Line(0).find("hello-from-panel") != std::string::npos;
    }
    IM_CHECK(seen);
    IM_CHECK(ui::TerminalRunning(*s_App));

    // Turning the preference off ends the session — a process kept alive
    // behind a closed panel is one nobody can see or stop.
    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
    IM_CHECK(!ui::TerminalRunning(*s_App));
  };

  t = IM_REGISTER_TEST(engine, "terminal", "drag_selects_and_copies_text");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    s_App->settings.terminalPanel = true;
    ctx->Yield(6);

    ui::StartTerminalCommand(*s_App, "/bin/sh",
                             {"-c", "printf 'copyme-token\\n'; sleep 30"});
    bool seen = false;
    for (int i = 0; i < 200 && !seen; i++) {
      ctx->Yield();
      const term::Screen* screen = ui::TerminalScreenOf(*s_App);
      if (screen != nullptr)
        seen = screen->Line(0).find("copyme-token") != std::string::npos;
    }
    IM_CHECK(seen);

    // Drag across the first line, from its start to past the token.
    ImGuiWindow* win = ctx->WindowInfo("//Terminal###TerminalPanel").Window;
    IM_CHECK(win != nullptr);
    ImGuiTestItemInfo grid =
        ctx->ItemInfo("//Terminal###TerminalPanel/###term_grid");
    IM_CHECK(grid.ID != 0);
    const term::Screen* screen = ui::TerminalScreenOf(*s_App);
    IM_CHECK(screen != nullptr);
    const float cellW = grid.RectFull.GetWidth() / (float)screen->Cols();
    const float cellH = grid.RectFull.GetHeight() / (float)screen->Rows();
    const ImVec2 rowStart(grid.RectFull.Min.x + cellW * 0.25f,
                          grid.RectFull.Min.y + cellH * 0.5f);
    const ImVec2 rowEnd(grid.RectFull.Min.x + cellW * 12.0f,
                        grid.RectFull.Min.y + cellH * 0.5f);
    ctx->MouseMoveToPos(rowStart);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(rowEnd);
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);

    // Copy needs the keyboard, which means the pointer must be over the panel
    // — the same rule as everything else typed here.
    ImGui::SetClipboardText("");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C);
    ctx->Yield(3);
    const char* clip = ImGui::GetClipboardText();
    IM_CHECK(clip != nullptr);
    IM_CHECK_STR_EQ(clip, "copyme-token");

    // A click with no drag clears the selection, as a terminal does — a
    // highlight that never goes away is worse than none.
    ctx->MouseMoveToPos(rowStart);
    ctx->MouseClick(ImGuiMouseButton_Left);
    ctx->Yield(2);
    ImGui::SetClipboardText("");
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C);
    ctx->Yield(3);
    clip = ImGui::GetClipboardText();
    IM_CHECK(clip == nullptr || clip[0] == '\0');

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
  };

  t = IM_REGISTER_TEST(engine, "terminal", "hover_decides_who_gets_the_keys");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    ToolbarCreateAccept(ctx, "###tb_box"); // one undoable feature
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1);

    s_App->settings.terminalPanel = true;
    ctx->Yield(6);
    ui::StartTerminalCommand(
        *s_App, "/bin/sh",
        {"-c", "read line; printf 'got:%s' \"$line\"; sleep 30"});
    ctx->Yield(10);

    ImGuiWindow* win = ctx->WindowInfo("//Terminal###TerminalPanel").Window;
    IM_CHECK(win != nullptr);
    const ImVec2 inside(win->Pos.x + win->Size.x * 0.5f,
                        win->Pos.y + win->Size.y * 0.6f);
    ctx->MouseMoveToPos(inside);
    ctx->Yield(2);
    IM_CHECK(ui::TerminalCapturesKeys(*s_App));

    // Typing reaches the process...
    ctx->KeyChars("hi");
    ctx->KeyPress(ImGuiKey_Enter);
    bool echoed = false;
    for (int i = 0; i < 200 && !echoed; i++) {
      ctx->Yield();
      const term::Screen* screen = ui::TerminalScreenOf(*s_App);
      if (screen == nullptr)
        continue;
      for (int r = 0; r < screen->Rows() && !echoed; r++)
        echoed = screen->Line(r).find("got:hi") != std::string::npos;
    }
    IM_CHECK(echoed);

    // ...and Ctrl+Z here suspends a job rather than undoing a feature. This
    // is the whole reason the gate exists: one chord, two meanings, and the
    // pointer is what says which.
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield(4);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)1); // the box is still there

    // Pointer over the viewport: the app has its shortcuts back.
    ctx->MouseMoveToPos(
        ImVec2(s_App->viewportPos.x + s_App->viewportSize.x * 0.5f,
               s_App->viewportPos.y + s_App->viewportSize.y * 0.5f));
    ctx->Yield(2);
    IM_CHECK(!ui::TerminalCapturesKeys(*s_App));
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
    ctx->Yield(4);
    IM_CHECK_EQ(Doc()->FeatureCount(), (size_t)0); // undone

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
    IM_CHECK(!ui::TerminalRunning(*s_App));
  };

  // T7. Middle-click pastes the panel's own selection. The trap this pins
  // down: a middle press ACTIVATES the grid's invisible button, and the
  // activation branch is the one that clears the selection — so the paste
  // has to be read before it, or middle-click destroys the thing it was
  // about to paste and silently substitutes the system clipboard.
  t = IM_REGISTER_TEST(engine, "terminal", "middle_click_pastes_the_selection");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    s_App->settings.terminalPanel = true;
    ctx->Yield(6);

    // `cat` echoes whatever it is given, so the paste is visible as output.
    ui::StartTerminalCommand(*s_App, "/bin/sh",
                             {"-c", "printf 'pasteme\n'; cat"});
    bool seen = false;
    for (int i = 0; i < 200 && !seen; i++) {
      ctx->Yield();
      const term::Screen* screen = ui::TerminalScreenOf(*s_App);
      if (screen != nullptr)
        seen = screen->Line(0).find("pasteme") != std::string::npos;
    }
    IM_CHECK(seen);

    ImGuiTestItemInfo grid =
        ctx->ItemInfo("//Terminal###TerminalPanel/###term_grid");
    IM_CHECK(grid.ID != 0);
    const term::Screen* screen = ui::TerminalScreenOf(*s_App);
    IM_CHECK(screen != nullptr);
    const float cellW = grid.RectFull.GetWidth() / (float)screen->Cols();
    const float cellH = grid.RectFull.GetHeight() / (float)screen->Rows();
    const ImVec2 rowStart(grid.RectFull.Min.x + cellW * 0.25f,
                          grid.RectFull.Min.y + cellH * 0.5f);
    const ImVec2 rowEnd(grid.RectFull.Min.x + cellW * 7.0f,
                        grid.RectFull.Min.y + cellH * 0.5f);

    // The system clipboard holds something DIFFERENT, so a test that passes
    // by accident through the fallback path is impossible.
    ImGui::SetClipboardText("not-the-selection");

    ctx->MouseMoveToPos(rowStart);
    ctx->MouseDown(ImGuiMouseButton_Left);
    ctx->MouseMoveToPos(rowEnd);
    ctx->Yield(2);
    ctx->MouseUp(ImGuiMouseButton_Left);
    ctx->Yield(2);

    ctx->MouseClick(ImGuiMouseButton_Middle);
    ctx->KeyPress(ImGuiKey_Enter); // so `cat` flushes the line back
    bool echoed = false;
    for (int i = 0; i < 200 && !echoed; i++) {
      ctx->Yield();
      const term::Screen* sc = ui::TerminalScreenOf(*s_App);
      if (sc == nullptr)
        continue;
      for (int r = 1; r < sc->Rows() && !echoed; r++)
        echoed = sc->Line(r).find("pasteme") != std::string::npos;
    }
    IM_CHECK(echoed);
    // ...and it was the SELECTION, not the clipboard.
    bool wrong = false;
    const term::Screen* sc = ui::TerminalScreenOf(*s_App);
    for (int r = 0; r < sc->Rows(); r++)
      wrong =
          wrong || sc->Line(r).find("not-the-selection") != std::string::npos;
    IM_CHECK(!wrong);

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
  };

  // T8. Tabs. The part worth a test is not that a second tab appears — it is
  // that the one you are NOT looking at keeps being drained, and that closing
  // a tab kills its process instead of orphaning it.
  t = IM_REGISTER_TEST(engine, "terminal", "tabs_run_and_drain_in_background");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    s_App->settings.terminalPanel = true;
    ctx->Yield(6);
    IM_CHECK_EQ(ui::TerminalSessionCount(*s_App), 1);

    ui::NewTerminalTab(*s_App);
    ctx->Yield(4);
    IM_CHECK_EQ(ui::TerminalSessionCount(*s_App), 2);

    // ~180 KB of output, then a marker, then exit. Well past a pty buffer
    // (~64 KB), so a tab nobody drains BLOCKS mid-write: it never prints the
    // marker and never exits. That is the failure this loop exists to
    // prevent — a build left running in another tab that silently stops.
    ui::StartTerminalCommand(
        *s_App, "/bin/sh",
        {"-c", "i=0; while [ $i -lt 4000 ]; do "
               "printf 'bg-%d-padding-padding-padding-pad\n' $i; "
               "i=$((i+1)); done; printf 'BG-FINISHED\n'"});
    ctx->Yield(4);

    // Look away: tab 1 is on screen, tab 2 is drawn by nobody.
    ui::SelectTerminalTab(*s_App, 0);
    ctx->Yield(4);
    // Really looking away. The tab bar owns the selection, so asking is not
    // the same as getting it — and if this slips back to tab 1 the rest of
    // the test proves nothing, because the "background" tab would be the one
    // on screen.
    IM_CHECK_EQ(ui::TerminalActiveTab(*s_App), 0);
    IM_CHECK(ui::TerminalTabScreen(*s_App, 1) != nullptr);

    bool finished = false;
    for (int i = 0; i < 600 && !finished; i++) {
      ctx->Yield();
      finished = !ui::TerminalTabRunning(*s_App, 1); // ran to completion
    }
    IM_CHECK(finished);

    // ...and its output really landed, rather than the child having died.
    const term::Screen* bg = ui::TerminalTabScreen(*s_App, 1);
    IM_CHECK(bg != nullptr);
    bool marker = false;
    for (int r = 0; r < bg->Rows() && !marker; r++)
      marker = bg->Line(r).find("BG-FINISHED") != std::string::npos;
    IM_CHECK(marker);

    // Closing reaps: what is left is tab 1's own shell, still alive.
    ui::CloseTerminalTab(*s_App, 1);
    ctx->Yield(2);
    IM_CHECK_EQ(ui::TerminalSessionCount(*s_App), 1);
    IM_CHECK(ui::TerminalRunning(*s_App));

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
    IM_CHECK_EQ(ui::TerminalSessionCount(*s_App), 0); // and off means gone
  };

  t = IM_REGISTER_TEST(engine, "terminal", "ctrl_shift_t_opens_a_tab");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    ctx->SetRef("//##MainMenuBar");
    s_App->settings.terminalPanel = true;
    ctx->Yield(6);
    ui::StartTerminalCommand(*s_App, "/bin/sh", {"-c", "cat"});
    ctx->Yield(10);
    IM_CHECK_EQ(ui::TerminalSessionCount(*s_App), 1);

    ImGuiWindow* win = ctx->WindowInfo("//Terminal###TerminalPanel").Window;
    IM_CHECK(win != nullptr);
    ctx->MouseMoveToPos(ImVec2(win->Pos.x + win->Size.x * 0.5f,
                               win->Pos.y + win->Size.y * 0.6f));
    ctx->Yield(2);
    IM_CHECK(ui::TerminalCapturesKeys(*s_App));

    // The chord is the panel's, not the app's and not the program's: the
    // terminal swallows the keyboard here, so this is the only layer that
    // can claim it.
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_T);
    ctx->Yield(6);
    IM_CHECK_EQ(ui::TerminalSessionCount(*s_App), 2);

    // Ctrl+Shift+PageUp/PageDown move between them — and the press must NOT
    // also reach the program as a PageUp, or scrolling a TUI would jump tabs.
    IM_CHECK_EQ(ui::TerminalActiveTab(*s_App), 1); // the new one is on top
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_PageUp);
    ctx->Yield(4);
    IM_CHECK_EQ(ui::TerminalActiveTab(*s_App), 0);
    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_PageDown);
    ctx->Yield(4);
    IM_CHECK_EQ(ui::TerminalActiveTab(*s_App), 1);
    IM_CHECK_EQ(ui::TerminalSessionCount(*s_App), 2); // moved, not closed

    ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_W);
    ctx->Yield(6);
    IM_CHECK_EQ(ui::TerminalSessionCount(*s_App), 1);

    s_App->settings.terminalPanel = false;
    ctx->Yield(3);
  };

  // Frame timing is always measured; the HUD is not shown unless asked for
  // (docs/rendering/frame-timing.md). Both halves asserted here — a
  // measurement nobody can reach and an overlay nobody asked for are the two
  // ways this feature goes wrong.
  // Category "frametime", NOT "perf": the test engine routes a category of
  // exactly "perf" into ImGuiTestGroup_Perfs, which this suite never queues
  // (it queues ImGuiTestGroup_Tests) — the test would list but never run.
  // Turning the terminal preference off and on again must give a working
  // shell. ShutdownTerminal terminated the PTY but KEPT the TerminalState, so
  // the panel could never open a shell again until the app restarted — the
  // same one-shot trap as the chooser this panel replaced (2eda31d).
  //
  // This comment lived in GuiTestsViewport.cpp until 2026-08-25, above an
  // unrelated `handles` test: the RegisterGuiTests split moved the test and
  // left its explanation behind. Worth knowing about that refactor — the test
  // NAMES were verified byte-identical, and nothing checks that a comment
  // travelled with the code it describes.
  t = IM_REGISTER_TEST(engine, "terminal", "reopens_after_being_turned_off");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    const bool had = s_App->settings.terminalPanel;
    s_App->settings.terminalPanel = true;
    ctx->Yield(8);
    IM_CHECK(ui::TerminalRunning(*s_App));

    s_App->settings.terminalPanel = false;
    ctx->Yield(4);
    IM_CHECK(!ui::TerminalRunning(*s_App)); // ended, not hidden

    // And again — this is what used to be impossible.
    s_App->settings.terminalPanel = true;
    ctx->Yield(8);
    IM_CHECK(ui::TerminalRunning(*s_App));

    s_App->settings.terminalPanel = had;
    ctx->Yield(4);
  };
#endif

  // F10: the other half. Autosave writing the right documents is only useful
  // if recovery puts them back — and the old path opened the sidecar AS the
  // project, which on a multi-document project would have replaced it with
  // one part.
}
