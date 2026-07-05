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
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DVK_PROTOTYPES")
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DVK_PROTOTYPES")

find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(spdlog REQUIRED)

# Dear ImGui
set(IMGUI_DIR deps/imgui)
include_directories(${IMGUI_DIR} ${IMGUI_DIR}/backends ..)

# Dear ImGui Test Engine (GUI testing)
set(IMGUI_TEST_ENGINE_DIR deps/imgui_test_engine/imgui_test_engine)
include_directories(${IMGUI_TEST_ENGINE_DIR})
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


# OpenCASCADE - manual setup
set(OPENCASCADE_INCLUDE_DIR "/usr/include/opencascade")
if(EXISTS ${OPENCASCADE_INCLUDE_DIR})
    include_directories(${OPENCASCADE_INCLUDE_DIR})
    message(STATUS "OpenCASCADE includes found at: ${OPENCASCADE_INCLUDE_DIR}")
    
    # Core OpenCASCADE libraries we need
    set(OPENCASCADE_LIBS
        TKernel TKMath TKBRep TKG2d TKG3d TKGeomBase TKGeomAlgo
        TKTopAlgo TKPrim TKMesh TKBO TKXSBase
    )

    # STEP/STL data-exchange libraries were renamed in OCCT 7.8
    # (TKSTEP*/TKSTL -> TKDESTEP/TKDESTL)
    find_library(OCCT_TKDESTEP TKDESTEP)
    if(OCCT_TKDESTEP)
        list(APPEND OPENCASCADE_LIBS TKDESTEP TKDESTL)
    else()
        list(APPEND OPENCASCADE_LIBS TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKSTL)
    endif()
    
    set(OPENCASCADE_FOUND TRUE)
    add_definitions(-DHAVE_OPENCASCADE)
else()
    message(WARNING "OpenCASCADE not found - CAD functionality will be limited")
    set(OPENCASCADE_FOUND FALSE)
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
    src/SketchPlane.cpp
    src/Sketch.cpp
    src/SketchManager.cpp
    src/FaceSelector.cpp
    src/core/Feature.cpp
    src/core/Document.cpp
    src/core/Command.cpp
    src/core/Serialization.cpp
)

find_package(nlohmann_json 3 REQUIRED)

add_library(designer_core STATIC ${core_sources})
target_include_directories(designer_core PUBLIC src)
target_link_libraries(designer_core PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json)
# Vulkan clip-space conventions everywhere glm is used (propagates to all
# dependents): depth 0..1; the Y-flip lives in Camera::UpdateProjectionMatrix
target_compile_definitions(designer_core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(OPENCASCADE_FOUND)
    target_link_libraries(designer_core PUBLIC ${OPENCASCADE_LIBS})
endif()

# Application: windowing, rendering, UI.
set(app_sources
    src/main.cpp
    src/MeshRenderer.cpp
    src/OutlineRenderer.cpp
    src/RayTracingRenderer.cpp
    src/GuiTests.cpp
    src/ui/UI.cpp
    src/ui/DockLayout.cpp
    src/ui/MainMenuBar.cpp
    src/ui/FeatureTreePanel.cpp
    src/ui/PropertiesPanel.cpp
    src/ui/SketchToolbar.cpp
    src/ui/FileDialogs.cpp
    src/ui/AppActions.cpp
    src/ui/Theme.cpp
    src/ui/ViewportInput.cpp
    src/Screenshot.cpp
    src/remote/RemoteServer.cpp
)

# UI font, loaded at runtime relative to the executable
file(COPY ${CMAKE_SOURCE_DIR}/deps/imgui/misc/fonts/Roboto-Medium.ttf
     DESTINATION ${CMAKE_BINARY_DIR}/assets/fonts)

add_executable(designer ${app_sources} ${NFD_SOURCES} ${IMGUI_TEST_ENGINE_SOURCES} ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_demo.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)

# imgui.cpp must see IMGUI_ENABLE_TEST_ENGINE so it emits the test hooks
target_compile_definitions(designer PRIVATE
    IMGUI_ENABLE_TEST_ENGINE
    IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
)

target_link_libraries(designer designer_core ${LIBRARIES})

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
        COMMAND ${GLSLC} --target-spv=spv1.4 ${SHADER_SOURCE} -o ${SHADER_OUTPUT}
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

# Create a target for all shaders
add_custom_target(shaders ALL DEPENDS mesh_vert_shader mesh_frag_shader raygen_shader miss_shader closesthit_shader shadow_miss_shader outline_vert_shader outline_frag_shader wireframe_vert_shader wireframe_frag_shader)

# Make sure shaders are built before the main executable
add_dependencies(designer shaders)

