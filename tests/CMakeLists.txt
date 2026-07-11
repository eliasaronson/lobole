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
    test_golden.cpp
    test_measure.cpp
    test_shell.cpp
    test_loft.cpp
    test_autosaver.cpp
    test_command_modify.cpp
    test_expression.cpp
    test_variables.cpp
    test_datum.cpp
    test_revolve.cpp
    test_boolean.cpp
    test_pattern.cpp
    test_toponaming.cpp
    test_fillet.cpp
    test_faceselector.cpp
    test_sketch_feature.cpp
    test_extrude.cpp
    test_sketch_solver.cpp
    test_sketch_inference.cpp
    test_sketch_selection.cpp
    test_camera.cpp
    test_ibl.cpp
)
target_link_libraries(designer_tests PRIVATE designer_core Catch2::Catch2WithMain)
target_include_directories(designer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(designer_tests PRIVATE
    DESIGNER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

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

# Drives designer --headless --remote: no window, GPU, or display server
# needed, so no gui label — this one runs on CI.
add_test(NAME headless_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/headless_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(headless_smoke PROPERTIES TIMEOUT 300)

# Models a bracket through the pythonic client (S3).
add_test(NAME pyclient_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/pyclient_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(pyclient_smoke PROPERTIES TIMEOUT 120)

# Records a session and replays it into a fresh headless instance (S4).
add_test(NAME replay_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/replay_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(replay_smoke PROPERTIES TIMEOUT 300)

# Models a bracket through the MCP server (stdio) over the same live app.
# Skips (77) when the python mcp SDK is not installed.
add_test(NAME mcp_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/mcp_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(mcp_smoke PROPERTIES LABELS gui TIMEOUT 600
                     SKIP_RETURN_CODE 77)
