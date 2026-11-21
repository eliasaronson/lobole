#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Sign Lobole.app inside out: every library, then the bundle, which signs its
# executable and seals its resources (docs/packaging/README.md#signing-and-notarisation).
#
#   packaging/macos/sign.sh Lobole.app          # ad-hoc (codesign -s -)
#   packaging/macos/sign.sh Lobole.app IDENTITY # a Developer ID: hardened
#                                               # runtime and a timestamp,
#                                               # what notarisation requires
#
# Apple Silicon runs nothing unsigned, and packaging/macos/bundle.py's edits
# void the signatures the libraries came with, so even an unsigned release
# is ad-hoc signed here.
set -euo pipefail
app="$1"
identity="${2:--}"
flags=(--force --sign "$identity")
[ "$identity" != "-" ] && flags+=(--options runtime --timestamp)

for lib in "$app"/Contents/Frameworks/*.dylib; do
  codesign "${flags[@]}" "$lib"
done
codesign "${flags[@]}" "$app"
codesign --verify --strict --verbose=2 "$app"
if [ "$identity" = "-" ]; then
  echo "sign: ad-hoc signed; Gatekeeper will ask the user to allow it"
else
  echo "sign: signed as $identity"
fi
