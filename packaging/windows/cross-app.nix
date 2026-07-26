# Cross-compile Designer for Windows (x86_64-w64-mingw32) from Linux, via nixpkgs
# cross infrastructure. Scope: the windowed app + PlaneGCS sketch solver + Spectra
# modal, minus Netgen (FEM meshing) and GPU-FEM (Vulkan compute) — deferred to
# keep the build lighter. Shaders compile natively (glslc → arch-independent
# SPIR-V). Build:  nix-build packaging/windows/cross-app.nix
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

  # Boost headers only. PlaneGCS uses Boost headers, but Boost's own cross-build
  # fails on its MPI component, and CMake 3.30+ needs a BoostConfig.cmake for
  # find_package(Boost). So we extract just the headers and ship a hand-written
  # config that exposes the Boost::boost INTERFACE target — no compiled libs.
  boostHeaders = pkgs.stdenvNoCC.mkDerivation {
    pname = "boost-headers-cross";
    version = "1.89.0";
    src = cross.boost.src;
    dontConfigure = true;
    dontBuild = true;
    installPhase = ''
      runHook preInstall
      mkdir -p $out/include $out/lib/cmake/Boost-1.89.0
      cp -r boost $out/include/
      cp ${./BoostConfig.cmake} $out/lib/cmake/Boost-1.89.0/BoostConfig.cmake
      cp ${./BoostConfigVersion.cmake} $out/lib/cmake/Boost-1.89.0/BoostConfigVersion.cmake
      runHook postInstall
    '';
  };
in
cross.stdenv.mkDerivation {
  pname = "designer-win";
  version = "0.1.0";
  src = ../..;

  # Build-platform tools (run on Linux): CMake/Ninja + glslc for SPIR-V.
  nativeBuildInputs = with pkgs.buildPackages; [ cmake ninja pkg-config shaderc ];

  # Host-platform (Windows) libraries.
  buildInputs = [ occ spdlog boostHeaders ] ++ (with cross; [
    eigen
    spectra
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

  # Build the app + its PlaneGCS shared lib.
  ninjaFlags = [ "designer" ];
  enableParallelBuilding = true;

  # Make bin/ a self-contained, zippable Windows app: nix already copies the
  # vulkan/glfw/spdlog/mingw-runtime DLLs, but OpenCASCADE's live in a
  # non-standard win64/gcc/bin path, so gather them (and libplanegcs.dll) here.
  postInstall = ''
    cp ${occ}/win64/gcc/bin/*.dll $out/bin/
  '';
}
