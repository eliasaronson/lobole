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
    test_actions.cpp
    test_conventions.cpp
    test_frame_pacer.cpp
    # The sanitizer runtime options, in the binary rather than in the caller's
    # environment (empty unless ENABLE_SANITIZERS=ON). See the top-level
    # CMakeLists for why this is per-executable.
    ${SANITIZER_DEFAULTS_SRC}
    test_assembly_flatten.cpp
    test_cadkernel.cpp
    test_sketch.cpp
    test_document.cpp
    test_document_copy.cpp
    test_determinism.cpp
    test_parts.cpp
    test_serialization.cpp
    test_baked_geometry.cpp
    test_instance_registry.cpp
    test_screen_space_mapping.cpp
    test_recent_projects.cpp
    test_gitvault.cpp
    test_projectmerge.cpp
    test_projectstamp.cpp
    test_settings.cpp
    test_keymap.cpp
    test_golden.cpp
    test_units.cpp
    test_window_geometry.cpp
    test_assembly.cpp
    test_assembly_solve.cpp
    test_assembly_loops.cpp
    test_assembly_relations.cpp
    test_derived.cpp
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
    test_frame_timer.cpp
    test_terminal.cpp
    test_cam.cpp
    test_cam_sim.cpp
    test_command_modify.cpp
    test_reorder.cpp
    test_remove_features.cpp
    test_expression.cpp
    test_variables.cpp
    test_datum.cpp
    test_frame.cpp
    test_meshcollide.cpp
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
    test_world_up.cpp
    test_outline_edges.cpp
    test_ibl.cpp
    test_fem.cpp
    test_topopt.cpp
    test_drawing_hlr.cpp
    test_drawing.cpp
    test_configuration.cpp
)
# designer_app brings the ui/ actions layer in so it can be exercised
# headlessly (tests/test_actions.cpp). It drags ImGui and Vulkan along as link
# dependencies, which is fine — no test creates a context or a device — but it
# does mean calling an ImGui function without a context CRASHES rather than
# failing to compile. Test the actions layer, not the widgets.
target_link_libraries(designer_tests PRIVATE designer_app designer_core Catch2::Catch2WithMain)
target_include_directories(designer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(designer_tests PRIVATE
    DESIGNER_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

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
catch_discover_tests(designer_tests ADD_TAGS_AS_LABELS)

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
    catch_discover_tests(femgpu_tests ADD_TAGS_AS_LABELS
                         PROPERTIES RESOURCE_LOCK gpu SKIP_RETURN_CODE 4)
endif()

# GUI smoke tests drive the real app through ImGui Test Engine. They go
# through tools/xvfb-exec.sh, which runs them on a virtual display and holds
# the GPU lock: on a developer's own display they stall to ~1 fps whenever the
# screen locks (FIFO present), which reads as a 600 s timeout and not as the
# passing suite it is. Set DESIGNER_NO_XVFB=1 to watch them run for real.
set(XVFB_EXEC ${CMAKE_CURRENT_SOURCE_DIR}/../tools/xvfb-exec.sh)

# `gui_smoke` is the SAME suite tools/gui-tests-xvfb.sh shards, run serially
# and unsharded: ~365 s against ~100 s. It keeps the `gui` label so the
# headless run (`ctest -LE gui`) still excludes it — but the four python tests
# below also carry `pygui`, so "the python gui ones" is `ctest -L pygui`
# rather than `ctest -L gui -E gui_smoke`. Excluding it by NAME every time was
# knowledge where a label would do (docs/capability-review-2026-08-31.md T3).
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

# Drives designer --headless --remote: no window, GPU, or display server
# needed, so no gui label — this one runs on CI.
add_test(NAME headless_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/headless_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(headless_smoke PROPERTIES LABELS slow TIMEOUT 300)

# Drives designer --offscreen --remote: a real Vulkan renderer with NO window
# and NO display server (the test strips DISPLAY to prove it). Needs an ICD,
# not a display — the femgpu precedent — so it skips (77) in a container and
# runs wherever a device exists. RESOURCE_LOCK serializes it against the
# other GPU tests.
add_test(NAME offscreen_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/offscreen_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(offscreen_smoke PROPERTIES LABELS slow TIMEOUT 300
                     SKIP_RETURN_CODE 77 RESOURCE_LOCK gpu)

# The MCP server's autostart lifecycle: nothing running → it spawns
# --offscreen and the child dies with it. Skips (77) without the mcp SDK or
# a Vulkan device. Finds the binary itself (build/designer beside tools/),
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

# The SDK (agent-scale plan G1b): the RPC surface is GENERATED from
# `describe` (so nothing is duplicated and nothing can drift), and only the
# semantics describe cannot express are hand-written — reference objects,
# plane arithmetic, compositions. Builds a piston and checks it by number.
add_test(NAME sdk_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/sdk_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(sdk_smoke PROPERTIES TIMEOUT 180)

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

# v54 appearance override: set_appearance, inheritance, persistence,
# clearing, validation, and the no-rebuild guarantee. Headless.
add_test(NAME appearance_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/appearance_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(appearance_smoke PROPERTIES TIMEOUT 120)

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

# `describe`'s parameter schema matches the keys each handler actually reads.
# Source-only (no app, no GPU, no display) — RpcParams knows a method's exact
# vocabulary but only at runtime, after the handler has run, so the schema is
# declared in MethodDoc and this keeps it honest in both directions.
add_test(NAME rpc_params_gate
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/rpc_params_gate.py)
set_tests_properties(rpc_params_gate PROPERTIES TIMEOUT 60)

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
# `python3 -m py_compile` writes __pycache__ NEXT TO THE SOURCE, which fails
# against a read-only checkout (tools/ci-local.sh mounts one) and litters the
# tree otherwise. Compile to an explicit path in the build dir instead.
add_test(NAME mcp_server_compiles
         COMMAND python3 -c
                 "import py_compile,sys; py_compile.compile(sys.argv[1], cfile=sys.argv[2], doraise=True)"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/designer_mcp.py
                 ${CMAKE_CURRENT_BINARY_DIR}/designer_mcp.pyc)
set_tests_properties(mcp_server_compiles PROPERTIES TIMEOUT 60)

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
add_test(NAME generators_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/generators_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(generators_smoke PROPERTIES TIMEOUT 300)

# Same for the python client every script and smoke test imports.
add_test(NAME py_client_compiles
         COMMAND python3 -c
                 "import py_compile,sys; py_compile.compile(sys.argv[1], cfile=sys.argv[2], doraise=True)"
                 ${CMAKE_CURRENT_SOURCE_DIR}/../tools/designer_remote.py
                 ${CMAKE_CURRENT_BINARY_DIR}/designer_remote.pyc)
set_tests_properties(py_client_compiles PROPERTIES TIMEOUT 60)

# The pyproject's claims resolve: its modules exist and each console script
# names a real callable — `uv tool install` happens on machines where no test
# runs, so metadata rot has to be caught here.
add_test(NAME packaging_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/packaging_smoke.py)
set_tests_properties(packaging_smoke PROPERTIES TIMEOUT 60)

# Models a bracket through the MCP server (stdio) over the same live app.
# Skips (77) when the python mcp SDK is not installed.
add_test(NAME mcp_smoke
         COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/mcp_smoke.py
                 $<TARGET_FILE:designer>)
set_tests_properties(mcp_smoke PROPERTIES LABELS "gui;pygui" TIMEOUT 600
                     SKIP_RETURN_CODE 77)
