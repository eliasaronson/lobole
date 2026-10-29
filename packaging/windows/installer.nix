# Package the cross-built Windows app into distributables — all on Linux:
#   - lobole-<ver>-win64-portable.zip   (unzip and run bin/lobole.exe)
#   - lobole-<ver>-win64-setup.exe       (NSIS installer, makensis runs on Linux)
#   - ccx_2.23.src.tar.bz2               (the source of the ccx.exe both carry)
# Build:  nix-build packaging/windows/installer.nix
{ }:
let
  pkgs = import <nixpkgs> { config.allowUnsupportedSystem = true; };
  app = import ./cross-app.nix { };
  version = pkgs.lib.fileContents ../../VERSION;
  # CalculiX for Windows (docs/packaging/README.md#the-bundled-calculix):
  # dhondt.de's build, whose ccx_static.exe has SPOOLES and PaStiX linked in
  # and needs no DLL (the ccx_dynamic ones need MKL's, which is not shipped).
  # nixpkgs cannot cross-build ccx for mingw. Pinned: a changed zip at the
  # same URL fails here rather than shipping unseen.
  ccxWin = pkgs.fetchurl {
    url = "https://www.dhondt.de/calculix_2.23_4win.zip";
    hash = "sha256-h/xOFyHfNzcOxn0KJEuxT5v4BLCpXfSeKuc2nTDscug=";
  };
  # Its source, which the release attaches beside the installer (GPL-2
  # section 3: whoever distributes the binary offers the source).
  ccxSrc = pkgs.fetchurl {
    url = "https://www.dhondt.de/ccx_2.23.src.tar.bz2";
    hash = "sha256-nIg4XBD7BPXcbE6YAnpRvr3YruOSDgUZDWwd0INX1uc=";
  };
in
pkgs.stdenvNoCC.mkDerivation {
  pname = "lobole-win-dist";
  inherit version;
  dontUnpack = true;

  nativeBuildInputs = [ pkgs.nsis pkgs.zip pkgs.unzip ];

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

    # ccx.exe beside lobole.exe, where fem::FindCalculix looks, with the
    # zip's README (who built it, from what) beside the GPL text CMake
    # already installs.
    unzip -q -j ${ccxWin} calculix_2.23_4win/ccx_static.exe \
      calculix_2.23_4win/README_Install -d ccx
    install -m755 ccx/ccx_static.exe app/bin/ccx.exe
    install -Dm644 ccx/README_Install app/share/doc/calculix-ccx/README_Install

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
    cp ${ccxSrc} $out/ccx_2.23.src.tar.bz2
    runHook postInstall
  '';
}
