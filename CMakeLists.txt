# Example usage:
#  mkdir build
#  cd build
#  cmake -g "Visual Studio 14 2015" ..

cmake_minimum_required(VERSION 3.14)
project(designer C CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
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
  message(STATUS "Sanitizers enabled (ASan + UBSan)")
endif()

find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)

# Dear ImGui (SYSTEM: third-party, don't warn on its headers)
set(IMGUI_DIR deps/imgui)
include_directories(SYSTEM ${IMGUI_DIR} ${IMGUI_DIR}/backends ..)

# Dear ImGui Test Engine (GUI testing)
set(IMGUI_TEST_ENGINE_DIR deps/imgui_test_engine/imgui_test_engine)
include_directories(SYSTEM ${IMGUI_TEST_ENGINE_DIR})
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
        list(APPEND OPENCASCADE_LIBS TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKSTL)
    endif()

    add_definitions(-DHAVE_OPENCASCADE)
endif()

# nativefiledialog - build from submodule
set(NFD_DIR deps/nativefiledialog)
if(EXISTS ${CMAKE_SOURCE_DIR}/${NFD_DIR}/src/include/nfd.h)
    message(STATUS "Building nativefiledialog from submodule")
    
    # Include nativefiledialog headers
    include_directories(${NFD_DIR}/src/include)
    add_definitions(-DHAVE_NATIVEFILEDIALOG)
    
    # Platform-specific source files
    if(WIN32)
        set(NFD_SOURCES ${NFD_DIR}/src/nfd_common.c ${NFD_DIR}/src/nfd_win.cpp)
        set(NFD_LIBS "")
    elseif(APPLE)
        set(NFD_SOURCES ${NFD_DIR}/src/nfd_common.c ${NFD_DIR}/src/nfd_cocoa.m)
        set(NFD_LIBS "-framework Cocoa")
    else()
        # Linux - check for GTK3 or fall back to Zenity
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(GTK3 gtk+-3.0)
        endif()
        
        if(GTK3_FOUND)
            set(NFD_SOURCES ${NFD_DIR}/src/nfd_common.c ${NFD_DIR}/src/nfd_gtk.c)
            set(NFD_LIBS ${GTK3_LIBRARIES})
            include_directories(${GTK3_INCLUDE_DIRS})
            link_directories(${GTK3_LIBRARY_DIRS})
            add_definitions(${GTK3_CFLAGS_OTHER})
            message(STATUS "Using GTK3 for nativefiledialog")
        else()
            set(NFD_SOURCES ${NFD_DIR}/src/nfd_common.c ${NFD_DIR}/src/nfd_zenity.c)
            set(NFD_LIBS "")
            message(STATUS "Using Zenity fallback for nativefiledialog")
        endif()
    endif()
    # Vendored third-party C: don't warn on its (harmless) format-truncation.
    if(NFD_SOURCES)
        set_source_files_properties(${NFD_SOURCES} PROPERTIES
            COMPILE_OPTIONS "-w")
    endif()
else()
    message(WARNING "nativefiledialog submodule not found - will use ImGui file browser fallback")
    set(NFD_SOURCES "")
    set(NFD_LIBS "")
endif()

# Libraries
find_package(Vulkan REQUIRED)
set(LIBRARIES "glfw;Vulkan::Vulkan;spdlog::spdlog;${NFD_LIBS}")

# Add OpenCASCADE libraries if available
if(OPENCASCADE_FOUND)
    list(APPEND LIBRARIES ${OPENCASCADE_LIBS})
endif()

