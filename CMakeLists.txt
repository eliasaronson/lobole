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


# OpenCASCADE - manual setup
set(OPENCASCADE_INCLUDE_DIR "/usr/include/opencascade")
if(EXISTS ${OPENCASCADE_INCLUDE_DIR})
    include_directories(${OPENCASCADE_INCLUDE_DIR})
    message(STATUS "OpenCASCADE includes found at: ${OPENCASCADE_INCLUDE_DIR}")
    
    # Core OpenCASCADE libraries we need
    set(OPENCASCADE_LIBS
        TKernel TKMath TKBRep TKG2d TKG3d TKGeomBase TKGeomAlgo
        TKTopAlgo TKPrim TKMesh TKBO TKDESTEP TKDESTL TKXSBase
    )
    
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

# file(GLOB sources src/*.cpp)
set(sources 
    src/main.cpp
    src/Logger.cpp
    src/Scene.cpp
    src/Camera.cpp
    src/CADObject.cpp
    src/CADKernel.cpp
    src/MeshRenderer.cpp
    src/OutlineRenderer.cpp
    src/CADMeshConverter.cpp
    src/CADMaterials.cpp
    src/SketchPlane.cpp
    src/Sketch.cpp
    src/SketchManager.cpp
    src/RayTracingRenderer.cpp
    src/FaceSelector.cpp
)

add_executable(designer ${sources} ${NFD_SOURCES} ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_demo.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)

target_link_libraries(designer ${LIBRARIES})

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

