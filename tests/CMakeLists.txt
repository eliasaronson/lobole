find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found on system - fetching v3.5.4")
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.4
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

add_executable(designer_tests
    test_cadkernel.cpp
    test_sketch.cpp
    test_document.cpp
    test_serialization.cpp
    test_command_modify.cpp
    test_faceselector.cpp
    test_sketch_feature.cpp
    test_extrude.cpp
    test_sketch_solver.cpp
    test_sketch_inference.cpp
)
target_link_libraries(designer_tests PRIVATE designer_core Catch2::Catch2WithMain)
target_include_directories(designer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(CTest)
include(Catch)
catch_discover_tests(designer_tests)

# GUI smoke tests drive the real app through ImGui Test Engine.
# Needs a display (use `ctest -LE gui` on headless machines, or xvfb-run).
add_test(NAME gui_smoke COMMAND designer --gui-tests)
set_tests_properties(gui_smoke PROPERTIES LABELS gui TIMEOUT 600)

# Drives a live app over the JSON-RPC remote interface (needs a display)
add_test(NAME remote_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/remote_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(remote_smoke PROPERTIES LABELS gui TIMEOUT 600)
