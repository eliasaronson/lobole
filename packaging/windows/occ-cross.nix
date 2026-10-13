# Minimal OpenCASCADE for the mingw-w64 Windows cross target.
# Stock nixpkgs opencascade-occt is Linux-shaped: it pulls tcl/tk (Draw) — which
# drag in bash (unbuildable for Windows) — plus X11/GL for Visualization. We
# don't need Draw or Visualization (the app renders via Vulkan itself), so strip
# every optional third-party input and disable those modules. Keeps
# FoundationClasses/ModelingData/ModelingAlgorithms/DataExchange/AppFramework.
{ cross }:
cross.opencascade-occt.overrideAttrs (old: {
  buildInputs = [ ];                       # drop tcl tk libglvnd glu libx* rapidjson
  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    # mingw GCC 16.2 at -O3 miscompiles BRepClass3d_SClassifier.cxx: IPA-CP
    # clones NCollection_UBTree::Select for the LINE selector, devirtualized,
    # and the POINT selector's recursion calls that clone, so
    # BRepClass3d_BndBoxTreeSelectorLine::Accept runs on the point selector
    # and reads a curve from past its end. A solid classifier on a plain box
    # crashed (tests/fuzz/cases/crash-181.jsonl, Hole 'H6'); -O2, or -O3
    # without the cloning, is right, and so is the Linux build.
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
