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

# Libraries
find_package(Vulkan REQUIRED)
set(LIBRARIES "glfw;Vulkan::Vulkan")

# Add OpenCASCADE libraries if available
if(OPENCASCADE_FOUND)
    list(APPEND LIBRARIES ${OPENCASCADE_LIBS})
endif()

file(GLOB sources src/*.cpp)

add_executable(designer ${sources} ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_demo.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp)

target_link_libraries(designer ${LIBRARIES})

