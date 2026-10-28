# The manifold probe for Windows: the cross build of cross-app.nix, but only
# the `mesh_boolean_probe` target -- one boolean of two organic fixture
# meshes, which checks its own answer (tests/mesh_boolean_probe.cpp). The
# app's build has no test suite, so this is what proves the vendored mesh
# boolean builds, links and is right under mingw. Static, so it runs alone:
#
#   nix-build packaging/windows/manifold-probe.nix --cores 8
#   wine result/bin/mesh_boolean_probe.exe     # exit 0, one line saying so
{ }:
(import ./cross-app.nix { }).overrideAttrs (o: {
  pname = "lobole-win-manifold-probe";
  ninjaFlags = [ "mesh_boolean_probe" ];
  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp mesh_boolean_probe.exe $out/bin/
    runHook postInstall
  '';
  postInstall = "";
})
