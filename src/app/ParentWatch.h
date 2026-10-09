// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// An app a test launched must not outlive the test.
//
// A smoke's python process can die without running a line of cleanup — a
// ctest timeout, an agent stopping it, SIGKILL — and the `designer --headless
// --remote 0` it started then reparents to init and serves nothing, forever:
// two of them were found 16 hours on, each holding a scratch state dir and
// its memory. `--exit-with-parent`, or DESIGNER_EXIT_WITH_PARENT=<launcher
// pid>, asks the kernel for SIGTERM when the parent dies
// (prctl(PR_SET_PDEATHSIG)); the headless and offscreen loops already stop
// cleanly on SIGTERM. tests/smoke_app.py sets the variable for every launch.
//
// The environment form names the launcher, which closes the one race the
// flag cannot: a parent that died between fork and this call. The process is
// then already init's (or a subreaper's), so the kernel will never signal it,
// and only the pid says it was orphaned. The variable is removed from the
// environment once read, so nothing this process spawns inherits it.
//
// Returns -1 to carry on, or the exit status main should return: 0 when the
// launcher is already gone, 2 for a request that cannot be honoured (a
// malformed pid, a platform without the call) — refused, never ignored.
// Call it from main, before any thread starts.
int ArmExitWithParent(bool requestedByFlag);
