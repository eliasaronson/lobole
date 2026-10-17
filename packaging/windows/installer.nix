# Package the cross-built Windows app into distributables — all on Linux:
#   - lobole-<ver>-win64-portable.zip   (unzip and run bin/lobole.exe)
#   - lobole-<ver>-win64-setup.exe       (NSIS installer, makensis runs on Linux)
# Build:  nix-build packaging/windows/installer.nix
{ }:
let
  pkgs = import <nixpkgs> { config.allowUnsupportedSystem = true; };
  app = import ./cross-app.nix { };
  version = pkgs.lib.fileContents ../../VERSION;
in
pkgs.stdenvNoCC.mkDerivation {
  pname = "lobole-win-dist";
  inherit version;
  dontUnpack = true;

  nativeBuildInputs = [ pkgs.nsis pkgs.zip ];

  buildPhase = ''
    runHook preBuild
    # Stage the self-contained bundle (bin/ + share/) writable.
    mkdir -p app/bin app/share
    # -L, not -r alone: eight of the DLLs nix puts in bin/ are SYMLINKS into
    # other store paths (glfw, vulkan-1, spdlog, fmt and the four mingw runtime
    # libraries). Copied as links they dangle the moment they leave the store,
    # and every dependent DLL then reports itself missing -- libTKernel.dll
    # "not found" when the file is plainly there, because what is really
    # missing is the libstdc++-6.dll it imports.
    cp -rL ${app}/bin/. app/bin/
    cp -rL ${app}/share/. app/share/
    chmod -R u+w app

    # Portable zip: unzip anywhere, run bin/lobole.exe.
    zip -qr lobole-${version}-win64-portable.zip app/bin app/share

    # NSIS setup.exe (makensis reads app/{bin,share} relative to CWD).
    cp ${./installer.nsi} installer.nsi
    cp ${../../assets/icons/org.lobole.Lobole.ico} lobole.ico
    makensis -DVERSION=${version} installer.nsi
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp lobole-${version}-win64-portable.zip $out/
    cp lobole-${version}-win64-setup.exe $out/
    runHook postInstall
  '';
}
