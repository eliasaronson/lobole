#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build the GUI AppImage (docs/packaging/README.md#the-gui-appimage):
#
#   packaging/appimage/build.sh VERSION OUTDIR
#
# Runs as root in a fresh ubuntu:24.04 image, whose glibc is the oldest the
# AppImage runs on. Installs the build dependencies, builds OpenCASCADE and
# Netgen into $LOBOLE_DEPS (default /opt/lobole-deps) unless they are
# already there, builds the app, and bundles every library except glibc and
# the graphics stack, which come from the host. Bundles Ubuntu's CalculiX
# (ccx) where the app looks for one beside it, usr/lib/lobole/ccx. Writes
# OUTDIR/lobole-VERSION-x86_64.AppImage and, because ccx is GPL and we
# distribute it, OUTDIR/calculix-ccx-<debian version>-source.tar: the exact
# Ubuntu source package of the ccx inside
# (docs/packaging/README.md#the-bundled-calculix).
set -euo pipefail

version="$1"
mkdir -p "$2"
out="$(realpath "$2")"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
deps="${LOBOLE_DEPS:-/opt/lobole-deps}"
jobs="${JOBS:-$(nproc)}"
work="${LOBOLE_APPIMAGE_WORK:-$(mktemp -d)}"

# The stock-ubuntu CI job's list without OpenCASCADE (24.04 has 7.6) and
# GTK (the file dialogs use the portal), plus what deps.sh and OCC's glTF
# reader need, Spectra, and the bundlers' own needs (file, the
# desktop-file check), ccache, which CMake uses when it is there, and the
# ccx the AppImage carries. deb-src too, for that ccx's source package.
export DEBIAN_FRONTEND=noninteractive
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates git curl file python3 desktop-file-utils \
  cmake ninja-build build-essential \
  libvulkan-dev glslc libglfw3-dev libglm-dev \
  libspdlog-dev nlohmann-json3-dev libeigen3-dev libboost-dev \
  libtbb-dev libvterm-dev libdbus-1-dev libx11-dev \
  libfreetype-dev libfontconfig-dev rapidjson-dev libspectra-dev \
  libgl-dev libxi-dev libxmu-dev zlib1g-dev ccache calculix-ccx
# CMake reads the commit with git, which refuses a checkout another user
# owns ("dubious ownership"): CI's container runs as root over the runner's.
git config --global --add safe.directory "$repo"

[ -e "$deps/lib/libTKernel.so" ] || JOBS="$jobs" "$here/deps.sh" "$deps"

cmake -S "$repo" -B "$work/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH="$deps" -DLOBOLE_BUILD_TESTS=OFF
cmake --build "$work/build" -j"$jobs" --target designer
rm -rf "$work/AppDir"
DESTDIR="$work/AppDir" cmake --install "$work/build"

# CalculiX; linuxdeploy below brings its libraries (SPOOLES, ARPACK and the
# OpenMPI it links, BLAS, LAPACK, libgfortran). It goes in usr/bin, where
# linuxdeploy points an executable's rpath at usr/lib ($ORIGIN/../lib): given
# usr/lib/lobole/ccx it wrote $ORIGIN and the copy found none of them. Its
# source package, at exactly the version inside, goes beside the AppImage
# for the release to attach (GPL-2 section 3).
ccx_version="$(dpkg-query -W -f='${source:Version}' calculix-ccx)"
install -D -m755 "$(readlink -f /usr/bin/ccx)" "$work/AppDir/usr/bin/ccx"
install -D -m644 /usr/share/doc/calculix-ccx/copyright \
  "$work/AppDir/usr/share/doc/calculix-ccx/copyright"
rm -rf "$work/ccx-src" && mkdir -p "$work/ccx-src"
(cd "$work/ccx-src" && apt-get source --download-only "calculix-ccx=$ccx_version")
tar -C "$work/ccx-src" --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
  -cf "$out/calculix-ccx-$ccx_version-source.tar" .

# Pinned bundlers, checked, run extracted (a container has no FUSE).
tool() {  # url sha256
  local f="$work/$(basename "$1")"
  [ -e "$f" ] || curl -fsSL -o "$f" "$1"
  echo "$2  $f" | sha256sum -c --quiet
  chmod +x "$f"
  echo "$f"
}
linuxdeploy="$(tool https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage \
  c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d)"
appimagetool="$(tool https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage \
  ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0)"
export APPIMAGE_EXTRACT_AND_RUN=1

# linuxdeploy copies every library the binary needs and sets rpaths; its
# excludelist leaves glibc, libGL, X11 and xcb to the host. The Vulkan
# loader is the host's too, so it finds the host's drivers. So is D-Bus,
# which every desktop has: bundled, it brought libsystemd and libgcrypt but
# not the libgpg-error the excludelist keeps out, and Debian 13 failed.
# linuxdeploy still copies an excluded library's own dependencies, so that
# chain is named too; nothing else loads it.
LD_LIBRARY_PATH="$deps/lib" "$linuxdeploy" --appdir "$work/AppDir" \
  --executable "$work/AppDir/usr/bin/lobole" \
  --executable "$work/AppDir/usr/bin/ccx" \
  --desktop-file "$work/AppDir/usr/share/applications/org.lobole.Lobole.desktop" \
  --icon-file "$work/AppDir/usr/share/icons/hicolor/256x256/apps/org.lobole.Lobole.png" \
  --exclude-library 'libvulkan.so*' --exclude-library 'libdbus-1.so*' \
  --exclude-library 'libsystemd.so*' --exclude-library 'libgcrypt.so*'

# Where fem::FindCalculix looks beside the app. A link: the loader takes
# $ORIGIN from the file it resolves to, usr/bin.
mkdir -p "$work/AppDir/usr/lib/lobole"
ln -sfn ../../bin/ccx "$work/AppDir/usr/lib/lobole/ccx"

ARCH=x86_64 VERSION="$version" "$appimagetool" --no-appstream "$work/AppDir" \
  "$out/lobole-$version-x86_64.AppImage"
