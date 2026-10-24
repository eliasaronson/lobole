# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

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
    TestPaths.cpp
    test_actions.cpp
    test_conventions.cpp
    test_frame_pacer.cpp
    # The sanitizer runtime options, in the binary rather than in the caller's
    # environment (empty unless ENABLE_SANITIZERS=ON). See the top-level
    # CMakeLists for why this is per-executable.
    ${SANITIZER_DEFAULTS_SRC}
    test_assembly_flatten.cpp
    test_step_export.cpp
    test_face_appearance.cpp
    test_cam_workholding.cpp
    test_cadkernel.cpp
    test_sketch_import.cpp
    test_sketch.cpp
    test_document.cpp
    test_document_copy.cpp
    test_determinism.cpp
    test_regen_cache.cpp
    test_parts.cpp
    test_bearings.cpp
    test_retaining_rings.cpp
    test_fits.cpp
    test_tolerances.cpp
    test_press_fits.cpp
    test_print_allowance.cpp
    test_print_check.cpp
    test_measure_seal.cpp
    test_measure_insertion.cpp
    test_print_job.cpp
    test_serialization.cpp
    test_baked_geometry.cpp
    test_open_project.cpp
    test_cam_rough_rest.cpp
    test_instance_registry.cpp
    test_crash_reports.cpp
    test_rpc_contract.cpp
    test_rpc_errors.cpp
    test_screen_space_mapping.cpp
    test_recent_projects.cpp
    test_gitvault.cpp
    test_project_trust.cpp
    test_projectmerge.cpp
    test_projectstamp.cpp
    test_settings.cpp
    test_startup_report.cpp
    test_keymap.cpp
    test_golden.cpp
    test_reader_fuzz.cpp
    test_reader_cases.cpp
    mutate/Mutate.cpp
    mutate/ReaderFuzz.cpp
    test_units.cpp
    test_window_geometry.cpp
    test_assembly.cpp
    test_sequence_timeline.cpp
    test_assembly_solve.cpp
    test_assembly_loops.cpp
    test_assembly_relations.cpp
    test_derived.cpp
    test_templates.cpp
    test_measure.cpp
    test_receipt.cpp
    test_materials.cpp
    test_body_material.cpp
    test_tone_curve.cpp
    test_shell.cpp
    test_loft.cpp
    test_rib.cpp
    test_thread.cpp
    test_worm.cpp
    test_sheetmetal.cpp
    test_flange.cpp
    test_unfold.cpp
    test_refold.cpp
    test_hem_jog.cpp
    test_corners.cpp
    test_normal_cut.cpp
    test_flat_drawing.cpp
    test_sheetmetal_check.cpp
    test_tube.cpp
    test_cope.cpp
    test_frame.cpp
    test_sweep.cpp
    test_curve3d.cpp
    test_pipe.cpp
    test_boolean_volume.cpp
    test_catalog.cpp
    test_hole.cpp
    test_draft.cpp
    test_direct_edit.cpp
    test_autosaver.cpp
    test_save_stamp.cpp
    test_frame_timer.cpp
    test_terminal.cpp
    test_cam.cpp
    test_cam_verify.cpp
    test_cam_setup.cpp
    test_cam_machine.cpp
    test_cam_tools.cpp
    test_cam_sim.cpp
    test_cam_surface.cpp
    test_cam_capability.cpp
    test_cam_travel.cpp
    test_cam_rotary.cpp
    test_cam_surface_store.cpp
    test_cam_spiral.cpp
    test_cam_helical.cpp
    test_cam_engrave.cpp
    test_cam_thread.cpp
    test_cam_honest.cpp
    test_cam_comp.cpp
    test_cam_setup_sheet.cpp
    test_command_modify.cpp
    test_reorder.cpp
    test_remove_features.cpp
    test_expression.cpp
    test_lookup_table.cpp
    test_literals.cpp
    test_requirements.cpp
    test_design_sweep.cpp
    test_variables.cpp
    test_datum.cpp
    test_primitives.cpp
    test_frame.cpp
    test_meshcollide.cpp
    test_flexible_subassembly.cpp
    test_motion.cpp
    test_revolve.cpp
    test_element_origin.cpp
    test_boolean.cpp
    test_split.cpp
    test_refine.cpp
    test_external.cpp
    test_spline.cpp
    test_ellipse_arc.cpp
    test_sketch_curves.cpp
    test_sketch_g2.cpp
    test_sketch_edits.cpp
    test_sketch_text.cpp
    test_reference_image.cpp
    test_pattern.cpp
    test_toponaming.cpp
    test_fillet.cpp
    test_faceselector.cpp
    test_sketch_feature.cpp
    test_extrude.cpp
    test_twist_extrude.cpp
    test_printed_features.cpp
    test_sketch_precision.cpp
    test_model_precision.cpp
    test_sketch_solver.cpp
    test_sketch_inference.cpp
    test_sketch_selection.cpp
    test_camera.cpp
    test_world_up.cpp
    test_light_rig.cpp
    test_outline_edges.cpp
    test_still_outlines.cpp
    test_watchdog.cpp
    test_ibl.cpp
    test_fem.cpp
    test_fem_loads.cpp
    test_topopt.cpp
    test_topo_build.cpp
    test_study_inputs.cpp
    test_drawing_hlr.cpp
    test_drawing.cpp
    test_drawing_sheet.cpp
    test_drawing_frames.cpp
    test_configuration.cpp
)
# designer_app brings the ui/ actions layer in so it can be exercised
# headlessly (tests/test_actions.cpp). It drags ImGui and Vulkan along as link
# dependencies, which is fine — no test creates a context or a device — but it
# does mean calling an ImGui function without a context CRASHES rather than
# failing to compile. Test the actions layer, not the widgets.
target_link_libraries(designer_tests PRIVATE designer_app designer_core Catch2::Catch2WithMain)
target_include_directories(designer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src
                           ${CMAKE_CURRENT_SOURCE_DIR})
# The reader-fuzz budget per case is longer under the sanitizers
# (docs/versioning/README.md#reader-fuzzing).
if(ENABLE_SANITIZERS)
    target_compile_definitions(designer_tests PRIVATE
                               LOBOLE_READER_FUZZ_SANITIZED)
endif()
# What the build option promised, stated apart from the define the app's
# library carries, so a test can hold the one to the other.
if(HAVE_FEM_GPU)
    target_compile_definitions(designer_tests PRIVATE LOBOLE_EXPECT_TOPO_GPU)
endif()

# Precompiled headers. designer_core has carried one since round 1 for the same
# reason (CMakeLists.txt): the OpenCASCADE tree behind core/Document.h is
# ~146k preprocessed lines and re-parses in every TU that touches a Document,
# which is 52 of the 163 test files. Catch2's macros are on top of that in 79.
#
# It is also what pays for splitting a big test file: five files re-parse the
# same headers five times, so test_cam.cpp's split cost 21 s of duplicated
# parsing before this line existed (docs/architecture.md#precompiled-headers
# — split first, PCH second, and never one without the other).
#
# Stable headers only: cad/CADKernel.h rather than core/Document.h and
# core/Feature.h, which change often. GCC's .gch is not byte-reproducible,
# so a project header in it made every touch of it a full, uncached rebuild
# of all 112 test sources.
if(ENABLE_PCH)
target_precompile_headers(designer_tests PRIVATE
    <catch2/catch_test_macros.hpp>
    <catch2/matchers/catch_matchers_floating_point.hpp>
    <catch2/matchers/catch_matchers_string.hpp>
    "${CMAKE_SOURCE_DIR}/src/cad/CADKernel.h"
    <glm/glm.hpp>
    <nlohmann/json.hpp>
    <vector> <string> <memory> <optional> <map> <set> <filesystem> <cmath>
)
endif()

