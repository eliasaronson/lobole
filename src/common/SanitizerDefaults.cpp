// What the sanitizers should do here, compiled INTO the binary — built when
// ENABLE_SANITIZERS=ON (ASan + UBSan) or ENABLE_TSAN=ON.
//
// The options used to live in wrapper scripts, which meant they applied only
// if you launched through one. Anyone who ran build-asan/designer directly —
// the natural thing to do, and how the leaks in the ray tracer and the XCAF
// documents were actually found — got a report full of dependency noise with
// the real findings buried in it, and no hint that a suppression list existed.
// A binary that carries its own defaults cannot be launched "wrong"; the
// environment still wins where it is set, so nothing here takes away a knob.
//
// Only DEPENDENCY-OWNED allocations belong in the leak list below, named by
// the function that allocates them. Never a whole library: everything we
// forget to free in a CAD operation is allocated inside OpenCASCADE too, and
// `leak:libTK*` would hide all of it. If an entry does not come with the
// reasoning for why the allocation cannot be freed from our side, it is a bug
// being silenced — the XCAF documents we were retaining looked exactly like
// library noise until someone counted them (XcafDocument.h).

extern "C" {

const char* __asan_default_options() {
  // Our std containers cross into non-instrumented OpenCASCADE, which
  // false-positives as container-overflow. Everything else stays on —
  // including leak detection, which is what found the two real leaks above.
  return "detect_container_overflow=0";
}

const char* __lsan_default_suppressions() {
  return
      // XCAFApp_Application::GetApplication() — the XCAF application
      // singleton and its driver table, allocated once on first use and
      // owned by OCC for the life of the process. Reached from any STEP
      // import or visual export.
      "leak:XCAFApp_Application\n"
      "leak:TPrsStd_DriverTable\n"
      // Poly_MergeNodesTool's node map, allocated inside
      // RWStl_Reader::ReadBinary. Our side is a stack-local StlAPI_Reader
      // (CADKernel::LoadSTL) — there is no handle to free.
      "leak:Poly_MergeNodesTool\n"
      // XCAFPrs_DocumentExplorer's label-id strings, allocated while a
      // CafWriter walks the document (glTF/OBJ/PLY export). The explorer is
      // the writer's own stack object; we hand it a document and get the
      // file back. NOTE: our own XCAF documents WERE leaking through this
      // same path and are now closed (XcafDocument) — this covers only the
      // strings OCC drops during the walk.
      "leak:XCAFPrs_DocumentExplorer\n"
      // libdbus's per-connection registration state, from the GTK file
      // dialog (nativefiledialog → portal). One-time, per process, and the
      // connection is a library global we never see.
      "leak:dbus_bus_register\n"
      "leak:internal_bus_get\n";
}

const char* __ubsan_default_options() {
  // The alignment reports from OpenCASCADE's inline headers, and Dear
  // ImGui's (VkPresentModeKHR)~0 "unset" sentinel.
  // DESIGNER_SANITIZER_SOURCE_DIR is baked in by CMake: a sanitizer build is a
  // developer build, and its source tree is where it was configured.
  return "suppressions=" DESIGNER_SANITIZER_SOURCE_DIR
         "/tools/ubsan-suppressions.txt:print_stacktrace=1";
}

#ifdef DESIGNER_TSAN
const char* __tsan_default_options() {
  // Netgen is not instrumented, so TSan cannot see its task scheduler's
  // happens-before edges and reports its internal parallel-for as a race —
  // 28 of them on the first run, all with libnglib on both sides. The file
  // says which, and why suppressing a whole library is right HERE and wrong
  // for OpenCASCADE.
  //
  // history_size lifted because the default 1 keeps too few stack frames of
  // the *previous* access to identify it, which is the half of a race report
  // that tells you what raced with what.
  return "suppressions=" DESIGNER_SANITIZER_SOURCE_DIR
         "/tools/tsan-suppressions.txt:history_size=4:second_deadlock_stack=1";
}
#endif

} // extern "C"
