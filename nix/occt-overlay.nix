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
#
# And no IPA-CP clones: GCC 16 at -O3 miscompiles BRepClass3d_SClassifier.cxx,
# so every point-in-solid test could call through a garbage vtable and every
# boolean's refusal check crashed (docs/packaging/README.md#gcc-16-and-ipa-cp-clones).
final: prev: {
  opencascade-occt = prev.opencascade-occt.overrideAttrs (old: {
    patches = (old.patches or [ ]) ++ [ ./patches/occt-shell-cap-order.patch ];
    env = (old.env or { }) // {
      NIX_CFLAGS_COMPILE = toString (old.env.NIX_CFLAGS_COMPILE or "") + " -fno-ipa-cp-clone";
    };
  });
}
