# Cross-compile Designer for Windows (x86_64-w64-mingw32) from Linux, via nixpkgs
# cross infrastructure. Spike/MVP scope: the windowed app minus the optional
# heavy features (Netgen FEM meshing, Spectra modal, GPU-FEM) to keep the first
# build's dependency + porting surface small. Shaders compile natively (glslc →
# arch-independent SPIR-V). Build:  nix-build packaging/windows/cross-app.nix
{ }:
let
  # allowUnsupportedSystem: some header-only deps (eigen, spectra, …) don't list
  # mingw in meta.platforms though they build/run fine cross-compiled to Windows.
  pkgs = import <nixpkgs> { config.allowUnsupportedSystem = true; };
  cross = pkgs.pkgsCross.mingwW64;
  occ = import ./occ-cross.nix { inherit cross; };
  # spdlog's packaged cross-build runs its own unit tests, which don't link on
  # mingw (test_timezone needs dupenv_s). We only need the library.
  spdlog = cross.spdlog.overrideAttrs (o: {
    doCheck = false;
    cmakeFlags = (o.cmakeFlags or [ ]) ++ [ "-DSPDLOG_BUILD_TESTS=OFF" ];
  });
in
cross.stdenv.mkDerivation {
  pname = "designer-win";
  version = "0.1.0";
  src = ../..;

  # Build-platform tools (run on Linux): CMake/Ninja + glslc for SPIR-V.
  nativeBuildInputs = with pkgs.buildPackages; [ cmake ninja pkg-config shaderc ];

  # Host-platform (Windows) libraries.
  # MVP scope note: Boost is omitted (its nixpkgs cross-build fails on the MPI
  # component; it's headers-only for us). Without it find_package(Boost) fails
  # and PlaneGCS (sketch constraint solver) is disabled for now — a solvable
  # packaging follow-up, not a code blocker. Eigen stays (FEM solver uses it).
  buildInputs = [ occ spdlog ] ++ (with cross; [
    eigen
    glm
    nlohmann_json
    glfw
    vulkan-headers
    vulkan-loader
    windows.mingw_w64_pthreads # imgui_test_engine includes <pthread.h> on mingw
  ]);

  # The repo tree carries a native-Linux build/ dir with a stale CMakeCache.txt;
  # drop it (and other host-only junk) so the cross configure starts clean.
  preConfigure = "rm -rf build result designer-appimage scratch";

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DDESIGNER_BUILD_TESTS=OFF"
    "-DDESIGNER_INSTALL_METAINFO=OFF"
    "-DDESIGNER_ENABLE_FEM_GPU=OFF"
    # find_package(Vulkan) needs to see the cross loader/headers under the sysroot.
    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"
  ];

  # Let CMake find the cross Vulkan/GLFW config packages.
  ninjaFlags = [ "designer" ];
  enableParallelBuilding = true;
}
