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
    test_baked_geometry.cpp
    test_instance_registry.cpp
    test_golden.cpp
    test_assembly.cpp
    test_assembly_solve.cpp
    test_measure.cpp
    test_materials.cpp
    test_shell.cpp
    test_loft.cpp
    test_rib.cpp
    test_thread.cpp
    test_sweep.cpp
    test_curve3d.cpp
    test_pipe.cpp
    test_hole.cpp
    test_draft.cpp
    test_autosaver.cpp
    test_command_modify.cpp
    test_reorder.cpp
    test_expression.cpp
    test_variables.cpp
    test_datum.cpp
    test_revolve.cpp
    test_boolean.cpp
    test_split.cpp
    test_refine.cpp
    test_external.cpp
    test_spline.cpp
    test_ellipse_arc.cpp
    test_reference_image.cpp
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
    test_outline_edges.cpp
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
# Discovered cases are individually registered with ctest, so the suite can
# run parallel (`ctest -j$(nproc)`): every case uses distinct temp filenames
# and the golden corpus is read-only.
catch_discover_tests(designer_tests)

# Release metadata smoke: --version must exit before GLFW/Vulkan initialization,
# so the packaged binary can be identified on a display-less machine.
add_test(NAME version_cli COMMAND $<TARGET_FILE:designer> --version)
set_tests_properties(version_cli PROPERTIES
    PASS_REGULAR_EXPRESSION "^Designer ${PROJECT_VERSION}")

# GPU solver tests live in their own binary so designer_tests stays free of
# Vulkan. Each case SKIPs itself when no usable compute device is present, so
# these are safe to run headlessly (a compute queue needs no display).
if(TARGET fem_gpu)
    add_executable(femgpu_tests test_femgpu.cpp)
    target_link_libraries(femgpu_tests PRIVATE fem_gpu designer_core
                          Catch2::Catch2WithMain)
    target_include_directories(femgpu_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
    # GPU cases contend for the compute device — a shared RESOURCE_LOCK
    # serializes them among themselves while the CPU suite runs parallel
    # around them (the reason `ctest -j` is safe repo-wide). (A RUN_SERIAL
    # split for the TopologyOptimize case was tried and reverted: its flake
    # was fp32 run-to-run nondeterminism, not load — fixed by right-sizing
    # the test's compliance tolerance instead.)
    # SKIP_RETURN_CODE 4 is Catch2's SKIP(): every femgpu case self-skips when
    # there is no usable compute device, but without this ctest reports those
    # skips as FAILURES — which is what a GPU-less machine (a container, a CI
    # runner) sees.
    catch_discover_tests(femgpu_tests
                         PROPERTIES RESOURCE_LOCK gpu SKIP_RETURN_CODE 4)
endif()

# GUI smoke tests drive the real app through ImGui Test Engine. They go
# through tools/xvfb-exec.sh, which runs them on a virtual display and holds
# the GPU lock: on a developer's own display they stall to ~1 fps whenever the
# screen locks (FIFO present), which reads as a 600 s timeout and not as the
# passing suite it is. Set DESIGNER_NO_XVFB=1 to watch them run for real.
set(XVFB_EXEC ${CMAKE_CURRENT_SOURCE_DIR}/../tools/xvfb-exec.sh)

add_test(NAME gui_smoke COMMAND ${XVFB_EXEC} $<TARGET_FILE:designer> --gui-tests)
set_tests_properties(gui_smoke PROPERTIES LABELS gui TIMEOUT 600)

# Drives a live app over the JSON-RPC remote interface (needs a display)
add_test(NAME remote_smoke
         COMMAND ${XVFB_EXEC} python3 ${CMAKE_CURRENT_SOURCE_DIR}/remote_smoke.py
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

# V1 contact & connectivity: interference distance/state + contact_report
# components/near-misses/clashes. Headless (no display).
add_test(NAME contact_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/contact_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(contact_smoke PROPERTIES TIMEOUT 120)

# V2 strict inputs + digests: unknown feature fields error, mutating
# responses carry body digests with deltas, bom warnings. Headless.
add_test(NAME strict_inputs_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/strict_inputs_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(strict_inputs_smoke PROPERTIES TIMEOUT 120)

# V4 compare_geometry: boolean-diff metrics, golden-file compare, diff-solid
# export, occurrence compare. Headless.
add_test(NAME compare_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/compare_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(compare_smoke PROPERTIES TIMEOUT 120)

# V5 papercuts: set_camera eye, occurrence rotate, feature param docs,
# honest render_still assembly error. Headless.
add_test(NAME papercuts_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/papercuts_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(papercuts_smoke PROPERTIES TIMEOUT 120)

# Blind-spot suite: tests/blindspots/NN_*.py, one file per known agent
# mistake — the wrong pattern must be caught loudly, the right one works.
# Add a file per new trap; never delete one. Headless.
add_test(NAME blindspot_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/blindspot_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(blindspot_smoke PROPERTIES TIMEOUT 120)

# query_geometry: declarative element predicates + expected_count. Headless.
add_test(NAME query_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/query_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(query_smoke PROPERTIES TIMEOUT 120)

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

# Interchange I3 Onshape importer: pure mapping-layer unit tests against a
# small checked-in fixture (no app, no network, no Onshape quota) — the
# expression/entity/constraint translation that regresses most often.
add_test(NAME onshape_import_test
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/onshape_import_test.py)
set_tests_properties(onshape_import_test PROPERTIES TIMEOUT 60)

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

# Async topology optimization over JSON-RPC (TO2): start a run, poll
# study_status to completion, cancel a second run.
add_test(NAME topo_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/topo_smoke.py
                 $<TARGET_FILE:designer>)
# topo runs the in-app GPU solver when a device is present: same lock as the
# femgpu cases so it never contends with them under `ctest -j`.
set_tests_properties(topo_smoke PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
                     RESOURCE_LOCK gpu)

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

# The MCP server has to at least COMPILE, in every run and in CI. mcp_smoke
# below is gui-labeled (it needs a live app and the python mcp SDK), so a
# plain syntax error in the server shipped undetected for four days in August
# 2026 — nothing on the default path ever imported the file. This costs
# milliseconds and needs neither a display nor the SDK.
add_test(NAME mcp_server_compiles
         COMMAND python3 -m py_compile
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/designer_mcp.py)
set_tests_properties(mcp_server_compiles PROPERTIES TIMEOUT 60)

# Same for the python client every script and smoke test imports.
add_test(NAME py_client_compiles
         COMMAND python3 -m py_compile
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/designer_remote.py)
set_tests_properties(py_client_compiles PROPERTIES TIMEOUT 60)

# Models a bracket through the MCP server (stdio) over the same live app.
# Skips (77) when the python mcp SDK is not installed.
add_test(NAME mcp_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/mcp_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(mcp_smoke PROPERTIES LABELS gui TIMEOUT 600
                     SKIP_RETURN_CODE 77)
