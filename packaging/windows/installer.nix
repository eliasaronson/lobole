# Package the cross-built Windows app into distributables — all on Linux:
#   - designer-<ver>-win64-portable.zip   (unzip and run bin/designer.exe)
#   - designer-<ver>-win64-setup.exe       (NSIS installer, makensis runs on Linux)
# Build:  nix-build packaging/windows/installer.nix
{ }:
let
  pkgs = import <nixpkgs> { config.allowUnsupportedSystem = true; };
  app = import ./cross-app.nix { };
  version = "0.1.0";
in
pkgs.stdenvNoCC.mkDerivation {
  pname = "designer-win-dist";
  inherit version;
  dontUnpack = true;

  nativeBuildInputs = [ pkgs.nsis pkgs.zip ];

  buildPhase = ''
    runHook preBuild
    # Stage the self-contained bundle (bin/ + share/) writable.
    mkdir -p app/bin app/share
    cp -r ${app}/bin/. app/bin/
    cp -r ${app}/share/. app/share/
    chmod -R u+w app

    # Portable zip: unzip anywhere, run bin/designer.exe.
    zip -qr designer-${version}-win64-portable.zip app/bin app/share

    # NSIS setup.exe (makensis reads app/{bin,share} relative to CWD).
    cp ${./installer.nsi} installer.nsi
    makensis -DVERSION=${version} installer.nsi
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp designer-${version}-win64-portable.zip $out/
    cp designer-${version}-win64-setup.exe $out/
    runHook postInstall
  '';
}
