# Example usage:
#  mkdir build
#  cd build
#  cmake -g "Visual Studio 14 2015" ..

cmake_minimum_required(VERSION 3.14)
project(designer VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
# std::stacktrace (CrashHandler) lives in libstdc++'s experimental archive
# until GCC ships it in the main library; harmless on compilers that don't
# need it (checked below).
# Export compile_commands.json for clang-tidy / clangd / editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DVK_PROTOTYPES")
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVK_PROTOTYPES")

# Use ccache when available (no-op otherwise) for fast unchanged-TU rebuilds.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
endif()

# Opt-in AddressSanitizer + UndefinedBehaviorSanitizer build
# (cmake -DENABLE_SANITIZERS=ON). Off by default so normal builds pay nothing.
option(ENABLE_SANITIZERS "Build with ASan + UBSan" OFF)
if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
  # Note: GCC 15 in this toolchain rejects -fsanitize-ignorelist/-blacklist, so
  # the OpenCASCADE UBSan `alignment` false positives are filtered at RUNTIME via
  # tools/ubsan-suppressions.txt instead of a compile-time ignore-list.
  #
  # src/common/SanitizerDefaults.cpp carries the runtime options (including
  # that suppressions path) inside each executable, so a sanitized binary
  # behaves the same however it is launched — no wrapper required. It is added
  # to the EXECUTABLES rather than designer_core: the callbacks are only ever
  # called by the runtime, so nothing references them and the linker would
  # drop the object from a static library.
  set(SANITIZER_DEFAULTS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/common/SanitizerDefaults.cpp)
  # Where the UBSan suppression file lives. Global (sanitizer builds only)
  # rather than a source-file property, because the same file is compiled into
  # targets in two different CMake directory scopes.
  add_compile_definitions(DESIGNER_SANITIZER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  message(STATUS "Sanitizers enabled (ASan + UBSan)")
endif()

# Opt-in ThreadSanitizer (cmake -DENABLE_TSAN=ON). SEPARATE from the block
# above because TSan and ASan cannot be combined — they both instrument memory
# accesses and the runtimes refuse to co-exist — so this is its own build tree
# (build-tsan/), run occasionally like tools/coverage.sh rather than in CI.
#
# It exists because ASan and UBSan catch no data races, and the tree stopped
# being single-threaded: STEP import runs on a worker, OCC meshing runs across
# threads grouped by TShape, FEM studies solve async, and the remote server has
# its own thread. The meshing work reasoned carefully about which TShapes are
# shared and grouped so a race is impossible rather than merely absent — TSan
# is the tool that CHECKS that reasoning instead of trusting it
# (docs/capability-review-2026-08-31.md, P3).
option(ENABLE_TSAN "Build with ThreadSanitizer" OFF)
if(ENABLE_TSAN AND ENABLE_SANITIZERS)
  message(FATAL_ERROR
    "ENABLE_TSAN and ENABLE_SANITIZERS are mutually exclusive: ASan and TSan "
    "cannot be linked into the same binary. Use separate build trees.")
endif()
if(ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
  # Same arrangement as the ASan block: the runtime options live IN the binary
  # (src/common/SanitizerDefaults.cpp), so build-tsan/designer run straight
  # from a shell behaves the same as one launched through a wrapper. Without
  # it the first run drowns in Netgen's uninstrumented thread pool.
  set(SANITIZER_DEFAULTS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/common/SanitizerDefaults.cpp)
  add_compile_definitions(DESIGNER_SANITIZER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
                          DESIGNER_TSAN)
  message(STATUS "ThreadSanitizer enabled")
endif()

# Opt-in GCC static analyzer (cmake -DENABLE_ANALYZER=ON). Compile-time only, no
# run needed; emits -Wanalyzer-* warnings by path-exploring each TU (null deref,
# use-after-free, leaks on error paths, ...). Complements the sanitizers, which
# only see executed paths. Caveats: slow (path exploration), C++/template support
# is partial and noisy from dep headers — so it's an occasional audit build, not
# CI, and deliberately NOT -Werror. Applies only to our targets (deps prebuilt).
option(ENABLE_ANALYZER "Build with GCC -fanalyzer static analysis" OFF)
if(ENABLE_ANALYZER)
  add_compile_options(-fanalyzer)
  message(STATUS "GCC static analyzer enabled (-fanalyzer)")
endif()

find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)

# Dear ImGui (SYSTEM: third-party, don't warn on its headers)
set(IMGUI_DIR deps/imgui)
include_directories(SYSTEM ${IMGUI_DIR} ${IMGUI_DIR}/backends ..)

# Dear ImGui Test Engine (GUI testing + the remote UI-parity tier).
#
# OPTIONAL, and the only dependency here that is not open source: it is free
# for individuals, non-profits, entities under 2M USD turnover, and for
# building open-source derivative works (which Designer is) — but a larger
# company that wants to build or ship it needs a paid license from DISCO
# HELLO. -DENABLE_TEST_ENGINE=OFF removes the dependency entirely: no
# sources, no headers, no IMGUI_ENABLE_TEST_ENGINE hooks in imgui.cpp.
#
# What that costs: `--gui-tests` (the GUI suite) and the remote `ui_action`
# tier — clicking the app's own widgets. Modeling, sketching, FEM, drawings,
# assemblies, export, the renderer, screenshots and every semantic RPC are
# unaffected, so an agent can still drive the whole application.
option(ENABLE_TEST_ENGINE
       "Build with Dear ImGui Test Engine (GUI tests + remote ui_action)" ON)

set(IMGUI_TEST_ENGINE_DIR deps/imgui_test_engine/imgui_test_engine)
if(ENABLE_TEST_ENGINE)
  include_directories(SYSTEM ${IMGUI_TEST_ENGINE_DIR})
endif()
set(IMGUI_TEST_ENGINE_SOURCES
    ${IMGUI_TEST_ENGINE_DIR}/imgui_capture_tool.cpp
    ${IMGUI_TEST_ENGINE_DIR}/imgui_te_context.cpp
    ${IMGUI_TEST_ENGINE_DIR}/imgui_te_coroutine.cpp
    ${IMGUI_TEST_ENGINE_DIR}/imgui_te_engine.cpp
    ${IMGUI_TEST_ENGINE_DIR}/imgui_te_exporters.cpp
    ${IMGUI_TEST_ENGINE_DIR}/imgui_te_perftool.cpp
    ${IMGUI_TEST_ENGINE_DIR}/imgui_te_ui.cpp
    ${IMGUI_TEST_ENGINE_DIR}/imgui_te_utils.cpp
)


# OpenCASCADE: prefer the CMake config package (any prefix — NixOS, custom
# installs), fall back to the conventional /usr/include/opencascade layout.
find_package(OpenCASCADE QUIET)
if(OpenCASCADE_FOUND)
    # SYSTEM: OCC headers use deprecated std::iterator etc.; don't warn on them.
    include_directories(SYSTEM ${OpenCASCADE_INCLUDE_DIR})
    message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION} found via CMake config: ${OpenCASCADE_INCLUDE_DIR}")
    set(OPENCASCADE_FOUND TRUE)
else()
    set(OPENCASCADE_INCLUDE_DIR "/usr/include/opencascade")
    if(EXISTS ${OPENCASCADE_INCLUDE_DIR})
        include_directories(SYSTEM ${OPENCASCADE_INCLUDE_DIR})
        message(STATUS "OpenCASCADE includes found at: ${OPENCASCADE_INCLUDE_DIR}")
        set(OPENCASCADE_FOUND TRUE)
    else()
        message(WARNING "OpenCASCADE not found - CAD functionality will be limited")
        set(OPENCASCADE_FOUND FALSE)
    endif()
endif()

if(OPENCASCADE_FOUND)
    # Core OpenCASCADE libraries we need. With the config package these are
    # imported targets; with the /usr fallback, plain library names.
    set(OPENCASCADE_LIBS
        TKernel TKMath TKBRep TKG2d TKG3d TKGeomBase TKGeomAlgo
        TKTopAlgo TKPrim TKMesh TKBO TKXSBase TKShHealing TKOffset
        # Hidden-line-removed projection for 2D drawings (D0+).
        TKHLR
        # Visual-mesh export (interchange X2): the XCAF document stack +
        # RWMesh. The glTF/OBJ/PLY writers live in TKRWMesh through OCCT 7.7
        # and split into TKDE* in 7.8 (appended below when present).
        TKLCAF TKCAF TKXCAF TKVCAF TKRWMesh
    )

    # STEP/STL data-exchange libraries were renamed in OCCT 7.8
    # (TKSTEP*/TKSTL -> TKDESTEP/TKDESTL)
    find_library(OCCT_TKDESTEP TKDESTEP)
    if(TARGET TKDESTEP OR OCCT_TKDESTEP)
        list(APPEND OPENCASCADE_LIBS TKDESTEP TKDESTL TKDEGLTF TKDEOBJ TKDEPLY)
    else()
        # TKXDESTEP carries the XCAF-aware STEP reader/writer
        # (STEPCAFControl_*, the named/coloured import path) through 7.7; in
        # 7.8 it folded into TKDESTEP. Omitting it linked fine until the
        # importer started using XCAF, then failed only on the older OCC —
        # which is Ubuntu's, so nobody building here saw it.
        list(APPEND OPENCASCADE_LIBS TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209
                                     TKXDESTEP TKSTL)
    endif()

    add_definitions(-DHAVE_OPENCASCADE)
endif()

# nativefiledialog-extended - native dialogs via the XDG desktop portal on
# Linux (D-Bus, out-of-process — the old in-process GTK chooser ABORTED the
# whole app when GSettings schemas weren't installed). If no portal service
# runs at runtime, FileDialogs falls back to the ImGui browser.
set(NFDE_DIR deps/nativefiledialog-extended)
if(EXISTS ${CMAKE_SOURCE_DIR}/${NFDE_DIR}/CMakeLists.txt)
    message(STATUS "Building nativefiledialog-extended from submodule (portal)")
    if(UNIX AND NOT APPLE)
        set(NFD_PORTAL ON CACHE BOOL "xdg-desktop-portal dialogs" FORCE)
    endif()
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${NFDE_DIR})
    add_definitions(-DHAVE_NATIVEFILEDIALOG)
    set(HAVE_NATIVEFILEDIALOG ON)
    set(NFD_SOURCES "")
    set(NFD_LIBS nfd)
