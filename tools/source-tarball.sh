#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# The release's complete source: the repository at HEAD with every submodule
# in place, so a packager downloads one file and checks one checksum instead
# of pinning each submodule a second time (docs/packaging/README.md#the-source-tarball).
#
#   tools/source-tarball.sh <version> <out-dir>   -> <out-dir>/lobole-<version>-src.tar.gz
#
# Built from the commits git records (`git archive`), never the working
# tree, so uncommitted files cannot leak in; gzip -n leaves the time out, so
# the same commit gives the same bytes.
set -euo pipefail
version="$1"
mkdir -p "$2"
out="$(realpath "$2")"
root="$(git -C "$(dirname "$0")/.." rev-parse --show-toplevel)"
prefix="lobole-$version"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cd "$root"
git archive --format=tar --prefix="$prefix/" HEAD > "$work/src.tar"
# Each submodule at the commit HEAD records for it, not whatever is checked
# out: `git ls-tree` reads the gitlink.
git config --file .gitmodules --get-regexp '^submodule\..*\.path$' |
while read -r _ path; do
  sha="$(git ls-tree HEAD "$path" | awk '{print $3}')"
  git -C "$path" archive --format=tar --prefix="$prefix/$path/" "$sha" > "$work/sub.tar"
  tar --concatenate --file="$work/src.tar" "$work/sub.tar"
done
# No .git in a tarball, so the commit CMake would ask git for travels in a
# file (CMakeLists.txt reads it when git cannot answer).
mkdir -p "$work/$prefix"
git rev-parse --short HEAD > "$work/$prefix/.lobole-commit"
tar --append --file="$work/src.tar" -C "$work" --owner=0 --group=0 \
  --mtime='@0' "$prefix/.lobole-commit"

gzip -n -9 < "$work/src.tar" > "$out/$prefix-src.tar.gz"
echo "$out/$prefix-src.tar.gz"
