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
# - occt-offset-arc-bind-order.patch: BRepOffset_MakeOffset::BuildOffsetByArc
#   registered the offset faces in the order of a map hashed on shape
#   addresses, so the same shell came back valid from one copy of a solid and
#   invalid from the next (the toolmaker's clamp's M3 screw). It now visits
#   them in the order they were bound. Upstream's OCCT#1600 (its first
#   commit), backported to 7.9; not yet merged there.
# - occt-offset-inter3d-progress.patch: the arc-join offset intersected two
#   neighbouring offset faces with a pave filler that never saw the caller's
#   progress indicator, and on a worm one edge/face pair of it ran for
#   minutes past a deadline; the range now reaches the edge/face
#   intersector, which asks it between its extremum searches
#   (nix/patches/upstream/offset-inter3d-progress.md).
# - occt-error-handler-thread-local.patch: every OCC_CATCH_SIGNALS (two in
#   each function evaluation of GeomLib_CheckCurveOnSurface, which every
#   boolean's face/face tolerance check runs) linked a Standard_ErrorHandler
#   into ONE process-wide list behind ONE mutex, so query booleans on
#   separate threads queued on it: an exact clash check spent more time in
#   the kernel than computing. The list is now per thread; it only ever held
#   the thread's own handlers anyway. Upstream since OCCT 8.0 (OCCT#980,
#   which also reworks the class); this is the .cxx half alone, the ABI
#   unchanged (docs/performance/README.md#the-kernels-error-handler-lock).
#
# And no IPA-CP clones: GCC 16 at -O3 miscompiles BRepClass3d_SClassifier.cxx,
# so every point-in-solid test could call through a garbage vtable and every
# boolean's refusal check crashed (docs/packaging/README.md#gcc-16-and-ipa-cp-clones).
#
# The install lists the patches it carries in share/lobole/occt-patches, one
# name a line, which CMakeLists.txt reads at configure time
# (src/cad/OccPatches.h): the Shell draws less on a patched kernel, ping
# reports the list, and the tests of what a patch fixes skip where it is not
# there -- CI's stock Ubuntu jobs build against apt's unpatched OpenCASCADE.
final: prev:
let
  ours = [
    ./patches/occt-shell-cap-order.patch
    ./patches/occt-fillet-reorder-faces-loop.patch
    ./patches/occt-draft-null-curve-and-parallel.patch
    ./patches/occt-topopebrep-unclosed-wire-iterator.patch
    ./patches/occt-offset-arc-bind-order.patch
    ./patches/occt-offset-inter3d-progress.patch
    ./patches/occt-error-handler-thread-local.patch
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
