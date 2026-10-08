{
  description = "Designer — parametric B-Rep CAD (Vulkan, OpenCASCADE, PlaneGCS)";

  # ---------------------------------------------------------------------------
  # Inputs
  #
  # nixpkgs provides opencascade-occt 7.9.3 (matches what the CMake config
  # package expects — the TKDE* library split is handled in CMakeLists.txt).
  #
  # The four git submodules are pinned here as flake inputs (flake = false)
  # rather than relying on `?submodules=1`, so `nix build` is reproducible from
  # a plain flake ref. planegcs and deps/fonts are vendored in-tree (git-tracked)
  # and come in with the main source. Keep these SHAs in sync with .gitmodules
  # when the submodules are bumped (`git submodule status`).
  # ---------------------------------------------------------------------------
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    imgui-src = {
      url = "github:ocornut/imgui/4806a1924ff6181180bf5e4b8b79ab4394118875"; # v1.91.9b-docking
      flake = false;
    };
    imgui-test-engine-src = {
      url = "github:ocornut/imgui_test_engine/5144b462b33effcacb2890fe17b2126c861d8ab0"; # v1.91.9
      flake = false;
    };
    nfd-src = {
      url = "github:btzy/nativefiledialog-extended/fc168e8605bfa51aaec22ab0c4e46b9de665a437"; # v1.3.0
      flake = false;
    };
    # CAM's polygon offsetting: without it CMake configures with CAM off.
    clipper2-src = {
      url = "github:AngusJohnson/Clipper2/ef88ee97c0e759792e43a2b2d8072def6c9244e8"; # Clipper2_1.5.4
      flake = false;
    };
  };

  outputs = { self, nixpkgs, imgui-src, imgui-test-engine-src, nfd-src, clipper2-src }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };

      # Netgen minus MPI/python/GUI — see the file for what that saves and
      # why deleting the files afterwards does not work.
      netgenLean = import ./nix/netgen-lean.nix pkgs;

      # Everything the app links against at build/run time. Mirrors the
      # `Libraries` block of shell.nix. Kept in a `let` so the package and the
      # dev shell share one list and can't drift.
      runtimeDeps = (with pkgs; [
        vulkan-headers
        vulkan-loader
        glfw
        glm
        spdlog
        nlohmann_json
        opencascade-occt # CAD kernel — find_package(OpenCASCADE)
        eigen # PlaneGCS + FEM solver
        boost # PlaneGCS (headers)
        spectra # sparse eigensolver (HAVE_SPECTRA), header-only
        dbus # nativefiledialog-extended xdg-desktop-portal backend
        systemd # libsystemd.pc — dbus-1.pc Requires.private needs it resolvable
        xorg.libX11 # non-blocking XMapWindow path (main.cpp)
        catch2_3 # find_package(Catch2) runs at configure even for the app target
      ]) ++ [ netgenLean ];

      designer = pkgs.stdenv.mkDerivation {
        pname = "designer";
        version = "0.1.0"; # Keep in sync with CMake project(VERSION).

        # The flake source is the git tree (submodule dirs arrive empty and are
        # overlaid in postUnpack below). assets/ and deps/{planegcs,fonts} are
        # git-tracked so they come in here directly.
        src = self;

        nativeBuildInputs = with pkgs; [
          cmake
          ninja
          pkg-config
          shaderc # glslc — hard requirement; compiles shaders at build time
        ];

        buildInputs = runtimeDeps;

        # Drop the pinned submodule inputs into deps/ where CMakeLists expects
        # them (add_subdirectory(deps/nativefiledialog-extended), IMGUI_DIR, …).
        # Copy (not symlink) so the tree is writable for the in-tree builds.
        postUnpack = ''
          for d in imgui:${imgui-src} \
                   imgui_test_engine:${imgui-test-engine-src} \
                   nativefiledialog-extended:${nfd-src} \
                   clipper2:${clipper2-src}; do
            name="''${d%%:*}"; path="''${d#*:}"
            rm -rf "$sourceRoot/deps/$name"
            cp -r --no-preserve=mode,ownership "$path" "$sourceRoot/deps/$name"
          done
        '';

        # nixpkgs passes -DCMAKE_BUILD_TYPE=Release by default, so the
        # `if(NOT CMAKE_BUILD_TYPE) -> Debug` fallback in CMakeLists is skipped.
        # Build only the app target (it depends on the `shaders` target, so the
        # .spv files get built too); skip designer_tests here.
        ninjaFlags = [ "designer" ];
        enableParallelBuilding = true;

        # Keep the build-tree PlaneGCS RPATH relocatable. CMake's canonical
        # install rules place the app in bin/, PlaneGCS in lib/, and runtime
        # data under share/designer; the app resolves data relative to itself.
        cmakeFlags = [ "-DCMAKE_BUILD_RPATH_USE_ORIGIN=ON" ];

        # buildInputs' store paths land in the binary's RPATH automatically
        # (OCC/netgen/etc. are linked by absolute path via CMake imported
        # targets), so no LD_LIBRARY_PATH wrapping is needed. The Vulkan ICD is
        # the host's responsibility — on NixOS the loader finds it via
        # /run/opengl-driver (hardware.graphics.enable = true).

        meta = with pkgs.lib; {
          description = "Parametric B-Rep CAD (Vulkan, OpenCASCADE, PlaneGCS)";
          platforms = [ "x86_64-linux" ];
          mainProgram = "lobole";
          # AGPL-3.0-or-later, with a section-7 additional permission to link
          # Dear ImGui Test Engine (see LICENSE + README). Bundled LGPL/MPL/etc.
          # dependencies are inventoried in THIRD_PARTY.md.
          license = licenses.agpl3Plus;
        };
      };

      designerHeadlessChecks = designer.overrideAttrs (old: {
        pname = "designer-headless-checks";
        nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.git pkgs.python3 ];
        ninjaFlags = [ "designer" "designer_tests" "femgpu_tests" ];
        doCheck = true;
        checkPhase = ''
          runHook preCheck
          export HOME="$TMPDIR/home"
          export XDG_CACHE_HOME="$HOME/.cache"
          export DESIGNER_TOPO_GPU=0
          mkdir -p "$XDG_CACHE_HOME"
          ctest -LE gui -j "$NIX_BUILD_CORES" --output-on-failure
          runHook postCheck
        '';
      });
    in
    {
      packages.${system} = {
        default = designer;
        designer = designer;

        # A headless image: the document engine and its JSON-RPC surface with
        # no GPU, no display server and no desktop stack. That is the whole
        # application minus the viewport — modelling, measurement, FEM,
        # drawings, import/export — which is what CI, batch jobs and agents
        # actually drive. A GUI image would need the host's GPU drivers
        # inside it; use Nix or the Flatpak for that.
        #
        # Build and run:
        #   nix build .#dockerHeadless && docker load < result
        #   docker run --rm -p 8437:8437 designer-headless:latest
        #   python3 tools/designer_remote.py 8437 get_state
        #
        # --remote-bind-all is what makes the published port reachable: the
        # server is loopback-only by default because it is unauthenticated
        # and touches files. Inside a container with a namespaced network
        # that is the right trade; do not publish the port to a network you
        # do not trust.
        dockerHeadless = pkgs.dockerTools.buildLayeredImage {
          name = "designer-headless";
          tag = "latest";
          # Layer the closure so a rebuild of the app does not re-push
          # OpenCASCADE and Netgen.
          maxLayers = 100;
          contents = [
            designer
            pkgs.cacert  # TLS roots, for anything that reaches out
            pkgs.busybox # a shell, so `docker exec` is useful
          ];
          # No python inside: clients drive the port from OUTSIDE the
          # container, and it costs 200 MB.
          #
          # The image is designer's runtime closure. Netgen comes from
          # nix/netgen-lean.nix, built without MPI, python bindings and its
          # GUI, so numpy and gfortran are not in it (the closure measured
          # 503 MiB there, against 1.8 GiB with stock netgen).
          config = {
            Entrypoint = [ "${designer}/bin/designer" ];
            Cmd = [ "--headless" "--remote" "8437" "--remote-bind-all" ];
            ExposedPorts = { "8437/tcp" = { }; };
            WorkingDir = "/work";
            Labels = {
              "org.opencontainers.image.title" = "Designer (headless)";
              "org.opencontainers.image.description" =
                "Parametric B-Rep CAD engine with a JSON-RPC interface; no GUI.";
              "org.opencontainers.image.licenses" = "AGPL-3.0-or-later";
              "org.opencontainers.image.source" =
                "https://github.com/eliasaronson/Designer";
            };
          };
        };
      };

      checks.${system}.headless = designerHeadlessChecks;

      # `nix develop` == today's `nix-shell shell.nix`: build tools + libs +
      # the test/tooling extras (validation layers, python-mcp, ccache).
      devShells.${system}.default = pkgs.mkShell {
        inputsFrom = [ designer ];
        packages = with pkgs; [
          gcc
          ccache
          git
          vulkan-validation-layers
          (python3.withPackages (ps: [ ps.mcp ps.pillow ps.mypy ]))
        ];
        # Let the Vulkan loader find the validation layers from the dev shell.
        VK_LAYER_PATH = "${pkgs.vulkan-validation-layers}/share/vulkan/explicit_layer.d";
      };
    };
}