# The source tree and this binary's path, on the ONE file that reads them
# (TestPaths.cpp; the tests call TestSourceDir() / TestBinaryPath()). On every
# test source they were a different flag in every tree, and a worktree missed
# ccache on each file that expanded them. The binary's path is there so a test
# can compare its own mtime against the source it was built from: a stale
# binary fails like a regression, and twice in one round it was one.
set_source_files_properties(TestPaths.cpp PROPERTIES COMPILE_DEFINITIONS
    "LOBOLE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\";LOBOLE_TEST_BINARY=\"$<TARGET_FILE:designer_tests>\"")

include(CTest)
include(Catch)
# Discovered cases are individually registered with ctest, so the suite can
# run parallel (`ctest -j$(nproc)`): every case uses distinct temp filenames
# and the golden corpus is read-only.
#
# ADD_TAGS_AS_LABELS turns each case's Catch2 tags into ctest LABELS, which is
# what makes `[slow]` self-maintaining: a heavy test opts itself out of the
# inner loop where it is WRITTEN, by whoever knows it is heavy. The previous
# arrangement was a by-name regex in tools/test-fast.sh, which could not know
# about a test added here — it named only the topology/FEM gates and never
# learned about the CAM ones, so the "8 s" inner loop had quietly become 49 s.
# It also buys `ctest -L cam`, `-L sketch` and friends for free.
# XDG_STATE_HOME and XDG_CONFIG_HOME point INTO THE BUILD TREE, so no C++
# test can write to the developer's own state.
#
# They could, and did. `Autosaver::StateDir()` falls back to
# $HOME/.local/state/lobole when XDG_STATE_HOME is unset, and the unit tests
# run in-process with whatever environment ctest was given — so a test that
# saved a project called RecentProjects::Remember and put its temp directory
# in the RECENT PROJECTS MENU of the app on that machine. Nine entries like
# /tmp/nix-shell-3687090-.../designer_save_drawing_tab, pointing at
# directories deleted seconds later, from three runs of the suite.
#
# The python smoke tests already do this per test (smoke_app.py sets both);
# this is the same isolation for the ones that run in-process, and it covers
# every future test rather than the one that was noticed. Autosave sidecars
# for unsaved documents and the instance registry land here too.
set(LOBOLE_TEST_HOME "${CMAKE_CURRENT_BINARY_DIR}/test-home")
file(MAKE_DIRECTORY "${LOBOLE_TEST_HOME}/state" "${LOBOLE_TEST_HOME}/config")
# The semicolon is ESCAPED: unescaped, CMake splits the string into two list
# elements, so ENVIRONMENT takes the first and the second is read as another
# property name — and the whole thing is silently dropped. It generated tests
# with no ENVIRONMENT at all, which looked exactly like it had worked.
catch_discover_tests(designer_tests ADD_TAGS_AS_LABELS
                     TEST_SPEC "~[reader_fuzz]"
                     PROPERTIES ENVIRONMENT
                     "XDG_STATE_HOME=${LOBOLE_TEST_HOME}/state\;XDG_CONFIG_HOME=${LOBOLE_TEST_HOME}/config")
# The reader-fuzz families (docs/versioning/README.md#reader-fuzzing) with a
# TIMEOUT of their own: a reader that never returns is stopped here, and
# build/reader-fuzz-last-case.<family> names the case it was reading.
catch_discover_tests(designer_tests ADD_TAGS_AS_LABELS
                     TEST_SPEC "[reader_fuzz]"
                     PROPERTIES TIMEOUT 1200 ENVIRONMENT
                     "XDG_STATE_HOME=${LOBOLE_TEST_HOME}/state\;XDG_CONFIG_HOME=${LOBOLE_TEST_HOME}/config")

# Release metadata smoke: --version must exit before GLFW/Vulkan initialization,
# so the packaged binary can be identified on a display-less machine.
add_test(NAME version_cli COMMAND $<TARGET_FILE:designer> --version)
set_tests_properties(version_cli PROPERTIES
    PASS_REGULAR_EXPRESSION "^Lobole ${PROJECT_VERSION}")

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
    catch_discover_tests(femgpu_tests ADD_TAGS_AS_LABELS
                         PROPERTIES RESOURCE_LOCK gpu SKIP_RETURN_CODE 4
                         ENVIRONMENT
                         "XDG_STATE_HOME=${LOBOLE_TEST_HOME}/state\;XDG_CONFIG_HOME=${LOBOLE_TEST_HOME}/config")
endif()

# GUI smoke tests drive the real app through ImGui Test Engine. They go
# through tools/xvfb-exec.sh, which runs them on a virtual display and holds
# the GPU lock: on a developer's own display they stall to ~1 fps whenever the
# screen locks (FIFO present), which reads as a 600 s timeout and not as the
# passing suite it is. Set LOBOLE_NO_XVFB=1 to watch them run for real.
set(XVFB_EXEC ${CMAKE_CURRENT_SOURCE_DIR}/../tools/xvfb-exec.sh)

# `gui_smoke` is the SAME suite tools/gui-tests-xvfb.sh shards, run serially
# and unsharded: ~365 s against ~100 s. It keeps the `gui` label so the
# headless run (`ctest -LE gui`) still excludes it — but the four python tests
# below also carry `pygui`, so "the python gui ones" is `ctest -L pygui`
# rather than `ctest -L gui -E gui_smoke`. Excluding it by NAME every time was
# knowledge where a label would do (docs/architecture.md#tests).
add_test(NAME gui_smoke COMMAND ${XVFB_EXEC} $<TARGET_FILE:designer> --gui-tests)
set_tests_properties(gui_smoke PROPERTIES LABELS gui TIMEOUT 600)