else()
    message(WARNING "nativefiledialog-extended submodule not found - will use ImGui file browser fallback")
    set(NFD_SOURCES "")
    set(NFD_LIBS "")
endif()

# Libraries
find_package(Vulkan REQUIRED)
set(LIBRARIES "glfw;Vulkan::Vulkan;spdlog::spdlog;${NFD_LIBS}")
# std::stacktrace (CrashHandler) needs libstdc++'s experimental archive
# (-lstdc++exp). Most gcc installs ship it, but some toolchains do not (e.g. the
# Freedesktop Flatpak SDK). Probe, and only link/define when it's actually
# available; CrashHandler falls back to glibc backtrace() when the macro is off.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES stdc++exp)
    check_cxx_source_compiles("#include <stacktrace>
        int main() { return static_cast<int>(std::stacktrace::current().size()); }"
        HAVE_STDCXX_STACKTRACE)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(HAVE_STDCXX_STACKTRACE)
        list(APPEND LIBRARIES stdc++exp) # std::stacktrace backend
    else()
        message(STATUS "libstdc++exp absent — CrashHandler uses glibc backtrace()")
    endif()
endif()

# Add OpenCASCADE libraries if available
if(OPENCASCADE_FOUND)
    list(APPEND LIBRARIES ${OPENCASCADE_LIBS})
endif()

# Core library: geometry, document and scene logic. Must stay free of
# Vulkan/GLFW/ImGui so it can be unit-tested and scripted headlessly.
set(core_sources
    src/render/Camera.cpp
    src/common/BuildInfo.cpp
    src/common/Logger.cpp
    # Per-user directories and the runtime data root. In CORE, not the app:
    # core/parts, core/io/Autosaver and core/io/Settings all resolve paths
    # through it, and a test binary that links only core still needs them.
    src/common/RuntimePaths.cpp
    src/render/OutlineEdges.cpp
    src/cad/CADKernel.cpp
    src/cad/CADMeshConverter.cpp
    src/cad/CADMaterials.cpp
    src/cad/MeshExport.cpp
    src/render/IBL.cpp
    src/cad/SketchPlane.cpp
    src/cad/FaceSelector.cpp
    src/core/io/Autosaver.cpp
    src/core/io/RecentProjects.cpp
    # Pure logic despite living under remote/: a JSON file per open port,
    # no sockets and no ImGui, so the tests can link and exercise it.
    src/remote/InstanceRegistry.cpp
    src/core/io/Checkpoints.cpp
    src/core/io/GitVault.cpp
    src/core/io/ProjectMerge.cpp
    src/core/io/ProjectStamp.cpp
    src/core/io/ImportAssembly.cpp
    src/core/Measure.cpp
    src/core/ThreadStandards.cpp
    src/core/FitStandards.cpp
    src/core/SheetMetal.cpp
    src/core/features/BaseFlangeFeature.cpp
    src/core/features/EdgeFlangeFeature.cpp
    src/core/features/FlatPatternFeature.cpp
    src/core/features/UnfoldFeature.cpp
    src/core/features/HemFeature.cpp
    src/core/features/JogFeature.cpp
    src/core/features/CornerReliefFeature.cpp
    src/core/features/NormalCutFeature.cpp
    src/core/features/RefoldFeature.cpp
    src/core/sheet/Unfold.cpp
    src/core/sheet/Flange.cpp
    src/core/sheet/Check.cpp
    src/core/tube/Rules.cpp
    src/core/tube/Check.cpp
    src/core/tube/CutList.cpp
    src/core/tube/Frame.cpp
    src/core/io/ExportDxf.cpp
    src/core/ToleranceStack.cpp
    src/core/MassProps.cpp
    src/core/Feature.cpp
    src/core/Document.cpp
    src/core/Units.cpp
    src/core/Occurrence.cpp
    src/core/AssemblySolve.cpp
    src/core/AssemblyLoopSolve.cpp
    src/core/drawing/DrawingProjection.cpp
    src/core/drawing/DrawingDocument.cpp
    src/core/cam/JobDocument.cpp
    src/core/cam/CamMachine.cpp
    # Clipper2 is what CAM is optional ON, and these five need none of it: the
    # job document's names, its frames and bounds, what a pick resolves to, the
    # post's dialect list, and where parts sit in the sheet. They are the
    # document model and its file format, which exist whether or not this build
    # can generate a toolpath -- a job tab is a DocKind either way. Only
    # CamGeometry / CamDerive / CamStrategies (and CamAdaptive) actually offset
    # polygons, and those stay behind ENABLE_CAM below.
    src/core/cam/CamNames.cpp
    src/core/cam/CamSetup.cpp
    src/core/cam/CamPicks.cpp
    src/core/cam/CamTravel.cpp
    src/core/cam/CamOpFields.cpp
    src/core/cam/CamPost.cpp
    src/core/cam/CamRotaryFeed.cpp
    src/core/drawing/DrawingExport.cpp
    src/core/drawing/DrawingDimension.cpp
    src/core/drawing/DrawingValidate.cpp
    src/core/drawing/DrawingCallout.cpp
    src/core/io/Format.cpp
    src/core/Command.cpp
    src/core/Expression.cpp
    src/core/catalog/Catalog.cpp
    src/core/io/GeometryCache.cpp
    src/core/parts/AxisStack.cpp
    src/core/parts/FastenerProfile.cpp
    src/core/parts/FastenerTable.cpp
    src/core/io/AtomicWrite.cpp
    src/core/io/Settings.cpp
    src/core/io/WindowGeometry.cpp
    src/core/io/Keymap.cpp
    src/core/io/Serialization.cpp
    src/core/io/SerializeFeatures.cpp
    src/core/io/SerializeStudies.cpp
    src/core/io/SerializeCam.cpp
    src/core/io/SerializeConfigurations.cpp
    src/core/io/SerializeAssembly.cpp
    src/core/features/SketchFeature.cpp
    src/core/sketch/SketchProfile.cpp
    src/core/sketch/SketchInference.cpp
    src/core/sketch/SketchSelection.cpp
    src/core/features/ExtrudeFeature.cpp
    src/core/features/FastenerFeature.cpp
    src/core/features/DerivedFeature.cpp
    src/core/features/DatumPlaneFeature.cpp
    src/core/features/FrameFeature.cpp
    src/core/MeshCollide.cpp
    src/core/MotionStudy.cpp
    src/core/io/GlbAnimation.cpp
    src/core/BodyMode.cpp
    src/core/features/RevolveFeature.cpp
    src/core/features/ShellFeature.cpp
    src/core/features/LoftFeature.cpp
    src/core/features/RibFeature.cpp
    src/core/features/ThreadFeature.cpp
    src/core/features/SweepFeature.cpp
    src/core/features/PipeFeature.cpp
    src/core/features/CopeFeature.cpp
    src/core/features/HoleFeature.cpp
    src/core/features/DraftFeature.cpp
    src/core/features/DatumAxisFeature.cpp
    src/core/features/CurveFeature.cpp
    src/core/features/BooleanFeature.cpp
    src/core/features/SplitFeature.cpp
    src/core/features/RefineFeature.cpp
    src/core/ElementHistory.cpp
    src/core/features/FilletFeature.cpp
    src/core/features/PatternFeature.cpp
    src/core/sketch/SketchSolver.cpp
    src/core/fem/FemStudy.cpp
    src/core/fem/AssemblyStudy.cpp
    src/core/fem/FemSolver.cpp
    src/core/fem/FemTopo.cpp
)

