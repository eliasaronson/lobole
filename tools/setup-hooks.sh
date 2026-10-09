#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# One-time per clone: point git at the version-controlled hooks in .githooks/.
# (Hooks live in the repo so they're shared, but core.hooksPath is local config.)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
git config core.hooksPath .githooks
echo "core.hooksPath -> .githooks  (pre-commit clang-format check enabled)"