# Drives a live app over the JSON-RPC remote interface (needs a display)
add_test(NAME remote_smoke
         COMMAND ${XVFB_EXEC} python3 ${CMAKE_CURRENT_SOURCE_DIR}/remote_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(remote_smoke PROPERTIES LABELS "gui;pygui" TIMEOUT 600)

# Screen-space rendering invariants on a real GPU: ambient occlusion must land
# on the geometry that causes it. The renderers have no other pixel-level
# coverage, and that gap let the SSAO map be sampled through the framebuffer
# instead of the 3D viewport rect. Skips (77) without pillow.
add_test(NAME render_ao_smoke
         COMMAND ${XVFB_EXEC} python3
                 ${CMAKE_CURRENT_SOURCE_DIR}/render_ao_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(render_ao_smoke PROPERTIES LABELS "gui;pygui" TIMEOUT 300
                     SKIP_RETURN_CODE 77)

# Drives lobole --headless --remote: no window, GPU, or display server
# needed, so no gui label — this one runs on CI.
add_test(NAME headless_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/headless_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(headless_smoke PROPERTIES LABELS slow TIMEOUT 300)

# Drives lobole --offscreen --remote: a real Vulkan renderer with NO window
# and NO display server (the test strips DISPLAY to prove it). Needs an ICD,
# not a display — the femgpu precedent — so it skips (77) in a container and
# runs wherever a device exists. RESOURCE_LOCK serializes it against the
# other GPU tests.
add_test(NAME offscreen_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(offscreen_smoke PROPERTIES LABELS slow TIMEOUT 300
                     SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)

# The light rig follows the camera: the back of a block reads as its front,
# and Lock lights to model switches that off (docs/rendering/README.md#the-light-rig).
add_test(NAME render_light_rig_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/render_light_rig_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(render_light_rig_smoke PROPERTIES LABELS slow TIMEOUT 300
                     SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)

# The MCP server's autostart lifecycle: nothing running → it spawns
# --offscreen and the child dies with it. Skips (77) without the mcp SDK or
# a Vulkan device. Finds the binary itself (build/lobole beside tools/),
# so no $<TARGET_FILE> — which is the point: it tests the server's own probe.
# Pixel regression on the offscreen renderer. Its baseline is keyed to the
# DEVICE that produced it (ping reports it), so this skips rather than fails
# where no baseline matches — which is every developer machine until someone
# records one, and is the only honest answer for a check whose subject is not
# bit-reproducible across vendors. CI pins its driver in the container image
# and runs the script directly with --require-baseline, so a Mesa bump there
# is a red build rather than a gate that quietly stopped applying.
add_test(NAME render_pixel_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/render_pixel_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(render_pixel_smoke PROPERTIES LABELS slow TIMEOUT 300
                     SKIP_RETURN_CODE 77)
# ...and across a device rebuild in place: the scene before and after it
# byte-identical to each other and to the baseline
# (docs/rendering/README.md#rebuilding-the-device).
add_test(NAME render_pixel_rebuild_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/render_pixel_smoke.py
                 $<TARGET_FILE:designer> --rebuild)
set_tests_properties(render_pixel_rebuild_smoke PROPERTIES LABELS slow
                     TIMEOUT 300 SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)

add_test(NAME mcp_autostart_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/mcp_autostart_smoke.py)
set_tests_properties(mcp_autostart_smoke PROPERTIES TIMEOUT 120
                     SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)

# Mode flags must conflict loudly, never win by precedence: --headless once
# silently overrode the --hidden a test passed, and the test spent weeks
# testing nothing (CLAUDE.md). All four exit at argument parsing, before any
# window or GPU exists, so they run anywhere.
add_test(NAME offscreen_conflicts_headless
         COMMAND $<TARGET_FILE:designer> --offscreen --headless --remote 0)
set_tests_properties(offscreen_conflicts_headless PROPERTIES
    PASS_REGULAR_EXPRESSION "--offscreen cannot run with --headless")
add_test(NAME offscreen_conflicts_hidden
         COMMAND $<TARGET_FILE:designer> --offscreen --hidden --remote 0)
set_tests_properties(offscreen_conflicts_hidden PROPERTIES
    PASS_REGULAR_EXPRESSION "--offscreen cannot run with --hidden")
add_test(NAME offscreen_conflicts_guitests
         COMMAND $<TARGET_FILE:designer> --offscreen --gui-tests --remote 0)
set_tests_properties(offscreen_conflicts_guitests PROPERTIES
    PASS_REGULAR_EXPRESSION "--gui-tests cannot run with --offscreen")
add_test(NAME offscreen_requires_remote
         COMMAND $<TARGET_FILE:designer> --offscreen)
set_tests_properties(offscreen_requires_remote PROPERTIES
    PASS_REGULAR_EXPRESSION "--offscreen requires --remote")

# The part gate vs the handlers it gates: two lists that must agree, and
# until 2026-08-23 nothing checked that they did.
add_test(NAME rpc_gate_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/rpc_gate_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(rpc_gate_smoke PROPERTIES TIMEOUT 120)

# A refused call changes nothing: every method refuses an unknown key before
# its handler runs, every wrong-typed key leaves the world as it was, and the
# calls that write files or move tabs are tried from a valid baseline with
# each key made wrong (docs/scripting/rpc.md#a-refused-call-changes-nothing).
add_test(NAME refused_calls_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/refused_calls_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(refused_calls_smoke PROPERTIES TIMEOUT 180)

# A web page can POST to localhost; the JSON in an HTTP body must not run.
add_test(NAME http_on_rpc_port_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/http_on_rpc_port_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(http_on_rpc_port_smoke PROPERTIES TIMEOUT 120)

# A reference image is a PNG, JPEG or BMP up to 16384 px a side; anything else
# is refused (src/common/ImageLimits.h).
add_test(NAME reference_image_formats_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/reference_image_formats_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(reference_image_formats_smoke PROPERTIES TIMEOUT 120)

# Two people, one shared project, NO NETWORK (F9): a bare repository in a temp
# directory is a real remote as far as git is concerned, so two clones of it
# are a real two-person scenario that runs anywhere, CI included. Two headless
# instances at once, so it is also the second-client case under load.
add_test(NAME two_collaborators
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/two_collaborators.py
                 $<TARGET_FILE:designer>)
set_tests_properties(two_collaborators PROPERTIES TIMEOUT 300)

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

# Drawings v147: section views, datums, the title block's derived rows, a
# parts list and its balloons, through RPC, export and a reopen. Headless.
add_test(NAME drawing_sheet_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/drawing_sheet_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(drawing_sheet_smoke PROPERTIES TIMEOUT 120)

# Drawings v151: feature control frames on datum features, a detail view,
# values along their lines, balloons laid out by the app, an ASME B18.3
# screw and a material grade on the sheets. Headless.
add_test(NAME drawing_frames_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/drawing_frames_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(drawing_frames_smoke PROPERTIES TIMEOUT 120)

# Assemblies A2 project container: parts + assembly + occurrences, round-trip.
add_test(NAME assembly_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_smoke PROPERTIES TIMEOUT 120)

# Assemblies A5 sub-assemblies: an assembly placed inside another. Checks the
# claims that only hold if the whole TREE is walked — combined BOM, mass,
# section area over leaves — plus the two refusals (cycle, deleting a
# referenced tab) and a save/reopen round-trip.
add_test(NAME nested_assembly_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/nested_assembly_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(nested_assembly_smoke PROPERTIES TIMEOUT 180)

# Assemblies A3 mates: add/set/remove/list/solve over RPC, error reporting,
# undo, and a save -> reopen round-trip. Headless (no display).
add_test(NAME assembly_mates_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_mates_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_mates_smoke PROPERTIES TIMEOUT 120)

# Assemblies A6 mate loops: what solve_assembly reports about a loop -- DOF,
# closure, which mates are redundant-but-consistent. Headless (no display).
add_test(NAME assembly_loops_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_loops_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_loops_smoke PROPERTIES TIMEOUT 120)

# Relations (v83): a gear pair's ratio derived from tooth counts and driven
# from either side, a rack, the refusals — over the RPC, headless.
add_test(NAME assembly_relations_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_relations_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_relations_smoke PROPERTIES TIMEOUT 120)

# Fan-out (agent-scale plan G5): three instances, three parts, one project —
# and the refusal that makes two instances on ONE directory loud.
add_test(NAME fanout_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/fanout_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(fanout_smoke PROPERTIES TIMEOUT 300)

# Motion studies (agent-scale plan G2): a study over the RPC, run sync and
# as a job, the collision table, a scrub, the animated GLB, the refusals.
add_test(NAME motion_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/motion_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(motion_smoke PROPERTIES TIMEOUT 300)

# What the PES pump relies on (generators/pes_pump/README.md#how-it-moves): custom axes, a
# helix's start, a study's configuration and mate values, one clash
# vocabulary, the section's kept half, and the Python verbs.
add_test(NAME api_friction_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/api_friction_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(api_friction_smoke PROPERTIES TIMEOUT 180)

# A scrub lands on the branch the run walked, not the mirror image of it.
add_test(NAME scrub_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scrub_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(scrub_smoke PROPERTIES TIMEOUT 300)

# A study is a job: an id, cancelled with its client, reused by its inputs.
add_test(NAME study_jobs_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/study_jobs_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(study_jobs_smoke PROPERTIES TIMEOUT 300)

# Every study kind's result saved with the project and reused after a
# reopen; a moved input reads stale (docs/simulation/README.md
# #studies-as-jobs).
add_test(NAME study_reopen_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/study_reopen_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(study_reopen_smoke PROPERTIES TIMEOUT 300
                     SKIP_RETURN_CODE 77)

# fit_fastener and check_fasteners agree about where a joint ENDS (W1).
add_test(NAME fastener_stack_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/fastener_stack_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(fastener_stack_smoke PROPERTIES TIMEOUT 300)

# A read reaches another part by reference and never switches tabs (W2).
add_test(NAME tabless_read_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tabless_read_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(tabless_read_smoke PROPERTIES TIMEOUT 300)

# A drawing of an assembly is affordable, or says why not (W3).
add_test(NAME drawing_quality_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/drawing_quality_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(drawing_quality_smoke PROPERTIES TIMEOUT 300)

# The SDK (agent-scale plan G1b): the RPC surface is GENERATED from
# `describe` (so nothing is duplicated and nothing can drift), and only the
# semantics describe cannot express are hand-written — reference objects,
# plane arithmetic, compositions. Builds a piston and checks it by number.
add_test(NAME sdk_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/sdk_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(sdk_smoke PROPERTIES TIMEOUT 180)

# Declared clearances (peer-survey-plan W3, v100): the five standings a boolean
# cannot carry — never / holds / violated / stale / unverifiable — plus the
# non-event that matters most, an unrelated edit NOT making one stale.
add_test(NAME assertion_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assertion_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assertion_smoke PROPERTIES TIMEOUT 180)

# A parameter survives the script boundary (peer-survey-plan W1): a part
# built with Param arithmetic is DRIVEN by its variables afterwards, and the
# perturbation half — nudge one variable, diff the saved document, assert
# exactly the fields it drives moved — fails on a dimension that is quietly a
# literal.
add_test(NAME param_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/param_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(param_smoke PROPERTIES TIMEOUT 180)

# batch (agent-scale plan G1a): N calls, ONE regeneration, ONE undo step,
# and a failed call rolls the whole thing back.
add_test(NAME batch_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/batch_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(batch_smoke PROPERTIES TIMEOUT 120)

# Named datum frames (v86, agent-scale plan G0): a frame built FROM a bolt
# pattern, a mate anchored to it BY NAME, and the mate following when a
# variable moves the geometry — the position an agent never transcribed.
add_test(NAME frame_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/frame_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(frame_smoke PROPERTIES TIMEOUT 120)

# Derived (v84): a jaw derives a servo as a ghost over the RPC — refusals,
# stale reported and never auto-resolved, update as one undo step, the
# in-context placement, the round trip through disk.
add_test(NAME derived_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/derived_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(derived_smoke PROPERTIES TIMEOUT 120)

# Feature templates (v161): a seat saved from one part, placed twice on
# another over the RPC -- refusals that change nothing, an upstream edit,
# a changed template marked and updated only on request, unlink, and the
# library through export, import, save and open.
add_test(NAME templates_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/templates_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(templates_smoke PROPERTIES TIMEOUT 120)

# list_elements by what made a face: a pocket's floor and walls, a hole's
# bores, a pattern's copies, a Derived copy through `via`, and the why of a
# filter that matches nothing.
add_test(NAME elements_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/elements_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(elements_smoke PROPERTIES TIMEOUT 120)

# One part placed in two configurations and materials: BOM lines, mass, a
# non-active configuration's edit, STEP products, a job in a configuration.
add_test(NAME part_configuration_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/part_configuration_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(part_configuration_smoke PROPERTIES TIMEOUT 120)

# CAM requests that wait off the frame (docs/cam/README.md#requests-that-wait):
# a parked cam_verify answers exactly what the synchronous handler says while
# a second connection is answered; order per connection, edits, cancel.
add_test(NAME cam_parked_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/cam_parked_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(cam_parked_smoke PROPERTIES LABELS "cam;slow;asan_slow"
                     TIMEOUT 300)

# Sheet metal (v108/v109): the rules and the first two features over the RPC —
# the gauge tables, the refusals in words, and a flanged volume checked
# against the bend allowance by hand.
add_test(NAME sheetmetal_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/sheetmetal_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(sheetmetal_smoke PROPERTIES TIMEOUT 120)

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

# Tolerances and fits (docs/tolerances/README.md): a tolerance as design intent
# over RPC, and check_fits pairing faces off the assembly's own mates —
# including a sub-assembly, where reaching the part behind an anchor is a walk
# through three documents. Headless (no display).
add_test(NAME tolerances_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tolerances_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(tolerances_smoke PROPERTIES TIMEOUT 120)

# Press fits (docs/tolerances/README.md#press-fits): a bearing modelled at its
# K7 seat's interference is relieved and listed as a press fit by the named
# pair, the exact sweep, contact_report and an exact motion study, and an H7
# seat turns the same overlap back into a clash in each. Headless.
add_test(NAME press_fit_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/press_fit_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(press_fit_smoke PROPERTIES TIMEOUT 120)

# Design requirements (docs/requirements/README.md): each measure kind passes
# and fails, a model edit makes only what it reaches stale, an edited
# requirement reads unchecked, a deleted reference unresolved, a re-check of
# an unchanged design measures nothing, the async check is a job, the
# refusals name the vocabulary, and both project forms carry the evidence.
# Headless.
add_test(NAME requirements_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/requirements_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(requirements_smoke PROPERTIES TIMEOUT 240)

# The design sweep (docs/requirements/README.md#design-sweep) on a generator
# part: the pan/tilt head's camera swept over cam_d breaks where its grip
# can no longer take its fillet, bracketed and named; the async form is a
# job, cancel keeps the points done, and a sweep whose client leaves is
# cancelled. Headless.
add_test(NAME design_sweep_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/design_sweep_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(design_sweep_smoke PROPERTIES TIMEOUT 300)

# Project tables and lookup() over RPC: both row forms, a variable and a
# feature reading one, the refusals naming what exists, undo, and the tables
# surviving both project forms (flat file, project.json). Headless.
add_test(NAME tables_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tables_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(tables_smoke PROPERTIES TIMEOUT 120)

# check_literals and extract_variables over RPC
# (docs/variables/README.md#literals-that-should-be-expressions): the lint's
# two findings, a dry run that changes nothing, an apply that moves no
# geometry and is one undo step in either scope, reuse, refusals. Headless.
add_test(NAME literals_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/literals_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(literals_smoke PROPERTIES TIMEOUT 120)

# The printing process allowance (docs/tolerances/README.md#printed-parts):
# check_fit, check_fits, print_allowance and a Thread's answer on a printed
# part say what to model, and the advice, followed, is accepted. Headless.
add_test(NAME print_allowance_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/print_allowance_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(print_allowance_smoke PROPERTIES TIMEOUT 120)

# check_print, the one printability check: its negative controls (a 60
# degree overhang, a fused print-in-place pair, a near pair, a part off the
# bed), the limits it takes from the print, and its refusals. Headless.
add_test(NAME print_check_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/print_check_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(print_check_smoke PROPERTIES TIMEOUT 120)

# Slicer settings, the printed mass and export_print_job
# (docs/tolerances/README.md#print-jobs): the refusals, a two-plate job read
# back, and -- when Bambu Studio's CLI is installed -- the job sliced and
# every object's walls, shells and infill read out of the G-code. Without
# the CLI that half is skipped, and said so. Headless.
add_test(NAME print_job_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/print_job_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(print_job_smoke PROPERTIES TIMEOUT 300)

# V1 contact & connectivity: interference distance/state + contact_report
# components/near-misses/clashes. Headless (no display).
add_test(NAME contact_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/contact_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(contact_smoke PROPERTIES TIMEOUT 120)

# Insertion paths: check_insertion and the `insertion` requirement
# (docs/inspect/README.md#insertion). Headless.
add_test(NAME insertion_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/insertion_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(insertion_smoke PROPERTIES TIMEOUT 180)

# Assembly sequences: each step slid home against everything placed before
# it (docs/assemblies/README.md#assembly-sequences). Headless.
add_test(NAME assembly_sequence_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_sequence_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_sequence_smoke PROPERTIES TIMEOUT 240)

# The clash sweep runs its booleans on every core. A race in OpenCASCADE's
# lazily-filled per-shape caches, or a `record` that stops running in plan
# order, corrupts an ANSWER rather than crashing — so this diffs a
# one-thread report against an all-thread one, byte for byte.
add_test(NAME sweep_parallel_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/sweep_parallel_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(sweep_parallel_smoke PROPERTIES TIMEOUT 300)

# Two widgets in one window must not answer to one ImGui id. A source check,
# because neither ImGui nor the test engine can see it: ImGui notices only
# while the mouse is over one of the pair, and the engine gathers items BY id,
# so a collision shows up in a listing as one entry rather than two. Needs no
# display and no build — it reads the source.
add_test(NAME widget_ids_lint
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/widget_ids_lint.py)
set_tests_properties(widget_ids_lint PROPERTIES TIMEOUT 60)

# A test that launches the app must give it its own config dir, or the suite
# rewrites the developer's real preferences (set_settings persists at once).
add_test(NAME config_isolation_lint
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/config_isolation_lint.py)
set_tests_properties(config_isolation_lint PROPERTIES TIMEOUT 60)

# A merge (exclusive on build/'s lock) waits for a suite run through
# tools/ctest (shared); two suites run together. A scratch tree, no app.
add_test(NAME build_lock_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/build_lock_smoke.py)
set_tests_properties(build_lock_smoke PROPERTIES TIMEOUT 120)

# V2 strict inputs + digests: unknown feature fields error, mutating
# responses carry body digests with deltas, bom warnings. Headless.
add_test(NAME strict_inputs_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/strict_inputs_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(strict_inputs_smoke PROPERTIES TIMEOUT 120)

# The honesty contract: a mutating RPC that leaves a feature broken says so in
# ITS OWN response, rather than returning a plausible success a caller has to
# poll behind. Headless.
add_test(NAME regen_honesty_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/regen_honesty_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(regen_honesty_smoke PROPERTIES TIMEOUT 120)

# add_constraint refuses a constraint that constrains nothing (repeats or
# contradicts the sketch), names what it runs into and changes nothing;
# `driven: true` adds a reference dimension that answers `measured`. Headless.
add_test(NAME sketch_constraint_refusal_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/sketch_constraint_refusal_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(sketch_constraint_refusal_smoke PROPERTIES TIMEOUT 120
                     LABELS "sketch")

# An edit that is saved but regenerates nothing (a tolerance, the units, a
# study, a configuration that is not active) reads unsaved in `ping.dirty`,
# the question the close prompt and autosave ask. Headless.
add_test(NAME unsaved_edits_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/unsaved_edits_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(unsaved_edits_smoke PROPERTIES TIMEOUT 120)

# A lossy open (docs/versioning/README.md#a-lossy-load): open_project reports
# what it dropped, and save_project back over it is refused until
# overwrite_lossy. Headless.
add_test(NAME lossy_open_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/lossy_open_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(lossy_open_smoke PROPERTIES TIMEOUT 120)

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

# Reference parts: whole parts built over the RPC against closed forms, a
# STEP round trip each, the known-failure ratchet and the generated page
# (docs/modeling/reference-parts.md). `tools/ctest -L reference`.
add_test(NAME reference_parts
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/reference_parts.py
                 $<TARGET_FILE:designer>)
set_tests_properties(reference_parts PROPERTIES TIMEOUT 300
                     LABELS reference)

# v54 appearance override: set_appearance, inheritance, persistence,
# clearing, validation, and the no-rebuild guarantee. Headless.
add_test(NAME appearance_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/appearance_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(appearance_smoke PROPERTIES TIMEOUT 120)

# v164 face appearance: set_appearance {element} by name, an upstream edit,
# a face a cut made, a stale name reported (not dropped), clear, undo,
# persistence. Headless.
add_test(NAME face_appearance_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/face_appearance_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(face_appearance_smoke PROPERTIES TIMEOUT 120)

# Variables C2 units: unit-suffixed expressions resolve, unit names reserved,
# v28 round-trip. Headless (no display).
add_test(NAME units_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/units_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(units_smoke PROPERTIES TIMEOUT 120)

# Interchange X2 visual export: glTF/OBJ/PLY of a part + assembly, material
# carry-through, instancing. Headless (no display).
# The path-traced still must reach the whole part: the ray far limit was a
# hard-coded 10000 in raygen.rgen, and that is a LENGTH — anything more than
# ten metres from the camera was invisible, so a 3 m dock (framed from ~11 m)
# rendered as pure backdrop and the app called it a successful save. Needs a
# GPU with ray tracing; skips (77) where there is none.
add_test(NAME render_still_reach_smoke
         COMMAND python3
                 ${CMAKE_CURRENT_SOURCE_DIR}/render_still_reach_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(render_still_reach_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300 SKIP_RETURN_CODE 77)

# Glass is see-through in a still: a red block behind a pane shows red
# through `glass` and not through `steel` (docs/rendering/materials.md#glass).
add_test(NAME render_glass_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/render_glass_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(render_glass_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300 SKIP_RETURN_CODE 77)

# The outdoor sky behind and over a still: blue above the horizon, ground
# below, the sun reported, the refusals (docs/rendering/still-render.md#the-sky).
add_test(NAME render_sky_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/render_sky_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(render_sky_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300 SKIP_RETURN_CODE 77)

# A show-always underlay is in a quick still with no sketch in edit, and not
# without the flag (docs/sketching/README.md#reference-images-underlays). A
# renderer, not ray tracing.
add_test(NAME underlay_show_always_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/underlay_show_always_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(underlay_show_always_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300)

# The frame watchdog: a stalled main thread is logged with its label and a
# symbolized stack, and the app carries on (docs/architecture.md#crashes-and-stalls).
add_test(NAME watchdog_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/watchdog_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(watchdog_smoke PROPERTIES TIMEOUT 120)

# A crash leaves a report the next start collects: a real SIGSEGV, a kill -9,
# a clean quit and an uncaught exception, in a scratch state dir
# (docs/architecture.md#crashes-and-stalls).
add_test(NAME crash_report_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/crash_report_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(crash_report_smoke PROPERTIES TIMEOUT 120)

# A start that cannot draw says why and exits 3; the device ladder, a missing
# renderer, LOBOLE_RENDERER=software, and safe mode after two early crashes
# (docs/architecture.md#when-the-renderer-cannot-start). The windowed half
# needs the display tools/xvfb-exec.sh gives, hence pygui.
add_test(NAME startup_failure_smoke
         COMMAND ${XVFB_EXEC} python3
                 ${CMAKE_CURRENT_SOURCE_DIR}/startup_failure_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(startup_failure_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300)

# The backstop: a lost device a rebuild cannot save (a second loss within 60 s
# of a rebuild, or LOBOLE_FAULT=rebuild) is a state: the work autosaved at
# once, a report, every drawing method refused as device_lost, the rest
# answering, and Restart (docs/rendering/README.md#a-lost-device), windowed
# and offscreen.
add_test(NAME device_loss_smoke
         COMMAND ${XVFB_EXEC} python3
                 ${CMAKE_CURRENT_SOURCE_DIR}/device_loss_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(device_loss_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300)

# A lost device is rebuilt in place and the session goes on: screenshot,
# select_face and a quick still answer on the new device, ping counts the
# rebuild, and set_settings {gpu_device} answers with where it landed
# (docs/rendering/README.md#rebuilding-the-device). The offscreen half carries
# no gui label, so the headless and sanitized suites run it; it skips (77)
# without a Vulkan device, like offscreen_smoke.
add_test(NAME device_rebuild_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/device_rebuild_smoke.py
                 $<TARGET_FILE:designer> offscreen)
set_tests_properties(device_rebuild_smoke PROPERTIES TIMEOUT 300
                     SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)
add_test(NAME device_rebuild_windowed_smoke
         COMMAND ${XVFB_EXEC} python3
                 ${CMAKE_CURRENT_SOURCE_DIR}/device_rebuild_smoke.py
                 $<TARGET_FILE:designer> windowed)
set_tests_properties(device_rebuild_windowed_smoke PROPERTIES
                     LABELS "gui;pygui" TIMEOUT 300)

# An app a test launched dies with the test, SIGKILL included: smoke_app arms
# LOBOLE_EXIT_WITH_PARENT for every launch (src/app/ParentWatch.h).
add_test(NAME exit_with_parent_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/exit_with_parent_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(exit_with_parent_smoke PROPERTIES TIMEOUT 90)

# The performance harness itself works: it launches, runs two cut-down
# workloads, records, compares and refuses (docs/performance/README.md). It
# asserts no duration, and writes nothing of the user's.
add_test(NAME bench_harness_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/bench_harness_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(bench_harness_smoke PROPERTIES LABELS bench TIMEOUT 300)

# The quick still: the raster renderers into an image of exactly W x H, path
# parameters refused, the viewport left as it was
# (docs/rendering/still-render.md#quick-mode). A renderer, not ray tracing.
add_test(NAME render_quick_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/render_quick_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(render_quick_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300)

# A motion study filmed (docs/assemblies/README.md#motion-video): png frames,
# and gif/mp4/webm through ffmpeg read back by ffprobe; the refusals,
# including a PATH without ffmpeg; the pose put back; an async cancel. A
# renderer, not ray tracing. Skips (77) the encoded formats when the machine
# has no ffmpeg — shell.nix installs it, so a nix build never skips.
# A drawing request waits for its projection off the frame: parked, in a
# batch, cancelled, async (docs/drawings/README.md#requests-that-wait).
# --offscreen, because --headless projects inline.
add_test(NAME drawing_parked_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/drawing_parked_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(drawing_parked_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300 SKIP_RETURN_CODE 77)

add_test(NAME motion_video_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/motion_video_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(motion_video_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300 SKIP_RETURN_CODE 77)

# An assembly sequence filmed (docs/assemblies/README.md
# #playing-and-filming-a-sequence): the parts appear in order and travel
# home along their paths; a sequence that does not check is refused.
add_test(NAME assembly_video_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/assembly_video_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(assembly_video_smoke PROPERTIES LABELS "gui;pygui"
                     TIMEOUT 300)

add_test(NAME visual_export_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/visual_export_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(visual_export_smoke PROPERTIES TIMEOUT 120)

# `describe`'s parameter schema matches the keys each handler actually reads.
# Source-only (no app, no GPU, no display) — RpcParams knows a method's exact
# vocabulary but only at runtime, after the handler has run, so the schema is
# declared in MethodDoc and this keeps it honest in both directions.
# flake.nix pins the submodules a second time, as flake inputs; this keeps
# the two copies on the same commits.
add_test(NAME flake_submodules_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/flake_submodules_gate.py)
set_tests_properties(flake_submodules_gate PROPERTIES TIMEOUT 30)

add_test(NAME rpc_params_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/rpc_params_gate.py)
set_tests_properties(rpc_params_gate PROPERTIES TIMEOUT 60)

add_test(NAME rpc_returns_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/rpc_returns_gate.py)
set_tests_properties(rpc_returns_gate PROPERTIES TIMEOUT 60)

add_test(NAME rpc_names_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/rpc_names_gate.py)
set_tests_properties(rpc_names_gate PROPERTIES TIMEOUT 60)

# The committed icon font is what its script draws (docs/ui/README.md#icons).
add_test(NAME icon_font_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/icon_font_gate.py)
set_tests_properties(icon_font_gate PROPERTIES TIMEOUT 30)

# ...and that a dimensional one says what UNIT it is in. Same source-only
# scan, same MethodDoc rows, different question: every angle bug this project
# has shipped was a number crossing the JSON boundary with nothing but its
# name to say whether it was degrees or radians, millimetres or inches.
add_test(NAME rpc_units_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/rpc_units_gate.py)
set_tests_properties(rpc_units_gate PROPERTIES TIMEOUT 60)

# The RPC surface against its last published snapshot (api/rpc-surface.json):
# a method or parameter that goes away needs kApiVersion raised once the
# surface is released, and the snapshot refreshed either way, so a caller
# written against one api_version keeps working while it stands.
add_test(NAME api_surface_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/api_surface_gate.py)
set_tests_properties(api_surface_gate PROPERTIES TIMEOUT 60)

# A format bump on a branch is written as a placeholder and numbered when it
# lands (docs/versioning/format.md#a-bump-takes-its-number-when-it-lands): the
# header's arithmetic, the pending rows, the goldens' names against their
# "version", and no placeholder beyond what is pending. CI adds --landed on
# main, where nothing may be pending.
add_test(NAME format_version_gate
         COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/format-version.py check)
set_tests_properties(format_version_gate PROPERTIES TIMEOUT 30
                     SKIP_RETURN_CODE 77)

# The LGPL-family dependencies are linked as SEPARATE SHARED OBJECTS, which is
# the whole reason this project's own licence stays a choice: LGPL-2.1 s6 lets
# us combine and license on our terms only while a recipient can relink the
# library. One word turns SHARED into STATIC and nothing else changes -- the
# build succeeds, this suite passes, every render is identical -- and the
# promise THIRD_PARTY.md makes is void in binaries already shipped. Reads
# DT_NEEDED out of the linked binary rather than grepping the build files, and
# fails on a dependency no THIRD_PARTY.md row covers. It demands only what this
# build was configured with -- each optional component's HAVE_* result is
# passed below, so a stock-Ubuntu build without Netgen is not asked to link it.
# Skips (77) on a non-ELF build; Windows import tables are not read yet.
add_test(NAME linkage_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/linkage_gate.py
                 $<TARGET_FILE:designer>
                 HAVE_PLANEGCS=$<BOOL:${HAVE_PLANEGCS}>
                 HAVE_CAM_ADAPTIVE=$<BOOL:${HAVE_CAM_ADAPTIVE}>
                 HAVE_NETGEN=$<BOOL:${HAVE_NETGEN}>
                 OPENCASCADE_FOUND=$<BOOL:${OPENCASCADE_FOUND}>)
set_tests_properties(linkage_gate PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# Every authored source file says what it is licensed as. The top-level
# LICENSE is what makes the licence valid, so this is not correctness -- it is
# the two places a missing notice costs something: packagers (Debian, Fedora,
# Flathub) check for SPDX identifiers and file bugs when they are absent, and a
# file with no notice is one whose licence someone copying a function out of it
# has to guess. Not `reuse lint`, which requires LICENSES/ to be named by SPDX
# identifier; ours is a distribution bundle named by component and referenced
# by filename from THIRD_PARTY.md (spdx_gate.py says what is exempt).
add_test(NAME spdx_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/spdx_gate.py)
set_tests_properties(spdx_gate PROPERTIES TIMEOUT 60)

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

# The worker-thread half of the same surface, split out 2026-08-25: it is the
# slow one (it solves, then polls), so a failure here names the async contract
# rather than taking the CRUD/staleness contract down with it.
add_test(NAME fem_async_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/fem_async_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(fem_async_smoke PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# Async topology optimization over JSON-RPC (TO2): start a run, poll
# study_status to completion, cancel a second run.
add_test(NAME topo_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/topo_smoke.py
                 $<TARGET_FILE:designer>)
# topo runs the in-app GPU solver when a device is present: same lock as the
# femgpu cases so it never contends with them under `ctest -j`.
set_tests_properties(topo_smoke PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
                     RESOURCE_LOCK gpu)

# Topology you can build from
# (docs/topopt/README.md#topology-you-can-build-from): load cases, a keep-out, the connectivity report, check_topology, a
# case through probe_study and the display, and split_for_printing.
add_test(NAME topo_build_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/topo_build_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(topo_build_smoke PROPERTIES TIMEOUT 240
                     SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)

# Records a session and replays it into a fresh headless instance (S4).
add_test(NAME replay_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/replay_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(replay_smoke PROPERTIES TIMEOUT 300)

# Every module of the lobole package has to at least COMPILE, in every run
# and in CI. mcp_smoke below is gui-labeled (it needs a live app and the
# python mcp SDK), so a plain syntax error in the MCP server shipped
# undetected for four days in August 2026 — nothing on the default path ever
# imported the file. This costs milliseconds and needs neither a display nor
# the SDK. It compiles in memory and writes nothing: `py_compile` writes
# __pycache__ next to the source, which fails against a read-only checkout
# (tools/ci-local.sh mounts one), and a `mcp.pyc` written into this
# directory was imported as the `mcp` SDK by every test run from here.
add_test(NAME python_package_compiles
         COMMAND python3 -c
                 "import glob,os,sys; [compile(open(f).read(), f, 'exec') for f in glob.glob(os.path.join(sys.argv[1], '*.py'))]"
                 ${CMAKE_SOURCE_DIR}/python/lobole)
set_tests_properties(python_package_compiles PROPERTIES TIMEOUT 60)

# The server's hand-written format prose (FEATURE_FORMATS, MATERIAL_SLUGS)
# against the app's `describe`, HEADLESS. mcp_smoke has the same check, but
# it is gui-labelled (a hidden window), and a feature type added without a
# row stayed red there for two days in September 2026 with nothing on the
# default path to say so. This needs no display, no GPU and no mcp SDK.
add_test(NAME mcp_formats_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/mcp_formats_gate.py
                 $<TARGET_FILE:designer>)
set_tests_properties(mcp_formats_gate PROPERTIES TIMEOUT 120)

# generators/*.py are parts written as programs (a spur gear first); this
# runs one against a headless app and checks the part by numbers. It is the
# standing answer to "do we need scripted features for X" — so it stays on
# the default path.
# The watch generator (generators/watch/README.md): every part on
# its receipt, every occurrence placed by a mate, the going train and motion
# works keeping time EXACTLY, and the declared clearances holding.
add_test(NAME watch_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/watch_smoke.py
                 $<TARGET_FILE:designer>)
# 73 s: the whole watch generator. It set the headless suite's floor and kept
# tools/test-fast.sh from its figure, so it is `slow` (docs/architecture.md#the-development-loop).
# Under ASan it runs past its 300 s even alone, so a sanitized run leaves it
# out.
set_tests_properties(watch_smoke PROPERTIES LABELS "slow;asan_slow" TIMEOUT 300)

add_test(NAME generators_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/generators_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(generators_smoke PROPERTIES TIMEOUT 300)

# The big generators as tests (generators/README.md#the-big-generators-as-tests):
# each one's cheapest checking stage from nothing, against its own instance.
# Minutes each, and a GPU for the offscreen ones, so they run only when asked
# for: CONFIGURATIONS keeps every plain ctest -- the headless suite, CI, the
# nix sandbox's check -- from seeing them at all, and
#   ctest -C generators -L generators
# runs them (tools/merge-branch.sh --full does, after the headless suite).
# They are `slow` too, for anything that does pass -C generators.
# One test per generators/<name>/test.json, which holds its budget (about
# twice the run measured alone, for a loaded machine) beside its command:
# the TIMEOUT is the budget plus a minute, since the script kills its
# generator at the budget and says so, which a ctest kill would not. A new
# file is seen at the next build (CONFIGURE_DEPENDS).
file(GLOB gen_tests CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/generators/*/test.json)
foreach(gen_test ${gen_tests})
  get_filename_component(gen_dir ${gen_test} DIRECTORY)
  get_filename_component(gen ${gen_dir} NAME)
  file(READ ${gen_test} gen_json)
  string(JSON budget ERROR_VARIABLE gen_error GET "${gen_json}" budget_s)
  if(gen_error OR NOT budget MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "${gen_test}: budget_s is not a whole number of "
                        "seconds over 0 (${gen_error}${budget})")
  endif()
  math(EXPR gen_timeout "${budget} + 60")
  add_test(NAME generator_${gen} CONFIGURATIONS generators
           COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/generator_runs.py
                   $<TARGET_FILE:designer> ${gen} ${budget})
  set_tests_properties(generator_${gen} PROPERTIES
                       LABELS "generators;slow" TIMEOUT ${gen_timeout})
endforeach()

# The README's table of those tests is made from the same files
# (tools/generator_tests.py); this fails when it was not remade.
add_test(NAME generator_table_gate
         COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/generator_tests.py --check)
set_tests_properties(generator_table_gate PROPERTIES TIMEOUT 30)

# Every instance claims its port outside its XDG_STATE_HOME, and
# tools/scratch_port.py honours a live claim and drops a dead one
# (docs/scripting/README.md#the-instance-registry).
add_test(NAME port_claims_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/port_claims_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(port_claims_smoke PROPERTIES TIMEOUT 120)

# The pyproject's claims resolve: its modules exist and each console script
# names a real callable — `uv tool install` happens on machines where no test
# runs, so metadata rot has to be caught here.
add_test(NAME packaging_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/packaging_smoke.py)
set_tests_properties(packaging_smoke PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)

# Models a bracket through the MCP server (stdio) over the same live app.
# Skips (77) when the python mcp SDK is not installed.
#
# Through XVFB_EXEC like the other two windowed smokes: it launches the app
# --hidden, which on a Wayland session opened a window on the developer's own
# compositor that never received a frame callback — the app then hung before
# answering its first RPC and this timed out at 600 s.
add_test(NAME mcp_smoke
         COMMAND ${XVFB_EXEC} python3 ${CMAKE_CURRENT_SOURCE_DIR}/mcp_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(mcp_smoke PROPERTIES LABELS "gui;pygui" TIMEOUT 600
                     SKIP_RETURN_CODE 77)

add_test(NAME catalog_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/catalog_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(catalog_smoke PROPERTIES TIMEOUT 120)

# Typed RPC contracts (docs/scripting/rpc.md#typed-contracts): the stubs
# match the rows, and the running server enforces and publishes them.
add_test(NAME rpc_contracts_generated
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_rpc_contracts.py --check)
add_test(NAME rpc_contracts_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/rpc_contracts_smoke.py
                         $<TARGET_FILE:designer>)
set_tests_properties(rpc_contracts_smoke PROPERTIES TIMEOUT 120)

add_test(NAME inspection_contracts_generated
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/generate_inspection_contracts.py --check)
add_test(NAME inspection_contracts_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/inspection_contracts_smoke.py
                         $<TARGET_FILE:designer>)
set_tests_properties(inspection_contracts_smoke PROPERTIES TIMEOUT 120)
# Skips (77) where this interpreter has no mypy — a stock-Ubuntu container
# built from the README's list, which has no reason to carry a dev linter.
# CI's build-and-test installs mypy and runs the script DIRECTLY, so the gate
# still applies on every push (see ci.yml).
add_test(NAME inspection_typing_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/inspection_typing_gate.py)
set_tests_properties(inspection_typing_gate PROPERTIES TIMEOUT 120
                     SKIP_RETURN_CODE 77)

# The regeneration cache on generator output: every return to a state seen
# before (a value set back, undo, redo) executes nothing and reads the same.
add_test(NAME regen_cache_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/regen_cache_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(regen_cache_smoke PROPERTIES TIMEOUT 120)

add_test(NAME component_update_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/component_update_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(component_update_smoke PROPERTIES TIMEOUT 120)

# Exercise real generator geometry and cache reuse without a whole engine sweep.
add_test(NAME om615_accessories_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/om615_accessories_smoke.py
                 $<TARGET_FILE:designer>)
# 37 s, the next longest after watch_smoke.
set_tests_properties(om615_accessories_smoke PROPERTIES LABELS slow TIMEOUT 180)

add_test(NAME om615_mounting_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/om615_mounting_smoke.py
                 $<TARGET_FILE:designer>)
# ~58 s alone, and not on one core: ~105 s user + ~50 s system across the
# app's worker threads (2.7 cores on average). Counted as ONE slot, a -j$(nproc)
# suite packed the rest of the machine around it and it ran 136-140 s, then
# past its 180 s timeout once anything else was also building. PROCESSORS is
# what it actually takes, so ctest leaves it room.
set_tests_properties(om615_mounting_smoke PROPERTIES LABELS "slow;asan_slow" TIMEOUT 180
                     PROCESSORS 3)

add_test(NAME cncest6090_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/cncest6090_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(cncest6090_smoke PROPERTIES TIMEOUT 120)

add_test(NAME toolmakers_clamp_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/toolmakers_clamp_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(toolmakers_clamp_smoke PROPERTIES TIMEOUT 120)

# Flexible sub-assemblies (v129,
# docs/assemblies/README.md#flexible-sub-assemblies) over RPC through the
# SDK: a parent relation into an inner mate by path, set_mate by path, a
# study driven by path, the per-leaf GLB node, and the refusals. Headless.
add_test(NAME flexible_subassembly_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/flexible_subassembly_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(flexible_subassembly_smoke PROPERTIES TIMEOUT 180)

# --- Hostile parameters and random designs -----------------------------------
# (docs/scripting/rpc.md#the-hostile-sweep, docs/modeling/README.md#random-designs)
# The hostile sweep: every method of `describe`, every key of its row, each
# hostile value alone and inside a valid baseline, on an instance of its own
# per shard (methods sharded by a stable hash of their name). The compact set
# runs everywhere; --full is `slow` (tools/ci-local.sh --asan runs it, which is
# where UBSan reports a 1e308 narrowed to int).
set(HOSTILE_SHARDS 8)
math(EXPR HOSTILE_LAST "${HOSTILE_SHARDS} - 1")
foreach(k RANGE ${HOSTILE_LAST})
  add_test(NAME hostile_params_${k}
           COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/hostile_params_smoke.py
                   $<TARGET_FILE:designer> --shard ${k}/${HOSTILE_SHARDS})
  set_tests_properties(hostile_params_${k} PROPERTIES TIMEOUT 180)
  add_test(NAME hostile_params_full_${k}
           COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/hostile_params_smoke.py
                   $<TARGET_FILE:designer> --shard ${k}/${HOSTILE_SHARDS} --full)
  set_tests_properties(hostile_params_full_${k} PROPERTIES LABELS slow
                       TIMEOUT 900)
endforeach()
# The sweep detects what it claims: debug_fault plants each failure (an
# internal error, a NaN answer, a write before a refusal, a kernel throw in a
# batch) and debug_crash a SIGSEGV whose crash report must be found.
add_test(NAME hostile_params_selftest
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/hostile_params_smoke.py
                 $<TARGET_FILE:designer> --self-test)
set_tests_properties(hostile_params_selftest PROPERTIES TIMEOUT 120)
# The action matrix (docs/modeling/README.md#action-matrix): every ordinary
# action on every face, edge and body of a fixed corpus, against the committed
# baseline (tests/action_matrix/). The sample runs everywhere. The full sweep,
# designs sharded by a stable hash of their name, and the determinism check
# are some forty CPU-minutes between them, so like the big generators they
# exist only under a configuration: no plain ctest (the headless suite, CI)
# sees them, and
#   tools/ctest -C action_matrix -L action_matrix -j4
# runs them. A shard is up to 500 s on a quiet machine and twice that loaded.
add_test(NAME action_matrix_sample
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/action_matrix_smoke.py
                 $<TARGET_FILE:designer> --sample)
set_tests_properties(action_matrix_sample PROPERTIES TIMEOUT 240)
set(ACTION_MATRIX_SHARDS 8)
math(EXPR ACTION_MATRIX_LAST "${ACTION_MATRIX_SHARDS} - 1")
foreach(k RANGE ${ACTION_MATRIX_LAST})
  add_test(NAME action_matrix_full_${k} CONFIGURATIONS action_matrix
           COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/action_matrix_smoke.py
                   $<TARGET_FILE:designer> --shard ${k}/${ACTION_MATRIX_SHARDS})
  set_tests_properties(action_matrix_full_${k} PROPERTIES
                       LABELS "slow;action_matrix" TIMEOUT 1800)
endforeach()
add_test(NAME action_matrix_determinism CONFIGURATIONS action_matrix
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/action_matrix_smoke.py
                 $<TARGET_FILE:designer> --determinism)
set_tests_properties(action_matrix_determinism PROPERTIES
                     LABELS "slow;action_matrix" TIMEOUT 900)
# The classifier and the ratchet against planted answers.
add_test(NAME action_matrix_selftest
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/action_matrix_smoke.py
                 --self-test)
set_tests_properties(action_matrix_selftest PROPERTIES TIMEOUT 60)
# Random designs: a fixed list of seeds that must stay clean (with the
# fuzzer's own self-test first), and every minimised case replayed.
add_test(NAME fuzz_seeds_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_seeds_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(fuzz_seeds_smoke PROPERTIES TIMEOUT 180)
add_test(NAME fuzz_corpus_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_corpus_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(fuzz_corpus_smoke PROPERTIES TIMEOUT 180)

# Every test finds the lobole Python package (python/lobole/) without
# editing sys.path, on the stock-Ubuntu CI image as in the nix shell (which
# exports the same). Last in the file, so it reaches every test above; the
# Catch2 cases discovered at test time are C++ and do not need it.
get_property(LOBOLE_ALL_TESTS DIRECTORY PROPERTY TESTS)
set_property(TEST ${LOBOLE_ALL_TESTS} APPEND PROPERTY
             ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/python")