# Core library: geometry, document and scene logic. Must stay free of
# Vulkan/GLFW/ImGui so it can be unit-tested and scripted headlessly.
set(core_sources
    src/Camera.cpp
    src/Logger.cpp
    src/Scene.cpp
    src/CADObject.cpp
    src/CADKernel.cpp
    src/CADMeshConverter.cpp
    src/CADMaterials.cpp
    src/MeshExport.cpp
    src/IBL.cpp
    src/SketchPlane.cpp
    src/Sketch.cpp
    src/SketchManager.cpp
    src/FaceSelector.cpp
    src/core/Autosaver.cpp
    src/core/Checkpoints.cpp
    src/core/GitVault.cpp
    src/core/Measure.cpp
    src/core/MassProps.cpp
    src/core/Feature.cpp
    src/core/Document.cpp
    src/core/Occurrence.cpp
    src/core/AssemblySolve.cpp
    src/core/Command.cpp
    src/core/Expression.cpp
    src/core/Serialization.cpp
    src/core/SketchFeature.cpp
    src/core/SketchProfile.cpp
    src/core/SketchInference.cpp
    src/core/SketchSelection.cpp
    src/core/ExtrudeFeature.cpp
    src/core/DatumPlaneFeature.cpp
    src/core/BodyMode.cpp
    src/core/RevolveFeature.cpp
    src/core/ShellFeature.cpp
    src/core/LoftFeature.cpp
    src/core/SweepFeature.cpp
    src/core/HoleFeature.cpp
    src/core/DraftFeature.cpp
    src/core/DatumAxisFeature.cpp
    src/core/BooleanFeature.cpp
    src/core/ElementHistory.cpp
    src/core/FilletFeature.cpp
    src/core/PatternFeature.cpp
    src/core/SketchSolver.cpp
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
    add_library(planegcs STATIC ${PLANEGCS_SOURCES})
    target_compile_features(planegcs PUBLIC cxx_std_20)
    target_include_directories(planegcs PUBLIC deps/planegcs)
    target_link_libraries(planegcs PUBLIC Eigen3::Eigen Boost::boost)
    set(HAVE_PLANEGCS TRUE)
    message(STATUS "PlaneGCS enabled (Eigen + Boost found)")
else()
    message(WARNING "PlaneGCS disabled: Eigen3/Boost not found")
endif()

add_library(designer_core STATIC ${core_sources})
target_include_directories(designer_core PUBLIC src)
target_link_libraries(designer_core PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json)
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
    "${CMAKE_SOURCE_DIR}/src/CADKernel.h"
    <glm/glm.hpp>
    <nlohmann/json.hpp>
    <vector> <string> <memory> <optional> <variant> <map> <array>
)

# Application: windowing, rendering, UI.
set(app_sources
    src/main.cpp
    src/HeadlessMain.cpp
    src/IBLTextures.cpp
    src/MeshRenderer.cpp
    src/OutlineRenderer.cpp
    src/RayTracingRenderer.cpp
    src/SketchRenderer.cpp
    src/GuiTests.cpp
    src/ui/UI.cpp
    src/ui/DockLayout.cpp
    src/ui/MainMenuBar.cpp
    src/ui/FeatureTreePanel.cpp
    src/ui/OccurrencePanel.cpp
    src/ui/PropertiesPanel.cpp
    src/ui/VariablesPanel.cpp
    src/ui/SketchToolbar.cpp
    src/ui/FileDialogs.cpp
    src/ui/AppActions.cpp
    src/ui/FeatureEditor.cpp
    src/ui/PreviewFeature.cpp
    src/ui/Theme.cpp
    src/ui/ViewportInput.cpp
    src/ui/SketchInteraction.cpp
    src/ui/Toolbar.cpp
    src/Screenshot.cpp
    src/remote/RemoteServer.cpp
)

# UI fonts, loaded at runtime relative to the executable
file(COPY ${CMAKE_SOURCE_DIR}/deps/imgui/misc/fonts/Roboto-Medium.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/assets/fonts)
file(COPY ${CMAKE_SOURCE_DIR}/deps/fonts/fa-solid-900.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/assets/fonts)
include_directories(${CMAKE_SOURCE_DIR}/deps/fonts)

add_executable(designer ${app_sources} ${NFD_SOURCES} ${IMGUI_TEST_ENGINE_SOURCES} ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_demo.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)

