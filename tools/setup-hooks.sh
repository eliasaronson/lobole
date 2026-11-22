#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# One-time per clone: point git at the version-controlled hooks in .githooks/.
# (Hooks live in the repo so they're shared, but core.hooksPath is local config.)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
git config core.hooksPath .githooks
echo "core.hooksPath -> .githooks  (pre-commit clang-format check enabled)"
# `git clang-format` with no paths formats every staged file whose extension
# it knows, JSON among them (.clang-format disables the other languages
# too); here it looks at C and C++ alone.
git config clangFormat.extensions c,cc,cpp,cxx,h,hh,hpp,hxx
echo "clangFormat.extensions -> C and C++ only"
