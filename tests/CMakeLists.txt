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
    test_assembly.cpp
    test_assembly_solve.cpp
    test_measure.cpp
    test_shell.cpp
    test_loft.cpp
    test_sweep.cpp
    test_hole.cpp
    test_draft.cpp
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
    test_fem.cpp
    test_topopt.cpp
    test_drawing_hlr.cpp
    test_drawing.cpp
    test_configuration.cpp
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

# Drawings D1/D2: a drawing tab (third DocKind), sheet + title block, projected
# part views (HLR), v30 round-trip, and SVG export. Headless (no display).
add_test(NAME drawing_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/drawing_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(drawing_smoke PROPERTIES TIMEOUT 120)

# Configurations: part variants (parameter overrides + feature suppression),
# active-config switching, undo, family STEP export, v31 round-trip. Headless.
add_test(NAME configuration_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/configuration_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(configuration_smoke PROPERTIES TIMEOUT 120)

# Drawings D2b: assembly-sourced views + projected-child folding. Headless.
add_test(NAME drawing_assembly_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/drawing_assembly_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(drawing_assembly_smoke PROPERTIES TIMEOUT 120)

# Assemblies A2 project container: parts + assembly + occurrences, round-trip.
add_test(NAME assembly_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_smoke PROPERTIES TIMEOUT 120)

# Assemblies A3 mates: add/set/remove/list/solve over RPC, error reporting,
# undo, and a save -> reopen round-trip. Headless (no display).
add_test(NAME assembly_mates_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_mates_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_mates_smoke PROPERTIES TIMEOUT 120)

# Assemblies A4 propagation: editing a part re-solves the assembly on tab
# activation, without an explicit solve_assembly. Headless (no display).
add_test(NAME assembly_propagation_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_propagation_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_propagation_smoke PROPERTIES TIMEOUT 120)

# Inspection C3 measure: face area / edge length+radius / distance + mass CoM.
add_test(NAME measure_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/measure_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(measure_smoke PROPERTIES TIMEOUT 120)

# Variables C2 units: unit-suffixed expressions resolve, unit names reserved,
# v28 round-trip. Headless (no display).
add_test(NAME units_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/units_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(units_smoke PROPERTIES TIMEOUT 120)

# Interchange X2 visual export: glTF/OBJ/PLY of a part + assembly, material
# carry-through, instancing. Headless (no display).
add_test(NAME visual_export_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/visual_export_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(visual_export_smoke PROPERTIES TIMEOUT 120)

# Assemblies A4 exploded views: RPCs, pure-view-state contract, persistence.
add_test(NAME exploded_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/exploded_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(exploded_smoke PROPERTIES TIMEOUT 120)

# Assemblies A4 BOM: grouped rows, mass taint, CSV export. Headless.
add_test(NAME bom_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/bom_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(bom_smoke PROPERTIES TIMEOUT 120)

# FEM stress study: fix + load a steel bar, solve, iterate (S3-style loop).
# Needs meshing; self-skips (77) at runtime on a build without Netgen.
add_test(NAME fem_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/fem_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(fem_smoke PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)

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
