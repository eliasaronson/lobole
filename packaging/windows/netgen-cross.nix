# Netgen for the Windows cross target, built from scratch. The nixpkgs netgen
# derivation is deeply tied to Python/MPI/tcl/tk/GUI (unbuildable cross), but we
# only need the headless mesher libraries (nglib + ngcore) linked against our
# cross OpenCASCADE. So: fetch the same source, turn everything optional off.
#
# STATUS: NOT working — Netgen's source assumes MSVC on Windows. Its CMake adds
# MSVC-only compiler flags under `if(WIN32)` (/bigobj /MP /W1 /wd4068) which
# mingw g++ rejects as bogus input files, and libsrc/core/utils.cpp does a
# FARPROC→void* cast that mingw won't do implicitly. Making it mingw-clean needs
# upstream-style patches (guard the flags on `if(MSVC)`, reinterpret_cast the
# FARPROC). Deferred — FEM meshing on Windows waits on those patches. Kept here
# as the starting point.
{ cross, occ }:
cross.stdenv.mkDerivation {
  pname = "netgen";
  version = "6.2.2605";
  src = cross.netgen.src; # reuse the pinned NGSolve/netgen source (no re-hash)

  nativeBuildInputs = with cross.buildPackages; [ cmake ninja git ];
  buildInputs = [ occ cross.zlib ];

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DUSE_OCC=ON"
    "-DUSE_GUI=OFF"
    "-DUSE_PYTHON=OFF"
    "-DUSE_MPI=OFF"
    "-DUSE_SUPERBUILD=OFF"
    "-DUSE_NATIVE_ARCH=OFF"
    "-DUSE_JPEG=OFF"
    "-DUSE_MPEG=OFF"
    "-DBUILD_STUB_FILES=OFF"
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
  ];

  # The nixpkgs source has python/tcl substituteInPlace patches in its prePatch;
  # we skip all that — a plain source build is what we want.
  dontPatch = true;
}
