# Minimal OpenCASCADE for the mingw-w64 Windows cross target.
# Stock nixpkgs opencascade-occt is Linux-shaped: it pulls tcl/tk (Draw) — which
# drag in bash (unbuildable for Windows) — plus X11/GL for Visualization. We
# don't need Draw or Visualization (the app renders via Vulkan itself), so strip
# every optional third-party input and disable those modules. Keeps
# FoundationClasses/ModelingData/ModelingAlgorithms/DataExchange/AppFramework.
{ cross }:
cross.opencascade-occt.overrideAttrs (old: {
  # The same kernel patches as the Linux overlay (nix/occt-overlay.nix).
  patches = (old.patches or [ ]) ++ [
    ../../nix/patches/occt-shell-cap-order.patch
    ../../nix/patches/occt-fillet-reorder-faces-loop.patch
    ../../nix/patches/occt-draft-null-curve-and-parallel.patch
    ../../nix/patches/occt-topopebrep-unclosed-wire-iterator.patch
    ../../nix/patches/occt-offset-arc-bind-order.patch
    ../../nix/patches/occt-offset-inter3d-progress.patch
  ];
  # The patches it carries, as the Linux overlay lists them.
  postInstall = (old.postInstall or "") + ''
    mkdir -p $out/share/lobole
    printf '%s\n' occt-shell-cap-order.patch occt-fillet-reorder-faces-loop.patch \
      occt-draft-null-curve-and-parallel.patch \
      occt-topopebrep-unclosed-wire-iterator.patch \
      occt-offset-arc-bind-order.patch occt-offset-inter3d-progress.patch \
      > $out/share/lobole/occt-patches
  '';
  buildInputs = [ ];                       # drop tcl tk libglvnd glu libx* rapidjson
  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    # GCC 16.2 at -O3 miscompiles BRepClass3d_SClassifier.cxx, here as in the
    # Linux overlay: a point-in-solid test calls through a garbage vtable
    # (docs/packaging/README.md#gcc-16-and-ipa-cp-clones).
    "-DCMAKE_CXX_FLAGS=-fno-ipa-cp-clone"
    "-DBUILD_LIBRARY_TYPE=Shared"
    "-DBUILD_MODULE_Draw=OFF"
    "-DBUILD_MODULE_Visualization=OFF"
    "-DUSE_TK=OFF"
    "-DUSE_FREETYPE=OFF"
    "-DUSE_VTK=OFF"
    "-DUSE_FREEIMAGE=OFF"
    "-DUSE_RAPIDJSON=OFF"
    "-DUSE_OPENGL=OFF"
    "-DUSE_GLES2=OFF"
    "-DUSE_TBB=OFF"
    "-DBUILD_RELEASE_DISABLE_EXCEPTIONS=FALSE"
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.10"
    "-DBUILD_DOC_Overview=OFF"
  ];
})
