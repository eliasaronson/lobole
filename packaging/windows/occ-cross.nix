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