find_package(nlohmann_json 3 REQUIRED)

# PlaneGCS constraint solver (vendored from FreeCAD, LGPL-2.1). Optional:
# needs Eigen3 + Boost headers. Gated by HAVE_PLANEGCS; SketchSolver falls
# back to a no-op stub if unavailable.
find_package(Eigen3 3.3 QUIET)
find_package(Boost QUIET)
if(Eigen3_FOUND AND Boost_FOUND)
    set(PLANEGCS_SOURCES
        deps/planegcs/GCS.cpp deps/planegcs/Constraints.cpp
        deps/planegcs/Geo.cpp deps/planegcs/SubSystem.cpp
        deps/planegcs/qp_eq.cpp)
    # SHARED (not STATIC): PlaneGCS is LGPL-2.1 and locally modified; a shared
    # library satisfies LGPL's relink requirement automatically (users can swap
    # the .so). Perf impact is nil — the designer_core->planegcs boundary is
    # crossed once per sketch solve, and the hot inner loops (+ header-only
    # Eigen) stay inlined inside the .so.
    add_library(planegcs SHARED ${PLANEGCS_SOURCES})
    target_compile_features(planegcs PUBLIC cxx_std_20)
    target_include_directories(planegcs PUBLIC deps/planegcs)
    target_link_libraries(planegcs PUBLIC Eigen3::Eigen Boost::boost)
    set(HAVE_PLANEGCS TRUE)
    message(STATUS "PlaneGCS enabled (Eigen + Boost found)")
else()
    message(WARNING "PlaneGCS disabled: Eigen3/Boost not found")
endif()

# Netgen tetrahedral mesher (LGPL, linked — not vendored; see THIRD_PARTY.md).
# Optional, gated HAVE_NETGEN on the HAVE_PLANEGCS template: the app builds and
# runs without it (fem::Mesh returns a "built without FEM support" error).
# The wrapper is its own static library so Netgen's heavy build requirements
# (the PARALLEL/MPI defines and -fabi-version=17 the nixpkgs package was built
# with, which must match or the process heap-corrupts — proven in the FEM0
# spike) stay contained to FemMesher.cpp and never leak into the rest of core.
find_package(Netgen CONFIG QUIET)
add_library(fem_mesher STATIC src/core/fem/FemMesher.cpp)
target_include_directories(fem_mesher PRIVATE src)
target_link_libraries(fem_mesher PRIVATE glm::glm)
target_compile_definitions(fem_mesher PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(OPENCASCADE_FOUND)
    target_link_libraries(fem_mesher PRIVATE ${OPENCASCADE_LIBS})
endif()
if(Netgen_FOUND AND OPENCASCADE_FOUND)
    set(HAVE_NETGEN TRUE)
    # Linking nglib/ngcore PRIVATE applies their include dirs, PARALLEL/ABI
    # definitions and -fabi-version=17 to this TU only; $<LINK_ONLY> keeps them
    # out of designer_core's usage requirements.
    target_link_libraries(fem_mesher PRIVATE nglib ngcore)
    target_compile_definitions(fem_mesher PRIVATE HAVE_NETGEN)
    message(STATUS "Netgen enabled (FEM meshing)")
else()
    message(WARNING "Netgen not found: FEM meshing disabled (app runs; run_study errors)")
endif()

# What this build IS, for Help > About and for bug reports (src/common/BuildInfo).
# The commit is read at CONFIGURE time — it names the commit the build tree was
# configured on, which is why About says so rather than implying it is live.
execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD
                OUTPUT_VARIABLE DESIGNER_GIT_COMMIT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)

# Optional features that bring a dependency of their own. Both default ON
# where the dependency is present, and both can be switched off outright —
# a build flag is cheaper than trusting that a dependency stays small.
option(ENABLE_CAM "Milling toolpaths + G-code (vendored Clipper2)" ON)
option(ENABLE_TERMINAL "Embedded terminal panel (libvterm)" ON)
# Adaptive clearing is the one LGPL dependency: separable on purpose, both
# so it can be dropped and so the licence boundary stays a dynamic link.
option(ENABLE_CAM_ADAPTIVE "Adaptive clearing (LGPL libarea, linked as a shared library)" ON)

# Clipper2 (vendored, Boost licence): polygon offsetting for CAM toolpaths
# (docs/cam/plan.md). Vendored rather than found: it is packaged in nixpkgs
# but not in Ubuntu, and three .cpp files are cheaper than an optional
# dependency that silently drops a workstream on half the CI matrix.
set(CLIPPER2_DIR ${CMAKE_SOURCE_DIR}/deps/clipper2/CPP/Clipper2Lib)
if(NOT ENABLE_CAM)
    message(STATUS "CAM disabled (-DENABLE_CAM=OFF)")
