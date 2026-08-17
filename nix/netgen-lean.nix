# Netgen without the parts Designer never touches.
#
# nixpkgs builds netgen with MPI, python bindings and its own Tk GUI. Designer
# links libnglib/libngcore for tet meshing and nothing else — no MPI ranks, no
# embedded python, no Tk — but those three features drag ~1.3 GiB of non-CAD
# into every runtime closure: numpy (443 MiB), a Fortran compiler (382 MiB),
# openmpi's and pmix's dev outputs.
#
# None of it is *linked*. It is reachable through metadata:
#   - netgen's nix-support/propagated-build-inputs  -> numpy, mpi4py
#   - lib/cmake/netgen/NetgenConfig.cmake, which names openmpi's DEV output
#     -> gfortran-wrapper -> gfortran
#   - a Tcl string inside libnglib.so that dlopens libnggui.so by ABSOLUTE
#     path -> back to the full netgen output -> the two above
#
# Deleting those files from a copy of the output does not work — measured:
# the closure stayed at 1.7 GiB because of that embedded dlopen path. The
# references have to not exist, which means building without the features.
#
# Measured result: designer's runtime closure 1.8 GiB -> 503 MiB, and the
# headless test suite passes unchanged (586/586, FEM meshing included).
pkgs:

pkgs.netgen.overrideAttrs (old: {
  cmakeFlags = (old.cmakeFlags or [ ]) ++ [
    (pkgs.lib.cmakeBool "USE_MPI" false)
    (pkgs.lib.cmakeBool "USE_MPI4PY" false)
    (pkgs.lib.cmakeBool "USE_PYTHON" false)
    (pkgs.lib.cmakeBool "USE_GUI" false)
    (pkgs.lib.cmakeBool "BUILD_STUB_FILES" false) # needs the python module
    (pkgs.lib.cmakeBool "USE_JPEG" false) # GUI-only
    (pkgs.lib.cmakeBool "USE_MPEG" false) # GUI-only
  ];
  # These ARE the removed features; keeping them would re-add the closure.
  propagatedBuildInputs = [ ];
  nativeBuildInputs = builtins.filter (
    p:
    !(
      pkgs.lib.hasInfix "pybind11-stubgen" (p.name or "")
      || pkgs.lib.hasInfix "pythonImportsCheckHook" (p.name or "")
    )
  ) (old.nativeBuildInputs or [ ]);
  pythonImportsCheck = [ ];
  doInstallCheck = false; # the suite drives the python module
  desktopItems = [ ]; # no GUI to launch
})
