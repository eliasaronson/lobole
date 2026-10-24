# OpenCASCADE with the patches Loböle carries, as an overlay so that every
# package built against it (netgen's OCC interface too) links the same one:
# two libTKOffset.so in one process is not a configuration anyone tests.
#
# - occt-shell-cap-order.patch: MakeThickSolidByJoin glued the shell's faces in
#   an order that followed a map hashed on shape addresses, and with a removed
#   face on the floor of a blind hole about one copy in ten of the same solid
#   came out inside out
#   (docs/modeling/README.md#a-reshaping-feature-keeps-only-a-valid-solid).
#   Not yet upstream.
# - occt-fillet-reorder-faces-loop.patch: ChFi3d_Builder's ReorderFaces
#   walked the faces round a vertex of a fillet's tangent chain with no test
#   of having been somewhere before, and round a seam it went for ever inside
#   BRepFilletAPI_MakeFillet::Add (and the chamfer's): it now throws
#   Standard_ConstructionError (nix/patches/upstream/fillet-reorder-faces-loop.md).
# - occt-draft-null-curve-and-parallel.patch: Draft_Modification::Perform
#   dereferenced the 3D curve of an edge that has none (a sphere's pole) and
#   read the points of an Extrema_ExtCS that found the curve parallel to the
#   surface; both are now a failed draft (Draft_EdgeRecomputation,
#   Draft_VertexRecomputation) (nix/patches/upstream/draft-null-curve-and-parallel.md).
# - occt-topopebrep-unclosed-wire-iterator.patch: TopOpeBRepBuild's
#   CorrectUnclosedWire removed edges from a wire while iterating it, and the
#   iterator faulted (a fillet whose blend went through the old boolean's
#   repair) (nix/patches/upstream/topopebrep-unclosed-wire-iterator.md).
#
# And no IPA-CP clones: GCC 16 at -O3 miscompiles BRepClass3d_SClassifier.cxx,
# so every point-in-solid test could call through a garbage vtable and every
# boolean's refusal check crashed (docs/packaging/README.md#gcc-16-and-ipa-cp-clones).
#
# The install lists the patches it carries in share/lobole/occt-patches, one
# name a line: the tests that need a patched kernel read it at configure time
# (tests/CMakeLists.txt) and skip where it is not there -- CI's stock Ubuntu
# jobs build against apt's unpatched OpenCASCADE.
final: prev:
let
  ours = [
    ./patches/occt-shell-cap-order.patch
    ./patches/occt-fillet-reorder-faces-loop.patch
    ./patches/occt-draft-null-curve-and-parallel.patch
    ./patches/occt-topopebrep-unclosed-wire-iterator.patch
  ];
in
{
  opencascade-occt = prev.opencascade-occt.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ ours;
    postInstall = (old.postInstall or "") + ''
      mkdir -p $out/share/lobole
      printf '%s\n' ${toString (map baseNameOf ours)} > $out/share/lobole/occt-patches
    '';
    env = (old.env or { }) // {
      NIX_CFLAGS_COMPILE = toString (old.env.NIX_CFLAGS_COMPILE or "") + " -fno-ipa-cp-clone";
    };
  });
}