elseif(EXISTS ${CLIPPER2_DIR}/src/clipper.engine.cpp)
    set(HAVE_CAM ON)
    add_library(clipper2 STATIC
        ${CLIPPER2_DIR}/src/clipper.engine.cpp
        ${CLIPPER2_DIR}/src/clipper.offset.cpp
        ${CLIPPER2_DIR}/src/clipper.rectclip.cpp)
    target_include_directories(clipper2 SYSTEM PUBLIC ${CLIPPER2_DIR}/include)
    # USINGZ gives Point64 a `z` field and Clipper64 a SetZCallback, which
    # libarea's Adaptive relies on to track which input edge produced each
    # output vertex. It changes the LAYOUT of Point64, so it has to be PUBLIC:
    # one translation unit compiled without it and the two disagree about a
    # struct they both pass around.
    target_compile_definitions(clipper2 PUBLIC USINGZ)
    set_target_properties(clipper2 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    list(APPEND core_sources src/core/cam/CamGeometry.cpp
                             src/core/cam/CamDerive.cpp
                             src/core/cam/CamDropCutter.cpp
                             src/core/cam/CamSurface.cpp
                             src/core/cam/CamSurfacePick.cpp
                             src/core/cam/CamStrategies.cpp
                             src/core/cam/CamSurfaceStrategies.cpp
                             src/core/cam/CamGcode.cpp
                             src/core/cam/CamRotary.cpp
                             src/core/cam/CamVerify.cpp
                             src/core/cam/CamFeeds.cpp
                             src/core/cam/CamSim.cpp
                             src/core/cam/CamStockMesh.cpp)
    message(STATUS "Clipper2 found - CAM enabled")

    # Adaptive clearing (C8): Kresimir Tusek's Adaptive2d from FreeCAD's
    # libarea, vendored at deps/libarea/ from FreeCAD b2da06bf.
    #
    # It is **LGPL-2.1-or-later**, and this is the one dependency here that is
    # copyleft. It is therefore built as its OWN SHARED LIBRARY and linked
    # dynamically, never absorbed into designer_core: the LGPL's relinking
    # condition is satisfied by a boundary a user can replace, which keeps
    # this application's own licence a decision we can still make later. Do
    # not "simplify" this to a static library or fold the sources into
    # core_sources — the arrangement is the point, not the build tidiness.
    # (libarea's own Clipper1, which Adaptive uses internally, is BSL-1.0 and
    # rides along inside the same .so.)
    set(LIBAREA_DIR ${CMAKE_SOURCE_DIR}/deps/libarea)
    if(ENABLE_CAM_ADAPTIVE AND EXISTS ${LIBAREA_DIR}/Adaptive.cpp)
        set(HAVE_CAM_ADAPTIVE ON)
        add_library(designer_adaptive SHARED
            ${LIBAREA_DIR}/Adaptive.cpp
            ${LIBAREA_DIR}/clipper.cpp)
        target_include_directories(designer_adaptive SYSTEM PUBLIC
            ${LIBAREA_DIR})
        target_link_libraries(designer_adaptive PRIVATE clipper2)
        set_target_properties(designer_adaptive PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            SOVERSION 1
            VERSION 1.0.0)
        message(STATUS "libarea found - adaptive clearing enabled (LGPL, shared)")
    elseif(NOT ENABLE_CAM_ADAPTIVE)
        message(STATUS "Adaptive clearing disabled (-DENABLE_CAM_ADAPTIVE=OFF)")
    else()
        message(STATUS "deps/libarea missing - adaptive clearing disabled")
    endif()
    # Compiled with OR without libarea: its #else half answers "not in this
    # build" for the clearing, and the request/key/cache half
    # (docs/ui/long-operations.md) is plain C++ that every CAM build needs.
    list(APPEND core_sources src/core/cam/CamAdaptive.cpp)
else()
    message(STATUS "deps/clipper2 missing (submodule not initialised) - CAM disabled")
endif()

# libvterm: the terminal panel's VT emulation (docs/ui/terminal-panel.md).
# Optional — without it the panel compiles out and the preference hides,
# because a CAD app must still build on a machine that has no terminal
# library. Ubuntu: libvterm-dev; nix: libvterm-neovim (in shell.nix).
find_package(PkgConfig QUIET)
if(ENABLE_TERMINAL AND PkgConfig_FOUND AND NOT WIN32)
    pkg_check_modules(VTERM QUIET vterm)
endif()
if(NOT ENABLE_TERMINAL)
    message(STATUS "Terminal panel disabled (-DENABLE_TERMINAL=OFF)")
elseif(VTERM_FOUND)
    set(HAVE_TERMINAL ON)
    list(APPEND core_sources src/common/TerminalScreen.cpp
                             src/common/PtySession.cpp)
    message(STATUS "libvterm found - terminal panel enabled")
else()
    message(STATUS "libvterm not found - terminal panel disabled")
endif()

# -Werror=switch, for src/core/cam/ ONLY.
#
# The CAM dispatches are deliberately `default:`-less switches on OpType, so
# -Wswitch names a type nobody wrote a case for. -Werror is off tree-wide
# (see the comment above -Wall), which made that guarantee notional: the
# warning went into a build log while `DeriveOperation` fell through and
# returned a DEFAULT Operation -- top 0, bottom -1, no target -- reporting
# ok. That shipped once for Adaptive and tests/test_cam.cpp is named after it.
#
# Scoped to this directory rather than raised tree-wide: it is where an enum
# with a row-per-value table is dispatched on, and it is about to grow two
# more values.
set(cam_sources ${core_sources})
list(FILTER cam_sources INCLUDE REGEX "src/core/cam/")
set_source_files_properties(${cam_sources} PROPERTIES COMPILE_OPTIONS
                            "-Werror=switch")

add_library(designer_core STATIC ${core_sources})
target_compile_definitions(designer_core PUBLIC
    DESIGNER_VERSION="${PROJECT_VERSION}"
    DESIGNER_GIT_COMMIT="${DESIGNER_GIT_COMMIT}")
target_include_directories(designer_core PUBLIC src)
target_link_libraries(designer_core PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json fem_mesher)
if(HAVE_CAM)
    target_compile_definitions(designer_core PUBLIC HAVE_CAM)
    target_link_libraries(designer_core PUBLIC clipper2)
endif()
if(HAVE_CAM_ADAPTIVE)
    target_compile_definitions(designer_core PUBLIC HAVE_CAM_ADAPTIVE)
    # PRIVATE would still put the .so on the link line of everything that
    # links designer_core; the point of the boundary is that only the bridge
    # translation unit touches the LGPL symbols.
    target_link_libraries(designer_core PUBLIC designer_adaptive)
endif()
if(HAVE_TERMINAL)
    target_compile_definitions(designer_core PUBLIC HAVE_TERMINAL)
    target_include_directories(designer_core PUBLIC ${VTERM_INCLUDE_DIRS})
    target_link_libraries(designer_core PUBLIC ${VTERM_LINK_LIBRARIES})
    # forkpty lives in libutil on older glibc, in libc since 2.34.
    find_library(UTIL_LIBRARY util)
    if(UTIL_LIBRARY)
        target_link_libraries(designer_core PUBLIC ${UTIL_LIBRARY})
    endif()
endif()
if(HAVE_NETGEN)
    # Lets core sources (and tests, transitively) switch on FEM availability
    # without pulling in any Netgen header.
    target_compile_definitions(designer_core PUBLIC HAVE_NETGEN)
endif()
# The FEM solver (FemSolver.cpp) needs Eigen directly — independently of
# PlaneGCS, which links Eigen only through its own target. Eigen is header-only,
# so this is a compile requirement; gated HAVE_FEM_SOLVER so a build without
# Eigen still links (Solve returns an error).
if(Eigen3_FOUND)
    target_link_libraries(designer_core PUBLIC Eigen3::Eigen)
    target_compile_definitions(designer_core PUBLIC HAVE_FEM_SOLVER)
    set(HAVE_FEM_SOLVER ON)
    # A6's mate-loop solve (AssemblyLoopSolve.cpp) also needs Eigen, but it is
    # not the FEM solver and must not be gated on it — its own name, so that
    # dropping one never silently disables the other. Absent it, mate loops
    # are reported exactly as A3 reported them.
    target_compile_definitions(designer_core PUBLIC HAVE_EIGEN)
    set(HAVE_EIGEN ON)
endif()
# Spectra: header-only sparse eigensolver for FEM modal analysis. Optional,
# gated HAVE_SPECTRA on the HAVE_FEM_SOLVER template: absent it, static FEM still
# works and modal solves return "built without modal support". Pulls Eigen.
find_package(Spectra QUIET)
if(Spectra_FOUND AND Eigen3_FOUND)
    target_link_libraries(designer_core PUBLIC Spectra::Spectra)
    target_compile_definitions(designer_core PUBLIC HAVE_SPECTRA)
    set(HAVE_SPECTRA ON)
    message(STATUS "Spectra enabled (FEM modal analysis)")
else()
    message(WARNING "Spectra not found: FEM modal analysis disabled")
endif()
if(HAVE_PLANEGCS)
    target_link_libraries(designer_core PUBLIC planegcs)
    target_compile_definitions(designer_core PUBLIC HAVE_PLANEGCS)
endif()
# Vulkan clip-space conventions everywhere glm is used (propagates to all
# dependents): depth 0..1; the Y-flip lives in Camera::UpdateProjectionMatrix
target_compile_definitions(designer_core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(OPENCASCADE_FOUND)
    target_link_libraries(designer_core PUBLIC ${OPENCASCADE_LIBS})
endif()

# Precompiled header for the heavy, stable includes. CADKernel.h drags in the
# whole OpenCASCADE header tree, which otherwise re-parses in every core TU and
# dominates compile time. PRIVATE so it applies to designer_core's own sources
# only (not dependents). Kept off the app target, whose ImGui sources shouldn't
# be forced to include OCC.
target_precompile_headers(designer_core PRIVATE
    "${CMAKE_SOURCE_DIR}/src/cad/CADKernel.h"
    <glm/glm.hpp>
    <nlohmann/json.hpp>
    <vector> <string> <memory> <optional> <variant> <map> <array>
)

# Application: windowing, rendering, UI.
set(app_sources
    src/app/main.cpp
    src/app/FramePacer.cpp
    src/render/VulkanCommon.cpp
    src/render/GpuTimer.cpp
    src/app/CrashHandler.cpp
    src/app/HeadlessMain.cpp
    src/render/IBLTextures.cpp
    src/render/MeshRenderer.cpp
    src/render/OutlineRenderer.cpp
    src/render/ShadowRenderer.cpp
    src/render/SSAORenderer.cpp
    src/render/RayTracingRenderer.cpp
    src/render/SketchRenderer.cpp
    src/render/UnderlayRenderer.cpp
    src/render/FieldRenderer.cpp
    src/render/StockRenderer.cpp
    src/app/GuiTests.cpp
    src/app/guitests/GuiTestsCommon.cpp
    src/app/guitests/GuiTestsShell.cpp
    src/app/guitests/GuiTestsModeling.cpp
    src/app/guitests/GuiTestsSketch.cpp
    src/app/guitests/GuiTestsViewport.cpp
    src/app/guitests/GuiTestsAssembly.cpp
    src/app/guitests/GuiTestsDrawing.cpp
    src/app/guitests/GuiTestsCam.cpp
    src/app/guitests/GuiTestsSimulation.cpp
    src/app/guitests/GuiTestsProject.cpp
    src/app/guitests/GuiTestsTerminal.cpp
    src/ui/UI.cpp
    src/ui/DockLayout.cpp
    src/ui/MainMenuBar.cpp
    src/ui/FeatureTreePanel.cpp
    src/ui/OccurrencePanel.cpp
    src/ui/ListSelection.cpp
    src/ui/StudyPanel.cpp
    src/ui/DrawingPanel.cpp
    src/ui/ConfigurationsPanel.cpp
    src/ui/VariablesPanel.cpp
    src/ui/HistoryPanel.cpp
    src/ui/ConflictsPanel.cpp
    src/ui/BackgroundJobs.cpp
    src/ui/ImportJob.cpp
    src/ui/OpenJob.cpp
    src/ui/FloatingWindow.cpp
    src/ui/SyncJob.cpp
    src/ui/SettingsWindow.cpp
    src/ui/CamJobs.cpp
    src/ui/CamPanel.cpp
    src/ui/CamPlayback.cpp
    src/ui/CamTool.cpp
    src/ui/CamGenerate.cpp
    src/ui/PerfHud.cpp
    src/ui/TerminalPanel.cpp
    src/ui/SketchToolbar.cpp
    src/ui/FastenerTool.cpp
    src/ui/DerivedActions.cpp
    src/ui/FileDialogs.cpp
    src/ui/ProjectActions.cpp
    src/ui/RendererSync.cpp
    src/ui/MeasureActions.cpp
    src/ui/CreateActions.cpp
    src/ui/DocumentSlots.cpp
    src/ui/ViewActions.cpp
    src/ui/DrawingActions.cpp
    src/ui/SceneOverlays.cpp
    src/ui/VariableActions.cpp
    src/ui/CatalogActions.cpp
    src/ui/CatalogPanel.cpp
    src/ui/TolerancePanel.cpp
    src/ui/SheetMetalPanel.cpp
    src/ui/ToleranceActions.cpp
    src/ui/SketchOverlay.cpp
    src/ui/ImportActions.cpp
    src/ui/AssemblyActions.cpp
    src/ui/Bom.cpp
    src/ui/MateActions.cpp
    src/ui/GizmoInput.cpp
    src/ui/TessellationCache.cpp
    src/ui/FastenerActions.cpp
    src/ui/MotionActions.cpp
    src/ui/JobActions.cpp
    src/ui/MachineActions.cpp
    src/ui/SimulationActions.cpp
    src/ui/FeatureEditor.cpp
    src/ui/ExprInput.cpp
    src/ui/ExprComplete.cpp
    src/ui/PreviewFeature.cpp
    src/ui/PreviewHandles.cpp
    src/ui/ViewportHandleDraw.cpp
    src/ui/Overlays.cpp
    src/ui/TextFit.cpp
    src/ui/Theme.cpp
    src/ui/ViewportInput.cpp
    src/ui/MeasureOverlay.cpp
    src/ui/SelectionPipeline.cpp
    src/ui/CommandPalette.cpp
    src/ui/Commands.cpp
    src/ui/SketchInteraction.cpp
    src/ui/SketchTools.cpp
    src/ui/SketchDimensions.cpp
    src/ui/SketchToolOverlay.cpp
    src/ui/SketchDimensionDraw.cpp
    src/ui/SketchPick.cpp
    src/ui/SketchOps.cpp
    src/ui/Toolbar.cpp
    src/ui/Widgets.cpp
    src/render/RenderSheet.cpp
    src/render/Screenshot.cpp
    src/remote/RemoteServer.cpp
    src/remote/RemoteSession.cpp
    src/remote/RemoteDispatch.cpp
    src/remote/RemoteUiActions.cpp
    src/remote/RemoteConfigurations.cpp
    src/remote/RemoteSheetMetal.cpp
    src/remote/RemoteTube.cpp
    src/remote/RemoteSimulation.cpp
    src/remote/RemoteCam.cpp
    src/remote/RemoteInspection.cpp
    src/remote/RemoteCatalog.cpp
    src/remote/RemoteModeling.cpp
    src/remote/RemoteDrawings.cpp
    src/remote/RemoteAssembly.cpp
    src/remote/RemoteMotion.cpp
    src/remote/RemoteProject.cpp
)

# UI fonts, loaded at runtime relative to the executable
file(COPY ${CMAKE_SOURCE_DIR}/deps/imgui/misc/fonts/Roboto-Medium.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/assets/fonts)
file(COPY ${CMAKE_SOURCE_DIR}/deps/fonts/fa-solid-900.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/assets/fonts)
# Monospace for the terminal panel: the UI font has no box-drawing glyphs, and
# a TUI drawn in a proportional font is unreadable (docs/ui/terminal-panel.md).
file(COPY ${CMAKE_SOURCE_DIR}/deps/fonts/JetBrainsMono-Regular.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/assets/fonts)
# The MCP server and its client, beside the assets: the terminal panel points
# a Claude Code session at THIS instance by generating a config that names
# this file, so it has to exist in a dev tree and in an install alike.
file(COPY ${CMAKE_SOURCE_DIR}/tools/designer_mcp.py
          ${CMAKE_SOURCE_DIR}/tools/designer_remote.py
     DESTINATION ${CMAKE_BINARY_DIR}/assets/tools)
include_directories(${CMAKE_SOURCE_DIR}/deps/fonts)

# Studio HDRI for image-based lighting, loaded at runtime relative to the
# executable (main.cpp ExecutableDir()) — same relocatable scheme as the fonts.
# One name, staged here and installed below; it must match
# RuntimePaths::kEnvironmentHdriFile, which is the C++ side of the same name.
set(DESIGNER_HDRI "brown_photostudio_02_2k.hdr")
file(COPY ${CMAKE_SOURCE_DIR}/assets/hdri/${DESIGNER_HDRI}
     DESTINATION ${CMAKE_BINARY_DIR}/assets/hdri)
# The vendored standard-parts tables (docs/parts/plan.md). Staged beside the
# executable for a development build; installed under share/designer below.
file(COPY ${CMAKE_SOURCE_DIR}/assets/parts
     DESTINATION ${CMAKE_BINARY_DIR}/assets)

if(NOT ENABLE_TEST_ENGINE)
  # No engine: drop its sources and the suite written against it.
  set(IMGUI_TEST_ENGINE_SOURCES "")
  list(REMOVE_ITEM app_sources
       src/app/GuiTests.cpp
       src/app/guitests/GuiTestsCommon.cpp
       src/app/guitests/GuiTestsShell.cpp
       src/app/guitests/GuiTestsModeling.cpp
       src/app/guitests/GuiTestsSketch.cpp
       src/app/guitests/GuiTestsViewport.cpp
       src/app/guitests/GuiTestsAssembly.cpp
       src/app/guitests/GuiTestsDrawing.cpp
       src/app/guitests/GuiTestsCam.cpp
       src/app/guitests/GuiTestsSimulation.cpp
       src/app/guitests/GuiTestsProject.cpp
       src/app/guitests/GuiTestsTerminal.cpp)
endif()

# designer_app: everything the app is EXCEPT its entry points (ui/, render/,
# remote/ and the GUI suite). It exists so designer_tests can link the actions
# layer and exercise it headlessly — see tests/test_actions.cpp for the
# pattern and docs/architecture-review-2026-08-24.md for why this beats an
# abstract renderer interface.
#
# No renderer seam is needed, and none was added: the actions layer already
# null-checks every renderer pointer (HeadlessMain.cpp runs the whole app that
# way), so a test constructs an AppState, leaves the pointers null, and the
# sync functions no-op exactly as they do headless.
set(app_lib_sources ${app_sources})
list(REMOVE_ITEM app_lib_sources
     src/app/main.cpp
     src/app/HeadlessMain.cpp
     src/app/CrashHandler.cpp)
add_library(designer_app STATIC
    ${app_lib_sources}
    ${NFD_SOURCES}
    ${IMGUI_TEST_ENGINE_SOURCES}
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp)
target_include_directories(designer_app PUBLIC src)
target_link_libraries(designer_app PUBLIC designer_core ${LIBRARIES})

# The vendored sources are compiled here too, and they must NOT get the PCH
# below: ImGui's own .cpp files `#define IMGUI_DEFINE_MATH_OPERATORS` before
# including imgui.h, and ui/AppState.h (the PCH's whole point) pulls imgui.h in
# ahead of them. imgui_draw.cpp then fails on its own header. They are also the
# files with the least to gain -- none of them includes anything of ours.
set_source_files_properties(
    ${NFD_SOURCES}
    ${IMGUI_TEST_ENGINE_SOURCES}
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Precompiled header. designer_core has had one since the 2026-08-24 round for
# the same reason; the app target never got one, and it is the larger half.
#
# What it is paying for: ui/AppState.h preprocesses to 224k lines and 65 files
# include it, because core/Document.h behind it drags in the whole OpenCASCADE
# tree (146k lines on its own -- 53% of a typical UI translation unit).
#
# It is also the counterpart to splitting the big files. A split makes N
# translation units re-parse what one used to parse once, so the split alone
# raises total CPU while it lowers the critical path; the PCH is what pays that
# back. Do not remove one without the other
# (docs/architecture-review-2026-09-06.md, section 2).
#
# PRIVATE, like designer_core's: it applies to this target's own sources, not
# to everything that links it.

target_precompile_headers(designer_app PRIVATE
    "${CMAKE_SOURCE_DIR}/src/ui/AppState.h"
    <glm/glm.hpp>
    <nlohmann/json.hpp>
    <vector> <string> <memory> <optional> <map> <set> <algorithm> <cmath>
)
if(ENABLE_TEST_ENGINE)
  # PUBLIC, not PRIVATE: GuiTests*.cpp and the app both switch on it, and a
  # consumer linking this library must agree about ImGui's ABI.
  target_compile_definitions(designer_app PUBLIC
      HAVE_TEST_ENGINE
      IMGUI_ENABLE_TEST_ENGINE
      IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
      # Where the checked-in GUI-test fixtures live (tests/fixtures/...).
      # Only ever read by test code, which is why it rides the test-engine
      # branch rather than being defined for a release build.
      DESIGNER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

add_executable(designer ${SANITIZER_DEFAULTS_SRC}
    src/app/main.cpp
    src/app/HeadlessMain.cpp
    src/app/CrashHandler.cpp)
if(HAVE_STDCXX_STACKTRACE)
    target_compile_definitions(designer PRIVATE HAVE_STDCXX_STACKTRACE)
endif()
if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(DESIGNER_INSTALL_RPATH "${CMAKE_INSTALL_LIBDIR}")
else()
    set(DESIGNER_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
set_target_properties(designer PROPERTIES INSTALL_RPATH "${DESIGNER_INSTALL_RPATH}")

# imgui.cpp must see IMGUI_ENABLE_TEST_ENGINE so it emits the test hooks.
# HAVE_TEST_ENGINE is ours: the app's own code switches on it (matching the
# HAVE_NETGEN / HAVE_FEM_GPU convention).
if(ENABLE_TEST_ENGINE)
  # The library already sets these PUBLIC; main.cpp needs them too.
else()
  message(STATUS "Dear ImGui Test Engine DISABLED: no --gui-tests, no remote "
                 "ui_action; everything else builds")
endif()

target_link_libraries(designer designer_app designer_core ${LIBRARIES})

# X11 for the non-blocking XMapWindow window-show path (see main.cpp); avoids
# GLFW's waitForVisibilityNotify deadlock under i3. Linux/X11 only.
if(UNIX AND NOT APPLE)
    find_package(X11)
    if(X11_FOUND)
        target_link_libraries(designer ${X11_LIBRARIES})
    endif()
endif()

# Winsock for the RemoteServer JSON-RPC socket (Windows).
if(WIN32)
    target_link_libraries(designer ws2_32)
endif()

# fem_gpu: headless Vulkan compute for the topology solver
# (docs/topopt/gpu-plan.md). Deliberately a separate library — designer_core
# must stay GPU-free. Compute shaders are compiled here (glslc is found early;
# the same cache variable satisfies the graphics-shader section below) and
# embedded as byte arrays so the library and its tests need no runtime shader
# path (renderer .spv files load relative to the app executable, which test
# binaries don't share).
# GPU-compute FEM is opt-out so slimmer builds (e.g. the Windows cross MVP) can
# drop the topology-solver compute stack. Default ON preserves current behavior.
option(DESIGNER_ENABLE_FEM_GPU "Build the Vulkan-compute FEM topology solver" ON)
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin)
if(GLSLC AND DESIGNER_ENABLE_FEM_GPU)
    set(FEMGPU_GEN_DIR "${CMAKE_BINARY_DIR}/femgpu_gen")
    file(MAKE_DIRECTORY ${FEMGPU_GEN_DIR})
    function(embed_compute_shader NAME)
        set(spv ${FEMGPU_GEN_DIR}/${NAME}.spv)
        set(hdr ${FEMGPU_GEN_DIR}/${NAME}_spv.h)
        add_custom_command(
            OUTPUT ${spv}
            COMMAND ${GLSLC} --target-env=vulkan1.2
                    ${CMAKE_SOURCE_DIR}/src/shaders/${NAME}.comp -o ${spv}
            DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/${NAME}.comp
            COMMENT "Compiling compute shader ${NAME}.comp")
        add_custom_command(
            OUTPUT ${hdr}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${hdr} -DNAME=${NAME}
                    -P ${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake
            DEPENDS ${spv} ${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake
            COMMENT "Embedding ${NAME}.spv")
    endfunction()
    embed_compute_shader(fem_spmv)
    embed_compute_shader(gpu_sanity)
    embed_compute_shader(cg_diag)
    embed_compute_shader(cg_dot)
    embed_compute_shader(cg_precond_dot)
    embed_compute_shader(cg_residual)
    embed_compute_shader(cg_copy)
    embed_compute_shader(cg_update_xr)
    embed_compute_shader(cg_update_p)
    embed_compute_shader(cheb_init)
    embed_compute_shader(cheb_iter)
    embed_compute_shader(vec_divd)
    embed_compute_shader(vec_scale)
    embed_compute_shader(mg_restrict)
    embed_compute_shader(mg_prolong)
    embed_compute_shader(vec_add)
    embed_compute_shader(fem_energy)
    add_library(fem_gpu STATIC
        src/femgpu/ComputeContext.cpp
        src/femgpu/TopoSpmv.cpp
        src/femgpu/TopoCgSolver.cpp
        ${FEMGPU_GEN_DIR}/fem_spmv_spv.h
        ${FEMGPU_GEN_DIR}/gpu_sanity_spv.h
        ${FEMGPU_GEN_DIR}/cg_diag_spv.h
        ${FEMGPU_GEN_DIR}/cg_dot_spv.h
        ${FEMGPU_GEN_DIR}/cg_precond_dot_spv.h
        ${FEMGPU_GEN_DIR}/cg_residual_spv.h
        ${FEMGPU_GEN_DIR}/cg_copy_spv.h
        ${FEMGPU_GEN_DIR}/cg_update_xr_spv.h
        ${FEMGPU_GEN_DIR}/cg_update_p_spv.h
        ${FEMGPU_GEN_DIR}/cheb_init_spv.h
        ${FEMGPU_GEN_DIR}/cheb_iter_spv.h
        ${FEMGPU_GEN_DIR}/vec_divd_spv.h
        ${FEMGPU_GEN_DIR}/vec_scale_spv.h
        ${FEMGPU_GEN_DIR}/mg_restrict_spv.h
        ${FEMGPU_GEN_DIR}/mg_prolong_spv.h
        ${FEMGPU_GEN_DIR}/vec_add_spv.h
        ${FEMGPU_GEN_DIR}/fem_energy_spv.h)
    # Generated headers are PUBLIC so femgpu_tests can use the sanity kernel.
    target_include_directories(fem_gpu PUBLIC src ${FEMGPU_GEN_DIR})
    target_link_libraries(fem_gpu PUBLIC Vulkan::Vulkan)
    if(Eigen3_FOUND)
        # The TopoLinearSolver adapter packs element matrices with the shared
        # fem element math (header-inline), so it needs Eigen but not the
        # designer_core library itself.
        target_sources(fem_gpu PRIVATE src/femgpu/TopoGpuSolver.cpp
                                       src/femgpu/TopoMultigrid.cpp)
        target_link_libraries(fem_gpu PUBLIC Eigen3::Eigen)
        target_compile_definitions(fem_gpu PRIVATE HAVE_FEM_SOLVER)
        # The app auto-selects the GPU solver for topology runs when a usable
        # hardware device exists (DESIGNER_TOPO_GPU=0 disables, =1 admits a
        # software device).
        target_link_libraries(designer fem_gpu)
        target_compile_definitions(designer PRIVATE HAVE_FEM_GPU)
        set(HAVE_FEM_GPU ON)
    endif()
endif()

# Tests. Opt-out so packaging builds (e.g. the Flatpak) don't configure the test
# tree — which would otherwise FetchContent Catch2 over the network, unavailable
# in an offline build sandbox. Default ON preserves the normal dev/CI behavior.
option(DESIGNER_BUILD_TESTS "Configure the test targets" ON)
if(DESIGNER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmark oracle. Grades an exported STEP by reading it back with
# OpenCASCADE and NOTHING else — deliberately no designer_core, so it can
# contradict the app instead of agreeing with it (benchmarks/README.md).
if(OPENCASCADE_FOUND)
    add_executable(bench_oracle benchmarks/oracle/oracle.cpp)
    target_link_libraries(bench_oracle PRIVATE nlohmann_json::nlohmann_json
                                               ${OPENCASCADE_LIBS})
endif()

# Find glslc (Vulkan shader compiler)
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found! Make sure Vulkan SDK is installed and glslc is in PATH")
endif()
message(STATUS "Found glslc: ${GLSLC}")

# Create shader output directory
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/libraries/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Function to compile a shader. Extra args (ARGN) are additional dependency
# files — e.g. a shared #include'd .glsl — so editing them retriggers the build.
function(compile_shader TARGET SHADER_SOURCE SHADER_OUTPUT)
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC} ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER_SOURCE} ${ARGN}
        COMMENT "Compiling shader ${SHADER_SOURCE}"
    )
    add_custom_target(${TARGET} DEPENDS ${SHADER_OUTPUT})
endfunction()

# Function to compile a ray tracing shader (ARGN = extra include dependencies).
function(compile_rt_shader TARGET SHADER_SOURCE SHADER_OUTPUT)
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC} --target-env=vulkan1.2 ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER_SOURCE} ${ARGN}
        COMMENT "Compiling RT shader ${SHADER_SOURCE}"
    )
    add_custom_target(${TARGET} DEPENDS ${SHADER_OUTPUT})
endfunction()

# Shared shader include (procedural grain), a dependency of every shader that
# #includes it.
set(GRAIN_INCLUDE "${CMAKE_SOURCE_DIR}/src/shaders/grain.glsl")

# Compile vertex shader
set(VERTEX_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/mesh.vert")
set(VERTEX_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/mesh_vert.spv")
compile_shader(mesh_vert_shader ${VERTEX_SHADER_SOURCE} ${VERTEX_SHADER_OUTPUT})

# Compile fragment shader
set(FRAGMENT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/mesh.frag")
set(FRAGMENT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/mesh_frag.spv")
compile_shader(mesh_frag_shader ${FRAGMENT_SHADER_SOURCE} ${FRAGMENT_SHADER_OUTPUT} ${GRAIN_INCLUDE})

# Compile ray tracing shaders
set(RAYGEN_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/raygen.rgen")
set(RAYGEN_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/raygen.spv")
compile_rt_shader(raygen_shader ${RAYGEN_SHADER_SOURCE} ${RAYGEN_SHADER_OUTPUT})

set(MISS_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/miss.rmiss")
set(MISS_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/miss.spv")
compile_rt_shader(miss_shader ${MISS_SHADER_SOURCE} ${MISS_SHADER_OUTPUT})

set(CLOSESTHIT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/closesthit.rchit")
set(CLOSESTHIT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/closesthit.spv")
compile_rt_shader(closesthit_shader ${CLOSESTHIT_SHADER_SOURCE} ${CLOSESTHIT_SHADER_OUTPUT} ${GRAIN_INCLUDE})

set(SHADOW_MISS_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/shadow.rmiss")
set(SHADOW_MISS_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/shadow.spv")
compile_rt_shader(shadow_miss_shader ${SHADOW_MISS_SHADER_SOURCE} ${SHADOW_MISS_SHADER_OUTPUT})

# Section any-hit: the clip planes, applied where a ray tracer can act on them.
set(SECTION_AHIT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/section.rahit")
set(SECTION_AHIT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/section.spv")
compile_rt_shader(section_ahit_shader ${SECTION_AHIT_SHADER_SOURCE} ${SECTION_AHIT_SHADER_OUTPUT})

# Compile outline shaders
set(OUTLINE_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/outline.vert")
set(OUTLINE_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/outline_vert.spv")
compile_shader(outline_vert_shader ${OUTLINE_VERT_SHADER_SOURCE} ${OUTLINE_VERT_SHADER_OUTPUT})

set(OUTLINE_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/outline.frag")
set(OUTLINE_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/outline_frag.spv")
compile_shader(outline_frag_shader ${OUTLINE_FRAG_SHADER_SOURCE} ${OUTLINE_FRAG_SHADER_OUTPUT})

set(WIREFRAME_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/wireframe.vert")
set(WIREFRAME_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/wireframe_vert.spv")
compile_shader(wireframe_vert_shader ${WIREFRAME_VERT_SHADER_SOURCE} ${WIREFRAME_VERT_SHADER_OUTPUT})

set(WIREFRAME_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/wireframe.frag")
set(WIREFRAME_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/wireframe_frag.spv")
compile_shader(wireframe_frag_shader ${WIREFRAME_FRAG_SHADER_SOURCE} ${WIREFRAME_FRAG_SHADER_OUTPUT})

# Sketch line/quad shaders
set(SKETCH_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/sketch.vert")
set(SKETCH_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/sketch_vert.spv")
compile_shader(sketch_vert_shader ${SKETCH_VERT_SHADER_SOURCE} ${SKETCH_VERT_SHADER_OUTPUT})

set(SKETCH_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/sketch.frag")
set(SKETCH_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/sketch_frag.spv")
compile_shader(sketch_frag_shader ${SKETCH_FRAG_SHADER_SOURCE} ${SKETCH_FRAG_SHADER_OUTPUT})

# Reference-image underlay (textured quad on the sketch plane)
set(UNDERLAY_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/underlay.vert")
set(UNDERLAY_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/underlay_vert.spv")
compile_shader(underlay_vert_shader ${UNDERLAY_VERT_SHADER_SOURCE} ${UNDERLAY_VERT_SHADER_OUTPUT})

set(UNDERLAY_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/underlay.frag")
set(UNDERLAY_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/underlay_frag.spv")
compile_shader(underlay_frag_shader ${UNDERLAY_FRAG_SHADER_SOURCE} ${UNDERLAY_FRAG_SHADER_OUTPUT})

set(SKETCH_PRIM_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/sketch_prim.vert")
set(SKETCH_PRIM_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/sketch_prim_vert.spv")
compile_shader(sketch_prim_vert_shader ${SKETCH_PRIM_VERT_SHADER_SOURCE} ${SKETCH_PRIM_VERT_SHADER_OUTPUT})

set(SKETCH_PRIM_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/sketch_prim.frag")
set(SKETCH_PRIM_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/sketch_prim_frag.spv")
compile_shader(sketch_prim_frag_shader ${SKETCH_PRIM_FRAG_SHADER_SOURCE} ${SKETCH_PRIM_FRAG_SHADER_OUTPUT})

# Key-light shadow map shaders (depth-only pass; S1). Outputs are
# shadow_vert/shadow_frag — distinct from shadow.spv (the RT miss shader).
set(SHADOWMAP_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/shadow.vert")
set(SHADOWMAP_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/shadow_vert.spv")
compile_shader(shadowmap_vert_shader ${SHADOWMAP_VERT_SHADER_SOURCE} ${SHADOWMAP_VERT_SHADER_OUTPUT})

set(SHADOWMAP_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/shadow.frag")
set(SHADOWMAP_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/shadow_frag.spv")
compile_shader(shadowmap_frag_shader ${SHADOWMAP_FRAG_SHADER_SOURCE} ${SHADOWMAP_FRAG_SHADER_OUTPUT})

# SSAO shaders (S2): depth+normal prepass, hemisphere AO, bilateral blur,
# plus the shared fullscreen-triangle vertex stage.
set(SSAO_PREPASS_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/ssao_prepass.vert")
set(SSAO_PREPASS_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/ssao_prepass_vert.spv")
compile_shader(ssao_prepass_vert_shader ${SSAO_PREPASS_VERT_SHADER_SOURCE} ${SSAO_PREPASS_VERT_SHADER_OUTPUT})

set(SSAO_PREPASS_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/ssao_prepass.frag")
set(SSAO_PREPASS_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/ssao_prepass_frag.spv")
compile_shader(ssao_prepass_frag_shader ${SSAO_PREPASS_FRAG_SHADER_SOURCE} ${SSAO_PREPASS_FRAG_SHADER_OUTPUT})

set(FULLSCREEN_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/fullscreen.vert")
set(FULLSCREEN_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/fullscreen_vert.spv")
compile_shader(fullscreen_vert_shader ${FULLSCREEN_VERT_SHADER_SOURCE} ${FULLSCREEN_VERT_SHADER_OUTPUT})

set(SSAO_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/ssao.frag")
set(SSAO_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/ssao_frag.spv")
compile_shader(ssao_frag_shader ${SSAO_FRAG_SHADER_SOURCE} ${SSAO_FRAG_SHADER_OUTPUT})

set(SSAO_BLUR_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/ssao_blur.frag")
set(SSAO_BLUR_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/ssao_blur_frag.spv")
compile_shader(ssao_blur_frag_shader ${SSAO_BLUR_FRAG_SHADER_SOURCE} ${SSAO_BLUR_FRAG_SHADER_OUTPUT})

set(SSAO_UPSAMPLE_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/ssao_upsample.frag")
set(SSAO_UPSAMPLE_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/ssao_upsample_frag.spv")
compile_shader(ssao_upsample_frag_shader ${SSAO_UPSAMPLE_FRAG_SHADER_SOURCE} ${SSAO_UPSAMPLE_FRAG_SHADER_OUTPUT})

# FEM result field shaders (deform warp + Turbo colormap)
set(FIELD_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/field.vert")
set(FIELD_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/field_vert.spv")
compile_shader(field_vert_shader ${FIELD_VERT_SHADER_SOURCE} ${FIELD_VERT_SHADER_OUTPUT})

set(FIELD_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/field.frag")
set(FIELD_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/field_frag.spv")
compile_shader(field_frag_shader ${FIELD_FRAG_SHADER_SOURCE} ${FIELD_FRAG_SHADER_OUTPUT})

# CAM cutting-simulation stock shaders (flat-lit dynamic block)
set(STOCK_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/stock.vert")
set(STOCK_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/stock_vert.spv")
compile_shader(stock_vert_shader ${STOCK_VERT_SHADER_SOURCE} ${STOCK_VERT_SHADER_OUTPUT})

set(STOCK_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/stock.frag")
set(STOCK_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/stock_frag.spv")
compile_shader(stock_frag_shader ${STOCK_FRAG_SHADER_SOURCE} ${STOCK_FRAG_SHADER_OUTPUT})

# Create a target for all shaders
add_custom_target(shaders ALL DEPENDS mesh_vert_shader mesh_frag_shader raygen_shader miss_shader closesthit_shader shadow_miss_shader section_ahit_shader shadowmap_vert_shader shadowmap_frag_shader ssao_prepass_vert_shader ssao_prepass_frag_shader fullscreen_vert_shader ssao_frag_shader ssao_blur_frag_shader ssao_upsample_frag_shader outline_vert_shader outline_frag_shader wireframe_vert_shader wireframe_frag_shader sketch_vert_shader sketch_frag_shader sketch_prim_vert_shader sketch_prim_frag_shader underlay_vert_shader underlay_frag_shader field_vert_shader field_frag_shader stock_vert_shader stock_frag_shader)

# Make sure shaders are built before the main executable
add_dependencies(designer shaders)

# Canonical installation contract used by Nix, AppImage, and Flatpak.
install(TARGETS designer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET planegcs)
    # LIBRARY covers the ELF .so (Linux); RUNTIME covers the Windows DLL, which
    # must sit next to the executable.
    install(TARGETS planegcs
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET designer_adaptive)
    # Adaptive clearing is a HARD dependency of designer_core, not a plugin:
    # leave it out of the install and the installed binary does not start at
    # all (`libdesigner_adaptive.so.1: cannot open shared object file`).
    # It had no install rule from the day it was added -- invisible because
    # every test runs from the build tree, where it sits beside the binary.
    # Same LIBRARY/RUNTIME split as planegcs, and for the same reason: the
    # LGPL boundary is only replaceable if the library is actually shipped.
    install(TARGETS designer_adaptive
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY "${SHADER_OUTPUT_DIR}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/designer/shaders")
install(FILES
        "${CMAKE_SOURCE_DIR}/deps/imgui/misc/fonts/Roboto-Medium.ttf"
        "${CMAKE_SOURCE_DIR}/deps/fonts/fa-solid-900.ttf"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/designer/fonts")
install(FILES "${CMAKE_SOURCE_DIR}/assets/hdri/${DESIGNER_HDRI}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/designer/hdri")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/assets/parts"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/designer")
install(FILES "${CMAKE_SOURCE_DIR}/packaging/io.github.eliasaronson.Designer.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
# AppStream metainfo. Installing it triggers `appstreamcli compose` during a
# Flatpak build; that step needs working gdk-pixbuf image loaders to read the
# app icon, which a bare local toolchain (e.g. nix-shell) may lack, aborting the
# build. Flathub's builders have them, so keep this ON by default and only pass
# OFF for a local Flatpak smoke build. Off just omits catalog metadata — the
# desktop entry and icons below still install.
option(DESIGNER_INSTALL_METAINFO "Install AppStream metainfo" ON)
if(DESIGNER_INSTALL_METAINFO)
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/io.github.eliasaronson.Designer.metainfo.xml"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
endif()
install(FILES "${CMAKE_SOURCE_DIR}/assets/icons/io.github.eliasaronson.Designer.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
# A 256x256 raster alongside the scalable icon, as a fallback for desktops (and
# icon-cache tools) that don't render SVG.
install(FILES "${CMAKE_SOURCE_DIR}/assets/icons/io.github.eliasaronson.Designer.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps")
install(FILES
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "${CMAKE_SOURCE_DIR}/ADDITIONAL_PERMISSION.md"
        "${CMAKE_SOURCE_DIR}/THIRD_PARTY.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/designer")
install(DIRECTORY "${CMAKE_SOURCE_DIR}/LICENSES/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/designer/LICENSES")


# ---------------------------------------------------------------------------
# What this build actually contains. Printed at the end of configure because
# "which optional pieces am I compiling in, and what do they drag along?" is
# otherwise answered by reading this file — and a dependency you cannot see
# is a dependency you cannot decide about.
message(STATUS "")
message(STATUS "Designer ${PROJECT_VERSION} — optional features")
macro(_feature_line label state note)
    if(${state})
        message(STATUS "  [x] ${label}  ${note}")
    else()
        message(STATUS "  [ ] ${label}  ${note}")
    endif()
endmacro()
_feature_line("CAM (milling, G-code)      " HAVE_CAM "deps/clipper2 (vendored, Boost)  -DENABLE_CAM=OFF")
_feature_line("CAM adaptive clearing      " HAVE_CAM_ADAPTIVE "deps/libarea (vendored, LGPL .so) -DENABLE_CAM_ADAPTIVE=OFF")
_feature_line("Terminal panel             " HAVE_TERMINAL "libvterm (system)  -DENABLE_TERMINAL=OFF")
_feature_line("FEM solver                 " HAVE_FEM_SOLVER "Eigen (system)")
_feature_line("FEM meshing                " HAVE_NETGEN "Netgen (system)")
_feature_line("FEM modal analysis         " HAVE_SPECTRA "Spectra (system)")
_feature_line("FEM GPU acceleration       " HAVE_FEM_GPU "Vulkan compute")
_feature_line("Sketch constraint solver   " HAVE_PLANEGCS "deps/planegcs (vendored)")
_feature_line("Native file dialogs        " HAVE_NATIVEFILEDIALOG "deps/nativefiledialog-extended (vendored)")
_feature_line("GUI test engine            " ENABLE_TEST_ENGINE "deps/imgui_test_engine (vendored)  -DENABLE_TEST_ENGINE=OFF")
_feature_line("Crash backtraces           " HAVE_STDCXX_STACKTRACE "libstdc++ <stacktrace>")
message(STATUS "")