# imgui.cpp must see IMGUI_ENABLE_TEST_ENGINE so it emits the test hooks
target_compile_definitions(designer PRIVATE
    IMGUI_ENABLE_TEST_ENGINE
    IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
    DESIGNER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(designer designer_core ${LIBRARIES})

# X11 for the non-blocking XMapWindow window-show path (see main.cpp); avoids
# GLFW's waitForVisibilityNotify deadlock under i3. Linux/X11 only.
if(UNIX AND NOT APPLE)
    find_package(X11)
    if(X11_FOUND)
        target_link_libraries(designer ${X11_LIBRARIES})
    endif()
endif()

# Tests
enable_testing()
add_subdirectory(tests)

# Find glslc (Vulkan shader compiler)
find_program(GLSLC glslc HINTS $ENV{VULKAN_SDK}/bin)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found! Make sure Vulkan SDK is installed and glslc is in PATH")
endif()
message(STATUS "Found glslc: ${GLSLC}")

# Create shader output directory
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/libraries/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Function to compile a shader
function(compile_shader TARGET SHADER_SOURCE SHADER_OUTPUT)
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC} ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling shader ${SHADER_SOURCE}"
    )
    add_custom_target(${TARGET} DEPENDS ${SHADER_OUTPUT})
endfunction()

# Function to compile a ray tracing shader
function(compile_rt_shader TARGET SHADER_SOURCE SHADER_OUTPUT)
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC} --target-env=vulkan1.2 ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling RT shader ${SHADER_SOURCE}"
    )
    add_custom_target(${TARGET} DEPENDS ${SHADER_OUTPUT})
endfunction()

# Compile vertex shader
set(VERTEX_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/mesh.vert")
set(VERTEX_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/mesh_vert.spv")
compile_shader(mesh_vert_shader ${VERTEX_SHADER_SOURCE} ${VERTEX_SHADER_OUTPUT})

# Compile fragment shader
set(FRAGMENT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/mesh.frag")
set(FRAGMENT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/mesh_frag.spv")
compile_shader(mesh_frag_shader ${FRAGMENT_SHADER_SOURCE} ${FRAGMENT_SHADER_OUTPUT})

# Compile ray tracing shaders
set(RAYGEN_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/raygen.rgen")
set(RAYGEN_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/raygen.spv")
compile_rt_shader(raygen_shader ${RAYGEN_SHADER_SOURCE} ${RAYGEN_SHADER_OUTPUT})

set(MISS_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/miss.rmiss")
set(MISS_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/miss.spv")
compile_rt_shader(miss_shader ${MISS_SHADER_SOURCE} ${MISS_SHADER_OUTPUT})

set(CLOSESTHIT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/closesthit.rchit")
set(CLOSESTHIT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/closesthit.spv")
compile_rt_shader(closesthit_shader ${CLOSESTHIT_SHADER_SOURCE} ${CLOSESTHIT_SHADER_OUTPUT})

set(SHADOW_MISS_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/shadow.rmiss")
set(SHADOW_MISS_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/shadow.spv")
compile_rt_shader(shadow_miss_shader ${SHADOW_MISS_SHADER_SOURCE} ${SHADOW_MISS_SHADER_OUTPUT})

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

set(SKETCH_PRIM_VERT_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/sketch_prim.vert")
set(SKETCH_PRIM_VERT_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/sketch_prim_vert.spv")
compile_shader(sketch_prim_vert_shader ${SKETCH_PRIM_VERT_SHADER_SOURCE} ${SKETCH_PRIM_VERT_SHADER_OUTPUT})

set(SKETCH_PRIM_FRAG_SHADER_SOURCE "${CMAKE_SOURCE_DIR}/src/shaders/sketch_prim.frag")
set(SKETCH_PRIM_FRAG_SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/sketch_prim_frag.spv")
compile_shader(sketch_prim_frag_shader ${SKETCH_PRIM_FRAG_SHADER_SOURCE} ${SKETCH_PRIM_FRAG_SHADER_OUTPUT})

# Create a target for all shaders
add_custom_target(shaders ALL DEPENDS mesh_vert_shader mesh_frag_shader raygen_shader miss_shader closesthit_shader shadow_miss_shader outline_vert_shader outline_frag_shader wireframe_vert_shader wireframe_frag_shader sketch_vert_shader sketch_frag_shader sketch_prim_vert_shader sketch_prim_frag_shader)

# Make sure shaders are built before the main executable
add_dependencies(designer shaders)

