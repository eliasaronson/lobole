#!/usr/bin/env bash
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Build the GUI AppImage (docs/packaging/README.md#the-gui-appimage):
#
#   packaging/appimage/build.sh VERSION OUTDIR
#
# Runs as root in a fresh ubuntu:22.04 image, the oldest Ubuntu LTS in
# standard support, whose glibc (2.35) is the oldest the AppImage runs on.
# Installs the build dependencies and gcc 13 from the toolchain PPA (the
# C++23 here needs it), builds OpenCASCADE, Netgen, Spectra and glslc into
# $LOBOLE_DEPS (default /opt/lobole-deps) unless they are already there,
# builds the app, and bundles every library except glibc and the graphics
# stack, which come from the host; libstdc++ is bundled for a host whose own
# is older (AppRun). Builds Ubuntu 24.04's CalculiX (ccx) source package
# here, 22.04's being older than the app accepts, and bundles it where the
# app looks for one beside it, usr/lib/lobole/ccx. Writes
# OUTDIR/lobole-VERSION-x86_64.AppImage and, because ccx is GPL and we
# distribute it, OUTDIR/calculix-ccx-<debian version>-source.tar: the exact
# source package of the ccx inside
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

# The stock-ubuntu CI job's list without OpenCASCADE and GTK (the file
# dialogs use the portal), plus what deps.sh and OCC's glTF reader need, the
# bundlers' own needs (file, the desktop-file check), ccache, which CMake
# uses when it is there, and what building ccx's package takes. 22.04 has no
# glslc and no Spectra, its libvterm (0.1) predates the selection API the
# terminal uses (deps.sh builds those), and its gcc 11 is too old.
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends software-properties-common gpg-agent
add-apt-repository -y ppa:ubuntu-toolchain-r/test
# ccx's source from 24.04 (noble), sources only: nothing binary comes from it.
echo 'deb-src http://archive.ubuntu.com/ubuntu noble main universe' \
  > /etc/apt/sources.list.d/noble-src.list
apt-get update
apt-get install -y --no-install-recommends \
  ca-certificates git curl file python3 desktop-file-utils \
  cmake ninja-build build-essential g++-13 \
  libvulkan-dev libglfw3-dev libglm-dev \
  libspdlog-dev nlohmann-json3-dev libeigen3-dev libboost-dev \
  libtbb-dev libtool-bin libdbus-1-dev libx11-dev \
  libfreetype-dev libfontconfig-dev rapidjson-dev \
  libgl-dev libxi-dev libxmu-dev zlib1g-dev ccache dpkg-dev devscripts \
  equivs
export CC=gcc-13 CXX=g++-13
# CMake reads the commit with git, which refuses a checkout another user
# owns ("dubious ownership"): CI's container runs as root over the runner's.
git config --global --add safe.directory "$repo"

[ -e "$deps/lib/libTKernel.so" ] || JOBS="$jobs" "$here/deps.sh" "$deps"

PATH="$deps/bin:$PATH" cmake -S "$repo" -B "$work/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH="$deps" -DLOBOLE_BUILD_TESTS=OFF
cmake --build "$work/build" -j"$jobs" --target designer
rm -rf "$work/AppDir"
DESTDIR="$work/AppDir" cmake --install "$work/build"

# CalculiX, 24.04's source package built here with 22.04's own compiler;
# linuxdeploy below brings its libraries (SPOOLES, ARPACK and the OpenMPI it
# links, BLAS, LAPACK, libgfortran). It goes in usr/bin, where linuxdeploy
# points an executable's rpath at usr/lib ($ORIGIN/../lib): given
# usr/lib/lobole/ccx it wrote $ORIGIN and the copy found none of them. The
# source package, exactly what was built, goes beside the AppImage for the
# release to attach (GPL-2 section 3).
ccx_version=2.21-1
rm -rf "$work/ccx-src" "$work/ccx-build" && mkdir -p "$work/ccx-src" "$work/ccx-build"
(cd "$work/ccx-src" && apt-get source --download-only "calculix-ccx=$ccx_version")
(cd "$work/ccx-build" && dpkg-source -x "$work"/ccx-src/calculix-ccx_*.dsc src \
  && mk-build-deps -i -r -t 'apt-get -y --no-install-recommends' src/debian/control \
  && cd src && dpkg-buildpackage -b -uc -us -j"$jobs")
apt-get install -y --no-install-recommends "$work"/ccx-build/calculix-ccx_*.deb
install -D -m755 "$(readlink -f /usr/bin/ccx)" "$work/AppDir/usr/bin/ccx"
install -D -m644 /usr/share/doc/calculix-ccx/copyright \
  "$work/AppDir/usr/share/doc/calculix-ccx/copyright"
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

# The C++ runtime is gcc 13's, newer than 22.04's. It cannot simply be
# bundled: the host's graphics driver loads into the process and may need a
# libstdc++ newer than ours (Mesa links LLVM). So it goes outside every
# rpath, in usr/lib/runtime, and AppRun puts it on the library path only
# where the host's lacks the newest version ours defines.
rt="$work/AppDir/usr/lib/runtime"
mkdir -p "$rt"
for lib in libstdc++.so.6 libgcc_s.so.1; do
  rm -f "$work/AppDir/usr/lib/$lib"
  cp -L "$("$CXX" -print-file-name="$lib")" "$rt/$lib"
done
newest="$(strings "$rt/libstdc++.so.6" | grep -E '^GLIBCXX_3\.4\.[0-9]+$' \
  | sort -t. -k3 -n | tail -1)"
rm -f "$work/AppDir/AppRun"
sed "s/@GLIBCXX@/$newest/" "$here/AppRun" > "$work/AppDir/AppRun"
chmod +x "$work/AppDir/AppRun"

ARCH=x86_64 VERSION="$version" "$appimagetool" --no-appstream "$work/AppDir" \
  "$out/lobole-$version-x86_64.AppImage"
