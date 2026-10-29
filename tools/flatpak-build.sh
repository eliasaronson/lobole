#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build (and user-install) the Loböle Flatpak.
#
# Runs entirely in user mode via nix-shell, so it needs NO NixOS
# `services.flatpak.enable` and no root — just a working `nix-shell`. The first
# build compiles OpenCASCADE from source (~20-40 min); --ccache makes reruns
# fast. Work/state dirs live OUTSIDE the repo so the app module's `type: dir`
# source doesn't recursively copy them.
#
# Usage:
#   tools/flatpak-build.sh            # build + install to the user
#   tools/flatpak-build.sh --run      # after a build, launch the app
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$REPO/packaging/flatpak/org.lobole.Lobole.yaml"
WORK="$(dirname "$REPO")/lobole-flatpak-work"
BUILDDIR="$WORK/build"
STATEDIR="$WORK/state"   # persistent: downloaded sources + ccache
APP_ID="org.lobole.Lobole"

mkdir -p "$WORK"

if [[ "${1:-}" == "--run" ]]; then
  exec nix-shell -p flatpak --run "flatpak --user run $APP_ID ${*:2}"
fi

# Jobs by free memory, not cores (tools/build-jobs): flatpak-builder runs
# one compiler per core otherwise.
JOBS="$("$REPO/tools/build-jobs")"

exec nix-shell -p flatpak flatpak-builder appstream --run "bash -euc '
  flatpak --user remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
  flatpak-builder \
    --user --force-clean --ccache --jobs=$JOBS \
    --install-deps-from=flathub \
    --state-dir=\"$STATEDIR\" \
    --install \
    \"$BUILDDIR\" \"$MANIFEST\"
'"
